#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <vio/task.h>

#include "photon/detail/message.h"
#include "photon/error.h"
#include "photon/params.h"
#include "photon/result.h"
#include "photon/row_binding.h"

namespace photon
{
class connection_t;

enum class pipeline_mode_t
{
  independent,
  atomic,
};

template <typename T>
class pipe_slot_t
{
public:
  pipe_slot_t() = default;
  explicit pipe_slot_t(std::shared_ptr<std::optional<result_t<T>>> storage)
    : _storage(std::move(storage))
  {
  }

  result_t<T> get()
  {
    if (!_storage || !_storage->has_value())
    {
      return fail(error_kind_t::protocol, "pipeline step has no result; call run() first");
    }
    return std::move(**_storage);
  }

private:
  std::shared_ptr<std::optional<result_t<T>>> _storage;
};

class pipeline_t
{
public:
  pipeline_t(const pipeline_t &) = delete;
  pipeline_t &operator=(const pipeline_t &) = delete;
  pipeline_t(pipeline_t &&) = default;
  pipeline_t &operator=(pipeline_t &&) = default;

  template <typename Row, typename... Params>
  pipe_slot_t<result_set_t<Row>> query(std::string_view sql, Params &&...params)
  {
    std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
    detail::append_extended_query(_buffer, "", sql, encoded, _mode == pipeline_mode_t::independent);
    auto storage = std::make_shared<std::optional<result_t<result_set_t<Row>>>>();
    _finishers.push_back([storage](result_t<detail::query_data_t> data) { *storage = finish_query<Row>(std::move(data)); });
    return pipe_slot_t<result_set_t<Row>>(storage);
  }

  template <typename... Params>
  pipe_slot_t<command_result_t> execute(std::string_view sql, Params &&...params)
  {
    std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
    detail::append_extended_query(_buffer, "", sql, encoded, _mode == pipeline_mode_t::independent);
    auto storage = std::make_shared<std::optional<result_t<command_result_t>>>();
    _finishers.push_back(
      [storage](result_t<detail::query_data_t> data)
      {
        if (!data.has_value())
        {
          *storage = std::unexpected(data.error());
          return;
        }
        *storage = make_command_result(std::move(data->command_tag));
      });
    return pipe_slot_t<command_result_t>(storage);
  }

  vio::task_t<result_t<void>> run();

  [[nodiscard]] std::size_t size() const
  {
    return _finishers.size();
  }

private:
  friend class connection_t;

  pipeline_t(connection_t *connection, pipeline_mode_t mode)
    : _conn(connection)
    , _mode(mode)
  {
  }

  template <typename Row>
  static result_t<result_set_t<Row>> finish_query(result_t<detail::query_data_t> data)
  {
    if (!data.has_value())
    {
      return std::unexpected(data.error());
    }
    return make_result_set<Row>(std::move(*data));
  }

  vio::task_t<result_t<void>> run_atomic();

  connection_t *_conn = nullptr;
  pipeline_mode_t _mode = pipeline_mode_t::independent;
  std::vector<std::uint8_t> _buffer;
  std::vector<std::function<void(result_t<detail::query_data_t>)>> _finishers;
  bool _ran = false;
};

template <typename Row, typename... Params>
struct pipe_query_step_t
{
  std::string sql;
  std::tuple<Params...> params;
  using result_type = result_t<result_set_t<Row>>;

  pipe_slot_t<result_set_t<Row>> add_to(pipeline_t &pipe)
  {
    return std::apply([&](auto &...values) { return pipe.template query<Row>(sql, values...); }, params);
  }
};

template <typename... Params>
struct pipe_execute_step_t
{
  std::string sql;
  std::tuple<Params...> params;
  using result_type = result_t<command_result_t>;

  pipe_slot_t<command_result_t> add_to(pipeline_t &pipe)
  {
    return std::apply([&](auto &...values) { return pipe.execute(sql, values...); }, params);
  }
};

template <typename Row, typename... Params>
pipe_query_step_t<Row, std::decay_t<Params>...> pquery(std::string_view sql, Params &&...params)
{
  return {std::string(sql), std::tuple<std::decay_t<Params>...>(std::forward<Params>(params)...)};
}

template <typename... Params>
pipe_execute_step_t<std::decay_t<Params>...> pexecute(std::string_view sql, Params &&...params)
{
  return {std::string(sql), std::tuple<std::decay_t<Params>...>(std::forward<Params>(params)...)};
}

template <typename T, typename = void>
struct is_pipe_step_impl : std::false_type
{
};
template <typename T>
struct is_pipe_step_impl<T, std::void_t<typename T::result_type>> : std::true_type
{
};
template <typename T>
inline constexpr bool is_pipe_step_v = is_pipe_step_impl<std::decay_t<T>>::value;

template <typename T>
using step_result_t = typename std::decay_t<T>::result_type;

template <typename Step>
using step_slot_t = decltype(std::declval<Step &>().add_to(std::declval<pipeline_t &>()));
} // namespace photon
