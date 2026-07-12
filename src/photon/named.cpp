#include "photon/named.h"

#include <string>

namespace photon::detail
{
namespace
{
bool is_ident_start(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_ident(char c)
{
  return is_ident_start(c) || (c >= '0' && c <= '9');
}
} // namespace

result_t<rewritten_query_t> rewrite_named_params(std::string_view sql, const std::vector<std::pair<std::string, encoded_param_t>> &named)
{
  rewritten_query_t out;
  out.sql.reserve(sql.size() + 8);
  std::vector<std::string_view> order;

  auto position_of = [&order](std::string_view name) -> std::size_t
  {
    for (std::size_t i = 0; i < order.size(); ++i)
    {
      if (order[i] == name)
      {
        return i + 1;
      }
    }
    return 0;
  };
  auto find_value = [&named](std::string_view name) -> const encoded_param_t *
  {
    for (const auto &entry : named)
    {
      if (entry.first == name)
      {
        return &entry.second;
      }
    }
    return nullptr;
  };

  std::size_t i = 0;
  while (i < sql.size())
  {
    char c = sql[i];

    if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-')
    {
      while (i < sql.size() && sql[i] != '\n')
      {
        out.sql.push_back(sql[i++]);
      }
      continue;
    }

    if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*')
    {
      int depth = 1;
      out.sql.push_back(sql[i++]);
      out.sql.push_back(sql[i++]);
      while (i < sql.size() && depth > 0)
      {
        if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*')
        {
          ++depth;
          out.sql.push_back(sql[i++]);
          out.sql.push_back(sql[i++]);
        }
        else if (sql[i] == '*' && i + 1 < sql.size() && sql[i + 1] == '/')
        {
          --depth;
          out.sql.push_back(sql[i++]);
          out.sql.push_back(sql[i++]);
        }
        else
        {
          out.sql.push_back(sql[i++]);
        }
      }
      continue;
    }

    if (c == '\'' || c == '"')
    {
      char quote = c;
      bool escapes = quote == '\'' && i > 0 && (sql[i - 1] == 'E' || sql[i - 1] == 'e') && (i < 2 || !is_ident(sql[i - 2]));
      out.sql.push_back(sql[i++]);
      while (i < sql.size())
      {
        if (escapes && sql[i] == '\\' && i + 1 < sql.size())
        {
          out.sql.push_back(sql[i++]);
          out.sql.push_back(sql[i++]);
          continue;
        }
        if (sql[i] == quote)
        {
          out.sql.push_back(sql[i++]);
          if (i < sql.size() && sql[i] == quote)
          {
            out.sql.push_back(sql[i++]);
            continue;
          }
          break;
        }
        out.sql.push_back(sql[i++]);
      }
      continue;
    }

    if (c == '$')
    {
      std::size_t j = i + 1;
      bool dollar_quote = false;
      if (j < sql.size() && (sql[j] == '$' || is_ident_start(sql[j])))
      {
        if (sql[j] != '$')
        {
          while (j < sql.size() && is_ident(sql[j]))
          {
            ++j;
          }
        }
        if (j < sql.size() && sql[j] == '$')
        {
          dollar_quote = true;
          std::string_view tag = sql.substr(i, j - i + 1);
          out.sql.append(tag);
          std::size_t body = j + 1;
          std::size_t close = sql.find(tag, body);
          if (close == std::string_view::npos)
          {
            out.sql.append(sql.substr(body));
            i = sql.size();
          }
          else
          {
            out.sql.append(sql.substr(body, close - body + tag.size()));
            i = close + tag.size();
          }
        }
      }
      if (!dollar_quote)
      {
        out.sql.push_back(sql[i++]);
      }
      continue;
    }

    if (c == ':')
    {
      if (i + 1 < sql.size() && sql[i + 1] == ':')
      {
        out.sql.push_back(sql[i++]);
        out.sql.push_back(sql[i++]);
        continue;
      }
      if (i + 1 < sql.size() && is_ident_start(sql[i + 1]))
      {
        std::size_t j = i + 1;
        while (j < sql.size() && is_ident(sql[j]))
        {
          ++j;
        }
        std::string_view name = sql.substr(i + 1, j - (i + 1));
        std::size_t pos = position_of(name);
        if (pos == 0)
        {
          if (find_value(name) == nullptr)
          {
            return fail(error_kind_t::protocol, "no value provided for named parameter :" + std::string(name));
          }
          order.push_back(name);
          pos = order.size();
        }
        out.sql.push_back('$');
        out.sql.append(std::to_string(pos));
        i = j;
        continue;
      }
    }

    out.sql.push_back(sql[i++]);
  }

  out.params.reserve(order.size());
  for (std::string_view name : order)
  {
    out.params.push_back(*find_value(name));
  }
  return out;
}
} // namespace photon::detail
