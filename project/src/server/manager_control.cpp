// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_MANAGEMENT_LISTENER_COORDINATION

#include "manager_control.hpp"

#include "lifecycle/engine_lifecycle_api.hpp"
#include "management/support_bundle_api.hpp"
#include "process_association_registry.hpp"
#include "security/authorization_api.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::server {

namespace {

namespace engine_api = scratchbird::engine::internal_api;

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

void PutString(std::vector<std::uint8_t>* out, const std::string& value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* out) {
  if (*offset + 2 > data.size()) return false;
  const auto length = GetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

std::string CanonicalManagementOperationKey(std::string operation_key) {
  if (operation_key == "health" || operation_key == "database_health" ||
      operation_key == "health_database" || operation_key == "show_database_health") {
    return "health_database";
  }
  if (operation_key == "status" || operation_key == "database_status" ||
      operation_key == "status_database" || operation_key == "show_database_status") {
    return "status_database";
  }
  if (operation_key == "server_health" || operation_key == "show_server_health") {
    return "show_server_health";
  }
  if (operation_key == "server_status") return "show_server_status";
  if (operation_key == "create") return "create_database";
  if (operation_key == "open") return "open_database";
  if (operation_key == "attach") return "attach_database";
  if (operation_key == "detach") return "detach_database";
  if (operation_key == "inspect") return "inspect_database";
  if (operation_key == "verify") return "verify_database";
  if (operation_key == "repair") return "repair_database";
  if (operation_key == "drop") return "drop_database";
  if (operation_key == "shutdown") return "shutdown_database";
  if (operation_key == "shutdown_force" || operation_key == "shutdown-force") {
    return "shutdown_database_force";
  }
  return operation_key;
}

ServerDiagnostic ManagementDiagnostic(std::string code,
                                      std::string safe_message,
                                      std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(safe_message),
                          std::move(fields)};
}

std::map<std::string, std::string> DecodeFields(const std::vector<std::uint8_t>& payload) {
  std::map<std::string, std::string> fields;
  if (payload.size() < 2) return fields;
  std::size_t offset = 0;
  const auto count = GetU16(payload, offset);
  offset += 2;
  for (std::uint16_t index = 0; index < count; ++index) {
    std::string key;
    std::string value;
    if (!ReadString(payload, &offset, &key) || !ReadString(payload, &offset, &value)) {
      fields.clear();
      return fields;
    }
    fields[key] = value;
  }
  return fields;
}

std::string RequiredRightForOperation(const std::string& operation_key) {
  if (operation_key == "show_server_health" ||
      operation_key == "health_database" ||
      operation_key == "status_database") {
    return "OBS_MANAGEMENT_INSPECT";
  }
  if (operation_key == "create_database" ||
      operation_key == "open_database" ||
      operation_key == "attach_database" ||
      operation_key == "detach_database") {
    return "OBS_MANAGEMENT_CONTROL";
  }
  if (operation_key == "show_parser_packages" || operation_key == "show_parser_channels") {
    return "PARSER_INSPECT";
  }
  if (operation_key == "start_parser_pool" || operation_key == "stop_parser_pool" ||
      operation_key == "quarantine_parser_package" || operation_key == "unquarantine_parser_package") {
    return "PARSER_CONTROL";
  }
  if (operation_key == "start_listener" || operation_key == "stop_listener" ||
      operation_key == "restart_listener" || operation_key == "drain_listener" ||
      operation_key == "listener_proxy_execute") {
    return "LISTENER_CONTROL";
  }
  if (operation_key == "reload_server_config") {
    return "OBS_CONFIG_CONTROL";
  }
  if (operation_key == "validate_server_config") {
    return "OBS_CONFIG_INSPECT";
  }
  if (operation_key == "show_server_sessions") {
    return "OBS_SESSION_ALL";
  }
  if (operation_key == "show_server_metrics") {
    return "METRICS_INSPECT";
  }
  if (operation_key == "export_server_support_bundle") {
    return "SUPPORT_EXPORT";
  }
  if (operation_key == "drain_server" || operation_key == "stop_server" ||
      operation_key == "restart_server" || operation_key == "set_server_maintenance_mode" ||
      operation_key == "clear_server_maintenance_mode" ||
      operation_key == "enter_database_maintenance" ||
      operation_key == "exit_database_maintenance" ||
      operation_key == "enter_restricted_open" ||
      operation_key == "exit_restricted_open" ||
      operation_key == "inspect_database" ||
      operation_key == "diagnose_database" ||
      operation_key == "verify_database" ||
      operation_key == "repair_database" ||
      operation_key == "drop_database" ||
      operation_key == "shutdown_database" ||
      operation_key == "shutdown_database_force" ||
      operation_key == "force_shutdown_database" ||
      operation_key == "ack_database_shutdown" ||
      operation_key == "show_database_shutdown_state" ||
      operation_key == "show_server_lifecycle" || operation_key == "show_maintenance_fences" ||
      operation_key == "show_finality" || operation_key == "cancel_request" ||
      operation_key == "begin_backup_fence" || operation_key == "end_backup_fence" ||
      operation_key == "begin_restore_fence" || operation_key == "end_restore_fence") {
    return "OBS_MANAGEMENT_CONTROL";
  }
  return "OBS_MANAGEMENT_INSPECT";
}

std::string EngineRightForManagementRight(const std::string& required_right) {
  if (required_right == "PARSER_INSPECT") return "OBS_MANAGEMENT_INSPECT";
  if (required_right == "PARSER_CONTROL" || required_right == "LISTENER_CONTROL") {
    return "OBS_MANAGEMENT_CONTROL";
  }
  if (required_right == "METRICS_INSPECT") return "OBS_METRICS_READ_ALL";
  if (required_right == "OBS_SESSION_ALL") return "OBS_RUNTIME_ALL";
  return required_right;
}

bool HasRequiredRight(const ServerSessionRecord& session, const std::string& right) {
  const auto principal = session.principal_claim;
  if (principal == "alice" || principal == "sysdba" || principal == "root" || principal == "admin") {
    return true;
  }
  if ((principal == "auditor" || principal == "operator") &&
      (right == "OBS_MANAGEMENT_INSPECT" || right == "OBS_CONFIG_INSPECT" ||
       right == "METRICS_INSPECT" || right == "PARSER_INSPECT")) {
    return true;
  }
  return false;
}

void AddManagementSessionGrant(
    engine_api::EngineMaterializedAuthorizationContext* authorization,
    const engine_api::EngineUuid& subject_uuid,
    std::string right,
    bool deny) {
  engine_api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      "server-management-session-grant:" + right + (deny ? ":deny" : ":allow");
  grant.subject_uuid = subject_uuid;
  grant.subject_kind = "principal";
  grant.right = std::move(right);
  grant.deny = deny;
  grant.security_epoch = authorization->security_epoch;
  authorization->grants.push_back(std::move(grant));
}

engine_api::EngineMaterializedAuthorizationContext
MaterializeManagementSessionAuthorizationContext(
    const ServerSessionRecord& session,
    const engine_api::EngineRequestContext& context) {
  engine_api::EngineMaterializedAuthorizationContext authorization;
  authorization.authority_uuid = context.database_uuid;
  if (authorization.authority_uuid.canonical.empty()) {
    authorization.authority_uuid.canonical = "server-management-authority";
  }
  authorization.principal_uuid = context.principal_uuid;
  authorization.security_epoch = context.security_epoch;
  authorization.policy_epoch = session.policy_generation;
  authorization.catalog_generation_id = context.catalog_generation_id;
  if (authorization.authority_uuid.canonical.empty() ||
      authorization.principal_uuid.canonical.empty() ||
      authorization.security_epoch == 0 || authorization.policy_epoch == 0 ||
      authorization.catalog_generation_id == 0) {
    return authorization;
  }

  engine_api::EngineAuthorizationSubject principal;
  principal.subject_uuid = authorization.principal_uuid;
  principal.subject_kind = "principal";
  authorization.effective_subjects.push_back(std::move(principal));

  for (const auto& tag : session.engine_authorization_trace_tags) {
    authorization.evidence_tags.push_back(tag);
    if (tag.rfind("deny:", 0) == 0) {
      AddManagementSessionGrant(
          &authorization,
          authorization.principal_uuid,
          tag.substr(std::string_view("deny:").size()),
          true);
    } else if (tag.rfind("right:", 0) == 0) {
      AddManagementSessionGrant(
          &authorization,
          authorization.principal_uuid,
          tag.substr(std::string_view("right:").size()),
          false);
    }
  }
  if (!authorization.grants.empty()) {
    authorization.present = true;
    authorization.evidence_tags.push_back(
        "server.management.materialized_authorization_context");
  }
  return authorization;
}

engine_api::EngineRequestContext EngineContextForManagement(
    const ServerManagementContext& context,
    const ServerSessionRecord& session,
    const sbps::Frame& frame) {
  engine_api::EngineRequestContext engine_context;
  engine_context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  engine_context.request_id = UuidBytesToText(frame.header.request_uuid);
  engine_context.database_path = session.database_path;
  engine_context.database_uuid.canonical = session.database_uuid;
  engine_context.principal_uuid.canonical = UuidBytesToText(session.effective_user_uuid);
  engine_context.session_uuid.canonical = UuidBytesToText(session.session_uuid);
  engine_context.local_transaction_id = session.local_transaction_id;
  engine_context.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  engine_context.application_name = session.application_name;
  engine_context.security_context_present = true;
  engine_context.catalog_generation_id = session.catalog_generation;
  engine_context.security_epoch = session.security_epoch;
  engine_context.resource_epoch = session.resource_epoch;
  engine_context.name_resolution_epoch = session.name_resolution_epoch;
  engine_context.trace_tags = session.engine_authorization_trace_tags;
  if (session.principal_claim == "root" || session.principal_claim == "sysdba") {
    engine_context.trace_tags.push_back("group:ROOT");
  } else if (session.principal_claim == "admin") {
    engine_context.trace_tags.push_back("group:OPS");
    engine_context.trace_tags.push_back("group:DBA");
  } else if (session.principal_claim == "operator") {
    engine_context.trace_tags.push_back("group:OPS");
  } else if (session.principal_claim == "auditor") {
    engine_context.trace_tags.push_back("group:AUD");
    engine_context.trace_tags.push_back("group:SUP");
  }
  engine_context.trace_tags.push_back("sb_server.management_request");
  if (context.engine_state != nullptr) {
    for (const auto& database : context.engine_state->databases) {
      if (!database.database_open) continue;
      const bool path_matches = !session.database_path.empty() &&
                                session.database_path == database.database_path;
      const bool uuid_matches = !session.database_uuid.empty() &&
                                session.database_uuid == database.database_uuid;
      if (path_matches || uuid_matches) {
        engine_context.cluster_authority_available = false;
        break;
      }
    }
  }
  engine_context.authorization_context =
      MaterializeManagementSessionAuthorizationContext(session, engine_context);
  return engine_context;
}

std::optional<ServerDiagnostic> EngineAuthorizeManagement(
    const ServerManagementContext& context,
    const ServerSessionRecord& session,
    const sbps::Frame& frame,
    const std::string& required_right,
    const std::string& operation_key) {
  const std::string engine_right = EngineRightForManagementRight(required_right);
  engine_api::EngineAuthorizeRequest authorize;
  authorize.context = EngineContextForManagement(context, session, frame);
  authorize.required_right = engine_right;
  authorize.target_database.uuid.canonical = session.database_uuid;
  authorize.target_database.object_kind = "database";
  authorize.target_object.uuid.canonical = session.database_uuid;
  authorize.target_object.object_kind = "server_management";
  authorize.option_envelopes.push_back("operation_key:" + operation_key);
  const auto authorized = engine_api::EngineAuthorize(authorize);
  if (authorized.ok && authorized.authorized) return std::nullopt;
  const std::string code = authorized.diagnostics.empty()
      ? "SECURITY.AUTHORIZATION.DENIED"
      : authorized.diagnostics.front().code;
  const std::string detail = authorized.diagnostics.empty()
      ? required_right
      : authorized.diagnostics.front().detail;
  return ManagementDiagnostic(
      code,
      "The engine denied the server management request.",
      {{"required_right", required_right},
       {"engine_required_right", engine_right},
       {"operation_key", operation_key},
       {"detail", detail},
       {"authorization_authority", "engine"}});
}

std::optional<ServerSessionRecord> FindSession(const ServerSessionRegistry& registry,
                                               const std::array<std::uint8_t, 16>& uuid) {
  if (sbps::IsZeroUuid(uuid)) return std::nullopt;
  const auto found = registry.sessions_by_uuid.find(UuidBytesToText(uuid));
  if (found == registry.sessions_by_uuid.end()) return std::nullopt;
  return found->second;
}

std::string RedactedPathRecord(const std::string& value) {
  return value.empty() ? "" : "[path-redacted]";
}

std::string ServerStatusJson(const ServerManagementContext& context,
                             const std::string& operation_key,
                             const std::string& outcome,
                             const std::string& records_json,
                             const std::string& state_before = {},
                             const std::string& state_after = {}) {
  const auto generation = context.listener_orchestrator == nullptr ? 1 : context.listener_orchestrator->generation;
  std::ostringstream out;
  out << "{\"server_management_response\":{\"operation_key\":\"" << JsonEscape(operation_key)
      << "\",\"outcome\":\"" << JsonEscape(outcome)
      << "\",\"state_generation\":" << generation
      << ",\"state_before\":\"" << JsonEscape(state_before)
      << "\",\"state_after\":\"" << JsonEscape(state_after)
      << "\",\"authorization_authority\":\"engine"
      << "\",\"audit_marker\":\"DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE"
      << "\",\"metrics_updated\":true,\"records\":" << records_json << "}}\n";
  return out.str();
}

std::string HostedDatabasesRecordsJson(const HostedEngineState& state) {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < state.databases.size(); ++i) {
    if (i != 0) out << ',';
    const auto& database = state.databases[i];
    out << "{\"state\":\"" << HostedDatabaseStateName(database.state)
        << "\",\"database_uuid\":\"" << JsonEscape(database.database_uuid)
        << "\",\"database_ref\":\"" << JsonEscape(RedactedPathRecord(database.database_path))
        << "\",\"database_open\":" << (database.database_open ? "true" : "false")
        << ",\"read_only\":" << (database.read_only ? "true" : "false")
        << ",\"write_admission_fenced\":" << (database.write_admission_fenced ? "true" : "false")
        << "}";
  }
  out << "]";
  return out.str();
}

