// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbu_firebird_parser_support.hpp"

#include "firebird_dialect.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::udr::firebird_parser_support {
namespace {

struct ContextField {
  std::string name;
  std::string value;
};

struct RenderedDiagnostic {
  std::string code;
  std::string severity;
};

struct StatusMapping {
  int sqlcode{0};
  std::string sqlstate{"00000"};
  std::string status_class{"success"};
  std::string gds_symbol{"isc_success"};
  std::uint32_t gds_code{0};
  std::uint32_t status_arg{1};
};

struct CatalogOverlayInstallState {
  bool installed{false};
  bool version_drift{false};
  int overlay_revision{0};
  int overlay_profile_version{50};
};

std::string EscapeJson(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
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

bool IsManagementOperation(std::string_view operation_name) {
  for (const auto operation : kManagementOperations) {
    if (operation == operation_name) return true;
  }
  return false;
}

bool MgmtStartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string_view ManagementResultClass(std::string_view operation_name) {
  if (operation_name == "classify_external_authority_command") {
    return "REFUSED_EXTERNAL_AUTHORITY";
  }
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
      MgmtStartsWith(operation_name, "validate_") ||
      MgmtStartsWith(operation_name, "export_")) {
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
  out << "{\"package\":\"sbup_firebird\","
      << "\"package_logical_name\":\"firebird-v5_0\","
      << "\"package_call_name\":\"sbup_firebird\","
      << "\"donor_family\":\"firebird\","
      << "\"management_abi_version\":\"1.0\","
      << "\"routine_count\":" << std::size(kManagementOperations) << ','
      << "\"native_sbsql_excluded\":true,"
      << "\"parser_authority\":false,"
      << "\"engine_authorizes_before_udr\":true,"
      << "\"mga_transaction_authority\":\"scratchbird_engine\","
      << "\"donor_storage_authority\":false,"
      << "\"donor_recovery_authority\":false,"
      << "\"real_firebird_file_effects\":false,"
      << "\"inventory_detail\":\""
      << (include_details ? "release" : "summary") << "\","
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
    const auto token =
        scratchbird::parser::firebird::TrimAscii(context_packet.substr(begin, end - begin));
    const auto separator = token.find('=');
    if (separator != std::string::npos) {
      fields.push_back(
          {scratchbird::parser::firebird::TrimAscii(token.substr(0, separator)),
           scratchbird::parser::firebird::TrimAscii(token.substr(separator + 1))});
    }
    if (end == context_packet.size()) break;
    begin = end + 1;
  }
  return fields;
}

bool HasContextField(std::string_view context_packet,
                     std::string_view name,
                     std::string_view value) {
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

std::string ContextFieldValue(std::string_view context_packet,
                              std::string_view name) {
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
  const std::vector<scratchbird::parser::firebird::Diagnostic> diagnostics{
      {std::string(code), "ERROR", std::string(message),
       "sbu_firebird_parser_support", {}}};
  return {false, {}, scratchbird::parser::firebird::MessageVectorToJson(diagnostics)};
}

UdrResult SecurityDenied(std::string_view function_name, std::string_view reason) {
  const std::vector<scratchbird::parser::firebird::Diagnostic> diagnostics{
      {"UDR.FIREBIRD.SECURITY_DENIED", "ERROR",
       "Firebird parser-support UDR denied the requested operation.",
       "sbu_firebird_parser_support",
       {{"udr_function", std::string(function_name)},
        {"reason", std::string(reason)}}}};
  return {false, {}, scratchbird::parser::firebird::MessageVectorToJson(diagnostics)};
}

UdrResult MissingContext(std::string_view function_name,
                         std::string_view required_fields) {
  const std::vector<scratchbird::parser::firebird::Diagnostic> diagnostics{
      {"UDR.FIREBIRD.CONTEXT_MISSING", "ERROR",
       "Firebird parser-support UDR requires engine-supplied trusted context.",
       "sbu_firebird_parser_support",
       {{"udr_function", std::string(function_name)},
        {"required_context_fields", std::string(required_fields)}}}};
  return {false, {}, scratchbird::parser::firebird::MessageVectorToJson(diagnostics)};
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

std::string ExtractJsonStringValue(std::string_view json,
                                   std::string_view key,
                                   std::size_t search_from,
                                   std::size_t& next_position) {
  const auto key_position = json.find(key, search_from);
  if (key_position == std::string_view::npos) return {};
  const auto value_begin = key_position + key.size();
  std::string value;
  bool escaped = false;
  for (std::size_t i = value_begin; i < json.size(); ++i) {
    const char ch = json[i];
    if (escaped) {
      switch (ch) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(ch); break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      next_position = i + 1;
      return value;
    }
    value.push_back(ch);
  }
  next_position = json.size();
  return value;
}

std::vector<RenderedDiagnostic> ExtractRenderedDiagnostics(std::string_view message_vector_json) {
  std::vector<RenderedDiagnostic> diagnostics;
  std::size_t position = 0;
  while (position < message_vector_json.size()) {
    std::size_t next_code = position;
    const auto code =
        ExtractJsonStringValue(message_vector_json, "\"code\":\"", position, next_code);
    if (code.empty()) break;

    std::size_t next_severity = next_code;
    auto severity = ExtractJsonStringValue(
        message_vector_json, "\"severity\":\"", next_code, next_severity);
    if (severity.empty()) severity = "ERROR";

    diagnostics.push_back({code, severity});
    position = next_severity > next_code ? next_severity : next_code;
  }
  return diagnostics;
}

bool SeverityEquals(std::string_view severity, std::string_view expected) {
  return scratchbird::parser::firebird::ToUpperAscii(severity) == expected;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string JsonStringValue(std::string_view packet, std::string_view key) {
  const std::string quoted_key = "\"" + std::string(key) + "\":\"";
  const auto start = packet.find(quoted_key);
  if (start == std::string_view::npos) return {};
  const auto value_start = start + quoted_key.size();
  std::string value;
  bool escaped = false;
  for (std::size_t i = value_start; i < packet.size(); ++i) {
    const char ch = packet[i];
    if (escaped) {
      value.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') return value;
    value.push_back(ch);
  }
  return {};
}

std::string PacketValue(std::string_view packet, std::string_view key) {
  const auto context_value = ContextFieldValue(packet, key);
  if (!context_value.empty()) return context_value;
  return JsonStringValue(packet, key);
}

bool HasRawSecretMaterial(std::string_view packet) {
  return packet.find("password=") != std::string_view::npos ||
         packet.find("secret=") != std::string_view::npos ||
         packet.find("\"password\"") != std::string_view::npos ||
         packet.find("\"secret\"") != std::string_view::npos;
}

UdrResult BridgeDiagnostic(std::string_view code,
                           std::string_view message,
                           std::vector<scratchbird::parser::firebird::Field> fields = {}) {
  fields.push_back({"message_vector_class", std::string(code)});
  const std::vector<scratchbird::parser::firebird::Diagnostic> diagnostics{
      {std::string(code), "ERROR", std::string(message),
       "sbu_firebird_parser_support", std::move(fields)}};
  return {false, {}, scratchbird::parser::firebird::MessageVectorToJson(diagnostics)};
}

bool RequireBridgeContext(std::string_view context_packet,
                          std::string_view function_name,
                          UdrResult& failure) {
  constexpr std::string_view required =
      "engine_context=trusted;bridge_authority=engine;user_uuid=<uuid>;"
      "request_uuid=<uuid>;operation_policy_ref=<uuid>";
  if (HasRawSecretMaterial(context_packet)) {
    failure = BridgeDiagnostic("UDR.BRIDGE.SECRET_MATERIAL_DENIED",
                               "Firebird bridge requests must use secret references, not raw secrets.",
                               {{"udr_function", std::string(function_name)}});
    return false;
  }
  if (!HasContextField(context_packet, "engine_context", "trusted") ||
      !HasContextField(context_packet, "bridge_authority", "engine")) {
    failure = BridgeDiagnostic("UDR.BRIDGE.CONTEXT_MISSING",
                               "Firebird bridge dispatch requires trusted engine bridge context.",
                               {{"udr_function", std::string(function_name)},
                                {"required_context_fields", std::string(required)}});
    return false;
  }
  if (!HasContextFieldValue(context_packet, "user_uuid") ||
      !HasContextFieldValue(context_packet, "request_uuid") ||
      !HasContextFieldValue(context_packet, "operation_policy_ref")) {
    failure = BridgeDiagnostic("UDR.BRIDGE.CONTEXT_MISSING",
                               "Firebird bridge dispatch requires user, request, and policy identifiers.",
                               {{"udr_function", std::string(function_name)},
                                {"required_context_fields", std::string(required)}});
    return false;
  }
  return true;
}

bool IsClusterBridgeOperation(std::string_view operation) {
  return StartsWith(operation, "cluster.") ||
         StartsWith(operation, "cluster_") ||
         operation == "distributed_query" ||
         operation == "cross_node_query";
}

bool IsTransactionBridgeOperation(std::string_view operation) {
  return operation == "begin" ||
         operation == "commit" ||
         operation == "rollback" ||
         operation == "prepare" ||
         operation == "savepoint";
}

bool IsStreamBridgeOperation(std::string_view operation) {
  return operation == "stream_open" ||
         operation == "stream_read" ||
         operation == "stream_write" ||
         operation == "stream_close";
}

bool IsSupportedBridgeOperation(std::string_view operation) {
  static constexpr std::array<std::string_view, 31> kSupported = {
      "describe_capabilities", "connect", "attach", "authenticate",
      "open_session", "close_session", "detach", "ping", "health",
      "cancel", "drain", "shutdown", "begin", "commit", "rollback",
      "prepare", "savepoint", "execute", "cursor_open", "cursor_fetch",
      "cursor_close", "stream_open", "stream_read", "stream_write",
      "stream_close", "cdc_start", "cdc_read", "cdc_apply",
      "proxy_route", "compare_result", "cutover"};
  for (const auto candidate : kSupported) {
    if (operation == candidate) return true;
  }
  return false;
}

std::string BridgeOpcodeForOperation(std::string_view operation) {
  if (operation == "describe_capabilities") return "SBLR_BRIDGE_DESCRIBE_CAPABILITIES";
  if (operation == "connect" || operation == "attach") return "SBLR_BRIDGE_OPEN_CHANNEL";
  if (operation == "authenticate") return "SBLR_BRIDGE_AUTHENTICATE";
  if (operation == "open_session") return "SBLR_BRIDGE_OPEN_SESSION";
  if (operation == "close_session" || operation == "detach") return "SBLR_BRIDGE_CLOSE_SESSION";
  if (operation == "ping" || operation == "health") return "SBLR_BRIDGE_HEALTH";
  if (operation == "cancel") return "SBLR_BRIDGE_CANCEL";
  if (operation == "drain" || operation == "shutdown") return "SBLR_BRIDGE_DRAIN";
  if (operation == "begin") return "SBLR_BRIDGE_TX_BEGIN";
  if (operation == "commit") return "SBLR_BRIDGE_TX_COMMIT";
  if (operation == "rollback") return "SBLR_BRIDGE_TX_ROLLBACK";
  if (operation == "prepare") return "SBLR_BRIDGE_TX_PREPARE";
  if (operation == "savepoint") return "SBLR_BRIDGE_TX_SAVEPOINT";
  if (operation == "execute") return "SBLR_BRIDGE_EXECUTE";
  if (operation == "cursor_open") return "SBLR_BRIDGE_CURSOR_OPEN";
  if (operation == "cursor_fetch") return "SBLR_BRIDGE_CURSOR_FETCH";
  if (operation == "cursor_close") return "SBLR_BRIDGE_CURSOR_CLOSE";
  if (operation == "stream_open") return "SBLR_BRIDGE_STREAM_OPEN";
  if (operation == "stream_read") return "SBLR_BRIDGE_STREAM_READ";
  if (operation == "stream_write") return "SBLR_BRIDGE_STREAM_WRITE";
  if (operation == "stream_close") return "SBLR_BRIDGE_STREAM_CLOSE";
  if (operation == "cdc_start") return "SBLR_BRIDGE_CDC_START";
  if (operation == "cdc_read") return "SBLR_BRIDGE_CDC_READ";
  if (operation == "cdc_apply") return "SBLR_BRIDGE_CDC_APPLY";
  if (operation == "proxy_route") return "SBLR_BRIDGE_PROXY_ROUTE";
  if (operation == "compare_result") return "SBLR_BRIDGE_COMPARE_RESULT";
  if (operation == "cutover") return "SBLR_BRIDGE_CUTOVER";
  return "SBLR_BRIDGE_VALIDATE";
}

std::string FirebirdBridgeCapabilitiesJson(std::string_view render_policy) {
  const bool include_detail = render_policy == "release_evidence" ||
                              render_policy == "allow_debug_artifacts" ||
                              HasContextField(render_policy, "release_evidence", "true");
  std::ostringstream out;
  out << "{\"bridge_abi\":\"sb_universal_bridge_v1\","
      << "\"provider\":\"sbu_firebird_parser_support\","
      << "\"provider_family\":\"firebird\","
      << "\"package_role\":\"parser_support.firebird\","
      << "\"single_common_abi\":true,"
      << "\"donor_specialization\":\"firebird\","
      << "\"engine_authorizes_before_udr\":true,"
      << "\"mga_transaction_authority\":\"per_database_engine_mga\","
      << "\"parser_transaction_authority\":false,"
      << "\"udr_transaction_authority\":false,"
      << "\"raw_secret_material_allowed\":false,"
      << "\"secret_refs_required\":true,"
      << "\"cluster_public_implementation\":false,"
      << "\"native_sbsql_excluded\":true,"
      << "\"firebird_only\":true,"
      << "\"logical_backup_restore\":\"remote_stream_only\","
      << "\"physical_page_copy_backup_restore\":\"denied\","
      << "\"repair_verify_low_level_maintenance\":\"denied_in_donor_parser\","
      << "\"cdc_replication_etl\":\"source_and_target_when_configured\","
      << "\"donor_rendering\":\"firebird_status_vector\","
      << "\"remote_table_query_class\":\"non_cluster_remote_table\","
      << "\"distributed_query_class\":\"cluster_only\","
      << "\"detail\":\"" << (include_detail ? "release" : "summary") << "\","
      << "\"supported_topologies\":["
      << "\"outbound_federation\",\"inbound_cdc\",\"outbound_replication\","
      << "\"proxy_live_migration\",\"logical_backup_restore\"],"
      << "\"supported_operations\":[";
  static constexpr std::array<std::string_view, 31> kOperations = {
      "describe_capabilities", "connect", "attach", "authenticate",
      "open_session", "close_session", "detach", "ping", "health",
      "cancel", "drain", "shutdown", "begin", "commit", "rollback",
      "prepare", "savepoint", "execute", "cursor_open", "cursor_fetch",
      "cursor_close", "stream_open", "stream_read", "stream_write",
      "stream_close", "cdc_start", "cdc_read", "cdc_apply",
      "proxy_route", "compare_result", "cutover"};
  for (std::size_t i = 0; i < kOperations.size(); ++i) {
    if (i != 0) out << ',';
    out << "{\"operation\":\"" << kOperations[i]
        << "\",\"sblr_opcode\":\"" << BridgeOpcodeForOperation(kOperations[i])
        << "\"}";
  }
  out << "]}";
  return out.str();
}

UdrResult FirebirdBridgeDispatch(std::string_view request_packet,
                                 std::string_view context_packet) {
  if (HasRawSecretMaterial(request_packet)) {
    return BridgeDiagnostic("UDR.BRIDGE.SECRET_MATERIAL_DENIED",
                            "Firebird bridge requests must use secret references, not raw secrets.",
                            {{"provider", "sbu_firebird_parser_support"}});
  }
  UdrResult failure;
  if (!RequireBridgeContext(context_packet, "sbu_firebird_bridge_dispatch", failure)) {
    return failure;
  }

  std::string operation = PacketValue(request_packet, "operation");
  if (operation.empty()) operation = ContextFieldValue(context_packet, "bridge_operation");
  if (operation.empty()) operation = std::string(request_packet);
  operation = scratchbird::parser::firebird::TrimAscii(operation);
  if (operation.empty()) {
    return BridgeDiagnostic("UDR.BRIDGE.MISSING_CAPABILITY",
                            "Firebird bridge dispatch requires an operation name.",
                            {{"provider", "sbu_firebird_parser_support"}});
  }
  if (operation == "validate") operation = "describe_capabilities";

  if (IsClusterBridgeOperation(operation)) {
    if (HasContextField(context_packet, "cluster_provider_gate", "admitted")) {
      return BridgeDiagnostic("UDR.BRIDGE.UNLICENSED",
                              "Cluster bridge commands are routed to the public stub and are unlicensed in this build.",
                              {{"provider", "sbu_firebird_parser_support"},
                               {"operation", operation},
                               {"cluster_stub_route", "public_stub_unlicensed"}});
    }
    return BridgeDiagnostic("UDR.BRIDGE.UNSUPPORTED",
                            "Cluster bridge commands are disabled by compile-time policy.",
                            {{"provider", "sbu_firebird_parser_support"},
                             {"operation", operation},
                             {"cluster_stub_route", "compile_time_gate_disabled"}});
  }

  if (!IsSupportedBridgeOperation(operation)) {
    return BridgeDiagnostic("UDR.BRIDGE.UNSUPPORTED",
                            "Firebird bridge operation is not part of the universal public bridge ABI.",
                            {{"provider", "sbu_firebird_parser_support"},
                             {"operation", operation}});
  }

  const std::string stream_kind = PacketValue(request_packet, "stream_kind");
  if (stream_kind == "physical_page_copy" ||
      stream_kind == "physical_backup" ||
      stream_kind == "nbackup" ||
      stream_kind == "server_local_file") {
    return BridgeDiagnostic("UDR.BRIDGE.SANDBOX_DENIED",
                            "Firebird donor bridge denies nbackup, physical page-copy, and server-local file streams.",
                            {{"provider", "sbu_firebird_parser_support"},
                             {"operation", operation},
                             {"stream_kind", stream_kind}});
  }
  if (operation == "execute" &&
      (PacketValue(request_packet, "maintenance") == "repair" ||
       PacketValue(request_packet, "maintenance") == "verify" ||
       PacketValue(request_packet, "service_action") == "repair" ||
       PacketValue(request_packet, "service_action") == "validate_pages")) {
    return BridgeDiagnostic("UDR.BRIDGE.UNSUPPORTED",
                            "Firebird repair, verify, and low-level maintenance remain SBsql-only.",
                            {{"provider", "sbu_firebird_parser_support"},
                             {"operation", operation}});
  }
  if (operation == "authenticate" &&
      ContextFieldValue(context_packet, "target_auth_state") == "failed") {
    return BridgeDiagnostic("UDR.BRIDGE.AUTH_FAILED",
                            "Firebird bridge target authentication failed.",
                            {{"provider", "sbu_firebird_parser_support"}});
  }
  if (IsTransactionBridgeOperation(operation) &&
      !HasContextFieldValue(context_packet, "local_transaction_uuid")) {
    return BridgeDiagnostic("UDR.BRIDGE.CONTEXT_MISSING",
                            "Firebird bridge transaction operations require a local MGA transaction UUID.",
                            {{"provider", "sbu_firebird_parser_support"},
                             {"operation", operation},
                             {"required_context_fields", "local_transaction_uuid=<uuid>"}});
  }
  if (operation == "prepare" &&
      !HasContextField(context_packet, "remote_supports_prepare", "true")) {
    return BridgeDiagnostic("UDR.BRIDGE.MISSING_CAPABILITY",
                            "Firebird bridge prepare is admitted only when the target supports prepare.",
                            {{"provider", "sbu_firebird_parser_support"},
                             {"operation", operation}});
  }
  if (IsStreamBridgeOperation(operation) &&
      (operation == "stream_read" || operation == "stream_write" ||
       operation == "stream_close") &&
      PacketValue(request_packet, "stream_uuid").empty()) {
    return BridgeDiagnostic("UDR.BRIDGE.STREAM_INVALID",
                            "Firebird bridge stream continuation requires a stream UUID.",
                            {{"provider", "sbu_firebird_parser_support"},
                             {"operation", operation}});
  }
  if ((operation == "cdc_apply" || operation == "cdc_read") &&
      PacketValue(request_packet, "idempotency_key").empty()) {
    return BridgeDiagnostic("UDR.BRIDGE.IDEMPOTENCY_MISSING",
                            "Firebird bridge CDC read/apply requires an idempotency key.",
                            {{"provider", "sbu_firebird_parser_support"},
                             {"operation", operation}});
  }
  if (operation == "cutover" &&
      !HasContextField(context_packet, "cutover_evidence", "validated")) {
    return BridgeDiagnostic("UDR.BRIDGE.CUTOVER_FAILED",
                            "Firebird bridge cutover requires validated compare and drain evidence.",
                            {{"provider", "sbu_firebird_parser_support"},
                             {"operation", operation}});
  }

  std::ostringstream out;
  out << "{\"bridge_abi\":\"sb_universal_bridge_v1\","
      << "\"provider\":\"sbu_firebird_parser_support\","
      << "\"provider_family\":\"firebird\","
      << "\"donor_legacy_bridge\":true,"
      << "\"firebird_only\":true,"
      << "\"operation\":\"" << EscapeJson(operation) << "\","
      << "\"sblr_opcode\":\"" << EscapeJson(BridgeOpcodeForOperation(operation)) << "\","
      << "\"result_class\":\"admitted\","
      << "\"engine_authorized_context\":true,"
      << "\"parser_authority\":false,"
      << "\"udr_transaction_authority\":false,"
      << "\"mga_transaction_authority\":\"per_database_engine_mga\","
      << "\"local_transaction_uuid\":\""
      << EscapeJson(ContextFieldValue(context_packet, "local_transaction_uuid")) << "\","
      << "\"remote_transaction_ref\":\""
      << EscapeJson(ContextFieldValue(context_packet, "remote_transaction_ref")) << "\","
      << "\"user_uuid\":\"" << EscapeJson(ContextFieldValue(context_packet, "user_uuid"))
      << "\","
      << "\"request_uuid\":\""
      << EscapeJson(ContextFieldValue(context_packet, "request_uuid")) << "\","
      << "\"operation_policy_ref\":\""
      << EscapeJson(ContextFieldValue(context_packet, "operation_policy_ref")) << "\","
      << "\"secret_refs_only\":true,"
      << "\"raw_sql_text_included\":false,"
      << "\"raw_secret_material_included\":false,"
      << "\"real_firebird_file_effects\":false,"
      << "\"cluster_execution\":false,"
      << "\"remote_table_semantics\":\"non_cluster_remote_table\","
      << "\"distributed_query_semantics\":\"cluster_only_refused_in_public_bridge\"";
  if (!stream_kind.empty()) {
    out << ",\"stream_kind\":\"" << EscapeJson(stream_kind) << "\"";
  }
  if (stream_kind == "logical_restore" || stream_kind == "logical_backup" ||
      stream_kind == "gbak_logical_restore" || stream_kind == "gbak_logical_backup") {
    out << ",\"firebird_logical_service_stream\":\"remote_stream_admitted\"";
  }
  if (operation == "describe_capabilities") {
    out << ",\"capabilities\":" << FirebirdBridgeCapabilitiesJson("summary");
  }
  out << "}";
  return {true, out.str(), scratchbird::parser::firebird::MessageVectorToJson({})};
}

StatusMapping MapDiagnosticStatus(const RenderedDiagnostic* diagnostic) {
  if (diagnostic == nullptr) return {};
  const auto code = std::string_view(diagnostic->code);
  if (SeverityEquals(diagnostic->severity, "WARNING")) {
    return {100, "01000", "warning", "isc_dsql_warning_number_ambiguous",
            336003080u, 18};
  }
  if (StartsWith(code, "FIREBIRD.PARSE.")) {
    return {-104, "42000", "parse_syntax", "isc_syntaxerr",
            335544390u, 1};
  }
  if (code == "UDR.FIREBIRD.SECURITY_DENIED") {
    return {-551, "28000", "security_denied", "isc_no_priv",
            335544352u, 1};
  }
  if (StartsWith(code, "UDR.BRIDGE.SECRET_MATERIAL_DENIED") ||
      StartsWith(code, "UDR.BRIDGE.SANDBOX_DENIED") ||
      StartsWith(code, "UDR.BRIDGE.AUTH_FAILED")) {
    return {-551, "28000", "security_denied", "isc_no_priv",
            335544352u, 1};
  }
  if (StartsWith(code, "UDR.BRIDGE.UNSUPPORTED") ||
      StartsWith(code, "UDR.BRIDGE.UNLICENSED") ||
      StartsWith(code, "UDR.BRIDGE.MISSING_CAPABILITY")) {
    return {-901, "0A000", "package_admission", "isc_wish_list",
            335544378u, 1};
  }
  if (StartsWith(code, "UDR.BRIDGE.CONTEXT_MISSING") ||
      StartsWith(code, "UDR.BRIDGE.STREAM_INVALID") ||
      StartsWith(code, "UDR.BRIDGE.IDEMPOTENCY_MISSING") ||
      StartsWith(code, "UDR.BRIDGE.CUTOVER_FAILED")) {
    return {-804, "07002", "call_contract", "isc_wronumarg",
            335544380u, 1};
  }
  if (code == "SBU_FIREBIRD.DEBUG_POLICY_DENIED" ||
      code == "FIREBIRD.CATALOG_OVERLAY.READ_ONLY") {
    return {-551, "28000", "security_denied", "isc_no_priv",
            335544352u, 1};
  }
  if (code == "UDR.FIREBIRD.CONTEXT_MISSING") {
    return {-804, "07002", "context_missing", "isc_wronumarg",
            335544380u, 1};
  }
  if (code == "UDR.FIREBIRD.PROFILE_MISMATCH" ||
      code == "UDR.FIREBIRD.INSTALL_MODE_INVALID" ||
      code == "UDR.FIREBIRD.MESSAGE_VECTOR_INVALID") {
    return {-804, "07002", "call_contract", "isc_wronumarg",
            335544380u, 1};
  }
  if (StartsWith(code, "FIREBIRD.PACKAGE.")) {
    return {-901, "0A000", "package_admission", "isc_wish_list",
            335544378u, 1};
  }
  if (StartsWith(code, "FIREBIRD.WIRE.")) {
    return {-901, "HY000", "wire_protocol", "isc_random",
            335544382u, 1};
  }
  if (SeverityEquals(diagnostic->severity, "ERROR")) {
    return {-901, "HY000", "generic_error", "isc_random",
            335544382u, 1};
  }
  return {};
}

const RenderedDiagnostic* PrimaryDiagnostic(
    const std::vector<RenderedDiagnostic>& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    if (SeverityEquals(diagnostic.severity, "ERROR")) return &diagnostic;
  }
  for (const auto& diagnostic : diagnostics) {
    if (SeverityEquals(diagnostic.severity, "WARNING")) return &diagnostic;
  }
  if (!diagnostics.empty()) return &diagnostics.front();
  return nullptr;
}

std::string RenderStatusVectorPayload(
    const std::vector<RenderedDiagnostic>& diagnostics) {
  bool has_errors = false;
  bool has_warnings = false;
  for (const auto& diagnostic : diagnostics) {
    has_errors = has_errors || SeverityEquals(diagnostic.severity, "ERROR");
    has_warnings = has_warnings || SeverityEquals(diagnostic.severity, "WARNING");
  }

  const auto* primary = PrimaryDiagnostic(diagnostics);
  const auto mapping = MapDiagnosticStatus(primary);

  std::ostringstream out;
  out << "{\"renderer\":\"sbu_firebird_parser_support\","
      << "\"dialect\":\"firebird\","
      << "\"source\":\"parser_message_vector\","
      << "\"message_vector_included\":false,"
      << "\"diagnostic_count\":" << diagnostics.size() << ','
      << "\"has_errors\":" << BoolJson(has_errors) << ','
      << "\"has_warnings\":" << BoolJson(has_warnings) << ','
      << "\"primary_sqlcode\":" << mapping.sqlcode << ','
      << "\"primary_sqlstate\":\""
      << EscapeJson(mapping.sqlstate) << "\","
      << "\"primary_gds_code\":" << mapping.gds_code << ','
      << "\"primary_gds_symbol\":\"" << EscapeJson(mapping.gds_symbol) << "\","
      << "\"primary_status_arg\":" << mapping.status_arg << ','
      << "\"primary_sqlcode_compat\":" << mapping.sqlcode << ','
      << "\"primary_sqlstate_compat\":\""
      << EscapeJson(mapping.sqlstate) << "\","
      << "\"status_class\":\"" << EscapeJson(mapping.status_class) << "\","
      << "\"status_vector_exact\":[";

  if (primary == nullptr) {
    out << "{\"arg\":\"isc_arg_gds\",\"value\":0},"
        << "{\"arg\":\"isc_arg_end\",\"value\":0}";
  } else {
    out << "{\"arg\":\""
        << (mapping.status_arg == 18 ? "isc_arg_warning" : "isc_arg_gds")
        << "\",\"value\":" << mapping.status_arg << "},"
        << "{\"symbol\":\"" << EscapeJson(mapping.gds_symbol)
        << "\",\"value\":" << mapping.gds_code << "},"
        << "{\"arg\":\"isc_arg_sql_state\",\"value\":19},"
        << "{\"sqlstate\":\"" << EscapeJson(mapping.sqlstate) << "\"},"
        << "{\"arg\":\"isc_arg_end\",\"value\":0}";
  }

  out << "],\"status_vector_tokens\":[";

  if (primary == nullptr) {
    out << "\"gds:success\",\"end\"";
  } else {
    out << "\"gds:" << EscapeJson(mapping.status_class) << "\","
        << "\"sqlcode:" << mapping.sqlcode << "\","
        << "\"sqlstate:" << EscapeJson(mapping.sqlstate) << "\","
        << "\"gds_symbol:" << EscapeJson(mapping.gds_symbol) << "\","
        << "\"gds_code:" << mapping.gds_code << "\","
        << "\"diagnostic:" << EscapeJson(primary->code) << "\","
        << "\"end\"";
  }

  out << "],\"diagnostics\":[";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0) out << ',';
    out << "{\"code\":\"" << EscapeJson(diagnostics[i].code)
        << "\",\"severity\":\"" << EscapeJson(diagnostics[i].severity)
        << "\"}";
  }
  out << "]}";
  return out.str();
}

std::string OverlayHash(std::string_view catalog_uuid,
                        const CatalogOverlayInstallState& state) {
  std::uint32_t hash = 2166136261u;
  for (const char ch : catalog_uuid) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 16777619u;
  }
  hash ^= static_cast<std::uint32_t>(state.overlay_revision * 131u);
  hash ^= static_cast<std::uint32_t>(state.overlay_profile_version * 17u);
  hash ^= state.installed ? 0x5f3759dfu : 0x13579bdfu;
  std::ostringstream out;
  out << "firebird_catalog_overlay_" << hash;
  return out.str();
}

std::string CatalogOverlayStateName(const CatalogOverlayInstallState& state) {
  if (!state.installed) return "not_installed";
  if (state.version_drift) return "version_drift_detected";
  return "installed";
}

scratchbird::udr::runtime::UdrCallResult ToRuntimeResult(UdrResult result) {
  return {result.ok, std::move(result.payload), std::move(result.message_vector_json)};
}

scratchbird::udr::runtime::UdrStatus FirebirdLifecycle(std::string_view package_uuid) {
  if (package_uuid != kSbuFirebirdPackageUuid) {
    return {false, "UDR.FIREBIRD.PACKAGE_UUID_MISMATCH", "unexpected_package_uuid"};
  }
  return {true, "UDR.OK", {}};
}

scratchbird::udr::runtime::UdrCallResult RuntimeValidateSyntax(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_validate_syntax(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeParseToSblr(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_parse_to_sblr(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeParseConcatenatedDynamicSql(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  constexpr std::string_view kSeparator = "\n--SB_UDR_DYNAMIC_SQL_RIGHT--\n";
  const auto split = input.payload.find(kSeparator);
  const std::string left =
      split == std::string::npos ? input.payload : input.payload.substr(0, split);
  const std::string right = split == std::string::npos
                                ? std::string()
                                : input.payload.substr(split + kSeparator.size());
  return ToRuntimeResult(
      sbu_firebird_parse_concatenated_dynamic_sql(left, right, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeNormalize(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_normalize(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeDescribeStatement(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_describe_statement(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeInstallEnvironment(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_install_environment(input.context_packet, input.payload));
}

scratchbird::udr::runtime::UdrCallResult RuntimeVerifyEnvironment(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_verify_environment(input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeManagementOperationInventory(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_management_operation_inventory(input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeManagementPackageRequest(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(
      sbu_firebird_management_package_request(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeDebugCapabilities(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_debug_capabilities(input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeRenderStatusVector(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_render_status_vector(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeBridgeCapabilities(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_bridge_capabilities(input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeBridgeDispatch(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_firebird_bridge_dispatch(input.payload, input.context_packet));
}

} // namespace

UdrResult sbu_firebird_validate_syntax(std::string_view sql_text,
                                       std::string_view profile) {
  if (profile != "firebird") {
    return Diagnostic("UDR.FIREBIRD.PROFILE_MISMATCH",
                      "Firebird parser-support UDR only accepts the firebird profile.");
  }
  const auto result = scratchbird::parser::firebird::ParseStatement(sql_text);
  return {result.ok, {}, result.message_vector_json};
}

UdrResult sbu_firebird_parse_to_sblr(std::string_view sql_text,
                                     std::string_view context_packet) {
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, "sbu_firebird_parse_to_sblr",
                             "engine_context=trusted;resolver=uuid", failure)) {
    return failure;
  }
  if (!RequireContextField(context_packet, "resolver", "uuid",
                           "sbu_firebird_parse_to_sblr",
                           "engine_context=trusted;resolver=uuid", failure)) {
    return failure;
  }
  const auto result = scratchbird::parser::firebird::ParseStatement(sql_text);
  return {result.ok, result.sblr_envelope, result.message_vector_json};
}

UdrResult sbu_firebird_parse_concatenated_dynamic_sql(std::string_view left_fragment,
                                                      std::string_view right_fragment,
                                                      std::string_view context_packet) {
  constexpr std::string_view function_name =
      "sbu_firebird_parse_concatenated_dynamic_sql";
  constexpr std::string_view required_context =
      "engine_context=trusted;dynamic_sql=psql_execute_statement;resolver=uuid";
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, function_name, required_context, failure)) {
    return failure;
  }
  if (!RequireContextField(context_packet, "dynamic_sql", "psql_execute_statement",
                           function_name, required_context, failure)) {
    return failure;
  }
  if (!RequireContextField(context_packet, "resolver", "uuid", function_name,
                           required_context, failure)) {
    return failure;
  }
  if (HasContextField(context_packet, "security_policy", "deny_dynamic_sql")) {
    return SecurityDenied(function_name, "dynamic SQL is denied by trusted context");
  }
  if (HasContextField(context_packet, "psql_runtime", "procedure") &&
      !HasContextFieldValue(context_packet, "caller_procedure_uuid")) {
    return MissingContext(function_name,
                          "engine_context=trusted;dynamic_sql=psql_execute_statement;resolver=uuid;psql_runtime=procedure;caller_procedure_uuid=<uuid>");
  }
  const std::string combined = std::string(left_fragment) + std::string(right_fragment);
  auto result = scratchbird::parser::firebird::ParseStatement(combined);
  if (!result.ok) return {false, {}, result.message_vector_json};
  const auto procedure_runtime =
      HasContextField(context_packet, "psql_runtime", "procedure");
  const auto caller_uuid = ContextFieldValue(context_packet, "caller_procedure_uuid");
  return {true,
          "{\"dynamic_sql_parser\":\"sbu_firebird_parser_support\","
          "\"source\":\"psql_execute_statement\","
          "\"psql_runtime_context\":\"" +
              std::string(procedure_runtime ? "procedure" : "statement") + "\","
          "\"caller_procedure_uuid\":\"" + EscapeJson(caller_uuid) + "\","
          "\"uuid_resolution\":\"resolver_context_verified\","
          "\"security_verification\":\"" +
              std::string(HasContextField(context_packet, "security_policy", "allow_dynamic_sql")
                              ? "context_policy_allow"
                              : "trusted_context_default") + "\","
          "\"generated_code_admitted\":true,"
          "\"sql_text_included\":false,"
          "\"sblr\":" + result.sblr_envelope + "}",
          result.message_vector_json};
}

UdrResult sbu_firebird_normalize(std::string_view sql_text,
                                 std::string_view profile) {
  if (profile != "firebird") {
    return Diagnostic("UDR.FIREBIRD.PROFILE_MISMATCH",
                      "Firebird parser-support UDR only accepts the firebird profile.");
  }
  const auto result = scratchbird::parser::firebird::ParseStatement(sql_text);
  if (!result.ok) return {false, {}, result.message_vector_json};
  return {true, result.normalized_sql, result.message_vector_json};
}

UdrResult sbu_firebird_describe_statement(std::string_view sql_text,
                                          std::string_view context_packet) {
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, "sbu_firebird_describe_statement",
                             "engine_context=trusted", failure)) {
    return failure;
  }
  const auto result = scratchbird::parser::firebird::ParseStatement(sql_text);
  if (!result.ok) return {false, {}, result.message_vector_json};
  return {true,
          "{\"dialect\":\"firebird\",\"statement_family\":\"" +
              result.statement_family + "\",\"operation_family\":\"" +
              result.operation_family + "\"}",
          result.message_vector_json};
}

UdrResult sbu_firebird_install_environment(std::string_view context_packet,
                                           std::string_view install_mode) {
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, "sbu_firebird_install_environment",
                             "engine_context=trusted;catalog_uuid=<uuid>", failure)) {
    return failure;
  }
  if (install_mode != "install" && install_mode != "verify" &&
      install_mode != "force_reinstall" && install_mode != "upgrade" &&
      install_mode != "repair_version_drift") {
    return Diagnostic("UDR.FIREBIRD.INSTALL_MODE_INVALID",
                      "Firebird environment installer mode must be install, verify, force_reinstall, upgrade, or repair_version_drift.");
  }
  if (!RequireContextFieldValue(context_packet, "catalog_uuid",
                                "sbu_firebird_install_environment",
                                "engine_context=trusted;catalog_uuid=<uuid>", failure)) {
    return failure;
  }
  const auto catalog_uuid = ContextFieldValue(context_packet, "catalog_uuid");
  auto& store = CatalogOverlayInstallStore();
  auto& state = store[catalog_uuid];
  const auto state_before = state;
  if (HasContextField(context_packet, "catalog_overlay_state", "drifted") ||
      HasContextField(context_packet, "version_drift", "detected")) {
    state.installed = true;
    if (state.overlay_revision == 0) state.overlay_revision = 1;
    state.version_drift = true;
  }

  const bool mutation_mode = install_mode == "install" ||
                             install_mode == "force_reinstall" ||
                             install_mode == "upgrade" ||
                             install_mode == "repair_version_drift";
  const bool drift_detected = state.version_drift ||
                              install_mode == "repair_version_drift";
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
          "{\"installer\":\"sbu_firebird_parser_support\","
          "\"role\":\"environment_installer\","
          "\"dialect\":\"firebird\","
          "\"catalog_uuid\":\"" + EscapeJson(catalog_uuid) + "\","
          "\"mode\":\"" + std::string(install_mode) + "\","
          "\"idempotent\":true,"
          "\"atomic\":true,"
          "\"silent_repair\":false,"
          "\"catalog_mutation_applied\":" + BoolJson(mutation_mode) + ","
          "\"catalog_state_before\":\"" + CatalogOverlayStateName(state_before) + "\","
          "\"catalog_state_after\":\"" + CatalogOverlayStateName(state) + "\","
          "\"overlay_revision\":" + std::to_string(state.overlay_revision) + ","
          "\"overlay_profile_version\":" +
              std::to_string(state.overlay_profile_version) + ","
          "\"overlay_row_count\":7,"
          "\"overlay_hash\":\"" + EscapeJson(OverlayHash(catalog_uuid, state)) + "\","
          "\"version_drift_detected\":" + BoolJson(drift_detected) + ","
          "\"version_drift_repaired\":" +
              BoolJson(install_mode == "repair_version_drift") + ","
          "\"catalog_overlays\":[\"RDB$\",\"MON$\",\"SEC$\",\"INFORMATION_SCHEMA\"],"
          "\"mutated_objects\":[\"RDB$DATABASE\",\"RDB$RELATIONS\",\"RDB$FIELDS\",\"RDB$PROCEDURES\",\"RDB$TRIGGERS\",\"MON$ATTACHMENTS\",\"SEC$USERS\"],"
          "\"real_firebird_file_effects\":false}",
          scratchbird::parser::firebird::MessageVectorToJson({})};
}

UdrResult sbu_firebird_verify_environment(std::string_view context_packet) {
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, "sbu_firebird_verify_environment",
                             "engine_context=trusted;catalog_uuid=<uuid>", failure)) {
    return failure;
  }
  if (!RequireContextFieldValue(context_packet, "catalog_uuid",
                                "sbu_firebird_verify_environment",
                                "engine_context=trusted;catalog_uuid=<uuid>", failure)) {
    return failure;
  }
  const auto catalog_uuid = ContextFieldValue(context_packet, "catalog_uuid");
  const auto& store = CatalogOverlayInstallStore();
  const auto found = store.find(catalog_uuid);
  const CatalogOverlayInstallState state =
      found == store.end() ? CatalogOverlayInstallState{} : found->second;
  return {true,
          "{\"installer\":\"sbu_firebird_parser_support\","
          "\"role\":\"environment_verifier\","
          "\"dialect\":\"firebird\","
          "\"catalog_uuid\":\"" + EscapeJson(catalog_uuid) + "\","
          "\"catalog_overlay_installed\":" + BoolJson(state.installed) + ","
          "\"catalog_state\":\"" + CatalogOverlayStateName(state) + "\","
          "\"catalog_overlays_present\":[\"RDB$\",\"MON$\",\"SEC$\",\"INFORMATION_SCHEMA\"],"
          "\"overlay_revision\":" + std::to_string(state.overlay_revision) + ","
          "\"overlay_profile_version\":" +
              std::to_string(state.overlay_profile_version) + ","
          "\"overlay_row_count\":7,"
          "\"overlay_hash\":\"" + EscapeJson(OverlayHash(catalog_uuid, state)) + "\","
          "\"version_drift_detected\":" + BoolJson(state.version_drift) + "}",
          scratchbird::parser::firebird::MessageVectorToJson({})};
}

UdrResult sbu_firebird_management_operation_inventory(std::string_view render_policy) {
  if (render_policy != "release_evidence" &&
      render_policy != "allow_debug_artifacts" &&
      render_policy != "summary") {
    return Diagnostic("UDR.FIREBIRD.MGMT_INVENTORY_POLICY_DENIED",
                      "Firebird management ABI inventory requires release evidence, summary, or debug artifact policy.");
  }
  return {true, ManagementInventoryJson(render_policy),
          scratchbird::parser::firebird::MessageVectorToJson({})};
}

UdrResult sbu_firebird_management_package_request(std::string_view operation_name,
                                                  std::string_view context_packet) {
  constexpr std::string_view function_name =
      "sbu_firebird_management_package_request";
  constexpr std::string_view required_context =
      "engine_context=trusted;package_uuid=<uuid>;request_uuid=<uuid>;operation_policy_ref=<uuid>;transaction_uuid=<uuid>";
  UdrResult failure;
  if (!IsManagementOperation(operation_name)) {
    return Diagnostic("UDR.FIREBIRD.MGMT_OPERATION_UNKNOWN",
                      "Firebird management package request names must be registered in the standard donor management ABI.");
  }
  if (!RequireTrustedContext(context_packet, function_name,
                             required_context, failure)) {
    return failure;
  }
  if (!RequireContextField(context_packet, "package_uuid",
                           kSbuFirebirdPackageUuid, function_name,
                           required_context, failure)) {
    return failure;
  }
  if (!RequireContextFieldValue(context_packet, "request_uuid", function_name,
                                required_context, failure) ||
      !RequireContextFieldValue(context_packet, "operation_policy_ref", function_name,
                                required_context, failure)) {
    return failure;
  }
  if (ManagementOperationMutates(operation_name) &&
      !RequireContextFieldValue(context_packet, "transaction_uuid", function_name,
                                required_context, failure)) {
    return failure;
  }
  const auto result_class = ManagementResultClass(operation_name);
  const bool refused = result_class == "REFUSED_EXTERNAL_AUTHORITY";
  return {true,
          "{\"package\":\"sbup_firebird\","
          "\"package_logical_name\":\"firebird-v5_0\","
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
          "\"requires_mga_transaction\":" +
              BoolJson(ManagementOperationMutates(operation_name)) + ","
          "\"donor_storage_authority\":false,"
          "\"donor_recovery_authority\":false,"
          "\"real_firebird_file_effects\":false,"
          "\"exact_refusal\":" + BoolJson(refused) + ","
          "\"idempotency_state\":\"engine_request_uuid_bound\","
          "\"support_evidence_ref\":\"project/tests/donor_regression/firebird/management_package_abi/management_package_abi_manifest.csv\"}",
          scratchbird::parser::firebird::MessageVectorToJson({})};
}

UdrResult sbu_firebird_debug_capabilities(std::string_view render_policy) {
  if (render_policy != "allow_debug_artifacts") {
    return Diagnostic("SBU_FIREBIRD.DEBUG_POLICY_DENIED",
                      "Debug capability reporting requires explicit debug artifact policy.");
  }
  return {true, scratchbird::parser::firebird::FirebirdPackageIdentityJson(),
          scratchbird::parser::firebird::MessageVectorToJson({})};
}

UdrResult sbu_firebird_render_status_vector(std::string_view message_vector_json,
                                            std::string_view context_packet) {
  constexpr std::string_view function_name = "sbu_firebird_render_status_vector";
  constexpr std::string_view required_context =
      "engine_context=trusted;diagnostic_render=firebird_status_vector";
  UdrResult failure;
  if (!RequireTrustedContext(context_packet, function_name, required_context, failure)) {
    return failure;
  }
  if (!RequireContextField(context_packet, "diagnostic_render",
                           "firebird_status_vector", function_name,
                           required_context, failure)) {
    return failure;
  }
  if (message_vector_json.find("\"diagnostics\"") == std::string_view::npos) {
    return Diagnostic("UDR.FIREBIRD.MESSAGE_VECTOR_INVALID",
                      "Firebird status rendering requires parser message vector JSON.");
  }

  return {true, RenderStatusVectorPayload(ExtractRenderedDiagnostics(message_vector_json)),
          scratchbird::parser::firebird::MessageVectorToJson({})};
}

UdrResult sbu_firebird_bridge_capabilities(std::string_view render_policy) {
  if (render_policy != "summary" &&
      render_policy != "release_evidence" &&
      render_policy != "allow_debug_artifacts" &&
      !HasContextField(render_policy, "engine_context", "trusted")) {
    return BridgeDiagnostic("UDR.BRIDGE.CAPABILITY_POLICY_DENIED",
                            "Firebird bridge capability reporting requires summary, release evidence, debug artifacts, or trusted engine context.");
  }
  return {true, FirebirdBridgeCapabilitiesJson(render_policy),
          scratchbird::parser::firebird::MessageVectorToJson({})};
}

UdrResult sbu_firebird_bridge_dispatch(std::string_view request_packet,
                                       std::string_view context_packet) {
  return FirebirdBridgeDispatch(request_packet, context_packet);
}

scratchbird::udr::runtime::UdrPackageDescriptor sbu_firebird_package_descriptor() {
  scratchbird::udr::runtime::UdrPackageDescriptor descriptor;
  descriptor.package_uuid = std::string(kSbuFirebirdPackageUuid);
  descriptor.package_name = std::string(kSbuFirebirdPackageName);
  descriptor.abi_version = "sb_udr_v1";
  descriptor.source_revision = "firebird-parser-support-db-lifecycle";
  descriptor.binary_hash = "sha256:sbu_firebird_parser_support_builtin";
  descriptor.signature_policy = "builtin-trusted-private";
  descriptor.capability_role = "parser_support.firebird";
  descriptor.trusted_cpp = true;
  descriptor.entrypoints = {
      {"sbu_firebird_validate_syntax", "parser.validate_syntax", &RuntimeValidateSyntax},
      {"sbu_firebird_parse_to_sblr", "parser.parse_to_sblr", &RuntimeParseToSblr},
      {"sbu_firebird_parse_concatenated_dynamic_sql", "parser.dynamic_sql", &RuntimeParseConcatenatedDynamicSql},
      {"sbu_firebird_normalize", "parser.normalize", &RuntimeNormalize},
      {"sbu_firebird_describe_statement", "parser.describe_statement", &RuntimeDescribeStatement},
      {"sbu_firebird_install_environment", "parser.install_environment", &RuntimeInstallEnvironment},
      {"sbu_firebird_verify_environment", "parser.verify_environment", &RuntimeVerifyEnvironment},
      {"sbu_firebird_management_operation_inventory", "parser.management_abi_inventory", &RuntimeManagementOperationInventory},
      {"sbu_firebird_management_package_request", "parser.management_abi_dispatch", &RuntimeManagementPackageRequest},
      {"sbu_firebird_debug_capabilities", "parser.debug_capabilities", &RuntimeDebugCapabilities},
      {"sbu_firebird_render_status_vector", "parser.render_status_vector", &RuntimeRenderStatusVector},
      {"sbu_firebird_bridge_capabilities", "bridge.describe_capabilities", &RuntimeBridgeCapabilities},
      {"sbu_firebird_bridge_dispatch", "bridge.dispatch", &RuntimeBridgeDispatch},
  };
  descriptor.init = &FirebirdLifecycle;
  descriptor.shutdown = &FirebirdLifecycle;
  return descriptor;
}

} // namespace scratchbird::udr::firebird_parser_support
