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
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kTableUuid = "019f0000-0000-7000-8000-000000063001";
constexpr std::string_view kTimeSeriesUuid = "019f0000-0000-7000-8000-000000063002";

struct GrammarRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view canonical_sblr_operation_family;
};

constexpr GrammarRow kRows[] = {
    {"SBSQL-0AC427EBCC12", "window_def", "sblr.general.operation.v3"},
    {"SBSQL-0C21F22A9420", "time_series_window_expr", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-1BD209DEB737", "window_clause", "sblr.general.operation.v3"},
    {"SBSQL-3AF5139D4E9A", "existing_window_name", "sblr.general.operation.v3"},
    {"SBSQL-541127FA7A87", "window_exclude_clause", "sblr.general.operation.v3"},
    {"SBSQL-777A3A7148F0", "window_frame_clause", "sblr.general.operation.v3"},
    {"SBSQL-D2633118D544", "window_exclude_target", "sblr.general.operation.v3"},
};

struct WindowCase {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view operation_family;
  std::vector<std::string_view> surface_ids;
  std::vector<std::string> resolved_uuids;
  std::vector<std::string_view> payload_markers;
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

void PrintApiDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
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

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000063101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000063102";
  session.database_uuid = "019f0000-0000-7000-8000-000000063103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 363;
  session.security_policy_epoch = 364;
  session.descriptor_epoch = 365;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_063_window_grammar_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000063104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-063-window-grammar-route-test";
  config.build_id = "sbsql-sbsfc-063-window-grammar-route-test";
  return config;
}

PipelineArtifacts RunPipeline(const WindowCase& test_case) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(test_case.sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            test_case.resolved_uuids);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-063 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-063 generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "SBSFC-063 generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-063 generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-063 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.canonical_sblr_operation_family,
            "SBSFC-063 generated registry SBLR family drifted");
  }
}

void RequireExactLowering(const WindowCase& test_case,
                          const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) {
    std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  }
  if (artifacts.ast.messages.has_errors()) {
    std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  }
  if (!artifacts.bound.bound) {
    std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  }
  if (!artifacts.verifier.admitted) {
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-063 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-063 AST failed");
  Require(artifacts.bound.bound, "SBSFC-063 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-063 verifier rejected exact route");
  Require(artifacts.envelope.operation_id == test_case.operation_id,
          "SBSFC-063 operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == test_case.opcode,
          "SBSFC-063 SBLR opcode mismatch");
  Require(artifacts.envelope.operation_family == test_case.operation_family,
          "SBSFC-063 operation family mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-063 no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-063 no-storage-finality authority missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-063 lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-063 payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, test_case.sql),
          "SBSFC-063 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-063 payload used forbidden replay/refusal evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-063 payload carried WAL/recovery authority");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string("SBSFC-063 payload missing row marker ") +
                std::string(surface_id));
  }
  for (const auto marker : test_case.payload_markers) {
    Require(Contains(artifacts.envelope.payload, marker),
            std::string("SBSFC-063 payload missing marker ") + std::string(marker));
  }
}

void RequireServerAdmission(const WindowCase& test_case,
                            const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected SBSFC-063 exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SBSFC-063");
  Require(admission.operation_id == test_case.operation_id,
          "server admission SBSFC-063 operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-063-window-grammar-exact-route";
  context.database_path =
      (std::filesystem::temp_directory_path() /
       "sbsql_sbsfc_063_window_grammar_exact_route.sbdb").string();
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000063201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000063202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000063203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000063204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000063205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000063206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000063207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000063208";
  context.local_transaction_id = 63;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:WINDOW_SURFACE_TEST");
  for (const auto& row : kRows) {
    context.trace_tags.push_back(std::string("sbsql_surface_id:") +
                                 std::string(row.surface_id));
  }
  return context;
}

void RemoveTimeSeriesApiArtifacts() {
  std::error_code ignored;
  const auto path = std::filesystem::temp_directory_path() /
                    "sbsql_sbsfc_063_window_grammar_exact_route.sbdb";
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.api_events", ignored);
}

api::EngineTypedValue Int64Value(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int64";
  typed.descriptor.encoded_descriptor = "type=int64";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string row_uuid, std::string id) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", Int64Value(std::move(id))});
  return row;
}

