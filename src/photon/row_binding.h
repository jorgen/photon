#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

#include <structify/structify.h>

#include "photon/decode.h"
#include "photon/detail/message.h"
#include "photon/error.h"

namespace photon
{
namespace detail
{
template <typename Row>
constexpr auto row_members()
{
  return STFY::Internal::StructifyBaseDummy<Row, Row>::stfy_static_meta_data_info();
}

template <typename Row>
inline constexpr std::size_t row_field_count = std::remove_cvref_t<decltype(row_members<Row>())>::size;
} // namespace detail

template <typename Row>
struct column_map_t
{
  std::array<int, detail::row_field_count<Row>> field_to_column{};
};

template <typename Row>
result_t<column_map_t<Row>> build_column_map(const detail::row_description_t &description)
{
  auto members = detail::row_members<Row>();
  column_map_t<Row> map;
  std::string missing;

  [&]<std::size_t... I>(std::index_sequence<I...>)
  {
    (
      [&]
      {
        const char *name = members.template get<I>().names.template get<0>().data;
        int found = -1;
        for (std::size_t c = 0; c < description.fields.size(); ++c)
        {
          if (description.fields[c].name == name)
          {
            found = static_cast<int>(c);
            break;
          }
        }
        map.field_to_column[I] = found;
        if (found < 0 && missing.empty())
        {
          missing = name;
        }
      }(),
      ...);
  }(std::make_index_sequence<detail::row_field_count<Row>>{});

  if (!missing.empty())
  {
    return fail(error_kind_t::decode, "result has no column for field '" + missing + "'");
  }
  return map;
}

template <typename Row>
result_t<void> bind_row(const detail::data_row_t &row, const column_map_t<Row> &map, Row &out)
{
  auto members = detail::row_members<Row>();
  result_t<void> status{};

  [&]<std::size_t... I>(std::index_sequence<I...>)
  {
    (
      [&]
      {
        if (!status.has_value())
        {
          return;
        }
        int col = map.field_to_column[I];
        if (col < 0 || static_cast<std::size_t>(col) >= row.columns.size())
        {
          status = fail(error_kind_t::decode, "result row is missing a bound column");
          return;
        }
        auto &field = out.*(members.template get<I>().member);
        auto decoded = decode_field(row.columns[static_cast<std::size_t>(col)], field);
        if (!decoded.has_value())
        {
          status = std::unexpected(decoded.error());
        }
      }(),
      ...);
  }(std::make_index_sequence<detail::row_field_count<Row>>{});

  return status;
}
} // namespace photon
