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

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kTableUuid = "019f0000-0000-7000-8000-000000065001";

struct GrammarRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view canonical_sblr_operation_family;
};

constexpr GrammarRow kRows[] = {
    {"SBSQL-0100E654C08A", "pivot_clause", "sblr.general.operation.v3"},
    {"SBSQL-04A5A902018B", "pivot_agg_list", "sblr.general.operation.v3"},
    {"SBSQL-272C2607CD1A", "unpivot_value_columns", "sblr.general.operation.v3"},
    {"SBSQL-2FC490557319", "pivot_for_columns", "sblr.general.operation.v3"},
    {"SBSQL-49FFC54C3042", "unpivot_in_item", "sblr.general.operation.v3"},
    {"SBSQL-6EA06E502ED7", "pivot_in_item", "sblr.general.operation.v3"},
    {"SBSQL-816B796B486F", "pivot_in_list", "sblr.general.operation.v3"},
    {"SBSQL-8AB61EB176C5", "unpivot_pivot_column", "sblr.general.operation.v3"},
    {"SBSQL-D07E07A7470C", "unpivot_clause", "sblr.general.operation.v3"},
    {"SBSQL-E6995A4824AD", "unpivot_in_list", "sblr.general.operation.v3"},
    {"SBSQL-E8446176569C", "pivot_agg", "sblr.general.operation.v3"},
};

struct RouteCase {
  std::string_view sql;
  std::vector<std::string_view> surface_ids;
  std::vector<std::string_view> payload_markers;
  std::vector<std::string_view> forbidden_payload_markers;
};

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
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

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field,
                       std::size_t row_index = 0) {
  if (row_index >= result.result_shape.rows.size()) return {};
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) return value.encoded_value;
  }
  return {};
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << "  " << field.name << '=' << field.value << '\n';
    }
  }
}

void PrintApiDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000065101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000065102";
  session.database_uuid = "019f0000-0000-7000-8000-000000065103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 565;
  session.security_policy_epoch = 566;
  session.descriptor_epoch = 567;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_065_pivot_unpivot_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000065104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-065-pivot-unpivot-route-test";
  config.build_id = "sbsql-sbsfc-065-pivot-unpivot-route-test";
  return config;
}

PipelineArtifacts RunPipeline(const RouteCase& test_case) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(test_case.sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(kTableUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-065 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-065 generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "SBSFC-065 generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-065 generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-065 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.canonical_sblr_operation_family,
            "SBSFC-065 generated registry SBLR family drifted");
  }
}

void RequireExactLowering(const RouteCase& test_case,
                          const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-065 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-065 AST failed");
  Require(artifacts.bound.bound, "SBSFC-065 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-065 verifier rejected exact route");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "SBSFC-065 operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "SBSFC-065 SBLR opcode mismatch");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "SBSFC-065 operation family mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-065 no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-065 no-storage-finality authority missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-065 lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-065 payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"object_name_text_included\":false"),
          "SBSFC-065 payload did not prove no object-name text authority");
  Require(!Contains(artifacts.envelope.payload, test_case.sql),
          "SBSFC-065 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-065 payload used forbidden replay/refusal evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-065 payload carried WAL/recovery authority");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string("SBSFC-065 payload missing row marker ") +
                std::string(surface_id));
  }
  for (const auto marker : test_case.payload_markers) {
    Require(Contains(artifacts.envelope.payload, marker),
            std::string("SBSFC-065 payload missing marker ") + std::string(marker));
  }
  for (const auto marker : test_case.forbidden_payload_markers) {
    Require(!Contains(artifacts.envelope.payload, marker),
            std::string("SBSFC-065 payload embedded forbidden marker ") +
                std::string(marker));
  }
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected SBSFC-065 exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SBSFC-065");
  Require(admission.operation_id == "query.plan_operation",
          "server admission SBSFC-065 operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-065-pivot-unpivot-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000065201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000065202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000065203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000065204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000065205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000065206";
  context.local_transaction_id = 65;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:READ");
  for (const auto& row : kRows) {
    context.trace_tags.push_back(std::string("sbsql_surface_id:") +
                                 std::string(row.surface_id));
  }
  return context;
}

void AddTextOperand(sblr::SblrOperationEnvelope* envelope,
                    std::string name,
                    std::string value) {
  envelope->operands.push_back({"text", std::move(name), std::move(value)});
}

sblr::SblrOperationEnvelope EngineEnvelope(std::string_view trace_key) {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         std::string(trace_key));
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

