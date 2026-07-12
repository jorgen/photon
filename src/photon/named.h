#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "photon/error.h"
#include "photon/params.h"

namespace photon
{
class named_args_t
{
public:
  named_args_t() = default;

  template <typename T>
  named_args_t &set(std::string_view name, const T &value)
  {
    _values.emplace_back(std::string(name), encode_param(value));
    return *this;
  }

  [[nodiscard]] const std::vector<std::pair<std::string, encoded_param_t>> &values() const
  {
    return _values;
  }

private:
  std::vector<std::pair<std::string, encoded_param_t>> _values;
};

template <typename... Params>
inline constexpr bool is_named_args_call_v = (sizeof...(Params) == 1) && (std::is_same_v<std::remove_cvref_t<Params>, named_args_t> && ...);

namespace detail
{
struct rewritten_query_t
{
  std::string sql;
  std::vector<encoded_param_t> params;
};

result_t<rewritten_query_t> rewrite_named_params(std::string_view sql, const std::vector<std::pair<std::string, encoded_param_t>> &named);
} // namespace detail
} // namespace photon
