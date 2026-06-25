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
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kTableUuid = "019f6700-0000-7000-8000-000000000101";
constexpr std::string_view kSchemaUuid = "019f6700-0000-7000-8000-000000000102";
constexpr std::string_view kIndexUuid = "019f6700-0000-7000-8000-000000000103";
constexpr std::string_view kIdColumnUuid = "019f6700-0000-7000-8000-000000000104";
constexpr std::string_view kNameColumnUuid = "019f6700-0000-7000-8000-000000000105";
constexpr std::string_view kRowAda = "019f6700-0000-7000-8000-000000000201";
constexpr std::string_view kRowGrace = "019f6700-0000-7000-8000-000000000202";
constexpr std::string_view kRowKatherine = "019f6700-0000-7000-8000-000000000203";

struct ConflictRow {
  std::string_view surface_id;
  std::string_view canonical_name;
};

constexpr ConflictRow kRows[] = {
    {"SBSQL-0084E23B9299", "on_conflict_clause"},
    {"SBSQL-3635EA022CA5", "conflict_action"},
    {"SBSQL-4C7F112544DA", "conflict_target"},
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
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
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

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f6700-0000-7000-8000-000000000301";
  session.connection_uuid = "019f6700-0000-7000-8000-000000000302";
  session.database_uuid = "019f6700-0000-7000-8000-000000000303";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 67;
  session.security_policy_epoch = 68;
  session.descriptor_epoch = 69;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_067_on_conflict_route";
  config.parser_uuid = "019f6700-0000-7000-8000-000000000304";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-067-on-conflict-route-test";
  config.build_id = "sbsql-sbsfc-067-on-conflict-route-test";
  return config;
}

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(sql));
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
    Require(registry_row != nullptr, "SBSFC-067 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-067 generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "SBSFC-067 generated registry kind drifted");
    Require(registry_row->family == "general",
            "SBSFC-067 generated registry family drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-067 generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-067 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
            "SBSFC-067 generated registry SBLR family drifted");
  }
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

void RequireExactLowering(std::string_view sql,
                          std::string_view expected_action,
                          bool update_action) {
  const auto artifacts = RunPipeline(sql);
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-067 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-067 AST failed");
  Require(artifacts.bound.bound, "SBSFC-067 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-067 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.dml.operation.v3",
          "SBSFC-067 operation family mismatch");
  Require(artifacts.envelope.operation_id == "dml.insert_rows",
          "SBSFC-067 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "dml.insert_rows",
          "SBSFC-067 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_INSERT_ROWS",
          "SBSFC-067 SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-067 no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-067 no-storage-finality authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_row_mutation_required"),
          "SBSFC-067 MGA row mutation authority missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.storage.unique_index_descriptor"),
          "SBSFC-067 unique index descriptor missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.dml.conflict_policy_descriptor"),
          "SBSFC-067 conflict policy descriptor missing");
  Require(HasValue(artifacts.envelope.policy_refs, "row_conflict_policy"),
          "SBSFC-067 row conflict policy missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-067 lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"on_conflict_present\":true"),
          "SBSFC-067 payload missing ON CONFLICT marker");
  Require(Contains(artifacts.envelope.payload, "\"on_conflict_target_column\":\"id\""),
          "SBSFC-067 payload missing conflict target column");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"on_conflict_action\":\"") +
                       std::string(expected_action) + "\""),
          "SBSFC-067 payload missing expected conflict action");
  Require(Contains(artifacts.envelope.payload, "SBSQL-0084E23B9299"),
          "SBSFC-067 payload missing on_conflict_clause row marker");
  Require(Contains(artifacts.envelope.payload, "SBSQL-3635EA022CA5"),
          "SBSFC-067 payload missing conflict_action row marker");
  Require(Contains(artifacts.envelope.payload, "SBSQL-4C7F112544DA"),
          "SBSFC-067 payload missing conflict_target row marker");
  if (update_action) {
    Require(Contains(artifacts.envelope.payload, "\"on_conflict_update_column\":\"name\""),
            "SBSFC-067 payload missing update column descriptor");
    Require(Contains(artifacts.envelope.payload,
                     "\"on_conflict_update_source_column\":\"name\""),
            "SBSFC-067 payload missing excluded source descriptor");
  }
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-067 payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, sql),
          "SBSFC-067 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "SBSFC-067 payload embedded target object name");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-067 payload used forbidden replay/refusal evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-067 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "SBSFC-067 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-067 server admission did not require public ABI dispatch");
  Require(admission.operation_id == "dml.insert_rows",
          "SBSFC-067 server admission operation id mismatch");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  static const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("sbsql_sbsfc_067_on_conflict_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
  return path;
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock",
                            ".sb.mga_row_versions",
                            ".sb.mga_relation_metadata",
                            ".sb.mga_index_entries",
                            ".sb.mga_relation_descriptors",
                            ".sb.mga_large_values",
                            ".sb.mga_savepoints"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabaseForEngineDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779821606700).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779821606701).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779821606702;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "SBSFC-067 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContextForDatabase(const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-067-on-conflict";
  context.database_path = TestDatabasePath().string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f6700-0000-7000-8000-000000000401";
  context.principal_uuid.canonical = "019f6700-0000-7000-8000-000000000402";
  context.security_context_present = true;
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:DML_ROUTE_TEST");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-0084E23B9299");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-3635EA022CA5");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-4C7F112544DA");
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id,
                                     std::string opcode,
                                     std::string trace_key) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         std::move(trace_key));
  envelope.parser_package_uuid = "019f6700-0000-7000-8000-000000000501";
  envelope.registry_snapshot_uuid = "019f6700-0000-7000-8000-000000000502";
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  return envelope;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

