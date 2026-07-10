#include <doctest/doctest.h>

#include <chrono>

#include <photon/connect_params.h>

using namespace photon;

TEST_CASE("parse_dsn extracts user, password, host, port and database")
{
  auto p = parse_dsn("postgresql://photon:secret@127.0.0.1:55432/shop");
  REQUIRE(p.has_value());
  CHECK(p->user == "photon");
  CHECK(p->password == "secret");
  CHECK(p->host == "127.0.0.1");
  CHECK(p->port == 55432);
  CHECK(p->database == "shop");
}

TEST_CASE("parse_dsn accepts the postgres:// scheme and defaults the port")
{
  auto p = parse_dsn("postgres://alice@db.example.com/inventory");
  REQUIRE(p.has_value());
  CHECK(p->user == "alice");
  CHECK(p->password.empty());
  CHECK(p->host == "db.example.com");
  CHECK(p->port == 5432);
  CHECK(p->database == "inventory");
}

TEST_CASE("parse_dsn percent-decodes userinfo")
{
  auto p = parse_dsn("postgresql://a%40b:p%3Aw@host/db");
  REQUIRE(p.has_value());
  CHECK(p->user == "a@b");
  CHECK(p->password == "p:w");
}

TEST_CASE("parse_dsn reads query parameters")
{
  auto p = parse_dsn("postgresql://u@h/d?sslmode=require&connect_timeout=3&application_name=photon");
  REQUIRE(p.has_value());
  CHECK(p->sslmode == sslmode_t::require);
  CHECK(p->connect_timeout == std::chrono::seconds{3});
  REQUIRE(p->options.size() == 1);
  CHECK(p->options[0].first == "application_name");
  CHECK(p->options[0].second == "photon");
}

TEST_CASE("parse_dsn handles a bracketed IPv6 host with a port")
{
  auto p = parse_dsn("postgresql://u@[::1]:5433/d");
  REQUIRE(p.has_value());
  CHECK(p->host == "::1");
  CHECK(p->port == 5433);
}

TEST_CASE("parse_dsn handles a bracketed IPv6 host without a port")
{
  auto p = parse_dsn("postgresql://u@[2001:db8::1]/d");
  REQUIRE(p.has_value());
  CHECK(p->host == "2001:db8::1");
  CHECK(p->port == 5432);
}

TEST_CASE("parse_dsn defaults the database to the user when the path is empty")
{
  auto p = parse_dsn("postgresql://photon@host/");
  REQUIRE(p.has_value());
  CHECK(p->database == "photon");
}

TEST_CASE("parse_dsn rejects a missing scheme and a missing user")
{
  CHECK_FALSE(parse_dsn("mysql://u@h/d").has_value());
  CHECK_FALSE(parse_dsn("postgresql://host/db").has_value());
}

TEST_CASE("parse_dsn rejects a malformed port and sslmode")
{
  CHECK_FALSE(parse_dsn("postgresql://u@h:notaport/d").has_value());
  CHECK_FALSE(parse_dsn("postgresql://u@h/d?sslmode=bogus").has_value());
}
