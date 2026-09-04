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
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
namespace memory = scratchbird::core::memory;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kTableUuid = "019f0000-0000-7000-8000-000000064001";
constexpr std::string_view kProcedureUuid = "019f0000-0000-7000-8000-000000064002";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000064003";
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
    {"SBSQL-00BB5F6A382D", "parameter_marker", "sblr.general.operation.v3"},
    {"SBSQL-0B00DEA678E2", "parameter_def", "sblr.general.operation.v3"},
    {"SBSQL-C5D151D17944", "parameter_name", "sblr.general.operation.v3"},
    {"SBSQL-095E9B3C6EDF", "sample_clause", "sblr.general.operation.v3"},
    {"SBSQL-999D9DF49F00", "sample_method", "sblr.general.operation.v3"},
};

struct RouteCase {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view operation_family;
  std::vector<std::string_view> surface_ids;
  std::vector<std::string> resolved_uuids;
  std::vector<std::string_view> payload_markers;
  std::vector<std::string_view> forbidden_payload_markers;
};

struct RefusalCase {
  std::string_view sql;
  std::string_view operation_family;
  std::string_view command_family;
  std::string_view trace_key;
  std::string_view canonical_parent_operation_id;
  std::string_view canonical_parent_sblr_opcode;
  std::vector<std::string_view> surface_ids;
};

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

std::filesystem::path MakePipelineFixtureDatabase() {
  static std::atomic<std::uint64_t> identity_time{1788204000000ULL};
  std::string template_path = "/tmp/sbsql_sbsfc_064_parameter_sample.XXXXXX";
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
      std::filesystem::path(directory) / "sbsfc_064_parameter_sample.sbdb";
  db::DatabaseCreateConfig create;
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
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok() ||
      created.create_finality != db::DatabaseCreateFinalityClass::committed) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return {};
  }
  return path;
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string DiagnosticField(const Diagnostic& diagnostic,
                            std::string_view name) {
  for (const auto& field : diagnostic.fields) {
    if (field.name == name) return field.value;
  }
  return {};
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
  session.session_uuid = "019f0000-0000-7000-8000-000000064101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000064102";
  session.database_uuid = "019f0000-0000-7000-8000-000000064103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 464;
  session.security_policy_epoch = 465;
  session.descriptor_epoch = 466;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_064_parameter_sample_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000064104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-064-parameter-sample-route-test";
  config.build_id = "sbsql-sbsfc-064-parameter-sample-route-test";
  return config;
}

PipelineResult RunEnginePipelineForConformance(
    SbsqlTestWireSession* session,
    const RouteCase& test_case,
    SbsqlPipelineConformanceSummary* summary) {
  std::vector<PreparedParameterWireValue> parameters;
  if (test_case.sql == "SELECT ? AS marker_value") {
    PreparedParameterWireValue value;
    value.encoding = PreparedParameterPayloadEncoding::utf8_text;
    constexpr std::string_view kValue = "7";
    value.raw_bytes.assign(kValue.begin(), kValue.end());
    parameters.push_back(std::move(value));
  }
  return session->RunPipeline(test_case.sql, true, false, 0, false,
                              parameters, nullptr, false, nullptr, {}, 0,
                              nullptr, nullptr, summary);
}

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-064 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-064 generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "SBSFC-064 generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-064 generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-064 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.canonical_sblr_operation_family,
            "SBSFC-064 generated registry SBLR family drifted");
  }
}

