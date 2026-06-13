// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/sblr/lowering.hpp"

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sbsql = scratchbird::parser::sbsql;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;
namespace sbps = scratchbird::server::sbps;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasDiagnosticCode(const std::vector<scratchbird::server::ServerDiagnostic>& diagnostics,
                       std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDiagnostic(const SessionOperationResult& result, std::string_view code) {
  return HasDiagnosticCode(result.diagnostics, code);
}

std::string PayloadText(const SessionOperationResult& result) {
  return std::string(result.payload.begin(), result.payload.end());
}

bool PayloadContains(const SessionOperationResult& result, std::string_view needle) {
  const std::string text = PayloadText(result);
  return text.find(needle) != std::string::npos;
}

bool ClusterProviderStubBuild() {
  const auto info = scratchbird::engine::cluster_provider::DescribeClusterProvider();
  return info.compile_link_only && info.provider_type == "compile_link_stub";
}

void RequireClusterProviderOutcome(const SessionOperationResult& result,
                                   const std::string& label,
                                   std::string_view expected_operation_id) {
  if (ClusterProviderStubBuild()) {
    Require(!result.accepted &&
                HasDiagnostic(result,
                              scratchbird::engine::cluster_provider::
                                  kClusterHandshakeStubCompileLinkOnlyCode),
            label + " did not return the cluster provider stub compile-link vector");
    Require(PayloadContains(result, "cluster_provider=compile_link_stub"),
            label + " did not include the cluster stub provider detail");
    return;
  }

  Require(!result.accepted &&
              HasDiagnostic(result,
                            scratchbird::engine::cluster_provider::kClusterSupportNotEnabledCode),
          label + " did not return the no-cluster provider error vector");
  Require(!PayloadContains(result, "product=ScratchBird"),
          label + " returned an unrelated public engine payload");
}

void RequireClusterProviderInfoOutcome(const SessionOperationResult& result,
                                       const std::string& label) {
  if (!result.accepted) {
    std::cerr << label << " payload=" << PayloadText(result) << '\n';
  }
  Require(result.accepted, label + " did not return the cluster provider info row");
  Require(PayloadContains(result, "operation_id=cluster.inspect_provider"),
          label + " did not preserve the provider info operation id");
  Require(PayloadContains(result, "result_kind=cluster.provider.info.v1"),
          label + " did not return the provider info result kind");
  Require(PayloadContains(result, "provider_name="),
          label + " omitted provider name");
  Require(PayloadContains(result, "provider_type="),
          label + " omitted provider type");
  Require(PayloadContains(result, "provider_version="),
          label + " omitted provider version");
  Require(PayloadContains(result, "support_status="),
          label + " omitted provider support status");
  Require(PayloadContains(result, "supports_execution="),
          label + " omitted provider execution support flag");
}

void RequireClusterRequestLifecycle(const ServerSessionRegistry* registry,
                                    const std::array<std::uint8_t, 16>& request_uuid,
                                    const std::string& label,
                                    std::string_view expected_operation_id) {
  const auto request_it = registry->requests_by_uuid.find(
      scratchbird::server::UuidBytesToText(request_uuid));
  Require(request_it != registry->requests_by_uuid.end(),
          label + " did not record request lifecycle evidence");
  if (ClusterProviderStubBuild()) {
    Require(request_it->second.state == scratchbird::server::ServerRequestLifecycleState::kFailed,
            label + " request lifecycle did not fail closed through the cluster stub provider");
    Require(request_it->second.detail == "cluster_provider=compile_link_stub;cluster_support=compile_link_only",
            label + " request lifecycle carried the wrong stub compile-link detail");
  } else {
    Require(request_it->second.state == scratchbird::server::ServerRequestLifecycleState::kFailed,
            label + " request lifecycle did not fail through the no-cluster provider");
    Require(request_it->second.detail == "cluster_provider=no_cluster;cluster_support=not_enabled",
            label + " request lifecycle carried the wrong no-cluster detail");
  }
  Require(request_it->second.operation_id == expected_operation_id,
          label + " request lifecycle did not preserve the cluster operation id");
  Require(!request_it->second.engine_result_retained,
          label + " retained an engine result for a non-cursor cluster request");
  Require(request_it->second.prepared_statement_uuid == std::array<std::uint8_t, 16>{},
          label + " linked a prepared statement for a direct cluster request");
  Require(request_it->second.cursor_uuid == std::array<std::uint8_t, 16>{},
          label + " linked a cursor for a direct cluster request");
  Require(request_it->second.transaction_finality_preserved,
          label + " changed request finality preservation evidence");
}

void RequireFamilyReconciliationFailure(const ServerSessionRegistry* registry,
                                        const sbps::Frame& frame,
                                        const SessionOperationResult& result,
                                        const std::string& label) {
  Require(!result.accepted && HasDiagnostic(result, "SBLR.FAMILY_RECONCILIATION_REQUIRED"),
          label + " did not fail closed at SBLR family reconciliation");
  const auto request_it = registry->requests_by_uuid.find(
      scratchbird::server::UuidBytesToText(frame.header.request_uuid));
  Require(request_it != registry->requests_by_uuid.end(),
          label + " did not record admission-refusal lifecycle evidence");
  Require(request_it->second.state == scratchbird::server::ServerRequestLifecycleState::kFailed,
          label + " request lifecycle did not fail closed");
  Require(request_it->second.detail == "SBLR.FAMILY_RECONCILIATION_REQUIRED",
          label + " request lifecycle did not preserve the reconciliation diagnostic");
  Require(request_it->second.operation_id == "sblr.dispatch.pending",
          label + " updated operation authority before SBLR family reconciliation");
  Require(request_it->second.prepared_statement_uuid == std::array<std::uint8_t, 16>{},
          label + " linked a prepared statement during admission refusal");
  Require(request_it->second.cursor_uuid == std::array<std::uint8_t, 16>{},
          label + " linked a cursor during admission refusal");
  Require(!request_it->second.engine_result_retained,
          label + " retained an engine result during admission refusal");
  Require(request_it->second.transaction_finality_preserved,
          label + " did not preserve transaction-finality evidence");
}

bool DiagnosticContains(const SessionOperationResult& result, std::string_view needle) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(needle) != std::string::npos ||
        diagnostic.message_key.find(needle) != std::string::npos ||
        diagnostic.safe_message.find(needle) != std::string::npos ||
        diagnostic.diagnostic_shape_id.find(needle) != std::string::npos ||
        diagnostic.correlation_uuid.find(needle) != std::string::npos ||
        diagnostic.request_uuid.find(needle) != std::string::npos ||
        diagnostic.session_uuid.find(needle) != std::string::npos ||
        diagnostic.database_uuid.find(needle) != std::string::npos) {
      return true;
    }
    for (const auto& field : diagnostic.fields) {
      if (field.key.find(needle) != std::string::npos ||
          field.value.find(needle) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

std::string ParserJsonEnvelope(std::string_view family,
                               bool source_payload_embedded = false,
                               bool omit_result_shape = false,
                               std::string_view operation_id = {}) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"";
  out += family;
  out += "\",\"surface_key\":\"fspe010.fixture\",";
  if (!operation_id.empty()) {
    out += "\"operation_id\":\"";
    out += operation_id;
    out += "\",";
  }
  out += "\"sblr_operation_key\":\"op.fspe010.fixture\",";
  if (!omit_result_shape) out += "\"result_shape\":\"rs.fspe010.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010.v1\",";
  out += "\"resource_contract\":\"resource.fspe010.v1\",";
  out += "\"trace_key\":\"FSPE-010\",";
  out += "\"source_payload_embedded\":";
  out += source_payload_embedded ? "true" : "false";
  out += ",\"resolved_object_uuids\":[],\"descriptor_refs\":[],\"policy_refs\":[]}";
  return out;
}

std::string ClusterPrivateParserJsonEnvelope(
    const sbsql::GeneratedSurfaceRegistryRow& row) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"";
  out += row.sblr_operation_family;
  out += "\",\"surface_key\":\"";
  out += row.surface_id;
  out += "\",\"sbsql_surface_id\":\"";
  out += row.surface_id;
  out += "\",\"sbsql_canonical_name\":\"";
  out += row.canonical_name;
  out += "\",\"sblr_operation_key\":\"";
  out += row.surface_id;
  out += ".direct_cluster_private_refusal\",";
  out += "\"result_shape\":\"rs.sbsfc025r_a.public_refusal.v1\",";
  out += "\"diagnostic_shape\":\"diag.sbsfc025r_a.public_refusal.v1\",";
  out += "\"resource_contract\":\"resource.sbsfc025r_a.public_refusal.v1\",";
  out += "\"trace_key\":\"SBSFC-025R-A\",";
  out += "\"contains_sql_text\":false,";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[],\"descriptor_refs\":[],\"policy_refs\":[]}";
  return out;
}

