// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "sblr_admission.hpp"
#include "sblr_opcode_registry.hpp"
#include "sbu_sbsql_parser_support.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;
namespace udr = scratchbird::udr::sbsql_parser_support;

constexpr std::string_view kTrustedBridgeContext =
    "engine_context=trusted;bridge_authority=engine;"
    "user_uuid=019e13c0-0000-7000-8000-00000000b001;"
    "request_uuid=019e13c0-0000-7000-8000-00000000b002;"
    "operation_policy_ref=019e13c0-0000-7000-8000-00000000b003;"
    "bridge_connection_uuid=019e13c0-0000-7000-8000-00000000b004;"
    "local_transaction_uuid=019e13c0-0000-7000-8000-00000000b005;"
    "remote_transaction_ref=remote-txn-1;remote_supports_prepare=true;"
    "cutover_evidence=validated";

struct BridgeRouteRow {
  std::string_view label;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view operation;
  std::string_view opcode;
  bool requires_transaction_context;
  bool cluster_route;
  std::string_view udr_request_suffix;
  std::string_view expected_refusal_code;
  std::string_view expected_udr_operation;
};

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasDiagnosticCode(const MessageVectorSet& messages, std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string DiagnosticCodes(const MessageVectorSet& messages) {
  std::string out;
  for (const auto& diagnostic : messages.diagnostics) {
    if (!out.empty()) out += ',';
    out += diagnostic.code;
  }
  return out;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019e14c0-0000-7000-8000-00000000c001";
  config.listener_uuid = "019e14c0-0000-7000-8000-00000000c002";
  config.bundle_contract_id = "sbp_sbsql@bridge-route-proof";
  config.build_id = "sbsql_bridge_command_route_conformance";
  return config;
}

SessionContext SessionForTest() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019e14c0-0000-7000-8000-00000000d001";
  session.connection_uuid = "019e14c0-0000-7000-8000-00000000d002";
  session.database_uuid = "019e14c0-0000-7000-8000-00000000d003";
  session.authenticated_user_uuid = "019e14c0-0000-7000-8000-00000000d004";
  session.dialect_profile_uuid = "sbsql.default";
  session.transaction_context = "mga.local.transaction.context";
  session.transaction_uuid = "019e14c0-0000-7000-8000-00000000d005";
  session.local_transaction_id = 42;
  session.snapshot_visible_through_local_transaction_id = 42;
  session.catalog_epoch = 101;
  session.security_policy_epoch = 102;
  session.descriptor_epoch = 103;
  return session;
}

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = SessionForTest();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireParserPipeline(const PipelineArtifacts& artifacts, const BridgeRouteRow& row) {
  Require(!HasDiagnosticCode(artifacts.cst.messages, "SBSQL.LEXER.ERROR"),
          std::string(row.label) + " produced lexer diagnostics: " +
              DiagnosticCodes(artifacts.cst.messages));
  Require(artifacts.ast.family == StatementFamily::kBridge,
          std::string(row.label) + " was not classified as a bridge statement");
  Require(artifacts.ast.operation_family == "sblr.bridge.operation.v3",
          std::string(row.label) + " did not carry the bridge AST operation family");
  Require(artifacts.bound.bound,
          std::string(row.label) + " did not bind");
  Require(artifacts.bound.requires_security_authority,
          std::string(row.label) + " did not require server security authority");
  Require(artifacts.envelope.operation_family == "sblr.bridge.operation.v3",
          std::string(row.label) + " lowered to the wrong SBLR family");
  Require(artifacts.envelope.sblr_operation_key == "sblr.bridge.operation.v3",
          std::string(row.label) + " lowered to the wrong SBLR operation key");
  Require(artifacts.envelope.operation_id == row.operation_id,
          std::string(row.label) + " lowered to operation id " +
              artifacts.envelope.operation_id);
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          std::string(row.label) + " lowered to opcode " +
              artifacts.envelope.sblr_opcode);
  Require(artifacts.envelope.engine_api_function == "TrustedParserSupportUdrBridgeDispatch",
          std::string(row.label) + " did not route through trusted bridge UDR dispatch");
  Require(artifacts.envelope.parser_executes_sql == false,
          std::string(row.label) + " allowed parser-side SQL execution");
  Require(artifacts.verifier.admitted,
          std::string(row.label) + " verifier refused bridge SBLR: " +
              DiagnosticCodes(artifacts.verifier.messages));
  Require(Contains(artifacts.envelope.payload, "\"bridge_command_route\":true"),
          std::string(row.label) + " payload is missing bridge route evidence");
  Require(Contains(artifacts.envelope.payload, "\"bridge_abi\":\"sb_universal_bridge_v1\""),
          std::string(row.label) + " payload is missing bridge ABI evidence");
  Require(Contains(artifacts.envelope.payload, "\"bridge_operation_id\":\"") &&
              Contains(artifacts.envelope.payload, row.operation_id),
          std::string(row.label) + " payload is missing bridge operation id evidence");
  Require(Contains(artifacts.envelope.payload, "\"bridge_request_packet\":\"operation=") &&
              Contains(artifacts.envelope.payload, row.operation),
          std::string(row.label) + " payload is missing bridge request packet evidence");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_bridge_operation\":false"),
          std::string(row.label) + " payload did not deny parser execution authority");
  Require(Contains(artifacts.envelope.payload, "\"udr_transaction_authority\":false"),
          std::string(row.label) + " payload did not deny UDR transaction finality");
  Require(Contains(artifacts.envelope.payload, "\"secret_refs_required\":true") &&
              Contains(artifacts.envelope.payload, "\"raw_secret_material_included\":false"),
          std::string(row.label) + " payload did not require secret references");
  Require(Contains(artifacts.envelope.payload,
                   row.requires_transaction_context
                       ? "\"mga_transaction_context_required\":true"
                       : "\"mga_transaction_context_required\":false"),
          std::string(row.label) + " payload has the wrong MGA transaction-context flag");
  Require(Contains(artifacts.envelope.payload,
                   row.cluster_route ? "\"bridge_cluster_route\":true"
                                     : "\"bridge_cluster_route\":false"),
          std::string(row.label) + " payload has the wrong cluster route flag");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.bridge_context_required"),
          std::string(row.label) + " missing bridge context authority step");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.trusted_udr_bridge_dispatch_required"),
          std::string(row.label) + " missing trusted UDR dispatch authority step");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          std::string(row.label) + " missing parser no-SQL authority step");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.udr.no_transaction_finality"),
          std::string(row.label) + " missing UDR no-finality authority step");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.bridge.connection") &&
              HasValue(artifacts.envelope.descriptor_refs, "sys.bridge.policy") &&
              HasValue(artifacts.envelope.descriptor_refs, "sys.udr_package_registry"),
          std::string(row.label) + " missing bridge descriptor evidence");
  if (row.requires_transaction_context) {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.engine.mga_transaction_context_required") &&
                HasValue(artifacts.envelope.descriptor_refs,
                         "sys.mga.transaction_inventory"),
            std::string(row.label) + " missing local MGA transaction evidence");
  }
  if (row.cluster_route) {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.cluster.provider_compile_time_gate_required") &&
                HasValue(artifacts.envelope.required_rights, "right.cluster_control") &&
                HasValue(artifacts.envelope.descriptor_refs, "sys.cluster.provider"),
            std::string(row.label) + " missing cluster compile-gate evidence");
  } else {
    Require(HasValue(artifacts.envelope.required_rights, "right.bridge.use") &&
                HasValue(artifacts.envelope.required_authority_steps,
                         "authority.cluster.provider_dispatch_not_required"),
            std::string(row.label) + " missing non-cluster bridge authority evidence");
  }
}

