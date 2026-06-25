// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "crud_support/crud_store.hpp"
#include "dml/insert_api.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_INSERT_BATCH_ARCHITECTURE_CORE
// Engine-owned insert optimization context. This is not SQL authority and never
// accepts durable object names. Parser surfaces must resolve names to UUIDs
// before this layer is called.

enum class InsertBatchMode {
  singleton,
  multi_values,
  insert_select,
  copy_import,
  reference_bulk,
  native_bulk
};

enum class InsertDuplicateMode {
  error,
  ignore,
  replace,
  update,
  merge_policy
};

enum class InsertFeatureState {
  enabled,
  disabled,
  policy_required,
  refused
};

enum class InsertIndexMaintenanceAction {
  synchronous_exact_insert,
  synchronous_exact_probe_then_insert,
  batch_local_buffer_then_insert,
  committed_delta_ledger,
  sorted_run_build,
  shadow_rebuild_then_cutover,
  reject_batch_path
};

struct InsertFeatureGates {
  InsertFeatureState insert_batch_context = InsertFeatureState::enabled;
  InsertFeatureState bound_row_template = InsertFeatureState::enabled;
  InsertFeatureState page_reservation = InsertFeatureState::enabled;
  InsertFeatureState identity_range_reservation = InsertFeatureState::enabled;
  InsertFeatureState exact_unique_preflight = InsertFeatureState::enabled;
  InsertFeatureState deferred_secondary_index_runtime = InsertFeatureState::disabled;
  InsertFeatureState secondary_index_delta_ledger = InsertFeatureState::disabled;
  InsertFeatureState strict_bulk_load = InsertFeatureState::disabled;
  InsertFeatureState sorted_run_shadow_load = InsertFeatureState::disabled;
};

struct InsertBatchMemoryPolicy {
  std::uint64_t context_budget_bytes = 1024 * 1024;
  std::uint64_t unique_preflight_budget_bytes = 1024 * 1024;
  std::uint64_t delta_ledger_stage_budget_bytes = 1024 * 1024;
  std::uint64_t sorted_run_budget_bytes = 8 * 1024 * 1024;
  std::uint64_t bulk_load_budget_bytes = 16 * 1024 * 1024;
  bool spill_allowed = false;
};

struct InsertBatchTraceEvent {
  std::string event_name;
  std::string phase;
  std::string detail;
};

struct BoundInsertRowTemplate {
  std::string template_id;
  std::string table_uuid;
  std::vector<std::pair<std::string, std::string>> columns;
  std::size_t descriptor_count = 0;
  bool has_opaque_render_only_column = false;
  bool requires_generated_row_uuid = true;
  bool toast_required = false;
  std::uint64_t max_inline_encoded_bytes = kCrudVerticalSliceMaxEncodedValueBytes;
};

struct InsertRowEncoderColumnPlan {
  std::string column_name;
  std::string canonical_type_name;
  std::string descriptor_digest;
  std::size_t ordinal = 0;
  bool default_bound = false;
  bool domain_bound = false;
  bool check_bound = false;
  bool not_null_bound = false;
  bool unique_bound = false;
  bool foreign_key_bound = false;
};

struct InsertRowEncoderPlan {
  std::string plan_id;
  std::string table_uuid;
  std::string row_shape_signature;
  std::string validator_signature;
  std::string default_signature;
  std::string domain_signature;
  std::string check_signature;
  std::string index_validator_signature;
  std::string security_policy_signature;
  std::vector<InsertRowEncoderColumnPlan> columns;
  std::uint64_t column_count = 0;
  std::uint64_t default_validator_count = 0;
  std::uint64_t domain_validator_count = 0;
  std::uint64_t check_validator_count = 0;
  std::uint64_t not_null_validator_count = 0;
  std::uint64_t unique_validator_count = 0;
  std::uint64_t foreign_key_validator_count = 0;
  std::uint64_t runtime_policy_recheck_count = 0;
  bool has_sblr_backed_default = false;
  bool has_sblr_backed_check = false;
  bool unsupported_sblr_validators_fail_closed = true;
};

struct IndexMaintenancePlanEntry {
  CrudIndexRecord index;
  InsertIndexMaintenanceAction action = InsertIndexMaintenanceAction::synchronous_exact_insert;
  std::string reason;
};

struct IndexMaintenancePlan {
  std::string plan_id;
  std::string table_uuid;
  std::vector<IndexMaintenancePlanEntry> entries;
  bool has_unique_exact = false;
  bool has_delta_eligible = false;
  bool rejected = false;
  std::string rejection_reason;
};

struct IdentityReservationPlan {
  std::string reservation_id;
  std::uint64_t requested_count = 0;
  std::uint64_t reserved_count = 0;
  bool range_reserved = false;
  std::string refusal_reason;
};

