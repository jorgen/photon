#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <photon/detail/message.h>
#include <photon/detail/protocol.h>

using namespace photon::detail;

namespace
{
std::span<const std::uint8_t> body_of(const std::vector<std::uint8_t> &framed, bool typed)
{
  std::size_t header = typed ? 5 : 4;
  return std::span<const std::uint8_t>(framed).subspan(header);
}

std::int32_t length_of(const std::vector<std::uint8_t> &framed, std::size_t offset)
{
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(framed[offset]) << 24 | static_cast<std::uint32_t>(framed[offset + 1]) << 16 | static_cast<std::uint32_t>(framed[offset + 2]) << 8 | static_cast<std::uint32_t>(framed[offset + 3]));
}
} // namespace

TEST_CASE("startup message frames protocol version and parameters")
{
  auto msg = startup_message("alice", "shop");
  CHECK(length_of(msg, 0) == static_cast<std::int32_t>(msg.size()));
  wire_reader_t r(body_of(msg, false));
  CHECK(*r.i32() == protocol_version_3);
  CHECK(*r.cstr() == "user");
  CHECK(*r.cstr() == "alice");
  CHECK(*r.cstr() == "database");
  CHECK(*r.cstr() == "shop");
  CHECK(*r.u8() == 0);
  CHECK(r.empty());
}

TEST_CASE("ssl request is a fixed 8-byte untyped message")
{
  auto msg = ssl_request_message();
  REQUIRE(msg.size() == 8);
  CHECK(length_of(msg, 0) == 8);
  wire_reader_t r(body_of(msg, false));
  CHECK(*r.i32() == ssl_request_code);
}

TEST_CASE("query message carries a type byte and a length that includes itself")
{
  auto msg = query_message("SELECT 1");
  CHECK(static_cast<char>(msg[0]) == 'Q');
  CHECK(length_of(msg, 1) == static_cast<std::int32_t>(msg.size() - 1));
  wire_reader_t r(body_of(msg, true));
  CHECK(*r.cstr() == "SELECT 1");
}

TEST_CASE("bind message serializes formats, params and result formats")
{
  std::vector<std::int16_t> param_formats{format_binary};
  std::vector<encoded_param_t> params{std::vector<std::uint8_t>{0, 0, 0, 42}, std::nullopt};
  std::vector<std::int16_t> result_formats{format_binary};
  auto msg = bind_message("", "s1", param_formats, params, result_formats);
  CHECK(static_cast<char>(msg[0]) == 'B');
  wire_reader_t r(body_of(msg, true));
  CHECK(*r.cstr() == "");
  CHECK(*r.cstr() == "s1");
  CHECK(*r.i16() == 1);
  CHECK(*r.i16() == format_binary);
  CHECK(*r.i16() == 2);
  CHECK(*r.i32() == 4);
  auto v = r.bytes(4);
  REQUIRE(v.has_value());
  CHECK((*v)[3] == 42);
  CHECK(*r.i32() == -1);
  CHECK(*r.i16() == 1);
  CHECK(*r.i16() == format_binary);
}

TEST_CASE("authentication parse extracts subtype and trailing data")
{
  wire_writer_t w('R');
  w.i32(static_cast<std::int32_t>(auth_request_t::md5_password));
  std::uint8_t salt[4] = {0xde, 0xad, 0xbe, 0xef};
  w.bytes(salt);
  auto framed = w.finish();
  auto parsed = parse_authentication(body_of(framed, true));
  REQUIRE(parsed.has_value());
  CHECK(parsed->type == auth_request_t::md5_password);
  REQUIRE(parsed->data.size() == 4);
  CHECK(parsed->data[0] == 0xde);
  CHECK(parsed->data[3] == 0xef);
}

TEST_CASE("row description round-trips a field")
{
  wire_writer_t w('T');
  w.i16(1);
  w.cstr("id").i32(16385).i16(1).i32(23).i16(4).i32(-1).i16(format_binary);
  auto framed = w.finish();
  auto parsed = parse_row_description(body_of(framed, true));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->fields.size() == 1);
  const auto &f = parsed->fields[0];
  CHECK(f.name == "id");
  CHECK(f.table_oid == 16385);
  CHECK(f.column_id == 1);
  CHECK(f.type_oid == 23);
  CHECK(f.type_size == 4);
  CHECK(f.type_modifier == -1);
  CHECK(f.format == format_binary);
}

TEST_CASE("data row parses null and non-null columns as spans")
{
  wire_writer_t w('D');
  w.i16(2);
  w.i32(3).raw("abc");
  w.i32(-1);
  auto framed = w.finish();
  auto parsed = parse_data_row(body_of(framed, true));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->columns.size() == 2);
  REQUIRE(parsed->columns[0].has_value());
  CHECK(parsed->columns[0]->size() == 3);
  CHECK((*parsed->columns[0])[0] == 'a');
  CHECK_FALSE(parsed->columns[1].has_value());
}

TEST_CASE("error response collects fields with accessors")
{
  wire_writer_t w('E');
  w.u8('S').cstr("FATAL");
  w.u8('C').cstr("28P01");
  w.u8('M').cstr("password authentication failed");
  w.u8(0);
  auto framed = w.finish();
  auto parsed = parse_error_response(body_of(framed, true));
  REQUIRE(parsed.has_value());
  CHECK(parsed->severity() == "FATAL");
  CHECK(parsed->sqlstate() == "28P01");
  CHECK(parsed->message() == "password authentication failed");
}

TEST_CASE("ready for query reports the transaction status")
{
  wire_writer_t w('Z');
  w.u8('T');
  auto framed = w.finish();
  auto parsed = parse_ready_for_query(body_of(framed, true));
  REQUIRE(parsed.has_value());
  CHECK(parsed->transaction_status == 'T');
}
