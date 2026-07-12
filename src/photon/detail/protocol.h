#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "photon/error.h"

namespace photon::detail
{
inline constexpr std::int32_t protocol_version_3 = 196608;
inline constexpr std::int32_t ssl_request_code = 80877103;
inline constexpr std::int32_t cancel_request_code = 80877102;

inline constexpr std::int16_t format_text = 0;
inline constexpr std::int16_t format_binary = 1;

enum class backend_tag_t : std::uint8_t
{
  authentication = 'R',
  parameter_status = 'S',
  backend_key_data = 'K',
  ready_for_query = 'Z',
  row_description = 'T',
  data_row = 'D',
  command_complete = 'C',
  empty_query_response = 'I',
  error_response = 'E',
  notice_response = 'N',
  parse_complete = '1',
  bind_complete = '2',
  close_complete = '3',
  no_data = 'n',
  parameter_description = 't',
  portal_suspended = 's',
  notification_response = 'A',
  copy_in_response = 'G',
  copy_out_response = 'H',
  copy_both_response = 'W',
  copy_data = 'd',
  copy_done = 'c',
};

enum class auth_request_t : std::int32_t
{
  ok = 0,
  kerberos_v5 = 2,
  cleartext_password = 3,
  md5_password = 5,
  gss = 7,
  sasl = 10,
  sasl_continue = 11,
  sasl_final = 12,
};

class wire_writer_t
{
public:
  explicit wire_writer_t(std::uint8_t type = 0)
    : _type(type)
  {
  }

  wire_writer_t &u8(std::uint8_t value)
  {
    _body.push_back(value);
    return *this;
  }

  wire_writer_t &i16(std::int16_t value)
  {
    auto v = static_cast<std::uint16_t>(value);
    _body.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    _body.push_back(static_cast<std::uint8_t>(v & 0xFF));
    return *this;
  }

  wire_writer_t &i32(std::int32_t value)
  {
    auto v = static_cast<std::uint32_t>(value);
    _body.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    _body.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    _body.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    _body.push_back(static_cast<std::uint8_t>(v & 0xFF));
    return *this;
  }

  wire_writer_t &cstr(std::string_view value)
  {
    _body.insert(_body.end(), value.begin(), value.end());
    _body.push_back(0);
    return *this;
  }

  wire_writer_t &raw(std::string_view value)
  {
    _body.insert(_body.end(), value.begin(), value.end());
    return *this;
  }

  wire_writer_t &bytes(std::span<const std::uint8_t> value)
  {
    _body.insert(_body.end(), value.begin(), value.end());
    return *this;
  }

  std::vector<std::uint8_t> finish() const
  {
    std::vector<std::uint8_t> out;
    std::size_t length = _body.size() + 4;
    if (_type != 0)
    {
      out.reserve(length + 1);
      out.push_back(_type);
    }
    else
    {
      out.reserve(length);
    }
    auto len = static_cast<std::uint32_t>(length);
    out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));
    out.insert(out.end(), _body.begin(), _body.end());
    return out;
  }

private:
  std::uint8_t _type;
  std::vector<std::uint8_t> _body;
};

class wire_reader_t
{
public:
  explicit wire_reader_t(std::span<const std::uint8_t> data)
    : _data(data)
  {
  }

  [[nodiscard]] bool empty() const
  {
    return _pos >= _data.size();
  }

  [[nodiscard]] std::size_t remaining() const
  {
    return _data.size() - _pos;
  }

  result_t<std::uint8_t> u8()
  {
    if (remaining() < 1)
    {
      return fail(error_kind_t::protocol, "wire underrun reading u8");
    }
    return _data[_pos++];
  }

  result_t<std::int16_t> i16()
  {
    if (remaining() < 2)
    {
      return fail(error_kind_t::protocol, "wire underrun reading i16");
    }
    std::uint16_t v = static_cast<std::uint16_t>(_data[_pos]) << 8 | static_cast<std::uint16_t>(_data[_pos + 1]);
    _pos += 2;
    return static_cast<std::int16_t>(v);
  }

  result_t<std::int32_t> i32()
  {
    if (remaining() < 4)
    {
      return fail(error_kind_t::protocol, "wire underrun reading i32");
    }
    std::uint32_t v = static_cast<std::uint32_t>(_data[_pos]) << 24 | static_cast<std::uint32_t>(_data[_pos + 1]) << 16 | static_cast<std::uint32_t>(_data[_pos + 2]) << 8 | static_cast<std::uint32_t>(_data[_pos + 3]);
    _pos += 4;
    return static_cast<std::int32_t>(v);
  }

  result_t<std::string_view> cstr()
  {
    for (std::size_t i = _pos; i < _data.size(); ++i)
    {
      if (_data[i] == 0)
      {
        std::string_view out(reinterpret_cast<const char *>(_data.data() + _pos), i - _pos);
        _pos = i + 1;
        return out;
      }
    }
    return fail(error_kind_t::protocol, "wire underrun reading cstring (no terminator)");
  }

  result_t<std::span<const std::uint8_t>> bytes(std::size_t n)
  {
    if (remaining() < n)
    {
      return fail(error_kind_t::protocol, "wire underrun reading bytes");
    }
    std::span<const std::uint8_t> out = _data.subspan(_pos, n);
    _pos += n;
    return out;
  }

private:
  std::span<const std::uint8_t> _data;
  std::size_t _pos = 0;
};

inline std::span<const std::uint8_t> as_bytes(std::string_view s)
{
  return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}
} // namespace photon::detail
