#include <doctest/doctest.h>
#include <limits>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <structify/structify.h>

#include <photon/detail/message.h>
#include <photon/detail/protocol.h>
#include <photon/result.h>
#include <photon/row_binding.h>

using namespace photon;
using namespace photon::detail;

namespace
{
struct user_t
{
  std::int32_t id;
  std::string name;
  std::optional<std::int32_t> age;
  STFY_OBJ(id, name, age);
};

row_description_t describe(std::initializer_list<std::pair<const char *, std::int32_t>> cols)
{
  row_description_t d;
  for (auto [name, oid] : cols)
  {
    field_description_t f;
    f.name = name;
    f.type_oid = oid;
    f.format = format_binary;
    d.fields.push_back(f);
  }
  return d;
}

std::vector<std::uint8_t> data_row_body(std::initializer_list<std::optional<std::vector<std::uint8_t>>> cols)
{
  wire_writer_t w('D');
  w.i16(static_cast<std::int16_t>(cols.size()));
  for (const auto &c : cols)
  {
    if (!c.has_value())
    {
      w.i32(-1);
    }
    else
    {
      w.i32(static_cast<std::int32_t>(c->size()));
      w.bytes(*c);
    }
  }
  auto framed = w.finish();
  return {framed.begin() + 5, framed.end()};
}

std::vector<std::uint8_t> i32be(std::int32_t v)
{
  auto u = static_cast<std::uint32_t>(v);
  return {static_cast<std::uint8_t>(u >> 24), static_cast<std::uint8_t>(u >> 16), static_cast<std::uint8_t>(u >> 8), static_cast<std::uint8_t>(u)};
}

std::vector<std::uint8_t> text(std::string_view s)
{
  return {s.begin(), s.end()};
}
} // namespace

TEST_CASE("build_column_map matches struct fields to result columns by name")
{
  auto desc = describe({{"name", 25}, {"id", 23}, {"age", 23}});
  auto map = build_column_map<user_t>(desc);
  REQUIRE(map.has_value());
  CHECK(map->field_to_column[0] == 1);
  CHECK(map->field_to_column[1] == 0);
  CHECK(map->field_to_column[2] == 2);
}

TEST_CASE("build_column_map fails when a field has no column")
{
  auto desc = describe({{"id", 23}, {"name", 25}});
  auto map = build_column_map<user_t>(desc);
  CHECK_FALSE(map.has_value());
}

TEST_CASE("result_set materialises a typed row, NULL -> empty optional")
{
  auto desc = describe({{"id", 23}, {"name", 25}, {"age", 23}});
  auto map = build_column_map<user_t>(desc);
  REQUIRE(map.has_value());

  std::vector<std::vector<std::uint8_t>> rows;
  rows.push_back(data_row_body({i32be(7), text("ada"), i32be(36)}));
  rows.push_back(data_row_body({i32be(8), text("bob"), std::nullopt}));

  result_set_t<user_t> set(desc, std::move(rows), *map);
  CHECK(set.size() == 2);

  auto r0 = set.at(0);
  REQUIRE(r0.has_value());
  CHECK(r0->id == 7);
  CHECK(r0->name == "ada");
  REQUIRE(r0->age.has_value());
  CHECK(*r0->age == 36);

  auto r1 = set.at(1);
  REQUIRE(r1.has_value());
  CHECK(r1->id == 8);
  CHECK(r1->name == "bob");
  CHECK_FALSE(r1->age.has_value());
}

TEST_CASE("result_set collect and one; structured bindings work on a row")
{
  auto desc = describe({{"id", 23}, {"name", 25}, {"age", 23}});
  auto map = build_column_map<user_t>(desc);
  REQUIRE(map.has_value());
  std::vector<std::vector<std::uint8_t>> rows;
  rows.push_back(data_row_body({i32be(1), text("x"), i32be(20)}));

  result_set_t<user_t> set(desc, std::move(rows), *map);
  auto all = set.collect();
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 1);
  auto &[id, name, age] = (*all)[0];
  CHECK(id == 1);
  CHECK(name == "x");
  CHECK(age == 20);

  auto only = set.one();
  REQUIRE(only.has_value());
  REQUIRE(only->has_value());
  CHECK((*only)->id == 1);
}

TEST_CASE("make_command_result parses the affected-row count from the tag")
{
  CHECK(make_command_result("INSERT 0 5").rows_affected == 5);
  CHECK(make_command_result("UPDATE 2").rows_affected == 2);
  CHECK(make_command_result("DELETE 1").rows_affected == 1);
  CHECK(make_command_result("SELECT 3").rows_affected == 3);
  CHECK(make_command_result("CREATE TABLE").rows_affected == 0);
}

TEST_CASE("make_command_result saturates instead of overflowing on a hostile tag")
{
  std::string huge = "INSERT 0 ";
  huge.append(40, '9');
  auto r = make_command_result(huge);
  CHECK(r.rows_affected == std::numeric_limits<std::int64_t>::max());

  CHECK(make_command_result("SELECT 12abc").rows_affected == 0);
  CHECK(make_command_result("MOVE 9223372036854775807").rows_affected == 9223372036854775807LL);
}
