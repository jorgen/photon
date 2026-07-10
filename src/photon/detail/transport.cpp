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

vio::task_t<result_t<std::optional<vio::tcp_t>>> connect_raw_tcp(vio::event_loop_t &loop, const std::string &host, std::uint16_t port, std::chrono::milliseconds timeout)
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
    co_return std::optional<vio::tcp_t>(std::move(client.value()));
  }

  co_return fail(error_kind_t::connection, "could not connect to '" + host + "': " + last_error);
}

result_t<std::unique_ptr<transport_t>> make_tcp_transport(vio::tcp_t tcp)
{
  auto reader = vio::tcp_create_reader(tcp);
  if (!reader.has_value())
  {
    return fail(error_kind_t::connection, "failed to create socket reader: " + reader.error().msg);
  }
  return std::make_unique<tcp_transport_t>(std::move(tcp), std::move(reader.value()));
}

vio::task_t<result_t<std::unique_ptr<transport_t>>> upgrade_to_tls(vio::tcp_t tcp, vio::ssl_config_t config, std::string host)
{
  auto upgraded = co_await vio::ssl_client_upgrade(std::move(tcp), config, host);
  if (!upgraded.has_value())
  {
    co_return fail(error_kind_t::connection, "TLS handshake failed: " + upgraded.error().msg);
  }

  auto reader = vio::ssl_client_create_reader(upgraded.value());
  if (!reader.has_value())
  {
    co_return fail(error_kind_t::connection, "failed to create TLS reader: " + reader.error().msg);
  }
  co_return std::make_unique<tls_transport_t>(std::move(upgraded.value()), std::move(reader.value()));
}
} // namespace photon::detail
