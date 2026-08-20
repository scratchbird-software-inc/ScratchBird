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
  std::string preliminary_receipt_uuid;
  std::uint64_t structural_occurrence_id = 0;
  std::string parent_transaction_uuid;
  std::string parent_frame_uuid;
  std::string database_uuid;
  std::string attachment_uuid;
  std::string session_uuid;
  std::string principal_uuid;
  std::string security_snapshot_uuid;
  std::string policy_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::uint64_t capability_generation = 0;
  std::string body_sblr_uuid;
  std::string dynamic_statement_sblr_uuid;
  std::string body_sblr_sha256;
  std::uint8_t intent = 0;
  std::uint8_t nesting_depth = 0;
  std::uint16_t effect_count = 0;
  std::string effect_set_sha256;
  std::string projection_evidence_sha256;
};

struct SblrAutonomousFrameSnapshot {
  SblrAutonomousBodyFrameProjectionV1 authority;
  std::string frame_uuid;
  std::uint64_t frame_generation = 0;
  std::string child_transaction_uuid;
  std::uint64_t child_transaction_number = 0;
  std::string descriptor_evidence_sha256;
  std::string recovery_token_uuid;
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
    const EngineRequestContext&, const std::string& preliminary_receipt_uuid,
    std::uint64_t structural_occurrence_id);
EngineApiDiagnostic RevokeSblrAutonomousBodyFrameProjection(
    const EngineRequestContext&, const std::string& preliminary_receipt_uuid);

SblrAutonomousFrameCoordinatorResult ReserveSblrAutonomousFrame(
    const EngineRequestContext&, const std::string& preliminary_receipt_uuid,
    std::uint64_t structural_occurrence_id);
SblrAutonomousFrameCoordinatorResult FinalizeSblrAutonomousFrame(
    const EngineRequestContext&, const std::string& frame,
    std::uint64_t generation, bool commit);
EngineApiDiagnostic RecoverSblrAutonomousFrameCoordinator(
    const EngineRequestContext&);

}  // namespace scratchbird::engine::internal_api