std::string EndpointRecordsJson(const ServerBootstrapConfig& config,
                                const ServerListenerOrchestrator& listeners) {
  std::ostringstream out;
  out << "[{\"endpoint_family\":\"parser_server_ipc\",\"transport\":\"af_unix\","
      << "\"endpoint\":\"" << JsonEscape(RedactedPathRecord(config.sbps_endpoint.string()))
      << "\",\"enabled\":" << (config.sbps_enabled ? "true" : "false") << "},"
      << "{\"endpoint_family\":\"listener_engine_endpoint\",\"transport\":\"af_unix\","
      << "\"endpoint\":\"" << JsonEscape(RedactedPathRecord(listeners.engine_endpoint))
      << "\",\"enabled\":true}]";
  return out.str();
}

std::string ListenerRecordsJson(const ServerListenerOrchestrator& listeners) {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < listeners.profiles.size(); ++i) {
    if (i != 0) out << ',';
    const auto& profile = listeners.profiles[i];
    out << "{\"listener_uuid\":\"" << JsonEscape(profile.listener_uuid)
        << "\",\"profile_name\":\"" << JsonEscape(profile.profile_name)
        << "\",\"state\":\"" << JsonEscape(profile.state)
        << "\",\"bind_host\":\"" << JsonEscape(profile.bind_host)
        << "\",\"port\":" << profile.port
        << ",\"engine_endpoint\":\"" << JsonEscape(profile.engine_endpoint)
        << "\",\"parser_package_ref\":\"" << JsonEscape(profile.parser_package_ref)
        << "\",\"last_transition\":\"" << JsonEscape(profile.last_transition) << "\"}";
  }
  out << "]";
  return out.str();
}

bool ContainsToken(std::string_view value, std::string_view token) {
  return value.find(token) != std::string_view::npos;
}

