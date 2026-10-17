// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace scratchbird::engine::internal_api {

// Mechanism only: neither these bytes nor a successful comparison admit a
// principal, provider, configuration, memory grant, session or transaction.
// The owning native startup adapter retains its actual committed source,
// protected credential and protected derived-key workspace, and supplies the
// explicit owning crypto-memory scope. No credential or verifier is copied
// into an owning string. Salt and stored verifier are public encoded material.
enum class LocalPasswordCredentialError {
  none, invalid_credential, invalid_verifier, invalid_workspace, mismatch,
  provider_failure
};
struct LocalPasswordCredentialVerification {
  LocalPasswordCredentialError error = LocalPasswordCredentialError::invalid_verifier;
  bool verified() const noexcept { return error == LocalPasswordCredentialError::none; }
};

namespace local_password_credential_detail {
inline int Hex(char value) noexcept {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return 10 + value - 'a';
  return -1;
}
template <std::size_t N>
inline bool Decode(std::string_view value, std::array<unsigned char, N>& out) noexcept {
  if (value.size() != 2 * N) return false;
  for (std::size_t i = 0; i < N; ++i) {
    const int high = Hex(value[2 * i]), low = Hex(value[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    out[i] = static_cast<unsigned char>((high << 4) | low);
  }
  return true;
}
inline bool Disjoint(std::span<std::byte, 32> out, std::string_view input) noexcept {
  if (input.empty()) return true;
  if (!input.data()) return false;
  const auto a = reinterpret_cast<std::uintptr_t>(out.data());
  const auto b = reinterpret_cast<std::uintptr_t>(input.data());
  const auto maximum = std::numeric_limits<std::uintptr_t>::max();
  if (a > maximum - out.size() || b > maximum - input.size()) return false;
  return a + out.size() <= b || b + input.size() <= a;
}
}  // namespace local_password_credential_detail

// The caller exclusively owns the entire 32-byte derived workspace throughout
// this synchronous call. It must not overlap either immutable input. An invalid
// workspace is untouched (in particular it cannot erase its aliased input).
// Every admitted workspace is erased on success, rejection and provider failure.
// OpenSSL allocation/accounting/protection admission remains the owner's duty.
inline LocalPasswordCredentialVerification VerifyLocalPasswordCredential(
    std::string_view stored, std::string_view password,
    std::span<std::byte, 32> derived) noexcept {
  namespace detail = local_password_credential_detail;
  using E = LocalPasswordCredentialError;
  if (!derived.data() || !detail::Disjoint(derived, stored) ||
      !detail::Disjoint(derived, password)) return {E::invalid_workspace};
  struct Erase {
    std::span<std::byte, 32> bytes;
    ~Erase() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
  } erase{derived};
  if (password.empty() || password.size() > 1024 ||
      password.find('\0') != std::string_view::npos) return {E::invalid_credential};
  constexpr std::string_view prefix = "local-password-pbkdf2-sha256:v1:iterations=";
  constexpr std::string_view salt_marker = ":salt=", verifier_marker = ":verifier=";
  constexpr auto maximum_encoding_bytes = prefix.size() + 8 + salt_marker.size() + 32 +
                                           verifier_marker.size() + 64;
  if (stored.size() > maximum_encoding_bytes || !stored.starts_with(prefix))
    return {E::invalid_verifier};
  const auto salt_at = stored.find(salt_marker, prefix.size());
  if (salt_at == std::string_view::npos) return {E::invalid_verifier};
  const auto iteration_text = stored.substr(prefix.size(), salt_at - prefix.size());
  if (iteration_text.empty() || iteration_text.size() > 8 ||
      (iteration_text.size() > 1 && iteration_text.front() == '0'))
    return {E::invalid_verifier};
  std::uint64_t iterations = 0;
  for (const char digit : iteration_text) {
    if (digit < '0' || digit > '9') return {E::invalid_verifier};
    iterations = iterations * 10 + static_cast<unsigned>(digit - '0');
  }
  if (iterations < 600000 || iterations > 10000000) return {E::invalid_verifier};
  const auto salt_begin = salt_at + salt_marker.size();
  const auto verifier_at = stored.find(verifier_marker, salt_begin);
  if (verifier_at == std::string_view::npos || verifier_at - salt_begin != 32)
    return {E::invalid_verifier};
  const auto verifier_begin = verifier_at + verifier_marker.size();
  if (stored.size() - verifier_begin != 64) return {E::invalid_verifier};
  std::array<unsigned char, 16> salt{};
  std::array<unsigned char, 32> expected{};
  if (!detail::Decode(stored.substr(salt_begin, 32), salt) ||
      !detail::Decode(stored.substr(verifier_begin, 64), expected))
    return {E::invalid_verifier};
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
          salt.data(), static_cast<int>(salt.size()), static_cast<int>(iterations),
          EVP_sha256(), static_cast<int>(derived.size()),
          reinterpret_cast<unsigned char*>(derived.data())) != 1)
    return {E::provider_failure};
  return {CRYPTO_memcmp(expected.data(), derived.data(), expected.size()) == 0
              ? E::none : E::mismatch};
}
}  // namespace scratchbird::engine::internal_api