void RequireExactLowering(
    const RouteCase& test_case,
    const PipelineResult& result,
    const SbsqlPipelineConformanceSummary& summary) {
  if (!result.accepted || result.messages.has_errors()) {
    std::cerr << "SBSFC-064 SQL: " << test_case.sql << '\n';
    std::cerr << "SBSFC-064 SBLR: " << result.sblr_payload << '\n';
    PrintMessages(result.messages);
  }
  Require(result.accepted && !result.messages.has_errors(),
          "SBSFC-064 live engine pipeline failed");
  Require(summary.captured, "SBSFC-064 final parser artifacts were not captured");
  Require(summary.bound && !summary.bound_has_errors,
          "SBSFC-064 did not bind under engine descriptor authority");
  Require(summary.verifier_admitted && !summary.verifier_has_errors,
          "SBSFC-064 verifier rejected exact route");
  Require(summary.payload_nonempty && !result.sblr_payload.empty(),
          "SBSFC-064 produced no canonical SBLR payload");
  Require(summary.operation_id == test_case.operation_id,
          "SBSFC-064 operation id mismatch");
  Require(summary.sblr_opcode == test_case.opcode,
          "SBSFC-064 SBLR opcode mismatch");
  Require(summary.operation_family == test_case.operation_family,
          "SBSFC-064 operation family mismatch");
  Require(result.server_operation_id == test_case.operation_id,
          "SBSFC-064 engine operation id mismatch");
  Require(summary.has_syntax_authority,
          "SBSFC-064 syntax authority projection missing");
  Require(!result.parser_executes_sql,
          "SBSFC-064 lowering allowed parser SQL execution");
  Require(Contains(result.sblr_payload, "\"sql_text_included\":false"),
          "SBSFC-064 payload did not prove no SQL text authority");
  Require(!Contains(result.sblr_payload, test_case.sql),
          "SBSFC-064 payload embedded source SQL text");
  Require(!Contains(result.sblr_payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(result.sblr_payload, "refusal"),
          "SBSFC-064 payload used forbidden replay/refusal evidence");
  Require(!Contains(result.sblr_payload, "WAL") &&
              !Contains(result.sblr_payload, "wal") &&
              !Contains(result.sblr_payload, "recovery"),
          "SBSFC-064 payload carried WAL/recovery authority");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(result.sblr_payload, surface_id),
            std::string("SBSFC-064 payload missing row marker ") +
                std::string(surface_id));
  }
  for (const auto marker : test_case.payload_markers) {
    Require(Contains(result.sblr_payload, marker),
            std::string("SBSFC-064 payload missing marker ") + std::string(marker));
  }
  for (const auto marker : test_case.forbidden_payload_markers) {
    Require(!Contains(result.sblr_payload, marker),
            std::string("SBSFC-064 payload embedded forbidden marker ") +
                std::string(marker));
  }
}

void RequireExactRefusal(
    const RefusalCase& test_case,
    const PipelineResult& result,
    const SbsqlPipelineConformanceSummary& summary) {
  if (result.accepted || !result.messages.has_errors()) {
    std::cerr << "SBSFC-064 refusal SQL: " << test_case.sql << '\n';
    std::cerr << "SBSFC-064 refusal SBLR: " << result.sblr_payload << '\n';
    PrintMessages(result.messages);
  }
  Require(!result.accepted && result.messages.has_errors(),
          "SBSFC-064 unavailable parent route was admitted");
  if (!summary.captured || summary.bound_has_errors) {
    std::cerr << "SBSFC-064 refusal SQL: " << test_case.sql << '\n'
              << "  captured=" << summary.captured
              << " bound=" << summary.bound
              << " bound_has_errors=" << summary.bound_has_errors << '\n';
    PrintMessages(result.messages);
  }
  Require(summary.captured && !summary.bound_has_errors,
          "SBSFC-064 refusal did not preserve syntax/bind component evidence");
  Require(!summary.verifier_admitted && summary.verifier_has_errors,
          "SBSFC-064 unavailable parent route passed SBLR verification");
  Require(!summary.payload_nonempty && result.sblr_payload.empty(),
          "SBSFC-064 exact refusal emitted executable SBLR");
  Require(summary.operation_family == test_case.operation_family &&
              summary.sblr_operation_key == test_case.operation_family &&
              summary.command_family == test_case.command_family,
          "SBSFC-064 refusal family projection drifted");
  Require(summary.operation_id == "engine.op.diagnostic_refusal" &&
              summary.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              summary.result_shape_key == "diagnostic_vector.v1" &&
              summary.diagnostic_shape_key == "diagnostic_vector.v1" &&
              summary.resource_contract_key ==
                  "sbsql.command.no_execution.v1",
          "SBSFC-064 refusal tuple drifted");
  Require(summary.has_syntax_authority && summary.has_descriptor_refs &&
              summary.resolved_object_uuids.empty() &&
              !result.parser_executes_sql && result.server_operation_id.empty(),
          "SBSFC-064 refusal retained execution or resolver authority");

  const auto diagnostic = std::find_if(
      result.messages.diagnostics.begin(), result.messages.diagnostics.end(),
      [](const Diagnostic& candidate) {
        return candidate.code == "SBSQL.IMPL.NOT_AVAILABLE";
      });
  Require(diagnostic != result.messages.diagnostics.end(),
          "SBSFC-064 exact refusal diagnostic missing");
  Require(DiagnosticField(*diagnostic, "canonical_parent_operation_id") ==
                  test_case.canonical_parent_operation_id &&
              DiagnosticField(*diagnostic,
                              "canonical_parent_sblr_opcode") ==
                  test_case.canonical_parent_sblr_opcode &&
              DiagnosticField(*diagnostic, "executable_sblr_emitted") ==
                  "false",
          "SBSFC-064 exact refusal parent identity drifted");
  const auto recognized =
      DiagnosticField(*diagnostic, "recognized_surface_ids");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(recognized, surface_id),
            std::string("SBSFC-064 refusal omitted surface ") +
                std::string(surface_id));
  }
}