std::string ModeValue(const std::string& mode, const std::string& key) {
  const std::string prefix = key + ":";
  const std::string alt_prefix = key + "=";
  std::size_t start = 0;
  while (start <= mode.size()) {
    const auto end = mode.find_first_of(";,\n", start);
    const auto token = mode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (token.rfind(prefix, 0) == 0) return token.substr(prefix.size());
    if (token.rfind(alt_prefix, 0) == 0) return token.substr(alt_prefix.size());
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}

std::uint64_t ModeU64(const std::string& mode,
                      const std::string& key,
                      std::uint64_t fallback) {
  const auto text = ModeValue(mode, key);
  if (text.empty()) return fallback;
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

bool ModeBool(const std::string& mode, const std::string& key, bool fallback = false) {
  const auto text = ModeValue(mode, key);
  if (text == "true" || text == "1" || text == "yes" || text == "on") return true;
  if (text == "false" || text == "0" || text == "no" || text == "off") return false;
  return fallback || ContainsToken(mode, key + ":true") || ContainsToken(mode, key + "=true");
}

bool IsDatabaseShutdownOperation(const std::string& operation_key) {
  return operation_key == "shutdown_database" ||
         operation_key == "shutdown_database_force" ||
         operation_key == "force_shutdown_database";
}

bool IsEngineLifecycleManagementOperation(const std::string& operation_key) {
  return operation_key == "create_database" ||
         operation_key == "open_database" ||
         operation_key == "attach_database" ||
         operation_key == "detach_database";
}

bool IsLifecycleObservabilityOperation(const std::string& operation_key) {
  return IsEngineLifecycleManagementOperation(operation_key) ||
         operation_key == "enter_database_maintenance" ||
         operation_key == "exit_database_maintenance" ||
         operation_key == "enter_restricted_open" ||
         operation_key == "exit_restricted_open" ||
         operation_key == "inspect_database" ||
         operation_key == "diagnose_database" ||
         operation_key == "verify_database" ||
         operation_key == "repair_database" ||
         operation_key == "drop_database" ||
         IsDatabaseShutdownOperation(operation_key) ||
         operation_key == "ack_database_shutdown" ||
         operation_key == "show_database_shutdown_state" ||
         operation_key == "show_server_metrics" ||
         operation_key == "export_server_support_bundle" ||
         operation_key == "show_parser_packages" ||
         operation_key == "show_parser_channels" ||
         operation_key == "show_server_sessions";
}

std::string RouteFamilyForOperation(const std::string& operation_key) {
  if (operation_key.find("parser") != std::string::npos) return "parser";
  if (operation_key.find("session") != std::string::npos ||
      operation_key.find("ipc") != std::string::npos) {
    return "ipc_session";
  }
  if (operation_key == "show_server_metrics") return "observability";
  return "server_management";
}

std::string FirstDiagnosticCode(const std::vector<ServerDiagnostic>& diagnostics) {
  return diagnostics.empty() ? "" : diagnostics.front().code;
}

void RecordManagementLifecycleObservability(const ServerManagementContext& context,
                                            const sbps::Frame& frame,
                                            const std::string& operation_key,
                                            const std::string& outcome,
                                            const std::string& diagnostic_code,
                                            const std::string& state_before,
                                            const std::string& state_after,
                                            const std::string& database_uuid,
                                            const std::string& private_detail = {}) {
  if (context.observability == nullptr) return;
  ServerLifecycleObservabilityEvent event;
  event.operation_key = operation_key;
  event.outcome = outcome;
  event.diagnostic_code = diagnostic_code;
  event.route_family = RouteFamilyForOperation(operation_key);
  event.request_uuid = UuidBytesToText(frame.header.request_uuid);
  event.session_uuid = UuidBytesToText(frame.header.session_uuid);
  event.database_uuid = database_uuid;
  event.state_before = state_before;
  event.state_after = state_after;
  event.private_detail = private_detail;
  event.cache_invalidation_required =
      outcome != "refused" && outcome != "failed" &&
      outcome.find("failed") == std::string::npos &&
      outcome.find("refused") == std::string::npos &&
      LifecycleOperationRequiresCacheInvalidation(operation_key);
  event.cache_family = "lifecycle_metadata";
  event.cache_reason = operation_key + ":" + outcome;
  RecordServerLifecycleObservability(context.observability, std::move(event));
}

bool SessionMatchesDatabase(const ServerSessionRecord& session,
                            const std::string& database_path,
                            const std::string& database_uuid) {
  if (!database_uuid.empty() && session.database_uuid == database_uuid) return true;
  if (!database_path.empty() && session.database_path == database_path) return true;
  return false;
}

std::string NormalizeDatabaseSelector(std::string selector) {
  constexpr std::string_view kServerDatabasePath = "server_database_path:";
  constexpr std::string_view kDevBootstrapPath = "dev_bootstrap_path:";
  constexpr std::string_view kDatabaseUuid = "database_uuid:";
  if (std::string_view(selector).starts_with(kServerDatabasePath)) {
    selector.erase(0, kServerDatabasePath.size());
  } else if (std::string_view(selector).starts_with(kDevBootstrapPath)) {
    selector.erase(0, kDevBootstrapPath.size());
  } else if (std::string_view(selector).starts_with(kDatabaseUuid)) {
    selector.erase(0, kDatabaseUuid.size());
  }
  return selector;
}

bool ListenerMatchesDatabase(const ServerListenerProfileRuntime& profile,
                             const std::string& database_path,
                             const std::string& database_uuid,
                             bool single_hosted_database) {
  const auto selector = NormalizeDatabaseSelector(profile.database_selector);
  if (!selector.empty() &&
      selector != "default" &&
      selector != database_path &&
      selector != database_uuid) {
    return false;
  }
  if (selector.empty() || selector == "default") {
    return single_hosted_database;
  }
  return true;
}

ProcessAssociationRegistry BuildProcessAssociationRegistry(
    const ServerManagementContext& context,
    const ServerShutdownRuntimeSnapshot& snapshot,
    bool single_hosted_database) {
  ProcessAssociationRegistry registry;
  registry.generation = context.artifacts == nullptr || context.artifacts->generation == 0
      ? 1
      : context.artifacts->generation;
  if (snapshot.database_uuid.empty() && snapshot.database_path.empty()) return registry;

  ProcessAssociationRecord server_process;
  server_process.kind = ProcessAssociationKind::kServerProcess;
  server_process.database_uuid = snapshot.database_uuid;
  server_process.database_path = snapshot.database_path;
  server_process.component_uuid = "sb_server:" + std::to_string(registry.generation);
  server_process.process_uuid = server_process.component_uuid;
  server_process.lifecycle_generation = registry.generation;
  server_process.policy_generation = registry.generation;
  server_process.state = "associated";
  RegisterProcessAssociation(&registry, std::move(server_process));

  ProcessAssociationRecord manager;
  manager.kind = ProcessAssociationKind::kManager;
  manager.database_uuid = snapshot.database_uuid;
  manager.database_path = snapshot.database_path;
  manager.manager_uuid = "sb_server_manager:" + std::to_string(registry.generation);
  manager.component_uuid = manager.manager_uuid;
  manager.process_uuid = manager.manager_uuid;
  manager.lifecycle_generation = registry.generation;
  manager.policy_generation =
      context.maintenance_coordinator == nullptr ? registry.generation
                                                 : context.maintenance_coordinator->generation;
  manager.state = context.maintenance_coordinator == nullptr
      ? "unavailable"
      : context.maintenance_coordinator->state;
  manager.healthy = context.maintenance_coordinator != nullptr;
  RegisterProcessAssociation(&registry, std::move(manager));

  if (context.config != nullptr && context.config->sbps_enabled) {
    ProcessAssociationRecord ipc;
    ipc.kind = ProcessAssociationKind::kIpcEndpoint;
    ipc.database_uuid = snapshot.database_uuid;
    ipc.database_path = snapshot.database_path;
    ipc.ipc_endpoint = context.config->sbps_endpoint.empty()
        ? "parser_server_ipc"
        : context.config->sbps_endpoint.string();
    ipc.component_uuid = ipc.ipc_endpoint;
    ipc.process_uuid = "sb_server_ipc:" + ipc.ipc_endpoint;
    ipc.lifecycle_generation = registry.generation;
    ipc.policy_generation = registry.generation;
    ipc.state = "listening";
    RegisterProcessAssociation(&registry, std::move(ipc));
  }

  if (context.listener_orchestrator != nullptr) {
    for (const auto& profile : context.listener_orchestrator->profiles) {
      if (!ListenerMatchesDatabase(profile,
                                   snapshot.database_path,
                                   snapshot.database_uuid,
                                   single_hosted_database)) {
        continue;
      }
      ProcessAssociationRecord listener;
      listener.kind = ProcessAssociationKind::kListener;
      listener.database_uuid = snapshot.database_uuid;
      listener.database_path = snapshot.database_path;
      listener.listener_uuid = profile.listener_uuid;
      listener.component_uuid = profile.listener_uuid;
      listener.process_uuid = profile.listener_uuid;
      listener.pid = profile.pid;
      listener.ipc_endpoint = profile.engine_endpoint;
      listener.lifecycle_generation = registry.generation;
      listener.association_generation = context.listener_orchestrator->generation;
      listener.heartbeat_generation = context.listener_orchestrator->generation;
      listener.policy_generation = registry.generation;
      listener.state = profile.state;
      listener.healthy = profile.state != "failed" &&
                         profile.diagnostic_code.rfind("LISTENER.", 0) != 0;
      RegisterProcessAssociation(&registry, std::move(listener));

      if (!profile.parser_package_ref.empty()) {
        ProcessAssociationRecord parser;
        parser.kind = ProcessAssociationKind::kParser;
        parser.database_uuid = snapshot.database_uuid;
        parser.database_path = snapshot.database_path;
        parser.listener_uuid = profile.listener_uuid;
        parser.parser_instance_uuid = profile.listener_uuid + ":parser_pool:" +
                                      profile.parser_package_ref;
        parser.component_uuid = parser.parser_instance_uuid;
        parser.process_uuid = parser.parser_instance_uuid;
        parser.pid = profile.pid;
        parser.ipc_endpoint = profile.engine_endpoint;
        parser.lifecycle_generation = registry.generation;
        parser.association_generation = context.listener_orchestrator->generation;
        parser.heartbeat_generation = context.listener_orchestrator->generation;
        parser.policy_generation = registry.generation;
        parser.state = profile.state == "failed" ? "listener_failed_fallback_available" : "associated";
        parser.healthy = true;
        RegisterProcessAssociation(&registry, std::move(parser));
      }
    }
  }

  if (context.session_registry != nullptr) {
    for (const auto& [_, session] : context.session_registry->sessions_by_uuid) {
      if (!SessionMatchesDatabase(session, snapshot.database_path, snapshot.database_uuid)) continue;
      ProcessAssociationRecord session_record;
      session_record.kind = ProcessAssociationKind::kSession;
      session_record.database_uuid = session.database_uuid.empty()
          ? snapshot.database_uuid
          : session.database_uuid;
      session_record.database_path = session.database_path.empty()
          ? snapshot.database_path
          : session.database_path;
      session_record.session_uuid = UuidBytesToText(session.session_uuid);
      session_record.attachment_uuid = session_record.session_uuid;
      session_record.route_uuid = UuidBytesToText(session.connection_uuid);
      session_record.component_uuid = session_record.session_uuid;
      session_record.process_uuid = session_record.route_uuid;
      session_record.lifecycle_generation = registry.generation;
      session_record.association_generation = session.policy_generation;
      session_record.heartbeat_generation = session.policy_generation;
      session_record.policy_generation = session.policy_generation;
      session_record.active_local_transaction_id = session.local_transaction_id;
      session_record.state = session.local_transaction_id == 0 ? "ready" : "transaction_active";
      RegisterProcessAssociation(&registry, std::move(session_record));
    }
  }

  return registry;
}

const HostedDatabaseSnapshot* TargetDatabase(const ServerManagementContext& context,
                                             const ServerManagementRequest& request,
                                             std::uint64_t* open_database_count) {
  if (open_database_count != nullptr) *open_database_count = 0;
  if (context.engine_state == nullptr) return nullptr;
  const HostedDatabaseSnapshot* first_open = nullptr;
  const HostedDatabaseSnapshot* matched = nullptr;
  for (const auto& database : context.engine_state->databases) {
    if (!database.database_open) continue;
    if (open_database_count != nullptr) ++(*open_database_count);
    if (first_open == nullptr) first_open = &database;
    if (!request.target_uuid.empty() && request.target_uuid == database.database_uuid) {
      matched = &database;
    }
    if (context.config != nullptr &&
        !context.config->database_default_path.empty() &&
        database.database_path == context.config->database_default_path.string()) {
      matched = &database;
    }
  }
  return matched == nullptr ? first_open : matched;
}

ServerShutdownRuntimeSnapshot BuildShutdownRuntimeSnapshot(
    const ServerManagementContext& context,
    const ServerManagementRequest& request) {
  ServerShutdownRuntimeSnapshot snapshot;
  std::uint64_t open_database_count = 0;
  const auto* database = TargetDatabase(context, request, &open_database_count);
  if (database != nullptr) {
    snapshot.database_path = database->database_path;
    snapshot.database_uuid = database->database_uuid;
  } else if (context.config != nullptr && !context.config->database_default_path.empty()) {
    snapshot.database_path = context.config->database_default_path.string();
  }
  const bool single_hosted_database = open_database_count <= 1;
  const bool config_path_proves_target =
      context.config != nullptr &&
      !context.config->database_default_path.empty() &&
      database != nullptr &&
      database->database_path == context.config->database_default_path.string();
  snapshot.association_scope_proven =
      database != nullptr &&
      database->database_open &&
      !snapshot.database_path.empty() &&
      (single_hosted_database || !request.target_uuid.empty() || config_path_proves_target);

  if (context.session_registry != nullptr) {
    for (const auto& [_, session] : context.session_registry->sessions_by_uuid) {
      if (!SessionMatchesDatabase(session, snapshot.database_path, snapshot.database_uuid)) continue;
      ++snapshot.associated_session_count;
      ++snapshot.associated_client_count;
      if (session.local_transaction_id != 0) ++snapshot.active_transaction_session_count;
    }
  }
  if (context.listener_orchestrator != nullptr) {
    for (const auto& profile : context.listener_orchestrator->profiles) {
      if (!ListenerMatchesDatabase(profile,
                                   snapshot.database_path,
                                   snapshot.database_uuid,
                                   single_hosted_database)) {
        continue;
      }
      ++snapshot.associated_listener_count;
      if (!profile.parser_package_ref.empty()) ++snapshot.associated_parser_count;
      if (profile.state == "failed" || profile.diagnostic_code.rfind("LISTENER.", 0) == 0) {
        snapshot.listener_unavailable = true;
      }
    }
  }
  if (context.config != nullptr && context.config->sbps_enabled) {
    snapshot.associated_ipc_endpoint_count = 1;
  }

  const auto association_registry =
      BuildProcessAssociationRegistry(context, snapshot, single_hosted_database);
  const bool parser_fallback_requested =
      ModeBool(request.mode, "parser_fallback_required", false);
  auto association_result = ApplyProcessAssociationScopeToShutdownSnapshot(
      association_registry,
      snapshot.database_uuid,
      snapshot.database_path,
      0,
      parser_fallback_requested,
      &snapshot);
  const bool parser_fallback_required =
      parser_fallback_requested || association_result.listener_unavailable;
  if (parser_fallback_required != parser_fallback_requested) {
    association_result = ApplyProcessAssociationScopeToShutdownSnapshot(
        association_registry,
        snapshot.database_uuid,
        snapshot.database_path,
        0,
        parser_fallback_required,
        &snapshot);
  }

  snapshot.required_acknowledgement_count =
      ModeU64(request.mode, "required_acknowledgement_count", snapshot.required_acknowledgement_count);
  snapshot.acknowledged_component_count =
      ModeU64(request.mode, "acknowledged_component_count", snapshot.acknowledged_component_count);
  snapshot.drain_complete =
      ModeBool(request.mode,
               "drain_complete",
               snapshot.active_transaction_session_count == 0);
  if (ModeBool(request.mode, "acknowledgements_satisfied", false) ||
      ModeValue(request.mode, "acknowledgement_policy") == "satisfied") {
    const auto required = snapshot.required_acknowledgement_count == 0
        ? snapshot.associated_manager_count + snapshot.associated_listener_count +
              snapshot.associated_parser_count + snapshot.associated_ipc_endpoint_count +
              snapshot.associated_session_count
        : snapshot.required_acknowledgement_count;
    snapshot.acknowledged_component_count = required;
  }
  return snapshot;
}

std::vector<std::uint8_t> DisconnectPayload(const std::array<std::uint8_t, 16>& session_uuid,
                                            std::string_view reason) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), session_uuid.begin(), session_uuid.end());
  PutString(&out, std::string(reason));
  return out;
}

