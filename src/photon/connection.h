#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <vio/event_loop.h>
#include <vio/task.h>

#include "photon/connect_params.h"
#include "photon/copy.h"
#include "photon/detail/frame_reader.h"
#include "photon/detail/message.h"
#include "photon/detail/transport.h"
#include "photon/error.h"
#include "photon/named.h"
#include "photon/params.h"
#include "photon/pipeline.h"
#include "photon/result.h"
#include "photon/row_binding.h"
#include "photon/transaction.h"

namespace photon
{
struct notification_t
{
  std::int32_t process_id = 0;
  std::string channel;
  std::string payload;
};

class cancel_handle_t
{
public:
  cancel_handle_t() = default;
  cancel_handle_t(connect_params_t params, std::int32_t process_id, std::int32_t secret_key)
    : _params(std::move(params))
    , _process_id(process_id)
    , _secret_key(secret_key)
  {
  }

  vio::task_t<result_t<void>> cancel(vio::event_loop_t &loop) const;

  [[nodiscard]] std::int32_t process_id() const
  {
    return _process_id;
  }

private:
  connect_params_t _params;
  std::int32_t _process_id = 0;
  std::int32_t _secret_key = 0;
};

class connection_t;

class prepared_statement_t
{
public:
  prepared_statement_t() = default;
  prepared_statement_t(std::shared_ptr<connection_t> connection, std::string name)
    : _connection(std::move(connection))
    , _name(std::move(name))
  {
  }

  template <typename Row, typename... Params>
  vio::task_t<result_t<result_set_t<Row>>> query(Params &&...params);
  template <typename... Params>
  vio::task_t<result_t<command_result_t>> execute(Params &&...params);

  [[nodiscard]] const std::string &name() const
  {
    return _name;
  }

private:
  std::shared_ptr<connection_t> _connection;
  std::string _name;
};

class connection_t : public std::enable_shared_from_this<connection_t>
{
public:
  connection_t(const connection_t &) = delete;
  connection_t &operator=(const connection_t &) = delete;
  connection_t(connection_t &&) = delete;
  connection_t &operator=(connection_t &&) = delete;
  ~connection_t() = default;

  static vio::task_t<result_t<std::shared_ptr<connection_t>>> connect(vio::event_loop_t &loop, connect_params_t params);
  static vio::task_t<result_t<std::shared_ptr<connection_t>>> connect(vio::event_loop_t &loop, std::string_view dsn);

  template <typename Row, typename... Params, std::enable_if_t<!is_named_args_call_v<Params...>, int> = 0>
  vio::task_t<result_t<result_set_t<Row>>> query(std::string_view sql, Params &&...params)
  {
    std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
    auto data = co_await exec_extended("", sql, std::move(encoded));
    if (!data.has_value())
    {
      co_return std::unexpected(data.error());
    }
    co_return make_result_set<Row>(std::move(*data));
  }

  template <typename Row>
  vio::task_t<result_t<result_set_t<Row>>> query(std::string_view sql, const named_args_t &args)
  {
    auto data = co_await exec_named(sql, args);
    if (!data.has_value())
    {
      co_return std::unexpected(data.error());
    }
    co_return make_result_set<Row>(std::move(*data));
  }

  template <typename... Params, std::enable_if_t<!is_named_args_call_v<Params...>, int> = 0>
  vio::task_t<result_t<command_result_t>> execute(std::string_view sql, Params &&...params)
  {
    std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
    auto data = co_await exec_extended("", sql, std::move(encoded));
    if (!data.has_value())
    {
      co_return std::unexpected(data.error());
    }
    co_return make_command_result(std::move(data->command_tag));
  }

  vio::task_t<result_t<command_result_t>> execute(std::string_view sql, const named_args_t &args)
  {
    auto data = co_await exec_named(sql, args);
    if (!data.has_value())
    {
      co_return std::unexpected(data.error());
    }
    co_return make_command_result(std::move(data->command_tag));
  }