std::string TextOperationEnvelope(bool contains_sql_text = false,
                                  bool names_resolved = true,
                                  bool cluster = false) {
  std::string out;
  out += "operation_id=";
  out += cluster ? "cluster.inspect_state" : "observability.show_version";
  out += "\n";
  out += "opcode=";
  out += cluster ? "SBLR_CLUSTER_INSPECT_STATE" : "SBLR_OBSERVABILITY_SHOW_VERSION";
  out += "\n";
  out += "sblr_operation_family=";
  out += cluster ? "sblr.cluster.report.v3" : "sblr.management.report.v3";
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=FSPE-010\n";
  out += "contains_sql_text=";
  out += contains_sql_text ? "true" : "false";
  out += "\n";
  out += "parser_resolved_names_to_uuids=";
  out += names_resolved ? "true" : "false";
  out += "\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=false\n";
  out += "requires_cluster_authority=";
  out += cluster ? "true" : "false";
  out += "\n";
  return out;
}

std::string ClusterProviderInfoTextEnvelope() {
  std::string out;
  out += "operation_id=cluster.inspect_provider\n";
  out += "opcode=SBLR_CLUSTER_INSPECT_PROVIDER\n";
  out += "sblr_operation_family=sblr.cluster.report.v3\n";
  out += "result_shape=cluster.provider.info.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=CLUSTER-PROVIDER-INFO\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

std::string BinaryOperationEnvelopeFromText(const std::string& text) {
  const auto binary = scratchbird::engine::sblr::EnvelopeBuilder()
                          .operation(scratchbird::engine::SblrOperationFamily::management_inspect, 1)
                          .payload_kind(scratchbird::engine::SblrPayloadKind::operation_envelope)
                          .append_bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                        text.size())
                          .encode();
  return std::string(reinterpret_cast<const char*>(binary.data()), binary.size());
}

sbsql::SessionContext ParserSessionForClusterProviderCommand() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000cf0101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000cf0102";
  session.database_uuid = "019f0000-0000-7000-8000-000000cf0103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 47;
  session.security_policy_epoch = 48;
  session.descriptor_epoch = 49;
  return session;
}

sbsql::ParserConfig ParserConfigForClusterProviderCommand() {
  sbsql::ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_cluster_provider_command";
  config.parser_uuid = "019f0000-0000-7000-8000-000000cf0104";
  config.bundle_contract_id = "sbp_sbsql@cluster-provider-command-test";
  config.build_id = "sbsql-cluster-provider-command-test";
  return config;
}

