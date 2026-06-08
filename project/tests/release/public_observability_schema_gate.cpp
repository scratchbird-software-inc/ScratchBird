// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_OBSERVABILITY_SCHEMA_GATE

#include "metric_contracts.hpp"
#include "metric_registry.hpp"
#include "observability/agent_observability_api.hpp"
#include "observability/cluster_support_bundle_redaction_api.hpp"
#include "observability/optimizer_metric_support_bundle.hpp"
#include "observability/performance_metric_event.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "observability/repair_history_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace metrics = scratchbird::core::metrics;
namespace obs = scratchbird::engine::internal_api::observability;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::u64;
using scratchbird::core::platform::UuidKind;

struct EvidenceRow {
  std::string surface;
  std::string case_id;
  std::string result;
  std::string diagnostic;
  std::string redaction;
  std::string authority_boundary;
  std::string detail;
};

std::vector<EvidenceRow> g_rows;

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string CsvEscape(std::string_view value) {
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      out += "\"\"";
    } else {
      out += ch;
    }
  }
  out += '"';
  return out;
}

void Record(std::string surface,
            std::string case_id,
            bool ok,
            std::string diagnostic,
            std::string redaction,
            std::string authority_boundary,
            std::string detail) {
  g_rows.push_back({std::move(surface),
                    std::move(case_id),
                    ok ? "pass" : "fail",
                    std::move(diagnostic),
                    std::move(redaction),
                    std::move(authority_boundary),
                    std::move(detail)});
}

bool Expect(bool condition,
            std::string surface,
            std::string case_id,
            std::string diagnostic,
            std::string redaction = "redacted_or_not_sensitive",
            std::string authority_boundary = "observability_only",
            std::string detail = "stable_schema_checked") {
  Record(std::move(surface),
         std::move(case_id),
         condition,
         std::move(diagnostic),
         std::move(redaction),
         std::move(authority_boundary),
         std::move(detail));
  return condition;
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) {
      return value.encoded_value;
    }
  }
  return {};
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view evidence_kind,
                 std::string_view evidence_id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == evidence_kind &&
        (evidence_id.empty() || evidence.evidence_id == evidence_id)) {
      return true;
    }
  }
  return false;
}

api::EngineRequestContext Context(std::vector<std::string> rights,
                                  bool cluster_authority = false) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "public-observability-schema-gate";
  context.database_uuid.canonical = "database:public-observability-schema-gate";
  context.cluster_uuid.canonical = "cluster:public-observability-schema-gate";
  context.node_uuid.canonical = "node:public-observability-schema-gate";
  context.principal_uuid.canonical = "principal:public-observability-schema-gate";
  context.security_context_present = true;
  context.cluster_authority_available = cluster_authority;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.trace_tags.push_back("public_observability_schema_gate");
  api::EngineMaterializedAuthorizationContext authz;
  authz.present = true;
  authz.authority_uuid.canonical = "authority:public-observability-schema-gate";
  authz.principal_uuid = context.principal_uuid;
  authz.security_epoch = context.security_epoch;
  authz.policy_epoch = 1;
  authz.catalog_generation_id = context.catalog_generation_id;
  authz.effective_subjects.push_back({context.principal_uuid, "principal"});
  for (const auto& right : rights) {
    context.trace_tags.push_back("right:" + right);
    authz.grants.push_back({api::EngineUuid{"grant:public-observability-schema-gate:" + right},
                            context.principal_uuid,
                            "principal",
                            {},
                            right,
                            false,
                            context.security_epoch});
  }
  context.authorization_context = std::move(authz);
  return context;
}

std::string DurableUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1770900000000ull + offset);
  if (!generated.ok()) {
    return {};
  }
  return uuid::UuidToString(generated.value.value);
}

