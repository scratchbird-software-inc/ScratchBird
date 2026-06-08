// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef SCRATCHBIRD_PROJECT_SOURCE_DIR
#define SCRATCHBIRD_PROJECT_SOURCE_DIR "."
#endif

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;

struct RouteRow {
  std::string_view label;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view result_shape;
  bool engine_api_command;
  bool requires_transaction_context;
};

constexpr std::array<RouteRow, 24> kExactRows{{
    {"show.cluster_gpu_placement", "SHOW CLUSTER GPU PLACEMENT", "op.show.cluster_gpu_placement", "SBLR_OP_SHOW_CLUSTER_GPU_PLACEMENT", "rs.show.cluster_gpu_placement.v1", false, false},
    {"show.cluster.state", "SHOW CLUSTER STATE", "op.show.cluster.state", "SBLR_OP_SHOW_CLUSTER_STATE", "rs.cluster.state.v1", false, false},
    {"show.cluster.topology", "SHOW CLUSTER TOPOLOGY", "op.show.cluster.topology", "SBLR_OP_SHOW_CLUSTER_TOPOLOGY", "rs.cluster.state.v1", false, false},
    {"show.cluster.members", "SHOW CLUSTER MEMBERS", "op.show.cluster.members", "SBLR_OP_SHOW_CLUSTER_MEMBERS", "rs.cluster.state.v1", false, false},
    {"show.cluster.capabilities", "SHOW CLUSTER CAPABILITIES", "op.show.cluster.capabilities", "SBLR_OP_SHOW_CLUSTER_CAPABILITIES", "rs.cluster.state.v1", false, false},
    {"show.cluster.routing_plan", "SHOW CLUSTER ROUTING PLAN", "op.show.cluster.routing_plan", "SBLR_OP_SHOW_CLUSTER_ROUTING_PLAN", "rs.cluster.routing.v1", false, false},
    {"show.cluster.admission_status", "SHOW CLUSTER ADMISSION STATUS", "op.show.cluster.admission_status", "SBLR_OP_SHOW_CLUSTER_ADMISSION_STATUS", "rs.cluster.state.v1", false, false},
    {"show.cluster.route_epoch", "SHOW CLUSTER ROUTE EPOCH", "op.show.cluster.route_epoch", "SBLR_OP_SHOW_CLUSTER_ROUTE_EPOCH", "rs.cluster.routing.v1", false, false},
    {"show.cluster.decisions", "SHOW CLUSTER DECISIONS", "op.show.cluster.decisions", "SBLR_OP_SHOW_CLUSTER_DECISIONS", "rs.cluster.decision.v1", false, false},
    {"show.cluster.limbo", "SHOW CLUSTER LIMBO", "op.show.cluster.limbo", "SBLR_OP_SHOW_CLUSTER_LIMBO", "rs.cluster.decision.v1", false, false},
    {"show.cluster.recovery", "SHOW CLUSTER RECOVERY", "op.show.cluster.recovery", "SBLR_OP_SHOW_CLUSTER_RECOVERY", "rs.cluster.decision.v1", false, false},
    {"show.cluster.slo", "SHOW CLUSTER SLO", "op.show.cluster.slo", "SBLR_OP_SHOW_CLUSTER_SLO", "rs.cluster.state.v1", false, false},
    {"show.cluster.error_budget", "SHOW CLUSTER ERROR BUDGET", "op.show.cluster.error_budget", "SBLR_OP_SHOW_CLUSTER_ERROR_BUDGET", "rs.cluster.state.v1", false, false},
    {"show.cluster.readiness", "SHOW CLUSTER READINESS", "op.show.cluster.readiness", "SBLR_OP_SHOW_CLUSTER_READINESS", "rs.cluster.state.v1", false, false},
    {"show.cluster.alerts", "SHOW CLUSTER ALERTS", "op.show.cluster.alerts", "SBLR_OP_SHOW_CLUSTER_ALERTS", "rs.cluster.state.v1", false, false},
    {"show.cluster.replication", "SHOW CLUSTER REPLICATION", "op.show.cluster.replication", "SBLR_OP_SHOW_CLUSTER_REPLICATION", "rs.cluster.state.v1", false, false},
    {"show.cluster.shards", "SHOW CLUSTER SHARDS", "op.show.cluster.shards", "SBLR_OP_SHOW_CLUSTER_SHARDS", "rs.cluster.routing.v1", false, false},
    {"show.cluster.placement", "SHOW CLUSTER PLACEMENT", "op.show.cluster.placement", "SBLR_OP_SHOW_CLUSTER_PLACEMENT", "rs.cluster.routing.v1", false, false},
    {"show.cluster.archive", "SHOW CLUSTER ARCHIVE", "op.show.cluster.archive", "SBLR_OP_SHOW_CLUSTER_ARCHIVE", "rs.cluster.state.v1", false, false},
    {"alter.cluster.route_publish", "ALTER CLUSTER ROUTE PUBLISH route_alpha", "op.cluster.route_publish", "SBLR_OP_CLUSTER_ROUTE_PUBLISH", "rs.acceleration.control.v1", false, false},
    {"alter.cluster.placement_move", "ALTER CLUSTER PLACEMENT MOVE object_alpha TO member_alpha", "op.cluster.placement_move", "SBLR_OP_CLUSTER_PLACEMENT_MOVE", "rs.acceleration.control.v1", false, false},
    {"alter.cluster.admission_tune", "ALTER CLUSTER ADMISSION TUNE policy_alpha", "op.cluster.admission_tune", "SBLR_OP_CLUSTER_ADMISSION_TUNE", "rs.acceleration.control.v1", false, false},
    {"alter.cluster.recovery_resolution", "ALTER CLUSTER RECOVERY RESOLVE decision_alpha", "op.cluster.recovery_resolution", "SBLR_OP_CLUSTER_RECOVERY_RESOLUTION", "rs.acceleration.control.v1", false, false},
    {"alter.cluster.config", "ALTER CLUSTER CONFIG max_routes SET 128", "op.cluster.config", "SBLR_OP_CLUSTER_CONFIG", "rs.acceleration.control.v1", false, false},
}};