void RequireServerAdmission(const PipelineArtifacts& artifacts, const BridgeRouteRow& row) {
  const auto admission = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted,
          std::string(row.label) + " was refused by server admission");
  Require(admission.requires_public_abi_dispatch,
          std::string(row.label) + " did not require public ABI dispatch");
  Require(admission.operation_family == "sblr.bridge.operation.v3",
          std::string(row.label) + " server admission resolved wrong family " +
              admission.operation_family);
  Require(admission.operation_id == row.operation_id,
          std::string(row.label) + " server admission resolved wrong operation " +
              admission.operation_id);
}

void RequireRegistryEntry(const BridgeRouteRow& row) {
  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr,
          std::string(row.label) + " has no canonical SBLR registry entry");
  Require(entry->opcode == row.opcode,
          std::string(row.label) + " registry opcode mismatch");
  Require(entry->requires_transaction_context == row.requires_transaction_context,
          std::string(row.label) + " registry transaction-context flag mismatch");
  if (row.cluster_route) {
    Require(entry->category == sblr::SblrOpcodeCategory::cluster,
            std::string(row.label) + " registry did not mark cluster bridge route");
    Require(entry->support == sblr::SblrOpcodeSupport::cluster_refusal,
            std::string(row.label) + " registry did not mark cluster refusal");
    Require(entry->requires_cluster_authority,
            std::string(row.label) + " registry did not require cluster authority");
  } else {
    Require(entry->category == sblr::SblrOpcodeCategory::extensibility,
            std::string(row.label) + " registry did not mark bridge extensibility route");
    Require(entry->support == sblr::SblrOpcodeSupport::implemented,
            std::string(row.label) + " registry did not mark bridge route implemented");
    Require(!entry->requires_cluster_authority,
            std::string(row.label) + " registry incorrectly required cluster authority");
  }
}

