// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "canonical_sblr_admission_test_helper.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "uuid.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;
namespace database = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kTableUuid = "019f0000-0000-7000-8000-000000063001";
constexpr std::string_view kTimeSeriesUuid = "019f0000-0000-7000-8000-000000063002";
constexpr std::string_view kFixturePrincipal = "qow_packet7_user";
constexpr std::string_view kFixturePassword =
    "QOW-Packet7-live-route-password";
constexpr std::string_view kFixtureCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=7b622f17d15a5e6f2d5122c606937dfc"
    "76f5982782adb52b03f7b1ca024f72c9";

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
  std::vector<std::string_view> payload_markers;
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

std::filesystem::path MakeFixtureDatabase() {
  static std::atomic<std::uint64_t> identity_time{1788203000000ULL};
  std::string template_path = "/tmp/sbsql_sbsfc_063_window.XXXXXX";
  std::vector<char> writable(template_path.begin(), template_path.end());
  writable.push_back('\0');
  char* directory = ::mkdtemp(writable.data());
  if (directory == nullptr) return {};

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, identity_time.fetch_add(2));
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, identity_time.fetch_add(2));
  if (!database_uuid.ok() || !filespace_uuid.ok()) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return {};
  }

  const auto path =
      std::filesystem::path(directory) / "sbsfc_063_window.sbdb";
  database::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = identity_time.fetch_add(2);
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.allow_minimal_resource_bootstrap = false;
  create.require_resource_seed_pack = true;
  create.bootstrap_principal_name = std::string(kFixturePrincipal);
  create.bootstrap_credential_fingerprint =
      std::string(kFixtureCredentialFingerprint);
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  const auto created = database::CreateDatabaseFile(create);
  if (!created.ok() ||
      created.create_finality != database::DatabaseCreateFinalityClass::committed) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key;
    for (const auto& argument : created.diagnostic.arguments) {
      std::cerr << ':' << argument.key << '=' << argument.value;
    }
    std::cerr << '\n';
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return {};
  }
  return path;
}

PipelineResult RunEnginePipelineForConformance(
    SbsqlTestWireSession* session,
    std::string_view sql,
    SbsqlPipelineConformanceSummary* summary) {
  return session->RunPipeline(sql, true, false, 0, false, {}, nullptr, false,
                              nullptr, {}, 0, nullptr, nullptr, summary);
}

void RequireStaticScalarProjectionLowering(const WindowCase& test_case) {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000063101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000063102";
  session.database_uuid = "019f0000-0000-7000-8000-000000063103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 363;
  session.security_policy_epoch = 364;
  session.descriptor_epoch = 365;

  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_063_window_grammar_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000063104";
  config.bundle_contract_id =
      "sbp_sbsql@sbsfc-063-window-grammar-route-test";
  config.build_id = "sbsql-sbsfc-063-window-grammar-route-test";

  const auto cst = BuildCst(std::string(test_case.sql));
  const auto ast = BuildAst(cst);
  const auto bound = BindAst(ast, cst, config, session, {});
  const auto envelope = LowerToSblr(bound, cst, session);
  const auto verifier = VerifySblrEnvelope(envelope);
  if (cst.messages.has_errors()) {
    std::cerr << RenderMessageVectorSet(cst.messages);
  }
  if (ast.messages.has_errors()) {
    std::cerr << RenderMessageVectorSet(ast.messages);
  }
  if (!bound.bound) {
    std::cerr << RenderMessageVectorSet(bound.messages);
  }
  if (!verifier.admitted) {
    std::cerr << RenderMessageVectorSet(verifier.messages);
  }
  Require(!cst.messages.has_errors(), "SBSFC-063 scalar CST failed");
  Require(!ast.messages.has_errors(), "SBSFC-063 scalar AST failed");
  Require(bound.bound, "SBSFC-063 scalar bind failed");
  Require(verifier.admitted,
          "SBSFC-063 scalar verifier rejected exact route");
  Require(envelope.operation_id == test_case.operation_id,
          "SBSFC-063 scalar operation id mismatch");
  Require(envelope.sblr_opcode == test_case.opcode,
          "SBSFC-063 scalar opcode mismatch");
  Require(envelope.operation_family == test_case.operation_family,
          "SBSFC-063 scalar operation family mismatch");
  Require(!envelope.parser_executes_sql,
          "SBSFC-063 scalar lowering allowed parser SQL execution");
  Require(!Contains(envelope.payload, test_case.sql),
          "SBSFC-063 scalar payload embedded source SQL text");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(envelope.payload, surface_id),
            "SBSFC-063 scalar payload omitted its surface identity");
  }
  for (const auto marker : test_case.payload_markers) {
    Require(Contains(envelope.payload, marker),
            "SBSFC-063 scalar payload omitted an exact marker");
  }

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(envelope));
  Require(admission.admitted,
          "server admission rejected SBSFC-063 scalar exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-063 scalar route did not require public ABI dispatch");
  Require(admission.operation_id == test_case.operation_id,
          "server admission SBSFC-063 scalar operation id mismatch");
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