constexpr std::array<RouteRow, 8> kApiRows{{
    {"cluster.sys.agents", "ENGINE CLUSTER SYS AGENTS", "cluster.sys.agents", "SBLR_CLUSTER_SYS_AGENTS", "cluster.provider.stub.v1", true, false},
    {"cluster.inspect_state", "ENGINE CLUSTER INSPECT STATE", "cluster.inspect_state", "SBLR_CLUSTER_INSPECT_STATE", "cluster.provider.stub.v1", true, false},
    {"cluster.inspect_routing_plan", "ENGINE CLUSTER INSPECT ROUTING PLAN", "cluster.inspect_routing_plan", "SBLR_CLUSTER_INSPECT_ROUTING_PLAN", "cluster.provider.stub.v1", true, false},
    {"cluster.control_cluster", "ENGINE CLUSTER CONTROL CLUSTER", "cluster.control_cluster", "SBLR_CLUSTER_CONTROL_CLUSTER", "cluster.provider.stub.v1", true, false},
    {"cluster.prepare_remote_participant_insert", "ENGINE CLUSTER PREPARE REMOTE PARTICIPANT INSERT", "cluster.prepare_remote_participant_insert", "SBLR_CLUSTER_PREPARE_REMOTE_PARTICIPANT_INSERT", "cluster.provider.stub.v1", true, true},
    {"cluster.validate_insert_route_fence", "ENGINE CLUSTER VALIDATE INSERT ROUTE FENCE", "cluster.validate_insert_route_fence", "SBLR_CLUSTER_VALIDATE_INSERT_ROUTE_FENCE", "cluster.provider.stub.v1", true, true},
    {"cluster.place_object", "ENGINE CLUSTER PLACE OBJECT", "cluster.place_object", "SBLR_CLUSTER_PLACE_OBJECT", "cluster.provider.stub.v1", true, true},
    {"cluster.inspect_replication", "ENGINE CLUSTER INSPECT REPLICATION", "cluster.inspect_replication", "SBLR_CLUSTER_INSPECT_REPLICATION", "cluster.provider.stub.v1", true, false},
}};

struct SurfaceEvidenceRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view operation_id;
};

