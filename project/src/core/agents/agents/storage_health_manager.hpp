// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <string>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class StorageHealthManagerDecisionKind : u32 {
  no_action,
  quarantine_route_recommended,
  quarantine_operator_review_required,
  storage_cost_update_recommended,
  storage_health_summary_emitted,
  action_refused
};

enum class StorageHealthManagerActionKind : u32 {
  request_filespace_quarantine,
  update_storage_cost,
  emit_storage_health_summary,
  forbidden_request_filespace_expand,
  forbidden_request_filespace_move,
  forbidden_request_filespace_shrink,
  forbidden_request_filespace_truncate,
  forbidden_request_filespace_detach,
  forbidden_request_filespace_delete,
  forbidden_promote_filespace,
  forbidden_demote_filespace,
  forbidden_allocate_pages,
  forbidden_relocate_pages,
  forbidden_rebuild_indexes,
  forbidden_override_filespace_policy,
  forbidden_override_page_policy
};

enum class StorageHealthSeverity : u32 {
  unknown,
  healthy,
  degraded,
  critical,
  failed
};

enum class StorageHealthEvidenceKind : u32 {
  none,
  checksum_failure,
  unknown_page,
  device_error,
  latency_histogram,
  health_summary
};

struct StorageHealthManagerMetricSnapshot {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  StorageHealthSeverity filespace_health = StorageHealthSeverity::healthy;
  u64 device_error_count = 0;
  u64 checksum_failure_count = 0;
  u64 unknown_page_count = 0;
  u64 page_allocation_failure_count = 0;
  u64 fsync_latency_p99_microseconds = 0;
  bool scope_compatible = true;
  bool filespace_health_present = true;
  bool filespace_health_fresh = true;
  bool filespace_health_trusted = true;
  bool device_error_present = true;
  bool device_error_fresh = true;
  bool device_error_trusted = true;
  bool checksum_present = true;
  bool checksum_fresh = true;
  bool checksum_trusted = true;
  bool unknown_page_present = true;
  bool unknown_page_fresh = true;
  bool unknown_page_trusted = true;
  bool page_metric_applicable = true;
  bool page_metric_present = true;
  bool page_metric_fresh = true;
  bool page_metric_trusted = true;
  bool storage_latency_present = true;
  bool storage_latency_fresh = true;
  bool storage_latency_trusted = true;
};

struct StorageHealthManagerPolicy {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool quarantine_recommendation_allowed = true;
  bool critical_automatic_quarantine_policy = false;
  bool operator_review_route_allowed = true;
  bool storage_cost_recommendation_allowed = true;
  bool health_summary_allowed = true;
  u64 checksum_failure_quarantine_threshold = 1;
  u64 unknown_page_quarantine_threshold = 1;
  u64 device_error_quarantine_threshold = 1;
  u64 fsync_p99_cost_update_threshold_microseconds = 1000;
};

struct StorageHealthManagerActionRequest {
  StorageHealthManagerActionKind action =
      StorageHealthManagerActionKind::emit_storage_health_summary;
  StorageHealthEvidenceKind evidence_kind = StorageHealthEvidenceKind::health_summary;
  TypedUuid request_uuid;
  TypedUuid evidence_uuid;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid metric_evidence_uuid;
  bool explicit_evidence = true;
  bool metric_evidence_fresh = true;
  bool metric_evidence_trusted = true;
  bool operator_review_requested = false;
};

struct StorageHealthManagerEvidence {
  TypedUuid request_uuid;
  TypedUuid evidence_uuid;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  StorageHealthManagerActionKind action =
      StorageHealthManagerActionKind::emit_storage_health_summary;
  StorageHealthManagerDecisionKind decision =
      StorageHealthManagerDecisionKind::no_action;
  StorageHealthEvidenceKind evidence_kind = StorageHealthEvidenceKind::none;
  std::string diagnostic_code;
  std::string evidence_state;
  std::string route_target;
  std::string reason;
  bool durable_state_changed = false;
  bool physical_filespace_mutation_attempted = false;
  bool page_ledger_mutation_attempted = false;
  bool index_mutation_attempted = false;
  bool policy_override_attempted = false;
};

struct StorageHealthManagerActionResult {
  Status status;
  StorageHealthManagerDecisionKind decision =
      StorageHealthManagerDecisionKind::action_refused;
  DiagnosticRecord diagnostic;
  StorageHealthManagerEvidence evidence;
  StorageHealthManagerActionKind action =
      StorageHealthManagerActionKind::emit_storage_health_summary;
  bool fail_closed = false;
  bool refused = false;
  bool recommended = false;
  bool route_recommended = false;
  bool operator_review_required = false;
  bool summary_emitted = false;
  bool cost_update_recommended = false;
  bool physical_filespace_mutation_attempted = false;
  bool page_ledger_mutation_attempted = false;
  bool index_mutation_attempted = false;
  bool policy_override_attempted = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* StorageHealthManagerDecisionKindName(StorageHealthManagerDecisionKind kind);
const char* StorageHealthManagerActionKindName(StorageHealthManagerActionKind action);
const char* StorageHealthSeverityName(StorageHealthSeverity severity);
const char* StorageHealthEvidenceKindName(StorageHealthEvidenceKind kind);

StorageHealthManagerPolicy DefaultStorageHealthManagerPolicy();
StorageHealthManagerActionResult EvaluateStorageHealthManagerAction(
    const StorageHealthManagerActionRequest& request,
    const StorageHealthManagerMetricSnapshot& snapshot,
    const StorageHealthManagerPolicy& policy);
DiagnosticRecord MakeStorageHealthManagerDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail = {});

const char* storage_health_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
