// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-STRICT-BULK-LOAD-LIFECYCLE-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::bulk_load {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class StrictBulkLoadState : u32 {
  absent,
  begun,
  appending,
  finalize_evidence_durable,
  published_visible,
  rolled_back,
  recovery_required,
  refused,
  quarantine
};

enum class StrictBulkLoadRecoveryAction : u32 {
  no_action,
  complete_publication,
  roll_back_staging,
  operator_review,
  fail_closed
};

enum class CopyBatchMetricStatus : u32 {
  unknown,
  accepted,
  refused,
  completed
};

enum class DmlPageFilespaceDemandHintDecision : u32 {
  disabled,
  accepted,
  capped,
  refused
};

struct StrictBulkLoadPolicySnapshot {
  TypedUuid policy_uuid;
  bool enabled = false;
  bool allow_reference_relaxed_semantics = false;
  bool map_reference_relaxed_to_native_safe = false;
  bool require_all_constraints_valid = true;
  bool require_all_indexes_valid = true;
  bool require_all_domains_valid = true;
  bool require_all_policy_gates_valid = true;
};

struct StrictBulkLoadRow {
  TypedUuid row_uuid;
  std::string encoded_row;
  bool constraints_valid = true;
  bool indexes_valid = true;
  bool domains_valid = true;
  bool policy_gates_valid = true;
};

struct StrictBulkLoadBeginRequest {
  TypedUuid database_uuid;
  TypedUuid object_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  StrictBulkLoadPolicySnapshot policy;
  bool reference_relaxed_semantics_requested = false;
  std::string staging_target;
};

struct StrictBulkLoadAppendRequest {
  TypedUuid bulk_load_id;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::vector<StrictBulkLoadRow> rows;
};

struct StrictBulkLoadFinalizeRequest {
  TypedUuid bulk_load_id;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  bool simulate_finalize_failure_after_evidence = false;
  std::string visibility_fence;
};

struct StrictBulkLoadRollbackRequest {
  TypedUuid bulk_load_id;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::string reason;
};

struct StrictBulkLoadQuarantineRequest {
  TypedUuid bulk_load_id;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::string reason;
};

struct StrictBulkLoadEvidenceRecord {
  u64 sequence = 0;
  std::string action;
  TypedUuid evidence_id;
  TypedUuid bulk_load_id;
  TypedUuid database_uuid;
  TypedUuid object_uuid;
  TypedUuid transaction_uuid;
  TypedUuid policy_uuid;
  u64 local_transaction_id = 0;
  u64 staged_row_count = 0;
  u64 visible_row_count = 0;
  u64 index_closeout_count = 0;
  StrictBulkLoadState previous_state = StrictBulkLoadState::absent;
  StrictBulkLoadState new_state = StrictBulkLoadState::absent;
  std::string staging_target;
  std::string visibility_fence;
  std::string reason;
  std::string diagnostic_code;
  bool durable_state_changed = false;
};

struct StrictBulkLoadOperation {
  TypedUuid bulk_load_id;
  TypedUuid database_uuid;
  TypedUuid object_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  StrictBulkLoadPolicySnapshot policy;
  StrictBulkLoadState state = StrictBulkLoadState::absent;
  std::string staging_target;
  std::string visibility_fence;
  std::vector<StrictBulkLoadRow> staged_rows;
  std::vector<StrictBulkLoadRow> visible_rows;
  u64 index_closeout_count = 0;
};

struct StrictBulkLoadLedger {
  std::vector<StrictBulkLoadOperation> operations;
  std::vector<StrictBulkLoadEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
};

struct CopyBatchMetricTiming {
  u64 elapsed_nanos = 0;
  bool recorded = false;
};

struct CopyBatchMetricRequest {
  u64 batch_sequence = 0;
  u64 batch_row_count = 0;
  CopyBatchMetricTiming parse_timing;
  CopyBatchMetricTiming bind_timing;
  CopyBatchMetricTiming append_timing;
  CopyBatchMetricTiming page_timing;
  CopyBatchMetricTiming index_timing;
  CopyBatchMetricTiming finality_timing;
  CopyBatchMetricStatus message_status = CopyBatchMetricStatus::unknown;
  CopyBatchMetricStatus result_status = CopyBatchMetricStatus::unknown;
  std::string message_status_detail;
  std::string result_status_detail;
};

struct CopyBatchMetricRecord {
  u64 batch_sequence = 0;
  u64 batch_row_count = 0;
  CopyBatchMetricTiming parse_timing;
  CopyBatchMetricTiming bind_timing;
  CopyBatchMetricTiming append_timing;
  CopyBatchMetricTiming page_timing;
  CopyBatchMetricTiming index_timing;
  CopyBatchMetricTiming finality_timing;
  CopyBatchMetricStatus message_status = CopyBatchMetricStatus::unknown;
  CopyBatchMetricStatus result_status = CopyBatchMetricStatus::unknown;
  std::string message_status_detail;
  std::string result_status_detail;
};

struct CopyBatchMetricResult {
  Status status;
  bool accepted = false;
  CopyBatchMetricRecord record;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && accepted; }
};

struct CopyBatchMetricTotals {
  u64 metric_record_count = 0;
  u64 accepted_message_count = 0;
  u64 refused_message_count = 0;
  u64 accepted_result_count = 0;
  u64 refused_result_count = 0;
  u64 completed_result_count = 0;
  u64 total_batch_rows = 0;
  u64 parse_nanos = 0;
  u64 bind_nanos = 0;
  u64 append_nanos = 0;
  u64 page_nanos = 0;
  u64 index_nanos = 0;
  u64 finality_nanos = 0;
};