bool WriteEvidenceCsv(const std::filesystem::path& output) {
  if (output.empty()) {
    return true;
  }
  const auto parent = output.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  std::ofstream stream(output);
  if (!stream) {
    std::cerr << "could not open observability evidence csv: "
              << output.string() << '\n';
    return false;
  }
  stream << "surface,case_id,result,diagnostic,redaction,authority_boundary,detail\n";
  for (const auto& row : g_rows) {
    stream << CsvEscape(row.surface) << ','
           << CsvEscape(row.case_id) << ','
           << CsvEscape(row.result) << ','
           << CsvEscape(row.diagnostic) << ','
           << CsvEscape(row.redaction) << ','
           << CsvEscape(row.authority_boundary) << ','
           << CsvEscape(row.detail) << '\n';
  }
  return true;
}

bool CheckMetricRegistrySurface() {
  bool ok = true;
  const auto descriptors = metrics::DefaultMetricRegistry().Descriptors(true);
  std::map<std::string, std::string> required = {
      {"memory", "sys.metrics.memory"},
      {"storage", "sys.metrics.storage"},
      {"transactions", "sys.metrics.transactions"},
      {"mga_cleanup", "sys.metrics.mga.cleanup"},
      {"indexes", "sys.metrics.index"},
      {"optimizer", "sys.metrics.optimizer"},
      {"agents", "sys.metrics.agents"},
      {"backup", "sys.metrics.backup"},
      {"archive", "sys.metrics.archive"},
      {"security", "sys.metrics.security"},
      {"events", "sys.metrics.events"},
      {"support_bundle", "sys.metrics.supportability"},
      {"cluster_boundary", "cluster.sys.metrics"}};

  for (const auto& [surface, prefix] : required) {
    u64 count = 0;
    for (const auto& descriptor : descriptors) {
      if (StartsWith(descriptor.namespace_path, prefix)) {
        ++count;
      }
    }
    ok = Expect(count > 0,
                "metrics",
                surface,
                "ENTERPRISE_METRIC_SCHEMA",
                "descriptor_redaction_policy_present",
                prefix == "cluster.sys.metrics" ? "cluster_only_contract_ready"
                                                 : "local_metric_descriptor",
                "descriptor_count=" + std::to_string(count)) &&
         ok;
  }

  u64 documented = 0;
  u64 sensitive_labels = 0;
  u64 cluster_descriptors = 0;
  bool cluster_flag_consistent = true;
  for (const auto& descriptor : descriptors) {
    if (!descriptor.family.empty() && !descriptor.namespace_path.empty() &&
        !descriptor.help.empty() && !descriptor.producer_owner.empty() &&
        !descriptor.security_family.empty()) {
      ++documented;
    }
    for (const auto& label : descriptor.labels) {
      if (label.sensitive) {
        ++sensitive_labels;
      }
    }
    if (StartsWith(descriptor.namespace_path, "cluster.sys.metrics")) {
      ++cluster_descriptors;
      cluster_flag_consistent = cluster_flag_consistent && descriptor.cluster_only;
    } else {
      cluster_flag_consistent = cluster_flag_consistent && !descriptor.cluster_only;
    }
  }
  ok = Expect(documented == descriptors.size(),
              "metrics",
              "descriptor_documentation",
              "SB_METRICS_DESCRIPTOR_REGISTRY",
              "not_sensitive_schema_metadata",
              "observability_only",
              "documented=" + std::to_string(documented)) &&
       ok;
  ok = Expect(sensitive_labels > 0,
              "metrics",
              "sensitive_label_inventory",
              "SB_METRICS_DESCRIPTOR_REGISTRY",
              "sensitive_labels_registered",
              "observability_only",
              "sensitive_label_count=" + std::to_string(sensitive_labels)) &&
       ok;
  ok = Expect(cluster_descriptors > 0 && cluster_flag_consistent,
              "metrics",
              "cluster_namespace_boundary",
              "SB_METRICS_DESCRIPTOR_REGISTRY",
              "cluster_metrics_descriptor_only",
              "no_local_cluster_authority",
              "cluster_descriptor_count=" + std::to_string(cluster_descriptors)) &&
       ok;

  const auto* event_descriptor =
      metrics::DefaultMetricRegistry().FindDescriptor("sb_event_delivered_total");
  bool redacted = false;
  if (event_descriptor != nullptr) {
    metrics::MetricValue value;
    value.family = event_descriptor->family;
    value.type = event_descriptor->type;
    value.value = 1.0;
    value.labels = {{"session_uuid", "session-secret"},
                    {"principal_uuid", "principal-secret"},
                    {"machine_id", "host-secret"},
                    {"source_address", "10.0.0.1"},
                    {"result", "delivered"}};
    const auto safe = metrics::RedactSensitiveMetricValue(
        *event_descriptor, value, false);
    std::map<std::string, std::string> labels;
    for (const auto& label : safe.labels) {
      labels[label.key] = label.value;
    }
    redacted = labels["session_uuid"] == "<redacted>" &&
               labels["principal_uuid"] == "<redacted>" &&
               labels["machine_id"] == "<redacted>" &&
               labels["source_address"] == "<redacted>" &&
               labels["result"] == "delivered";
  }
  ok = Expect(redacted,
              "metrics",
              "sensitive_label_redaction",
              "SB_METRICS_DESCRIPTOR_REGISTRY",
              "sensitive_labels_redacted",
              "observability_only",
              "event_metric_sensitive_labels") &&
       ok;
  return ok;
}