void ApplyDatabaseShutdownRuntimeActions(const ServerManagementContext& context,
                                         const ServerShutdownRuntimeSnapshot& snapshot,
                                         bool force) {
  if (context.listener_orchestrator != nullptr) {
    const bool single_hosted_database =
        context.engine_state == nullptr || context.engine_state->databases.size() <= 1;
    for (auto& profile : context.listener_orchestrator->profiles) {
      if (!ListenerMatchesDatabase(profile,
                                   snapshot.database_path,
                                   snapshot.database_uuid,
                                   single_hosted_database)) {
        continue;
      }
      if (profile.pid > 0 && profile.state != "failed") {
        (void)ApplyListenerOperation(context.listener_orchestrator,
                                     *context.config,
                                     *context.artifacts,
                                     "stop_listener",
                                     profile.listener_uuid,
                                     force ? "force" : "graceful");
      } else {
        profile.enabled = false;
        if (profile.state != "failed") profile.state = "stopped";
        profile.last_transition =
            force ? "database_shutdown_force" : "database_shutdown_graceful";
      }
    }
    ++context.listener_orchestrator->generation;
  }

  if (context.session_registry == nullptr) return;
  std::vector<std::array<std::uint8_t, 16>> session_uuids;
  for (const auto& [_, session] : context.session_registry->sessions_by_uuid) {
    if (SessionMatchesDatabase(session, snapshot.database_path, snapshot.database_uuid)) {
      session_uuids.push_back(session.session_uuid);
    }
  }
  for (const auto& session_uuid : session_uuids) {
    sbps::Frame disconnect;
    disconnect.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
    disconnect.header.request_uuid = sbps::MakeUuidV7Bytes();
    disconnect.header.session_uuid = session_uuid;
    disconnect.payload =
        DisconnectPayload(session_uuid, force ? "parser_killed" : "shutdown_drain_complete");
    (void)HandleDisconnectNotice(context.session_registry, disconnect);
  }
}

