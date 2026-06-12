// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle_test_memory.hpp"
#include "management/support_bundle_api.hpp"
#include "observability/metrics_api.hpp"
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

namespace {

namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_MANAGEMENT_OBSERVABILITY_SUPPORT_BUNDLE_GATE";

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
        uuid::GenerateDurableEngineIdentityV7(kind, 1779524061000ull + seed);
    Require(generated.ok(), "DPC-061 generated UUID creation failed");
    generated_uuid = generated.value;
  } else {
    const auto raw =
        uuid::GenerateCompatibilityUnixTimeV7(1779524061000ull + seed);
    Require(raw.ok(), "DPC-061 generated UUID creation failed");
    const auto typed = uuid::MakeTypedUuid(kind, raw.value);
    Require(typed.ok(), "DPC-061 generated UUID creation failed");
    generated_uuid = typed.value;
  }
  const auto [inserted, _] =
      generated_ids.emplace(key, uuid::UuidToString(generated_uuid.value));
  return inserted->second;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.request_id = "dpc061-management-observability";
  context.database_path = "/tmp/dpc061-runtime.sbdb";
  context.database_uuid.canonical = Id(platform::UuidKind::database, 1);
  context.node_uuid.canonical = Id(platform::UuidKind::object, 2);
  context.session_uuid.canonical = Id(platform::UuidKind::session, 3);
  context.principal_uuid.canonical = Id(platform::UuidKind::principal, 4);
  context.transaction_uuid.canonical = Id(platform::UuidKind::transaction, 5);
  context.catalog_generation_id = 1061;
  context.name_resolution_epoch = 2061;
  context.security_epoch = 3061;
  context.resource_epoch = 4061;
  context.local_transaction_id = 61;
  context.trace_tags = {
      "right:OBS_METRICS_READ_FAMILY",
      "right:OBS_METRICS_READ_ALL",
      "right:OBS_MANAGEMENT_INSPECT",
      "right:OBS_CONFIG_INSPECT",
      "right:OBS_AGENT_STATE_READ",
      "dpc_management_observability_support_bundle_gate"};
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "dpc_management_observability_support_bundle_gate",
      {"OBS_METRICS_READ_FAMILY",
       "OBS_METRICS_READ_ALL",
       "OBS_MANAGEMENT_INSPECT",
       "OBS_CONFIG_INSPECT",
       "OBS_AGENT_STATE_READ",
       "OBS_INDEX_PROFILE_READ",
       "MGA_CLEANUP_INSPECT",
       "SUPPORT_EXPORT"});
  return context;
}

