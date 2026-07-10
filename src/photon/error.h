#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
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

struct server_error_t
{
  std::string severity;
  std::string sqlstate;
  std::string message;
  std::string detail;
  std::string hint;
  std::string position;
  std::string where;
  std::string schema;
  std::string table;
  std::string column;
  std::string data_type;
  std::string constraint;
  std::string routine;
};

struct error_t
{
  error_kind_t kind = error_kind_t::protocol;
  std::string msg;
  std::optional<server_error_t> server;

  [[nodiscard]] std::string_view sqlstate() const
  {
    return server.has_value() ? std::string_view(server->sqlstate) : std::string_view{};
  }
};

template <typename T>
using result_t = std::expected<T, error_t>;

inline std::unexpected<error_t> fail(error_kind_t kind, std::string msg)
{
  return std::unexpected(error_t{kind, std::move(msg), std::nullopt});
}

inline std::unexpected<error_t> fail_server(server_error_t server)
{
  std::string message = server.message;
  return std::unexpected(error_t{error_kind_t::server, std::move(message), std::move(server)});
}

inline std::string_view sqlstate_class(std::string_view sqlstate)
{
  return sqlstate.substr(0, std::min<std::size_t>(2, sqlstate.size()));
}

inline bool sqlstate_is(const error_t &error, std::string_view code)
{
  return error.sqlstate() == code;
}

inline bool sqlstate_in_class(const error_t &error, std::string_view class_code)
{
  return sqlstate_class(error.sqlstate()) == class_code;
}

inline bool is_unique_violation(const error_t &error)
{
  return sqlstate_is(error, "23505");
}
inline bool is_foreign_key_violation(const error_t &error)
{
  return sqlstate_is(error, "23503");
}
inline bool is_not_null_violation(const error_t &error)
{
  return sqlstate_is(error, "23502");
}
inline bool is_check_violation(const error_t &error)
{
  return sqlstate_is(error, "23514");
}
inline bool is_integrity_constraint_violation(const error_t &error)
{
  return sqlstate_in_class(error, "23");
}
inline bool is_undefined_table(const error_t &error)
{
  return sqlstate_is(error, "42P01");
}
inline bool is_undefined_column(const error_t &error)
{
  return sqlstate_is(error, "42703");
}
inline bool is_syntax_error_or_access_rule_violation(const error_t &error)
{
  return sqlstate_in_class(error, "42");
}
inline bool is_serialization_failure(const error_t &error)
{
  return sqlstate_is(error, "40001");
}
inline bool is_deadlock_detected(const error_t &error)
{
  return sqlstate_is(error, "40P01");
}
inline bool is_connection_exception(const error_t &error)
{
  return sqlstate_in_class(error, "08");
}
inline bool is_insufficient_resources(const error_t &error)
{
  return sqlstate_in_class(error, "53");
}
inline bool is_admin_shutdown(const error_t &error)
{
  return sqlstate_is(error, "57P01");
}
} // namespace photon