void FillHotPath(api::PerformanceMetricHotPathAttribution* hot_path) {
  hot_path->cpu_sample_count = 0;
  hot_path->cpu_sample_attributed_count = 0;
  hot_path->cpu_sample_attribution = "not_available_zeroed";
  hot_path->allocator_allocation_count = 0;
  hot_path->allocator_allocation_bytes = 0;
  hot_path->lock_latch_wait_count = 0;
  hot_path->lock_latch_wait_us = 0;
  hot_path->syscall_count = 0;
  hot_path->file_open_count = 0;
  hot_path->file_flush_count = 0;
  hot_path->file_fsync_count = 0;
  hot_path->page_fault_count = 0;
  hot_path->context_switch_count = 0;
  hot_path->evidence_rendering_us = 0;
  hot_path->result_formatting_us = 0;
  hot_path->regression_budget_us = 0;
  hot_path->regression_budget_margin_us = 0;
  hot_path->regression_budget_validated = true;
  hot_path->cpu_sample_measurement_source = "not_available_zeroed";
  hot_path->cpu_sample_measurement_quality = "not_available_zeroed";
  hot_path->allocator_counter_measurement_source = "not_available_zeroed";
  hot_path->allocator_counter_measurement_quality = "not_available_zeroed";
  hot_path->lock_latch_wait_measurement_source = "not_available_zeroed";
  hot_path->lock_latch_wait_measurement_quality = "not_available_zeroed";
  hot_path->syscall_count_measurement_source = "not_available_zeroed";
  hot_path->syscall_count_measurement_quality = "not_available_zeroed";
  hot_path->file_io_count_measurement_source = "not_available_zeroed";
  hot_path->file_io_count_measurement_quality = "not_available_zeroed";
  hot_path->page_fault_count_measurement_source = "not_available_zeroed";
  hot_path->page_fault_count_measurement_quality = "not_available_zeroed";
  hot_path->context_switch_count_measurement_source = "not_available_zeroed";
  hot_path->context_switch_count_measurement_quality = "not_available_zeroed";
  hot_path->evidence_rendering_measurement_source = "not_available_zeroed";
  hot_path->evidence_rendering_measurement_quality = "not_available_zeroed";
  hot_path->result_formatting_measurement_source = "not_available_zeroed";
  hot_path->result_formatting_measurement_quality = "not_available_zeroed";
  hot_path->regression_budget_measurement_source = "not_available_zeroed";
  hot_path->regression_budget_measurement_quality = "not_available_zeroed";
  hot_path->parser_lowering_us = 0;
  hot_path->sbps_listener_us = 0;
  hot_path->sblr_dispatch_us = 0;
  hot_path->internal_api_us = 0;
  hot_path->executor_us = 0;
  hot_path->storage_us = 0;
  hot_path->index_layer_us = 0;
  hot_path->transaction_us = 0;
  hot_path->result_rendering_us = 0;
  hot_path->evidence_construction_us = 0;
  hot_path->allocation_us = 0;
  hot_path->syscall_us = 0;
  hot_path->wait_us = 0;
  hot_path->parser_lowering_measurement_source = "not_available_zeroed";
  hot_path->sbps_listener_measurement_source = "not_available_zeroed";
  hot_path->sblr_dispatch_measurement_source = "not_available_zeroed";
  hot_path->internal_api_measurement_source = "not_available_zeroed";
  hot_path->executor_measurement_source = "not_available_zeroed";
  hot_path->storage_measurement_source = "not_available_zeroed";
  hot_path->index_layer_measurement_source = "not_available_zeroed";
  hot_path->transaction_measurement_source = "not_available_zeroed";
  hot_path->result_rendering_measurement_source = "not_available_zeroed";
  hot_path->evidence_construction_measurement_source = "not_available_zeroed";
  hot_path->allocation_measurement_source = "not_available_zeroed";
  hot_path->syscall_measurement_source = "not_available_zeroed";
  hot_path->wait_measurement_source = "not_available_zeroed";
  hot_path->parser_lowering_measurement_quality = "not_available_zeroed";
  hot_path->sbps_listener_measurement_quality = "not_available_zeroed";
  hot_path->sblr_dispatch_measurement_quality = "not_available_zeroed";
  hot_path->internal_api_measurement_quality = "not_available_zeroed";
  hot_path->executor_measurement_quality = "not_available_zeroed";
  hot_path->storage_measurement_quality = "not_available_zeroed";
  hot_path->index_layer_measurement_quality = "not_available_zeroed";
  hot_path->transaction_measurement_quality = "not_available_zeroed";
  hot_path->result_rendering_measurement_quality = "not_available_zeroed";
  hot_path->evidence_construction_measurement_quality = "not_available_zeroed";
  hot_path->allocation_measurement_quality = "not_available_zeroed";
  hot_path->syscall_measurement_quality = "not_available_zeroed";
  hot_path->wait_measurement_quality = "not_available_zeroed";
}

