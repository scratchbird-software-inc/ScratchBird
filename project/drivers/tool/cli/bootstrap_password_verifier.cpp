// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "bootstrap_password_verifier.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstddef>

namespace scratchbird::cli {
namespace {

std::string HexEncode(const unsigned char* bytes, std::size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    output.push_back(kHex[(bytes[index] >> 4) & 0x0f]);
    output.push_back(kHex[bytes[index] & 0x0f]);
  }
  return output;
}

}  // namespace

bool BootstrapPasswordSecretValid(std::string_view password) {
  return password.size() >= 12 && password.size() <= 1024 &&
         password.find('\0') == std::string_view::npos;
}

bool DeriveBootstrapPasswordVerifier(const std::string& password,
                                     std::string* envelope) {
  if (envelope == nullptr || !BootstrapPasswordSecretValid(password)) {
    return false;
  }
  unsigned char salt[16]{};
  unsigned char verifier[32]{};
  const bool ok = RAND_priv_bytes(salt, sizeof(salt)) == 1 &&
                  PKCS5_PBKDF2_HMAC(
                      password.data(), static_cast<int>(password.size()), salt,
                      sizeof(salt), kBootstrapPasswordPbkdf2Iterations,
                      EVP_sha256(), sizeof(verifier), verifier) == 1;
  if (ok) {
    *envelope = "local-password-pbkdf2-sha256:v1:iterations=" +
                std::to_string(kBootstrapPasswordPbkdf2Iterations) +
                ":salt=" + HexEncode(salt, sizeof(salt)) +
                ":verifier=" + HexEncode(verifier, sizeof(verifier));
  }
  OPENSSL_cleanse(salt, sizeof(salt));
  OPENSSL_cleanse(verifier, sizeof(verifier));
  return ok;
}

}  // namespace scratchbird::cli
