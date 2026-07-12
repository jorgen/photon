#include "photon/connection.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include <vio/cancellation.h>
#include <vio/operation/sleep.h>
#include <vio/operation/tcp.h>

#include "photon/detail/scram.h"

namespace photon
{
namespace
{
std::string_view bytes_as_view(const std::vector<std::uint8_t> &bytes)
{
  return std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

bool mechanism_list_has_scram(std::span<const std::uint8_t> list)
{
  detail::wire_reader_t reader(list);
  for (;;)
  {
    auto name = reader.cstr();
    if (!name.has_value() || name->empty())
    {
      return false;
    }
    if (*name == "SCRAM-SHA-256")
    {
      return true;
    }
  }
}

bool sslmode_attempts_tls(sslmode_t mode)
{
  return mode == sslmode_t::prefer || mode == sslmode_t::require || mode == sslmode_t::verify_ca || mode == sslmode_t::verify_full;
}

bool sslmode_requires_tls(sslmode_t mode)
{
  return mode == sslmode_t::require || mode == sslmode_t::verify_ca || mode == sslmode_t::verify_full;
}

vio::ssl_config_t build_ssl_config(const connect_params_t &params)
{
  vio::ssl_config_t config;
  config.peer_verify = (params.sslmode == sslmode_t::verify_ca || params.sslmode == sslmode_t::verify_full) ? vio::peer_verify_t::required : vio::peer_verify_t::disabled;
  if (!params.sslrootcert.empty())
  {
    config.ca_file = params.sslrootcert;
  }
  return config;
}

std::string tls_peer_name(const connect_params_t &params)
{
  // The host doubles as SNI and, when peer_verify is on, the verified hostname.
  // verify-ca verifies the chain but must NOT pin the hostname, so it passes an
  // empty name; every other TLS mode passes the host so SNI is sent (hostname
  // verification only actually bites under verify-full, where peer_verify is
  // required; under prefer/require peer_verify is disabled so a mismatch is
  // ignored and only SNI is conveyed).
  return params.sslmode == sslmode_t::verify_ca ? std::string{} : params.host;
}

vio::task_t<result_t<bool>> request_ssl(vio::tcp_t &tcp)
{
  auto sent = co_await vio::write_tcp(tcp, detail::ssl_request_message());
  if (!sent.has_value())
  {
    co_return fail(error_kind_t::connection, "failed to send SSLRequest: " + sent.error().msg);
  }

  auto reader = vio::tcp_create_reader(tcp);
  if (!reader.has_value())
  {
    co_return fail(error_kind_t::connection, "failed to create reader for SSL negotiation: " + reader.error().msg);
  }
  std::byte response[1];
  auto read = co_await reader.value().read_into(std::span<std::byte>(response, 1));
  if (!read.has_value())
  {
    co_return fail(error_kind_t::connection, "failed to read SSL negotiation response: " + read.error().msg);
  }
  if (*read == 0)
  {
    co_return fail(error_kind_t::connection, "server closed the connection during SSL negotiation");
  }
  co_return static_cast<char>(response[0]) == 'S';
}

vio::task_t<result_t<std::unique_ptr<detail::transport_t>>> establish_transport(vio::event_loop_t &loop, const connect_params_t &params)
{
  auto tcp_result = co_await detail::connect_raw_tcp(loop, params.host, params.port, params.connect_timeout);
  if (!tcp_result.has_value())
  {
    co_return std::unexpected(tcp_result.error());
  }
  vio::tcp_t tcp = std::move(*tcp_result.value());

  if (!sslmode_attempts_tls(params.sslmode))
  {
    co_return detail::make_tcp_transport(std::move(tcp));
  }

  auto ssl_supported = co_await request_ssl(tcp);
  if (!ssl_supported.has_value())
  {
    co_return std::unexpected(ssl_supported.error());
  }
  if (*ssl_supported)
  {
    co_return co_await detail::upgrade_to_tls(std::move(tcp), build_ssl_config(params), tls_peer_name(params));
  }
  if (sslmode_requires_tls(params.sslmode))
  {
    co_return fail(error_kind_t::connection, "server does not support SSL but sslmode requires it");
  }
  co_return detail::make_tcp_transport(std::move(tcp));
}
} // namespace

connection_t::connection_t(vio::event_loop_t &loop, std::unique_ptr<detail::transport_t> transport, connect_params_t params)
  : _loop(&loop)
  , _transport(std::move(transport))
  , _reader(*_transport)
  , _params(std::move(params))
{
}

vio::task_t<result_t<std::shared_ptr<connection_t>>> connection_t::connect(vio::event_loop_t &loop, std::string_view dsn)
{
  auto params = parse_dsn(dsn);
  if (!params.has_value())
  {
    co_return std::unexpected(params.error());
  }
  co_return co_await connect(loop, std::move(*params));
}

vio::task_t<result_t<std::shared_ptr<connection_t>>> connection_t::connect(vio::event_loop_t &loop, connect_params_t params)
{
  auto transport = co_await establish_transport(loop, params);
  if (!transport.has_value())
  {
    co_return std::unexpected(transport.error());
  }

  auto connection = std::shared_ptr<connection_t>(new connection_t(loop, std::move(*transport), std::move(params)));
  auto ready = co_await connection->handshake();
  if (!ready.has_value())
  {
    co_return std::unexpected(ready.error());
  }
  co_return connection;
}

vio::task_t<result_t<void>> cancel_handle_t::cancel(vio::event_loop_t &loop) const
{
  if (_process_id == 0)
  {
    co_return fail(error_kind_t::connection, "connection has no backend key data to cancel");
  }
  auto transport = co_await establish_transport(loop, _params);
  if (!transport.has_value())
  {
    co_return std::unexpected(transport.error());
  }
  auto sent = co_await (*transport)->write_all(detail::cancel_request_message(_process_id, _secret_key));
  if (!sent.has_value())
  {
    co_return std::unexpected(sent.error());
  }
  (*transport)->close();
  co_return result_t<void>{};
}

vio::task_t<result_t<void>> connection_t::handshake()
{
  std::vector<detail::startup_param_t> options;
  options.reserve(_params.options.size());
  for (const auto &option : _params.options)
  {
    options.push_back(detail::startup_param_t{option.first, option.second});
  }

  auto sent = co_await _transport->write_all(detail::startup_message(_params.user, _params.database, options));
  if (!sent.has_value())
  {
    co_return std::unexpected(sent.error());
  }

  for (;;)
  {
    auto message = co_await _reader.next();
    if (!message.has_value())
    {
      co_return std::unexpected(message.error());
    }

    switch (static_cast<detail::backend_tag_t>(message->type))
    {
    case detail::backend_tag_t::authentication:
    {
      auto auth = detail::parse_authentication(message->body);
      if (!auth.has_value())
      {
        co_return std::unexpected(auth.error());
      }
      switch (auth->type)
      {
      case detail::auth_request_t::ok:
        break;
      case detail::auth_request_t::cleartext_password:
      {
        auto pw = co_await _transport->write_all(detail::password_message(_params.password));
        if (!pw.has_value())
        {
          co_return std::unexpected(pw.error());
        }
        break;
      }
      case detail::auth_request_t::sasl:
      {
        auto scram = co_await authenticate_scram(auth->data);
        if (!scram.has_value())
        {
          co_return std::unexpected(scram.error());
        }
        break;
      }
      case detail::auth_request_t::md5_password:
        co_return fail(error_kind_t::auth, "md5 password authentication is not supported yet");
      default:
        co_return fail(error_kind_t::auth, "unsupported authentication method requested by server");
      }
      break;
    }
    case detail::backend_tag_t::parameter_status:
    {
      auto status = detail::parse_parameter_status(message->body);
      if (status.has_value())
      {
        _server_params.push_back(std::move(*status));
      }
      break;
    }
    case detail::backend_tag_t::backend_key_data:
    {
      auto key = detail::parse_backend_key_data(message->body);
      if (key.has_value())
      {
        _backend_process_id = key->process_id;
        _backend_secret_key = key->secret_key;
      }
      break;
    }
    case detail::backend_tag_t::ready_for_query:
      co_return result_t<void>{};
    case detail::backend_tag_t::error_response:
    {
      auto error = detail::parse_error_response(message->body);
      if (!error.has_value())
      {
        co_return std::unexpected(error.error());
      }
      co_return fail_server(detail::to_server_error(*error));
    }
    case detail::backend_tag_t::notice_response:
      dispatch_notice(message->body);
      break;
    default:
      co_return fail(error_kind_t::protocol, "unexpected message during startup");
    }
  }
}

vio::task_t<result_t<detail::raw_message_t>> connection_t::next_significant_frame()
{
  for (;;)
  {
    auto message = co_await _reader.next();
    if (!message.has_value())
    {
      co_return std::unexpected(message.error());
    }
    switch (static_cast<detail::backend_tag_t>(message->type))
    {
    case detail::backend_tag_t::notice_response:
      dispatch_notice(message->body);
      continue;
    case detail::backend_tag_t::notification_response:
      dispatch_notification(message->body);
      continue;
    case detail::backend_tag_t::parameter_status:
    {
      auto status = detail::parse_parameter_status(message->body);
      if (status.has_value())
      {
        _server_params.push_back(std::move(*status));
      }
      continue;
    }
    default:
      co_return std::move(*message);
    }
  }
}

vio::task_t<result_t<void>> connection_t::authenticate_scram(std::span<const std::uint8_t> mechanism_list)
{
  if (!mechanism_list_has_scram(mechanism_list))
  {
    co_return fail(error_kind_t::auth, "server does not offer SCRAM-SHA-256");
  }

  detail::scram_client_t client(std::string{}, _params.password);
  auto client_first = client.client_first_message();
  if (!client_first.has_value())
  {
    co_return std::unexpected(client_first.error());
  }

  auto sent_initial = co_await _transport->write_all(detail::sasl_initial_response_message("SCRAM-SHA-256", *client_first));
  if (!sent_initial.has_value())
  {
    co_return std::unexpected(sent_initial.error());
  }

  auto continue_message = co_await next_significant_frame();
  if (!continue_message.has_value())
  {
    co_return std::unexpected(continue_message.error());
  }
  if (static_cast<detail::backend_tag_t>(continue_message->type) == detail::backend_tag_t::error_response)
  {
    auto error = detail::parse_error_response(continue_message->body);
    if (!error.has_value())
    {
      co_return std::unexpected(error.error());
    }
    co_return fail_server(detail::to_server_error(*error));
  }
  if (static_cast<detail::backend_tag_t>(continue_message->type) != detail::backend_tag_t::authentication)
  {
    co_return fail(error_kind_t::auth, "expected SASL continue from server");
  }
  auto continue_auth = detail::parse_authentication(continue_message->body);
  if (!continue_auth.has_value())
  {
    co_return std::unexpected(continue_auth.error());
  }
  if (continue_auth->type != detail::auth_request_t::sasl_continue)
  {
    co_return fail(error_kind_t::auth, "expected SASL continue message");
  }

  auto client_final = client.handle_server_first(bytes_as_view(continue_auth->data));
  if (!client_final.has_value())
  {
    co_return std::unexpected(client_final.error());
  }

  auto sent_final = co_await _transport->write_all(detail::sasl_response_message(*client_final));
  if (!sent_final.has_value())
  {
    co_return std::unexpected(sent_final.error());
  }

  auto final_message = co_await next_significant_frame();
  if (!final_message.has_value())
  {
    co_return std::unexpected(final_message.error());
  }
  if (static_cast<detail::backend_tag_t>(final_message->type) == detail::backend_tag_t::error_response)
  {
    auto error = detail::parse_error_response(final_message->body);
    if (!error.has_value())
    {
      co_return std::unexpected(error.error());
    }
    co_return fail_server(detail::to_server_error(*error));
  }
  if (static_cast<detail::backend_tag_t>(final_message->type) != detail::backend_tag_t::authentication)
  {
    co_return fail(error_kind_t::auth, "expected SASL final from server");
  }
  auto final_auth = detail::parse_authentication(final_message->body);
  if (!final_auth.has_value())
  {
    co_return std::unexpected(final_auth.error());
  }
  if (final_auth->type != detail::auth_request_t::sasl_final)
  {
    co_return fail(error_kind_t::auth, "expected SASL final message");
  }

  co_return client.handle_server_final(bytes_as_view(final_auth->data));
}

vio::task_t<result_t<detail::query_data_t>> connection_t::exec_extended(std::string_view statement_name, std::string_view sql, std::vector<encoded_param_t> params)
{
  if (_broken)
  {
    co_return fail(error_kind_t::connection, "connection is broken");
  }
  std::vector<std::uint8_t> out;
  detail::append_extended_query(out, statement_name, sql, params, true);

  auto sent = co_await _transport->write_all(std::move(out));
  if (!sent.has_value())
  {
    _broken = true;
    co_return std::unexpected(sent.error());
  }

  co_return co_await read_query_result();
}

vio::task_t<result_t<detail::query_data_t>> connection_t::read_query_result()
{
  if (_params.query_timeout.count() <= 0)
  {
    co_return co_await read_frames();
  }

  vio::cancellation_t token;
  bool timed_out = false;
  auto watchdog = [](vio::event_loop_t &loop, detail::transport_t *transport, vio::cancellation_t &cancel_token, std::chrono::milliseconds duration, bool &flag) -> vio::task_t<void>
  {
    auto fired = co_await vio::sleep(loop, duration, &cancel_token);
    if (fired.has_value() && !cancel_token.is_cancelled())
    {
      flag = true;
      transport->cancel_read();
    }
  }(*_loop, _transport.get(), token, _params.query_timeout, timed_out);

  auto result = co_await read_frames();
  token.cancel();
  co_await std::move(watchdog);

  if (timed_out)
  {
    co_return fail(error_kind_t::timeout, "query timed out");
  }
  co_return result;
}

vio::task_t<result_t<detail::query_data_t>> connection_t::read_frames()
{
  detail::query_data_t data;
  std::optional<error_t> server_error;
  for (;;)
  {
    auto message = co_await _reader.next();
    if (!message.has_value())
    {
      _broken = true;
      co_return std::unexpected(message.error());
    }

    switch (static_cast<detail::backend_tag_t>(message->type))
    {
    case detail::backend_tag_t::row_description:
    {
      auto description = detail::parse_row_description(message->body);
      if (!description.has_value())
      {
        _broken = true;
        co_return std::unexpected(description.error());
      }
      data.description = std::move(*description);
      data.has_description = true;
      break;
    }
    case detail::backend_tag_t::data_row:
      data.rows.push_back(std::move(message->body));
      break;
    case detail::backend_tag_t::command_complete:
    {
      auto complete = detail::parse_command_complete(message->body);
      if (complete.has_value())
      {
        data.command_tag = std::move(complete->tag);
      }
      break;
    }
    case detail::backend_tag_t::error_response:
    {
      auto error = detail::parse_error_response(message->body);
      if (error.has_value())
      {
        server_error = error_t{error_kind_t::server, std::string(error->message()), detail::to_server_error(*error)};
      }
      break;
    }
    case detail::backend_tag_t::notice_response:
      dispatch_notice(message->body);
      break;
    case detail::backend_tag_t::notification_response:
      dispatch_notification(message->body);
      break;
    case detail::backend_tag_t::ready_for_query:
      if (server_error.has_value())
      {
        co_return std::unexpected(*server_error);
      }
      co_return data;
    default:
      break;
    }
  }
}

vio::task_t<result_t<void>> connection_t::parse_statement(std::string name, std::string_view sql)
{
  if (_broken)
  {
    co_return fail(error_kind_t::connection, "connection is broken");
  }
  std::vector<std::uint8_t> out;
  auto append = [&out](std::vector<std::uint8_t> message) { out.insert(out.end(), message.begin(), message.end()); };
  append(detail::parse_message(name, sql, {}));
  append(detail::sync_message());

  auto sent = co_await _transport->write_all(std::move(out));
  if (!sent.has_value())
  {
    _broken = true;
    co_return std::unexpected(sent.error());
  }

  std::optional<error_t> server_error;
  for (;;)
  {
    auto message = co_await _reader.next();
    if (!message.has_value())
    {
      _broken = true;
      co_return std::unexpected(message.error());
    }
    switch (static_cast<detail::backend_tag_t>(message->type))
    {
    case detail::backend_tag_t::error_response:
    {
      auto error = detail::parse_error_response(message->body);
      if (error.has_value())
      {
        server_error = error_t{error_kind_t::server, std::string(error->message()), detail::to_server_error(*error)};
      }
      break;
    }
    case detail::backend_tag_t::notice_response:
      dispatch_notice(message->body);
      break;
    case detail::backend_tag_t::notification_response:
      dispatch_notification(message->body);
      break;
    case detail::backend_tag_t::ready_for_query:
      if (server_error.has_value())
      {
        co_return std::unexpected(*server_error);
      }
      co_return result_t<void>{};
    default:
      break;
    }
  }
}

vio::task_t<result_t<prepared_statement_t>> connection_t::prepare(std::string_view sql)
{
  std::string name = "photon_stmt_" + std::to_string(++_statement_counter);
  auto parsed = co_await parse_statement(name, sql);
  if (!parsed.has_value())
  {
    co_return std::unexpected(parsed.error());
  }
  co_return prepared_statement_t(shared_from_this(), std::move(name));
}

vio::task_t<result_t<transaction_t>> connection_t::begin()
{
  auto started = co_await execute("BEGIN");
  if (!started.has_value())
  {
    co_return std::unexpected(started.error());
  }
  co_return transaction_t(this);
}

vio::task_t<result_t<void>> connection_t::listen(std::string_view channel)
{
  std::string sql = "LISTEN \"";
  for (char c : channel)
  {
    if (c == '"')
    {
      sql.push_back('"');
    }
    sql.push_back(c);
  }
  sql.push_back('"');

  auto result = co_await execute(sql);
  if (!result.has_value())
  {
    co_return std::unexpected(result.error());
  }
  co_return result_t<void>{};
}

vio::task_t<result_t<notification_t>> connection_t::next_notification()
{
  if (_broken)
  {
    co_return fail(error_kind_t::connection, "connection is broken");
  }
  for (;;)
  {
    auto message = co_await _reader.next();
    if (!message.has_value())
    {
      _broken = true;
      co_return std::unexpected(message.error());
    }
    switch (static_cast<detail::backend_tag_t>(message->type))
    {
    case detail::backend_tag_t::notification_response:
    {
      auto parsed = detail::parse_notification(message->body);
      if (!parsed.has_value())
      {
        _broken = true;
        co_return std::unexpected(parsed.error());
      }
      co_return notification_t{parsed->process_id, std::move(parsed->channel), std::move(parsed->payload)};
    }
    case detail::backend_tag_t::notice_response:
      dispatch_notice(message->body);
      break;
    case detail::backend_tag_t::parameter_status:
    {
      auto status = detail::parse_parameter_status(message->body);
      if (status.has_value())
      {
        _server_params.push_back(std::move(*status));
      }
      break;
    }
    case detail::backend_tag_t::error_response:
    {
      auto error = detail::parse_error_response(message->body);
      if (!error.has_value())
      {
        _broken = true;
        co_return std::unexpected(error.error());
      }
      _broken = true;
      co_return fail_server(detail::to_server_error(*error));
    }
    default:
      break;
    }
  }
}

vio::task_t<void> connection_t::close()
{
  co_await _transport->write_all(detail::terminate_message());
  _transport->close();
  co_return;
}

void connection_t::dispatch_notice(std::span<const std::uint8_t> body)
{
  if (!_on_notice)
  {
    return;
  }
  auto notice = detail::parse_error_response(body);
  if (!notice.has_value())
  {
    return;
  }
  auto callback = _on_notice;
  callback(detail::to_server_error(*notice));
}

void connection_t::dispatch_notification(std::span<const std::uint8_t> body)
{
  if (!_on_notification)
  {
    return;
  }
  auto parsed = detail::parse_notification(body);
  if (!parsed.has_value())
  {
    return;
  }
  auto callback = _on_notification;
  callback(notification_t{parsed->process_id, std::move(parsed->channel), std::move(parsed->payload)});
}

std::string_view connection_t::parameter(std::string_view key) const
{
  for (const auto &param : _server_params)
  {
    if (param.name == key)
    {
      return param.value;
    }
  }
  return {};
}
} // namespace photon