sblr::SblrOperationEnvelope EngineEnvelope(std::string_view operation_id,
                                           std::string_view opcode,
                                           std::string_view trace_key) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(operation_id),
                                         std::string(opcode),
                                         std::string(trace_key));
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireWindowPlanDispatch() {
  auto envelope = EngineEnvelope("query.plan_operation",
                                 "SBLR_QUERY_PLAN_OPERATION",
                                 "trace.sbsfc063.window.plan");
  envelope.operands.push_back({"text", "query_operation", "row_number_window"});
  envelope.operands.push_back({"text", "execute", "true"});
  envelope.operands.push_back({"text", "order_by", "id"});
  envelope.operands.push_back({"text", "order_column", "0"});
  envelope.operands.push_back({"text", "window_function", "row_number"});
  envelope.operands.push_back({"text", "window_frame_clause_present", "true"});
  envelope.operands.push_back({"text", "window_frame_mode", "rows"});
  envelope.operands.push_back({"text", "window_exclude_target", "no_others"});

  api::EngineApiRequest api_request;
  api_request.rows.push_back(Row("relation-0-row-019f0000-0000-7000-8000-000000063301", "2"));
  api_request.rows.push_back(Row("relation-0-row-019f0000-0000-7000-8000-000000063302", "1"));
  api_request.rows.push_back(Row("relation-0-row-019f0000-0000-7000-8000-000000063303", "3"));

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, std::move(api_request)};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-063 window plan envelope invalid");
  Require(result.accepted, "SBSFC-063 window plan dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-063 window plan did not dispatch");
  if (!result.api_result.ok) PrintApiDiagnostics(result.api_result);
  Require(result.api_result.ok, "EnginePlanOperation window route failed");
  Require(result.api_result.result_shape.rows.size() == 3,
          "EnginePlanOperation window row count mismatch");
  Require(HasEvidence(result.api_result, "query_window", "row_number"),
          "SBSFC-063 window runtime evidence missing");
  Require(HasEvidence(result.api_result, "query_window_binding", "descriptor_field"),
          "SBSFC-063 window descriptor binding evidence missing");
  Require(FieldValue(result.api_result, "c1", 0) == "1" &&
              FieldValue(result.api_result, "c1", 1) == "2" &&
              FieldValue(result.api_result, "c1", 2) == "3",
          "EnginePlanOperation row_number window ordinals mismatch");
}

void RequireTimeSeriesProjectionDispatch() {
  auto envelope = EngineEnvelope("query.evaluate_projection",
                                 "SBLR_QUERY_EVALUATE_PROJECTION",
                                 "trace.sbsfc063.timeseries.window.expr");
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "ts_window"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_0_type", "real64"});
  envelope.operands.push_back({"text", "projection_0_function_id", "timeseries.aggregate"});
  envelope.operands.push_back({"text", "projection_0_function_arg_count", "2"});
  envelope.operands.push_back({"text", "projection_0_arg_0_type", "text"});
  envelope.operands.push_back({"text", "projection_0_arg_0_value", "sum"});
  envelope.operands.push_back({"text", "projection_0_arg_1_type", "text"});
  envelope.operands.push_back({"text", "projection_0_arg_1_value", "1,2,3"});
  envelope.operands.push_back({"text", "projection_0_time_series_window_expr_present", "true"});
  envelope.operands.push_back({"text", "projection_0_time_series_window_interval_value", "1 day"});

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-063 time-series projection envelope invalid");
  Require(result.accepted, "SBSFC-063 time-series projection dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-063 time-series projection did not dispatch");
  if (!result.api_result.ok) PrintApiDiagnostics(result.api_result);
  Require(result.api_result.ok, "EngineEvaluateProjection time-series route failed");
  Require(HasEvidence(result.api_result, "function_runtime", "timeseries.aggregate"),
          "SBSFC-063 time-series function runtime evidence missing");
  Require(result.api_result.result_shape.rows.size() == 1,
          "SBSFC-063 time-series projection row count mismatch");
}

