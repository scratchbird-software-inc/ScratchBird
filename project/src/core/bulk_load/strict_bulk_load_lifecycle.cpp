// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-STRICT-BULK-LOAD-LIFECYCLE-ANCHOR
#include "strict_bulk_load_lifecycle.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace scratchbird::core::bulk_load {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status BulkLoadOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status BulkLoadErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

StrictBulkLoadOperation* FindMutableOperation(StrictBulkLoadLedger* ledger, const TypedUuid& bulk_load_id) {
  if (ledger == nullptr) {
    return nullptr;
  }
  const auto found = std::find_if(ledger->operations.begin(),
                                  ledger->operations.end(),
                                  [&](const StrictBulkLoadOperation& operation) {
                                    return SameUuid(operation.bulk_load_id, bulk_load_id);
                                  });
  return found == ledger->operations.end() ? nullptr : &(*found);
}

bool OwnTransaction(const StrictBulkLoadOperation& operation,
                    const TypedUuid& transaction_uuid,
                    u64 local_transaction_id) {
  return SameUuid(operation.transaction_uuid, transaction_uuid) &&
         operation.local_transaction_id == local_transaction_id;
}

bool RowValidForPolicy(const StrictBulkLoadOperation& operation, const StrictBulkLoadRow& row) {
  if (!row.row_uuid.valid() || row.encoded_row.empty()) {
    return false;
  }
  if (operation.policy.require_all_constraints_valid && !row.constraints_valid) {
    return false;
  }
  if (operation.policy.require_all_indexes_valid && !row.indexes_valid) {
    return false;
  }
  if (operation.policy.require_all_domains_valid && !row.domains_valid) {
    return false;
  }
  if (operation.policy.require_all_policy_gates_valid && !row.policy_gates_valid) {
    return false;
  }
  return true;
}

bool CopyBatchMetricStatusTerminal(CopyBatchMetricStatus status) {
  return status == CopyBatchMetricStatus::accepted ||
         status == CopyBatchMetricStatus::refused ||
         status == CopyBatchMetricStatus::completed;
}

CopyBatchMetricTiming NormalizeTiming(CopyBatchMetricTiming timing) {
  if (timing.elapsed_nanos != 0) {
    timing.recorded = true;
  }
  return timing;
}

u64 SaturatingAdd(u64 left, u64 right) {
  const auto max = std::numeric_limits<u64>::max();
  return max - left < right ? max : left + right;
}

void AddTiming(u64* total, const CopyBatchMetricTiming& timing) {
  if (total != nullptr && timing.recorded) {
    *total = SaturatingAdd(*total, timing.elapsed_nanos);
  }
}

void AddStatusCount(CopyBatchMetricStatus status,
                    u64* accepted_count,
                    u64* refused_count,
                    u64* completed_count) {
  if (status == CopyBatchMetricStatus::accepted && accepted_count != nullptr) {
    *accepted_count = SaturatingAdd(*accepted_count, 1);
  } else if (status == CopyBatchMetricStatus::refused && refused_count != nullptr) {
    *refused_count = SaturatingAdd(*refused_count, 1);
  } else if (status == CopyBatchMetricStatus::completed && completed_count != nullptr) {
    *completed_count = SaturatingAdd(*completed_count, 1);
  }
}

u64 CeilDiv(u64 numerator, u64 denominator) {
  if (denominator == 0 || numerator == 0) {
    return 0;
  }
  return 1 + ((numerator - 1) / denominator);
}

u64 EstimateDemandPages(const DmlPageFilespaceDemandHintRequest& request) {
  if (request.batch_row_count == 0 || request.estimated_row_bytes == 0 || request.page_size_bytes == 0) {
    return 0;
  }
  if (request.batch_row_count > std::numeric_limits<u64>::max() / request.estimated_row_bytes) {
    return request.max_preallocation_pages;
  }
  return CeilDiv(request.batch_row_count * request.estimated_row_bytes, request.page_size_bytes);
}

