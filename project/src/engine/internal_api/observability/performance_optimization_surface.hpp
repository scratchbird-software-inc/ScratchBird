// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: CDP_USER_FACING_OBSERVABILITY_SURFACE_GATE
// SEARCH_KEY: DPC_OBSERVABILITY_SURFACE
// SEARCH_KEY: DPC_CONFIG_DEFAULTS_PACKAGING_GATE
// User-facing, engine-owned snapshot of the current DML/planner performance
// optimization state. This is observability-only: it does not execute SQL,
// mutate storage, or own parser/client transaction finality.

struct PerformanceOptimizationSurfaceField {
  std::string name;
  std::string type;
  bool required = true;
  std::string category;
};

struct PerformanceOptimizationSelectedPathSurface {
  std::string path_id = "runtime_default.scan";
  std::string path_kind = "physical_access_path";
  std::string selected_path = "unknown";
  std::string path_state = "unknown";
  std::string authority_source = "engine_optimizer";
  std::string fallback_reason = "none";
};

struct PerformanceOptimizationFeatureGateSurface {
  std::string gate_id = "optimizer_enabled";
  bool enabled = true;
  std::string gate_state = "enabled";
  std::string authority_source = "engine_config_policy";
  std::string refusal_code = "none";
};

struct PerformanceOptimizationFallbackSurface {
  std::string route_id = "runtime_default";
  std::string reason_code = "none";
  std::string detail = "none";
  std::string scalar_fallback_state = "not_required";
};

struct PerformanceOptimizationQuotaSurface {
  std::string quota_family = "resource_governor";
  std::string quota_state = "unthrottled";
  EngineApiU64 quota_limit = 0;
  EngineApiU64 quota_requested = 0;
  EngineApiU64 quota_in_use = 0;
  EngineApiU64 grants = 0;
  EngineApiU64 refusals = 0;
  std::string action = "admit";
  std::string diagnostic_code = "none";
};

struct PerformanceOptimizationRuntimeCompatibilitySurface {
  std::string route_id = "runtime_default";
  std::string compatibility_status = "unknown";
  std::string compatibility_action = "not_reported";
  EngineApiU64 runtime_generation = 0;
  std::string required_capabilities = "unknown";
  std::string provider_capabilities = "unknown";
  std::string diagnostic_code = "none";
  std::string fallback_reason = "none";
};

struct PerformanceOptimizationRebuildSurface {
  std::string object_kind = "index_family";
  std::string rebuild_state = "idle";
  std::string rebuild_phase = "none";
  EngineApiU64 generation = 0;
  EngineApiU64 progress_numerator = 0;
  EngineApiU64 progress_denominator = 0;
  std::string refusal_code = "none";
};

struct PerformanceOptimizationExactRefusalSurface {
  std::string source = "none";
  std::string diagnostic_code = "none";
  std::string message_vector = "none";
  std::string refusal_state = "not_refused";
  std::string redaction_state = "public_safe_summary";
  bool public_safe = true;
};

struct PerformanceOptimizationSurfaceSnapshot {
  bool optimizer_enabled = true;
  bool copy_append_batching_enabled = true;
  bool native_ingest_enabled = false;
  bool plan_cache_enabled = true;
  bool descriptor_metadata_cache_enabled = true;
  bool statistics_enabled = true;
  bool summary_prune_enabled = true;
  bool agent_workers_enabled = true;
  bool resource_governor_enabled = true;
  bool page_filespace_preallocation_enabled = true;
  bool cancellation_enabled = true;
  bool backpressure_enabled = true;
  std::string optimization_profile = "runtime_default";

  std::string copy_batching_status = "idle";
  EngineApiU64 copy_batch_rows_configured = 1024;
  EngineApiU64 copy_batches_started = 0;
  EngineApiU64 copy_batches_completed = 0;
  EngineApiU64 copy_rows_batched = 0;
  EngineApiU64 copy_singleton_fallback_batches = 0;

  std::string native_ingest_state = "not_active";
  EngineApiU64 native_ingest_rows_processed = 0;
  EngineApiU64 native_ingest_rows_total = 0;
  bool native_ingest_refused = false;
  std::string native_ingest_refusal_code = "none";

  EngineApiU64 plan_cache_hits = 0;
  EngineApiU64 plan_cache_misses = 0;
  EngineApiU64 plan_cache_invalidations = 0;
  std::string plan_cache_last_invalidation_reason = "none";

  EngineApiU64 descriptor_metadata_cache_hits = 0;
  EngineApiU64 descriptor_metadata_cache_misses = 0;
  EngineApiU64 descriptor_metadata_cache_epoch = 0;

  EngineApiU64 statistics_epoch = 0;
  bool stale_statistics_fail_safe_active = false;
  std::string stale_statistics_fail_safe_reason = "none";

