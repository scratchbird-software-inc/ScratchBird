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
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

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

struct ValuesSurfaceEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view family;
  std::string_view sblr_operation_family;
  std::string_view parser_handler_key;
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
  std::string_view validation_fixture_id;
};

constexpr ValuesSurfaceEvidence kValuesStmtRow{
    "SBSQL-AE9DF01841E8",
    "values_stmt",
    "grammar_production",
    "general",
    "sblr.query.values.v3",
    "parser.grammar_ast",
    "lowering.sblr_family.sblr_query_values_v3",
    "server.admission.sblr_query_values_v3",
    "engine.rule.sblr_query_values_v3",
    "SBSQL-SURFACE-0A17BF28BDCF"};

constexpr ValuesSurfaceEvidence kSetOpRow{
    "SBSQL-CD51FAC537C3",
    "set_op",
    "grammar_production",
    "general",
    "sblr.general.operation.v3",
    "parser.grammar_ast",
    "lowering.sblr_family.sblr_general_operation_v3",
    "server.admission.sblr_general_operation_v3",
    "engine.rule.sblr_general_operation_v3",
    "SBSQL-SURFACE-5FE0FC6AEB6D"};

std::string EvidenceMessage(const ValuesSurfaceEvidence& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string EvidenceMessage(std::string_view phase, std::string_view message) {
  return EvidenceMessage(kValuesStmtRow, phase, message);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000003211";
  session.connection_uuid = "019f0000-0000-7000-8000-000000003212";
  session.database_uuid = "019f0000-0000-7000-8000-000000003213";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000003214";
  config.bundle_contract_id = "sbp_sbsql@values-rowset-route-test";
  config.build_id = "sbsql-values-rowset-route-test";
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
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireGeneratedRegistryEvidence(const ValuesSurfaceEvidence& evidence) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(evidence.surface_id);
  Require(registry_row != nullptr,
          EvidenceMessage(evidence, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == evidence.canonical_name,
          EvidenceMessage(evidence, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == evidence.surface_kind,
          EvidenceMessage(evidence, "registry", "surface kind mismatch"));
  Require(registry_row->family == evidence.family,
          EvidenceMessage(evidence, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          EvidenceMessage(evidence, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(evidence, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == evidence.sblr_operation_family,
          EvidenceMessage(evidence, "registry", "SBLR operation family mismatch"));
  Require(registry_row->parser_handler_key == evidence.parser_handler_key,
          EvidenceMessage(evidence, "parser_bind_lower", "parser handler key mismatch"));
  Require(registry_row->lowering_handler_key == evidence.lowering_handler_key,
          EvidenceMessage(evidence, "parser_bind_lower", "lowering handler key mismatch"));
  Require(registry_row->server_admission_key == evidence.server_admission_key,
          EvidenceMessage(evidence, "server_admission", "server admission key mismatch"));
  Require(registry_row->engine_rule_key == evidence.engine_rule_key,
          EvidenceMessage(evidence, "engine_dispatch", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == evidence.validation_fixture_id,
          EvidenceMessage(evidence, "registry", "validation fixture id mismatch"));
}

void RequireRegistryEvidence() {
  RequireGeneratedRegistryEvidence(kValuesStmtRow);
  RequireGeneratedRegistryEvidence(kSetOpRow);
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-values-rowset";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000003221";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000003222";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000003223";
  context.security_context_present = true;
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.query.values_rowset");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "execute", "true"});
  envelope.operands.push_back({"text", "query_operation", "values"});
  envelope.operands.push_back({"row_field:bigint", "values-row-0|c0", "1"});
  envelope.operands.push_back({"row_field:text", "values-row-0|c1", "two"});
  envelope.operands.push_back({"row_field:bigint", "values-row-1|c0", "3"});
  envelope.operands.push_back({"row_null_field:null", "values-row-1|c1", ""});
  return envelope;
}

sblr::SblrOperationEnvelope SetOperationEngineEnvelope(
    std::string operation,
    std::vector<std::string> left_values = {"1", "2"},
    std::vector<std::string> right_values = {"2", "3"}) {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.query.values_set_operation");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "execute", "true"});
  envelope.operands.push_back({"text", "query_operation", operation});
  envelope.operands.push_back({"text", "set_operation", operation});
  for (std::size_t index = 0; index < left_values.size(); ++index) {
    envelope.operands.push_back({"row_field:bigint",
                                 "relation-0-row-" + std::to_string(index) + "|c0",
                                 left_values[index]});
  }
  for (std::size_t index = 0; index < right_values.size(); ++index) {
    envelope.operands.push_back({"row_field:bigint",
                                 "relation-1-row-" + std::to_string(index) + "|c0",
                                 right_values[index]});
  }
  return envelope;
}

void RequireValuesLowering() {
  const auto artifacts = RunPipeline("VALUES (1, 'two'), (3, NULL)");
  Require(artifacts.bound.bound, "VALUES statement did not bind");
  Require(artifacts.ast.statement_surface_id == kValuesStmtRow.surface_id,
          EvidenceMessage("parser", "AST surface id mismatch"));
  Require(artifacts.ast.statement_surface_name == kValuesStmtRow.canonical_name,
          EvidenceMessage("parser", "AST canonical name mismatch"));
  Require(artifacts.bound.statement_surface_id == kValuesStmtRow.surface_id,
          EvidenceMessage("binder", "bound surface id mismatch"));
  Require(artifacts.bound.statement_surface_name == kValuesStmtRow.canonical_name,
          EvidenceMessage("binder", "bound canonical name mismatch"));
  Require(artifacts.bound.surface_key == kValuesStmtRow.surface_id,
          EvidenceMessage("binder", "bound surface key mismatch"));
  Require(artifacts.verifier.admitted, "VALUES SBLR verifier rejected exact route");
  Require(artifacts.envelope.surface_key == kValuesStmtRow.surface_id,
          EvidenceMessage("lowering", "envelope surface key mismatch"));
  Require(artifacts.envelope.operation_family == "sblr.query.values.v3",
          "VALUES operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.values.v3",
          "VALUES SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "VALUES operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "query.plan_operation",
          "VALUES engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "VALUES opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          "engine query plan authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_security_authorization"),
          "parser no-security-authorization authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "parser no-storage/finality authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "parser no-SQL-execution authority step missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.query.values_rowset_descriptor"),
          "VALUES rowset descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"values_rowset\""),
          "VALUES payload marker missing");
  Require(Contains(artifacts.envelope.payload, "\"values_row_count\":\"2\""),
          "VALUES row count missing");
  Require(Contains(artifacts.envelope.payload, "\"values_column_count\":\"2\""),
          "VALUES column count missing");
  Require(Contains(artifacts.envelope.payload, "\"values_0_0_value\":\"1\""),
          "VALUES first value missing");
  Require(Contains(artifacts.envelope.payload, "\"values_0_1_value\":\"two\""),
          "VALUES text literal missing");
  Require(Contains(artifacts.envelope.payload, "\"values_1_1_is_null\":\"true\""),
          "VALUES NULL marker missing");
  Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":false"),
          "VALUES claimed a source relation");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "VALUES claimed row storage");
  Require(!Contains(artifacts.envelope.payload, "VALUES (1"),
          "VALUES payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          "VALUES payload embedded source_text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected VALUES route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require engine public ABI dispatch for VALUES");
  Require(admission.operation_id == "query.plan_operation",
          "server admission operation id mismatch");
  Require(admission.operation_family == "sblr.query.values.v3",
          "server admission operation family mismatch");
}

void RequireMalformedValuesFailClosed() {
  const auto artifacts = RunPipeline("VALUES (1), (2, 3)");
  Require(!artifacts.bound.bound || artifacts.envelope.messages.has_errors() ||
              artifacts.verifier.messages.has_errors(),
          "ragged VALUES rowset did not fail closed");
}

void RequireValuesSetOperationLowering() {
  struct Case {
    std::string_view sql;
    std::string_view operation;
  };
  constexpr Case kCases[] = {
      {"VALUES (1), (2) UNION VALUES (2), (3)", "union_distinct"},
      {"VALUES (1), (2) UNION ALL VALUES (2), (3)", "union_all"},
      {"VALUES (1), (2) INTERSECT ALL VALUES (2), (2)", "intersect_all"},
      {"VALUES (1), (2) EXCEPT ALL VALUES (2), (2)", "except_all"}};
  for (const auto& test : kCases) {
    const auto artifacts = RunPipeline(test.sql);
    Require(artifacts.bound.bound,
            EvidenceMessage(kSetOpRow, "parser_bind_lower", "VALUES set operation did not bind"));
    Require(artifacts.verifier.admitted,
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation SBLR verifier rejected exact route"));
    Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
            EvidenceMessage(kSetOpRow, "lowering", "VALUES set operation route family mismatch"));
    Require(artifacts.envelope.sblr_operation_key == "sblr.query.relational.v3",
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation route SBLR operation key mismatch"));
    Require(artifacts.envelope.operation_id == "query.plan_operation",
            EvidenceMessage(kSetOpRow, "lowering", "VALUES set operation id mismatch"));
    Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            EvidenceMessage(kSetOpRow, "lowering", "VALUES set operation opcode mismatch"));
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.engine.query_plan_api_required"),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation query plan authority step missing"));
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.parser.no_storage_or_finality"),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation parser storage/finality refusal missing"));
    Require(HasValue(artifacts.envelope.descriptor_refs,
                     "sys.query.values_set_operation_descriptor"),
            EvidenceMessage(kSetOpRow, "lowering", "VALUES set operation descriptor ref missing"));
    Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"values_set_operation\""),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation payload marker missing"));
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"set_operation\":\"") + std::string(test.operation) + "\""),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation payload missing expected set operation"));
    Require(Contains(artifacts.envelope.payload, "\"relation_count\":\"2\""),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation payload missing relation count"));
    Require(Contains(artifacts.envelope.payload, "\"relation_0_row_count\":\"2\""),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation payload missing left relation row count"));
    Require(Contains(artifacts.envelope.payload, "\"relation_1_row_count\":\"2\""),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation payload missing right relation row count"));
    Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":false"),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation claimed source relation"));
    Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":false"),
            EvidenceMessage(kSetOpRow, "lowering", "VALUES set operation claimed row storage"));
    Require(!Contains(artifacts.envelope.payload, "VALUES (1)"),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation payload embedded source SQL text"));
    Require(!Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"table_set_operation\""),
            EvidenceMessage(kSetOpRow,
                            "lowering",
                            "VALUES set operation claimed table-backed set query route"));

    const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    Require(admission.admitted,
            EvidenceMessage(kSetOpRow,
                            "server_admission",
                            "server admission rejected VALUES set operation route"));
    Require(admission.requires_public_abi_dispatch,
            EvidenceMessage(kSetOpRow,
                            "server_admission",
                            "server admission did not require public ABI dispatch for VALUES set operation"));
    Require(admission.operation_id == "query.plan_operation",
            EvidenceMessage(kSetOpRow,
                            "server_admission",
                            "server admission VALUES set operation id mismatch"));
    Require(admission.operation_family == "sblr.query.relational.v3",
            EvidenceMessage(kSetOpRow,
                            "server_admission",
                            "server admission VALUES set operation route family mismatch"));
  }
}

