// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "crud_support/crud_store.hpp"
#include "dml/delete_api.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_DELETE_BATCH_CONTEXT_CORE
// Engine-owned delete optimization context. This layer accepts UUID-bound engine
// inputs only; parser/reference names must already be resolved before it is built.

// SEARCH_KEY: SB_PID003_DELETE_BATCH_CONTEXT

enum class DeleteBatchMode {
  singleton_row_uuid,
  predicate_scan,
  indexed_predicate,
  reference_bulk,
  native_bulk
};

enum class DeleteFeatureState {
  enabled,
  disabled,
  policy_required,
  refused
};

enum class DeleteIndexMaintenanceAction {
  visibility_recheck_only,
  tombstone_delta_ledger,
  synchronous_tombstone_rewrite,
  shadow_rebuild_then_cutover,
  reject_batch_path
};

struct DeleteFeatureGates {
  DeleteFeatureState delete_batch_context = DeleteFeatureState::enabled;
  DeleteFeatureState predicate_preflight = DeleteFeatureState::enabled;
  DeleteFeatureState tombstone_evidence = DeleteFeatureState::enabled;
  DeleteFeatureState page_reclamation_notice = DeleteFeatureState::disabled;
  DeleteFeatureState deferred_secondary_index_runtime = DeleteFeatureState::disabled;
  DeleteFeatureState secondary_index_delta_ledger = DeleteFeatureState::disabled;
  DeleteFeatureState physical_cow_bridge = DeleteFeatureState::disabled;
};

struct DeleteBatchMemoryPolicy {
  std::uint64_t context_budget_bytes = 1024 * 1024;
  std::uint64_t predicate_budget_bytes = 1024 * 1024;
  std::uint64_t delta_ledger_stage_budget_bytes = 1024 * 1024;
  bool spill_allowed = false;
};

struct DeleteBatchTraceEvent {
  std::string event_name;
  std::string phase;
  std::string detail;
};

struct BoundDeletePredicateTemplate {
  std::string template_id;
  std::string table_uuid;
  std::string predicate_kind;
  std::string predicate_envelope;
  std::vector<std::string> touched_columns;
  bool touches_opaque_render_only_column = false;
  bool row_uuid_singleton = false;
};

struct DeleteIndexMaintenancePlanEntry {
  CrudIndexRecord index;
  DeleteIndexMaintenanceAction action = DeleteIndexMaintenanceAction::visibility_recheck_only;
  std::string reason;
};

struct DeleteIndexMaintenancePlan {
  std::string plan_id;
  std::string table_uuid;
  std::vector<DeleteIndexMaintenancePlanEntry> entries;
  bool has_delta_eligible = false;
  bool rejected = false;
  std::string rejection_reason;
};

struct DeletePageReclamationPlan {
  std::string plan_id;
  std::uint64_t estimated_rows = 0;
  std::uint64_t notify_page_agent_after_tombstones = 0;
  bool page_reclamation_notice_enabled = false;
  std::string refusal_reason;
};

struct DeleteSecondaryIndexDeltaLedgerPolicy {
  bool enabled = false;
  bool runtime_enabled = false;
  bool readers_overlay_committed_deltas = false;
  bool cleanup_horizon_bound = false;
  bool recovery_classifiable = false;
  bool synchronous_fallback_required = true;
  std::string fallback_reason = "runtime_deferred_secondary_index_disabled";
};

struct DeleteBatchContext {
  std::string statement_uuid;
  std::uint64_t local_transaction_id = 0;
  std::string transaction_uuid;
  std::string database_uuid;
  std::string target_object_uuid;
  std::uint64_t estimated_match_count = 0;
  std::uint64_t actual_match_count = 0;
  std::uint64_t actual_delete_count = 0;
  DeleteBatchMode delete_mode = DeleteBatchMode::predicate_scan;
  std::string security_context_uuid;
  std::string policy_snapshot_uuid;
  DeleteFeatureGates feature_gates;
  DeleteBatchMemoryPolicy memory_policy;
  BoundDeletePredicateTemplate predicate_template;
  DeleteIndexMaintenancePlan index_plan;
  DeletePageReclamationPlan reclamation_plan;
  DeleteSecondaryIndexDeltaLedgerPolicy delta_ledger_policy;
  std::vector<DeleteBatchTraceEvent> trace_events;
  std::vector<EngineEvidenceReference> evidence;
  std::vector<EngineApiDiagnostic> diagnostics;
  std::string fallback_reason;
  bool tombstone_only = true;
  bool accepted = false;
  bool cancelled = false;
};

const char* DeleteBatchModeName(DeleteBatchMode mode);
const char* DeleteFeatureStateName(DeleteFeatureState state);
const char* DeleteIndexMaintenanceActionName(DeleteIndexMaintenanceAction action);

DeleteBatchMode ResolveDeleteBatchMode(const EngineDeleteRowsRequest& request,
                                       const CrudState& state,
                                       const CrudTableRecord& table,
                                       const std::vector<CrudIndexRecord>& indexes);
DeleteFeatureGates ResolveDeleteFeatureGates(const EngineDeleteRowsRequest& request);
DeleteBatchMemoryPolicy ResolveDeleteMemoryPolicy(const EngineDeleteRowsRequest& request);
BoundDeletePredicateTemplate BuildBoundDeletePredicateTemplate(const EngineDeleteRowsRequest& request,
                                                              const CrudTableRecord& table);
DeleteIndexMaintenancePlan BuildDeleteIndexMaintenancePlan(const EngineDeleteRowsRequest& request,
                                                           const CrudState& state,
                                                           const CrudTableRecord& table,
                                                           const std::vector<CrudIndexRecord>& indexes,
                                                           const DeleteFeatureGates& feature_gates);
DeletePageReclamationPlan BuildDeletePageReclamationPlan(const EngineDeleteRowsRequest& request,
                                                         std::uint64_t estimated_matches,
                                                         const DeleteFeatureGates& feature_gates);
DeleteSecondaryIndexDeltaLedgerPolicy ResolveDeleteSecondaryIndexDeltaLedgerPolicy(const EngineDeleteRowsRequest& request,
                                                                                  const DeleteFeatureGates& feature_gates);
DeleteBatchContext BuildDeleteBatchContext(const EngineDeleteRowsRequest& request,
                                           const CrudState& state,
                                           const CrudTableRecord& table,
                                           const std::vector<CrudIndexRecord>& indexes);

EngineApiDiagnostic ValidateDeleteBatchMemoryBudget(const DeleteBatchContext& context,
                                                    std::uint64_t projected_bytes);
void AddDeleteTrace(DeleteBatchContext* context, std::string event_name, std::string phase, std::string detail = {});
void AddDeleteBatchEvidenceToResult(const DeleteBatchContext& context, EngineApiResult* result);
void RecordDeleteBatchMetric(const DeleteBatchContext& context, std::string metric, double value, std::string result, std::string reason = {});

}  // namespace scratchbird::engine::internal_api
