// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../core/uuid/uuid.hpp"
#include <algorithm>
#include <array>
#include <span>

namespace scratchbird::wire {
// Structural identity packet only. This does not prove manifest signatures,
// resource provenance, package admission, or authorization.
struct LanguageBundleIdentityV1 {
  core::platform::Uuid bundle_uuid;
  core::platform::Uuid dialect_profile_uuid;
  core::platform::Uuid topology_profile_uuid;
  bool operator==(const LanguageBundleIdentityV1&) const = default;
};
using LanguageBundleIdentityBytesV1 = std::array<std::uint8_t, 48>;
inline bool LanguageBundleIdentityValidV1(const LanguageBundleIdentityV1& value) {
  return core::uuid::IsEngineIdentityUuid(value.bundle_uuid) &&
      (value.dialect_profile_uuid.is_nil() || core::uuid::IsEngineIdentityUuid(value.dialect_profile_uuid)) &&
      (value.topology_profile_uuid.is_nil() || core::uuid::IsEngineIdentityUuid(value.topology_profile_uuid));
}
inline bool EncodeLanguageBundleIdentityV1(const LanguageBundleIdentityV1& value,
                                           LanguageBundleIdentityBytesV1* output) {
  if (!output || !LanguageBundleIdentityValidV1(value)) return false;
  LanguageBundleIdentityBytesV1 bytes{};
  std::copy(value.bundle_uuid.bytes.begin(), value.bundle_uuid.bytes.end(), bytes.begin());
  std::copy(value.dialect_profile_uuid.bytes.begin(), value.dialect_profile_uuid.bytes.end(), bytes.begin() + 16);
  std::copy(value.topology_profile_uuid.bytes.begin(), value.topology_profile_uuid.bytes.end(), bytes.begin() + 32);
  *output = bytes;
  return true;
}
inline bool DecodeLanguageBundleIdentityV1(std::span<const std::uint8_t> bytes,
                                           LanguageBundleIdentityV1* output) {
  if (!output || bytes.size() != 48) return false;
  LanguageBundleIdentityV1 value;
  std::copy_n(bytes.begin(), 16, value.bundle_uuid.bytes.begin());
  std::copy_n(bytes.begin() + 16, 16, value.dialect_profile_uuid.bytes.begin());
  std::copy_n(bytes.begin() + 32, 16, value.topology_profile_uuid.bytes.begin());
  if (!LanguageBundleIdentityValidV1(value)) return false;
  *output = value;
  return true;
}
}  // namespace scratchbird::wire