api::PerformanceOptimizationSurfaceSnapshot RichSnapshot() {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "dpc061_p6_aggregate_surface";
  snapshot.native_ingest_enabled = true;
  snapshot.native_ingest_state = "refused";
  snapshot.native_ingest_rows_processed = 64;
  snapshot.native_ingest_rows_total = 128;
  snapshot.native_ingest_refused = true;
  snapshot.native_ingest_refusal_code =
      "DPC.OBSERVABILITY.NATIVE_INGEST_REFUSED";
  snapshot.plan_cache_hits = 17;
  snapshot.plan_cache_misses = 3;
  snapshot.plan_cache_invalidations = 2;
  snapshot.plan_cache_last_invalidation_reason = "catalog_epoch_changed";
  snapshot.descriptor_metadata_cache_epoch = 91;
  snapshot.statistics_epoch = 92;
  snapshot.stale_statistics_fail_safe_active = true;
  snapshot.stale_statistics_fail_safe_reason = "statistics_horizon_lag";
  snapshot.catalog_generation_id = 1061;
  snapshot.name_resolution_epoch = 2061;
  snapshot.security_epoch = 3061;
  snapshot.resource_epoch = 4061;
  snapshot.optimization_state_epoch = 5061;

  snapshot.selected_join_algorithm = "merge";
  snapshot.selected_join_plan_summary = "merge_join:index=orders_customer_idx";
  snapshot.selected_join_left_rows = 4096;
  snapshot.selected_join_right_rows = 2048;
  snapshot.selected_join_from_statistics = true;
  snapshot.selected_join_statistics_version = "statistics_epoch:92";

  snapshot.summary_prune_status = "summary_prune";
  snapshot.summary_prune_last_reason = "range_summary_exact_match";
  snapshot.summary_prune_fallback_reason = "none";
  snapshot.summary_prune_summary_status = "fresh";
  snapshot.summary_prune_generation = 93;
  snapshot.summary_prune_ranges_considered = 10;
  snapshot.summary_prune_ranges_pruned = 6;
  snapshot.summary_prune_ranges_scanned = 4;
  snapshot.summary_prune_pages_considered = 100;
  snapshot.summary_prune_pages_pruned = 70;
  snapshot.summary_prune_pages_scanned = 30;

  snapshot.cleanup_horizon_authority_status = "authoritative";
  snapshot.cleanup_horizon_authoritative = true;
  snapshot.cleanup_horizon_local_transaction_id = 61;
  snapshot.oldest_interesting_transaction_id = 41;
  snapshot.oldest_active_transaction_id = 44;
  snapshot.oldest_snapshot_transaction_id = 47;
  snapshot.oldest_cleanup_transaction_id = 49;

  snapshot.agent_worker_status = "running";
  snapshot.agent_worker_thread_count = 4;
  snapshot.agent_worker_actions_accepted = 12;
  snapshot.agent_worker_actions_refused = 1;
  snapshot.agent_work_backlog_count = 5;
  snapshot.last_agent_type_id = "index_garbage_cleanup";
  snapshot.last_agent_action = "drain_index_garbage_backlog";
  snapshot.last_agent_decision = "accepted_cleanup_batch";
  snapshot.last_agent_diagnostic_code =
      "DPC.OBSERVABILITY.AGENT_DECISION.ACCEPTED";

  snapshot.storage_row_version_backlog_count = 8;
  snapshot.index_delta_backlog_count = 7;
  snapshot.index_garbage_backlog_count = 6;
  snapshot.page_summary_backlog_count = 2;
  snapshot.secondary_index_state = "online_delta_overlay";
  snapshot.shadow_index_state = "build_visible_after_publish";
  snapshot.summary_index_state = "fresh_authoritative";
  snapshot.specialized_index_state = "vector_generation_published";
  snapshot.index_state_authority_source = "engine_catalog_and_mga_inventory";

  snapshot.resource_governor_state = "background_limited";
  snapshot.resource_quota_grants = 15;
  snapshot.resource_quota_refusals = 1;
  snapshot.resource_quota_in_use = 3;
  snapshot.backpressure_active = true;
  snapshot.backpressure_state = "agent_backlog_throttled";
  snapshot.backpressure_reason = "resource_governor_background_quota";
  snapshot.backpressure_deferral_count = 4;
  snapshot.benchmark_correlation_id = "dpc061-benchmark-evidence";
  snapshot.support_bundle_correlation_id = "dpc061-support-bundle";
  snapshot.request_correlation_id = "dpc061-request";

  snapshot.exact_refusal_diagnostic_code =
      "DPC.OBSERVABILITY.NON_AUTHORITATIVE_INPUT_REFUSED";
  snapshot.exact_refusal_message_vector =
      "DPC.OBSERVABILITY.NON_AUTHORITATIVE_INPUT_REFUSED|"
      "SBLR.ENVELOPE.AUTHORITY_REQUIRED|MGA.BOUNDARY.ENGINE_OWNS_FINALITY";
  snapshot.exact_refusal_source = "management.performance_optimization";
  snapshot.message_vector_ready = true;
  snapshot.metric_sample_count = 8;
  snapshot.audit_event_count = 3;
  snapshot.audit_last_decision = "refused_non_authoritative_input";
  snapshot.support_bundle_redaction_state = "public_safe_summary";
  snapshot.support_bundle_completeness_state = "complete";
  snapshot.support_bundle_forbidden_fields_absent = true;
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
                 std::string_view field_name,
                 std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    if (Field(row, field_name) == value) {
      return true;
    }
  }
  return false;
}

