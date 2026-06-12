// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/insert_batch.hpp"

#include "api_diagnostics.hpp"
#include "deferred_secondary_index_runtime_policy.hpp"
#include "metric_contracts.hpp"
#include "metric_producer.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace idx = scratchbird::core::index;

constexpr const char* kInsertMetricsProducer = "engine_insert";

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool OptionEquals(const std::string& actual, const std::string& expected) {
  if (actual == expected) {
    return true;
  }
  const auto equals = expected.find('=');
  if (equals == std::string::npos) {
    return false;
  }
  return actual == expected.substr(0, equals) + ":" + expected.substr(equals + 1);
}

std::size_t OptionValueOffset(const std::string& actual, const std::string& prefix) {
  if (StartsWith(actual, prefix)) {
    return prefix.size();
  }
  if (!prefix.empty() && prefix.back() == '=') {
    const std::string colon_prefix = prefix.substr(0, prefix.size() - 1) + ":";
    if (StartsWith(actual, colon_prefix)) {
      return colon_prefix.size();
    }
  }
  return std::string::npos;
}

std::string MakeId(const std::string& prefix, const std::string& stable) {
  return prefix + ":" + stable;
}

std::uint64_t EstimateRows(const EngineInsertRowsRequest& request) {
  if (request.estimated_row_count != 0) {
    return request.estimated_row_count;
  }
  return request.EffectiveInputRows().size();
}

std::string TargetUuid(const EngineInsertRowsRequest& request) {
  if (!request.target_table.uuid.canonical.empty()) {
    return request.target_table.uuid.canonical;
  }
  return request.target_object.uuid.canonical;
}

bool IsUniqueIndex(const CrudIndexRecord& index) {
  return index.unique || std::find(index.key_envelopes.begin(), index.key_envelopes.end(), "unique") != index.key_envelopes.end();
}

bool IsDeltaEligibleFamily(const CrudIndexRecord& index) {
  if (IsUniqueIndex(index)) {
    return false;
  }
  return index.family == kCrudIndexFamilyBtree ||
         index.family == kCrudIndexFamilyHash ||
         index.family.empty();
}

InsertIndexMaintenanceAction ActionForIndex(const CrudIndexRecord& index,
                                            bool delta_ledger_enabled,
                                            const InsertFeatureGates& feature_gates) {
  if (IsUniqueIndex(index)) {
    return InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert;
  }
  if (IsDeltaEligibleFamily(index) && delta_ledger_enabled) {
    return InsertIndexMaintenanceAction::committed_delta_ledger;
  }
  if (feature_gates.sorted_run_shadow_load == InsertFeatureState::enabled &&
      (index.family == kCrudIndexFamilyBtree || index.family == kCrudIndexFamilyHash)) {
    return InsertIndexMaintenanceAction::sorted_run_build;
  }
  if (index.family == kCrudIndexFamilyVectorHnsw ||
      index.family == kCrudIndexFamilyVectorIvf ||
      index.family == kCrudIndexFamilyFullText ||
      index.family == kCrudIndexFamilySpatial ||
      index.family == kCrudIndexFamilyGraphAdjacency) {
    return InsertIndexMaintenanceAction::synchronous_exact_insert;
  }
  return InsertIndexMaintenanceAction::synchronous_exact_insert;
}