StrictBulkLoadEvidenceRecord BuildEvidence(StrictBulkLoadLedger* ledger,
                                           const StrictBulkLoadOperation& operation,
                                           std::string action,
                                           StrictBulkLoadState previous_state,
                                           StrictBulkLoadState new_state,
                                           std::string diagnostic_code,
                                           std::string reason,
                                           bool durable_state_changed) {
  StrictBulkLoadEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.action = std::move(action);
  evidence.evidence_id = GeneratedId(UuidKind::object, evidence.sequence);
  evidence.bulk_load_id = operation.bulk_load_id;
  evidence.database_uuid = operation.database_uuid;
  evidence.object_uuid = operation.object_uuid;
  evidence.transaction_uuid = operation.transaction_uuid;
  evidence.policy_uuid = operation.policy.policy_uuid;
  evidence.local_transaction_id = operation.local_transaction_id;
  evidence.staged_row_count = operation.staged_rows.size();
  evidence.visible_row_count = operation.visible_rows.size();
  evidence.index_closeout_count = operation.index_closeout_count;
  evidence.previous_state = previous_state;
  evidence.new_state = new_state;
  evidence.staging_target = operation.staging_target;
  evidence.visibility_fence = operation.visibility_fence;
  evidence.reason = std::move(reason);
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.durable_state_changed = durable_state_changed;
  return evidence;
}

StrictBulkLoadBeginResult RefuseBegin(StrictBulkLoadLedger* ledger,
                                      const StrictBulkLoadBeginRequest& request,
                                      std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail) {
  StrictBulkLoadBeginResult result;
  result.status = BulkLoadErrorStatus();
  StrictBulkLoadOperation operation;
  operation.database_uuid = request.database_uuid;
  operation.object_uuid = request.object_uuid;
  operation.transaction_uuid = request.transaction_uuid;
  operation.local_transaction_id = request.local_transaction_id;
  operation.policy = request.policy;
  operation.state = StrictBulkLoadState::refused;
  operation.staging_target = request.staging_target;
  result.evidence = BuildEvidence(ledger,
                                  operation,
                                  "refuse_strict_bulk_load_begin",
                                  StrictBulkLoadState::absent,
                                  StrictBulkLoadState::refused,
                                  diagnostic_code,
                                  detail,
                                  false);
  result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  if (ledger != nullptr) {
    ledger->evidence.push_back(result.evidence);
  }
  return result;
}

}  // namespace

const char* StrictBulkLoadStateName(StrictBulkLoadState state) {
  switch (state) {
    case StrictBulkLoadState::absent:
      return "absent";
    case StrictBulkLoadState::begun:
      return "begun";
    case StrictBulkLoadState::appending:
      return "appending";
    case StrictBulkLoadState::finalize_evidence_durable:
      return "finalize_evidence_durable";
    case StrictBulkLoadState::published_visible:
      return "published_visible";
    case StrictBulkLoadState::rolled_back:
      return "rolled_back";
    case StrictBulkLoadState::recovery_required:
      return "recovery_required";
    case StrictBulkLoadState::refused:
      return "refused";
    case StrictBulkLoadState::quarantine:
      return "quarantine";
  }
  return "unknown";
}

