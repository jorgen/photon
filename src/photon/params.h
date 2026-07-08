#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace photon
{
using encoded_param_t = std::optional<std::vector<std::uint8_t>>;

namespace detail
{
inline std::vector<std::uint8_t> text_bytes(std::string_view text)
{
  return {reinterpret_cast<const std::uint8_t *>(text.data()), reinterpret_cast<const std::uint8_t *>(text.data()) + text.size()};
}
} // namespace detail

template <typename T, typename Enable = void>
struct param_codec_t;

template <typename T>
struct param_codec_t<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
{
  static encoded_param_t encode(T value)
  {
    return detail::text_bytes(std::to_string(value));
  }
};

template <>
struct param_codec_t<bool>
{
  static encoded_param_t encode(bool value)
  {
    return detail::text_bytes(value ? "t" : "f");
  }
};

template <typename T>
struct param_codec_t<T, std::enable_if_t<std::is_floating_point_v<T>>>
{
  static encoded_param_t encode(T value)
  {
    char buffer[64];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return detail::text_bytes(std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
  }
};

template <>
struct param_codec_t<std::string>
{
  static encoded_param_t encode(const std::string &value)
  {
    return detail::text_bytes(value);
  }
};

template <>
struct param_codec_t<std::string_view>
{
  static encoded_param_t encode(std::string_view value)
  {
    return detail::text_bytes(value);
  }
};

template <>
struct param_codec_t<const char *>
{
  static encoded_param_t encode(const char *value)
  {
    return detail::text_bytes(value == nullptr ? std::string_view{} : std::string_view(value));
  }
};

template <typename T>
struct param_codec_t<std::optional<T>>
{
  static encoded_param_t encode(const std::optional<T> &value)
  {
    if (!value.has_value())
    {
      return std::nullopt;
    }
    return param_codec_t<T>::encode(*value);
  }
};

template <typename T>
encoded_param_t encode_param(const T &value)
{
  if constexpr (std::is_array_v<std::remove_reference_t<T>>)
  {
    return param_codec_t<const char *>::encode(value);
  }
  else
  {
    return param_codec_t<T>::encode(value);
  }
}
} // namespace photon