sbsql::SblrEnvelope LowerClusterProviderCommand() {
  const auto session = ParserSessionForClusterProviderCommand();
  const auto cst = sbsql::BuildCst("SHOW CLUSTER PROVIDER;");
  const auto ast = sbsql::BuildAst(cst);
  const auto bound = sbsql::BindAst(ast, cst, ParserConfigForClusterProviderCommand(), session, {});
  Require(bound.bound, "SHOW CLUSTER PROVIDER did not bind");
  const auto envelope = sbsql::LowerToSblr(bound, cst, session);
  const auto verifier = sbsql::VerifySblrEnvelope(envelope);
  Require(verifier.admitted, "SHOW CLUSTER PROVIDER did not produce an admitted SBLR envelope");
  Require(envelope.operation_family == "sblr.cluster.report.v3" ||
              envelope.operation_family == "sblr.cluster.private_operation.v3",
          "SHOW CLUSTER PROVIDER did not lower to a recognized cluster provider family");
  Require(envelope.operation_id == "cluster.inspect_provider",
          "SHOW CLUSTER PROVIDER did not lower to cluster.inspect_provider");
  Require(envelope.sblr_opcode == "SBLR_CLUSTER_INSPECT_PROVIDER",
          "SHOW CLUSTER PROVIDER did not lower to the provider inspect opcode");
  Require(envelope.result_shape_key == "cluster.provider.info.v1",
          "SHOW CLUSTER PROVIDER did not declare the provider info result shape");
  Require(envelope.payload.find("\"cluster_provider_dispatch\":true") != std::string::npos,
          "SHOW CLUSTER PROVIDER payload did not mark provider dispatch");
  Require(envelope.payload.find("\"source_payload_embedded\":false") != std::string::npos,
          "SHOW CLUSTER PROVIDER payload embedded SQL text");
  return envelope;
}

std::string ClusterProfileGateTextEnvelope(
    const sbsql::GeneratedSurfaceRegistryRow& row) {
  std::string out;
  out += "operation_id=cluster.profile_gate.public_refusal\n";
  out += "opcode=SBSQL_CLUSTER_PROFILE_GATE_PUBLIC_REFUSAL\n";
  out += "sblr_operation_family=";
  out += row.sblr_operation_family;
  out += "\n";
  out += "result_shape=rs.sbsfc025r_c.public_refusal.v1\n";
  out += "diagnostic_shape=diag.sbsfc025r_c.public_refusal.v1\n";
  out += "trace_key=SBSFC-025R-C\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=false\n";
  out += "requires_cluster_authority=true\n";
  return out;
}

std::string ClusterScopeResidualTextEnvelope(
    const sbsql::GeneratedSurfaceRegistryRow& row) {
  std::string out;
  out += "operation_id=cluster.scope_residual.public_refusal\n";
  out += "opcode=SBSQL_CLUSTER_SCOPE_PUBLIC_REFUSAL\n";
  out += "sblr_operation_family=";
  out += row.sblr_operation_family;
  out += "\n";
  out += "result_shape=rs.sbsfc025r_z.public_refusal.v1\n";
  out += "diagnostic_shape=diag.sbsfc025r_z.public_refusal.v1\n";
  out += "trace_key=SBSFC-025R-Z\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=false\n";
  out += "requires_cluster_authority=true\n";
  return out;
}

std::string BinaryOperationEnvelope(bool contains_sql_text = false) {
  return BinaryOperationEnvelopeFromText(TextOperationEnvelope(contains_sql_text));
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::array<std::uint8_t, 16>& prepared_uuid,
                         const std::string& encoded,
                         bool cursor_requested = false) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, prepared_uuid, encoded, cursor_requested);
  return frame;
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodePrepareSblrPayloadForTest(session_uuid, encoded);
  return frame;
}

sbps::Frame CursorFrame(sbps::MessageType type,
                        const std::array<std::uint8_t, 16>& session_uuid,
                        const std::array<std::uint8_t, 16>& cursor_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  if (type == sbps::MessageType::kFetch) {
    frame.payload = scratchbird::server::EncodeFetchPayloadForTest(session_uuid, cursor_uuid);
  } else {
    frame.payload = scratchbird::server::EncodeCloseCursorPayloadForTest(session_uuid, cursor_uuid);
  }
  return frame;
}

ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_server_sbsql_admission_conformance.sbdb";
  session.database_uuid = "019e05bf-f010-7000-8000-000000000001";
  *session_uuid = session.session_uuid;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

HostedEngineState MakeEngineState() {
  HostedEngineState state;
  state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_server_sbsql_admission_conformance.sbdb";
  database.database_uuid = "019e05bf-f010-7000-8000-000000000001";
  state.databases.push_back(database);
  return state;
}

struct ClusterPrivateExpectedRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view source_status;
};

struct ClusterProfileGateExpectedRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view sblr_operation_family;
  std::string_view blocked_execution_path;
};

