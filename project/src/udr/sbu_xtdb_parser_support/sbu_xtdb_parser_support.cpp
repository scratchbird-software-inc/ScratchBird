// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbu_xtdb_parser_support.hpp"

#include "xtdb_dialect.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::udr::xtdb_parser_support {
namespace {

struct ContextField {
  std::string name;
  std::string value;
};

struct CatalogOverlayInstallState {
  bool installed{false};
  bool version_drift{false};
  int overlay_revision{0};
  int overlay_profile_version{210};
};

std::string EscapeJson(std::string_view text) {
  return scratchbird::parser::compatibility::EscapeJson(text);
}

std::string BoolJson(bool value) {
  return value ? "true" : "false";
}

constexpr std::string_view kManagementOperations[] = {
    "describe_package",
    "validate_package",
    "get_capabilities",
    "classify_management_request",
    "render_management_diagnostic",
    "setup_pseudo_server",
    "alter_pseudo_server",
    "drop_pseudo_server",
    "validate_pseudo_server",
    "list_pseudo_servers",
    "setup_database",
    "alter_database",
    "drop_database",
    "rename_database",
    "attach_database",
    "detach_database",
    "validate_database",
    "create_catalog_projection",
    "refresh_catalog_projection",
    "validate_catalog_projection",
    "seed_catalog_rowsets",
    "export_catalog_projection",
    "install_domain_emulation",
    "refresh_domain_emulation",
    "validate_domain_emulation",
    "install_helper_routines",
    "validate_helper_routines",
    "normalize_login_identity",
    "validate_auth_evidence",
    "add_user",
    "alter_user",
    "drop_user",
    "map_external_identity",
    "validate_user_mapping",
    "create_role",
    "alter_role",
    "drop_role",
    "grant_role",
    "revoke_role",
    "grant_privilege",
    "revoke_privilege",
    "set_security_policy",
    "export_security_policy",
    "validate_security_policy",
    "set_session_option",
    "set_database_option",
    "set_server_option",
    "run_admin_command",
    "classify_external_authority_command",
    "render_status_report",
    "prepare_migration_context",
    "apply_migration_batch",
    "finalize_migration_context",
    "abort_migration_context",
    "start_replication_channel",
    "stop_replication_channel",
    "apply_replication_event",
    "get_replication_status",
    "validate_emulation_state",
    "get_operational_status",
    "collect_support_bundle",
    "export_metadata_snapshot",
    "import_metadata_snapshot",
    "retire_emulation_profile",
};

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsManagementOperation(std::string_view operation_name) {
  for (const auto operation : kManagementOperations) {
    if (operation == operation_name) return true;
  }
  return false;
}

std::string_view ManagementResultClass(std::string_view operation_name) {
  if (operation_name == "classify_external_authority_command") return "REFUSED_EXTERNAL_AUTHORITY";
  if (operation_name == "describe_package" ||
      operation_name == "validate_package" ||
      operation_name == "get_capabilities" ||
      operation_name == "classify_management_request" ||
      operation_name == "render_management_diagnostic" ||
      operation_name == "list_pseudo_servers" ||
      operation_name == "normalize_login_identity" ||
      operation_name == "render_status_report" ||
      operation_name == "get_replication_status" ||
      operation_name == "get_operational_status" ||
      operation_name == "collect_support_bundle" ||
      StartsWith(operation_name, "validate_") ||
      StartsWith(operation_name, "export_")) {
    return "SUCCESS_REPORT";
  }
  return "SUCCESS_MUTATED";
}

bool ManagementOperationMutates(std::string_view operation_name) {
  return ManagementResultClass(operation_name) == "SUCCESS_MUTATED";
}

std::string ManagementInventoryJson(std::string_view render_policy) {
  const bool include_details = render_policy == "allow_debug_artifacts" ||
                               render_policy == "release_evidence";
  std::ostringstream out;
  out << "{\"package\":\"sbup_xtdb\","
      << "\"package_logical_name\":\"xtdb-v2_1\","
      << "\"package_call_name\":\"sbup_xtdb\","
      << "\"reference_family\":\"xtdb\","
      << "\"management_abi_version\":\"1.0\","
      << "\"routine_count\":" << std::size(kManagementOperations) << ','
      << "\"native_sbsql_excluded\":true,"
      << "\"parser_authority\":false,"
      << "\"engine_authorizes_before_udr\":true,"
      << "\"mga_transaction_authority\":\"scratchbird_engine\","
      << "\"reference_storage_authority\":false,"
      << "\"reference_recovery_authority\":false,"
      << "\"real_xtdb_file_effects\":false,"
      << "\"inventory_detail\":\"" << (include_details ? "release" : "summary") << "\","
      << "\"routines\":[";
  for (std::size_t i = 0; i < std::size(kManagementOperations); ++i) {
    if (i != 0) out << ',';
    const auto operation = kManagementOperations[i];
    out << "{\"name\":\"" << operation << "\","
        << "\"result_class\":\"" << ManagementResultClass(operation) << "\","
        << "\"requires_engine_authorization\":true,"
        << "\"requires_mga_transaction\":"
        << BoolJson(ManagementOperationMutates(operation)) << "}";
  }
  out << "]}";
  return out.str();
}

std::vector<ContextField> ParseContextPacket(std::string_view context_packet) {
  std::vector<ContextField> fields;
  std::size_t begin = 0;
  while (begin <= context_packet.size()) {
    std::size_t end = context_packet.find(';', begin);
    if (end == std::string_view::npos) end = context_packet.size();
    const auto token = scratchbird::parser::xtdb::TrimAscii(context_packet.substr(begin, end - begin));
    const auto separator = token.find('=');
    if (separator != std::string::npos) {
      fields.push_back({scratchbird::parser::xtdb::TrimAscii(token.substr(0, separator)),
                       scratchbird::parser::xtdb::TrimAscii(token.substr(separator + 1))});
    }
    if (end == context_packet.size()) break;
    begin = end + 1;
  }
  return fields;
}

bool HasContextField(std::string_view context_packet, std::string_view name, std::string_view value) {
  for (const auto& field : ParseContextPacket(context_packet)) {
    if (field.name == name && field.value == value) return true;
  }
  return false;
}

bool HasContextFieldName(std::string_view context_packet, std::string_view name) {
  for (const auto& field : ParseContextPacket(context_packet)) {
    if (field.name == name) return true;
  }
  return false;
}

bool HasContextFieldValue(std::string_view context_packet, std::string_view name) {
  for (const auto& field : ParseContextPacket(context_packet)) {
    if (field.name == name && !field.value.empty()) return true;
  }
  return false;
}

std::string ContextFieldValue(std::string_view context_packet, std::string_view name) {
  for (const auto& field : ParseContextPacket(context_packet)) {
    if (field.name == name) return field.value;
  }
  return {};
}

std::map<std::string, CatalogOverlayInstallState>& CatalogOverlayInstallStore() {
  static std::map<std::string, CatalogOverlayInstallState> store;
  return store;
}

UdrResult Diagnostic(std::string_view code, std::string_view message) {
  const std::vector<scratchbird::parser::xtdb::Diagnostic> diagnostics{
      {std::string(code), "ERROR", std::string(message), "sbu_xtdb_parser_support", {}}};
  return {false, {}, scratchbird::parser::xtdb::MessageVectorToJson(diagnostics)};
}

UdrResult SecurityDenied(std::string_view function_name, std::string_view reason) {
  const std::vector<scratchbird::parser::xtdb::Diagnostic> diagnostics{
      {"UDR.XTDB.SECURITY_DENIED", "ERROR",
       "XTDB parser-support UDR denied the requested operation.",
       "sbu_xtdb_parser_support",
       {{"udr_function", std::string(function_name)},
        {"reason", std::string(reason)}}}};
  return {false, {}, scratchbird::parser::xtdb::MessageVectorToJson(diagnostics)};
}

UdrResult MissingContext(std::string_view function_name, std::string_view required_fields) {
  const std::vector<scratchbird::parser::xtdb::Diagnostic> diagnostics{
      {"UDR.XTDB.CONTEXT_MISSING", "ERROR",
       "XTDB parser-support UDR requires engine-supplied trusted context.",
       "sbu_xtdb_parser_support",
       {{"udr_function", std::string(function_name)},
        {"required_context_fields", std::string(required_fields)}}}};
  return {false, {}, scratchbird::parser::xtdb::MessageVectorToJson(diagnostics)};
}

bool RequireTrustedContext(std::string_view context_packet,
                           std::string_view function_name,
                           std::string_view required_fields,
                           UdrResult& failure) {
  if (HasContextFieldName(context_packet, "engine_context") &&
      !HasContextField(context_packet, "engine_context", "trusted")) {
    failure = SecurityDenied(function_name, "engine_context is not trusted");
    return false;
  }
  if (!HasContextField(context_packet, "engine_context", "trusted")) {
    failure = MissingContext(function_name, required_fields);
    return false;
  }
  return true;
}

bool RequireContextField(std::string_view context_packet,
                         std::string_view name,
                         std::string_view value,
                         std::string_view function_name,
                         std::string_view required_fields,
                         UdrResult& failure) {
  if (HasContextField(context_packet, name, value)) return true;
  failure = MissingContext(function_name, required_fields);
  return false;
}

bool RequireContextFieldValue(std::string_view context_packet,
                              std::string_view name,
                              std::string_view function_name,
                              std::string_view required_fields,
                              UdrResult& failure) {
  if (HasContextFieldValue(context_packet, name)) return true;
  failure = MissingContext(function_name, required_fields);
  return false;
}

std::string OverlayHash(std::string_view catalog_uuid, const CatalogOverlayInstallState& state) {
  std::uint32_t hash = 2166136261u;
  for (const char ch : catalog_uuid) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 16777619u;
  }
  hash ^= static_cast<std::uint32_t>(state.overlay_revision * 151u);
  hash ^= static_cast<std::uint32_t>(state.overlay_profile_version * 19u);
  hash ^= state.installed ? 0x9e3779b9u : 0x2468ace0u;
  std::ostringstream out;
  out << "xtdb_catalog_overlay_" << hash;
  return out.str();
}

