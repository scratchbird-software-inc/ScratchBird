// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-108 management/catalog, EXPLAIN, and support surface gate.

#include "management/support_bundle_api.hpp"
#include "observability/explain_api.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "observability/show_api.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

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

std::string Id(platform::UuidKind kind, platform::u64 seed) {
  static std::map<std::pair<int, platform::u64>, std::string> generated_ids;
  const auto key = std::make_pair(static_cast<int>(kind), seed);
  const auto found = generated_ids.find(key);
  if (found != generated_ids.end()) {
    return found->second;
  }

  platform::TypedUuid generated_uuid;
  if (uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto generated =
        uuid::GenerateDurableEngineIdentityV7(kind, 1779524108000ull + seed);
    Require(generated.ok(), "ODF-108 generated UUID creation failed");
    generated_uuid = generated.value;
  } else {
    const auto raw =
        uuid::GenerateCompatibilityUnixTimeV7(1779524108000ull + seed);
    Require(raw.ok(), "ODF-108 generated UUID creation failed");
    const auto typed = uuid::MakeTypedUuid(kind, raw.value);
    Require(typed.ok(), "ODF-108 generated UUID creation failed");
    generated_uuid = typed.value;
  }

  const auto [inserted, _] =
      generated_ids.emplace(key, uuid::UuidToString(generated_uuid.value));
  return inserted->second;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.request_id = "odf108-management-explain-support";
  context.database_path = "/tmp/odf108-runtime.sbdb";
  context.database_uuid.canonical = Id(platform::UuidKind::database, 1);
  context.node_uuid.canonical = Id(platform::UuidKind::object, 2);
  context.session_uuid.canonical = Id(platform::UuidKind::session, 3);
  context.principal_uuid.canonical = Id(platform::UuidKind::principal, 4);
  context.transaction_uuid.canonical = Id(platform::UuidKind::transaction, 5);
  context.statement_uuid.canonical = Id(platform::UuidKind::object, 6);
  context.local_transaction_id = 108;
  context.catalog_generation_id = 2108;
  context.name_resolution_epoch = 3108;
  context.security_epoch = 4108;
  context.resource_epoch = 5108;
  context.trace_tags = {
      "right:OBS_MANAGEMENT_INSPECT",
      "right:OBS_INDEX_PROFILE_READ",
      "right:MGA_CLEANUP_INSPECT",
      "optimizer_deficiency_odf_108_gate"};
  return context;
}