const char* StrictBulkLoadRecoveryActionName(StrictBulkLoadRecoveryAction action) {
  switch (action) {
    case StrictBulkLoadRecoveryAction::no_action:
      return "no_action";
    case StrictBulkLoadRecoveryAction::complete_publication:
      return "complete_publication";
    case StrictBulkLoadRecoveryAction::roll_back_staging:
      return "roll_back_staging";
    case StrictBulkLoadRecoveryAction::operator_review:
      return "operator_review";
    case StrictBulkLoadRecoveryAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

const char* CopyBatchMetricStatusName(CopyBatchMetricStatus status) {
  switch (status) {
    case CopyBatchMetricStatus::unknown:
      return "unknown";
    case CopyBatchMetricStatus::accepted:
      return "accepted";
    case CopyBatchMetricStatus::refused:
      return "refused";
    case CopyBatchMetricStatus::completed:
      return "completed";
  }
  return "unknown";
}

const char* DmlPageFilespaceDemandHintDecisionName(DmlPageFilespaceDemandHintDecision decision) {
  switch (decision) {
    case DmlPageFilespaceDemandHintDecision::disabled:
      return "disabled";
    case DmlPageFilespaceDemandHintDecision::accepted:
      return "accepted";
    case DmlPageFilespaceDemandHintDecision::capped:
      return "capped";
    case DmlPageFilespaceDemandHintDecision::refused:
      return "refused";
  }
  return "unknown";
}

StrictBulkLoadBeginResult BeginStrictBulkLoad(StrictBulkLoadLedger* ledger,
                                              const StrictBulkLoadBeginRequest& request) {
  if (ledger == nullptr) {
    return RefuseBegin(nullptr,
                       request,
                       "strict_bulk_load_missing_ledger",
                       "core.bulk_load.strict.missing_ledger",
                       "strict bulk-load ledger is required");
  }
  if (!request.database_uuid.valid() || !request.object_uuid.valid() || !request.transaction_uuid.valid()) {
    return RefuseBegin(ledger,
                       request,
                       "strict_bulk_load_invalid_identity",
                       "core.bulk_load.strict.invalid_identity",
                       "database_uuid, object_uuid, and transaction_uuid must be valid engine UUIDs");
  }
  if (request.local_transaction_id == 0) {
    return RefuseBegin(ledger,
                       request,
                       "strict_bulk_load_invalid_transaction",
                       "core.bulk_load.strict.invalid_transaction",
                       "local_transaction_id must be non-zero");
  }
  if (!request.policy.enabled) {
    return RefuseBegin(ledger,
                       request,
                       "strict_bulk_load_policy_not_enabled",
                       "core.bulk_load.strict.policy_not_enabled",
                       "strict bulk-load policy is disabled");
  }
  if (request.donor_relaxed_semantics_requested &&
      !(request.policy.allow_donor_relaxed_semantics && request.policy.map_donor_relaxed_to_native_safe)) {
    return RefuseBegin(ledger,
                       request,
                       "strict_bulk_load_donor_relaxed_refused",
                       "core.bulk_load.strict.donor_relaxed_refused",
                       "donor relaxed semantics are refused unless policy maps them to native safe behavior");
  }
  if (request.staging_target.empty()) {
    return RefuseBegin(ledger,
                       request,
                       "strict_bulk_load_missing_staging_target",
                       "core.bulk_load.strict.missing_staging_target",
                       "staging target is required");
  }

  StrictBulkLoadOperation operation;
  operation.bulk_load_id = GeneratedId(UuidKind::object, 200000 + ledger->next_evidence_sequence);
  operation.database_uuid = request.database_uuid;
  operation.object_uuid = request.object_uuid;
  operation.transaction_uuid = request.transaction_uuid;
  operation.local_transaction_id = request.local_transaction_id;
  operation.policy = request.policy;
  operation.state = StrictBulkLoadState::begun;
  operation.staging_target = request.staging_target;

  StrictBulkLoadBeginResult result;
  result.status = BulkLoadOkStatus();
  result.begun = true;
  result.operation = operation;
  result.evidence = BuildEvidence(ledger,
                                  operation,
                                  "begin_strict_bulk_load",
                                  StrictBulkLoadState::absent,
                                  StrictBulkLoadState::begun,
                                  "ok",
                                  "strict bulk-load begun",
                                  true);
  result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                  "ok",
                                                  "core.bulk_load.strict.begun",
                                                  "strict bulk-load begun");
  ledger->operations.push_back(operation);
  ledger->evidence.push_back(result.evidence);
  return result;
}

StrictBulkLoadAppendResult AppendStrictBulkLoadRows(StrictBulkLoadLedger* ledger,
                                                    const StrictBulkLoadAppendRequest& request) {
  StrictBulkLoadAppendResult result;
  auto* operation = FindMutableOperation(ledger, request.bulk_load_id);
  if (operation == nullptr || !OwnTransaction(*operation, request.transaction_uuid, request.local_transaction_id) ||
      (operation->state != StrictBulkLoadState::begun && operation->state != StrictBulkLoadState::appending)) {
    result.status = BulkLoadErrorStatus();
    result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                    "strict_bulk_load_append_refused",
                                                    "core.bulk_load.strict.append_refused",
                                                    "bulk load is not append-eligible");
    return result;
  }
  if (request.rows.empty()) {
    result.status = BulkLoadErrorStatus();
    result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                    "strict_bulk_load_append_empty",
                                                    "core.bulk_load.strict.append_empty",
                                                    "append requires at least one row");
    return result;
  }
  for (const auto& row : request.rows) {
    if (!RowValidForPolicy(*operation, row)) {
      const auto previous = operation->state;
      operation->state = StrictBulkLoadState::rolled_back;
      operation->staged_rows.clear();
      operation->visible_rows.clear();
      result.status = BulkLoadErrorStatus();
      result.evidence = BuildEvidence(ledger,
                                      *operation,
                                      "rollback_strict_bulk_load_append_validation",
                                      previous,
                                      StrictBulkLoadState::rolled_back,
                                      "strict_bulk_load_append_validation_failed",
                                      "strict bulk-load row validation failed",
                                      true);
      result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                      "strict_bulk_load_append_validation_failed",
                                                      "core.bulk_load.strict.append_validation_failed",
                                                      "row failed constraint/domain/index/policy validation");
      ledger->evidence.push_back(result.evidence);
      return result;
    }
  }

  const auto previous = operation->state;
  operation->state = StrictBulkLoadState::appending;
  operation->staged_rows.insert(operation->staged_rows.end(), request.rows.begin(), request.rows.end());
  result.status = BulkLoadOkStatus();
  result.appended = true;
  result.staged_row_count = operation->staged_rows.size();
  result.evidence = BuildEvidence(ledger,
                                  *operation,
                                  "append_strict_bulk_load_rows",
                                  previous,
                                  StrictBulkLoadState::appending,
                                  "ok",
                                  "strict bulk-load rows staged",
                                  true);
  result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                  "ok",
                                                  "core.bulk_load.strict.appended",
                                                  "strict bulk-load rows staged without visibility publication");
  ledger->evidence.push_back(result.evidence);
  return result;
}