constexpr std::array<ClusterPrivateExpectedRow, 19> kSbsfc025rAClusterPrivateRows{{
    {"SBSQL-00FA147ED5B6", "member_ref_list", "cluster_private"},
    {"SBSQL-1AB70DEEAD48", "cluster_setting_stmt", "native_now"},
    {"SBSQL-3DBCE185F851", "cluster_ref", "cluster_private"},
    {"SBSQL-627269ECF1F9", "cluster_prepare_options", "cluster_private"},
    {"SBSQL-6657E884971F", "cluster_reconcile_stmt", "cluster_private"},
    {"SBSQL-7129A967AB22", "cluster_audit_stmt", "native_now"},
    {"SBSQL-71A081B78B8A", "cluster_stmt", "native_now"},
    {"SBSQL-7D8D2A369443", "cluster_tx_stmt", "cluster_private"},
    {"SBSQL-88E2119F5FFF", "cluster_topology_stmt", "native_now"},
    {"SBSQL-A32AF92DF5F2", "cluster_lifecycle_ddl", "cluster_private"},
    {"SBSQL-AE72C38E901A", "cluster_node_op_stmt", "cluster_private"},
    {"SBSQL-B9CAFA029514", "cluster_throttle_stmt", "native_now"},
    {"SBSQL-CD249C7FC6A4", "cluster_job_control_stmt", "native_now"},
    {"SBSQL-E04EF6DF0271", "cluster_system_op_stmt", "native_now"},
    {"SBSQL-E2D1CE36612D", "cluster_member_op_stmt", "native_now"},
    {"SBSQL-E338883A13CD", "cluster_failover_stmt", "native_now"},
    {"SBSQL-EB755314527A", "cluster_control_stmt", "cluster_private"},
    {"SBSQL-F1E1F8A83B4F", "route_ref", "cluster_private"},
    {"SBSQL-FC6C131013BF", "member_ref", "cluster_private"},
}};

constexpr std::array<ClusterProfileGateExpectedRow, 27> kSbsfc025rClusterProfileGateRows{{
    {"SBSQL-6689D8CFD6EA",
     "create_cluster_stmt",
     "sblr.catalog.mutation.v3",
     "cluster DDL catalog mutation or private cluster creation execution"},
    {"SBSQL-5FEA0732FD1C",
     "alter_cluster_stmt",
     "sblr.catalog.mutation.v3",
     "cluster DDL catalog mutation or private cluster alteration execution"},
    {"SBSQL-E24BF89F2917",
     "alter_cluster_action",
     "sblr.catalog.mutation.v3",
     "alter-cluster action catalog mutation or private cluster action execution"},
    {"SBSQL-4EF886377AB2",
     "drop_cluster_stmt",
     "sblr.catalog.mutation.v3",
     "cluster DDL catalog mutation or private cluster drop execution"},
    {"SBSQL-39C545BEBF5A",
     "cluster_publish_options",
     "sblr.archive_replication.operation.v3",
     "archive/replication execution"},
    {"SBSQL-3AA85DA2ED21",
     "cluster_commit_options",
     "sblr.transaction.control.v3",
     "cluster commit or transaction finality execution"},
    {"SBSQL-3D3DCFA99D24",
     "cluster_rollback_options",
     "sblr.transaction.control.v3",
     "cluster rollback, transaction finality, or rollback semantics execution"},
    {"SBSQL-01D7277D7AD1",
     "evidence_item",
     "sblr.general.operation.v3",
     "evidence-item publication or private cluster/profile execution"},
    {"SBSQL-91A719925D7D",
     "evidence_list",
     "sblr.general.operation.v3",
     "evidence-list publication or private cluster/profile execution"},
    {"SBSQL-1586EBD60331",
     "reason_code",
     "sblr.general.operation.v3",
     "reason-code publication or private cluster/profile execution"},
    {"SBSQL-3AD6213D718C",
     "config_key",
     "sblr.general.operation.v3",
     "config-key publication or private cluster/profile execution"},
    {"SBSQL-8E6FE3EB1CDB",
     "conflict_strategy",
     "sblr.general.operation.v3",
     "conflict-strategy publication or private cluster/profile execution"},
    {"SBSQL-FEA1D03D90A7",
     "quorum_rule",
     "sblr.general.operation.v3",
     "quorum-rule publication or private cluster/profile execution"},
    {"SBSQL-31D7FA95A32E",
     "idempotency_key",
     "sblr.general.operation.v3",
     "idempotency-key publication or private cluster/profile execution"},
    {"SBSQL-1F5D197F682F",
     "partition_ref",
     "sblr.general.operation.v3",
     "partition-ref publication or private cluster/profile execution"},
    {"SBSQL-2D5711FE6D48",
     "branch_ref",
     "sblr.general.operation.v3",
     "branch-ref publication or private cluster/profile execution"},
    {"SBSQL-BE088AF09680",
     "decision_ref",
     "sblr.general.operation.v3",
     "decision-ref publication or private cluster/profile execution"},
    {"SBSQL-3AE3460649B5",
     "decision_service_profile",
     "sblr.general.operation.v3",
     "decision-service-profile publication or private profile execution"},
    {"SBSQL-AD04656773E5",
     "object_ref",
     "sblr.general.operation.v3",
     "object-ref resolution, publication, or private profile execution"},
    {"SBSQL-3547284AA7F8",
     "show_cluster_extended",
     "sblr.observability.inspect.v3",
     "show-cluster-extended cluster observability or private profile execution"},
    {"SBSQL-70D0DE52A93E",
     "cluster_show_target",
     "sblr.observability.inspect.v3",
     "cluster-show-target observability or private profile execution"},
    {"SBSQL-BC81B2A0B934",
     "show_decisions_filter",
     "sblr.observability.inspect.v3",
     "show-decisions-filter decision observability or private profile execution"},
    {"SBSQL-A25FD5F1EFD2",
     "reconcile_options",
     "sblr.general.operation.v3",
     "reconcile-options cluster reconciliation or private profile execution"},
    {"SBSQL-D873CD6AEFCF",
     "member_role",
     "sblr.security.mutation.v3",
     "member-role security cluster profile execution"},
    {"SBSQL-F704A29FE1C8",
     "policy_ref",
     "sblr.policy.operation.v3",
     "policy-ref security cluster profile execution"},
    {"SBSQL-E47D4A865961",
     "cluster_node_uuid",
     "sblr.expression.runtime.v3",
     "cluster-node UUID execution or private cluster authority resolution"},
    {"SBSQL-3F704EACCE08",
     "cluster_role",
     "sblr.expression.runtime.v3",
     "cluster-role execution or private cluster authority resolution"},
}};

