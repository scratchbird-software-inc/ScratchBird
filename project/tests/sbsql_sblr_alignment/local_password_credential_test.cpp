// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "security/local_password_credential.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace api = scratchbird::engine::internal_api;
namespace {
unsigned checks = 0, calls = 0;
int observed_iterations = 0;
bool fail_provider = false;
constexpr std::string_view verifier =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=4ce03aa5a5657aaf221192635ed9c63acdb76d78a0994ec6e6ab55286e29e6a5";
constexpr std::string_view password = "DBLC013G-fixture-password";
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
void Verify(std::string_view stored, std::string_view candidate,
            api::LocalPasswordCredentialError expected, bool provider_called) {
  std::array<std::byte, 32> scratch;
  scratch.fill(std::byte{0xa5});
  const auto before = calls;
  const auto result = api::VerifyLocalPasswordCredential(stored, candidate, scratch);
  Check(result.error == expected && result.verified() == (expected == api::LocalPasswordCredentialError::none),
        "verification disposition differs from exact expectation");
  Check(calls - before == unsigned(provider_called), "malformed input reached provider or valid input bypassed it");
  Check(std::all_of(scratch.begin(), scratch.end(), [](auto byte) { return byte == std::byte{}; }),
        "derived workspace not erased on terminal outcome");
}
}  // namespace

extern "C" int __real_PKCS5_PBKDF2_HMAC(const char*, int, const unsigned char*,
    int, int, const EVP_MD*, int, unsigned char*);
extern "C" int __wrap_PKCS5_PBKDF2_HMAC(const char* pass, int passlen,
    const unsigned char* salt, int saltlen, int iterations, const EVP_MD* md,
    int keylen, unsigned char* out) {
  ++calls; observed_iterations = iterations;
  Check(passlen > 0 && passlen <= 1024 && saltlen == 16 && keylen == 32 &&
            iterations >= 600000 && iterations <= 10000000 && md,
        "shared verifier changed native PBKDF2 bounds");
  if (fail_provider) { std::fill_n(out, keylen, 0x5a); return 0; }
  return __real_PKCS5_PBKDF2_HMAC(pass, passlen, salt, saltlen, iterations, md, keylen, out);
}

int main() try {
  using E = api::LocalPasswordCredentialError;
  // Frozen PBKDF2 fixture shared with runtime_principal_observation_checks.hpp,
  // independently generated from password+salt. Do not derive the expected
  // verifier with the function under test. Exercise the actual provider.
  Verify(verifier, password, E::none, true);
  Verify(verifier, "wrong-password", E::mismatch, true);
  auto other = std::string(verifier); other.back() = '4';
  Verify(other, password, E::mismatch, true);
  for (std::size_t size = 0; size < verifier.size(); ++size)
    Verify(verifier.substr(0, size), password, E::invalid_verifier, false);
  for (const auto& suffix : std::array<std::string, 5>{"0", ":", "\n", ":verifier=", std::string(1, '\0')})
    Verify(std::string(verifier) + suffix, password, E::invalid_verifier, false);
  for (const std::string iterations : {"", "0", "1", "599999", "0600000", "10000001", "999999999",
                                       "-600000", "+600000", "600000 ", "60000a", "6e5"}) {
    auto bad = std::string(verifier); bad.replace(bad.find("600000"), 6, iterations);
    Verify(bad, password, E::invalid_verifier, false);
  }
  const auto salt = verifier.find(":salt=") + 6;
  const auto digest = verifier.find(":verifier=") + 10;
  for (const auto position : {salt, salt + 31, digest, digest + 63})
    for (const char invalid : {'G', 'A', ' ', ':', '\0', '\n'}) {
      auto bad = std::string(verifier); bad[position] = invalid;
      Verify(bad, password, E::invalid_verifier, false);
    }
  Verify(verifier, {}, E::invalid_credential, false);
  Verify(verifier, std::string(1025, 'x'), E::invalid_credential, false);
  Verify(verifier, std::string_view("x\0y", 3), E::invalid_credential, false);
  fail_provider = true;
  Verify(verifier, password, E::provider_failure, true);
  auto maximum = std::string(verifier); maximum.replace(maximum.find("600000"), 6, "10000000");
  Verify(maximum, std::string(1024, 'x'), E::provider_failure, true);
  Check(observed_iterations == 10000000, "maximum cost was narrowed or rewritten");
  fail_provider = false;
  // Workspace aliasing must refuse WITHOUT erasing immutable credential bytes.
  std::array<std::byte, 96> backing; backing.fill(std::byte{0xa5});
  const auto before = backing;
  const auto text = std::string_view(reinterpret_cast<const char*>(backing.data()), backing.size());
  std::span<std::byte, 32> overlapping(backing.data() + 16, 32);
  const auto call_count = calls;
  Check(api::VerifyLocalPasswordCredential(verifier, text, overlapping).error == E::invalid_workspace,
        "password/derived overlap admitted");
  Check(api::VerifyLocalPasswordCredential(text, password, overlapping).error == E::invalid_workspace,
        "verifier/derived overlap admitted");
  Check(backing == before && calls == call_count, "invalid alias touched input or invoked provider");
  std::array<std::byte, 192> boundaries; boundaries.fill(std::byte{0xa5});
  std::memcpy(boundaries.data() + 64, password.data(), password.size());
  const auto credential = std::string_view(reinterpret_cast<const char*>(boundaries.data() + 64), password.size());
  for (const auto offset : {std::size_t{33}, std::size_t{64}, 64 + password.size() - 1}) {
    const auto unchanged = boundaries;
    const auto prior = calls;
    Check(api::VerifyLocalPasswordCredential(verifier, credential,
              std::span<std::byte, 32>{boundaries.data() + offset, 32}).error == E::invalid_workspace,
          "one-byte or exact workspace/input overlap was admitted");
    Check(boundaries == unchanged && calls == prior, "overlap refusal changed immutable input");
  }
  fail_provider = true;
  for (const auto offset : {std::size_t{32}, 64 + password.size()}) {
    const auto prior = calls;
    const auto result = api::VerifyLocalPasswordCredential(verifier, credential,
        std::span<std::byte, 32>{boundaries.data() + offset, 32});
    Check(result.error == E::provider_failure && calls == prior + 1,
          "exactly adjacent input/workspace was incorrectly classified as overlap");
    Check(credential == password, "admitted adjacent scratch erased credential input");
  }
  fail_provider = false;
  std::cout << "PASS local password credential mechanism " << checks << " checks\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL " << error.what() << " checks=" << checks << '\n';
  return 1;
}
