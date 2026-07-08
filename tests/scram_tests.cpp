#include <doctest/doctest.h>

#include <string>

#include <photon/detail/scram.h>

using photon::detail::scram_client_t;

TEST_CASE("SCRAM-SHA-256 client reproduces the RFC 7677 example exchange")
{
  scram_client_t client("user", "pencil");
  client.set_test_nonce("rOprNGfwEbeRWgbNEkqO");

  auto first = client.client_first_message();
  REQUIRE(first.has_value());
  CHECK(*first == "n,,n=user,r=rOprNGfwEbeRWgbNEkqO");

  std::string server_first = "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
  auto final_message = client.handle_server_first(server_first);
  REQUIRE(final_message.has_value());
  CHECK(*final_message == "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=");

  std::string server_final = "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";
  CHECK(client.handle_server_final(server_final).has_value());
}

TEST_CASE("SCRAM rejects a server nonce that does not extend the client nonce")
{
  scram_client_t client("user", "pencil");
  client.set_test_nonce("rOprNGfwEbeRWgbNEkqO");
  (void)client.client_first_message();
  auto r = client.handle_server_first("r=differentNonce,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096");
  CHECK_FALSE(r.has_value());
}

TEST_CASE("SCRAM rejects a tampered server signature")
{
  scram_client_t client("user", "pencil");
  client.set_test_nonce("rOprNGfwEbeRWgbNEkqO");
  (void)client.client_first_message();
  (void)client.handle_server_first("r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096");
  auto r = client.handle_server_final("v=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
  CHECK_FALSE(r.has_value());
}

TEST_CASE("SCRAM surfaces a server-final error attribute")
{
  scram_client_t client("user", "pencil");
  client.set_test_nonce("rOprNGfwEbeRWgbNEkqO");
  (void)client.client_first_message();
  (void)client.handle_server_first("r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096");
  auto r = client.handle_server_final("e=invalid-proof");
  CHECK_FALSE(r.has_value());
}