  EngineApiU64 catalog_generation_id = 0;
  EngineApiU64 name_resolution_epoch = 0;
  EngineApiU64 security_epoch = 0;
  EngineApiU64 resource_epoch = 0;
  EngineApiU64 optimization_state_epoch = 0;

  std::string selected_join_algorithm = "none";
  std::string selected_join_plan_summary = "no_join_selected";
  EngineApiU64 selected_join_left_rows = 0;
  EngineApiU64 selected_join_right_rows = 0;
  bool selected_join_from_statistics = false;
  std::string selected_join_statistics_version = "none";

  std::string summary_prune_status = "not_selected";
  std::string summary_prune_last_reason = "none";
  std::string summary_prune_fallback_reason = "none";
  std::string summary_prune_summary_status = "missing";
  EngineApiU64 summary_prune_generation = 0;
  EngineApiU64 summary_prune_ranges_considered = 0;
  EngineApiU64 summary_prune_ranges_pruned = 0;
  EngineApiU64 summary_prune_ranges_scanned = 0;
  EngineApiU64 summary_prune_pages_considered = 0;
  EngineApiU64 summary_prune_pages_pruned = 0;
  EngineApiU64 summary_prune_pages_scanned = 0;
  std::string summary_prune_authority_source = "engine_mga_base_pages";
  bool summary_prune_base_row_mga_recheck_required = true;
  bool summary_prune_base_row_security_recheck_required = true;

  std::string cleanup_horizon_authority_status = "not_reported";
  bool cleanup_horizon_authoritative = false;
  EngineApiU64 cleanup_horizon_local_transaction_id = 0;
  EngineApiU64 oldest_interesting_transaction_id = 0;
  EngineApiU64 oldest_active_transaction_id = 0;
  EngineApiU64 oldest_snapshot_transaction_id = 0;
  EngineApiU64 oldest_cleanup_transaction_id = 0;

  std::string agent_worker_status = "not_started";
  EngineApiU64 agent_worker_thread_count = 0;
  EngineApiU64 agent_worker_actions_accepted = 0;
  EngineApiU64 agent_worker_actions_refused = 0;
  EngineApiU64 agent_work_backlog_count = 0;
  std::string last_agent_type_id = "none";
  std::string last_agent_action = "none";
  std::string last_agent_decision = "none";
  std::string last_agent_diagnostic_code = "none";

  EngineApiU64 storage_row_version_backlog_count = 0;
  EngineApiU64 index_delta_backlog_count = 0;
  EngineApiU64 index_garbage_backlog_count = 0;
  EngineApiU64 page_summary_backlog_count = 0;

  std::string secondary_index_state = "not_reported";
  std::string shadow_index_state = "not_reported";
  std::string summary_index_state = "not_reported";
  std::string specialized_index_state = "not_reported";
  std::string index_state_authority_source = "engine_catalog";

  std::string resource_governor_state = "unthrottled";
  EngineApiU64 resource_quota_grants = 0;
  EngineApiU64 resource_quota_refusals = 0;
  EngineApiU64 resource_quota_in_use = 0;

  EngineApiU64 page_preallocation_demand_pages = 0;
  EngineApiU64 page_preallocation_granted_pages = 0;
  EngineApiU64 filespace_preallocation_demand_pages = 0;
  EngineApiU64 filespace_preallocation_granted_pages = 0;
  EngineApiU64 preallocation_refusal_count = 0;

  bool cancellation_requested = false;
  bool backpressure_active = false;
  std::string backpressure_state = "none";
  std::string backpressure_reason = "none";
  EngineApiU64 cancellation_checkpoint_count = 0;
  EngineApiU64 backpressure_deferral_count = 0;

  std::string benchmark_correlation_id = "none";
  std::string support_bundle_correlation_id = "none";
  std::string request_correlation_id = "none";

  std::string exact_refusal_diagnostic_code = "none";
  std::string exact_refusal_message_vector = "none";
  std::string exact_refusal_source = "none";
  bool message_vector_ready = false;

  std::string metric_family = "sys.metrics.dpc.performance_optimization";
  EngineApiU64 metric_sample_count = 0;
  std::string audit_event_family = "engine.audit.dpc.performance_optimization";
  EngineApiU64 audit_event_count = 0;
  std::string audit_last_decision = "none";

  std::string support_bundle_redaction_state = "public_safe_summary";
  std::string support_bundle_completeness_state = "complete";
  bool support_bundle_forbidden_fields_absent = true;

