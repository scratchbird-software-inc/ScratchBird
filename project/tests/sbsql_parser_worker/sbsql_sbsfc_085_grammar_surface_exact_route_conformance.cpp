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
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000085001";

struct CaseRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view sql;
  std::string_view route_kind;
  std::string_view descriptor_role;
  std::string_view descriptor_ref;
};

const CaseRow kCases[] = {
    {"SBSQL-F178404D32D6", "psql_pipe_row_stmt", "grammar_production", "PSQL PIPE ROW STMT result;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-F24C10C05F96", "call_result_clause", "grammar_production", "CALL RESULT CLAUSE row;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-F2580B10CA17", "psql_compound_stmt", "grammar_production", "PSQL COMPOUND STMT block;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-F29DE2ED8D20", "reference_profile_options", "grammar_production", "REFERENCE PROFILE OPTIONS local;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-F3006C91D952", "call", "canonical_surface", "CALL routine;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-F375BA38C102", "new_surface_stmt", "grammar_production", "NEW SURFACE STMT descriptor;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-F5E78906D903", "psql_exit_stmt", "grammar_production", "PSQL EXIT STMT loop;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-F6DE057B7557", "ch_system_target", "grammar_production", "CH SYSTEM TARGET local;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-F7175D85A4DE", "cleanup_control_stmt", "grammar_production", "CLEANUP CONTROL STMT retain;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-F87CEF07BC0E", "topology_assign", "grammar_production", "TOPOLOGY ASSIGN region;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-F8B5D61CE628", "frame_bound", "grammar_production", "FRAME BOUND CURRENT;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-F9ED7C5325E9", "object_path", "grammar_production", "OBJECT PATH schema.table;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-FAC34DDEAC9D", "call_stmt", "grammar_production", "CALL STMT routine;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-FBE931BE7E40", "fdb_tx_options", "grammar_production", "FDB TX OPTIONS snapshot;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-FDA029BA9834", "server_action", "grammar_production", "SERVER ACTION restart;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-FDBD9AF96128", "subpartition_spec_list", "grammar_production", "SUBPARTITION SPEC LIST range;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-FEC5437FD5D9", "exception_condition", "grammar_production", "EXCEPTION CONDITION sqlstate;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-FEE85792235D", "psql_continue_stmt", "grammar_production", "PSQL CONTINUE STMT loop;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-FF3DF4E417E0", "declare_item", "grammar_production", "DECLARE ITEM variable;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
};

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

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000085101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000085102";
  session.database_uuid = "019f0000-0000-7000-8000-000000085103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 85;
  session.security_policy_epoch = 86;
  session.descriptor_epoch = 87;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_085_grammar_surface";
  config.parser_uuid = "019f0000-0000-7000-8000-000000085104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-085-grammar-surface";
  config.build_id = "sbsql-sbsfc-085-grammar-surface";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(const CaseRow& row) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(row.sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const CaseRow& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "SBSFC-085 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-085 generated registry canonical name drifted");
  Require(registry_row->surface_kind == row.surface_kind,
          "SBSFC-085 generated registry surface kind drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-085 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-085 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          "SBSFC-085 generated registry canonical family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-085 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-085 AST failed");
  Require(artifacts.ast.statement_surface_id == row.surface_id,
          "SBSFC-085 AST row surface id mismatch");
  Require(artifacts.ast.statement_surface_name == row.canonical_name,
          "SBSFC-085 AST canonical name mismatch");
  Require(artifacts.ast.registry_family == "sbsql.general.operation.v3",
          "SBSFC-085 AST registry family mismatch");
  Require(artifacts.ast.operation_family == "sblr.general.operation.v3",
          "SBSFC-085 AST canonical operation family mismatch");
  Require(artifacts.bound.bound, "SBSFC-085 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-085 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.values.v3",
          "SBSFC-085 route operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.values.v3",
          "SBSFC-085 route operation key mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "SBSFC-085 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "query.plan_operation",
          "SBSFC-085 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "SBSFC-085 opcode mismatch");
  Require(artifacts.envelope.engine_api_function == "EnginePlanOperation",
          "SBSFC-085 engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-085 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-085 parser no-finality authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          "SBSFC-085 cluster provider exclusion authority missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, row.descriptor_ref),
          "SBSFC-085 descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-085 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.route_kind),
          "SBSFC-085 payload missing route kind");
  Require(Contains(artifacts.envelope.payload, row.descriptor_role),
          "SBSFC-085 payload missing descriptor role");
  Require(Contains(artifacts.envelope.payload, row.descriptor_ref),
          "SBSFC-085 payload missing descriptor ref");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-085 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-085 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "exact_refusal") &&
              !Contains(artifacts.envelope.payload, "cluster_support_not_enabled"),
          "SBSFC-085 payload used replay, refusal, or cluster-provider error evidence");
  Require(Contains(artifacts.envelope.payload, "\"parser_claims_transaction_finality\":false") &&
              Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":false") &&
              Contains(artifacts.envelope.payload, "\"private_cluster_execution\":false"),
          "SBSFC-085 payload did not prove no finality or cluster/private dispatch");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "recovery_authority\":true"),
          "SBSFC-085 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "SBSFC-085 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-085 server admission did not require public ABI dispatch");
  Require(admission.operation_id == "query.plan_operation",
          "SBSFC-085 server admission operation id mismatch");
  Require(admission.operation_family == "sblr.query.values.v3",
          "SBSFC-085 server admission operation family mismatch");

  const auto* opcode = sblr::LookupSblrOperation("query.plan_operation");
  Require(opcode != nullptr, "SBSFC-085 opcode registry row missing");
  Require(opcode->opcode == "SBLR_QUERY_PLAN_OPERATION", "SBSFC-085 opcode registry drifted");
  Require(opcode->requires_cluster_authority == false,
          "SBSFC-085 opcode claimed cluster authority");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-085-grammar-surface";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000085201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000085202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000085203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000085204";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000085205";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000085206";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.current_sqlstate = "00000";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000085207";
  context.trace_tags.push_back("sbsfc085.grammar_surface");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.sbsfc085.grammar_surface");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "sbsfc085_grammar_surface"});
  envelope.operands.push_back({"text", "sbsfc085_surface_id", std::string(row.surface_id)});
  envelope.operands.push_back({"text", "sbsfc085_runtime_evidence_kind", "sbsfc085_grammar_surface_route"});
  envelope.operands.push_back({"text", "sbsfc085_runtime_evidence_id", std::string(row.canonical_name)});
  envelope.operands.push_back({"text", "sbsfc085_descriptor_role", std::string(row.descriptor_role)});
  envelope.operands.push_back({"text", "sbsfc085_descriptor_ref", std::string(row.descriptor_ref)});
  envelope.operands.push_back({"text", "query_operation", "descriptor_validation"});
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "sbsfc085_grammar_surface";
  request.option_envelopes.push_back(std::string("sbsfc085_surface_id:") + std::string(row.surface_id));
  request.option_envelopes.push_back("sbsfc085_runtime_evidence_kind:sbsfc085_grammar_surface_route");
  request.option_envelopes.push_back(std::string("sbsfc085_runtime_evidence_id:") + std::string(row.canonical_name));
  request.option_envelopes.push_back(std::string("sbsfc085_descriptor_role:") + std::string(row.descriptor_role));
  request.option_envelopes.push_back(std::string("sbsfc085_descriptor_ref:") + std::string(row.descriptor_ref));
  request.option_envelopes.push_back("query_operation:descriptor_validation");
  return request;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

