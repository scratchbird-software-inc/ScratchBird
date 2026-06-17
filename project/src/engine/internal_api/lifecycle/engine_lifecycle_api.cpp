// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lifecycle/engine_lifecycle_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "database_lifecycle.hpp"
#include "extensibility/extensibility_support.hpp"
#include "uuid.hpp"

#include <chrono>
#include <string>

namespace scratchbird::engine::internal_api {
namespace {

bool Contains(const std::string& value, const std::string& token) { return value.find(token) != std::string::npos; }
bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

bool HasOptionToken(const EngineApiRequest& request, const std::string& token) {
  for (const auto& option : request.option_envelopes) {
    if (Contains(option, token)) { return true; }
  }
  return false;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

bool OptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback = false) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) { return fallback; }
  if (value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on") { return true; }
  if (value == "0" || value == "false" || value == "FALSE" || value == "no" || value == "off") { return false; }
  return fallback;
}

EngineApiDiagnostic LifecycleDiagnostic(const std::string& operation_id,
                                        const std::string& code,
                                        const std::string& detail) {
  return MakeEngineApiDiagnostic(code, "engine.lifecycle", operation_id + ":" + detail, true);
}

template <typename TResult>
TResult LifecycleFailure(const EngineRequestContext& context,
                         const std::string& operation_id,
                         const std::string& code,
                         const std::string& detail) {
  return MakeApiBehaviorDiagnostic<TResult>(context, operation_id, LifecycleDiagnostic(operation_id, code, detail));
}

bool RequiresSecurity(const EngineApiRequest& request) {
  return HasOptionToken(request, "mode:maintenance") ||
         HasOptionToken(request, "mode:restricted_open") ||
         HasOptionToken(request, "attach") ||
         HasOptionToken(request, "detach") ||
         HasOptionToken(request, "shutdown") ||
         HasOptionToken(request, "control") ||
         HasOptionToken(request, "policy:");
}

bool UnsafeProfileCombination(const EngineApiRequest& request) {
  return (HasOptionToken(request, "profile:release_complete") && HasOptionToken(request, "mode:emergency")) ||
         (HasOptionToken(request, "mode:read_only") && HasOptionToken(request, "allow_writes:true")) ||
         (HasOptionToken(request, "mode:restricted_open") && HasOptionToken(request, "allow_writes:true")) ||
         HasOptionToken(request, "unsafe_profile_combo");
}

template <typename TResult>
TResult ValidateLifecycleAuthority(const EngineApiRequest& request,
                                   const std::string& operation_id,
                                   bool force_security) {
  if (request.context.database_path.empty()) {
    return LifecycleFailure<TResult>(request.context, operation_id, "SB_ENGINE_API_LIFECYCLE_PATH_REQUIRED", "database_path_required");
  }
  if ((force_security || RequiresSecurity(request)) && !request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<TResult>(request.context, operation_id, MakeSecurityContextRequiredDiagnostic(operation_id));
  }
  if (!request.context.cluster_authority_available && EngineExtensionRequestsClusterAuthority(request)) {
    return EngineExtensionClusterAuthorityUnavailable<TResult>(request, operation_id);
  }
  if (UnsafeProfileCombination(request)) {
    return LifecycleFailure<TResult>(request.context, operation_id, "SB_ENGINE_API_LIFECYCLE_UNSAFE_PROFILE", "unsafe_profile_combination");
  }
  return MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
}

bool ReadOnlyRequested(const EngineApiRequest& request) {
  return HasOptionToken(request, "mode:read_only") ||
         HasOptionToken(request, "mode:maintenance") ||
         HasOptionToken(request, "mode:restricted_open") ||
         HasOptionToken(request, "read_only:true");
}

std::string RequestedMode(const EngineApiRequest& request, const std::string& fallback) {
  const auto mode = OptionValue(request, "mode:");
  return mode.empty() ? fallback : mode;
}

std::uint64_t CurrentUnixEpochMillisForLifecycle() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::uint32_t RequestedPageSize(const EngineApiRequest& request) {
  const auto value = OptionValue(request, "page_size:");
  if (value.empty()) {
    return static_cast<std::uint32_t>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  }
  try {
    return static_cast<std::uint32_t>(std::stoul(value));
  } catch (...) {
    return 0;
  }
}

bool MinimalBootstrapAllowed(const EngineApiRequest& request) {
  return HasOptionToken(request, "allow_minimal_resource_bootstrap:true");
}

scratchbird::storage::database::DatabaseLifecycleResult OpenDatabaseForLifecycle(
    const EngineApiRequest& request,
    bool read_only) {
  scratchbird::storage::database::DatabaseOpenConfig open;
  open.path = request.context.database_path;
  open.cluster_authority_available = request.context.cluster_authority_available;
  open.read_only = read_only;
  return scratchbird::storage::database::OpenDatabaseFile(open);
}

scratchbird::storage::database::DatabaseLifecycleOperationConfig LifecycleOperationConfig(
    const EngineApiRequest& request,
    bool write_evidence = true) {
  scratchbird::storage::database::DatabaseLifecycleOperationConfig config;
  config.path = request.context.database_path;
  config.cluster_authority_available = request.context.cluster_authority_available;
  config.decryption_available = OptionBool(request, "decryption_available:", false);
  config.operation_uuid = request.context.request_id;
  config.actor_uuid = request.context.principal_uuid.canonical;
  config.write_evidence = write_evidence;
  return config;
}

scratchbird::storage::database::DatabaseLifecycleRepairConfig LifecycleRepairConfig(
    const EngineRepairLifecycleRequest& request) {
  scratchbird::storage::database::DatabaseLifecycleRepairConfig config;
  config.path = request.context.database_path;
  config.cluster_authority_available = request.context.cluster_authority_available;
  config.decryption_available = OptionBool(request, "decryption_available:", false);
  config.operation_uuid = request.context.request_id;
  config.actor_uuid = request.context.principal_uuid.canonical;
  config.repair_plan_id = OptionValue(request, "repair_plan_id:");
  if (config.repair_plan_id.empty()) { config.repair_plan_id = OptionValue(request, "repair_plan:"); }
  config.expected_database_uuid = OptionValue(request, "expected_database_uuid:");
  if (config.expected_database_uuid.empty()) { config.expected_database_uuid = request.context.database_uuid.canonical; }
  config.expected_filespace_uuid = OptionValue(request, "expected_filespace_uuid:");
  config.repair_admission_proven = OptionBool(request, "repair_admission_proven:", false) ||
                                   OptionBool(request, "restricted_or_maintenance_admission:", false);
  config.allow_mutation = OptionBool(request, "allow_repair:", false) ||
                          OptionBool(request, "allow_mutation:", false) ||
                          OptionBool(request, "repair_execute:", false);
  return config;
}

scratchbird::storage::database::DatabaseDropConfig LifecycleDropConfig(
    const EngineDropLifecycleRequest& request) {
  scratchbird::storage::database::DatabaseDropConfig config;
  config.path = request.context.database_path;
  config.cluster_authority_available = request.context.cluster_authority_available;
  config.decryption_available = OptionBool(request, "decryption_available:", false);
  config.operation_uuid = request.context.request_id;
  config.actor_uuid = request.context.principal_uuid.canonical;
  config.drop_mode = OptionValue(request, "drop_mode:");
  if (config.drop_mode.empty()) config.drop_mode = "logical";
  config.expected_database_uuid = OptionValue(request, "expected_database_uuid:");
  if (config.expected_database_uuid.empty()) config.expected_database_uuid = request.context.database_uuid.canonical;
  config.expected_filespace_uuid = OptionValue(request, "expected_filespace_uuid:");
  config.drop_safety_preconditions = OptionBool(request, "drop_safety_preconditions:", false);
  config.session_drain_complete = OptionBool(request, "session_drain_complete:", false);
  config.ownership_release_verified = OptionBool(request, "ownership_release_verified:", false);
  config.retention_policy_satisfied = OptionBool(request, "retention_policy_satisfied:", false);
  config.backup_coverage_verified = OptionBool(request, "backup_coverage_verified:", false);
  config.legal_hold_clear = OptionBool(request, "legal_hold_clear:", false);
  config.allow_physical_delete = OptionBool(request, "allow_physical_delete:", false) ||
                                 OptionBool(request, "physical_delete_policy_approved:", false);
  config.allow_quarantine = OptionBool(request, "allow_quarantine:", false) ||
                            OptionBool(request, "quarantine_policy_approved:", false);
  return config;
}

std::string LifecycleStorageDiagnosticCode(
    const scratchbird::storage::database::DatabaseLifecycleResult& result,
    const std::string& fallback) {
  if (result.diagnostic.diagnostic_code.find("CLUSTER") != std::string::npos ||
      result.diagnostic.diagnostic_code == "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED") {
    return "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED";
  }
  if (fallback.rfind("ENGINE.DBLC_", 0) == 0) {
    return fallback;
  }
  return result.diagnostic.diagnostic_code.empty() ? fallback : result.diagnostic.diagnostic_code;
}

template <typename TResult>
TResult LifecycleStorageFailure(const EngineApiRequest& request,
                                const std::string& operation_id,
                                const scratchbird::storage::database::DatabaseLifecycleResult& opened,
                                const std::string& fallback_code) {
  const auto code = LifecycleStorageDiagnosticCode(opened, fallback_code);
  auto result = LifecycleFailure<TResult>(
      request.context,
      operation_id,
      code,
      opened.diagnostic.message_key.empty() ? code : opened.diagnostic.message_key);
  if (!opened.diagnostic.diagnostic_code.empty() && opened.diagnostic.diagnostic_code != code) {
    result.diagnostics.push_back(MakeEngineApiDiagnostic(
        opened.diagnostic.diagnostic_code,
        opened.diagnostic.message_key.empty() ? "engine.lifecycle.storage" : opened.diagnostic.message_key,
        opened.diagnostic.remediation_hint.empty() ? opened.diagnostic.message_key
                                                   : opened.diagnostic.remediation_hint,
        true));
  }
  result.cluster_authority_required = code == "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED" ||
                                      opened.diagnostic.diagnostic_code.find("CLUSTER") != std::string::npos;
  return result;
}

template <typename TResult>
TResult LifecycleStateResultFromOpened(const EngineApiRequest& request,
                                       const std::string& operation_id,
                                       const std::string& lifecycle_state,
                                       const scratchbird::storage::database::DatabaseLifecycleResult& opened) {
  auto persisted_request = request;
  persisted_request.localized_names.clear();
  persisted_request.localized_names.push_back({"en", "default", "/sys/lifecycle", lifecycle_state, true});
  auto result = PersistedRecordResult<TResult>(persisted_request, operation_id, "engine_lifecycle", false, lifecycle_state);
  if (!result.ok) { return result; }
  AddApiBehaviorRow(&result, {{"lifecycle_state", lifecycle_state},
                              {"mode", RequestedMode(request, lifecycle_state)},
                              {"phase", scratchbird::storage::database::DatabaseLifecyclePhaseName(opened.state.phase)},
                              {"read_only", opened.state.read_only_open ? "true" : "false"},
                              {"write_admission_fenced", opened.state.write_admission_fenced ? "true" : "false"},
                              {"startup_recovery_classification", opened.state.startup_recovery_classification},
                              {"owner_token", opened.state.startup_owner_token},
                              {"config_authority_loaded", opened.state.startup_state.config_authority_loaded ? "true" : "false"},
                              {"security_authority_loaded", opened.state.startup_state.security_authority_loaded ? "true" : "false"},
                              {"i18n_authority_loaded", opened.state.startup_state.i18n_authority_loaded ? "true" : "false"},
                              {"first_open_activation_local_transaction_id", std::to_string(opened.state.startup_state.first_open_activation_local_transaction_id)},
                              {"clean_shutdown_local_transaction_id", std::to_string(opened.state.startup_state.clean_shutdown_local_transaction_id)}});
  AddApiBehaviorEvidence(&result, "engine_lifecycle", lifecycle_state);
  AddApiBehaviorEvidence(&result, "lifecycle_metric", "transition_event_emitted");
  AddApiBehaviorEvidence(&result, "lifecycle_policy", "unsafe_profiles_fail_closed");
  AddApiBehaviorEvidence(&result, "write_admission", opened.state.write_admission_fenced ? "fenced" : "open");
  AddApiBehaviorEvidence(&result, "mga_lifecycle_evidence", "transaction_inventory_recorded");
  return result;
}

template <typename TResult>
TResult OpenAndReport(const EngineApiRequest& request,
                      const std::string& operation_id,
                      const std::string& lifecycle_state,
                      bool force_read_only) {
  const auto opened = OpenDatabaseForLifecycle(request, force_read_only || ReadOnlyRequested(request));
  if (!opened.ok()) {
    return LifecycleStorageFailure<TResult>(request,
                                           operation_id,
                                           opened,
                                           "SB_ENGINE_API_LIFECYCLE_OPEN_FAILED");
  }
  return LifecycleStateResultFromOpened<TResult>(request, operation_id, lifecycle_state, opened);
}

template <typename TResult>
TResult OpenReadOnlyInspection(const EngineApiRequest& request,
                               const std::string& operation_id,
                               const std::string& inspection_kind) {
  const auto opened = inspection_kind == "verified"
      ? scratchbird::storage::database::VerifyDatabaseLifecycle(LifecycleOperationConfig(request, true))
      : scratchbird::storage::database::InspectDatabaseLifecycle(LifecycleOperationConfig(request, false));
  if (!opened.ok()) {
    return LifecycleStorageFailure<TResult>(
        request,
        operation_id,
        opened,
        inspection_kind == "verified" ? "ENGINE.DBLC_VERIFY_FAILED" : "SB_ENGINE_API_LIFECYCLE_OPEN_FAILED");
  }
  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  AddApiBehaviorRow(&result, {{"lifecycle_state", inspection_kind},
                              {"phase", scratchbird::storage::database::DatabaseLifecyclePhaseName(opened.state.phase)},
                              {"database_path", opened.state.path},
                              {"page_size", std::to_string(opened.state.header.page_size)},
                              {"read_only", opened.state.read_only_open ? "true" : "false"},
                              {"write_admission_fenced", opened.state.write_admission_fenced ? "true" : "false"},
                              {"startup_recovery_classification", opened.state.startup_recovery_classification},
                              {"database_open_compatibility_class",
                               scratchbird::storage::database::DatabaseOpenCompatibilityClassName(
                                   opened.state.database_open_compatibility_class)},
                              {"config_authority_loaded", opened.state.startup_state.config_authority_loaded ? "true" : "false"},
                              {"security_authority_loaded", opened.state.startup_state.security_authority_loaded ? "true" : "false"},
                              {"i18n_authority_loaded", opened.state.startup_state.i18n_authority_loaded ? "true" : "false"},
                              {"owner_token_present", opened.state.startup_owner_token.empty() ? "false" : "true"},
                              {"local_transaction_inventory_present", opened.state.local_transaction_inventory_present ? "true" : "false"},
                              {"first_open_activation_local_transaction_id", std::to_string(opened.state.startup_state.first_open_activation_local_transaction_id)},
                              {"clean_shutdown_local_transaction_id", std::to_string(opened.state.startup_state.clean_shutdown_local_transaction_id)},
                              {"redacted_security_fields", "true"}});
  AddApiBehaviorEvidence(&result, "engine_lifecycle", inspection_kind);
  AddApiBehaviorEvidence(&result, "database_file", "read_only_open_classified");
  AddApiBehaviorEvidence(&result, "lifecycle_policy", "read_only_inspection");
  if (inspection_kind == "verified") {
    AddApiBehaviorRow(&result, {{"verification_scope", "database_header_core_pages_startup_state_catalog_transaction_inventory"},
                                {"verification_result", "passed"},
                                {"repairs_performed", "false"}});
    AddApiBehaviorEvidence(&result, "lifecycle_verify", "storage_open_classification_passed");
  }
  return result;
}

bool DropSafetyPreconditionsPresent(const EngineApiRequest& request) {
  const auto drop_mode = OptionValue(request, "drop_mode:");
  const bool supported_mode = drop_mode.empty() ||
                              drop_mode == "logical" ||
                              drop_mode == "logical_preserve" ||
                              drop_mode == "quarantine" ||
                              drop_mode == "physical_delete";
  return supported_mode &&
         OptionBool(request, "drop_safety_preconditions:", false) &&
         OptionBool(request, "session_drain_complete:", false) &&
         OptionBool(request, "ownership_release_verified:", false) &&
         OptionBool(request, "retention_policy_satisfied:", false) &&
         OptionBool(request, "backup_coverage_verified:", false) &&
         OptionBool(request, "legal_hold_clear:", false);
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_LIFECYCLE_API_BEHAVIOR
EngineOpenLifecycleResult EngineOpenLifecycle(const EngineOpenLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.open_database";
  auto authority = ValidateLifecycleAuthority<EngineOpenLifecycleResult>(request, operation, false);
  if (!authority.ok) { return authority; }
  return OpenAndReport<EngineOpenLifecycleResult>(request, operation, RequestedMode(request, "opened"), false);
}

EngineCreateLifecycleResult EngineCreateLifecycle(const EngineCreateLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.create_database";
  auto authority = ValidateLifecycleAuthority<EngineCreateLifecycleResult>(request, operation, false);
  if (!authority.ok) { return authority; }
  const auto now = CurrentUnixEpochMillisForLifecycle();
  const auto database_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database, now);
  if (!database_uuid.ok()) {
    return LifecycleFailure<EngineCreateLifecycleResult>(request.context,
                                                        operation,
                                                        database_uuid.diagnostic.diagnostic_code,
                                                        database_uuid.diagnostic.message_key);
  }
  const auto filespace_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::filespace, now + 1);
  if (!filespace_uuid.ok()) {
    return LifecycleFailure<EngineCreateLifecycleResult>(request.context,
                                                        operation,
                                                        filespace_uuid.diagnostic.diagnostic_code,
                                                        filespace_uuid.diagnostic.message_key);
  }
  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = request.context.database_path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = RequestedPageSize(request);
  create.creation_unix_epoch_millis = now;
  create.resource_seed_pack_root = OptionValue(request, "resource_seed_pack_root:");
  create.allow_minimal_resource_bootstrap = MinimalBootstrapAllowed(request);
  create.require_resource_seed_pack = !create.allow_minimal_resource_bootstrap;
  create.policy_seed_pack_root = OptionValue(request, "policy_seed_pack_root:");
  create.require_policy_seed_pack = !create.allow_minimal_resource_bootstrap;
  create.allow_overwrite = false;
  const auto created = scratchbird::storage::database::CreateDatabaseFile(create);
  if (!created.ok()) {
    auto result = LifecycleFailure<EngineCreateLifecycleResult>(
        request.context,
        operation,
        created.diagnostic.diagnostic_code.empty() ? "SB_ENGINE_API_LIFECYCLE_CREATE_FAILED" : created.diagnostic.diagnostic_code,
        created.diagnostic.message_key);
    result.cluster_authority_required = created.diagnostic.diagnostic_code.find("CLUSTER") != std::string::npos;
    return result;
  }
  auto persisted_request = request;
  persisted_request.localized_names.clear();
  persisted_request.localized_names.push_back({"en", "default", "/sys/lifecycle", "created", true});
  auto result = PersistedRecordResult<EngineCreateLifecycleResult>(persisted_request,
                                                                  operation,
                                                                  "engine_lifecycle",
                                                                  false,
                                                                  "created");
  if (result.ok) {
    result.primary_object.uuid.canonical = scratchbird::core::uuid::UuidToString(created.state.database_uuid.value);
    result.primary_object.object_kind = "database";
    AddApiBehaviorRow(&result, {{"lifecycle_state", "created"},
                                {"phase", scratchbird::storage::database::DatabaseLifecyclePhaseName(created.state.phase)},
                                {"database_path", created.state.path},
                                {"database_uuid", scratchbird::core::uuid::UuidToString(created.state.database_uuid.value)},
                                {"filespace_uuid", scratchbird::core::uuid::UuidToString(created.state.filespace_uuid.value)},
                                {"write_admission_fenced", created.state.write_admission_fenced ? "true" : "false"}});
    AddApiBehaviorEvidence(&result, "engine_lifecycle", "created");
    AddApiBehaviorEvidence(&result, "database_file", "created");
  }
  return result;
}