StrictBulkLoadFinalizeEvidenceResult FinalizeStrictBulkLoadEvidenceDurable(
    StrictBulkLoadLedger* ledger,
    const StrictBulkLoadFinalizeRequest& request) {
  StrictBulkLoadFinalizeEvidenceResult result;
  auto* operation = FindMutableOperation(ledger, request.bulk_load_id);
  if (operation == nullptr || !OwnTransaction(*operation, request.transaction_uuid, request.local_transaction_id) ||
      operation->state != StrictBulkLoadState::appending || operation->staged_rows.empty()) {
    result.status = BulkLoadErrorStatus();
    result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                    "strict_bulk_load_finalize_refused",
                                                    "core.bulk_load.strict.finalize_refused",
                                                    "bulk load is not finalize-eligible");
    return result;
  }
  for (const auto& row : operation->staged_rows) {
    if (!RowValidForPolicy(*operation, row)) {
      const auto previous = operation->state;
      operation->state = StrictBulkLoadState::rolled_back;
      operation->staged_rows.clear();
      operation->visible_rows.clear();
      result.status = BulkLoadErrorStatus();
      result.evidence = BuildEvidence(ledger,
                                      *operation,
                                      "rollback_strict_bulk_load_finalize_validation",
                                      previous,
                                      StrictBulkLoadState::rolled_back,
                                      "strict_bulk_load_finalize_validation_failed",
                                      "strict bulk-load validation failed before visibility publication",
                                      true);
      result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                      "strict_bulk_load_finalize_validation_failed",
                                                      "core.bulk_load.strict.finalize_validation_failed",
                                                      "staged rows failed final validation");
      ledger->evidence.push_back(result.evidence);
      return result;
    }
  }

  const auto previous = operation->state;
  operation->state = StrictBulkLoadState::finalize_evidence_durable;
  operation->visibility_fence = request.visibility_fence.empty() ? "strict-bulk-load-visibility-fence"
                                                                 : request.visibility_fence;
  operation->index_closeout_count = operation->staged_rows.size();
  result.status = BulkLoadOkStatus();
  result.finalized = true;
  result.staged_row_count = operation->staged_rows.size();
  result.index_closeout_count = operation->index_closeout_count;
  result.evidence = BuildEvidence(ledger,
                                  *operation,
                                  "finalize_strict_bulk_load_evidence",
                                  previous,
                                  StrictBulkLoadState::finalize_evidence_durable,
                                  "ok",
                                  "strict bulk-load finalize evidence durable before visibility publication",
                                  true);
  ledger->evidence.push_back(result.evidence);

  if (request.simulate_finalize_failure_after_evidence) {
    operation->state = StrictBulkLoadState::recovery_required;
    operation->visible_rows.clear();
    result.status = BulkLoadErrorStatus();
    result.finalized = false;
    result.recovery_required = true;
    result.evidence = BuildEvidence(ledger,
                                    *operation,
                                    "require_strict_bulk_load_recovery",
                                    StrictBulkLoadState::finalize_evidence_durable,
                                    StrictBulkLoadState::recovery_required,
                                    "strict_bulk_load_publish_recovery",
                                    "finalize evidence is durable but publication did not complete",
                                    true);
    result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                    "strict_bulk_load_publish_recovery",
                                                    "core.bulk_load.strict.publish_recovery",
                                                    "finalize evidence is durable but publication did not complete");
    ledger->evidence.push_back(result.evidence);
    return result;
  }

  result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                  "ok",
                                                  "core.bulk_load.strict.finalize_evidence_durable",
                                                  "strict bulk-load finalize evidence is durable");
  return result;
}

