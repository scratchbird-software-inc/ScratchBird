// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_SHADOW_INDEX_DELTA_DRAIN_RECOVERY
#include "shadow_index_delta_drain_recovery.hpp"

#include "uuid.hpp"

#include <algorithm>
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

Status LimitStatus() {
  return {StatusCode::memory_limit_exceeded, Severity::error, Subsystem::engine};
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool GeneratedDurableUuid(const TypedUuid& value, UuidKind expected_kind) {
  return value.kind == expected_kind && value.valid() &&
         scratchbird::core::uuid::IsDurableEngineIdentityKind(value.kind) &&
         scratchbird::core::uuid::IsEngineIdentityUuid(value.value);
}

bool ValidBuildIdentity(const TypedUuid& build_id,
                        const TypedUuid& shadow_index_uuid,
                        const TypedUuid& table_uuid) {
  return GeneratedDurableUuid(build_id, UuidKind::object) &&
         GeneratedDurableUuid(shadow_index_uuid, UuidKind::object) &&
         GeneratedDurableUuid(table_uuid, UuidKind::object);
}

bool ValidBuildRecordIdentity(const ShadowIndexBuildRecord& record) {
  return ValidBuildIdentity(record.build_id,
                            record.shadow_index_uuid,
                            record.table_uuid);
}

bool ValidDeltaIdentity(const SecondaryIndexDeltaEntry& delta) {
  return GeneratedDurableUuid(delta.delta_id, UuidKind::object) &&
         GeneratedDurableUuid(delta.index_uuid, UuidKind::object) &&
         GeneratedDurableUuid(delta.table_uuid, UuidKind::object) &&
         GeneratedDurableUuid(delta.row_uuid, UuidKind::row) &&
         GeneratedDurableUuid(delta.version_uuid, UuidKind::row) &&
         GeneratedDurableUuid(delta.transaction_uuid, UuidKind::transaction) &&
         delta.local_transaction_id != 0 &&
         !delta.key_payload.empty() &&
         !delta.cleanup_horizon_token.empty();
}

bool SameTarget(const TypedUuid& shadow_index_uuid,
                const TypedUuid& table_uuid,
                const SecondaryIndexDeltaEntry& delta) {
  return SameUuid(shadow_index_uuid, delta.index_uuid) &&
         SameUuid(table_uuid, delta.table_uuid);
}

bool SameBaseDeltaEntry(const SecondaryIndexBaseEntry& base,
                        const SecondaryIndexDeltaEntry& delta) {
  return SameUuid(base.index_uuid, delta.index_uuid) &&
         SameUuid(base.table_uuid, delta.table_uuid) &&
         SameUuid(base.row_uuid, delta.row_uuid) &&
         base.key_payload == delta.key_payload;
}

bool SameDeltaIdentity(const SecondaryIndexDeltaEntry& left,
                       const SecondaryIndexDeltaEntry& right) {
  return SameUuid(left.delta_id, right.delta_id);
}

bool SameAppliedRecord(const ShadowIndexAppliedDeltaRecord& left,
                       const ShadowIndexAppliedDeltaRecord& right) {
  return SameDeltaIdentity(left.delta, right.delta) &&
         SameUuid(left.delta.index_uuid, right.delta.index_uuid) &&
         SameUuid(left.delta.table_uuid, right.delta.table_uuid) &&
         SameUuid(left.delta.row_uuid, right.delta.row_uuid) &&
         SameUuid(left.delta.version_uuid, right.delta.version_uuid) &&
         SameUuid(left.delta.transaction_uuid, right.delta.transaction_uuid) &&
         left.delta.local_transaction_id == right.delta.local_transaction_id &&
         left.delta.delta_kind == right.delta.delta_kind &&
         left.delta.key_payload == right.delta.key_payload &&
         left.delta.cleanup_horizon_token == right.delta.cleanup_horizon_token &&
         left.delta.committed == right.delta.committed;
}

bool AppliedDeltaPresent(const ShadowIndexDeltaDrainLedger& ledger,
                         const SecondaryIndexDeltaEntry& delta,
                         const ShadowIndexAppliedDeltaRecord** record = nullptr) {
  for (const auto& applied : ledger.applied_deltas) {
    if (SameDeltaIdentity(applied.delta, delta)) {
      if (record != nullptr) {
        *record = &applied;
      }
      return true;
    }
  }
  return false;
}

void RemoveBaseEntry(std::vector<SecondaryIndexBaseEntry>* shadow_entries,
                     const SecondaryIndexDeltaEntry& delta) {
  shadow_entries->erase(std::remove_if(shadow_entries->begin(),
                                       shadow_entries->end(),
                                       [&](const SecondaryIndexBaseEntry& base) {
                                         return SameBaseDeltaEntry(base, delta);
                                       }),
                        shadow_entries->end());
}

bool AddBaseEntryIfMissing(std::vector<SecondaryIndexBaseEntry>* shadow_entries,
                           const SecondaryIndexDeltaEntry& delta) {
  const auto found = std::find_if(shadow_entries->begin(),
                                  shadow_entries->end(),
                                  [&](const SecondaryIndexBaseEntry& base) {
                                    return SameBaseDeltaEntry(base, delta);
                                  });
  if (found != shadow_entries->end()) {
    return false;
  }
  SecondaryIndexBaseEntry entry;
  entry.index_uuid = delta.index_uuid;
  entry.table_uuid = delta.table_uuid;
  entry.row_uuid = delta.row_uuid;
  entry.version_uuid = delta.version_uuid;
  entry.key_payload = delta.key_payload;
  entry.committed_local_transaction_id = delta.local_transaction_id;
  entry.deleted = false;
  shadow_entries->push_back(std::move(entry));
  return true;
}

bool ApplyDeltaToShadowEntries(std::vector<SecondaryIndexBaseEntry>* shadow_entries,
                               const SecondaryIndexDeltaEntry& delta) {
  switch (delta.delta_kind) {
    case SecondaryIndexDeltaKind::insert:
    case SecondaryIndexDeltaKind::update_after:
      return AddBaseEntryIfMissing(shadow_entries, delta);
    case SecondaryIndexDeltaKind::delete_row:
    case SecondaryIndexDeltaKind::update_before:
      RemoveBaseEntry(shadow_entries, delta);
      return true;
  }
  return false;
}

bool MgaAuthorityPresent(const ShadowIndexDeltaDrainRequest& request) {
  return request.durable_transaction_inventory_authoritative &&
         request.durable_transaction_horizon_authoritative &&
         request.authoritative_visible_through_local_transaction_id != 0 &&
         !request.engine_mga_inventory_evidence_ref.empty() &&
         !request.engine_mga_horizon_evidence_ref.empty();
}

bool UnsafeExternalAuthorityRequested(const ShadowIndexDeltaDrainRequest& request) {
  return request.parser_finality_authority ||
         request.client_state_authority ||
         request.timestamp_ordering_authority ||
         request.uuid_ordering_authority ||
         request.event_stream_authority;
}

bool LifecyclePublishEvidenceComplete(const ShadowIndexBuildRecord& record) {
  return ValidBuildRecordIdentity(record) &&
         record.index_kind == SecondaryIndexKind::non_unique &&
         record.validation_evidence_present &&
         !record.validation_evidence_ref.empty() &&
         record.publish_barrier_evidence_present &&
         !record.publish_barrier_evidence_ref.empty() &&
         record.publish_barrier_engine_owned_mga;
}

ShadowIndexDeltaDrainEvidenceRow BuildEvidence(
    ShadowIndexDeltaDrainLedger* ledger,
    const ShadowIndexDeltaDrainRequest& request,
    ShadowIndexDeltaDrainState state,
    u64 scanned_delta_count,
    u64 eligible_committed_delta_count,
    u64 newly_applied_delta_count,
    u64 idempotent_replayed_delta_count,
    u64 skipped_uncommitted_delta_count,
    u64 skipped_wrong_target_delta_count,
    bool drain_complete,
    std::string diagnostic_code,
    std::string detail) {
  ShadowIndexDeltaDrainEvidenceRow evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.evidence_id = GeneratedId(UuidKind::object, 4200000 + evidence.sequence);
  evidence.build_id = request.build_id;
  evidence.shadow_index_uuid = request.shadow_index_uuid;
  evidence.table_uuid = request.table_uuid;
  evidence.state = state;
  evidence.scanned_delta_count = scanned_delta_count;
  evidence.eligible_committed_delta_count = eligible_committed_delta_count;
  evidence.newly_applied_delta_count = newly_applied_delta_count;
  evidence.idempotent_replayed_delta_count = idempotent_replayed_delta_count;
  evidence.skipped_uncommitted_delta_count = skipped_uncommitted_delta_count;
  evidence.skipped_wrong_target_delta_count = skipped_wrong_target_delta_count;
  evidence.applied_delta_total_count =
      ledger == nullptr ? 0 : ledger->applied_deltas.size();
  evidence.drain_complete = drain_complete;
  evidence.durable_transaction_inventory_authoritative =
      request.durable_transaction_inventory_authoritative;
  evidence.durable_transaction_horizon_authoritative =
      request.durable_transaction_horizon_authoritative;
  evidence.parser_finality_authority = false;
  evidence.client_state_authority = false;
  evidence.timestamp_ordering_authority = false;
  evidence.uuid_ordering_authority = false;
  evidence.event_stream_authority = false;
  evidence.engine_mga_inventory_evidence_ref =
      request.engine_mga_inventory_evidence_ref;
  evidence.engine_mga_horizon_evidence_ref =
      request.engine_mga_horizon_evidence_ref;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.diagnostic_detail = std::move(detail);
  return evidence;
}

ShadowIndexDeltaDrainResult FinishDrain(ShadowIndexDeltaDrainLedger* ledger,
                                        const ShadowIndexDeltaDrainRequest& request,
                                        Status status,
                                        bool drained,
                                        bool fail_closed,
                                        u64 scanned_delta_count,
                                        u64 eligible_committed_delta_count,
                                        u64 newly_applied_delta_count,
                                        u64 idempotent_replayed_delta_count,
                                        u64 skipped_uncommitted_delta_count,
                                        u64 skipped_wrong_target_delta_count,
                                        ShadowIndexDeltaDrainState state,
                                        std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail) {
  ShadowIndexDeltaDrainResult result;
  result.status = status;
  result.drained = drained;
  result.fail_closed = fail_closed;
  result.scanned_delta_count = scanned_delta_count;
  result.eligible_committed_delta_count = eligible_committed_delta_count;
  result.newly_applied_delta_count = newly_applied_delta_count;
  result.idempotent_replayed_delta_count = idempotent_replayed_delta_count;
  result.skipped_uncommitted_delta_count = skipped_uncommitted_delta_count;
  result.skipped_wrong_target_delta_count = skipped_wrong_target_delta_count;
  result.evidence = BuildEvidence(ledger,
                                  request,
                                  state,
                                  scanned_delta_count,
                                  eligible_committed_delta_count,
                                  newly_applied_delta_count,
                                  idempotent_replayed_delta_count,
                                  skipped_uncommitted_delta_count,
                                  skipped_wrong_target_delta_count,
                                  drained && !fail_closed,
                                  diagnostic_code,
                                  detail);
  result.diagnostic = MakeShadowIndexDeltaDrainDiagnostic(
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

ShadowIndexDeltaDrainResult RefuseDrain(ShadowIndexDeltaDrainLedger* ledger,
                                        const ShadowIndexDeltaDrainRequest& request,
                                        std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail,
                                        Status status = RefuseStatus()) {
  return FinishDrain(ledger,
                     request,
                     status,
                     false,
                     true,
                     0,
                     0,
                     0,
                     0,
                     0,
                     0,
                     ShadowIndexDeltaDrainState::refused,
                     std::move(diagnostic_code),
                     std::move(message_key),
                     std::move(detail));
}

bool DrainLedgerIdentityMatches(const ShadowIndexDeltaDrainLedger& ledger,
                                const ShadowIndexDeltaDrainRequest& request) {
  return SameUuid(ledger.build_id, request.build_id) &&
         SameUuid(ledger.shadow_index_uuid, request.shadow_index_uuid) &&
         SameUuid(ledger.table_uuid, request.table_uuid);
}

bool HasAuthoritativeCompletionEvidence(const ShadowIndexDeltaDrainLedger& ledger) {
  for (const auto& evidence : ledger.evidence) {
    if (evidence.drain_complete &&
        evidence.state == ShadowIndexDeltaDrainState::drained &&
        evidence.durable_transaction_inventory_authoritative &&
        evidence.durable_transaction_horizon_authoritative &&
        !evidence.engine_mga_inventory_evidence_ref.empty() &&
        !evidence.engine_mga_horizon_evidence_ref.empty()) {
      return true;
    }
  }
  return false;
}

DiagnosticRecord ValidateAppliedDeltaEvidence(
    const ShadowIndexDeltaDrainLedger& ledger,
    const ShadowIndexDeltaDrainRequest& request) {
  if (!DrainLedgerIdentityMatches(ledger, request)) {
    return MakeShadowIndexDeltaDrainDiagnostic(
        RefuseStatus(),
        "shadow_delta_recovery_identity_mismatch",
        "core.index.shadow_delta_drain.recovery_identity_mismatch",
        "drain evidence build table or shadow index identity does not match reopen request");
  }
  for (std::size_t i = 0; i < ledger.applied_deltas.size(); ++i) {
    const auto& applied = ledger.applied_deltas[i];
    if (!ValidDeltaIdentity(applied.delta) ||
        !SameTarget(request.shadow_index_uuid, request.table_uuid, applied.delta) ||
        !applied.delta.committed ||
        applied.delta.local_transaction_id >
            request.authoritative_visible_through_local_transaction_id ||
        applied.durable_delta_identity_ref.empty() ||
        applied.engine_mga_inventory_evidence_ref.empty() ||
        applied.engine_mga_horizon_evidence_ref.empty()) {
      return MakeShadowIndexDeltaDrainDiagnostic(
          RefuseStatus(),
          "shadow_delta_recovery_corrupt_delta_identity",
          "core.index.shadow_delta_drain.recovery_corrupt_delta_identity",
          "applied delta evidence is missing generated identity or MGA finality evidence");
    }
    for (std::size_t j = i + 1; j < ledger.applied_deltas.size(); ++j) {
      if (SameDeltaIdentity(applied.delta, ledger.applied_deltas[j].delta) &&
          !SameAppliedRecord(applied, ledger.applied_deltas[j])) {
        return MakeShadowIndexDeltaDrainDiagnostic(
            RefuseStatus(),
            "shadow_delta_recovery_corrupt_delta_identity",
            "core.index.shadow_delta_drain.recovery_corrupt_delta_identity",
            "durable delta identity maps to conflicting applied records");
      }
    }
  }
  return MakeShadowIndexDeltaDrainDiagnostic(
      OkStatus(), "ok", "core.index.shadow_delta_drain.applied_evidence_valid");
}

DiagnosticRecord ValidateLedgerAppliedDeltaEvidence(
    const ShadowIndexDeltaDrainLedger& ledger) {
  if (!ValidBuildIdentity(ledger.build_id,
                          ledger.shadow_index_uuid,
                          ledger.table_uuid)) {
    return MakeShadowIndexDeltaDrainDiagnostic(
        RefuseStatus(),
        "shadow_delta_publish_drain_evidence_corrupt",
        "core.index.shadow_delta_drain.publish_drain_evidence_corrupt",
        "drain evidence build table or shadow index identity is invalid");
  }
  if (ledger.authoritative_visible_through_local_transaction_id == 0) {
    return MakeShadowIndexDeltaDrainDiagnostic(
        RefuseStatus(),
        "shadow_delta_publish_drain_evidence_incomplete",
        "core.index.shadow_delta_drain.publish_drain_evidence_incomplete",
        "drain evidence lacks authoritative MGA visible-through horizon");
  }
  for (std::size_t i = 0; i < ledger.applied_deltas.size(); ++i) {
    const auto& applied = ledger.applied_deltas[i];
    if (!ValidDeltaIdentity(applied.delta) ||
        !SameTarget(ledger.shadow_index_uuid, ledger.table_uuid, applied.delta) ||
        !applied.delta.committed ||
        applied.delta.local_transaction_id >
            ledger.authoritative_visible_through_local_transaction_id ||
        applied.durable_delta_identity_ref.empty() ||
        applied.engine_mga_inventory_evidence_ref.empty() ||
        applied.engine_mga_horizon_evidence_ref.empty()) {
      return MakeShadowIndexDeltaDrainDiagnostic(
          RefuseStatus(),
          "shadow_delta_publish_drain_evidence_corrupt",
          "core.index.shadow_delta_drain.publish_drain_evidence_corrupt",
          "applied delta evidence is missing generated identity or MGA finality evidence");
    }
    for (std::size_t j = i + 1; j < ledger.applied_deltas.size(); ++j) {
      if (SameDeltaIdentity(applied.delta, ledger.applied_deltas[j].delta) &&
          !SameAppliedRecord(applied, ledger.applied_deltas[j])) {
        return MakeShadowIndexDeltaDrainDiagnostic(
            RefuseStatus(),
            "shadow_delta_publish_drain_evidence_corrupt",
            "core.index.shadow_delta_drain.publish_drain_evidence_corrupt",
            "durable delta identity maps to conflicting applied records");
      }
    }
  }
  return MakeShadowIndexDeltaDrainDiagnostic(
      OkStatus(),
      "ok",
      "core.index.shadow_delta_drain.publish_applied_evidence_valid");
}

ShadowIndexDeltaDrainRecoveryResult FinishRecovery(
    ShadowIndexDeltaDrainLedger* ledger,
    const ShadowIndexDeltaDrainRequest& request,
    Status status,
    bool recovered,
    bool fail_closed,
    ShadowIndexDeltaDrainRecoveryClass recovery_class,
    ShadowIndexDeltaDrainRecoveryAction action,
    u64 replayed_delta_count,
    u64 idempotent_replayed_delta_count,
    std::string stable_reason,
    std::string diagnostic_code,
    std::string message_key) {
  ShadowIndexDeltaDrainRecoveryResult result;
  result.status = status;
  result.recovered = recovered;
  result.fail_closed = fail_closed;
  result.recovery_class = recovery_class;
  result.action = action;
  result.replayed_delta_count = replayed_delta_count;
  result.idempotent_replayed_delta_count = idempotent_replayed_delta_count;
  result.stable_reason = stable_reason;
  result.evidence = BuildEvidence(ledger,
                                  request,
                                  recovered ? ShadowIndexDeltaDrainState::recovered
                                            : (fail_closed ? ShadowIndexDeltaDrainState::refused
                                                           : ShadowIndexDeltaDrainState::drained),
                                  0,
                                  0,
                                  replayed_delta_count,
                                  idempotent_replayed_delta_count,
                                  0,
                                  0,
                                  recovered || (!fail_closed &&
                                                recovery_class == ShadowIndexDeltaDrainRecoveryClass::clean_complete),
                                  diagnostic_code,
                                  stable_reason);
  result.diagnostic = MakeShadowIndexDeltaDrainDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(stable_reason));
  if (ledger != nullptr) {
    ledger->evidence.push_back(result.evidence);
    if (!fail_closed) {
      ++ledger->ledger_generation;
    }
  }
  return result;
}

}  // namespace

const char* ShadowIndexDeltaDrainStateName(ShadowIndexDeltaDrainState state) {
  switch (state) {
    case ShadowIndexDeltaDrainState::absent: return "absent";
    case ShadowIndexDeltaDrainState::draining: return "draining";
    case ShadowIndexDeltaDrainState::drained: return "drained";
    case ShadowIndexDeltaDrainState::recovered: return "recovered";
    case ShadowIndexDeltaDrainState::refused: return "refused";
    case ShadowIndexDeltaDrainState::corrupt: return "corrupt";
  }
  return "corrupt";
}

const char* ShadowIndexDeltaDrainRecoveryClassName(
    ShadowIndexDeltaDrainRecoveryClass recovery_class) {
  switch (recovery_class) {
    case ShadowIndexDeltaDrainRecoveryClass::clean_complete:
      return "clean_complete";
    case ShadowIndexDeltaDrainRecoveryClass::recoverable_replay:
      return "recoverable_replay";
    case ShadowIndexDeltaDrainRecoveryClass::incomplete_refuse_visible_use:
      return "incomplete_refuse_visible_use";
    case ShadowIndexDeltaDrainRecoveryClass::corrupt_refuse_visible_use:
      return "corrupt_refuse_visible_use";
    case ShadowIndexDeltaDrainRecoveryClass::non_authoritative_refuse_visible_use:
      return "non_authoritative_refuse_visible_use";
  }
  return "corrupt_refuse_visible_use";
}

const char* ShadowIndexDeltaDrainRecoveryActionName(
    ShadowIndexDeltaDrainRecoveryAction action) {
  switch (action) {
    case ShadowIndexDeltaDrainRecoveryAction::no_action: return "no_action";
    case ShadowIndexDeltaDrainRecoveryAction::replay_applied_deltas_once:
      return "replay_applied_deltas_once";
    case ShadowIndexDeltaDrainRecoveryAction::refuse_visible_index_use:
      return "refuse_visible_index_use";
  }
  return "refuse_visible_index_use";
}

ShadowIndexDeltaDrainResult DrainShadowIndexCommittedDeltas(
    ShadowIndexDeltaDrainLedger* drain_ledger,
    const SecondaryIndexDeltaLedger& source_delta_ledger,
    const ShadowIndexDeltaDrainRequest& request) {
  if (drain_ledger == nullptr) {
    return RefuseDrain(nullptr,
                       request,
                       "shadow_delta_drain_missing_ledger",
                       "core.index.shadow_delta_drain.missing_ledger",
                       "shadow delta drain ledger is required");
  }
  if (!ValidBuildIdentity(request.build_id,
                          request.shadow_index_uuid,
                          request.table_uuid)) {
    return RefuseDrain(drain_ledger,
                       request,
                       "shadow_delta_drain_invalid_identity",
                       "core.index.shadow_delta_drain.invalid_identity",
                       "build table and shadow index UUIDs must be generated durable object UUIDs");
  }
  if (!MgaAuthorityPresent(request)) {
    return RefuseDrain(drain_ledger,
                       request,
                       "shadow_delta_drain_non_authoritative_mga",
                       "core.index.shadow_delta_drain.non_authoritative_mga",
                       "drain requires durable MGA transaction inventory and horizon evidence");
  }
  if (UnsafeExternalAuthorityRequested(request)) {
    return RefuseDrain(drain_ledger,
                       request,
                       "shadow_delta_drain_external_authority_refused",
                       "core.index.shadow_delta_drain.external_authority_refused",
                       "parser client timestamp UUID ordering and event streams are not finality authority");
  }
  if (request.max_records_to_scan == 0 || request.max_records_to_apply == 0) {
    return RefuseDrain(drain_ledger,
                       request,
                       "shadow_delta_drain_resource_governor_throttled",
                       "core.index.shadow_delta_drain.resource_governor_throttled",
                       "drain requires nonzero bounded scan and apply budgets",
                       LimitStatus());
  }
  if (!drain_ledger->build_id.valid()) {
    drain_ledger->build_id = request.build_id;
    drain_ledger->shadow_index_uuid = request.shadow_index_uuid;
    drain_ledger->table_uuid = request.table_uuid;
  } else if (!DrainLedgerIdentityMatches(*drain_ledger, request)) {
    return RefuseDrain(drain_ledger,
                       request,
                       "shadow_delta_drain_identity_mismatch",
                       "core.index.shadow_delta_drain.identity_mismatch",
                       "existing drain ledger belongs to a different build table or shadow index");
  }

  u64 scanned_count = 0;
  u64 eligible_count = 0;
  u64 new_apply_count = 0;
  u64 idempotent_count = 0;
  u64 skipped_uncommitted_count = 0;
  u64 skipped_wrong_target_count = 0;

  for (const auto& delta : source_delta_ledger.deltas) {
    if (!SameTarget(request.shadow_index_uuid, request.table_uuid, delta)) {
      ++skipped_wrong_target_count;
      continue;
    }
    if (++scanned_count > request.max_records_to_scan) {
      return FinishDrain(drain_ledger,
                         request,
                         LimitStatus(),
                         false,
                         true,
                         scanned_count,
                         eligible_count,
                         new_apply_count,
                         idempotent_count,
                         skipped_uncommitted_count,
                         skipped_wrong_target_count,
                         ShadowIndexDeltaDrainState::refused,
                         "shadow_delta_drain_resource_governor_throttled",
                         "core.index.shadow_delta_drain.resource_governor_throttled",
                         "delta ledger scan budget exhausted before drain completed");
    }
    if (!ValidDeltaIdentity(delta)) {
      return FinishDrain(drain_ledger,
                         request,
                         RefuseStatus(),
                         false,
                         true,
                         scanned_count,
                         eligible_count,
                         new_apply_count,
                         idempotent_count,
                         skipped_uncommitted_count,
                         skipped_wrong_target_count,
                         ShadowIndexDeltaDrainState::corrupt,
                         "shadow_delta_drain_corrupt_delta_identity",
                         "core.index.shadow_delta_drain.corrupt_delta_identity",
                         "target delta lacks generated durable identity or cleanup horizon evidence");
    }
    if (!delta.committed ||
        delta.local_transaction_id >
            request.authoritative_visible_through_local_transaction_id) {
      ++skipped_uncommitted_count;
      continue;
    }
    ++eligible_count;
    const ShadowIndexAppliedDeltaRecord* existing = nullptr;
    if (AppliedDeltaPresent(*drain_ledger, delta, &existing)) {
      if (existing == nullptr || !SameAppliedRecord(*existing, {delta, {}, {}, {}})) {
        return FinishDrain(drain_ledger,
                           request,
                           RefuseStatus(),
                           false,
                           true,
                           scanned_count,
                           eligible_count,
                           new_apply_count,
                           idempotent_count,
                           skipped_uncommitted_count,
                           skipped_wrong_target_count,
                           ShadowIndexDeltaDrainState::corrupt,
                           "shadow_delta_drain_conflicting_delta_identity",
                           "core.index.shadow_delta_drain.conflicting_delta_identity",
                           "durable delta identity maps to conflicting shadow drain records");
      }
      ++idempotent_count;
      continue;
    }
    if (new_apply_count >= request.max_records_to_apply) {
      return FinishDrain(drain_ledger,
                         request,
                         LimitStatus(),
                         false,
                         true,
                         scanned_count,
                         eligible_count,
                         new_apply_count,
                         idempotent_count,
                         skipped_uncommitted_count,
                         skipped_wrong_target_count,
                         ShadowIndexDeltaDrainState::refused,
                         "shadow_delta_drain_resource_governor_throttled",
                         "core.index.shadow_delta_drain.resource_governor_throttled",
                         "eligible delta drain batch exceeds apply budget");
    }

    ApplyDeltaToShadowEntries(&drain_ledger->shadow_entries, delta);
    ShadowIndexAppliedDeltaRecord applied;
    applied.delta = delta;
    applied.durable_delta_identity_ref =
        "durable_delta_id:" +
        scratchbird::core::uuid::UuidToString(delta.delta_id.value);
    applied.engine_mga_inventory_evidence_ref =
        request.engine_mga_inventory_evidence_ref;
    applied.engine_mga_horizon_evidence_ref =
        request.engine_mga_horizon_evidence_ref;
    drain_ledger->applied_deltas.push_back(std::move(applied));
    ++new_apply_count;
  }

  drain_ledger->drain_complete = true;
  drain_ledger->authoritative_visible_through_local_transaction_id =
      request.authoritative_visible_through_local_transaction_id;
  drain_ledger->durable_transaction_inventory_authoritative = true;
  drain_ledger->durable_transaction_horizon_authoritative = true;
  drain_ledger->engine_mga_inventory_evidence_ref =
      request.engine_mga_inventory_evidence_ref;
  drain_ledger->engine_mga_horizon_evidence_ref =
      request.engine_mga_horizon_evidence_ref;

  return FinishDrain(drain_ledger,
                     request,
                     OkStatus(),
                     true,
                     false,
                     scanned_count,
                     eligible_count,
                     new_apply_count,
                     idempotent_count,
                     skipped_uncommitted_count,
                     skipped_wrong_target_count,
                     ShadowIndexDeltaDrainState::drained,
                     "shadow_delta_drain_complete",
                     "core.index.shadow_delta_drain.complete",
                     "committed shadow deltas drained under durable MGA authority");
}

ShadowIndexDeltaDrainRecoveryResult ClassifyShadowIndexDeltaDrainForRecovery(
    const ShadowIndexDeltaDrainLedger& drain_ledger,
    const ShadowIndexDeltaDrainRequest& request) {
  if (!MgaAuthorityPresent(request)) {
    return FinishRecovery(nullptr,
                          request,
                          RefuseStatus(),
                          false,
                          true,
                          ShadowIndexDeltaDrainRecoveryClass::non_authoritative_refuse_visible_use,
                          ShadowIndexDeltaDrainRecoveryAction::refuse_visible_index_use,
                          0,
                          0,
                          "recovery requires durable MGA transaction inventory and horizon evidence",
                          "shadow_delta_recovery_non_authoritative_mga",
                          "core.index.shadow_delta_drain.recovery_non_authoritative_mga");
  }
  const auto validation = ValidateAppliedDeltaEvidence(drain_ledger, request);
  if (!validation.status.ok()) {
    return FinishRecovery(nullptr,
                          request,
                          validation.status,
                          false,
                          true,
                          ShadowIndexDeltaDrainRecoveryClass::corrupt_refuse_visible_use,
                          ShadowIndexDeltaDrainRecoveryAction::refuse_visible_index_use,
                          0,
                          0,
                          validation.arguments.empty()
                              ? "drain evidence is corrupt"
                              : validation.arguments.front().value,
                          validation.diagnostic_code,
                          validation.message_key);
  }
  if (!drain_ledger.drain_complete ||
      !drain_ledger.durable_transaction_inventory_authoritative ||
      !drain_ledger.durable_transaction_horizon_authoritative ||
      drain_ledger.engine_mga_inventory_evidence_ref.empty() ||
      drain_ledger.engine_mga_horizon_evidence_ref.empty() ||
      !HasAuthoritativeCompletionEvidence(drain_ledger)) {
    return FinishRecovery(nullptr,
                          request,
                          RefuseStatus(),
                          false,
                          true,
                          ShadowIndexDeltaDrainRecoveryClass::incomplete_refuse_visible_use,
                          ShadowIndexDeltaDrainRecoveryAction::refuse_visible_index_use,
                          0,
                          0,
                          "drain completion evidence is missing incomplete or non-authoritative",
                          "shadow_delta_recovery_incomplete_drain_evidence",
                          "core.index.shadow_delta_drain.recovery_incomplete_drain_evidence");
  }

  u64 missing_shadow_entries = 0;
  for (const auto& applied : drain_ledger.applied_deltas) {
    const bool insert_like =
        applied.delta.delta_kind == SecondaryIndexDeltaKind::insert ||
        applied.delta.delta_kind == SecondaryIndexDeltaKind::update_after;
    if (!insert_like) {
      continue;
    }
    const auto found = std::find_if(drain_ledger.shadow_entries.begin(),
                                    drain_ledger.shadow_entries.end(),
                                    [&](const SecondaryIndexBaseEntry& entry) {
                                      return SameBaseDeltaEntry(entry, applied.delta);
                                    });
    if (found == drain_ledger.shadow_entries.end()) {
      ++missing_shadow_entries;
    }
  }
  if (missing_shadow_entries > 0) {
    return FinishRecovery(nullptr,
                          request,
                          OkStatus(),
                          false,
                          false,
                          ShadowIndexDeltaDrainRecoveryClass::recoverable_replay,
                          ShadowIndexDeltaDrainRecoveryAction::replay_applied_deltas_once,
                          0,
                          0,
                          "authoritative applied delta evidence can be replayed idempotently",
                          "shadow_delta_recovery_replay_required",
                          "core.index.shadow_delta_drain.recovery_replay_required");
  }
  return FinishRecovery(nullptr,
                        request,
                        OkStatus(),
                        false,
                        false,
                        ShadowIndexDeltaDrainRecoveryClass::clean_complete,
                        ShadowIndexDeltaDrainRecoveryAction::no_action,
                        0,
                        0,
                        "drain evidence is complete and shadow entries are already consistent",
                        "shadow_delta_recovery_clean_complete",
                        "core.index.shadow_delta_drain.recovery_clean_complete");
}

ShadowIndexDeltaDrainRecoveryResult RecoverShadowIndexDeltaDrain(
    ShadowIndexDeltaDrainLedger* drain_ledger,
    const ShadowIndexDeltaDrainRequest& request) {
  if (drain_ledger == nullptr) {
    return FinishRecovery(nullptr,
                          request,
                          RefuseStatus(),
                          false,
                          true,
                          ShadowIndexDeltaDrainRecoveryClass::corrupt_refuse_visible_use,
                          ShadowIndexDeltaDrainRecoveryAction::refuse_visible_index_use,
                          0,
                          0,
                          "shadow delta drain ledger is required",
                          "shadow_delta_recovery_missing_ledger",
                          "core.index.shadow_delta_drain.recovery_missing_ledger");
  }
  const auto classified = ClassifyShadowIndexDeltaDrainForRecovery(*drain_ledger, request);
  if (classified.fail_closed ||
      classified.action == ShadowIndexDeltaDrainRecoveryAction::no_action) {
    return FinishRecovery(drain_ledger,
                          request,
                          classified.status,
                          !classified.fail_closed,
                          classified.fail_closed,
                          classified.recovery_class,
                          classified.action,
                          0,
                          0,
                          classified.stable_reason,
                          classified.diagnostic.diagnostic_code,
                          classified.diagnostic.message_key);
  }

  u64 replayed_count = 0;
  u64 idempotent_count = 0;
  for (const auto& applied : drain_ledger->applied_deltas) {
    const std::size_t before = drain_ledger->shadow_entries.size();
    ApplyDeltaToShadowEntries(&drain_ledger->shadow_entries, applied.delta);
    if (drain_ledger->shadow_entries.size() == before) {
      ++idempotent_count;
    } else {
      ++replayed_count;
    }
  }
  drain_ledger->drain_complete = true;
  drain_ledger->durable_transaction_inventory_authoritative = true;
  drain_ledger->durable_transaction_horizon_authoritative = true;
  return FinishRecovery(drain_ledger,
                        request,
                        OkStatus(),
                        true,
                        false,
                        ShadowIndexDeltaDrainRecoveryClass::recoverable_replay,
                        ShadowIndexDeltaDrainRecoveryAction::replay_applied_deltas_once,
                        replayed_count,
                        idempotent_count,
                        "recoverable shadow delta drain replay completed exactly once",
                        "shadow_delta_recovery_replayed",
                        "core.index.shadow_delta_drain.recovery_replayed");
}

ShadowIndexDeltaPublishEligibilityResult EvaluateShadowIndexDeltaDrainPublishEligibility(
    const ShadowIndexBuildRecord& lifecycle_record,
    const ShadowIndexDeltaDrainLedger* drain_ledger) {
  ShadowIndexDeltaPublishEligibilityResult result;
  if (drain_ledger == nullptr) {
    result.status = RefuseStatus();
    result.diagnostic = MakeShadowIndexDeltaDrainDiagnostic(
        result.status,
        "shadow_delta_publish_drain_evidence_missing",
        "core.index.shadow_delta_drain.publish_drain_evidence_missing",
        "publish eligibility requires shadow delta drain evidence");
    return result;
  }
  if (!SameUuid(lifecycle_record.build_id, drain_ledger->build_id) ||
      !SameUuid(lifecycle_record.shadow_index_uuid, drain_ledger->shadow_index_uuid) ||
      !SameUuid(lifecycle_record.table_uuid, drain_ledger->table_uuid)) {
    result.status = RefuseStatus();
    result.diagnostic = MakeShadowIndexDeltaDrainDiagnostic(
        result.status,
        "shadow_delta_publish_identity_mismatch",
        "core.index.shadow_delta_drain.publish_identity_mismatch",
        "lifecycle and drain evidence identities do not match");
    return result;
  }
  if (!LifecyclePublishEvidenceComplete(lifecycle_record)) {
    result.status = RefuseStatus();
    result.diagnostic = MakeShadowIndexDeltaDrainDiagnostic(
        result.status,
        "shadow_delta_publish_lifecycle_evidence_incomplete",
        "core.index.shadow_delta_drain.publish_lifecycle_evidence_incomplete",
        "publish eligibility requires generated lifecycle identity validation evidence and engine-owned publish barrier");
    return result;
  }
  if (!drain_ledger->drain_complete ||
      !drain_ledger->durable_transaction_inventory_authoritative ||
      !drain_ledger->durable_transaction_horizon_authoritative ||
      drain_ledger->engine_mga_inventory_evidence_ref.empty() ||
      drain_ledger->engine_mga_horizon_evidence_ref.empty() ||
      !HasAuthoritativeCompletionEvidence(*drain_ledger)) {
    result.status = RefuseStatus();
    result.diagnostic = MakeShadowIndexDeltaDrainDiagnostic(
        result.status,
        "shadow_delta_publish_drain_evidence_incomplete",
        "core.index.shadow_delta_drain.publish_drain_evidence_incomplete",
        "publish eligibility requires complete authoritative shadow delta drain evidence");
    return result;
  }
  const auto applied_evidence_validation =
      ValidateLedgerAppliedDeltaEvidence(*drain_ledger);
  if (!applied_evidence_validation.status.ok()) {
    result.status = applied_evidence_validation.status;
    result.diagnostic = applied_evidence_validation;
    return result;
  }

  auto lifecycle_route = EvaluateShadowIndexPlannerVisibility(lifecycle_record);
  if (lifecycle_record.state != ShadowIndexBuildState::publish_ready &&
      lifecycle_record.state != ShadowIndexBuildState::published) {
    result.status = RefuseStatus();
    result.diagnostic = MakeShadowIndexDeltaDrainDiagnostic(
        result.status,
        "shadow_delta_publish_lifecycle_not_ready",
        "core.index.shadow_delta_drain.publish_lifecycle_not_ready",
        ShadowIndexBuildStateName(lifecycle_record.state));
    return result;
  }
  if (lifecycle_record.state == ShadowIndexBuildState::published &&
      !lifecycle_route.ok()) {
    result.status = lifecycle_route.status;
    result.diagnostic = MakeShadowIndexDeltaDrainDiagnostic(
        result.status,
        lifecycle_route.diagnostic.diagnostic_code,
        lifecycle_route.diagnostic.message_key,
        "published lifecycle route refused visibility");
    return result;
  }

  result.status = OkStatus();
  result.publish_allowed = true;
  result.planner_visible = lifecycle_record.state == ShadowIndexBuildState::published &&
                           lifecycle_route.planner_visible;
  result.read_visible = lifecycle_record.state == ShadowIndexBuildState::published &&
                        lifecycle_route.read_visible;
  result.visible_index_uuid =
      result.planner_visible && result.read_visible
          ? lifecycle_route.visible_index_uuid
          : TypedUuid{};
  result.diagnostic = MakeShadowIndexDeltaDrainDiagnostic(
      result.status,
      "shadow_delta_publish_eligible",
      "core.index.shadow_delta_drain.publish_eligible",
      "lifecycle and authoritative delta drain evidence permit publish route");
  return result;
}

ShadowIndexLifecycleResult PublishShadowIndexBuildWithDeltaDrainEvidence(
    ShadowIndexBuildLedger* lifecycle_ledger,
    ShadowIndexBuildRecord* lifecycle_record,
    const ShadowIndexDeltaDrainLedger* drain_ledger) {
  if (lifecycle_record == nullptr) {
    return RefuseShadowIndexBuild(lifecycle_ledger,
                                  lifecycle_record,
                                  "shadow_delta_publish_missing_lifecycle_record",
                                  "shadow delta publish requires a lifecycle record");
  }
  const auto eligibility =
      EvaluateShadowIndexDeltaDrainPublishEligibility(*lifecycle_record, drain_ledger);
  if (!eligibility.ok()) {
    return RefuseShadowIndexBuild(lifecycle_ledger,
                                  lifecycle_record,
                                  eligibility.diagnostic.diagnostic_code,
                                  eligibility.diagnostic.arguments.empty()
                                      ? "shadow delta publish eligibility refused"
                                      : eligibility.diagnostic.arguments.front().value);
  }
  return PublishShadowIndexBuild(lifecycle_ledger, lifecycle_record);
}

DiagnosticRecord MakeShadowIndexDeltaDrainDiagnostic(Status status,
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
                        "core.index.shadow_delta_drain",
                        status.ok() ? "" : "refuse shadow index visibility until durable MGA delta drain evidence is complete");
}

}  // namespace scratchbird::core::index
