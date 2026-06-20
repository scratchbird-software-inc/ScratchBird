// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/delete_batch.hpp"

#include "api_diagnostics.hpp"
#include "deferred_secondary_index_runtime_policy.hpp"
#include "metric_producer.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace idx = scratchbird::core::index;

constexpr const char* kDeleteMetricsProducer = "engine_delete";

std::string MakeId(const std::string& prefix, const std::string& stable) {
  return prefix + ":" + stable;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

// DPC_UNIQUE_INDEX_DEFERRAL_POLICY
bool IsUniqueIndex(const CrudIndexRecord& index) {
  return index.unique ||
         std::find(index.key_envelopes.begin(), index.key_envelopes.end(), "unique") !=
             index.key_envelopes.end();
}

bool IsDeltaEligibleFamily(const CrudIndexRecord& index) {
  if (IsUniqueIndex(index)) {
    return false;
  }
  return index.family == kCrudIndexFamilyBtree ||
         index.family == kCrudIndexFamilyHash ||
         index.family == kCrudIndexFamilyBitmap ||
         index.family.empty();
}

bool PredicateEnvelopeTouchesColumn(const EnginePredicateEnvelope& predicate, const std::string& column_name) {
  if (column_name.empty()) {
    return false;
  }
  if (predicate.canonical_predicate_envelope == column_name) {
    return true;
  }
  const auto& envelope = predicate.canonical_predicate_envelope;
  if ((envelope.rfind("lower:", 0) == 0 || envelope.rfind("upper:", 0) == 0) && envelope.substr(6) == column_name) {
    return true;
  }
  if (envelope.rfind("length:", 0) == 0 && envelope.substr(7) == column_name) {
    return true;
  }
  if (envelope.rfind("identity:", 0) == 0 && envelope.substr(9) == column_name) {
    return true;
  }
  const auto open = envelope.find('(');
  const auto close = envelope.size() > open ? envelope.rfind(')') : std::string::npos;
  if (open != std::string::npos && close != std::string::npos && close > open + 1) {
    return envelope.substr(open + 1, close - open - 1) == column_name;
  }
  return false;
}

std::vector<std::string> PredicateTouchedColumns(const EngineDeleteRowsRequest& request, const CrudTableRecord& table) {
  std::vector<std::string> columns;
  if (request.delete_predicate.predicate_kind.empty() || request.delete_predicate.predicate_kind == "row_uuid_match") {
    return columns;
  }
  for (const auto& column : table.columns) {
    if (PredicateEnvelopeTouchesColumn(request.delete_predicate, column.first)) {
      columns.push_back(column.first);
    }
  }
  if (columns.empty() && !request.delete_predicate.canonical_predicate_envelope.empty()) {
    columns.push_back(request.delete_predicate.canonical_predicate_envelope);
  }
  return columns;
}

DeleteIndexMaintenanceAction ActionForIndex(const CrudIndexRecord& index, const DeleteFeatureGates& feature_gates) {
  if (IsDeltaEligibleFamily(index) && feature_gates.secondary_index_delta_ledger == DeleteFeatureState::enabled) {
    return DeleteIndexMaintenanceAction::tombstone_delta_ledger;
  }
  if (index.family == kCrudIndexFamilyBtree || index.family == kCrudIndexFamilyHash || index.family == kCrudIndexFamilyBitmap ||
      index.family == kCrudIndexFamilyFullText || index.family == kCrudIndexFamilySpatial ||
      index.family == kCrudIndexFamilyVectorExact || index.family == kCrudIndexFamilyVectorHnsw ||
      index.family == kCrudIndexFamilyVectorIvf || index.family == kCrudIndexFamilyColumnarZone ||
      index.family == kCrudIndexFamilyGraphAdjacency || index.family == kCrudIndexFamilyExpression ||
      index.family == kCrudIndexFamilyPartial || index.family == kCrudIndexFamilyCovering ||
      index.family == kCrudIndexFamilyInMemory || index.family == kCrudIndexFamilyReferenceEmulated ||
      index.family == kCrudIndexFamilyPolicyBlocked ||
      index.family.empty()) {
    return DeleteIndexMaintenanceAction::visibility_recheck_only;
  }
  return DeleteIndexMaintenanceAction::reject_batch_path;
}

std::string ActionReason(DeleteIndexMaintenanceAction action) {
  switch (action) {
    case DeleteIndexMaintenanceAction::visibility_recheck_only: return "tombstone_visibility_recheck_suppresses_stale_entries";
    case DeleteIndexMaintenanceAction::tombstone_delta_ledger: return "secondary_index_tombstone_delta_ledger_enabled";
    case DeleteIndexMaintenanceAction::synchronous_tombstone_rewrite: return "synchronous_tombstone_index_rewrite";
    case DeleteIndexMaintenanceAction::shadow_rebuild_then_cutover: return "shadow_rebuild_policy_enabled";
    case DeleteIndexMaintenanceAction::reject_batch_path: return "index_rejects_delete_batch_path";
  }
  return "unknown";
}

std::uint64_t EstimateMatches(const EngineDeleteRowsRequest& request,
                              const CrudState& state,
                              const CrudTableRecord& table,
                              const std::vector<CrudIndexRecord>&) {
  if (request.delete_predicate.predicate_kind == "row_uuid_match") {
    return 1;
  }
  return static_cast<std::uint64_t>(VisibleCrudRows(state, table.table_uuid, request.context.local_transaction_id).size());
}

std::uint64_t PredicateBytes(const EngineDeleteRowsRequest& request) {
  std::uint64_t total = request.delete_predicate.predicate_kind.size() + request.delete_predicate.canonical_predicate_envelope.size();
  for (const auto& value : request.delete_predicate.bound_values) {
    total += value.encoded_value.size();
    total += value.descriptor.canonical_type_name.size();
    total += value.descriptor.encoded_descriptor.size();
  }
  return total;
}

}  // namespace

