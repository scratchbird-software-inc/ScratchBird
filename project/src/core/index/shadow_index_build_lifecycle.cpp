// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_SHADOW_INDEX_BUILD_LIFECYCLE
#include "shadow_index_build_lifecycle.hpp"

#include "uuid.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

bool GeneratedDurableUuid(const TypedUuid& value, UuidKind expected_kind) {
  return value.kind == expected_kind && value.valid() &&
         scratchbird::core::uuid::IsDurableEngineIdentityKind(value.kind) &&
         scratchbird::core::uuid::IsEngineIdentityUuid(value.value);
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool ValidIdentity(const ShadowIndexBuildRecord& record) {
  return GeneratedDurableUuid(record.build_id, UuidKind::object) &&
         GeneratedDurableUuid(record.shadow_index_uuid, UuidKind::object) &&
         GeneratedDurableUuid(record.table_uuid, UuidKind::object);
}

bool Terminal(ShadowIndexBuildState state) {
  return state == ShadowIndexBuildState::published ||
         state == ShadowIndexBuildState::cancelled ||
         state == ShadowIndexBuildState::refused;
}

bool SupportedIndexKind(SecondaryIndexKind kind) {
  return kind == SecondaryIndexKind::non_unique;
}

bool MgaEvidencePresent(const ShadowIndexBuildRecord& record) {
  return !record.engine_mga_inventory_evidence_ref.empty() &&
         !record.engine_mga_horizon_evidence_ref.empty();
}

bool PublishEvidenceComplete(const ShadowIndexBuildRecord& record) {
  return record.validation_evidence_present &&
         !record.validation_evidence_ref.empty() &&
         record.publish_barrier_evidence_present &&
         !record.publish_barrier_evidence_ref.empty() &&
         record.publish_barrier_engine_owned_mga;
}

void ApplyVisibility(ShadowIndexBuildRecord* record) {
  const bool visible = record->state == ShadowIndexBuildState::published &&
                       ValidIdentity(*record) &&
                       PublishEvidenceComplete(*record);
  record->planner_visible = visible;
  record->read_visible = visible;
  record->published_index_uuid = visible ? record->shadow_index_uuid : TypedUuid{};
}

ShadowIndexBuildEvidenceRow BuildEvidence(ShadowIndexBuildLedger* ledger,
                                          const ShadowIndexBuildRecord& record,
                                          std::string diagnostic_code,
                                          std::string detail) {
  ShadowIndexBuildEvidenceRow evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.evidence_id = GeneratedId(UuidKind::object, 4100000 + evidence.sequence);
  evidence.build_id = record.build_id;
  evidence.shadow_index_uuid = record.shadow_index_uuid;
  evidence.table_uuid = record.table_uuid;
  evidence.state = record.state;
  evidence.planner_visible = record.planner_visible;
  evidence.read_visible = record.read_visible;
  evidence.validation_evidence_present = record.validation_evidence_present;
  evidence.publish_barrier_evidence_present = record.publish_barrier_evidence_present;
  evidence.publish_barrier_engine_owned_mga = record.publish_barrier_engine_owned_mga;
  evidence.validation_evidence_ref = record.validation_evidence_ref;
  evidence.publish_barrier_evidence_ref = record.publish_barrier_evidence_ref;
  evidence.engine_mga_inventory_evidence_ref =
      record.engine_mga_inventory_evidence_ref;
  evidence.engine_mga_horizon_evidence_ref =
      record.engine_mga_horizon_evidence_ref;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.diagnostic_detail = std::move(detail);
  return evidence;
}

ShadowIndexLifecycleResult Finish(ShadowIndexBuildLedger* ledger,
                                  const ShadowIndexBuildRecord& record,
                                  ShadowIndexBuildDecision decision,
                                  std::string diagnostic_code,
                                  std::string message_key,
                                  std::string detail,
                                  bool fail_closed) {
  ShadowIndexLifecycleResult result;
  result.status = fail_closed ? RefuseStatus() : OkStatus();
  result.decision = decision;
  result.record = record;
  result.fail_closed = fail_closed;
  result.evidence = BuildEvidence(ledger, record, diagnostic_code, detail);
  result.diagnostic = MakeShadowIndexBuildDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  if (ledger != nullptr) {
    ledger->evidence.push_back(result.evidence);
    if (!fail_closed) {
      ++ledger->ledger_generation;
    }
  }
  return result;
}

ShadowIndexLifecycleResult RefuseRecord(ShadowIndexBuildLedger* ledger,
                                        ShadowIndexBuildRecord* record,
                                        std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail) {
  if (record != nullptr && record->state != ShadowIndexBuildState::published &&
      record->state != ShadowIndexBuildState::cancelled) {
    record->state = ShadowIndexBuildState::refused;
    ApplyVisibility(record);
  }
  ShadowIndexBuildRecord snapshot = record == nullptr ? ShadowIndexBuildRecord{} : *record;
  ApplyVisibility(&snapshot);
  return Finish(ledger,
                snapshot,
                ShadowIndexBuildDecision::refused,
                std::move(diagnostic_code),
                std::move(message_key),
                std::move(detail),
                true);
}

ShadowIndexLifecycleResult Transition(ShadowIndexBuildLedger* ledger,
                                      ShadowIndexBuildRecord* record,
                                      ShadowIndexBuildState expected,
                                      ShadowIndexBuildState next,
                                      std::string diagnostic_code,
                                      std::string message_key) {
  if (record == nullptr) {
    ShadowIndexBuildRecord empty;
    return Finish(ledger,
                  empty,
                  ShadowIndexBuildDecision::refused,
                  "shadow_index_build_missing_record",
                  "core.index.shadow_build.missing_record",
                  "shadow index build record is required",
                  true);
  }
  if (Terminal(record->state)) {
    ApplyVisibility(record);
    return Finish(ledger,
                  *record,
                  ShadowIndexBuildDecision::refused,
                  "shadow_index_build_terminal_state",
                  "core.index.shadow_build.terminal_state",
                  ShadowIndexBuildStateName(record->state),
                  true);
  }
  if (record->state != expected) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_build_invalid_transition",
                        "core.index.shadow_build.invalid_transition",
                        std::string(ShadowIndexBuildStateName(record->state)) +
                            "->" + ShadowIndexBuildStateName(next));
  }
  record->state = next;
  ApplyVisibility(record);
  return Finish(ledger,
                *record,
                ShadowIndexBuildDecision::accepted,
                std::move(diagnostic_code),
                std::move(message_key),
                {},
                false);
}

}  // namespace

