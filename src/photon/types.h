#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "photon/decode.h"
#include "photon/error.h"
#include "photon/params.h"

namespace photon
{
struct uuid_t
{
  std::array<std::uint8_t, 16> bytes{};

  [[nodiscard]] std::string str() const;
  static result_t<uuid_t> parse(std::string_view text);

  bool operator==(const uuid_t &) const = default;
};

struct bytea_t
{
  std::vector<std::byte> data;

  bool operator==(const bytea_t &) const = default;
};

struct json_t
{
  std::string value;

  bool operator==(const json_t &) const = default;
};

struct numeric_t
{
  std::string value;

  bool operator==(const numeric_t &) const = default;
};

namespace detail
{
inline constexpr char hex_lower[] = "0123456789abcdef";

inline result_t<int> hex_nibble(char c)
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
  return fail(error_kind_t::decode, "invalid hexadecimal digit");
}

inline void append_uint_padded(std::string &out, unsigned long long value, int width)
{
  char buffer[24];
  int i = static_cast<int>(sizeof(buffer));
  do
  {
    buffer[--i] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  int digits = static_cast<int>(sizeof(buffer)) - i;
  for (int p = digits; p < width; ++p)
  {
    out.push_back('0');
  }
  out.append(buffer + i, static_cast<std::size_t>(digits));
}

inline void append_year(std::string &out, int year)
{
  if (year < 0)
  {
    out.push_back('-');
    append_uint_padded(out, static_cast<unsigned long long>(-static_cast<long long>(year)), 4);
  }
  else
  {
    append_uint_padded(out, static_cast<unsigned long long>(year), 4);
  }
}

inline std::chrono::system_clock::time_point micros_to_system_time(std::int64_t micros)
{
  using sc = std::chrono::system_clock;
  using dur = sc::duration;
  constexpr std::int64_t max_us = std::chrono::duration_cast<std::chrono::microseconds>(dur::max()).count();
  constexpr std::int64_t min_us = std::chrono::duration_cast<std::chrono::microseconds>(dur::min()).count();
  if (micros > max_us)
  {
    return sc::time_point(dur::max());
  }
  if (micros < min_us)
  {
    return sc::time_point(dur::min());
  }
  return sc::time_point(std::chrono::duration_cast<dur>(std::chrono::microseconds(micros)));
}
} // namespace detail

inline std::string uuid_t::str() const
{
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < bytes.size(); ++i)
  {
    out.push_back(detail::hex_lower[bytes[i] >> 4]);
    out.push_back(detail::hex_lower[bytes[i] & 0x0F]);
    if (i == 3 || i == 5 || i == 7 || i == 9)
    {
      out.push_back('-');
    }
  }
  return out;
}

inline result_t<uuid_t> uuid_t::parse(std::string_view text)
{
  uuid_t out;
  std::size_t written = 0;
  int high = -1;
  for (char c : text)
  {
    if (c == '-')
    {
      continue;
    }
    auto nibble = detail::hex_nibble(c);
    if (!nibble.has_value())
    {
      return std::unexpected(nibble.error());
    }
    if (high < 0)
    {
      high = *nibble;
      continue;
    }
    if (written >= out.bytes.size())
    {
      return fail(error_kind_t::decode, "uuid has too many hex digits");
    }
    out.bytes[written++] = static_cast<std::uint8_t>((high << 4) | *nibble);
    high = -1;
  }
  if (high >= 0)
  {
    return fail(error_kind_t::decode, "uuid has an odd number of hex digits");
  }
  if (written != out.bytes.size())
  {
    return fail(error_kind_t::decode, "uuid must contain 16 bytes");
  }
  return out;
}

template <>
struct value_codec_t<uuid_t>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, uuid_t &out)
  {
    if (bytes.size() != out.bytes.size())
    {
      return fail(error_kind_t::decode, "uuid binary value must be 16 bytes");
    }
    std::copy(bytes.begin(), bytes.end(), out.bytes.begin());
    return {};
  }
};

template <>
struct value_codec_t<bytea_t>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, bytea_t &out)
  {
    out.data.resize(bytes.size());
    if (!bytes.empty())
    {
      std::memcpy(out.data.data(), bytes.data(), bytes.size());
    }
    return {};
  }
};

