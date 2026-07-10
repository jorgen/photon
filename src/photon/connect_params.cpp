#include "photon/connect_params.h"

#include <charconv>
#include <optional>

namespace photon
{
namespace
{
int hex_value(char c)
{
  if (c >= '0' && c <= '9')
  {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f')
  {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F')
  {
    return c - 'A' + 10;
  }
  return -1;
}

result_t<std::string> percent_decode(std::string_view in)
{
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i)
  {
    if (in[i] == '%')
    {
      if (i + 2 >= in.size())
      {
        return fail(error_kind_t::connection, "truncated percent-escape in DSN");
      }
      int hi = hex_value(in[i + 1]);
      int lo = hex_value(in[i + 2]);
      if (hi < 0 || lo < 0)
      {
        return fail(error_kind_t::connection, "invalid percent-escape in DSN");
      }
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    }
    else
    {
      out.push_back(in[i]);
    }
  }
  return out;
}

result_t<sslmode_t> parse_sslmode(std::string_view value)
{
  if (value == "disable")
  {
    return sslmode_t::disable;
  }
  if (value == "allow")
  {
    return sslmode_t::allow;
  }
  if (value == "prefer")
  {
    return sslmode_t::prefer;
  }
  if (value == "require")
  {
    return sslmode_t::require;
  }
  if (value == "verify-ca")
  {
    return sslmode_t::verify_ca;
  }
  if (value == "verify-full")
  {
    return sslmode_t::verify_full;
  }
  return fail(error_kind_t::connection, "unknown sslmode '" + std::string(value) + "'");
}

result_t<void> apply_option(connect_params_t &params, std::string_view key, std::string_view value)
{
  if (key == "sslmode")
  {
    auto mode = parse_sslmode(value);
    if (!mode.has_value())
    {
      return std::unexpected(mode.error());
    }
    params.sslmode = *mode;
    return {};
  }
  if (key == "connect_timeout")
  {
    long seconds = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (ec != std::errc{} || ptr != value.data() + value.size() || seconds < 0)
    {
      return fail(error_kind_t::connection, "invalid connect_timeout in DSN");
    }
    params.connect_timeout = std::chrono::seconds{seconds};
    return {};
  }
  if (key == "dbname")
  {
    params.database = std::string(value);
    return {};
  }
  if (key == "user")
  {
    params.user = std::string(value);
    return {};
  }
  if (key == "password")
  {
    params.password = std::string(value);
    return {};
  }
  params.options.emplace_back(std::string(key), std::string(value));
  return {};
}
} // namespace

result_t<connect_params_t> parse_dsn(std::string_view dsn)
{
  std::string_view rest = dsn;
  if (rest.starts_with("postgresql://"))
  {
    rest.remove_prefix(std::string_view("postgresql://").size());
  }
  else if (rest.starts_with("postgres://"))
  {
    rest.remove_prefix(std::string_view("postgres://").size());
  }
  else
  {
    return fail(error_kind_t::connection, "DSN must start with postgresql:// or postgres://");
  }

  connect_params_t params;

  std::string_view query;
  if (auto q = rest.find('?'); q != std::string_view::npos)
  {
    query = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }

  std::string_view authority = rest;
  std::string_view path;
  if (auto slash = rest.find('/'); slash != std::string_view::npos)
  {
    authority = rest.substr(0, slash);
    path = rest.substr(slash + 1);
  }

  std::string_view host_port = authority;
  if (auto at = authority.rfind('@'); at != std::string_view::npos)
  {
    std::string_view userinfo = authority.substr(0, at);
    host_port = authority.substr(at + 1);
    std::string_view user_part = userinfo;
    if (auto colon = userinfo.find(':'); colon != std::string_view::npos)
    {
      user_part = userinfo.substr(0, colon);
      auto pw = percent_decode(userinfo.substr(colon + 1));
      if (!pw.has_value())
      {
        return std::unexpected(pw.error());
      }
      params.password = *pw;
    }
    auto user = percent_decode(user_part);
    if (!user.has_value())
    {
      return std::unexpected(user.error());
    }
    params.user = *user;
  }

  auto parse_port = [&params](std::string_view port_text) -> result_t<void>
  {
    std::uint16_t port = 0;
    auto [ptr, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (ec != std::errc{} || ptr != port_text.data() + port_text.size())
    {
      return fail(error_kind_t::connection, "invalid port in DSN");
    }
    params.port = port;
    return {};
  };

  if (!host_port.empty())
  {
    if (host_port.front() == '[')
    {
      auto close = host_port.find(']');
      if (close == std::string_view::npos)
      {
        return fail(error_kind_t::connection, "unterminated IPv6 host in DSN");
      }
      auto host = percent_decode(host_port.substr(1, close - 1));
      if (!host.has_value())
      {
        return std::unexpected(host.error());
      }
      params.host = *host;
      std::string_view after = host_port.substr(close + 1);
      if (!after.empty())
      {
        if (after.front() != ':')
        {
          return fail(error_kind_t::connection, "expected ':port' after IPv6 host in DSN");
        }
        auto ok = parse_port(after.substr(1));
        if (!ok.has_value())
        {
          return std::unexpected(ok.error());
        }
      }
    }
    else
    {
      std::string_view host_part = host_port;
      std::string_view port_part;
      if (auto colon = host_port.rfind(':'); colon != std::string_view::npos)
      {
        host_part = host_port.substr(0, colon);
        port_part = host_port.substr(colon + 1);
      }
      if (!host_part.empty())
      {
        auto host = percent_decode(host_part);
        if (!host.has_value())
        {
          return std::unexpected(host.error());
        }
        params.host = *host;
      }
      if (!port_part.empty())
      {
        auto ok = parse_port(port_part);
        if (!ok.has_value())
        {
          return std::unexpected(ok.error());
        }
      }
    }
  }

  if (!path.empty())
  {
    auto database = percent_decode(path);
    if (!database.has_value())
    {
      return std::unexpected(database.error());
    }
    params.database = *database;
  }

  std::size_t pos = 0;
  while (pos < query.size())
  {
    std::size_t amp = query.find('&', pos);
    std::string_view pair = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
    std::size_t eq = pair.find('=');
    std::string_view key = eq == std::string_view::npos ? pair : pair.substr(0, eq);
    std::string_view raw_value = eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
    auto value = percent_decode(raw_value);
    if (!value.has_value())
    {
      return std::unexpected(value.error());
    }
    auto applied = apply_option(params, key, *value);
    if (!applied.has_value())
    {
      return std::unexpected(applied.error());
    }
    if (amp == std::string_view::npos)
    {
      break;
    }
    pos = amp + 1;
  }

  if (params.user.empty())
  {
    return fail(error_kind_t::connection, "DSN is missing a user");
  }
  if (params.database.empty())
  {
    params.database = params.user;
  }
  return params;
}
} // namespace photon
