// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-SECONDARY-INDEX-DELTA-MERGE-ANCHOR
// DPC_SECONDARY_INDEX_DELTA_MERGE_AGENT_GATE
#include "secondary_index_delta_merge.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status MergeOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status MergeErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool SameIndexTable(const TypedUuid& left_index,
                    const TypedUuid& left_table,
                    const TypedUuid& right_index,
                    const TypedUuid& right_table) {
  return SameUuid(left_index, right_index) && SameUuid(left_table, right_table);
}

bool SameBaseDeltaEntry(const SecondaryIndexBaseEntry& base, const SecondaryIndexDeltaEntry& delta) {
  return SameUuid(base.index_uuid, delta.index_uuid) &&
         SameUuid(base.table_uuid, delta.table_uuid) &&
         SameUuid(base.row_uuid, delta.row_uuid) &&
         base.key_payload == delta.key_payload;
}

TypedUuid NewEvidenceId(const SecondaryIndexDeltaMergeLedger* ledger) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      ledger == nullptr ? 1 : ledger->next_evidence_sequence);
  return generated.ok() ? generated.value : TypedUuid{};
}

TypedUuid DefaultMergeId(const SecondaryIndexDeltaMergeLedger* ledger) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      ledger == nullptr ? 9000 : 9000 + ledger->next_evidence_sequence);
  return generated.ok() ? generated.value : TypedUuid{};
}

SecondaryIndexMergeEvidenceRecord BuildEvidence(SecondaryIndexDeltaMergeLedger* ledger,
                                                const TypedUuid& merge_id,
                                                const TypedUuid& index_uuid,
                                                const TypedUuid& table_uuid,
                                                u64 merged_count,
                                                u64 retained_delta_count,
                                                u64 cleaned_delta_count,
                                                u64 scanned_delta_count,
                                                u64 max_records_to_scan,
                                                u64 max_records_to_merge,
                                                u64 cleanup_horizon,
                                                SecondaryIndexMergeState new_state,
                                                std::string diagnostic_code,
                                                std::string throttle_or_refusal_reason,
                                                bool durable_state_changed) {
  SecondaryIndexMergeEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.evidence_id = NewEvidenceId(ledger);
  evidence.merge_id = merge_id;
  evidence.index_uuid = index_uuid;
  evidence.table_uuid = table_uuid;
  evidence.ledger_generation_before = ledger == nullptr ? 0 : ledger->ledger_generation;
  evidence.ledger_generation_after = durable_state_changed && ledger != nullptr ? ledger->ledger_generation + 1
                                                                               : (ledger == nullptr ? 0 : ledger->ledger_generation);
  evidence.merged_count = merged_count;
  evidence.retained_delta_count = retained_delta_count;
  evidence.cleaned_delta_count = cleaned_delta_count;
  evidence.scanned_delta_count = scanned_delta_count;
  evidence.max_records_to_scan = max_records_to_scan;
  evidence.max_records_to_merge = max_records_to_merge;
  evidence.authoritative_cleanup_horizon_local_transaction_id = cleanup_horizon;
  evidence.previous_state = SecondaryIndexMergeState::absent;
  evidence.new_state = new_state;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.throttle_or_refusal_reason = std::move(throttle_or_refusal_reason);
  evidence.durable_state_changed = durable_state_changed;
  return evidence;
}

