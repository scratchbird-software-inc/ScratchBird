// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_batch.hpp"

#include "api_diagnostics.hpp"
#include "deferred_secondary_index_runtime_policy.hpp"
#include "metric_producer.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace idx = scratchbird::core::index;

constexpr const char* kUpdateMetricsProducer = "engine_update";

std::string MakeId(const std::string& prefix, const std::string& stable) {
  return prefix + ":" + stable;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool IsUniqueIndex(const CrudIndexRecord& index) {
  return index.unique || std::find(index.key_envelopes.begin(), index.key_envelopes.end(), "unique") != index.key_envelopes.end();
}

bool IsDeltaEligibleFamily(const CrudIndexRecord& index) {
  if (IsUniqueIndex(index)) {
    return false;
  }
  return index.family == kCrudIndexFamilyBtree || index.family == kCrudIndexFamilyHash || index.family.empty();
}

bool AssignmentTouchesColumn(const std::vector<std::string>& assigned_columns, const std::string& column) {
  if (column.empty()) {
    return false;
  }
  return std::find(assigned_columns.begin(), assigned_columns.end(), column) != assigned_columns.end();
}

bool EnvelopeTouchesAssignedColumn(const std::string& envelope, const std::vector<std::string>& assigned_columns) {
  if (envelope.empty() || envelope == "unique") {
    return false;
  }
  if (envelope.rfind("include:", 0) == 0) {
    return AssignmentTouchesColumn(assigned_columns, envelope.substr(8));
  }
  if (envelope.rfind("where_eq:", 0) == 0) {
    const auto body = envelope.substr(9);
    const auto eq = body.find('=');
    return AssignmentTouchesColumn(assigned_columns, eq == std::string::npos ? body : body.substr(0, eq));
  }
  if (envelope.rfind("lower:", 0) == 0 || envelope.rfind("upper:", 0) == 0) {
    return AssignmentTouchesColumn(assigned_columns, envelope.substr(6));
  }
  if (envelope.rfind("length:", 0) == 0) {
    return AssignmentTouchesColumn(assigned_columns, envelope.substr(7));
  }
  if (envelope.rfind("identity:", 0) == 0) {
    return AssignmentTouchesColumn(assigned_columns, envelope.substr(9));
  }
  const auto open = envelope.find('(');
  const auto close = envelope.size() > open ? envelope.rfind(')') : std::string::npos;
  if (open != std::string::npos && close != std::string::npos && close > open + 1) {
    return AssignmentTouchesColumn(assigned_columns, envelope.substr(open + 1, close - open - 1));
  }
  return AssignmentTouchesColumn(assigned_columns, envelope);
}

bool IndexAffectedByAssignments(const CrudIndexRecord& index, const std::vector<std::string>& assigned_columns) {
  if (AssignmentTouchesColumn(assigned_columns, index.column_name) ||
      AssignmentTouchesColumn(assigned_columns, index.predicate_column)) {
    return true;
  }
  for (const auto& include : index.include_columns) {
    if (AssignmentTouchesColumn(assigned_columns, include)) {
      return true;
    }
  }
  for (const auto& envelope : index.key_envelopes) {
    if (EnvelopeTouchesAssignedColumn(envelope, assigned_columns)) {
      return true;
    }
  }
  return false;
}

bool UpdateBatchOptionEnabled(const EngineUpdateRowsRequest& request, const std::string& option) {
  return std::find(request.option_envelopes.begin(), request.option_envelopes.end(), option) != request.option_envelopes.end();
}

std::string UpdateBatchOptionValue(const EngineUpdateRowsRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) {
      return option.substr(prefix.size());
    }
  }
  return {};
}

