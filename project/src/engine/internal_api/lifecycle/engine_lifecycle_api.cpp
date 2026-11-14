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
#include "native_catalog_relation_lease_reader.hpp"
#include "native_database_create.hpp"
#include "native_policy_families.hpp"
#include "catalog_identifier_lookup_key.hpp"
#include "admitted_datatype_cohort.hpp"
#include "node_uuid_issuer.hpp"
#include "extensibility/extensibility_support.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {
namespace {
std::string LifecycleIdentityBytes(const EngineUuid& identity) {
  if (identity.is_nil()) return {};
  return {reinterpret_cast<const char*>(identity.bytes.data()), identity.bytes.size()};
}


bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

// SEARCH_KEY: SB_LIFECYCLE_OPTION_WHOLE_KEY_MATCH (owner Q110 = 1,
// 2026-11-14). An option check matches whole option entries by key, never a
// substring anywhere in an option: option values carry user text (principal
// names, an audit reason) and raw binary UUIDs, and a substring match let that
// text trigger or defeat a check. Three token shapes:
//   "key:"        the key is present (the entry starts with "key:");
//   "key:value"   the entry is exactly "key:value";
//   "key"         the entry is exactly "key" or starts with "key:".
bool OptionTokenMatches(const std::string& option, const std::string& token) {
  if (token.empty()) return false;
  if (token.back() == ':') return StartsWith(option, token);
  if (token.find(':') != std::string::npos) return option == token;
  return option == token ||
         (option.size() > token.size() && StartsWith(option, token) && option[token.size()] == ':');
}

bool HasOptionToken(const EngineApiRequest& request, const std::string& token) {
  for (const auto& option : request.option_envelopes) {
    if (OptionTokenMatches(option, token)) { return true; }
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

std::uint32_t OptionU32(const EngineApiRequest& request,
                        const std::string& prefix,
                        std::uint32_t fallback) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) return fallback;
  std::uint64_t parsed = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') return fallback;
    parsed = parsed * 10u + static_cast<std::uint64_t>(ch - '0');
    if (parsed > UINT32_MAX) return fallback;
  }
  return static_cast<std::uint32_t>(parsed);
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
  // Retain the engine-issued statement identity; request_id is a label.
  config.operation_uuid = request.context.statement_uuid;
  config.actor_uuid = request.context.principal_uuid;
  config.write_evidence = write_evidence;
  return config;
}

scratchbird::storage::database::DatabaseLifecycleRepairConfig LifecycleRepairConfig(
    const EngineRepairLifecycleRequest& request) {
  scratchbird::storage::database::DatabaseLifecycleRepairConfig config;
  config.path = request.context.database_path;
  config.cluster_authority_available = request.context.cluster_authority_available;
  config.decryption_available = OptionBool(request, "decryption_available:", false);
  // Retain the engine-issued statement identity; request_id is a label.
  config.operation_uuid = request.context.statement_uuid;
  config.actor_uuid = request.context.principal_uuid;
  config.repair_plan_id = OptionValue(request, "repair_plan_id:");
  if (config.repair_plan_id.empty()) { config.repair_plan_id = OptionValue(request, "repair_plan:"); }
  config.expected_database_uuid = OptionValue(request, "expected_database_uuid:");
  if (config.expected_database_uuid.empty()) { config.expected_database_uuid = LifecycleIdentityBytes(request.context.database_uuid); }
  config.expected_filespace_uuid = OptionValue(request, "expected_filespace_uuid:");
  config.repair_admission_proven = OptionBool(request, "repair_admission_proven:", false) ||
                                   OptionBool(request, "restricted_or_maintenance_admission:", false);
  config.allow_mutation = OptionBool(request, "allow_repair:", false) ||
                          OptionBool(request, "allow_mutation:", false) ||
                          OptionBool(request, "repair_execute:", false);
  config.engine_read_identity_proof =
      OptionBool(request, "engine_read_identity_proof:", false);
  return config;
}