const char* ShadowIndexBuildStateName(ShadowIndexBuildState state) {
  switch (state) {
    case ShadowIndexBuildState::requested: return "requested";
    case ShadowIndexBuildState::building: return "building";
    case ShadowIndexBuildState::built: return "built";
    case ShadowIndexBuildState::validated: return "validated";
    case ShadowIndexBuildState::publish_ready: return "publish_ready";
    case ShadowIndexBuildState::published: return "published";
    case ShadowIndexBuildState::cancelled: return "cancelled";
    case ShadowIndexBuildState::refused: return "refused";
  }
  return "refused";
}

const char* ShadowIndexBuildDecisionName(ShadowIndexBuildDecision decision) {
  switch (decision) {
    case ShadowIndexBuildDecision::accepted: return "accepted";
    case ShadowIndexBuildDecision::cancelled: return "cancelled";
    case ShadowIndexBuildDecision::refused: return "refused";
  }
  return "refused";
}

ShadowIndexLifecycleResult RequestShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    const ShadowIndexBuildRequest& request) {
  ShadowIndexBuildRecord record;
  record.build_id = request.build_id.valid()
                        ? request.build_id
                        : GeneratedId(UuidKind::object,
                                      4000000 + (ledger == nullptr
                                                     ? 1
                                                     : ledger->next_evidence_sequence));
  record.shadow_index_uuid = request.shadow_index_uuid;
  record.table_uuid = request.table_uuid;
  record.index_kind = request.index_kind;
  record.state = ShadowIndexBuildState::requested;
  record.engine_mga_inventory_evidence_ref =
      request.engine_mga_inventory_evidence_ref;
  record.engine_mga_horizon_evidence_ref =
      request.engine_mga_horizon_evidence_ref;
  ApplyVisibility(&record);

  if (!ValidIdentity(record)) {
    record.state = ShadowIndexBuildState::refused;
    ApplyVisibility(&record);
    return Finish(ledger,
                  record,
                  ShadowIndexBuildDecision::refused,
                  "shadow_index_build_invalid_identity",
                  "core.index.shadow_build.invalid_identity",
                  "build_id shadow_index_uuid and table_uuid require generated durable object UUIDs",
                  true);
  }
  if (!SupportedIndexKind(record.index_kind)) {
    record.state = ShadowIndexBuildState::refused;
    ApplyVisibility(&record);
    return Finish(ledger,
                  record,
                  ShadowIndexBuildDecision::refused,
                  "shadow_index_build_unsupported_index_kind",
                  "core.index.shadow_build.unsupported_index_kind",
                  "DPC-040 supports only non-unique secondary shadow build lifecycle",
                  true);
  }
  if (!MgaEvidencePresent(record)) {
    record.state = ShadowIndexBuildState::refused;
    ApplyVisibility(&record);
    return Finish(ledger,
                  record,
                  ShadowIndexBuildDecision::refused,
                  "shadow_index_build_mga_evidence_missing",
                  "core.index.shadow_build.mga_evidence_missing",
                  "shadow build requires engine MGA inventory and horizon evidence",
                  true);
  }
  return Finish(ledger,
                record,
                ShadowIndexBuildDecision::accepted,
                "shadow_index_build_requested",
                "core.index.shadow_build.requested",
                {},
                false);
}

