#include <doctest/doctest.h>

#include <photon/detail/message.h>
#include <photon/detail/protocol.h>
#include <photon/error.h>

using namespace photon;

namespace
{
photon::error_t server(std::string sqlstate)
{
  server_error_t s;
  s.sqlstate = std::move(sqlstate);
  return fail_server(std::move(s)).error();
}
} // namespace

TEST_CASE("sqlstate_class returns the two-character class")
{
  CHECK(sqlstate_class("23505") == "23");
  CHECK(sqlstate_class("08006") == "08");
  CHECK(sqlstate_class("").empty());
  CHECK(sqlstate_class("4") == "4");
}

TEST_CASE("sqlstate classification helpers match the right codes")
{
  CHECK(is_unique_violation(server("23505")));
  CHECK(is_foreign_key_violation(server("23503")));
  CHECK(is_not_null_violation(server("23502")));
  CHECK(is_check_violation(server("23514")));
  CHECK(is_integrity_constraint_violation(server("23505")));
  CHECK(is_integrity_constraint_violation(server("23000")));
  CHECK(is_undefined_table(server("42P01")));
  CHECK(is_undefined_column(server("42703")));
  CHECK(is_syntax_error_or_access_rule_violation(server("42601")));
  CHECK(is_serialization_failure(server("40001")));
  CHECK(is_deadlock_detected(server("40P01")));
  CHECK(is_connection_exception(server("08006")));
  CHECK(is_insufficient_resources(server("53300")));
  CHECK(is_admin_shutdown(server("57P01")));

  CHECK_FALSE(is_unique_violation(server("23503")));
  CHECK_FALSE(is_undefined_table(server("42703")));
  CHECK_FALSE(is_integrity_constraint_violation(server("42P01")));
}

TEST_CASE("classification helpers are false for a non-server error")
{
  auto err = fail(error_kind_t::connection, "no route to host").error();
  CHECK_FALSE(err.server.has_value());
  CHECK(err.sqlstate().empty());
  CHECK_FALSE(is_unique_violation(err));
  CHECK_FALSE(is_connection_exception(err));
}

TEST_CASE("to_server_error maps all ErrorResponse fields")
{
  detail::wire_writer_t w('E');
  w.u8('S').cstr("ERROR");
  w.u8('V').cstr("ERROR");
  w.u8('C').cstr("23505");
  w.u8('M').cstr("duplicate key value violates unique constraint \"users_pkey\"");
  w.u8('D').cstr("Key (id)=(1) already exists.");
  w.u8('H').cstr("try another id");
  w.u8('P').cstr("42");
  w.u8('W').cstr("in a trigger");
  w.u8('s').cstr("public");
  w.u8('t').cstr("users");
  w.u8('c').cstr("id");
  w.u8('d').cstr("int4");
  w.u8('n').cstr("users_pkey");
  w.u8('R').cstr("_bt_check_unique");
  w.u8(0);
  auto framed = w.finish();
  std::span<const std::uint8_t> body(framed.begin() + 5, framed.end());

  auto parsed = detail::parse_error_response(body);
  REQUIRE(parsed.has_value());
  auto server_error = detail::to_server_error(*parsed);

  CHECK(server_error.severity == "ERROR");
  CHECK(server_error.sqlstate == "23505");
  CHECK(server_error.message == "duplicate key value violates unique constraint \"users_pkey\"");
  CHECK(server_error.detail == "Key (id)=(1) already exists.");
  CHECK(server_error.hint == "try another id");
  CHECK(server_error.position == "42");
  CHECK(server_error.where == "in a trigger");
  CHECK(server_error.schema == "public");
  CHECK(server_error.table == "users");
  CHECK(server_error.column == "id");
  CHECK(server_error.data_type == "int4");
  CHECK(server_error.constraint == "users_pkey");
  CHECK(server_error.routine == "_bt_check_unique");

  auto err = fail_server(server_error).error();
  CHECK(is_unique_violation(err));
  CHECK(err.server->constraint == "users_pkey");
}