void RequireEngineDispatch(const api::EngineRequestContext& context, const CaseRow& row) {
  const auto result = sblr::DispatchSblrOperation({context, EngineEnvelope(row), ApiRequestFor(row)});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-085 engine envelope rejected");
  Require(result.accepted, "SBSFC-085 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-085 engine did not dispatch to API");
  Require(result.api_result.operation_id == "query.plan_operation",
          "SBSFC-085 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-085 runtime API did not complete");
  Require(HasEvidence(result.api_result, "sbsfc085_grammar_surface_route", row.canonical_name),
          "SBSFC-085 runtime evidence missing");
  Require(HasEvidence(result.api_result, "sbsfc085_surface", row.surface_id),
          "SBSFC-085 runtime did not carry row surface evidence");
  Require(HasEvidence(result.api_result, "sbsfc085_descriptor_role", row.descriptor_role),
          "SBSFC-085 runtime did not carry descriptor-role evidence");
  Require(HasEvidence(result.api_result, "sbsfc085_descriptor_ref", row.descriptor_ref),
          "SBSFC-085 runtime did not carry descriptor-ref evidence");
  Require(HasEvidence(result.api_result, "parser_executes_sql", "false"),
          "SBSFC-085 runtime allowed parser SQL execution");
  Require(HasEvidence(result.api_result, "parser_claims_transaction_finality", "false"),
          "SBSFC-085 runtime claimed parser transaction finality");
  Require(HasEvidence(result.api_result, "cluster_provider_dispatch", "false"),
          "SBSFC-085 runtime claimed cluster provider dispatch");
  Require(HasEvidence(result.api_result, "private_cluster_execution", "false"),
          "SBSFC-085 runtime claimed private cluster execution");
  Require(HasEvidence(result.api_result, "wal_recovery_authority", "false"),
          "SBSFC-085 runtime carried WAL/recovery authority");
}

}  // namespace

int main() {
  static_assert(sizeof(kCases) / sizeof(kCases[0]) == 19);
  for (const auto& row : kCases) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row, RunPipeline(row));
  }

  const auto context = EngineContext();
  for (const auto& row : kCases) {
    RequireEngineDispatch(context, row);
  }

  std::cout << "sbsql_sbsfc_085_grammar_surface_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
