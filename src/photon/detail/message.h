#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photon/detail/protocol.h"
#include "photon/error.h"

namespace photon::detail
{
struct startup_param_t
{
  std::string key;
  std::string value;
};

std::vector<std::uint8_t> startup_message(std::string_view user, std::string_view database, std::span<const startup_param_t> extra = {});
std::vector<std::uint8_t> ssl_request_message();
std::vector<std::uint8_t> password_message(std::string_view password);
std::vector<std::uint8_t> sasl_initial_response_message(std::string_view mechanism, std::string_view initial_response);
std::vector<std::uint8_t> sasl_response_message(std::string_view response);
std::vector<std::uint8_t> parse_message(std::string_view statement, std::string_view query, std::span<const std::int32_t> param_type_oids = {});

using encoded_param_t = std::optional<std::vector<std::uint8_t>>;

std::vector<std::uint8_t> bind_message(std::string_view portal, std::string_view statement, std::span<const std::int16_t> param_formats, std::span<const encoded_param_t> params, std::span<const std::int16_t> result_formats);
std::vector<std::uint8_t> describe_message(char kind, std::string_view name);
std::vector<std::uint8_t> execute_message(std::string_view portal, std::int32_t max_rows = 0);
std::vector<std::uint8_t> sync_message();
std::vector<std::uint8_t> query_message(std::string_view sql);
std::vector<std::uint8_t> terminate_message();

void append_extended_query(std::vector<std::uint8_t> &out, std::string_view statement, std::string_view sql, std::span<const encoded_param_t> params, bool with_sync);

struct auth_message_t
{
  auth_request_t type = auth_request_t::ok;
  std::vector<std::uint8_t> data;
};

struct parameter_status_t
{
  std::string name;
  std::string value;
};

struct backend_key_data_t
{
  std::int32_t process_id = 0;
  std::int32_t secret_key = 0;
};

struct ready_for_query_t
{
  char transaction_status = 'I';
};

struct field_description_t
{
  std::string name;
  std::int32_t table_oid = 0;
  std::int16_t column_id = 0;
  std::int32_t type_oid = 0;
  std::int16_t type_size = 0;
  std::int32_t type_modifier = 0;
  std::int16_t format = format_text;
};

struct row_description_t
{
  std::vector<field_description_t> fields;
};

struct query_data_t
{
  row_description_t description;
  std::vector<std::vector<std::uint8_t>> rows;
  std::string command_tag;
  bool has_description = false;
};

struct data_row_t
{
  std::vector<std::optional<std::span<const std::uint8_t>>> columns;
};

struct command_complete_t
{
  std::string tag;
};

struct notification_response_t
{
  std::int32_t process_id = 0;
  std::string channel;
  std::string payload;
};

struct error_field_t
{
  char code = 0;
  std::string value;
};

struct error_response_t
{
  std::vector<error_field_t> fields;

  [[nodiscard]] std::string_view field(char code) const;
  [[nodiscard]] std::string_view severity() const
  {
    return field('S');
  }
  [[nodiscard]] std::string_view sqlstate() const
  {
    return field('C');
  }
  [[nodiscard]] std::string_view message() const
  {
    return field('M');
  }
};

server_error_t to_server_error(const error_response_t &response);

result_t<auth_message_t> parse_authentication(std::span<const std::uint8_t> body);
result_t<parameter_status_t> parse_parameter_status(std::span<const std::uint8_t> body);
result_t<backend_key_data_t> parse_backend_key_data(std::span<const std::uint8_t> body);
result_t<ready_for_query_t> parse_ready_for_query(std::span<const std::uint8_t> body);
result_t<row_description_t> parse_row_description(std::span<const std::uint8_t> body);
result_t<data_row_t> parse_data_row(std::span<const std::uint8_t> body);
result_t<command_complete_t> parse_command_complete(std::span<const std::uint8_t> body);
result_t<error_response_t> parse_error_response(std::span<const std::uint8_t> body);
result_t<notification_response_t> parse_notification(std::span<const std::uint8_t> body);
} // namespace photon::detail
