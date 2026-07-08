#pragma once

#include <string>
#include <string_view>

#include <vio/crypto.h>

#include "photon/error.h"

namespace photon::detail
{
class scram_client_t
{
public:
  scram_client_t(std::string authcid, std::string password);

  void set_test_nonce(std::string nonce);

  result_t<std::string> client_first_message();
  result_t<std::string> handle_server_first(std::string_view server_first);
  result_t<void> handle_server_final(std::string_view server_final);

private:
  std::string _authcid;
  std::string _password;
  std::string _client_nonce;
  std::string _client_first_bare;
  vio::crypto::sha256_digest_t _server_signature{};
  bool _have_server_signature = false;
};
} // namespace photon::detail