  std::vector<PerformanceOptimizationSelectedPathSurface> odf108_selected_paths = {
      PerformanceOptimizationSelectedPathSurface{}};
  std::vector<PerformanceOptimizationFeatureGateSurface> odf108_feature_gates = {
      PerformanceOptimizationFeatureGateSurface{}};
  std::vector<PerformanceOptimizationFallbackSurface> odf108_fallbacks = {
      PerformanceOptimizationFallbackSurface{}};
  std::vector<PerformanceOptimizationQuotaSurface> odf108_quotas = {
      PerformanceOptimizationQuotaSurface{}};
  std::vector<PerformanceOptimizationRuntimeCompatibilitySurface>
      odf108_runtime_compatibility = {
          PerformanceOptimizationRuntimeCompatibilitySurface{}};
  std::vector<PerformanceOptimizationRebuildSurface> odf108_rebuild_states = {
      PerformanceOptimizationRebuildSurface{}};
  std::vector<PerformanceOptimizationExactRefusalSurface> odf108_exact_refusals = {
      PerformanceOptimizationExactRefusalSurface{}};

  bool parser_finality_authority = false;
  bool reference_finality_authority = false;
  bool client_finality_authority = false;
  bool storage_shortcut_finality_authority = false;
  bool wal_recovery_authority = false;
};

struct PerformanceOptimizationConfigSurfaceField {
  std::string surface_name;
  std::string value_type;
  std::string packaged_default;
  std::string config_key;
  std::string env_var;
  std::string cli_option;
  std::string admin_override_operation;
  std::string admin_override_key;
  std::string management_view_field;
  std::string support_bundle_field;
  std::string disabled_behavior;
};

struct PerformanceOptimizationConfigOverride {
  std::string surface_name;
  std::string source;
  std::string value;
  bool allowed_by_policy = true;
  std::string refusal_code;
  std::string refusal_reason;
  std::string refusal_message_vector;
};

struct PerformanceOptimizationConfigEffectiveValue {
  PerformanceOptimizationConfigSurfaceField metadata;
  std::string configured_value;
  std::string effective_value;
  std::string value_source;
  std::string precedence_order;
  std::string override_refusal_code;
  std::string override_refusal_reason;
  std::string override_refusal_message_vector;
};

struct PerformanceOptimizationConfigResolution {
  std::vector<PerformanceOptimizationConfigEffectiveValue> fields;
};

struct PerformanceOptimizationSurfaceValidationResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> missing_or_invalid_fields;
};

struct EngineInspectPerformanceOptimizationSurfaceRequest : EngineApiRequest {
  PerformanceOptimizationSurfaceSnapshot snapshot;
  bool snapshot_present = false;
  std::vector<PerformanceOptimizationConfigOverride> config_overrides;
};

struct EngineInspectPerformanceOptimizationSurfaceResult : EngineApiResult {
  EngineApiU64 surface_schema_version = 0;
  bool management_api_ready = false;
  bool support_bundle_ready = false;
  bool sys_view_contract_ready = false;
  bool parser_finality_authority = false;
  bool reference_finality_authority = false;
  std::string management_api_json;
  std::string support_bundle_json;
};

EngineApiU64 PerformanceOptimizationSurfaceSchemaVersion();
const char* PerformanceOptimizationSurfaceSchemaId();
const std::vector<PerformanceOptimizationSurfaceField>&
PerformanceOptimizationSurfaceSchema();
const std::vector<PerformanceOptimizationConfigSurfaceField>&
PerformanceOptimizationConfigSurface();
PerformanceOptimizationConfigResolution
ResolvePerformanceOptimizationConfigSurface(
    const std::vector<PerformanceOptimizationConfigOverride>& overrides);

PerformanceOptimizationSurfaceSnapshot DefaultPerformanceOptimizationSurfaceSnapshot();
PerformanceOptimizationSurfaceValidationResult
ValidatePerformanceOptimizationSurfaceSnapshot(
    const PerformanceOptimizationSurfaceSnapshot& snapshot);
std::string SerializePerformanceOptimizationConfigResolutionJson(
    const PerformanceOptimizationConfigResolution& resolution);
std::string SerializePerformanceOptimizationSurfaceJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot);
std::string SerializePerformanceOptimizationManagementJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot);
std::string SerializePerformanceOptimizationManagementJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot,
    const PerformanceOptimizationConfigResolution& config_resolution);
std::string SerializePerformanceOptimizationSupportBundleJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot);
std::string SerializePerformanceOptimizationSupportBundleJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot,
    const PerformanceOptimizationConfigResolution& config_resolution);
void AddPerformanceOptimizationSurfaceRow(
    EngineApiResult* result,
    const PerformanceOptimizationSurfaceSnapshot& snapshot);

EngineInspectPerformanceOptimizationSurfaceResult
EngineInspectPerformanceOptimizationSurface(
    const EngineInspectPerformanceOptimizationSurfaceRequest& request);

}  // namespace scratchbird::engine::internal_api