EngineAttachLifecycleResult EngineAttachLifecycle(const EngineAttachLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.attach_database";
  auto authority = ValidateLifecycleAuthority<EngineAttachLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  return OpenAndReport<EngineAttachLifecycleResult>(request, operation, "attached", false);
}

EngineDetachLifecycleResult EngineDetachLifecycle(const EngineDetachLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.detach_database";
  auto authority = ValidateLifecycleAuthority<EngineDetachLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  auto result = PersistedRecordResult<EngineDetachLifecycleResult>(request, operation, "engine_lifecycle", false, "detached");
  if (result.ok) {
    AddApiBehaviorEvidence(&result, "engine_lifecycle", "detached");
    AddApiBehaviorEvidence(&result, "lifecycle_metric", "transition_event_emitted");
  }
  return result;
}

EngineShutdownLifecycleResult EngineShutdownLifecycle(const EngineShutdownLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.shutdown_database";
  auto authority = ValidateLifecycleAuthority<EngineShutdownLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  const auto clean = scratchbird::storage::database::MarkDatabaseCleanShutdown(request.context.database_path);
  if (!clean.ok()) {
    return LifecycleFailure<EngineShutdownLifecycleResult>(request.context,
                                                          operation,
                                                          clean.diagnostic.diagnostic_code.empty() ? "SB_ENGINE_API_LIFECYCLE_SHUTDOWN_FAILED" : clean.diagnostic.diagnostic_code,
                                                          clean.diagnostic.message_key);
  }
  auto result = PersistedRecordResult<EngineShutdownLifecycleResult>(request, operation, "engine_lifecycle", false, "shutdown_clean");
  if (result.ok) {
    AddApiBehaviorRow(&result, {{"lifecycle_state", "shutdown_clean"}, {"clean_shutdown", "true"}, {"write_admission_fenced", "true"}});
    AddApiBehaviorEvidence(&result, "engine_lifecycle", "shutdown_clean");
    AddApiBehaviorEvidence(&result, "lifecycle_metric", "transition_event_emitted");
  }
  return result;
}