api::PerformanceMetricEvent CompletePerformanceMetricEvent() {
  api::PerformanceMetricEvent event;
  event.route = "public.release.observability";
  event.operation = "gate";
  event.phase_timings.parse_us = 0;
  event.phase_timings.bind_us = 0;
  event.phase_timings.lower_us = 0;
  event.phase_timings.plan_us = 0;
  event.phase_timings.execute_us = 0;
  event.phase_timings.measurement_source = "not_available_zeroed";
  event.phase_timings.measurement_quality = "not_available_zeroed";
  event.storage_timings.append_us = 0;
  event.storage_timings.page_us = 0;
  event.storage_timings.index_us = 0;
  event.storage_timings.measurement_source = "not_available_zeroed";
  event.storage_timings.measurement_quality = "not_available_zeroed";
  event.agent_counters.agent_thread_count = 0;
  event.agent_counters.agent_cpu_user_us = 0;
  event.agent_counters.agent_cpu_system_us = 0;
  event.agent_counters.agent_wait_us = 0;
  event.agent_counters.agent_io_read_bytes = 0;
  event.agent_counters.agent_io_write_bytes = 0;
  event.agent_counters.measurement_source = "not_available_zeroed";
  event.agent_counters.measurement_quality = "not_available_zeroed";
  event.cache_flags.plan_cache_hit = false;
  event.cache_flags.metadata_cache_hit = false;
  event.cache_flags.page_cache_hit = false;
  event.cache_flags.index_cache_hit = false;
  event.cache_flags.measurement_source = "not_available_zeroed";
  event.cache_flags.measurement_quality = "not_available_zeroed";
  FillHotPath(&event.hot_path_attribution);
  event.statistics_epoch = 1;
  event.resource_governor_state = "unthrottled";
  event.message_vector_present = false;
  event.result_hash = "sha256:public-observability-schema-gate";
  event.overhead_mode = api::InstrumentationOverheadMode::kBenchmarkClean;
  return event;
}

