#include "photon/transaction.h"

#include "photon/connection.h"

namespace photon
{
void transaction_t::abandon()
{
  if (_active && _conn != nullptr)
  {
    _conn->poison();
  }
  _conn = nullptr;
  _active = false;
}

transaction_t::~transaction_t()
{
  abandon();
}

transaction_t &transaction_t::operator=(transaction_t &&other) noexcept
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

vio::task_t<result_t<void>> transaction_t::commit()
{
  if (_conn == nullptr || !_active)
  {
    co_return fail(error_kind_t::protocol, "transaction is not active");
  }
  _active = false;
  auto result = co_await _conn->execute("COMMIT");
  if (!result.has_value())
  {
    co_return std::unexpected(result.error());
  }
  co_return result_t<void>{};
}

vio::task_t<result_t<void>> transaction_t::rollback()
{
  if (_conn == nullptr || !_active)
  {
    co_return fail(error_kind_t::protocol, "transaction is not active");
  }
  _active = false;
  auto result = co_await _conn->execute("ROLLBACK");
  if (!result.has_value())
  {
    co_return std::unexpected(result.error());
  }
  co_return result_t<void>{};
}
} // namespace photon