const char* DeleteBatchModeName(DeleteBatchMode mode) {
  switch (mode) {
    case DeleteBatchMode::singleton_row_uuid: return "singleton_row_uuid";
    case DeleteBatchMode::predicate_scan: return "predicate_scan";
    case DeleteBatchMode::indexed_predicate: return "indexed_predicate";
    case DeleteBatchMode::reference_bulk: return "reference_bulk";
    case DeleteBatchMode::native_bulk: return "native_bulk";
  }
  return "unknown";
}

const char* DeleteFeatureStateName(DeleteFeatureState state) {
  switch (state) {
    case DeleteFeatureState::enabled: return "enabled";
    case DeleteFeatureState::disabled: return "disabled";
    case DeleteFeatureState::policy_required: return "policy_required";
    case DeleteFeatureState::refused: return "refused";
  }
  return "unknown";
}

const char* DeleteIndexMaintenanceActionName(DeleteIndexMaintenanceAction action) {
  switch (action) {
    case DeleteIndexMaintenanceAction::visibility_recheck_only: return "visibility_recheck_only";
    case DeleteIndexMaintenanceAction::tombstone_delta_ledger: return "tombstone_delta_ledger";
    case DeleteIndexMaintenanceAction::synchronous_tombstone_rewrite: return "synchronous_tombstone_rewrite";
    case DeleteIndexMaintenanceAction::shadow_rebuild_then_cutover: return "shadow_rebuild_then_cutover";
    case DeleteIndexMaintenanceAction::reject_batch_path: return "reject_batch_path";
  }
  return "unknown";
}

DeleteBatchMode ResolveDeleteBatchMode(const EngineDeleteRowsRequest& request,
                                       const CrudState&,
                                       const CrudTableRecord&,
                                       const std::vector<CrudIndexRecord>& indexes) {
  if (request.delete_predicate.predicate_kind == "row_uuid_match") {
    return DeleteBatchMode::singleton_row_uuid;
  }
  if (request.delete_predicate.predicate_kind == "reference_bulk") {
    return DeleteBatchMode::reference_bulk;
  }
  if (request.delete_predicate.predicate_kind == "native_bulk") {
    return DeleteBatchMode::native_bulk;
  }
  for (const auto& index : indexes) {
    if (CrudIndexSupportsPredicate(index, request.delete_predicate)) {
      return DeleteBatchMode::indexed_predicate;
    }
  }
  return DeleteBatchMode::predicate_scan;
}

