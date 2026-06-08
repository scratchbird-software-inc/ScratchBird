// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/storage_version_cleanup_agent.hpp"

#include "row_version_cleanup_batch.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

namespace mga = scratchbird::transaction::mga;
namespace storage_page = scratchbird::storage::page;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CleanupAgentOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status CleanupAgentErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddEvidence(StorageVersionCleanupAgentResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

bool IsActiveBlocker(const mga::CleanupHorizonBlocker& blocker) {
  switch (blocker.kind) {
    case mga::CleanupHorizonBlockerKind::active_transaction:
    case mga::CleanupHorizonBlockerKind::active_snapshot:
    case mga::CleanupHorizonBlockerKind::always_active_session:
      return true;
    case mga::CleanupHorizonBlockerKind::unresolved_outcome:
    case mga::CleanupHorizonBlockerKind::inventory_authority:
    case mga::CleanupHorizonBlockerKind::unknown:
      return false;
  }
  return false;
}

u64 CountActiveBlockers(const AuthoritativeCleanupHorizonResult& horizon) {
  u64 count = 0;
  for (const auto& blocker : horizon.blockers) {
    if (IsActiveBlocker(blocker)) {
      ++count;
    }
  }
  return count;
}

StorageVersionCleanupPressureMetrics MeasureBefore(
    const StorageVersionCleanupAgentRequest& request,
    const AuthoritativeCleanupHorizonResult& horizon,
    const storage_page::StorageRowVersionCleanupCandidateBatch& batch) {
  StorageVersionCleanupPressureMetrics metrics;
  (void)request;
  metrics.total_row_versions = batch.metrics.total_row_versions;
  metrics.cleanup_candidate_row_versions =
      batch.metrics.cleanup_candidate_row_versions;
  metrics.current_visible_row_versions =
      batch.metrics.current_visible_row_versions;
  metrics.active_cleanup_blockers = CountActiveBlockers(horizon);
  return metrics;
}

StorageVersionCleanupAgentResult Finish(StorageVersionCleanupAgentResult result,
                                        StorageVersionCleanupDecisionKind decision,
                                        std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail,
                                        bool fail_closed) {
  result.status = fail_closed ? CleanupAgentErrorStatus() : CleanupAgentOkStatus();
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeStorageVersionCleanupAgentDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  AddEvidence(&result,
              "storage_version_cleanup_agent",
              "dpc032_storage_version_cleanup_agent_v1");
  AddEvidence(&result,
              "cleanup_horizon_service",
              "dpc030_authoritative_cleanup_horizon_v1");
  AddEvidence(&result, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(&result, "decision", StorageVersionCleanupDecisionKindName(decision));
  AddEvidence(&result, "fail_closed", BoolText(fail_closed));
  AddEvidence(&result, "bounded_batch", BoolText(result.bounded_batch));
  AddEvidence(&result, "budget_exhausted", BoolText(result.budget_exhausted));
  AddEvidence(&result, "parser_finality_authority", "false");
  AddEvidence(&result, "client_state_authority", "false");
  AddEvidence(&result, "timestamp_ordering_authority", "false");
  AddEvidence(&result, "uuid_ordering_authority", "false");
  AddEvidence(&result, "crud_event_stream_authority", "false");
  AddEvidence(&result, "cluster_private_implementation", "false");
  AddEvidence(&result, "before_total_row_versions",
              std::to_string(result.before.total_row_versions));
  AddEvidence(&result, "before_cleanup_candidate_row_versions",
              std::to_string(result.before.cleanup_candidate_row_versions));
  AddEvidence(&result, "before_current_visible_row_versions",
              std::to_string(result.before.current_visible_row_versions));
  AddEvidence(&result, "before_active_cleanup_blockers",
              std::to_string(result.before.active_cleanup_blockers));
  AddEvidence(&result, "after_total_row_versions",
              std::to_string(result.after.total_row_versions));
  AddEvidence(&result, "after_reclaimed_row_versions",
              std::to_string(result.after.reclaimed_row_versions));
  AddEvidence(&result, "after_retained_row_versions",
              std::to_string(result.after.retained_row_versions));
  AddEvidence(&result, "after_blocked_row_versions",
              std::to_string(result.after.blocked_row_versions));
  if (result.horizon.cleanup_horizon.valid()) {
    AddEvidence(&result,
                "cleanup_horizon_local_transaction_id",
                std::to_string(result.horizon.cleanup_horizon.value));
  }
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

StorageVersionCleanupPressureMetrics MeasureAfter(
    const StorageVersionCleanupPressureMetrics& before,
    const LocalGarbageCollectionSweepResult& sweep) {
  StorageVersionCleanupPressureMetrics after = before;
  after.reclaimed_row_versions = sweep.cleanup.reclaimed_row_version_count;
  after.retained_row_versions = sweep.cleanup.retained_row_version_count;
  after.blocked_row_versions = sweep.cleanup.horizon_blocked_row_version_count +
                               sweep.cleanup.backup_archive_blocked_row_version_count +
                               sweep.cleanup.retention_blocked_row_version_count +
                               sweep.cleanup.limbo_or_unknown_outcome_blocked_row_version_count;
  if (after.total_row_versions >= after.reclaimed_row_versions) {
    after.total_row_versions -= after.reclaimed_row_versions;
  } else {
    after.total_row_versions = 0;
  }
  return after;
}

}  // namespace

const char* StorageVersionCleanupDecisionKindName(
    StorageVersionCleanupDecisionKind decision) {
  switch (decision) {
    case StorageVersionCleanupDecisionKind::success:
      return "success";
    case StorageVersionCleanupDecisionKind::no_op:
      return "no_op";
    case StorageVersionCleanupDecisionKind::budget_exhausted:
      return "budget_exhausted";
    case StorageVersionCleanupDecisionKind::blocked_by_active_transactions:
      return "blocked_by_active_transactions";
    case StorageVersionCleanupDecisionKind::refused_non_authoritative:
      return "refused_non_authoritative";
    case StorageVersionCleanupDecisionKind::refused:
      return "refused";
  }
  return "refused";
}

StorageVersionCleanupAgentResult RunStorageVersionCleanupAgentBatch(
    const StorageVersionCleanupAgentRequest& request) {
  StorageVersionCleanupAgentResult result;
  result.horizon = mga::ComputeAuthoritativeCleanupHorizon(request.horizon_request);
  auto candidate_batch = storage_page::BuildStorageRowVersionCleanupCandidateBatch(
      request.row_versions,
      result.horizon,
      request.max_candidate_row_versions);
  if (!request.engine_mga_authoritative || !result.horizon.ok()) {
    result.before = MeasureBefore(request, result.horizon, candidate_batch);
    result.after = result.before;
    return Finish(std::move(result),
                  StorageVersionCleanupDecisionKind::refused_non_authoritative,
                  "STORAGE_VERSION_CLEANUP.NON_AUTHORITATIVE_REFUSAL",
                  "agents.storage_version_cleanup.non_authoritative_refusal",
                  result.horizon.diagnostic.diagnostic_code.empty()
                      ? "local MGA engine and authoritative cleanup horizon are required"
                      : result.horizon.diagnostic.diagnostic_code,
                  true);
  }
  result.before = MeasureBefore(request, result.horizon, candidate_batch);
  result.bounded_batch = request.max_candidate_row_versions != 0;
  if (request.max_candidate_row_versions == 0) {
    result.after = result.before;
    return Finish(std::move(result),
                  StorageVersionCleanupDecisionKind::refused,
                  "STORAGE_VERSION_CLEANUP.BUDGET_REQUIRED",
                  "agents.storage_version_cleanup.budget_required",
                  "max_candidate_row_versions must be non-zero",
                  true);
  }
  if (result.before.cleanup_candidate_row_versions == 0) {
    result.after = result.before;
    return Finish(std::move(result),
                  StorageVersionCleanupDecisionKind::no_op,
                  "STORAGE_VERSION_CLEANUP.NO_OP",
                  "agents.storage_version_cleanup.no_op",
                  "no row versions are below the authoritative cleanup horizon",
                  false);
  }

  result.budget_exhausted = candidate_batch.budget_exhausted;

  mga::LocalCleanupWorksetRequest workset;
  workset.inventory = request.horizon_request.inventory;
  workset.inventory_authoritative = request.horizon_request.inventory_authoritative;
  workset.inventory_complete = request.horizon_request.inventory_complete;
  workset.active_snapshot_horizons = request.horizon_request.active_snapshot_horizons;
  workset.active_snapshot_inventory_authoritative =
      request.horizon_request.active_snapshot_inventory_authoritative;
  workset.always_in_transaction_policy =
      request.horizon_request.always_in_transaction_policy;
  workset.always_active_session_inventory_authoritative =
      request.horizon_request.always_active_session_inventory_authoritative;
  workset.always_active_sessions = request.horizon_request.always_active_sessions;
  workset.additional_holds = request.additional_holds;
  workset.history_retention_horizons = request.history_retention_horizons;
  workset.row_versions = std::move(candidate_batch.candidates);
  workset.retain_row_versions_in_result = request.retain_row_versions_in_result;

  mga::LocalGarbageCollectionSweepRequest sweep_request;
  sweep_request.workset = std::move(workset);
  sweep_request.family = mga::LocalCleanupSweepFamily::background;
  sweep_request.engine_mga_authoritative = request.engine_mga_authoritative;
  sweep_request.max_candidate_row_versions = request.max_candidate_row_versions;
  sweep_request.max_retained_row_versions = request.max_retained_row_versions;
  sweep_request.retain_row_versions_in_result = request.retain_row_versions_in_result;
  result.sweep = mga::RunLocalGarbageCollectionSweep(sweep_request);
  result.after = MeasureAfter(result.before, result.sweep);
  if (!result.sweep.ok()) {
    return Finish(std::move(result),
                  StorageVersionCleanupDecisionKind::refused,
                  result.sweep.diagnostic.diagnostic_code.empty()
                      ? "STORAGE_VERSION_CLEANUP.REFUSED"
                      : result.sweep.diagnostic.diagnostic_code,
                  "agents.storage_version_cleanup.sweep_refused",
                  "MGA sweep refused the selected storage version batch",
                  true);
  }
  if (result.budget_exhausted) {
    return Finish(std::move(result),
                  StorageVersionCleanupDecisionKind::budget_exhausted,
                  "STORAGE_VERSION_CLEANUP.BUDGET_EXHAUSTED",
                  "agents.storage_version_cleanup.budget_exhausted",
                  std::to_string(request.max_candidate_row_versions),
                  false);
  }
  if (result.sweep.cleanup.reclaimed_row_version_count == 0 &&
      result.before.active_cleanup_blockers != 0 &&
      result.sweep.cleanup.horizon_blocked_row_version_count != 0) {
    return Finish(std::move(result),
                  StorageVersionCleanupDecisionKind::blocked_by_active_transactions,
                  "STORAGE_VERSION_CLEANUP.BLOCKED_BY_ACTIVE_TRANSACTIONS",
                  "agents.storage_version_cleanup.blocked_by_active_transactions",
                  std::to_string(result.before.active_cleanup_blockers),
                  false);
  }
  if (result.sweep.cleanup.reclaimed_row_version_count == 0) {
    return Finish(std::move(result),
                  StorageVersionCleanupDecisionKind::no_op,
                  "STORAGE_VERSION_CLEANUP.NO_OP",
                  "agents.storage_version_cleanup.no_op",
                  "eligible batch retained by current visibility or holds",
                  false);
  }
  return Finish(std::move(result),
                StorageVersionCleanupDecisionKind::success,
                "STORAGE_VERSION_CLEANUP.SUCCESS",
                "agents.storage_version_cleanup.success",
                std::to_string(result.sweep.cleanup.reclaimed_row_version_count),
                false);
}

DiagnosticRecord MakeStorageVersionCleanupAgentDiagnostic(Status status,
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
                        "core.agents.storage_version_cleanup");
}

// Canonical storage_version_cleanup_agent behavior is registered in CanonicalAgentRegistry().
// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_storage_version_cleanup_agent
// DPC_STORAGE_VERSION_CLEANUP_AGENT
const char* storage_version_cleanup_agent_implementation_anchor() {
  return "storage_version_cleanup_agent";
}

}  // namespace scratchbird::core::agents::implemented_agents
