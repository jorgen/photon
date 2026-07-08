#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>

#include "photon/error.h"

namespace photon
{
template <typename T>
struct is_optional_t : std::false_type
{
};

template <typename T>
struct is_optional_t<std::optional<T>> : std::true_type
{
};

template <typename T>
inline constexpr bool is_optional_v = is_optional_t<T>::value;

namespace detail
{
template <typename Uint>
result_t<Uint> read_be(std::span<const std::uint8_t> bytes)
{
  if (bytes.size() != sizeof(Uint))
  {
    return fail(error_kind_t::decode, "binary value has unexpected width");
  }
  Uint value = 0;
  for (std::size_t i = 0; i < sizeof(Uint); ++i)
  {
    value = static_cast<Uint>(value << 8) | static_cast<Uint>(bytes[i]);
  }
  return value;
}
} // namespace detail

template <typename T, typename Enable = void>
struct value_codec_t;

template <>
struct value_codec_t<std::int16_t>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, std::int16_t &out)
  {
    auto v = detail::read_be<std::uint16_t>(bytes);
    if (!v.has_value())
    {
      return std::unexpected(v.error());
    }
    out = static_cast<std::int16_t>(*v);
    return {};
  }
};

template <>
struct value_codec_t<std::int32_t>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, std::int32_t &out)
  {
    auto v = detail::read_be<std::uint32_t>(bytes);
    if (!v.has_value())
    {
      return std::unexpected(v.error());
    }
    out = static_cast<std::int32_t>(*v);
    return {};
  }
};

template <>
struct value_codec_t<std::int64_t>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, std::int64_t &out)
  {
    auto v = detail::read_be<std::uint64_t>(bytes);
    if (!v.has_value())
    {
      return std::unexpected(v.error());
    }
    out = static_cast<std::int64_t>(*v);
    return {};
  }
};

template <>
struct value_codec_t<bool>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, bool &out)
  {
    if (bytes.size() != 1)
    {
      return fail(error_kind_t::decode, "binary bool has unexpected width");
    }
    out = bytes[0] != 0;
    return {};
  }
};

template <>
struct value_codec_t<float>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, float &out)
  {
    auto v = detail::read_be<std::uint32_t>(bytes);
    if (!v.has_value())
    {
      return std::unexpected(v.error());
    }
    out = std::bit_cast<float>(*v);
    return {};
  }
};

template <>
struct value_codec_t<double>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, double &out)
  {
    auto v = detail::read_be<std::uint64_t>(bytes);
    if (!v.has_value())
    {
      return std::unexpected(v.error());
    }
    out = std::bit_cast<double>(*v);
    return {};
  }
};

template <>
struct value_codec_t<std::string>
{
  static result_t<void> decode(std::span<const std::uint8_t> bytes, std::string &out)
  {
    out.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return {};
  }
};

template <typename T>
result_t<void> decode_field(const std::optional<std::span<const std::uint8_t>> &column, T &out)
{
  if constexpr (is_optional_v<T>)
  {
    if (!column.has_value())
    {
      out.reset();
      return {};
    }
    out.emplace();
    return value_codec_t<typename T::value_type>::decode(*column, *out);
  }
  else
  {
    if (!column.has_value())
    {
      return fail(error_kind_t::decode, "NULL value in a non-optional field");
    }
    return value_codec_t<T>::decode(*column, out);
  }
}
} // namespace photon