api::PerformanceOptimizationSurfaceSnapshot RichSnapshot() {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "odf108_management_explain_support";
  snapshot.copy_batching_status = "active";
  snapshot.plan_cache_hits = 12;
  snapshot.plan_cache_misses = 2;
  snapshot.plan_cache_last_invalidation_reason = "none";
  snapshot.statistics_epoch = 108;
  snapshot.catalog_generation_id = 2108;
  snapshot.name_resolution_epoch = 3108;
  snapshot.security_epoch = 4108;
  snapshot.resource_epoch = 5108;
  snapshot.optimization_state_epoch = 6108;
  snapshot.selected_join_algorithm = "hash";
  snapshot.selected_join_plan_summary =
      "hash_join:left=orders:right=order_items";
  snapshot.selected_join_left_rows = 2048;
  snapshot.selected_join_right_rows = 8192;
  snapshot.selected_join_from_statistics = true;
  snapshot.selected_join_statistics_version = "statistics_epoch:108";
  snapshot.summary_prune_status = "summary_prune";
  snapshot.summary_prune_last_reason = "range_summary_exact_match";
  snapshot.summary_prune_fallback_reason = "none";
  snapshot.summary_prune_summary_status = "fresh";
  snapshot.summary_prune_generation = 108;
  snapshot.summary_prune_ranges_considered = 8;
  snapshot.summary_prune_ranges_pruned = 5;
  snapshot.summary_prune_ranges_scanned = 3;
  snapshot.summary_prune_pages_considered = 512;
  snapshot.summary_prune_pages_pruned = 320;
  snapshot.summary_prune_pages_scanned = 192;
  snapshot.agent_worker_status = "running";
  snapshot.agent_worker_thread_count = 4;
  snapshot.agent_worker_actions_accepted = 18;
  snapshot.agent_worker_actions_refused = 1;
  snapshot.agent_work_backlog_count = 3;
  snapshot.last_agent_type_id = "page_allocator";
  snapshot.last_agent_action = "preallocate_filespace_pages";
  snapshot.last_agent_decision = "accepted_preallocation";
  snapshot.last_agent_diagnostic_code =
      "ODF108.AGENT.PREALLOCATION.ACCEPTED";
  snapshot.storage_row_version_backlog_count = 9;
  snapshot.index_delta_backlog_count = 4;
  snapshot.index_garbage_backlog_count = 2;
  snapshot.page_summary_backlog_count = 1;
  snapshot.secondary_index_state = "online_delta_overlay";
  snapshot.shadow_index_state = "publish_pending";
  snapshot.summary_index_state = "fresh_authoritative";
  snapshot.specialized_index_state = "runtime_compatible";
  snapshot.index_state_authority_source = "engine_catalog_and_mga_inventory";
  snapshot.resource_governor_state = "foreground_admitted_background_limited";
  snapshot.resource_quota_grants = 44;
  snapshot.resource_quota_refusals = 1;
  snapshot.resource_quota_in_use = 6;
  snapshot.page_preallocation_demand_pages = 2048;
  snapshot.page_preallocation_granted_pages = 1536;
  snapshot.filespace_preallocation_demand_pages = 4096;
  snapshot.filespace_preallocation_granted_pages = 3072;
  snapshot.preallocation_refusal_count = 1;
  snapshot.backpressure_active = true;
  snapshot.backpressure_state = "quota_limited";
  snapshot.backpressure_reason = "resource_governor_background_quota";
  snapshot.cancellation_checkpoint_count = 6;
  snapshot.backpressure_deferral_count = 2;
  snapshot.benchmark_correlation_id = "odf108-benchmark";
  snapshot.support_bundle_correlation_id = "odf108-support";
  snapshot.request_correlation_id = "odf108-request";
  snapshot.exact_refusal_diagnostic_code = "ODF108.QUOTA.REFUSED";
  snapshot.exact_refusal_message_vector =
      "ODF108.QUOTA.REFUSED|RESOURCE.QUOTA.BACKPRESSURE|"
      "MGA.AUTHORITY.ENGINE_OWNS_FINALITY";
  snapshot.exact_refusal_source = "management.performance_optimization";
  snapshot.message_vector_ready = true;
  snapshot.metric_sample_count = 16;
  snapshot.audit_event_count = 5;
  snapshot.audit_last_decision = "refused_quota_over_limit";

  api::PerformanceOptimizationSelectedPathSurface selected_path;
  selected_path.path_id = "orders.customer_id.lookup";
  selected_path.path_kind = "secondary_index_access";
  selected_path.selected_path = "btree_index_lookup";
  selected_path.path_state = "selected";
  selected_path.authority_source = "engine_optimizer";
  selected_path.fallback_reason = "none";
  snapshot.odf108_selected_paths = {selected_path};

  api::PerformanceOptimizationFeatureGateSurface gate;
  gate.gate_id = "copy_append_batching_enabled";
  gate.enabled = true;
  gate.gate_state = "enabled";
  gate.authority_source = "engine_config_policy";
  gate.refusal_code = "none";
  api::PerformanceOptimizationFeatureGateSurface disabled_gate;
  disabled_gate.gate_id = "native_ingest_enabled";
  disabled_gate.enabled = false;
  disabled_gate.gate_state = "disabled_by_policy";
  disabled_gate.authority_source = "engine_config_policy";
  disabled_gate.refusal_code = "ODF108.FEATURE.DISABLED_BY_POLICY";
  snapshot.odf108_feature_gates = {gate, disabled_gate};

  api::PerformanceOptimizationFallbackSurface fallback;
  fallback.route_id = "summary_prune";
  fallback.reason_code = "none";
  fallback.detail = "summary_index_authoritative";
  fallback.scalar_fallback_state = "not_required";
  snapshot.odf108_fallbacks = {fallback};

  api::PerformanceOptimizationQuotaSurface quota;
  quota.quota_family = "odf101_resource_governor";
  quota.quota_state = "backpressure";
  quota.quota_limit = 8;
  quota.quota_requested = 10;
  quota.quota_in_use = 6;
  quota.grants = 44;
  quota.refusals = 1;
  quota.action = "defer_background_work";
  quota.diagnostic_code = "ODF108.QUOTA.REFUSED";
  snapshot.odf108_quotas = {quota};

  api::PerformanceOptimizationRuntimeCompatibilitySurface compatibility;
  compatibility.route_id = "odf107.direct_binary_result_frame";
  compatibility.compatibility_status = "compatible";
  compatibility.compatibility_action = "admit";
  compatibility.runtime_generation = 107;
  compatibility.required_capabilities = "little_endian,scalar_exact";
  compatibility.provider_capabilities =
      "little_endian,scalar_exact,direct_binary_frame";
  compatibility.diagnostic_code = "none";
  compatibility.fallback_reason = "none";
  snapshot.odf108_runtime_compatibility = {compatibility};

  api::PerformanceOptimizationRebuildSurface rebuild;
  rebuild.object_kind = "secondary_index";
  rebuild.rebuild_state = "running";
  rebuild.rebuild_phase = "backfill";
  rebuild.generation = 108;
  rebuild.progress_numerator = 64;
  rebuild.progress_denominator = 128;
  rebuild.refusal_code = "none";
  snapshot.odf108_rebuild_states = {rebuild};

  api::PerformanceOptimizationExactRefusalSurface refusal;
  refusal.source = snapshot.exact_refusal_source;
  refusal.diagnostic_code = snapshot.exact_refusal_diagnostic_code;
  refusal.message_vector = snapshot.exact_refusal_message_vector;
  refusal.refusal_state = "refused";
  refusal.redaction_state = "public_safe_summary";
  refusal.public_safe = true;
  snapshot.odf108_exact_refusals = {refusal};
  return snapshot;
}