ShadowIndexLifecycleResult StartShadowIndexBuild(ShadowIndexBuildLedger* ledger,
                                                 ShadowIndexBuildRecord* record) {
  return Transition(ledger,
                    record,
                    ShadowIndexBuildState::requested,
                    ShadowIndexBuildState::building,
                    "shadow_index_build_building",
                    "core.index.shadow_build.building");
}

ShadowIndexLifecycleResult CompleteShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record) {
  return Transition(ledger,
                    record,
                    ShadowIndexBuildState::building,
                    ShadowIndexBuildState::built,
                    "shadow_index_build_built",
                    "core.index.shadow_build.built");
}

ShadowIndexLifecycleResult ValidateShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record,
    const ShadowIndexValidationRequest& request) {
  if (record == nullptr) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_build_missing_record",
                        "core.index.shadow_build.missing_record",
                        "shadow index build record is required");
  }
  if (Terminal(record->state)) {
    ApplyVisibility(record);
    return Finish(ledger,
                  *record,
                  ShadowIndexBuildDecision::refused,
                  "shadow_index_build_terminal_state",
                  "core.index.shadow_build.terminal_state",
                  ShadowIndexBuildStateName(record->state),
                  true);
  }
  if (record->state != ShadowIndexBuildState::built) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_build_invalid_transition",
                        "core.index.shadow_build.invalid_transition",
                        std::string(ShadowIndexBuildStateName(record->state)) +
                            "->validated");
  }
  if (!request.validation_succeeded || request.validation_evidence_ref.empty() ||
      !request.engine_mga_inventory_evidence_present) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_build_validation_refused",
                        "core.index.shadow_build.validation_refused",
                        "validation requires success evidence tied to engine MGA inventory");
  }
  record->state = ShadowIndexBuildState::validated;
  record->validation_evidence_present = true;
  record->validation_evidence_ref = request.validation_evidence_ref;
  ApplyVisibility(record);
  return Finish(ledger,
                *record,
                ShadowIndexBuildDecision::accepted,
                "shadow_index_build_validated",
                "core.index.shadow_build.validated",
                {},
                false);
}