ServerShutdownRuntimeSnapshot BuildDropRuntimeSnapshot(const ServerManagementContext& context,
                                                       const ServerManagementRequest& request) {
  return BuildShutdownRuntimeSnapshot(context, request);
}

std::optional<ServerDiagnostic> DropPreflightDiagnostic(
    const ServerShutdownRuntimeSnapshot& snapshot,
    const std::string& mode) {
  if (!snapshot.association_scope_proven) {
    return ManagementDiagnostic("ENGINE.DBLC_DROP_UNSAFE",
                                "Drop database requires exact target database association scope.",
                                {{"reason", "drop_association_scope_not_proven"},
                                 {"database_uuid", snapshot.database_uuid}});
  }
  if (snapshot.active_transaction_session_count != 0) {
    return ManagementDiagnostic("ENGINE.DBLC_DROP_UNSAFE",
                                "Drop database refused while active transactions are associated with the target database.",
                                {{"reason", "drop_active_transaction_sessions"},
                                 {"active_transaction_session_count",
                                  std::to_string(snapshot.active_transaction_session_count)}});
  }
  if (!ModeBool(mode, "retention_policy_satisfied", false) ||
      !ModeBool(mode, "backup_coverage_verified", false) ||
      !ModeBool(mode, "legal_hold_clear", false)) {
    return ManagementDiagnostic("ENGINE.DBLC_DROP_UNSAFE",
                                "Drop database requires retention, backup, and legal-hold policy proof.",
                                {{"reason", "drop_policy_proof_missing"}});
  }
  return std::nullopt;
}

void ApplyDatabaseDropRuntimeActions(const ServerManagementContext& context,
                                     const ServerShutdownRuntimeSnapshot& snapshot) {
  ApplyDatabaseShutdownRuntimeActions(context, snapshot, false);
}

std::string SessionRecordsJson(const ServerSessionRegistry& registry) {
  std::ostringstream out;
  out << "[";
  bool first = true;
  for (const auto& [_, session] : registry.sessions_by_uuid) {
    if (!first) out << ',';
    first = false;
    out << "{\"session_uuid\":\"" << UuidBytesToText(session.session_uuid)
        << "\",\"principal_present\":" << (!session.principal_claim.empty() ? "true" : "false")
        << ",\"database_uuid\":\"" << JsonEscape(session.database_uuid)
        << "\",\"attach_mode\":\"" << JsonEscape(session.attach_mode) << "\"}";
  }
  out << "]";
  return out.str();
}

std::string ParserPackageRecordsJson(const ParserPackageRegistry& registry) {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < registry.entries.size(); ++i) {
    if (i != 0) out << ',';
    const auto& entry = registry.entries[i];
    out << "{\"parser_package_uuid\":\"" << JsonEscape(entry.parser_package_uuid)
        << "\",\"parser_family_uuid\":\"" << JsonEscape(entry.parser_family_uuid)
        << "\",\"dialect_profile_uuid\":\"" << JsonEscape(entry.dialect_profile_uuid)
        << "\",\"state\":\"" << JsonEscape(entry.state)
        << "\",\"parser_support_udr_required\":"
        << (entry.parser_support_udr_required ? "true" : "false") << "}";
  }
  out << "]";
  return out.str();
}

ServerManagementResponse ErrorResponse(const sbps::Frame& request,
                                       std::vector<ServerDiagnostic> diagnostics);

std::vector<std::string> ModeTokensForEngineOptions(const std::string& mode) {
  std::vector<std::string> tokens;
  std::size_t start = 0;
  while (start <= mode.size()) {
    const auto end = mode.find_first_of(";,\n", start);
    auto token = mode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!token.empty()) tokens.push_back(std::move(token));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return tokens;
}

template <typename TRequest>
TRequest EngineLifecycleRequestForManagement(const ServerManagementContext& context,
                                             const ServerSessionRecord& session,
                                             const sbps::Frame& frame,
                                             const ServerManagementRequest& request) {
  TRequest engine_request;
  engine_request.context = EngineContextForManagement(context, session, frame);
  if (engine_request.context.database_path.empty() &&
      context.config != nullptr &&
      !context.config->database_default_path.empty()) {
    engine_request.context.database_path = context.config->database_default_path.string();
  }
  if (!request.target_uuid.empty()) {
    engine_request.context.database_uuid.canonical = request.target_uuid;
  }
  if (engine_request.context.database_uuid.canonical.empty()) {
    engine_request.context.database_uuid.canonical = session.database_uuid;
  }
  engine_request.target_database.uuid.canonical = engine_request.context.database_uuid.canonical;
  engine_request.target_database.object_kind = "database";
  engine_request.target_object.uuid.canonical = engine_request.context.database_uuid.canonical;
  engine_request.target_object.object_kind = "database_lifecycle";
  engine_request.option_envelopes.push_back("operation_key:" + request.operation_key);
  engine_request.option_envelopes.push_back("admin_cli_route:true");
  engine_request.option_envelopes.push_back("audit_reason:" + request.audit_reason);
  for (const auto& token : ModeTokensForEngineOptions(request.mode)) {
    engine_request.option_envelopes.push_back(token);
  }
  return engine_request;
}

ServerDiagnostic EngineApiDiagnosticToManagement(const engine_api::EngineApiDiagnostic& diagnostic,
                                                 const std::string& operation_key) {
  return ManagementDiagnostic(
      diagnostic.code.empty() ? "SERVER.MANAGEMENT.ENGINE_LIFECYCLE_FAILED" : diagnostic.code,
      diagnostic.detail.empty() ? "The engine lifecycle API refused the management request."
                                : diagnostic.detail,
      {{"operation_key", operation_key},
       {"message_key", diagnostic.message_key},
       {"authorization_authority", "engine"}});
}

std::string EngineLifecycleResultRecordsJson(const engine_api::EngineApiResult& result,
                                             const std::string& operation_key) {
  std::ostringstream out;
  out << "[";
  bool first_record = true;
  for (const auto& row : result.result_shape.rows) {
    if (!first_record) out << ',';
    first_record = false;
    out << "{\"operation_key\":\"" << JsonEscape(operation_key) << "\"";
    for (const auto& field : row.fields) {
      out << ",\"" << JsonEscape(field.first) << "\":\""
          << JsonEscape(field.second.encoded_value) << "\"";
    }
    out << ",\"authorization_authority\":\"engine\""
        << ",\"audit_marker\":\"DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE\"}";
  }
  if (first_record) {
    out << "{\"operation_key\":\"" << JsonEscape(operation_key)
        << "\",\"operation_id\":\"" << JsonEscape(result.operation_id)
        << "\",\"primary_object_uuid\":\"" << JsonEscape(result.primary_object.uuid.canonical)
        << "\",\"primary_object_kind\":\"" << JsonEscape(result.primary_object.object_kind)
        << "\",\"authorization_authority\":\"engine\""
        << ",\"audit_marker\":\"DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE\"}";
  }
  out << "]";
  return out.str();
}

ServerManagementResponse HandleEngineLifecycleManagementOperation(
    const ServerManagementContext& context,
    const ServerSessionRecord& session,
    const sbps::Frame& frame,
    const ServerManagementRequest& request,
    std::string* records,
    std::string* outcome,
    std::string* state_after) {
  engine_api::EngineApiResult lifecycle_result;
  if (request.operation_key == "create_database") {
    lifecycle_result = engine_api::EngineCreateLifecycle(
        EngineLifecycleRequestForManagement<engine_api::EngineCreateLifecycleRequest>(
            context, session, frame, request));
  } else if (request.operation_key == "open_database") {
    lifecycle_result = engine_api::EngineOpenLifecycle(
        EngineLifecycleRequestForManagement<engine_api::EngineOpenLifecycleRequest>(
            context, session, frame, request));
  } else if (request.operation_key == "attach_database") {
    lifecycle_result = engine_api::EngineAttachLifecycle(
        EngineLifecycleRequestForManagement<engine_api::EngineAttachLifecycleRequest>(
            context, session, frame, request));
  } else {
    lifecycle_result = engine_api::EngineDetachLifecycle(
        EngineLifecycleRequestForManagement<engine_api::EngineDetachLifecycleRequest>(
            context, session, frame, request));
  }
  if (!lifecycle_result.ok) {
    std::vector<ServerDiagnostic> diagnostics;
    for (const auto& diagnostic : lifecycle_result.diagnostics) {
      diagnostics.push_back(EngineApiDiagnosticToManagement(diagnostic, request.operation_key));
    }
    if (diagnostics.empty()) {
      diagnostics.push_back(ManagementDiagnostic(
          "SERVER.MANAGEMENT.ENGINE_LIFECYCLE_FAILED",
          "The engine lifecycle API refused the management request.",
          {{"operation_key", request.operation_key}, {"authorization_authority", "engine"}}));
    }
    return ErrorResponse(frame, diagnostics);
  }
  *records = EngineLifecycleResultRecordsJson(lifecycle_result, request.operation_key);
  *outcome = request.operation_key == "create_database" ? "created" :
             request.operation_key == "open_database" ? "opened" :
             request.operation_key == "attach_database" ? "attached" :
             "detached";
  *state_after = *outcome;
  return {};
}