struct PageReservationPlan {
  std::string reservation_id;
  std::uint64_t requested_pages = 0;
  std::uint64_t target_available_pages = 8;
  std::uint64_t notify_below_pages = 4;
  bool reservation_available = true;
  std::string refusal_reason;
};

struct InsertAdaptiveBatchPlan {
  std::uint64_t requested_rows = 0;
  std::uint64_t admitted_rows = 0;
  std::uint64_t requested_bytes = 0;
  std::uint64_t admitted_bytes = 0;
  std::uint64_t estimated_row_bytes = 0;
  std::uint64_t index_count = 0;
  std::uint64_t page_size_bytes = 0;
  std::uint64_t page_window_rows = 0;
  std::uint64_t commit_window_rows = 0;
  std::uint64_t contention_window_rows = 0;
  bool large_value_pressure = false;
  bool reduced = false;
  std::string reason = "within_policy";
};

struct SecondaryIndexDeltaLedgerPolicy {
  bool enabled = false;
  bool runtime_enabled = false;
  bool readers_overlay_committed_deltas = false;
  bool cleanup_horizon_bound = false;
  bool recovery_classifiable = false;
  bool synchronous_fallback_required = true;
  std::string fallback_reason = "runtime_deferred_secondary_index_disabled";
};

struct StrictBulkLoadPolicy {
  bool requested = false;
  bool enabled = false;
  bool target_empty_required = true;
  bool allow_triggers = false;
  bool allow_foreign_keys = false;
  bool allow_opaque_columns = false;
};

struct InsertBatchContext {
  std::string statement_uuid;
  std::uint64_t local_transaction_id = 0;
  std::string transaction_uuid;
  std::string database_uuid;
  std::string schema_uuid;
  std::string target_object_uuid;
  std::uint64_t estimated_row_count = 0;
  std::uint64_t actual_row_count = 0;
  InsertBatchMode insert_mode = InsertBatchMode::singleton;
  InsertDuplicateMode duplicate_mode = InsertDuplicateMode::error;
  std::string security_context_uuid;
  std::string policy_snapshot_uuid;
  InsertFeatureGates feature_gates;
  InsertBatchMemoryPolicy memory_policy;
  BoundInsertRowTemplate row_template;
  InsertRowEncoderPlan row_encoder_plan;
  IndexMaintenancePlan index_plan;
  IdentityReservationPlan identity_reservation;
  PageReservationPlan page_reservation;
  SecondaryIndexDeltaLedgerPolicy delta_ledger_policy;
  StrictBulkLoadPolicy bulk_load_policy;
  InsertAdaptiveBatchPlan adaptive_batch_plan;
  std::string prepared_descriptor_cache_key;
  std::string prepared_descriptor_id;
  std::string prepared_descriptor_authorization_digest;
  std::string prepared_descriptor_principal_uuid;
  std::string prepared_descriptor_role_uuid;
  std::string prepared_descriptor_session_uuid;
  std::uint64_t prepared_descriptor_generation = 0;
  std::uint64_t prepared_descriptor_catalog_epoch = 0;
  std::uint64_t prepared_descriptor_security_epoch = 0;
  std::uint64_t prepared_descriptor_policy_epoch = 0;
  std::uint64_t prepared_descriptor_cache_size = 0;
  std::uint64_t prepared_descriptor_eviction_count = 0;
  std::uint64_t prepared_descriptor_cache_limit = 0;
  std::uint64_t prepared_descriptor_effective_cache_limit = 0;
  std::uint64_t prepared_descriptor_trim_target_entries = 0;
  std::uint64_t prepared_descriptor_trim_entries_before = 0;
  std::uint64_t prepared_descriptor_trim_entries_after = 0;
  std::uint64_t prepared_descriptor_trim_evictions = 0;
  bool prepared_descriptor_cache_hit = false;
  bool prepared_descriptor_memory_pressure_detected = false;
  bool prepared_descriptor_trim_requested = false;
  bool prepared_descriptor_backoff_active = false;
  std::string prepared_descriptor_pressure_reason;
  std::string prepared_descriptor_authority_after_trim;
  bool prepared_descriptor_authority_refused = false;
  std::string prepared_descriptor_refusal_reason;
  bool memory_arena_granted = false;
  bool memory_arena_released = false;
  bool memory_arena_reset = false;
  bool memory_arena_fail_closed = false;
  std::uint64_t memory_arena_requested_bytes = 0;
  std::uint64_t memory_arena_granted_bytes = 0;
  std::uint64_t memory_arena_peak_bytes = 0;
  std::uint64_t memory_arena_leak_count = 0;
  std::set<std::string> unique_request_keys;
  std::vector<InsertBatchTraceEvent> trace_events;
  std::uint64_t trace_event_count = 0;
  std::uint64_t trace_event_compacted_count = 0;
  std::vector<EngineEvidenceReference> evidence;
  std::vector<EngineApiDiagnostic> diagnostics;
  std::string fallback_reason;
  bool accepted = false;
  bool strict_bulk_load_selected = false;
  bool cancelled = false;
};

