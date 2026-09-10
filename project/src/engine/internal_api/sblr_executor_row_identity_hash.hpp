// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_executor_availability_registry.hpp"
#include "hash_digest.hpp"

namespace scratchbird::engine::internal_api {
// Pure canonical tuple hashing. This deliberately performs NO availability
// admission and issues no installed-executor evidence. The registry's public
// private-API wrapper retains its exact admitted-row check.
inline std::string HashSblrExecutorRowIdentityMaterial(
    const SblrExecutorAvailabilityRowIdentity& identity) {
  std::string payload;
  const auto add = [&](std::string_view key, std::string_view value) {
    payload.append(std::to_string(key.size())); payload.push_back(':'); payload.append(key);
    payload.append(std::to_string(value.size())); payload.push_back(':'); payload.append(value);
  };
  add("executor_id", identity.executor_id);
  add("opcode_code", std::to_string(identity.opcode_code));
  add("opcode_version", identity.opcode_version);
  add("operand_descriptor_id", identity.operand_descriptor_id);
  add("result_descriptor_id", identity.result_descriptor_id);
  add("result_descriptor_version", std::to_string(identity.result_descriptor_version));
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
  if (!hash.ok() || hash.digest_bytes != scratchbird::core::hash::kSha256DigestBytes) return {};
  return "sha256:" + scratchbird::core::hash::HexLower(hash.digest);
}
}  // namespace scratchbird::engine::internal_api
