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

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000084001";

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
    {"SBSQL-CDBCD92B12EB", "reranker_spec", "grammar_production", "RERANKER SPEC local;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-CE7735D5B9EF", "limbo_stmt", "grammar_production", "LIMBO STMT inspect;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-CE84AC72F5A6", "pre_split_clause", "grammar_production", "PRE SPLIT CLAUSE region;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-CEDFBAFE07AC", "salvage_options", "grammar_production", "SALVAGE OPTIONS repair;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-CFBAFFF843B6", "reference_profile_stmt", "grammar_production", "REFERENCE PROFILE STMT inspect;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-CFF8590ACD1B", "returning_clause", "grammar_production", "RETURNING CLAUSE row;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-D055AACF2C98", "historical_read_clause", "grammar_production", "HISTORICAL READ CLAUSE as_of;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-D059E587EC5D", "artifact_ref", "grammar_production", "ARTIFACT REF package;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-D12BA62D1B14", "fill_strategy", "grammar_production", "FILL STRATEGY interpolate;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-D18104C5CB1F", "branch_options", "grammar_production", "BRANCH OPTIONS metadata;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-D1A76E604974", "hex_digit", "grammar_production", "HEX DIGIT f;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-D22F75D62CC7", "resignal", "canonical_surface", "RESIGNAL condition;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-D243D80D4824", "for_cursor_form", "grammar_production", "FOR CURSOR FORM loop;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-D2C72368C35A", "into_target", "grammar_production", "INTO TARGET variable;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-D2FCB9E1F0D6", "pk_column_spec", "grammar_production", "PK COLUMN SPEC id;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-D39E68AF2C1B", "consistency_options", "grammar_production", "CONSISTENCY OPTIONS quorum;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-D3BE005BB092", "routine_name", "grammar_production", "ROUTINE NAME proc;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-D3DDBB36799B", "cte_cycle_clause", "grammar_production", "CTE CYCLE CLAUSE mark;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-D8919039F37B", "pit_stmt", "grammar_production", "PIT STMT restore;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-D9CA1FB549E6", "for_period_clause", "grammar_production", "FOR PERIOD CLAUSE valid_time;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-DB2F4E45D033", "arg", "grammar_production", "ARG VALUE input;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-DC0294147D59", "eviction_clause", "grammar_production", "EVICTION CLAUSE lru;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-DD512E8D3592", "fk_action", "grammar_production", "FK ACTION restrict;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-DD5AE76C7692", "psql_dml_stmt", "grammar_production", "PSQL DML STMT update;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-DD8881681320", "idempotency_level", "grammar_production", "IDEMPOTENCY LEVEL strict;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-DEF1C0A3ABAF", "tenant_action", "grammar_production", "TENANT ACTION inspect;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-DF5B25BE0F5E", "encryption_clause", "grammar_production", "ENCRYPTION CLAUSE local;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-DFC6A08BE094", "size_spec", "grammar_production", "SIZE SPEC medium;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-E0049EB6F526", "capped_option", "grammar_production", "CAPPED OPTION limit;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-E04A57BD05B4", "operator_attr", "grammar_production", "OPERATOR ATTR commutative;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-E07E01BC8853", "tcl_stmt", "grammar_production", "TCL STMT control;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-E108420EE7DB", "treat_form", "grammar_production", "TREAT FORM subtype;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-E2303CE8321E", "plan_options", "grammar_production", "PLAN OPTIONS stable;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-E34B661E63AF", "multiset_constructor", "grammar_production", "MULTISET CONSTRUCTOR values;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-E3B2563CB486", "try_cast_form", "grammar_production", "TRY CAST FORM numeric;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-E4B3A4E3B06A", "param_list", "grammar_production", "PARAM LIST args;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-E57785E2BD95", "set_assignment", "grammar_production", "SET ASSIGNMENT value;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-E7060F899D27", "call_target", "grammar_production", "CALL TARGET routine;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-E9DBAB2CD5A5", "container_predicate", "grammar_production", "CONTAINER PREDICATE contains;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-E9FD6988D3F2", "sweep_action", "grammar_production", "SWEEP ACTION inspect;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-EBFDBD3C1F98", "psql_suspend_stmt", "grammar_production", "PSQL SUSPEND STMT yield;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-ED73971E237F", "secret_attr", "grammar_production", "SECRET ATTR redacted;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-EDE0266E4273", "cte_function_def", "grammar_production", "CTE FUNCTION DEF inline;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-EEEBE71B1DEE", "period_name", "grammar_production", "PERIOD NAME valid_time;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-EF81CD34B2B5", "ddl_relational_stmt", "grammar_production", "DDL RELATIONAL STMT alter;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-EFB43EDCB5D3", "id_start", "grammar_production", "ID START letter;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-F062F95FE9DD", "pipeline_processor", "grammar_production", "PIPELINE PROCESSOR transform;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-F088E1C57601", "previous_value_form", "grammar_production", "PREVIOUS VALUE FORM sequence;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-F092D5BC8027", "variable_declaration", "grammar_production", "VARIABLE DECLARATION local;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
    {"SBSQL-F0B32E269DA7", "partition_spec", "grammar_production", "PARTITION SPEC range;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
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
  session.session_uuid = "019f0000-0000-7000-8000-000000084101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000084102";
  session.database_uuid = "019f0000-0000-7000-8000-000000084103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 84;
  session.security_policy_epoch = 85;
  session.descriptor_epoch = 86;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_084_grammar_surface";
  config.parser_uuid = "019f0000-0000-7000-8000-000000084104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-084-grammar-surface";
  config.build_id = "sbsql-sbsfc-084-grammar-surface";
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
  Require(registry_row != nullptr, "SBSFC-084 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-084 generated registry canonical name drifted");
  Require(registry_row->surface_kind == row.surface_kind,
          "SBSFC-084 generated registry surface kind drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-084 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-084 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          "SBSFC-084 generated registry canonical family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) {
    std::cerr << "SBSFC-084 failing row SQL: " << row.sql << '\n';
    std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  }
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-084 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-084 AST failed");
  Require(artifacts.ast.statement_surface_id == row.surface_id,
          "SBSFC-084 AST row surface id mismatch");
  Require(artifacts.ast.statement_surface_name == row.canonical_name,
          "SBSFC-084 AST canonical name mismatch");
  Require(artifacts.ast.registry_family == "sbsql.general.operation.v3",
          "SBSFC-084 AST registry family mismatch");
  Require(artifacts.ast.operation_family == "sblr.general.operation.v3",
          "SBSFC-084 AST canonical operation family mismatch");
  Require(artifacts.bound.bound, "SBSFC-084 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-084 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.values.v3",
          "SBSFC-084 route operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.values.v3",
          "SBSFC-084 route operation key mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "SBSFC-084 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "query.plan_operation",
          "SBSFC-084 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "SBSFC-084 opcode mismatch");
  Require(artifacts.envelope.engine_api_function == "EnginePlanOperation",
          "SBSFC-084 engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-084 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-084 parser no-finality authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          "SBSFC-084 cluster provider exclusion authority missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, row.descriptor_ref),
          "SBSFC-084 descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-084 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.route_kind),
          "SBSFC-084 payload missing route kind");
  Require(Contains(artifacts.envelope.payload, row.descriptor_role),
          "SBSFC-084 payload missing descriptor role");
  Require(Contains(artifacts.envelope.payload, row.descriptor_ref),
          "SBSFC-084 payload missing descriptor ref");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-084 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-084 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "exact_refusal") &&
              !Contains(artifacts.envelope.payload, "cluster_support_not_enabled"),
          "SBSFC-084 payload used replay, refusal, or cluster-provider error evidence");
  Require(Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":false") &&
              Contains(artifacts.envelope.payload, "\"private_cluster_execution\":false"),
          "SBSFC-084 payload did not prove no cluster/private dispatch");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "recovery_authority\":true"),
          "SBSFC-084 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "SBSFC-084 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-084 server admission did not require public ABI dispatch");
  Require(admission.operation_id == "query.plan_operation",
          "SBSFC-084 server admission operation id mismatch");
  Require(admission.operation_family == "sblr.optimizer.plan.v3",
          "SBSFC-084 server admission operation family mismatch");

  const auto* opcode = sblr::LookupSblrOperation("query.plan_operation");
  Require(opcode != nullptr, "SBSFC-084 opcode registry row missing");
  Require(opcode->opcode == "SBLR_QUERY_PLAN_OPERATION", "SBSFC-084 opcode registry drifted");
  Require(opcode->requires_cluster_authority == false,
          "SBSFC-084 opcode claimed cluster authority");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-084-grammar-surface";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000084201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000084202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000084203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000084204";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000084205";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000084206";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.current_sqlstate = "00000";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000084207";
  context.trace_tags.push_back("sbsfc084.grammar_surface");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.sbsfc084.grammar_surface");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "sbsfc084_grammar_surface"});
  envelope.operands.push_back({"text", "sbsfc084_surface_id", std::string(row.surface_id)});
  envelope.operands.push_back({"text", "sbsfc084_runtime_evidence_kind", "sbsfc084_grammar_surface_route"});
  envelope.operands.push_back({"text", "sbsfc084_runtime_evidence_id", std::string(row.canonical_name)});
  envelope.operands.push_back({"text", "sbsfc084_descriptor_role", std::string(row.descriptor_role)});
  envelope.operands.push_back({"text", "sbsfc084_descriptor_ref", std::string(row.descriptor_ref)});
  envelope.operands.push_back({"text", "query_operation", "descriptor_validation"});
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "sbsfc084_grammar_surface";
  request.option_envelopes.push_back(std::string("sbsfc084_surface_id:") + std::string(row.surface_id));
  request.option_envelopes.push_back("sbsfc084_runtime_evidence_kind:sbsfc084_grammar_surface_route");
  request.option_envelopes.push_back(std::string("sbsfc084_runtime_evidence_id:") + std::string(row.canonical_name));
  request.option_envelopes.push_back(std::string("sbsfc084_descriptor_role:") + std::string(row.descriptor_role));
  request.option_envelopes.push_back(std::string("sbsfc084_descriptor_ref:") + std::string(row.descriptor_ref));
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
  Require(result.envelope_validated, "SBSFC-084 engine envelope rejected");
  Require(result.accepted, "SBSFC-084 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-084 engine did not dispatch to API");
  Require(result.api_result.operation_id == "query.plan_operation",
          "SBSFC-084 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-084 runtime API did not complete");
  Require(HasEvidence(result.api_result, "sbsfc084_grammar_surface_route", row.canonical_name),
          "SBSFC-084 runtime evidence missing");
  Require(HasEvidence(result.api_result, "sbsfc084_surface", row.surface_id),
          "SBSFC-084 runtime did not carry row surface evidence");
  Require(HasEvidence(result.api_result, "sbsfc084_descriptor_role", row.descriptor_role),
          "SBSFC-084 runtime did not carry descriptor-role evidence");
  Require(HasEvidence(result.api_result, "sbsfc084_descriptor_ref", row.descriptor_ref),
          "SBSFC-084 runtime did not carry descriptor-ref evidence");
  Require(HasEvidence(result.api_result, "parser_executes_sql", "false"),
          "SBSFC-084 runtime allowed parser SQL execution");
  Require(HasEvidence(result.api_result, "cluster_provider_dispatch", "false"),
          "SBSFC-084 runtime claimed cluster provider dispatch");
  Require(HasEvidence(result.api_result, "private_cluster_execution", "false"),
          "SBSFC-084 runtime claimed private cluster execution");
  Require(HasEvidence(result.api_result, "wal_recovery_authority", "false"),
          "SBSFC-084 runtime carried WAL/recovery authority");
}

}  // namespace

int main() {
  static_assert(sizeof(kCases) / sizeof(kCases[0]) == 50);
  for (const auto& row : kCases) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row, RunPipeline(row));
  }

  const auto context = EngineContext();
  for (const auto& row : kCases) {
    RequireEngineDispatch(context, row);
  }

  std::cout << "sbsql_sbsfc_084_grammar_surface_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