api::EngineTypedValue Int64Value(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int64";
  typed.descriptor.encoded_descriptor = "type=int64";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue PivotRow(std::string row_uuid,
                             std::string region,
                             std::string quarter,
                             std::string amount) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"region", TextValue(std::move(region))});
  row.fields.push_back({"quarter", TextValue(std::move(quarter))});
  row.fields.push_back({"amount", Int64Value(std::move(amount))});
  return row;
}

api::EngineRowValue UnpivotRow(std::string row_uuid,
                               std::string region,
                               std::string q1,
                               std::string q2) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"region", TextValue(std::move(region))});
  row.fields.push_back({"q1", Int64Value(std::move(q1))});
  row.fields.push_back({"q2", Int64Value(std::move(q2))});
  return row;
}

void RequirePivotPlanDispatch() {
  auto envelope = EngineEnvelope("trace.sbsfc065.pivot.plan");
  AddTextOperand(&envelope, "query_operation", "pivot");
  AddTextOperand(&envelope, "execute", "true");
  AddTextOperand(&envelope, "pivot_group_field", "region");
  AddTextOperand(&envelope, "pivot_for_field", "quarter");
  AddTextOperand(&envelope, "pivot_value_field", "amount");
  AddTextOperand(&envelope, "pivot_aggregate_function", "sum");
  AddTextOperand(&envelope, "pivot_in_values", "Q1,Q2");
  AddTextOperand(&envelope, "pivot_aggregate_list_count", "1");
  AddTextOperand(&envelope, "pivot_in_item_count", "2");

  api::EngineApiRequest api_request;
  api_request.rows.push_back(PivotRow("relation-0-row-019f0000-0000-7000-8000-000000065301",
                                      "north",
                                      "Q1",
                                      "10"));
  api_request.rows.push_back(PivotRow("relation-0-row-019f0000-0000-7000-8000-000000065302",
                                      "north",
                                      "Q2",
                                      "7"));
  api_request.rows.push_back(PivotRow("relation-0-row-019f0000-0000-7000-8000-000000065303",
                                      "south",
                                      "Q1",
                                      "3"));
  api_request.rows.push_back(PivotRow("relation-0-row-019f0000-0000-7000-8000-000000065304",
                                      "south",
                                      "Q2",
                                      "4"));

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, std::move(api_request)};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-065 pivot envelope invalid");
  Require(result.accepted, "SBSFC-065 pivot dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-065 pivot did not dispatch");
  if (!result.api_result.ok) PrintApiDiagnostics(result.api_result);
  Require(result.api_result.ok, "EnginePlanOperation pivot route failed");
  Require(result.api_result.result_shape.rows.size() == 2,
          "SBSFC-065 pivot row count mismatch");
  Require(HasEvidence(result.api_result, "query_pivot", "sum_by_for_values"),
          "SBSFC-065 pivot runtime evidence missing");
  Require(HasEvidence(result.api_result, "query_pivot_descriptor",
                      "engine_row_descriptor_pivot_route"),
          "SBSFC-065 pivot descriptor runtime evidence missing");
  Require(FieldValue(result.api_result, "c0", 0) == "north" &&
              FieldValue(result.api_result, "c1", 0) == "10" &&
              FieldValue(result.api_result, "c2", 0) == "7" &&
              FieldValue(result.api_result, "c0", 1) == "south" &&
              FieldValue(result.api_result, "c1", 1) == "3" &&
              FieldValue(result.api_result, "c2", 1) == "4",
          "SBSFC-065 pivot deterministic rowset mismatch");
}