DeleteFeatureGates ResolveDeleteFeatureGates(const EngineDeleteRowsRequest& request) {
  DeleteFeatureGates gates;
  // DPC_DEFERRED_INDEX_FEATURE_FLAG
  if (idx::DeferredIndexOptionEnabled(request.option_envelopes,
                                      idx::kDeferredSecondaryIndexRuntimeOption)) {
    gates.deferred_secondary_index_runtime = DeleteFeatureState::enabled;
  }
  if (idx::DeferredIndexOptionEnabled(request.option_envelopes,
                                      idx::kSecondaryIndexDeltaLedgerFeatureOption)) {
    gates.secondary_index_delta_ledger = DeleteFeatureState::enabled;
  }
  if (idx::DeferredIndexOptionEnabled(request.option_envelopes,
                                      "feature.physical_cow_bridge=enabled")) {
    gates.physical_cow_bridge = DeleteFeatureState::enabled;
  }
  return gates;
}

DeleteBatchMemoryPolicy ResolveDeleteMemoryPolicy(const EngineDeleteRowsRequest&) {
  return {};
}

BoundDeletePredicateTemplate BuildBoundDeletePredicateTemplate(const EngineDeleteRowsRequest& request,
                                                              const CrudTableRecord& table) {
  BoundDeletePredicateTemplate predicate_template;
  predicate_template.table_uuid = table.table_uuid;
  predicate_template.predicate_kind = request.delete_predicate.predicate_kind.empty() ? "all_visible_rows" : request.delete_predicate.predicate_kind;
  predicate_template.predicate_envelope = request.delete_predicate.canonical_predicate_envelope;
  predicate_template.row_uuid_singleton = request.delete_predicate.predicate_kind == "row_uuid_match";
  predicate_template.touched_columns = PredicateTouchedColumns(request, table);
  predicate_template.template_id = MakeId("delete_predicate_template",
                                          table.table_uuid + ":" + predicate_template.predicate_kind + ":" +
                                              predicate_template.predicate_envelope);
  for (const auto& column : table.columns) {
    if (!CrudColumnDescriptorIsOpaqueRenderOnly(column.second)) {
      continue;
    }
    if (PredicateEnvelopeTouchesColumn(request.delete_predicate, column.first)) {
      predicate_template.touches_opaque_render_only_column = true;
    }
  }
  for (const auto& value : request.delete_predicate.bound_values) {
    if (CrudColumnDescriptorIsOpaqueRenderOnly(value.descriptor.canonical_type_name) ||
        CrudColumnDescriptorIsOpaqueRenderOnly(value.descriptor.encoded_descriptor)) {
      predicate_template.touches_opaque_render_only_column = true;
    }
  }
  return predicate_template;
}

DeleteIndexMaintenancePlan BuildDeleteIndexMaintenancePlan(const EngineDeleteRowsRequest&,
                                                           const CrudState&,
                                                           const CrudTableRecord& table,
                                                           const std::vector<CrudIndexRecord>& indexes,
                                                           const DeleteFeatureGates& feature_gates) {
  DeleteIndexMaintenancePlan plan;
  plan.table_uuid = table.table_uuid;
  plan.plan_id = MakeId("delete_index_plan", table.table_uuid + ":" + std::to_string(indexes.size()));
  for (const auto& index : indexes) {
    DeleteIndexMaintenancePlanEntry entry;
    entry.index = index;
    entry.action = ActionForIndex(index, feature_gates);
    entry.reason = ActionReason(entry.action);
    if (entry.action == DeleteIndexMaintenanceAction::tombstone_delta_ledger) {
      plan.has_delta_eligible = true;
    }
    if (entry.action == DeleteIndexMaintenanceAction::reject_batch_path) {
      plan.rejected = true;
      plan.rejection_reason = entry.reason;
    }
    plan.entries.push_back(std::move(entry));
  }
  return plan;
}

DeletePageReclamationPlan BuildDeletePageReclamationPlan(const EngineDeleteRowsRequest& request,
                                                         std::uint64_t estimated_matches,
                                                         const DeleteFeatureGates& feature_gates) {
  DeletePageReclamationPlan plan;
  plan.estimated_rows = estimated_matches;
  plan.notify_page_agent_after_tombstones = std::max<std::uint64_t>(1, estimated_matches / 2);
  plan.plan_id = MakeId("delete_reclamation_notice", request.target_table.uuid.canonical + ":" + std::to_string(estimated_matches));
  plan.page_reclamation_notice_enabled = feature_gates.page_reclamation_notice == DeleteFeatureState::enabled;
  if (!plan.page_reclamation_notice_enabled) {
    plan.refusal_reason = "page_reclamation_notice_disabled";
  }
  return plan;
}

