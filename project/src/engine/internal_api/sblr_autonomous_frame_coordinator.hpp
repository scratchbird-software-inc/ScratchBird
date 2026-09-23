#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {

enum class SblrAutonomousFrameState : std::uint8_t {
  reserved = 1,
  committed = 2,
  rolled_back = 3,
  revoked = 4,
};

// Immutable result of engine.psql_autonomous_body_frame_projection.v1.
// Callers may select it only by authenticated receipt UUID + occurrence id.
struct SblrAutonomousBodyFrameProjectionV1 {
  EngineUuid preliminary_receipt_uuid;
  std::uint64_t structural_occurrence_id = 0;
  EngineUuid parent_transaction_uuid;
  EngineUuid parent_frame_uuid;
  EngineUuid database_uuid;
  EngineUuid attachment_uuid;
  EngineUuid session_uuid;
  EngineUuid principal_uuid;
  EngineUuid security_snapshot_uuid;
  EngineUuid policy_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::uint64_t capability_generation = 0;
  EngineUuid body_sblr_uuid;
  EngineUuid dynamic_statement_sblr_uuid;
  std::string body_sblr_sha256;
  std::uint8_t intent = 0;
  std::uint8_t nesting_depth = 0;
  std::uint16_t effect_count = 0;
  std::string effect_set_sha256;
  std::string projection_evidence_sha256;
};

struct SblrAutonomousFrameSnapshot {
  SblrAutonomousBodyFrameProjectionV1 authority;
  EngineUuid frame_uuid;
  std::uint64_t frame_generation = 0;
  EngineUuid child_transaction_uuid;
  std::uint64_t child_transaction_number = 0;
  std::string descriptor_evidence_sha256;
  EngineUuid recovery_token_uuid;
  std::uint64_t recovery_generation = 0;
  std::uint64_t finality_sequence = 0;
  SblrAutonomousFrameState state = SblrAutonomousFrameState::revoked;
};

struct SblrAutonomousFrameCoordinatorResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  SblrAutonomousFrameSnapshot snapshot;
};

EngineApiDiagnostic PublishSblrAutonomousBodyFrameProjection(
    const EngineRequestContext&,
    const SblrAutonomousBodyFrameProjectionV1&);
EngineApiDiagnostic CompileAndPublishSblrAutonomousBodyFrameProjection(
    const EngineRequestContext&, const EngineUuid& preliminary_receipt_uuid,
    std::uint64_t structural_occurrence_id);
EngineApiDiagnostic RevokeSblrAutonomousBodyFrameProjection(
    const EngineRequestContext&, const EngineUuid& preliminary_receipt_uuid);

SblrAutonomousFrameCoordinatorResult ReserveSblrAutonomousFrame(
    const EngineRequestContext&, const EngineUuid& preliminary_receipt_uuid,
    std::uint64_t structural_occurrence_id);
SblrAutonomousFrameCoordinatorResult FinalizeSblrAutonomousFrame(
    const EngineRequestContext&, const EngineUuid& frame,
    std::uint64_t generation, bool commit);
EngineApiDiagnostic RecoverSblrAutonomousFrameCoordinator(
    const EngineRequestContext&);

}  // namespace scratchbird::engine::internal_api
