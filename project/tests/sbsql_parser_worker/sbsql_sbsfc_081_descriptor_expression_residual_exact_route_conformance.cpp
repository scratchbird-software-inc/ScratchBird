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

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000081001";

struct CaseRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view sql;
  std::string_view route_kind;
  std::string_view descriptor_role;
};

const CaseRow kCases[] = {
    {"SBSQL-0125E8D0A6D1", "event_trigger_filter", "grammar_production", "EVENT TRIGGER FILTER ddl_command_start;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-03702B1136AD", "aggregate_attr", "grammar_production", "AGGREGATE ATTR parallel_safe;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-04B9187EAD75", "op_class_ref", "grammar_production", "OP CLASS REF btree_int4;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-080C7025BD17", "cassandra_using_clause", "grammar_production", "CASSANDRA USING compaction;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-082DAD9A42A9", "letter", "grammar_production", "LETTER alpha;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-0935AF9FA568", "exp_part", "grammar_production", "EXP PART exponent;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-0BA7367A3ECB", "partition_spec_list", "grammar_production", "PARTITION SPEC LIST by_hash;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-0C3D98AE0669", "pattern_name", "grammar_production", "PATTERN NAME p_customer;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-0F1A2CA1C1B3", "label_ref", "grammar_production", "LABEL REF customer;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-103AEFEC035F", "trigger_timing", "grammar_production", "TRIGGER TIMING before;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-11832617333F", "postfix_op", "grammar_production", "POSTFIX OP factorial;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-13E1A83F7DD9", "fork_stmt", "grammar_production", "FORK STMT branch;", "query_descriptor_validation", "query_plan_descriptor"},
    {"SBSQL-1670155F0C42", "action_destination", "grammar_production", "ACTION DESTINATION queue;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-16DADF94E1FE", "log_message", "canonical_surface", "LOG MESSAGE info;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-174532CAE182", "object_filter", "grammar_production", "OBJECT FILTER visible;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-18BDD689AEAB", "substring_regex_form", "grammar_production", "SUBSTRING REGEX FORM similar;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-1AC9A54E0B23", "trigger_event_list", "grammar_production", "TRIGGER EVENT LIST insert_update;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-1B8738FF028A", "range_spec", "grammar_production", "RANGE SPEC closed_open;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-1D78D4177491", "strict_typing_clause", "grammar_production", "STRICT TYPING CLAUSE enabled;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-1DBD2F18F169", "plan_format", "grammar_production", "PLAN FORMAT json;", "query_descriptor_validation", "query_plan_descriptor"},
    {"SBSQL-1E8CD8B54D76", "dict_attribute", "grammar_production", "DICT ATTRIBUTE locale;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-1ED9EAA8A3F5", "xt_column", "grammar_production", "XT COLUMN revision;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-1F9E6ED125AD", "regex_flags", "grammar_production", "REGEX FLAGS im;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-20D58A36E93B", "event_trigger_event", "grammar_production", "EVENT TRIGGER EVENT ddl_command_end;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-21A4A17B1D91", "call_arg", "grammar_production", "CALL ARG value;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-24CA8E39DB9D", "ch_column_source_mode", "grammar_production", "CH COLUMN SOURCE MODE materialized;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-29C683A6E05A", "tz_clause", "grammar_production", "TZ CLAUSE utc;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-2A33559E2EAE", "opaque_options", "grammar_production", "OPAQUE OPTIONS enabled;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-2B7126C58E41", "uuid_to_name", "canonical_surface", "UUID TO NAME target;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-30081E8BD543", "monitor_trigger", "grammar_production", "MONITOR TRIGGER latency;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-3164FCB6F0C2", "locator_class", "grammar_production", "LOCATOR CLASS lob;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-323D7DC9A6B3", "colon_lambda", "grammar_production", "COLON LAMBDA arg;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-326F53DE3AF3", "for_clause", "grammar_production", "FOR CLAUSE row;", "query_descriptor_validation", "query_plan_descriptor"},
    {"SBSQL-32D9056B1A68", "partition_values", "grammar_production", "PARTITION VALUES default;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-348929A6D6DB", "translate_regex_form", "grammar_production", "TRANSLATE REGEX FORM global;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-3516F5E8D621", "set_clause_list", "grammar_production", "SET CLAUSE LIST assignments;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-37AB85284359", "id_continue", "grammar_production", "ID CONTINUE digit;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-37EEAF2C07FF", "consistency_clause", "grammar_production", "CONSISTENCY CLAUSE quorum;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-38220B8A0FA4", "tag_options", "grammar_production", "TAG OPTIONS retain;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-387A13A6980A", "interval_unit", "grammar_production", "INTERVAL UNIT day;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-3A85B0A04312", "diag_field", "grammar_production", "DIAG FIELD message_text;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-3B1681CCCB3A", "lang_name", "grammar_production", "LANG NAME sbsql;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-3B27132FBBCD", "path_segment", "grammar_production", "PATH SEGMENT root;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-3E788869BFCD", "exclusion_element", "grammar_production", "EXCLUSION ELEMENT overlap;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-411811D60025", "hint_list", "grammar_production", "HINT LIST index_scan;", "query_descriptor_validation", "query_plan_descriptor"},
    {"SBSQL-43D4215C83C4", "subpartition_spec", "grammar_production", "SUBPARTITION SPEC region;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-4D9BF2CA1E65", "os_field_mapping", "grammar_production", "OS FIELD MAPPING user_name;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-4DBC46FD3C16", "revision_spec", "grammar_production", "REVISION SPEC head;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
    {"SBSQL-4E6A112CC36E", "occurrences_regex_form", "grammar_production", "OCCURRENCES REGEX FORM first;", "expression_descriptor_validation", "expression_descriptor"},
    {"SBSQL-4EC678B20CF4", "operator_name", "grammar_production", "OPERATOR NAME plus;", "ddl_descriptor_validation", "catalog_ddl_descriptor"},
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
  session.session_uuid = "019f0000-0000-7000-8000-000000081101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000081102";
  session.database_uuid = "019f0000-0000-7000-8000-000000081103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 81;
  session.security_policy_epoch = 82;
  session.descriptor_epoch = 83;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_081_descriptor_expression_residual";
  config.parser_uuid = "019f0000-0000-7000-8000-000000081104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-081-descriptor-expression-residual";
  config.build_id = "sbsql-sbsfc-081-descriptor-expression-residual";
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
  Require(registry_row != nullptr, "SBSFC-081 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-081 generated registry canonical name drifted");
  Require(registry_row->surface_kind == row.surface_kind,
          "SBSFC-081 generated registry surface kind drifted");
  Require(registry_row->source_status == "native_now" ||
              registry_row->source_status == "e2e_passed",
          "SBSFC-081 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-081 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          "SBSFC-081 generated registry canonical family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.ast.messages.has_errors()) {
    std::cerr << row.surface_id << ':' << row.canonical_name << ':' << row.sql << '\n';
  }
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-081 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-081 AST failed");
  Require(artifacts.ast.statement_surface_id == row.surface_id,
          "SBSFC-081 AST row surface id mismatch");
  Require(artifacts.ast.statement_surface_name == row.canonical_name,
          "SBSFC-081 AST canonical name mismatch");
  Require(artifacts.ast.registry_family == "sbsql.general.operation.v3",
          "SBSFC-081 AST registry family mismatch");
  Require(artifacts.ast.operation_family == "sblr.general.operation.v3",
          "SBSFC-081 AST canonical operation family mismatch");
  Require(artifacts.bound.bound, "SBSFC-081 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-081 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "SBSFC-081 route operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.relational.v3",
          "SBSFC-081 route operation key mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "SBSFC-081 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "query.plan_operation",
          "SBSFC-081 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "SBSFC-081 opcode mismatch");
  Require(artifacts.envelope.engine_api_function == "EnginePlanOperation",
          "SBSFC-081 engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-081 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-081 parser no-finality authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          "SBSFC-081 cluster provider exclusion authority missing");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-081 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.route_kind),
          "SBSFC-081 payload missing route kind");
  Require(Contains(artifacts.envelope.payload, row.descriptor_role),
          "SBSFC-081 payload missing descriptor role");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-081 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-081 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "exact_refusal") &&
              !Contains(artifacts.envelope.payload, "cluster_support_not_enabled"),
          "SBSFC-081 payload used replay, refusal, or cluster-provider error evidence");
  Require(Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":false") &&
              Contains(artifacts.envelope.payload, "\"private_cluster_execution\":false"),
          "SBSFC-081 payload did not prove no cluster/private dispatch");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "recovery_authority\":true"),
          "SBSFC-081 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "SBSFC-081 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-081 server admission did not require public ABI dispatch");
  Require(admission.operation_id == "query.plan_operation",
          "SBSFC-081 server admission operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "SBSFC-081 server admission operation family mismatch");

  const auto* opcode = sblr::LookupSblrOperation("query.plan_operation");
  Require(opcode != nullptr, "SBSFC-081 opcode registry row missing");
  Require(opcode->opcode == "SBLR_QUERY_PLAN_OPERATION", "SBSFC-081 opcode registry drifted");
  Require(opcode->requires_cluster_authority == false,
          "SBSFC-081 opcode claimed cluster authority");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-081-descriptor-expression-residual";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000081201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000081202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000081203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000081204";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000081205";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000081206";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.current_sqlstate = "00000";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000081207";
  context.trace_tags.push_back("sbsfc081.descriptor_expression");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.sbsfc081.descriptor_expression");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "sbsfc081_descriptor_expression_surface"});
  envelope.operands.push_back({"text", "sbsfc081_surface_id", std::string(row.surface_id)});
  envelope.operands.push_back({"text", "sbsfc081_runtime_evidence_kind", "sbsfc081_descriptor_expression_route"});
  envelope.operands.push_back({"text", "sbsfc081_runtime_evidence_id", std::string(row.canonical_name)});
  envelope.operands.push_back({"text", "sbsfc081_descriptor_role", std::string(row.descriptor_role)});
  envelope.operands.push_back({"text", "query_operation", "descriptor_validation"});
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "sbsfc081_descriptor_expression_surface";
  request.option_envelopes.push_back(std::string("sbsfc081_surface_id:") + std::string(row.surface_id));
  request.option_envelopes.push_back("sbsfc081_runtime_evidence_kind:sbsfc081_descriptor_expression_route");
  request.option_envelopes.push_back(std::string("sbsfc081_runtime_evidence_id:") + std::string(row.canonical_name));
  request.option_envelopes.push_back(std::string("sbsfc081_descriptor_role:") + std::string(row.descriptor_role));
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
  Require(result.envelope_validated, "SBSFC-081 engine envelope rejected");
  Require(result.accepted, "SBSFC-081 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-081 engine did not dispatch to API");
  Require(result.api_result.operation_id == "query.plan_operation",
          "SBSFC-081 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-081 runtime API did not complete");
  Require(HasEvidence(result.api_result, "sbsfc081_descriptor_expression_route", row.canonical_name),
          "SBSFC-081 runtime evidence missing");
  Require(HasEvidence(result.api_result, "sbsfc081_surface", row.surface_id),
          "SBSFC-081 runtime did not carry row surface evidence");
  Require(HasEvidence(result.api_result, "sbsfc081_descriptor_role", row.descriptor_role),
          "SBSFC-081 runtime did not carry descriptor-role evidence");
  Require(HasEvidence(result.api_result, "parser_executes_sql", "false"),
          "SBSFC-081 runtime allowed parser SQL execution");
  Require(HasEvidence(result.api_result, "cluster_provider_dispatch", "false"),
          "SBSFC-081 runtime claimed cluster provider dispatch");
  Require(HasEvidence(result.api_result, "private_cluster_execution", "false"),
          "SBSFC-081 runtime claimed private cluster execution");
  Require(HasEvidence(result.api_result, "wal_recovery_authority", "false"),
          "SBSFC-081 runtime carried WAL/recovery authority");
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

  std::cout << "sbsql_sbsfc_081_descriptor_expression_residual_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