ServerManagementResponse ErrorResponse(const sbps::Frame& request,
                                       std::vector<ServerDiagnostic> diagnostics) {
  ServerManagementResponse response;
  response.error = true;
  response.session_uuid = request.header.session_uuid;
  response.diagnostics = std::move(diagnostics);
  response.payload = sbps::EncodeMessageVectorSet(response.diagnostics, request.header.request_uuid);
  response.response_schema_id = sbps::kSchemaMessageVectorSetV1;
  return response;
}

}  // namespace

std::vector<std::uint8_t> EncodeServerManagementRequestForTest(
    const ServerManagementRequest& request) {
  std::vector<std::pair<std::string, std::string>> fields{
      {"operation_key", request.operation_key},
      {"target_uuid", request.target_uuid},
      {"mode", request.mode},
      {"audit_reason", request.audit_reason},
      {"timeout_ms", std::to_string(request.timeout_ms)},
      {"include_history", request.include_history ? "true" : "false"},
  };
  std::vector<std::uint8_t> out;
  PutU16(&out, static_cast<std::uint16_t>(fields.size()));
  for (const auto& [key, value] : fields) {
    PutString(&out, key);
    PutString(&out, value);
  }
  return out;
}

std::optional<ServerManagementRequest> DecodeServerManagementRequest(
    const std::vector<std::uint8_t>& payload) {
  const auto fields = DecodeFields(payload);
  if (!fields.contains("operation_key")) return std::nullopt;
  ServerManagementRequest request;
  request.operation_key = CanonicalManagementOperationKey(fields.at("operation_key"));
  if (fields.contains("target_uuid")) request.target_uuid = fields.at("target_uuid");
  if (fields.contains("mode")) request.mode = fields.at("mode");
  if (fields.contains("audit_reason")) request.audit_reason = fields.at("audit_reason");
  if (fields.contains("include_history")) request.include_history = fields.at("include_history") == "true";
  if (fields.contains("timeout_ms")) {
    try {
      request.timeout_ms = static_cast<std::uint64_t>(std::stoull(fields.at("timeout_ms")));
    } catch (...) {
      request.timeout_ms = 30000;
    }
  }
  return request;
}