constexpr std::array<SurfaceEvidenceRow, 56> kSurfaceEvidenceRows{{
    {"SBSQL-00FA147ED5B6", "member_ref_list", "cluster.surface.member_ref_list"},
    {"SBSQL-01D7277D7AD1", "evidence_item", "cluster.surface.evidence_item"},
    {"SBSQL-04AAB83BBDFB", "cluster", "cluster.surface.cluster"},
    {"SBSQL-1586EBD60331", "reason_code", "cluster.surface.reason_code"},
    {"SBSQL-1AB70DEEAD48", "cluster_setting_stmt", "cluster.surface.cluster_setting_stmt"},
    {"SBSQL-1F5D197F682F", "partition_ref", "cluster.surface.partition_ref"},
    {"SBSQL-2D0B6A36D19F", "cluster_member_id", "cluster.surface.cluster_member_id"},
    {"SBSQL-2D5711FE6D48", "branch_ref", "cluster.surface.branch_ref"},
    {"SBSQL-31D7FA95A32E", "idempotency_key", "cluster.surface.idempotency_key"},
    {"SBSQL-33CE719FCB2A", "clustering_order_spec", "cluster.surface.clustering_order_spec"},
    {"SBSQL-3547284AA7F8", "show_cluster_extended", "cluster.surface.show_cluster_extended"},
    {"SBSQL-39C545BEBF5A", "cluster_publish_options", "cluster.surface.cluster_publish_options"},
    {"SBSQL-3AA85DA2ED21", "cluster_commit_options", "cluster.surface.cluster_commit_options"},
    {"SBSQL-3AD6213D718C", "config_key", "cluster.surface.config_key"},
    {"SBSQL-3AE3460649B5", "decision_service_profile", "cluster.surface.decision_service_profile"},
    {"SBSQL-3D3DCFA99D24", "cluster_rollback_options", "cluster.surface.cluster_rollback_options"},
    {"SBSQL-3DBCE185F851", "cluster_ref", "cluster.surface.cluster_ref"},
    {"SBSQL-3F704EACCE08", "cluster_role", "cluster.surface.cluster_role"},
    {"SBSQL-46E8F5296EAC", "sb.system.current_cluster", "cluster.surface.sb.system.current_cluster"},
    {"SBSQL-4EF886377AB2", "drop_cluster_stmt", "cluster.surface.drop_cluster_stmt"},
    {"SBSQL-5FEA0732FD1C", "alter_cluster_stmt", "cluster.surface.alter_cluster_stmt"},
    {"SBSQL-627269ECF1F9", "cluster_prepare_options", "cluster.surface.cluster_prepare_options"},
    {"SBSQL-6657E884971F", "cluster_reconcile_stmt", "cluster.surface.cluster_reconcile_stmt"},
    {"SBSQL-6689D8CFD6EA", "create_cluster_stmt", "cluster.surface.create_cluster_stmt"},
    {"SBSQL-70D0DE52A93E", "cluster_show_target", "cluster.surface.cluster_show_target"},
    {"SBSQL-7129A967AB22", "cluster_audit_stmt", "cluster.surface.cluster_audit_stmt"},
    {"SBSQL-71A081B78B8A", "cluster_stmt", "cluster.surface.cluster_stmt"},
    {"SBSQL-7B64081CB18C", "current_cluster", "cluster.surface.current_cluster"},
    {"SBSQL-7D8D2A369443", "cluster_tx_stmt", "cluster.surface.cluster_tx_stmt"},
    {"SBSQL-88E2119F5FFF", "cluster_topology_stmt", "cluster.surface.cluster_topology_stmt"},
    {"SBSQL-8E6FE3EB1CDB", "conflict_strategy", "cluster.surface.conflict_strategy"},
    {"SBSQL-91A719925D7D", "evidence_list", "cluster.surface.evidence_list"},
    {"SBSQL-9D9FDE1CCEB8", "current_cluster_uuid", "cluster.surface.current_cluster_uuid"},
    {"SBSQL-A25FD5F1EFD2", "reconcile_options", "cluster.surface.reconcile_options"},
    {"SBSQL-A32AF92DF5F2", "cluster_lifecycle_ddl", "cluster.surface.cluster_lifecycle_ddl"},
    {"SBSQL-AD04656773E5", "object_ref", "cluster.surface.object_ref"},
    {"SBSQL-AE72C38E901A", "cluster_node_op_stmt", "cluster.surface.cluster_node_op_stmt"},
    {"SBSQL-B9CAFA029514", "cluster_throttle_stmt", "cluster.surface.cluster_throttle_stmt"},
    {"SBSQL-BB44F09607ED", "show_cluster", "cluster.surface.show_cluster"},
    {"SBSQL-BC81B2A0B934", "show_decisions_filter", "cluster.surface.show_decisions_filter"},
    {"SBSQL-BE088AF09680", "decision_ref", "cluster.surface.decision_ref"},
    {"SBSQL-CD249C7FC6A4", "cluster_job_control_stmt", "cluster.surface.cluster_job_control_stmt"},
    {"SBSQL-CEDACEC08071", "cluster_authority", "cluster.surface.cluster_authority"},
    {"SBSQL-D873CD6AEFCF", "member_role", "cluster.surface.member_role"},
    {"SBSQL-E04EF6DF0271", "cluster_system_op_stmt", "cluster.surface.cluster_system_op_stmt"},
    {"SBSQL-E24BF89F2917", "alter_cluster_action", "cluster.surface.alter_cluster_action"},
    {"SBSQL-E2D1CE36612D", "cluster_member_op_stmt", "cluster.surface.cluster_member_op_stmt"},
    {"SBSQL-E338883A13CD", "cluster_failover_stmt", "cluster.surface.cluster_failover_stmt"},
    {"SBSQL-E47D4A865961", "cluster_node_uuid", "cluster.surface.cluster_node_uuid"},
    {"SBSQL-EB755314527A", "cluster_control_stmt", "cluster.surface.cluster_control_stmt"},
    {"SBSQL-F1E1F8A83B4F", "route_ref", "cluster.surface.route_ref"},
    {"SBSQL-F704A29FE1C8", "policy_ref", "cluster.surface.policy_ref"},
    {"SBSQL-F736AA74FE0C", "SBSQL.CLUSTER_AUTHORITY_REQUIRED", "cluster.surface.sbsql.cluster_authority_required"},
    {"SBSQL-FA8EA998EFC0", "cluster_epoch", "cluster.surface.cluster_epoch"},
    {"SBSQL-FC6C131013BF", "member_ref", "cluster.surface.member_ref"},
    {"SBSQL-FEA1D03D90A7", "quorum_rule", "cluster.surface.quorum_rule"},
}};

