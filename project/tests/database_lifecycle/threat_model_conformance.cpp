// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_feature_gates.hpp"
#include "agent_workload_resource_quota.hpp"
#include "backup_archive/backup_archive_api.hpp"
#include "config_policy_security_lifecycle.hpp"
#include "management/support_bundle_api.hpp"
#include "manager_control.hpp"
#include "maintenance_coordinator.hpp"
#include "sbps.hpp"
#include "server_observability.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013t_threat_model.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013T threat-model test");
  return std::filesystem::path(made);
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const server::DatabaseLifecycleThreatModelResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasManagementDiagnostic(const server::ServerManagementResponse& result,
                             std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasMaintenanceDiagnostic(const server::ServerMaintenanceOperationResult& result,
                              std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEngineDiagnostic(const api::EngineApiResult& result,
                         std::string_view code_or_detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code_or_detail || diagnostic.detail == code_or_detail) return true;
    if (diagnostic.detail.size() >= code_or_detail.size() &&
        diagnostic.detail.compare(diagnostic.detail.size() - code_or_detail.size(),
                                  code_or_detail.size(),
                                  code_or_detail) == 0) {
      return true;
    }
  }
  return false;
}

bool HasBackupAdmissionDiagnostic(const api::BackupArchiveLifecycleAdmission& admission,
                                  std::string_view code) {
  return admission.diagnostic.detail == code ||
         (admission.diagnostic.detail.size() >= code.size() &&
          admission.diagnostic.detail.compare(admission.diagnostic.detail.size() - code.size(),
                                              code.size(),
                                              code) == 0);
}

server::DatabaseLifecycleThreatModelInput ThreatInput(
    server::DatabaseLifecycleThreatSurface surface) {
  server::DatabaseLifecycleThreatModelInput input;
  input.surface = surface;
  input.operation_key = server::DatabaseLifecycleThreatSurfaceName(surface);
  input.required_right = "OBS_MANAGEMENT_CONTROL";
  if (surface == server::DatabaseLifecycleThreatSurface::kIpcAuthorization) {
    input.required_right = "OBS_MANAGEMENT_INSPECT";
  }
  if (surface == server::DatabaseLifecycleThreatSurface::kUdrLoading) {
    input.required_right = "UDR_MANAGE";
    input.udr_or_plugin_load_requested = true;
  }
  if (surface == server::DatabaseLifecycleThreatSurface::kBackupRestore) {
    input.required_right = "BACKUP_CONTROL";
  }
  if (surface == server::DatabaseLifecycleThreatSurface::kSupportBundle) {
    input.required_right = "SUPPORT_EXPORT";
  }
  if (surface == server::DatabaseLifecycleThreatSurface::kForceShutdown) {
    input.force_shutdown_requested = true;
  }
  if (surface == server::DatabaseLifecycleThreatSurface::kResourceQuota) {
    input.quota_requested = true;
  }
  return input;
}

