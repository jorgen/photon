#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include <photon/photon.h>
#include <photon/prism.h>

#include <prism/prism.h>

#include <vio/run.h>

namespace
{
vio::task_t<prism::response_t> count_route(photon::prism::db db)
{
  auto n = co_await db->query_value<std::int64_t>("SELECT count(*) FROM photon_prism_items");
  if (!n.has_value())
  {
    co_return prism::response_t::text(prism::status_t::internal_server_error, n.error().msg);
  }
  co_return prism::response_t::text(prism::status_t::ok, std::to_string(n->value_or(-1)));
}
} // namespace

TEST_CASE("prism integration: a handler queries a per-thread photon pool")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  if (dsn == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN is unset; skipping the prism integration test");
    return;
  }
  std::string dsn_text = dsn;

  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto params = photon::parse_dsn(dsn_text);
      if (!params.has_value())
      {
        MESSAGE("bad DSN: " << params.error().msg);
        co_return 1;
      }

      auto seed = co_await photon::connection_t::connect(loop, *params);
      if (!seed.has_value())
      {
        MESSAGE("connect failed: " << seed.error().msg);
        co_return 1;
      }
      (void)co_await (*seed)->execute("DROP TABLE IF EXISTS photon_prism_items");
      auto create = co_await (*seed)->execute("CREATE TABLE photon_prism_items(id int)");
      REQUIRE(create.has_value());
      auto insert = co_await (*seed)->execute("INSERT INTO photon_prism_items VALUES (1), (2), (3), (4)");
      REQUIRE(insert.has_value());
      co_await (*seed)->close();

      prism::app_t app;
      photon::prism::provide(app, *params);
      app.get("/count", count_route);

      prism::request_t request;
      request.method = prism::method_t::get;
      request.target = "/count";
      request.path = "/count";
      request.loop = &loop;
      auto response = co_await app.handle(std::move(request));
      CHECK(response.status == prism::status_t::ok);
      CHECK(response.body == "4");

      auto again = co_await photon::connection_t::connect(loop, *params);
      if (again.has_value())
      {
        (void)co_await (*again)->execute("DROP TABLE IF EXISTS photon_prism_items");
        co_await (*again)->close();
      }
      co_return 0;
    });
  CHECK(rc == 0);
}
