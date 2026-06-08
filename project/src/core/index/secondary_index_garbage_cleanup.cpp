// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT
#include "secondary_index_garbage_cleanup.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CleanupOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status CleanupErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
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

bool RelevantBase(const SecondaryIndexGarbageCleanupRequest& request,
                  const SecondaryIndexBaseEntry& entry) {
  return !entry.deleted &&
         SameIndexTable(request.index_uuid,
                        request.table_uuid,
                        entry.index_uuid,
                        entry.table_uuid);
}

bool RelevantTableSnapshot(const SecondaryIndexGarbageCleanupRequest& request,
                           const SecondaryIndexTableSnapshotEntry& entry) {
  return !entry.deleted &&
         SameIndexTable(request.index_uuid,
                        request.table_uuid,
                        entry.index_uuid,
                        entry.table_uuid);
}

bool RelevantDelta(const SecondaryIndexGarbageCleanupRequest& request,
                   const SecondaryIndexDeltaLedgerRecord& record) {
  return SameIndexTable(request.index_uuid,
                        request.table_uuid,
                        record.delta.index_uuid,
                        record.delta.table_uuid);
}

std::string UuidKey(const TypedUuid& value) {
  return std::to_string(static_cast<u32>(value.kind)) + ":" +
         scratchbird::core::uuid::UuidToString(value.value);
}

std::string EntryKey(const TypedUuid& row_uuid,
                     const TypedUuid& version_uuid,
                     const std::string& key_payload) {
  return UuidKey(row_uuid) + "|" + UuidKey(version_uuid) + "|" + key_payload;
}

std::string EntryKey(const SecondaryIndexBaseEntry& entry) {
  return EntryKey(entry.row_uuid, entry.version_uuid, entry.key_payload);
}

std::string EntryKey(const SecondaryIndexDeltaEntry& entry) {
  return EntryKey(entry.row_uuid, entry.version_uuid, entry.key_payload);
}

std::string EntryKey(const SecondaryIndexTableSnapshotEntry& entry) {
  return EntryKey(entry.row_uuid, entry.version_uuid, entry.key_payload);
}

void ApplyCommittedDelta(std::multiset<std::string>* effective,
                         const SecondaryIndexDeltaEntry& delta) {
  const std::string key = EntryKey(delta);
  switch (delta.delta_kind) {
    case SecondaryIndexDeltaKind::insert:
    case SecondaryIndexDeltaKind::update_after:
      effective->insert(key);
      break;
    case SecondaryIndexDeltaKind::delete_row:
    case SecondaryIndexDeltaKind::update_before: {
      const auto found = effective->find(key);
      if (found != effective->end()) {
        effective->erase(found);
      }
      break;
    }
  }
}

struct ValidationSnapshot {
  bool ok = false;
  u64 expected_entries = 0;
  u64 effective_entries = 0;
  std::string detail;
};

ValidationSnapshot ValidateEffectiveIndex(
    const SecondaryIndexGarbageCleanupRequest& request,
    const PersistentSecondaryIndexDeltaLedger& ledger) {
  std::multiset<std::string> expected;
  std::multiset<std::string> effective;
  for (const auto& row : request.table_snapshot) {
    if (RelevantTableSnapshot(request, row)) {
      expected.insert(EntryKey(row));
    }
  }
  for (const auto& base : request.base_entries) {
    if (RelevantBase(request, base)) {
      effective.insert(EntryKey(base));
    }
  }
  for (const auto& record : ledger.records) {
    if (!RelevantDelta(request, record) ||
        record.commit_state !=
            SecondaryIndexDeltaLedgerCommitState::committed_premerge ||
        !record.delta.committed) {
      continue;
    }
    ApplyCommittedDelta(&effective, record.delta);
  }

  ValidationSnapshot snapshot;
  snapshot.expected_entries = expected.size();
  snapshot.effective_entries = effective.size();
  snapshot.ok = expected == effective;
  if (!snapshot.ok) {
    snapshot.detail = "expected=" + std::to_string(snapshot.expected_entries) +
                      ";effective=" +
                      std::to_string(snapshot.effective_entries);
  }
  return snapshot;
}

