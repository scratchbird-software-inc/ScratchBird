// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_support_bundle.hpp"
#include "management/support_bundle_api.hpp"
#include "memory_support_bundle.hpp"
#include "observability/cluster_support_bundle_redaction_api.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace listener = scratchbird::listener;
namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

using platform::Severity;
using platform::StatusCode;
using platform::Subsystem;
using platform::UuidKind;
using platform::u64;

constexpr u64 kBaseMillis = 1771200880000ull;

struct MatrixRow {
  std::string surface;
  std::string case_id;
  std::string status;
  std::string canary_class;
  std::string injection_surface;
  std::string emitted_marker;
  std::string evidence;
};

std::vector<MatrixRow> g_rows;

[[noreturn]] void Fail(std::string message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string message) {
  if (!condition) {
    Fail(std::move(message));
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void RequireContains(std::string_view haystack,
                     std::string_view needle,
                     std::string message) {
  if (!Contains(haystack, needle)) {
    std::cerr << message << "\nneedle=" << needle << "\nhaystack=" << haystack
              << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void RequireNoCanary(std::string_view text,
                     const std::vector<std::string>& canaries,
                     std::string_view surface) {
  for (const auto& canary : canaries) {
    if (Contains(text, canary)) {
      std::cerr << "ELER-088 canary leaked on " << surface
                << "\ncanary=" << canary << "\ntext=" << text << '\n';
      std::exit(EXIT_FAILURE);
    }
  }
}

std::string CsvEscape(std::string value) {
  bool quote = false;
  for (const char ch : value) {
    quote = quote || ch == ',' || ch == '"' || ch == '\n' || ch == '\r';
  }
  if (!quote) {
    return value;
  }
  std::string escaped = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      escaped.push_back('"');
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

void AddRow(std::string surface,
            std::string case_id,
            std::string canary_class,
            std::string injection_surface,
            std::string emitted_marker,
            std::string evidence) {
  g_rows.push_back({std::move(surface),
                    std::move(case_id),
                    "pass",
                    std::move(canary_class),
                    std::move(injection_surface),
                    std::move(emitted_marker),
                    std::move(evidence)});
}

void WriteMatrix(const std::filesystem::path& output) {
  if (output.empty()) {
    return;
  }
  std::filesystem::create_directories(output.parent_path());
  std::ofstream file(output);
  Require(file.good(), "ELER-088 matrix output could not be opened");
  file << "surface,case_id,status,canary_class,injection_surface,emitted_marker,evidence\n";
  for (const auto& row : g_rows) {
    file << CsvEscape(row.surface) << ',' << CsvEscape(row.case_id) << ','
         << CsvEscape(row.status) << ',' << CsvEscape(row.canary_class) << ','
         << CsvEscape(row.injection_surface) << ','
         << CsvEscape(row.emitted_marker) << ',' << CsvEscape(row.evidence)
         << '\n';
  }
  Require(file.good(), "ELER-088 matrix output write failed");
}

std::string MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  Require(generated.ok(), "ELER-088 durable UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

api::EngineUuid EngineUuid(UuidKind kind, u64 offset) {
  api::EngineUuid out;
  out.canonical = MakeUuid(kind, offset);
  return out;
}

api::EngineRequestContext SupportContext() {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "eler088-support-bundle-redaction";
  context.database_uuid = EngineUuid(UuidKind::database, 1);
  context.cluster_uuid = EngineUuid(UuidKind::cluster, 2);
  context.node_uuid = EngineUuid(UuidKind::object, 3);
  context.principal_uuid = EngineUuid(UuidKind::principal, 4);
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.catalog_generation_id = 88001;
  context.security_epoch = 88002;
  context.resource_epoch = 88004;

  api::EngineMaterializedAuthorizationContext authz;
  authz.present = true;
  authz.authority_uuid = EngineUuid(UuidKind::object, 6);
  authz.principal_uuid = context.principal_uuid;
  authz.security_epoch = context.security_epoch;
  authz.policy_epoch = 88003;
  authz.catalog_generation_id = context.catalog_generation_id;
  authz.effective_subjects.push_back({context.principal_uuid, "principal"});
  authz.grants.push_back({EngineUuid(UuidKind::object, 7),
                          context.principal_uuid,
                          "principal",
                          {},
                          "SUPPORT_EXPORT",
                          false,
                          context.security_epoch});
  authz.evidence_tags.push_back("eler088_materialized_support_export_only");
  context.authorization_context = std::move(authz);
  return context;
}

std::string FlattenApiResult(const api::EngineApiResult& result) {
  std::ostringstream out;
  out << "ok=" << (result.ok ? "true" : "false") << '\n'
      << "operation_id=" << result.operation_id << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    out << "diagnostic.code=" << diagnostic.code << '\n'
        << "diagnostic.message_key=" << diagnostic.message_key << '\n'
        << "diagnostic.detail=" << diagnostic.detail << '\n';
  }
  for (const auto& unsupported : result.unsupported_features) {
    out << "unsupported.feature=" << unsupported.feature << '\n'
        << "unsupported.reason=" << unsupported.reason << '\n';
  }
  for (const auto& evidence : result.evidence) {
    out << "evidence.kind=" << evidence.evidence_kind << '\n'
        << "evidence.id=" << evidence.evidence_id << '\n';
  }
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      out << "row." << field.first << '=' << field.second.encoded_value
          << '\n';
    }
  }
  return out.str();
}

std::vector<std::string> CanarySet() {
  return {
      "secret-canary-eler088-auth-token",
      "secret-canary-eler088-protected-material",
      "secret-canary-eler088-tls-key",
      "secret-canary-eler088-env-config",
      "secret-canary-eler088-parser-handoff",
      "secret-canary-eler088-donor-connector",
      "secret_canary_eler088_listener_metric",
      "secret-canary-eler088-memory",
  };
}

void ListenerSupportBundleCanaryProof(const std::vector<std::string>& canaries) {
  listener::ListenerSupportBundleSnapshot snapshot;
  snapshot.config.protocol_family = "sbsql";
  snapshot.config.listener_uuid = "listener-eler088";
  snapshot.config.listener_profile = "enterprise";
  snapshot.config.database_selector =
      "dev_bootstrap_path:/tmp/secret-canary-eler088-env-config.sbdb";
  snapshot.config.server_endpoint = "unix:/tmp/eler088-public-path.sock";
  snapshot.config.parser_executable =
      "/tmp/secret-canary-eler088-parser-handoff-worker";
  snapshot.config.tls_cert_file = "/tmp/eler088-public-cert.pem";
  snapshot.config.tls_key_file = "/tmp/secret-canary-eler088-tls-key.key";
  snapshot.config.tls_ca_file = "/tmp/eler088-public-ca.pem";
  snapshot.config.bundle_contract_id =
      "bundle.default@1-secret-canary-eler088-auth-token";
  snapshot.identity.listener_uuid = snapshot.config.listener_uuid;
  snapshot.identity.profile = snapshot.config.listener_profile;
  snapshot.identity.endpoint_hash = "endpoint-hash-public";
  snapshot.identity.generation = "88";
  snapshot.lifecycle_state = "running";
  snapshot.accepting_new_connections = true;
  snapshot.pool_status.running = true;
  snapshot.pool_status.running_worker_count = 1;
  snapshot.metrics_json =
      "{\"sys.metrics.listener.secret_canary_eler088_listener_metric\":1}";

  listener::ParserPoolFaultEvent fault;
  fault.timestamp_ms = 88001;
  fault.worker_id = "worker-eler088";
  fault.event = "crash";
  fault.diagnostic =
      "parser handoff failed bearer secret-canary-eler088-parser-handoff";
  snapshot.pool_status.fault_history.push_back(std::move(fault));

  snapshot.management_decisions.push_back(
      {88002,
       "management_decision",
       "SUPPORT_BUNDLE",
       "accepted",
       "LISTENER.SUPPORT_BUNDLE.OK",
       "auth token secret-canary-eler088-auth-token"});
  snapshot.runtime_events.push_back(
      {88003,
       "handoff_failure",
       "PARSER_HANDOFF",
       "refused",
       "LISTENER.HANDOFF.FAIL",
       "socket /tmp/secret-canary-eler088-parser-handoff.sock"});
  snapshot.runtime_events.push_back(
      {88004,
       "auth_refusal",
       "TLS_CHANNEL_BINDING",
       "refused",
       "SECURITY.AUTHENTICATION.FAILED",
       "tls private_key secret-canary-eler088-tls-key"});

  const auto bundle = listener::BuildListenerSupportBundleJson(snapshot);
  RequireNoCanary(bundle, canaries, "listener_support_bundle");
  RequireContains(bundle,
                  "\"support_bundle_is_authority\":false",
                  "ELER-088 listener support bundle authority boundary missing");
  RequireContains(bundle,
                  "[redacted:security]",
                  "ELER-088 listener security redaction marker missing");
  RequireContains(bundle,
                  "[path-redacted]",
                  "ELER-088 listener path redaction marker missing");
  RequireContains(bundle,
                  "\"metrics\":{\"redacted\":true",
                  "ELER-088 listener metrics canary was not summarized");
  AddRow("listener",
         "listener_config_tls_metrics_handoff_auth",
         "auth_tls_env_parser_handoff",
         "config;metrics;parser_fault;management_decision;runtime_event",
         "[redacted:security];[path-redacted];metrics.redacted=true",
         "BuildListenerSupportBundleJson flattened JSON contains no canary");
}

api::EngineSupportBundleAgentEvidenceSource AgentCanaryEvidence() {
  api::EngineSupportBundleAgentEvidenceSource source;
  source.agent_type_id = "donor_connector_supportability_probe";
  source.agent_uuid = MakeUuid(UuidKind::object, 20);
  source.filespace_uuid = MakeUuid(UuidKind::filespace, 21);
  source.policy_uuid = MakeUuid(UuidKind::object, 22);
  source.evidence_uuid = MakeUuid(UuidKind::object, 23);
  source.evidence_kind =
      "agent_runtime_evidence secret-canary-eler088-donor-connector";
  source.result_state = "success";
  source.diagnostic_code =
      "AGENT.SECRET-CANARY-ELER088-DONOR-CONNECTOR";
  source.payload_digest =
      "sha256:secret-canary-eler088-protected-material";
  source.retention_class =
      "support_bundle_evidence secret-canary-eler088-auth-token";
  source.retention_policy_ref =
      "retention.secret-canary-eler088-env-config";
  source.retention_deadline =
      "deadline secret-canary-eler088-protected-material";
  source.physical_path =
      "/tmp/secret-canary-eler088-donor-connector/runtime.sbdb";
  source.unsafe_payload =
      "password=secret-canary-eler088-auth-token "
      "protected_reference=secret-canary-eler088-protected-material";
  source.payload_redacted = true;
  return source;
}

api::PerformanceOptimizationSurfaceSnapshot PerformanceCanarySnapshot() {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.plan_cache_last_invalidation_reason =
      "token secret-canary-eler088-env-config";
  snapshot.stale_statistics_fail_safe_reason =
      "password secret-canary-eler088-protected-material";
  snapshot.cleanup_horizon_authority_status = "not_reported";
  snapshot.last_agent_decision =
      "credential secret-canary-eler088-donor-connector";
  snapshot.last_agent_diagnostic_code = "AGENT.CANARY.REDACTED";
  snapshot.secondary_index_state =
      "private_key secret-canary-eler088-tls-key";
  snapshot.backpressure_reason =
      "bearer secret-canary-eler088-auth-token";
  snapshot.support_bundle_correlation_id =
      "support secret-canary-eler088-env-config";
  snapshot.request_correlation_id =
      "request secret-canary-eler088-protected-material";
  snapshot.exact_refusal_diagnostic_code = "SECURITY.AUTHENTICATION.FAILED";
  snapshot.exact_refusal_message_vector =
      "auth secret-canary-eler088-auth-token";
  snapshot.exact_refusal_source =
      "parser secret-canary-eler088-parser-handoff";
  snapshot.message_vector_ready = true;
  return snapshot;
}

std::vector<api::PerformanceOptimizationConfigOverride> ConfigCanaryOverrides() {
  api::PerformanceOptimizationConfigOverride override;
  override.surface_name = "copy_batch_rows_configured";
  override.source = "environment";
  override.value = "secret-canary-eler088-env-config";
  override.allowed_by_policy = true;
  return {override};
}

void EngineSupportBundleCanaryProof(const std::vector<std::string>& canaries) {
  api::EnginePrepareSupportBundleRequest request;
  request.context = SupportContext();
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  request.option_envelopes.push_back(
      "redaction_profile_ref:server.support_bundle.default_redaction.v1");
  request.option_envelopes.push_back(
      "retention_policy_ref:support.bundle.default_retention.v1");
  request.agent_runtime_evidence.push_back(AgentCanaryEvidence());
  request.performance_optimization_snapshot = PerformanceCanarySnapshot();
  request.performance_optimization_snapshot_present = true;
  request.performance_optimization_config_overrides = ConfigCanaryOverrides();

  const auto result = api::EnginePrepareSupportBundle(request);
  Require(result.ok, "ELER-088 engine support bundle refused canary proof");
  Require(result.redaction_applied && result.forbidden_fields_absent,
          "ELER-088 engine support bundle did not declare redaction");
  Require(result.agent_runtime_evidence_collected,
          "ELER-088 engine support bundle missed agent evidence");
  Require(result.performance_optimization_surface_collected,
          "ELER-088 engine support bundle missed performance evidence");

  const std::string flat = FlattenApiResult(result) + "\n" +
                           result.support_bundle_json + "\n" +
                           result.retention_policy_ref + "\n" +
                           result.redaction_profile_ref + "\n" +
                           result.authority_path + "\n" +
                           result.audit_envelope_ref;
  RequireNoCanary(flat, canaries, "engine_management_support_bundle");
  RequireContains(flat,
                  "<redacted>",
                  "ELER-088 engine support bundle row redaction marker missing");
  RequireContains(result.support_bundle_json,
                  "[redacted:security]",
                  "ELER-088 performance support bundle JSON marker missing");
  AddRow("engine_management",
         "agent_performance_config_canaries",
         "auth_protected_material_env_donor_connector",
         "agent_runtime_evidence;performance_snapshot;config_override",
         "<redacted>;[redacted:security]",
         "EnginePrepareSupportBundle rows JSON diagnostics and evidence contain no canary");
}

void EngineSupportBundleFailClosedProof() {
  auto base = api::EnginePrepareSupportBundleRequest{};
  base.context = SupportContext();
  base.option_envelopes.push_back("engine_authorized_support_export:true");

  auto plaintext = base;
  plaintext.option_envelopes.push_back("include_plaintext_secret");
  const auto plaintext_refused = api::EnginePrepareSupportBundle(plaintext);
  Require(!plaintext_refused.ok,
          "ELER-088 include_plaintext_secret was not refused");

  auto disabled = base;
  disabled.option_envelopes.push_back("redaction_disabled");
  const auto disabled_refused = api::EnginePrepareSupportBundle(disabled);
  Require(!disabled_refused.ok,
          "ELER-088 redaction_disabled was not refused");

  auto no_policy = base;
  no_policy.option_envelopes.push_back("support_bundle_policy_installed:false");
  const auto no_policy_refused = api::EnginePrepareSupportBundle(no_policy);
  Require(!no_policy_refused.ok,
          "ELER-088 missing support-bundle policy was not refused");

  api::EnginePrepareSupportBundleRequest missing_security;
  missing_security.option_envelopes.push_back("engine_authorized_support_export:true");
  const auto missing_security_refused =
      api::EnginePrepareSupportBundle(missing_security);
  Require(!missing_security_refused.ok,
          "ELER-088 missing security context was not refused");

  AddRow("engine_management",
         "support_bundle_redaction_bypass_refusal",
         "fail_closed_controls",
         "include_plaintext_secret;redaction_disabled;missing_policy;missing_security_context",
         "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN;OPS.SUPPORT_BUNDLE.POLICY_REQUIRED;OPS.SUPPORT_BUNDLE.SECURITY_CONTEXT_REQUIRED",
         "bypass and malformed support-bundle requests fail closed");
}

void MemorySupportBundleCanaryProof(const std::vector<std::string>& canaries) {
  memory::MemorySupportBundleRequest request;
  request.mode = memory::MemorySupportBundleMode::low_memory;
  request.limits.max_rows = 64;
  request.limits.max_output_bytes = 4096;
  request.snapshot.current_bytes = 2048;
  request.snapshot.peak_bytes = 4096;
  request.snapshot.contexts.push_back(
      {"statement",
       "secret-canary-eler088-memory protected_reference",
       2048,
       4096,
       1,
       0,
       0,
       1});
  request.snapshot.categories.push_back(
      {memory::MemoryCategory::diagnostics, 2048, 4096, 1, 0, 0, 1});
  platform::DiagnosticRecord diagnostic;
  diagnostic.status = {StatusCode::memory_invalid_request,
                       Severity::error,
                       Subsystem::memory};
  diagnostic.diagnostic_code = "MEMORY.SECRET_CANARY";
  diagnostic.message_key = "memory.redaction";
  diagnostic.arguments.push_back(
      {"password", "secret-canary-eler088-memory"});
  request.diagnostics.push_back(std::move(diagnostic));
  request.protected_memory_review.enabled = true;
  request.protected_memory_review.diagnostics_log_exception_scan_complete = true;
  request.protected_memory_review.support_bundle_scan_complete = true;
  request.protected_memory_review.zeroization_not_optimized_away = true;
  request.protected_memory_review.hsm_kms_plugin_routes_use_protected_buffers = true;
  memory::ProtectedMemoryEvidence protected_buffer;
  protected_buffer.platform_lock_attempted = true;
  protected_buffer.no_dump_attempted = true;
  protected_buffer.protected_material_redacted = true;
  protected_buffer.zero_on_release = true;
  protected_buffer.platform_name = "linux";
  protected_buffer.authority_scope = "evidence_only";
  request.protected_memory_review.protected_buffer_evidence.push_back(
      std::move(protected_buffer));
  request.protected_memory_review.secret_routes.push_back(
      {"auth_token",
       "secret-canary-eler088-protected-material",
       true,
       true,
       false});

  const auto result = memory::BuildMemorySupportBundleEvidence(request);
  Require(result.ok(), "ELER-088 memory support bundle refused valid canary proof");
  std::ostringstream flat;
  for (const auto& row : result.rows) {
    flat << row.key << '=' << row.value << ':' << row.redaction_class << '\n';
  }
  for (const auto& evidence : result.evidence) {
    flat << "evidence=" << evidence << '\n';
  }
  RequireNoCanary(flat.str(), canaries, "memory_support_bundle");
  RequireContains(flat.str(),
                  "<protected-material-excluded>",
                  "ELER-088 memory support bundle protected marker missing");

  auto emergency = request;
  emergency.mode = memory::MemorySupportBundleMode::emergency_summary;
  const auto emergency_summary =
      memory::BuildMemorySupportBundleEmergencySummary(emergency);
  std::ostringstream emergency_flat;
  for (u64 i = 0; i < emergency_summary.row_count; ++i) {
    emergency_flat << emergency_summary.rows[static_cast<std::size_t>(i)].key.data()
                   << '='
                   << emergency_summary.rows[static_cast<std::size_t>(i)].value.data()
                   << '\n';
  }
  RequireNoCanary(emergency_flat.str(),
                  canaries,
                  "memory_emergency_support_bundle");
  RequireContains(emergency_flat.str(),
                  "<protected-material-excluded>",
                  "ELER-088 emergency summary protected marker missing");
  AddRow("memory",
         "low_memory_and_emergency_canaries",
         "protected_material_memory",
         "diagnostic_arguments;top_context;protected_route",
         "<protected-material-excluded>",
         "low-memory rows and emergency summary contain no canary");
}

api::ClusterSupportBundleProjectionSource ClusterProjection(
    api::ClusterProjectionRedactionSensitivity sensitivity,
    std::string canary) {
  api::ClusterSupportBundleProjectionSource source;
  source.sensitivity = sensitivity;
  source.projection_id = "eler088-cluster-projection";
  source.projection_source = "donor_connector_supportability_projection";
  source.target_uuid = MakeUuid(UuidKind::object, 80);
  source.retention_policy_ref = "support.cluster.retention.redacted";
  source.support_bundle_policy_ref = "support.cluster.bundle.redacted";
  source.retention_evidence_present = true;
  source.fields.push_back({"sensitive_detail", std::move(canary), true});
  source.fields.push_back({"projection_state", "active", false});
  return source;
}

void ClusterSupportBundleCanaryProof(const std::vector<std::string>& canaries) {
  api::EngineBuildClusterProjectionSupportBundleRedactionRequest request;
  request.context = SupportContext();
  request.operation_id = "eler088.cluster_support_bundle";
  request.support_bundle_id = "support-bundle:eler088";
  request.capture_generation = "generation:88";
  request.projections.push_back(ClusterProjection(
      api::ClusterProjectionRedactionSensitivity::security,
      "secret-canary-eler088-donor-connector"));
  request.projections.push_back(ClusterProjection(
      api::ClusterProjectionRedactionSensitivity::route,
      "secret-canary-eler088-parser-handoff"));
  const auto result =
      api::EngineBuildClusterProjectionSupportBundleRedaction(request);
  Require(result.ok && result.bundle_allowed && result.sensitive_values_redacted,
          "ELER-088 cluster support bundle redaction did not redact canaries");
  Require(!result.local_runtime_execution_enabled &&
              !result.local_projection_cluster_authority,
          "ELER-088 cluster support bundle claimed local cluster authority");
  const std::string flat = FlattenApiResult(result) + "\n" +
                           result.support_bundle_json;
  RequireNoCanary(flat, canaries, "cluster_projection_support_bundle");
  RequireContains(result.support_bundle_json,
                  "[redacted:security]",
                  "ELER-088 cluster security redaction marker missing");
  RequireContains(result.support_bundle_json,
                  "[redacted:route]",
                  "ELER-088 cluster route redaction marker missing");
  AddRow("cluster_projection",
         "donor_connector_projection_canaries",
         "donor_connector_parser_handoff",
         "cluster_projection_sensitive_fields",
         "[redacted:security];[redacted:route]",
         "cluster support-bundle projection is evidence-only and contains no canary");
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path output =
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
  const auto canaries = CanarySet();
  ListenerSupportBundleCanaryProof(canaries);
  EngineSupportBundleCanaryProof(canaries);
  EngineSupportBundleFailClosedProof();
  MemorySupportBundleCanaryProof(canaries);
  ClusterSupportBundleCanaryProof(canaries);
  WriteMatrix(output);
  std::cout << "engine_listener_support_bundle_redaction_gate=passed\n";
  return EXIT_SUCCESS;
}