std::string CatalogOverlayStateName(const CatalogOverlayInstallState& state) {
  if (!state.installed) return "not_installed";
  if (state.version_drift) return "version_drift_detected";
  return "installed";
}

std::string OverlayArrayJson() {
  return "[\"XT\",\"PG_CATALOG\",\"INFORMATION_SCHEMA\",\"PUBLIC\","
         "\"INFORMATION_SCHEMA.TABLES\",\"INFORMATION_SCHEMA.COLUMNS\","
         "\"PG_CATALOG.PG_TYPE\",\"XT.TRIE_STATS\",\"XT.LIVE_TABLES\",\"XTDB_MODULES\"]";
}

scratchbird::udr::runtime::UdrCallResult ToRuntimeResult(UdrResult result) {
  return {result.ok, std::move(result.payload), std::move(result.message_vector_json)};
}

scratchbird::udr::runtime::UdrStatus XtdbLifecycle(std::string_view package_uuid) {
  if (package_uuid != kSbuXtdbPackageUuid) {
    return {false, "UDR.XTDB.PACKAGE_UUID_MISMATCH", "unexpected_package_uuid"};
  }
  return {true, "UDR.OK", {}};
}

scratchbird::udr::runtime::UdrCallResult RuntimeValidateSyntax(const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_xtdb_validate_syntax(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeParseToSblr(const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_xtdb_parse_to_sblr(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeNormalize(const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_xtdb_normalize(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeDescribeStatement(const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_xtdb_describe_statement(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeInstallEnvironment(const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_xtdb_install_environment(input.context_packet, input.payload));
}

scratchbird::udr::runtime::UdrCallResult RuntimeVerifyEnvironment(const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_xtdb_verify_environment(input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeManagementOperationInventory(const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_xtdb_management_operation_inventory(input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeManagementPackageRequest(const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_xtdb_management_package_request(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeDebugCapabilities(const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_xtdb_debug_capabilities(input.context_packet));
}

} // namespace

UdrResult sbu_xtdb_validate_syntax(std::string_view sql_text, std::string_view profile) {
  if (profile != "xtdb") {
    return Diagnostic("UDR.XTDB.PROFILE_MISMATCH", "XTDB parser-support UDR only accepts the xtdb profile.");
  }
  const auto result = scratchbird::parser::xtdb::ParseStatement(sql_text);
  return {result.ok, {}, result.message_vector_json};
}

UdrResult sbu_xtdb_parse_to_sblr(std::string_view sql_text, std::string_view context_packet) {
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, "sbu_xtdb_parse_to_sblr", "engine_context=trusted;resolver=uuid", failure)) return failure;
  if (!RequireContextField(context_packet, "resolver", "uuid", "sbu_xtdb_parse_to_sblr", "engine_context=trusted;resolver=uuid", failure)) return failure;
  const auto result = scratchbird::parser::xtdb::ParseStatement(sql_text);
  return {result.ok, result.sblr_envelope, result.message_vector_json};
}

UdrResult sbu_xtdb_normalize(std::string_view sql_text, std::string_view profile) {
  if (profile != "xtdb") {
    return Diagnostic("UDR.XTDB.PROFILE_MISMATCH", "XTDB parser-support UDR only accepts the xtdb profile.");
  }
  const auto result = scratchbird::parser::xtdb::ParseStatement(sql_text);
  if (!result.ok) return {false, {}, result.message_vector_json};
  return {true, result.normalized_sql, result.message_vector_json};
}

UdrResult sbu_xtdb_describe_statement(std::string_view sql_text, std::string_view context_packet) {
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, "sbu_xtdb_describe_statement", "engine_context=trusted", failure)) return failure;
  const auto result = scratchbird::parser::xtdb::ParseStatement(sql_text);
  if (!result.ok) return {false, {}, result.message_vector_json};
  return {true,
          "{\"dialect\":\"xtdb\",\"statement_family\":\"" + result.statement_family +
              "\",\"operation_family\":\"" + result.operation_family +
              "\",\"authority_disposition\":\"" + result.authority_disposition + "\"}",
          result.message_vector_json};
}

UdrResult sbu_xtdb_install_environment(std::string_view context_packet, std::string_view install_mode) {
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, "sbu_xtdb_install_environment", "engine_context=trusted;catalog_uuid=<uuid>", failure)) return failure;
  if (install_mode != "install" && install_mode != "verify" && install_mode != "force_reinstall" &&
      install_mode != "upgrade" && install_mode != "repair_version_drift") {
    return Diagnostic("UDR.XTDB.INSTALL_MODE_INVALID", "XTDB environment installer mode must be install, verify, force_reinstall, upgrade, or repair_version_drift.");
  }
  if (!RequireContextFieldValue(context_packet, "catalog_uuid", "sbu_xtdb_install_environment", "engine_context=trusted;catalog_uuid=<uuid>", failure)) return failure;
  const auto catalog_uuid = ContextFieldValue(context_packet, "catalog_uuid");
  auto& state = CatalogOverlayInstallStore()[catalog_uuid];
  const auto state_before = state;
  if (HasContextField(context_packet, "catalog_overlay_state", "drifted") ||
      HasContextField(context_packet, "version_drift", "detected")) {
    state.installed = true;
    if (state.overlay_revision == 0) state.overlay_revision = 1;
    state.version_drift = true;
  }
  const bool mutation_mode = install_mode == "install" || install_mode == "force_reinstall" ||
                             install_mode == "upgrade" || install_mode == "repair_version_drift";
  const bool drift_detected = state.version_drift || install_mode == "repair_version_drift";
  if (mutation_mode) {
    state.installed = true;
    if (install_mode == "install") {
      if (state.overlay_revision == 0) state.overlay_revision = 1;
    } else {
      ++state.overlay_revision;
    }
    if (install_mode == "upgrade") ++state.overlay_profile_version;
    if (install_mode == "repair_version_drift") state.version_drift = false;
  }
  return {true,
          "{\"installer\":\"sbu_xtdb_parser_support\","
          "\"role\":\"environment_installer\","
          "\"dialect\":\"xtdb\","
          "\"catalog_uuid\":\"" + EscapeJson(catalog_uuid) + "\","
          "\"mode\":\"" + std::string(install_mode) + "\","
          "\"idempotent\":true,"
          "\"atomic\":true,"
          "\"silent_repair\":false,"
          "\"catalog_mutation_applied\":" + BoolJson(mutation_mode) + ","
          "\"catalog_state_before\":\"" + CatalogOverlayStateName(state_before) + "\","
          "\"catalog_state_after\":\"" + CatalogOverlayStateName(state) + "\","
          "\"overlay_revision\":" + std::to_string(state.overlay_revision) + ","
          "\"overlay_profile_version\":" + std::to_string(state.overlay_profile_version) + ","
          "\"overlay_row_count\":10,"
          "\"overlay_hash\":\"" + EscapeJson(OverlayHash(catalog_uuid, state)) + "\","
          "\"version_drift_detected\":" + BoolJson(drift_detected) + ","
          "\"version_drift_repaired\":" + BoolJson(install_mode == "repair_version_drift") + ","
          "\"catalog_overlays\":" + OverlayArrayJson() + ","
          "\"real_xtdb_file_effects\":false}",
          scratchbird::parser::xtdb::MessageVectorToJson({})};
}

UdrResult sbu_xtdb_verify_environment(std::string_view context_packet) {
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, "sbu_xtdb_verify_environment", "engine_context=trusted;catalog_uuid=<uuid>", failure)) return failure;
  if (!RequireContextFieldValue(context_packet, "catalog_uuid", "sbu_xtdb_verify_environment", "engine_context=trusted;catalog_uuid=<uuid>", failure)) return failure;
  const auto catalog_uuid = ContextFieldValue(context_packet, "catalog_uuid");
  const auto& store = CatalogOverlayInstallStore();
  const auto found = store.find(catalog_uuid);
  const CatalogOverlayInstallState state = found == store.end() ? CatalogOverlayInstallState{} : found->second;
  return {true,
          "{\"installer\":\"sbu_xtdb_parser_support\","
          "\"role\":\"environment_verifier\","
          "\"dialect\":\"xtdb\","
          "\"catalog_uuid\":\"" + EscapeJson(catalog_uuid) + "\","
          "\"catalog_overlay_installed\":" + BoolJson(state.installed) + ","
          "\"catalog_state\":\"" + CatalogOverlayStateName(state) + "\","
          "\"catalog_overlays_present\":" + OverlayArrayJson() + ","
          "\"overlay_revision\":" + std::to_string(state.overlay_revision) + ","
          "\"overlay_profile_version\":" + std::to_string(state.overlay_profile_version) + ","
          "\"overlay_row_count\":10,"
          "\"overlay_hash\":\"" + EscapeJson(OverlayHash(catalog_uuid, state)) + "\","
          "\"version_drift_detected\":" + BoolJson(state.version_drift) + "}",
          scratchbird::parser::xtdb::MessageVectorToJson({})};
}

UdrResult sbu_xtdb_management_operation_inventory(std::string_view render_policy) {
  if (render_policy != "release_evidence" && render_policy != "allow_debug_artifacts" && render_policy != "summary") {
    return Diagnostic("UDR.XTDB.MGMT_INVENTORY_POLICY_DENIED", "XTDB management ABI inventory requires release evidence, summary, or debug artifact policy.");
  }
  return {true, ManagementInventoryJson(render_policy), scratchbird::parser::xtdb::MessageVectorToJson({})};
}

UdrResult sbu_xtdb_management_package_request(std::string_view operation_name, std::string_view context_packet) {
  constexpr std::string_view function_name = "sbu_xtdb_management_package_request";
  constexpr std::string_view required_context =
      "engine_context=trusted;package_uuid=<uuid>;request_uuid=<uuid>;operation_policy_ref=<uuid>;transaction_uuid=<uuid>";
  UdrResult failure;
  if (!IsManagementOperation(operation_name)) {
    return Diagnostic("UDR.XTDB.MGMT_OPERATION_UNKNOWN", "XTDB management package request names must be registered in the standard compatibility management ABI.");
  }
  if (!RequireTrustedContext(context_packet, function_name, required_context, failure)) return failure;
  if (!RequireContextField(context_packet, "package_uuid", kSbuXtdbPackageUuid, function_name, required_context, failure)) return failure;
  if (!RequireContextFieldValue(context_packet, "request_uuid", function_name, required_context, failure) ||
      !RequireContextFieldValue(context_packet, "operation_policy_ref", function_name, required_context, failure)) return failure;
  if (ManagementOperationMutates(operation_name) &&
      !RequireContextFieldValue(context_packet, "transaction_uuid", function_name, required_context, failure)) return failure;
  const auto result_class = ManagementResultClass(operation_name);
  const bool refused = result_class == "REFUSED_EXTERNAL_AUTHORITY";
  return {true,
          "{\"package\":\"sbup_xtdb\","
          "\"package_logical_name\":\"xtdb-v2_1\","
          "\"management_abi_version\":\"1.0\","
          "\"operation_name\":\"" + std::string(operation_name) + "\","
          "\"result_class\":\"" + std::string(result_class) + "\","
          "\"engine_authorized_context\":true,"
          "\"package_policy_can_only_tighten_engine_decision\":true,"
          "\"parser_authority\":false,"
          "\"parser_selected_package_authority\":false,"
          "\"sbsql_management_route\":false,"
          "\"native_sbsql_excluded\":true,"
          "\"mga_transaction_authority\":\"scratchbird_engine\","
          "\"requires_mga_transaction\":" + BoolJson(ManagementOperationMutates(operation_name)) + ","
          "\"reference_storage_authority\":false,"
          "\"reference_recovery_authority\":false,"
          "\"real_xtdb_file_effects\":false,"
          "\"exact_refusal\":" + BoolJson(refused) + ","
          "\"idempotency_state\":\"engine_request_uuid_bound\","
          "\"support_evidence_ref\":\"project/tests/reference_regression/xtdb/management_package_abi/management_package_abi_manifest.csv\"}",
          scratchbird::parser::xtdb::MessageVectorToJson({})};
}

UdrResult sbu_xtdb_debug_capabilities(std::string_view render_policy) {
  if (render_policy != "allow_debug_artifacts") {
    return Diagnostic("SBU_XTDB.DEBUG_POLICY_DENIED", "Debug capability reporting requires explicit debug artifact policy.");
  }
  return {true, scratchbird::parser::xtdb::XtdbPackageIdentityJson(), scratchbird::parser::xtdb::MessageVectorToJson({})};
}

scratchbird::udr::runtime::UdrPackageDescriptor sbu_xtdb_package_descriptor() {
  scratchbird::udr::runtime::UdrPackageDescriptor descriptor;
  descriptor.package_uuid = std::string(kSbuXtdbPackageUuid);
  descriptor.package_name = std::string(kSbuXtdbPackageName);
  descriptor.abi_version = "sb_udr_v1";
  descriptor.source_revision = "xtdb-parser-support-beta-closure";
  descriptor.binary_hash = "sha256:sbu_xtdb_parser_support_builtin";
  descriptor.signature_policy = "builtin-trusted-private";
  descriptor.capability_role = "parser_support.xtdb";
  descriptor.trusted_cpp = true;
  descriptor.entrypoints = {
      {"sbu_xtdb_validate_syntax", "parser.validate_syntax", &RuntimeValidateSyntax},
      {"sbu_xtdb_parse_to_sblr", "parser.parse_to_sblr", &RuntimeParseToSblr},
      {"sbu_xtdb_normalize", "parser.normalize", &RuntimeNormalize},
      {"sbu_xtdb_describe_statement", "parser.describe_statement", &RuntimeDescribeStatement},
      {"sbu_xtdb_install_environment", "parser.install_environment", &RuntimeInstallEnvironment},
      {"sbu_xtdb_verify_environment", "parser.verify_environment", &RuntimeVerifyEnvironment},
      {"sbu_xtdb_management_operation_inventory", "parser.management_abi_inventory", &RuntimeManagementOperationInventory},
      {"sbu_xtdb_management_package_request", "parser.management_abi_dispatch", &RuntimeManagementPackageRequest},
      {"sbu_xtdb_debug_capabilities", "parser.debug_capabilities", &RuntimeDebugCapabilities},
  };
  descriptor.init = &XtdbLifecycle;
  descriptor.shutdown = &XtdbLifecycle;
  return descriptor;
}

} // namespace scratchbird::udr::xtdb_parser_support
