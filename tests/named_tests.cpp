#include <doctest/doctest.h>

#include <string>

#include <photon/named.h>

using namespace photon;

namespace
{
detail::rewritten_query_t must_rewrite(std::string_view sql, const named_args_t &args)
{
  auto r = detail::rewrite_named_params(sql, args.values());
  REQUIRE(r.has_value());
  return std::move(*r);
}
} // namespace

TEST_CASE("named params substitute positionally in first-seen order")
{
  named_args_t a;
  a.set("id", 5).set("name", std::string("ada"));
  auto r = must_rewrite("SELECT * FROM t WHERE id = :id AND name = :name", a);
  CHECK(r.sql == "SELECT * FROM t WHERE id = $1 AND name = $2");
  CHECK(r.params.size() == 2);
}

TEST_CASE("a repeated named param reuses its position")
{
  named_args_t a;
  a.set("x", 7);
  auto r = must_rewrite("SELECT :x WHERE a = :x OR b = :x", a);
  CHECK(r.sql == "SELECT $1 WHERE a = $1 OR b = $1");
  CHECK(r.params.size() == 1);
}

TEST_CASE("the cast operator :: is left untouched")
{
  named_args_t a;
  a.set("id", 1);
  auto r = must_rewrite("SELECT :id::text, now()::date", a);
  CHECK(r.sql == "SELECT $1::text, now()::date");
  CHECK(r.params.size() == 1);
}

TEST_CASE("colons inside string literals and quoted identifiers are ignored")
{
  named_args_t a;
  a.set("id", 1);
  auto r = must_rewrite("SELECT ':not_a_param', \"col:umn\", :id", a);
  CHECK(r.sql == "SELECT ':not_a_param', \"col:umn\", $1");
  CHECK(r.params.size() == 1);
}

TEST_CASE("a doubled quote inside a string literal does not end it early")
{
  named_args_t a;
  a.set("id", 1);
  auto r = must_rewrite("SELECT 'it''s :here', :id", a);
  CHECK(r.sql == "SELECT 'it''s :here', $1");
  CHECK(r.params.size() == 1);
}

TEST_CASE("dollar-quoted strings are opaque")
{
  named_args_t a;
  a.set("id", 1);
  auto r = must_rewrite("SELECT $$ :nope $$, $tag$ :also_nope $tag$, :id", a);
  CHECK(r.sql == "SELECT $$ :nope $$, $tag$ :also_nope $tag$, $1");
  CHECK(r.params.size() == 1);
}

TEST_CASE("colons inside comments are ignored")
{
  named_args_t a;
  a.set("id", 1);
  auto line = must_rewrite("SELECT :id -- comment :nope\n", a);
  CHECK(line.sql == "SELECT $1 -- comment :nope\n");
  auto block = must_rewrite("SELECT /* :nope /* nested :x */ */ :id", a);
  CHECK(block.sql == "SELECT /* :nope /* nested :x */ */ $1");
  CHECK(block.params.size() == 1);
}

TEST_CASE("a backslash-escaped quote in an E-string does not end the literal early")
{
  named_args_t a;
  a.set("b", 1);
  auto r = must_rewrite("SELECT x FROM t WHERE c = E'a\\':b' AND d = :b", a);
  CHECK(r.sql == "SELECT x FROM t WHERE c = E'a\\':b' AND d = $1");
  CHECK(r.params.size() == 1);
}

TEST_CASE("an array slice with numeric bounds is not mistaken for a param")
{
  named_args_t a;
  a.set("id", 1);
  auto r = must_rewrite("SELECT arr[1:2], :id", a);
  CHECK(r.sql == "SELECT arr[1:2], $1");
  CHECK(r.params.size() == 1);
}

TEST_CASE("a positional placeholder passes through unchanged")
{
  named_args_t a;
  auto r = must_rewrite("SELECT $1, $2", a);
  CHECK(r.sql == "SELECT $1, $2");
  CHECK(r.params.empty());
}

TEST_CASE("a named param with no supplied value is an error")
{
  named_args_t a;
  a.set("id", 1);
  auto r = detail::rewrite_named_params("SELECT :id, :missing", a.values());
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == error_kind_t::protocol);
}
