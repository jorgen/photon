#include <cstdint>
#include <print>
#include <string>

#include <photon/photon.h>
#include <photon/prism.h>

#include <prism/prism.h>

#include <vio/run.h>

namespace
{
struct item_t
{
  std::int32_t id;
  std::string label;
  STFY_OBJ(id, label);
};

vio::task_t<prism::response_t> list_items(photon::prism::db db)
{
  auto items = co_await db->query<item_t>("SELECT id, label FROM photon_items ORDER BY id");
  if (!items.has_value())
  {
    co_return prism::response_t::text(prism::status_t::internal_server_error, items.error().msg);
  }
  auto rows = items->collect();
  if (!rows.has_value())
  {
    co_return prism::response_t::text(prism::status_t::internal_server_error, rows.error().msg);
  }
  co_return prism::json::respond(prism::status_t::ok, *rows);
}

vio::task_t<prism::response_t> count_items(photon::prism::db db)
{
  auto n = co_await db->query_value<std::int64_t>("SELECT count(*) FROM photon_items");
  if (!n.has_value())
  {
    co_return prism::response_t::text(prism::status_t::internal_server_error, n.error().msg);
  }
  co_return prism::response_t::text(prism::status_t::ok, std::to_string(n->value_or(0)) + "\n");
}
} // namespace

VIO_MAIN(loop, argc, argv)
{
  std::string dsn = argc > 1 ? argv[1] : "postgresql://photon:photon@127.0.0.1:55432/photon";
  auto params = photon::parse_dsn(dsn);
  if (!params.has_value())
  {
    std::println(stderr, "bad DSN: {}", params.error().msg);
    co_return 1;
  }

  std::println("photon+prism service on http://127.0.0.1:8080 (GET /items, /count)");
  co_return co_await prism::run(loop, "127.0.0.1", 8080,
                                [params = *params](prism::app_t &app)
                                {
                                  photon::prism::provide(app, params);
                                  app.get("/items", list_items);
                                  app.get("/count", count_items);
                                });
}
