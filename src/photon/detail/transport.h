#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vio/event_loop.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tls_client.h>
#include <vio/ssl_config_t.h>
#include <vio/task.h>

#include "photon/error.h"

namespace photon::detail
{
struct transport_t
{
  virtual ~transport_t() = default;
  virtual vio::task_t<result_t<std::size_t>> read_some(std::span<std::byte> dst) = 0;
  virtual vio::task_t<result_t<void>> write_all(std::vector<std::uint8_t> bytes) = 0;
  virtual void cancel_read() = 0;
  virtual void close() = 0;
};

class tcp_transport_t final : public transport_t
{
public:
  tcp_transport_t(vio::tcp_t tcp, vio::tcp_reader_t reader)
    : _tcp(std::move(tcp))
    , _reader(std::move(reader))
  {
  }

  vio::task_t<result_t<std::size_t>> read_some(std::span<std::byte> dst) override
  {
    auto r = co_await _reader.read_into(dst);
    if (!r.has_value())
    {
      co_return fail(error_kind_t::connection, "socket read failed: " + r.error().msg);
    }
    co_return *r;
  }

  vio::task_t<result_t<void>> write_all(std::vector<std::uint8_t> bytes) override
  {
    auto r = co_await vio::write_tcp(_tcp, std::move(bytes));
    if (!r.has_value())
    {
      co_return fail(error_kind_t::connection, "socket write failed: " + r.error().msg);
    }
    co_return result_t<void>{};
  }

  void cancel_read() override
  {
    _reader.cancel();
  }

  void close() override
  {
  }

private:
  vio::tcp_t _tcp;
  vio::tcp_reader_t _reader;
};

class tls_transport_t final : public transport_t
{
public:
  tls_transport_t(vio::ssl_client_t client, vio::tls_client_reader_t reader)
    : _client(std::move(client))
    , _reader(std::move(reader))
  {
  }

  vio::task_t<result_t<std::size_t>> read_some(std::span<std::byte> dst) override
  {
    if (_leftover_pos < _leftover.size())
    {
      std::size_t available = _leftover.size() - _leftover_pos;
      std::size_t n = std::min(dst.size(), available);
      std::memcpy(dst.data(), _leftover.data() + _leftover_pos, n);
      _leftover_pos += n;
      if (_leftover_pos == _leftover.size())
      {
        _leftover.clear();
        _leftover_pos = 0;
      }
      co_return n;
    }

    auto chunk = co_await _reader;
    if (!chunk.has_value())
    {
      co_return fail(error_kind_t::connection, "tls read failed: " + chunk.error().msg);
    }
    auto &buffer = chunk.value();
    std::size_t length = buffer->len;
    const auto *src = reinterpret_cast<const std::uint8_t *>(buffer->base);
    std::size_t n = std::min(dst.size(), length);
    std::memcpy(dst.data(), src, n);
    if (n < length)
    {
      _leftover.assign(src + n, src + length);
      _leftover_pos = 0;
    }
    co_return n;
  }

  vio::task_t<result_t<void>> write_all(std::vector<std::uint8_t> bytes) override
  {
    uv_buf_t buffer = uv_buf_init(reinterpret_cast<char *>(bytes.data()), static_cast<unsigned int>(bytes.size()));
    auto r = co_await vio::ssl_client_write(_client, buffer);
    if (!r.has_value())
    {
      co_return fail(error_kind_t::connection, "tls write failed: " + r.error().msg);
    }
    co_return result_t<void>{};
  }

  void cancel_read() override
  {
    _reader.cancel();
  }

  void close() override
  {
  }

private:
  vio::ssl_client_t _client;
  vio::tls_client_reader_t _reader;
  std::vector<std::uint8_t> _leftover;
  std::size_t _leftover_pos = 0;
};

vio::task_t<result_t<std::optional<vio::tcp_t>>> connect_raw_tcp(vio::event_loop_t &loop, const std::string &host, std::uint16_t port, std::chrono::milliseconds timeout);
result_t<std::unique_ptr<transport_t>> make_tcp_transport(vio::tcp_t tcp);
vio::task_t<result_t<std::unique_ptr<transport_t>>> upgrade_to_tls(vio::tcp_t tcp, vio::ssl_config_t config, std::string host);
} // namespace photon::detail
