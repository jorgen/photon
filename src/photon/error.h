#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace photon
{
enum class error_kind_t : std::uint8_t
{
  connection,
  protocol,
  auth,
  server,
  decode,
  timeout,
};

struct error_t
{
  error_kind_t kind = error_kind_t::protocol;
  std::string sqlstate;
  std::string msg;
};

template <typename T>
using result_t = std::expected<T, error_t>;

inline std::unexpected<error_t> fail(error_kind_t kind, std::string msg)
{
  return std::unexpected(error_t{kind, std::string{}, std::move(msg)});
}

inline std::unexpected<error_t> fail_server(std::string sqlstate, std::string msg)
{
  return std::unexpected(error_t{error_kind_t::server, std::move(sqlstate), std::move(msg)});
}
} // namespace photon