EngineEnterMaintenanceLifecycleResult EngineEnterMaintenanceLifecycle(const EngineEnterMaintenanceLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.enter_maintenance";
  auto authority = ValidateLifecycleAuthority<EngineEnterMaintenanceLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  const auto entered =
      scratchbird::storage::database::EnterDatabaseMaintenanceMode(LifecycleOperationConfig(request, true));
  if (!entered.ok()) {
    return LifecycleStorageFailure<EngineEnterMaintenanceLifecycleResult>(
        request, operation, entered, "SB_ENGINE_API_LIFECYCLE_OPEN_FAILED");
  }
  return LifecycleStateResultFromOpened<EngineEnterMaintenanceLifecycleResult>(
      request, operation, "maintenance", entered);
}

EngineExitMaintenanceLifecycleResult EngineExitMaintenanceLifecycle(const EngineExitMaintenanceLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.exit_maintenance";
  auto authority = ValidateLifecycleAuthority<EngineExitMaintenanceLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  const auto exited =
      scratchbird::storage::database::ExitDatabaseMaintenanceMode(LifecycleOperationConfig(request, true));
  if (!exited.ok()) {
    return LifecycleStorageFailure<EngineExitMaintenanceLifecycleResult>(
        request, operation, exited, "SB_ENGINE_API_LIFECYCLE_OPEN_FAILED");
  }
  auto result = LifecycleStateResultFromOpened<EngineExitMaintenanceLifecycleResult>(
      request, operation, "maintenance_exited", exited);
  if (result.ok) {
    AddApiBehaviorEvidence(&result, "maintenance_exit", "storage_open_reclassified");
    AddApiBehaviorEvidence(&result, "write_admission", "policy_rechecked");
  }
  return result;
}