void TestCentralThreatModelGate() {
  const std::vector<server::DatabaseLifecycleThreatSurface> surfaces = {
      server::DatabaseLifecycleThreatSurface::kForceShutdown,
      server::DatabaseLifecycleThreatSurface::kIpcAuthentication,
      server::DatabaseLifecycleThreatSurface::kIpcAuthorization,
      server::DatabaseLifecycleThreatSurface::kManagerSupervision,
      server::DatabaseLifecycleThreatSurface::kListenerSupervision,
      server::DatabaseLifecycleThreatSurface::kParserSupervision,
      server::DatabaseLifecycleThreatSurface::kUdrLoading,
      server::DatabaseLifecycleThreatSurface::kHealthReporting,
      server::DatabaseLifecycleThreatSurface::kBackupRestore,
      server::DatabaseLifecycleThreatSurface::kSupportBundle,
      server::DatabaseLifecycleThreatSurface::kServiceFiles,
      server::DatabaseLifecycleThreatSurface::kResourceQuota,
  };
  for (const auto surface : surfaces) {
    const auto accepted =
        server::EvaluateDatabaseLifecycleThreatModelGate(ThreatInput(surface));
    Require(accepted.ok(), "safe threat-model surface was not admitted");
    const auto json = server::SerializeDatabaseLifecycleThreatModelResultJson(accepted);
    Require(Contains(json, "DBLC_P13T_THREAT_MODEL_COMPLETE"),
            "threat-model result missing completion gate label");
    Require(Contains(json, "database_lifecycle_threat_model"),
            "threat-model result missing ctest gate label");
  }

  auto missing_session = ThreatInput(server::DatabaseLifecycleThreatSurface::kIpcAuthentication);
  missing_session.session_context_present = false;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(missing_session),
                        "DBLC.THREAT.SESSION_REQUIRED"),
          "missing session context did not fail closed");

  auto missing_auth = ThreatInput(server::DatabaseLifecycleThreatSurface::kIpcAuthorization);
  missing_auth.security_context_present = false;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(missing_auth),
                        "DBLC.THREAT.SECURITY_CONTEXT_REQUIRED"),
          "missing security context did not fail closed");

  auto parser_authority = ThreatInput(server::DatabaseLifecycleThreatSurface::kParserSupervision);
  parser_authority.authority_actor = "parser";
  parser_authority.parser_listener_driver_claims_authority = true;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(parser_authority),
                        "DBLC.THREAT.ENGINE_AUTHORITY_REQUIRED"),
          "parser authority claim was not refused");

  auto insufficient_role = ThreatInput(server::DatabaseLifecycleThreatSurface::kIpcAuthorization);
  insufficient_role.engine_authorization_granted = false;
  insufficient_role.required_role_present = false;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(insufficient_role),
                        "SECURITY.AUTHORIZATION.DENIED"),
          "insufficient role did not fail closed");

  auto ambiguous_force = ThreatInput(server::DatabaseLifecycleThreatSurface::kForceShutdown);
  ambiguous_force.ambiguous_process_association = true;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(ambiguous_force),
                        "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS"),
          "ambiguous force-shutdown association was not refused");

  auto unsafe_force = ThreatInput(server::DatabaseLifecycleThreatSurface::kForceShutdown);
  unsafe_force.force_termination_policy_present = false;
  unsafe_force.mga_recovery_evidence_preserved = false;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(unsafe_force),
                        "ENGINE.SHUTDOWN_FORCE_UNSAFE_REFUSED"),
          "unsafe force shutdown was not refused");

  auto untrusted_udr = ThreatInput(server::DatabaseLifecycleThreatSurface::kUdrLoading);
  untrusted_udr.trusted_package_signature_present = false;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(untrusted_udr),
                        "SB_ENGINE_API_UDR_TRUST_REQUIRED"),
          "unauthorized UDR/plugin/package loading was not refused");

  auto backup_no_policy = ThreatInput(server::DatabaseLifecycleThreatSurface::kBackupRestore);
  backup_no_policy.backup_restore_policy_present = false;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(backup_no_policy),
                        "BACKUP_POLICY_REQUIRED"),
          "backup/restore without policy was not refused");

  auto health_leak = ThreatInput(server::DatabaseLifecycleThreatSurface::kHealthReporting);
  health_leak.health_redaction_applied = false;
  health_leak.protected_material_requested = true;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(health_leak),
                        "OPS.HEALTH.REDACTION_REQUIRED"),
          "health-reporting redaction bypass was not refused");

  auto support_leak = ThreatInput(server::DatabaseLifecycleThreatSurface::kSupportBundle);
  support_leak.support_bundle_redaction_applied = false;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(support_leak),
                        "OPS.SUPPORT_BUNDLE.REDACTION_REQUIRED"),
          "support-bundle redaction bypass was not refused");

  auto unsafe_service_file = ThreatInput(server::DatabaseLifecycleThreatSurface::kServiceFiles);
  unsafe_service_file.service_file_private_mode = false;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(unsafe_service_file),
                        "SERVER.SERVICE_FILE.PRIVATE_MODE_REQUIRED"),
          "unsafe lifecycle service-file mode was not refused");

  auto quota_bypass = ThreatInput(server::DatabaseLifecycleThreatSurface::kResourceQuota);
  quota_bypass.quota_bypass_requested = true;
  Require(HasDiagnostic(server::EvaluateDatabaseLifecycleThreatModelGate(quota_bypass),
                        "WORKLOAD_RESOURCE.BYPASS_REFUSED"),
          "quota bypass attempt was not refused");
}

server::ServerMaintenanceOperationRequest MaintenanceRequest(std::string operation_key,
                                                             std::string mode) {
  server::ServerMaintenanceOperationRequest request;
  request.operation_key = std::move(operation_key);
  request.mode = std::move(mode);
  request.request_uuid = sbps::MakeUuidV7Bytes();
  request.session_uuid = sbps::MakeUuidV7Bytes();
  return request;
}

