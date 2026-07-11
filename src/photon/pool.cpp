#include "photon/pool.h"

namespace photon
{
void lease_t::release()
{
  if (_pool != nullptr && _connection)
  {
    _pool->release(std::move(_connection));
  }
  _pool = nullptr;
  _connection = nullptr;
}

vio::task_t<result_t<lease_t>> pool_t::acquire()
{
  for (;;)
  {
    while (!_idle.empty())
    {
      auto connection = std::move(_idle.back());
      _idle.pop_back();
      if (connection->is_broken())
      {
        --_total;
        continue;
      }
      co_return lease_t{this, std::move(connection)};
    }

    if (_total < _options.max_size)
    {
      ++_total;
      auto connection = co_await connection_t::connect(*_loop, _params);
      if (!connection.has_value())
      {
        --_total;
        wake_one_waiter({});
        co_return std::unexpected(connection.error());
      }
      co_return lease_t{this, std::move(*connection)};
    }

    waiter_t waiter{this};
    auto handed = co_await waiter;
    if (handed)
    {
      co_return lease_t{this, std::move(handed)};
    }
  }
}

void pool_t::release(std::shared_ptr<connection_t> connection)
{
  if (connection->is_broken())
  {
    --_total;
    wake_one_waiter({});
    return;
  }
  if (!_waiters.empty())
  {
    wake_one_waiter(std::move(connection));
    return;
  }
  _idle.push_back(std::move(connection));
}

void pool_t::wake_one_waiter(std::shared_ptr<connection_t> connection)
{
  if (_waiters.empty())
  {
    if (connection)
    {
      _idle.push_back(std::move(connection));
    }
    return;
  }
  waiter_t *waiter = _waiters.front();
  _waiters.pop_front();
  waiter->handoff = std::move(connection);
  auto continuation = waiter->continuation;
  _loop->run_in_loop([continuation]() { continuation.resume(); });
}
} // namespace photon
