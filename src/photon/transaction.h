#pragma once

#include <utility>

#include <vio/task.h>

#include "photon/error.h"

namespace photon
{
class connection_t;

class transaction_t
{
public:
  transaction_t() = default;
  transaction_t(const transaction_t &) = delete;
  transaction_t &operator=(const transaction_t &) = delete;
  transaction_t(transaction_t &&other) noexcept
    : _conn(other._conn)
    , _active(other._active)
  {
    other._conn = nullptr;
    other._active = false;
  }
  transaction_t &operator=(transaction_t &&other) noexcept;
  ~transaction_t();

  vio::task_t<result_t<void>> commit();
  vio::task_t<result_t<void>> rollback();

  [[nodiscard]] bool active() const
  {
    return _active;
  }

private:
  friend class connection_t;

  explicit transaction_t(connection_t *connection)
    : _conn(connection)
    , _active(true)
  {
  }

  void abandon();

  connection_t *_conn = nullptr;
  bool _active = false;
};
} // namespace photon
