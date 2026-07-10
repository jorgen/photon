#pragma once

#include <cstdint>
#include <memory>
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

class connection_t
{
public:
  connection_t(const connection_t &) = delete;
  connection_t &operator=(const connection_t &) = delete;
  connection_t(connection_t &&) = delete;
  connection_t &operator=(connection_t &&) = delete;
  ~connection_t() = default;

  static vio::task_t<result_t<std::unique_ptr<connection_t>>> connect(vio::event_loop_t &loop, connect_params_t params);

  template <typename Row, typename... Params>
  vio::task_t<result_t<result_set_t<Row>>> query(std::string_view sql, Params &&...params)
  {
    std::vector<encoded_param_t> encoded{encode_param(std::forward<Params>(params))...};
    auto data = co_await exec_extended(sql, std::move(encoded));
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
    auto data = co_await exec_extended(sql, std::move(encoded));
    if (!data.has_value())
    {
      co_return std::unexpected(data.error());
    }
    co_return make_command_result(std::move(data->command_tag));
  }

  vio::task_t<void> close();

  [[nodiscard]] std::string_view parameter(std::string_view key) const;
  [[nodiscard]] std::int32_t backend_process_id() const
  {
    return _backend_process_id;
  }

private:
  connection_t(vio::event_loop_t &loop, std::unique_ptr<detail::transport_t> transport, connect_params_t params);

  vio::task_t<result_t<void>> handshake();
  vio::task_t<result_t<detail::raw_message_t>> next_significant_frame();
  vio::task_t<result_t<void>> authenticate_scram(std::span<const std::uint8_t> mechanism_list);
  vio::task_t<result_t<detail::query_data_t>> exec_extended(std::string_view sql, std::vector<encoded_param_t> params);

  vio::event_loop_t *_loop;
  std::unique_ptr<detail::transport_t> _transport;
  detail::frame_reader_t _reader;
  connect_params_t _params;
  std::vector<detail::parameter_status_t> _server_params;
  std::int32_t _backend_process_id = 0;
  std::int32_t _backend_secret_key = 0;
};
} // namespace photon