EngineEnterRestrictedOpenLifecycleResult EngineEnterRestrictedOpenLifecycle(const EngineEnterRestrictedOpenLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.enter_restricted_open";
  auto authority = ValidateLifecycleAuthority<EngineEnterRestrictedOpenLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  const auto entered =
      scratchbird::storage::database::EnterDatabaseRestrictedOpenMode(LifecycleOperationConfig(request, true));
  if (!entered.ok()) {
    return LifecycleStorageFailure<EngineEnterRestrictedOpenLifecycleResult>(
        request, operation, entered, "SB_ENGINE_API_LIFECYCLE_OPEN_FAILED");
  }
  auto result = LifecycleStateResultFromOpened<EngineEnterRestrictedOpenLifecycleResult>(
      request, operation, "restricted_open", entered);
  if (result.ok) {
    AddApiBehaviorEvidence(&result, "restricted_open_allowed_operations",
                           "inspect,diagnostic,verify,repair,backup,recovery,admin");
  }
  return result;
}

EngineExitRestrictedOpenLifecycleResult EngineExitRestrictedOpenLifecycle(const EngineExitRestrictedOpenLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.exit_restricted_open";
  auto authority = ValidateLifecycleAuthority<EngineExitRestrictedOpenLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  const auto exited =
      scratchbird::storage::database::ExitDatabaseRestrictedOpenMode(LifecycleOperationConfig(request, true));
  if (!exited.ok()) {
    return LifecycleStorageFailure<EngineExitRestrictedOpenLifecycleResult>(
        request, operation, exited, "SB_ENGINE_API_LIFECYCLE_OPEN_FAILED");
  }
  auto result = LifecycleStateResultFromOpened<EngineExitRestrictedOpenLifecycleResult>(
      request, operation, "restricted_open_exited", exited);
  if (result.ok) {
    AddApiBehaviorEvidence(&result, "restricted_open_exit", "storage_open_reclassified");
    AddApiBehaviorEvidence(&result, "write_admission", "policy_rechecked");
  }
  return result;
}

