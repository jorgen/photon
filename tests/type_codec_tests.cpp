#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <photon/decode.h>
#include <photon/params.h>
#include <photon/types.h>

using namespace photon;

namespace
{
std::string as_text(const encoded_param_t &p)
{
  return std::string(p->begin(), p->end());
}

std::vector<std::uint8_t> raw(std::initializer_list<std::uint8_t> b)
{
  return {b};
}

std::vector<std::uint8_t> be_i64(std::int64_t value)
{
  auto v = static_cast<std::uint64_t>(value);
  std::vector<std::uint8_t> out(8);
  for (int i = 0; i < 8; ++i)
  {
    out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (56 - i * 8)) & 0xFF);
  }
  return out;
}

std::vector<std::uint8_t> be_i32(std::int32_t value)
{
  auto v = static_cast<std::uint32_t>(value);
  std::vector<std::uint8_t> out(4);
  for (int i = 0; i < 4; ++i)
  {
    out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (24 - i * 8)) & 0xFF);
  }
  return out;
}
} // namespace

TEST_CASE("uuid decodes 16 raw bytes into the canonical string")
{
  auto bytes = raw({0x55, 0x0e, 0x84, 0x00, 0xe2, 0x9b, 0x41, 0xd4, 0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00});
  uuid_t value;
  REQUIRE(value_codec_t<uuid_t>::decode(bytes, value).has_value());
  CHECK(value.str() == "550e8400-e29b-41d4-a716-446655440000");
}

TEST_CASE("uuid rejects a value that is not 16 bytes")
{
  auto bytes = raw({0x55, 0x0e, 0x84, 0x00});
  uuid_t value;
  CHECK_FALSE(value_codec_t<uuid_t>::decode(bytes, value).has_value());
}

TEST_CASE("uuid parse round-trips the canonical string and encodes as text")
{
  auto parsed = uuid_t::parse("550e8400-e29b-41d4-a716-446655440000");
  REQUIRE(parsed.has_value());
  CHECK(parsed->str() == "550e8400-e29b-41d4-a716-446655440000");
  CHECK(as_text(encode_param(*parsed)) == "550e8400-e29b-41d4-a716-446655440000");
}

TEST_CASE("uuid parse rejects malformed input")
{
  CHECK_FALSE(uuid_t::parse("550e8400").has_value());
  CHECK_FALSE(uuid_t::parse("550e8400-e29b-41d4-a716-4466554400zz").has_value());
  CHECK_FALSE(uuid_t::parse("550e8400-e29b-41d4-a716-446655440000-ff").has_value());
}

TEST_CASE("bytea decodes raw bytes and encodes hex")
{
  auto bytes = raw({0xde, 0xad, 0xbe, 0xef});
  bytea_t value;
  REQUIRE(value_codec_t<bytea_t>::decode(bytes, value).has_value());
  REQUIRE(value.data.size() == 4);
  CHECK(static_cast<std::uint8_t>(value.data[0]) == 0xde);
  CHECK(static_cast<std::uint8_t>(value.data[3]) == 0xef);

  bytea_t encode_me{{std::byte{0x61}, std::byte{0x62}}};
  CHECK(as_text(encode_param(encode_me)) == "\\x6162");

  bytea_t empty;
  REQUIRE(value_codec_t<bytea_t>::decode(std::span<const std::uint8_t>{}, empty).has_value());
  CHECK(empty.data.empty());
  CHECK(as_text(encode_param(empty)) == "\\x");
}

