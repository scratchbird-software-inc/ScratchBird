// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "crud_support/crud_store.hpp"
#include "dml/update_api.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_UPDATE_BATCH_CONTEXT_CORE
// Engine-owned update optimization context. This layer accepts UUID-bound engine
// inputs only; parser/donor names must already be resolved before it is built.

// SEARCH_KEY: SB_PID002_UPDATE_BATCH_CONTEXT

enum class UpdateBatchMode {
  singleton_row_uuid,
  predicate_scan,
  indexed_predicate,
  donor_bulk,
  native_bulk
};

enum class UpdateFeatureState {
  enabled,
  disabled,
  policy_required,
  refused
};

enum class UpdateIndexMaintenanceAction {
  unaffected,
  synchronous_exact_rewrite,
  synchronous_exact_probe_then_rewrite,
  committed_delta_ledger,
  shadow_rebuild_then_cutover,
  reject_batch_path
};

struct UpdateFeatureGates {
  UpdateFeatureState update_batch_context = UpdateFeatureState::enabled;
  UpdateFeatureState assignment_template = UpdateFeatureState::enabled;
  UpdateFeatureState predicate_preflight = UpdateFeatureState::enabled;
  UpdateFeatureState page_reservation = UpdateFeatureState::enabled;
  UpdateFeatureState exact_unique_preflight = UpdateFeatureState::enabled;
  UpdateFeatureState deferred_secondary_index_runtime = UpdateFeatureState::disabled;
  UpdateFeatureState secondary_index_delta_ledger = UpdateFeatureState::disabled;
  UpdateFeatureState physical_cow_bridge = UpdateFeatureState::disabled;
};

struct UpdateBatchMemoryPolicy {
  std::uint64_t context_budget_bytes = 1024 * 1024;
  std::uint64_t assignment_budget_bytes = 1024 * 1024;
  std::uint64_t unique_preflight_budget_bytes = 1024 * 1024;
  std::uint64_t delta_ledger_stage_budget_bytes = 1024 * 1024;
  bool spill_allowed = false;
};

struct UpdateBatchTraceEvent {
  std::string event_name;
  std::string phase;
  std::string detail;
};

struct BoundUpdateAssignmentTemplate {
  std::string template_id;
  std::string table_uuid;
  std::vector<std::pair<std::string, std::string>> columns;
  std::vector<std::string> assigned_columns;
  std::size_t assignment_count = 0;
  bool touches_opaque_render_only_column = false;
  bool toast_required = false;
  std::uint64_t max_inline_encoded_bytes = kCrudVerticalSliceMaxEncodedValueBytes;
};

struct UpdateIndexMaintenancePlanEntry {
  CrudIndexRecord index;
  UpdateIndexMaintenanceAction action = UpdateIndexMaintenanceAction::unaffected;
  std::string reason;
  bool key_or_predicate_affected = false;
};

struct UpdateIndexMaintenancePlan {
  std::string plan_id;
  std::string table_uuid;
  std::vector<UpdateIndexMaintenancePlanEntry> entries;
  bool has_affected_unique_exact = false;
  bool has_delta_eligible = false;
  bool rejected = false;
  std::string rejection_reason;
};

struct UpdatePageReservationPlan {
  std::string reservation_id;
  std::uint64_t estimated_rows = 0;
  std::uint64_t requested_pages = 0;
  std::uint64_t target_available_pages = 8;
  std::uint64_t notify_below_pages = 4;
  bool reservation_available = true;
  std::string refusal_reason;
};

struct UpdateSecondaryIndexDeltaLedgerPolicy {
  bool enabled = false;
  bool runtime_enabled = false;
  bool readers_overlay_committed_deltas = false;
  bool cleanup_horizon_bound = false;
  bool recovery_classifiable = false;
  bool synchronous_fallback_required = true;
  std::string fallback_reason = "runtime_deferred_secondary_index_disabled";
};

