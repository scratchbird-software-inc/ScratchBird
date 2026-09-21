// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "runtime_platform.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {

// Opaque private map bytes, never text UUIDs or SQL syntax. The caller must
// independently validate the transaction/session and local lock authority.
inline std::string MakeNamedLockBinaryKey(
    const core::platform::Uuid& database_uuid, std::string_view semantic_key) {
  if (!core::uuid::IsEngineIdentityUuid(database_uuid) ||
      semantic_key.empty() || semantic_key.size() > 65536) {
    return {};
  }
  std::string key;
  key.reserve(24 + semantic_key.size());
  key.append("NLK1", 4);
  key.append(reinterpret_cast<const char*>(database_uuid.bytes.data()), 16);
  const auto length = static_cast<std::uint32_t>(semantic_key.size());
  for (unsigned byte = 0; byte < 4; ++byte) {
    key.push_back(static_cast<char>((length >> (byte * 8)) & 0xffu));
  }
  key.append(semantic_key);
  return key;
}

// Observability only: cannot substitute for the exact key or lock authority.
// Empty output means hash failure; callers must compute before lock mutation.
inline std::string NamedLockKeyFingerprint(std::string_view key) {
  const auto hash = core::hash::ComputeSha256Digest(
      reinterpret_cast<const core::platform::byte*>(key.data()), key.size());
  return hash.ok() ? "sha256:" + core::hash::HexLower(hash.digest) : std::string{};
}

}  // namespace scratchbird::engine::internal_api