void RequireServerAdmission(const RouteCase& test_case,
                            const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(envelope));
  Require(admission.admitted, "server admission rejected SBSFC-064 exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SBSFC-064");
  Require(admission.operation_id == test_case.operation_id,
          "server admission SBSFC-064 operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-064-parameter-sample-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000064201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000064202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000064203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000064204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000064205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000064206";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000064208";
  context.local_transaction_id = 64;
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

void RequireParameterMarkerDispatch() {
  auto envelope = EngineEnvelope("query.evaluate_projection",
                                 "SBLR_QUERY_EVALUATE_PROJECTION",
                                 "trace.sbsfc064.parameter.marker");
  AddTextOperand(&envelope, "projection_count", "1");
  AddTextOperand(&envelope, "projection_0_name", "marker_value");
  AddTextOperand(&envelope, "projection_0_expr_kind", "parameter");
  AddTextOperand(&envelope, "projection_0_type", "unknown");
  AddTextOperand(&envelope, "projection_0_parameter_descriptor_kind", "input");
  AddTextOperand(&envelope, "projection_0_parameter_marker_kind", "anonymous");
  AddTextOperand(&envelope, "projection_0_parameter_ordinal", "1");
  AddTextOperand(&envelope, "projection_0_parameter_value_embedded", "false");
  AddTextOperand(&envelope, "projection_0_parser_bound_parameter_value", "false");
  envelope = scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-064 parameter envelope invalid");
  Require(result.accepted, "SBSFC-064 parameter dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-064 parameter did not dispatch");
  if (!result.api_result.ok) PrintApiDiagnostics(result.api_result);
  Require(result.api_result.ok, "EngineEvaluateProjection parameter route failed");
  Require(HasEvidence(result.api_result, "query_parameter_descriptor", "anonymous"),
          "SBSFC-064 parameter descriptor evidence missing");
  Require(HasEvidence(result.api_result, "query_parameter_value_execution", "false"),
          "SBSFC-064 parameter value-execution evidence missing");
  Require(result.api_result.result_shape.rows.size() == 1,
          "SBSFC-064 parameter projection row count mismatch");
  Require(result.api_result.result_shape.columns.size() == 1 &&
              result.api_result.result_shape.columns.front().descriptor_kind == "parameter",
          "SBSFC-064 parameter descriptor shape mismatch");
  Require(FieldValue(result.api_result, "marker_value") == "unbound_parameter_descriptor",
          "SBSFC-064 parameter descriptor payload mismatch");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_sbsfc_064_parameter_sample_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810640000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810640001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810640002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "SBSFC-064 engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContextForDatabase(const std::filesystem::path& path,
                                                   const std::string& database_uuid) {
  auto context = EngineContext();
  context.request_id = "sbsql-sbsfc-064-procedure-parameter-dispatch";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.local_transaction_id = 0;
  context.transaction_uuid.canonical.clear();
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContextForDatabase(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.sbsfc064.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  envelope = scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-064 transaction begin envelope invalid");
  Require(result.accepted, "SBSFC-064 transaction begin dispatch rejected");
  if (!result.api_result.ok) PrintApiDiagnostics(result.api_result);
  Require(result.api_result.ok, "SBSFC-064 transaction begin failed");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  return context;
}

void RequireProcedureParameterDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginEngineTransaction(path, database_uuid);

  auto envelope = EngineEnvelope("ddl.create_procedure",
                                 "SBLR_DDL_CREATE_PROCEDURE",
                                 "trace.sbsfc064.procedure.parameter");
  AddTextOperand(&envelope, "target_object_uuid", std::string(kProcedureUuid));
  AddTextOperand(&envelope, "target_object_kind", "procedure");
  AddTextOperand(&envelope, "procedure_object_uuid", std::string(kProcedureUuid));
  AddTextOperand(&envelope, "procedure_name", "process_order");
  AddTextOperand(&envelope, "target_schema_uuid", std::string(kSchemaUuid));
  AddTextOperand(&envelope, "executable_object_kind", "procedure");
  AddTextOperand(&envelope, "signature_descriptor_kind", "routine_parameter_descriptor");
  AddTextOperand(&envelope, "routine_parameter_count", "1");
  AddTextOperand(&envelope, "routine_parameter_0_name_descriptor", "routine_parameter_0");
  AddTextOperand(&envelope, "routine_parameter_0_type", "bigint");
  AddTextOperand(&envelope, "routine_parameter_0_mode", "in");
  AddTextOperand(&envelope, "routine_parameter_name_text_included", "false");
  envelope = scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));

  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-064 procedure envelope invalid");
  Require(result.accepted, "SBSFC-064 procedure dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-064 procedure did not dispatch");
  if (!result.api_result.ok) PrintApiDiagnostics(result.api_result);
  Require(result.api_result.ok, "EngineCreateProcedure parameter route failed");
  Require(result.api_result.primary_object.object_kind == "procedure",
          "SBSFC-064 procedure returned wrong object kind");
  Require(result.api_result.primary_object.uuid.canonical == kProcedureUuid,
          "SBSFC-064 procedure returned wrong object UUID");
  Require(HasEvidence(result.api_result, "api_behavior_event", "ddl.create_procedure"),
          "SBSFC-064 procedure API event evidence missing");
  Require(HasEvidence(result.api_result, "name_registry", kProcedureUuid),
          "SBSFC-064 procedure name-registry evidence missing");
  Require(Contains(FieldValue(result.api_result, "payload"),
                   "routine_parameter_count:1"),
          "SBSFC-064 procedure parameter descriptor payload missing");
  Require(Contains(FieldValue(result.api_result, "payload"),
                   "routine_parameter_0_name_descriptor:routine_parameter_0"),
          "SBSFC-064 procedure parameter-name descriptor payload missing");
  RemoveDatabaseArtifacts(path);
}

