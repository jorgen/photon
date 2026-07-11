#pragma once

#include <utility>

#include <prism/app.h>
#include <prism/params.h>

#include <vio/event_loop.h>

#include "photon/connect_params.h"
#include "photon/pool.h"

namespace photon::prism
{
inline void provide(::prism::app_t &app, connect_params_t params, pool_options_t options = {})
{
  app.provide_per_thread<pool_t>([params = std::move(params), options](vio::event_loop_t &loop) { return pool_t{loop, params, options}; });
}

using db = ::prism::per_thread<pool_t>;
} // namespace photon::prism
