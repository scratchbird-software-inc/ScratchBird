// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbu_sbsql_parser_support.hpp"

#include "sblr_engine_envelope.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace runtime = scratchbird::udr::runtime;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kSourcePreservingPolicy =
    "allow_debug_artifacts=true;decompile_policy=source_preserving";
constexpr std::string_view kRelationUuid =
    "019dffbb-f000-7000-8000-000000000201";
constexpr std::string_view kTrustedBridgeContext =
    "engine_context=trusted;bridge_authority=engine;"
    "user_uuid=019e13c0-0000-7000-8000-00000000b001;"
    "request_uuid=019e13c0-0000-7000-8000-00000000b002;"
    "operation_policy_ref=019e13c0-0000-7000-8000-00000000b003;"
    "bridge_connection_uuid=019e13c0-0000-7000-8000-00000000b004;"
    "local_transaction_uuid=019e13c0-0000-7000-8000-00000000b005;"
    "remote_transaction_ref=remote-txn-1;remote_supports_prepare=true;"
    "cutover_evidence=validated";

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool ExpectOk(const scratchbird::udr::sbsql_parser_support::UdrResult& result,
              std::string_view label) {
  if (result.ok) return true;
  std::cerr << label << " unexpectedly failed: " << result.message_vector_json << '\n';
  return false;
}

