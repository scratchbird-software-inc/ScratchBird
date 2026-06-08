// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "diagnostics.hpp"
#include "diagnostics/diagnostic_rendering.hpp"
#include "api_diagnostics.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "observability/metrics_api.hpp"
#include "security/audit_api.hpp"
#include "server_observability.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace server = scratchbird::server;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc015_observability.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-015 observability test");
  return std::filesystem::path(made);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

server::ServerBootstrapConfig Config(const std::filesystem::path& temp_dir) {
  server::ServerBootstrapConfig config;
  config.control_dir = temp_dir / "control";
  config.data_dir = temp_dir / "data";
  config.log_file = (temp_dir / "control" / "sb_server.log").string();
  config.database_default_path = temp_dir / "data" / "observability.sbdb";
  config.metrics_enabled = true;
  return config;
}

server::ServerLifecycleArtifacts Artifacts() {
  server::ServerLifecycleArtifacts artifacts;
  artifacts.generation = 15015;
  artifacts.state = "service_ready";
  artifacts.pid_file = "[path-redacted]";
  artifacts.lifecycle_state_file = "[path-redacted]";
  artifacts.lifecycle_journal_file = "[path-redacted]";
  return artifacts;
}

server::HostedEngineState Engine(const std::filesystem::path& temp_dir) {
  server::HostedEngineState state;
  state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_uuid = "019e150f-0000-7000-8000-000000000015";
  database.database_path = (temp_dir / "data" / "observability.sbdb").string();
  database.database_open = true;
  state.databases.push_back(database);
  return state;
}

api::EngineRequestContext EngineContext(const std::filesystem::path& temp_dir) {
  std::filesystem::create_directories(temp_dir / "data");
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.security_context_present = true;
  context.database_path = (temp_dir / "data" / "observability.sbdb").string();
  context.database_uuid.canonical = "019e150f-0000-7000-8000-000000000015";
  context.session_uuid.canonical = "019e150f-0000-7000-8000-000000000016";
  context.principal_uuid.canonical = "019e150f-0000-7000-8000-000000000017";
  context.request_id = "019e150f-0000-7000-8000-000000000018";
  context.local_transaction_id = 15;
  context.trace_tags = {"group:OPS", "right:AUDIT_READ"};
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "database_lifecycle_observability_conformance",
      {"OBS_METRICS_READ_FAMILY", "OBS_METRICS_READ_ALL", "AUDIT_READ"});
  return context;
}

void TestDiagnosticShapes() {
  server::ServerDiagnostic diagnostic;
  diagnostic.code = "ENGINE.SHUTDOWN_ACK_TIMEOUT";
  diagnostic.message_key = "engine.shutdown.ack_timeout";
  diagnostic.safe_message = "Shutdown acknowledgement timed out.";
  diagnostic.retryable = true;
  diagnostic.correlation_uuid = "019e150f-0000-7000-8000-000000000019";
  diagnostic.request_uuid = "019e150f-0000-7000-8000-000000000018";
  diagnostic.session_uuid = "019e150f-0000-7000-8000-000000000016";
  diagnostic.database_uuid = "019e150f-0000-7000-8000-000000000015";
  diagnostic.fields = {
      {"operation_key", "shutdown_database"},
      {"database_uuid", diagnostic.database_uuid},
      {"database_path", "/tmp/secret/observability.sbdb"},
      {"password", "cleartext"},
      {"reason", "ack_timeout"}};

  const auto public_vector = server::ToMessageVectorJsonLine(diagnostic);
  Require(Contains(public_vector, "\"visibility\":\"public\""),
          "public diagnostic vector missing public shape");
  Require(Contains(public_vector, "\"retryable\":true"),
          "public diagnostic vector missing retryability");
  Require(Contains(public_vector, "\"parser_finality_authority\":false"),
          "public diagnostic vector missing parser authority denial");
  Require(!Contains(public_vector, diagnostic.database_uuid),
          "public diagnostic vector leaked database UUID");
  Require(!Contains(public_vector, "/tmp/secret"),
          "public diagnostic vector leaked local path");
  Require(!Contains(public_vector, "cleartext"),
          "public diagnostic vector leaked protected material");

  const auto private_vector = server::ToPrivateMessageVectorJsonLine(diagnostic);
  Require(Contains(private_vector, "\"visibility\":\"private\""),
          "private diagnostic vector missing private shape");
  Require(Contains(private_vector, diagnostic.database_uuid),
          "private diagnostic vector missing correlation evidence");
  Require(Contains(private_vector, "[redacted]"),
          "private diagnostic vector missing protected-material redaction");
  Require(!Contains(private_vector, "cleartext"),
          "private diagnostic vector leaked protected material");
}