std::string Field(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) {
      return field.second.encoded_value;
    }
  }
  return {};
}

bool HasRowField(const api::EngineApiResult& result,
                 std::string_view field,
                 std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    if (Field(row, field) == value) {
      return true;
    }
  }
  return false;
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
    if (diagnostic.code == code || Contains(diagnostic.detail, code)) {
      return true;
    }
  }
  return false;
}

const api::PerformanceOptimizationSurfaceField* SchemaField(
    std::string_view name) {
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
  Require(field != nullptr, "ODF-108 schema field missing");
  Require(field->type == type, "ODF-108 schema field type mismatch");
  Require(field->category == category,
          "ODF-108 schema field category mismatch");
  Require(field->required, "ODF-108 schema field unexpectedly optional");
}

void RequireCleanPayload(std::string_view payload, std::string_view surface) {
  for (const auto token :
       {"docs/", "execution-plans", "findings", "contracts", "references",
        "parser_finality_authority\":true", "reference_finality_authority\":true",
        "client_finality_authority\":true",
        "storage_shortcut_finality_authority\":true",
        "wal_recovery_authority\":true", "write_ahead_log"}) {
    Require(!Contains(payload, token), surface);
  }
}

void RequireCleanResultRows(const api::EngineApiResult& result) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      RequireCleanPayload(field.second.encoded_value, "ODF-108 row hygiene");
    }
  }
  for (const auto& evidence : result.evidence) {
    RequireCleanPayload(evidence.evidence_kind, "ODF-108 evidence hygiene");
    RequireCleanPayload(evidence.evidence_id, "ODF-108 evidence hygiene");
  }
}

void TestSchemaAndValidation() {
  const std::string category = "odf108_management_explain_support";
  RequireSchemaField("odf108_surface_ready", "bool", category);
  RequireSchemaField("odf108_selected_path_count", "uint64", category);
  RequireSchemaField("odf108_feature_gate_count", "uint64", category);
  RequireSchemaField("odf108_fallback_reason_count", "uint64", category);
  RequireSchemaField("odf108_quota_state_count", "uint64", category);
  RequireSchemaField("odf108_runtime_compatibility_count", "uint64", category);
  RequireSchemaField("odf108_rebuild_state_count", "uint64", category);
  RequireSchemaField("odf108_exact_refusal_count", "uint64", category);
  RequireSchemaField("odf108_selected_path", "text", category);
  RequireSchemaField("odf108_feature_gate", "text", category);
  RequireSchemaField("odf108_fallback_reason", "text", category);
  RequireSchemaField("odf108_quota_state", "text", category);
  RequireSchemaField("odf108_runtime_compatibility_status", "text", category);
  RequireSchemaField("odf108_rebuild_state", "text", category);
  RequireSchemaField("odf108_exact_refusal_state", "text", category);

  const auto snapshot = RichSnapshot();
  const auto validation =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  Require(validation.ok, "ODF-108 rich snapshot failed validation");

  auto missing = snapshot;
  missing.odf108_selected_paths.clear();
  const auto missing_result =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(missing);
  Require(!missing_result.ok, "ODF-108 accepted missing selected paths");
  Require(Contains(missing_result.detail, "odf108_selected_paths"),
          "ODF-108 missing selected path diagnostic mismatch");

  auto bad_quota = snapshot;
  bad_quota.odf108_quotas.front().refusals = 1;
  bad_quota.odf108_quotas.front().diagnostic_code = "none";
  const auto quota_result =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(bad_quota);
  Require(!quota_result.ok, "ODF-108 accepted refused quota without diagnostic");
  Require(Contains(quota_result.detail, "odf108_quotas.diagnostic_code"),
          "ODF-108 quota diagnostic mismatch");

  auto authority_drift = snapshot;
  authority_drift.parser_finality_authority = true;
  const auto authority_result =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(authority_drift);
  Require(!authority_result.ok, "ODF-108 accepted parser finality authority");
  Require(Contains(authority_result.detail, "non_authority_boundary"),
          "ODF-108 authority diagnostic mismatch");

  auto dirty = snapshot;
  dirty.odf108_selected_paths.front().fallback_reason =
      "docs" "/execution-plans/runtime_dependency";
  const auto dirty_result =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(dirty);
  Require(!dirty_result.ok, "ODF-108 accepted execution_plan runtime dependency");
  Require(Contains(dirty_result.detail, "forbidden_runtime_token"),
          "ODF-108 runtime dependency diagnostic mismatch");
}