  template <typename Row, typename... Params, std::enable_if_t<!is_named_args_call_v<Params...>, int> = 0>
  vio::task_t<result_t<std::optional<Row>>> query_one(std::string_view sql, Params &&...params)
  {
    auto set = co_await query<Row>(sql, std::forward<Params>(params)...);
    if (!set.has_value())
    {
      co_return std::unexpected(set.error());
    }
    co_return set->one();
  }

  template <typename Row>
  vio::task_t<result_t<std::optional<Row>>> query_one(std::string_view sql, const named_args_t &args)
  {
    auto set = co_await query<Row>(sql, args);
    if (!set.has_value())
    {
      co_return std::unexpected(set.error());
    }
    co_return set->one();
  }

  template <typename T, typename... Params, std::enable_if_t<!is_named_args_call_v<Params...>, int> = 0>
  vio::task_t<result_t<std::optional<T>>> query_value(std::string_view sql, Params &&...params)
  {
    std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
    co_return decode_first_value<T>(co_await exec_extended("", sql, std::move(encoded)));
  }

  template <typename T>
  vio::task_t<result_t<std::optional<T>>> query_value(std::string_view sql, const named_args_t &args)
  {
    co_return decode_first_value<T>(co_await exec_named(sql, args));
  }

  vio::task_t<result_t<prepared_statement_t>> prepare(std::string_view sql);

  vio::task_t<result_t<copy_in_t>> copy_in(std::string_view sql);
  vio::task_t<result_t<copy_out_t>> copy_out(std::string_view sql);

  vio::task_t<result_t<transaction_t>> begin();

  pipeline_t pipeline(pipeline_mode_t mode = pipeline_mode_t::independent)
  {
    return pipeline_t(this, mode);
  }

  template <typename S0, typename... S, std::enable_if_t<is_pipe_step_v<S0>, int> = 0>
  vio::task_t<std::tuple<step_result_t<S0>, step_result_t<S>...>> pipeline(S0 first, S... rest)
  {
    return run_pipeline_steps(pipeline_mode_t::independent, std::move(first), std::move(rest)...);
  }

  template <typename S0, typename... S, std::enable_if_t<is_pipe_step_v<S0>, int> = 0>
  vio::task_t<std::tuple<step_result_t<S0>, step_result_t<S>...>> pipeline(pipeline_mode_t mode, S0 first, S... rest)
  {
    return run_pipeline_steps(mode, std::move(first), std::move(rest)...);
  }

  vio::task_t<void> close();

  void on_notice(std::function<void(const server_error_t &)> handler)
  {
    _on_notice = std::move(handler);
  }

  void on_notification(std::function<void(const notification_t &)> handler)
  {
    _on_notification = std::move(handler);
  }

  vio::task_t<result_t<void>> listen(std::string_view channel);
  vio::task_t<result_t<notification_t>> next_notification();

  [[nodiscard]] bool is_broken() const
  {
    return _broken;
  }

  [[nodiscard]] std::string_view parameter(std::string_view key) const;
  [[nodiscard]] std::int32_t backend_process_id() const
  {
    return _backend_process_id;
  }

  [[nodiscard]] cancel_handle_t cancel_handle() const
  {
    return cancel_handle_t(_params, _backend_process_id, _backend_secret_key);
  }

private:
  friend class prepared_statement_t;
  friend class transaction_t;
  friend class pipeline_t;
  friend class copy_in_t;
  friend class copy_out_t;

  connection_t(vio::event_loop_t &loop, std::unique_ptr<detail::transport_t> transport, connect_params_t params);

  vio::task_t<result_t<void>> handshake();
  vio::task_t<result_t<detail::raw_message_t>> next_significant_frame();
  vio::task_t<result_t<void>> authenticate_scram(std::span<const std::uint8_t> mechanism_list);
  vio::task_t<result_t<detail::query_data_t>> exec_extended(std::string_view statement_name, std::string_view sql, std::vector<encoded_param_t> params);
  vio::task_t<result_t<detail::query_data_t>> read_query_result();
  vio::task_t<result_t<detail::query_data_t>> read_frames();

