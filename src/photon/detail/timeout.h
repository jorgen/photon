#pragma once

#include <chrono>
#include <utility>

#include <vio/cancellation.h>
#include <vio/event_loop.h>
#include <vio/operation/sleep.h>
#include <vio/task.h>

namespace photon::detail
{
template <typename Result, typename MakeOp>
vio::task_t<Result> with_timeout(vio::event_loop_t &loop, std::chrono::milliseconds timeout, MakeOp make_op)
{
  if (timeout <= std::chrono::milliseconds::zero())
  {
    co_return co_await make_op(static_cast<vio::cancellation_t *>(nullptr));
  }

  vio::cancellation_t token;
  auto fut = make_op(&token);

  auto watchdog = [](vio::event_loop_t &el, vio::cancellation_t &tok, std::chrono::milliseconds dur) -> vio::task_t<void>
  {
    auto fired = co_await vio::sleep(el, dur, &tok);
    if (fired.has_value() && !tok.is_cancelled())
    {
      tok.cancel();
    }
  }(loop, token, timeout);

  Result result = co_await fut;
  token.cancel();
  co_await std::move(watchdog);
  co_return result;
}
} // namespace photon::detail