void TestManagementRowsAndJson() {
  api::EngineInspectPerformanceOptimizationSurfaceRequest request;
  request.context = Context();
  request.snapshot = RichSnapshot();
  request.snapshot_present = true;

  const auto result = api::EngineInspectPerformanceOptimizationSurface(request);
  Require(result.ok, "ODF-108 management surface refused rich snapshot");
  Require(result.management_api_ready && result.support_bundle_ready &&
              result.sys_view_contract_ready,
          "ODF-108 readiness flags incomplete");
  Require(HasEvidence(result, "management_explain_support_surface", "ODF-108"),
          "ODF-108 management evidence missing");
  Require(HasRowField(result, "odf108_surface_ready", "true"),
          "ODF-108 management row missing readiness");
  Require(HasRowField(result, "record_kind", "odf108_selected_path"),
          "ODF-108 selected path row missing");
  Require(HasRowField(result, "selected_path", "btree_index_lookup"),
          "ODF-108 selected path value missing");
  Require(HasRowField(result, "record_kind", "odf108_feature_gate"),
          "ODF-108 feature gate row missing");
  Require(HasRowField(result, "gate_id", "native_ingest_enabled"),
          "ODF-108 feature gate value missing");
  Require(HasRowField(result, "record_kind", "odf108_quota_state"),
          "ODF-108 quota row missing");
  Require(HasRowField(result, "diagnostic_code", "ODF108.QUOTA.REFUSED"),
          "ODF-108 exact quota diagnostic row missing");
  Require(HasRowField(result, "record_kind", "odf108_runtime_compatibility"),
          "ODF-108 compatibility row missing");
  Require(HasRowField(result, "runtime_generation", "107"),
          "ODF-108 runtime generation row missing");
  Require(HasRowField(result, "record_kind", "odf108_rebuild_state"),
          "ODF-108 rebuild state row missing");
  Require(HasRowField(result, "rebuild_phase", "backfill"),
          "ODF-108 rebuild phase row missing");
  Require(Contains(result.management_api_json, "\"odf108_selected_paths\""),
          "ODF-108 management JSON missing selected paths");
  Require(Contains(result.management_api_json,
                   "\"selected_path\":\"btree_index_lookup\""),
          "ODF-108 management JSON missing selected path value");
  Require(Contains(result.management_api_json,
                   "\"gate_id\":\"native_ingest_enabled\""),
          "ODF-108 management JSON missing feature gate");
  Require(Contains(result.management_api_json,
                   "\"quota_family\":\"odf101_resource_governor\""),
          "ODF-108 management JSON missing quota state");
  Require(Contains(result.management_api_json,
                   "\"route_id\":\"odf107.direct_binary_result_frame\""),
          "ODF-108 management JSON missing runtime compatibility");
  Require(Contains(result.management_api_json, "\"rebuild_phase\":\"backfill\""),
          "ODF-108 management JSON missing rebuild state");
  Require(Contains(result.management_api_json,
                   request.snapshot.exact_refusal_message_vector),
          "ODF-108 management JSON missing exact refusal vector");
  RequireCleanPayload(result.management_api_json, "ODF-108 management JSON");
  RequireCleanPayload(result.support_bundle_json, "ODF-108 support JSON");
  RequireCleanResultRows(result);
}