bool CheckPerformanceEventSchema() {
  bool ok = true;
  const auto schema = api::PerformanceMetricEventSchema();
  ok = Expect(api::PerformanceMetricEventSchemaVersion() == 3 &&
                  schema.size() >= 60,
              "events",
              "performance_metric_schema_version",
              "ENTERPRISE_EVENT_SCHEMA",
              "not_sensitive_schema_metadata",
              "observability_only",
              "schema_fields=" + std::to_string(schema.size())) &&
       ok;

  const auto event = CompletePerformanceMetricEvent();
  const auto validation = api::ValidatePerformanceMetricEvent(event);
  const auto json = api::SerializePerformanceMetricEventJson(event);
  ok = Expect(validation.ok &&
                  Contains(json, "scratchbird.performance_metric_event.v3") &&
                  Contains(json, "\"transaction_us\""),
              "events",
              "performance_metric_event_valid",
              validation.diagnostic_code,
              "redacted_or_not_sensitive",
              "transaction_timing_evidence_not_authority",
              "serialized_schema_version=3") &&
       ok;

  auto missing = event;
  missing.phase_timings.parse_us.reset();
  const auto refused = api::ValidatePerformanceMetricEvent(missing);
  ok = Expect(!refused.ok &&
                  refused.diagnostic_code ==
                      "CDP.PERFORMANCE_METRIC_EVENT.MISSING_REQUIRED_FIELD",
              "events",
              "performance_metric_event_fail_closed",
              refused.diagnostic_code,
              "redacted_or_not_sensitive",
              "observability_only",
              refused.detail) &&
       ok;
  return ok;
}

bool CheckOptimizationSurface() {
  bool ok = true;
  const auto schema = api::PerformanceOptimizationSurfaceSchema();
  const auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  const auto validation =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  const auto surface_json =
      api::SerializePerformanceOptimizationSurfaceJson(snapshot);
  const auto support_json =
      api::SerializePerformanceOptimizationSupportBundleJson(snapshot);

  ok = Expect(validation.ok && schema.size() >= 80 &&
                  Contains(surface_json,
                           "scratchbird.performance_optimization_surface.v1") &&
                  Contains(support_json, "public_safe_summary") &&
                  !Contains(support_json, "write_ahead_log"),
              "health",
              "performance_optimization_surface_valid",
              validation.diagnostic_code,
              "support_bundle_public_safe_summary",
              "no_parser_donor_storage_or_wal_authority",
              "schema_fields=" + std::to_string(schema.size())) &&
       ok;

  auto drift = snapshot;
  drift.wal_recovery_authority = true;
  const auto drift_validation =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(drift);
  ok = Expect(!drift_validation.ok &&
                  Contains(drift_validation.detail, "non_authority_boundary"),
              "health",
              "performance_optimization_surface_authority_drift",
              drift_validation.diagnostic_code,
              "redacted_or_not_sensitive",
              "wal_recovery_authority_refused",
              drift_validation.detail) &&
       ok;
  return ok;
}