ShadowIndexLifecycleResult MarkShadowIndexPublishReady(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record,
    const ShadowIndexPublishBarrierRequest& request) {
  if (record == nullptr) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_build_missing_record",
                        "core.index.shadow_build.missing_record",
                        "shadow index build record is required");
  }
  if (Terminal(record->state)) {
    ApplyVisibility(record);
    return Finish(ledger,
                  *record,
                  ShadowIndexBuildDecision::refused,
                  "shadow_index_build_terminal_state",
                  "core.index.shadow_build.terminal_state",
                  ShadowIndexBuildStateName(record->state),
                  true);
  }
  if (record->state != ShadowIndexBuildState::validated) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_build_invalid_transition",
                        "core.index.shadow_build.invalid_transition",
                        std::string(ShadowIndexBuildStateName(record->state)) +
                            "->publish_ready");
  }
  if (!record->validation_evidence_present ||
      record->validation_evidence_ref.empty()) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_publish_ready_validation_missing",
                        "core.index.shadow_build.publish_ready_validation_missing",
                        "publish-ready transition requires validation evidence");
  }
  if (request.publish_barrier_evidence_ref.empty() ||
      !request.engine_owned_mga_publish_barrier) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_publish_barrier_refused",
                        "core.index.shadow_build.publish_barrier_refused",
                        "publish requires engine-owned MGA publish barrier evidence");
  }
  record->state = ShadowIndexBuildState::publish_ready;
  record->publish_barrier_evidence_present = true;
  record->publish_barrier_engine_owned_mga = true;
  record->publish_barrier_evidence_ref = request.publish_barrier_evidence_ref;
  ApplyVisibility(record);
  return Finish(ledger,
                *record,
                ShadowIndexBuildDecision::accepted,
                "shadow_index_publish_ready",
                "core.index.shadow_build.publish_ready",
                {},
                false);
}

ShadowIndexLifecycleResult PublishShadowIndexBuild(ShadowIndexBuildLedger* ledger,
                                                   ShadowIndexBuildRecord* record) {
  if (record == nullptr) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_publish_missing_record",
                        "core.index.shadow_build.publish_missing_record",
                        "shadow index build record is required");
  }
  if (!ValidIdentity(*record)) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_publish_invalid_identity",
                        "core.index.shadow_build.publish_invalid_identity",
                        "build table and index UUIDs must be generated durable object UUIDs");
  }
  if (!SupportedIndexKind(record->index_kind)) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_publish_unsafe_index_kind",
                        "core.index.shadow_build.publish_unsafe_index_kind",
                        "DPC-040 refuses shadow publish for unsupported or unsafe index kinds");
  }
  if (record->state != ShadowIndexBuildState::published &&
      Terminal(record->state)) {
    ApplyVisibility(record);
    return Finish(ledger,
                  *record,
                  ShadowIndexBuildDecision::refused,
                  "shadow_index_publish_terminal_state",
                  "core.index.shadow_build.publish_terminal_state",
                  ShadowIndexBuildStateName(record->state),
                  true);
  }
  if (!record->validation_evidence_present ||
      record->validation_evidence_ref.empty()) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_publish_validation_missing",
                        "core.index.shadow_build.publish_validation_missing",
                        "publish requires validation evidence");
  }
  if (!record->publish_barrier_evidence_present ||
      record->publish_barrier_evidence_ref.empty() ||
      !record->publish_barrier_engine_owned_mga) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_publish_barrier_missing",
                        "core.index.shadow_build.publish_barrier_missing",
                        "publish requires engine-owned publish barrier evidence");
  }
  if (record->state == ShadowIndexBuildState::published) {
    ApplyVisibility(record);
    return Finish(ledger,
                  *record,
                  ShadowIndexBuildDecision::accepted,
                  "shadow_index_publish_already_published",
                  "core.index.shadow_build.already_published",
                  {},
                  false);
  }
  if (record->state != ShadowIndexBuildState::publish_ready) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_publish_state_not_ready",
                        "core.index.shadow_build.publish_state_not_ready",
                        ShadowIndexBuildStateName(record->state));
  }

  record->state = ShadowIndexBuildState::published;
  ApplyVisibility(record);
  return Finish(ledger,
                *record,
                ShadowIndexBuildDecision::accepted,
                "shadow_index_publish_success",
                "core.index.shadow_build.publish_success",
                {},
                false);
}