SecondaryIndexGarbageCleanupMetrics Measure(
    const SecondaryIndexGarbageCleanupRequest& request,
    const PersistentSecondaryIndexDeltaLedger& ledger,
    const ValidationSnapshot& validation) {
  SecondaryIndexGarbageCleanupMetrics metrics;
  metrics.delta_ledger_records = ledger.records.size();
  metrics.validation_expected_entries = validation.expected_entries;
  metrics.validation_effective_entries = validation.effective_entries;
  for (const auto& row : request.table_snapshot) {
    if (RelevantTableSnapshot(request, row)) {
      ++metrics.table_snapshot_entries;
    }
  }
  for (const auto& base : request.base_entries) {
    if (RelevantBase(request, base)) {
      ++metrics.base_index_entries;
    }
  }
  for (const auto& record : ledger.records) {
    if (!RelevantDelta(request, record)) {
      continue;
    }
    ++metrics.relevant_delta_records;
    if (record.commit_state ==
        SecondaryIndexDeltaLedgerCommitState::merged_cleaned) {
      if (record.delta.committed &&
          record.delta.local_transaction_id <=
              request.authoritative_cleanup_horizon_local_transaction_id) {
        ++metrics.eligible_garbage_records;
      } else {
        ++metrics.retained_garbage_records;
      }
    } else {
      ++metrics.retained_unmerged_delta_records;
    }
  }
  return metrics;
}

SecondaryIndexGarbageCleanupResult Finish(
    SecondaryIndexGarbageCleanupResult result,
    SecondaryIndexGarbageCleanupDecisionKind decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    bool fail_closed) {
  result.status = fail_closed ? CleanupErrorStatus() : CleanupOkStatus();
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeSecondaryIndexGarbageCleanupDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

}  // namespace

const char* SecondaryIndexGarbageCleanupDecisionKindName(
    SecondaryIndexGarbageCleanupDecisionKind decision) {
  switch (decision) {
    case SecondaryIndexGarbageCleanupDecisionKind::success:
      return "success";
    case SecondaryIndexGarbageCleanupDecisionKind::no_op:
      return "no_op";
    case SecondaryIndexGarbageCleanupDecisionKind::budget_exhausted:
      return "budget_exhausted";
    case SecondaryIndexGarbageCleanupDecisionKind::horizon_blocked:
      return "horizon_blocked";
    case SecondaryIndexGarbageCleanupDecisionKind::validation_refused:
      return "validation_refused";
    case SecondaryIndexGarbageCleanupDecisionKind::refused_non_authoritative:
      return "refused_non_authoritative";
    case SecondaryIndexGarbageCleanupDecisionKind::refused:
      return "refused";
  }
  return "refused";
}