struct PreparedInsertRow {
  std::vector<std::pair<std::string, std::string>> values;
  std::string row_uuid;
  bool toast_required = false;
  std::uint64_t encoded_bytes = 0;
};

const char* InsertBatchModeName(InsertBatchMode mode);
const char* InsertDuplicateModeName(InsertDuplicateMode mode);
const char* InsertFeatureStateName(InsertFeatureState state);
const char* InsertIndexMaintenanceActionName(InsertIndexMaintenanceAction action);

InsertBatchMode ResolveInsertBatchMode(const EngineInsertRowsRequest& request);
InsertDuplicateMode ResolveInsertDuplicateMode(const EngineInsertRowsRequest& request);
InsertFeatureGates ResolveInsertFeatureGates(const EngineInsertRowsRequest& request);
InsertBatchMemoryPolicy ResolveInsertMemoryPolicy(const EngineInsertRowsRequest& request);
StrictBulkLoadPolicy ResolveStrictBulkLoadPolicy(const EngineInsertRowsRequest& request);

BoundInsertRowTemplate BuildBoundInsertRowTemplate(const EngineInsertRowsRequest& request,
                                                   const CrudTableRecord& table);
IndexMaintenancePlan BuildIndexMaintenancePlan(const EngineInsertRowsRequest& request,
                                               const CrudState& state,
                                               const CrudTableRecord& table,
                                               const std::vector<CrudIndexRecord>& indexes,
                                               const InsertFeatureGates& feature_gates,
                                               const SecondaryIndexDeltaLedgerPolicy& delta_ledger_policy);
IdentityReservationPlan ReserveInsertIdentityRange(const EngineInsertRowsRequest& request,
                                                   const BoundInsertRowTemplate& row_template);
PageReservationPlan ReserveInsertPages(const EngineInsertRowsRequest& request,
                                       const BoundInsertRowTemplate& row_template,
                                       std::uint64_t estimated_rows);
SecondaryIndexDeltaLedgerPolicy ResolveSecondaryIndexDeltaLedgerPolicy(const EngineInsertRowsRequest& request,
                                                                       const InsertFeatureGates& feature_gates);

InsertBatchContext BeginInsertBatchContext(const EngineInsertRowsRequest& request,
                                           const CrudState& state,
                                           const CrudTableRecord& table,
                                           const std::vector<CrudIndexRecord>& indexes);

EngineApiDiagnostic ValidateStrictBulkLoadEligibility(const InsertBatchContext& context,
                                                      const CrudTableRecord& table);
EngineApiDiagnostic ValidateInsertBatchMemoryBudget(const InsertBatchContext& context,
                                                    std::uint64_t projected_bytes);
EngineApiDiagnostic ValidateInsertBatchConstraints(const InsertBatchContext& context,
                                                   const CrudState& state,
                                                   const PreparedInsertRow& row);
EngineApiDiagnostic ValidateInsertBatchUniquePreflight(InsertBatchContext* context,
                                                       const std::vector<std::pair<std::string, std::string>>& values);
PreparedInsertRow PrepareInsertRowForBatch(const EngineInsertRowsRequest& request,
                                           const EngineRowValue& input_row,
                                           const BoundInsertRowTemplate& row_template);
PreparedInsertRow PrepareInsertRowForBatch(const EngineInsertRowsRequest& request,
                                           const EngineRowValue& input_row,
                                           const BoundInsertRowTemplate& row_template,
                                           const InsertRowEncoderPlan& row_encoder_plan);
EngineApiDiagnostic AppendSecondaryIndexDeltaLedgerEntries(const EngineRequestContext& request_context,
                                                           const InsertBatchContext& context,
                                                           const PreparedInsertRow& row,
                                                           const std::string& version_uuid);
void AddInsertTrace(InsertBatchContext* context, std::string event_name, std::string phase, std::string detail = {});
void AddInsertBatchEvidenceToResult(const InsertBatchContext& context, EngineApiResult* result);
void RecordInsertBatchMetric(const InsertBatchContext& context, std::string metric, double value, std::string result, std::string reason = {});
bool InsertBatchOptionEnabled(const EngineInsertRowsRequest& request, const std::string& option);
std::string InsertBatchOptionValue(const EngineInsertRowsRequest& request, const std::string& prefix);

}  // namespace scratchbird::engine::internal_api