EngineInspectLifecycleResult EngineInspectLifecycle(const EngineInspectLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.inspect_database";
  auto authority = ValidateLifecycleAuthority<EngineInspectLifecycleResult>(request, operation, false);
  if (!authority.ok) { return authority; }
  return OpenReadOnlyInspection<EngineInspectLifecycleResult>(request, operation, "inspected");
}

EngineVerifyLifecycleResult EngineVerifyLifecycle(const EngineVerifyLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.verify_database";
  auto authority = ValidateLifecycleAuthority<EngineVerifyLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  return OpenReadOnlyInspection<EngineVerifyLifecycleResult>(request, operation, "verified");
}

EngineRepairLifecycleResult EngineRepairLifecycle(const EngineRepairLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.repair_database";
  auto authority = ValidateLifecycleAuthority<EngineRepairLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  const auto repaired =
      scratchbird::storage::database::RepairDatabaseLifecycle(LifecycleRepairConfig(request));
  if (!repaired.ok()) {
    return LifecycleStorageFailure<EngineRepairLifecycleResult>(
        request, operation, repaired, "ENGINE.DBLC_REPAIR_REFUSED");
  }
  auto result = LifecycleStateResultFromOpened<EngineRepairLifecycleResult>(
      request, operation, "repair_completed", repaired);
  if (result.ok) {
    AddApiBehaviorRow(&result, {{"repair_plan_id", OptionValue(request, "repair_plan_id:")},
                                {"repair_result", "completed"},
                                {"before_after_evidence", "startup_lifecycle_generation_updated"},
                                {"identity_proof", "database_and_filespace_uuid_matched"},
                                {"ordinary_write_admission", "policy_rechecked_after_repair"}});
    AddApiBehaviorEvidence(&result, "lifecycle_repair", "storage_repair_evidence_recorded");
    AddApiBehaviorEvidence(&result, "mga_lifecycle_evidence", "repair_transaction_recorded");
  }
  return result;
}