bool HasRowFieldContaining(const api::EngineApiResult& result,
                           std::string_view field_name,
                           std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    if (Contains(Field(row, field_name), value)) {
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
  Require(field != nullptr, std::string("DPC-061 schema field missing: ") +
                                std::string(name));
  Require(field->type == type, std::string("DPC-061 schema type mismatch: ") +
                                  std::string(name));
  Require(field->category == category,
          std::string("DPC-061 schema category mismatch: ") +
              std::string(name));
  Require(field->required,
          std::string("DPC-061 schema field unexpectedly optional: ") +
              std::string(name));
}

void RequireNoUnsafePayload(std::string_view value, std::string_view surface) {
  Require(!Contains(value, "password=cleartext"),
          std::string(surface) + " leaked cleartext password");
  Require(!Contains(value, "secret-token"),
          std::string(surface) + " leaked secret token");
  Require(!Contains(value, "/tmp/protected"),
          std::string(surface) + " leaked protected path");
  Require(!Contains(value, "docs" "/execution-plans"),
          std::string(surface) + " depends on execution_plan artifacts");
}

void TestSchemaAndValidation() {
  RequireSchemaField("catalog_generation_id", "uint64", "epochs");
  RequireSchemaField("oldest_interesting_transaction_id",
                     "uint64",
                     "mga_horizons");
  RequireSchemaField("oldest_active_transaction_id",
                     "uint64",
                     "mga_horizons");
  RequireSchemaField("oldest_snapshot_transaction_id",
                     "uint64",
                     "mga_horizons");
  RequireSchemaField("storage_row_version_backlog_count",
                     "uint64",
                     "backlogs");
  RequireSchemaField("index_delta_backlog_count", "uint64", "backlogs");
  RequireSchemaField("index_garbage_backlog_count", "uint64", "backlogs");
  RequireSchemaField("last_agent_decision", "text", "agent_decisions");
  RequireSchemaField("secondary_index_state", "text", "index_states");
  RequireSchemaField("exact_refusal_diagnostic_code",
                     "text",
                     "message_vectors");
  RequireSchemaField("exact_refusal_message_vector",
                     "text",
                     "message_vectors");
  RequireSchemaField("metric_family", "text", "metrics");
  RequireSchemaField("audit_event_family", "text", "audit");
  RequireSchemaField("support_bundle_completeness_state",
                     "text",
                     "support_bundle");
  RequireSchemaField("parser_finality_authority",
                     "bool",
                     "authority_boundaries");
  RequireSchemaField("reference_finality_authority",
                     "bool",
                     "authority_boundaries");
  RequireSchemaField("wal_recovery_authority",
                     "bool",
                     "authority_boundaries");

  std::set<std::string> required_categories = {
      "flags",
      "epochs",
      "mga_horizons",
      "backlogs",
      "agent_decisions",
      "index_states",
      "message_vectors",
      "metrics",
      "audit",
      "support_bundle",
      "authority_boundaries"};
  for (const auto& field : api::PerformanceOptimizationSurfaceSchema()) {
    required_categories.erase(field.category);
  }
  Require(required_categories.empty(),
          "DPC-061 schema missing aggregate P6 field category");

  const auto snapshot = RichSnapshot();
  const auto validation =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  Require(validation.ok, "DPC-061 rich snapshot failed validation");

  auto missing_vector = snapshot;
  missing_vector.message_vector_ready = false;
  const auto vector_refused =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(missing_vector);
  Require(!vector_refused.ok,
          "DPC-061 snapshot accepted missing message vector readiness");
  Require(Contains(vector_refused.detail, "exact_refusal_message_vector"),
          "DPC-061 missing message vector diagnostic mismatch");

  auto authority_drift = snapshot;
  authority_drift.reference_finality_authority = true;
  const auto authority_refused =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(authority_drift);
  Require(!authority_refused.ok,
          "DPC-061 snapshot accepted reference finality authority");
  Require(Contains(authority_refused.detail, "non_authority_boundary"),
          "DPC-061 non-authority diagnostic mismatch");
}

void TestManagementAndSupportBundleJson() {
  api::EngineInspectPerformanceOptimizationSurfaceRequest request;
  request.context = Context();
  request.snapshot = RichSnapshot();
  request.snapshot_present = true;

  const auto result = api::EngineInspectPerformanceOptimizationSurface(request);
  Require(result.ok, "DPC-061 performance surface refused rich snapshot");
  Require(result.management_api_ready && result.support_bundle_ready &&
              result.sys_view_contract_ready,
          "DPC-061 readiness flags were incomplete");
  Require(!result.parser_finality_authority && !result.reference_finality_authority,
          "DPC-061 result claimed parser or reference finality authority");
  Require(HasEvidence(result, "user_observability_surface", "DPC-061"),
          "DPC-061 result missing DPC-061 evidence marker");
  Require(HasEvidence(result,
                      "performance_optimization_message_vector",
                      request.snapshot.exact_refusal_message_vector),
          "DPC-061 result missing message-vector evidence");
  Require(HasEvidence(result,
                      "performance_optimization_audit_surface",
                      request.snapshot.audit_event_family),
          "DPC-061 result missing audit-surface evidence");

  const auto& management_json = result.management_api_json;
  Require(Contains(management_json,
                   "\"cleanup_horizon_authority_status\":\"authoritative\""),
          "DPC-061 management JSON missing cleanup authority status");
  Require(Contains(management_json, "\"oldest_interesting_transaction_id\":41"),
          "DPC-061 management JSON missing OIT field");
  Require(Contains(management_json, "\"oldest_active_transaction_id\":44"),
          "DPC-061 management JSON missing OAT field");
  Require(Contains(management_json, "\"oldest_snapshot_transaction_id\":47"),
          "DPC-061 management JSON missing OST field");
  Require(Contains(management_json, "\"index_delta_backlog_count\":7"),
          "DPC-061 management JSON missing index delta backlog");
  Require(Contains(management_json,
                   "\"last_agent_decision\":\"accepted_cleanup_batch\""),
          "DPC-061 management JSON missing agent decision");
  Require(Contains(management_json,
                   "\"secondary_index_state\":\"online_delta_overlay\""),
          "DPC-061 management JSON missing secondary index state");
  Require(Contains(management_json,
                   "\"exact_refusal_diagnostic_code\":\"DPC.OBSERVABILITY."
                   "NON_AUTHORITATIVE_INPUT_REFUSED\""),
          "DPC-061 management JSON missing exact refusal diagnostic");
  Require(Contains(management_json, "\"parser_finality_authority\":false"),
          "DPC-061 management JSON missing parser non-authority");
  Require(Contains(management_json, "\"reference_finality_authority\":false"),
          "DPC-061 management JSON missing reference non-authority");
  Require(Contains(management_json, "\"wal_recovery_authority\":false"),
          "DPC-061 management JSON missing WAL non-authority");

  const auto& support_json = result.support_bundle_json;
  Require(Contains(support_json,
                   "\"section\":\"performance_optimization_surface\""),
          "DPC-061 support-bundle JSON missing section");
  Require(Contains(support_json, "\"completeness_state\":\"complete\""),
          "DPC-061 support-bundle JSON missing completeness proof");
  Require(Contains(support_json, "\"forbidden_fields_absent\":true"),
          "DPC-061 support-bundle JSON missing redaction proof");
  Require(Contains(support_json, "\"storage_row_version_backlog_count\":8"),
          "DPC-061 support-bundle JSON missing storage backlog");
  Require(Contains(support_json, request.snapshot.exact_refusal_message_vector),
          "DPC-061 support-bundle JSON missing exact message vector");
  RequireNoUnsafePayload(management_json, "management JSON");
  RequireNoUnsafePayload(support_json, "support-bundle JSON");
}

void TestShowManagementRows() {
  api::EngineShowManagementRequest request;
  request.context = Context();
  request.performance_optimization_snapshot = RichSnapshot();
  request.performance_optimization_snapshot_present = true;

  const auto result = api::EngineShowManagement(request);
  Require(result.ok, "DPC-061 SHOW MANAGEMENT refused rich snapshot");
  Require(HasRowField(result, "catalog_generation_id", "1061"),
          "DPC-061 SHOW MANAGEMENT missing catalog epoch");
  Require(HasRowField(result, "security_epoch", "3061"),
          "DPC-061 SHOW MANAGEMENT missing security epoch");
  Require(HasRowField(result, "resource_epoch", "4061"),
          "DPC-061 SHOW MANAGEMENT missing resource epoch");
  Require(HasRowField(result, "cleanup_horizon_authority_status",
                      "authoritative"),
          "DPC-061 SHOW MANAGEMENT missing cleanup horizon status");
  Require(HasRowField(result, "oldest_interesting_transaction_id", "41"),
          "DPC-061 SHOW MANAGEMENT missing OIT row");
  Require(HasRowField(result, "oldest_active_transaction_id", "44"),
          "DPC-061 SHOW MANAGEMENT missing OAT row");
  Require(HasRowField(result, "oldest_snapshot_transaction_id", "47"),
          "DPC-061 SHOW MANAGEMENT missing OST row");
  Require(HasRowField(result, "storage_row_version_backlog_count", "8"),
          "DPC-061 SHOW MANAGEMENT missing storage backlog");
  Require(HasRowField(result, "index_delta_backlog_count", "7"),
          "DPC-061 SHOW MANAGEMENT missing index delta backlog");
  Require(HasRowField(result, "index_garbage_backlog_count", "6"),
          "DPC-061 SHOW MANAGEMENT missing index garbage backlog");
  Require(HasRowField(result, "last_agent_decision",
                      "accepted_cleanup_batch"),
          "DPC-061 SHOW MANAGEMENT missing agent decision");
  Require(HasRowField(result, "secondary_index_state",
                      "online_delta_overlay"),
          "DPC-061 SHOW MANAGEMENT missing secondary index state");
  Require(HasRowField(result, "exact_refusal_diagnostic_code",
                      "DPC.OBSERVABILITY.NON_AUTHORITATIVE_INPUT_REFUSED"),
          "DPC-061 SHOW MANAGEMENT missing exact refusal code");
  Require(HasRowField(result, "support_bundle_completeness_state",
                      "complete"),
          "DPC-061 SHOW MANAGEMENT missing support-bundle completeness");
  Require(HasRowField(result, "parser_finality_authority", "false"),
          "DPC-061 SHOW MANAGEMENT claimed parser finality authority");
  Require(HasRowField(result, "reference_finality_authority", "false"),
          "DPC-061 SHOW MANAGEMENT claimed reference finality authority");
  Require(HasRowField(result, "wal_recovery_authority", "false"),
          "DPC-061 SHOW MANAGEMENT claimed WAL recovery authority");
}

api::EngineSupportBundleAgentEvidenceSource AgentEvidence() {
  api::EngineSupportBundleAgentEvidenceSource source;
  source.agent_type_id = "index_garbage_cleanup";
  source.agent_uuid = Id(platform::UuidKind::object, 20);
  source.filespace_uuid = Id(platform::UuidKind::filespace, 21);
  source.policy_uuid = Id(platform::UuidKind::object, 22);
  source.evidence_uuid = Id(platform::UuidKind::object, 23);
  source.evidence_kind = "dpc061_agent_decision";
  source.result_state = "success";
  source.diagnostic_code = "DPC.OBSERVABILITY.AGENT_DECISION.ACCEPTED";
  source.payload_digest = "sha256:dpc061";
  source.physical_path = "/tmp/protected/dpc061/runtime.sbdb";
  source.unsafe_payload = "password=cleartext token=secret-token";
  source.payload_redacted = true;
  return source;
}

void TestSupportBundleApiCompleteness() {
  api::EnginePrepareSupportBundleRequest request;
  request.context = Context();
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  request.performance_optimization_snapshot = RichSnapshot();
  request.performance_optimization_snapshot_present = true;
  request.agent_runtime_evidence.push_back(AgentEvidence());

  const auto result = api::EnginePrepareSupportBundle(request);
  Require(result.ok, "DPC-061 support-bundle API refused rich snapshot");
  Require(result.redaction_applied && result.forbidden_fields_absent,
          "DPC-061 support-bundle API did not apply redaction");
  Require(result.performance_optimization_surface_collected,
          "DPC-061 support-bundle API missed performance surface");
  Require(result.agent_runtime_evidence_collected,
          "DPC-061 support-bundle API missed agent evidence");
  Require(HasEvidence(result,
                      "support_bundle_performance_optimization_surface",
                      api::PerformanceOptimizationSurfaceSchemaId()),
          "DPC-061 support-bundle API missing surface evidence");
  Require(HasRowField(result, "bundle_record_kind",
                      "performance_optimization_surface"),
          "DPC-061 support-bundle API missing performance row");
  Require(HasRowField(result, "physical_path", "<redacted>"),
          "DPC-061 support-bundle API did not redact physical path");
  Require(HasRowField(result, "unsafe_payload", "<redacted>"),
          "DPC-061 support-bundle API did not redact unsafe payload");
  Require(Contains(result.support_bundle_json,
                   "\"index_garbage_backlog_count\":6"),
          "DPC-061 support-bundle API JSON missing index backlog");
  Require(Contains(result.support_bundle_json,
                   "\"support_bundle_completeness_state\":\"complete\""),
          "DPC-061 support-bundle API JSON missing completeness state");
  RequireNoUnsafePayload(result.support_bundle_json, "support-bundle API JSON");

  api::EnginePrepareSupportBundleRequest invalid = request;
  invalid.performance_optimization_snapshot.message_vector_ready = false;
  const auto refused = api::EnginePrepareSupportBundle(invalid);
  Require(!refused.ok,
          "DPC-061 support-bundle API accepted invalid message vector state");
  Require(HasDiagnostic(refused,
                        "CDP.USER_OBSERVABILITY_SURFACE.INVALID_SNAPSHOT"),
          "DPC-061 support-bundle invalid snapshot diagnostic mismatch");
}

void TestMetricsAuditAndMessageVectorEvidence() {
  const auto snapshot = RichSnapshot();
  api::EngineRecordLifecycleMetricRequest record;
  record.context = Context();
  record.operation_key = "dpc061.performance_optimization_surface";
  record.outcome = "refused";
  record.diagnostic_code = snapshot.exact_refusal_diagnostic_code;
  record.route_family = "management_observability";
  record.cache_invalidation_required = true;
  record.cache_family = "performance_optimization_surface";
  record.cache_reason = "catalog_epoch_changed";

  const auto recorded = api::EngineRecordLifecycleMetric(record);
  Require(recorded.ok, "DPC-061 lifecycle metric record failed");
  Require(recorded.metric_recorded && recorded.cache_invalidation_recorded,
          "DPC-061 lifecycle metric flags incomplete");
  Require(HasEvidence(recorded,
                      "lifecycle_audit_event",
                      "dpc061.performance_optimization_surface:failure"),
          "DPC-061 lifecycle audit event evidence missing");
  Require(HasRowField(recorded, "diagnostic_code",
                      snapshot.exact_refusal_diagnostic_code),
          "DPC-061 lifecycle metric missing exact diagnostic row");
  Require(HasRowField(recorded, "parser_finality_authority", "false"),
          "DPC-061 lifecycle metric claimed parser authority");
  Require(HasRowField(recorded, "reference_finality_authority", "false"),
          "DPC-061 lifecycle metric claimed reference authority");

  api::EngineSysMetricsCurrentRequest diagnostics;
  diagnostics.context = Context();
  diagnostics.option_envelopes.push_back("family:sb_lifecycle_diagnostic_total");
  const auto diagnostic_metrics = api::EngineSysMetricsCurrent(diagnostics);
  Require(diagnostic_metrics.ok,
          "DPC-061 diagnostic metrics surface refused request");
  Require(HasRowField(diagnostic_metrics, "metric",
                      "sb_lifecycle_diagnostic_total"),
          "DPC-061 diagnostic metric family missing");
  Require(HasRowFieldContaining(diagnostic_metrics,
                                "labels",
                                snapshot.exact_refusal_diagnostic_code),
          "DPC-061 diagnostic metric labels missing exact message code");

  api::EngineSysMetricsCurrentRequest audit;
  audit.context = Context();
  audit.option_envelopes.push_back("family:sb_lifecycle_audit_event_total");
  const auto audit_metrics = api::EngineSysMetricsCurrent(audit);
  Require(audit_metrics.ok, "DPC-061 audit metrics surface refused request");
  Require(HasRowField(audit_metrics, "metric",
                      "sb_lifecycle_audit_event_total"),
          "DPC-061 audit metric family missing");
  Require(HasRowFieldContaining(audit_metrics,
                                "labels",
                                "management_observability"),
          "DPC-061 audit metric labels missing route family");
}

}  // namespace

int main() {
  std::cout << kGateSearchKey << '\n';
  TestSchemaAndValidation();
  TestManagementAndSupportBundleJson();
  TestShowManagementRows();
  TestSupportBundleApiCompleteness();
  TestMetricsAuditAndMessageVectorEvidence();
  return EXIT_SUCCESS;
}