void RequireUdrRoute(const BridgeRouteRow& row) {
  std::string request = "operation=" + std::string(row.operation);
  request += std::string(row.udr_request_suffix);
  const auto result = udr::sbu_sbsql_bridge_dispatch(request, kTrustedBridgeContext);
  if (!row.expected_refusal_code.empty()) {
    Require(!result.ok && Contains(result.message_vector_json, row.expected_refusal_code),
            std::string(row.label) + " did not fail closed with " +
                std::string(row.expected_refusal_code) + ": " +
                result.message_vector_json);
    if (row.cluster_route) {
      const auto unlicensed = udr::sbu_sbsql_bridge_dispatch(
          request, std::string(kTrustedBridgeContext) + ";cluster_provider_gate=admitted");
      Require(!unlicensed.ok &&
                  Contains(unlicensed.message_vector_json, "UDR.BRIDGE.UNLICENSED"),
              std::string(row.label) + " did not route admitted cluster calls to the stub "
                                      "unlicensed vector: " +
                  unlicensed.message_vector_json);
    }
    return;
  }
  Require(result.ok,
          std::string(row.label) + " was refused by the trusted SBsql bridge UDR: " +
              result.message_vector_json);
  const std::string expected_operation =
      row.expected_udr_operation.empty()
          ? std::string(row.operation)
          : std::string(row.expected_udr_operation);
  Require(Contains(result.payload, "\"bridge_abi\":\"sb_universal_bridge_v1\"") &&
              Contains(result.payload, "\"provider_family\":\"sbsql\"") &&
              Contains(result.payload, "\"operation\":\"" + expected_operation + "\"") &&
              Contains(result.payload, "\"parser_authority\":false") &&
              Contains(result.payload, "\"udr_transaction_authority\":false") &&
              Contains(result.payload,
                       "\"mga_transaction_authority\":\"per_database_engine_mga\"") &&
              Contains(result.payload, "\"raw_secret_material_included\":false"),
          std::string(row.label) + " returned incomplete bridge UDR payload: " +
              result.payload);
}

void RequireRoute(const BridgeRouteRow& row) {
  const auto artifacts = RunPipeline(row.sql);
  RequireParserPipeline(artifacts, row);
  RequireServerAdmission(artifacts, row);
  RequireRegistryEntry(row);
  RequireUdrRoute(row);
}

}  // namespace

