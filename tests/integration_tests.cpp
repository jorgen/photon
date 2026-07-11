#include <doctest/doctest.h>

#include <chrono>
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
struct user_t
{
  std::int32_t id;
  std::string name;
  std::optional<std::int32_t> age;
  STFY_OBJ(id, name, age);
};

struct types_row_t
{
  photon::uuid_t id;
  std::chrono::system_clock::time_point at;
  std::chrono::sys_days day;
  photon::bytea_t blob;
  photon::json_t doc;
  photon::numeric_t amount;
  STFY_OBJ(id, at, day, blob, doc, amount);
};

struct ssl_status_t
{
  bool ssl;
  STFY_OBJ(ssl);
};

struct id_row_t
{
  std::int32_t id;
  STFY_OBJ(id);
};
} // namespace

TEST_CASE("integration: connect over SCRAM, typed SELECT with a bound param, NULL handling")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  if (dsn == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN is unset; skipping the live Postgres integration test");
    return;
  }
  std::string dsn_text = dsn;

  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto params = photon::parse_dsn(dsn_text);
      if (!params.has_value())
      {
        MESSAGE("bad PHOTON_PG_TEST_DSN: " << params.error().msg);
        co_return 1;
      }

      auto connection = co_await photon::connection_t::connect(loop, *params);
      if (!connection.has_value())
      {
        MESSAGE("connect failed: " << connection.error().msg);
        co_return 1;
      }
      auto &conn = *connection;

      auto drop = co_await conn->execute("DROP TABLE IF EXISTS photon_users");
      CHECK(drop.has_value());

      auto create = co_await conn->execute("CREATE TABLE photon_users(id int PRIMARY KEY, name text NOT NULL, age int)");
      CHECK(create.has_value());

      auto insert = co_await conn->execute("INSERT INTO photon_users(id, name, age) VALUES (1, 'ada', 36), (2, 'bob', NULL), (3, 'carol', 15)");
      REQUIRE(insert.has_value());
      CHECK(insert->rows_affected == 3);

      auto adults = co_await conn->query<user_t>("SELECT id, name, age FROM photon_users WHERE age > $1 ORDER BY id", 18);
      if (!adults.has_value())
      {
        MESSAGE("query failed: " << adults.error().msg);
        co_return 1;
      }
      CHECK(adults->size() == 1);
      auto first = adults->at(0);
      REQUIRE(first.has_value());
      CHECK(first->id == 1);
      CHECK(first->name == "ada");
      REQUIRE(first->age.has_value());
      CHECK(*first->age == 36);

      auto everyone = co_await conn->query<user_t>("SELECT id, name, age FROM photon_users ORDER BY id");
      REQUIRE(everyone.has_value());
      auto all = everyone->collect();
      REQUIRE(all.has_value());
      REQUIRE(all->size() == 3);
      CHECK((*all)[0].name == "ada");
      CHECK_FALSE((*all)[1].age.has_value());
      CHECK((*all)[2].name == "carol");

      auto bad = co_await conn->query<user_t>("SELECT id, name, age FROM no_such_photon_table");
      REQUIRE_FALSE(bad.has_value());
      CHECK(bad.error().kind == photon::error_kind_t::server);
      CHECK(bad.error().sqlstate() == "42P01");
      CHECK(photon::is_undefined_table(bad.error()));

      auto reuse = co_await conn->execute("DROP TABLE photon_users");
      CHECK(reuse.has_value());

      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("integration: rich server errors, query_value, prepared statements, arrays, notices, range-for")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  if (dsn == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN is unset; skipping the API-revisit integration test");
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
      CHECK_FALSE(conn->is_broken());

      (void)co_await conn->execute("DROP TABLE IF EXISTS photon_items");
      auto create = co_await conn->execute("CREATE TABLE photon_items(id int PRIMARY KEY, label text)");
      REQUIRE(create.has_value());
      auto ins = co_await conn->execute("INSERT INTO photon_items(id, label) VALUES (1, 'a'), (2, 'b'), (3, 'c')");
      REQUIRE(ins.has_value());
      CHECK(ins->rows_affected == 3);

      auto dup = co_await conn->execute("INSERT INTO photon_items(id, label) VALUES (1, 'x')");
      REQUIRE_FALSE(dup.has_value());
      CHECK(photon::is_unique_violation(dup.error()));
      REQUIRE(dup.error().server.has_value());
      CHECK(dup.error().server->constraint == "photon_items_pkey");
      CHECK_FALSE(dup.error().server->detail.empty());
      CHECK_FALSE(conn->is_broken());

      auto count = co_await conn->query_value<std::int64_t>("SELECT count(*) FROM photon_items");
      REQUIRE(count.has_value());
      REQUIRE(count->has_value());
      CHECK(**count == 3);

      auto missing = co_await conn->query_value<std::int32_t>("SELECT id FROM photon_items WHERE id = $1", 999);
      REQUIRE(missing.has_value());
      CHECK_FALSE(missing->has_value());

      auto stmt = co_await conn->prepare("SELECT id FROM photon_items WHERE id = $1");
      REQUIRE(stmt.has_value());
      for (int id : {2, 3})
      {
        auto one = co_await stmt->query<id_row_t>(id);
        REQUIRE(one.has_value());
        auto value = one->one();
        REQUIRE(value.has_value());
        REQUIRE(value->has_value());
        CHECK((*value)->id == id);
      }

      auto in_list = co_await conn->query<id_row_t>("SELECT id FROM photon_items WHERE id = ANY($1) ORDER BY id", std::vector<int>{1, 3});
      REQUIRE(in_list.has_value());
      auto ids = in_list->collect();
      REQUIRE(ids.has_value());
      REQUIRE(ids->size() == 2);
      CHECK((*ids)[0].id == 1);
      CHECK((*ids)[1].id == 3);

      int seen = 0;
      auto ordered = co_await conn->query<id_row_t>("SELECT id FROM photon_items ORDER BY id");
      REQUIRE(ordered.has_value());
      for (auto row : *ordered)
      {
        REQUIRE(row.has_value());
        ++seen;
      }
      CHECK(seen == 3);

      std::string notice_text;
      conn->on_notice([&notice_text](const photon::server_error_t &notice) { notice_text = notice.message; });
      auto raised = co_await conn->execute("DO $$ BEGIN RAISE NOTICE 'photon-notice-42'; END $$;");
      REQUIRE(raised.has_value());
      CHECK(notice_text.find("photon-notice-42") != std::string::npos);

      (void)co_await conn->execute("DROP TABLE photon_items");
      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("integration: pool reuses idle connections and bounds concurrency to max_size")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  if (dsn == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN is unset; skipping the pool integration test");
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
      photon::pool_t pool(loop, *params, photon::pool_options_t{.max_size = 2, .min_size = 0});

      {
        auto lease = co_await pool.acquire();
        REQUIRE(lease.has_value());
        auto value = co_await (*lease)->query_value<std::int32_t>("SELECT 1");
        REQUIRE(value.has_value());
      }
      CHECK(pool.size() == 1);
      CHECK(pool.idle() == 1);

      {
        auto lease = co_await pool.acquire();
        REQUIRE(lease.has_value());
      }
      CHECK(pool.size() == 1);

      constexpr int concurrent = 6;
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
      CHECK(pool.size() <= 2);

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("integration: sslmode=require encrypts the connection")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  const char *tls = std::getenv("PHOTON_PG_SSLROOTCERT");
  if (dsn == nullptr || tls == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN / PHOTON_PG_SSLROOTCERT unset; skipping the TLS integration test");
    return;
  }
  std::string dsn_text = dsn;

  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto params = photon::parse_dsn(dsn_text);
      if (!params.has_value())
      {
        MESSAGE("bad PHOTON_PG_TEST_DSN: " << params.error().msg);
        co_return 1;
      }
      params->sslmode = photon::sslmode_t::require;

      auto connection = co_await photon::connection_t::connect(loop, *params);
      if (!connection.has_value())
      {
        MESSAGE("TLS connect failed (is ssl enabled on the server?): " << connection.error().msg);
        co_return 1;
      }
      auto &conn = *connection;

      auto status = co_await conn->query<ssl_status_t>("SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()");
      if (!status.has_value())
      {
        MESSAGE("pg_stat_ssl query failed: " << status.error().msg);
        co_return 1;
      }
      auto row = status->one();
      REQUIRE(row.has_value());
      REQUIRE(row->has_value());
      CHECK((*row)->ssl == true);

      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("integration: sslmode=verify-full verifies the server certificate")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  const char *root = std::getenv("PHOTON_PG_SSLROOTCERT");
  if (dsn == nullptr || root == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN / PHOTON_PG_SSLROOTCERT unset; skipping verify-full test");
    return;
  }
  std::string dsn_text = dsn;
  std::string root_text = root;

  int rc = vio::run(
    [&dsn_text, &root_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto params = photon::parse_dsn(dsn_text);
      if (!params.has_value())
      {
        MESSAGE("bad DSN: " << params.error().msg);
        co_return 1;
      }

      auto verified = *params;
      verified.sslmode = photon::sslmode_t::verify_full;
      verified.sslrootcert = root_text;
      auto trusted = co_await photon::connection_t::connect(loop, verified);
      if (!trusted.has_value())
      {
        MESSAGE("verify-full with the CA failed: " << trusted.error().msg);
        co_return 1;
      }
      co_await (*trusted)->close();

      auto untrusted_params = *params;
      untrusted_params.sslmode = photon::sslmode_t::verify_full;
      auto untrusted = co_await photon::connection_t::connect(loop, untrusted_params);
      CHECK_FALSE(untrusted.has_value());

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("integration: type codec round-trips for uuid, timestamptz, date, bytea, jsonb, numeric")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  if (dsn == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN is unset; skipping the type codec integration test");
    return;
  }
  std::string dsn_text = dsn;

  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      using namespace std::chrono;

      auto connection = co_await photon::connection_t::connect(loop, dsn_text);
      if (!connection.has_value())
      {
        MESSAGE("connect failed: " << connection.error().msg);
        co_return 1;
      }
      auto &conn = *connection;

      (void)co_await conn->execute("DROP TABLE IF EXISTS photon_types");
      auto create = co_await conn->execute("CREATE TABLE photon_types(id uuid PRIMARY KEY, at timestamptz, day date, blob bytea, doc jsonb, amount numeric)");
      REQUIRE(create.has_value());

      auto id = photon::uuid_t::parse("550e8400-e29b-41d4-a716-446655440000");
      REQUIRE(id.has_value());
      system_clock::time_point at = sys_days{year{2021} / 6 / 15} + hours{12} + minutes{34} + seconds{56} + microseconds{789012};
      auto day = sys_days{year{2021} / 6 / 15};
      photon::bytea_t blob{{std::byte{0x01}, std::byte{0x02}, std::byte{0xff}}};
      photon::json_t doc{"{\"k\": 42}"};
      photon::numeric_t amount{"12345.6789"};

      auto ins = co_await conn->execute("INSERT INTO photon_types(id, at, day, blob, doc, amount) VALUES ($1, $2, $3, $4, $5, $6)", *id, at, day, blob, doc, amount);
      if (!ins.has_value())
      {
        MESSAGE("insert failed: " << ins.error().msg);
        co_return 1;
      }

      auto selected = co_await conn->query<types_row_t>("SELECT id, at, day, blob, doc, amount FROM photon_types");
      if (!selected.has_value())
      {
        MESSAGE("select failed: " << selected.error().msg);
        co_return 1;
      }
      auto row = selected->one();
      REQUIRE(row.has_value());
      REQUIRE(row->has_value());
      const auto &got = **row;

      CHECK(got.id == *id);
      CHECK(got.day == day);
      auto want_us = duration_cast<microseconds>(at.time_since_epoch()).count();
      auto got_us = duration_cast<microseconds>(got.at.time_since_epoch()).count();
      CHECK(want_us == got_us);
      REQUIRE(got.blob.data.size() == 3);
      CHECK(static_cast<std::uint8_t>(got.blob.data[2]) == 0xff);
      CHECK(got.doc.value.find("42") != std::string::npos);
      CHECK(got.amount.value == "12345.6789");

      (void)co_await conn->execute("DROP TABLE photon_types");
      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("integration: date BC and infinity round-trips through Postgres")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  if (dsn == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN is unset; skipping the date edge-case integration test");
    return;
  }
  std::string dsn_text = dsn;

  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      using namespace std::chrono;

      auto connection = co_await photon::connection_t::connect(loop, dsn_text);
      if (!connection.has_value())
      {
        MESSAGE("connect failed: " << connection.error().msg);
        co_return 1;
      }
      auto &conn = *connection;

      auto bc = sys_days{year{-44} / 3 / 15};
      auto round_trip = co_await conn->query_value<sys_days>("SELECT $1::date", bc);
      if (!round_trip.has_value())
      {
        MESSAGE("BC date round-trip failed: " << round_trip.error().msg);
        co_return 1;
      }
      REQUIRE(round_trip->has_value());
      CHECK(**round_trip == bc);

      auto pos_inf = co_await conn->query_value<sys_days>("SELECT 'infinity'::date");
      REQUIRE(pos_inf.has_value());
      REQUIRE(pos_inf->has_value());
      CHECK(**pos_inf == sys_days::max());

      auto neg_inf = co_await conn->query_value<sys_days>("SELECT '-infinity'::date");
      REQUIRE(neg_inf.has_value());
      REQUIRE(neg_inf->has_value());
      CHECK(**neg_inf == sys_days::min());

      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("integration: transactions commit, roll back, and poison the connection when dropped active")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  if (dsn == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN is unset; skipping the transaction integration test");
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

      (void)co_await conn->execute("DROP TABLE IF EXISTS photon_txn");
      auto create = co_await conn->execute("CREATE TABLE photon_txn(id int PRIMARY KEY)");
      REQUIRE(create.has_value());

      {
        auto txn = co_await conn->begin();
        REQUIRE(txn.has_value());
        auto ins = co_await conn->execute("INSERT INTO photon_txn(id) VALUES (1)");
        REQUIRE(ins.has_value());
        auto committed = co_await txn->commit();
        REQUIRE(committed.has_value());
        CHECK_FALSE(txn->active());
      }

      {
        auto txn = co_await conn->begin();
        REQUIRE(txn.has_value());
        auto ins = co_await conn->execute("INSERT INTO photon_txn(id) VALUES (2)");
        REQUIRE(ins.has_value());
        auto rolled = co_await txn->rollback();
        REQUIRE(rolled.has_value());
      }

      auto after = co_await conn->query_value<std::int64_t>("SELECT count(*) FROM photon_txn");
      REQUIRE(after.has_value());
      REQUIRE(after->has_value());
      CHECK(**after == 1);

      {
        auto poisoned = co_await photon::connection_t::connect(loop, dsn_text);
        REQUIRE(poisoned.has_value());
        {
          auto txn = co_await (*poisoned)->begin();
          REQUIRE(txn.has_value());
          auto ins = co_await (*poisoned)->execute("INSERT INTO photon_txn(id) VALUES (3)");
          REQUIRE(ins.has_value());
        }
        CHECK((*poisoned)->is_broken());
        co_await (*poisoned)->close();
      }

      auto absent = co_await conn->query_value<std::int64_t>("SELECT count(*) FROM photon_txn WHERE id = 3");
      REQUIRE(absent.has_value());
      REQUIRE(absent->has_value());
      CHECK(**absent == 0);

      (void)co_await conn->execute("DROP TABLE photon_txn");
      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("integration: LISTEN/NOTIFY delivers the process id, channel and payload")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  if (dsn == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN is unset; skipping the LISTEN/NOTIFY integration test");
    return;
  }
  std::string dsn_text = dsn;

  int rc = vio::run(
    [&dsn_text](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto listener = co_await photon::connection_t::connect(loop, dsn_text);
      if (!listener.has_value())
      {
        MESSAGE("listener connect failed: " << listener.error().msg);
        co_return 1;
      }
      auto notifier = co_await photon::connection_t::connect(loop, dsn_text);
      if (!notifier.has_value())
      {
        MESSAGE("notifier connect failed: " << notifier.error().msg);
        co_return 1;
      }

      auto listened = co_await (*listener)->listen("photon chan");
      REQUIRE(listened.has_value());

      auto pending = (*listener)->next_notification();
      auto notified = co_await (*notifier)->execute("NOTIFY \"photon chan\", 'photon-payload'");
      REQUIRE(notified.has_value());

      auto note = co_await std::move(pending);
      REQUIRE(note.has_value());
      CHECK(note->channel == "photon chan");
      CHECK(note->payload == "photon-payload");
      CHECK(note->process_id == (*notifier)->backend_process_id());

      co_await (*listener)->close();
      co_await (*notifier)->close();
      co_return 0;
    });
  CHECK(rc == 0);
}