EngineForceShutdownLifecycleResult EngineForceShutdownLifecycle(const EngineForceShutdownLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.shutdown_force";
  auto authority = ValidateLifecycleAuthority<EngineForceShutdownLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  std::string force_policy_uuid = OptionValue(request, "force_termination_policy_uuid:");
  if (force_policy_uuid.empty()) { force_policy_uuid = OptionValue(request, "force_policy_uuid:"); }
  if (force_policy_uuid.empty()) {
    return LifecycleFailure<EngineForceShutdownLifecycleResult>(
        request.context,
        operation,
        "SB_ENGINE_API_LIFECYCLE_FORCE_POLICY_REQUIRED",
        "shutdown_force_requires_force_termination_policy_uuid");
  }
  if (!OptionBool(request, "association_scope_proven:", false)) {
    return LifecycleFailure<EngineForceShutdownLifecycleResult>(
        request.context,
        operation,
        "SB_ENGINE_API_LIFECYCLE_FORCE_SCOPE_REQUIRED",
        "shutdown_force_requires_database_association_scope_proof");
  }
  if (!OptionBool(request, "recovery_evidence_preserved:", false)) {
    return LifecycleFailure<EngineForceShutdownLifecycleResult>(
        request.context,
        operation,
        "SB_ENGINE_API_LIFECYCLE_FORCE_RECOVERY_EVIDENCE_REQUIRED",
        "shutdown_force_requires_mga_recovery_evidence_preserved");
  }
  auto result = PersistedRecordResult<EngineForceShutdownLifecycleResult>(
      request,
      operation,
      "engine_lifecycle",
      false,
      "shutdown_force_evidence_recorded");
  if (result.ok) {
    AddApiBehaviorRow(&result, {{"lifecycle_state", "shutdown_force_evidence_recorded"},
                                {"shutdown_mode", "force"},
                                {"force_termination_policy_uuid", force_policy_uuid},
                                {"association_scope_proven", "true"},
                                {"recovery_evidence_preserved", "true"},
                                {"unsafe_process_kill_performed", "false"}});
    AddApiBehaviorEvidence(&result, "engine_lifecycle", "shutdown_force_evidence_recorded");
    AddApiBehaviorEvidence(&result, "database_association_scope", "proven");
    AddApiBehaviorEvidence(&result, "mga_recovery_evidence", "preserved");
  }
  return result;
}