int main() {
  const auto capabilities = udr::sbu_sbsql_bridge_capabilities("release_evidence");
  Require(capabilities.ok, "bridge capabilities refused release evidence");
  Require(Contains(capabilities.payload, "\"SBLR_BRIDGE_CUTOVER\"") &&
              Contains(capabilities.payload, "\"sb_universal_bridge_v1\""),
          "bridge capabilities did not expose the universal bridge SBLR surface");

  constexpr std::array<BridgeRouteRow, 34> rows{{
      {"show_capabilities", "SHOW BRIDGE CAPABILITIES", "bridge.describe_capabilities", "describe_capabilities", "SBLR_BRIDGE_DESCRIBE_CAPABILITIES", false, false, "", "", ""},
      {"create_bridge", "CREATE BRIDGE CONNECTION fb_remote", "bridge.connect", "connect", "SBLR_BRIDGE_OPEN_CHANNEL", false, false, "", "", ""},
      {"attach_bridge", "ATTACH BRIDGE fb_remote", "bridge.attach", "attach", "SBLR_BRIDGE_OPEN_CHANNEL", false, false, "", "", ""},
      {"authenticate_bridge", "BRIDGE AUTHENTICATE fb_remote", "bridge.authenticate", "authenticate", "SBLR_BRIDGE_AUTHENTICATE", false, false, "", "", ""},
      {"open_session", "OPEN BRIDGE SESSION fb_remote", "bridge.open_session", "open_session", "SBLR_BRIDGE_OPEN_SESSION", false, false, "", "", ""},
      {"close_session", "CLOSE BRIDGE SESSION fb_remote", "bridge.close_session", "close_session", "SBLR_BRIDGE_CLOSE_SESSION", false, false, "", "", ""},
      {"detach_bridge", "DETACH BRIDGE fb_remote", "bridge.detach", "detach", "SBLR_BRIDGE_CLOSE_SESSION", false, false, "", "", ""},
      {"ping_bridge", "BRIDGE PING fb_remote", "bridge.ping", "ping", "SBLR_BRIDGE_HEALTH", false, false, "", "", ""},
      {"health_bridge", "BRIDGE HEALTH fb_remote", "bridge.health", "health", "SBLR_BRIDGE_HEALTH", false, false, "", "", ""},
      {"cancel_bridge", "BRIDGE CANCEL fb_remote", "bridge.cancel", "cancel", "SBLR_BRIDGE_CANCEL", false, false, "", "", ""},
      {"drain_bridge", "BRIDGE DRAIN fb_remote", "bridge.drain", "drain", "SBLR_BRIDGE_DRAIN", false, false, "", "", ""},
      {"shutdown_bridge", "BRIDGE SHUTDOWN fb_remote", "bridge.shutdown", "shutdown", "SBLR_BRIDGE_DRAIN", false, false, "", "", ""},
      {"begin_bridge_tx", "BRIDGE BEGIN fb_remote", "bridge.begin", "begin", "SBLR_BRIDGE_TX_BEGIN", true, false, "", "", ""},
      {"commit_bridge_tx", "BRIDGE COMMIT fb_remote", "bridge.commit", "commit", "SBLR_BRIDGE_TX_COMMIT", true, false, "", "", ""},
      {"rollback_bridge_tx", "BRIDGE ROLLBACK fb_remote", "bridge.rollback", "rollback", "SBLR_BRIDGE_TX_ROLLBACK", true, false, "", "", ""},
      {"prepare_bridge_tx", "BRIDGE PREPARE fb_remote", "bridge.prepare", "prepare", "SBLR_BRIDGE_TX_PREPARE", true, false, "", "", ""},
      {"savepoint_bridge_tx", "BRIDGE SAVEPOINT fb_remote sp1", "bridge.savepoint", "savepoint", "SBLR_BRIDGE_TX_SAVEPOINT", true, false, "", "", ""},
      {"execute_bridge", "BRIDGE EXECUTE fb_remote", "bridge.execute", "execute", "SBLR_BRIDGE_EXECUTE", true, false, "", "", ""},
      {"cursor_open", "BRIDGE CURSOR OPEN fb_remote", "bridge.cursor_open", "cursor_open", "SBLR_BRIDGE_CURSOR_OPEN", true, false, "", "", ""},
      {"cursor_fetch", "BRIDGE CURSOR FETCH fb_remote", "bridge.cursor_fetch", "cursor_fetch", "SBLR_BRIDGE_CURSOR_FETCH", true, false, "", "", ""},
      {"cursor_close", "BRIDGE CURSOR CLOSE fb_remote", "bridge.cursor_close", "cursor_close", "SBLR_BRIDGE_CURSOR_CLOSE", true, false, "", "", ""},
      {"logical_restore_stream", "BRIDGE STREAM OPEN LOGICAL RESTORE fb_remote", "bridge.stream_open", "stream_open", "SBLR_BRIDGE_STREAM_OPEN", true, false, ";stream_kind=logical_restore", "", ""},
      {"stream_read", "BRIDGE STREAM READ fb_remote", "bridge.stream_read", "stream_read", "SBLR_BRIDGE_STREAM_READ", true, false, ";stream_uuid=019e14c0-0000-7000-8000-00000000e001", "", ""},
      {"stream_write", "BRIDGE STREAM WRITE fb_remote", "bridge.stream_write", "stream_write", "SBLR_BRIDGE_STREAM_WRITE", true, false, ";stream_uuid=019e14c0-0000-7000-8000-00000000e001", "", ""},
      {"stream_close", "BRIDGE STREAM CLOSE fb_remote", "bridge.stream_close", "stream_close", "SBLR_BRIDGE_STREAM_CLOSE", true, false, ";stream_uuid=019e14c0-0000-7000-8000-00000000e001", "", ""},
      {"cdc_start", "BRIDGE CDC START fb_remote", "bridge.cdc_start", "cdc_start", "SBLR_BRIDGE_CDC_START", true, false, "", "", ""},
      {"cdc_read", "BRIDGE CDC READ fb_remote", "bridge.cdc_read", "cdc_read", "SBLR_BRIDGE_CDC_READ", true, false, ";idempotency_key=cdc-read-1", "", ""},
      {"cdc_apply", "BRIDGE CDC APPLY fb_remote", "bridge.cdc_apply", "cdc_apply", "SBLR_BRIDGE_CDC_APPLY", true, false, ";idempotency_key=cdc-apply-1", "", ""},
      {"proxy_route", "BRIDGE PROXY ROUTE fb_remote", "bridge.proxy_route", "proxy_route", "SBLR_BRIDGE_PROXY_ROUTE", true, false, "", "", ""},
      {"compare_result", "BRIDGE COMPARE RESULT fb_remote", "bridge.compare_result", "compare_result", "SBLR_BRIDGE_COMPARE_RESULT", true, false, "", "", ""},
      {"cutover", "BRIDGE CUTOVER fb_remote", "bridge.cutover", "cutover", "SBLR_BRIDGE_CUTOVER", true, false, "", "", ""},
      {"validate_bridge", "VALIDATE BRIDGE CONNECTION fb_remote", "bridge.validate", "validate", "SBLR_BRIDGE_VALIDATE", false, false, "", "", "describe_capabilities"},
      {"physical_page_copy_denied", "BRIDGE STREAM OPEN PHYSICAL PAGE COPY fb_remote", "bridge.stream_open", "stream_open", "SBLR_BRIDGE_STREAM_OPEN", true, false, ";stream_kind=physical_page_copy", "UDR.BRIDGE.SANDBOX_DENIED", ""},
      {"cluster_route_stub", "BRIDGE CLUSTER ROUTE fb_remote", "bridge.cluster_route", "cluster.route", "SBLR_BRIDGE_VALIDATE", true, true, "", "UDR.BRIDGE.UNSUPPORTED", ""},
  }};

  for (const auto& row : rows) {
    RequireRoute(row);
  }

  std::cout << "sbsql_bridge_command_route_conformance=passed rows="
            << rows.size() << '\n';
  return EXIT_SUCCESS;
}