void RequireUnpivotPlanDispatch() {
  auto envelope = EngineEnvelope("trace.sbsfc065.unpivot.plan");
  AddTextOperand(&envelope, "query_operation", "unpivot");
  AddTextOperand(&envelope, "execute", "true");
  AddTextOperand(&envelope, "unpivot_group_field", "region");
  AddTextOperand(&envelope, "unpivot_pivot_column_field", "quarter");
  AddTextOperand(&envelope, "unpivot_value_output_field", "amount");
  AddTextOperand(&envelope, "unpivot_value_fields", "q1,q2");
  AddTextOperand(&envelope, "unpivot_name_values", "Q1,Q2");
  AddTextOperand(&envelope, "unpivot_value_column_count", "1");
  AddTextOperand(&envelope, "unpivot_in_item_count", "2");

  api::EngineApiRequest api_request;
  api_request.rows.push_back(UnpivotRow("relation-0-row-019f0000-0000-7000-8000-000000065401",
                                        "north",
                                        "10",
                                        "7"));
  api_request.rows.push_back(UnpivotRow("relation-0-row-019f0000-0000-7000-8000-000000065402",
                                        "south",
                                        "3",
                                        "4"));

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, std::move(api_request)};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-065 unpivot envelope invalid");
  Require(result.accepted, "SBSFC-065 unpivot dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-065 unpivot did not dispatch");
  if (!result.api_result.ok) PrintApiDiagnostics(result.api_result);
  Require(result.api_result.ok, "EnginePlanOperation unpivot route failed");
  Require(result.api_result.result_shape.rows.size() == 4,
          "SBSFC-065 unpivot row count mismatch");
  Require(HasEvidence(result.api_result, "query_unpivot", "columns_to_rows"),
          "SBSFC-065 unpivot runtime evidence missing");
  Require(HasEvidence(result.api_result, "query_unpivot_descriptor",
                      "engine_row_descriptor_unpivot_route"),
          "SBSFC-065 unpivot descriptor runtime evidence missing");
  Require(FieldValue(result.api_result, "c0", 0) == "north" &&
              FieldValue(result.api_result, "c1", 0) == "Q1" &&
              FieldValue(result.api_result, "c2", 0) == "10" &&
              FieldValue(result.api_result, "c0", 1) == "north" &&
              FieldValue(result.api_result, "c1", 1) == "Q2" &&
              FieldValue(result.api_result, "c2", 1) == "7" &&
              FieldValue(result.api_result, "c0", 2) == "south" &&
              FieldValue(result.api_result, "c1", 2) == "Q1" &&
              FieldValue(result.api_result, "c2", 2) == "3" &&
              FieldValue(result.api_result, "c0", 3) == "south" &&
              FieldValue(result.api_result, "c1", 3) == "Q2" &&
              FieldValue(result.api_result, "c2", 3) == "4",
          "SBSFC-065 unpivot deterministic rowset mismatch");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const std::vector<RouteCase> cases = {
      {"SELECT * FROM sales PIVOT (SUM(amount) FOR quarter IN ('Q1' AS q1, 'Q2' AS q2))",
       {"SBSQL-0100E654C08A",
        "SBSQL-04A5A902018B",
        "SBSQL-E8446176569C",
        "SBSQL-2FC490557319",
        "SBSQL-816B796B486F",
        "SBSQL-6EA06E502ED7"},
       {"\"query_envelope_kind\":\"table_pivot\"",
        "\"query_operation\":\"pivot\"",
        "\"pivot_clause_present\":true",
        "\"pivot_aggregate_list_count\":1",
        "\"pivot_aggregate_function\":\"sb.aggregate.sum\"",
        "\"pivot_aggregate_value_field\":\"amount\"",
        "\"pivot_for_column_field\":\"quarter\"",
        "\"pivot_for_column_count\":1",
        "\"pivot_in_item_count\":2",
        "\"pivot_binding_model\":\"engine_row_descriptor_pivot_route\"",
        "sys.query.pivot_descriptor"},
       {"sales"}},
      {"SELECT * FROM sales UNPIVOT (amount FOR quarter IN (q1 AS 'Q1', q2 AS 'Q2'))",
       {"SBSQL-D07E07A7470C",
        "SBSQL-272C2607CD1A",
        "SBSQL-8AB61EB176C5",
        "SBSQL-E6995A4824AD",
        "SBSQL-49FFC54C3042"},
       {"\"query_envelope_kind\":\"table_unpivot\"",
        "\"query_operation\":\"unpivot\"",
        "\"unpivot_clause_present\":true",
        "\"unpivot_value_column_count\":1",
        "\"unpivot_value_field\":\"amount\"",
        "\"unpivot_pivot_column_field\":\"quarter\"",
        "\"unpivot_in_item_count\":2",
        "\"unpivot_binding_model\":\"engine_row_descriptor_unpivot_route\"",
        "sys.query.unpivot_descriptor"},
       {"sales"}},
  };

  for (const auto& test_case : cases) {
    const auto artifacts = RunPipeline(test_case);
    RequireExactLowering(test_case, artifacts);
    RequireServerAdmission(artifacts.envelope);
  }
  RequirePivotPlanDispatch();
  RequireUnpivotPlanDispatch();
  std::cout << "sbsql_sbsfc_065_pivot_unpivot_grammar_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