void RequireSamplePlanDispatch() {
  auto envelope = EngineEnvelope("query.plan_operation",
                                 "SBLR_QUERY_PLAN_OPERATION",
                                 "trace.sbsfc064.sample.plan");
  AddTextOperand(&envelope, "query_operation", "sample");
  AddTextOperand(&envelope, "execute", "true");
  AddTextOperand(&envelope, "sample_method", "bernoulli");
  AddTextOperand(&envelope, "sample_percent", "50");
  AddTextOperand(&envelope, "sample_clause_present", "true");
  envelope = scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));

  api::EngineApiRequest api_request;
  api_request.rows.push_back(Row("relation-0-row-019f0000-0000-7000-8000-000000064301", "1"));
  api_request.rows.push_back(Row("relation-0-row-019f0000-0000-7000-8000-000000064302", "2"));
  api_request.rows.push_back(Row("relation-0-row-019f0000-0000-7000-8000-000000064303", "3"));
  api_request.rows.push_back(Row("relation-0-row-019f0000-0000-7000-8000-000000064304", "4"));

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, std::move(api_request)};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-064 sample envelope invalid");
  Require(result.accepted, "SBSFC-064 sample dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-064 sample did not dispatch");
  if (!result.api_result.ok) PrintApiDiagnostics(result.api_result);
  Require(result.api_result.ok, "EnginePlanOperation sample route failed");
  Require(result.api_result.result_shape.rows.size() == 2,
          "SBSFC-064 sample row count mismatch");
  Require(HasEvidence(result.api_result, "query_sample", "bernoulli"),
          "SBSFC-064 sample method runtime evidence missing");
  Require(HasEvidence(result.api_result, "query_sample_descriptor",
                      "engine_row_descriptor_sample_route"),
          "SBSFC-064 sample descriptor runtime evidence missing");
  Require(FieldValue(result.api_result, "c0", 0) == "1" &&
              FieldValue(result.api_result, "c0", 1) == "2",
          "SBSFC-064 sample deterministic rowset mismatch");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const RouteCase parameter_case =
      {"SELECT ? AS marker_value",
       "query.evaluate_projection",
       "SBLR_QUERY_EVALUATE_PROJECTION",
       "sblr.query.relational.v3",
       {"SBSQL-00BB5F6A382D"},
       {},
       {"\"query_envelope_kind\":\"scalar_projection\"",
        "\"projection_0_expr_kind\":\"parameter\"",
        "\"projection_0_expr_opcode\":\"SBLR_PARAMETER_DESCRIPTOR\"",
        "\"projection_0_parameter_descriptor_kind\":\"input\"",
        "\"projection_0_parameter_marker_kind\":\"anonymous\"",
        "\"projection_0_parameter_ordinal\":\"1\"",
        "\"projection_0_parameter_value_embedded\":false",
        "\"projection_0_parser_bound_parameter_value\":false",
        "\"projection_0_parameter_name_text_included\":false",
        "sys.query.parameter_descriptor"},
       {"?"}};
  const std::vector<RefusalCase> refusal_cases = {
      {"CREATE PROCEDURE process_order(customer_id BIGINT)",
       "sblr.catalog.mutation.v3",
       "ddl_catalog",
       "trace.sbsql.create_procedure_exact_refusal",
       "engine.op.ddl_create_procedure",
       "SBLR_DDL_CREATE_PROCEDURE",
       {"SBSQL-13F5A8364A50", "SBSQL-0B00DEA678E2",
        "SBSQL-C5D151D17944"}},
      {"SELECT * FROM customer TABLESAMPLE BERNOULLI (50)",
       "sblr.query.relational.v3",
       "query",
       "trace.sbsql.table_sample_exact_refusal",
       "query.execute",
       "SBLR_QUERY_EXECUTE",
       {"SBSQL-095E9B3C6EDF", "SBSQL-999D9DF49F00"}},
  };

  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "sbsql_sbsfc_064_parameter_sample_conformance";
  const auto memory_configured = memory::ConfigureDefaultMemoryManagerForFixture(
      memory_policy, "sbsql_sbsfc_064_parameter_sample_conformance");
  Require(memory_configured.ok(),
          "SBSFC-064 memory manager configuration failed");

  const auto fixture_database = MakePipelineFixtureDatabase();
  Require(!fixture_database.empty(), "SBSFC-064 fixture database creation failed");
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
    credentials.application_name = "sbsfc_064_parameter_sample_conformance";
    credentials.credential_evidence = std::string(kFixturePassword);
    credentials.credential_evidence_present = true;
    MessageVectorSet authentication_messages;
    const bool authenticated =
        session.AuthenticateCredentials(credentials, &authentication_messages);
    if (!authenticated) PrintMessages(authentication_messages);
    Require(authenticated && session.session().authenticated,
            "SBSFC-064 fixture did not authenticate");
    const auto created = session.RunPipeline("CREATE TABLE customer (id INT)", true);
    if (!created.accepted) PrintMessages(created.messages);
    Require(created.accepted && !created.messages.has_errors(),
            "SBSFC-064 fixture relation creation failed");

    SbsqlPipelineConformanceSummary parameter_summary;
    const auto parameter_result = RunEnginePipelineForConformance(
        &session, parameter_case, &parameter_summary);
    RequireExactLowering(parameter_case, parameter_result, parameter_summary);
    Require(parameter_result.server_row_count == 1,
            "SBSFC-064 bound parameter projection did not execute one row");

    for (const auto& test_case : refusal_cases) {
      SbsqlPipelineConformanceSummary refusal_summary;
      const auto refusal_result = session.RunPipeline(
          test_case.sql, true, false, 0, false, {}, nullptr, false, nullptr,
          {}, 0, nullptr, nullptr, &refusal_summary);
      RequireExactRefusal(test_case, refusal_result, refusal_summary);
    }
  }
  std::error_code cleanup_error;
  std::filesystem::remove_all(fixture_database.parent_path(), cleanup_error);
  std::cout << "sbsql_sbsfc_064_parameter_sample_grammar_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
