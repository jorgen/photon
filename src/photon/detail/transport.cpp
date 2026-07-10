#include "photon/detail/transport.h"

#include <expected>
#include <utility>

#include <vio/operation/dns.h>

#include "photon/detail/timeout.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace photon::detail
{
namespace
{
void set_port(sockaddr *addr, std::uint16_t port)
{
  if (addr == nullptr)
  {
    return;
  }
  if (addr->sa_family == AF_INET)
  {
    reinterpret_cast<sockaddr_in *>(addr)->sin_port = htons(port);
  }
  else if (addr->sa_family == AF_INET6)
  {
    reinterpret_cast<sockaddr_in6 *>(addr)->sin6_port = htons(port);
  }
}
} // namespace

vio::task_t<result_t<std::unique_ptr<transport_t>>> connect_tcp(vio::event_loop_t &loop, const std::string &host, std::uint16_t port, std::chrono::milliseconds timeout)
{
  vio::address_info_t hints;
  hints.family = AF_UNSPEC;
  hints.socktype = SOCK_STREAM;
  hints.protocol = IPPROTO_TCP;

  auto resolved = co_await with_timeout<std::expected<vio::address_info_list_t, vio::error_t>>(loop, timeout, [&](vio::cancellation_t *token) { return vio::get_addrinfo(loop, host, hints, token); });
  if (!resolved.has_value())
  {
    if (vio::is_cancelled(resolved.error()))
    {
      co_return fail(error_kind_t::timeout, "DNS resolution of '" + host + "' timed out");
    }
    co_return fail(error_kind_t::connection, "DNS resolution of '" + host + "' failed: " + resolved.error().msg);
  }

  std::string last_error = "no addresses resolved";
  for (auto &address : *resolved)
  {
    sockaddr *sa = address.get_sockaddr();
    if (sa == nullptr)
    {
      continue;
    }
    set_port(sa, port);

    auto client = vio::tcp_create(loop);
    if (!client.has_value())
    {
      last_error = client.error().msg;
      continue;
    }

    auto connected = co_await with_timeout<std::expected<void, vio::error_t>>(loop, timeout, [&](vio::cancellation_t *token) { return vio::tcp_connect(client.value(), sa, token); });
    if (!connected.has_value())
    {
      if (vio::is_cancelled(connected.error()))
      {
        co_return fail(error_kind_t::timeout, "connection to " + host + " timed out");
      }
      last_error = connected.error().msg;
      continue;
    }

    auto reader = vio::tcp_create_reader(client.value());
    if (!reader.has_value())
    {
      co_return fail(error_kind_t::connection, "failed to create socket reader: " + reader.error().msg);
    }
    co_return std::make_unique<tcp_transport_t>(std::move(client.value()), std::move(reader.value()));
  }

  co_return fail(error_kind_t::connection, "could not connect to '" + host + "': " + last_error);
}
} // namespace photon::detail
