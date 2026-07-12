#include "photon/copy.h"

#include "photon/connection.h"
#include "photon/detail/message.h"
#include "photon/detail/protocol.h"

namespace photon
{
copy_in_t &copy_in_t::operator=(copy_in_t &&other) noexcept
{
  if (this != &other)
  {
    abandon();
    _conn = other._conn;
    _active = other._active;
    other._conn = nullptr;
    other._active = false;
  }
  return *this;
}

copy_in_t::~copy_in_t()
{
  abandon();
}

void copy_in_t::abandon()
{
  if (_active && _conn != nullptr)
  {
    _conn->poison();
  }
  _conn = nullptr;
  _active = false;
}

vio::task_t<result_t<void>> copy_in_t::write(std::span<const std::uint8_t> data)
{
  if (_conn == nullptr || !_active)
  {
    co_return photon::fail(error_kind_t::protocol, "copy is not active");
  }
  auto sent = co_await _conn->_transport->write_all(detail::copy_data_message(data));
  if (!sent.has_value())
  {
    _conn->_broken = true;
    _active = false;
    co_return std::unexpected(sent.error());
  }
  co_return result_t<void>{};
}

vio::task_t<result_t<void>> copy_in_t::write(std::string_view data)
{
  co_return co_await write(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(data.data()), data.size()));
}

vio::task_t<result_t<void>> copy_in_t::write_row(std::span<const std::optional<std::string>> fields)
{
  std::string row;
  for (std::size_t i = 0; i < fields.size(); ++i)
  {
    if (i != 0)
    {
      row.push_back('\t');
    }
    if (!fields[i].has_value())
    {
      row.append("\\N");
      continue;
    }
    for (char c : *fields[i])
    {
      switch (c)
      {
      case '\\':
        row.append("\\\\");
        break;
      case '\t':
        row.append("\\t");
        break;
      case '\n':
        row.append("\\n");
        break;
      case '\r':
        row.append("\\r");
        break;
      default:
        row.push_back(c);
      }
    }
  }
  row.push_back('\n');
  co_return co_await write(std::string_view(row));
}

vio::task_t<result_t<command_result_t>> copy_in_t::finish()
{
  if (_conn == nullptr || !_active)
  {
    co_return photon::fail(error_kind_t::protocol, "copy is not active");
  }
  _active = false;
  auto sent = co_await _conn->_transport->write_all(detail::copy_done_message());
  if (!sent.has_value())
  {
    _conn->_broken = true;
    co_return std::unexpected(sent.error());
  }
  auto data = co_await _conn->read_frames();
  if (!data.has_value())
  {
    co_return std::unexpected(data.error());
  }
  co_return make_command_result(std::move(data->command_tag));
}

vio::task_t<result_t<void>> copy_in_t::fail(std::string_view message)
{
  if (_conn == nullptr || !_active)
  {
    co_return photon::fail(error_kind_t::protocol, "copy is not active");
  }
  _active = false;
  auto sent = co_await _conn->_transport->write_all(detail::copy_fail_message(message));
  if (!sent.has_value())
  {
    _conn->_broken = true;
    co_return std::unexpected(sent.error());
  }
  co_return co_await _conn->drain_to_ready();
}

copy_out_t &copy_out_t::operator=(copy_out_t &&other) noexcept
{
  if (this != &other)
  {
    abandon();
    _conn = other._conn;
    _active = other._active;
    _result = std::move(other._result);
    other._conn = nullptr;
    other._active = false;
  }
  return *this;
}

copy_out_t::~copy_out_t()
{
  abandon();
}

void copy_out_t::abandon()
{
  if (_active && _conn != nullptr)
  {
    _conn->poison();
  }
  _conn = nullptr;
  _active = false;
}