scratchbird::storage::database::DatabaseDropConfig LifecycleDropConfig(
    const EngineDropLifecycleRequest& request) {
  scratchbird::storage::database::DatabaseDropConfig config;
  config.path = request.context.database_path;
  config.cluster_authority_available = request.context.cluster_authority_available;
  config.decryption_available = OptionBool(request, "decryption_available:", false);
  // Retain the engine-issued statement identity; request_id is a label.
  config.operation_uuid = request.context.statement_uuid;
  config.actor_uuid = request.context.principal_uuid;
  config.drop_mode = OptionValue(request, "drop_mode:");
  if (config.drop_mode.empty()) config.drop_mode = "logical";
  config.expected_database_uuid = OptionValue(request, "expected_database_uuid:");
  if (config.expected_database_uuid.empty() && OptionValue(request, "database_path:").empty()) {
    config.expected_database_uuid = LifecycleIdentityBytes(request.context.database_uuid);
  }
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

std::uint64_t OptionU64(const EngineApiRequest& request,
                        const std::string& prefix,
                        std::uint64_t fallback) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) return fallback;
  std::uint64_t parsed = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') return fallback;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (UINT64_MAX - digit) / 10u) return fallback;
    parsed = parsed * 10u + digit;
  }
  return parsed;
}

const char* NativeCreateErrorName(scratchbird::storage::database::NativeDatabaseCreateError error) {
  using E = scratchbird::storage::database::NativeDatabaseCreateError;
  switch (error) {
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::invalid_principal: return "invalid_principal";
    case E::identity_failure: return "identity_failure";
    case E::policy_assembly: return "policy_assembly";
    case E::database_assembly: return "database_assembly";
    case E::schema_assembly: return "schema_assembly";
    case E::agent_assembly: return "agent_assembly";
    case E::audit_failure: return "audit_failure";
    case E::index_failure: return "index_failure";
    case E::capacity_exceeded: return "capacity_exceeded";
    case E::workspace_failure: return "workspace_failure";
    case E::resource_exhausted: return "resource_exhausted";
    case E::datatype_assembly: return "datatype_assembly";
    case E::name_path: return "name_path";
  }
  return "unknown";
}