void TestForceShutdownRuntimeGate(const std::filesystem::path& temp_dir) {
  server::ServerBootstrapConfig config;
  config.database_default_path = temp_dir / "shutdown-target.sbdb";
  server::ServerLifecycleArtifacts artifacts;
  artifacts.generation = 13;
  artifacts.state = "dblc013t";
  auto coordinator = server::BuildMaintenanceCoordinator(config, artifacts);

  server::ServerShutdownRuntimeSnapshot snapshot;
  snapshot.database_path = config.database_default_path.string();
  snapshot.database_uuid = "019e13d0-0000-7000-8000-000000000001";
  snapshot.association_scope_proven = true;
  snapshot.associated_manager_count = 1;
  snapshot.associated_listener_count = 1;
  snapshot.associated_parser_count = 1;
  snapshot.associated_ipc_endpoint_count = 1;
  snapshot.associated_session_count = 1;
  snapshot.active_transaction_session_count = 1;
  snapshot.required_acknowledgement_count = 5;
  snapshot.acknowledged_component_count = 5;
  snapshot.drain_complete = false;

  auto ambiguous = snapshot;
  ambiguous.association_scope_proven = false;
  const auto ambiguous_result = server::ApplyDatabaseShutdownOperation(
      &coordinator,
      config,
      MaintenanceRequest("shutdown_database_force", "acknowledgements_satisfied:true"),
      ambiguous);
  Require(!ambiguous_result.ok &&
              HasMaintenanceDiagnostic(ambiguous_result, "ENGINE.SHUTDOWN_SCOPE_INVALID"),
          "ambiguous force-shutdown process association was admitted");

  const auto unsafe = server::ApplyDatabaseShutdownOperation(
      &coordinator,
      config,
      MaintenanceRequest("shutdown_database_force", "acknowledgements_satisfied:true"),
      snapshot);
  Require(!unsafe.ok && HasMaintenanceDiagnostic(unsafe, "ENGINE.SHUTDOWN_INPUT_INVALID"),
          "force shutdown without policy and recovery evidence was admitted");

  const auto safe = server::ApplyDatabaseShutdownOperation(
      &coordinator,
      config,
      MaintenanceRequest("shutdown_database_force",
                         "force_termination_policy_uuid:019e13d0-0000-7000-8000-000000000011;"
                         "recovery_evidence_preserved:true;acknowledgements_satisfied:true"),
      snapshot);
  Require(safe.ok && Contains(safe.records_json, "unknown_transaction_finality_preserved"),
          "force shutdown with explicit policy did not preserve finality evidence");
}

std::array<std::uint8_t, 16> AddSession(server::ServerSessionRegistry* registry,
                                        std::string_view principal,
                                        const std::filesystem::path& database_path,
                                        std::string database_uuid) {
  server::ServerSessionRecord session;
  session.connection_uuid = sbps::MakeUuidV7Bytes();
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_claim = std::string(principal);
  session.database_path = database_path.string();
  session.database_uuid = std::move(database_uuid);
  session.effective_user_uuid = sbps::MakeUuidV7Bytes();
  session.catalog_generation = 1;
  session.security_epoch = 1;
  session.resource_epoch = 1;
  session.name_resolution_epoch = 1;
  registry->sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] = session;
  return session.session_uuid;
}

sbps::Frame ManagementFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string operation_key,
                            std::string mode = {}) {
  server::ServerManagementRequest request;
  request.operation_key = std::move(operation_key);
  request.mode = std::move(mode);
  request.audit_reason = "dblc013t_threat_model";
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kManagementRequest);
  frame.header.payload_schema_id = sbps::kSchemaManagementRequestV1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = server::EncodeServerManagementRequestForTest(request);
  return frame;
}

