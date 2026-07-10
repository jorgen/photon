#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <vio/event_loop.h>
#include <vio/operation/tcp.h>
#include <vio/task.h>

#include "photon/error.h"

namespace photon::detail
{
struct transport_t
{
  virtual ~transport_t() = default;
  virtual vio::task_t<result_t<std::size_t>> read_some(std::span<std::byte> dst) = 0;
  virtual vio::task_t<result_t<void>> write_all(std::vector<std::uint8_t> bytes) = 0;
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

  void close() override
  {
  }

private:
  vio::tcp_t _tcp;
  vio::tcp_reader_t _reader;
};

vio::task_t<result_t<std::unique_ptr<transport_t>>> connect_tcp(vio::event_loop_t &loop, const std::string &host, std::uint16_t port, std::chrono::milliseconds timeout);
} // namespace photon::detail
