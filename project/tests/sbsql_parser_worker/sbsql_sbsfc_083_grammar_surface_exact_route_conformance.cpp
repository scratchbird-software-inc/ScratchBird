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

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000083001";

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
    {"SBSQL-9CB80D4097E7", "jt_column", "grammar_production", "JT COLUMN doc_path;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-9CFE8241A531", "dict_source", "grammar_production", "DICT SOURCE external_table;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-9E96E1E438E2", "conformance_target", "grammar_production", "CONFORMANCE TARGET native_profile;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-9E979F06D084", "package_item", "grammar_production", "PACKAGE ITEM routine;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-A13651AA0904", "period_clause", "grammar_production", "PERIOD CLAUSE system_time;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-A14E48F035C4", "historical_read_target", "grammar_production", "HISTORICAL READ TARGET snapshot;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-A4A957234B3D", "management_stmt", "grammar_production", "MANAGEMENT STMT inspect;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-A5103C6736BE", "hash_field_value", "grammar_production", "HASH FIELD VALUE digest;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-A5C10C819E2E", "ch_codec_spec", "grammar_production", "CH CODEC SPEC lz4;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-A5C25356BB39", "publication_stmt", "grammar_production", "PUBLICATION STMT publish;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-A6572A3FA0B5", "for_portion_of_clause", "grammar_production", "FOR PORTION OF CLAUSE valid_time;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-A6E66E386FC2", "fk_action_clause", "grammar_production", "FK ACTION CLAUSE cascade;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-A8A3735959A8", "tag", "grammar_production", "TAG audit;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-A9F5EAE695BC", "collate_postfix", "grammar_production", "COLLATE POSTFIX locale;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-AB19887B6AF7", "next_value_form", "grammar_production", "NEXT VALUE FORM sequence;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-AC7EA9FF937C", "trigger_referencing", "grammar_production", "TRIGGER REFERENCING new_row;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-AD7F131BA1B5", "dict_layout", "grammar_production", "DICT LAYOUT hashed;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-AE5CF079F635", "profile_name", "grammar_production", "PROFILE NAME native;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-AEADDD1DDCA0", "generic_options", "grammar_production", "GENERIC OPTIONS enabled;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-B076619988F5", "match_arg", "grammar_production", "MATCH ARG pattern;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-B09E4276557F", "service_stmt", "grammar_production", "SERVICE STMT inspect;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-B194BCE246C0", "transform_op", "grammar_production", "TRANSFORM OP normalize;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-B360961F4863", "ddl_routine_stmt", "grammar_production", "DDL ROUTINE STMT create;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-B403B588D61C", "timeline_clause", "grammar_production", "TIMELINE CLAUSE as_of;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-B47294E7BD81", "capped_clause", "grammar_production", "CAPPED CLAUSE bounded;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-B47DAD013054", "historical_time_spec", "grammar_production", "HISTORICAL TIME SPEC timestamp;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-B5C70DC3E5D5", "label_set", "grammar_production", "LABEL SET labels;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-B5EF49FB3CA0", "collate_clause", "grammar_production", "COLLATE CLAUSE locale;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-BA6B24CF59F6", "field_def", "grammar_production", "FIELD DEF field;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-BA8D5321870E", "position_regex_form", "grammar_production", "POSITION REGEX FORM first;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-BC26101DC25F", "ann_options", "grammar_production", "ANN OPTIONS vector;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-BD25AB099FC3", "row_shape", "grammar_production", "ROW SHAPE record;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-BD8F7D730110", "continuous_aggregate_stmt", "grammar_production", "CONTINUOUS AGGREGATE STMT refresh;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-BE52C03DA5E8", "enforced_modifier", "grammar_production", "ENFORCED MODIFIER enforced;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-C256F1C2BE87", "multiset_set_op", "grammar_production", "MULTISET SET OP union;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-C3C1B9ED259C", "trigger_event", "grammar_production", "TRIGGER EVENT insert;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-C3E1013D5D56", "bucket_attr", "grammar_production", "BUCKET ATTR interval;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-C549C7094D59", "identity_options", "grammar_production", "IDENTITY OPTIONS generated;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-C597F4442BC2", "correlation_name", "grammar_production", "CORRELATION NAME c;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-C5A3F8222FAA", "refresh_strategy", "grammar_production", "REFRESH STRATEGY incremental;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-C68E170B31A3", "trigger_target", "grammar_production", "TRIGGER TARGET table;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-C6B57B9FAD6B", "pipeline_stage", "grammar_production", "PIPELINE STAGE transform;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-C7736B152780", "consistency_level", "grammar_production", "CONSISTENCY LEVEL quorum;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-C7FB78BCCCB7", "throttle_assign", "grammar_production", "THROTTLE ASSIGN limit;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-C8CC497D7495", "var_default", "grammar_production", "VAR DEFAULT value;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-CA1E1DFF7F04", "monitor_action", "grammar_production", "MONITOR ACTION sample;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-CB4343FAB245", "postfix_unary_expr", "grammar_production", "POSTFIX UNARY EXPR factorial;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-CB8490F98825", "ch_system_verb", "grammar_production", "CH SYSTEM VERB reload;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-CC098A428576", "data_change_stmt", "grammar_production", "DATA CHANGE STMT merge;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-CD6D9CB540EC", "psql_if_stmt", "grammar_production", "PSQL IF STMT branch;", "procedural_descriptor_validation", "procedural_descriptor", "sys.procedure.control_flow"},
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
  session.session_uuid = "019f0000-0000-7000-8000-000000083101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000083102";
  session.database_uuid = "019f0000-0000-7000-8000-000000083103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 83;
  session.security_policy_epoch = 84;
  session.descriptor_epoch = 85;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_083_grammar_surface";
  config.parser_uuid = "019f0000-0000-7000-8000-000000083104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-083-grammar-surface";
  config.build_id = "sbsql-sbsfc-083-grammar-surface";
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
  Require(registry_row != nullptr, "SBSFC-083 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-083 generated registry canonical name drifted");
  Require(registry_row->surface_kind == row.surface_kind,
          "SBSFC-083 generated registry surface kind drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-083 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-083 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          "SBSFC-083 generated registry canonical family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) {
    std::cerr << "SBSFC-083 failing row SQL: " << row.sql << '\n';
    std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  }
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-083 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-083 AST failed");
  Require(artifacts.ast.statement_surface_id == row.surface_id,
          "SBSFC-083 AST row surface id mismatch");
  Require(artifacts.ast.statement_surface_name == row.canonical_name,
          "SBSFC-083 AST canonical name mismatch");
  Require(artifacts.ast.registry_family == "sbsql.general.operation.v3",
          "SBSFC-083 AST registry family mismatch");
  Require(artifacts.ast.operation_family == "sblr.general.operation.v3",
          "SBSFC-083 AST canonical operation family mismatch");
  Require(artifacts.bound.bound, "SBSFC-083 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-083 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.values.v3",
          "SBSFC-083 route operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.values.v3",
          "SBSFC-083 route operation key mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "SBSFC-083 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "query.plan_operation",
          "SBSFC-083 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "SBSFC-083 opcode mismatch");
  Require(artifacts.envelope.engine_api_function == "EnginePlanOperation",
          "SBSFC-083 engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-083 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-083 parser no-finality authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          "SBSFC-083 cluster provider exclusion authority missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, row.descriptor_ref),
          "SBSFC-083 descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-083 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.route_kind),
          "SBSFC-083 payload missing route kind");
  Require(Contains(artifacts.envelope.payload, row.descriptor_role),
          "SBSFC-083 payload missing descriptor role");
  Require(Contains(artifacts.envelope.payload, row.descriptor_ref),
          "SBSFC-083 payload missing descriptor ref");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-083 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-083 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "exact_refusal") &&
              !Contains(artifacts.envelope.payload, "cluster_support_not_enabled"),
          "SBSFC-083 payload used replay, refusal, or cluster-provider error evidence");
  Require(Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":false") &&
              Contains(artifacts.envelope.payload, "\"private_cluster_execution\":false"),
          "SBSFC-083 payload did not prove no cluster/private dispatch");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "recovery_authority\":true"),
          "SBSFC-083 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "SBSFC-083 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-083 server admission did not require public ABI dispatch");
  Require(admission.operation_id == "query.plan_operation",
          "SBSFC-083 server admission operation id mismatch");
  Require(admission.operation_family == "sblr.query.values.v3",
          "SBSFC-083 server admission operation family mismatch");

  const auto* opcode = sblr::LookupSblrOperation("query.plan_operation");
  Require(opcode != nullptr, "SBSFC-083 opcode registry row missing");
  Require(opcode->opcode == "SBLR_QUERY_PLAN_OPERATION", "SBSFC-083 opcode registry drifted");
  Require(opcode->requires_cluster_authority == false,
          "SBSFC-083 opcode claimed cluster authority");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-083-grammar-surface";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000083201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000083202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000083203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000083204";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000083205";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000083206";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.current_sqlstate = "00000";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000083207";
  context.trace_tags.push_back("sbsfc083.grammar_surface");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.sbsfc083.grammar_surface");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "sbsfc083_grammar_surface"});
  envelope.operands.push_back({"text", "sbsfc083_surface_id", std::string(row.surface_id)});
  envelope.operands.push_back({"text", "sbsfc083_runtime_evidence_kind", "sbsfc083_grammar_surface_route"});
  envelope.operands.push_back({"text", "sbsfc083_runtime_evidence_id", std::string(row.canonical_name)});
  envelope.operands.push_back({"text", "sbsfc083_descriptor_role", std::string(row.descriptor_role)});
  envelope.operands.push_back({"text", "sbsfc083_descriptor_ref", std::string(row.descriptor_ref)});
  envelope.operands.push_back({"text", "query_operation", "descriptor_validation"});
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "sbsfc083_grammar_surface";
  request.option_envelopes.push_back(std::string("sbsfc083_surface_id:") + std::string(row.surface_id));
  request.option_envelopes.push_back("sbsfc083_runtime_evidence_kind:sbsfc083_grammar_surface_route");
  request.option_envelopes.push_back(std::string("sbsfc083_runtime_evidence_id:") + std::string(row.canonical_name));
  request.option_envelopes.push_back(std::string("sbsfc083_descriptor_role:") + std::string(row.descriptor_role));
  request.option_envelopes.push_back(std::string("sbsfc083_descriptor_ref:") + std::string(row.descriptor_ref));
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
  Require(result.envelope_validated, "SBSFC-083 engine envelope rejected");
  Require(result.accepted, "SBSFC-083 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-083 engine did not dispatch to API");
  Require(result.api_result.operation_id == "query.plan_operation",
          "SBSFC-083 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-083 runtime API did not complete");
  Require(HasEvidence(result.api_result, "sbsfc083_grammar_surface_route", row.canonical_name),
          "SBSFC-083 runtime evidence missing");
  Require(HasEvidence(result.api_result, "sbsfc083_surface", row.surface_id),
          "SBSFC-083 runtime did not carry row surface evidence");
  Require(HasEvidence(result.api_result, "sbsfc083_descriptor_role", row.descriptor_role),
          "SBSFC-083 runtime did not carry descriptor-role evidence");
  Require(HasEvidence(result.api_result, "sbsfc083_descriptor_ref", row.descriptor_ref),
          "SBSFC-083 runtime did not carry descriptor-ref evidence");
  Require(HasEvidence(result.api_result, "parser_executes_sql", "false"),
          "SBSFC-083 runtime allowed parser SQL execution");
  Require(HasEvidence(result.api_result, "cluster_provider_dispatch", "false"),
          "SBSFC-083 runtime claimed cluster provider dispatch");
  Require(HasEvidence(result.api_result, "private_cluster_execution", "false"),
          "SBSFC-083 runtime claimed private cluster execution");
  Require(HasEvidence(result.api_result, "wal_recovery_authority", "false"),
          "SBSFC-083 runtime carried WAL/recovery authority");
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

  std::cout << "sbsql_sbsfc_083_grammar_surface_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