void TestManagementIpcHealthAuthGate(const std::filesystem::path& temp_dir) {
  const auto database_path = temp_dir / "management.sbdb";
  const std::string database_uuid = "019e13d0-0000-7000-8000-000000000002";
  server::ServerBootstrapConfig config;
  config.database_default_path = database_path;
  config.control_dir = temp_dir / "control";
  server::ServerLifecycleArtifacts artifacts;
  artifacts.generation = 13;
  artifacts.state = "service_ready";
  server::HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_path = database_path.string();
  database.database_uuid = database_uuid;
  database.database_open = true;
  engine_state.databases.push_back(database);
  server::ParserPackageRegistry parser_registry;
  server::ServerListenerOrchestrator listeners;
  server::ServerSessionRegistry registry;
  auto coordinator = server::BuildMaintenanceCoordinator(config, artifacts);
  server::ServerObservabilityState observability;
  observability.metrics_enabled = false;

  server::ServerManagementContext context;
  context.config = &config;
  context.artifacts = &artifacts;
  context.engine_state = &engine_state;
  context.session_registry = &registry;
  context.parser_registry = &parser_registry;
  context.listener_orchestrator = &listeners;
  context.maintenance_coordinator = &coordinator;
  context.observability = &observability;

  const auto missing_session =
      server::HandleServerManagementRequest(context, ManagementFrame({}, "show_server_health"));
  Require(missing_session.error &&
              HasManagementDiagnostic(missing_session, "PARSER_SERVER_IPC.SESSION_NOT_BOUND"),
          "management IPC admitted missing session context");

  const auto auditor = AddSession(&registry, "auditor", database_path, database_uuid);
  const auto denied =
      server::HandleServerManagementRequest(context, ManagementFrame(auditor, "verify_database"));
  Require(denied.error && HasManagementDiagnostic(denied, "SECURITY.ACCESS_DENIED"),
          "management IPC admitted insufficient role");

  const auto health =
      server::HandleServerManagementRequest(context, ManagementFrame(auditor, "show_server_health"));
  Require(health.accepted && !health.error, "least-privilege health request was refused");
  const std::string payload(health.payload.begin(), health.payload.end());
  Require(Contains(payload, "\"authorization_authority\":\"engine\""),
          "health response did not report engine authorization authority");
  Require(Contains(payload, "\"redaction_state\":\"redacted\""),
          "health response did not report redaction state");
  Require(!Contains(payload, database_path.string()),
          "health response leaked local database path");
}

api::EngineRequestContext EngineContext(const std::filesystem::path& database_path) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = "019e13d0-0000-7000-8000-000000000003";
  context.principal_uuid.canonical = "019e13d0-0000-7000-8000-000000000004";
  context.session_uuid.canonical = "019e13d0-0000-7000-8000-000000000005";
  context.security_context_present = true;
  context.trace_tags.push_back("security.bootstrap");
  return context;
}

void TestBackupRestoreSupportBundlePolicyGates(const std::filesystem::path& temp_dir) {
  api::EngineApiRequest backup;
  backup.context = EngineContext(temp_dir / "backup.sbdb");
  backup.option_envelopes.push_back("backup_policy_installed:false");
  const auto no_backup_policy = api::EvaluateBackupArchiveLifecycleAdmission(
      backup, api::BackupArchiveLifecycleOperation::physical_backup);
  Require(!no_backup_policy.admitted &&
              HasBackupAdmissionDiagnostic(no_backup_policy, "BACKUP_POLICY_REQUIRED"),
          "backup without policy was admitted");

  api::EngineApiRequest missing_security = backup;
  missing_security.context.security_context_present = false;
  missing_security.option_envelopes.clear();
  const auto missing_security_result = api::EvaluateBackupArchiveLifecycleAdmission(
      missing_security, api::BackupArchiveLifecycleOperation::physical_backup);
  Require(!missing_security_result.admitted &&
              HasBackupAdmissionDiagnostic(missing_security_result,
                                           "BACKUP_SECURITY_CONTEXT_REQUIRED"),
          "backup without security context was admitted");

  api::EngineApiRequest restore;
  restore.context = EngineContext(temp_dir / "restore.sbdb");
  restore.option_envelopes.push_back("restore_policy_installed:false");
  const auto no_restore_policy = api::EvaluateBackupArchiveLifecycleAdmission(
      restore, api::BackupArchiveLifecycleOperation::physical_restore);
  Require(!no_restore_policy.admitted &&
              HasBackupAdmissionDiagnostic(no_restore_policy, "RESTORE_POLICY_REQUIRED"),
          "restore without policy was admitted");

  api::EnginePrepareSupportBundleRequest support;
  support.context = EngineContext(temp_dir / "support.sbdb");
  support.option_envelopes.push_back("engine_authorized_support_export:true");
  support.option_envelopes.push_back("support_bundle_policy_installed:false");
  const auto no_support_policy = api::EnginePrepareSupportBundle(support);
  Require(!no_support_policy.ok &&
              HasEngineDiagnostic(no_support_policy, "OPS.SUPPORT_BUNDLE.POLICY_REQUIRED"),
          "support bundle without policy was admitted");

  support.option_envelopes.clear();
  support.option_envelopes.push_back("engine_authorized_support_export:true");
  support.option_envelopes.push_back("redaction_disabled:true");
  const auto redaction_disabled = api::EnginePrepareSupportBundle(support);
  Require(!redaction_disabled.ok &&
              HasEngineDiagnostic(redaction_disabled,
                                  "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN"),
          "support bundle redaction bypass was admitted");
}