StrictBulkLoadPublishResult PublishStrictBulkLoadVisible(
    StrictBulkLoadLedger* ledger,
    const StrictBulkLoadPublishRequest& request) {
  StrictBulkLoadPublishResult result;
  auto* operation = FindMutableOperation(ledger, request.bulk_load_id);
  if (operation == nullptr || !OwnTransaction(*operation, request.transaction_uuid, request.local_transaction_id) ||
      operation->state != StrictBulkLoadState::finalize_evidence_durable || operation->staged_rows.empty()) {
    result.status = BulkLoadErrorStatus();
    result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                    "strict_bulk_load_publish_refused",
                                                    "core.bulk_load.strict.publish_refused",
                                                    "bulk load is not publish-eligible");
    return result;
  }
  if (!request.visibility_fence.empty()) {
    operation->visibility_fence = request.visibility_fence;
  }
  operation->visible_rows = operation->staged_rows;
  operation->staged_rows.clear();
  operation->state = StrictBulkLoadState::published_visible;
  result.status = BulkLoadOkStatus();
  result.published_visible = true;
  result.visible_row_count = operation->visible_rows.size();
  result.index_closeout_count = operation->index_closeout_count;
  result.evidence = BuildEvidence(ledger,
                                  *operation,
                                  "publish_strict_bulk_load",
                                  StrictBulkLoadState::finalize_evidence_durable,
                                  StrictBulkLoadState::published_visible,
                                  "ok",
                                  "strict bulk-load rows and indexes published atomically",
                                  true);
  result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                  "ok",
                                                  "core.bulk_load.strict.finalized",
                                                  "strict bulk-load finalized and published visible");
  ledger->evidence.push_back(result.evidence);
  return result;
}

StrictBulkLoadFinalizeResult FinalizeStrictBulkLoad(StrictBulkLoadLedger* ledger,
                                                    const StrictBulkLoadFinalizeRequest& request) {
  StrictBulkLoadFinalizeResult result;
  const auto finalized = FinalizeStrictBulkLoadEvidenceDurable(ledger, request);
  result.status = finalized.status;
  result.finalized = finalized.finalized;
  result.recovery_required = finalized.recovery_required;
  result.index_closeout_count = finalized.index_closeout_count;
  result.evidence = finalized.evidence;
  result.diagnostic = finalized.diagnostic;
  if (!finalized.ok()) {
    return result;
  }
  const auto published = PublishStrictBulkLoadVisible(
      ledger,
      StrictBulkLoadPublishRequest{
          request.bulk_load_id,
          request.transaction_uuid,
          request.local_transaction_id,
          request.visibility_fence});
  result.status = published.status;
  result.finalized = published.ok();
  result.published_visible = published.published_visible;
  result.visible_row_count = published.visible_row_count;
  result.index_closeout_count = published.index_closeout_count;
  result.evidence = published.evidence;
  result.diagnostic = published.diagnostic;
  return result;
}

StrictBulkLoadRollbackResult RollbackStrictBulkLoad(StrictBulkLoadLedger* ledger,
                                                    const StrictBulkLoadRollbackRequest& request) {
  StrictBulkLoadRollbackResult result;
  auto* operation = FindMutableOperation(ledger, request.bulk_load_id);
  if (operation == nullptr || !OwnTransaction(*operation, request.transaction_uuid, request.local_transaction_id)) {
    result.status = BulkLoadErrorStatus();
    result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                    "strict_bulk_load_rollback_refused",
                                                    "core.bulk_load.strict.rollback_refused",
                                                    "bulk load is not rollback-eligible");
    return result;
  }
  const auto previous = operation->state;
  operation->state = StrictBulkLoadState::rolled_back;
  operation->staged_rows.clear();
  operation->visible_rows.clear();
  result.status = BulkLoadOkStatus();
  result.rolled_back = true;
  result.evidence = BuildEvidence(ledger,
                                  *operation,
                                  "rollback_strict_bulk_load",
                                  previous,
                                  StrictBulkLoadState::rolled_back,
                                  "ok",
                                  request.reason,
                                  true);
  result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                  "ok",
                                                  "core.bulk_load.strict.rolled_back",
                                                  "strict bulk-load rolled back idempotently");
  ledger->evidence.push_back(result.evidence);
  return result;
}

