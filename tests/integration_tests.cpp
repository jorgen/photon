#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

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

struct ssl_status_t
{
  bool ssl;
  STFY_OBJ(ssl);
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
      CHECK(bad.error().sqlstate == "42P01");

      auto reuse = co_await conn->execute("DROP TABLE photon_users");
      CHECK(reuse.has_value());

      co_await conn->close();
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("integration: sslmode=require encrypts the connection")
{
  const char *dsn = std::getenv("PHOTON_PG_TEST_DSN");
  if (dsn == nullptr)
  {
    MESSAGE("PHOTON_PG_TEST_DSN is unset; skipping the TLS integration test");
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
