// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle_test_memory.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "observability/show_api.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.request_id = "cdp033-user-observability-surface";
  context.catalog_generation_id = 33;
  context.security_epoch = 7;
  context.resource_epoch = 9;
  context.trace_tags = {
      "right:OBS_METRICS_READ_FAMILY",
      "right:OBS_MANAGEMENT_INSPECT",
      "right:OBS_AGENT_STATE_READ",
      "right:OBS_CONFIG_INSPECT",
      "cdp_user_observability_surface_gate"};
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "cdp_user_observability_surface_gate",
      {"OBS_METRICS_READ_FAMILY",
       "OBS_MANAGEMENT_INSPECT",
       "OBS_AGENT_STATE_READ",
       "OBS_CONFIG_INSPECT",
       "OBS_INDEX_PROFILE_READ",
       "MGA_CLEANUP_INSPECT"});
  return context;
}

api::PerformanceOptimizationSurfaceSnapshot RichSnapshot() {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "benchmark_clean_with_runtime_summary";

  snapshot.copy_batching_status = "active";
  snapshot.copy_batch_rows_configured = 4096;
  snapshot.copy_batches_started = 12;
  snapshot.copy_batches_completed = 11;
  snapshot.copy_rows_batched = 45056;
  snapshot.copy_singleton_fallback_batches = 1;

  snapshot.native_ingest_enabled = true;
  snapshot.native_ingest_state = "refused";
  snapshot.native_ingest_rows_processed = 128;
  snapshot.native_ingest_rows_total = 1024;
  snapshot.native_ingest_refused = true;
  snapshot.native_ingest_refusal_code = "INGEST.QUOTA_REFUSED";

  snapshot.plan_cache_hits = 44;
  snapshot.plan_cache_misses = 6;
  snapshot.plan_cache_invalidations = 3;
  snapshot.plan_cache_last_invalidation_reason = "statistics_epoch_changed";

  snapshot.descriptor_metadata_cache_hits = 51;
  snapshot.descriptor_metadata_cache_misses = 4;
  snapshot.descriptor_metadata_cache_epoch = 17;

  snapshot.statistics_epoch = 19;
  snapshot.stale_statistics_fail_safe_active = true;
  snapshot.stale_statistics_fail_safe_reason = "stale_index_statistics";

  snapshot.selected_join_algorithm = "hash";
  snapshot.selected_join_plan_summary = "hash_join:left=orders:right=order_items";
  snapshot.selected_join_left_rows = 1000;
  snapshot.selected_join_right_rows = 5000;
  snapshot.selected_join_from_statistics = true;
  snapshot.selected_join_statistics_version = "stats_epoch:19";

  snapshot.agent_worker_status = "running";
  snapshot.agent_worker_thread_count = 4;
  snapshot.agent_worker_actions_accepted = 32;
  snapshot.agent_worker_actions_refused = 2;

  snapshot.resource_governor_state = "foreground_unthrottled_background_limited";
  snapshot.resource_quota_grants = 100;
  snapshot.resource_quota_refusals = 5;
  snapshot.resource_quota_in_use = 8;

  snapshot.page_preallocation_demand_pages = 2048;
  snapshot.page_preallocation_granted_pages = 1536;
  snapshot.filespace_preallocation_demand_pages = 4096;
  snapshot.filespace_preallocation_granted_pages = 3072;
  snapshot.preallocation_refusal_count = 1;

  snapshot.cancellation_requested = true;
  snapshot.backpressure_active = true;
  snapshot.backpressure_state = "background_throttled";
  snapshot.backpressure_reason = "agent_resource_quota";
  snapshot.cancellation_checkpoint_count = 7;
  snapshot.backpressure_deferral_count = 3;

  snapshot.benchmark_correlation_id = "benchmark-cdp033-route-split";
  snapshot.support_bundle_correlation_id = "support-cdp033-bundle";
  snapshot.request_correlation_id = "request-cdp033-observability";
  return snapshot;
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field_name) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name) {
        return field.second.encoded_value;
      }
    }
  }
  return {};
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