vio::task_t<result_t<std::optional<std::vector<std::uint8_t>>>> copy_out_t::read_chunk()
{
  if (_conn == nullptr || !_active)
  {
    co_return std::optional<std::vector<std::uint8_t>>{};
  }
  for (;;)
  {
    auto message = co_await _conn->_reader.next();
    if (!message.has_value())
    {
      _conn->_broken = true;
      _active = false;
      co_return std::unexpected(message.error());
    }
    switch (static_cast<detail::backend_tag_t>(message->type))
    {
    case detail::backend_tag_t::copy_data:
      co_return std::optional<std::vector<std::uint8_t>>{std::move(message->body)};
    case detail::backend_tag_t::copy_done:
    {
      auto tail = co_await _conn->read_frames();
      _active = false;
      if (!tail.has_value())
      {
        co_return std::unexpected(tail.error());
      }
      _result = make_command_result(std::move(tail->command_tag));
      co_return std::optional<std::vector<std::uint8_t>>{};
    }
    case detail::backend_tag_t::error_response:
    {
      auto error = detail::parse_error_response(message->body);
      error_t surfaced = error.has_value() ? error_t{error_kind_t::server, std::string(error->message()), detail::to_server_error(*error)} : error_t{error_kind_t::protocol, "malformed error response", std::nullopt};
      auto drained = co_await _conn->drain_to_ready();
      _active = false;
      if (!drained.has_value())
      {
        co_return std::unexpected(drained.error());
      }
      co_return std::unexpected(surfaced);
    }
    case detail::backend_tag_t::notice_response:
      _conn->dispatch_notice(message->body);
      break;
    case detail::backend_tag_t::notification_response:
      _conn->dispatch_notification(message->body);
      break;
    default:
      break;
    }
  }
}

vio::task_t<result_t<std::string>> copy_out_t::read_all()
{
  std::string out;
  for (;;)
  {
    auto chunk = co_await read_chunk();
    if (!chunk.has_value())
    {
      co_return std::unexpected(chunk.error());
    }
    if (!chunk->has_value())
    {
      co_return out;
    }
    out.append(reinterpret_cast<const char *>((*chunk)->data()), (*chunk)->size());
  }
}

vio::task_t<result_t<copy_in_t>> connection_t::copy_in(std::string_view sql)
{
  auto sent = co_await _transport->write_all(detail::query_message(sql));
  if (!sent.has_value())
  {
    _broken = true;
    co_return std::unexpected(sent.error());
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
    case detail::backend_tag_t::copy_in_response:
      co_return copy_in_t(this);
    case detail::backend_tag_t::error_response:
    {
      auto error = detail::parse_error_response(message->body);
      error_t surfaced = error.has_value() ? error_t{error_kind_t::server, std::string(error->message()), detail::to_server_error(*error)} : error_t{error_kind_t::protocol, "malformed error response", std::nullopt};
      auto drained = co_await drain_to_ready();
      if (!drained.has_value())
      {
        co_return std::unexpected(drained.error());
      }
      co_return std::unexpected(surfaced);
    }
    case detail::backend_tag_t::notice_response:
      dispatch_notice(message->body);
      break;
    case detail::backend_tag_t::notification_response:
      dispatch_notification(message->body);
      break;
    default:
      break;
    }
  }
}

vio::task_t<result_t<copy_out_t>> connection_t::copy_out(std::string_view sql)
{
  auto sent = co_await _transport->write_all(detail::query_message(sql));
  if (!sent.has_value())
  {
    _broken = true;
    co_return std::unexpected(sent.error());
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
    case detail::backend_tag_t::copy_out_response:
      co_return copy_out_t(this);
    case detail::backend_tag_t::error_response:
    {
      auto error = detail::parse_error_response(message->body);
      error_t surfaced = error.has_value() ? error_t{error_kind_t::server, std::string(error->message()), detail::to_server_error(*error)} : error_t{error_kind_t::protocol, "malformed error response", std::nullopt};
      auto drained = co_await drain_to_ready();
      if (!drained.has_value())
      {
        co_return std::unexpected(drained.error());
      }
      co_return std::unexpected(surfaced);
    }
    case detail::backend_tag_t::notice_response:
      dispatch_notice(message->body);
      break;
    case detail::backend_tag_t::notification_response:
      dispatch_notification(message->body);
      break;
    default:
      break;
    }
  }
}

vio::task_t<result_t<void>> connection_t::drain_to_ready()
{
  for (;;)
  {
    auto message = co_await _reader.next();
    if (!message.has_value())
    {
      _broken = true;
      co_return std::unexpected(message.error());
    }
    if (static_cast<detail::backend_tag_t>(message->type) == detail::backend_tag_t::ready_for_query)
    {
      co_return result_t<void>{};
    }
    if (static_cast<detail::backend_tag_t>(message->type) == detail::backend_tag_t::notice_response)
    {
      dispatch_notice(message->body);
    }
    else if (static_cast<detail::backend_tag_t>(message->type) == detail::backend_tag_t::notification_response)
    {
      dispatch_notification(message->body);
    }
  }
}
} // namespace photon