void RequireExactLowering(
    const WindowCase& test_case,
    const PipelineResult& result,
    const SbsqlPipelineConformanceSummary& summary) {
  if (!result.accepted || result.messages.has_errors()) {
    std::cerr << "SBSFC-063 SQL: " << test_case.sql << '\n';
    std::cerr << RenderMessageVectorSet(result.messages);
  }
  Require(result.accepted && !result.messages.has_errors(),
          "SBSFC-063 live engine pipeline failed");
  Require(summary.captured, "SBSFC-063 final parser artifacts were not captured");
  Require(summary.bound && !summary.bound_has_errors,
          "SBSFC-063 did not bind under engine descriptor authority");
  Require(summary.verifier_admitted && !summary.verifier_has_errors,
          "SBSFC-063 verifier rejected the engine-bound route");
  Require(summary.payload_nonempty && !result.sblr_payload.empty(),
          "SBSFC-063 produced no canonical SBLR payload");
  Require(summary.operation_id == test_case.operation_id,
          "SBSFC-063 operation id mismatch");
  Require(summary.sblr_opcode == test_case.opcode,
          "SBSFC-063 SBLR opcode mismatch");
  Require(summary.operation_family == test_case.operation_family,
          "SBSFC-063 operation family mismatch");
  Require(result.server_operation_id == test_case.operation_id,
          "SBSFC-063 engine operation id mismatch");
  Require(summary.has_read_right && summary.has_syntax_authority,
          "SBSFC-063 read/syntax authority projection missing");
  Require(!result.parser_executes_sql,
          "SBSFC-063 lowering allowed parser SQL execution");
  Require(!Contains(result.sblr_payload, test_case.sql),
          "SBSFC-063 payload embedded source SQL text");
  Require(!Contains(result.sblr_payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(result.sblr_payload, "refusal"),
          "SBSFC-063 payload used forbidden replay/refusal evidence");
  Require(!Contains(result.sblr_payload, "WAL") &&
              !Contains(result.sblr_payload, "wal") &&
              !Contains(result.sblr_payload, "recovery"),
          "SBSFC-063 payload carried WAL/recovery authority");
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
  envelope = scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));

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
  envelope.operands.push_back({"text", "projection_0_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_0_type", "real64"});
  envelope.operands.push_back({"text", "projection_0_value", ""});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_function_id", "timeseries.aggregate"});
  envelope.operands.push_back({"text", "projection_0_function_arg_count", "2"});
  envelope.operands.push_back({"text", "projection_0_arg_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_0_type", "text"});
  envelope.operands.push_back({"text", "projection_0_arg_0_value", "sum"});
  envelope.operands.push_back({"text", "projection_0_arg_0_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_arg_1_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_1_type", "text"});
  envelope.operands.push_back({"text", "projection_0_arg_1_value", "1,2,3"});
  envelope.operands.push_back({"text", "projection_0_arg_1_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_time_series_window_expr_present", "true"});
  envelope.operands.push_back({"text", "projection_0_time_series_window_interval_value", "1 day"});
  envelope = scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));

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
  envelope = scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));

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
       "query.execute",
       "SBLR_QUERY_EXECUTE",
       "sblr.query.relational.v3",
       {"SBSQL-0AC427EBCC12", "SBSQL-1BD209DEB737", "SBSQL-3AF5139D4E9A"},
       {"\"query_envelope_kind\":\"table_row_number_window\"",
        "\"window_named_reference_present\":true",
        "\"window_named_definition_present\":true",
        "\"window_name_text_included\":false",
        "\"window_name_ref_descriptor\":\"named_window_0\"",
        "\"window_definition_descriptor\":\"named_window_0\"",
        "\"order_by\":\"id\""}},
      {"SELECT row_number() OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE NO OTHERS) FROM customer",
       "query.execute",
       "SBLR_QUERY_EXECUTE",
       "sblr.query.relational.v3",
       {"SBSQL-541127FA7A87", "SBSQL-777A3A7148F0", "SBSQL-D2633118D544"},
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
       {"\"query_envelope_kind\":\"scalar_projection\"",
        "\"projection_0_name\":\"ts_window\"",
        "\"projection_0_function_id\":\"timeseries.aggregate\"",
        "\"projection_0_time_series_window_expr_present\":true",
        "\"projection_0_time_series_window_interval_value\":\"1 day\""}},
  };

  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "sbsql_sbsfc_063_window_conformance";
  const auto memory_configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          memory_policy, "sbsql_sbsfc_063_window_conformance");
  Require(memory_configured.ok(),
          "SBSFC-063 memory manager configuration failed");

  const auto fixture_database = MakeFixtureDatabase();
  Require(!fixture_database.empty(), "SBSFC-063 fixture database creation failed");
  ParserConfig config;
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.embedded_database_path = fixture_database.string();
  ParserMetrics metrics;
  SblrTemplateCache cache;
  {
    SbsqlTestWireSession session(config, &metrics, &cache);
    AuthCredentialEnvelope credentials;
    credentials.provider_family = "local_password";
    credentials.principal = std::string(kFixturePrincipal);
    credentials.requested_database = fixture_database.string();
    credentials.application_name = "sbsfc_063_window_conformance";
    credentials.credential_evidence = std::string(kFixturePassword);
    credentials.credential_evidence_present = true;
    MessageVectorSet authentication_messages;
    const bool authenticated =
        session.AuthenticateCredentials(credentials, &authentication_messages);
    if (!authenticated) {
      std::cerr << RenderMessageVectorSet(authentication_messages);
    }
    Require(authenticated && session.session().authenticated,
            "SBSFC-063 fixture did not authenticate");
    const auto created = session.RunPipeline("CREATE TABLE customer (id INT)", true);
    if (!created.accepted) std::cerr << RenderMessageVectorSet(created.messages);
    Require(created.accepted && !created.messages.has_errors(),
            "SBSFC-063 fixture relation creation failed");

    for (std::size_t index = 0; index < 2; ++index) {
      const auto& test_case = cases[index];
      SbsqlPipelineConformanceSummary summary;
      const auto result =
          RunEnginePipelineForConformance(&session, test_case.sql, &summary);
      RequireExactLowering(test_case, result, summary);
    }
  }
  std::error_code cleanup_error;
  std::filesystem::remove_all(fixture_database.parent_path(), cleanup_error);
  RequireStaticScalarProjectionLowering(cases.back());
  RequireTimeSeriesProjectionDispatch();
  RequireTimeSeriesAppendDispatch();
  std::cout << "sbsql_sbsfc_063_window_grammar_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