SecondaryIndexGarbageCleanupResult RunSecondaryIndexGarbageCleanupBatch(
    const SecondaryIndexGarbageCleanupRequest& request) {
  SecondaryIndexGarbageCleanupResult result;
  result.cleaned_ledger = request.ledger;
  result.bounded_batch =
      request.max_records_to_scan != 0 && request.max_records_to_clean != 0;

  if (!request.index_uuid.valid() || !request.table_uuid.valid()) {
    return Finish(std::move(result),
                  SecondaryIndexGarbageCleanupDecisionKind::refused,
                  "INDEX_GARBAGE_CLEANUP.INVALID_IDENTITY",
                  "core.index.garbage_cleanup.invalid_identity",
                  "index_uuid and table_uuid are required",
                  true);
  }
  if (request.index_kind == SecondaryIndexKind::unique) {
    return Finish(std::move(result),
                  SecondaryIndexGarbageCleanupDecisionKind::refused,
                  "INDEX_GARBAGE_CLEANUP.UNIQUE_INDEX_REFUSED",
                  "core.index.garbage_cleanup.unique_index_refused",
                  "unique secondary indexes remain synchronous",
                  true);
  }
  if (!request.cleanup_horizon_authoritative) {
    return Finish(std::move(result),
                  SecondaryIndexGarbageCleanupDecisionKind::refused_non_authoritative,
                  "INDEX_GARBAGE_CLEANUP.NON_AUTHORITATIVE_REFUSAL",
                  "core.index.garbage_cleanup.non_authoritative_refusal",
                  "cleanup requires authoritative MGA cleanup horizon",
                  true);
  }
  if (request.max_records_to_scan == 0 || request.max_records_to_clean == 0) {
    return Finish(std::move(result),
                  SecondaryIndexGarbageCleanupDecisionKind::refused,
                  "INDEX_GARBAGE_CLEANUP.BUDGET_REQUIRED",
                  "core.index.garbage_cleanup.budget_required",
                  "cleanup requires nonzero scan and clean budgets",
                  true);
  }

  const auto before_validation =
      ValidateEffectiveIndex(request, request.ledger);
  result.validation_before_ok = before_validation.ok;
  result.before = Measure(request, request.ledger, before_validation);
  if (!before_validation.ok) {
    result.after = result.before;
    return Finish(std::move(result),
                  SecondaryIndexGarbageCleanupDecisionKind::validation_refused,
                  "INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "core.index.garbage_cleanup.validation_refused",
                  before_validation.detail,
                  true);
  }

  if (result.before.eligible_garbage_records == 0) {
    if (result.before.retained_garbage_records != 0) {
      result.horizon_blocked = true;
      result.after = result.before;
      result.validation_after_ok = true;
      return Finish(std::move(result),
                    SecondaryIndexGarbageCleanupDecisionKind::horizon_blocked,
                    "INDEX_GARBAGE_CLEANUP.HORIZON_BLOCKED",
                    "core.index.garbage_cleanup.horizon_blocked",
                    std::to_string(result.before.retained_garbage_records),
                    false);
    }
    result.after = result.before;
    result.validation_after_ok = true;
    return Finish(std::move(result),
                  SecondaryIndexGarbageCleanupDecisionKind::no_op,
                  "INDEX_GARBAGE_CLEANUP.NO_OP",
                  "core.index.garbage_cleanup.no_op",
                  "no merged-cleaned index garbage is below the cleanup horizon",
                  false);
  }

  std::vector<SecondaryIndexDeltaLedgerRecord> retained_records;
  retained_records.reserve(request.ledger.records.size());
  u64 relevant_seen = 0;
  u64 cleaned = 0;
  bool budget_exhausted = false;
  for (std::size_t i = 0; i < request.ledger.records.size(); ++i) {
    const auto& record = request.ledger.records[i];
    if (!RelevantDelta(request, record)) {
      retained_records.push_back(record);
      continue;
    }
    ++relevant_seen;
    if (relevant_seen > request.max_records_to_scan) {
      budget_exhausted = true;
      retained_records.push_back(record);
      retained_records.insert(retained_records.end(),
                              std::next(request.ledger.records.begin(),
                                        static_cast<long>(i + 1)),
                              request.ledger.records.end());
      break;
    }
    result.before.scanned_delta_records = relevant_seen;
    const bool eligible =
        record.commit_state ==
            SecondaryIndexDeltaLedgerCommitState::merged_cleaned &&
        record.delta.committed &&
        record.delta.local_transaction_id <=
            request.authoritative_cleanup_horizon_local_transaction_id;
    if (!eligible) {
      retained_records.push_back(record);
      continue;
    }
    if (cleaned >= request.max_records_to_clean) {
      budget_exhausted = true;
      retained_records.push_back(record);
      retained_records.insert(retained_records.end(),
                              std::next(request.ledger.records.begin(),
                                        static_cast<long>(i + 1)),
                              request.ledger.records.end());
      break;
    }
    ++cleaned;
  }

  result.cleaned_ledger.records = std::move(retained_records);
  const auto after_validation =
      ValidateEffectiveIndex(request, result.cleaned_ledger);
  result.validation_after_ok = after_validation.ok;
  result.after = Measure(request, result.cleaned_ledger, after_validation);
  result.after.scanned_delta_records = result.before.scanned_delta_records;
  result.after.cleaned_garbage_records = cleaned;
  if (!after_validation.ok) {
    result.cleaned_ledger = request.ledger;
    return Finish(std::move(result),
                  SecondaryIndexGarbageCleanupDecisionKind::validation_refused,
                  "INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "core.index.garbage_cleanup.validation_refused",
                  after_validation.detail,
                  true);
  }
  result.budget_exhausted = budget_exhausted;
  if (budget_exhausted) {
    return Finish(std::move(result),
                  SecondaryIndexGarbageCleanupDecisionKind::budget_exhausted,
                  "INDEX_GARBAGE_CLEANUP.BUDGET_EXHAUSTED",
                  "core.index.garbage_cleanup.budget_exhausted",
                  std::to_string(cleaned),
                  false);
  }
  return Finish(std::move(result),
                SecondaryIndexGarbageCleanupDecisionKind::success,
                "INDEX_GARBAGE_CLEANUP.SUCCESS",
                "core.index.garbage_cleanup.success",
                std::to_string(cleaned),
                false);
}

DiagnosticRecord MakeSecondaryIndexGarbageCleanupDiagnostic(
    Status status,
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
                        "core.index.secondary_index_garbage_cleanup",
                        status.ok() ? "" : "retain index ledger and retry only after authoritative validation succeeds");
}

}  // namespace scratchbird::core::index