StrictBulkLoadQuarantineResult QuarantineStrictBulkLoad(
    StrictBulkLoadLedger* ledger,
    const StrictBulkLoadQuarantineRequest& request) {
  StrictBulkLoadQuarantineResult result;
  auto* operation = FindMutableOperation(ledger, request.bulk_load_id);
  if (operation == nullptr || !OwnTransaction(*operation, request.transaction_uuid, request.local_transaction_id)) {
    result.status = BulkLoadErrorStatus();
    result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                    "strict_bulk_load_quarantine_refused",
                                                    "core.bulk_load.strict.quarantine_refused",
                                                    "bulk load is not quarantine-eligible");
    return result;
  }
  const auto previous = operation->state;
  operation->state = StrictBulkLoadState::quarantine;
  operation->visible_rows.clear();
  result.status = BulkLoadOkStatus();
  result.quarantined = true;
  result.evidence = BuildEvidence(ledger,
                                  *operation,
                                  "quarantine_strict_bulk_load",
                                  previous,
                                  StrictBulkLoadState::quarantine,
                                  "ok",
                                  request.reason,
                                  true);
  result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                  "ok",
                                                  "core.bulk_load.strict.quarantined",
                                                  "strict bulk-load quarantined for operator review");
  ledger->evidence.push_back(result.evidence);
  return result;
}

StrictBulkLoadRecoveryResult ClassifyStrictBulkLoadLedgerForRecovery(const StrictBulkLoadLedger& ledger) {
  StrictBulkLoadRecoveryResult result;
  result.status = BulkLoadOkStatus();
  result.diagnostic = MakeStrictBulkLoadDiagnostic(result.status,
                                                  "ok",
                                                  "core.bulk_load.strict.recovery_classified",
                                                  "strict bulk-load ledger classified");
  result.classifications.reserve(ledger.operations.size());
  for (const auto& operation : ledger.operations) {
    StrictBulkLoadRecoveryClassification classification;
    classification.bulk_load_id = operation.bulk_load_id;
    classification.observed_state = operation.state;
    switch (operation.state) {
      case StrictBulkLoadState::begun:
      case StrictBulkLoadState::appending:
        classification.action = StrictBulkLoadRecoveryAction::roll_back_staging;
        classification.fail_closed = false;
        classification.stable_reason = "non-final strict bulk-load staging rolls back during recovery";
        break;
      case StrictBulkLoadState::finalize_evidence_durable:
      case StrictBulkLoadState::recovery_required:
        classification.action = StrictBulkLoadRecoveryAction::complete_publication;
        classification.fail_closed = false;
        classification.stable_reason = "finalize evidence is durable and publication must complete idempotently or be operator-resolved";
        break;
      case StrictBulkLoadState::published_visible:
      case StrictBulkLoadState::rolled_back:
      case StrictBulkLoadState::refused:
      case StrictBulkLoadState::absent:
        classification.action = StrictBulkLoadRecoveryAction::no_action;
        classification.fail_closed = false;
        classification.stable_reason = "terminal state has no restart mutation";
        break;
      case StrictBulkLoadState::quarantine:
        classification.action = StrictBulkLoadRecoveryAction::fail_closed;
        classification.fail_closed = true;
        classification.stable_reason = "quarantined strict bulk-load blocks automatic restart";
        break;
    }
    result.classifications.push_back(classification);
  }
  return result;
}

const StrictBulkLoadOperation* FindStrictBulkLoadOperation(const StrictBulkLoadLedger& ledger,
                                                          const TypedUuid& bulk_load_id) {
  const auto found = std::find_if(ledger.operations.begin(),
                                  ledger.operations.end(),
                                  [&](const StrictBulkLoadOperation& operation) {
                                    return SameUuid(operation.bulk_load_id, bulk_load_id);
                                  });
  return found == ledger.operations.end() ? nullptr : &(*found);
}