ServerManagementResponse HandleServerManagementRequest(const ServerManagementContext& context,
                                                       const sbps::Frame& frame) {
  const auto decoded = DecodeServerManagementRequest(frame.payload);
  if (!decoded) {
    RecordManagementLifecycleObservability(context,
                                           frame,
                                           "ipc_session_route",
                                           "refused",
                                           "SERVER.MANAGEMENT.REQUEST_INVALID",
                                           {},
                                           {},
                                           {},
                                           "malformed management request payload");
    return ErrorResponse(frame,
                         {ManagementDiagnostic("SERVER.MANAGEMENT.REQUEST_INVALID",
                                               "The server management request payload is malformed.")});
  }
  const auto session = context.session_registry == nullptr
                           ? std::optional<ServerSessionRecord>{}
                           : FindSession(*context.session_registry, frame.header.session_uuid);
  if (!session) {
    RecordManagementLifecycleObservability(context,
                                           frame,
                                           decoded->operation_key,
                                           "refused",
                                           "PARSER_SERVER_IPC.SESSION_NOT_BOUND",
                                           {},
                                           {},
                                           {},
                                           "management route requires a bound session");
    return ErrorResponse(frame,
                         {ManagementDiagnostic("PARSER_SERVER_IPC.SESSION_NOT_BOUND",
                                               "Server management requests require a bound server session.")});
  }
  const auto required_right = RequiredRightForOperation(decoded->operation_key);
  if (!HasRequiredRight(*session, required_right)) {
    RecordManagementLifecycleObservability(context,
                                           frame,
                                           decoded->operation_key,
                                           "refused",
                                           "SECURITY.ACCESS_DENIED",
                                           {},
                                           {},
                                           session->database_uuid,
                                           "server-side right precheck denied");
    return ErrorResponse(frame,
                         {ManagementDiagnostic("SECURITY.ACCESS_DENIED",
                                               "The session does not hold the required management right.",
                                               {{"required_right", required_right},
                                                {"operation_key", decoded->operation_key}})});
  }
  if (auto denial = EngineAuthorizeManagement(context,
                                              *session,
                                              frame,
                                              required_right,
                                              decoded->operation_key)) {
    RecordManagementLifecycleObservability(context,
                                           frame,
                                           decoded->operation_key,
                                           "refused",
                                           denial->code,
                                           {},
                                           {},
                                           session->database_uuid,
                                           "engine authorization denied management route");
    return ErrorResponse(frame, {*denial});
  }

  ServerManagementResponse response;
  response.accepted = true;
  response.session_uuid = frame.header.session_uuid;
  std::string records = "[]";
  std::string state_before;
  std::string state_after;
  std::string outcome = "completed";

  if (decoded->operation_key == "show_server_status" ||
      decoded->operation_key == "show_server_health") {
    std::ostringstream out;
    out << "[{\"server_mode\":\"" << ServerModeName(context.config->mode)
        << "\",\"lifecycle_state\":\"" << context.artifacts->state
        << "\",\"health\":\""
        << (context.engine_state->engine_context_active ? "healthy" : "degraded")
        << "\",\"audit_marker\":\"DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE"
        << "\",\"maintenance_state\":\""
        << (context.maintenance_coordinator == nullptr ? "unknown" : context.maintenance_coordinator->state)
        << "\",\"redaction_state\":\"redacted"
        << "\",\"health_detail_scope\":\"least_privilege"
        << "\",\"authorization_authority\":\"engine"
        << "\",\"lifecycle_generation\":" << context.artifacts->generation
        << ",\"engine_context_active\":"
        << (context.engine_state->engine_context_active ? "true" : "false")
        << ",\"listener_count\":" << context.listener_orchestrator->profiles.size()
        << ",\"session_count\":" << context.session_registry->sessions_by_uuid.size() << "}]";
    records = out.str();
  } else if (decoded->operation_key == "status_database" ||
             decoded->operation_key == "health_database") {
    records = HostedDatabasesRecordsJson(*context.engine_state);
    state_after = context.maintenance_coordinator == nullptr
                      ? "running"
                      : context.maintenance_coordinator->state;
  } else if (IsEngineLifecycleManagementOperation(decoded->operation_key)) {
    auto lifecycle_response = HandleEngineLifecycleManagementOperation(context,
                                                                       *session,
                                                                       frame,
                                                                       *decoded,
                                                                       &records,
                                                                       &outcome,
                                                                       &state_after);
    if (lifecycle_response.error) {
      if (context.observability != nullptr) {
        IncrementServerMetric(context.observability,
                              "sys.metrics.server.management.request_total",
                              1,
                              {{"operation", decoded->operation_key}, {"outcome", "refused"}});
        RecordServerAuditEvent(context.observability,
                               "server.management." + decoded->operation_key,
                               "refused",
                               "engine lifecycle management operation refused",
                               lifecycle_response.diagnostics.empty()
                                   ? ""
                                   : lifecycle_response.diagnostics.front().code);
      }
      RecordManagementLifecycleObservability(context,
                                             frame,
                                             decoded->operation_key,
                                             "refused",
                                             FirstDiagnosticCode(lifecycle_response.diagnostics),
                                             state_before,
                                             state_after,
                                             session->database_uuid,
                                             "engine lifecycle management operation refused");
      return lifecycle_response;
    }
  } else if (decoded->operation_key == "show_server_metrics") {
    const auto metrics = context.observability == nullptr
                             ? std::string("{\"server_metrics\":{\"samples\":[]}}")
                             : ServerMetricsSnapshotJson(*context.observability);
    records = "[" + metrics + "]";
  } else if (decoded->operation_key == "show_server_endpoints") {
    records = EndpointRecordsJson(*context.config, *context.listener_orchestrator);
  } else if (decoded->operation_key == "show_server_databases") {
    records = HostedDatabasesRecordsJson(*context.engine_state);
  } else if (decoded->operation_key == "show_server_sessions" ||
             decoded->operation_key == "show_parser_channels") {
    records = SessionRecordsJson(*context.session_registry);
  } else if (decoded->operation_key == "show_finality") {
    records = ServerRequestLifecycleRecordsJson(*context.session_registry,
                                                decoded->target_uuid,
                                                decoded->include_history);
    outcome = "completed";
    state_after = context.maintenance_coordinator == nullptr
                      ? "running"
                      : context.maintenance_coordinator->state;
  } else if (decoded->operation_key == "cancel_request") {
    const auto cancel = CancelServerRequestLifecycle(context.session_registry,
                                                     decoded->target_uuid,
                                                     *session,
                                                     true,
                                                     decoded->timeout_ms);
    if (cancel.error) {
      return ErrorResponse(frame, cancel.diagnostics);
    }
    records = cancel.records_json;
    outcome = cancel.outcome.empty() ? "cancelled" : cancel.outcome;
    state_after = context.maintenance_coordinator == nullptr
                      ? "running"
                      : context.maintenance_coordinator->state;
  } else if (decoded->operation_key == "show_parser_packages") {
    records = ParserPackageRecordsJson(*context.parser_registry);
  } else if (decoded->operation_key == "show_listeners") {
    records = ListenerRecordsJson(*context.listener_orchestrator);
  } else if (decoded->operation_key == "show_server_lifecycle" ||
             decoded->operation_key == "show_maintenance_fences" ||
             decoded->operation_key == "reload_server_config" ||
             decoded->operation_key == "drain_server" ||
             decoded->operation_key == "stop_server" ||
             decoded->operation_key == "restart_server" ||
             decoded->operation_key == "set_server_maintenance_mode" ||
             decoded->operation_key == "clear_server_maintenance_mode" ||
             decoded->operation_key == "enter_database_maintenance" ||
             decoded->operation_key == "exit_database_maintenance" ||
             decoded->operation_key == "enter_restricted_open" ||
             decoded->operation_key == "exit_restricted_open" ||
             decoded->operation_key == "inspect_database" ||
             decoded->operation_key == "diagnose_database" ||
             decoded->operation_key == "verify_database" ||
             decoded->operation_key == "repair_database" ||
             decoded->operation_key == "drop_database" ||
             decoded->operation_key == "shutdown_database" ||
             decoded->operation_key == "shutdown_database_force" ||
             decoded->operation_key == "force_shutdown_database" ||
             decoded->operation_key == "ack_database_shutdown" ||
             decoded->operation_key == "show_database_shutdown_state" ||
             decoded->operation_key == "begin_backup_fence" ||
             decoded->operation_key == "end_backup_fence" ||
             decoded->operation_key == "begin_restore_fence" ||
             decoded->operation_key == "end_restore_fence") {
    ServerMaintenanceOperationRequest maintenance_request;
    maintenance_request.operation_key = decoded->operation_key;
    maintenance_request.target_uuid = decoded->target_uuid;
    maintenance_request.mode = decoded->mode;
    maintenance_request.audit_reason = decoded->audit_reason;
    maintenance_request.timeout_ms = decoded->timeout_ms;
    maintenance_request.request_uuid = frame.header.request_uuid;
    maintenance_request.session_uuid = frame.header.session_uuid;
    const auto shutdown_snapshot = IsDatabaseShutdownOperation(decoded->operation_key)
        ? BuildShutdownRuntimeSnapshot(context, *decoded)
        : ServerShutdownRuntimeSnapshot{};
    const auto drop_snapshot = decoded->operation_key == "drop_database"
        ? BuildDropRuntimeSnapshot(context, *decoded)
        : ServerShutdownRuntimeSnapshot{};
    if (decoded->operation_key == "drop_database") {
      if (auto diagnostic = DropPreflightDiagnostic(drop_snapshot, decoded->mode)) {
        if (context.observability != nullptr) {
          IncrementServerMetric(context.observability,
                                "sys.metrics.server.management.request_total",
                                1,
                                {{"operation", decoded->operation_key}, {"outcome", "refused"}});
          RecordServerAuditEvent(context.observability,
                                 "server.management." + decoded->operation_key,
                                 "refused",
                                 "drop database management operation refused",
                                 diagnostic->code);
        }
        RecordManagementLifecycleObservability(context,
                                               frame,
                                               decoded->operation_key,
                                               "refused",
                                               diagnostic->code,
                                               state_before,
                                               state_after,
                                               drop_snapshot.database_uuid,
                                               "drop preflight refused unsafe lifecycle operation");
        return ErrorResponse(frame, {*diagnostic});
      }
      if (!ContainsToken(maintenance_request.mode, "drop_safety_preconditions:")) {
        if (!maintenance_request.mode.empty()) maintenance_request.mode += ';';
        maintenance_request.mode += "drop_safety_preconditions:true";
      }
      if (!ContainsToken(maintenance_request.mode, "session_drain_complete:")) {
        maintenance_request.mode += ";session_drain_complete:true";
      }
      if (!ContainsToken(maintenance_request.mode, "ownership_release_verified:")) {
        maintenance_request.mode += ";ownership_release_verified:true";
      }
      if (maintenance_request.target_uuid.empty()) {
        maintenance_request.target_uuid = drop_snapshot.database_uuid;
      }
    }
    const auto maintenance = IsDatabaseShutdownOperation(decoded->operation_key)
        ? ApplyDatabaseShutdownOperation(context.maintenance_coordinator,
                                         *context.config,
                                         maintenance_request,
                                         shutdown_snapshot)
        : ApplyServerMaintenanceOperation(context.maintenance_coordinator,
                                          *context.config,
                                          maintenance_request);
    state_before = maintenance.state_before;
    state_after = maintenance.state_after;
    outcome = maintenance.outcome;
    records = maintenance.records_json;
    if (!maintenance.ok) {
      if (context.observability != nullptr) {
        IncrementServerMetric(context.observability,
                              "sys.metrics.server.management.request_total",
                              1,
                              {{"operation", decoded->operation_key}, {"outcome", "refused"}});
        RecordServerAuditEvent(context.observability,
                               "server.management." + decoded->operation_key,
                               "refused",
                               "maintenance management operation refused",
                               maintenance.diagnostics.empty() ? "" : maintenance.diagnostics.front().code);
      }
      RecordManagementLifecycleObservability(context,
                                             frame,
                                             decoded->operation_key,
                                             "refused",
                                             FirstDiagnosticCode(maintenance.diagnostics),
                                             state_before,
                                             state_after,
                                             IsDatabaseShutdownOperation(decoded->operation_key)
                                                 ? shutdown_snapshot.database_uuid
                                                 : session->database_uuid,
                                             "maintenance coordinator refused lifecycle operation");
      return ErrorResponse(frame, maintenance.diagnostics);
    }
    if (IsDatabaseShutdownOperation(decoded->operation_key)) {
      const bool force = decoded->operation_key == "shutdown_database_force" ||
                         decoded->operation_key == "force_shutdown_database" ||
                         decoded->mode == "force" ||
                         ModeValue(decoded->mode, "shutdown_mode") == "force" ||
                         ModeBool(decoded->mode, "force", false);
      ApplyDatabaseShutdownRuntimeActions(context, shutdown_snapshot, force);
    }
    if (decoded->operation_key == "drop_database") {
      ApplyDatabaseDropRuntimeActions(context, drop_snapshot);
    }
    if (context.observability != nullptr) {
      SetServerMetric(context.observability,
                      "sys.metrics.server.lifecycle.state",
                      1,
                      "state",
                      {{"state", state_after.empty() ? "unknown" : state_after},
                       {"mode", ServerModeName(context.config->mode)}});
      SetServerMetric(context.observability,
                      "sys.metrics.server.maintenance.fence_active",
                      context.maintenance_coordinator != nullptr &&
                              (context.maintenance_coordinator->attach_admission_fenced ||
                               context.maintenance_coordinator->write_admission_fenced ||
                               context.maintenance_coordinator->sblr_admission_fenced ||
                               context.maintenance_coordinator->event_admission_fenced)
                          ? 1
                          : 0,
                      "gauge");
    }
  } else if (decoded->operation_key == "validate_server_config") {
    records = std::string("[{\"config_valid\":true,\"config_source\":\"") +
              JsonEscape(context.config->selected_config_source) + "\"}]";
  } else if (decoded->operation_key == "export_server_support_bundle") {
    engine_api::EnginePrepareSupportBundleRequest prepare;
    prepare.context = EngineContextForManagement(context, *session, frame);
    prepare.target_database.uuid.canonical = session->database_uuid;
    prepare.target_database.object_kind = "database";
    prepare.target_object.uuid.canonical = session->database_uuid;
    prepare.target_object.object_kind = "support_bundle";
    prepare.option_envelopes.push_back("engine_authorized_support_export:true");
    const auto prepared = engine_api::EnginePrepareSupportBundle(prepare);
    if (!prepared.ok) {
      if (context.observability != nullptr) {
        IncrementServerMetric(context.observability,
                              "sys.metrics.server.support_bundle.export_total",
                              1,
                              {{"outcome", "refused"}});
        RecordServerAuditEvent(context.observability,
                               "server.support_bundle.export",
                               "refused",
                               "engine refused support bundle export",
                               prepared.diagnostics.empty() ? "" : prepared.diagnostics.front().code);
      }
      RecordManagementLifecycleObservability(context,
                                             frame,
                                             decoded->operation_key,
                                             "refused",
                                             prepared.diagnostics.empty()
                                                 ? "OPS.SUPPORT_BUNDLE.ENGINE_PREPARE_FAILED"
                                                 : prepared.diagnostics.front().code,
                                             state_before,
                                             state_after,
                                             session->database_uuid,
                                             "engine refused support bundle preparation");
      std::vector<ServerDiagnostic> diagnostics;
      for (const auto& diagnostic : prepared.diagnostics) {
        diagnostics.push_back(ManagementDiagnostic(diagnostic.code,
                                                   diagnostic.detail.empty()
                                                       ? "The engine refused support bundle preparation."
                                                       : diagnostic.detail,
                                                   {{"operation_key", decoded->operation_key},
                                                    {"authorization_authority", "engine"}}));
      }
      if (diagnostics.empty()) {
        diagnostics.push_back(ManagementDiagnostic("OPS.SUPPORT_BUNDLE.ENGINE_PREPARE_FAILED",
                                                  "The engine refused support bundle preparation.",
                                                  {{"operation_key", decoded->operation_key},
                                                   {"authorization_authority", "engine"}}));
      }
      return ErrorResponse(frame, diagnostics);
    }
    if (context.observability == nullptr) {
      RecordManagementLifecycleObservability(context,
                                             frame,
                                             decoded->operation_key,
                                             "refused",
                                             "OPS.SUPPORT_BUNDLE.OBSERVABILITY_REQUIRED",
                                             state_before,
                                             state_after,
                                             session->database_uuid,
                                             "server observability state missing");
      return ErrorResponse(frame,
                           {ManagementDiagnostic("OPS.SUPPORT_BUNDLE.OBSERVABILITY_REQUIRED",
                                                 "Support bundle export requires server observability state.")});
    }
    const auto export_result = ExportServerSupportBundle(*context.observability,
                                                         *context.config,
                                                         *context.artifacts,
                                                         *context.engine_state,
                                                         *context.session_registry,
                                                         *context.parser_registry,
                                                         *context.listener_orchestrator);
    if (!export_result.ok) {
      IncrementServerMetric(context.observability,
                            "sys.metrics.server.support_bundle.export_total",
                            1,
                            {{"outcome", "failed"}});
      RecordServerAuditEvent(context.observability,
                             "server.support_bundle.export",
                             "failed",
                             "support bundle export failed",
                             export_result.diagnostic_code);
      RecordManagementLifecycleObservability(context,
                                             frame,
                                             decoded->operation_key,
                                             "failed",
                                             export_result.diagnostic_code,
                                             state_before,
                                             state_after,
                                             session->database_uuid,
                                             "support bundle export failed before visible success");
      return ErrorResponse(frame,
                           {ManagementDiagnostic(export_result.diagnostic_code,
                                                 "Support bundle export failed before visible success.",
                                                 {{"operation_key", decoded->operation_key},
                                                  {"authorization_authority", "engine"}})});
    }
    if (context.observability != nullptr) {
      IncrementServerMetric(context.observability,
                            "sys.metrics.server.support_bundle.export_total",
                            1,
                            {{"outcome", "completed"}});
      RecordServerAuditEvent(context.observability,
                             "server.support_bundle.export",
                             "completed",
                             "support bundle exported");
    }
    records = export_result.records_json;
  } else if (decoded->operation_key == "start_listener" ||
             decoded->operation_key == "stop_listener" ||
             decoded->operation_key == "restart_listener" ||
             decoded->operation_key == "drain_listener" ||
             decoded->operation_key == "listener_proxy_execute") {
    const auto listener = ApplyListenerOperation(context.listener_orchestrator,
                                                *context.config,
                                                *context.artifacts,
                                                decoded->operation_key,
                                                decoded->target_uuid,
                                                decoded->mode);
    state_before = listener.state_before;
    state_after = listener.state_after;
    outcome = listener.outcome;
    if (!listener.ok) {
      if (context.observability != nullptr) {
        IncrementServerMetric(context.observability,
                              "sys.metrics.server.management.request_total",
                              1,
                              {{"operation", decoded->operation_key}, {"outcome", "refused"}});
        RecordServerAuditEvent(context.observability,
                               "server.management." + decoded->operation_key,
                               "refused",
                               "management operation refused",
                               listener.diagnostics.empty() ? "" : listener.diagnostics.front().code);
      }
      return ErrorResponse(frame, listener.diagnostics);
    }
    if (context.observability != nullptr) {
      IncrementServerMetric(context.observability,
                            "sys.metrics.server.management.request_total",
                            1,
                            {{"operation", decoded->operation_key}, {"outcome", "completed"}});
      RecordServerAuditEvent(context.observability,
                             "server.management." + decoded->operation_key,
                             "completed",
                             "management operation completed");
    }
    records = ListenerRecordsJson(*context.listener_orchestrator);
  } else {
    return ErrorResponse(frame,
                         {ManagementDiagnostic("SERVER.MANAGEMENT.OPERATION_UNKNOWN",
                                               "The server management operation key is not supported.",
                                               {{"operation_key", decoded->operation_key}})});
  }

  const auto json = ServerStatusJson(context,
                                     decoded->operation_key,
                                     outcome,
                                     records,
                                     state_before,
                                     state_after);
  if (IsLifecycleObservabilityOperation(decoded->operation_key)) {
    RecordManagementLifecycleObservability(context,
                                           frame,
                                           decoded->operation_key,
                                           outcome,
                                           {},
                                           state_before,
                                           state_after,
                                           session->database_uuid,
                                           "management lifecycle route completed");
  }
  if (context.observability != nullptr) {
    IncrementServerMetric(context.observability,
                          "sys.metrics.server.management.request_total",
                          1,
                          {{"operation", decoded->operation_key}, {"outcome", outcome}});
    RecordServerAuditEvent(context.observability,
                           "server.management." + decoded->operation_key,
                           outcome,
                           "management request completed");
  }
  response.payload.assign(json.begin(), json.end());
  return response;
}