sblr::SblrDispatchResult Dispatch(api::EngineRequestContext context,
                                  sblr::SblrOperationEnvelope envelope,
                                  api::EngineApiRequest request = {}) {
  const sblr::SblrDispatchRequest dispatch{std::move(context),
                                           std::move(envelope),
                                           std::move(request)};
  auto result = sblr::DispatchSblrOperation(dispatch);
  if (!result.accepted || !result.envelope_validated || !result.dispatched_to_api ||
      !result.api_result.ok) {
    PrintDispatchDiagnostics(result);
    std::cerr << sblr::SerializeSblrDispatchResultToJson(result) << '\n';
  }
  Require(result.envelope_validated, "SBSFC-067 runtime envelope invalid");
  Require(result.accepted, "SBSFC-067 runtime dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-067 runtime dispatch missed engine API");
  Require(result.api_result.ok, "SBSFC-067 runtime API failed");
  return result;
}

api::EngineRequestContext BeginEngineTransaction(const std::string& database_uuid) {
  auto context = EngineContextForDatabase(database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.sbsfc067.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-067 transaction begin envelope invalid");
  Require(result.accepted, "SBSFC-067 transaction begin dispatch rejected");
  Require(result.api_result.ok, "SBSFC-067 transaction begin failed");
  Require(result.api_result.local_transaction_id != 0,
          "SBSFC-067 transaction begin did not assign local transaction id");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = context.local_transaction_id;
  return context;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineColumnDefinition Column(std::uint32_t ordinal,
                                   std::string name,
                                   std::string uuid_text) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical = std::move(uuid_text);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  return column;
}

api::EngineIndexDefinition UniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = std::string(kIndexUuid);
  index.names.push_back(Name("sbsfc067_customer_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

void SeedSchemaAndTable(const api::EngineRequestContext& context) {
  api::EngineApiRequest schema_request;
  schema_request.target_object.uuid.canonical = std::string(kSchemaUuid);
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("sbsfc067"));
  auto schema = Dispatch(context,
                         Envelope("ddl.create_schema",
                                  "SBLR_DDL_CREATE_SCHEMA",
                                  "trace.sbsfc067.create_schema"),
                         schema_request);
  Require(schema.api_result.primary_object.uuid.canonical == kSchemaUuid,
          "SBSFC-067 schema create did not preserve UUID");

  api::EngineApiRequest table_request;
  table_request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  table_request.target_schema.object_kind = "schema";
  table_request.target_object.uuid.canonical = std::string(kTableUuid);
  table_request.target_object.object_kind = "table";
  table_request.localized_names.push_back(Name("customer"));
  table_request.columns.push_back(Column(0, "id", std::string(kIdColumnUuid)));
  table_request.columns.push_back(Column(1, "name", std::string(kNameColumnUuid)));
  table_request.indexes.push_back(UniqueIdIndex());
  auto table = Dispatch(context,
                        Envelope("ddl.create_table",
                                 "SBLR_DDL_CREATE_TABLE",
                                 "trace.sbsfc067.create_table"),
                        table_request);
  Require(table.api_result.primary_object.uuid.canonical == kTableUuid,
          "SBSFC-067 table create did not preserve UUID");
  Require(HasEvidence(table.api_result, "mga_relation_metadata", "table_create"),
          "SBSFC-067 table create MGA metadata evidence missing");
}

void AddTextOperand(sblr::SblrOperationEnvelope* envelope,
                    std::string name,
                    std::string value) {
  envelope->operands.push_back({"text", std::move(name), std::move(value)});
}

void AddRowField(sblr::SblrOperationEnvelope* envelope,
                 std::string row_uuid,
                 std::string field,
                 std::string value) {
  envelope->operands.push_back(
      {"row_field", std::move(row_uuid) + "|" + std::move(field), std::move(value)});
}

sblr::SblrDispatchResult InsertCustomer(const api::EngineRequestContext& context,
                                        std::string trace_key,
                                        std::string row_uuid,
                                        std::string id,
                                        std::string name,
                                        std::string conflict_action = {},
                                        bool update_name = false) {
  auto envelope = Envelope("dml.insert_rows", "SBLR_DML_INSERT_ROWS", std::move(trace_key));
  AddTextOperand(&envelope, "target_object_uuid", std::string(kTableUuid));
  AddTextOperand(&envelope, "target_object_kind", "table");
  if (!conflict_action.empty()) {
    AddTextOperand(&envelope, "on_conflict_action", std::move(conflict_action));
    AddTextOperand(&envelope, "conflict_target_column", "id");
    if (update_name) AddTextOperand(&envelope, "on_conflict_update_column", "name");
  }
  AddRowField(&envelope, row_uuid, "id", std::move(id));
  AddRowField(&envelope, std::move(row_uuid), "name", std::move(name));
  return Dispatch(context, std::move(envelope));
}

void RequireRuntimeConflictBehavior() {
  const std::string database_uuid = CreateMinimalDatabaseForEngineDispatch();
  auto context = BeginEngineTransaction(database_uuid);
  SeedSchemaAndTable(context);

  auto inserted = InsertCustomer(context,
                                 "trace.sbsfc067.insert_initial",
                                 std::string(kRowAda),
                                 "1",
                                 "Ada");
  Require(HasEvidence(inserted.api_result, "mga_row_store", "row_insert"),
          "SBSFC-067 initial insert evidence missing");
  Require(FieldValue(inserted.api_result, "name") == "Ada",
          "SBSFC-067 initial insert result mismatch");

  auto skipped = InsertCustomer(context,
                                "trace.sbsfc067.on_conflict_do_nothing",
                                std::string(kRowGrace),
                                "1",
                                "Grace",
                                "do_nothing");
  Require(HasEvidence(skipped.api_result, "on_conflict_action", "do_nothing"),
          "SBSFC-067 DO NOTHING action evidence missing");
  Require(HasEvidence(skipped.api_result, "on_conflict_target", "id"),
          "SBSFC-067 conflict target evidence missing");
  Require(HasEvidence(skipped.api_result, "mga_row_store", "row_conflict_skipped"),
          "SBSFC-067 DO NOTHING skip evidence missing");
  Require(skipped.api_result.result_shape.rows.empty(),
          "SBSFC-067 DO NOTHING returned an inserted row");

  auto updated = InsertCustomer(context,
                                "trace.sbsfc067.on_conflict_do_update",
                                std::string(kRowKatherine),
                                "1",
                                "Grace",
                                "do_update",
                                true);
  Require(HasEvidence(updated.api_result, "on_conflict_action", "do_update"),
          "SBSFC-067 DO UPDATE action evidence missing");
  Require(HasEvidence(updated.api_result, "on_conflict_target", "id"),
          "SBSFC-067 DO UPDATE target evidence missing");
  Require(HasEvidence(updated.api_result, "mga_row_store", "row_update"),
          "SBSFC-067 DO UPDATE row update evidence missing");
  Require(updated.api_result.result_shape.rows.size() == 1,
          "SBSFC-067 DO UPDATE did not return one updated row");
  Require(FieldValue(updated.api_result, "id") == "1",
          "SBSFC-067 DO UPDATE id result mismatch");
  Require(FieldValue(updated.api_result, "name") == "Grace",
          "SBSFC-067 DO UPDATE name result mismatch");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  RequireExactLowering(
      "INSERT INTO customer (id, name) VALUES (1, 'Ada') ON CONFLICT (id) DO NOTHING",
      "do_nothing",
      false);
  RequireExactLowering(
      "INSERT INTO customer (id, name) VALUES (1, 'Grace') ON CONFLICT (id) DO UPDATE SET name = excluded.name",
      "do_update",
      true);
  RequireRuntimeConflictBehavior();
  std::cout << "sbsql_sbsfc_067_on_conflict_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