EngineAcknowledgeShutdownLifecycleResult EngineAcknowledgeShutdownLifecycle(
    const EngineAcknowledgeShutdownLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.shutdown_acknowledge";
  auto authority = ValidateLifecycleAuthority<EngineAcknowledgeShutdownLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  const std::string acknowledger_kind = OptionValue(request, "acknowledger_kind:");
  std::string acknowledger_uuid = OptionValue(request, "acknowledger_uuid:");
  if (acknowledger_uuid.empty()) { acknowledger_uuid = OptionValue(request, "process_uuid:"); }
  const std::string acknowledgement_generation = OptionValue(request, "acknowledgement_generation:");
  std::string acknowledgement_state = OptionValue(request, "acknowledgement_state:");
  if (acknowledgement_state.empty()) { acknowledgement_state = "acknowledged"; }
  const bool valid_state = acknowledgement_state == "acknowledged" ||
                           acknowledgement_state == "draining" ||
                           acknowledgement_state == "clean_stop_complete";
  if (acknowledger_kind.empty() || acknowledger_uuid.empty() || acknowledgement_generation.empty() || !valid_state) {
    return LifecycleFailure<EngineAcknowledgeShutdownLifecycleResult>(
        request.context,
        operation,
        "ENGINE.SHUTDOWN_ACK_INVALID",
        "shutdown_acknowledge_requires_acknowledger_kind_uuid_generation_and_valid_state");
  }
  auto result = PersistedRecordResult<EngineAcknowledgeShutdownLifecycleResult>(
      request,
      operation,
      "engine_lifecycle",
      false,
      "shutdown_acknowledged");
  if (result.ok) {
    AddApiBehaviorRow(&result, {{"lifecycle_state", "shutdown_acknowledged"},
                                {"acknowledger_kind", acknowledger_kind},
                                {"acknowledger_uuid", acknowledger_uuid},
                                {"acknowledgement_generation", acknowledgement_generation},
                                {"acknowledgement_state", acknowledgement_state},
                                {"route_uuid", OptionValue(request, "route_uuid:")},
                                {"process_uuid", OptionValue(request, "process_uuid:")},
                                {"outstanding_session_count", OptionValue(request, "outstanding_session_count:")},
                                {"deadline_epoch", OptionValue(request, "deadline_epoch:")}});
    AddApiBehaviorEvidence(&result, "engine_lifecycle", "shutdown_acknowledged");
    AddApiBehaviorEvidence(&result, "shutdown_acknowledgement", acknowledger_uuid);
  }
  return result;
}