bool CheckAgentObservability() {
  bool ok = true;
  api::EngineCollectAgentRuntimeObservabilityRequest request;
  request.context = Context({"OBS_RUNTIME_ALL"});
  request.operation_id = "public_observability_schema_gate.agent";
  api::EngineAgentRuntimeEvidenceRecord record;
  record.source_surface = "public_release_gate";
  record.agent_type_id = "filespace_capacity_manager";
  record.agent_uuid = DurableUuid(UuidKind::object, 101);
  record.filespace_uuid = DurableUuid(UuidKind::filespace, 102);
  record.policy_uuid = DurableUuid(UuidKind::object, 103);
  record.evidence_uuid = DurableUuid(UuidKind::object, 104);
  record.action_id = "observe_capacity";
  record.evidence_kind = "agent_runtime_evidence";
  record.result_state = "success";
  record.diagnostic_code = "AGENT.NONE";
  record.payload_digest = "sha256:/tmp/private/principal-token";
  record.physical_path = "/tmp/private/token";
  record.raw_principal = "principal-token";
  record.unsafe_payload = "secret=agent-token";
  request.records.push_back(record);

  const auto result = api::EngineCollectAgentRuntimeObservability(request);
  bool row_redacted = false;
  if (!result.result_shape.rows.empty()) {
    const auto& row = result.result_shape.rows.front();
    row_redacted =
        FieldValue(row, "payload_digest") == "<redacted>" &&
        FieldValue(row, "physical_path") == "<redacted>" &&
        FieldValue(row, "principal_redacted") == "true" &&
        FieldValue(row, "unsafe_payload") == "<redacted>" &&
        FieldValue(row, "parser_finality_authority") == "false" &&
        FieldValue(row, "client_catalog_uuid_authority") == "false" &&
        FieldValue(row, "support_bundle_safe") == "true";
  }
  ok = Expect(result.ok && result.metrics_recorded && result.audit_recorded &&
                  result.diagnostics_rendered && result.support_bundle_ready &&
                  result.redaction_applied && row_redacted &&
                  HasEvidence(result,
                              "agent_observability_support_bundle",
                              "redacted"),
              "agents",
              "agent_runtime_observability_redaction",
              result.diagnostics.empty() ? "SB_ENGINE_API_OK"
                                         : result.diagnostics.front().code,
              "paths_principals_payloads_redacted",
              "no_parser_or_client_catalog_uuid_authority",
              "rows=" + std::to_string(result.result_shape.rows.size())) &&
       ok;
  return ok;
}

bool CheckRepairHistory() {
  bool ok = true;
  api::EngineInspectRepairHistoryRequest request;
  request.context = Context({"OBS_RUNTIME_ALL"});
  request.operation_id = "public_observability_schema_gate.repair";
  request.inspection.durable_mga_inventory_authority = true;
  const auto result = api::EngineInspectRepairHistory(request);
  ok = Expect(result.ok && result.repair_history_ready &&
                  result.durable_mga_inventory_authority &&
                  !result.repair_evidence_is_transaction_authority &&
                  HasEvidence(result, "durable_mga_inventory_authority", "true") &&
                  HasEvidence(result,
                              "repair_evidence_transaction_authority",
                              "false"),
              "repair",
              "repair_history_health_observability",
              result.diagnostics.empty() ? "SB_ENGINE_API_OK"
                                         : result.diagnostics.front().code,
              "not_sensitive_schema_metadata",
              "durable_mga_inventory_is_sole_transaction_authority",
              "rows=" + std::to_string(result.result_shape.rows.size())) &&
       ok;

  auto drift = request;
  drift.inspection.parser_or_donor_authority = true;
  const auto refused = api::EngineInspectRepairHistory(drift);
  ok = Expect(!refused.ok && !refused.diagnostics.empty() &&
                  refused.diagnostics.front().code ==
                      "SB-REPAIR-HISTORY-AUTHORITY-REFUSED",
              "repair",
              "repair_history_authority_drift",
              refused.diagnostics.empty() ? "missing_diagnostic"
                                          : refused.diagnostics.front().code,
              "not_sensitive_schema_metadata",
              "parser_or_donor_authority_refused",
              "stable_fail_closed") &&
       ok;
  return ok;
}

