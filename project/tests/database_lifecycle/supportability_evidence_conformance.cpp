// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/support_bundle_api.hpp"
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

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013ai_supportability.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013AI supportability test");
  return std::filesystem::path(made);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code_or_detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code_or_detail || diagnostic.detail == code_or_detail) return true;
    if (diagnostic.detail.size() > code_or_detail.size() &&
        diagnostic.detail.compare(diagnostic.detail.size() - code_or_detail.size(),
                                  code_or_detail.size(),
                                  code_or_detail) == 0) {
      return true;
    }
  }
  return false;
}

server::ServerBootstrapConfig Config(const std::filesystem::path& temp_dir) {
  server::ServerBootstrapConfig config;
  config.control_dir = temp_dir / "control";
  config.data_dir = temp_dir / "data";
  config.log_file = (temp_dir / "control" / "sb_server.log").string();
  config.database_default_path = temp_dir / "data" / "example.sbdb";
  config.metrics_enabled = true;
  return config;
}

server::ServerLifecycleArtifacts Artifacts() {
  server::ServerLifecycleArtifacts artifacts;
  artifacts.generation = 13001;
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
  database.database_uuid = "019e13a1-0000-7000-8000-000000000001";
  database.database_path = (temp_dir / "data" / "example.sbdb").string();
  database.database_open = true;
  database.write_admission_fenced = false;
  state.databases.push_back(database);
  return state;
}

void TestServerSupportabilityLifecycle(const std::filesystem::path& temp_dir) {
  const auto config = Config(temp_dir);
  const auto artifacts = Artifacts();
  const auto engine = Engine(temp_dir);
  server::ParserPackageRegistry parser_registry;
  parser_registry.entries.push_back({});
  server::ServerListenerOrchestrator listeners;
  listeners.profiles.push_back({});
  server::ServerSessionRegistry sessions;

  auto observability =
      server::InitializeServerObservability(config, artifacts, engine, parser_registry, listeners);
  server::RecordServerAuditEvent(&observability,
                                 "security.audit",
                                 "completed",
                                 "password=cleartext token=secret-token");
  server::RecordServerLog(&observability,
                          {"diagnostic.capture",
                           "warning",
                           "sb_server",
                           "token=secret-token",
                           "private_key=secret-key credential=secret-credential encryption_key=secret-key-handle",
                           "clean"});

  const auto audit_snapshot = server::ServerAuditSnapshotJson(observability);
  Require(!Contains(audit_snapshot, "cleartext"),
          "audit snapshot leaked protected password value");
  Require(!Contains(audit_snapshot, "secret-token"),
          "audit snapshot leaked protected token value");
  Require(Contains(audit_snapshot, "[redacted]"),
          "audit snapshot did not publish redaction evidence");

  const auto flush = server::FlushServerObservability(&observability, "shutdown token=secret-token");
  Require(flush.flushed, "supportability flush did not persist all evidence files");
  Require(flush.diagnostic_code == "SUPPORTABILITY.FLUSH_COMPLETE",
          "supportability flush diagnostic mismatch");
  Require(Contains(ReadFile(observability.audit_path), "supportability_flush"),
          "audit file missing supportability flush evidence");
  Require(Contains(ReadFile(observability.log_path), "supportability_flush"),
          "operational log missing supportability flush evidence");
  Require(!Contains(ReadFile(observability.log_path), "secret-token"),
          "operational log leaked protected token after redaction");

  const auto export_result =
      server::ExportServerSupportBundle(observability, config, artifacts, engine, sessions, parser_registry, listeners);
  Require(export_result.ok, "support bundle export did not complete");
  Require(export_result.diagnostic_code == "OPS.SUPPORT_BUNDLE.EXPORT_COMPLETE",
          "support bundle export diagnostic mismatch");
  const auto& descriptor = export_result.records_json;
  Require(Contains(descriptor, "\"redaction_state\":\"redacted\""),
          "support bundle descriptor missing redaction state");
  Require(Contains(descriptor, "\"bundle_ref\":\"[path-redacted]\""),
          "support bundle descriptor exposed local path");
  Require(Contains(descriptor, "\"authority_path\":\"engine.authorization.management.SUPPORT_EXPORT\""),
          "support bundle descriptor missing authority path");
  Require(Contains(descriptor, "\"tamper_checksum\":\"fnv1a64:"),
          "support bundle descriptor missing checksum");
  Require(!Contains(descriptor, config.control_dir.string()),
          "support bundle descriptor leaked control directory path");
  Require(!observability.support_bundle_export_uuids.empty(),
          "support bundle export did not record export UUID");
  Require(Contains(ReadFile(observability.support_bundle_index_path), "forbidden_fields_absent"),
          "support bundle index missing forbidden-field evidence");

  std::size_t bundle_count = 0;
  std::filesystem::path bundle_path;
  for (const auto& entry : std::filesystem::directory_iterator(observability.support_bundle_dir)) {
    if (entry.path().extension() == ".json") {
      ++bundle_count;
      bundle_path = entry.path();
    }
  }
  Require(bundle_count == 1, "support bundle export did not create exactly one bundle");
  const auto bundle = ReadFile(bundle_path);
  Require(Contains(bundle, "\"forbidden_fields_absent\":true"),
          "support bundle omitted forbidden-field evidence");
  Require(Contains(bundle, "\"ratio\":100"),
          "support bundle omitted completeness score");
  Require(Contains(bundle, "\"supportability_flush\":\"required_before_export\""),
          "support bundle omitted flush-before-export evidence");
  Require(Contains(bundle, "\"request_uuid\":\""),
          "support bundle manifest omitted request UUID");
  Require(Contains(bundle, "\"excluded_protected_material\""),
          "support bundle manifest omitted protected-material exclusions");
  Require(Contains(bundle, "\"authority_path\":\"engine.authorization.management.SUPPORT_EXPORT\""),
          "support bundle manifest omitted authority path");
  Require(Contains(bundle, "\"tamper_checksum\":\"fnv1a64:"),
          "support bundle manifest omitted tamper checksum");
  Require(!Contains(bundle, "cleartext") && !Contains(bundle, "secret-token") &&
              !Contains(bundle, "secret-key") && !Contains(bundle, "secret-key-handle") &&
              !Contains(bundle, config.control_dir.string()) &&
              !Contains(bundle, engine.databases.front().database_path),
          "support bundle leaked protected material or local paths");

  server::RecordServerLog(&observability,
                          {"large.log", "info", "sb_server", {}, std::string(512, 'x'), "clean"});
  const auto rotation = server::RotateServerOperationalLog(&observability, 1);
  Require(rotation.log_flushed, "operational log rotation failed");
  Require(std::filesystem::exists(observability.log_path.string() + ".1"),
          "operational log rotation did not create rotated file");
  Require(Contains(ReadFile(observability.audit_path), "server.log.rotation"),
          "log rotation did not emit audit evidence");

  auto blocked = observability;
  blocked.support_bundle_export_uuids.clear();
  blocked.support_bundle_dir = temp_dir / "support-bundle-dir-is-file";
  blocked.support_bundle_index_path = blocked.support_bundle_dir / "support_bundle_index.jsonl";
  {
    std::ofstream out(blocked.support_bundle_dir);
    out << "not a directory";
  }
  const auto failed_export =
      server::ExportServerSupportBundle(blocked, config, artifacts, engine, sessions, parser_registry, listeners);
  Require(!failed_export.ok, "support bundle export succeeded after evidence write failure");
  Require(blocked.support_bundle_export_uuids.empty(),
          "failed support bundle export recorded a visible export UUID");
}