DeleteSecondaryIndexDeltaLedgerPolicy ResolveDeleteSecondaryIndexDeltaLedgerPolicy(const EngineDeleteRowsRequest& request,
                                                                                  const DeleteFeatureGates& feature_gates) {
  DeleteSecondaryIndexDeltaLedgerPolicy policy;
  // DPC_DEFERRED_INDEX_FEATURE_FLAG_GATE
  const auto decision =
      idx::ResolveDeferredSecondaryIndexRuntimePolicy(request.option_envelopes);
  policy.enabled = decision.enabled &&
                   feature_gates.deferred_secondary_index_runtime == DeleteFeatureState::enabled &&
                   feature_gates.secondary_index_delta_ledger == DeleteFeatureState::enabled;
  policy.runtime_enabled = decision.runtime_enabled;
  policy.readers_overlay_committed_deltas = decision.readers_overlay_committed_deltas;
  policy.cleanup_horizon_bound = decision.cleanup_horizon_bound;
  policy.recovery_classifiable = decision.recovery_classifiable;
  policy.synchronous_fallback_required = !policy.enabled;
  policy.fallback_reason = policy.enabled ? std::string{} : decision.fallback_reason;
  return policy;
}

DeleteBatchContext BuildDeleteBatchContext(const EngineDeleteRowsRequest& request,
                                           const CrudState& state,
                                           const CrudTableRecord& table,
                                           const std::vector<CrudIndexRecord>& indexes) {
  DeleteBatchContext context;
  context.statement_uuid = request.context.request_id.empty() ? GenerateCrudEngineUuid("transaction") : request.context.request_id;
  context.local_transaction_id = request.context.local_transaction_id;
  context.transaction_uuid = request.context.transaction_uuid.canonical;
  context.database_uuid = request.context.database_uuid.canonical;
  context.target_object_uuid = request.target_table.uuid.canonical;
  context.estimated_match_count = EstimateMatches(request, state, table, indexes);
  context.delete_mode = ResolveDeleteBatchMode(request, state, table, indexes);
  context.security_context_uuid = request.context.principal_uuid.canonical;
  context.policy_snapshot_uuid = {};
  context.feature_gates = ResolveDeleteFeatureGates(request);
  context.memory_policy = ResolveDeleteMemoryPolicy(request);
  context.predicate_template = BuildBoundDeletePredicateTemplate(request, table);
  context.delta_ledger_policy = ResolveDeleteSecondaryIndexDeltaLedgerPolicy(request, context.feature_gates);
  auto effective_feature_gates = context.feature_gates;
  if (!context.delta_ledger_policy.enabled) {
    effective_feature_gates.secondary_index_delta_ledger = DeleteFeatureState::disabled;
  }
  context.index_plan = BuildDeleteIndexMaintenancePlan(request, state, table, indexes, effective_feature_gates);
  context.reclamation_plan = BuildDeletePageReclamationPlan(request, context.estimated_match_count, context.feature_gates);
  context.tombstone_only = request.tombstone_only;
  context.accepted = request.context.local_transaction_id != 0 &&
                     !request.target_table.uuid.canonical.empty() &&
                     request.tombstone_only &&
                     !context.predicate_template.touches_opaque_render_only_column &&
                     !context.index_plan.rejected;
  if (!context.accepted) {
    if (request.context.local_transaction_id == 0) {
      context.fallback_reason = "local_transaction_id_required";
    } else if (request.target_table.uuid.canonical.empty()) {
      context.fallback_reason = "target_table_uuid_required";
    } else if (!request.tombstone_only) {
      context.fallback_reason = "physical_delete_requires_policy";
    } else if (context.predicate_template.touches_opaque_render_only_column) {
      context.fallback_reason = "opaque_column_comparison_denied";
    } else if (!context.index_plan.rejection_reason.empty()) {
      context.fallback_reason = context.index_plan.rejection_reason;
    } else {
      context.fallback_reason = "delete_batch_refused";
    }
  }
  AddDeleteTrace(&context, "delete.batch.begin", "begin", DeleteBatchModeName(context.delete_mode));
  AddDeleteTrace(&context, "delete.predicate.bind", "bind", context.predicate_template.template_id);
  AddDeleteTrace(&context, "delete.reclamation.plan", "plan", context.reclamation_plan.plan_id);
  context.evidence.push_back({"delete_batch_context", context.statement_uuid});
  context.evidence.push_back({"delete_predicate_template", context.predicate_template.template_id});
  context.evidence.push_back({"delete_index_plan", context.index_plan.plan_id});
  context.evidence.push_back({"delete_reclamation_plan", context.reclamation_plan.plan_id});
  if (context.delta_ledger_policy.synchronous_fallback_required) {
    context.evidence.push_back({"delete_index_maintenance_mode", "synchronous_fallback"});
    context.evidence.push_back({"delete_deferred_secondary_index_fallback_reason",
                                context.delta_ledger_policy.fallback_reason});
  } else {
    context.evidence.push_back({"delete_index_maintenance_mode", "deferred_secondary_index"});
  }
  return context;
}