api::ClusterSupportBundleProjectionSource BundleProjection(
    api::ClusterProjectionRedactionSensitivity sensitivity) {
  api::ClusterSupportBundleProjectionSource source;
  source.sensitivity = sensitivity;
  source.projection_id =
      "bundle-projection:" +
      std::string(api::ClusterProjectionRedactionSensitivityName(sensitivity));
  source.projection_source =
      "cluster.sys.catalog." +
      std::string(api::ClusterProjectionRedactionSensitivityName(sensitivity));
  source.target_uuid =
      "bundle-target:" +
      std::string(api::ClusterProjectionRedactionSensitivityName(sensitivity));
  source.retention_policy_ref = "retention.cluster.projection.90d";
  source.support_bundle_policy_ref = "support.cluster.projection.redacted";
  source.retention_evidence_present = true;
  source.fields.push_back({"sensitive_detail",
                           "cluster-secret-" +
                               std::string(api::ClusterProjectionRedactionSensitivityName(
                                   sensitivity)),
                           true});
  source.fields.push_back({"projection_state", "active", false});
  return source;
}

bool CheckClusterSupportBundleRedaction() {
  bool ok = true;
  api::EngineBuildClusterProjectionSupportBundleRedactionRequest request;
  request.context = Context({"SUPPORT_EXPORT"}, true);
  request.operation_id = "public_observability_schema_gate.cluster_bundle";
  request.support_bundle_id = "support-bundle:pcr137";
  request.capture_generation = "generation:1";
  request.projections.push_back(
      BundleProjection(api::ClusterProjectionRedactionSensitivity::topology));
  request.projections.push_back(
      BundleProjection(api::ClusterProjectionRedactionSensitivity::security));
  request.projections.push_back(
      BundleProjection(api::ClusterProjectionRedactionSensitivity::route));
  request.projections.push_back(
      BundleProjection(api::ClusterProjectionRedactionSensitivity::metric));
  const auto result =
      api::EngineBuildClusterProjectionSupportBundleRedaction(request);

  ok = Expect(result.ok && result.bundle_allowed &&
                  result.sensitive_values_redacted &&
                  result.retention_evidence_present &&
                  !result.failed_closed &&
                  !result.local_runtime_execution_enabled &&
                  !result.local_projection_cluster_authority &&
                  HasEvidence(result, "CLUSTER_SUPPORT_BUNDLE_REDACTION") &&
                  !Contains(result.support_bundle_json, "cluster-secret-") &&
                  Contains(result.support_bundle_json, "[redacted:security]"),
              "cluster_boundary",
              "cluster_support_bundle_redaction",
              result.diagnostics.empty() ? "SB_ENGINE_API_OK"
                                         : result.diagnostics.front().code,
              "cluster_projection_sensitive_values_redacted",
              "support_bundle_not_local_cluster_authority",
              "projection_count=" +
                  std::to_string(result.projection_decisions.size())) &&
       ok;

  auto denied = request;
  denied.context = Context({}, true);
  const auto refused =
      api::EngineBuildClusterProjectionSupportBundleRedaction(denied);
  ok = Expect(!refused.ok && refused.failed_closed && !refused.bundle_allowed,
              "cluster_boundary",
              "cluster_support_bundle_requires_export_right",
              refused.diagnostics.empty() ? "missing_diagnostic"
                                          : refused.diagnostics.front().code,
              "cluster_projection_sensitive_values_redacted",
              "support_bundle_not_local_cluster_authority",
              "stable_fail_closed") &&
       ok;
  return ok;
}