EngineDropLifecycleResult EngineDropLifecycle(const EngineDropLifecycleRequest& request) {
  constexpr const char* operation = "lifecycle.drop_database";
  auto authority = ValidateLifecycleAuthority<EngineDropLifecycleResult>(request, operation, true);
  if (!authority.ok) { return authority; }
  if (!DropSafetyPreconditionsPresent(request)) {
    return LifecycleFailure<EngineDropLifecycleResult>(
        request.context,
        operation,
        "SB_ENGINE_API_LIFECYCLE_DROP_PRECONDITIONS_NOT_SATISFIED",
        "drop_database_requires_logical_mode_drain_ownership_retention_backup_and_legal_hold_proof");
  }
  const auto dropped =
      scratchbird::storage::database::DropDatabaseLifecycle(LifecycleDropConfig(request));
  if (!dropped.ok()) {
    return LifecycleStorageFailure<EngineDropLifecycleResult>(
        request, operation, dropped, "ENGINE.DBLC_DROP_UNSAFE");
  }
  auto result = LifecycleStateResultFromOpened<EngineDropLifecycleResult>(
      request, operation, "drop_evidence_recorded", dropped);
  if (result.ok) {
    AddApiBehaviorRow(&result, {{"lifecycle_state", "drop_evidence_recorded"},
                                {"drop_mode", OptionValue(request, "drop_mode:").empty()
                                                  ? "logical"
                                                  : OptionValue(request, "drop_mode:")},
                                {"session_drain_complete", "true"},
                                {"ownership_release_verified", "true"},
                                {"retention_policy_satisfied", "true"},
                                {"backup_coverage_verified", "true"},
                                {"legal_hold_clear", "true"},
                                {"storage_file_deleted",
                                 OptionValue(request, "drop_mode:") == "physical_delete" ? "true" : "false"},
                                {"storage_quarantined",
                                 OptionValue(request, "drop_mode:") == "quarantine" ? "true" : "false"}});
    AddApiBehaviorEvidence(&result, "engine_lifecycle", "drop_evidence_recorded");
    AddApiBehaviorEvidence(&result,
                           "database_file",
                           OptionValue(request, "drop_mode:") == "physical_delete"
                               ? "deleted_after_drop_evidence"
                               : OptionValue(request, "drop_mode:") == "quarantine"
                                     ? "quarantined_after_drop_evidence"
                                     : "preserved");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
