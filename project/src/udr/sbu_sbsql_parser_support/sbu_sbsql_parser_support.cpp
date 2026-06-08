// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbu_sbsql_parser_support.hpp"

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "common/common.hpp"
#include "expression/expression_catalog.hpp"
#include "lowering/lowering.hpp"
#include "rendering/rendering.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_to_sbsql.hpp"
#include "statement/statement_catalog.hpp"

#include <array>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::udr::sbsql_parser_support {
namespace {

bool HasContextFlag(std::string_view context_packet, std::string_view flag) {
  return context_packet.find(flag) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

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

bool AllowsSourcePreservingDecompile(std::string_view render_policy) {
  return render_policy == "allow_source_preserving_artifacts" ||
         HasContextFlag(render_policy, "decompile_policy=source_preserving") ||
         HasContextFlag(render_policy, "source_preserving=true");
}

bool AllowsDebugArtifacts(std::string_view render_policy) {
  return render_policy == "allow_debug_artifacts" ||
         HasContextFlag(render_policy, "allow_debug_artifacts=true");
}

std::string ContextValue(std::string_view context_packet, std::string_view key) {
  const auto start = context_packet.find(key);
  if (start == std::string_view::npos) return {};
  const auto value_start = start + key.size();
  const auto value_end = context_packet.find(';', value_start);
  return std::string(context_packet.substr(
      value_start,
      value_end == std::string_view::npos ? std::string_view::npos
                                          : value_end - value_start));
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
  const auto semicolon_value = ContextValue(packet, std::string(key) + "=");
  if (!semicolon_value.empty()) return semicolon_value;
  return JsonStringValue(packet, key);
}

std::string ContextValueOr(std::string_view context_packet,
                           std::string_view key,
                           std::string fallback) {
  auto value = ContextValue(context_packet, key);
  return value.empty() ? std::move(fallback) : value;
}

std::uint64_t ContextU64Or(std::string_view context_packet,
                           std::string_view key,
                           std::uint64_t fallback) {
  const auto value = ContextValue(context_packet, key);
  if (value.empty()) return fallback;
  char* end = nullptr;
  const auto parsed = std::strtoull(value.c_str(), &end, 10);
  return end == value.c_str() ? fallback : static_cast<std::uint64_t>(parsed);
}

scratchbird::parser::sbsql::MessageVectorSet ParseOnly(std::string_view sql_text) {
  auto cst = scratchbird::parser::sbsql::BuildCst(sql_text);
  auto ast = scratchbird::parser::sbsql::BuildAst(cst);
  return ast.messages;
}

UdrResult Refuse(std::string code,
                 std::string message,
                 std::vector<scratchbird::parser::sbsql::Field> fields = {}) {
  scratchbird::parser::sbsql::MessageVectorSet messages;
  messages.diagnostics.push_back(scratchbird::parser::sbsql::MakeDiagnostic(
      std::move(code), "ERROR", std::move(message), "sbu_sbsql_parser_support",
      std::move(fields)));
  return {false, {}, scratchbird::parser::sbsql::MessageVectorToJson(messages)};
}

UdrResult RefuseMissingContext(std::string_view function_name,
                               std::string_view required_context_fields) {
  return Refuse("UDR.SBSQL.CONTEXT_MISSING",
                "Parser-support UDR requires engine-supplied context.",
                {{"udr_function", std::string(function_name)},
                 {"required_context_fields", std::string(required_context_fields)},
                 {"operation_uuid", "not_assigned"}});
}

UdrResult BridgeRefuse(std::string code,
                       std::string message,
                       std::vector<scratchbird::parser::sbsql::Field> fields = {}) {
  fields.push_back({"message_vector_class", code});
  return Refuse(std::move(code), std::move(message), std::move(fields));
}

bool HasRawSecretMaterial(std::string_view packet) {
  return HasContextFlag(packet, "password=") ||
         HasContextFlag(packet, "secret=") ||
         HasContextFlag(packet, "\"password\"") ||
         HasContextFlag(packet, "\"secret\"");
}

bool RequireBridgeContext(std::string_view context_packet,
                          std::string_view function_name,
                          UdrResult& failure) {
  constexpr std::string_view required =
      "engine_context=trusted;bridge_authority=engine;user_uuid=<uuid>;"
      "request_uuid=<uuid>;operation_policy_ref=<uuid>";
  if (HasRawSecretMaterial(context_packet)) {
    failure = BridgeRefuse("UDR.BRIDGE.SECRET_MATERIAL_DENIED",
                           "Bridge requests must use secret references, not raw secrets.",
                           {{"udr_function", std::string(function_name)}});
    return false;
  }
  if (!HasContextFlag(context_packet, "engine_context=trusted") ||
      !HasContextFlag(context_packet, "bridge_authority=engine")) {
    failure = BridgeRefuse("UDR.BRIDGE.CONTEXT_MISSING",
                           "Bridge dispatch requires trusted engine bridge context.",
                           {{"udr_function", std::string(function_name)},
                            {"required_context_fields", std::string(required)}});
    return false;
  }
  if (ContextValue(context_packet, "user_uuid=").empty() ||
      ContextValue(context_packet, "request_uuid=").empty() ||
      ContextValue(context_packet, "operation_policy_ref=").empty()) {
    failure = BridgeRefuse("UDR.BRIDGE.CONTEXT_MISSING",
                           "Bridge dispatch requires user, request, and policy identifiers.",
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
  if (operation == "close_session") return "SBLR_BRIDGE_CLOSE_SESSION";
  if (operation == "detach") return "SBLR_BRIDGE_CLOSE_SESSION";
  if (operation == "ping" || operation == "health") return "SBLR_BRIDGE_HEALTH";
  if (operation == "cancel") return "SBLR_BRIDGE_CANCEL";
  if (operation == "drain") return "SBLR_BRIDGE_DRAIN";
  if (operation == "shutdown") return "SBLR_BRIDGE_DRAIN";
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

std::string BridgeCapabilitiesJson(std::string_view family,
                                   std::string_view provider_name,
                                   std::string_view render_policy) {
  const bool include_detail = render_policy == "release_evidence" ||
                              render_policy == "allow_debug_artifacts" ||
                              HasContextFlag(render_policy, "release_evidence=true");
  std::ostringstream out;
  out << "{\"bridge_abi\":\"sb_universal_bridge_v1\","
      << "\"provider\":\"" << EscapeJson(provider_name) << "\","
      << "\"provider_family\":\"" << EscapeJson(family) << "\","
      << "\"package_role\":\"parser_support." << EscapeJson(family) << "\","
      << "\"single_common_abi\":true,"
      << "\"donor_specialization\":\"" << EscapeJson(family) << "\","
      << "\"engine_authorizes_before_udr\":true,"
      << "\"mga_transaction_authority\":\"per_database_engine_mga\","
      << "\"parser_transaction_authority\":false,"
      << "\"udr_transaction_authority\":false,"
      << "\"raw_secret_material_allowed\":false,"
      << "\"secret_refs_required\":true,"
      << "\"cluster_public_implementation\":false,"
      << "\"remote_table_query_class\":\"non_cluster_remote_table\","
      << "\"distributed_query_class\":\"cluster_only\","
      << "\"policy_defaults\":\"deny_by_default\","
      << "\"detail\":\"" << (include_detail ? "release" : "summary") << "\","
      << "\"supported_topologies\":["
      << "\"outbound_federation\",\"inbound_cdc\",\"outbound_replication\","
      << "\"proxy_live_migration\",\"sb_to_sb\",\"logical_backup_restore\"],"
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
  out << "],\"refusal_classes\":["
      << "\"UDR.BRIDGE.CONTEXT_MISSING\","
      << "\"UDR.BRIDGE.SECRET_MATERIAL_DENIED\","
      << "\"UDR.BRIDGE.SANDBOX_DENIED\","
      << "\"UDR.BRIDGE.UNSUPPORTED\","
      << "\"UDR.BRIDGE.MISSING_CAPABILITY\","
      << "\"UDR.BRIDGE.UNLICENSED\","
      << "\"UDR.BRIDGE.STREAM_INVALID\","
      << "\"UDR.BRIDGE.AUTH_FAILED\","
      << "\"UDR.BRIDGE.IDEMPOTENCY_MISSING\","
      << "\"UDR.BRIDGE.CUTOVER_FAILED\"]}";
  return out.str();
}

UdrResult BridgeDispatch(std::string_view request_packet,
                         std::string_view context_packet,
                         std::string_view provider_name,
                         std::string_view family,
                         bool donor_legacy_bridge) {
  if (HasRawSecretMaterial(request_packet)) {
    return BridgeRefuse("UDR.BRIDGE.SECRET_MATERIAL_DENIED",
                        "Bridge requests must use secret references, not raw secrets.",
                        {{"provider", std::string(provider_name)}});
  }
  UdrResult failure;
  if (!RequireBridgeContext(context_packet, "bridge_dispatch", failure)) {
    return failure;
  }

  std::string operation = PacketValue(request_packet, "operation");
  if (operation.empty()) operation = ContextValue(context_packet, "bridge_operation=");
  if (operation.empty()) operation = std::string(request_packet);
  operation = scratchbird::parser::sbsql::TrimAscii(operation);
  if (operation.empty()) {
    return BridgeRefuse("UDR.BRIDGE.MISSING_CAPABILITY",
                        "Bridge dispatch requires an operation name.",
                        {{"provider", std::string(provider_name)}});
  }
  if (operation == "validate") operation = "describe_capabilities";

  if (IsClusterBridgeOperation(operation)) {
    if (HasContextFlag(context_packet, "cluster_provider_gate=admitted")) {
      return BridgeRefuse("UDR.BRIDGE.UNLICENSED",
                          "Cluster bridge commands are routed to the public stub and are unlicensed in this build.",
                          {{"provider", std::string(provider_name)},
                           {"operation", operation},
                           {"cluster_stub_route", "public_stub_unlicensed"}});
    }
    return BridgeRefuse("UDR.BRIDGE.UNSUPPORTED",
                        "Cluster bridge commands are disabled by compile-time policy.",
                        {{"provider", std::string(provider_name)},
                         {"operation", operation},
                         {"cluster_stub_route", "compile_time_gate_disabled"}});
  }

  if (!IsSupportedBridgeOperation(operation)) {
    return BridgeRefuse("UDR.BRIDGE.UNSUPPORTED",
                        "Bridge operation is not part of the universal public bridge ABI.",
                        {{"provider", std::string(provider_name)},
                         {"operation", operation}});
  }

  const std::string stream_kind = PacketValue(request_packet, "stream_kind");
  if (stream_kind == "physical_page_copy" ||
      stream_kind == "physical_backup" ||
      stream_kind == "server_local_file") {
    return BridgeRefuse("UDR.BRIDGE.SANDBOX_DENIED",
                        "Bridge dispatch denies physical page-copy and server-local file streams.",
                        {{"provider", std::string(provider_name)},
                         {"operation", operation},
                         {"stream_kind", stream_kind}});
  }
  if (operation == "execute" &&
      (PacketValue(request_packet, "maintenance") == "repair" ||
       PacketValue(request_packet, "maintenance") == "verify")) {
    return BridgeRefuse("UDR.BRIDGE.UNSUPPORTED",
                        "Low-level repair and verification are SBsql-only maintenance surfaces.",
                        {{"provider", std::string(provider_name)},
                         {"operation", operation}});
  }

  if (operation == "authenticate" &&
      ContextValue(context_packet, "target_auth_state=") == "failed") {
    return BridgeRefuse("UDR.BRIDGE.AUTH_FAILED",
                        "Target bridge authentication failed.",
                        {{"provider", std::string(provider_name)}});
  }

  if (IsTransactionBridgeOperation(operation) &&
      ContextValue(context_packet, "local_transaction_uuid=").empty()) {
    return BridgeRefuse("UDR.BRIDGE.CONTEXT_MISSING",
                        "Bridge transaction operations require a local MGA transaction UUID.",
                        {{"provider", std::string(provider_name)},
                         {"operation", operation},
                         {"required_context_fields", "local_transaction_uuid=<uuid>"}});
  }
  if (operation == "prepare" &&
      !HasContextFlag(context_packet, "remote_supports_prepare=true")) {
    return BridgeRefuse("UDR.BRIDGE.MISSING_CAPABILITY",
                        "Bridge prepare is admitted only when the remote provider supports prepare.",
                        {{"provider", std::string(provider_name)},
                         {"operation", operation}});
  }

  if (IsStreamBridgeOperation(operation) &&
      (operation == "stream_read" || operation == "stream_write" ||
       operation == "stream_close") &&
      PacketValue(request_packet, "stream_uuid").empty()) {
    return BridgeRefuse("UDR.BRIDGE.STREAM_INVALID",
                        "Bridge stream continuation requires a stream UUID.",
                        {{"provider", std::string(provider_name)},
                         {"operation", operation}});
  }
  if ((operation == "cdc_apply" || operation == "cdc_read") &&
      PacketValue(request_packet, "idempotency_key").empty()) {
    return BridgeRefuse("UDR.BRIDGE.IDEMPOTENCY_MISSING",
                        "Bridge CDC read/apply requires an idempotency key.",
                        {{"provider", std::string(provider_name)},
                         {"operation", operation}});
  }
  if (operation == "cutover" &&
      !HasContextFlag(context_packet, "cutover_evidence=validated")) {
    return BridgeRefuse("UDR.BRIDGE.CUTOVER_FAILED",
                        "Bridge cutover requires validated compare and drain evidence.",
                        {{"provider", std::string(provider_name)},
                         {"operation", operation}});
  }

  std::ostringstream out;
  out << "{\"bridge_abi\":\"sb_universal_bridge_v1\","
      << "\"provider\":\"" << EscapeJson(provider_name) << "\","
      << "\"provider_family\":\"" << EscapeJson(family) << "\","
      << "\"donor_legacy_bridge\":" << BoolJson(donor_legacy_bridge) << ','
      << "\"operation\":\"" << EscapeJson(operation) << "\","
      << "\"sblr_opcode\":\"" << EscapeJson(BridgeOpcodeForOperation(operation)) << "\","
      << "\"result_class\":\"admitted\","
      << "\"engine_authorized_context\":true,"
      << "\"parser_authority\":false,"
      << "\"udr_transaction_authority\":false,"
      << "\"mga_transaction_authority\":\"per_database_engine_mga\","
      << "\"local_transaction_uuid\":\""
      << EscapeJson(ContextValue(context_packet, "local_transaction_uuid=")) << "\","
      << "\"remote_transaction_ref\":\""
      << EscapeJson(ContextValue(context_packet, "remote_transaction_ref=")) << "\","
      << "\"user_uuid\":\"" << EscapeJson(ContextValue(context_packet, "user_uuid="))
      << "\","
      << "\"request_uuid\":\""
      << EscapeJson(ContextValue(context_packet, "request_uuid=")) << "\","
      << "\"operation_policy_ref\":\""
      << EscapeJson(ContextValue(context_packet, "operation_policy_ref=")) << "\","
      << "\"secret_refs_only\":true,"
      << "\"raw_sql_text_included\":false,"
      << "\"raw_secret_material_included\":false,"
      << "\"cluster_execution\":false,"
      << "\"remote_table_semantics\":\"non_cluster_remote_table\","
      << "\"distributed_query_semantics\":\"cluster_only_refused_in_public_bridge\"";
  if (!stream_kind.empty()) {
    out << ",\"stream_kind\":\"" << EscapeJson(stream_kind) << "\"";
  }
  if (operation == "describe_capabilities") {
    out << ",\"capabilities\":" << BridgeCapabilitiesJson(family, provider_name, "summary");
  }
  out << "}";
  return {true, out.str(), scratchbird::parser::sbsql::MessageVectorToJson({})};
}

scratchbird::parser::sbsql::SessionContext SessionFromContext(std::string_view context_packet) {
  scratchbird::parser::sbsql::SessionContext session;
  session.authenticated = HasContextFlag(context_packet, "engine_context=trusted") ||
                          HasContextFlag(context_packet, "authenticated=true");
  session.session_uuid = ContextValueOr(
      context_packet, "session_uuid=", "019e13c0-0000-7000-8000-000000000008");
  session.connection_uuid = ContextValueOr(
      context_packet, "connection_uuid=", "019e13c0-0000-7000-8000-000000000108");
  session.database_uuid = ContextValueOr(
      context_packet, "database_uuid=", "019e13c0-0000-7000-8000-000000000208");
  session.catalog_epoch = ContextU64Or(context_packet, "catalog_epoch=", 1);
  session.security_policy_epoch =
      ContextU64Or(context_packet, "security_policy_epoch=", 1);
  session.descriptor_epoch = ContextU64Or(context_packet, "descriptor_epoch=", 1);
  session.transaction_context = ContextValueOr(
      context_packet, "transaction_context=", "udr.engine_supplied_context");
  return session;
}

scratchbird::parser::sbsql::ParserConfig ParserConfigFromContext(std::string_view context_packet) {
  scratchbird::parser::sbsql::ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = ContextValueOr(
      context_packet, "parser_uuid=", "019e13c0-0000-7000-8000-000000000308");
  config.bundle_contract_id = ContextValueOr(
      context_packet, "bundle_contract_id=", "sbu_sbsql_parser_support@1");
  config.build_id =
      ContextValueOr(context_packet, "build_id=", "udr-parser-support");
  if (HasContextFlag(context_packet, "resolver=public")) {
    config.server_endpoint = "engine-context:ResolveNameRegistryPublic";
  }
  return config;
}

UdrResult ParseBindLower(std::string_view sql_text, std::string_view context_packet) {
  if (!HasContextFlag(context_packet, "engine_context=trusted")) {
    return RefuseMissingContext("sbu_sbsql_parse_to_sblr",
                                "engine_context=trusted;resolver=public");
  }
  const auto session = SessionFromContext(context_packet);
  auto config = ParserConfigFromContext(context_packet);
  auto cst = scratchbird::parser::sbsql::BuildCst(sql_text);
  auto ast = scratchbird::parser::sbsql::BuildAst(cst);
  if (ast.messages.has_errors()) {
    return {false, {}, scratchbird::parser::sbsql::MessageVectorToJson(ast.messages)};
  }
  std::vector<std::string> resolved;
  const auto resolved_uuid = ContextValue(context_packet, "resolved_uuid=");
  if (ast.requires_name_resolution && !resolved_uuid.empty()) {
    resolved.push_back(resolved_uuid);
  }
  auto bound = scratchbird::parser::sbsql::BindAst(ast, cst, config, session, resolved);
  if (bound.messages.has_errors() || !bound.bound) {
    return {false, {}, scratchbird::parser::sbsql::MessageVectorToJson(bound.messages)};
  }
  auto envelope = scratchbird::parser::sbsql::LowerToSblr(bound, cst, session);
  auto verifier = scratchbird::parser::sbsql::VerifySblrEnvelope(envelope);
  if (!verifier.admitted) {
    return {false, {}, scratchbird::parser::sbsql::MessageVectorToJson(verifier.messages)};
  }
  return {true, envelope.payload, scratchbird::parser::sbsql::MessageVectorToJson(envelope.messages)};
}

scratchbird::udr::runtime::UdrCallResult ToRuntimeResult(UdrResult result) {
  return {result.ok, std::move(result.payload), std::move(result.message_vector_json)};
}

scratchbird::udr::runtime::UdrStatus SbsqlLifecycle(std::string_view package_uuid) {
  if (package_uuid != kSbuSbsqlPackageUuid) {
    return {false, "UDR.SBSQL.PACKAGE_UUID_MISMATCH", "unexpected_package_uuid"};
  }
  return {true, "UDR.OK", {}};
}

scratchbird::udr::runtime::UdrCallResult RuntimeValidateSyntax(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_sbsql_validate_syntax(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeParseToSblr(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_sbsql_parse_to_sblr(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeParseExpression(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_sbsql_parse_expression(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeNormalize(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_sbsql_normalize(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeDescribeStatement(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_sbsql_describe_statement(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeDecompileSblr(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_sbsql_decompile_sblr(input.payload, input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeDebugCapabilities(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_sbsql_debug_capabilities(input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeBridgeCapabilities(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_sbsql_bridge_capabilities(input.context_packet));
}

scratchbird::udr::runtime::UdrCallResult RuntimeBridgeDispatch(
    const scratchbird::udr::runtime::UdrCallInput& input) {
  return ToRuntimeResult(sbu_sbsql_bridge_dispatch(input.payload, input.context_packet));
}

} // namespace

UdrResult sbu_sbsql_validate_syntax(std::string_view sql_text, std::string_view profile) {
  (void)profile;
  auto messages = ParseOnly(sql_text);
  return {!messages.has_errors(), {}, scratchbird::parser::sbsql::MessageVectorToJson(messages)};
}

UdrResult sbu_sbsql_parse_to_sblr(std::string_view sql_text, std::string_view context_packet) {
  return ParseBindLower(sql_text, context_packet);
}

UdrResult sbu_sbsql_parse_expression(std::string_view sql_text, std::string_view descriptor_context) {
  if (!HasContextFlag(descriptor_context, "descriptor_context=engine")) {
    return RefuseMissingContext("sbu_sbsql_parse_expression", "descriptor_context=engine");
  }
  auto messages = ParseOnly("SELECT " + std::string(sql_text));
  if (messages.has_errors()) return {false, {}, scratchbird::parser::sbsql::MessageVectorToJson(messages)};
  const auto descriptor_count =
      scratchbird::parser::sbsql::BuiltinExpressionSurfaceDescriptors().size();
  return {true, "{\"expression_parser\":\"sbu_sbsql_parser_support\",\"descriptor_count\":" +
                    std::to_string(descriptor_count) + "}",
          scratchbird::parser::sbsql::MessageVectorToJson(messages)};
}

UdrResult sbu_sbsql_normalize(std::string_view sql_text, std::string_view profile) {
  (void)profile;
  auto messages = ParseOnly(sql_text);
  if (messages.has_errors()) return {false, {}, scratchbird::parser::sbsql::MessageVectorToJson(messages)};
  return {true, scratchbird::parser::sbsql::TrimAscii(sql_text), scratchbird::parser::sbsql::MessageVectorToJson(messages)};
}

UdrResult sbu_sbsql_describe_statement(std::string_view sql_text, std::string_view context_packet) {
  if (!HasContextFlag(context_packet, "engine_context=trusted")) {
    return RefuseMissingContext("sbu_sbsql_describe_statement", "engine_context=trusted");
  }
  auto cst = scratchbird::parser::sbsql::BuildCst(sql_text);
  auto ast = scratchbird::parser::sbsql::BuildAst(cst);
  if (ast.messages.has_errors()) return {false, {}, scratchbird::parser::sbsql::MessageVectorToJson(ast.messages)};
  return {true, "{\"statement_kind\":\"" + ast.statement_kind +
                    "\",\"surface_id\":\"" + ast.statement_surface_id +
                    "\",\"operation_family\":\"" + ast.operation_family + "\"}",
          scratchbird::parser::sbsql::MessageVectorToJson(ast.messages)};
}

UdrResult sbu_sbsql_decompile_sblr(std::string_view sblr_packet, std::string_view render_policy) {
  if (AllowsSourcePreservingDecompile(render_policy)) {
    const auto decoded = scratchbird::engine::sblr::DecodeSblrEnvelope(sblr_packet);
    if (!decoded.ok) {
      const auto code = decoded.diagnostics.empty()
                            ? std::string("SB_SBLR_TO_SBSQL_DECODE_FAILED")
                            : decoded.diagnostics.front().code;
      const auto message = decoded.diagnostics.empty()
                               ? std::string("SBLR envelope decode failed")
                               : decoded.diagnostics.front().message;
      return Refuse(code, message);
    }
    scratchbird::engine::sblr::SblrToSbsqlOptions options;
    options.source_preserving = true;
    const auto rendered =
        scratchbird::engine::sblr::RenderSblrEnvelopeToSbsql(decoded.envelope, options);
    if (!rendered.ok) {
      const auto code = rendered.diagnostics.empty()
                            ? std::string("SB_SBLR_TO_SBSQL_FAILED")
                            : rendered.diagnostics.front().code;
      const auto message = rendered.diagnostics.empty()
                               ? std::string("SBLR-to-SBsql conversion failed")
                               : rendered.diagnostics.front().message;
      return Refuse(code, message);
    }
    scratchbird::parser::sbsql::MessageVectorSet messages;
    return {true, rendered.sbsql_text,
            scratchbird::parser::sbsql::MessageVectorToJson(messages)};
  }
  if (!AllowsDebugArtifacts(render_policy)) {
    return Refuse("SBU_SBSQL.DECOMPILE_POLICY_REFUSED", "SBLR decompilation requires explicit debug artifact policy");
  }
  scratchbird::parser::sbsql::MessageVectorSet messages;
  return {true, "<sblr-debug-text-redacted>", scratchbird::parser::sbsql::MessageVectorToJson(messages)};
}

UdrResult sbu_sbsql_debug_capabilities(std::string_view render_policy) {
  if (render_policy != "allow_debug_artifacts") {
    return Refuse("SBU_SBSQL.DEBUG_POLICY_REFUSED",
                  "debug capability reporting requires explicit debug artifact policy");
  }
  return {true,
          "{\"validate_syntax\":true,\"parse_to_sblr\":true,\"parse_expression\":true,"
          "\"describe_statement\":true,\"normalize\":true,\"decompile_sblr\":\"redacted\"}",
          scratchbird::parser::sbsql::MessageVectorToJson({})};
}

UdrResult sbu_sbsql_bridge_capabilities(std::string_view render_policy) {
  if (render_policy != "summary" &&
      render_policy != "release_evidence" &&
      render_policy != "allow_debug_artifacts" &&
      !HasContextFlag(render_policy, "engine_context=trusted")) {
    return BridgeRefuse("UDR.BRIDGE.CAPABILITY_POLICY_DENIED",
                        "Bridge capability reporting requires summary, release evidence, debug artifacts, or trusted engine context.");
  }
  return {true, BridgeCapabilitiesJson("sbsql", "sbu_sbsql_parser_support", render_policy),
          scratchbird::parser::sbsql::MessageVectorToJson({})};
}

UdrResult sbu_sbsql_bridge_dispatch(std::string_view request_packet,
                                    std::string_view context_packet) {
  return BridgeDispatch(request_packet, context_packet,
                        "sbu_sbsql_parser_support", "sbsql",
                        false);
}

scratchbird::udr::runtime::UdrPackageDescriptor sbu_sbsql_package_descriptor() {
  scratchbird::udr::runtime::UdrPackageDescriptor descriptor;
  descriptor.package_uuid = std::string(kSbuSbsqlPackageUuid);
  descriptor.package_name = std::string(kSbuSbsqlPackageName);
  descriptor.abi_version = "sb_udr_v1";
  descriptor.source_revision = "sbsql-parser-support-db-lifecycle";
  descriptor.binary_hash = "sha256:sbu_sbsql_parser_support_builtin";
  descriptor.signature_policy = "builtin-trusted-private";
  descriptor.capability_role = "parser_support.sbsql";
  descriptor.trusted_cpp = true;
  descriptor.entrypoints = {
      {"sbu_sbsql_validate_syntax", "parser.validate_syntax", &RuntimeValidateSyntax},
      {"sbu_sbsql_parse_to_sblr", "parser.parse_to_sblr", &RuntimeParseToSblr},
      {"sbu_sbsql_parse_expression", "parser.parse_expression", &RuntimeParseExpression},
      {"sbu_sbsql_normalize", "parser.normalize", &RuntimeNormalize},
      {"sbu_sbsql_describe_statement", "parser.describe_statement", &RuntimeDescribeStatement},
      {"sbu_sbsql_decompile_sblr", "parser.decompile_sblr", &RuntimeDecompileSblr},
      {"sbu_sbsql_debug_capabilities", "parser.debug_capabilities", &RuntimeDebugCapabilities},
      {"sbu_sbsql_bridge_capabilities", "bridge.describe_capabilities", &RuntimeBridgeCapabilities},
      {"sbu_sbsql_bridge_dispatch", "bridge.dispatch", &RuntimeBridgeDispatch},
  };
  descriptor.init = &SbsqlLifecycle;
  descriptor.shutdown = &SbsqlLifecycle;
  return descriptor;
}

} // namespace scratchbird::udr::sbsql_parser_support