void RequireTimeSeriesAppendDispatch() {
  RemoveTimeSeriesApiArtifacts();
  auto envelope = EngineEnvelope("nosql.time_series_append",
                                 "SBLR_NOSQL_TIME_SERIES_APPEND",
                                 "trace.sbsfc063.timeseries.append");
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTimeSeriesUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "time_series"});

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-063 time-series append envelope invalid");
  Require(result.accepted, "SBSFC-063 time-series append dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-063 time-series append did not dispatch");
  if (!result.api_result.ok) PrintApiDiagnostics(result.api_result);
  Require(result.api_result.ok, "EngineTimeSeriesAppend route failed");
  Require(HasEvidence(result.api_result, "nosql_surface", "time_series"),
          "SBSFC-063 time-series surface evidence missing");
  Require(HasEvidence(result.api_result, "nosql_behavior", "persisted_time_series_append"),
          "SBSFC-063 time-series append behavior evidence missing");
  RemoveTimeSeriesApiArtifacts();
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const std::vector<WindowCase> cases = {
      {"SELECT row_number() OVER win FROM customer WINDOW win AS (ORDER BY id)",
       "query.plan_operation",
       "SBLR_QUERY_PLAN_OPERATION",
       "sblr.query.relational.v3",
       {"SBSQL-0AC427EBCC12", "SBSQL-1BD209DEB737", "SBSQL-3AF5139D4E9A"},
       {std::string(kTableUuid)},
       {"\"query_envelope_kind\":\"table_row_number_window\"",
        "\"window_named_reference_present\":true",
        "\"window_named_definition_present\":true",
        "\"window_name_text_included\":false",
        "\"window_name_ref_descriptor\":\"named_window_0\"",
        "\"window_definition_descriptor\":\"named_window_0\"",
        "\"order_by\":\"id\""}},
      {"SELECT row_number() OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE NO OTHERS) FROM customer",
       "query.plan_operation",
       "SBLR_QUERY_PLAN_OPERATION",
       "sblr.query.relational.v3",
       {"SBSQL-541127FA7A87", "SBSQL-777A3A7148F0", "SBSQL-D2633118D544"},
       {std::string(kTableUuid)},
       {"\"query_envelope_kind\":\"table_row_number_window\"",
        "\"window_frame_clause_present\":true",
        "\"window_frame_mode\":\"rows\"",
        "\"window_frame_start\":\"unbounded_preceding\"",
        "\"window_frame_end\":\"current_row\"",
        "\"window_exclude_clause_present\":true",
        "\"window_exclude_target\":\"no_others\""}},
      {"SELECT timeseries.aggregate('sum', '1,2,3') WITHIN INTERVAL '1' DAY AS ts_window",
       "query.evaluate_projection",
       "SBLR_QUERY_EVALUATE_PROJECTION",
       "sblr.query.relational.v3",
       {"SBSQL-0C21F22A9420"},
       {},
       {"\"query_envelope_kind\":\"scalar_projection\"",
        "\"projection_0_name\":\"ts_window\"",
        "\"projection_0_function_id\":\"timeseries.aggregate\"",
        "\"projection_0_time_series_window_expr_present\":true",
        "\"projection_0_time_series_window_interval_value\":\"1 day\""}},
  };

  for (const auto& test_case : cases) {
    const auto artifacts = RunPipeline(test_case);
    RequireExactLowering(test_case, artifacts);
    RequireServerAdmission(test_case, artifacts.envelope);
  }
  RequireWindowPlanDispatch();
  RequireTimeSeriesProjectionDispatch();
  RequireTimeSeriesAppendDispatch();
  std::cout << "sbsql_sbsfc_063_window_grammar_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