std::uint64_t ParseU64Option(const EngineUpdateRowsRequest& request,
                             const std::string& prefix,
                             std::uint64_t fallback) {
  const auto value = UpdateBatchOptionValue(request, prefix);
  if (value.empty()) {
    return fallback;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

UpdateIndexMaintenanceAction ActionForIndex(const CrudIndexRecord& index,
                                            bool affected,
                                            bool delta_ledger_enabled) {
  if (!affected) {
    return UpdateIndexMaintenanceAction::unaffected;
  }
  if (IsUniqueIndex(index)) {
    return UpdateIndexMaintenanceAction::synchronous_exact_probe_then_rewrite;
  }
  if (IsDeltaEligibleFamily(index) && delta_ledger_enabled) {
    return UpdateIndexMaintenanceAction::committed_delta_ledger;
  }
  if (index.family == kCrudIndexFamilyBtree ||
      index.family == kCrudIndexFamilyHash ||
      index.family == kCrudIndexFamilyBitmap ||
      index.family.empty()) {
    return UpdateIndexMaintenanceAction::synchronous_exact_rewrite;
  }
  if (index.family == kCrudIndexFamilyVectorHnsw || index.family == kCrudIndexFamilyVectorIvf ||
      index.family == kCrudIndexFamilyFullText || index.family == kCrudIndexFamilySpatial ||
      index.family == kCrudIndexFamilyColumnarZone || index.family == kCrudIndexFamilyExpression ||
      index.family == kCrudIndexFamilyGraphAdjacency || index.family == kCrudIndexFamilyPartial ||
      index.family == kCrudIndexFamilyCovering || index.family == kCrudIndexFamilyInMemory ||
      index.family == kCrudIndexFamilyDonorEmulated) {
    return UpdateIndexMaintenanceAction::synchronous_exact_rewrite;
  }
  return UpdateIndexMaintenanceAction::reject_batch_path;
}

std::string ActionReason(UpdateIndexMaintenanceAction action) {
  switch (action) {
    case UpdateIndexMaintenanceAction::unaffected: return "assignment_does_not_touch_index";
    case UpdateIndexMaintenanceAction::synchronous_exact_rewrite: return "affected_index_exact_rewrite";
    case UpdateIndexMaintenanceAction::synchronous_exact_probe_then_rewrite: return "unique_index_exact_preflight_required";
    case UpdateIndexMaintenanceAction::committed_delta_ledger: return "non_unique_secondary_delta_ledger_enabled";
    case UpdateIndexMaintenanceAction::shadow_rebuild_then_cutover: return "shadow_rebuild_policy_enabled";
    case UpdateIndexMaintenanceAction::reject_batch_path: return "index_rejects_update_batch_path";
  }
  return "unknown";
}

std::uint64_t EstimateMatches(const EngineUpdateRowsRequest& request,
                              const CrudState& state,
                              const CrudTableRecord& table,
                              const std::vector<CrudIndexRecord>&) {
  if (request.update_predicate.predicate_kind == "row_uuid_match") {
    return 1;
  }
  return static_cast<std::uint64_t>(VisibleCrudRows(state, table.table_uuid, request.context.local_transaction_id).size());
}

std::uint64_t AssignmentBytes(const EngineUpdateRowsRequest& request) {
  std::uint64_t total = 0;
  for (const auto& assignment : request.assignments) {
    total += assignment.first.size();
    total += assignment.second.encoded_value.size();
    total += assignment.second.descriptor.canonical_type_name.size();
    total += assignment.second.descriptor.encoded_descriptor.size();
  }
  return total;
}

}  // namespace

const char* UpdateBatchModeName(UpdateBatchMode mode) {
  switch (mode) {
    case UpdateBatchMode::singleton_row_uuid: return "singleton_row_uuid";
    case UpdateBatchMode::predicate_scan: return "predicate_scan";
    case UpdateBatchMode::indexed_predicate: return "indexed_predicate";
    case UpdateBatchMode::donor_bulk: return "donor_bulk";
    case UpdateBatchMode::native_bulk: return "native_bulk";
  }
  return "unknown";
}

const char* UpdateFeatureStateName(UpdateFeatureState state) {
  switch (state) {
    case UpdateFeatureState::enabled: return "enabled";
    case UpdateFeatureState::disabled: return "disabled";
    case UpdateFeatureState::policy_required: return "policy_required";
    case UpdateFeatureState::refused: return "refused";
  }
  return "unknown";
}

const char* UpdateIndexMaintenanceActionName(UpdateIndexMaintenanceAction action) {
  switch (action) {
    case UpdateIndexMaintenanceAction::unaffected: return "unaffected";
    case UpdateIndexMaintenanceAction::synchronous_exact_rewrite: return "synchronous_exact_rewrite";
    case UpdateIndexMaintenanceAction::synchronous_exact_probe_then_rewrite: return "synchronous_exact_probe_then_rewrite";
    case UpdateIndexMaintenanceAction::committed_delta_ledger: return "committed_delta_ledger";
    case UpdateIndexMaintenanceAction::shadow_rebuild_then_cutover: return "shadow_rebuild_then_cutover";
    case UpdateIndexMaintenanceAction::reject_batch_path: return "reject_batch_path";
  }
  return "unknown";
}

UpdateBatchMode ResolveUpdateBatchMode(const EngineUpdateRowsRequest& request,
                                       const CrudState&,
                                       const CrudTableRecord&,
                                       const std::vector<CrudIndexRecord>& indexes) {
  if (request.update_predicate.predicate_kind == "row_uuid_match") {
    return UpdateBatchMode::singleton_row_uuid;
  }
  if (request.update_predicate.predicate_kind == "donor_bulk") {
    return UpdateBatchMode::donor_bulk;
  }
  if (request.update_predicate.predicate_kind == "native_bulk") {
    return UpdateBatchMode::native_bulk;
  }
  for (const auto& index : indexes) {
    if (CrudIndexSupportsPredicate(index, request.update_predicate)) {
      return UpdateBatchMode::indexed_predicate;
    }
  }
  return UpdateBatchMode::predicate_scan;
}

UpdateFeatureGates ResolveUpdateFeatureGates(const EngineUpdateRowsRequest& request) {
  UpdateFeatureGates gates;
  if (UpdateBatchOptionEnabled(request, "feature.page_reservation=disabled")) {
    gates.page_reservation = UpdateFeatureState::disabled;
  }
  // DPC_DEFERRED_INDEX_FEATURE_FLAG
  if (idx::DeferredIndexOptionEnabled(request.option_envelopes,
                                      idx::kDeferredSecondaryIndexRuntimeOption)) {
    gates.deferred_secondary_index_runtime = UpdateFeatureState::enabled;
  }
  if (idx::DeferredIndexOptionEnabled(request.option_envelopes,
                                      idx::kSecondaryIndexDeltaLedgerFeatureOption)) {
    gates.secondary_index_delta_ledger = UpdateFeatureState::enabled;
  }
  if (UpdateBatchOptionEnabled(request, "feature.physical_cow_bridge=enabled")) {
    gates.physical_cow_bridge = UpdateFeatureState::enabled;
  }
  return gates;
}

UpdateBatchMemoryPolicy ResolveUpdateMemoryPolicy(const EngineUpdateRowsRequest& request) {
  UpdateBatchMemoryPolicy policy;
  policy.context_budget_bytes = ParseU64Option(request, "memory.context_budget_bytes=", policy.context_budget_bytes);
  policy.assignment_budget_bytes = ParseU64Option(request, "memory.assignment_budget_bytes=", policy.assignment_budget_bytes);
  policy.unique_preflight_budget_bytes = ParseU64Option(request, "memory.unique_preflight_budget_bytes=", policy.unique_preflight_budget_bytes);
  policy.delta_ledger_stage_budget_bytes = ParseU64Option(request, "memory.delta_ledger_stage_budget_bytes=", policy.delta_ledger_stage_budget_bytes);
  policy.spill_allowed = UpdateBatchOptionEnabled(request, "memory.spill_allowed=true");
  return policy;
}

BoundUpdateAssignmentTemplate BuildBoundUpdateAssignmentTemplate(const EngineUpdateRowsRequest& request,
                                                                const CrudTableRecord& table) {
  BoundUpdateAssignmentTemplate assignment_template;
  assignment_template.table_uuid = table.table_uuid;
  assignment_template.columns = table.columns;
  assignment_template.assignment_count = request.assignments.size();
  assignment_template.template_id = MakeId("update_template", table.table_uuid + ":" + std::to_string(request.assignments.size()));
  for (const auto& assignment : request.assignments) {
    assignment_template.assigned_columns.push_back(assignment.first);
    if (CrudColumnDescriptorIsOpaqueRenderOnly(CrudColumnDescriptorForName(table.columns, assignment.first)) ||
        CrudColumnDescriptorIsOpaqueRenderOnly(assignment.second.descriptor.canonical_type_name) ||
        CrudColumnDescriptorIsOpaqueRenderOnly(assignment.second.descriptor.encoded_descriptor)) {
      assignment_template.touches_opaque_render_only_column = true;
    }
    if (assignment.second.encoded_value.size() > assignment_template.max_inline_encoded_bytes) {
      assignment_template.toast_required = true;
    }
  }
  return assignment_template;
}

UpdateIndexMaintenancePlan BuildUpdateIndexMaintenancePlan(const EngineUpdateRowsRequest& request,
                                                           const CrudState&,
                                                           const CrudTableRecord& table,
                                                           const std::vector<CrudIndexRecord>& indexes,
                                                           const UpdateFeatureGates&,
                                                           const UpdateSecondaryIndexDeltaLedgerPolicy& delta_ledger_policy) {
  const auto assignment_template = BuildBoundUpdateAssignmentTemplate(request, table);
  UpdateIndexMaintenancePlan plan;
  plan.table_uuid = table.table_uuid;
  plan.plan_id = MakeId("update_index_plan", table.table_uuid + ":" + std::to_string(indexes.size()) + ":" +
                                             std::to_string(assignment_template.assignment_count));
  for (const auto& index : indexes) {
    const bool affected = IndexAffectedByAssignments(index, assignment_template.assigned_columns);
    UpdateIndexMaintenancePlanEntry entry;
    entry.index = index;
    entry.key_or_predicate_affected = affected;
    entry.action = ActionForIndex(index, affected, delta_ledger_policy.enabled);
    entry.reason = ActionReason(entry.action);
    if (affected && IsUniqueIndex(index)) {
      plan.has_affected_unique_exact = true;
    }
    if (entry.action == UpdateIndexMaintenanceAction::committed_delta_ledger) {
      plan.has_delta_eligible = true;
    }
    if (entry.action == UpdateIndexMaintenanceAction::reject_batch_path) {
      plan.rejected = true;
      plan.rejection_reason = entry.reason;
    }
    plan.entries.push_back(std::move(entry));
  }
  return plan;
}

UpdatePageReservationPlan ReserveUpdatePages(const EngineUpdateRowsRequest& request,
                                             const BoundUpdateAssignmentTemplate&,
                                             std::uint64_t estimated_matches) {
  UpdatePageReservationPlan plan;
  plan.estimated_rows = estimated_matches;
  plan.requested_pages = std::max<std::uint64_t>(1, (estimated_matches + 127) / 128);
  plan.reservation_id = MakeId("update_page_reservation", request.target_table.uuid.canonical + ":" + std::to_string(plan.requested_pages));
  if (ResolveUpdateFeatureGates(request).page_reservation != UpdateFeatureState::enabled) {
    plan.reservation_available = false;
    plan.refusal_reason = "page_reservation_disabled";
  }
  return plan;
}

UpdateSecondaryIndexDeltaLedgerPolicy ResolveUpdateSecondaryIndexDeltaLedgerPolicy(const EngineUpdateRowsRequest& request,
                                                                                  const UpdateFeatureGates& feature_gates) {
  UpdateSecondaryIndexDeltaLedgerPolicy policy;
  // DPC_DEFERRED_INDEX_FEATURE_FLAG_GATE
  const auto decision =
      idx::ResolveDeferredSecondaryIndexRuntimePolicy(request.option_envelopes);
  policy.enabled = decision.enabled &&
                   feature_gates.deferred_secondary_index_runtime == UpdateFeatureState::enabled &&
                   feature_gates.secondary_index_delta_ledger == UpdateFeatureState::enabled;
  policy.runtime_enabled = decision.runtime_enabled;
  policy.readers_overlay_committed_deltas = decision.readers_overlay_committed_deltas;
  policy.cleanup_horizon_bound = decision.cleanup_horizon_bound;
  policy.recovery_classifiable = decision.recovery_classifiable;
  policy.synchronous_fallback_required = !policy.enabled;
  policy.fallback_reason = policy.enabled ? std::string{} : decision.fallback_reason;
  return policy;
}

UpdateBatchContext BuildUpdateBatchContext(const EngineUpdateRowsRequest& request,
                                           const CrudState& state,
                                           const CrudTableRecord& table,
                                           const std::vector<CrudIndexRecord>& indexes) {
  UpdateBatchContext context;
  context.statement_uuid = request.context.request_id.empty() ? GenerateCrudEngineUuid("transaction") : request.context.request_id;
  context.local_transaction_id = request.context.local_transaction_id;
  context.transaction_uuid = request.context.transaction_uuid.canonical;
  context.database_uuid = request.context.database_uuid.canonical;
  context.target_object_uuid = request.target_table.uuid.canonical;
  context.estimated_match_count = EstimateMatches(request, state, table, indexes);
  context.update_mode = ResolveUpdateBatchMode(request, state, table, indexes);
  context.predicate_kind = request.update_predicate.predicate_kind.empty() ? "all_visible_rows" : request.update_predicate.predicate_kind;
  context.security_context_uuid = request.context.principal_uuid.canonical;
  context.policy_snapshot_uuid = UpdateBatchOptionValue(request, "policy_snapshot_uuid=");
  context.feature_gates = ResolveUpdateFeatureGates(request);
  context.memory_policy = ResolveUpdateMemoryPolicy(request);
  context.assignment_template = BuildBoundUpdateAssignmentTemplate(request, table);
  context.page_reservation = ReserveUpdatePages(request, context.assignment_template, context.estimated_match_count);
  context.delta_ledger_policy = ResolveUpdateSecondaryIndexDeltaLedgerPolicy(request, context.feature_gates);
  context.index_plan = BuildUpdateIndexMaintenancePlan(request, state, table, indexes, context.feature_gates, context.delta_ledger_policy);
  context.accepted = request.context.local_transaction_id != 0 &&
                     !request.target_table.uuid.canonical.empty() &&
                     !request.assignments.empty() &&
                     !context.assignment_template.touches_opaque_render_only_column &&
                     !context.index_plan.rejected &&
                     context.page_reservation.reservation_available;
  if (!context.accepted) {
    if (request.context.local_transaction_id == 0) {
      context.fallback_reason = "local_transaction_id_required";
    } else if (request.target_table.uuid.canonical.empty()) {
      context.fallback_reason = "target_table_uuid_required";
    } else if (request.assignments.empty()) {
      context.fallback_reason = "at_least_one_assignment_required";
    } else if (context.assignment_template.touches_opaque_render_only_column) {
      context.fallback_reason = "opaque_column_mutation_denied";
    } else if (!context.index_plan.rejection_reason.empty()) {
      context.fallback_reason = context.index_plan.rejection_reason;
    } else {
      context.fallback_reason = context.page_reservation.refusal_reason.empty() ? "update_batch_refused" : context.page_reservation.refusal_reason;
    }
  }
  AddUpdateTrace(&context, "update.batch.begin", "begin", UpdateBatchModeName(context.update_mode));
  AddUpdateTrace(&context, "update.assignment.bind", "bind", context.assignment_template.template_id);
  AddUpdateTrace(&context, "update.page.reserve", "reserve", context.page_reservation.reservation_id);
  context.evidence.push_back({"update_batch_context", context.statement_uuid});
  context.evidence.push_back({"update_assignment_template", context.assignment_template.template_id});
  context.evidence.push_back({"update_index_plan", context.index_plan.plan_id});
  if (context.page_reservation.reservation_available) {
    context.evidence.push_back({"page_reservation", context.page_reservation.reservation_id});
  }
  if (context.delta_ledger_policy.synchronous_fallback_required) {
    context.evidence.push_back({"update_index_maintenance_mode", "synchronous_fallback"});
    context.evidence.push_back({"update_deferred_secondary_index_fallback_reason",
                                context.delta_ledger_policy.fallback_reason});
  } else {
    context.evidence.push_back({"update_index_maintenance_mode", "deferred_secondary_index"});
  }
  return context;
}

EngineApiDiagnostic ValidateUpdateBatchMemoryBudget(const UpdateBatchContext& context,
                                                    std::uint64_t projected_bytes) {
  if (projected_bytes > context.memory_policy.context_budget_bytes && !context.memory_policy.spill_allowed) {
    return MakeInvalidRequestDiagnostic("dml.update_rows", "update_batch_memory_budget_exceeded");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateUpdateBatchUniquePreflight(UpdateBatchContext* context,
                                                       const std::vector<std::pair<std::string, std::string>>& values,
                                                       const std::string& row_uuid) {
  (void)row_uuid;
  if (context == nullptr) {
    return MakeInvalidRequestDiagnostic("dml.update_rows", "update_batch_context_required");
  }
  for (const auto& entry : context->index_plan.entries) {
    if (!entry.key_or_predicate_affected || !IsUniqueIndex(entry.index)) {
      continue;
    }
    for (const auto& key : CrudIndexKeysForValues(entry.index, values)) {
      const std::string request_key = entry.index.index_uuid + "|" + key;
      if (!context->unique_request_keys.insert(request_key).second) {
        return MakeInvalidRequestDiagnostic("dml.update_rows", "unique_index_duplicate");
      }
    }
  }
  return OkDiagnostic();
}

void AddUpdateTrace(UpdateBatchContext* context, std::string event_name, std::string phase, std::string detail) {
  if (context == nullptr) {
    return;
  }
  context->trace_events.push_back({std::move(event_name), std::move(phase), std::move(detail)});
}

void AddUpdateBatchEvidenceToResult(const UpdateBatchContext& context, EngineApiResult* result) {
  if (result == nullptr) {
    return;
  }
  for (const auto& evidence : context.evidence) {
    result->evidence.push_back(evidence);
  }
  for (const auto& trace : context.trace_events) {
    result->evidence.push_back({"update_trace", trace.event_name + ":" + trace.phase + ":" + trace.detail});
  }
  result->evidence.push_back({"update_feature_gate.secondary_index_delta_ledger", UpdateFeatureStateName(context.feature_gates.secondary_index_delta_ledger)});
  result->evidence.push_back({"update_runtime.deferred_secondary_index", context.delta_ledger_policy.runtime_enabled ? "enabled" : "disabled"});
  result->evidence.push_back({"update_delta_ledger_policy", context.delta_ledger_policy.enabled ? "enabled" : "synchronous_fallback"});
  if (context.delta_ledger_policy.synchronous_fallback_required) {
    result->evidence.push_back({"update_deferred_secondary_index_fallback_reason",
                                context.delta_ledger_policy.fallback_reason});
  }
  result->evidence.push_back({"update_feature_gate.physical_cow_bridge", UpdateFeatureStateName(context.feature_gates.physical_cow_bridge)});
  if (!context.fallback_reason.empty()) {
    result->evidence.push_back({"update_fallback_reason", context.fallback_reason});
  }
}

void RecordUpdateBatchMetric(const UpdateBatchContext& context, std::string metric, double value, std::string result, std::string reason) {
  (void)scratchbird::core::metrics::IncrementCounter(
      std::move(metric),
      scratchbird::core::metrics::Labels({{"component", "engine.update"},
                                          {"object_uuid", context.target_object_uuid},
                                          {"operation", UpdateBatchModeName(context.update_mode)},
                                          {"result", std::move(result)},
                                          {"reason", reason.empty() ? "none" : std::move(reason)}}),
      value,
      kUpdateMetricsProducer);
}

}  // namespace scratchbird::engine::internal_api