void TestEngineSupportBundleApi() {
  api::EnginePrepareSupportBundleRequest request;
  request.context.trust_mode = api::EngineTrustMode::server_isolated;
  request.context.security_context_present = true;
  request.context.database_uuid.canonical = "019e13a1-0000-7000-8000-000000000002";
  request.context.database_path = "/tmp/protected/example.sbdb";
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  const auto prepared = api::EnginePrepareSupportBundle(request);
  Require(prepared.ok, "engine support bundle preparation failed");
  Require(prepared.redaction_applied && prepared.forbidden_fields_absent,
          "engine support bundle API did not enforce redaction");
  Require(prepared.flush_required_before_export,
          "engine support bundle API did not require flush before export");
  Require(prepared.authority_path == "engine.authorization.management.SUPPORT_EXPORT",
          "engine support bundle API did not publish authority path");

  request.option_envelopes.push_back("include_protected_material:true");
  const auto refused = api::EnginePrepareSupportBundle(request);
  Require(!refused.ok, "engine support bundle API accepted protected material export");
  Require(HasDiagnostic(refused, "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN"),
          "engine support bundle protected-material refusal diagnostic mismatch");

  api::EnginePrepareSupportBundleRequest unauth;
  unauth.context = request.context;
  const auto unauthorized = api::EnginePrepareSupportBundle(unauth);
  Require(!unauthorized.ok, "engine support bundle API accepted missing authorization evidence");
  Require(HasDiagnostic(unauthorized, "OPS.SUPPORT_BUNDLE.ENGINE_AUTHORIZATION_REQUIRED"),
          "engine support bundle authorization diagnostic mismatch");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  TestServerSupportabilityLifecycle(temp_dir);
  TestEngineSupportBundleApi();
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
