// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "ast/ast.hpp"
#include "artifacts/artifact_api.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "ddl/create_api.hpp"
#include "dml/import_api.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/import_reject_model.hpp"
#include "dml/import_resume_checkpoint.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "transaction/transaction_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;
namespace parser = scratchbird::parser::sbsql;

namespace {

#ifndef SB_MISS009_SEED_PACK_ROOT
#define SB_MISS009_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr const char* kDatabaseUuid = "019f2900-0000-7000-8000-000000000001";
constexpr const char* kSchemaUuid = "019f2900-0000-7000-8000-000000000101";
constexpr const char* kTableUuid = "019f2900-0000-7000-8000-000000000102";
constexpr const char* kUniqueIdIndexUuid = "019f2900-0000-7000-8000-000000000103";
constexpr const char* kArtifactUuid = "019f2900-0000-7000-8000-000000000901";
constexpr const char* kSourceUuid = "019f2900-0000-7000-8000-000000000903";

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_missing_functionality_bulk_import_export_conformance";
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_missing_functionality_bulk_import_export_conformance");
  Require(configured.ok(), "MISS-009 memory fixture configuration failed");
  Require(configured.fixture_mode, "MISS-009 memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_miss009_bulk.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineTypedValue TextValue(std::string value, bool is_null = false) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = is_null ? "type=text;nullable=true"
                                                : "type=text;nullable=false";
  typed.encoded_value = std::move(value);
  typed.is_null = is_null;
  return typed;
}

api::EngineTypedValue BoolValue(bool value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "boolean";
  typed.descriptor.encoded_descriptor = "type=boolean;nullable=false";
  typed.encoded_value = value ? "true" : "false";
  return typed;
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical =
      "019f2900-0000-7000-8000-00000000030" + std::to_string(ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical =
      "019f2900-0000-7000-8000-00000000040" + std::to_string(ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  return column;
}

api::EngineIndexDefinition UniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = kUniqueIdIndexUuid;
  index.names.push_back(Name("miss009_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

api::EngineRowValue Row(std::string row_uuid, std::string id, std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EngineRowValue ArtifactRow() {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = "019f2900-0000-7000-8000-000000000902";
  row.fields.push_back({"artifact_format", TextValue("sb.catalog.artifact.v1")});
  row.fields.push_back({"object_uuid", TextValue(kArtifactUuid)});
  row.fields.push_back({"object_kind", TextValue("bulk_runtime_probe")});
  row.fields.push_back({"default_name", TextValue("miss009_bulk_runtime_probe")});
  row.fields.push_back({"payload", TextValue("state=active;source=SBSQL-MISS-009")});
  row.fields.push_back({"remap_uuid", TextValue({}, true)});
  row.fields.push_back({"content_hash", TextValue({}, true)});
  row.fields.push_back({"value_redacted", BoolValue(false)});
  return row;
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

std::uint64_t EvidenceU64(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) continue;
    try {
      return static_cast<std::uint64_t>(std::stoull(evidence.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

void Grant(api::EngineRequestContext* context,
           std::string right,
           std::string target_uuid = kTableUuid) {
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      "019f2900-0000-7000-8000-0000000006" +
      std::to_string(context->authorization_context.grants.size());
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = std::move(target_uuid);
  grant.right = std::move(right);
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

void AddAuthorization(api::EngineRequestContext* context) {
  context->authorization_context.present = true;
  context->authorization_context.authority_uuid = context->database_uuid;
  context->authorization_context.principal_uuid = context->principal_uuid;
  context->authorization_context.security_epoch = context->security_epoch;
  context->authorization_context.policy_epoch = 1;
  context->authorization_context.catalog_generation_id =
      context->catalog_generation_id;
  context->authorization_context.effective_subjects.push_back(
      {context->principal_uuid, "principal"});
  Grant(context, "INSERT");
  Grant(context, "SELECT");
  Grant(context, "CATALOG_MUTATE", "*");
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      std::string_view session_suffix = "001") {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "miss009-bulk-import-export";
  context.database_path = database_path.string();
  context.database_uuid.canonical = kDatabaseUuid;
  context.principal_uuid.canonical = "019f2900-0000-7000-8000-000000000002";
  context.session_uuid.canonical =
      std::string("019f2900-0000-7000-8000-000000000") +
      std::string(session_suffix);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("SBSQL-MISS-009");
  return context;
}

api::EngineRequestContext BeginTransaction(const std::filesystem::path& database_path,
                                           std::string_view session_suffix,
                                           bool authorized = true) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(database_path, session_suffix);
  auto begin = api::EngineBeginTransaction(request);
  Require(begin.ok, "MISS-009 transaction begin failed");
  auto context = BaseContext(database_path, session_suffix);
  context.local_transaction_id = begin.local_transaction_id;
  context.transaction_uuid = begin.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begin.snapshot_visible_through_local_transaction_id != 0
          ? begin.snapshot_visible_through_local_transaction_id
          : EvidenceU64(begin, "snapshot_visible_through_local_transaction_id");
  if (authorized) AddAuthorization(&context);
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  auto commit = api::EngineCommitTransaction(request);
  Require(commit.ok, "MISS-009 commit failed");
}

void CreateSchemaAndTable(const std::filesystem::path& database_path) {
  auto context = BeginTransaction(database_path, "101", false);

  api::EngineCreateSchemaRequest schema_request;
  schema_request.context = context;
  schema_request.target_object.uuid.canonical = kSchemaUuid;
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("miss009_schema"));
  auto schema = api::EngineCreateSchema(schema_request);
  Require(schema.ok, "MISS-009 schema create failed");

  api::EngineCreateTableRequest table_request;
  table_request.context = context;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.requested_table_uuid.canonical = kTableUuid;
  table_request.target_object.uuid.canonical = kTableUuid;
  table_request.target_object.object_kind = "table";
  table_request.table_names.push_back(Name("miss009_table"));
  table_request.table_columns.push_back(Column(0, "id"));
  table_request.table_columns.push_back(Column(1, "note"));
  table_request.table_indexes.push_back(UniqueIdIndex());
  auto table = api::EngineCreateTable(table_request);
  Require(table.ok, "MISS-009 table create failed");
  Commit(context);
}

api::EnginePlanImportRowsResult VerifyImportPlanning(
    const api::EngineRequestContext& context) {
  api::EnginePlanImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.redacted_source_handle = "client-stream";
  request.format.format_family = "csv";
  request.format.encoding = "utf-8";
  request.format.header_policy = "no_header";

  auto plan = api::EnginePlanImportRows(request);
  Require(plan.ok, "MISS-009 import planning failed");
  Require(plan.surface_accepted, "MISS-009 import planning surface not accepted");
  Require(plan.planning_only, "MISS-009 import planning was not planning-only");
  Require(plan.execution_requires_execute_import_rows,
          "MISS-009 import plan did not require execute_import_rows");
  Require(plan.normalized_insert_mode == "copy_import",
          "MISS-009 import planning insert mode mismatch");
  Require(HasEvidence(plan, "parser_boundary",
                      "parser_decodes_bytes_engine_validates_uuid_rows"),
          "MISS-009 parser boundary evidence missing from plan");
  Require(HasEvidence(plan, "import_plan_row_persistence_claimed", "false"),
          "MISS-009 plan claimed row persistence");
  return plan;
}

void VerifyRejectAndCheckpointModels(const api::EngineRequestContext& context) {
  api::EngineNormalizeImportRejectModelRequest reject_request;
  reject_request.context = context;
  reject_request.target_table.uuid.canonical = kTableUuid;
  reject_request.target_table.object_kind = "table";
  reject_request.reject_policy.reject_mode = "reject_row";
  reject_request.reject_policy.reject_limit_rows = 10;
  reject_request.reject_policy.reject_payload_policy = "diagnostic_only";
  reject_request.reject_policy.resume_policy = "fail_closed";
  auto reject = api::EngineNormalizeImportRejectModel(reject_request);
  Require(reject.ok, "MISS-009 reject model normalization failed");
  Require(reject.error_row_schema.schema_version == 1,
          "MISS-009 reject row schema version mismatch");
  Require(HasEvidence(reject, "import_reject_model", "reject_row"),
          "MISS-009 reject model evidence missing");

  api::EngineNormalizeImportCheckpointRequest checkpoint_request;
  checkpoint_request.context = context;
  checkpoint_request.target_table.uuid.canonical = kTableUuid;
  checkpoint_request.target_table.object_kind = "table";
  checkpoint_request.checkpoint_policy.checkpoint_mode = "disabled";
  checkpoint_request.checkpoint_policy.resume_policy = "fail_closed";
  auto checkpoint = api::EngineNormalizeImportCheckpointModel(checkpoint_request);
  Require(checkpoint.ok, "MISS-009 checkpoint model normalization failed");
  Require(!checkpoint.checkpoint_required,
          "MISS-009 disabled checkpoint unexpectedly required checkpointing");
  Require(HasEvidence(checkpoint, "import_checkpoint_model", "disabled"),
          "MISS-009 checkpoint model evidence missing");
}

void VerifyFailFastCopyExecution(const api::EngineRequestContext& context) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_fingerprint = "miss009-copy-fast";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.checkpoint_policy.resume_policy = "fail_closed";
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000201", "1", "copy-fast-a"));
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000202", "2", "copy-fast-b"));
  request.estimated_row_count = request.canonical_rows.size();

  auto executed = api::EngineExecuteImportRows(request);
  Require(executed.ok, "MISS-009 fail-fast import execution failed");
  Require(executed.accepted_rows == 2 && executed.inserted_rows == 2,
          "MISS-009 fail-fast import row counts mismatch");
  Require(HasEvidence(executed, "import_execution", "direct_physical"),
          "MISS-009 fail-fast import did not use direct physical COPY path");
  Require(HasEvidence(executed, "import_execution_delegate", "none"),
          "MISS-009 fail-fast import delegated unexpectedly");
  Require(HasEvidence(executed, "import_plan_consumed", "true"),
          "MISS-009 import plan was not consumed");
  Require(HasEvidence(executed, "parser_finality_authority", "false"),
          "MISS-009 import allowed parser finality authority");
  Require(HasEvidence(executed, "reference_finality_authority", "false"),
          "MISS-009 import allowed reference finality authority");
}

void VerifyRejectRowExecution(const api::EngineRequestContext& context) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_fingerprint = "miss009-copy-reject";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "reject_row";
  request.import_policy.reject_limit_rows = 10;
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.checkpoint_policy.resume_policy = "fail_closed";
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000203", "3", "copy-reject-valid"));
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000204", "1", "copy-reject-duplicate"));
  request.estimated_row_count = request.canonical_rows.size();

  auto executed = api::EngineExecuteImportRows(request);
  Require(executed.ok, "MISS-009 reject-row import execution failed");
  Require(executed.accepted_rows == 1 && executed.rejected_rows == 1,
          "MISS-009 reject-row import counts mismatch");
  Require(HasEvidence(executed, "import_execution",
                      "delegated_to_dml.insert_rows"),
          "MISS-009 reject-row import did not use row reject path");
  Require(HasEvidence(executed, "import_reject_materialization",
                      "result_shape"),
          "MISS-009 reject-row diagnostics not materialized");
  Require(HasEvidence(executed, "copy_append_reject_fallback", "bisection"),
          "MISS-009 reject-row bisection proof missing");
  Require(FieldValue(executed, "diagnostic_code", executed.result_shape.rows.size() - 1) ==
              "CLI.CONSTRAINT_UNIQUE_VIOLATION",
          "MISS-009 reject diagnostic code mismatch");
}

void VerifyNativeBinaryRowset(const api::EngineRequestContext& context) {
  api::EngineExecuteNativeBulkIngestRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.checkpoint_policy.resume_policy = "fail_closed";
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000205", "4", "native-bulk-a"));
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000206", "5", "native-bulk-b"));
  request.estimated_row_count = request.canonical_rows.size();

  auto ingested = api::EngineExecuteNativeBulkIngest(request);
  Require(ingested.ok, "MISS-009 native binary rowset ingest failed");
  Require(ingested.accepted_rows == 2 && ingested.inserted_rows == 2,
          "MISS-009 native binary rowset counts mismatch");
  Require(HasEvidence(ingested, "native_bulk_ingest", "enabled"),
          "MISS-009 native bulk enabled evidence missing");
  Require(HasEvidence(ingested, "native_bulk_ingest_batch_format",
                      "engine_binary_row_batch"),
          "MISS-009 native binary rowset format evidence missing");
  Require(HasEvidence(ingested, "native_bulk_ingest_lane", "direct_physical"),
          "MISS-009 native bulk direct lane evidence missing");

  request.native_bulk_ingest_enabled = false;
  auto refused = api::EngineExecuteNativeBulkIngest(request);
  Require(!refused.ok, "MISS-009 disabled native bulk ingest unexpectedly passed");
  Require(HasDiagnostic(refused, "DML.NATIVE_BULK_INGEST.DISABLED"),
          "MISS-009 disabled native bulk diagnostic missing");
  Require(HasEvidence(refused, "native_bulk_ingest", "disabled"),
          "MISS-009 disabled native bulk evidence missing");
}

void VerifyCatalogArtifactExportImport(const api::EngineRequestContext& context) {
  api::EngineExportCatalogArtifactsRequest export_request;
  export_request.context = context;
  auto exported_before = api::EngineExportCatalogArtifacts(export_request);
  Require(exported_before.ok, "MISS-009 catalog artifact export failed");
  Require(HasEvidence(exported_before, "catalog_artifact_format",
                      "sb.catalog.artifact.v1"),
          "MISS-009 catalog artifact export format evidence missing");
  Require(HasEvidence(exported_before, "git_runtime_authority", "false"),
          "MISS-009 catalog artifact export claimed git runtime authority");

  api::EngineImportCatalogArtifactsRequest import_request;
  import_request.context = context;
  import_request.rows.push_back(ArtifactRow());
  import_request.option_envelopes.push_back("uuid_mode:preserve");
  import_request.option_envelopes.push_back("conflict_policy:reject");
  auto imported = api::EngineImportCatalogArtifacts(import_request);
  Require(imported.ok, "MISS-009 catalog artifact import failed");
  Require(HasEvidence(imported, "catalog_artifact_import_count", "1"),
          "MISS-009 catalog artifact import count evidence missing");
  Require(HasEvidence(imported, "catalog_artifact_imported", kArtifactUuid),
          "MISS-009 catalog artifact imported UUID evidence missing");
  Require(HasEvidence(imported, "git_runtime_authority", "false"),
          "MISS-009 catalog artifact import claimed git runtime authority");

  auto exported_after = api::EngineExportCatalogArtifacts(export_request);
  Require(exported_after.ok, "MISS-009 post-import artifact export failed");
  Require(HasEvidence(exported_after, "catalog_artifact_export_count"),
          "MISS-009 post-import artifact export count evidence missing");
}

struct ParserCase {
  std::string_view sql;
  std::string_view surface_variant;
  std::string_view source_kind;
  std::string_view format_family;
};

parser::SessionContext ParserSession() {
  parser::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f2900-0000-7000-8000-000000000801";
  session.connection_uuid = "019f2900-0000-7000-8000-000000000802";
  session.database_uuid = kDatabaseUuid;
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 290;
  session.security_policy_epoch = 291;
  session.descriptor_epoch = 292;
  return session;
}

parser::ParserConfig ParserConfigForTest() {
  parser::ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsql_miss009_bulk";
  config.parser_uuid = "019f2900-0000-7000-8000-000000000803";
  config.bundle_contract_id = "sbp_sbsql@sbsql-miss-009";
  config.build_id = "sbsql-miss-009-bulk-import-export";
  return config;
}

void VerifyBulkParserRoute(const ParserCase& test_case) {
  const auto session = ParserSession();
  auto cst = parser::BuildCst(std::string(test_case.sql));
  auto ast = parser::BuildAst(cst);
  auto bound = parser::BindAst(ast,
                               cst,
                               ParserConfigForTest(),
                               session,
                               {std::string(kTableUuid), std::string(kSourceUuid)});
  auto envelope = parser::LowerToSblr(bound, cst, session);
  auto verifier = parser::VerifySblrEnvelope(envelope);
  if (cst.messages.has_errors()) std::cerr << parser::RenderMessageVectorSet(cst.messages);
  if (ast.messages.has_errors()) std::cerr << parser::RenderMessageVectorSet(ast.messages);
  if (!bound.bound) std::cerr << parser::RenderMessageVectorSet(bound.messages);
  if (!verifier.admitted) std::cerr << parser::RenderMessageVectorSet(verifier.messages);
  Require(!cst.messages.has_errors(), "MISS-009 bulk CST failed");
  Require(!ast.messages.has_errors(), "MISS-009 bulk AST failed");
  Require(bound.bound, "MISS-009 bulk bind failed");
  Require(verifier.admitted, "MISS-009 bulk SBLR verifier rejected envelope");
  Require(envelope.operation_id == "dml.plan_import_rows",
          "MISS-009 bulk route operation mismatch");
  Require(envelope.engine_api_operation_id == "dml.plan_import_rows",
          "MISS-009 bulk route engine API mismatch");
  Require(envelope.sblr_opcode == "SBLR_DML_PLAN_IMPORT_ROWS",
          "MISS-009 bulk route opcode mismatch");
  Require(envelope.operation_family == "sblr.dml.operation.v3",
          "MISS-009 bulk route parser family mismatch");
  Require(envelope.payload.find(std::string("\"dml_surface_variant\":\"") +
                                std::string(test_case.surface_variant) + "\"") !=
              std::string::npos,
          "MISS-009 bulk surface variant missing");
  Require(envelope.payload.find(std::string("\"source_kind\":\"") +
                                std::string(test_case.source_kind) + "\"") !=
              std::string::npos,
          "MISS-009 bulk source kind missing");
  Require(envelope.payload.find(std::string("\"format_family\":\"") +
                                std::string(test_case.format_family) + "\"") !=
              std::string::npos,
          "MISS-009 bulk format family missing");
  Require(envelope.payload.find("\"import_execution_deferred\":true") !=
              std::string::npos,
          "MISS-009 bulk route did not defer execution");
  Require(envelope.payload.find("\"parser_decodes_bytes\":false") !=
              std::string::npos,
          "MISS-009 bulk route claimed parser byte decoding");
  Require(envelope.payload.find("\"row_persistence_claimed\":false") !=
              std::string::npos,
          "MISS-009 bulk route claimed row persistence");
  Require(envelope.payload.find(test_case.sql) == std::string::npos,
          "MISS-009 bulk route embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "MISS-009 bulk route server admission failed");
  Require(admission.requires_public_abi_dispatch,
          "MISS-009 bulk route did not require public ABI dispatch");
  Require(admission.operation_id == "dml.plan_import_rows",
          "MISS-009 bulk route server operation mismatch");
  Require(admission.operation_family == "sblr.bulk.import.v3",
          "MISS-009 bulk route server family mismatch");
}

void VerifyBulkParserRoutes() {
  const ParserCase cases[] = {
      {"LOAD CSV INTO customer FROM source;", "load_csv", "csv_stream", "csv"},
      {"LOAD XML INTO customer FROM source;", "load_xml", "xml_stream", "xml"},
      {"BULK IMPORT JOB customer FROM source;", "bulk_import_job", "bulk_import_job", "bulk_job"},
      {"INGEST LINE_PROTOCOL INTO customer FROM source;", "ingest_line_protocol", "line_protocol_stream", "line_protocol"},
  };
  for (const auto& test_case : cases) {
    VerifyBulkParserRoute(test_case);
  }
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  VerifyBulkParserRoutes();
  const auto work = MakeTempDir();
  Require(!work.empty(), "MISS-009 failed to create temp directory");
  const auto database_path = work / "miss009.sbdb";

  api::EngineCreateLifecycleRequest create;
  create.context = BaseContext(database_path);
  create.option_envelopes.push_back(std::string("resource_seed_pack_root:") +
                                    SB_MISS009_SEED_PACK_ROOT);
  auto created = api::EngineCreateLifecycle(create);
  Require(created.ok, "MISS-009 lifecycle create database failed");
  CreateSchemaAndTable(database_path);

  auto context = BeginTransaction(database_path, "201");
  VerifyImportPlanning(context);
  VerifyRejectAndCheckpointModels(context);
  VerifyFailFastCopyExecution(context);
  VerifyRejectRowExecution(context);
  VerifyNativeBinaryRowset(context);
  VerifyCatalogArtifactExportImport(context);
  Commit(context);

  std::error_code cleanup_error;
  std::filesystem::remove_all(work, cleanup_error);
  return EXIT_SUCCESS;
}