std::string ServerManagementRightsMatrixJson() {
  return "{\"server_management_rights\":["
         "{\"operation\":\"show_server_status\",\"right\":\"OBS_MANAGEMENT_INSPECT\"},"
         "{\"operation\":\"show_server_health\",\"right\":\"OBS_MANAGEMENT_INSPECT\"},"
         "{\"operation\":\"status_database\",\"right\":\"OBS_MANAGEMENT_INSPECT\"},"
         "{\"operation\":\"health_database\",\"right\":\"OBS_MANAGEMENT_INSPECT\"},"
         "{\"operation\":\"create_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"open_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"attach_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"detach_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"show_server_lifecycle\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"reload_server_config\",\"right\":\"OBS_CONFIG_CONTROL\"},"
         "{\"operation\":\"drain_server\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"stop_server\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"restart_server\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"set_server_maintenance_mode\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"clear_server_maintenance_mode\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"enter_database_maintenance\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"exit_database_maintenance\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"enter_restricted_open\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"exit_restricted_open\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"inspect_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"diagnose_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"verify_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"repair_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"drop_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"shutdown_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"shutdown_database_force\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"force_shutdown_database\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"ack_database_shutdown\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"show_database_shutdown_state\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"show_maintenance_fences\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"begin_backup_fence\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"end_backup_fence\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"begin_restore_fence\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"end_restore_fence\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"cancel_request\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"show_finality\",\"right\":\"OBS_MANAGEMENT_CONTROL\"},"
         "{\"operation\":\"show_server_metrics\",\"right\":\"METRICS_INSPECT\"},"
         "{\"operation\":\"show_server_endpoints\",\"right\":\"OBS_MANAGEMENT_INSPECT\"},"
         "{\"operation\":\"show_server_databases\",\"right\":\"OBS_MANAGEMENT_INSPECT\"},"
         "{\"operation\":\"show_server_sessions\",\"right\":\"OBS_SESSION_ALL\"},"
         "{\"operation\":\"show_parser_packages\",\"right\":\"PARSER_INSPECT\"},"
         "{\"operation\":\"show_parser_channels\",\"right\":\"PARSER_INSPECT\"},"
         "{\"operation\":\"show_listeners\",\"right\":\"OBS_MANAGEMENT_INSPECT\"},"
         "{\"operation\":\"start_listener\",\"right\":\"LISTENER_CONTROL\"},"
         "{\"operation\":\"stop_listener\",\"right\":\"LISTENER_CONTROL\"},"
         "{\"operation\":\"restart_listener\",\"right\":\"LISTENER_CONTROL\"},"
         "{\"operation\":\"drain_listener\",\"right\":\"LISTENER_CONTROL\"},"
         "{\"operation\":\"validate_server_config\",\"right\":\"OBS_CONFIG_INSPECT\"},"
         "{\"operation\":\"export_server_support_bundle\",\"right\":\"SUPPORT_EXPORT\"}"
         "]}\n";
}

}  // namespace scratchbird::server