void RequireUnsupportedValuesSetOperationsFailClosed() {
  for (const auto sql : {"VALUES ('a') UNION VALUES ('b')",
                         "VALUES (1) UNION BY NAME VALUES (1)",
                         "VALUES (1) UNION ALL DISTINCT VALUES (1)"}) {
    const auto artifacts = RunPipeline(sql);
    Require(!artifacts.bound.bound || artifacts.envelope.messages.has_errors() ||
                artifacts.verifier.messages.has_errors(),
            "unsupported VALUES set operation did not fail closed");
  }
}

void RequireEngineDispatch() {
  const sblr::SblrDispatchRequest request{EngineContext(), EngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept VALUES rowset");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route VALUES to query plan API");
  Require(result.api_result.ok, "engine VALUES rowset API failed");
  Require(result.api_result.operation_id == "query.plan_operation",
          "engine VALUES operation id mismatch");
  Require(result.api_result.result_shape.result_kind == "values_rowset",
          "engine VALUES result kind mismatch");
  Require(result.api_result.result_shape.rows.size() == 2,
          "engine VALUES row count mismatch");
  Require(result.api_result.result_shape.rows[0].fields.size() == 2,
          "engine VALUES first row width mismatch");
  Require(result.api_result.result_shape.rows[0].fields[0].second.encoded_value == "1",
          "engine VALUES first field mismatch");
  Require(result.api_result.result_shape.rows[0].fields[1].second.encoded_value == "two",
          "engine VALUES text field mismatch");
  Require(result.api_result.result_shape.rows[1].fields[1].second.is_null,
          "engine VALUES NULL field mismatch");
}