const api::PerformanceOptimizationSurfaceField* SchemaField(std::string_view name) {
  for (const auto& field : api::PerformanceOptimizationSurfaceSchema()) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

void RequireSchemaField(std::string_view name,
                        std::string_view type,
                        std::string_view category) {
  const auto* field = SchemaField(name);
  Require(field != nullptr, std::string("surface field missing: ") + std::string(name));
  Require(field->type == type, std::string("surface field type mismatch: ") + std::string(name));
  Require(field->category == category,
          std::string("surface field category mismatch: ") + std::string(name));
  Require(field->required, std::string("surface field unexpectedly optional: ") + std::string(name));
}

void TestSchemaContract() {
  Require(api::PerformanceOptimizationSurfaceSchemaVersion() == 1,
          "CDP-033 surface schema version mismatch");
  Require(std::string(api::PerformanceOptimizationSurfaceSchemaId()) ==
              "scratchbird.performance_optimization_surface.v1",
          "CDP-033 surface schema id mismatch");

  RequireSchemaField("copy_append_batching_enabled", "bool", "flags");
  RequireSchemaField("copy_batches_completed", "uint64", "copy");
  RequireSchemaField("native_ingest_refusal_code", "text", "native_ingest");
  RequireSchemaField("plan_cache_invalidations", "uint64", "plan_cache");
  RequireSchemaField("descriptor_metadata_cache_epoch", "uint64", "metadata_cache");
  RequireSchemaField("stale_statistics_fail_safe_active", "bool", "statistics");
  RequireSchemaField("catalog_generation_id", "uint64", "epochs");
  RequireSchemaField("oldest_interesting_transaction_id", "uint64", "mga_horizons");
  RequireSchemaField("selected_join_plan_summary", "text", "planner");
  RequireSchemaField("agent_worker_thread_count", "uint64", "agents");
  RequireSchemaField("last_agent_decision", "text", "agent_decisions");
  RequireSchemaField("storage_row_version_backlog_count", "uint64", "backlogs");
  RequireSchemaField("secondary_index_state", "text", "index_states");
  RequireSchemaField("resource_quota_refusals", "uint64", "resource_governor");
  RequireSchemaField("filespace_preallocation_granted_pages", "uint64", "preallocation");
  RequireSchemaField("backpressure_state", "text", "cancellation_backpressure");
  RequireSchemaField("support_bundle_correlation_id", "text", "correlation");
  RequireSchemaField("exact_refusal_message_vector", "text", "message_vectors");
  RequireSchemaField("metric_family", "text", "metrics");
  RequireSchemaField("audit_event_family", "text", "audit");
  RequireSchemaField("support_bundle_completeness_state", "text", "support_bundle");
  RequireSchemaField("wal_recovery_authority", "bool", "authority_boundaries");

  std::set<std::string> categories;
  for (const auto& field : api::PerformanceOptimizationSurfaceSchema()) {
    categories.insert(field.category);
    Require(!Contains(field.name, "uuid"),
            "CDP-033 user surface exposed catalog UUID field");
  }
  const std::set<std::string> required_categories = {
      "flags",
      "copy",
      "native_ingest",
      "plan_cache",
      "metadata_cache",
      "statistics",
      "epochs",
      "planner",
      "mga_horizons",
      "agents",
      "agent_decisions",
      "backlogs",
      "index_states",
      "resource_governor",
      "preallocation",
      "cancellation_backpressure",
      "correlation",
      "message_vectors",
      "metrics",
      "audit",
      "support_bundle",
      "authority_boundaries"};
  for (const auto& category : required_categories) {
    Require(categories.count(category) == 1,
            "CDP-033 surface schema missing category " + category);
  }
}

void TestValidationAndSerialization() {
  const auto snapshot = RichSnapshot();
  const auto validation = api::ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  Require(validation.ok, "CDP-033 rich surface snapshot failed validation");

  const auto json = api::SerializePerformanceOptimizationSurfaceJson(snapshot);
  Require(Contains(json, "\"copy_append_batching_enabled\":true"),
          "serialized surface missing COPY batching flag");
  Require(Contains(json, "\"native_ingest_refusal_code\":\"INGEST.QUOTA_REFUSED\""),
          "serialized surface missing native ingest refusal code");
  Require(Contains(json, "\"plan_cache_hits\":44"),
          "serialized surface missing plan cache hits");
  Require(Contains(json, "\"descriptor_metadata_cache_epoch\":17"),
          "serialized surface missing descriptor cache epoch");
  Require(Contains(json, "\"stale_statistics_fail_safe_active\":true"),
          "serialized surface missing stale-stat fail-safe state");
  Require(Contains(json, "\"selected_join_algorithm\":\"hash\""),
          "serialized surface missing selected join algorithm");
  Require(Contains(json, "\"agent_worker_thread_count\":4"),
          "serialized surface missing agent thread count");
  Require(Contains(json, "\"resource_quota_refusals\":5"),
          "serialized surface missing governor refusal count");
  Require(Contains(json, "\"page_preallocation_granted_pages\":1536"),
          "serialized surface missing page preallocation grant count");
  Require(Contains(json, "\"backpressure_active\":true"),
          "serialized surface missing backpressure state");
  Require(Contains(json, "\"support_bundle_correlation_id\":\"support-cdp033-bundle\""),
          "serialized surface missing support-bundle correlation id");
  Require(Contains(json, "\"parser_finality_authority\":false"),
          "serialized surface missing parser finality denial");
  Require(Contains(json, "\"reference_finality_authority\":false"),
          "serialized surface missing reference finality denial");
  Require(!Contains(json, "docs" "/execution-plans"), "serialized surface depends on execution_plan path");

  auto invalid = snapshot;
  invalid.copy_batches_completed = invalid.copy_batches_started + 1;
  invalid.selected_join_algorithm.clear();
  const auto invalid_result = api::ValidatePerformanceOptimizationSurfaceSnapshot(invalid);
  Require(!invalid_result.ok, "invalid CDP-033 snapshot was accepted");
  Require(Contains(invalid_result.detail, "copy_batches_completed"),
          "invalid COPY batch counter was not diagnosed");
  Require(Contains(invalid_result.detail, "selected_join_algorithm"),
          "invalid planner summary was not diagnosed");
}

void TestEngineSurface() {
  api::EngineInspectPerformanceOptimizationSurfaceRequest request;
  request.context = Context();
  request.snapshot = RichSnapshot();
  request.snapshot_present = true;

  const auto result = api::EngineInspectPerformanceOptimizationSurface(request);
  Require(result.ok, "CDP-033 engine surface refused valid snapshot");
  Require(result.surface_schema_version == api::PerformanceOptimizationSurfaceSchemaVersion(),
          "CDP-033 result schema version mismatch");
  Require(result.management_api_ready, "CDP-033 management API flag not ready");
  Require(result.support_bundle_ready, "CDP-033 support bundle flag not ready");
  Require(result.sys_view_contract_ready, "CDP-033 sys view contract flag not ready");
  Require(!result.parser_finality_authority && !result.reference_finality_authority,
          "CDP-033 result claimed parser or reference finality");
  Require(FieldValue(result, "native_ingest_refusal_code") == "INGEST.QUOTA_REFUSED",
          "CDP-033 result missing native ingest refusal row");
  Require(FieldValue(result, "selected_join_plan_summary") ==
              "hash_join:left=orders:right=order_items",
          "CDP-033 result missing join plan summary");
  Require(FieldValue(result, "resource_governor_state") ==
              "foreground_unthrottled_background_limited",
          "CDP-033 result missing resource governor state");
  Require(FieldValue(result, "catalog_uuid_authority") == "engine",
          "CDP-033 result did not declare engine catalog UUID authority");
  Require(HasEvidence(result, "user_observability_surface", "CDP-033"),
          "CDP-033 surface evidence missing");
  Require(HasEvidence(result, "support_bundle_surface", "performance_optimization_surface"),
          "CDP-033 support bundle evidence missing");
  Require(Contains(result.management_api_json, "\"management_api\""),
          "CDP-033 management API JSON missing wrapper");
  Require(Contains(result.support_bundle_json, "\"performance_optimization_surface\""),
          "CDP-033 support bundle JSON missing section");
  Require(Contains(result.support_bundle_json, "\"forbidden_fields_absent\":true"),
          "CDP-033 support bundle JSON missing redaction proof");
  Require(!Contains(result.management_api_json, "docs" "/execution-plans"),
          "CDP-033 management API JSON depends on execution_plan path");
  Require(!Contains(result.support_bundle_json, "docs" "/execution-plans"),
          "CDP-033 support bundle JSON depends on execution_plan path");
}

void TestEngineSurfaceRefusals() {
  api::EngineInspectPerformanceOptimizationSurfaceRequest missing_security;
  missing_security.context = Context();
  missing_security.context.security_context_present = false;
  missing_security.snapshot = RichSnapshot();
  missing_security.snapshot_present = true;
  const auto denied = api::EngineInspectPerformanceOptimizationSurface(missing_security);
  Require(!denied.ok, "CDP-033 surface accepted missing security context");
  Require(HasDiagnostic(denied, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "CDP-033 surface security diagnostic drifted");

  api::EngineInspectPerformanceOptimizationSurfaceRequest invalid;
  invalid.context = Context();
  invalid.snapshot = RichSnapshot();
  invalid.snapshot.native_ingest_refused = true;
  invalid.snapshot.native_ingest_refusal_code = "none";
  invalid.snapshot_present = true;
  const auto refused = api::EngineInspectPerformanceOptimizationSurface(invalid);
  Require(!refused.ok, "CDP-033 surface accepted invalid refusal state");
  Require(HasDiagnostic(refused, "CDP.USER_OBSERVABILITY_SURFACE.INVALID_SNAPSHOT"),
          "CDP-033 invalid snapshot diagnostic drifted");
}

void TestShowManagementExposure() {
  api::EngineShowManagementRequest request;
  request.context = Context();
  const auto show = api::EngineShowManagement(request);
  Require(show.ok, "SHOW MANAGEMENT refused CDP-033 management surface");
  Require(HasEvidence(show,
                      "management_performance_optimization_surface",
                      api::PerformanceOptimizationSurfaceSchemaId()),
          "SHOW MANAGEMENT missing CDP-033 evidence");
  Require(FieldValue(show, "performance_optimization_surface") ==
              api::PerformanceOptimizationSurfaceSchemaId(),
          "SHOW MANAGEMENT missing performance surface schema id");
  Require(FieldValue(show, "copy_append_batching_enabled") == "true",
          "SHOW MANAGEMENT missing COPY batching flag");
  Require(FieldValue(show, "plan_cache_enabled") == "true",
          "SHOW MANAGEMENT missing plan cache flag");
  Require(FieldValue(show, "descriptor_metadata_cache_enabled") == "true",
          "SHOW MANAGEMENT missing descriptor cache flag");
  Require(FieldValue(show, "resource_governor_state") == "unthrottled",
          "SHOW MANAGEMENT missing resource governor state");
  Require(FieldValue(show, "parser_finality_authority") == "false",
          "SHOW MANAGEMENT did not deny parser finality authority");
}

}  // namespace

int main() {
  TestSchemaContract();
  TestValidationAndSerialization();
  TestEngineSurface();
  TestEngineSurfaceRefusals();
  TestShowManagementExposure();
  std::cout << "cdp_user_observability_surface_gate=passed\n";
  return EXIT_SUCCESS;
}