std::string ActionReason(InsertIndexMaintenanceAction action) {
  switch (action) {
    case InsertIndexMaintenanceAction::synchronous_exact_insert: return "synchronous_exact_default";
    case InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert: return "unique_exact_preflight_required";
    case InsertIndexMaintenanceAction::batch_local_buffer_then_insert: return "batch_local_buffer";
    case InsertIndexMaintenanceAction::committed_delta_ledger: return "non_unique_secondary_delta_ledger_enabled";
    case InsertIndexMaintenanceAction::sorted_run_build: return "sorted_run_policy_enabled";
    case InsertIndexMaintenanceAction::shadow_rebuild_then_cutover: return "shadow_rebuild_policy_enabled";
    case InsertIndexMaintenanceAction::reject_batch_path: return "index_rejects_batch_path";
  }
  return "unknown";
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool IsOkDiagnostic(const EngineApiDiagnostic& diagnostic) {
  return !diagnostic.error || diagnostic.code == "SB_ENGINE_API_OK";
}

std::uint64_t ParseU64Option(const EngineInsertRowsRequest& request,
                             const std::string& prefix,
                             std::uint64_t fallback) {
  const auto value = InsertBatchOptionValue(request, prefix);
  if (value.empty()) {
    return fallback;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

}  // namespace

const char* InsertBatchModeName(InsertBatchMode mode) {
  switch (mode) {
    case InsertBatchMode::singleton: return "singleton";
    case InsertBatchMode::multi_values: return "multi_values";
    case InsertBatchMode::insert_select: return "insert_select";
    case InsertBatchMode::copy_import: return "copy_import";
    case InsertBatchMode::reference_bulk: return "reference_bulk";
    case InsertBatchMode::native_bulk: return "native_bulk";
  }
  return "unknown";
}

const char* InsertDuplicateModeName(InsertDuplicateMode mode) {
  switch (mode) {
    case InsertDuplicateMode::error: return "error";
    case InsertDuplicateMode::ignore: return "ignore";
    case InsertDuplicateMode::replace: return "replace";
    case InsertDuplicateMode::update: return "update";
    case InsertDuplicateMode::merge_policy: return "merge_policy";
  }
  return "unknown";
}

const char* InsertFeatureStateName(InsertFeatureState state) {
  switch (state) {
    case InsertFeatureState::enabled: return "enabled";
    case InsertFeatureState::disabled: return "disabled";
    case InsertFeatureState::policy_required: return "policy_required";
    case InsertFeatureState::refused: return "refused";
  }
  return "unknown";
}

const char* InsertIndexMaintenanceActionName(InsertIndexMaintenanceAction action) {
  switch (action) {
    case InsertIndexMaintenanceAction::synchronous_exact_insert: return "synchronous_exact_insert";
    case InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert: return "synchronous_exact_probe_then_insert";
    case InsertIndexMaintenanceAction::batch_local_buffer_then_insert: return "batch_local_buffer_then_insert";
    case InsertIndexMaintenanceAction::committed_delta_ledger: return "committed_delta_ledger";
    case InsertIndexMaintenanceAction::sorted_run_build: return "sorted_run_build";
    case InsertIndexMaintenanceAction::shadow_rebuild_then_cutover: return "shadow_rebuild_then_cutover";
    case InsertIndexMaintenanceAction::reject_batch_path: return "reject_batch_path";
  }
  return "unknown";
}

bool InsertBatchOptionEnabled(const EngineInsertRowsRequest& request, const std::string& option) {
  for (const auto& candidate : request.option_envelopes) {
    if (OptionEquals(candidate, option)) {
      return true;
    }
  }
  return false;
}

std::string InsertBatchOptionValue(const EngineInsertRowsRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    const auto offset = OptionValueOffset(option, prefix);
    if (offset != std::string::npos) {
      return option.substr(offset);
    }
  }
  return {};
}

InsertBatchMode ResolveInsertBatchMode(const EngineInsertRowsRequest& request) {
  if (!request.insert_mode.empty()) {
    if (request.insert_mode == "singleton") return InsertBatchMode::singleton;
    if (request.insert_mode == "multi_values") return InsertBatchMode::multi_values;
    if (request.insert_mode == "insert_select") return InsertBatchMode::insert_select;
    if (request.insert_mode == "copy_import") return InsertBatchMode::copy_import;
    if (request.insert_mode == "reference_bulk") return InsertBatchMode::reference_bulk;
    if (request.insert_mode == "native_bulk") return InsertBatchMode::native_bulk;
  }
  if (InsertBatchOptionEnabled(request, "insert_mode=insert_select")) return InsertBatchMode::insert_select;
  if (InsertBatchOptionEnabled(request, "insert_mode=copy_import")) return InsertBatchMode::copy_import;
  if (InsertBatchOptionEnabled(request, "insert_mode=reference_bulk")) return InsertBatchMode::reference_bulk;
  if (InsertBatchOptionEnabled(request, "insert_mode=native_bulk")) return InsertBatchMode::native_bulk;
  return request.input_rows.size() <= 1 ? InsertBatchMode::singleton : InsertBatchMode::multi_values;
}

InsertDuplicateMode ResolveInsertDuplicateMode(const EngineInsertRowsRequest& request) {
  if (request.duplicate_mode == "ignore") return InsertDuplicateMode::ignore;
  if (request.duplicate_mode == "replace") return InsertDuplicateMode::replace;
  if (request.duplicate_mode == "update") return InsertDuplicateMode::update;
  if (request.duplicate_mode == "merge_policy") return InsertDuplicateMode::merge_policy;
  if (InsertBatchOptionEnabled(request, "duplicate_mode=ignore")) return InsertDuplicateMode::ignore;
  if (InsertBatchOptionEnabled(request, "duplicate_mode=replace")) return InsertDuplicateMode::replace;
  if (InsertBatchOptionEnabled(request, "duplicate_mode=update")) return InsertDuplicateMode::update;
  return InsertDuplicateMode::error;
}

InsertFeatureGates ResolveInsertFeatureGates(const EngineInsertRowsRequest& request) {
  InsertFeatureGates gates;
  if (InsertBatchOptionEnabled(request, "feature.page_reservation=disabled")) {
    gates.page_reservation = InsertFeatureState::disabled;
  }
  if (InsertBatchOptionEnabled(request, "feature.identity_range_reservation=disabled")) {
    gates.identity_range_reservation = InsertFeatureState::disabled;
  }
  // DPC_DEFERRED_INDEX_FEATURE_FLAG
  if (InsertBatchOptionEnabled(request, idx::kDeferredSecondaryIndexRuntimeOption)) {
    gates.deferred_secondary_index_runtime = InsertFeatureState::enabled;
  }
  if (InsertBatchOptionEnabled(request, idx::kSecondaryIndexDeltaLedgerFeatureOption)) {
    gates.secondary_index_delta_ledger = InsertFeatureState::enabled;
  }
  if (InsertBatchOptionEnabled(request, "feature.strict_bulk_load=enabled")) {
    gates.strict_bulk_load = InsertFeatureState::enabled;
  }
  if (InsertBatchOptionEnabled(request, "feature.sorted_run_shadow_load=enabled")) {
    gates.sorted_run_shadow_load = InsertFeatureState::enabled;
  }
  return gates;
}

InsertBatchMemoryPolicy ResolveInsertMemoryPolicy(const EngineInsertRowsRequest& request) {
  InsertBatchMemoryPolicy policy;
  policy.context_budget_bytes = ParseU64Option(request, "memory.context_budget_bytes=", policy.context_budget_bytes);
  policy.unique_preflight_budget_bytes = ParseU64Option(request, "memory.unique_preflight_budget_bytes=", policy.unique_preflight_budget_bytes);
  policy.delta_ledger_stage_budget_bytes = ParseU64Option(request, "memory.delta_ledger_stage_budget_bytes=", policy.delta_ledger_stage_budget_bytes);
  policy.sorted_run_budget_bytes = ParseU64Option(request, "memory.sorted_run_budget_bytes=", policy.sorted_run_budget_bytes);
  policy.bulk_load_budget_bytes = ParseU64Option(request, "memory.bulk_load_budget_bytes=", policy.bulk_load_budget_bytes);
  policy.spill_allowed = InsertBatchOptionEnabled(request, "memory.spill_allowed=true");
  return policy;
}

StrictBulkLoadPolicy ResolveStrictBulkLoadPolicy(const EngineInsertRowsRequest& request) {
  StrictBulkLoadPolicy policy;
  policy.requested = request.strict_bulk_load_requested || InsertBatchOptionEnabled(request, "strict_bulk_load=requested");
  policy.enabled = InsertBatchOptionEnabled(request, "feature.strict_bulk_load=enabled");
  policy.allow_triggers = InsertBatchOptionEnabled(request, "bulk.allow_triggers=true");
  policy.allow_foreign_keys = InsertBatchOptionEnabled(request, "bulk.allow_foreign_keys=true");
  policy.allow_opaque_columns = InsertBatchOptionEnabled(request, "bulk.allow_opaque_columns=true");
  policy.target_empty_required = !InsertBatchOptionEnabled(request, "bulk.target_empty_required=false");
  return policy;
}

BoundInsertRowTemplate BuildBoundInsertRowTemplate(const EngineInsertRowsRequest& request,
                                                   const CrudTableRecord& table) {
  BoundInsertRowTemplate row_template;
  row_template.table_uuid = table.table_uuid;
  row_template.columns = table.columns;
  row_template.descriptor_count = table.columns.size();
  row_template.requires_generated_row_uuid = request.require_generated_row_uuid;
  row_template.template_id = MakeId("insert_template", table.table_uuid + ":" + std::to_string(table.columns.size()));
  for (const auto& column : table.columns) {
    if (CrudColumnDescriptorIsOpaqueRenderOnly(column.second)) {
      row_template.has_opaque_render_only_column = true;
    }
  }
  return row_template;
}

IndexMaintenancePlan BuildIndexMaintenancePlan(const EngineInsertRowsRequest& request,
                                               const CrudState&,
                                               const CrudTableRecord& table,
                                               const std::vector<CrudIndexRecord>& indexes,
                                               const InsertFeatureGates& feature_gates,
                                               const SecondaryIndexDeltaLedgerPolicy& delta_ledger_policy) {
  IndexMaintenancePlan plan;
  plan.table_uuid = table.table_uuid;
  plan.plan_id = MakeId("index_plan", table.table_uuid + ":" + std::to_string(indexes.size()));
  for (const auto& index : indexes) {
    IndexMaintenancePlanEntry entry;
    entry.index = index;
    entry.action = ActionForIndex(index, delta_ledger_policy.enabled, feature_gates);
    entry.reason = ActionReason(entry.action);
    if (IsUniqueIndex(index)) {
      plan.has_unique_exact = true;
    }
    if (entry.action == InsertIndexMaintenanceAction::committed_delta_ledger) {
      plan.has_delta_eligible = true;
    }
    if (entry.action == InsertIndexMaintenanceAction::reject_batch_path) {
      plan.rejected = true;
      plan.rejection_reason = entry.reason;
    }
    plan.entries.push_back(std::move(entry));
  }
  if (request.strict_bulk_load_requested && feature_gates.strict_bulk_load != InsertFeatureState::enabled) {
    plan.rejected = true;
    plan.rejection_reason = "strict_bulk_load_policy_not_enabled";
  }
  return plan;
}

IdentityReservationPlan ReserveInsertIdentityRange(const EngineInsertRowsRequest& request,
                                                   const BoundInsertRowTemplate&) {
  IdentityReservationPlan plan;
  plan.requested_count = EstimateRows(request);
  plan.reservation_id = MakeId("identity_reservation", TargetUuid(request) + ":" + std::to_string(plan.requested_count));
  if (ResolveInsertFeatureGates(request).identity_range_reservation == InsertFeatureState::enabled) {
    plan.reserved_count = plan.requested_count;
    plan.range_reserved = plan.requested_count != 0;
  } else {
    plan.refusal_reason = "identity_range_reservation_disabled";
  }
  return plan;
}

PageReservationPlan ReserveInsertPages(const EngineInsertRowsRequest& request,
                                       const BoundInsertRowTemplate&,
                                       std::uint64_t estimated_rows) {
  PageReservationPlan plan;
  plan.requested_pages = std::max<std::uint64_t>(1, (estimated_rows + 127) / 128);
  plan.reservation_id = MakeId("page_reservation", TargetUuid(request) + ":" + std::to_string(plan.requested_pages));
  if (ResolveInsertFeatureGates(request).page_reservation != InsertFeatureState::enabled) {
    plan.reservation_available = false;
    plan.refusal_reason = "page_reservation_disabled";
  }
  return plan;
}

SecondaryIndexDeltaLedgerPolicy ResolveSecondaryIndexDeltaLedgerPolicy(const EngineInsertRowsRequest& request,
                                                                       const InsertFeatureGates& feature_gates) {
  SecondaryIndexDeltaLedgerPolicy policy;
  // DPC_DEFERRED_INDEX_FEATURE_FLAG_GATE
  const auto decision =
      idx::ResolveDeferredSecondaryIndexRuntimePolicy(request.option_envelopes);
  policy.enabled = decision.enabled &&
                   feature_gates.deferred_secondary_index_runtime == InsertFeatureState::enabled &&
                   feature_gates.secondary_index_delta_ledger == InsertFeatureState::enabled;
  policy.runtime_enabled = decision.runtime_enabled;
  policy.readers_overlay_committed_deltas = decision.readers_overlay_committed_deltas;
  policy.cleanup_horizon_bound = decision.cleanup_horizon_bound;
  policy.recovery_classifiable = decision.recovery_classifiable;
  policy.synchronous_fallback_required = !policy.enabled;
  policy.fallback_reason = policy.enabled ? std::string{} : decision.fallback_reason;
  return policy;
}

InsertBatchContext BeginInsertBatchContext(const EngineInsertRowsRequest& request,
                                           const CrudState& state,
                                           const CrudTableRecord& table,
                                           const std::vector<CrudIndexRecord>& indexes) {
  InsertBatchContext context;
  context.statement_uuid = request.context.request_id.empty() ? GenerateCrudEngineUuid("transaction") : request.context.request_id;
  context.local_transaction_id = request.context.local_transaction_id;
  context.transaction_uuid = request.context.transaction_uuid.canonical;
  context.database_uuid = request.context.database_uuid.canonical;
  context.schema_uuid = request.target_schema.uuid.canonical;
  context.target_object_uuid = TargetUuid(request);
  context.estimated_row_count = EstimateRows(request);
  context.insert_mode = ResolveInsertBatchMode(request);
  context.duplicate_mode = ResolveInsertDuplicateMode(request);
  context.security_context_uuid = request.context.principal_uuid.canonical;
  context.policy_snapshot_uuid = InsertBatchOptionValue(request, "policy_snapshot_uuid=");
  context.feature_gates = ResolveInsertFeatureGates(request);
  context.memory_policy = ResolveInsertMemoryPolicy(request);
  context.row_template = BuildBoundInsertRowTemplate(request, table);
  context.identity_reservation = ReserveInsertIdentityRange(request, context.row_template);
  context.page_reservation = ReserveInsertPages(request, context.row_template, context.estimated_row_count);
  context.delta_ledger_policy = ResolveSecondaryIndexDeltaLedgerPolicy(request, context.feature_gates);
  context.index_plan = BuildIndexMaintenancePlan(request, state, table, indexes, context.feature_gates, context.delta_ledger_policy);
  context.bulk_load_policy = ResolveStrictBulkLoadPolicy(request);
  context.strict_bulk_load_selected = context.bulk_load_policy.requested && context.bulk_load_policy.enabled;
  context.accepted = !context.index_plan.rejected && context.page_reservation.reservation_available;
  if (!context.accepted) {
    context.fallback_reason = !context.index_plan.rejection_reason.empty()
                                  ? context.index_plan.rejection_reason
                                  : context.page_reservation.refusal_reason;
  }
  AddInsertTrace(&context, "insert.batch.begin", "begin", InsertBatchModeName(context.insert_mode));
  AddInsertTrace(&context, "insert.template.bind", "bind", context.row_template.template_id);
  AddInsertTrace(&context, "insert.identity.reserve", "reserve", context.identity_reservation.reservation_id);
  AddInsertTrace(&context, "insert.page.reserve", "reserve", context.page_reservation.reservation_id);
  context.evidence.push_back({"insert_batch_context", context.statement_uuid});
  context.evidence.push_back({"insert_row_template", context.row_template.template_id});
  context.evidence.push_back({"insert_index_plan", context.index_plan.plan_id});
  if (context.identity_reservation.range_reserved) {
    context.evidence.push_back({"identity_range_reservation", context.identity_reservation.reservation_id});
  }
  if (context.page_reservation.reservation_available) {
    context.evidence.push_back({"page_reservation", context.page_reservation.reservation_id});
  }
  if (context.delta_ledger_policy.synchronous_fallback_required) {
    context.evidence.push_back({"insert_index_maintenance_mode", "synchronous_fallback"});
    context.evidence.push_back({"insert_deferred_secondary_index_fallback_reason",
                                context.delta_ledger_policy.fallback_reason});
  } else {
    context.evidence.push_back({"insert_index_maintenance_mode", "deferred_secondary_index"});
  }
  return context;
}

EngineApiDiagnostic ValidateStrictBulkLoadEligibility(const InsertBatchContext& context,
                                                      const CrudTableRecord& table) {
  if (!context.bulk_load_policy.requested) {
    return OkDiagnostic();
  }
  if (!context.bulk_load_policy.enabled) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "strict_bulk_load_policy_not_enabled");
  }
  if (context.row_template.has_opaque_render_only_column && !context.bulk_load_policy.allow_opaque_columns) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "strict_bulk_load_opaque_column_refused");
  }
  if (context.bulk_load_policy.target_empty_required && !table.table_uuid.empty() && context.estimated_row_count == 0) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "strict_bulk_load_row_estimate_required");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateInsertBatchMemoryBudget(const InsertBatchContext& context,
                                                    std::uint64_t projected_bytes) {
  if (projected_bytes > context.memory_policy.context_budget_bytes && !context.memory_policy.spill_allowed) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "insert_batch_memory_budget_exceeded");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateInsertBatchConstraints(const InsertBatchContext&,
                                                   const CrudState&,
                                                   const PreparedInsertRow&) {
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateInsertBatchUniquePreflight(InsertBatchContext* context,
                                                       const std::vector<std::pair<std::string, std::string>>& values) {
  if (context == nullptr) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "insert_batch_context_required");
  }
  for (const auto& entry : context->index_plan.entries) {
    if (!IsUniqueIndex(entry.index)) {
      continue;
    }
    for (const auto& key : CrudIndexKeysForValues(entry.index, values)) {
      const std::string request_key = entry.index.index_uuid + "|" + key;
      if (!context->unique_request_keys.insert(request_key).second) {
        return MakeInvalidRequestDiagnostic("dml.insert_rows", "unique_index_duplicate");
      }
    }
  }
  return OkDiagnostic();
}

