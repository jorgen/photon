#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vio/task.h>

#include "photon/detail/protocol.h"
#include "photon/detail/transport.h"
#include "photon/error.h"

namespace photon::detail
{
struct raw_message_t
{
  std::uint8_t type = 0;
  std::vector<std::uint8_t> body;
};

class frame_reader_t
{
public:
  explicit frame_reader_t(transport_t &transport)
    : _transport(&transport)
  {
  }

  static constexpr std::size_t max_message_size = std::size_t{512} * 1024 * 1024;

  vio::task_t<result_t<raw_message_t>> next()
  {
    auto header = co_await fill_at_least(5);
    if (!header.has_value())
    {
      co_return std::unexpected(header.error());
    }

    std::uint8_t type = _buffer[_consumed];
    std::uint32_t length = static_cast<std::uint32_t>(_buffer[_consumed + 1]) << 24 | static_cast<std::uint32_t>(_buffer[_consumed + 2]) << 16 | static_cast<std::uint32_t>(_buffer[_consumed + 3]) << 8 | static_cast<std::uint32_t>(_buffer[_consumed + 4]);
    if (length < 4)
    {
      co_return fail(error_kind_t::protocol, "backend message length is too small");
    }
    if (length > max_message_size)
    {
      co_return fail(error_kind_t::protocol, "backend message exceeds the size cap");
    }

    std::size_t total = 1 + static_cast<std::size_t>(length);
    auto full = co_await fill_at_least(total);
    if (!full.has_value())
    {
      co_return std::unexpected(full.error());
    }

    raw_message_t message;
    message.type = type;
    auto begin = _buffer.begin() + static_cast<std::ptrdiff_t>(_consumed);
    message.body.assign(begin + 5, begin + static_cast<std::ptrdiff_t>(total));
    _consumed += total;
    if (_consumed == _buffer.size())
    {
      _buffer.clear();
      _consumed = 0;
    }
    co_return message;
  }

private:
  static constexpr std::size_t read_chunk_size = 16384;

  vio::task_t<result_t<void>> fill_at_least(std::size_t need)
  {
    if (_buffer.size() - _consumed >= need)
    {
      co_return result_t<void>{};
    }
    if (_consumed > 0)
    {
      _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(_consumed));
      _consumed = 0;
    }
    while (_buffer.size() < need)
    {
      std::size_t offset = _buffer.size();
      _buffer.resize(offset + read_chunk_size);
      auto read = co_await _transport->read_some(std::span<std::byte>(reinterpret_cast<std::byte *>(_buffer.data()) + offset, read_chunk_size));
      if (!read.has_value())
      {
        _buffer.resize(offset);
        co_return std::unexpected(read.error());
      }
      if (*read == 0)
      {
        _buffer.resize(offset);
        co_return fail(error_kind_t::connection, "server closed the connection");
      }
      _buffer.resize(offset + *read);
    }
    co_return result_t<void>{};
  }

  transport_t *_transport;
  std::vector<std::uint8_t> _buffer;
  std::size_t _consumed = 0;
};
} // namespace photon::detail