void TestServerLifecycleObservability(const std::filesystem::path& temp_dir) {
  const auto config = Config(temp_dir);
  const auto artifacts = Artifacts();
  const auto engine = Engine(temp_dir);
  server::ParserPackageRegistry parser_registry;
  server::ServerListenerOrchestrator listeners;
  auto observability =
      server::InitializeServerObservability(config, artifacts, engine, parser_registry, listeners);

  server::ServerLifecycleObservabilityEvent success;
  success.operation_key = "create_database";
  success.outcome = "created";
  success.route_family = "server_management";
  success.request_uuid = "019e150f-0000-7000-8000-000000000018";
  success.session_uuid = "019e150f-0000-7000-8000-000000000016";
  success.database_uuid = engine.databases.front().database_uuid;
  success.state_before = "none";
  success.state_after = "created";
  const auto recorded = server::RecordServerLifecycleObservability(&observability, success);
  Require(recorded.recorded, "lifecycle success observability was not recorded");
  Require(!recorded.audit_event_uuid.empty(), "lifecycle success missing audit event UUID");
  Require(!recorded.cache_marker_uuid.empty(), "lifecycle success missing cache invalidation marker");
  Require(observability.cache_invalidation_markers.size() == 1,
          "lifecycle cache invalidation marker not stored");
  Require(Contains(recorded.message_vector_public_json, "\"visibility\":\"public\""),
          "lifecycle success missing public message vector");
  Require(Contains(recorded.message_vector_private_json, "\"visibility\":\"private\""),
          "lifecycle success missing private message vector");
  Require(Contains(ReadFile(observability.audit_path), "server.lifecycle.create_database"),
          "lifecycle audit evidence not persisted");

  server::ServerLifecycleObservabilityEvent refused;
  refused.operation_key = "force_shutdown_database";
  refused.outcome = "refused";
  refused.diagnostic_code = "ENGINE.SHUTDOWN_INPUT_INVALID";
  refused.route_family = "server_management";
  refused.database_uuid = engine.databases.front().database_uuid;
  refused.private_detail = "force shutdown refused without MGA recovery evidence";
  const auto refused_record =
      server::RecordServerLifecycleObservability(&observability, refused);
  Require(refused_record.recorded, "lifecycle refusal observability was not recorded");
  Require(refused_record.cache_marker_uuid.empty(),
          "refused lifecycle operation should not publish success cache marker");
  Require(observability.lifecycle_events.size() == 2,
          "lifecycle event inventory missing success/refusal records");
  Require(Contains(server::ServerMetricsSnapshotJson(observability),
                   "sys.metrics.lifecycle.operation_total"),
          "lifecycle operation metric missing from server snapshot");
}

void TestEngineMetricsAndAudit(const std::filesystem::path& temp_dir) {
  api::EngineRecordLifecycleMetricRequest metric;
  metric.context = EngineContext(temp_dir);
  metric.operation_key = "repair_database";
  metric.outcome = "repaired";
  metric.route_family = "engine_internal";
  metric.cache_invalidation_required = true;
  metric.cache_family = "lifecycle_metadata";
  metric.cache_reason = "repair_database:repaired";
  const auto metric_result = api::EngineRecordLifecycleMetric(metric);
  Require(metric_result.ok, "engine lifecycle metric recording failed");
  Require(metric_result.metric_recorded, "engine lifecycle metric flag missing");
  Require(metric_result.cache_invalidation_recorded,
          "engine lifecycle cache invalidation metric flag missing");

  api::EngineEmitLifecycleAuditEventRequest audit;
  audit.context = EngineContext(temp_dir);
  audit.operation_key = "repair_database";
  audit.outcome = "repaired";
  audit.correlation_uuid = "019e150f-0000-7000-8000-000000000019";
  audit.cache_invalidation_recorded = true;
  audit.cache_marker_uuid = "019e150f-0000-7000-8000-000000000020";
  const auto audit_result = api::EngineEmitLifecycleAuditEvent(audit);
  Require(audit_result.ok, "engine lifecycle audit emission failed");
  Require(audit_result.emitted && audit_result.redacted && audit_result.cache_marker_linked,
          "engine lifecycle audit evidence flags incomplete");

  api::EngineSysMetricsCurrentRequest current;
  current.context = EngineContext(temp_dir);
  current.option_envelopes.push_back("family:sb_lifecycle_operation_total");
  const auto snapshot = api::EngineSysMetricsCurrent(current);
  Require(snapshot.ok, "engine lifecycle metric snapshot failed");
  bool found = false;
  for (const auto& row : snapshot.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == "metric" &&
          field.second.encoded_value == "sb_lifecycle_operation_total") {
        found = true;
      }
    }
  }
  Require(found, "engine lifecycle metric not exposed through sys.metrics.current");
}

void TestParserRendering() {
  api::EngineApiResult result;
  result.ok = false;
  result.operation_id = "lifecycle.shutdown_database";
  result.primary_object.object_kind = "database";
  result.primary_object.uuid.canonical = "019e150f-0000-7000-8000-000000000015";
  result.diagnostics.push_back(api::MakeEngineApiDiagnostic(
      "ENGINE.SHUTDOWN_ACK_TIMEOUT",
      "engine.shutdown.ack_timeout",
      "listener acknowledgement timeout for hidden internal route",
      true));
  api::EngineParserPackageRenderOptions options;
  options.parser_package_uuid = "019e150f-0000-7000-8000-000000000021";
  options.parser_package_version = "sbsql-observability";
  options.client_dialect = "sbsql_v3";
  options.correlation_uuid = "019e150f-0000-7000-8000-000000000019";
  options.request_uuid = "019e150f-0000-7000-8000-000000000018";
  options.session_uuid = "019e150f-0000-7000-8000-000000000016";
  const auto envelope = api::RenderEngineApiResultForParserPackage(result, options);
  std::vector<std::string> errors;
  Require(api::ValidateEngineRenderedResultEnvelope(envelope, &errors),
          "parser rendered lifecycle diagnostic envelope failed validation");
  Require(!envelope.parser_finality_authority && !envelope.donor_finality_authority,
          "parser rendered envelope claimed finality authority");
  Require(envelope.redaction_applied, "parser rendered envelope did not record redaction");
  Require(!envelope.diagnostics.empty() && envelope.diagnostics.front().retryable,
          "parser rendered lifecycle diagnostic missing retryability");
  Require(envelope.diagnostics.front().public_shape_id == "diag.server.lifecycle.v1",
          "parser rendered lifecycle diagnostic missing lifecycle shape");
  Require(envelope.diagnostics.front().detail == "redacted",
          "parser rendered lifecycle diagnostic leaked private detail");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  TestDiagnosticShapes();
  TestServerLifecycleObservability(temp_dir);
  TestEngineMetricsAndAudit(temp_dir);
  TestParserRendering();
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