struct CopyBatchMetricLedger {
  std::vector<CopyBatchMetricRecord> records;
  CopyBatchMetricTotals totals;
};

struct DmlPageFilespaceDemandHintRequest {
  TypedUuid database_uuid;
  TypedUuid object_uuid;
  TypedUuid filespace_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  u64 batch_sequence = 0;
  u64 batch_row_count = 0;
  u64 estimated_row_bytes = 0;
  u64 page_size_bytes = 0;
  u64 requested_page_count = 0;
  u64 max_preallocation_pages = 0;
  bool enabled = true;
  std::string source;
};

struct DmlPageFilespaceDemandHintRecord {
  TypedUuid database_uuid;
  TypedUuid object_uuid;
  TypedUuid filespace_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  u64 batch_sequence = 0;
  u64 batch_row_count = 0;
  u64 requested_page_count = 0;
  u64 granted_page_count = 0;
  u64 max_preallocation_pages = 0;
  DmlPageFilespaceDemandHintDecision decision = DmlPageFilespaceDemandHintDecision::refused;
  bool page_agent_hint = false;
  bool filespace_agent_hint = false;
  std::string source;
};

struct DmlPageFilespaceDemandHintResult {
  Status status;
  bool accepted = false;
  DmlPageFilespaceDemandHintRecord record;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && accepted; }
};

struct StrictBulkLoadBeginResult {
  Status status;
  bool begun = false;
  StrictBulkLoadOperation operation;
  StrictBulkLoadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && begun; }
};

struct StrictBulkLoadAppendResult {
  Status status;
  bool appended = false;
  u64 staged_row_count = 0;
  StrictBulkLoadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && appended; }
};

struct StrictBulkLoadFinalizeResult {
  Status status;
  bool finalized = false;
  bool published_visible = false;
  bool recovery_required = false;
  u64 visible_row_count = 0;
  u64 index_closeout_count = 0;
  StrictBulkLoadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && finalized && published_visible; }
};

struct StrictBulkLoadFinalizeEvidenceResult {
  Status status;
  bool finalized = false;
  bool recovery_required = false;
  u64 staged_row_count = 0;
  u64 index_closeout_count = 0;
  StrictBulkLoadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && finalized; }
};

struct StrictBulkLoadPublishRequest {
  TypedUuid bulk_load_id;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::string visibility_fence;
};

struct StrictBulkLoadPublishResult {
  Status status;
  bool published_visible = false;
  u64 visible_row_count = 0;
  u64 index_closeout_count = 0;
  StrictBulkLoadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && published_visible; }
};

struct StrictBulkLoadRollbackResult {
  Status status;
  bool rolled_back = false;
  StrictBulkLoadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && rolled_back; }
};

struct StrictBulkLoadQuarantineResult {
  Status status;
  bool quarantined = false;
  StrictBulkLoadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && quarantined; }
};

struct StrictBulkLoadRecoveryClassification {
  TypedUuid bulk_load_id;
  StrictBulkLoadState observed_state = StrictBulkLoadState::absent;
  StrictBulkLoadRecoveryAction action = StrictBulkLoadRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct StrictBulkLoadRecoveryResult {
  Status status;
  std::vector<StrictBulkLoadRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* StrictBulkLoadStateName(StrictBulkLoadState state);
const char* StrictBulkLoadRecoveryActionName(StrictBulkLoadRecoveryAction action);
const char* CopyBatchMetricStatusName(CopyBatchMetricStatus status);
const char* DmlPageFilespaceDemandHintDecisionName(DmlPageFilespaceDemandHintDecision decision);

StrictBulkLoadBeginResult BeginStrictBulkLoad(StrictBulkLoadLedger* ledger,
                                              const StrictBulkLoadBeginRequest& request);
StrictBulkLoadAppendResult AppendStrictBulkLoadRows(StrictBulkLoadLedger* ledger,
                                                    const StrictBulkLoadAppendRequest& request);
StrictBulkLoadFinalizeEvidenceResult FinalizeStrictBulkLoadEvidenceDurable(
    StrictBulkLoadLedger* ledger,
    const StrictBulkLoadFinalizeRequest& request);
StrictBulkLoadPublishResult PublishStrictBulkLoadVisible(
    StrictBulkLoadLedger* ledger,
    const StrictBulkLoadPublishRequest& request);
StrictBulkLoadFinalizeResult FinalizeStrictBulkLoad(StrictBulkLoadLedger* ledger,
                                                    const StrictBulkLoadFinalizeRequest& request);
StrictBulkLoadRollbackResult RollbackStrictBulkLoad(StrictBulkLoadLedger* ledger,
                                                    const StrictBulkLoadRollbackRequest& request);
StrictBulkLoadQuarantineResult QuarantineStrictBulkLoad(StrictBulkLoadLedger* ledger,
                                                        const StrictBulkLoadQuarantineRequest& request);
StrictBulkLoadRecoveryResult ClassifyStrictBulkLoadLedgerForRecovery(const StrictBulkLoadLedger& ledger);
const StrictBulkLoadOperation* FindStrictBulkLoadOperation(const StrictBulkLoadLedger& ledger,
                                                          const TypedUuid& bulk_load_id);
CopyBatchMetricResult MakeCopyBatchMetricRecord(const CopyBatchMetricRequest& request);
CopyBatchMetricResult RecordCopyBatchMetric(CopyBatchMetricLedger* ledger,
                                            const CopyBatchMetricRequest& request);
DmlPageFilespaceDemandHintResult MakeDmlPageFilespaceDemandHint(
    const DmlPageFilespaceDemandHintRequest& request);
DiagnosticRecord MakeStrictBulkLoadDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

}  // namespace scratchbird::core::bulk_load
