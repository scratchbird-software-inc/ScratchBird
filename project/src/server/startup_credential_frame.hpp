// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "uuid.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace scratchbird::server {
struct StartupCredentialTransportBinding {
  core::platform::Uuid database, principal, provider, credential_reference;
  std::uint64_t generation = 0;
  std::optional<std::uint32_t> custodian_uid;
};
inline constexpr std::size_t kStartupCredentialHeaderBytes = 88;
inline constexpr std::size_t kStartupCredentialMaximumBytes = 1024;
enum class StartupCredentialFrameError {
  none, invalid_binding, invalid_extent, invalid_header, binding_mismatch,
  invalid_credential
};
struct StartupCredentialFrameDescription {
  StartupCredentialFrameError error = StartupCredentialFrameError::invalid_extent;
  std::size_t credential_bytes = 0;
  bool ok() const noexcept { return error == StartupCredentialFrameError::none; }
};
inline bool ValidStartupCredentialFrameBinding(
    const StartupCredentialTransportBinding& binding) noexcept {
  if (!binding.generation) return false;
  for (const auto* identity : {&binding.database, &binding.principal,
                              &binding.provider, &binding.credential_reference})
    if (!core::uuid::IsEngineIdentityUuid(*identity)) return false;
  return true;
}
// Structural framing only: no source admission, secret-release permission,
// authentication, memory grant, revocation fence or BEGIN authority. UID is an
// OS source-protection expectation, not a field in the authenticated frame.
inline StartupCredentialFrameDescription InspectStartupCredentialFrameHeader(
    std::span<const std::uint8_t> header, std::size_t total_bytes,
    const StartupCredentialTransportBinding& expected) noexcept {
  using E = StartupCredentialFrameError;
  if (!ValidStartupCredentialFrameBinding(expected)) return {E::invalid_binding, 0};
  if (header.size() != kStartupCredentialHeaderBytes ||
      total_bytes < kStartupCredentialHeaderBytes + 1 ||
      total_bytes > kStartupCredentialHeaderBytes + kStartupCredentialMaximumBytes)
    return {E::invalid_extent, 0};
  if (std::memcmp(header.data(), "SBACRED1", 8)) return {E::invalid_header, 0};
  const auto integer = [&](std::size_t offset, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value = (value << 8) | header[offset + i];
    return value;
  };
  const auto count = integer(84, 4);
  if (integer(80, 4) != 1 || !count || count > kStartupCredentialMaximumBytes ||
      total_bytes != kStartupCredentialHeaderBytes + count)
    return {E::invalid_header, 0};
  const std::array identities{expected.database, expected.principal,
                              expected.provider, expected.credential_reference};
  for (std::size_t i = 0; i < identities.size(); ++i)
    if (std::memcmp(header.data() + 8 + 16 * i, identities[i].bytes.data(), 16))
      return {E::binding_mismatch, 0};
  if (integer(72, 8) != expected.generation) return {E::binding_mismatch, 0};
  return {E::none, static_cast<std::size_t>(count)};
}
// Borrowed immutable plaintext staging, supplied by its protected-memory owner.
// No secret copy or pointer is retained. Header-only inspection cannot certify
// the body; a decrypted file must pass this complete-frame inspection.
inline StartupCredentialFrameDescription InspectStartupCredentialFrame(
    std::span<const std::uint8_t> frame,
    const StartupCredentialTransportBinding& expected) noexcept {
  if (frame.size() < kStartupCredentialHeaderBytes)
    return {StartupCredentialFrameError::invalid_extent, 0};
  auto result = InspectStartupCredentialFrameHeader(
      frame.first(kStartupCredentialHeaderBytes), frame.size(), expected);
  if (result.ok() && std::memchr(frame.data() + kStartupCredentialHeaderBytes, 0,
                               result.credential_bytes))
    return {StartupCredentialFrameError::invalid_credential, 0};
  return result;
}
// Allocation-free encoder into exclusive caller-owned protected staging. Never
// allocates a secret string or converts UUIDs into text. Invalid binding/body,
// wrong output extent or overlapping input leaves ALL output untouched.
inline StartupCredentialFrameError EncodeStartupCredentialFrame(
    const StartupCredentialTransportBinding& binding,
    std::span<const std::uint8_t> credential,
    std::span<std::uint8_t> output) noexcept {
  using E = StartupCredentialFrameError;
  if (!ValidStartupCredentialFrameBinding(binding)) return E::invalid_binding;
  if (credential.empty() || credential.size() > kStartupCredentialMaximumBytes ||
      output.size() != kStartupCredentialHeaderBytes + credential.size())
    return E::invalid_extent;
  const auto input_begin = reinterpret_cast<std::uintptr_t>(credential.data());
  const auto output_begin = reinterpret_cast<std::uintptr_t>(output.data());
  if (input_begin > std::numeric_limits<std::uintptr_t>::max() - credential.size() ||
      output_begin > std::numeric_limits<std::uintptr_t>::max() - output.size() ||
      (input_begin < output_begin + output.size() &&
       output_begin < input_begin + credential.size())) return E::invalid_extent;
  if (std::memchr(credential.data(), 0, credential.size())) return E::invalid_credential;
  std::memcpy(output.data(), "SBACRED1", 8);
  const std::array identities{binding.database, binding.principal,
                              binding.provider, binding.credential_reference};
  for (std::size_t i = 0; i < identities.size(); ++i)
    std::memcpy(output.data() + 8 + 16 * i, identities[i].bytes.data(), 16);
  auto integer = [&](std::size_t offset, std::size_t count, std::uint64_t value) {
    for (std::size_t i = count; i; ) { output[offset + --i] = value & 255; value >>= 8; }
  };
  integer(72, 8, binding.generation);
  integer(80, 4, 1);
  integer(84, 4, credential.size());
  std::memcpy(output.data() + kStartupCredentialHeaderBytes,
              credential.data(), credential.size());
  return E::none;
}
} // namespace scratchbird::server
