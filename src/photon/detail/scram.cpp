#include "photon/detail/scram.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace photon::detail
{
namespace
{
std::span<const std::uint8_t> bytes_of(std::string_view s)
{
  return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}

std::span<const std::uint8_t> bytes_of(const vio::crypto::sha256_digest_t &d)
{
  return {d.data(), d.size()};
}

std::optional<std::string_view> attribute(std::string_view message, char key)
{
  std::size_t pos = 0;
  while (pos < message.size())
  {
    std::size_t comma = message.find(',', pos);
    std::string_view part = message.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
    if (part.size() >= 2 && part[0] == key && part[1] == '=')
    {
      return part.substr(2);
    }
    if (comma == std::string_view::npos)
    {
      break;
    }
    pos = comma + 1;
  }
  return std::nullopt;
}
} // namespace

scram_client_t::scram_client_t(std::string authcid, std::string password)
  : _authcid(std::move(authcid))
  , _password(std::move(password))
{
}

void scram_client_t::set_test_nonce(std::string nonce)
{
  _client_nonce = std::move(nonce);
}

result_t<std::string> scram_client_t::client_first_message()
{
  if (_client_nonce.empty())
  {
    std::array<std::uint8_t, 18> raw{};
    auto ok = vio::crypto::random_bytes(raw);
    if (!ok.has_value())
    {
      return fail(error_kind_t::auth, "failed to generate SCRAM client nonce: " + ok.error().msg);
    }
    _client_nonce = vio::crypto::base64_encode(raw);
  }
  _client_first_bare = "n=" + _authcid + ",r=" + _client_nonce;
  return "n,," + _client_first_bare;
}

result_t<std::string> scram_client_t::handle_server_first(std::string_view server_first)
{
  auto combined_nonce = attribute(server_first, 'r');
  auto salt_b64 = attribute(server_first, 's');
  auto iterations_text = attribute(server_first, 'i');
  if (!combined_nonce.has_value() || !salt_b64.has_value() || !iterations_text.has_value())
  {
    return fail(error_kind_t::auth, "malformed SCRAM server-first message");
  }
  if (!combined_nonce->starts_with(_client_nonce))
  {
    return fail(error_kind_t::auth, "SCRAM server nonce does not extend the client nonce");
  }

  std::uint32_t iterations = 0;
  for (char c : *iterations_text)
  {
    if (c < '0' || c > '9')
    {
      return fail(error_kind_t::auth, "SCRAM iteration count is not numeric");
    }
    iterations = iterations * 10 + static_cast<std::uint32_t>(c - '0');
  }
  if (iterations == 0)
  {
    return fail(error_kind_t::auth, "SCRAM iteration count must be positive");
  }

  auto salt = vio::crypto::base64_decode(*salt_b64);
  if (!salt.has_value())
  {
    return fail(error_kind_t::auth, "SCRAM salt is not valid base64");
  }

  auto salted_password = vio::crypto::pbkdf2_hmac_sha256(bytes_of(_password), *salt, iterations);
  auto client_key = vio::crypto::hmac_sha256(bytes_of(salted_password), bytes_of("Client Key"));
  auto stored_key = vio::crypto::sha256(bytes_of(client_key));

  std::string client_final_without_proof = "c=biws,r=" + std::string(*combined_nonce);
  std::string auth_message = _client_first_bare + "," + std::string(server_first) + "," + client_final_without_proof;

  auto client_signature = vio::crypto::hmac_sha256(bytes_of(stored_key), bytes_of(auth_message));

  std::array<std::uint8_t, vio::crypto::sha256_digest_size> client_proof{};
  for (std::size_t i = 0; i < client_proof.size(); ++i)
  {
    client_proof[i] = static_cast<std::uint8_t>(client_key[i] ^ client_signature[i]);
  }

  auto server_key = vio::crypto::hmac_sha256(bytes_of(salted_password), bytes_of("Server Key"));
  _server_signature = vio::crypto::hmac_sha256(bytes_of(server_key), bytes_of(auth_message));
  _have_server_signature = true;

  return client_final_without_proof + ",p=" + vio::crypto::base64_encode(client_proof);
}

result_t<void> scram_client_t::handle_server_final(std::string_view server_final)
{
  if (!_have_server_signature)
  {
    return fail(error_kind_t::auth, "SCRAM server-final received before server-first");
  }
  auto error = attribute(server_final, 'e');
  if (error.has_value())
  {
    return fail(error_kind_t::auth, "SCRAM authentication failed: " + std::string(*error));
  }
  auto verifier_b64 = attribute(server_final, 'v');
  if (!verifier_b64.has_value())
  {
    return fail(error_kind_t::auth, "malformed SCRAM server-final message");
  }
  auto verifier = vio::crypto::base64_decode(*verifier_b64);
  if (!verifier.has_value())
  {
    return fail(error_kind_t::auth, "SCRAM server signature is not valid base64");
  }
  if (verifier->size() != _server_signature.size())
  {
    return fail(error_kind_t::auth, "SCRAM server signature has unexpected length");
  }
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < _server_signature.size(); ++i)
  {
    diff |= static_cast<std::uint8_t>((*verifier)[i] ^ _server_signature[i]);
  }
  if (diff != 0)
  {
    return fail(error_kind_t::auth, "SCRAM server signature verification failed");
  }
  return {};
}
} // namespace photon::detail