// SEARCH_KEY: SB_ENGINE_CREATE_DATABASE_NATIVE (owner Q92/Q93/Q95/Q97/Q100 = 1,
// 2026-11-13; plan 31 step S2). CREATE DATABASE in the native single-file
// format through the production engine API. Authorised by the caller's server
// session (ValidateLifecycleAuthority, Q95); the construction transaction the
// native create writes is the MGA evidence, so no transaction is required
// before the database exists. Every input is validated before the file is
// opened; the file is opened by OpenNewNativeDatabaseFile only (O_EXCL + flock
// on the data file, no lock file, S1). The result carries the engine's receipt
// identities. Input sources (owner questions where marked, S2 report):
//   name, language     the request's typed fields (descriptor v2): data, and a
//                      catalog_language constant
//   page size          option page_size: (the API's existing option and its
//                      16384 default); 8K-128K only, 4K refused (Q100)
//   total pages        option total_pages:, required, no default (owner Q105 = 1)
//   policy pack        the server's own configuration only (owner Q104 = 2,
//                      SB_SERVER_POLICY_SEED_PACK_ROOT): the context's
//                      server_policy_seed_pack_root, required; a client
//                      option policy_seed_pack_root: is refused
//   principals         options bootstrap_principal_name:/..._fingerprint: and
//                      internal_service_principal_name:/..._fingerprint:, the
//                      legacy create's options; native create requires both
//   creation time      the engine clock, as legacy create
//   database identity  minted here (GenerateEngineIdentityV7): ONE UUIDv7 from
//                      the local clock at file creation; page zero, the
//                      database catalog object key, the schema-tree root and
//                      the name scope (owner Q111 = 1, SB_ONE_DATABASE_UUID)
//   policy snapshot    minted here: the construction transaction's policy
//                      snapshot reference (issuer binding, transaction header,
//                      construction intent); not a database identity
//   engine identity    a fresh runtime identity per create, as the hosted
//                      runtime issues one per owner lifetime (owner Q106 = 1:
//                      until a server engine-instance identity exists)
//   issuer policy      the local machine's clock and standard RFC 9562 UUIDv7
//                      (owner Q107, kStandardLocalClockUuidV7Policy)
//   datatype cohort    the build's cohort V11, as legacy create writes it
//   retained image     NativeHostedSourceBackingBytes(page size)
EngineCreateLifecycleResult NativeCreateLifecycle(const EngineCreateLifecycleRequest& request,
                                                  const char* operation) {
  namespace db = scratchbird::storage::database;
  namespace disk = scratchbird::storage::disk;
  const auto refuse = [&](const char* code, const std::string& detail) {
    return LifecycleFailure<EngineCreateLifecycleResult>(request.context, operation, code, detail);
  };
  // SEARCH_KEY: SB_CATALOG_IDENTIFIER_PROFILE_LENGTH_RULE (owner Q127 = 3,
  // 2026-11-14). The name's maximum length is the server pack's identifier
  // profile's, checked below once the pack is read (still before the open).
  // (Was: `request.database_name.size() > 1024u` here, a hand-written cap
  // refused with the detail database_name_required_as_data.)
  if (request.database_name.empty()) {
    return refuse("DATABASE.CREATE_NAME_INVALID", "database_name_required_as_data");
  }
  // SEARCH_KEY: SB_CATALOG_IDENTIFIER_TEXT_RULE (owner Q122 = 1, 2026-11-14).
  // The catalog's own name rule (the one the name entry builder applies at
  // database assembly, AdmitCatalogIdentifierText) is applied here, before the
  // file is opened: a name the catalog would refuse (U+0000, ill-formed or
  // non-canonical UTF-8) never creates a file. The identifier profile's own
  // rules (maximum length, quoting) are still checked at assembly; the pack's
  // profile sets no maximum length today and bootstrap names are unquoted.
  // (Was: such a name was refused at database assembly, after the O_EXCL open,
  // and Q108 then removed the still-empty file.)
  // Update 2026-11-14 (Q127 = 3, Q130 = 2): the two statements above about
  // the profile's maximum length are no longer true. The pack's profile now
  // declares a maximum (default pack: 1024 encoded octets), the database name
  // is held to it below (SB_CATALOG_IDENTIFIER_PROFILE_LENGTH_RULE), and
  // every other assembled name is held to it before the open by the plan
  // (SB_NATIVE_CREATE_NAMES_UNDER_PACK_PROFILE). Assembly still applies the
  // same rule again after the open.
  if (scratchbird::core::catalog::AdmitCatalogIdentifierText(request.database_name) !=
      scratchbird::core::catalog::CatalogIdentifierKeyError::none) {
    return refuse("DATABASE.CREATE_NAME_INVALID", "database_name_refused_by_catalog_name_rule");
  }
  if (!scratchbird::core::catalog::IsKnownCatalogLanguage(request.database_language)) {
    return refuse("DATABASE.CREATE_LANGUAGE_UNKNOWN", "language_must_be_a_catalog_language_constant");
  }
  const auto page_size = OptionU32(request, "page_size:", 16384u);
  const auto* profile = disk::FindCanonicalFilespacePageProfileForSize(page_size);
  if (profile == nullptr) {
    // Q100: the native profiles are 8K-128K; 4K is not in the slice.
    return refuse("DATABASE.CREATE_PAGE_SIZE_UNSUPPORTED",
                  "native_page_size_profile_8k_to_128k_only");
  }
  const auto total_pages = OptionU64(request, "total_pages:", 0u);
  if (total_pages == 0) {
    return refuse("DATABASE.CREATE_TOTAL_PAGES_REQUIRED", "native_total_pages_required");
  }
  // SEARCH_KEY: SB_SERVER_POLICY_SEED_PACK_ROOT (owner Q104 = 2, 2026-11-13).
  // The policy pack is the server's, from its own configuration
  // (ServerBootstrapConfig::database_policy_seed_pack_root), carried in the
  // context's server-only field. A client never chooses it: a request option
  // naming it is refused (exact option prefix, not a substring match), and
  // without the server's value the create is refused. Nothing is opened yet.
  for (const auto& option : request.option_envelopes) {
    if (option.starts_with("policy_seed_pack_root:")) {
      return refuse("DATABASE.CREATE_POLICY_PACK_CLIENT_REFUSED",
                    "native_policy_seed_pack_root_is_server_configuration");
    }
  }
  const auto& policy_root = request.context.server_policy_seed_pack_root;
  if (policy_root.empty()) {
    return refuse("DATABASE.CREATE_POLICY_PACK_REQUIRED", "native_server_policy_seed_pack_root_required");
  }
  // SEARCH_KEY: SB_CATALOG_IDENTIFIER_PROFILE_LENGTH_RULE (owner Q127 = 3,
  // 2026-11-14). The server's pack is read here, before the file is opened
  // (LoadNativePolicyFamilies reads only the pack), and the database name is
  // held to the maximum length of the identifier profile the create names it
  // under: the pack's first identifier profile, as database assembly uses
  // (CreateNativeDatabaseOnOpenDevice, "the pack's own identifier profile
  // names everything else"). One rule, shared with key derivation
  // (AdmitCatalogIdentifierProfileLength). A pack that does not load refuses
  // here too. (Was: a pack that did not load was refused after the O_EXCL
  // open, and Q108 removed the still-empty file.)
  {
    const auto families = db::LoadNativePolicyFamilies(policy_root);
    if (!families.ok() || !families.seeds || families.seeds->identifier_profiles.empty()) {
      return refuse("DATABASE.CREATE_POLICY_PACK_INVALID",
                    "native_server_policy_pack_refused_before_open:" + families.diagnostic.diagnostic_code);
    }
    const auto& naming = families.seeds->identifier_profiles.front();
    scratchbird::core::platform::u64 scalars = 0;
    if (scratchbird::core::catalog::AdmitCatalogIdentifierText(request.database_name, &scalars) !=
            scratchbird::core::catalog::CatalogIdentifierKeyError::none ||
        scratchbird::core::catalog::AdmitCatalogIdentifierProfileLength(
            request.database_name, scalars, naming.maximum_identifier_length, naming.length_unit) !=
            scratchbird::core::catalog::CatalogIdentifierKeyError::none) {
      return refuse("DATABASE.CREATE_NAME_INVALID", "database_name_longer_than_policy_identifier_profile_maximum");
    }
  }
  const auto bootstrap_name = OptionValue(request, "bootstrap_principal_name:");
  const auto bootstrap_fingerprint = OptionValue(request, "bootstrap_credential_fingerprint:");
  const auto service_name = OptionValue(request, "internal_service_principal_name:");
  const auto service_fingerprint = OptionValue(request, "internal_service_credential_fingerprint:");
  if (bootstrap_name.empty() || bootstrap_fingerprint.empty() ||
      service_name.empty() || service_fingerprint.empty()) {
    // The credentialed bootstrap principal and the L5 service principal are
    // both required by native create (owner 2026-11-04); nothing is defaulted.
    return refuse("DATABASE.CREATE_PRINCIPAL_REQUIRED",
                  "native_bootstrap_and_service_principals_required");
  }

  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto database_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database, now);
  const auto policy_snapshot_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, now);
  const auto engine_uuid = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!database_uuid.ok() || !policy_snapshot_uuid.ok() || !engine_uuid) {
    return refuse("DATABASE.CREATE_FAILED", "engine_owned_database_identity_generation_failed");
  }
  // Q107 (owner 2026-11-14): local clock, standard RFC 9562 UUIDv7.
  scratchbird::core::uuid::StandaloneUuidV7Issuer issuer(
      {database_uuid.value.value, policy_snapshot_uuid.value.value},
      scratchbird::core::uuid::kStandardLocalClockUuidV7Policy);

  db::NativeDatabaseCreateRequest create;
  create.policy_seed_pack_root = policy_root;
  create.database_name = request.database_name;
  create.database_default_language = request.database_language;
  create.page_size_profile_uuid = profile->uuid;
  create.page_size_bytes = profile->page_size_bytes;
  create.total_pages = total_pages;
  create.creation_utc_millis = now;
  create.engine_uuid = *engine_uuid;
  create.datatype_cohort = {{scratchbird::core::platform::UuidKind::object,
                             scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
  create.bootstrap_principal_name = bootstrap_name;
  create.bootstrap_credential_fingerprint = bootstrap_fingerprint;
  create.service_principal_name = service_name;
  create.service_credential_fingerprint = service_fingerprint;
  create.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(profile->page_size_bytes);

  // SEARCH_KEY: SB_NATIVE_CREATE_NAMES_UNDER_PACK_PROFILE (owner Q130 = 2,
  // 2026-11-14). Every name the create assembles (the pack's and the
  // engine's, the two principal names, the schema tree, agent declarations,
  // built-in datatypes and domains, catalog index rows) is held to the
  // maximum length of the identifier profile it is named under, BEFORE the
  // file is opened. The names are not listed here: the create's own assembly
  // is run without a device (PlanNativeDatabaseCreate, the same body as
  // CreateNativeDatabaseOnOpenDevice, stopped before its first write), so
  // the names checked are exactly the names the create writes. Its issuer is
  // a separate one bound like the create's; its identities are discarded.
  // A plan refused for a name over the profile maximum refuses here, naming
  // the kind of name, never its text. A plan refused for any other reason
  // is not acted on here: the create below meets the same refusal after the
  // open, as before (Q108 removes the empty file).
  // (Was: such a name was refused at assembly after the O_EXCL open,
  // DATABASE.CREATE_FAILED native_create_refused:policy_assembly (or the
  // stage's error), and Q108 removed the still-empty file.)
  {
    scratchbird::core::uuid::StandaloneUuidV7Issuer plan_issuer(
        {database_uuid.value.value, policy_snapshot_uuid.value.value},
        scratchbird::core::uuid::kStandardLocalClockUuidV7Policy);
    const auto plan = db::PlanNativeDatabaseCreate(create, plan_issuer);
    using N = db::NativeCreateNameClass;
    if (plan.name_over_profile_maximum == N::bootstrap_principal ||
        plan.name_over_profile_maximum == N::service_principal) {
      // The caller's own text, like the database name: a name refusal.
      return refuse("DATABASE.CREATE_NAME_INVALID",
                    std::string(db::NativeCreateNameClassName(plan.name_over_profile_maximum)) +
                        "_longer_than_policy_identifier_profile_maximum");
    }
    if (plan.name_over_profile_maximum != N::none) {
      // A name the pack or the engine supplies: the pack's profile cannot
      // name this database's own objects.
      return refuse("DATABASE.CREATE_POLICY_PACK_INVALID",
                    std::string("native_create_name_longer_than_policy_identifier_profile_maximum:") +
                        db::NativeCreateNameClassName(plan.name_over_profile_maximum));
    }
  }

  disk::FileDevice device;
  const auto opened = db::OpenNewNativeDatabaseFile(device, request.context.database_path);
  if (!opened.ok()) {
    return refuse("DATABASE.CREATE_FAILED",
                  "native_database_file_open_refused:" + opened.diagnostic.diagnostic_code);
  }
  const auto made = db::CreateNativeDatabaseOnOpenDevice(device, create, issuer);
  if (!made.ok()) {
    // SEARCH_KEY: SB_NATIVE_CREATE_EMPTY_FILE_UNLINK (owner Q108 = 1,
    // 2026-11-13). The refusal came after our O_EXCL open. While we still
    // hold the flock, our own file is removed only if it is still empty and
    // the path still names it; then the device is closed. A file that is not
    // empty, or a path that names another file, is left alone.
    const auto removal = device.RemoveAndCloseIfStillEmptyNativeCreate();
    const bool removed =
        removal == disk::FileDevice::EmptyNativeCreateRemoval::removed;
    return refuse("DATABASE.CREATE_FAILED",
                  std::string("native_create_refused:") + NativeCreateErrorName(made.error) +
                      (removed ? ";empty_file_removed" : ";empty_file_not_removed"));
  }
  const auto closed = device.Close();
  if (!closed.ok()) {
    return refuse("DATABASE.CREATE_FAILED",
                  "native_database_file_close_failed:" + closed.diagnostic.diagnostic_code);
  }
  const auto& receipt = *made.receipt;
  auto result = MakeApiBehaviorSuccess<EngineCreateLifecycleResult>(request.context, operation);
  result.primary_object.uuid = receipt.database_uuid;
  result.primary_object.object_kind = "database";
  result.transaction_uuid = receipt.construction_transaction.transaction_uuid.value;
  AddApiBehaviorRow(&result,
                    {{"operation_uuid", request.context.statement_uuid},
                     {"database_uuid", receipt.database_uuid},
                     {"filespace_uuid", receipt.filespace_uuid},
                     {"construction_transaction_uuid", receipt.construction_transaction.transaction_uuid.value},
                     {"bootstrap_principal_uuid", receipt.bootstrap_principal_uuid},
                     {"service_principal_uuid", receipt.service_principal_uuid},
                     {"service_credential_reference_uuid", receipt.service_credential_reference_uuid},
                     {"service_provider_uuid", receipt.service_provider_uuid},
                     {"service_policy_uuid", receipt.service_policy_uuid},
                     {"service_runtime_binding_uuid", receipt.service_runtime_binding_uuid},
                     {"page_size_profile_uuid", profile->uuid},
                     {"database_language_uuid", request.database_language},
                     {"database_format", std::string("native")},
                     {"lifecycle_state", std::string("created")},
                     // Construction only: the file stays initializing; open,
                     // verification and serving are the server's (S3).
                     {"creation_handoff", std::string("constructed_and_closed")},
                     {"server_ready", std::string("false")},
                     {"creation_request_id", request.context.request_id}});
  AddApiBehaviorEvidence(&result, "engine_lifecycle", "database_created");
  AddApiBehaviorEvidence(&result, "identity_authority", "engine");
  AddApiBehaviorEvidence(&result, "database_format", "native_single_file");
  AddApiBehaviorEvidence(&result, "mga_lifecycle_evidence", "construction_transaction_committed");
  return result;
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
  auto authority = ValidateLifecycleAuthority<EngineCreateLifecycleResult>(
      request, operation, true);
  if (!authority.ok) return authority;
  const bool native =
      request.database_format == EngineDatabaseFormat::native;
  // SEARCH_KEY: SB_DRIFT_CREATE_DATABASE_MANAGEMENT_TRANSACTION (owner Q95 = 1,
  // 2026-11-13; plan 31 step S2). Requiring a management transaction before
  // the database exists is drift: there is no transaction inventory to own
  // one. A CREATE DATABASE is authorised by the caller's server session (the
  // security context checked above); the engine's own construction
  // transaction is the MGA evidence. The native format does not require it;
  // the legacy create keeps it unchanged until workstream C (Q93).
  if (!native && (request.context.transaction_uuid.is_nil() ||
                  request.context.local_transaction_id == 0)) {
    return LifecycleFailure<EngineCreateLifecycleResult>(
        request.context, operation, "MGA.TRANSACTION.INVALID",
        "authenticated_management_transaction_required");
  }
  if (request.context.cluster_transaction_active ||
      request.context.route_fence_present) {
    return LifecycleFailure<EngineCreateLifecycleResult>(
        request.context, operation,
        "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
        "cluster_create_requires_registered_cluster_gateway");
  }
  if (request.context.query_cancellation_requested &&
      request.context.query_cancellation_requested()) {
    return LifecycleFailure<EngineCreateLifecycleResult>(
        request.context, operation, "PROCESS.CANCELLED",
        "database_create_cancelled_before_bootstrap");
  }
  if (native) return NativeCreateLifecycle(request, operation);

  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto database_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database, now);
  const auto filespace_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::filespace, now + 1u);
  if (!database_uuid.ok() || !filespace_uuid.ok()) {
    return LifecycleFailure<EngineCreateLifecycleResult>(
        request.context, operation, "DATABASE.CREATE_FAILED",
        "engine_owned_database_identity_generation_failed");
  }

  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = request.context.database_path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = OptionU32(request, "page_size:", 16384u);
  create.creation_unix_epoch_millis = now;
  create.resource_seed_pack_root = OptionValue(request, "resource_seed_pack_root:");
  create.metric_policy_defaults_path = OptionValue(request, "metric_policy_defaults_path:");
  create.metric_policy_families_path = OptionValue(request, "metric_policy_families_path:");
  create.allow_minimal_resource_bootstrap =
      OptionBool(request, "allow_minimal_resource_bootstrap:", false);
  create.require_resource_seed_pack =
      !create.allow_minimal_resource_bootstrap;
  create.bootstrap_principal_name =
      OptionValue(request, "bootstrap_principal_name:");
  create.bootstrap_credential_fingerprint =
      OptionValue(request, "bootstrap_credential_fingerprint:");
  create.require_bootstrap_principal =
      !create.bootstrap_principal_name.empty();
  create.allow_uncredentialed_bootstrap = false;
  // Internal principals, per `SECURITY-INTERNAL-PRINCIPAL-PROVISIONING`. The
  // caller supplies the name and a real credential fingerprint for the internal
  // service principal, and may declare additional internal actor principals as
  // repeated option envelopes. Nothing is defaulted: an absent option
  // provisions no principal, and a present name with an unparseable credential
  // refuses create rather than provisioning a principal nothing can
  // authenticate as.
  create.internal_service_principal_name =
      OptionValue(request, "internal_service_principal_name:");
  create.internal_service_credential_fingerprint =
      OptionValue(request, "internal_service_credential_fingerprint:");
  create.require_internal_service_principal =
      !create.internal_service_principal_name.empty();
  for (const auto& envelope : request.option_envelopes) {
    static constexpr std::string_view kInternalPrincipal = "internal_principal:";
    if (envelope.size() <= kInternalPrincipal.size() ||
        envelope.compare(0, kInternalPrincipal.size(), kInternalPrincipal) != 0) {
      continue;
    }
    // name=<principal>;credential=<fingerprint>
    const std::string body = envelope.substr(kInternalPrincipal.size());
    static constexpr std::string_view kName = "name=";
    static constexpr std::string_view kCredential = ";credential=";
    const auto split = body.find(kCredential);
    if (split == std::string::npos ||
        body.compare(0, kName.size(), kName) != 0) {
      continue;
    }
    create.internal_principals.push_back(
        {body.substr(kName.size(), split - kName.size()),
         body.substr(split + kCredential.size())});
  }
  create.allow_overwrite = false;

  const auto created =
      scratchbird::storage::database::CreateDatabaseFile(create);
  if (!created.ok()) {
    return LifecycleStorageFailure<EngineCreateLifecycleResult>(
        request, operation, created, "DATABASE.CREATE_FAILED");
  }

  auto result = MakeApiBehaviorSuccess<EngineCreateLifecycleResult>(
      request.context, operation);
  result.primary_object.uuid = created.state.database_uuid.value;
  result.primary_object.object_kind = "database";
  AddApiBehaviorRow(&result,
                    {{"operation_uuid", request.context.statement_uuid},
                     {"database_uuid", created.state.database_uuid.value},
                     {"filespace_uuid", created.state.filespace_uuid.value},
                     {"lifecycle_state", "created"},
                     {"creation_handoff", created.creation_ready_for_handoff()
                         ? "verified_and_closed" : "verification_required"},
                     {"server_ready", "false"},
                     {"publication_barrier", "durable"},
                     {"creation_request_id", request.context.request_id}});
  AddApiBehaviorEvidence(&result, "engine_lifecycle", "database_created");
  AddApiBehaviorEvidence(&result, "identity_authority", "engine");
  AddApiBehaviorEvidence(&result, "mga_lifecycle_evidence", "tx1_committed");
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
  const auto clean = scratchbird::storage::database::MarkDatabaseCleanShutdown(
      request.context.database_path, request.context.database_uuid);
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
  EngineUuid acknowledger_uuid, process_uuid, route_uuid;
  bool has_acknowledger = false, has_process = false, has_route = false;
  const auto read_identity = [&](std::string_view prefix, EngineUuid* identity, bool* present) {
    for (const auto& option : request.option_envelopes) {
      if (!option.starts_with(prefix)) continue;
      if (*present || !ReadMetadataUuid(std::string_view(option).substr(prefix.size()), identity))
        return false;
      *present = true;
    }
    return true;
  };
  if (!read_identity("acknowledger_uuid:", &acknowledger_uuid, &has_acknowledger) ||
      !read_identity("process_uuid:", &process_uuid, &has_process) ||
      !read_identity("route_uuid:", &route_uuid, &has_route)) {
    return LifecycleFailure<EngineAcknowledgeShutdownLifecycleResult>(
        request.context, operation, "ENGINE.SHUTDOWN_ACK_INVALID",
        "shutdown_acknowledge_requires_unique_binary_v7_identities");
  }
  if (!has_acknowledger && has_process) acknowledger_uuid = process_uuid;
  const std::string acknowledgement_generation = OptionValue(request, "acknowledgement_generation:");
  std::string acknowledgement_state = OptionValue(request, "acknowledgement_state:");
  if (acknowledgement_state.empty()) { acknowledgement_state = "acknowledged"; }
  const bool valid_state = acknowledgement_state == "acknowledged" ||
                           acknowledgement_state == "draining" ||
                           acknowledgement_state == "clean_stop_complete";
  if (acknowledger_kind.empty() || acknowledger_uuid.is_nil() || acknowledgement_generation.empty() || !valid_state) {
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
    ApiBehaviorFields fields{{"lifecycle_state", "shutdown_acknowledged"},
                                {"acknowledger_kind", acknowledger_kind},
                                {"acknowledger_uuid", acknowledger_uuid},
                                {"acknowledgement_generation", acknowledgement_generation},
                                {"acknowledgement_state", acknowledgement_state},
                                {"outstanding_session_count", OptionValue(request, "outstanding_session_count:")},
                                {"deadline_epoch", OptionValue(request, "deadline_epoch:")}};
    if (has_route) fields.emplace_back("route_uuid", route_uuid);
    if (has_process) fields.emplace_back("process_uuid", process_uuid);
    AddApiBehaviorRow(&result, std::move(fields));
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
