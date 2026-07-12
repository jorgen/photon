#include "photon/pipeline.h"

#include <cstddef>
#include <optional>

#include "photon/connection.h"
#include "photon/detail/message.h"
#include "photon/detail/protocol.h"

namespace photon
{
namespace
{
error_t broken_error()
{
  return error_t{error_kind_t::connection, "connection broken during pipeline", std::nullopt};
}

error_t aborted_error()
{
  return error_t{error_kind_t::server, "pipeline step aborted by an earlier error", std::nullopt};
}
} // namespace

vio::task_t<result_t<void>> pipeline_t::run()
{
  if (_ran)
  {
    co_return fail(error_kind_t::protocol, "pipeline has already been run");
  }
  _ran = true;
  if (_finishers.empty())
  {
    co_return result_t<void>{};
  }
  if (_conn->is_broken())
  {
    error_t err = broken_error();
    for (auto &finisher : _finishers)
    {
      finisher(std::unexpected(err));
    }
    co_return std::unexpected(err);
  }

  if (_mode == pipeline_mode_t::atomic)
  {
    auto sync = detail::sync_message();
    _buffer.insert(_buffer.end(), sync.begin(), sync.end());
  }

  auto write_task = _conn->_transport->write_all(std::move(_buffer));

  result_t<void> outcome{};
  if (_mode == pipeline_mode_t::independent)
  {
    for (std::size_t i = 0; i < _finishers.size(); ++i)
    {
      auto data = co_await _conn->read_query_result();
      std::optional<error_t> read_error = data.has_value() ? std::nullopt : std::optional<error_t>(data.error());
      _finishers[i](std::move(data));
      if (_conn->is_broken())
      {
        error_t err = read_error.has_value() ? *read_error : broken_error();
        for (std::size_t j = i + 1; j < _finishers.size(); ++j)
        {
          _finishers[j](std::unexpected(err));
        }
        outcome = std::unexpected(err);
        break;
      }
    }
  }
  else
  {
    outcome = co_await run_atomic();
  }

  auto written = co_await std::move(write_task);
  if (!written.has_value() && outcome.has_value())
  {
    outcome = std::unexpected(written.error());
  }
  co_return outcome;
}

vio::task_t<result_t<void>> pipeline_t::run_atomic()
{
  std::size_t step = 0;
  detail::query_data_t data;
  std::optional<error_t> server_error;

  for (;;)
  {
    auto message = co_await _conn->_reader.next();
    if (!message.has_value())
    {
      _conn->_broken = true;
      error_t err = message.error();
      for (std::size_t j = step; j < _finishers.size(); ++j)
      {
        _finishers[j](std::unexpected(err));
      }
      co_return std::unexpected(err);
    }

    switch (static_cast<detail::backend_tag_t>(message->type))
    {
    case detail::backend_tag_t::row_description:
    {
      auto description = detail::parse_row_description(message->body);
      if (!description.has_value())
      {
        _conn->_broken = true;
        error_t err = description.error();
        for (std::size_t j = step; j < _finishers.size(); ++j)
        {
          _finishers[j](std::unexpected(err));
        }
        co_return std::unexpected(err);
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
      if (step < _finishers.size())
      {
        _finishers[step](std::move(data));
      }
      ++step;
      data = detail::query_data_t{};
      break;
    }
    case detail::backend_tag_t::empty_query_response:
      if (step < _finishers.size())
      {
        _finishers[step](std::move(data));
      }
      ++step;
      data = detail::query_data_t{};
      break;
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
      _conn->dispatch_notice(message->body);
      break;
    case detail::backend_tag_t::notification_response:
      _conn->dispatch_notification(message->body);
      break;
    case detail::backend_tag_t::ready_for_query:
      if (server_error.has_value())
      {
        if (step < _finishers.size())
        {
          _finishers[step](std::unexpected(*server_error));
          ++step;
        }
        for (std::size_t j = step; j < _finishers.size(); ++j)
        {
          _finishers[j](std::unexpected(aborted_error()));
        }
        co_return std::unexpected(*server_error);
      }
      for (std::size_t j = step; j < _finishers.size(); ++j)
      {
        _finishers[j](fail(error_kind_t::protocol, "pipeline step produced no result"));
      }
      co_return result_t<void>{};
    default:
      break;
    }
  }
}
} // namespace photon