template <>
struct value_codec_t<json_t>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, json_t &out)
  {
    std::span<const std::uint8_t> text = bytes;
    if (!text.empty() && text[0] == 0x01)
    {
      text = text.subspan(1);
    }
    out.value.assign(reinterpret_cast<const char *>(text.data()), text.size());
    return {};
  }
};

template <>
struct value_codec_t<numeric_t>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, numeric_t &out)
  {
    if (bytes.size() < 8)
    {
      return fail(error_kind_t::decode, "numeric binary value is truncated");
    }
    auto rd16 = [&](std::size_t off) { return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[off]) << 8 | static_cast<std::uint16_t>(bytes[off + 1])); };
    auto ndigits = static_cast<std::int16_t>(rd16(0));
    auto weight = static_cast<std::int16_t>(rd16(2));
    std::uint16_t sign = rd16(4);
    auto dscale = static_cast<std::int16_t>(rd16(6));

    if (sign == 0xC000)
    {
      out.value = "NaN";
      return {};
    }
    if (sign == 0xD000)
    {
      out.value = "Infinity";
      return {};
    }
    if (sign == 0xF000)
    {
      out.value = "-Infinity";
      return {};
    }
    if (sign != 0x0000 && sign != 0x4000)
    {
      return fail(error_kind_t::decode, "numeric has an invalid sign");
    }
    if (ndigits < 0 || dscale < 0)
    {
      return fail(error_kind_t::decode, "numeric has invalid metadata");
    }
    if (bytes.size() != static_cast<std::size_t>(8 + static_cast<int>(ndigits) * 2))
    {
      return fail(error_kind_t::decode, "numeric length does not match ndigits");
    }

    std::vector<int> digits(static_cast<std::size_t>(ndigits));
    for (int i = 0; i < ndigits; ++i)
    {
      std::uint16_t group = rd16(static_cast<std::size_t>(8 + i * 2));
      if (group > 9999)
      {
        return fail(error_kind_t::decode, "numeric digit group out of range");
      }
      digits[static_cast<std::size_t>(i)] = static_cast<int>(group);
    }

    auto append_group = [](std::string &s, int value)
    {
      char buffer[4];
      for (int k = 3; k >= 0; --k)
      {
        buffer[k] = static_cast<char>('0' + value % 10);
        value /= 10;
      }
      s.append(buffer, 4);
    };

    std::string result;
    if (sign == 0x4000)
    {
      result.push_back('-');
    }

    int d = 0;
    if (weight < 0)
    {
      result.push_back('0');
      d = weight + 1;
    }
    else
    {
      for (d = 0; d <= weight; ++d)
      {
        int group = (d < ndigits) ? digits[static_cast<std::size_t>(d)] : 0;
        if (d > 0)
        {
          append_group(result, group);
        }
        else
        {
          result += std::to_string(group);
        }
      }
    }

    if (dscale > 0)
    {
      result.push_back('.');
      std::string fraction;
      for (int i = 0; i < dscale; ++d, i += 4)
      {
        int group = (d >= 0 && d < ndigits) ? digits[static_cast<std::size_t>(d)] : 0;
        append_group(fraction, group);
      }
      fraction.resize(static_cast<std::size_t>(dscale));
      result += fraction;
    }

    out.value = std::move(result);
    return {};
  }
};

template <>
struct value_codec_t<std::chrono::system_clock::time_point>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, std::chrono::system_clock::time_point &out)
  {
    auto raw = detail::read_be<std::uint64_t>(bytes);
    if (!raw.has_value())
    {
      return std::unexpected(raw.error());
    }
    auto pg_micros = static_cast<std::int64_t>(*raw);
    if (pg_micros == std::numeric_limits<std::int64_t>::max())
    {
      out = std::chrono::system_clock::time_point::max();
      return {};
    }
    if (pg_micros == std::numeric_limits<std::int64_t>::min())
    {
      out = std::chrono::system_clock::time_point::min();
      return {};
    }
    constexpr std::int64_t epoch_offset_micros = 946684800LL * 1000000LL;
    std::int64_t micros = (pg_micros > std::numeric_limits<std::int64_t>::max() - epoch_offset_micros) ? std::numeric_limits<std::int64_t>::max() : pg_micros + epoch_offset_micros;
    out = detail::micros_to_system_time(micros);
    return {};
  }
};

