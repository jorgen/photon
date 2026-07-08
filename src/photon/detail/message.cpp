#include "photon/detail/message.h"

namespace photon::detail
{
std::vector<std::uint8_t> startup_message(std::string_view user, std::string_view database, std::span<const startup_param_t> extra)
{
  wire_writer_t w;
  w.i32(protocol_version_3);
  w.cstr("user").cstr(user);
  if (!database.empty())
  {
    w.cstr("database").cstr(database);
  }
  for (const auto &param : extra)
  {
    w.cstr(param.key).cstr(param.value);
  }
  w.u8(0);
  return w.finish();
}

std::vector<std::uint8_t> ssl_request_message()
{
  wire_writer_t w;
  w.i32(ssl_request_code);
  return w.finish();
}

std::vector<std::uint8_t> password_message(std::string_view password)
{
  wire_writer_t w('p');
  w.cstr(password);
  return w.finish();
}

std::vector<std::uint8_t> sasl_initial_response_message(std::string_view mechanism, std::string_view initial_response)
{
  wire_writer_t w('p');
  w.cstr(mechanism);
  w.i32(static_cast<std::int32_t>(initial_response.size()));
  w.raw(initial_response);
  return w.finish();
}

std::vector<std::uint8_t> sasl_response_message(std::string_view response)
{
  wire_writer_t w('p');
  w.raw(response);
  return w.finish();
}

std::vector<std::uint8_t> parse_message(std::string_view statement, std::string_view query, std::span<const std::int32_t> param_type_oids)
{
  wire_writer_t w('P');
  w.cstr(statement);
  w.cstr(query);
  w.i16(static_cast<std::int16_t>(param_type_oids.size()));
  for (auto oid : param_type_oids)
  {
    w.i32(oid);
  }
  return w.finish();
}

std::vector<std::uint8_t> bind_message(std::string_view portal, std::string_view statement, std::span<const std::int16_t> param_formats, std::span<const encoded_param_t> params, std::span<const std::int16_t> result_formats)
{
  wire_writer_t w('B');
  w.cstr(portal);
  w.cstr(statement);
  w.i16(static_cast<std::int16_t>(param_formats.size()));
  for (auto format : param_formats)
  {
    w.i16(format);
  }
  w.i16(static_cast<std::int16_t>(params.size()));
  for (const auto &param : params)
  {
    if (!param.has_value())
    {
      w.i32(-1);
    }
    else
    {
      w.i32(static_cast<std::int32_t>(param->size()));
      w.bytes(*param);
    }
  }
  w.i16(static_cast<std::int16_t>(result_formats.size()));
  for (auto format : result_formats)
  {
    w.i16(format);
  }
  return w.finish();
}

std::vector<std::uint8_t> describe_message(char kind, std::string_view name)
{
  wire_writer_t w('D');
  w.u8(static_cast<std::uint8_t>(kind));
  w.cstr(name);
  return w.finish();
}

std::vector<std::uint8_t> execute_message(std::string_view portal, std::int32_t max_rows)
{
  wire_writer_t w('E');
  w.cstr(portal);
  w.i32(max_rows);
  return w.finish();
}

std::vector<std::uint8_t> sync_message()
{
  wire_writer_t w('S');
  return w.finish();
}

std::vector<std::uint8_t> query_message(std::string_view sql)
{
  wire_writer_t w('Q');
  w.cstr(sql);
  return w.finish();
}

std::vector<std::uint8_t> terminate_message()
{
  wire_writer_t w('X');
  return w.finish();
}

std::string_view error_response_t::field(char code) const
{
  for (const auto &f : fields)
  {
    if (f.code == code)
    {
      return f.value;
    }
  }
  return {};
}

result_t<auth_message_t> parse_authentication(std::span<const std::uint8_t> body)
{
  wire_reader_t r(body);
  auto subtype = r.i32();
  if (!subtype.has_value())
  {
    return std::unexpected(subtype.error());
  }
  auth_message_t out;
  out.type = static_cast<auth_request_t>(*subtype);
  out.data.assign(body.begin() + 4, body.end());
  return out;
}

result_t<parameter_status_t> parse_parameter_status(std::span<const std::uint8_t> body)
{
  wire_reader_t r(body);
  auto name = r.cstr();
  if (!name.has_value())
  {
    return std::unexpected(name.error());
  }
  auto value = r.cstr();
  if (!value.has_value())
  {
    return std::unexpected(value.error());
  }
  return parameter_status_t{std::string(*name), std::string(*value)};
}

result_t<backend_key_data_t> parse_backend_key_data(std::span<const std::uint8_t> body)
{
  wire_reader_t r(body);
  auto pid = r.i32();
  if (!pid.has_value())
  {
    return std::unexpected(pid.error());
  }
  auto key = r.i32();
  if (!key.has_value())
  {
    return std::unexpected(key.error());
  }
  return backend_key_data_t{*pid, *key};
}

result_t<ready_for_query_t> parse_ready_for_query(std::span<const std::uint8_t> body)
{
  wire_reader_t r(body);
  auto status = r.u8();
  if (!status.has_value())
  {
    return std::unexpected(status.error());
  }
  return ready_for_query_t{static_cast<char>(*status)};
}

result_t<row_description_t> parse_row_description(std::span<const std::uint8_t> body)
{
  wire_reader_t r(body);
  auto count = r.i16();
  if (!count.has_value())
  {
    return std::unexpected(count.error());
  }
  row_description_t out;
  out.fields.reserve(static_cast<std::size_t>(*count));
  for (std::int16_t i = 0; i < *count; ++i)
  {
    field_description_t field;
    auto name = r.cstr();
    if (!name.has_value())
    {
      return std::unexpected(name.error());
    }
    field.name = std::string(*name);
    auto table_oid = r.i32();
    auto column_id = r.i16();
    auto type_oid = r.i32();
    auto type_size = r.i16();
    auto type_modifier = r.i32();
    auto format = r.i16();
    if (!table_oid.has_value() || !column_id.has_value() || !type_oid.has_value() || !type_size.has_value() || !type_modifier.has_value() || !format.has_value())
    {
      return fail(error_kind_t::protocol, "truncated RowDescription field");
    }
    field.table_oid = *table_oid;
    field.column_id = *column_id;
    field.type_oid = *type_oid;
    field.type_size = *type_size;
    field.type_modifier = *type_modifier;
    field.format = *format;
    out.fields.push_back(std::move(field));
  }
  return out;
}

result_t<data_row_t> parse_data_row(std::span<const std::uint8_t> body)
{
  wire_reader_t r(body);
  auto count = r.i16();
  if (!count.has_value())
  {
    return std::unexpected(count.error());
  }
  data_row_t out;
  out.columns.reserve(static_cast<std::size_t>(*count));
  for (std::int16_t i = 0; i < *count; ++i)
  {
    auto length = r.i32();
    if (!length.has_value())
    {
      return std::unexpected(length.error());
    }
    if (*length < 0)
    {
      out.columns.emplace_back(std::nullopt);
      continue;
    }
    auto value = r.bytes(static_cast<std::size_t>(*length));
    if (!value.has_value())
    {
      return std::unexpected(value.error());
    }
    out.columns.emplace_back(*value);
  }
  return out;
}

result_t<command_complete_t> parse_command_complete(std::span<const std::uint8_t> body)
{
  wire_reader_t r(body);
  auto tag = r.cstr();
  if (!tag.has_value())
  {
    return std::unexpected(tag.error());
  }
  return command_complete_t{std::string(*tag)};
}

result_t<error_response_t> parse_error_response(std::span<const std::uint8_t> body)
{
  wire_reader_t r(body);
  error_response_t out;
  for (;;)
  {
    auto code = r.u8();
    if (!code.has_value())
    {
      return std::unexpected(code.error());
    }
    if (*code == 0)
    {
      break;
    }
    auto value = r.cstr();
    if (!value.has_value())
    {
      return std::unexpected(value.error());
    }
    out.fields.push_back(error_field_t{static_cast<char>(*code), std::string(*value)});
  }
  return out;
}
} // namespace photon::detail