  vio::task_t<result_t<detail::query_data_t>> exec_named(std::string_view sql, const named_args_t &args)
  {
    auto rewritten = detail::rewrite_named_params(sql, args.values());
    if (!rewritten.has_value())
    {
      co_return std::unexpected(rewritten.error());
    }
    co_return co_await exec_extended("", rewritten->sql, std::move(rewritten->params));
  }

  template <typename T>
  static result_t<std::optional<T>> decode_first_value(result_t<detail::query_data_t> data)
  {
    if (!data.has_value())
    {
      return std::unexpected(data.error());
    }
    if (data->rows.empty())
    {
      return std::optional<T>{};
    }
    auto parsed = detail::parse_data_row(data->rows.front());
    if (!parsed.has_value())
    {
      return std::unexpected(parsed.error());
    }
    if (parsed->columns.empty())
    {
      return fail(error_kind_t::decode, "query_value: result has no columns");
    }
    T value{};
    auto decoded = decode_field(parsed->columns.front(), value);
    if (!decoded.has_value())
    {
      return std::unexpected(decoded.error());
    }
    return std::optional<T>{std::move(value)};
  }
  vio::task_t<result_t<void>> parse_statement(std::string name, std::string_view sql);
  vio::task_t<result_t<void>> drain_to_ready();

  template <typename... Steps>
  vio::task_t<std::tuple<step_result_t<Steps>...>> run_pipeline_steps(pipeline_mode_t mode, Steps... steps)
  {
    auto pipe = pipeline(mode);
    std::tuple<step_slot_t<Steps>...> slots{steps.add_to(pipe)...};
    co_await pipe.run();
    co_return std::apply([](auto &...slot) { return std::make_tuple(slot.get()...); }, slots);
  }
  void dispatch_notice(std::span<const std::uint8_t> body);
  void dispatch_notification(std::span<const std::uint8_t> body);
  void poison()
  {
    _broken = true;
  }

  vio::event_loop_t *_loop;
  std::unique_ptr<detail::transport_t> _transport;
  detail::frame_reader_t _reader;
  connect_params_t _params;
  std::vector<detail::parameter_status_t> _server_params;
  std::function<void(const server_error_t &)> _on_notice;
  std::function<void(const notification_t &)> _on_notification;
  std::int32_t _backend_process_id = 0;
  std::int32_t _backend_secret_key = 0;
  std::uint64_t _statement_counter = 0;
  bool _broken = false;
};

template <typename Row, typename... Params>
vio::task_t<result_t<result_set_t<Row>>> prepared_statement_t::query(Params &&...params)
{
  if (!_connection)
  {
    co_return fail(error_kind_t::connection, "prepared statement is not bound to a connection");
  }
  std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
  auto data = co_await _connection->exec_extended(_name, {}, std::move(encoded));
  if (!data.has_value())
  {
    co_return std::unexpected(data.error());
  }
  if (!data->has_description)
  {
    co_return fail(error_kind_t::decode, "prepared query returned no result columns");
  }
  auto map = build_column_map<Row>(data->description);
  if (!map.has_value())
  {
    co_return std::unexpected(map.error());
  }
  co_return result_set_t<Row>(std::move(data->description), std::move(data->rows), *map);
}

template <typename... Params>
vio::task_t<result_t<command_result_t>> prepared_statement_t::execute(Params &&...params)
{
  if (!_connection)
  {
    co_return fail(error_kind_t::connection, "prepared statement is not bound to a connection");
  }
  std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
  auto data = co_await _connection->exec_extended(_name, {}, std::move(encoded));
  if (!data.has_value())
  {
    co_return std::unexpected(data.error());
  }
  co_return make_command_result(std::move(data->command_tag));
}
} // namespace photon