TEST_CASE("json decodes text and strips the jsonb version byte")
{
  std::string text = "{\"a\":1}";
  std::span<const std::uint8_t> plain(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
  json_t json_value;
  REQUIRE(value_codec_t<json_t>::decode(plain, json_value).has_value());
  CHECK(json_value.value == "{\"a\":1}");

  std::vector<std::uint8_t> jsonb;
  jsonb.push_back(0x01);
  jsonb.insert(jsonb.end(), text.begin(), text.end());
  json_t jsonb_value;
  REQUIRE(value_codec_t<json_t>::decode(jsonb, jsonb_value).has_value());
  CHECK(jsonb_value.value == "{\"a\":1}");

  CHECK(as_text(encode_param(json_t{"[1,2,3]"})) == "[1,2,3]");
}

TEST_CASE("numeric decodes the binary layout to a decimal string")
{
  auto decode = [](std::initializer_list<std::uint8_t> b)
  {
    numeric_t value;
    auto bytes = std::vector<std::uint8_t>(b);
    auto ok = value_codec_t<numeric_t>::decode(bytes, value);
    REQUIRE(ok.has_value());
    return value.value;
  };

  CHECK(decode({0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x7B, 0x11, 0x94}) == "123.45");
  CHECK(decode({0x00, 0x01, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x01}) == "-1");
  CHECK(decode({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) == "0");
  CHECK(decode({0x00, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x13, 0x88}) == "0.5");
  CHECK(decode({0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x13, 0x88}) == "1.5");
  CHECK(decode({0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0xD2, 0x16, 0x2E}) == "12345678");
  CHECK(decode({0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x0A}) == "10.00");
  CHECK(decode({0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00}) == "NaN");
}

TEST_CASE("numeric rejects truncated input and a length mismatch")
{
  numeric_t value;
  auto short_header = raw({0x00, 0x02, 0x00, 0x00});
  CHECK_FALSE(value_codec_t<numeric_t>::decode(short_header, value).has_value());

  auto missing_group = raw({0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01});
  CHECK_FALSE(value_codec_t<numeric_t>::decode(missing_group, value).has_value());
}

TEST_CASE("numeric renders as a text param unchanged")
{
  CHECK(as_text(encode_param(numeric_t{"3.14159"})) == "3.14159");
}

TEST_CASE("timestamp decodes microseconds since the 2000 epoch")
{
  std::int64_t pg_micros = 662774400LL * 1000000LL;
  auto bytes = be_i64(pg_micros);
  std::chrono::system_clock::time_point tp;
  REQUIRE(value_codec_t<std::chrono::system_clock::time_point>::decode(bytes, tp).has_value());
  auto unix_us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
  CHECK(unix_us == 1609459200LL * 1000000LL);
}

TEST_CASE("timestamp encodes as ISO-8601 UTC text")
{
  using namespace std::chrono;
  auto tp = sys_days{year{2021} / 6 / 15} + hours{12} + minutes{34} + seconds{56} + microseconds{789012};
  system_clock::time_point value = tp;
  CHECK(as_text(encode_param(value)) == "2021-06-15 12:34:56.789012+00");
}

TEST_CASE("date decodes days since the 2000 epoch and encodes YYYY-MM-DD")
{
  using namespace std::chrono;
  auto pg_days = static_cast<std::int32_t>((sys_days{year{2021} / 1 / 1} - sys_days{year{2000} / 1 / 1}).count());
  auto bytes = be_i32(pg_days);
  sys_days decoded;
  REQUIRE(value_codec_t<sys_days>::decode(bytes, decoded).has_value());
  CHECK(decoded == sys_days{year{2021} / 1 / 1});

  CHECK(as_text(encode_param(sys_days{year{2021} / 6 / 15})) == "2021-06-15");
}

TEST_CASE("date decodes the infinity sentinels to saturated bounds")
{
  using namespace std::chrono;
  sys_days pos;
  auto max_bytes = be_i32(std::numeric_limits<std::int32_t>::max());
  REQUIRE(value_codec_t<sys_days>::decode(max_bytes, pos).has_value());
  CHECK(pos == sys_days::max());

  sys_days neg;
  auto min_bytes = be_i32(std::numeric_limits<std::int32_t>::min());
  REQUIRE(value_codec_t<sys_days>::decode(min_bytes, neg).has_value());
  CHECK(neg == sys_days::min());
}

TEST_CASE("date encodes BC years in the Postgres era form")
{
  using namespace std::chrono;
  CHECK(as_text(encode_param(sys_days{year{0} / 1 / 1})) == "0001-01-01 BC");
  CHECK(as_text(encode_param(sys_days{year{-44} / 3 / 15})) == "0045-03-15 BC");
  CHECK(as_text(encode_param(sys_days{year{1} / 1 / 1})) == "0001-01-01");
}

TEST_CASE("decode_field maps NULL to an empty optional for a custom type")
{
  std::optional<std::span<const std::uint8_t>> null_column = std::nullopt;
  std::optional<uuid_t> value;
  REQUIRE(decode_field(null_column, value).has_value());
  CHECK_FALSE(value.has_value());
}
