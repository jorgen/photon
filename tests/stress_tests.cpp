#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <structify/structify.h>

#include <photon/photon.h>

#include <vio/run.h>

namespace
{
struct n_row_t
{
  std::int64_t n;
  STFY_OBJ(n);
};

struct text_row_t
{
  std::string t;
  STFY_OBJ(t);
};

struct bytea_row_t
{
  photon::bytea_t b;
  STFY_OBJ(b);
};

const char *live_dsn()
{
  return std::getenv("PHOTON_PG_TEST_DSN");
}
} // namespace

TEST_CASE("stress: a result of 100k small rows streams correctly (frame reassembly)")
{
  const char *dsn = live_dsn();
  if (dsn == nullptr)
  {
    return;
  }
  std::string dsn_text = dsn;
  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto connection = co_await photon::connection_t::connect(loop, dsn_text);
      if (!connection.has_value())
      {
        MESSAGE("connect failed: " << connection.error().msg);
        co_return 1;
      }
      auto &conn = *connection;

      auto rows = co_await conn->query<n_row_t>("SELECT i::bigint AS n FROM generate_series(1, 100000) i");
      if (!rows.has_value())
      {
        MESSAGE("query failed: " << rows.error().msg);
        co_return 1;
      }
      auto all = rows->collect();
      REQUIRE(all.has_value());
      REQUIRE(all->size() == 100000);
      std::int64_t sum = 0;
      for (const auto &row : *all)
      {
        sum += row.n;
      }
      CHECK(sum == static_cast<std::int64_t>(100000) * 100001 / 2);

      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("stress: a single multi-megabyte value reassembles across many reads")
{
  const char *dsn = live_dsn();
  if (dsn == nullptr)
  {
    return;
  }
  std::string dsn_text = dsn;
  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto connection = co_await photon::connection_t::connect(loop, dsn_text);
      if (!connection.has_value())
      {
        co_return 1;
      }
      auto &conn = *connection;

      auto value = co_await conn->query_value<std::string>("SELECT repeat('x', 4000000)");
      REQUIRE(value.has_value());
      REQUIRE(value->has_value());
      CHECK((*value)->size() == 4000000);

      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("stress: bulk COPY IN then COPY OUT of 50k rows round-trips")
{
  const char *dsn = live_dsn();
  if (dsn == nullptr)
  {
    return;
  }
  std::string dsn_text = dsn;
  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto connection = co_await photon::connection_t::connect(loop, dsn_text);
      if (!connection.has_value())
      {
        co_return 1;
      }
      auto &conn = *connection;

      (void)co_await conn->execute("DROP TABLE IF EXISTS stress_copy");
      REQUIRE((co_await conn->execute("CREATE TABLE stress_copy(id int, v text)")).has_value());

      auto ci = co_await conn->copy_in("COPY stress_copy FROM STDIN");
      REQUIRE(ci.has_value());
      std::string buffer;
      buffer.reserve(50000 * 12);
      for (int i = 0; i < 50000; ++i)
      {
        buffer.append(std::to_string(i));
        buffer.append("\trow");
        buffer.append(std::to_string(i));
        buffer.push_back('\n');
      }
      REQUIRE((co_await ci->write(buffer)).has_value());
      auto done = co_await ci->finish();
      REQUIRE(done.has_value());
      CHECK(done->rows_affected == 50000);

      auto reader = co_await conn->copy_out("COPY stress_copy TO STDOUT");
      REQUIRE(reader.has_value());
      auto text = co_await reader->read_all();
      REQUIRE(text.has_value());
      CHECK(std::count(text->begin(), text->end(), '\n') == 50000);

      (void)co_await conn->execute("DROP TABLE stress_copy");
      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("stress: the pool multiplexes many concurrent queries")
{
  const char *dsn = live_dsn();
  if (dsn == nullptr)
  {
    return;
  }
  std::string dsn_text = dsn;
  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto params = photon::parse_dsn(dsn_text);
      if (!params.has_value())
      {
        co_return 1;
      }
      photon::pool_t pool(loop, *params, photon::pool_options_t{.max_size = 4});

      constexpr int concurrent = 200;
      std::vector<vio::task_t<photon::result_t<std::optional<std::int32_t>>>> tasks;
      tasks.reserve(concurrent);
      for (int i = 0; i < concurrent; ++i)
      {
        tasks.push_back(pool.query_value<std::int32_t>("SELECT $1::int", i));
      }
      int ok = 0;
      for (int i = 0; i < concurrent; ++i)
      {
        auto value = co_await std::move(tasks[static_cast<std::size_t>(i)]);
        if (value.has_value() && value->has_value() && **value == i)
        {
          ++ok;
        }
      }
      CHECK(ok == concurrent);
      CHECK(pool.size() <= 4);
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("corner: empty result set, NULL vs empty string, unicode, and full-byte bytea")
{
  const char *dsn = live_dsn();
  if (dsn == nullptr)
  {
    return;
  }
  std::string dsn_text = dsn;
  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto connection = co_await photon::connection_t::connect(loop, dsn_text);
      if (!connection.has_value())
      {
        co_return 1;
      }
      auto &conn = *connection;

      auto empty = co_await conn->query<n_row_t>("SELECT i AS n FROM generate_series(1, 0) i");
      REQUIRE(empty.has_value());
      CHECK(empty->empty());
      auto collected = empty->collect();
      REQUIRE(collected.has_value());
      CHECK(collected->empty());

      auto empty_str = co_await conn->query_value<std::string>("SELECT ''::text");
      REQUIRE(empty_str.has_value());
      REQUIRE(empty_str->has_value());
      CHECK((*empty_str)->empty());

      auto null_str = co_await conn->query_value<std::optional<std::string>>("SELECT NULL::text");
      REQUIRE(null_str.has_value());
      REQUIRE(null_str->has_value());
      CHECK_FALSE((*null_str)->has_value());

      auto unicode = co_await conn->query_value<std::string>("SELECT $1::text", std::string("héllo — 世界 🌍"));
      REQUIRE(unicode.has_value());
      REQUIRE(unicode->has_value());
      CHECK(**unicode == "héllo — 世界 🌍");

      photon::bytea_t all_bytes;
      all_bytes.data.resize(256);
      for (int i = 0; i < 256; ++i)
      {
        all_bytes.data[static_cast<std::size_t>(i)] = static_cast<std::byte>(i);
      }
      auto echoed = co_await conn->query_one<bytea_row_t>("SELECT $1::bytea AS b", all_bytes);
      REQUIRE(echoed.has_value());
      REQUIRE(echoed->has_value());
      REQUIRE((*echoed)->b.data.size() == 256);
      bool identical = true;
      for (int i = 0; i < 256; ++i)
      {
        if ((*echoed)->b.data[static_cast<std::size_t>(i)] != static_cast<std::byte>(i))
        {
          identical = false;
        }
      }
      CHECK(identical);

      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("corner: empty COPY, and cancelling with no query running is a no-op")
{
  const char *dsn = live_dsn();
  if (dsn == nullptr)
  {
    return;
  }
  std::string dsn_text = dsn;
  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto connection = co_await photon::connection_t::connect(loop, dsn_text);
      if (!connection.has_value())
      {
        co_return 1;
      }
      auto &conn = *connection;

      (void)co_await conn->execute("DROP TABLE IF EXISTS empty_copy");
      REQUIRE((co_await conn->execute("CREATE TABLE empty_copy(id int)")).has_value());

      auto ci = co_await conn->copy_in("COPY empty_copy FROM STDIN");
      REQUIRE(ci.has_value());
      auto done = co_await ci->finish();
      REQUIRE(done.has_value());
      CHECK(done->rows_affected == 0);

      auto reader = co_await conn->copy_out("COPY empty_copy TO STDOUT");
      REQUIRE(reader.has_value());
      auto text = co_await reader->read_all();
      REQUIRE(text.has_value());
      CHECK(text->empty());

      auto handle = conn->cancel_handle();
      auto cancelled = co_await handle.cancel(loop);
      CHECK(cancelled.has_value());

      auto still_ok = co_await conn->query_value<std::int32_t>("SELECT 7");
      REQUIRE(still_ok.has_value());
      REQUIRE(still_ok->has_value());
      CHECK(**still_ok == 7);

      (void)co_await conn->execute("DROP TABLE empty_copy");
      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("corner: COPY on a non-COPY statement fails fast and the connection stays usable")
{
  const char *dsn = live_dsn();
  if (dsn == nullptr)
  {
    return;
  }
  std::string dsn_text = dsn;
  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto connection = co_await photon::connection_t::connect(loop, dsn_text);
      if (!connection.has_value())
      {
        co_return 1;
      }
      auto &conn = *connection;

      auto bad_in = co_await conn->copy_in("SELECT 1");
      CHECK_FALSE(bad_in.has_value());
      CHECK(bad_in.error().kind == photon::error_kind_t::protocol);

      auto still_ok = co_await conn->query_value<std::int32_t>("SELECT 42");
      REQUIRE(still_ok.has_value());
      REQUIRE(still_ok->has_value());
      CHECK(**still_ok == 42);

      auto bad_out = co_await conn->copy_out("SELECT 1");
      CHECK_FALSE(bad_out.has_value());
      CHECK(bad_out.error().kind == photon::error_kind_t::protocol);

      auto again = co_await conn->query_value<std::int32_t>("SELECT 43");
      REQUIRE(again.has_value());
      REQUIRE(again->has_value());
      CHECK(**again == 43);
      CHECK_FALSE(conn->is_broken());

      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("corner: a timed-out (broken) connection rejects further queries instead of hanging")
{
  const char *dsn = live_dsn();
  if (dsn == nullptr)
  {
    return;
  }
  std::string dsn_text = dsn;
  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto params = photon::parse_dsn(dsn_text);
      if (!params.has_value())
      {
        co_return 1;
      }
      params->query_timeout = std::chrono::milliseconds{300};
      auto connection = co_await photon::connection_t::connect(loop, *params);
      if (!connection.has_value())
      {
        co_return 1;
      }
      auto &conn = *connection;

      auto slow = co_await conn->execute("SELECT pg_sleep(5)");
      REQUIRE_FALSE(slow.has_value());
      CHECK(slow.error().kind == photon::error_kind_t::timeout);
      REQUIRE(conn->is_broken());

      auto reuse = co_await conn->query_value<std::int32_t>("SELECT 1");
      REQUIRE_FALSE(reuse.has_value());
      CHECK(reuse.error().kind == photon::error_kind_t::connection);

      auto reuse_copy = co_await conn->copy_in("COPY x FROM STDIN");
      CHECK_FALSE(reuse_copy.has_value());

      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}
