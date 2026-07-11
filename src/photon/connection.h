#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vio/event_loop.h>
#include <vio/task.h>

#include "photon/connect_params.h"
#include "photon/detail/frame_reader.h"
#include "photon/detail/message.h"
#include "photon/detail/transport.h"
#include "photon/error.h"
#include "photon/params.h"
#include "photon/result.h"
#include "photon/row_binding.h"
#include "photon/transaction.h"

namespace photon
{
namespace detail
{
struct query_data_t
{
  row_description_t description;
  std::vector<std::vector<std::uint8_t>> rows;
  std::string command_tag;
  bool has_description = false;
};
} // namespace detail

struct notification_t
{
  std::int32_t process_id = 0;
  std::string channel;
  std::string payload;
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

  template <typename Row, typename... Params>
  vio::task_t<result_t<result_set_t<Row>>> query(std::string_view sql, Params &&...params)
  {
    std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
    auto data = co_await exec_extended("", sql, std::move(encoded));
    if (!data.has_value())
    {
      co_return std::unexpected(data.error());
    }
    if (!data->has_description)
    {
      co_return fail(error_kind_t::decode, "query returned no result columns");
    }
    auto map = build_column_map<Row>(data->description);
    if (!map.has_value())
    {
      co_return std::unexpected(map.error());
    }
    co_return result_set_t<Row>(std::move(data->description), std::move(data->rows), *map);
  }

  template <typename... Params>
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

  template <typename Row, typename... Params>
  vio::task_t<result_t<std::optional<Row>>> query_one(std::string_view sql, Params &&...params)
  {
    auto set = co_await query<Row>(sql, std::forward<Params>(params)...);
    if (!set.has_value())
    {
      co_return std::unexpected(set.error());
    }
    co_return set->one();
  }

  template <typename T, typename... Params>
  vio::task_t<result_t<std::optional<T>>> query_value(std::string_view sql, Params &&...params)
  {
    std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
    auto data = co_await exec_extended("", sql, std::move(encoded));
    if (!data.has_value())
    {
      co_return std::unexpected(data.error());
    }
    if (data->rows.empty())
    {
      co_return std::optional<T>{};
    }
    auto parsed = detail::parse_data_row(data->rows.front());
    if (!parsed.has_value())
    {
      co_return std::unexpected(parsed.error());
    }
    if (parsed->columns.empty())
    {
      co_return fail(error_kind_t::decode, "query_value: result has no columns");
    }
    T value{};
    auto decoded = decode_field(parsed->columns.front(), value);
    if (!decoded.has_value())
    {
      co_return std::unexpected(decoded.error());
    }
    co_return std::optional<T>{std::move(value)};
  }

  vio::task_t<result_t<prepared_statement_t>> prepare(std::string_view sql);

  vio::task_t<result_t<transaction_t>> begin();

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

private:
  friend class prepared_statement_t;
  friend class transaction_t;

  connection_t(vio::event_loop_t &loop, std::unique_ptr<detail::transport_t> transport, connect_params_t params);

  vio::task_t<result_t<void>> handshake();
  vio::task_t<result_t<detail::raw_message_t>> next_significant_frame();
  vio::task_t<result_t<void>> authenticate_scram(std::span<const std::uint8_t> mechanism_list);
  vio::task_t<result_t<detail::query_data_t>> exec_extended(std::string_view statement_name, std::string_view sql, std::vector<encoded_param_t> params);
  vio::task_t<result_t<void>> parse_statement(std::string name, std::string_view sql);
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