constexpr std::array<ClusterProfileGateExpectedRow, 10> kSbsfc025rZResidualClusterScopeRows{{
    {"SBSQL-04AAB83BBDFB",
     "cluster",
     "sblr.expression.runtime.v3",
     "cluster variable execution or private cluster authority resolution"},
    {"SBSQL-2D0B6A36D19F",
     "cluster_member_id",
     "sblr.expression.runtime.v3",
     "cluster-member identity execution or private cluster authority resolution"},
    {"SBSQL-33CE719FCB2A",
     "clustering_order_spec",
     "sblr.query.relational.v3",
     "clustering-order query execution or private cluster ordering semantics"},
    {"SBSQL-46E8F5296EAC",
     "sb.system.current_cluster",
     "sblr.expression.runtime.v3",
     "system current-cluster execution or private cluster authority resolution"},
    {"SBSQL-7B64081CB18C",
     "current_cluster",
     "sblr.expression.runtime.v3",
     "current-cluster execution or private cluster authority resolution"},
    {"SBSQL-9D9FDE1CCEB8",
     "current_cluster_uuid",
     "sblr.expression.runtime.v3",
     "current-cluster UUID execution or private cluster authority resolution"},
    {"SBSQL-BB44F09607ED",
     "show_cluster",
     "sblr.observability.inspect.v3",
     "show-cluster observability or private cluster topology inspection"},
    {"SBSQL-CEDACEC08071",
     "cluster_authority",
     "sblr.expression.runtime.v3",
     "cluster-authority execution or private cluster authority resolution"},
    {"SBSQL-F736AA74FE0C",
     "SBSQL.CLUSTER_AUTHORITY_REQUIRED",
     "sblr.expression.runtime.v3",
     "cluster-authority-required diagnostic execution or private cluster authority resolution"},
    {"SBSQL-FA8EA998EFC0",
     "cluster_epoch",
     "sblr.expression.runtime.v3",
     "cluster-epoch execution or private cluster authority resolution"},
}};

std::string RowLabel(const sbsql::GeneratedSurfaceRegistryRow& row) {
  std::string label(row.surface_id);
  label += " ";
  label += row.canonical_name;
  return label;
}

void RequireNoRowDetailLeak(const SessionOperationResult& result,
                            const sbsql::GeneratedSurfaceRegistryRow& row) {
  const std::string payload = PayloadText(result);
  const std::array<std::string_view, 4> row_identity_needles{{
      row.surface_id,
      row.fixed_uuid_v7,
      row.validation_fixture_id,
      row.ctest_label,
  }};
  for (const auto needle : row_identity_needles) {
    Require(payload.find(needle) == std::string::npos,
            RowLabel(row) + " leaked row detail in public refusal payload");
    Require(!DiagnosticContains(result, needle),
            RowLabel(row) + " leaked row detail in public refusal diagnostic");
  }
  if (row.canonical_name != std::string_view{"cluster"}) {
    Require(payload.find(row.canonical_name) == std::string::npos,
            RowLabel(row) + " leaked row detail in public refusal payload");
    Require(!DiagnosticContains(result, row.canonical_name),
            RowLabel(row) + " leaked row detail in public refusal diagnostic");
  }
}

void VerifyClusterPrivateRowsFailClosed(ServerSessionRegistry* registry,
                                        const HostedEngineState& engine_state,
                                        const std::array<std::uint8_t, 16>& session_uuid) {
  const std::string session_key = scratchbird::server::UuidBytesToText(session_uuid);
  for (const auto& expected : kSbsfc025rAClusterPrivateRows) {
    const auto* row = sbsql::FindGeneratedSurfaceRegistryRowById(expected.surface_id);
    Require(row != nullptr, std::string(expected.surface_id) + " missing from generated registry");
    Require(row->canonical_name == expected.canonical_name,
            std::string(expected.surface_id) + " generated registry canonical-name mismatch");
    Require(row->family == "cluster_private",
            RowLabel(*row) + " generated registry family is not cluster_private");
    Require(row->source_status == expected.source_status,
            RowLabel(*row) + " generated registry source_status mismatch");
    Require(row->cluster_scope == "cluster_private",
            RowLabel(*row) + " generated registry cluster_scope is not cluster_private");
    Require(row->sblr_operation_family == "sblr.cluster.private_operation.v3",
            RowLabel(*row) + " generated registry SBLR family is not cluster-private");

    const auto session_before = registry->sessions_by_uuid.at(session_key);
    const auto prepared_before = registry->prepared_by_uuid.size();
    const auto cursors_before = registry->cursors_by_uuid.size();
    auto frame = ExecuteFrame(session_uuid, {}, ClusterPrivateParserJsonEnvelope(*row));
    const auto result = scratchbird::server::HandleExecuteSblr(registry, engine_state, frame);

    const std::string label = RowLabel(*row);
    RequireFamilyReconciliationFailure(registry, frame, result, label);
    RequireNoRowDetailLeak(result, *row);
    Require(registry->prepared_by_uuid.size() == prepared_before,
            RowLabel(*row) + " created prepared state on refusal");
    Require(registry->cursors_by_uuid.size() == cursors_before,
            RowLabel(*row) + " opened a cursor on refusal");

    const auto session_after = registry->sessions_by_uuid.at(session_key);
    Require(session_after.local_transaction_id == session_before.local_transaction_id &&
                session_after.snapshot_visible_through_local_transaction_id ==
                    session_before.snapshot_visible_through_local_transaction_id &&
                session_after.transaction_uuid == session_before.transaction_uuid &&
                session_after.transaction_timestamp == session_before.transaction_timestamp,
            RowLabel(*row) + " changed MGA transaction evidence during public refusal");
  }
}