obs::OptimizerMetricSupportBundleRequest OptimizerBundleRequest() {
  obs::OptimizerMetricSupportBundleRequest request;
  request.scope_uuid = DurableUuid(UuidKind::database, 201);
  request.support_bundle_id = "support-bundle:pcr137-optimizer";
  request.capture_generation = "generation:1";
  request.evidence_digest = "sha256:public-observability-schema-gate";
  request.min_source_generation = 0;
  request.max_metric_values = 16;
  request.benchmark_clean_export = false;
  request.allow_sensitive_labels = false;
  request.authority.metric_registry_authoritative = true;
  request.authority.optimizer_manifest_authoritative = true;
  request.authority.support_bundle_request_authorized = true;
  request.authority.redaction_policy_bound = true;
  request.authority.retention_policy_bound = true;
  request.authority.metrics_trusted = true;
  request.authority.snapshot_fresh = true;
  request.authority.engine_scope_bound = true;
  metrics::MetricValue value;
  value.family = "sb_optimizer_operator_actual_rows";
  value.type = metrics::MetricType::gauge;
  value.value = 42.0;
  value.labels = {{"source_generation", "1"},
                  {"route_label", "public_observability_schema_gate"},
                  {"plan_node_id", "scan:1"}};
  request.metric_snapshot.push_back(std::move(value));
  return request;
}

bool CheckOptimizerSupportBundle() {
  bool ok = true;
  const auto metric_status =
      metrics::PublishOptimizerPlanEstimateErrorRatio(1.25, "scan", "index_lookup");
  ok = Expect(metric_status.ok,
              "optimizer",
              "optimizer_metric_published",
              metric_status.ok ? "SB_METRIC_OK" : metric_status.diagnostic_code,
              "not_sensitive_schema_metadata",
              "optimizer_metric_evidence_only",
              metric_status.detail.empty() ? "metric_recorded"
                                           : metric_status.detail) &&
       ok;

  const auto result =
      obs::BuildOptimizerMetricSupportBundle(OptimizerBundleRequest());
  ok = Expect(result.ok && result.redaction_applied && !result.rows.empty() &&
                  !result.tamper_digest.empty() &&
                  std::any_of(result.evidence.begin(),
                              result.evidence.end(),
                              [](const std::string& evidence) {
                                return evidence ==
                                       "optimizer.metric_bundle.wal_redo_authority=false";
                              }),
              "optimizer",
              "optimizer_metric_support_bundle",
              result.diagnostic_code,
              "optimizer_metric_labels_redacted",
              "no_finality_visibility_security_recovery_wal_or_cluster_authority",
              "rows=" + std::to_string(result.rows.size())) &&
       ok;

  auto drift = OptimizerBundleRequest();
  drift.authority.wal_or_redo_authority = true;
  const auto refused = obs::BuildOptimizerMetricSupportBundle(drift);
  ok = Expect(!refused.ok &&
                  refused.diagnostic_code ==
                      "SB_OPTIMIZER_METRIC_BUNDLE.UNSAFE_AUTHORITY",
              "optimizer",
              "optimizer_metric_support_bundle_authority_drift",
              refused.diagnostic_code,
              "redacted_or_not_sensitive",
              "wal_or_redo_authority_refused",
              refused.detail) &&
       ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path output =
      argc >= 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{};

  bool ok = true;
  ok = CheckMetricRegistrySurface() && ok;
  ok = CheckPerformanceEventSchema() && ok;
  ok = CheckOptimizationSurface() && ok;
  ok = CheckAgentObservability() && ok;
  ok = CheckRepairHistory() && ok;
  ok = CheckClusterSupportBundleRedaction() && ok;
  ok = CheckOptimizerSupportBundle() && ok;

  const bool wrote_csv = WriteEvidenceCsv(output);
  const u64 pass_count = static_cast<u64>(std::count_if(
      g_rows.begin(), g_rows.end(), [](const EvidenceRow& row) {
        return row.result == "pass";
      }));
  const u64 fail_count = static_cast<u64>(g_rows.size()) - pass_count;
  std::cout << "public_observability_schema_gate="
            << (ok && wrote_csv ? "passed" : "failed")
            << " rows=" << g_rows.size()
            << " pass=" << pass_count
            << " fail=" << fail_count << '\n';
  return ok && wrote_csv ? EXIT_SUCCESS : EXIT_FAILURE;
}