struct UpdateBatchContext {
  std::string statement_uuid;
  std::uint64_t local_transaction_id = 0;
  std::string transaction_uuid;
  std::string database_uuid;
  std::string target_object_uuid;
  std::uint64_t estimated_match_count = 0;
  std::uint64_t actual_match_count = 0;
  std::uint64_t actual_update_count = 0;
  UpdateBatchMode update_mode = UpdateBatchMode::predicate_scan;
  std::string predicate_kind;
  std::string security_context_uuid;
  std::string policy_snapshot_uuid;
  UpdateFeatureGates feature_gates;
  UpdateBatchMemoryPolicy memory_policy;
  BoundUpdateAssignmentTemplate assignment_template;
  UpdateIndexMaintenancePlan index_plan;
  UpdatePageReservationPlan page_reservation;
  UpdateSecondaryIndexDeltaLedgerPolicy delta_ledger_policy;
  std::set<std::string> unique_request_keys;
  std::vector<UpdateBatchTraceEvent> trace_events;
  std::vector<EngineEvidenceReference> evidence;
  std::vector<EngineApiDiagnostic> diagnostics;
  std::string fallback_reason;
  bool accepted = false;
  bool cancelled = false;
};

const char* UpdateBatchModeName(UpdateBatchMode mode);
const char* UpdateFeatureStateName(UpdateFeatureState state);
const char* UpdateIndexMaintenanceActionName(UpdateIndexMaintenanceAction action);

UpdateBatchMode ResolveUpdateBatchMode(const EngineUpdateRowsRequest& request,
                                       const CrudState& state,
                                       const CrudTableRecord& table,
                                       const std::vector<CrudIndexRecord>& indexes);
UpdateFeatureGates ResolveUpdateFeatureGates(const EngineUpdateRowsRequest& request);
UpdateBatchMemoryPolicy ResolveUpdateMemoryPolicy(const EngineUpdateRowsRequest& request);
BoundUpdateAssignmentTemplate BuildBoundUpdateAssignmentTemplate(const EngineUpdateRowsRequest& request,
                                                                const CrudTableRecord& table);
UpdateIndexMaintenancePlan BuildUpdateIndexMaintenancePlan(const EngineUpdateRowsRequest& request,
                                                           const CrudState& state,
                                                           const CrudTableRecord& table,
                                                           const std::vector<CrudIndexRecord>& indexes,
                                                           const UpdateFeatureGates& feature_gates,
                                                           const UpdateSecondaryIndexDeltaLedgerPolicy& delta_ledger_policy);
UpdatePageReservationPlan ReserveUpdatePages(const EngineUpdateRowsRequest& request,
                                             const BoundUpdateAssignmentTemplate& assignment_template,
                                             std::uint64_t estimated_matches);
UpdateSecondaryIndexDeltaLedgerPolicy ResolveUpdateSecondaryIndexDeltaLedgerPolicy(const EngineUpdateRowsRequest& request,
                                                                                  const UpdateFeatureGates& feature_gates);
UpdateBatchContext BuildUpdateBatchContext(const EngineUpdateRowsRequest& request,
                                           const CrudState& state,
                                           const CrudTableRecord& table,
                                           const std::vector<CrudIndexRecord>& indexes);

EngineApiDiagnostic ValidateUpdateBatchMemoryBudget(const UpdateBatchContext& context,
                                                    std::uint64_t projected_bytes);
EngineApiDiagnostic ValidateUpdateBatchUniquePreflight(UpdateBatchContext* context,
                                                       const std::vector<std::pair<std::string, std::string>>& values,
                                                       const std::string& row_uuid);
void AddUpdateTrace(UpdateBatchContext* context, std::string event_name, std::string phase, std::string detail = {});
void AddUpdateBatchEvidenceToResult(const UpdateBatchContext& context, EngineApiResult* result);
void RecordUpdateBatchMetric(const UpdateBatchContext& context, std::string metric, double value, std::string result, std::string reason = {});

}  // namespace scratchbird::engine::internal_api
