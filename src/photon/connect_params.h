#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photon/error.h"

namespace photon
{
enum class sslmode_t : std::uint8_t
{
  disable,
  allow,
  prefer,
  require,
  verify_ca,
  verify_full,
};

struct connect_params_t
{
  std::string host = "localhost";
  std::uint16_t port = 5432;
  std::string database;
  std::string user;
  std::string password;
  sslmode_t sslmode = sslmode_t::prefer;
  std::string sslrootcert;
  std::chrono::milliseconds connect_timeout = std::chrono::seconds{10};
  std::chrono::milliseconds query_timeout = std::chrono::milliseconds{0};
  std::vector<std::pair<std::string, std::string>> options;
};

result_t<connect_params_t> parse_dsn(std::string_view dsn);
} // namespace photon