SecondaryIndexMergeResult RefuseMerge(SecondaryIndexDeltaMergeLedger* merge_ledger,
                                      const SecondaryIndexMergeRequest& request,
                                      std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail) {
  SecondaryIndexMergeResult result;
  result.status = MergeErrorStatus();
  result.merged = false;
  result.throttled = diagnostic_code == "resource_governor_throttled";
  result.evidence = BuildEvidence(merge_ledger,
                                  request.merge_id.valid() ? request.merge_id : DefaultMergeId(merge_ledger),
                                  request.index_uuid,
                                  request.table_uuid,
                                  0,
                                  0,
                                  0,
                                  0,
                                  request.max_records_to_scan,
                                  request.max_records_to_merge,
                                  request.authoritative_cleanup_horizon_local_transaction_id,
                                  SecondaryIndexMergeState::refused,
                                  diagnostic_code,
                                  detail,
                                  false);
  result.ledger_generation = merge_ledger == nullptr ? 0 : merge_ledger->ledger_generation;
  result.diagnostic = MakeSecondaryIndexMergeDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  if (merge_ledger != nullptr) {
    merge_ledger->evidence.push_back(result.evidence);
  }
  return result;
}

SecondaryIndexCleanupResult RefuseCleanup(SecondaryIndexDeltaMergeLedger* merge_ledger,
                                          const SecondaryIndexCleanupRequest& request,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail,
                                          bool horizon_blocked,
                                          u64 retained_count) {
  SecondaryIndexCleanupResult result;
  result.status = MergeErrorStatus();
  result.cleaned = false;
  result.horizon_blocked = horizon_blocked;
  result.retained_count = retained_count;
  result.evidence = BuildEvidence(merge_ledger,
                                  DefaultMergeId(merge_ledger),
                                  request.index_uuid,
                                  request.table_uuid,
                                  0,
                                  retained_count,
                                  0,
                                  0,
                                  0,
                                  0,
                                  request.authoritative_cleanup_horizon_local_transaction_id,
                                  horizon_blocked ? SecondaryIndexMergeState::cleanup_horizon_blocked
                                                  : SecondaryIndexMergeState::refused,
                                  diagnostic_code,
                                  detail,
                                  false);
  result.diagnostic = MakeSecondaryIndexMergeDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  if (merge_ledger != nullptr) {
    merge_ledger->evidence.push_back(result.evidence);
  }
  return result;
}

void RemoveBaseEntry(std::vector<SecondaryIndexBaseEntry>* base_entries,
                     const SecondaryIndexDeltaEntry& delta) {
  base_entries->erase(std::remove_if(base_entries->begin(),
                                     base_entries->end(),
                                     [&](const SecondaryIndexBaseEntry& base) {
                                       return SameBaseDeltaEntry(base, delta);
                                     }),
                      base_entries->end());
}

bool AddBaseEntryIfMissing(std::vector<SecondaryIndexBaseEntry>* base_entries,
                           const SecondaryIndexDeltaEntry& delta) {
  const auto found = std::find_if(base_entries->begin(),
                                  base_entries->end(),
                                  [&](const SecondaryIndexBaseEntry& base) {
                                    return SameBaseDeltaEntry(base, delta);
                                  });
  if (found != base_entries->end()) {
    return false;
  }
  SecondaryIndexBaseEntry base;
  base.index_uuid = delta.index_uuid;
  base.table_uuid = delta.table_uuid;
  base.row_uuid = delta.row_uuid;
  base.version_uuid = delta.version_uuid;
  base.key_payload = delta.key_payload;
  base.committed_local_transaction_id = delta.local_transaction_id;
  base.deleted = false;
  base_entries->push_back(base);
  return true;
}

}  // namespace

const char* SecondaryIndexMergeStateName(SecondaryIndexMergeState state) {
  switch (state) {
    case SecondaryIndexMergeState::absent:
      return "absent";
    case SecondaryIndexMergeState::merged:
      return "merged";
    case SecondaryIndexMergeState::cleanup_horizon_blocked:
      return "cleanup_horizon_blocked";
    case SecondaryIndexMergeState::cleaned:
      return "cleaned";
    case SecondaryIndexMergeState::refused:
      return "refused";
    case SecondaryIndexMergeState::quarantine:
      return "quarantine";
  }
  return "unknown";
}

