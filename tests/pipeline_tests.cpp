#include <doctest/doctest.h>

#include <cstdint>
#include <type_traits>
#include <vector>

#include <structify/structify.h>

#include <photon/detail/message.h>
#include <photon/pipeline.h>

using namespace photon;

namespace
{
struct id_row_t
{
  std::int32_t id;
  STFY_OBJ(id);
};

std::vector<char> message_tags(const std::vector<std::uint8_t> &buffer)
{
  std::vector<char> tags;
  std::size_t pos = 0;
  while (pos + 5 <= buffer.size())
  {
    char type = static_cast<char>(buffer[pos]);
    std::uint32_t length = static_cast<std::uint32_t>(buffer[pos + 1]) << 24 | static_cast<std::uint32_t>(buffer[pos + 2]) << 16 | static_cast<std::uint32_t>(buffer[pos + 3]) << 8 | static_cast<std::uint32_t>(buffer[pos + 4]);
    tags.push_back(type);
    pos += 1 + length;
  }
  return tags;
}
} // namespace

TEST_CASE("append_extended_query emits Parse/Bind/Describe/Execute and an optional Sync")
{
  std::vector<detail::encoded_param_t> no_params;

  std::vector<std::uint8_t> with_sync;
  detail::append_extended_query(with_sync, "", "SELECT 1", no_params, true);
  CHECK(message_tags(with_sync) == std::vector<char>{'P', 'B', 'D', 'E', 'S'});

  std::vector<std::uint8_t> without_sync;
  detail::append_extended_query(without_sync, "", "SELECT 1", no_params, false);
  CHECK(message_tags(without_sync) == std::vector<char>{'P', 'B', 'D', 'E'});
}

TEST_CASE("append_extended_query skips Parse for a named prepared statement")
{
  std::vector<detail::encoded_param_t> no_params;
  std::vector<std::uint8_t> out;
  detail::append_extended_query(out, "photon_stmt_1", "", no_params, true);
  CHECK(message_tags(out) == std::vector<char>{'B', 'D', 'E', 'S'});
}

TEST_CASE("append_extended_query concatenates one cycle per step (independent batch)")
{
  std::vector<detail::encoded_param_t> no_params;
  std::vector<std::uint8_t> out;
  detail::append_extended_query(out, "", "SELECT 1", no_params, true);
  detail::append_extended_query(out, "", "SELECT 2", no_params, true);
  detail::append_extended_query(out, "", "SELECT 3", no_params, true);
  auto tags = message_tags(out);
  int syncs = 0;
  for (char t : tags)
  {
    if (t == 'S')
    {
      ++syncs;
    }
  }
  CHECK(tags.size() == 15);
  CHECK(syncs == 3);
}

TEST_CASE("a pipe_slot yields a not-run error before run()")
{
  pipe_slot_t<command_result_t> slot;
  auto result = slot.get();
  CHECK_FALSE(result.has_value());
  CHECK(result.error().kind == error_kind_t::protocol);
}

TEST_CASE("step factories are detected and expose the right result types")
{
  static_assert(is_pipe_step_v<decltype(pquery<id_row_t>("SELECT id FROM t"))>);
  static_assert(is_pipe_step_v<decltype(pexecute("UPDATE t SET x = 1"))>);
  static_assert(!is_pipe_step_v<int>);
  static_assert(std::is_same_v<step_result_t<decltype(pquery<id_row_t>("SELECT id FROM t"))>, result_t<result_set_t<id_row_t>>>);
  static_assert(std::is_same_v<step_result_t<decltype(pexecute("UPDATE t SET x = 1"))>, result_t<command_result_t>>);
  CHECK(true);
}
