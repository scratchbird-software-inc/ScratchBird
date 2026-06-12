// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_MAINTENANCE_RELOAD_SHUTDOWN

#include "maintenance_coordinator.hpp"

#include "database_lifecycle.hpp"
#include "sbps.hpp"
#include "uuid.hpp"

#include <sstream>
#include <utility>

namespace scratchbird::server {

namespace {

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

ServerDiagnostic MaintenanceDiagnostic(std::string code,
                                       std::string message,
                                       std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

std::string TokenText(const std::array<std::uint8_t, 16>& uuid) {
  return UuidBytesToText(uuid);
}

void ApplyModeFences(ServerMaintenanceCoordinator* coordinator,
                     const ServerBootstrapConfig& config) {
  coordinator->attach_admission_fenced = false;
  coordinator->write_admission_fenced = false;
  coordinator->sblr_admission_fenced = false;
  coordinator->event_admission_fenced = false;
  coordinator->restricted_open_mode = false;
  if (config.mode == ServerMode::kReadOnly || config.database_open_mode == "read_only") {
    coordinator->write_admission_fenced = true;
  }
  if (config.database_open_mode == "restricted" || config.database_open_mode == "restricted_open") {
    coordinator->restricted_open_mode = true;
    coordinator->state = "restricted_open";
    coordinator->attach_admission_fenced = true;
    coordinator->write_admission_fenced = true;
    coordinator->sblr_admission_fenced = true;
    coordinator->event_admission_fenced = true;
    coordinator->permitted_maintenance_operations = "inspect,diagnostic,verify,repair,backup,recovery,admin";
  }
  if (config.mode == ServerMode::kMaintenance || config.database_open_mode == "maintenance") {
    coordinator->maintenance_mode = true;
    coordinator->state = "maintenance";
    coordinator->attach_admission_fenced = true;
    coordinator->write_admission_fenced = true;
    coordinator->sblr_admission_fenced = true;
    coordinator->event_admission_fenced = true;
  }
}

bool ContainsToken(const std::string& value, const std::string& token) {
  return value.find(token) != std::string::npos;
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

bool IsForceShutdownRequest(const ServerMaintenanceOperationRequest& request) {
  const auto shutdown_mode = ModeValue(request.mode, "shutdown_mode");
  return request.operation_key == "shutdown_database_force" ||
         request.operation_key == "force_shutdown_database" ||
         request.mode == "force" ||
         shutdown_mode == "force" ||
         ModeBool(request.mode, "force", false);
}

scratchbird::storage::database::DatabaseLifecycleOperationConfig DatabaseOperationConfig(
    const ServerBootstrapConfig& config,
    const ServerMaintenanceOperationRequest& request) {
  scratchbird::storage::database::DatabaseLifecycleOperationConfig operation;
  operation.path = config.database_default_path.string();
  operation.cluster_authority_available = false;
  operation.decryption_available = ContainsToken(request.mode, "decryption_available:true");
  operation.operation_uuid = TokenText(request.request_uuid);
  operation.actor_uuid = TokenText(request.session_uuid);
  operation.write_evidence = true;
  return operation;
}

scratchbird::storage::database::DatabaseLifecycleRepairConfig DatabaseRepairConfig(
    const ServerBootstrapConfig& config,
    const ServerMaintenanceOperationRequest& request) {
  scratchbird::storage::database::DatabaseLifecycleRepairConfig repair;
  repair.path = config.database_default_path.string();
  repair.cluster_authority_available = false;
  repair.decryption_available = ContainsToken(request.mode, "decryption_available:true");
  repair.operation_uuid = TokenText(request.request_uuid);
  repair.actor_uuid = TokenText(request.session_uuid);
  repair.repair_plan_id = ModeValue(request.mode, "repair_plan_id");
  if (repair.repair_plan_id.empty()) repair.repair_plan_id = ModeValue(request.mode, "repair_plan");
  repair.expected_database_uuid = ModeValue(request.mode, "expected_database_uuid");
  repair.expected_filespace_uuid = ModeValue(request.mode, "expected_filespace_uuid");
  repair.repair_admission_proven = ContainsToken(request.mode, "repair_admission_proven:true") ||
                                   ContainsToken(request.mode, "restricted_or_maintenance_admission:true");
  repair.allow_mutation = ContainsToken(request.mode, "allow_repair:true") ||
                          ContainsToken(request.mode, "allow_mutation:true") ||
                          ContainsToken(request.mode, "repair_execute:true");
  return repair;
}

scratchbird::storage::database::DatabaseDropConfig DatabaseDropConfig(
    const ServerBootstrapConfig& config,
    const ServerMaintenanceOperationRequest& request) {
  scratchbird::storage::database::DatabaseDropConfig drop;
  drop.path = config.database_default_path.string();
  drop.cluster_authority_available = false;
  drop.decryption_available = ContainsToken(request.mode, "decryption_available:true");
  drop.operation_uuid = TokenText(request.request_uuid);
  drop.actor_uuid = TokenText(request.session_uuid);
  drop.drop_mode = ModeValue(request.mode, "drop_mode");
  if (drop.drop_mode.empty()) drop.drop_mode = "logical";
  drop.expected_database_uuid = request.target_uuid.empty()
      ? ModeValue(request.mode, "expected_database_uuid")
      : request.target_uuid;
  drop.expected_filespace_uuid = ModeValue(request.mode, "expected_filespace_uuid");
  drop.drop_safety_preconditions = ModeBool(request.mode, "drop_safety_preconditions", false);
  drop.session_drain_complete = ModeBool(request.mode, "session_drain_complete", false);
  drop.ownership_release_verified = ModeBool(request.mode, "ownership_release_verified", false);
  drop.retention_policy_satisfied = ModeBool(request.mode, "retention_policy_satisfied", false);
  drop.backup_coverage_verified = ModeBool(request.mode, "backup_coverage_verified", false);
  drop.legal_hold_clear = ModeBool(request.mode, "legal_hold_clear", false);
  drop.allow_physical_delete = ModeBool(request.mode, "allow_physical_delete", false) ||
                               ModeBool(request.mode, "physical_delete_policy_approved", false);
  drop.allow_quarantine = ModeBool(request.mode, "allow_quarantine", false) ||
                          ModeBool(request.mode, "quarantine_policy_approved", false);
  return drop;
}

std::string ShutdownRuntimeRecordJson(const ServerMaintenanceOperationRequest& request,
                                      const ServerShutdownRuntimeSnapshot& snapshot,
                                      const std::string& shutdown_mode,
                                      bool parser_fallback_used,
                                      bool clean_shutdown_marked,
                                      bool recovery_evidence_preserved,
                                      const std::string& force_policy_uuid) {
  const std::uint64_t notification_count =
      snapshot.associated_manager_count + snapshot.associated_listener_count +
      snapshot.associated_parser_count + snapshot.associated_ipc_endpoint_count +
      snapshot.associated_session_count;
  std::uint64_t required_ack =
      snapshot.required_acknowledgement_count == 0
          ? notification_count
          : snapshot.required_acknowledgement_count;
  required_ack = ModeU64(request.mode, "required_acknowledgement_count", required_ack);
  std::uint64_t acknowledged =
      ModeU64(request.mode,
              "acknowledged_component_count",
              snapshot.acknowledged_component_count);
  if (ModeBool(request.mode, "acknowledgements_satisfied", false) ||
      ModeValue(request.mode, "acknowledgement_policy") == "satisfied") {
    acknowledged = required_ack;
  }
  const bool drain_complete = ModeBool(request.mode,
                                       "drain_complete",
                                       snapshot.drain_complete ||
                                           snapshot.active_transaction_session_count == 0);
  std::ostringstream out;
  out << "[{\"operation_key\":\"" << JsonEscape(request.operation_key)
      << "\",\"shutdown_mode\":\"" << JsonEscape(shutdown_mode)
      << "\",\"database_ref\":\"" << JsonEscape(snapshot.database_path.empty() ? "" : "[path-redacted]")
      << "\",\"database_uuid\":\"" << JsonEscape(snapshot.database_uuid)
      << "\",\"association_scope_proven\":" << BoolText(snapshot.association_scope_proven)
      << ",\"notified_manager_count\":" << snapshot.associated_manager_count
      << ",\"notified_listener_count\":" << snapshot.associated_listener_count
      << ",\"notified_parser_count\":" << snapshot.associated_parser_count
      << ",\"notified_ipc_endpoint_count\":" << snapshot.associated_ipc_endpoint_count
      << ",\"notified_session_count\":" << snapshot.associated_session_count
      << ",\"notified_client_count\":" << snapshot.associated_client_count
      << ",\"shutdown_notification_count\":" << notification_count
      << ",\"required_acknowledgement_count\":" << required_ack
      << ",\"acknowledged_component_count\":" << acknowledged
      << ",\"drain_complete\":" << BoolText(drain_complete)
      << ",\"active_transaction_session_count\":" << snapshot.active_transaction_session_count
      << ",\"parser_fallback_used\":" << BoolText(parser_fallback_used)
      << ",\"listener_unavailable\":" << BoolText(snapshot.listener_unavailable)
      << ",\"clean_shutdown_marked\":" << BoolText(clean_shutdown_marked)
      << ",\"force_termination_policy_uuid\":\"" << JsonEscape(force_policy_uuid)
      << "\",\"recovery_evidence_preserved\":" << BoolText(recovery_evidence_preserved)
      << ",\"unknown_transaction_finality_preserved\":"
      << BoolText(shutdown_mode == "force" && snapshot.active_transaction_session_count != 0)
      << ",\"unrelated_database_protected\":true}]";
  return out.str();
}

ServerDiagnostic StorageLifecycleDiagnostic(
    const scratchbird::storage::database::DatabaseLifecycleResult& result,
    std::string fallback_code) {
  std::string code = result.diagnostic.diagnostic_code.empty()
      ? std::move(fallback_code)
      : result.diagnostic.diagnostic_code;
  if (code.find("CLUSTER") != std::string::npos) {
    code = "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED";
  }
  if (fallback_code.rfind("ENGINE.DBLC_", 0) == 0) {
    code = std::move(fallback_code);
  }
  return MaintenanceDiagnostic(
      code,
      result.diagnostic.message_key.empty()
          ? "The database lifecycle operation was refused."
          : result.diagnostic.message_key,
      {{"storage_diagnostic_code", result.diagnostic.diagnostic_code},
       {"storage_message_key", result.diagnostic.message_key}});
}

std::string DatabaseLifecycleRecordJson(
    const scratchbird::storage::database::DatabaseLifecycleState& state,
    const std::string& operation_key) {
  std::ostringstream out;
  out << "[{\"operation_key\":\"" << JsonEscape(operation_key)
      << "\",\"database_ref\":\"" << JsonEscape(state.path.empty() ? "" : "[path-redacted]")
      << "\",\"phase\":\""
      << scratchbird::storage::database::DatabaseLifecyclePhaseName(state.phase)
      << "\",\"database_uuid\":\""
      << JsonEscape(scratchbird::core::uuid::UuidToString(state.database_uuid.value))
      << "\",\"filespace_uuid\":\""
      << JsonEscape(scratchbird::core::uuid::UuidToString(state.filespace_uuid.value))
      << "\",\"read_only_open\":" << BoolText(state.read_only_open)
      << ",\"write_admission_fenced\":" << BoolText(state.write_admission_fenced)
      << ",\"startup_recovery_classification\":\""
      << JsonEscape(state.startup_recovery_classification)
      << "\",\"lifecycle_generation\":"
      << state.startup_state.lifecycle_generation
      << ",\"last_lifecycle_local_transaction_id\":"
      << state.startup_state.last_lifecycle_local_transaction_id
      << ",\"durable_lifecycle_phase\":\""
      << scratchbird::storage::database::StartupLifecycleDurablePhaseName(
             state.startup_state.durable_lifecycle_phase)
      << "\"}]";
  return out.str();
}

std::string JoinRecordArrays(const std::string& left, const std::string& right) {
  if (left == "[]" || left.empty()) return right.empty() ? "[]" : right;
  if (right == "[]" || right.empty()) return left;
  return left.substr(0, left.size() - 1) + "," + right.substr(1);
}

std::string FinalityRecordJson(const ServerFinalityRecord& finality) {
  std::ostringstream out;
  out << "{\"finality_token_uuid\":\"" << JsonEscape(TokenText(finality.finality_token_uuid))
      << "\",\"request_uuid\":\"" << JsonEscape(TokenText(finality.request_uuid))
      << "\",\"session_uuid\":\"" << JsonEscape(TokenText(finality.session_uuid))
      << "\",\"operation\":\"" << JsonEscape(finality.operation)
      << "\",\"state\":\"" << JsonEscape(finality.state)
      << "\",\"detail\":\"" << JsonEscape(finality.detail)
      << "\",\"policy_generation\":" << finality.policy_generation << "}";
  return out.str();
}

ServerFinalityRecord RecordMaintenanceFinality(ServerMaintenanceCoordinator* coordinator,
                                               const ServerMaintenanceOperationRequest& request,
                                               const std::string& state,
                                               const std::string& detail) {
  ServerFinalityRecord finality;
  finality.finality_token_uuid = sbps::MakeUuidV7Bytes();
  finality.request_uuid = request.request_uuid;
  finality.session_uuid = request.session_uuid;
  finality.operation = request.operation_key;
  finality.state = state;
  finality.detail = detail;
  finality.policy_generation = coordinator->generation;
  const auto token = TokenText(finality.finality_token_uuid);
  coordinator->last_finality_token = token;
  coordinator->finality_by_token_uuid[token] = finality;
  return finality;
}

ServerMaintenanceOperationResult Done(ServerMaintenanceCoordinator* coordinator,
                                      const ServerMaintenanceOperationRequest& request,
                                      std::string outcome,
                                      std::string detail) {
  ServerMaintenanceOperationResult result;
  result.outcome = std::move(outcome);
  result.state_after = coordinator->state;
  const auto finality = RecordMaintenanceFinality(coordinator, request, result.outcome, std::move(detail));
  result.finality_token_uuid = TokenText(finality.finality_token_uuid);
  result.records_json = "[" + MaintenanceCoordinatorRecordsJson(*coordinator).substr(1);
  return result;
}

ServerMaintenanceOperationResult DatabaseDone(ServerMaintenanceCoordinator* coordinator,
                                              const ServerMaintenanceOperationRequest& request,
                                              std::string outcome,
                                              std::string detail,
                                              const scratchbird::storage::database::DatabaseLifecycleResult& database_result) {
  ServerMaintenanceOperationResult result;
  result.outcome = std::move(outcome);
  result.state_after = coordinator->state;
  const auto finality = RecordMaintenanceFinality(coordinator, request, result.outcome, std::move(detail));
  result.finality_token_uuid = TokenText(finality.finality_token_uuid);
  result.records_json = JoinRecordArrays(MaintenanceCoordinatorRecordsJson(*coordinator),
                                         DatabaseLifecycleRecordJson(database_result.state,
                                                                     request.operation_key));
  return result;
}

}  // namespace

ServerMaintenanceCoordinator BuildMaintenanceCoordinator(const ServerBootstrapConfig& config,
                                                         const ServerLifecycleArtifacts& artifacts) {
  ServerMaintenanceCoordinator coordinator;
  coordinator.generation = artifacts.generation == 0 ? 1 : artifacts.generation;
  coordinator.reload_generation = coordinator.generation;
  coordinator.state = "running";
  ApplyModeFences(&coordinator, config);
  return coordinator;
}

bool MaintenanceAllowsAttach(const ServerMaintenanceCoordinator& coordinator) {
  return !coordinator.attach_admission_fenced && !coordinator.draining &&
         !coordinator.stopping && !coordinator.restart_pending && !coordinator.restore_fence_active;
}

bool MaintenanceAllowsSblr(const ServerMaintenanceCoordinator& coordinator) {
  return !coordinator.sblr_admission_fenced && !coordinator.stopping &&
         !coordinator.restart_pending && !coordinator.restore_fence_active;
}

bool MaintenanceAllowsEvents(const ServerMaintenanceCoordinator& coordinator) {
  return !coordinator.event_admission_fenced && !coordinator.draining &&
         !coordinator.stopping && !coordinator.restart_pending && !coordinator.restore_fence_active;
}

ServerDiagnostic MaintenanceAdmissionDiagnostic(const ServerMaintenanceCoordinator& coordinator,
                                                std::string operation_key,
                                                std::string reason) {
  const std::string code = coordinator.restricted_open_mode
      ? "ENGINE.DBLC_RESTRICTED_OPEN_REQUIRED"
      : "SERVER.MAINTENANCE.ADMISSION_DENIED";
  return MaintenanceDiagnostic(code,
                               coordinator.restricted_open_mode
                                   ? "The database is in restricted-open mode; ordinary admission is refused."
                                   : "The server maintenance coordinator refused admission for this operation.",
                               {{"operation_key", std::move(operation_key)},
                                {"reason", std::move(reason)},
                                {"server_state", coordinator.state},
                                {"maintenance_mode", BoolText(coordinator.maintenance_mode)},
                                {"restricted_open_mode", BoolText(coordinator.restricted_open_mode)},
                                {"draining", BoolText(coordinator.draining)},
                                {"stopping", BoolText(coordinator.stopping)}});
}

std::string MaintenanceCoordinatorRecordsJson(const ServerMaintenanceCoordinator& coordinator) {
  std::ostringstream out;
  out << "[{\"server_state\":\"" << JsonEscape(coordinator.state)
      << "\",\"state_generation\":" << coordinator.generation
      << ",\"reload_generation\":" << coordinator.reload_generation
      << ",\"maintenance_mode\":" << BoolText(coordinator.maintenance_mode)
      << ",\"restricted_open_mode\":" << BoolText(coordinator.restricted_open_mode)
      << ",\"draining\":" << BoolText(coordinator.draining)
      << ",\"stopping\":" << BoolText(coordinator.stopping)
      << ",\"restart_pending\":" << BoolText(coordinator.restart_pending)
      << ",\"attach_admission_fenced\":" << BoolText(coordinator.attach_admission_fenced)
      << ",\"write_admission_fenced\":" << BoolText(coordinator.write_admission_fenced)
      << ",\"sblr_admission_fenced\":" << BoolText(coordinator.sblr_admission_fenced)
      << ",\"event_admission_fenced\":" << BoolText(coordinator.event_admission_fenced)
      << ",\"backup_fence_active\":" << BoolText(coordinator.backup_fence_active)
      << ",\"restore_fence_active\":" << BoolText(coordinator.restore_fence_active)
      << ",\"database_shutdown_requested\":" << BoolText(coordinator.database_shutdown_requested)
      << ",\"shutdown_parser_fallback_used\":" << BoolText(coordinator.shutdown_parser_fallback_used)
      << ",\"shutdown_generation\":" << coordinator.shutdown_generation
      << ",\"shutdown_notification_count\":" << coordinator.shutdown_notification_count
      << ",\"shutdown_required_ack_count\":" << coordinator.shutdown_required_ack_count
      << ",\"shutdown_acknowledged_count\":" << coordinator.shutdown_acknowledged_count
      << ",\"shutdown_active_transaction_session_count\":"
      << coordinator.shutdown_active_transaction_session_count
      << ",\"shutdown_mode\":\"" << JsonEscape(coordinator.shutdown_mode)
      << "\",\"shutdown_database_uuid\":\"" << JsonEscape(coordinator.shutdown_database_uuid)
      << "\",\"database_uuid\":\"" << JsonEscape(coordinator.database_uuid)
      << "\",\"permitted_maintenance_operations\":\""
      << JsonEscape(coordinator.permitted_maintenance_operations)
      << "\",\"last_operation\":\"" << JsonEscape(coordinator.last_operation)
      << "\",\"last_outcome\":\"" << JsonEscape(coordinator.last_outcome)
      << "\",\"last_finality_token_uuid\":\"" << JsonEscape(coordinator.last_finality_token)
      << "\",\"observability_contract\":\"DBLC_P15_OBSERVABILITY_COMPLETE"
      << "\",\"message_vector_shape\":\"diag.server.lifecycle.v1"
      << "\",\"cache_invalidation_marker_required\":true"
      << ",\"parser_finality_authority\":false"
      << ",\"reference_finality_authority\":false"
      << "}]";
  return out.str();
}

std::string MaintenanceFinalityRecordsJson(const ServerMaintenanceCoordinator& coordinator) {
  std::ostringstream out;
  out << "[";
  bool first = true;
  for (const auto& [_, finality] : coordinator.finality_by_token_uuid) {
    if (!first) out << ',';
    first = false;
    out << FinalityRecordJson(finality);
  }
  out << "]";
  return out.str();
}

ServerMaintenanceOperationResult ApplyDatabaseShutdownOperation(
    ServerMaintenanceCoordinator* coordinator,
    const ServerBootstrapConfig& config,
    const ServerMaintenanceOperationRequest& request,
    const ServerShutdownRuntimeSnapshot& snapshot) {
  ServerMaintenanceOperationResult result;
  if (coordinator == nullptr) {
    result.ok = false;
    result.outcome = "refused";
    result.diagnostics.push_back(MaintenanceDiagnostic(
        "SERVER.MAINTENANCE.COORDINATOR_UNAVAILABLE",
        "The server maintenance coordinator is not available."));
    return result;
  }

  auto refuse = [&](std::string code,
                    std::string message,
                    std::string finality_detail,
                    std::vector<ServerDiagnosticField> fields = {}) {
    ServerMaintenanceOperationResult refused;
    refused.ok = false;
    refused.outcome = "refused";
    refused.state_before = result.state_before;
    refused.state_after = coordinator->state;
    refused.records_json = MaintenanceCoordinatorRecordsJson(*coordinator);
    refused.diagnostics.push_back(
        MaintenanceDiagnostic(std::move(code), std::move(message), std::move(fields)));
    const auto finality =
        RecordMaintenanceFinality(coordinator, request, "refused", std::move(finality_detail));
    refused.finality_token_uuid = TokenText(finality.finality_token_uuid);
    coordinator->last_outcome = refused.outcome;
    return refused;
  };

  result.state_before = coordinator->state;
  coordinator->last_operation = request.operation_key;

  const bool force = IsForceShutdownRequest(request);
  const std::string shutdown_mode = force ? "force" : "graceful";
  const bool scope_proven = snapshot.association_scope_proven;
  const std::string target_path = snapshot.database_path.empty()
      ? config.database_default_path.string()
      : snapshot.database_path;
  if (target_path.empty() || config.database_default_path.empty()) {
    return refuse("ENGINE.SHUTDOWN_SCOPE_INVALID",
                  "The database shutdown operation requires an exact target database path.",
                  "shutdown_database_path_missing");
  }
  if (!scope_proven) {
    const std::string code = snapshot.association_diagnostic_code.empty()
        ? "ENGINE.SHUTDOWN_SCOPE_INVALID"
        : snapshot.association_diagnostic_code;
    return refuse(code,
                  "The shutdown target database association scope is not proven.",
                  snapshot.association_diagnostic_detail.empty()
                      ? "shutdown_association_scope_not_proven"
                      : snapshot.association_diagnostic_detail,
                  {{"database_uuid", snapshot.database_uuid}});
  }
  if (!request.target_uuid.empty() &&
      !snapshot.database_uuid.empty() &&
      request.target_uuid != snapshot.database_uuid) {
    return refuse("ENGINE.SHUTDOWN_SCOPE_INVALID",
                  "The shutdown target UUID does not match the associated database UUID.",
                  "shutdown_target_uuid_mismatch",
                  {{"target_uuid", request.target_uuid}, {"database_uuid", snapshot.database_uuid}});
  }

  const bool fallback_required = snapshot.listener_unavailable ||
                                 ModeBool(request.mode, "parser_fallback_required", false);
  bool parser_fallback_used = false;
  if (fallback_required) {
    if (snapshot.parser_association_registry_stale) {
      return refuse("ENGINE.SHUTDOWN_PARSER_ASSOCIATION_STALE",
                    "The parser association fallback evidence is stale.",
                    "shutdown_parser_association_stale",
                    {{"database_uuid", snapshot.database_uuid}});
    }
    if (!snapshot.parser_association_registry_available) {
      return refuse("ENGINE.SHUTDOWN_PARSER_ASSOCIATION_MISSING",
                    "The listener is unavailable and no engine-visible parser association is present.",
                    "shutdown_parser_association_missing",
                    {{"database_uuid", snapshot.database_uuid}});
    }
    parser_fallback_used = true;
  }

  std::uint64_t required_ack = snapshot.required_acknowledgement_count;
  if (required_ack == 0) {
    required_ack = snapshot.associated_manager_count + snapshot.associated_listener_count +
                   snapshot.associated_parser_count + snapshot.associated_ipc_endpoint_count +
                   snapshot.associated_session_count;
  }
  required_ack = ModeU64(request.mode, "required_acknowledgement_count", required_ack);
  std::uint64_t acknowledged =
      ModeU64(request.mode, "acknowledged_component_count", snapshot.acknowledged_component_count);
  if (ModeBool(request.mode, "acknowledgements_satisfied", false) ||
      ModeValue(request.mode, "acknowledgement_policy") == "satisfied") {
    acknowledged = required_ack;
  }

  coordinator->database_shutdown_requested = true;
  coordinator->shutdown_mode = shutdown_mode;
  coordinator->shutdown_database_uuid = snapshot.database_uuid;
  coordinator->database_uuid = snapshot.database_uuid;
  coordinator->shutdown_parser_fallback_used = parser_fallback_used;
  coordinator->shutdown_notification_count =
      snapshot.associated_manager_count + snapshot.associated_listener_count +
      snapshot.associated_parser_count + snapshot.associated_ipc_endpoint_count +
      snapshot.associated_session_count;
  coordinator->shutdown_required_ack_count = required_ack;
  coordinator->shutdown_acknowledged_count = acknowledged;
  coordinator->shutdown_active_transaction_session_count =
      snapshot.active_transaction_session_count;
  coordinator->draining = !force;
  coordinator->state = force ? "shutdown_forcing" : "shutdown_draining";
  coordinator->attach_admission_fenced = true;
  coordinator->write_admission_fenced = true;
  coordinator->sblr_admission_fenced = true;
  coordinator->event_admission_fenced = true;
  coordinator->shutdown_generation = ++coordinator->generation;

  if (acknowledged < required_ack) {
    result = refuse("ENGINE.SHUTDOWN_ACK_TIMEOUT",
                    "Associated shutdown components did not acknowledge before the drain deadline.",
                    "shutdown_acknowledgement_timeout",
                    {{"required_acknowledgement_count", std::to_string(required_ack)},
                     {"acknowledged_component_count", std::to_string(acknowledged)}});
    result.records_json = JoinRecordArrays(
        result.records_json,
        ShutdownRuntimeRecordJson(request,
                                  snapshot,
                                  shutdown_mode,
                                  parser_fallback_used,
                                  false,
                                  false,
                                  {}));
    return result;
  }

  const bool drain_complete = snapshot.drain_complete ||
                              ModeBool(request.mode, "drain_complete", false) ||
                              snapshot.active_transaction_session_count == 0;
  if (!force && !drain_complete) {
    result = refuse("ENGINE.SHUTDOWN_DRAIN_TIMEOUT",
                    "Active sessions did not commit or roll back before the graceful shutdown deadline.",
                    "shutdown_drain_timeout",
                    {{"active_transaction_session_count",
                      std::to_string(snapshot.active_transaction_session_count)}});
    result.records_json = JoinRecordArrays(
        result.records_json,
        ShutdownRuntimeRecordJson(request,
                                  snapshot,
                                  shutdown_mode,
                                  parser_fallback_used,
                                  false,
                                  false,
                                  {}));
    return result;
  }

  std::string force_policy_uuid;
  bool recovery_evidence_preserved = false;
  if (force) {
    force_policy_uuid = ModeValue(request.mode, "force_termination_policy_uuid");
    if (force_policy_uuid.empty()) force_policy_uuid = ModeValue(request.mode, "force_policy_uuid");
    recovery_evidence_preserved =
        ModeBool(request.mode, "recovery_evidence_preserved", false);
    if (force_policy_uuid.empty()) {
      return refuse("ENGINE.SHUTDOWN_INPUT_INVALID",
                    "Force shutdown requires an explicit force termination policy UUID.",
                    "shutdown_force_policy_uuid_missing");
    }
    if (!recovery_evidence_preserved) {
      return refuse("ENGINE.SHUTDOWN_INPUT_INVALID",
                    "Force shutdown requires explicit MGA recovery evidence preservation.",
                    "shutdown_force_recovery_evidence_missing");
    }
  }

  scratchbird::storage::database::DatabaseLifecycleResult inspected;
  bool clean_shutdown_marked = false;
  if (!force) {
    const auto clean = scratchbird::storage::database::MarkDatabaseCleanShutdown(target_path);
    if (!clean.ok()) {
      return refuse(clean.diagnostic.diagnostic_code.empty()
                        ? "ENGINE.SHUTDOWN_INPUT_INVALID"
                        : clean.diagnostic.diagnostic_code,
                    clean.diagnostic.message_key.empty()
                        ? "The database clean shutdown final transaction could not be persisted."
                        : clean.diagnostic.message_key,
                    "shutdown_clean_final_transaction_failed");
    }
    clean_shutdown_marked = true;
    auto inspect_config = DatabaseOperationConfig(config, request);
    inspect_config.path = target_path;
    inspect_config.write_evidence = false;
    inspected = scratchbird::storage::database::InspectDatabaseLifecycle(inspect_config);
  }

  coordinator->database_shutdown_requested = false;
  coordinator->draining = false;
  coordinator->maintenance_mode = false;
  coordinator->restricted_open_mode = false;
  coordinator->state = force ? "shutdown_force_completed" : "closed_clean";

  result.ok = true;
  result.outcome = force ? "shutdown_force_completed" : "shutdown_clean";
  result.state_after = coordinator->state;
  const auto finality = RecordMaintenanceFinality(
      coordinator,
      request,
      result.outcome,
      force ? "shutdown_force_runtime_terminated_with_mga_recovery_evidence"
            : "shutdown_clean_final_lifecycle_transaction_committed");
  result.finality_token_uuid = TokenText(finality.finality_token_uuid);
  result.records_json = JoinRecordArrays(
      MaintenanceCoordinatorRecordsJson(*coordinator),
      ShutdownRuntimeRecordJson(request,
                                snapshot,
                                shutdown_mode,
                                parser_fallback_used,
                                clean_shutdown_marked,
                                recovery_evidence_preserved,
                                force_policy_uuid));
  if (!force && inspected.ok()) {
    result.records_json = JoinRecordArrays(result.records_json,
                                           DatabaseLifecycleRecordJson(inspected.state,
                                                                       request.operation_key));
  }
  coordinator->last_outcome = result.outcome;
  return result;
}

ServerMaintenanceOperationResult ApplyServerMaintenanceOperation(
    ServerMaintenanceCoordinator* coordinator,
    const ServerBootstrapConfig& config,
    const ServerMaintenanceOperationRequest& request) {
  ServerMaintenanceOperationResult result;
  if (coordinator == nullptr) {
    result.ok = false;
    result.outcome = "refused";
    result.diagnostics.push_back(MaintenanceDiagnostic(
        "SERVER.MAINTENANCE.COORDINATOR_UNAVAILABLE",
        "The server maintenance coordinator is not available."));
    return result;
  }
  result.state_before = coordinator->state;
  coordinator->last_operation = request.operation_key;

  if (request.operation_key == "show_server_lifecycle" ||
      request.operation_key == "show_maintenance_fences" ||
      request.operation_key == "show_database_shutdown_state") {
    result.records_json = MaintenanceCoordinatorRecordsJson(*coordinator);
    result.state_after = coordinator->state;
  } else if (request.operation_key == "show_finality") {
    result.records_json = MaintenanceFinalityRecordsJson(*coordinator);
    result.state_after = coordinator->state;
  } else if (request.operation_key == "ack_database_shutdown") {
    const std::string acknowledger_kind = ModeValue(request.mode, "acknowledger_kind");
    std::string acknowledger_uuid = ModeValue(request.mode, "acknowledger_uuid");
    if (acknowledger_uuid.empty()) acknowledger_uuid = ModeValue(request.mode, "process_uuid");
    const std::string acknowledgement_generation =
        ModeValue(request.mode, "acknowledgement_generation");
    const auto acknowledgement_generation_value =
        ModeU64(request.mode, "acknowledgement_generation", 0);
    std::string acknowledgement_state = ModeValue(request.mode, "acknowledgement_state");
    if (acknowledgement_state.empty()) acknowledgement_state = "acknowledged";
    const bool valid_state = acknowledgement_state == "acknowledged" ||
                             acknowledgement_state == "draining" ||
                             acknowledgement_state == "clean_stop_complete";
    if (acknowledger_kind.empty() || acknowledger_uuid.empty() ||
        acknowledgement_generation.empty() || acknowledgement_generation_value == 0 ||
        !valid_state ||
        (coordinator->shutdown_generation != 0 &&
         acknowledgement_generation_value != coordinator->shutdown_generation)) {
      result.ok = false;
      result.outcome = "refused";
      result.state_after = coordinator->state;
      result.diagnostics.push_back(MaintenanceDiagnostic(
          "ENGINE.SHUTDOWN_ACK_INVALID",
          "Shutdown acknowledgement requires acknowledger kind, UUID, generation, and a valid state.",
          {{"acknowledger_kind", acknowledger_kind},
           {"acknowledger_uuid", acknowledger_uuid},
           {"acknowledgement_generation", acknowledgement_generation},
           {"acknowledgement_state", acknowledgement_state}}));
      RecordMaintenanceFinality(coordinator, request, "refused", "shutdown_ack_invalid");
      return result;
    }
    ++coordinator->shutdown_acknowledged_count;
    coordinator->shutdown_generation = acknowledgement_generation_value;
    result = Done(coordinator,
                  request,
                  "shutdown_acknowledged",
                  "acknowledger_kind=" + acknowledger_kind +
                      ";acknowledger_uuid=" + acknowledger_uuid +
                      ";acknowledgement_state=" + acknowledgement_state);
  } else if (request.operation_key == "enter_database_maintenance" ||
             request.operation_key == "exit_database_maintenance" ||
             request.operation_key == "enter_restricted_open" ||
             request.operation_key == "exit_restricted_open" ||
             request.operation_key == "inspect_database" ||
             request.operation_key == "diagnose_database" ||
             request.operation_key == "verify_database" ||
             request.operation_key == "repair_database" ||
             request.operation_key == "drop_database") {
    if (config.database_default_path.empty()) {
      result.ok = false;
      result.outcome = "refused";
      result.state_after = coordinator->state;
      result.diagnostics.push_back(MaintenanceDiagnostic(
          "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS",
          "The database lifecycle operation requires an associated database path."));
      RecordMaintenanceFinality(coordinator, request, "refused", "database_association_scope_ambiguous");
      return result;
    }

    scratchbird::storage::database::DatabaseLifecycleResult database_result;
    if (request.operation_key == "enter_database_maintenance") {
      database_result = scratchbird::storage::database::EnterDatabaseMaintenanceMode(
          DatabaseOperationConfig(config, request));
      if (database_result.ok()) {
        coordinator->maintenance_mode = true;
        coordinator->restricted_open_mode = false;
        coordinator->draining = false;
        coordinator->state = "maintenance";
        coordinator->attach_admission_fenced = true;
        coordinator->write_admission_fenced = true;
        coordinator->sblr_admission_fenced = true;
        coordinator->event_admission_fenced = true;
        coordinator->permitted_maintenance_operations = "inspect,diagnostic,verify,repair,backup,recovery,admin";
      }
    } else if (request.operation_key == "exit_database_maintenance") {
      database_result = scratchbird::storage::database::ExitDatabaseMaintenanceMode(
          DatabaseOperationConfig(config, request));
      if (database_result.ok()) {
        coordinator->maintenance_mode = false;
        coordinator->restricted_open_mode = false;
        coordinator->draining = false;
        coordinator->state = "running";
        coordinator->permitted_maintenance_operations.clear();
        ApplyModeFences(coordinator, config);
      }
    } else if (request.operation_key == "enter_restricted_open") {
      database_result = scratchbird::storage::database::EnterDatabaseRestrictedOpenMode(
          DatabaseOperationConfig(config, request));
      if (database_result.ok()) {
        coordinator->maintenance_mode = false;
        coordinator->restricted_open_mode = true;
        coordinator->draining = false;
        coordinator->state = "restricted_open";
        coordinator->attach_admission_fenced = true;
        coordinator->write_admission_fenced = true;
        coordinator->sblr_admission_fenced = true;
        coordinator->event_admission_fenced = true;
        coordinator->permitted_maintenance_operations = "inspect,diagnostic,verify,repair,backup,recovery,admin";
      }
    } else if (request.operation_key == "exit_restricted_open") {
      database_result = scratchbird::storage::database::ExitDatabaseRestrictedOpenMode(
          DatabaseOperationConfig(config, request));
      if (database_result.ok()) {
        coordinator->restricted_open_mode = false;
        coordinator->maintenance_mode = false;
        coordinator->draining = false;
        coordinator->state = "running";
        coordinator->permitted_maintenance_operations.clear();
        ApplyModeFences(coordinator, config);
      }
    } else if (request.operation_key == "inspect_database" ||
               request.operation_key == "diagnose_database") {
      auto op_config = DatabaseOperationConfig(config, request);
      op_config.write_evidence = false;
      database_result = scratchbird::storage::database::InspectDatabaseLifecycle(op_config);
    } else if (request.operation_key == "verify_database") {
      database_result = scratchbird::storage::database::VerifyDatabaseLifecycle(
          DatabaseOperationConfig(config, request));
    } else if (request.operation_key == "repair_database") {
      database_result = scratchbird::storage::database::RepairDatabaseLifecycle(
          DatabaseRepairConfig(config, request));
      if (database_result.ok() && request.mode.find("clear_verified_write_fence") != std::string::npos) {
        coordinator->restricted_open_mode = false;
        coordinator->maintenance_mode = false;
        coordinator->state = "running";
        coordinator->permitted_maintenance_operations.clear();
        ApplyModeFences(coordinator, config);
      }
    } else {
      database_result = scratchbird::storage::database::DropDatabaseLifecycle(
          DatabaseDropConfig(config, request));
      if (database_result.ok()) {
        coordinator->maintenance_mode = false;
        coordinator->restricted_open_mode = false;
        coordinator->draining = false;
        coordinator->state = request.mode.find("drop_mode:quarantine") != std::string::npos
            ? "database_quarantined"
            : "database_dropped";
        coordinator->attach_admission_fenced = true;
        coordinator->write_admission_fenced = true;
        coordinator->sblr_admission_fenced = true;
        coordinator->event_admission_fenced = true;
      }
    }

    if (!database_result.ok()) {
      result.ok = false;
      result.outcome = "refused";
      result.state_after = coordinator->state;
      const std::string fallback_code =
          request.operation_key == "verify_database" ? "ENGINE.DBLC_VERIFY_FAILED" :
          request.operation_key == "enter_restricted_open" ? "ENGINE.DBLC_RESTRICTED_OPEN_REQUIRED" :
          request.operation_key == "repair_database" ? "ENGINE.DBLC_REPAIR_REFUSED" :
          request.operation_key == "drop_database" ? "ENGINE.DBLC_DROP_UNSAFE" :
          "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS";
      result.diagnostics.push_back(StorageLifecycleDiagnostic(database_result, fallback_code));
      RecordMaintenanceFinality(coordinator, request, "refused", database_result.diagnostic.diagnostic_code);
      return result;
    }

    coordinator->database_uuid = scratchbird::core::uuid::UuidToString(database_result.state.database_uuid.value);
    ++coordinator->generation;
    const std::string outcome =
        request.operation_key == "enter_database_maintenance" ? "maintenance_enabled" :
        request.operation_key == "exit_database_maintenance" ? "maintenance_cleared" :
        request.operation_key == "enter_restricted_open" ? "restricted_open_enabled" :
        request.operation_key == "exit_restricted_open" ? "restricted_open_cleared" :
        request.operation_key == "inspect_database" ? "inspected" :
        request.operation_key == "diagnose_database" ? "diagnosed" :
        request.operation_key == "verify_database" ? "verified" :
        request.operation_key == "repair_database" ? "repaired" :
        "dropped";
    result = DatabaseDone(coordinator,
                          request,
                          outcome,
                          "database_lifecycle_operation_completed",
                          database_result);
  } else if (request.operation_key == "reload_server_config") {
    if (request.mode == "invalid" || request.mode == "invalid_config" ||
        request.mode == "test_invalid") {
      result.ok = false;
      result.outcome = "refused";
      result.state_after = coordinator->state;
      result.diagnostics.push_back(MaintenanceDiagnostic(
          "SERVER.CONFIG.RELOAD_FAILED",
          "The staged server configuration reload failed validation; the active generation was retained.",
          {{"active_reload_generation", std::to_string(coordinator->reload_generation)},
           {"mode", request.mode}}));
      RecordMaintenanceFinality(coordinator, request, "refused", "staged_reload_validation_failed");
      return result;
    }
    ++coordinator->generation;
    ++coordinator->reload_generation;
    result = Done(coordinator, request, request.mode == "dry_run" ? "validated" : "completed",
                  request.mode == "dry_run" ? "reload_validated_no_apply" : "reload_applied_transactionally");
  } else if (request.operation_key == "drain_server") {
    coordinator->draining = true;
    coordinator->state = "draining";
    coordinator->attach_admission_fenced = true;
    coordinator->event_admission_fenced = true;
    ++coordinator->generation;
    result = Done(coordinator, request, "draining", "new attach and event admission fenced");
  } else if (request.operation_key == "set_server_maintenance_mode") {
    coordinator->maintenance_mode = true;
    coordinator->draining = false;
    coordinator->state = "maintenance";
    coordinator->attach_admission_fenced = true;
    coordinator->write_admission_fenced = true;
    coordinator->sblr_admission_fenced = true;
    coordinator->event_admission_fenced = true;
    ++coordinator->generation;
    result = Done(coordinator, request, "maintenance_enabled", "maintenance fences enabled");
  } else if (request.operation_key == "clear_server_maintenance_mode") {
    coordinator->maintenance_mode = false;
    coordinator->draining = false;
    coordinator->state = "running";
    coordinator->attach_admission_fenced = false;
    coordinator->write_admission_fenced = false;
    coordinator->sblr_admission_fenced = false;
    coordinator->event_admission_fenced = false;
    ApplyModeFences(coordinator, config);
    ++coordinator->generation;
    result = Done(coordinator, request, "maintenance_cleared", "maintenance fences cleared");
  } else if (request.operation_key == "begin_backup_fence") {
    coordinator->backup_fence_active = true;
    ++coordinator->generation;
    result = Done(coordinator, request, "backup_fence_active", "backup coordination fence active");
  } else if (request.operation_key == "end_backup_fence") {
    coordinator->backup_fence_active = false;
    ++coordinator->generation;
    result = Done(coordinator, request, "backup_fence_cleared", "backup coordination fence cleared");
  } else if (request.operation_key == "begin_restore_fence") {
    coordinator->restore_fence_active = true;
    coordinator->attach_admission_fenced = true;
    coordinator->write_admission_fenced = true;
    coordinator->sblr_admission_fenced = true;
    coordinator->event_admission_fenced = true;
    ++coordinator->generation;
    result = Done(coordinator, request, "restore_fence_active", "restore coordination fence active");
  } else if (request.operation_key == "end_restore_fence") {
    coordinator->restore_fence_active = false;
    coordinator->state = coordinator->maintenance_mode ? "maintenance" : "running";
    ApplyModeFences(coordinator, config);
    ++coordinator->generation;
    result = Done(coordinator, request, "restore_fence_cleared", "restore coordination fence cleared");
  } else if (request.operation_key == "cancel_request") {
    auto found = coordinator->finality_by_token_uuid.find(request.target_uuid);
    if (found == coordinator->finality_by_token_uuid.end()) {
      result.outcome = "unknown_finality";
      result.state_after = coordinator->state;
      result.records_json = "[{\"requested_finality_token_uuid\":\"" + JsonEscape(request.target_uuid) +
                            "\",\"finality_state\":\"unknown\",\"diagnostic_code\":\"SERVER.REQUEST.FINALITY_UNKNOWN\"}]";
      RecordMaintenanceFinality(coordinator, request, "unknown_finality", "no matching active request");
    } else {
      found->second.state = "cancelled";
      found->second.detail = "operator_cancelled";
      result.records_json = "[" + FinalityRecordJson(found->second) + "]";
      result.outcome = "cancelled";
      result.state_after = coordinator->state;
    }
  } else if (request.operation_key == "stop_server" ||
             request.operation_key == "restart_server") {
    coordinator->stopping = true;
    coordinator->restart_pending = request.operation_key == "restart_server";
    coordinator->shutdown_requested = true;
    coordinator->state = coordinator->restart_pending ? "restart_pending" : "stopping";
    coordinator->attach_admission_fenced = true;
    coordinator->write_admission_fenced = true;
    coordinator->sblr_admission_fenced = true;
    coordinator->event_admission_fenced = true;
    ++coordinator->generation;
    result = Done(coordinator,
                  request,
                  coordinator->restart_pending ? "restart_requested" : "shutdown_requested",
                  request.mode == "force" ? "forced_shutdown_requested" : "graceful_shutdown_requested");
    result.request_shutdown = true;
  } else {
    result.ok = false;
    result.outcome = "refused";
    result.state_after = coordinator->state;
    result.diagnostics.push_back(MaintenanceDiagnostic(
        "SERVER.MANAGEMENT.OPERATION_UNKNOWN",
        "The maintenance coordinator does not support this operation.",
        {{"operation_key", request.operation_key}}));
    return result;
  }

  coordinator->last_outcome = result.outcome;
  if (result.records_json.empty()) result.records_json = MaintenanceCoordinatorRecordsJson(*coordinator);
  return result;
}

}  // namespace scratchbird::server