void Require(bool condition, std::string_view message) {
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

bool HasApiDiagnosticCode(const api::EngineApiResult& result,
                          std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDispatchDiagnosticCode(const sblr::SblrDispatchResult& result,
                               std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

bool ClusterProviderCompileLinkStubBuild() {
  const auto info = cluster_provider::DescribeClusterProvider();
  return info.compile_link_only && info.provider_type == "compile_link_stub";
}

bool HasField(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) return true;
  }
  return false;
}

std::string EvidenceMessage(const RouteRow& row,
                            std::string_view phase,
                            std::string_view detail) {
  std::string out(row.label);
  out += ' ';
  out += phase;
  out += ": ";
  out += detail;
  return out;
}

std::string_view ExpectedAdmissionFamily(const RouteRow& row) {
  if (row.operation_id == "cluster.inspect_replication") {
    return "sblr.replication.consumer.v3";
  }
  if (row.operation_id == "cluster.control_cluster" ||
      row.operation_id == "cluster.prepare_remote_participant_insert" ||
      row.operation_id == "cluster.validate_insert_route_fence" ||
      row.operation_id == "cluster.place_object" ||
      row.operation_id.starts_with("op.cluster.")) {
    return "sblr.cluster.control.v3";
  }
  return "sblr.cluster.report.v3";
}

std::string EvidenceMessage(const SurfaceEvidenceRow& row,
                            std::string_view phase,
                            std::string_view detail) {
  std::string out(row.surface_id);
  out += ' ';
  out += row.canonical_name;
  out += ' ';
  out += phase;
  out += ": ";
  out += detail;
  return out;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-0000000f1501";
  session.connection_uuid = "019f0000-0000-7000-8000-0000000f1502";
  session.database_uuid = "019f0000-0000-7000-8000-0000000f1503";
  session.catalog_epoch = 151;
  session.security_policy_epoch = 157;
  session.descriptor_epoch = 163;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-0000000f1504";
  config.bundle_contract_id = "sbp_sbsql@sbsql-sblr-final-cleanup-b015";
  config.build_id = "sbsql-sblr-final-cleanup-b015";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
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

api::EngineRequestContext EngineContext(bool security_context_present = true) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sblr-final-cleanup-b015";
  context.security_context_present = security_context_present;
  context.cluster_authority_available = cluster_provider::ClusterProviderSupportsExecution();
  context.database_path = "/tmp/sbsql_sblr_final_cleanup_b015.sbdb";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-0000000f1601";
  context.cluster_uuid.canonical = "019f0000-0000-7000-8000-0000000f1602";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-0000000f1603";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-0000000f1604";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-0000000f1605";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-0000000f1606";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-0000000f1607";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-0000000f1608";
  context.local_transaction_id = 1515;
  context.snapshot_visible_through_local_transaction_id = 1515;
  context.catalog_generation_id = 151;
  context.security_epoch = 157;
  context.resource_epoch = 163;
  context.trace_tags = {
      "security.bootstrap",
      "group:ROOT",
      "role:ROOT",
      "right:CLUSTER_INSPECT",
      "right:CLUSTER_CONTROL",
  };
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const RouteRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.b015.") +
                                             std::string(row.label));
  envelope.result_shape = row.result_shape;
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = row.requires_transaction_context;
  envelope.requires_cluster_authority = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

sblr::SblrOperationEnvelope SurfaceEnvelope(const SurfaceEvidenceRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         "SBLR_CLUSTER_PROFILE_OPERATION",
                                         std::string("trace.b015.surface.") +
                                             std::string(row.canonical_name));
  envelope.result_shape = "cluster.provider.stub.v1";
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_cluster_authority = true;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireRegistryAndDispatch(const RouteRow& row) {
  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr, EvidenceMessage(row, "registry", "operation id missing"));
  Require(entry->opcode == row.opcode, EvidenceMessage(row, "registry", "opcode mismatch"));
  Require(entry->category == sblr::SblrOpcodeCategory::cluster,
          EvidenceMessage(row, "registry", "operation is not in the cluster category"));
  Require(entry->support == sblr::SblrOpcodeSupport::cluster_refusal,
          EvidenceMessage(row, "registry", "operation does not preserve cluster-boundary support"));
  Require(entry->requires_cluster_authority,
          EvidenceMessage(row, "registry", "operation does not require cluster authority"));

  const auto envelope = EngineEnvelope(row);
  const auto opcode_validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(opcode_validation.ok, EvidenceMessage(row, "opcode_validation", "valid envelope rejected"));

  auto missing_cluster_authority = envelope;
  missing_cluster_authority.requires_cluster_authority = false;
  const auto missing_cluster_validation =
      sblr::ValidateSblrOpcodeForEnvelope(missing_cluster_authority);
  Require(!missing_cluster_validation.ok &&
              missing_cluster_validation.diagnostic_id == "SB_DIAG_CLUSTER_TXN_UNAVAILABLE",
          EvidenceMessage(row, "opcode_validation", "missing cluster authority was not refused"));

  sblr::SblrDispatchRequest request;
  request.context = EngineContext();
  request.envelope = envelope;
  request.api_request.context = request.context;
  request.api_request.operation_id = std::string(row.operation_id);
  request.api_request.option_envelopes.push_back(std::string("result_shape_contract:") +
                                                 std::string(row.result_shape));
  const auto dispatch = sblr::DispatchSblrOperation(request);
  Require(dispatch.envelope_validated,
          EvidenceMessage(row, "dispatch", "engine envelope validation failed"));
  Require(dispatch.accepted && dispatch.dispatched_to_api,
          EvidenceMessage(row, "dispatch", "operation did not reach API dispatch"));
  if (ClusterProviderCompileLinkStubBuild()) {
    Require(!dispatch.api_result.ok,
            EvidenceMessage(row, "dispatch", "cluster-stub build executed route"));
    Require(dispatch.api_result.cluster_authority_required,
            EvidenceMessage(row, "dispatch", "cluster-stub result did not require cluster authority"));
    Require(dispatch.api_result.result_shape.result_kind == "cluster.provider.stub.v1",
            EvidenceMessage(row, "dispatch", "cluster-stub result kind changed"));
    Require(HasApiDiagnosticCode(
                dispatch.api_result,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            EvidenceMessage(row, "dispatch", "cluster-stub compile-link vector missing"));
    Require(HasDispatchDiagnosticCode(
                dispatch,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            EvidenceMessage(row, "dispatch", "cluster-stub dispatch vector missing"));
    Require(HasEvidence(dispatch.api_result, "cluster_provider", "stub"),
            EvidenceMessage(row, "dispatch", "cluster-stub provider evidence missing"));
    Require(HasEvidence(dispatch.api_result,
                        "cluster_provider_name",
                        "scratchbird.cluster.compile_link_stub_provider"),
            EvidenceMessage(row, "dispatch", "cluster-stub provider name evidence missing"));
  } else if (cluster_provider::ClusterProviderSupportsExecution()) {
    Require(dispatch.api_result.ok,
            EvidenceMessage(row, "dispatch", "cluster provider did not return ok"));
  } else {
    Require(!dispatch.api_result.ok,
            EvidenceMessage(row, "dispatch", "non-cluster build unexpectedly executed route"));
    Require(dispatch.api_result.cluster_authority_required,
            EvidenceMessage(row, "dispatch", "non-cluster result did not require cluster authority"));
    Require(HasApiDiagnosticCode(dispatch.api_result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            EvidenceMessage(row, "dispatch", "non-cluster unsupported vector missing"));
    Require(HasDispatchDiagnosticCode(dispatch, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            EvidenceMessage(row, "dispatch", "non-cluster dispatch vector missing"));
    Require(!HasApiDiagnosticCode(dispatch.api_result, "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY"),
            EvidenceMessage(row, "dispatch", "non-cluster build called the stub provider"));
  }
}

void RequireSurfaceProviderEvidence(const SurfaceEvidenceRow& row) {
  sblr::SblrDispatchRequest request;
  request.context = EngineContext();
  request.envelope = SurfaceEnvelope(row);
  request.api_request.context = request.context;
  request.api_request.operation_id = std::string(row.operation_id);
  Require(request.envelope.result_shape == "cluster.provider.stub.v1",
          EvidenceMessage(row, "envelope", "surface result shape is not provider-boundary stub"));
  Require(request.envelope.requires_cluster_authority,
          EvidenceMessage(row, "envelope", "surface route does not require cluster authority"));
  Require(request.envelope.parser_resolved_names_to_uuids,
          EvidenceMessage(row, "envelope", "surface route did not preserve resolved-name requirement"));
  Require(!request.envelope.contains_sql_text,
          EvidenceMessage(row, "envelope", "surface route embedded SQL text"));
  const auto dispatch = sblr::DispatchSblrOperation(request);
  Require(dispatch.envelope_validated,
          EvidenceMessage(row, "dispatch", "surface envelope validation failed"));
  Require(dispatch.accepted && dispatch.dispatched_to_api,
          EvidenceMessage(row, "dispatch", "surface route did not enter provider boundary"));
  if (ClusterProviderCompileLinkStubBuild()) {
    Require(!dispatch.api_result.ok,
            EvidenceMessage(row, "dispatch", "surface route executed in cluster-stub build"));
    Require(dispatch.api_result.cluster_authority_required,
            EvidenceMessage(row, "dispatch", "surface stub result did not require cluster authority"));
    Require(dispatch.api_result.result_shape.result_kind == "cluster.provider.stub.v1",
            EvidenceMessage(row, "dispatch", "surface stub result kind changed"));
    Require(HasApiDiagnosticCode(
                dispatch.api_result,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            EvidenceMessage(row, "dispatch", "surface stub compile-link vector missing"));
    Require(HasDispatchDiagnosticCode(
                dispatch,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            EvidenceMessage(row, "dispatch", "surface stub dispatch vector missing"));
    Require(HasEvidence(dispatch.api_result, "cluster_provider", "stub"),
            EvidenceMessage(row, "dispatch", "surface stub provider evidence missing"));
    Require(HasEvidence(dispatch.api_result,
                        "cluster_provider_name",
                        "scratchbird.cluster.compile_link_stub_provider"),
            EvidenceMessage(row, "dispatch", "surface stub provider name evidence missing"));
  } else if (cluster_provider::ClusterProviderSupportsExecution()) {
    Require(dispatch.api_result.ok,
            EvidenceMessage(row, "dispatch", "surface route did not receive provider response"));
  } else {
    Require(!dispatch.api_result.ok,
            EvidenceMessage(row, "dispatch", "surface route executed in non-cluster build"));
    Require(dispatch.api_result.cluster_authority_required,
            EvidenceMessage(row, "dispatch", "surface unsupported result did not require cluster authority"));
    Require(HasApiDiagnosticCode(dispatch.api_result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            EvidenceMessage(row, "dispatch", "surface unsupported vector missing"));
    Require(HasDispatchDiagnosticCode(dispatch, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            EvidenceMessage(row, "dispatch", "surface dispatch unsupported vector missing"));
    Require(!HasApiDiagnosticCode(dispatch.api_result, "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY"),
            EvidenceMessage(row, "dispatch", "surface route called stub provider in non-cluster build"));
    Require(HasEvidence(dispatch.api_result, "cluster_provider", "no_cluster"),
            EvidenceMessage(row, "dispatch", "surface no-cluster provider evidence missing"));
  }
  Require(!HasEvidence(dispatch.api_result, "private_cluster_execution", "true"),
          EvidenceMessage(row, "dispatch", "surface route claimed private cluster execution"));
}

void RequireLowering(const RouteRow& row) {
  const auto artifacts = RunPipeline(row.sql);
  Require(artifacts.bound.bound, EvidenceMessage(row, "pipeline", "row did not bind"));
  Require(!artifacts.bound.requires_name_resolution,
          EvidenceMessage(row, "pipeline", "row required parser-side name resolution"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "pipeline", "SBLR verifier rejected row"));
  Require(artifacts.envelope.operation_id == row.operation_id,
          EvidenceMessage(row, "pipeline", "operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          EvidenceMessage(row, "pipeline", "opcode mismatch"));
  Require(artifacts.envelope.operation_family == "sblr.cluster.private_operation.v3",
          EvidenceMessage(row, "pipeline", "operation family mismatch"));
  Require(artifacts.envelope.result_shape_key == row.result_shape,
          EvidenceMessage(row, "pipeline", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == "EngineClusterProviderRoute",
          EvidenceMessage(row, "pipeline", "engine API function is not provider-boundary route"));
  Require(Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":true"),
          EvidenceMessage(row, "payload", "cluster provider dispatch flag missing"));
  Require(Contains(artifacts.envelope.payload, "\"private_cluster_execution\":false"),
          EvidenceMessage(row, "payload", "private cluster execution flag missing"));
  Require(Contains(artifacts.envelope.payload,
                   row.engine_api_command ? "\"engine_api_command_route\":true"
                                          : "\"public_sbsql_exact_command\":true"),
          EvidenceMessage(row, "payload", "expected route marker missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.cluster_provider_boundary_required"),
          EvidenceMessage(row, "authority", "provider-boundary authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_required"),
          EvidenceMessage(row, "authority", "provider-dispatch authority missing"));
  Require(!HasValue(artifacts.envelope.required_authority_steps,
                    "authority.cluster.provider_dispatch_not_required"),
          EvidenceMessage(row, "authority", "non-cluster provider authority leaked"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          EvidenceMessage(row, "authority", "no-SQL authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          EvidenceMessage(row, "authority", "no-finality authority missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.cluster.provider"),
          EvidenceMessage(row, "authority", "cluster provider descriptor missing"));
  Require(HasValue(artifacts.envelope.policy_refs, "cluster_provider_boundary_policy"),
          EvidenceMessage(row, "authority", "cluster provider boundary policy missing"));

  server::ServerSblrAdmissionRequest admission_request;
  admission_request.encoded_sblr_envelope = artifacts.envelope.payload;
  const auto admission = server::AdmitServerSblrEnvelope(admission_request);
  Require(admission.admitted, EvidenceMessage(row, "admission", "server rejected parser JSON envelope"));
  Require(admission.operation_id == row.operation_id,
          EvidenceMessage(row, "admission", "server changed cluster operation id"));
  Require(admission.operation_family == ExpectedAdmissionFamily(row),
          EvidenceMessage(row, "admission", "server changed cluster operation family"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(row, "admission", "server did not require public ABI dispatch"));
}

void RequireProviderInfoRoute() {
  const auto info = cluster_provider::DescribeClusterProvider();
  Require(!info.provider_name.empty(), "cluster provider name is empty");
  Require(!info.provider_type.empty(), "cluster provider type is empty");
  Require(!info.provider_version.empty(), "cluster provider version is empty");
  Require(!info.support_status.empty(), "cluster provider support status is empty");
  Require(info.supports_execution == cluster_provider::ClusterProviderSupportsExecution(),
          "cluster provider support flag drifted");

  const auto artifacts = RunPipeline("SHOW CLUSTER PROVIDER");
  Require(artifacts.verifier.admitted, "SHOW CLUSTER PROVIDER verifier rejected provider route");
  Require(artifacts.envelope.operation_id == "cluster.inspect_provider",
          "SHOW CLUSTER PROVIDER operation id changed");
  Require(artifacts.envelope.sblr_opcode == "SBLR_CLUSTER_INSPECT_PROVIDER",
          "SHOW CLUSTER PROVIDER opcode changed");
  Require(artifacts.envelope.result_shape_key == "cluster.provider.info.v1",
          "SHOW CLUSTER PROVIDER result shape changed");

  sblr::SblrDispatchRequest request;
  request.context = EngineContext();
  request.envelope = sblr::MakeSblrEnvelope("cluster.inspect_provider",
                                            "SBLR_CLUSTER_INSPECT_PROVIDER",
                                            "trace.b015.provider_info");
  request.envelope.result_shape = "cluster.provider.info.v1";
  request.envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = false;
  request.api_request.context = request.context;
  request.api_request.operation_id = "cluster.inspect_provider";
  const auto dispatch = sblr::DispatchSblrOperation(request);
  Require(dispatch.api_result.ok, "SHOW CLUSTER PROVIDER dispatch did not return provider info");
  Require(dispatch.api_result.result_shape.result_kind == "cluster.provider.info.v1",
          "SHOW CLUSTER PROVIDER result kind changed");
  Require(HasApiDiagnosticCode(dispatch.api_result, "SBLR.CLUSTER.PROVIDER_INFO"),
          "SHOW CLUSTER PROVIDER provider-info vector missing");
  Require(!dispatch.api_result.result_shape.rows.empty(),
          "SHOW CLUSTER PROVIDER provider-info row missing");
  const auto& row = dispatch.api_result.result_shape.rows.front();
  Require(HasField(row, "provider_name"), "SHOW CLUSTER PROVIDER provider_name field missing");
  Require(HasField(row, "provider_type"), "SHOW CLUSTER PROVIDER provider_type field missing");
  Require(HasField(row, "provider_version"), "SHOW CLUSTER PROVIDER provider_version field missing");
  Require(HasField(row, "support_status"), "SHOW CLUSTER PROVIDER support_status field missing");
  Require(HasField(row, "supports_execution"), "SHOW CLUSTER PROVIDER support flag field missing");
}

void RequireRefusalPreservation() {
  const auto unknown_family =
      server::AdmitServerSblrEnvelope({"sblr.not.real.v3", false});
  Require(!unknown_family.admitted &&
              !unknown_family.diagnostics.empty() &&
              unknown_family.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
          "unknown SBLR family refusal changed");

  const auto raw_sql = server::AdmitServerSblrEnvelope({"SELECT 1", false});
  Require(!raw_sql.admitted &&
              !raw_sql.diagnostics.empty() &&
              raw_sql.diagnostics.front().code == "SBLR.SQL_TEXT_FORBIDDEN",
          "raw SQL refusal changed");

  const std::string duplicate_text =
      "operation_id=cluster.inspect_state\n"
      "operation_id=cluster.inspect_routing_plan\n"
      "result_shape=cluster.provider.stub.v1\n"
      "diagnostic_shape=diagnostic.canonical_message_vector\n"
      "parser_resolved_names_to_uuids=true\n"
      "requires_cluster_authority=true\n";
  const auto duplicate_text_result =
      server::AdmitServerSblrEnvelope({duplicate_text, false});
  Require(!duplicate_text_result.admitted &&
              !duplicate_text_result.diagnostics.empty() &&
              duplicate_text_result.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.SBLR_DUPLICATE_FIELD",
          "duplicate text-field refusal changed");

  const std::string duplicate_json =
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"operation_family\":\"sblr.cluster.private_operation.v3\","
      "\"operation_id\":\"cluster.inspect_state\","
      "\"operation_id\":\"cluster.inspect_routing_plan\","
      "\"result_shape\":\"cluster.provider.stub.v1\","
      "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\"}";
  const auto duplicate_json_result =
      server::AdmitServerSblrEnvelope({duplicate_json, false});
  Require(!duplicate_json_result.admitted &&
              !duplicate_json_result.diagnostics.empty() &&
              duplicate_json_result.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.SBLR_DUPLICATE_FIELD",
          "duplicate JSON-field refusal changed");

  const std::string sql_text_json =
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"operation_family\":\"sblr.cluster.private_operation.v3\","
      "\"operation_id\":\"cluster.inspect_state\","
      "\"result_shape\":\"cluster.provider.stub.v1\","
      "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\","
      "\"sql_text\":\"SELECT 1\"}";
  const auto sql_text_json_result =
      server::AdmitServerSblrEnvelope({sql_text_json, false});
  Require(!sql_text_json_result.admitted &&
              !sql_text_json_result.diagnostics.empty() &&
              sql_text_json_result.diagnostics.front().code == "SBLR.SQL_TEXT_FORBIDDEN",
          "JSON SQL-text refusal changed");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 21> kForbidden = {
      "sbsql_sblr_final_cleanup",
      "final_cleanup",
      "B015Exact",
      "IsB015",
      "B016Exact",
      "IsB016",
      "b015_",
      "_b015",
      "b016_",
      "_b016",
      "AUDIT-0",
      "AUDIT-1",
      "AUDIT-2",
      "AUDIT-3",
      "AUDIT-4",
      "AUDIT-5",
      "AUDIT-6",
      "AUDIT-7",
      "AUDIT-8",
      "AUDIT-9",
      "SSFC-",
  };
  const std::filesystem::path src_root =
      std::filesystem::path(SCRATCHBIRD_PROJECT_SOURCE_DIR) / "src";
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src_root)) {
    if (!entry.is_regular_file()) continue;
    const auto extension = entry.path().extension().string();
    if (extension != ".cpp" && extension != ".hpp" && extension != ".h" &&
        extension != ".cc" && extension != ".cxx" && extension != ".yaml" &&
        extension != ".cmake" && extension != ".txt") {
      continue;
    }
    std::ifstream in(entry.path());
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    for (const auto forbidden : kForbidden) {
      Require(!Contains(content, forbidden),
              std::string("production source contains forbidden batch token: ") +
                  std::string(forbidden) + " in " + entry.path().string());
    }
  }
}

int Main() {
  for (const auto& row : kExactRows) {
    RequireLowering(row);
    RequireRegistryAndDispatch(row);
  }
  for (const auto& row : kApiRows) {
    RequireLowering(row);
    RequireRegistryAndDispatch(row);
  }
  for (const auto& row : kSurfaceEvidenceRows) {
    RequireSurfaceProviderEvidence(row);
  }
  RequireProviderInfoRoute();
  RequireRefusalPreservation();
  RequireProductionSourceIntegrity();
  return EXIT_SUCCESS;
}

}  // namespace

int main() {
  return Main();
}