ShadowIndexLifecycleResult CancelShadowIndexBuild(ShadowIndexBuildLedger* ledger,
                                                  ShadowIndexBuildRecord* record,
                                                  std::string reason) {
  if (record == nullptr) {
    return RefuseRecord(ledger,
                        record,
                        "shadow_index_build_missing_record",
                        "core.index.shadow_build.missing_record",
                        "shadow index build record is required");
  }
  if (record->state == ShadowIndexBuildState::published) {
    ApplyVisibility(record);
    return Finish(ledger,
                  *record,
                  ShadowIndexBuildDecision::refused,
                  "shadow_index_cancel_published_refused",
                  "core.index.shadow_build.cancel_published_refused",
                  "published shadow index cannot be cancelled",
                  true);
  }
  record->state = ShadowIndexBuildState::cancelled;
  ApplyVisibility(record);
  return Finish(ledger,
                *record,
                ShadowIndexBuildDecision::cancelled,
                "shadow_index_build_cancelled",
                "core.index.shadow_build.cancelled",
                std::move(reason),
                false);
}

ShadowIndexLifecycleResult RefuseShadowIndexBuild(ShadowIndexBuildLedger* ledger,
                                                  ShadowIndexBuildRecord* record,
                                                  std::string diagnostic_code,
                                                  std::string reason) {
  if (diagnostic_code.empty()) {
    diagnostic_code = "shadow_index_build_refused";
  }
  if (record == nullptr) {
    return RefuseRecord(ledger,
                        record,
                        std::move(diagnostic_code),
                        "core.index.shadow_build.refused",
                        std::move(reason));
  }
  if (record->state != ShadowIndexBuildState::published) {
    record->state = ShadowIndexBuildState::refused;
  }
  ApplyVisibility(record);
  return Finish(ledger,
                *record,
                ShadowIndexBuildDecision::refused,
                std::move(diagnostic_code),
                "core.index.shadow_build.refused",
                std::move(reason),
                true);
}

ShadowIndexPlannerVisibilityResult EvaluateShadowIndexPlannerVisibility(
    const ShadowIndexBuildRecord& record) {
  ShadowIndexPlannerVisibilityResult result;
  if (!ValidIdentity(record)) {
    result.status = RefuseStatus();
    result.diagnostic = MakeShadowIndexBuildDiagnostic(
        result.status,
        "shadow_index_visibility_invalid_identity",
        "core.index.shadow_build.visibility_invalid_identity",
        "planner route requires generated durable build table and index UUIDs");
    return result;
  }
  if (record.state == ShadowIndexBuildState::published &&
      !PublishEvidenceComplete(record)) {
    result.status = RefuseStatus();
    result.diagnostic = MakeShadowIndexBuildDiagnostic(
        result.status,
        "shadow_index_visibility_publish_evidence_missing",
        "core.index.shadow_build.visibility_publish_evidence_missing",
        "planner route requires validation and engine-owned MGA publish barrier evidence");
    return result;
  }
  if (record.state == ShadowIndexBuildState::published &&
      (!GeneratedDurableUuid(record.published_index_uuid, UuidKind::object) ||
       !SameUuid(record.published_index_uuid, record.shadow_index_uuid))) {
    result.status = RefuseStatus();
    result.diagnostic = MakeShadowIndexBuildDiagnostic(
        result.status,
        "shadow_index_visibility_publish_identity_mismatch",
        "core.index.shadow_build.visibility_publish_identity_mismatch",
        "published index UUID must preserve the generated shadow index UUID");
    return result;
  }
  result.planner_visible = record.state == ShadowIndexBuildState::published &&
                           record.planner_visible;
  result.read_visible = record.state == ShadowIndexBuildState::published &&
                        record.read_visible;
  result.visible_index_uuid =
      result.planner_visible && result.read_visible ? record.published_index_uuid
                                                    : TypedUuid{};
  result.status = result.planner_visible && result.read_visible ? OkStatus()
                                                               : RefuseStatus();
  result.diagnostic = MakeShadowIndexBuildDiagnostic(
      result.status,
      result.ok() ? "shadow_index_visibility_published"
                  : "shadow_index_visibility_hidden_until_publish",
      result.ok() ? "core.index.shadow_build.visibility_published"
                  : "core.index.shadow_build.visibility_hidden_until_publish",
      ShadowIndexBuildStateName(record.state));
  return result;
}

DiagnosticRecord MakeShadowIndexBuildDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.shadow_build_lifecycle");
}

}  // namespace scratchbird::core::index
