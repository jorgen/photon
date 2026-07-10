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

    std::uint8_t type = _buffer[0];
    std::uint32_t length = static_cast<std::uint32_t>(_buffer[1]) << 24 | static_cast<std::uint32_t>(_buffer[2]) << 16 | static_cast<std::uint32_t>(_buffer[3]) << 8 | static_cast<std::uint32_t>(_buffer[4]);
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
    message.body.assign(_buffer.begin() + 5, _buffer.begin() + static_cast<std::ptrdiff_t>(total));
    _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(total));
    co_return message;
  }

private:
  vio::task_t<result_t<void>> fill_at_least(std::size_t need)
  {
    while (_buffer.size() < need)
    {
      std::array<std::byte, 16384> chunk{};
      auto read = co_await _transport->read_some(chunk);
      if (!read.has_value())
      {
        co_return std::unexpected(read.error());
      }
      if (*read == 0)
      {
        co_return fail(error_kind_t::connection, "server closed the connection");
      }
      const auto *bytes = reinterpret_cast<const std::uint8_t *>(chunk.data());
      _buffer.insert(_buffer.end(), bytes, bytes + *read);
    }
    co_return result_t<void>{};
  }

  transport_t *_transport;
  std::vector<std::uint8_t> _buffer;
};
} // namespace photon::detail