template <>
struct value_codec_t<std::chrono::sys_days>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, std::chrono::sys_days &out)
  {
    auto raw = detail::read_be<std::uint32_t>(bytes);
    if (!raw.has_value())
    {
      return std::unexpected(raw.error());
    }
    auto pg_days = static_cast<std::int32_t>(*raw);
    if (pg_days == std::numeric_limits<std::int32_t>::max())
    {
      out = std::chrono::sys_days::max();
      return {};
    }
    if (pg_days == std::numeric_limits<std::int32_t>::min())
    {
      out = std::chrono::sys_days::min();
      return {};
    }
    constexpr std::int64_t epoch_offset_days = 10957;
    std::int64_t total = static_cast<std::int64_t>(pg_days) + epoch_offset_days;
    out = std::chrono::sys_days{std::chrono::days{static_cast<std::chrono::days::rep>(total)}};
    return {};
  }
};

template <>
struct param_codec_t<uuid_t>
{
  static encoded_param_t encode(const uuid_t &value)
  {
    return detail::text_bytes(value.str());
  }
};

template <>
struct param_codec_t<bytea_t>
{
  static encoded_param_t encode(const bytea_t &value)
  {
    std::string out = "\\x";
    out.reserve(2 + value.data.size() * 2);
    for (std::byte byte : value.data)
    {
      auto v = static_cast<std::uint8_t>(byte);
      out.push_back(detail::hex_lower[v >> 4]);
      out.push_back(detail::hex_lower[v & 0x0F]);
    }
    return detail::text_bytes(out);
  }
};

template <>
struct param_codec_t<json_t>
{
  static encoded_param_t encode(const json_t &value)
  {
    return detail::text_bytes(value.value);
  }
};

template <>
struct param_codec_t<numeric_t>
{
  static encoded_param_t encode(const numeric_t &value)
  {
    return detail::text_bytes(value.value);
  }
};

template <>
struct param_codec_t<std::chrono::system_clock::time_point>
{
  static encoded_param_t encode(std::chrono::system_clock::time_point value)
  {
    auto seconds = std::chrono::floor<std::chrono::seconds>(value);
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(value - seconds).count();
    auto days = std::chrono::floor<std::chrono::days>(seconds);
    std::chrono::hh_mm_ss<std::chrono::seconds> hms{seconds - days};
    std::chrono::year_month_day ymd{days};

    std::string out;
    detail::append_year(out, static_cast<int>(ymd.year()));
    out.push_back('-');
    detail::append_uint_padded(out, static_cast<unsigned>(ymd.month()), 2);
    out.push_back('-');
    detail::append_uint_padded(out, static_cast<unsigned>(ymd.day()), 2);
    out.push_back(' ');
    detail::append_uint_padded(out, static_cast<unsigned long long>(hms.hours().count()), 2);
    out.push_back(':');
    detail::append_uint_padded(out, static_cast<unsigned long long>(hms.minutes().count()), 2);
    out.push_back(':');
    detail::append_uint_padded(out, static_cast<unsigned long long>(hms.seconds().count()), 2);
    out.push_back('.');
    detail::append_uint_padded(out, static_cast<unsigned long long>(micros), 6);
    out += "+00";
    return detail::text_bytes(out);
  }
};

template <>
struct param_codec_t<std::chrono::sys_days>
{
  static encoded_param_t encode(std::chrono::sys_days value)
  {
    std::chrono::year_month_day ymd{value};
    int year = static_cast<int>(ymd.year());
    bool is_bc = year <= 0;
    std::string out;
    detail::append_uint_padded(out, static_cast<unsigned long long>(is_bc ? 1 - year : year), 4);
    out.push_back('-');
    detail::append_uint_padded(out, static_cast<unsigned>(ymd.month()), 2);
    out.push_back('-');
    detail::append_uint_padded(out, static_cast<unsigned>(ymd.day()), 2);
    if (is_bc)
    {
      out += " BC";
    }
    return detail::text_bytes(out);
  }
};
} // namespace photon
