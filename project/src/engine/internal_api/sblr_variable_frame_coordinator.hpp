// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "sblr_variable_descriptor_registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SBLR-VARIABLE-FRAME-COORDINATION-WORKFLOW-V1
enum class SblrVariableFrameState : std::uint8_t { active=1, acquired=2, revoked=3 };
struct SblrVariableFrameDemand {
  std::uint64_t declaration_occurrence_id{0};
  std::uint16_t datatype_context_code{0};
  bool nullable{false};
  SblrVariableMutability mutability{SblrVariableMutability::immutable};
  SblrVariableValueState initial_state{SblrVariableValueState::uninitialized};
  std::string declaration_token_sha256;
};
struct SblrVariableFrameMapping {
  std::uint64_t declaration_occurrence_id{0};
  SblrVariableDescriptorRow descriptor;
  EngineUuid datatype_type_uuid;
};
struct SblrVariableFrameSnapshot {
  EngineUuid public_coordination_uuid;
  EngineUuid operation_uuid;
  EngineUuid database_uuid;
  EngineUuid session_uuid;
  EngineUuid transaction_uuid;
  EngineUuid statement_receipt_uuid;
  EngineUuid scope_uuid;
  std::uint64_t scope_generation{0};
  EngineUuid frame_uuid;
  std::uint64_t frame_generation{0};
  std::uint64_t coordinator_generation{0};
  EngineUuid registry_snapshot_uuid;
  std::uint64_t registry_generation{0};
  std::uint64_t private_handle{0};
  SblrVariableFrameState state{SblrVariableFrameState::revoked};
  std::vector<SblrVariableFrameMapping> mappings;
  std::string mapping_sha256;
  std::string decision_evidence_sha256;
};
struct SblrVariableFrameResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrVariableFrameSnapshot snapshot;
  std::vector<EngineEvidenceReference> evidence;
};

SblrVariableFrameResult BeginSblrVariableFrame(
    const EngineRequestContext& context, const EngineUuid& operation_uuid,
    std::uint64_t expires_after_ns,
    const std::vector<SblrVariableFrameDemand>& demands);
SblrVariableFrameResult AcquireSblrVariableFrame(
    const EngineRequestContext& context,
    const EngineUuid& public_coordination_uuid,
    const EngineUuid& operation_uuid,
    std::uint64_t expected_coordinator_generation);
SblrVariableFrameResult AssignSblrVariableFrameValues(
    const EngineRequestContext& context,
    const EngineUuid& public_coordination_uuid,
    const EngineUuid& operation_uuid,
    const EngineUuid& preliminary_receipt_uuid,
    std::uint64_t expected_coordinator_generation,
    std::uint64_t expected_registry_generation,
    const std::vector<SblrVariableAssignment>& assignments);
SblrVariableFrameResult CloseSblrVariableFrame(
    const EngineRequestContext& context,
    const EngineUuid& public_coordination_uuid,
    const EngineUuid& operation_uuid,
    std::uint64_t expected_frame_generation,
    const std::string& reason_code);
EngineApiDiagnostic RecoverSblrVariableFrameCoordinator(
    const EngineRequestContext& context);

} // namespace scratchbird::engine::internal_api