void TestShowAndSupportBundleApi() {
  api::EngineShowManagementRequest show_request;
  show_request.context = Context();
  show_request.performance_optimization_snapshot = RichSnapshot();
  show_request.performance_optimization_snapshot_present = true;

  const auto show = api::EngineShowManagement(show_request);
  Require(show.ok, "ODF-108 SHOW MANAGEMENT refused rich snapshot");
  Require(HasRowField(show, "odf108_selected_path_count", "1"),
          "ODF-108 SHOW MANAGEMENT missing selected path count");
  Require(HasRowField(show, "selected_path", "btree_index_lookup"),
          "ODF-108 SHOW MANAGEMENT missing selected path row");
  Require(HasRowField(show, "compatibility_status", "compatible"),
          "ODF-108 SHOW MANAGEMENT missing compatibility status");
  RequireCleanResultRows(show);

  api::EnginePrepareSupportBundleRequest bundle;
  bundle.context = Context();
  bundle.option_envelopes.push_back("engine_authorized_support_export:true");
  bundle.performance_optimization_snapshot = RichSnapshot();
  bundle.performance_optimization_snapshot_present = true;
  const auto prepared = api::EnginePrepareSupportBundle(bundle);
  Require(prepared.ok, "ODF-108 support bundle refused rich snapshot");
  Require(prepared.performance_optimization_surface_collected,
          "ODF-108 support bundle missed surface");
  Require(Contains(prepared.support_bundle_json,
                   "\"odf108_runtime_compatibility\""),
          "ODF-108 support bundle JSON missing runtime compatibility");
  Require(HasRowField(prepared, "bundle_record_kind",
                      "performance_optimization_surface"),
          "ODF-108 support bundle missing performance row");
  Require(HasRowField(prepared, "odf108_quota_state_count", "1"),
          "ODF-108 support bundle missing quota count row");
  RequireCleanPayload(prepared.support_bundle_json, "ODF-108 support bundle JSON");
  RequireCleanResultRows(prepared);
}

void TestExplainSurface() {
  api::EngineExplainOperationRequest explain;
  explain.context = Context();
  explain.operation_id = "query.scan";
  explain.target_object.uuid.canonical = Id(platform::UuidKind::object, 100);
  explain.target_object.object_kind = "table";
  explain.performance_optimization_snapshot = RichSnapshot();
  explain.performance_optimization_snapshot_present = true;
  const auto explained = api::EngineExplainOperation(explain);
  Require(explained.ok, "ODF-108 EXPLAIN operation refused rich snapshot");
  Require(HasEvidence(explained, "management_explain_support_surface", "ODF-108"),
          "ODF-108 EXPLAIN evidence missing");
  Require(HasEvidence(explained,
                      "explain_performance_optimization_surface",
                      api::PerformanceOptimizationSurfaceSchemaId()),
          "ODF-108 EXPLAIN surface schema evidence missing");
  Require(HasRowField(explained, "selected_path", "btree_index_lookup"),
          "ODF-108 EXPLAIN missing selected path row");
  Require(HasRowField(explained, "quota_family", "odf101_resource_governor"),
          "ODF-108 EXPLAIN missing quota row");
  Require(HasRowField(explained, "compatibility_status", "compatible"),
          "ODF-108 EXPLAIN missing compatibility row");
  RequireCleanResultRows(explained);

  api::EngineExplainOptimizerEvidenceRequest evidence;
  evidence.context = Context();
  evidence.operation_id = "optimizer.odf108";
  evidence.candidate_evidence_rows = {"candidate:btree_index_lookup"};
  evidence.performance_optimization_snapshot = RichSnapshot();
  evidence.performance_optimization_snapshot_present = true;
  const auto optimizer = api::EngineExplainOptimizerEvidence(evidence);
  Require(optimizer.ok, "ODF-108 optimizer evidence refused rich snapshot");
  Require(HasRowField(optimizer, "candidate_evidence",
                      "candidate:btree_index_lookup"),
          "ODF-108 optimizer evidence missing candidate row");
  Require(HasRowField(optimizer, "rebuild_phase", "backfill"),
          "ODF-108 optimizer evidence missing rebuild row");
  RequireCleanResultRows(optimizer);

  auto invalid = explain;
  invalid.performance_optimization_snapshot.odf108_runtime_compatibility.front()
      .compatibility_status = "refused";
  invalid.performance_optimization_snapshot.odf108_runtime_compatibility.front()
      .diagnostic_code = "none";
  const auto refused = api::EngineExplainOperation(invalid);
  Require(!refused.ok, "ODF-108 EXPLAIN accepted invalid compatibility state");
  Require(HasDiagnostic(refused,
                        "CDP.USER_OBSERVABILITY_SURFACE.INVALID_SNAPSHOT"),
          "ODF-108 EXPLAIN invalid snapshot diagnostic mismatch");
}

}  // namespace

int main() {
  TestSchemaAndValidation();
  TestManagementRowsAndJson();
  TestShowAndSupportBundleApi();
  TestExplainSurface();
  std::cout << "optimizer_deficiency_odf_108_gate=passed\n";
  return EXIT_SUCCESS;
}