bool ExpectRefusal(const scratchbird::udr::sbsql_parser_support::UdrResult& result,
                   std::string_view code,
                   std::string_view label) {
  if (!result.ok && Contains(result.message_vector_json, code)) return true;
  std::cerr << label << " did not fail closed with " << code
            << ": ok=" << result.ok
            << " message_vector_json=" << result.message_vector_json << '\n';
  return false;
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool ExpectRuntimeOk(const runtime::UdrCallResult& result,
                     std::string_view label) {
  if (result.ok) return true;
  std::cerr << label << " unexpectedly failed: " << result.message_vector_json << '\n';
  return false;
}

bool ExpectRuntimeRefusal(const runtime::UdrCallResult& result,
                          std::string_view code,
                          std::string_view label) {
  if (!result.ok && Contains(result.message_vector_json, code)) return true;
  std::cerr << label << " did not fail closed with " << code
            << ": ok=" << result.ok
            << " message_vector_json=" << result.message_vector_json << '\n';
  return false;
}

runtime::UdrCallInput Input(std::string entrypoint,
                            std::string payload,
                            std::string context_packet = {}) {
  runtime::UdrCallInput input;
  input.package_uuid = std::string(scratchbird::udr::sbsql_parser_support::kSbuSbsqlPackageUuid);
  input.entrypoint = std::move(entrypoint);
  input.payload = std::move(payload);
  input.context_packet = std::move(context_packet);
  return input;
}

const runtime::UdrEntrypointDescriptor* FindEntrypoint(
    const runtime::UdrPackageDescriptor& descriptor,
    std::string_view name) {
  for (const auto& entrypoint : descriptor.entrypoints) {
    if (entrypoint.name == name) return &entrypoint;
  }
  return nullptr;
}

sblr::SblrOperand Operand(std::string name, std::string value) {
  sblr::SblrOperand operand;
  operand.type = "text";
  operand.name = std::move(name);
  operand.value = std::move(value);
  return operand;
}

sblr::SblrSourceSymbolArtifact Symbol(std::string symbol_kind,
                                      std::string stable_key,
                                      std::string resolved_uuid,
                                      std::string render_hint,
                                      std::string scope) {
  sblr::SblrSourceSymbolArtifact symbol;
  symbol.symbol_kind = std::move(symbol_kind);
  symbol.stable_key = std::move(stable_key);
  symbol.resolved_uuid = std::move(resolved_uuid);
  symbol.render_hint = std::move(render_hint);
  symbol.scope = std::move(scope);
  symbol.source_hash = "sha256:parser-udr-source-symbol";
  return symbol;
}

sblr::SblrOperationEnvelope BuildSourcePreservingEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("general.procedural_operation",
                                         "SBLR_GENERAL_PROCEDURAL_OPERATION",
                                         "CBQ-020-CORE-PARSER-UDR-DECOMPILE");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_procedural_bundle_v1"));
  envelope.operands.push_back(Operand("authority_descriptor_uuid",
                                      "019dffbb-f000-7000-8000-000000000202"));
  envelope.operands.push_back(Operand("relation_object_uuid",
                                      std::string(kRelationUuid)));
  envelope.operands.push_back(Operand("variable_type", "INT"));

  envelope.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  envelope.source_artifact_map.source_identity = "CBQ-020-core-parser-udr-source-preserving";
  envelope.source_artifact_map.source_hash = "sha256:cbq020-source-map";
  envelope.source_artifact_map.render_metadata_only = true;
  envelope.source_artifact_map.contains_sql_text = false;
  envelope.source_artifact_map.raw_sql_text_authoritative = false;
  envelope.source_artifact_map.symbols.push_back(
      Symbol("variable", "var.v_udr_ready", "", "v_udr_ready", "procedure.local"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("parameter", "param.p_udr_id", "", ":p_udr_id", "routine.input"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("cursor", "cursor.udr_scan", "", "udr_scan", "procedure.cursor"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("label", "label.udr_ready_loop", "", "udr_ready_loop", "procedure.label"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("exception_handler", "handler.udr_not_found", "", "udr_not_found", "procedure.handler"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("relation_alias", "alias.udr_ready.u", "", "u", "query.range"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("column_alias", "alias.column.udr_id", "", "udr_id", "query.projection"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("object_display_name", "object.udr_ready", std::string(kRelationUuid), "udr_ready", "query.from"));
  return envelope;
}

void CheckDescriptorAndLifecycle() {
  using namespace scratchbird::udr::sbsql_parser_support;
  auto descriptor = sbu_sbsql_package_descriptor();
  Require(descriptor.package_uuid == kSbuSbsqlPackageUuid, "package uuid mismatch");
  Require(descriptor.package_name == kSbuSbsqlPackageName, "package name mismatch");
  Require(descriptor.abi_version == "sb_udr_v1", "abi version mismatch");
  Require(descriptor.source_revision == "sbsql-parser-support-db-lifecycle",
          "source revision mismatch");
  Require(descriptor.binary_hash == "sha256:sbu_sbsql_parser_support_builtin",
          "binary hash mismatch");
  Require(descriptor.signature_policy == "builtin-trusted-private",
          "signature policy mismatch");
  Require(descriptor.capability_role == "parser_support.sbsql",
          "capability role mismatch");
  Require(descriptor.trusted_cpp, "trusted C++ marker missing");
  Require(descriptor.init != nullptr, "init lifecycle missing");
  Require(descriptor.shutdown != nullptr, "shutdown lifecycle missing");

  const std::vector<std::pair<std::string_view, std::string_view>> expected = {
      {"sbu_sbsql_validate_syntax", "parser.validate_syntax"},
      {"sbu_sbsql_parse_to_sblr", "parser.parse_to_sblr"},
      {"sbu_sbsql_parse_expression", "parser.parse_expression"},
      {"sbu_sbsql_normalize", "parser.normalize"},
      {"sbu_sbsql_describe_statement", "parser.describe_statement"},
      {"sbu_sbsql_decompile_sblr", "parser.decompile_sblr"},
      {"sbu_sbsql_debug_capabilities", "parser.debug_capabilities"},
      {"sbu_sbsql_bridge_capabilities", "bridge.describe_capabilities"},
      {"sbu_sbsql_bridge_dispatch", "bridge.dispatch"},
  };
  Require(descriptor.entrypoints.size() == expected.size(), "entrypoint count mismatch");
  for (const auto& [name, role] : expected) {
    const auto* entrypoint = FindEntrypoint(descriptor, name);
    Require(entrypoint != nullptr, std::string("missing entrypoint ") + std::string(name));
    Require(entrypoint->role == role, std::string("entrypoint role mismatch for ") + std::string(name));
    Require(entrypoint->callback != nullptr, std::string("entrypoint callback missing for ") + std::string(name));
    Require(Contains(role, "parser.") || Contains(role, "bridge."),
            "entrypoint is not parser or bridge scoped");
  }

  const auto init = descriptor.init(descriptor.package_uuid);
  Require(init.ok && init.diagnostic_code == "UDR.OK", "init lifecycle failed");
  const auto shutdown = descriptor.shutdown(descriptor.package_uuid);
  Require(shutdown.ok && shutdown.diagnostic_code == "UDR.OK", "shutdown lifecycle failed");
  const auto init_mismatch = descriptor.init("019e13c0-0000-7000-8000-00000000ffff");
  Require(!init_mismatch.ok &&
              init_mismatch.diagnostic_code == "UDR.SBSQL.PACKAGE_UUID_MISMATCH",
          "init UUID mismatch did not refuse deterministically");
  const auto shutdown_mismatch = descriptor.shutdown("019e13c0-0000-7000-8000-00000000ffff");
  Require(!shutdown_mismatch.ok &&
              shutdown_mismatch.diagnostic_code == "UDR.SBSQL.PACKAGE_UUID_MISMATCH",
          "shutdown UUID mismatch did not refuse deterministically");
}

void CheckRuntimeRegistrationAndEntrypoints() {
  using namespace scratchbird::udr::sbsql_parser_support;
  runtime::ResetRuntimeForTest();
  const auto descriptor = sbu_sbsql_package_descriptor();
  const auto registered = runtime::RegisterPackage(descriptor);
  Require(registered.ok, "package registration failed");
  const auto found = runtime::FindPackageDescriptor(kSbuSbsqlPackageUuid);
  Require(found.has_value(), "registered package descriptor not found");
  const auto loaded = runtime::LoadPackage(kSbuSbsqlPackageUuid);
  Require(loaded.ok, "package load failed");
  const auto state = runtime::GetPackageState(kSbuSbsqlPackageUuid);
  Require(state.has_value() && state->registered && state->loaded,
          "package state not registered and loaded");
  Require(state->capability_role == "parser_support.sbsql",
          "runtime package capability role mismatch");
  Require(state->entrypoint_names.size() == descriptor.entrypoints.size(),
          "runtime entrypoint inventory mismatch");

  Require(ExpectRuntimeOk(runtime::InvokePackage(Input("sbu_sbsql_validate_syntax", "select 1", "sbsql")),
                          "runtime validate_syntax"),
          "runtime validate_syntax failed");
  Require(ExpectRuntimeOk(runtime::InvokePackage(Input("sbu_sbsql_normalize", "  select 1  ", "sbsql")),
                          "runtime normalize"),
          "runtime normalize failed");
  Require(ExpectRuntimeRefusal(runtime::InvokePackage(Input("sbu_sbsql_parse_to_sblr", "select 1")),
                               "UDR.SBSQL.CONTEXT_MISSING",
                               "runtime parse_to_sblr missing context"),
          "runtime parse_to_sblr missing context did not refuse");
  Require(ExpectRuntimeOk(runtime::InvokePackage(Input("sbu_sbsql_parse_to_sblr",
                                                       "select 1",
                                                       "engine_context=trusted;resolver=public")),
                          "runtime parse_to_sblr trusted"),
          "runtime parse_to_sblr trusted failed");
  Require(ExpectRuntimeRefusal(runtime::InvokePackage(Input("sbu_sbsql_parse_expression", "1 + 1")),
                               "UDR.SBSQL.CONTEXT_MISSING",
                               "runtime parse_expression missing context"),
          "runtime parse_expression missing context did not refuse");
  Require(ExpectRuntimeOk(runtime::InvokePackage(Input("sbu_sbsql_parse_expression",
                                                       "1 + 1",
                                                       "descriptor_context=engine")),
                          "runtime parse_expression descriptor context"),
          "runtime parse_expression descriptor context failed");
  Require(ExpectRuntimeRefusal(runtime::InvokePackage(Input("sbu_sbsql_describe_statement", "select 1")),
                               "UDR.SBSQL.CONTEXT_MISSING",
                               "runtime describe_statement missing context"),
          "runtime describe_statement missing context did not refuse");
  Require(ExpectRuntimeOk(runtime::InvokePackage(Input("sbu_sbsql_describe_statement",
                                                       "select 1",
                                                       "engine_context=trusted")),
                          "runtime describe_statement trusted"),
          "runtime describe_statement trusted failed");
  Require(ExpectRuntimeRefusal(runtime::InvokePackage(Input("sbu_sbsql_decompile_sblr",
                                                           "packet",
                                                           "normal")),
                               "SBU_SBSQL.DECOMPILE_POLICY_REFUSED",
                               "runtime decompile normal policy"),
          "runtime decompile normal policy did not refuse");
  Require(ExpectRuntimeOk(runtime::InvokePackage(Input("sbu_sbsql_decompile_sblr",
                                                       "packet",
                                                       "allow_debug_artifacts")),
                          "runtime decompile debug policy"),
          "runtime decompile debug policy failed");
  Require(ExpectRuntimeRefusal(runtime::InvokePackage(Input("sbu_sbsql_debug_capabilities",
                                                           "",
                                                           "normal")),
                               "SBU_SBSQL.DEBUG_POLICY_REFUSED",
                               "runtime debug_capabilities normal policy"),
          "runtime debug_capabilities normal policy did not refuse");
  Require(ExpectRuntimeOk(runtime::InvokePackage(Input("sbu_sbsql_debug_capabilities",
                                                       "",
                                                       "allow_debug_artifacts")),
                          "runtime debug_capabilities"),
          "runtime debug_capabilities failed");
  Require(ExpectRuntimeOk(runtime::InvokePackage(Input("sbu_sbsql_bridge_capabilities",
                                                       "",
                                                       "summary")),
                          "runtime bridge_capabilities"),
          "runtime bridge_capabilities failed");
  Require(ExpectRuntimeRefusal(runtime::InvokePackage(Input("sbu_sbsql_bridge_dispatch",
                                                           "operation=open_session",
                                                           "")),
                               "UDR.BRIDGE.CONTEXT_MISSING",
                               "runtime bridge_dispatch missing context"),
          "runtime bridge_dispatch missing context did not refuse");
  Require(ExpectRuntimeOk(runtime::InvokePackage(Input("sbu_sbsql_bridge_dispatch",
                                                       "operation=open_session",
                                                       std::string(kTrustedBridgeContext))),
                          "runtime bridge_dispatch open_session"),
          "runtime bridge_dispatch open_session failed");

  const auto unloaded = runtime::UnloadPackage(kSbuSbsqlPackageUuid);
  Require(unloaded.ok, "package unload failed");
  runtime::ResetRuntimeForTest();
}

void CheckSourcePreservingDecompile() {
  using namespace scratchbird::udr::sbsql_parser_support;
  const auto encoded = sblr::EncodeSblrEnvelope(BuildSourcePreservingEnvelope());
  const auto decompiled = sbu_sbsql_decompile_sblr(encoded, kSourcePreservingPolicy);
  Require(ExpectOk(decompiled, "source_preserving_decompile"),
          "source preserving decompile failed");
  Require(Contains(decompiled.payload, "DECLARE VARIABLE v_udr_ready INT;"),
          "source preserving decompile did not preserve variable name");
  Require(Contains(decompiled.payload, "PARAM LIST p_udr_id;"),
          "source preserving decompile did not preserve parameter name");
  Require(Contains(decompiled.payload, "DECLARE udr_scan CURSOR;"),
          "source preserving decompile did not preserve cursor name");
  Require(Contains(decompiled.payload, "PSQL LEAVE udr_ready_loop;"),
          "source preserving decompile did not preserve label name");
  Require(Contains(decompiled.payload, "EXCEPTION HANDLER WHEN udr_not_found;"),
          "source preserving decompile did not preserve exception handler name");
  Require(Contains(decompiled.payload, "FROM udr_ready AS u"),
          "source preserving decompile did not preserve relation alias");
  Require(Contains(decompiled.payload, "AS udr_id"),
          "source preserving decompile did not preserve column alias");
  Require(!Contains(decompiled.payload, "<sblr-debug-text-redacted>"),
          "source preserving decompile returned debug redaction placeholder");
}

void CheckUniversalBridgeContract() {
  using namespace scratchbird::udr::sbsql_parser_support;
  const auto capabilities = sbu_sbsql_bridge_capabilities("release_evidence");
  Require(ExpectOk(capabilities, "bridge_capabilities"),
          "bridge capabilities failed");
  Require(Contains(capabilities.payload, "\"bridge_abi\":\"sb_universal_bridge_v1\""),
          "bridge capabilities missing universal ABI marker");
  Require(Contains(capabilities.payload, "\"provider_family\":\"sbsql\""),
          "bridge capabilities missing SBsql provider family");
  Require(Contains(capabilities.payload, "\"SBLR_BRIDGE_OPEN_SESSION\""),
          "bridge capabilities missing SBLR bridge opcode");
  Require(Contains(capabilities.payload, "\"cluster_public_implementation\":false"),
          "bridge capabilities leaked public cluster implementation");
  Require(Contains(capabilities.payload, "\"raw_secret_material_allowed\":false"),
          "bridge capabilities did not require secret references");

  Require(ExpectRefusal(sbu_sbsql_bridge_capabilities("normal"),
                        "UDR.BRIDGE.CAPABILITY_POLICY_DENIED",
                        "bridge_capabilities policy"),
          "bridge capabilities policy did not fail closed");
  Require(ExpectRefusal(sbu_sbsql_bridge_dispatch("operation=open_session", ""),
                        "UDR.BRIDGE.CONTEXT_MISSING",
                        "bridge_dispatch missing context"),
          "bridge dispatch missing context did not fail closed");
  Require(ExpectRefusal(
              sbu_sbsql_bridge_dispatch("operation=open_session;password=cleartext",
                                        kTrustedBridgeContext),
              "UDR.BRIDGE.SECRET_MATERIAL_DENIED",
              "bridge_dispatch raw secret"),
          "bridge dispatch raw secret did not fail closed");

  const auto open_session =
      sbu_sbsql_bridge_dispatch("operation=open_session", kTrustedBridgeContext);
  Require(ExpectOk(open_session, "bridge open_session"),
          "bridge open_session failed");
  Require(Contains(open_session.payload, "\"provider_family\":\"sbsql\"") &&
              Contains(open_session.payload, "\"operation\":\"open_session\"") &&
              Contains(open_session.payload, "\"SBLR_BRIDGE_OPEN_SESSION\"") &&
              Contains(open_session.payload, "\"parser_authority\":false") &&
              Contains(open_session.payload, "\"udr_transaction_authority\":false") &&
              Contains(open_session.payload,
                       "\"mga_transaction_authority\":\"per_database_engine_mga\"") &&
              Contains(open_session.payload, "\"raw_secret_material_included\":false"),
          "bridge open_session payload did not preserve authority boundaries");

  const auto prepare =
      sbu_sbsql_bridge_dispatch("operation=prepare", kTrustedBridgeContext);
  Require(ExpectOk(prepare, "bridge prepare"), "bridge prepare failed");
  Require(Contains(prepare.payload, "\"local_transaction_uuid\"") &&
              Contains(prepare.payload, "\"remote_transaction_ref\":\"remote-txn-1\""),
          "bridge prepare missing local/remote transaction evidence");
  Require(ExpectRefusal(
              sbu_sbsql_bridge_dispatch(
                  "operation=commit",
                  "engine_context=trusted;bridge_authority=engine;"
                  "user_uuid=u;request_uuid=r;operation_policy_ref=p"),
              "UDR.BRIDGE.CONTEXT_MISSING",
              "bridge commit without local transaction"),
          "bridge commit without local transaction did not fail closed");

  const auto logical_restore = sbu_sbsql_bridge_dispatch(
      "operation=stream_open;stream_kind=logical_restore", kTrustedBridgeContext);
  Require(ExpectOk(logical_restore, "bridge logical restore stream"),
          "bridge logical restore stream failed");
  Require(Contains(logical_restore.payload, "\"stream_kind\":\"logical_restore\""),
          "bridge logical restore stream did not keep stream kind evidence");
  Require(ExpectRefusal(
              sbu_sbsql_bridge_dispatch(
                  "operation=stream_open;stream_kind=physical_page_copy",
                  kTrustedBridgeContext),
              "UDR.BRIDGE.SANDBOX_DENIED",
              "bridge physical page-copy stream"),
          "bridge physical page-copy stream did not fail closed");
  Require(ExpectRefusal(
              sbu_sbsql_bridge_dispatch("operation=stream_read",
                                        kTrustedBridgeContext),
              "UDR.BRIDGE.STREAM_INVALID",
              "bridge stream read without stream uuid"),
          "bridge stream read without stream uuid did not fail closed");

  Require(ExpectRefusal(
              sbu_sbsql_bridge_dispatch("operation=cdc_apply",
                                        kTrustedBridgeContext),
              "UDR.BRIDGE.IDEMPOTENCY_MISSING",
              "bridge cdc apply without idempotency"),
          "bridge cdc apply without idempotency did not fail closed");
  const auto cdc_apply = sbu_sbsql_bridge_dispatch(
      "operation=cdc_apply;idempotency_key=cdc-1", kTrustedBridgeContext);
  Require(ExpectOk(cdc_apply, "bridge cdc apply"),
          "bridge cdc apply failed");

  Require(ExpectRefusal(
              sbu_sbsql_bridge_dispatch("operation=cluster.route",
                                        kTrustedBridgeContext),
              "UDR.BRIDGE.UNSUPPORTED",
              "bridge cluster disabled"),
          "bridge cluster disabled did not fail closed");
  Require(ExpectRefusal(
              sbu_sbsql_bridge_dispatch(
                  "operation=cluster.route",
                  std::string(kTrustedBridgeContext) +
                      ";cluster_provider_gate=admitted"),
              "UDR.BRIDGE.UNLICENSED",
              "bridge cluster public stub"),
          "bridge cluster public stub did not return unlicensed");
}

} // namespace

int main() {
  using namespace scratchbird::udr::sbsql_parser_support;

  CheckDescriptorAndLifecycle();
  CheckRuntimeRegistrationAndEntrypoints();
  CheckUniversalBridgeContract();

  if (!ExpectOk(sbu_sbsql_validate_syntax("select 1", "sbsql"), "validate_syntax")) {
    return EXIT_FAILURE;
  }

  const auto normalized = sbu_sbsql_normalize("  select 1  ", "sbsql");
  if (!ExpectOk(normalized, "normalize")) return EXIT_FAILURE;
  if (normalized.payload != "select 1") {
    std::cerr << "normalize did not trim input: " << normalized.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectRefusal(sbu_sbsql_parse_to_sblr("select 1", ""),
                     "UDR.SBSQL.CONTEXT_MISSING",
                     "parse_to_sblr")) {
    return EXIT_FAILURE;
  }

  const auto sblr = sbu_sbsql_parse_to_sblr(
      "select 1",
      "engine_context=trusted;resolver=public;"
      "session_uuid=019e13c0-0000-7000-8000-00000000a008;"
      "connection_uuid=019e13c0-0000-7000-8000-00000000a108;"
      "database_uuid=019e13c0-0000-7000-8000-00000000a208;"
      "parser_uuid=019e13c0-0000-7000-8000-00000000a308;"
      "catalog_epoch=77;security_policy_epoch=78;descriptor_epoch=79;"
      "transaction_context=udr.test.engine_context");
  if (!ExpectOk(sblr, "parse_to_sblr_trusted")) return EXIT_FAILURE;
  if (!Contains(sblr.payload, "SBLRExecutionEnvelope.v3") ||
      Contains(sblr.payload, "select 1")) {
    std::cerr << "parse_to_sblr trusted payload violated SBLR/no-SQL expectations: "
              << sblr.payload << '\n';
    return EXIT_FAILURE;
  }
  if (Contains(sblr.payload, "00000000u") ||
      !Contains(sblr.payload, "\"catalog_epoch\":77") ||
      !Contains(sblr.payload, "\"security_policy_epoch\":78") ||
      !Contains(sblr.payload, "\"descriptor_epoch\":79")) {
    std::cerr << "parse_to_sblr trusted payload did not preserve engine-supplied "
                 "context epochs or contained invalid synthetic UUIDs: "
              << sblr.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectRefusal(sbu_sbsql_parse_expression("1 + 1", ""),
                     "UDR.SBSQL.CONTEXT_MISSING",
                     "parse_expression")) {
    return EXIT_FAILURE;
  }

  const auto expression = sbu_sbsql_parse_expression("1 + 1", "descriptor_context=engine");
  if (!ExpectOk(expression, "parse_expression_context")) return EXIT_FAILURE;
  if (!Contains(expression.payload, "expression_parser")) {
    std::cerr << "parse_expression context payload missing parser marker: "
              << expression.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectRefusal(sbu_sbsql_describe_statement("select 1", ""),
                     "UDR.SBSQL.CONTEXT_MISSING",
                     "describe_statement")) {
    return EXIT_FAILURE;
  }

  const auto described = sbu_sbsql_describe_statement("select 1", "engine_context=trusted");
  if (!ExpectOk(described, "describe_statement_context")) return EXIT_FAILURE;
  if (!Contains(described.payload, "\"statement_kind\":\"query\"")) {
    std::cerr << "describe_statement context payload mismatch: "
              << described.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectRefusal(sbu_sbsql_decompile_sblr("packet", "normal"),
                     "SBU_SBSQL.DECOMPILE_POLICY_REFUSED",
                     "decompile_sblr")) {
    return EXIT_FAILURE;
  }

  const auto decompiled = sbu_sbsql_decompile_sblr("packet", "allow_debug_artifacts");
  if (!ExpectOk(decompiled, "decompile_sblr_debug")) return EXIT_FAILURE;
  if (decompiled.payload != "<sblr-debug-text-redacted>") {
    std::cerr << "decompile_sblr_debug returned unexpected payload: "
              << decompiled.payload << '\n';
    return EXIT_FAILURE;
  }
  CheckSourcePreservingDecompile();

  if (!ExpectRefusal(sbu_sbsql_debug_capabilities("normal"),
                     "SBU_SBSQL.DEBUG_POLICY_REFUSED",
                     "debug_capabilities_refusal")) {
    return EXIT_FAILURE;
  }
  const auto capabilities = sbu_sbsql_debug_capabilities("allow_debug_artifacts");
  if (!ExpectOk(capabilities, "debug_capabilities")) return EXIT_FAILURE;
  if (!Contains(capabilities.payload, "\"parse_to_sblr\":true")) {
    std::cerr << "debug_capabilities payload mismatch: " << capabilities.payload << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