agents::InstalledCapabilityRecord InstalledCapability() {
  agents::InstalledCapabilityRecord record;
  record.capability_id = "udr.parser.support";
  record.provider_id = "sb_server";
  record.edition_scope = agents::CapabilityEditionScope::enterprise;
  record.lifecycle_state = agents::CapabilityLifecycleState::enabled;
  record.capability_epoch = 3;
  record.installed_policy_epoch = 7;
  record.requires_parser_package = true;
  record.required_parser_package_id = "builtin_test_package";
  record.parser_package_installed = true;
  return record;
}

agents::FeatureGateRequest FeatureRequest() {
  agents::FeatureGateRequest request;
  request.request_id = "dblc013t";
  request.capability_id = "udr.parser.support";
  request.requested_edition_scope = agents::CapabilityEditionScope::community;
  request.observed_policy_epoch = 7;
  request.minimum_capability_epoch = 1;
  request.parser_package_available = true;
  request.plugin_or_package_load_requested = true;
  return request;
}

void TestFeatureAndQuotaThreatGates() {
  const auto record = InstalledCapability();
  const auto allowed = agents::EvaluateFeatureGateRequest(record, FeatureRequest(), 7);
  Require(allowed.decision_class == agents::FeatureGateDecisionClass::allow,
          "trusted feature/package gate was refused");

  auto missing_context = FeatureRequest();
  missing_context.security_context_present = false;
  const auto missing_context_result =
      agents::EvaluateFeatureGateRequest(record, missing_context, 7);
  Require(missing_context_result.decision_class == agents::FeatureGateDecisionClass::fail_closed &&
              missing_context_result.diagnostic_code ==
                  "SB_AGENT_CAPABILITY.SECURITY_CONTEXT_REQUIRED",
          "feature/package gate admitted missing security context");

  auto parser_authority = FeatureRequest();
  parser_authority.authority_actor = "parser";
  parser_authority.parser_listener_driver_claims_authority = true;
  const auto parser_authority_result =
      agents::EvaluateFeatureGateRequest(record, parser_authority, 7);
  Require(parser_authority_result.diagnostic_code ==
              "SB_AGENT_CAPABILITY.ENGINE_AUTHORITY_REQUIRED",
          "feature/package gate accepted parser authority");

  auto unsigned_package = FeatureRequest();
  unsigned_package.trusted_package_signature_present = false;
  const auto unsigned_result =
      agents::EvaluateFeatureGateRequest(record, unsigned_package, 7);
  Require(unsigned_result.diagnostic_code == "SB_AGENT_CAPABILITY.PACKAGE_TRUST_REQUIRED",
          "feature/package gate admitted untrusted package load");

  agents::WorkloadResourceQuotaController controller;
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = "parser";
  pool.workload_class = agents::WorkloadClass::parser;
  pool.limits.hard.memory_bytes = 10;
  pool.limits.hard.worker_slots = 10;
  pool.limits.hard.active_requests = 10;
  Require(controller.RegisterPool(pool).ok, "quota pool registration failed");

  agents::WorkloadAdmissionRequest over;
  over.request_uuid = "quota-over";
  over.pool_id = "parser";
  over.workload_class = agents::WorkloadClass::parser;
  over.source = agents::WorkloadAdmissionSource::parser;
  over.requested.memory_bytes = 11;
  const auto over_result = controller.Admit(over);
  Require(!over_result.status.ok &&
              over_result.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.HARD_DENIED" &&
              !over_result.reservation_created(),
          "parser route bypassed hard quota");

  agents::WorkloadAdmissionRequest empty;
  empty.request_uuid = "quota-empty";
  empty.pool_id = "parser";
  empty.workload_class = agents::WorkloadClass::parser;
  empty.source = agents::WorkloadAdmissionSource::listener;
  const auto empty_result = controller.Admit(empty);
  Require(!empty_result.status.ok &&
              empty_result.diagnostic.diagnostic_code ==
                  "WORKLOAD_RESOURCE.EMPTY_RESERVATION_REFUSED" &&
              !empty_result.reservation_created(),
          "listener route started work without quota reservation");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  TestCentralThreatModelGate();
  TestForceShutdownRuntimeGate(temp_dir);
  TestManagementIpcHealthAuthGate(temp_dir);
  TestBackupRestoreSupportBundlePolicyGates(temp_dir);
  TestFeatureAndQuotaThreatGates();
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
