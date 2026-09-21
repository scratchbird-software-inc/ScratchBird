// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../core/uuid/uuid.hpp"

#include <array>
#include <cstdint>

namespace scratchbird::server {

// Copy an already-binary system identity into its canonical wire carrier.
// This validates representation only, never receipt ownership or permission.
// In particular, malformed nonzero optional identities are not absent.
inline bool CopyCoordinationSystemUuid(
    const scratchbird::core::platform::Uuid& identity,
    std::array<std::uint8_t, 16>* output, bool allow_nil = false) noexcept {
  if (output == nullptr) return false;
  if (scratchbird::core::uuid::IsNilUuid(identity)) {
    if (!allow_nil) return false;
  } else if (!scratchbird::core::uuid::IsEngineIdentityUuid(identity)) {
    return false;
  }
  *output = identity.bytes;
  return true;
}

}  // namespace scratchbird::server
