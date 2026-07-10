#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photon/detail/message.h"
#include "photon/error.h"
#include "photon/row_binding.h"

namespace photon
{
struct command_result_t
{
  std::string tag;
  std::int64_t rows_affected = 0;
};

inline command_result_t make_command_result(std::string tag)
{
  command_result_t result;
  std::size_t space = tag.find_last_of(' ');
  if (space != std::string::npos && space + 1 < tag.size())
  {
    const char *begin = tag.data() + space + 1;
    const char *end = tag.data() + tag.size();
    std::int64_t affected = 0;
    auto [ptr, ec] = std::from_chars(begin, end, affected);
    if (ptr == end)
    {
      if (ec == std::errc::result_out_of_range)
      {
        result.rows_affected = std::numeric_limits<std::int64_t>::max();
      }
      else if (ec == std::errc{})
      {
        result.rows_affected = affected;
      }
    }
  }
  result.tag = std::move(tag);
  return result;
}

template <typename Row>
class result_set_t
{
public:
  result_set_t() = default;
  result_set_t(detail::row_description_t description, std::vector<std::vector<std::uint8_t>> rows, column_map_t<Row> map)
    : _description(std::move(description))
    , _rows(std::move(rows))
    , _map(map)
  {
  }

  [[nodiscard]] std::size_t size() const
  {
    return _rows.size();
  }
  [[nodiscard]] bool empty() const
  {
    return _rows.empty();
  }
  [[nodiscard]] const detail::row_description_t &description() const
  {
    return _description;
  }

  [[nodiscard]] result_t<Row> at(std::size_t index) const
  {
    if (index >= _rows.size())
    {
      return fail(error_kind_t::decode, "row index out of range");
    }
    auto parsed = detail::parse_data_row(_rows[index]);
    if (!parsed.has_value())
    {
      return std::unexpected(parsed.error());
    }
    Row row{};
    auto bound = bind_row(*parsed, _map, row);
    if (!bound.has_value())
    {
      return std::unexpected(bound.error());
    }
    return row;
  }

  [[nodiscard]] result_t<std::optional<Row>> one() const
  {
    if (_rows.empty())
    {
      return std::optional<Row>{};
    }
    auto row = at(0);
    if (!row.has_value())
    {
      return std::unexpected(row.error());
    }
    return std::optional<Row>{std::move(*row)};
  }

  [[nodiscard]] result_t<std::vector<Row>> collect() const
  {
    std::vector<Row> out;
    out.reserve(_rows.size());
    for (std::size_t i = 0; i < _rows.size(); ++i)
    {
      auto row = at(i);
      if (!row.has_value())
      {
        return std::unexpected(row.error());
      }
      out.push_back(std::move(*row));
    }
    return out;
  }

private:
  detail::row_description_t _description;
  std::vector<std::vector<std::uint8_t>> _rows;
  column_map_t<Row> _map;
};
} // namespace photon