const char* SecondaryIndexMergeRecoveryActionName(SecondaryIndexMergeRecoveryAction action) {
  switch (action) {
    case SecondaryIndexMergeRecoveryAction::no_action:
      return "no_action";
    case SecondaryIndexMergeRecoveryAction::complete_idempotent_merge:
      return "complete_idempotent_merge";
    case SecondaryIndexMergeRecoveryAction::retain_until_horizon:
      return "retain_until_horizon";
    case SecondaryIndexMergeRecoveryAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

SecondaryIndexMergeResult MergeSecondaryIndexDeltas(SecondaryIndexDeltaMergeLedger* merge_ledger,
                                                    std::vector<SecondaryIndexBaseEntry>* base_entries,
                                                    SecondaryIndexDeltaLedger* delta_ledger,
                                                    const SecondaryIndexMergeRequest& request) {
  if (merge_ledger == nullptr || base_entries == nullptr || delta_ledger == nullptr) {
    return RefuseMerge(merge_ledger,
                       request,
                       "secondary_index_delta_merge_missing_ledger",
                       "core.index.secondary_delta_merge.missing_ledger",
                       "merge ledger, base index entries, and delta ledger are required");
  }
  if (!request.index_uuid.valid() || !request.table_uuid.valid()) {
    return RefuseMerge(merge_ledger,
                       request,
                       "secondary_index_delta_merge_invalid_identity",
                       "core.index.secondary_delta_merge.invalid_identity",
                       "index_uuid and table_uuid must be valid engine UUIDs");
  }
  if (request.merge_disabled) {
    return RefuseMerge(merge_ledger,
                       request,
                       "merge_agent_disabled",
                       "core.index.secondary_delta_merge.disabled",
                       "secondary-index delta merge agent is disabled by request");
  }
  if (request.index_kind == SecondaryIndexKind::unique) {
    return RefuseMerge(merge_ledger,
                       request,
                       "unique_index_delta_refused",
                       "core.index.secondary_delta_merge.unique_index_delta_refused",
                       "unique secondary-index deltas cannot use deferred merge");
  }
  if (!request.cleanup_horizon_authoritative) {
    return RefuseMerge(merge_ledger,
                       request,
                       "not_authoritative_horizon",
                       "core.index.secondary_delta_merge.horizon_not_authoritative",
                       "merge requires authoritative cleanup horizon");
  }
  if (request.max_records_to_scan == 0 || request.max_records_to_merge == 0) {
    return RefuseMerge(merge_ledger,
                       request,
                       "resource_governor_throttled",
                       "core.index.secondary_delta_merge.resource_governor_throttled",
                       "merge request requires nonzero bounded scan and merge budgets");
  }

  const TypedUuid merge_id = request.merge_id.valid() ? request.merge_id : DefaultMergeId(merge_ledger);
  u64 scanned_count = 0;
  u64 eligible_count = 0;
  u64 retained_count = 0;
  for (const auto& delta : delta_ledger->deltas) {
    if (!SameIndexTable(request.index_uuid, request.table_uuid, delta.index_uuid, delta.table_uuid)) {
      continue;
    }
    if (++scanned_count > request.max_records_to_scan) {
      return RefuseMerge(merge_ledger,
                         request,
                         "resource_governor_throttled",
                         "core.index.secondary_delta_merge.resource_governor_throttled",
                         "delta ledger scan budget exhausted before merge");
    }
    if (delta.cleanup_horizon_token.empty() || delta.local_transaction_id == 0) {
      return RefuseMerge(merge_ledger,
                         request,
                         "corrupt_ledger_refused",
                         "core.index.secondary_delta_merge.corrupt_ledger_refused",
                         "delta ledger record is missing cleanup horizon or transaction authority");
    }
    if (!delta.committed ||
        delta.local_transaction_id > request.authoritative_cleanup_horizon_local_transaction_id) {
      ++retained_count;
      continue;
    }
    ++eligible_count;
  }
  if (eligible_count > request.max_records_to_merge) {
    return RefuseMerge(merge_ledger,
                       request,
                       "resource_governor_throttled",
                       "core.index.secondary_delta_merge.resource_governor_throttled",
                       "eligible delta merge batch exceeds request budget");
  }

  u64 merged_count = 0;
  for (const auto& delta : delta_ledger->deltas) {
    if (!SameIndexTable(request.index_uuid, request.table_uuid, delta.index_uuid, delta.table_uuid)) {
      continue;
    }
    if (!delta.committed || delta.local_transaction_id > request.authoritative_cleanup_horizon_local_transaction_id) {
      continue;
    }
    switch (delta.delta_kind) {
      case SecondaryIndexDeltaKind::insert:
      case SecondaryIndexDeltaKind::update_after:
        if (AddBaseEntryIfMissing(base_entries, delta)) {
          ++merged_count;
        }
        break;
      case SecondaryIndexDeltaKind::delete_row:
      case SecondaryIndexDeltaKind::update_before:
        RemoveBaseEntry(base_entries, delta);
        ++merged_count;
        break;
    }
  }

  SecondaryIndexMergeResult result;
  result.status = MergeOkStatus();
  result.merged = true;
  result.merged_count = merged_count;
  result.retained_count = retained_count;
  result.evidence = BuildEvidence(merge_ledger,
                                  merge_id,
                                  request.index_uuid,
                                  request.table_uuid,
                                  merged_count,
                                  retained_count,
                                  0,
                                  scanned_count,
                                  request.max_records_to_scan,
                                  request.max_records_to_merge,
                                  request.authoritative_cleanup_horizon_local_transaction_id,
                                  SecondaryIndexMergeState::merged,
                                  "successful_merge",
                                  {},
                                  true);
  merge_ledger->ledger_generation = result.evidence.ledger_generation_after;
  result.ledger_generation = merge_ledger->ledger_generation;
  result.diagnostic = MakeSecondaryIndexMergeDiagnostic(result.status,
                                                       "ok",
                                                       "core.index.secondary_delta_merge.merged",
                                                       "secondary-index deltas merged idempotently");
  merge_ledger->evidence.push_back(result.evidence);
  return result;
}

SecondaryIndexCleanupResult CleanupSecondaryIndexDeltas(SecondaryIndexDeltaMergeLedger* merge_ledger,
                                                        SecondaryIndexDeltaLedger* delta_ledger,
                                                        const SecondaryIndexCleanupRequest& request) {
  if (merge_ledger == nullptr || delta_ledger == nullptr) {
    return RefuseCleanup(merge_ledger,
                         request,
                         "secondary_index_delta_cleanup_missing_ledger",
                         "core.index.secondary_delta_cleanup.missing_ledger",
                         "merge ledger and delta ledger are required",
                         false,
                         0);
  }
  if (!request.index_uuid.valid() || !request.table_uuid.valid()) {
    return RefuseCleanup(merge_ledger,
                         request,
                         "secondary_index_delta_cleanup_invalid_identity",
                         "core.index.secondary_delta_cleanup.invalid_identity",
                         "index_uuid and table_uuid must be valid engine UUIDs",
                         false,
                         0);
  }
  if (!request.cleanup_horizon_authoritative) {
    return RefuseCleanup(merge_ledger,
                         request,
                         "secondary_index_delta_cleanup_horizon_not_authoritative",
                         "core.index.secondary_delta_cleanup.horizon_not_authoritative",
                         "cleanup requires authoritative transaction horizon",
                         true,
                         delta_ledger->deltas.size());
  }

  u64 retained_count = 0;
  u64 cleaned_count = 0;
  std::vector<SecondaryIndexDeltaEntry> retained;
  retained.reserve(delta_ledger->deltas.size());
  for (const auto& delta : delta_ledger->deltas) {
    if (!SameIndexTable(request.index_uuid, request.table_uuid, delta.index_uuid, delta.table_uuid)) {
      retained.push_back(delta);
      continue;
    }
    if (!delta.committed || delta.local_transaction_id > request.authoritative_cleanup_horizon_local_transaction_id) {
      ++retained_count;
      retained.push_back(delta);
      continue;
    }
    ++cleaned_count;
  }

  if (retained_count > 0) {
    return RefuseCleanup(merge_ledger,
                         request,
                         "secondary_index_delta_cleanup_horizon_blocked",
                         "core.index.secondary_delta_cleanup.horizon_blocked",
                         "authoritative cleanup horizon still retains one or more deltas",
                         true,
                         retained_count);
  }

  delta_ledger->deltas = std::move(retained);
  SecondaryIndexCleanupResult result;
  result.status = MergeOkStatus();
  result.cleaned = true;
  result.cleaned_count = cleaned_count;
  result.retained_count = 0;
  result.evidence = BuildEvidence(merge_ledger,
                                  DefaultMergeId(merge_ledger),
                                  request.index_uuid,
                                  request.table_uuid,
                                  0,
                                  0,
                                  cleaned_count,
                                  cleaned_count + retained_count,
                                  0,
                                  0,
                                  request.authoritative_cleanup_horizon_local_transaction_id,
                                  SecondaryIndexMergeState::cleaned,
                                  "ok",
                                  {},
                                  true);
  merge_ledger->ledger_generation = result.evidence.ledger_generation_after;
  result.diagnostic = MakeSecondaryIndexMergeDiagnostic(result.status,
                                                       "ok",
                                                       "core.index.secondary_delta_cleanup.cleaned",
                                                       "secondary-index deltas cleaned after authoritative horizon");
  merge_ledger->evidence.push_back(result.evidence);
  return result;
}

SecondaryIndexMergeRecoveryResult ClassifySecondaryIndexMergeLedgerForRecovery(
    const SecondaryIndexDeltaMergeLedger& merge_ledger) {
  SecondaryIndexMergeRecoveryResult result;
  result.status = MergeOkStatus();
  result.diagnostic = MakeSecondaryIndexMergeDiagnostic(result.status,
                                                       "ok",
                                                       "core.index.secondary_delta_merge.recovery_classified",
                                                       "secondary-index merge ledger classified");
  result.classifications.reserve(merge_ledger.evidence.size());
  for (const auto& evidence : merge_ledger.evidence) {
    SecondaryIndexMergeRecoveryClassification classification;
    classification.evidence_id = evidence.evidence_id;
    classification.observed_state = evidence.new_state;
    switch (evidence.new_state) {
      case SecondaryIndexMergeState::merged:
        classification.action = SecondaryIndexMergeRecoveryAction::complete_idempotent_merge;
        classification.fail_closed = false;
        classification.stable_reason = "merge evidence can be retried idempotently";
        break;
      case SecondaryIndexMergeState::cleanup_horizon_blocked:
        classification.action = SecondaryIndexMergeRecoveryAction::retain_until_horizon;
        classification.fail_closed = false;
        classification.stable_reason = "delta cleanup remains blocked by authoritative horizon";
        break;
      case SecondaryIndexMergeState::cleaned:
      case SecondaryIndexMergeState::refused:
      case SecondaryIndexMergeState::absent:
        classification.action = SecondaryIndexMergeRecoveryAction::no_action;
        classification.fail_closed = false;
        classification.stable_reason = "state has no restart mutation";
        break;
      case SecondaryIndexMergeState::quarantine:
        classification.action = SecondaryIndexMergeRecoveryAction::fail_closed;
        classification.fail_closed = true;
        classification.stable_reason = "quarantined merge state blocks automatic restart";
        break;
    }
    result.classifications.push_back(classification);
  }
  return result;
}

DiagnosticRecord MakeSecondaryIndexMergeDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(status.code,
                                                     status.severity,
                                                     status.subsystem,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(arguments),
                                                     {},
                                                     "core.index.secondary_delta_merge",
                                                     status.ok() ? "" : "retain delta ledger and retry only after authoritative index cleanup authority is available");
}

}  // namespace scratchbird::core::index
