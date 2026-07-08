#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <photon/decode.h>
#include <photon/params.h>

using namespace photon;

namespace
{
std::string as_text(const encoded_param_t &p)
{
  return std::string(p->begin(), p->end());
}

std::vector<std::uint8_t> be(std::initializer_list<std::uint8_t> b)
{
  return {b};
}
} // namespace

TEST_CASE("param codec renders integers, bools and floats as text")
{
  CHECK(as_text(encode_param(std::int32_t{42})) == "42");
  CHECK(as_text(encode_param(std::int64_t{-9000000000})) == "-9000000000");
  CHECK(as_text(encode_param(true)) == "t");
  CHECK(as_text(encode_param(false)) == "f");
  CHECK(as_text(encode_param(1.5)) == "1.5");
}

TEST_CASE("param codec renders strings and string literals")
{
  CHECK(as_text(encode_param(std::string("hello"))) == "hello");
  CHECK(as_text(encode_param(std::string_view("world"))) == "world");
  CHECK(as_text(encode_param("literal")) == "literal");
}

TEST_CASE("optional param encodes NULL as absent")
{
  std::optional<int> none;
  std::optional<int> some = 7;
  CHECK_FALSE(encode_param(none).has_value());
  CHECK(as_text(encode_param(some)) == "7");
}

TEST_CASE("value codec decodes big-endian integers")
{
  std::int32_t i32 = 0;
  auto bytes = be({0x00, 0x00, 0x00, 0x2a});
  REQUIRE(value_codec_t<std::int32_t>::decode(bytes, i32).has_value());
  CHECK(i32 == 42);

  std::int16_t i16 = 0;
  auto b16 = be({0xff, 0xfe});
  REQUIRE(value_codec_t<std::int16_t>::decode(b16, i16).has_value());
  CHECK(i16 == -2);

  std::int64_t i64 = 0;
  auto b64 = be({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00});
  REQUIRE(value_codec_t<std::int64_t>::decode(b64, i64).has_value());
  CHECK(i64 == 256);
}

TEST_CASE("value codec decodes bool and string")
{
  bool b = false;
  auto one = be({0x01});
  REQUIRE(value_codec_t<bool>::decode(one, b).has_value());
  CHECK(b);

  std::string s;
  std::string src = "photon";
  std::span<const std::uint8_t> sv(reinterpret_cast<const std::uint8_t *>(src.data()), src.size());
  REQUIRE(value_codec_t<std::string>::decode(sv, s).has_value());
  CHECK(s == "photon");
}

TEST_CASE("value codec rejects wrong-width integer input")
{
  std::int32_t i32 = 0;
  auto three = be({0x00, 0x00, 0x2a});
  CHECK_FALSE(value_codec_t<std::int32_t>::decode(three, i32).has_value());
}

TEST_CASE("decode_field maps NULL to empty optional and rejects NULL for non-optional")
{
  std::optional<std::span<const std::uint8_t>> null_col = std::nullopt;

  std::optional<std::int32_t> opt = 99;
  REQUIRE(decode_field(null_col, opt).has_value());
  CHECK_FALSE(opt.has_value());

  std::int32_t plain = 0;
  CHECK_FALSE(decode_field(null_col, plain).has_value());
}

TEST_CASE("decode_field decodes a present value into an optional")
{
  auto bytes = be({0x00, 0x00, 0x00, 0x07});
  std::optional<std::span<const std::uint8_t>> col = std::span<const std::uint8_t>(bytes);
  std::optional<std::int32_t> opt;
  REQUIRE(decode_field(col, opt).has_value());
  REQUIRE(opt.has_value());
  CHECK(*opt == 7);
}