void RequireSetOperationEngineDispatch(std::string operation,
                                       std::vector<std::string> expected_values,
                                       std::vector<std::string> left_values = {"1", "2"},
                                       std::vector<std::string> right_values = {"2", "3"}) {
  const sblr::SblrDispatchRequest request{
      EngineContext(),
      SetOperationEngineEnvelope(std::move(operation), left_values, right_values),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          EvidenceMessage(kSetOpRow,
                          "engine_dispatch",
                          "engine SBLR envelope did not validate VALUES set operation"));
  Require(result.accepted,
          EvidenceMessage(kSetOpRow,
                          "engine_dispatch",
                          "engine SBLR dispatch did not accept VALUES set operation"));
  Require(result.dispatched_to_api,
          EvidenceMessage(kSetOpRow,
                          "engine_dispatch",
                          "engine SBLR dispatch did not route VALUES set operation"));
  Require(result.api_result.ok,
          EvidenceMessage(kSetOpRow, "engine_dispatch", "engine VALUES set operation API failed"));
  Require(result.api_result.result_shape.result_kind == "query_rowset",
          EvidenceMessage(kSetOpRow,
                          "engine_dispatch",
                          "engine VALUES set operation result kind mismatch"));
  Require(result.api_result.result_shape.rows.size() == expected_values.size(),
          EvidenceMessage(kSetOpRow,
                          "engine_dispatch",
                          "engine VALUES set operation row count mismatch"));
  for (std::size_t index = 0; index < expected_values.size(); ++index) {
    Require(!result.api_result.result_shape.rows[index].fields.empty(),
            EvidenceMessage(kSetOpRow,
                            "engine_dispatch",
                            "engine VALUES set operation row missing fields"));
    Require(result.api_result.result_shape.rows[index].fields.front().second.encoded_value ==
                expected_values[index],
            EvidenceMessage(kSetOpRow,
                            "engine_dispatch",
                            "engine VALUES set operation row value mismatch"));
  }
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  RequireValuesLowering();
  RequireMalformedValuesFailClosed();
  RequireValuesSetOperationLowering();
  RequireUnsupportedValuesSetOperationsFailClosed();
  RequireEngineDispatch();
  RequireSetOperationEngineDispatch("union_distinct", {"1", "2", "3"});
  RequireSetOperationEngineDispatch("intersect", {"2"});
  RequireSetOperationEngineDispatch("except", {"1"});
  RequireSetOperationEngineDispatch("union_all",
                                    {"1", "2", "2", "4", "2", "2", "3"},
                                    {"1", "2", "2", "4"},
                                    {"2", "2", "3"});
  RequireSetOperationEngineDispatch("intersect_all",
                                    {"2", "2"},
                                    {"1", "2", "2", "4"},
                                    {"2", "2", "3"});
  RequireSetOperationEngineDispatch("except_all",
                                    {"1", "4"},
                                    {"1", "2", "2", "4"},
                                    {"2", "2", "3"});
  std::cout << "sbsql_values_rowset_conformance=passed\n";
  return EXIT_SUCCESS;
}