CopyBatchMetricResult MakeCopyBatchMetricRecord(const CopyBatchMetricRequest& request) {
  CopyBatchMetricResult result;
  if (request.batch_sequence == 0) {
    result.status = BulkLoadErrorStatus();
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "copy_batch_metric_invalid_sequence",
        "core.bulk_load.copy_batch_metric.invalid_sequence",
        "batch_sequence must be non-zero");
    return result;
  }
  if (!CopyBatchMetricStatusTerminal(request.message_status)) {
    result.status = BulkLoadErrorStatus();
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "copy_batch_metric_invalid_message_status",
        "core.bulk_load.copy_batch_metric.invalid_message_status",
        CopyBatchMetricStatusName(request.message_status));
    return result;
  }
  if (!CopyBatchMetricStatusTerminal(request.result_status)) {
    result.status = BulkLoadErrorStatus();
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "copy_batch_metric_invalid_result_status",
        "core.bulk_load.copy_batch_metric.invalid_result_status",
        CopyBatchMetricStatusName(request.result_status));
    return result;
  }

  result.status = BulkLoadOkStatus();
  result.accepted = true;
  result.record.batch_sequence = request.batch_sequence;
  result.record.batch_row_count = request.batch_row_count;
  result.record.parse_timing = NormalizeTiming(request.parse_timing);
  result.record.bind_timing = NormalizeTiming(request.bind_timing);
  result.record.append_timing = NormalizeTiming(request.append_timing);
  result.record.page_timing = NormalizeTiming(request.page_timing);
  result.record.index_timing = NormalizeTiming(request.index_timing);
  result.record.finality_timing = NormalizeTiming(request.finality_timing);
  result.record.message_status = request.message_status;
  result.record.result_status = request.result_status;
  result.record.message_status_detail = request.message_status_detail;
  result.record.result_status_detail = request.result_status_detail;
  result.diagnostic = MakeStrictBulkLoadDiagnostic(
      result.status,
      "ok",
      "core.bulk_load.copy_batch_metric.accepted",
      "COPY batch metric accepted without mutating bulk-load state");
  return result;
}

CopyBatchMetricResult RecordCopyBatchMetric(CopyBatchMetricLedger* ledger,
                                            const CopyBatchMetricRequest& request) {
  auto result = MakeCopyBatchMetricRecord(request);
  if (!result.ok()) {
    return result;
  }
  if (ledger == nullptr) {
    result.status = BulkLoadErrorStatus();
    result.accepted = false;
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "copy_batch_metric_missing_ledger",
        "core.bulk_load.copy_batch_metric.missing_ledger",
        "COPY batch metric ledger is required");
    return result;
  }

  ledger->records.push_back(result.record);
  auto& totals = ledger->totals;
  totals.metric_record_count = SaturatingAdd(totals.metric_record_count, 1);
  totals.total_batch_rows = SaturatingAdd(totals.total_batch_rows, result.record.batch_row_count);
  AddStatusCount(result.record.message_status,
                 &totals.accepted_message_count,
                 &totals.refused_message_count,
                 nullptr);
  AddStatusCount(result.record.result_status,
                 &totals.accepted_result_count,
                 &totals.refused_result_count,
                 &totals.completed_result_count);
  AddTiming(&totals.parse_nanos, result.record.parse_timing);
  AddTiming(&totals.bind_nanos, result.record.bind_timing);
  AddTiming(&totals.append_nanos, result.record.append_timing);
  AddTiming(&totals.page_nanos, result.record.page_timing);
  AddTiming(&totals.index_nanos, result.record.index_timing);
  AddTiming(&totals.finality_nanos, result.record.finality_timing);
  return result;
}

