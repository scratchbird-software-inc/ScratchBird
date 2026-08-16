// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SBLR-PARAMETER-PROVISIONAL-PREPARED-IDENTITY-V1
enum class SblrPreparedCoordinationState : std::uint8_t {
  begun = 1,
  acquired = 2,
  sealed = 3,
  revoked = 4,
};

struct SblrPreparedCoordinationSnapshot {
  std::string coordination_uuid;
  std::string operation_uuid;
  std::string database_uuid;
  std::string session_uuid;
  std::string provisional_prepared_uuid;
  std::uint64_t provisional_prepared_generation{0};
  std::uint64_t coordinator_generation{0};
  std::uint64_t private_handle{0};
  SblrPreparedCoordinationState state{SblrPreparedCoordinationState::revoked};
  std::string seal_evidence_sha256;
  std::string decision_evidence_sha256;
};

struct SblrPreparedCoordinationResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrPreparedCoordinationSnapshot snapshot;
  std::vector<EngineEvidenceReference> evidence;
};

SblrPreparedCoordinationResult BeginSblrPreparedCoordination(
    const EngineRequestContext& context, const std::string& operation_uuid);

SblrPreparedCoordinationResult AcquireSblrPreparedCoordination(
    const EngineRequestContext& context, const std::string& coordination_uuid,
    const std::string& operation_uuid,
    std::uint64_t expected_coordinator_generation);

SblrPreparedCoordinationResult SealSblrPreparedCoordination(
    const EngineRequestContext& context, const std::string& coordination_uuid,
    const std::string& operation_uuid,
    std::uint64_t expected_coordinator_generation,
    const std::string& expected_provisional_prepared_uuid,
    std::uint64_t expected_provisional_prepared_generation,
    const std::string& seal_evidence_sha256);

SblrPreparedCoordinationResult RevokeSblrPreparedCoordination(
    const EngineRequestContext& context, const std::string& coordination_uuid,
    const std::string& operation_uuid,
    std::uint64_t expected_coordinator_generation,
    const std::string& reason_code);

// Replays durable evidence, advances the monotonic high-water mark, and
// durably revokes every unfinished identity before admitting new work.
EngineApiDiagnostic RecoverSblrPreparedCoordinationRegistry(
    const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api