PreparedInsertRow PrepareInsertRowForBatch(const EngineInsertRowsRequest& request,
                                           const EngineRowValue& input_row,
                                           const BoundInsertRowTemplate& row_template) {
  PreparedInsertRow row;
  row.values = RowValuePairs(input_row);
  row.row_uuid = UuidStringOrGenerated(input_row.requested_row_uuid, "row");
  row.encoded_bytes = static_cast<std::uint64_t>(EncodedValueBytes(row.values));
  row.toast_required = row.encoded_bytes > row_template.max_inline_encoded_bytes ||
                       InsertBatchOptionEnabled(request, "large_value.force_toast=true");
  return row;
}

EngineApiDiagnostic AppendSecondaryIndexDeltaLedgerEntries(const EngineRequestContext& request_context,
                                                           const InsertBatchContext& context,
                                                           const PreparedInsertRow& row,
                                                           const std::string& version_uuid) {
  if (!context.delta_ledger_policy.enabled) {
    return OkDiagnostic();
  }
  // DPC_DEFERRED_INDEX_WRITE_PATH
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> entries;
  for (const auto& plan_entry : context.index_plan.entries) {
    if (plan_entry.action != InsertIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    MgaSecondaryIndexDeltaLedgerEntryInput input;
    input.index = plan_entry.index;
    input.table_uuid = context.target_object_uuid;
    input.row_uuid = row.row_uuid;
    input.version_uuid = version_uuid;
    input.values = row.values;
    input.delta_kind = idx::SecondaryIndexDeltaKind::insert;
    input.source_evidence_reference =
        "engine.dml.insert.secondary_index_delta:" + context.statement_uuid;
    entries.push_back(std::move(input));
  }
  return AppendMgaSecondaryIndexDeltaLedgerEntries(request_context, entries, nullptr);
}

void AddInsertTrace(InsertBatchContext* context, std::string event_name, std::string phase, std::string detail) {
  if (context == nullptr) {
    return;
  }
  context->trace_events.push_back({std::move(event_name), std::move(phase), std::move(detail)});
}

void AddInsertBatchEvidenceToResult(const InsertBatchContext& context, EngineApiResult* result) {
  if (result == nullptr) {
    return;
  }
  for (const auto& evidence : context.evidence) {
    result->evidence.push_back(evidence);
  }
  for (const auto& trace : context.trace_events) {
    result->evidence.push_back({"insert_trace", trace.event_name + ":" + trace.phase + ":" + trace.detail});
  }
  result->evidence.push_back({"insert_feature_gate.secondary_index_delta_ledger", InsertFeatureStateName(context.feature_gates.secondary_index_delta_ledger)});
  result->evidence.push_back({"insert_runtime.deferred_secondary_index", context.delta_ledger_policy.runtime_enabled ? "enabled" : "disabled"});
  result->evidence.push_back({"insert_delta_ledger_policy", context.delta_ledger_policy.enabled ? "enabled" : "synchronous_fallback"});
  if (context.delta_ledger_policy.synchronous_fallback_required) {
    result->evidence.push_back({"insert_deferred_secondary_index_fallback_reason",
                                context.delta_ledger_policy.fallback_reason});
  }
  result->evidence.push_back({"insert_feature_gate.strict_bulk_load", InsertFeatureStateName(context.feature_gates.strict_bulk_load)});
  if (!context.fallback_reason.empty()) {
    result->evidence.push_back({"insert_fallback_reason", context.fallback_reason});
  }
}

void RecordInsertBatchMetric(const InsertBatchContext& context, std::string metric, double value, std::string result, std::string reason) {
  if (metric == "sb_dml_insert_batch_started_total") {
    (void)scratchbird::core::metrics::RecordInsertBatchStarted(context.target_object_uuid,
                                                               InsertBatchModeName(context.insert_mode),
                                                               result);
    return;
  }
  if (metric == "sb_dml_insert_batch_fallback_total" ||
      metric == "sb_dml_insert_batch_fallback_reason_total") {
    (void)scratchbird::core::metrics::RecordInsertBatchFallback(context.target_object_uuid,
                                                                InsertBatchModeName(context.insert_mode),
                                                                reason.empty() ? "unspecified" : reason);
    return;
  }
  if (metric == "sb_dml_insert_rows_inserted_total") {
    (void)scratchbird::core::metrics::RecordInsertRowsInserted(value,
                                                               context.target_object_uuid,
                                                               InsertBatchModeName(context.insert_mode));
    (void)scratchbird::core::metrics::ObserveInsertRowsPerBatch(static_cast<double>(context.actual_row_count),
                                                                context.target_object_uuid,
                                                                InsertBatchModeName(context.insert_mode));
    return;
  }
  (void)scratchbird::core::metrics::IncrementCounter(
      metric,
      scratchbird::core::metrics::Labels({{"component", "engine.insert"},
                                          {"object_uuid", context.target_object_uuid},
                                          {"operation", InsertBatchModeName(context.insert_mode)},
                                          {"result", std::move(result)},
                                          {"reason", reason.empty() ? "none" : std::move(reason)}}),
      value,
      kInsertMetricsProducer);
}

}  // namespace scratchbird::engine::internal_api