void VerifyClusterProfileGateRowsFailClosed(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const std::array<std::uint8_t, 16>& session_uuid) {
  const std::string session_key = scratchbird::server::UuidBytesToText(session_uuid);
  for (const auto& expected : kSbsfc025rClusterProfileGateRows) {
    const auto* row = sbsql::FindGeneratedSurfaceRegistryRowById(expected.surface_id);
    Require(row != nullptr, std::string(expected.surface_id) + " missing from generated registry");
    Require(row->canonical_name == expected.canonical_name,
            std::string(expected.surface_id) + " generated registry canonical-name mismatch");
    Require(row->source_status == "cluster_private",
            RowLabel(*row) + " generated registry source_status is not cluster_private");
    Require(row->cluster_scope == "cluster_private",
            RowLabel(*row) + " generated registry cluster_scope is not cluster_private");
    Require(row->sblr_operation_family == expected.sblr_operation_family,
            RowLabel(*row) + " generated registry SBLR family mismatch");
    Require(row->parser_handler_key == "parser.cluster_profile_gate",
            RowLabel(*row) + " generated registry parser handler is not cluster profile gate");
    Require(row->server_admission_key == "server.admission.cluster_profile_gate",
            RowLabel(*row) + " generated registry server admission is not cluster profile gate");
    Require(row->engine_rule_key == "engine.rule.cluster_private_fail_closed_or_profile",
            RowLabel(*row) + " generated registry engine rule is not fail-closed/profile gate");
    Require(row->diagnostic_key == "diagnostic.cluster_profile_fail_closed",
            RowLabel(*row) + " generated registry diagnostic is not profile fail-closed");

    const auto session_before = registry->sessions_by_uuid.at(session_key);
    const auto prepared_before = registry->prepared_by_uuid.size();
    const auto cursors_before = registry->cursors_by_uuid.size();
    auto frame = ExecuteFrame(session_uuid, {}, ClusterProfileGateTextEnvelope(*row));
    const auto result = scratchbird::server::HandleExecuteSblr(registry, engine_state, frame);

    const std::string label = RowLabel(*row);
    RequireFamilyReconciliationFailure(registry, frame, result, label);
    RequireNoRowDetailLeak(result, *row);
    Require(registry->prepared_by_uuid.size() == prepared_before,
            RowLabel(*row) + " created prepared state on profile-gate refusal");
    Require(registry->cursors_by_uuid.size() == cursors_before,
            RowLabel(*row) + " opened a cursor on profile-gate refusal");

    const auto session_after = registry->sessions_by_uuid.at(session_key);
    Require(session_after.local_transaction_id == session_before.local_transaction_id &&
                session_after.snapshot_visible_through_local_transaction_id ==
                    session_before.snapshot_visible_through_local_transaction_id &&
                session_after.transaction_uuid == session_before.transaction_uuid &&
                session_after.transaction_timestamp == session_before.transaction_timestamp,
            RowLabel(*row) + " changed MGA transaction evidence during profile-gate refusal");
  }
}

void VerifyClusterScopeResidualRowsFailClosed(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const std::array<std::uint8_t, 16>& session_uuid) {
  const std::string session_key = scratchbird::server::UuidBytesToText(session_uuid);
  for (const auto& expected : kSbsfc025rZResidualClusterScopeRows) {
    const auto* row = sbsql::FindGeneratedSurfaceRegistryRowById(expected.surface_id);
    Require(row != nullptr, std::string(expected.surface_id) + " missing from generated registry");
    Require(row->canonical_name == expected.canonical_name,
            std::string(expected.surface_id) + " generated registry canonical-name mismatch");
    Require(row->source_status == "native_now",
            RowLabel(*row) + " generated registry source_status is not native_now");
    Require(row->cluster_scope == "cluster_private",
            RowLabel(*row) + " generated registry cluster_scope is not cluster_private");
    Require(row->sblr_operation_family == expected.sblr_operation_family,
            RowLabel(*row) + " generated registry SBLR family mismatch");
    Require(row->parser_handler_key == "parser.cluster_profile_gate",
            RowLabel(*row) + " generated registry parser handler is not cluster profile gate");
    Require(row->server_admission_key == "server.admission.cluster_profile_gate",
            RowLabel(*row) + " generated registry server admission is not cluster profile gate");
    Require(row->engine_rule_key == "engine.rule.cluster_private_fail_closed_or_profile",
            RowLabel(*row) + " generated registry engine rule is not fail-closed/profile gate");
    Require(row->diagnostic_key == "diagnostic.cluster_profile_fail_closed",
            RowLabel(*row) + " generated registry diagnostic is not profile fail-closed");

    const auto session_before = registry->sessions_by_uuid.at(session_key);
    const auto prepared_before = registry->prepared_by_uuid.size();
    const auto cursors_before = registry->cursors_by_uuid.size();
    auto frame = ExecuteFrame(session_uuid, {}, ClusterScopeResidualTextEnvelope(*row));
    const auto result = scratchbird::server::HandleExecuteSblr(registry, engine_state, frame);

    const std::string label = RowLabel(*row);
    RequireFamilyReconciliationFailure(registry, frame, result, label);
    RequireNoRowDetailLeak(result, *row);
    Require(registry->prepared_by_uuid.size() == prepared_before,
            RowLabel(*row) + " created prepared state on residual refusal");
    Require(registry->cursors_by_uuid.size() == cursors_before,
            RowLabel(*row) + " opened a cursor on residual refusal");

    const auto session_after = registry->sessions_by_uuid.at(session_key);
    Require(session_after.local_transaction_id == session_before.local_transaction_id &&
                session_after.snapshot_visible_through_local_transaction_id ==
                    session_before.snapshot_visible_through_local_transaction_id &&
                session_after.transaction_uuid == session_before.transaction_uuid &&
                session_after.transaction_timestamp == session_before.transaction_timestamp,
            RowLabel(*row) + " changed MGA transaction evidence during residual refusal");
  }
}

}  // namespace