DmlPageFilespaceDemandHintResult MakeDmlPageFilespaceDemandHint(
    const DmlPageFilespaceDemandHintRequest& request) {
  DmlPageFilespaceDemandHintResult result;
  result.record.database_uuid = request.database_uuid;
  result.record.object_uuid = request.object_uuid;
  result.record.filespace_uuid = request.filespace_uuid;
  result.record.transaction_uuid = request.transaction_uuid;
  result.record.local_transaction_id = request.local_transaction_id;
  result.record.batch_sequence = request.batch_sequence;
  result.record.batch_row_count = request.batch_row_count;
  result.record.max_preallocation_pages = request.max_preallocation_pages;
  result.record.source = request.source;

  if (!request.enabled) {
    result.status = BulkLoadOkStatus();
    result.record.decision = DmlPageFilespaceDemandHintDecision::disabled;
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "ok",
        "core.bulk_load.dml_demand_hint.disabled",
        "DML page/filespace demand hints are disabled");
    return result;
  }
  if (!request.database_uuid.valid() || !request.object_uuid.valid() ||
      !request.filespace_uuid.valid() || !request.transaction_uuid.valid()) {
    result.status = BulkLoadErrorStatus();
    result.record.decision = DmlPageFilespaceDemandHintDecision::refused;
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "dml_demand_hint_invalid_identity",
        "core.bulk_load.dml_demand_hint.invalid_identity",
        "database_uuid, object_uuid, filespace_uuid, and transaction_uuid must be valid engine UUIDs");
    return result;
  }
  if (request.database_uuid.kind != UuidKind::database ||
      request.object_uuid.kind != UuidKind::object ||
      request.transaction_uuid.kind != UuidKind::transaction) {
    result.status = BulkLoadErrorStatus();
    result.record.decision = DmlPageFilespaceDemandHintDecision::refused;
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "dml_demand_hint_invalid_identity_kind",
        "core.bulk_load.dml_demand_hint.invalid_identity_kind",
        "DML demand hints require database, object, filespace, and transaction UUID kinds");
    return result;
  }
  if (request.filespace_uuid.kind != UuidKind::filespace) {
    result.status = BulkLoadErrorStatus();
    result.record.decision = DmlPageFilespaceDemandHintDecision::refused;
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "dml_demand_hint_invalid_filespace_identity",
        "core.bulk_load.dml_demand_hint.invalid_filespace_identity",
        "filespace_uuid must carry filespace UUID kind");
    return result;
  }
  if (request.local_transaction_id == 0 || request.batch_sequence == 0 || request.batch_row_count == 0) {
    result.status = BulkLoadErrorStatus();
    result.record.decision = DmlPageFilespaceDemandHintDecision::refused;
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "dml_demand_hint_invalid_batch",
        "core.bulk_load.dml_demand_hint.invalid_batch",
        "local_transaction_id, batch_sequence, and batch_row_count must be non-zero");
    return result;
  }
  if (request.max_preallocation_pages == 0) {
    result.status = BulkLoadErrorStatus();
    result.record.decision = DmlPageFilespaceDemandHintDecision::refused;
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "dml_demand_hint_missing_bound",
        "core.bulk_load.dml_demand_hint.missing_bound",
        "max_preallocation_pages must bound DML demand hints");
    return result;
  }

  const u64 estimated_pages = EstimateDemandPages(request);
  const u64 requested_pages = std::max(request.requested_page_count, estimated_pages);
  if (requested_pages == 0) {
    result.status = BulkLoadErrorStatus();
    result.record.decision = DmlPageFilespaceDemandHintDecision::refused;
    result.diagnostic = MakeStrictBulkLoadDiagnostic(
        result.status,
        "dml_demand_hint_empty",
        "core.bulk_load.dml_demand_hint.empty",
        "requested or estimated page demand must be non-zero");
    return result;
  }

  result.status = BulkLoadOkStatus();
  result.accepted = true;
  result.record.requested_page_count = requested_pages;
  result.record.granted_page_count = std::min(requested_pages, request.max_preallocation_pages);
  result.record.decision = result.record.granted_page_count < requested_pages
                               ? DmlPageFilespaceDemandHintDecision::capped
                               : DmlPageFilespaceDemandHintDecision::accepted;
  result.record.page_agent_hint = true;
  result.record.filespace_agent_hint = true;
  result.diagnostic = MakeStrictBulkLoadDiagnostic(
      result.status,
      "ok",
      "core.bulk_load.dml_demand_hint.accepted",
      DmlPageFilespaceDemandHintDecisionName(result.record.decision));
  return result;
}

DiagnosticRecord MakeStrictBulkLoadDiagnostic(Status status,
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
                                                     "core.bulk_load.strict_lifecycle",
                                                     status.ok() ? "" : "refuse strict bulk-load success and retain rollback/recovery evidence");
}

}  // namespace scratchbird::core::bulk_load
