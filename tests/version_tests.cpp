#include <doctest/doctest.h>

#include <string_view>

#include <photon/error.h>
#include <photon/photon.h>

TEST_CASE("version is reported")
{
  std::string_view v = photon::version();
  CHECK_FALSE(v.empty());
}

TEST_CASE("fail builds an unexpected error carrying kind and message")
{
  photon::result_t<int> r = photon::fail(photon::error_kind_t::connection, "no route to host");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == photon::error_kind_t::connection);
  CHECK(r.error().msg == "no route to host");
  CHECK(r.error().sqlstate.empty());
}

TEST_CASE("fail_server records the sqlstate")
{
  photon::result_t<int> r = photon::fail_server("28P01", "password authentication failed");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == photon::error_kind_t::server);
  CHECK(r.error().sqlstate == "28P01");
}
