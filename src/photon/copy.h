#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vio/task.h>

#include "photon/error.h"
#include "photon/result.h"

namespace photon
{
class connection_t;

class copy_in_t
{
public:
  copy_in_t() = default;
  copy_in_t(const copy_in_t &) = delete;
  copy_in_t &operator=(const copy_in_t &) = delete;
  copy_in_t(copy_in_t &&other) noexcept
    : _conn(other._conn)
    , _active(other._active)
  {
    other._conn = nullptr;
    other._active = false;
  }
  copy_in_t &operator=(copy_in_t &&other) noexcept;
  ~copy_in_t();

  vio::task_t<result_t<void>> write(std::span<const std::uint8_t> data);
  vio::task_t<result_t<void>> write(std::string_view data);
  vio::task_t<result_t<void>> write_row(std::span<const std::optional<std::string>> fields);
  vio::task_t<result_t<command_result_t>> finish();
  vio::task_t<result_t<void>> fail(std::string_view message);

  [[nodiscard]] bool active() const
  {
    return _active;
  }

private:
  friend class connection_t;

  explicit copy_in_t(connection_t *connection)
    : _conn(connection)
    , _active(true)
  {
  }

  void abandon();

  connection_t *_conn = nullptr;
  bool _active = false;
};

class copy_out_t
{
public:
  copy_out_t() = default;
  copy_out_t(const copy_out_t &) = delete;
  copy_out_t &operator=(const copy_out_t &) = delete;
  copy_out_t(copy_out_t &&other) noexcept
    : _conn(other._conn)
    , _active(other._active)
    , _result(std::move(other._result))
  {
    other._conn = nullptr;
    other._active = false;
  }
  copy_out_t &operator=(copy_out_t &&other) noexcept;
  ~copy_out_t();

  vio::task_t<result_t<std::optional<std::vector<std::uint8_t>>>> read_chunk();
  vio::task_t<result_t<std::string>> read_all();

  [[nodiscard]] bool done() const
  {
    return !_active;
  }
  [[nodiscard]] const std::optional<command_result_t> &result() const
  {
    return _result;
  }

private:
  friend class connection_t;

  explicit copy_out_t(connection_t *connection)
    : _conn(connection)
    , _active(true)
  {
  }

  void abandon();

  connection_t *_conn = nullptr;
  bool _active = false;
  std::optional<command_result_t> _result;
};
} // namespace photon