EngineApiDiagnostic ValidateDeleteBatchMemoryBudget(const DeleteBatchContext& context,
                                                    std::uint64_t projected_bytes) {
  if (projected_bytes > context.memory_policy.context_budget_bytes && !context.memory_policy.spill_allowed) {
    return MakeInvalidRequestDiagnostic("dml.delete_rows", "delete_batch_memory_budget_exceeded");
  }
  if (projected_bytes > context.memory_policy.predicate_budget_bytes && !context.memory_policy.spill_allowed) {
    return MakeInvalidRequestDiagnostic("dml.delete_rows", "delete_predicate_memory_budget_exceeded");
  }
  return OkDiagnostic();
}

void AddDeleteTrace(DeleteBatchContext* context, std::string event_name, std::string phase, std::string detail) {
  if (context == nullptr) {
    return;
  }
  context->trace_events.push_back({std::move(event_name), std::move(phase), std::move(detail)});
}

void AddDeleteBatchEvidenceToResult(const DeleteBatchContext& context, EngineApiResult* result) {
  if (result == nullptr) {
    return;
  }
  for (const auto& evidence : context.evidence) {
    result->evidence.push_back(evidence);
  }
  for (const auto& trace : context.trace_events) {
    result->evidence.push_back({"delete_trace", trace.event_name + ":" + trace.phase + ":" + trace.detail});
  }
  result->evidence.push_back({"delete_feature_gate.secondary_index_delta_ledger", DeleteFeatureStateName(context.feature_gates.secondary_index_delta_ledger)});
  result->evidence.push_back({"delete_runtime.deferred_secondary_index", context.delta_ledger_policy.runtime_enabled ? "enabled" : "disabled"});
  result->evidence.push_back({"delete_delta_ledger_policy", context.delta_ledger_policy.enabled ? "enabled" : "synchronous_fallback"});
  if (context.delta_ledger_policy.synchronous_fallback_required) {
    result->evidence.push_back({"delete_deferred_secondary_index_fallback_reason",
                                context.delta_ledger_policy.fallback_reason});
  }
  result->evidence.push_back({"delete_feature_gate.physical_cow_bridge", DeleteFeatureStateName(context.feature_gates.physical_cow_bridge)});
  result->evidence.push_back({"delete_tombstone_only", context.tombstone_only ? "true" : "false"});
  if (!context.fallback_reason.empty()) {
    result->evidence.push_back({"delete_fallback_reason", context.fallback_reason});
  }
}

void RecordDeleteBatchMetric(const DeleteBatchContext& context, std::string metric, double value, std::string result, std::string reason) {
  (void)scratchbird::core::metrics::IncrementCounter(
      std::move(metric),
      scratchbird::core::metrics::Labels({{"component", "engine.delete"},
                                          {"object_uuid", context.target_object_uuid},
                                          {"operation", DeleteBatchModeName(context.delete_mode)},
                                          {"result", std::move(result)},
                                          {"reason", reason.empty() ? "none" : std::move(reason)}}),
      value,
      kDeleteMetricsProducer);
}

}  // namespace scratchbird::engine::internal_api