int main() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const std::vector<std::string> admitted_families = {
      "sblr.acceleration.gpu.v3",
      "sblr.acceleration.llvm.v3",
      "sblr.archive.operation.v3",
      "sblr.backup.operation.v3",
      "sblr.bulk.export.v3",
      "sblr.bulk.import.v3",
      "sblr.catalog.introspect.v3",
      "sblr.catalog.mutation.v3",
      "sblr.cluster.control.v3",
      "sblr.cluster.report.v3",
      "sblr.cursor.operation.v3",
      "sblr.database.management.v3",
      "sblr.diagnostic.control.v3",
      "sblr.diagnostic.refusal.v3",
      "sblr.dml.delete.v3",
      "sblr.dml.insert.v3",
      "sblr.dml.merge.v3",
      "sblr.dml.update.v3",
      "sblr.event.channel.v3",
      "sblr.event.delivery.v3",
      "sblr.event.publication.v3",
      "sblr.event.subscription.v3",
      "sblr.filespace.management.v3",
      "sblr.fulltext.execution.v3",
      "sblr.graph.execution.v3",
      "sblr.index.maintenance.v3",
      "sblr.management.control.v3",
      "sblr.management.report.v3",
      "sblr.metrics.read.v3",
      "sblr.mga.control.v3",
      "sblr.mga.report.v3",
      "sblr.optimizer.plan.v3",
      "sblr.parser.operation.v3",
      "sblr.policy.operation.v3",
      "sblr.query.document.v3",
      "sblr.query.graph.v3",
      "sblr.query.kv.v3",
      "sblr.query.relational.v3",
      "sblr.query.search.v3",
      "sblr.query.timeseries.v3",
      "sblr.query.vector.v3",
      "sblr.replication.consumer.v3",
      "sblr.replication.operation.v3",
      "sblr.routine.define.v3",
      "sblr.routine.execute.v3",
      "sblr.security.mutation.v3",
      "sblr.session.management.v3",
      "sblr.statement.management.v3",
      "sblr.transaction.control.v3",
      "sblr.udr.operation.v3",
      "sblr.vector.execution.v3",
  };
  for (const auto& family : admitted_families) {
    const auto result = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{ParserJsonEnvelope(family), false});
    Require(result.admitted, std::string("server admission rejected primary family: ") + family);
  }

  const std::vector<std::string> rejected_non_primary_families = {
      "sblr.acceleration.operation.v3",
      "sblr.archive_replication.operation.v3",
      "sblr.cluster.private_operation.v3",
      "sblr.dml.operation.v3",
      "sblr.expression.runtime.v3",
      "sblr.general.operation.v3",
      "sblr.jobs.operation.v3",
      "sblr.management.runtime_operation.v3",
      "sblr.observability.inspect.v3",
      "sblr.query.multimodel_or_ddl.v3",
  };
  for (const auto& family : rejected_non_primary_families) {
    auto frame = ExecuteFrame(session_uuid, {}, ParserJsonEnvelope(
        family, false, false, "unreconciled.audit.fixture"));
    const auto result = scratchbird::server::HandleExecuteSblr(&registry, engine_state, frame);
    RequireFamilyReconciliationFailure(&registry, frame, result, family);
  }

  const auto provider_command_envelope = LowerClusterProviderCommand();
  auto provider_command_frame = ExecuteFrame(session_uuid, {}, provider_command_envelope.payload);
  auto provider_command = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, provider_command_frame);
  RequireClusterProviderInfoOutcome(provider_command, "SHOW CLUSTER PROVIDER");
  const auto provider_command_request_it = registry.requests_by_uuid.find(
      scratchbird::server::UuidBytesToText(provider_command_frame.header.request_uuid));
  Require(provider_command_request_it != registry.requests_by_uuid.end(),
          "SHOW CLUSTER PROVIDER did not record request lifecycle evidence");
  Require(provider_command_request_it->second.state ==
              scratchbird::server::ServerRequestLifecycleState::kCompleted,
          "SHOW CLUSTER PROVIDER request lifecycle did not complete");
  Require(provider_command_request_it->second.operation_id == "cluster.inspect_provider",
          "SHOW CLUSTER PROVIDER request lifecycle did not preserve operation id");

  auto provider_info_frame = ExecuteFrame(session_uuid, {}, ClusterProviderInfoTextEnvelope());
  auto provider_info = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, provider_info_frame);
  RequireClusterProviderInfoOutcome(provider_info, "cluster provider info command");
  const auto provider_info_request_it = registry.requests_by_uuid.find(
      scratchbird::server::UuidBytesToText(provider_info_frame.header.request_uuid));
  Require(provider_info_request_it != registry.requests_by_uuid.end(),
          "cluster provider info did not record request lifecycle evidence");
  Require(provider_info_request_it->second.state ==
              scratchbird::server::ServerRequestLifecycleState::kCompleted,
          "cluster provider info request lifecycle did not complete");
  Require(provider_info_request_it->second.operation_id == "cluster.inspect_provider",
          "cluster provider info request lifecycle did not preserve operation id");

  VerifyClusterPrivateRowsFailClosed(&registry, engine_state, session_uuid);
  VerifyClusterProfileGateRowsFailClosed(&registry, engine_state, session_uuid);
  VerifyClusterScopeResidualRowsFailClosed(&registry, engine_state, session_uuid);

  auto cluster_required = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, {}, TextOperationEnvelope(false, true, true)));
  RequireClusterProviderOutcome(cluster_required,
                                "requires_cluster_authority operation",
                                "cluster.inspect_state");

  auto cluster_mapping_active = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{TextOperationEnvelope(false, true, false), true});
  Require(cluster_mapping_active.admitted && cluster_mapping_active.requires_public_abi_dispatch,
          "active cluster-authority admission did not route to the provider boundary");

  auto prepare = scratchbird::server::HandlePrepareSblr(
      &registry, engine_state, PrepareFrame(session_uuid, ParserJsonEnvelope("sblr.query.relational.v3")));
  Require(prepare.accepted, "prepare did not accept relational SBLR");
  const auto prepared_uuid = scratchbird::server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value(), "prepare did not return a prepared statement UUID");
  auto prepared_execute = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, *prepared_uuid, ""));
  Require(prepared_execute.accepted, "execute did not accept prepared SBLR");

  auto cursor_execute = scratchbird::server::HandleExecuteSblr(
      &registry,
      engine_state,
      ExecuteFrame(session_uuid, {}, ParserJsonEnvelope("sblr.query.relational.v3"), true));
  Require(cursor_execute.accepted, "cursor execute did not accept SBLR");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(cursor_execute.payload);
  Require(cursor_uuid.has_value(), "cursor execute did not return a cursor UUID");
  auto fetch = scratchbird::server::HandleFetch(
      &registry, CursorFrame(sbps::MessageType::kFetch, session_uuid, *cursor_uuid));
  Require(fetch.accepted, "fetch did not accept an open cursor");
  auto close = scratchbird::server::HandleCloseCursor(
      &registry, CursorFrame(sbps::MessageType::kCloseCursor, session_uuid, *cursor_uuid));
  Require(close.accepted, "close cursor did not accept an open cursor");

  auto raw_sql = scratchbird::server::HandlePrepareSblr(
      &registry, engine_state, PrepareFrame(session_uuid, scratchbird::server::EncodeRawSqlSblrBypassForTest()));
  Require(!raw_sql.accepted && HasDiagnostic(raw_sql, "SBLR.SQL_TEXT_FORBIDDEN"),
          "raw SQL bypass was not rejected at server admission");

  auto source_marker = scratchbird::server::HandleExecuteSblr(
      &registry,
      engine_state,
      ExecuteFrame(session_uuid, {}, ParserJsonEnvelope("sblr.query.relational.v3", true)));
  Require(!source_marker.accepted && HasDiagnostic(source_marker, "SBLR.SQL_TEXT_FORBIDDEN"),
          "source payload marker was not rejected at server admission");

  auto missing_shape = scratchbird::server::HandleExecuteSblr(
      &registry,
      engine_state,
      ExecuteFrame(session_uuid, {}, ParserJsonEnvelope("sblr.query.relational.v3", false, true)));
  Require(!missing_shape.accepted &&
              HasDiagnostic(missing_shape, "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"),
          "missing result shape was not rejected at server admission");

  auto unresolved = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, {}, TextOperationEnvelope(false, false)));
  Require(!unresolved.accepted &&
              HasDiagnostic(unresolved, "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"),
          "unresolved-name operation envelope was not rejected");

  auto missing_session = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame({}, {}, ParserJsonEnvelope("sblr.query.relational.v3")));
  Require(!missing_session.accepted && HasDiagnostic(missing_session, "PARSER_SERVER_IPC.SESSION_REQUIRED"),
          "missing session was not rejected");

  auto binary = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, {}, BinaryOperationEnvelope(false)));
  Require(binary.accepted && PayloadContains(binary, "product=ScratchBird"),
          "binary operation envelope did not dispatch through public engine ABI");

  auto binary_provider_info_frame =
      ExecuteFrame(session_uuid, {}, BinaryOperationEnvelopeFromText(ClusterProviderInfoTextEnvelope()));
  auto binary_provider_info = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, binary_provider_info_frame);
  RequireClusterProviderInfoOutcome(binary_provider_info,
                                    "binary cluster provider info operation envelope");
  const auto binary_provider_info_request_it = registry.requests_by_uuid.find(
      scratchbird::server::UuidBytesToText(binary_provider_info_frame.header.request_uuid));
  Require(binary_provider_info_request_it != registry.requests_by_uuid.end(),
          "binary cluster provider info did not record request lifecycle evidence");
  Require(binary_provider_info_request_it->second.state ==
              scratchbird::server::ServerRequestLifecycleState::kCompleted,
          "binary cluster provider info request lifecycle did not complete");
  Require(binary_provider_info_request_it->second.operation_id == "cluster.inspect_provider",
          "binary cluster provider info request lifecycle did not preserve operation id");

  auto binary_sql = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, {}, BinaryOperationEnvelope(true)));
  Require(!binary_sql.accepted && HasDiagnostic(binary_sql, "SBLR.SQL_TEXT_FORBIDDEN"),
          "SQL-text-marked binary operation envelope was not rejected");

  std::cout << "sb_server_sbsql_admission_conformance=passed\n";
  return EXIT_SUCCESS;
}
