// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hash_digest.hpp"
#include <stdexcept>
#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {
// Logical/physical keys can contain arbitrary user octets. Public text facts
// must not reinterpret these private bytes as UTF-8 or expose key contents.
inline std::string ConstraintKeyEvidenceFingerprint(std::string_view key) {
  const auto digest = core::hash::ComputeSha256Digest(
      reinterpret_cast<const core::platform::byte*>(key.data()), key.size());
  if (!digest.ok()) throw std::runtime_error("constraint_key_evidence_hash_failed");
  return "sha256:" + core::hash::HexLower(digest.digest);
}
}  // namespace scratchbird::engine::internal_api
