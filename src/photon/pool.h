#pragma once

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vio/event_loop.h>
#include <vio/task.h>

#include "photon/connect_params.h"
#include "photon/connection.h"
#include "photon/error.h"

namespace photon
{
struct pool_options_t
{
  std::size_t max_size = 8;
  std::size_t min_size = 0;
};

class pool_t;

class lease_t
{
public:
  lease_t() = default;
  lease_t(pool_t *pool, std::shared_ptr<connection_t> connection)
    : _pool(pool)
    , _connection(std::move(connection))
  {
  }
  lease_t(const lease_t &) = delete;
  lease_t &operator=(const lease_t &) = delete;
  lease_t(lease_t &&other) noexcept
    : _pool(other._pool)
    , _connection(std::move(other._connection))
  {
    other._pool = nullptr;
  }
  lease_t &operator=(lease_t &&other) noexcept
  {
    if (this != &other)
    {
      release();
      _pool = other._pool;
      _connection = std::move(other._connection);
      other._pool = nullptr;
    }
    return *this;
  }
  ~lease_t()
  {
    release();
  }

  connection_t *operator->() const
  {
    return _connection.get();
  }
  connection_t &operator*() const
  {
    return *_connection;
  }
  [[nodiscard]] const std::shared_ptr<connection_t> &connection() const
  {
    return _connection;
  }

private:
  void release();

  pool_t *_pool = nullptr;
  std::shared_ptr<connection_t> _connection;
};

class pool_t
{
public:
  pool_t(vio::event_loop_t &loop, connect_params_t params, pool_options_t options = {})
    : _loop(&loop)
    , _params(std::move(params))
    , _options(options)
  {
  }
  pool_t(const pool_t &) = delete;
  pool_t &operator=(const pool_t &) = delete;
  pool_t(pool_t &&) = default;
  pool_t &operator=(pool_t &&) = default;

  vio::task_t<result_t<lease_t>> acquire();

  template <typename Row, typename... Params>
  vio::task_t<result_t<result_set_t<Row>>> query(std::string sql, Params... params)
  {
    auto lease = co_await acquire();
    if (!lease.has_value())
    {
      co_return std::unexpected(lease.error());
    }
    co_return co_await (*lease)->template query<Row>(sql, std::move(params)...);
  }

  template <typename Row, typename... Params>
  vio::task_t<result_t<std::optional<Row>>> query_one(std::string sql, Params... params)
  {
    auto lease = co_await acquire();
    if (!lease.has_value())
    {
      co_return std::unexpected(lease.error());
    }
    co_return co_await (*lease)->template query_one<Row>(sql, std::move(params)...);
  }

  template <typename T, typename... Params>
  vio::task_t<result_t<std::optional<T>>> query_value(std::string sql, Params... params)
  {
    auto lease = co_await acquire();
    if (!lease.has_value())
    {
      co_return std::unexpected(lease.error());
    }
    co_return co_await (*lease)->template query_value<T>(sql, std::move(params)...);
  }

  template <typename... Params>
  vio::task_t<result_t<command_result_t>> execute(std::string sql, Params... params)
  {
    auto lease = co_await acquire();
    if (!lease.has_value())
    {
      co_return std::unexpected(lease.error());
    }
    co_return co_await (*lease)->execute(sql, std::move(params)...);
  }

  [[nodiscard]] std::size_t size() const
  {
    return _total;
  }
  [[nodiscard]] std::size_t idle() const
  {
    return _idle.size();
  }
  [[nodiscard]] std::size_t max_size() const
  {
    return _options.max_size;
  }

private:
  friend class lease_t;

  // A parked acquire(). release() hands a connection directly to the oldest
  // waiter (FIFO, no barge) via `handoff`, or wakes it with an empty handoff to
  // retry after a slot frees. The node lives in the parked acquire's coroutine
  // frame; its destructor deregisters it so an abandoned waiter can't be resumed.
  struct waiter_t
  {
    pool_t *pool;
    std::shared_ptr<connection_t> handoff;
    std::coroutine_handle<> continuation;

    explicit waiter_t(pool_t *owner)
      : pool(owner)
    {
    }
    waiter_t(const waiter_t &) = delete;
    waiter_t &operator=(const waiter_t &) = delete;
    ~waiter_t()
    {
      auto &waiters = pool->_waiters;
      waiters.erase(std::remove(waiters.begin(), waiters.end(), this), waiters.end());
    }

    bool await_ready() const noexcept
    {
      return false;
    }
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
      continuation = handle;
      pool->_waiters.push_back(this);
    }
    std::shared_ptr<connection_t> await_resume() noexcept
    {
      return std::move(handoff);
    }
  };

  void release(std::shared_ptr<connection_t> connection);
  void wake_one_waiter(std::shared_ptr<connection_t> connection);

  vio::event_loop_t *_loop;
  connect_params_t _params;
  pool_options_t _options;
  std::vector<std::shared_ptr<connection_t>> _idle;
  std::deque<waiter_t *> _waiters;
  std::size_t _total = 0;
};
} // namespace photon
