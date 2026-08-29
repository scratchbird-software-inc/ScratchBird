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
#include "dml/import_api.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "sblr_opcode_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
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
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000076001";
constexpr std::string_view kSourceUuid = "019f0000-0000-7000-8000-000000076002";
constexpr std::string_view kSeedSchemaUuid = "019f0000-0000-7000-8000-000000076501";
constexpr std::string_view kFamily = "sblr.dml.operation.v3";
constexpr std::string_view kPlanImportPublicAbiProofTarget =
    "sbsql_sblr_alignment_plan_import_rows_sbps_coordination";

struct CaseRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view variant;
  std::string_view runtime_evidence_kind;
  std::string_view runtime_evidence_id;
  int seed_ordinal;
};

const CaseRow kCases[] = {
    {"SBSQL-007B258290FF", "cypher_delete_clause", "CYPHER DELETE NODE person FROM social;", "dml.delete_rows", "SBLR_DML_DELETE_ROWS", "cypher_delete", "mga_row_version", "row_delete_tombstone", 401},
    {"SBSQL-096CE3380900", "graph_delete_node_stmt", "GRAPH DELETE NODE person;", "dml.delete_rows", "SBLR_DML_DELETE_ROWS", "graph_delete_node", "mga_row_version", "row_delete_tombstone", 402},
    {"SBSQL-1E17366B472E", "cypher_merge_action", "CYPHER MERGE NODE person ON MATCH SET touched;", "dml.merge_rows", "SBLR_DML_MERGE_ROWS", "cypher_merge", "merge_surface", "matched_update_or_not_matched_insert", 403},
    {"SBSQL-3254DF8E8CDD", "merge_strategy", "MERGE INTO customer USING staging WHEN MATCHED THEN UPDATE;", "dml.merge_rows", "SBLR_DML_MERGE_ROWS", "merge", "merge_surface", "matched_update_or_not_matched_insert", 404},
    {"SBSQL-38C10BF1D800", "bulk_target_list", "DOCUMENT BULK UPDATE docs TARGET (status,total);", "dml.update_rows", "SBLR_DML_UPDATE_ROWS", "document_bulk", "mga_row_version", "row_update", 405},
    {"SBSQL-3E9BCF3982B5", "graph_delete_edge_stmt", "GRAPH DELETE EDGE knows;", "dml.delete_rows", "SBLR_DML_DELETE_ROWS", "graph_delete_edge", "mga_row_version", "row_delete_tombstone", 406},
    {"SBSQL-4B841304A70D", "doc_bulk_op", "DOCUMENT BULK UPDATE docs TARGET (status,total);", "dml.update_rows", "SBLR_DML_UPDATE_ROWS", "document_bulk", "mga_row_version", "row_update", 407},
    {"SBSQL-5CA04B524AF6", "doc_bulk_stmt", "DOCUMENT BULK UPDATE docs TARGET (status,total);", "dml.update_rows", "SBLR_DML_UPDATE_ROWS", "document_bulk", "mga_row_version", "row_update", 408},
    {"SBSQL-7254347122CB", "gpu_workload_action", "GPU WORKLOAD APPLY batches;", "dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS", "gpu_workload_action", "accepted_executor_evidence", "", 409},
    {"SBSQL-728CB259DD81", "lock_row_for_update", "SELECT id FROM customer FOR UPDATE;", "dml.update_rows", "SBLR_DML_UPDATE_ROWS", "select_for_update", "mga_row_version", "row_update", 410},
    {"SBSQL-93FC08B02C21", "merge_when_clause", "MERGE INTO customer USING staging WHEN MATCHED THEN UPDATE;", "dml.merge_rows", "SBLR_DML_MERGE_ROWS", "merge", "merge_surface", "matched_update_or_not_matched_insert", 411},
    {"SBSQL-95DC04E9EFA1", "doc_update_op", "DOCUMENT UPDATE docs PATH $.status SET VALUE 'closed';", "dml.update_rows", "SBLR_DML_UPDATE_ROWS", "document_path_update", "mga_row_version", "row_update", 412},
    {"SBSQL-A87E3D993D3D", "doc_update_verb", "DOCUMENT UPDATE docs PATH $.status SET VALUE 'closed';", "dml.update_rows", "SBLR_DML_UPDATE_ROWS", "document_path_update", "mga_row_version", "row_update", 413},
    {"SBSQL-B7DCE9CB07B6", "cypher_load_csv", "CYPHER LOAD CSV FROM source INTO social;", "dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS", "cypher_load_csv", "accepted_executor_evidence", "", 414},
    {"SBSQL-BD2510DCAAF9", "doc_path_update_stmt", "DOCUMENT UPDATE docs PATH $.status SET VALUE 'closed';", "dml.update_rows", "SBLR_DML_UPDATE_ROWS", "document_path_update", "mga_row_version", "row_update", 415},
    {"SBSQL-C0FC7EA71693", "cypher_merge_clause", "CYPHER MERGE NODE person ON MATCH SET touched;", "dml.merge_rows", "SBLR_DML_MERGE_ROWS", "cypher_merge", "merge_surface", "matched_update_or_not_matched_insert", 416},
    {"SBSQL-DB993AE8EDBB", "load_data_clause", "LOAD DATA INTO customer FROM source CSV;", "dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS", "load_data", "accepted_executor_evidence", "", 417},
    {"SBSQL-E3C1995D3B54", "doc_update_op_list", "DOCUMENT UPDATE docs PATH $.status SET VALUE 'closed';", "dml.update_rows", "SBLR_DML_UPDATE_ROWS", "document_path_update", "mga_row_version", "row_update", 418},
    {"SBSQL-FACE78497E93", "bulk_target", "DOCUMENT BULK UPDATE docs TARGET (status,total);", "dml.update_rows", "SBLR_DML_UPDATE_ROWS", "document_bulk", "mga_row_version", "row_update", 419},
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_sbsfc_076_dml_residual_exact_route_conformance";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_sbsfc_076_dml_residual_exact_route_conformance");
  Require(configured.ok(), "SBSFC-076 memory fixture configuration failed");
  Require(configured.fixture_mode, "SBSFC-076 memory fixture mode was not active");
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
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

std::string_view ExpectedAdmissionFamily(std::string_view operation_id) {
  if (operation_id == "dml.delete_rows") return "sblr.dml.delete.v3";
  if (operation_id == "dml.merge_rows") return "sblr.dml.merge.v3";
  if (operation_id == "dml.update_rows") return "sblr.dml.update.v3";
  return kFamily;
}

std::string RowUuidFor(const CaseRow& row) {
  return "019f0000-0000-7000-8000-000000076" + std::to_string(row.seed_ordinal);
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000076101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000076102";
  session.database_uuid = "019f0000-0000-7000-8000-000000076103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 76;
  session.security_policy_epoch = 77;
  session.descriptor_epoch = 78;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_076_dml_residual";
  config.parser_uuid = "019f0000-0000-7000-8000-000000076104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-076-dml-residual";
  config.build_id = "sbsql-sbsfc-076-dml-residual";
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
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(kTargetUuid), std::string(kSourceUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const CaseRow& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "SBSFC-076 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-076 generated registry canonical name drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-076 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-076 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == kFamily,
          "SBSFC-076 generated registry SBLR family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-076 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-076 AST failed");
  Require(artifacts.bound.bound, "SBSFC-076 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-076 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily, "SBSFC-076 operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily, "SBSFC-076 operation key mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id, "SBSFC-076 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          "SBSFC-076 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode, "SBSFC-076 opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-076 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-076 parser no-finality authority missing");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-076 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.variant),
          "SBSFC-076 payload missing route variant");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-076 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-076 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-076 payload used forbidden replay/refusal evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-076 payload carried WAL/recovery authority");

  if (row.operation_id == "dml.plan_import_rows") {
    Require(Contains(artifacts.envelope.payload,
                     "\"import_execution_deferred\":true") &&
                Contains(artifacts.envelope.payload,
                         "\"mga_access_model\":\"import_plan_no_row_write\"") &&
                Contains(artifacts.envelope.payload,
                         "\"source_handle_included\":false") &&
                Contains(artifacts.envelope.payload,
                         "\"row_persistence_claimed\":false") &&
                !Contains(artifacts.envelope.payload, "customer") &&
                !Contains(artifacts.envelope.payload, "social") &&
                !Contains(artifacts.envelope.payload, "batches"),
            "SBSFC-076 import semantic demand was incomplete or unredacted");
    const auto* opcode = sblr::LookupSblrOperation("dml.plan_import_rows");
    Require(opcode != nullptr && opcode->code == 793 &&
                opcode->opcode == "SBLR_DML_PLAN_IMPORT_ROWS" &&
                opcode->operand_contract == "import_rows_plan_descriptor" &&
                opcode->result_contract == "import_plan_result" &&
                opcode->executor_evidence_required &&
                opcode->executor_evidence_accepted,
            "SBSFC-076 import parser handoff lost exact 793/v1 identity");
    Require(kPlanImportPublicAbiProofTarget ==
                "sbsql_sblr_alignment_plan_import_rows_sbps_coordination",
            "SBSFC-076 public-ABI proof provenance drifted");
    return;
  }

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(artifacts.envelope));
  Require(admission.admitted, "SBSFC-076 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          std::string("SBSFC-076 server admission did not require public ABI dispatch for ") +
              std::string(row.surface_id));
  Require(admission.operation_id == row.operation_id,
          "SBSFC-076 server admission operation id mismatch");
  Require(admission.operation_family == ExpectedAdmissionFamily(row.operation_id),
          "SBSFC-076 server admission operation family mismatch");

  const auto* opcode = sblr::LookupSblrOperation(std::string(row.operation_id));
  Require(opcode != nullptr, "SBSFC-076 opcode registry row missing");
  Require(opcode->opcode == row.opcode, "SBSFC-076 opcode registry drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_sbsfc_076_dml_residual_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events", ".sb.crud_events", ".sb.name_events",
                            ".sb.transaction_inventory", ".dirty.manifest",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810760000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810760001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810760002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "SBSFC-076 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-076-dml-residual";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000076201";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000076202";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000076203";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.name_resolution_epoch = 1;
  return context;
}

void ApplyRegisteredFixtureEnvelopeIdentity(
    sblr::SblrOperationEnvelope* envelope) {
  Require(envelope != nullptr, "SBSFC-076 fixture envelope pointer was null");
  const auto* operation = sblr::LookupSblrOperation(envelope->operation_id);
  Require(operation != nullptr && operation->code != 0,
          "SBSFC-076 fixture operation lacked numeric identity");
  envelope->opcode = operation->opcode;
  envelope->opcode_code = operation->code;
  envelope->parser_package_uuid = std::string(kSourceUuid);
  envelope->registry_snapshot_uuid = std::string(kSeedSchemaUuid);
  envelope->parser_resolved_names_to_uuids = true;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.sbsfc076.transaction.begin");
  ApplyRegisteredFixtureEnvelopeIdentity(&envelope);
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const auto result = sblr::DispatchSblrOperation({context, envelope, api::EngineApiRequest{}});
  Require(result.envelope_validated, "SBSFC-076 transaction begin envelope rejected");
  Require(result.accepted, "SBSFC-076 transaction begin not accepted");
  Require(result.api_result.ok, "SBSFC-076 transaction begin failed");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = context.local_transaction_id;
  return context;
}

api::EngineLocalizedName Name(std::string value) {
  return {"en", "primary", "", std::move(value), true};
}

api::EngineColumnDefinition TextColumn(std::uint32_t ordinal,
                                       std::string name,
                                       std::string uuid_text) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical = std::move(uuid_text);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  column.ordinal = ordinal;
  column.nullable = true;
  return column;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue SeedRowFor(const CaseRow& row) {
  api::EngineRowValue value;
  value.requested_row_uuid.canonical = RowUuidFor(row);
  value.fields.push_back({"id", TextValue(std::string(row.surface_id))});
  value.fields.push_back({"status", TextValue("open")});
  value.fields.push_back({"note", TextValue(std::string(row.canonical_name))});
  return value;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

void SeedDmlTargetTableAndRows(const api::EngineRequestContext& context,
                               bool seed_rows = true) {
  api::EngineApiRequest schema_request;
  schema_request.target_object.uuid.canonical = std::string(kSeedSchemaUuid);
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("sbsfc076"));
  auto create_schema = sblr::MakeSblrEnvelope("ddl.create_schema",
                                              "SBLR_DDL_CREATE_SCHEMA",
                                              "trace.sbsfc076.seed_schema");
  ApplyRegisteredFixtureEnvelopeIdentity(&create_schema);
  create_schema.requires_security_context = true;
  create_schema.requires_transaction_context = true;
  create_schema.contains_sql_text = false;
  const auto schema_result =
      sblr::DispatchSblrOperation({context, create_schema, schema_request});
  PrintDispatchDiagnostics(schema_result);
  Require(schema_result.envelope_validated, "SBSFC-076 seed schema envelope rejected");
  Require(schema_result.accepted, "SBSFC-076 seed schema dispatch rejected");
  Require(schema_result.dispatched_to_api, "SBSFC-076 seed schema did not dispatch");
  Require(schema_result.api_result.ok, "SBSFC-076 seed schema create failed");

  api::EngineApiRequest table_request;
  table_request.target_schema.uuid.canonical = std::string(kSeedSchemaUuid);
  table_request.target_schema.object_kind = "schema";
  table_request.target_object.uuid.canonical = std::string(kTargetUuid);
  table_request.target_object.object_kind = "table";
  table_request.localized_names.push_back(Name("sbsfc076_dml_residual_target"));
  table_request.columns.push_back(
      TextColumn(0, "id", "019f0000-0000-7000-8000-000000076502"));
  table_request.columns.push_back(
      TextColumn(1, "status", "019f0000-0000-7000-8000-000000076503"));
  table_request.columns.push_back(
      TextColumn(2, "note", "019f0000-0000-7000-8000-000000076504"));

  auto create_table = sblr::MakeSblrEnvelope("ddl.create_table",
                                             "SBLR_DDL_CREATE_TABLE",
                                             "trace.sbsfc076.seed_table");
  ApplyRegisteredFixtureEnvelopeIdentity(&create_table);
  create_table.requires_security_context = true;
  create_table.requires_transaction_context = true;
  create_table.contains_sql_text = false;
  const auto table_result =
      sblr::DispatchSblrOperation({context, create_table, table_request});
  PrintDispatchDiagnostics(table_result);
  Require(table_result.envelope_validated, "SBSFC-076 seed table envelope rejected");
  Require(table_result.accepted, "SBSFC-076 seed table dispatch rejected");
  Require(table_result.dispatched_to_api, "SBSFC-076 seed table did not dispatch");
  Require(table_result.api_result.ok, "SBSFC-076 seed table create failed");
  if (!seed_rows) return;

  api::EngineApiRequest insert_request;
  insert_request.target_object.uuid.canonical = std::string(kTargetUuid);
  insert_request.target_object.object_kind = "table";
  for (const auto& row : kCases) {
    insert_request.rows.push_back(SeedRowFor(row));
  }
  auto insert = sblr::MakeSblrEnvelope("dml.insert_rows",
                                       "SBLR_DML_INSERT_ROWS",
                                       "trace.sbsfc076.seed_rows");
  insert.requires_security_context = true;
  insert.requires_transaction_context = true;
  insert.contains_sql_text = false;
  insert.parser_resolved_names_to_uuids = true;
  insert.operands.push_back({"text", "result_payload_policy", "full_payload"});
  const auto insert_result =
      sblr::DispatchSblrOperation({context, insert, insert_request});
  PrintDispatchDiagnostics(insert_result);
  Require(insert_result.envelope_validated, "SBSFC-076 seed insert envelope rejected");
  Require(insert_result.accepted, "SBSFC-076 seed insert dispatch rejected");
  Require(insert_result.dispatched_to_api, "SBSFC-076 seed insert did not dispatch");
  Require(insert_result.api_result.ok, "SBSFC-076 seed insert failed");
  Require(insert_result.api_result.result_shape.rows.size() ==
              sizeof(kCases) / sizeof(kCases[0]),
          "SBSFC-076 seed insert row count mismatch");
}

std::string PlanFixtureUuid(std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(
      UuidKind::object, 1779810761000ull + salt);
  Require(generated.ok(), "SBSFC-076 plan UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

api::EngineRequestContext AttachPlanStatementAuthority(
    api::EngineRequestContext context) {
  const auto salt = context.local_transaction_id * 32;
  context.statement_uuid.canonical = PlanFixtureUuid(salt + 1);
  context.statement_snapshot_uuid.canonical.clear();
  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = context;
  const auto snapshot = api::EnginePublishStatementSnapshot(publish);
  Require(snapshot.ok, "SBSFC-076 statement snapshot publication failed");
  context.statement_snapshot_uuid = snapshot.statement_snapshot_uuid;
  context.statement_snapshot_generation =
      snapshot.snapshot_vector.publication_inventory_next_local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_receipt_uuid.canonical = PlanFixtureUuid(salt + 2);
  context.statement_metadata_snapshot_uuid.canonical = PlanFixtureUuid(salt + 3);
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_metadata_snapshot_active_excluded_local_transaction_ids =
      snapshot.snapshot_vector.active_excluded_local_transaction_ids;
  context.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      snapshot.snapshot_vector.in_doubt_excluded_local_transaction_ids;
  context.transaction_policy_snapshot_uuid.canonical = PlanFixtureUuid(salt + 4);
  context.transaction_policy_snapshot_generation = 1;
  context.resource_admission_uuid.canonical = PlanFixtureUuid(salt + 5);

  auto& authorization = context.authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical = PlanFixtureUuid(salt + 6);
  authorization.security_context_generation = 1;
  authorization.principal_uuid = context.principal_uuid;
  authorization.security_epoch = context.security_epoch;
  authorization.policy_epoch = 1;
  authorization.catalog_generation_id = context.catalog_generation_id;
  authorization.effective_subjects.clear();
  authorization.effective_subjects.push_back(
      {context.principal_uuid, "principal"});
  authorization.grants.clear();
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = PlanFixtureUuid(salt + 7);
  grant.subject_uuid = context.principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = std::string(kTargetUuid);
  grant.right = "INSERT";
  grant.security_epoch = context.security_epoch;
  authorization.grants.push_back(std::move(grant));
  return context;
}

api::SblrExecutorAvailabilityRowIdentity PlanAvailabilityIdentity() {
  return {api::kSblrDmlPlanImportRowsExecutorId,
          api::kSblrDmlPlanImportRowsOpcodeCode,
          api::kSblrDmlPlanImportRowsOpcodeVersion,
          api::kSblrDmlPlanImportRowsOperandDescriptorId,
          api::kSblrDmlPlanImportRowsResultDescriptorId,
          api::kSblrDmlPlanImportRowsResultDescriptorVersion};
}

sblr::SblrOperationEnvelope ExactPlanEnvelope(
    const sblr::PlanImportRowsDescriptorRefV1& descriptor_ref,
    std::string_view trace_suffix) {
  std::vector<std::uint8_t> encoded_ref;
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  Require(sblr::EncodePlanImportRowsDescriptorRefV1(
              descriptor_ref, &encoded_ref, &diagnostic) &&
              encoded_ref.size() == sblr::kPlanImportRowsDescriptorRefBytesV1,
          "SBSFC-076 exact descriptor ref encoding failed");
  auto envelope = sblr::MakeSblrEnvelope(
      "dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS",
      "trace.sbsfc076.plan_import_rows." + std::string(trace_suffix));
  envelope.opcode_code = 793;
  envelope.operation_version_major = 1;
  envelope.operation_version_minor = 0;
  envelope.result_shape = "import_plan_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = std::string(kSourceUuid);
  envelope.registry_snapshot_uuid = std::string(kSeedSchemaUuid);
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "import_rows_plan_descriptor";
  operand.name = "request";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body = std::move(encoded_ref);
  envelope.operands.push_back(std::move(operand));
  return envelope;
}

void RequireExactPlanImportDispatch(const api::EngineRequestContext& context,
                                    const CaseRow& row) {
  auto binder_context = context;
  binder_context.trace_tags = {"private_dml_plan_import_rows_binder"};
  api::EngineCreateImportRowsPlanDescriptorRequestV1 bind;
  bind.context = binder_context;
  bind.structural_occurrence_id = 1;
  bind.target_table_uuid.canonical = std::string(kTargetUuid);
  bind.source_kind = sblr::PlanImportRowsSourceKindV1::native_sbsql_import;
  bind.source_fingerprint_present = false;
  bind.mappings.clear();
  bind.format_family = sblr::PlanImportRowsFormatFamilyV1::csv;
  bind.reject_mode = sblr::PlanImportRowsRejectModeV1::fail_fast;
  bind.reject_payload_policy =
      sblr::PlanImportRowsRejectPayloadPolicyV1::diagnostic_only;
  bind.resume_policy = sblr::PlanImportRowsResumePolicyV1::fail_closed;
  bind.strict_bulk_load_requested = false;
  bind.reference_relaxed_semantics_requested = false;
  bind.reference_relaxed_semantics_authorized = false;
  bind.reject_limit_ppm = 0;
  bind.reject_limit_rows = 0;
  const auto bound =
      api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(bind);
  if (!bound.ok) {
    std::cerr << bound.diagnostic.code << ':' << bound.diagnostic.detail << '\n';
  }
  Require(bound.ok, "SBSFC-076 exact import binder failed");

  auto consumer_context = context;
  consumer_context.trace_tags = {"private_dml_plan_import_rows_consumer"};
  sblr::SblrDispatchRequest request;
  request.context = consumer_context;
  request.envelope = ExactPlanEnvelope(bound.descriptor_ref, row.surface_id);
  request.standalone_package_root = true;
  Require(request.envelope.opcode_code == 793 &&
              request.envelope.operation_version_major == 1 &&
              request.envelope.operation_version_minor == 0 &&
              request.envelope.result_shape == "import_plan_result" &&
              request.envelope.operands.size() == 1 &&
              request.envelope.operands.front().ordinal == 1 &&
              request.envelope.operands.front().type ==
                  "import_rows_plan_descriptor" &&
              request.envelope.operands.front().name == "request" &&
              request.envelope.operands.front().value.empty() &&
              request.envelope.operands.front().value_body.size() == 24,
          "SBSFC-076 exact 793/v1 descriptor-ref envelope drifted");
  const std::string opaque_operand(
      request.envelope.operands.front().value_body.begin(),
      request.envelope.operands.front().value_body.end());
  Require(!Contains(opaque_operand, kTargetUuid) &&
              !Contains(opaque_operand, row.sql) &&
              !Contains(opaque_operand, row.canonical_name),
          "SBSFC-076 descriptor-ref operand leaked UUID/name/SQL text");

  const auto result = sblr::DispatchSblrOperation(request);
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated && result.accepted &&
              result.dispatched_to_api && result.api_result.ok &&
              result.plan_import_rows_result.has_value(),
          "SBSFC-076 exact typed plan-import dispatch failed");
  const auto& plan = *result.plan_import_rows_result;
  Require(plan.surface_accepted && plan.planning_only &&
              plan.execution_requires_execute_import_rows &&
              !plan.row_execution_completed && !plan.row_persistence_claimed &&
              plan.normalized_insert_mode_code ==
                  static_cast<std::uint16_t>(
                      sblr::PlanImportRowsInsertModeV1::copy_import) &&
              plan.normalized_source_kind_code ==
                  static_cast<std::uint16_t>(
                      sblr::PlanImportRowsSourceKindV1::native_sbsql_import) &&
              plan.normalized_format_family_code ==
                  static_cast<std::uint16_t>(
                      sblr::PlanImportRowsFormatFamilyV1::csv) &&
              plan.mapped_column_count == 0 &&
              plan.validated_request_descriptor_generation ==
                  bound.descriptor_ref.descriptor_generation &&
              plan.accepted_executor_evidence.exact_bytes.size() ==
                  sblr::kPlanImportRowsExecutorEvidenceBytesV1 &&
              plan.accepted_executor_evidence.request_descriptor_uuid ==
                  bound.descriptor_ref.descriptor_uuid &&
              plan.accepted_executor_evidence.request_descriptor_generation ==
                  bound.descriptor_ref.descriptor_generation &&
              plan.accepted_executor_evidence.completed_validation_bits ==
                  sblr::kPlanImportRowsAcceptedValidationBitsV1 &&
              HasEvidence(plan, row.runtime_evidence_kind,
                          row.runtime_evidence_id),
          "SBSFC-076 exact planning result/evidence drifted");
  Require(kPlanImportPublicAbiProofTarget ==
              "sbsql_sblr_alignment_plan_import_rows_sbps_coordination",
          "SBSFC-076 public-ABI proof provenance drifted");
  const auto released =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(binder_context);
  Require(released.ok && released.released_row_count == 1,
          "SBSFC-076 exact import descriptor release failed");
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "trace.sbsfc076.dml_residual");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "table"});
  envelope.operands.push_back({"text", "dml_surface_variant", std::string(row.variant)});
  envelope.operands.push_back({"text", "sbsfc076_surface_id", std::string(row.surface_id)});
  if (row.operation_id == "dml.update_rows" || row.operation_id == "dml.delete_rows" ||
      row.operation_id == "dml.merge_rows") {
    envelope.operands.push_back({"predicate", "row_uuid_match", RowUuidFor(row)});
  }
  if (row.operation_id == "dml.update_rows" || row.operation_id == "dml.merge_rows") {
    envelope.operands.push_back({"assignment", "status", "closed"});
  }
  if (row.operation_id == "dml.merge_rows") {
    envelope.operands.push_back({"row_field:text", RowUuidFor(row) + "|status", "merged"});
    envelope.operands.push_back({"row_field:text", RowUuidFor(row) + "|note",
                                 std::string(row.canonical_name)});
  }
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "table";
  request.option_envelopes.push_back("source_kind:native_sbsql_import");
  request.option_envelopes.push_back("format_family:csv");
  request.option_envelopes.push_back(std::string("sbsfc076_surface_id:") + std::string(row.surface_id));
  return request;
}

void RequireEngineDispatch(const api::EngineRequestContext& context, const CaseRow& row) {
  if (row.operation_id == "dml.plan_import_rows") {
    RequireExactPlanImportDispatch(context, row);
    return;
  }
  const auto result = sblr::DispatchSblrOperation({context, EngineEnvelope(row), ApiRequestFor(row)});
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "SBSFC-076 engine envelope rejected");
  Require(result.accepted, "SBSFC-076 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-076 engine did not dispatch to API");
  Require(result.api_result.operation_id == row.operation_id,
          "SBSFC-076 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-076 runtime API did not complete");
  Require(HasEvidence(result.api_result, row.runtime_evidence_kind, row.runtime_evidence_id),
          "SBSFC-076 runtime evidence missing");
  Require(!result.api_result.result_shape.rows.empty(),
          "SBSFC-076 row DML did not return affected row evidence");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 1 ||
              (argc == 2 &&
               std::string_view(argv[1]) == "--plan-import-only"),
          "usage: sbsql_sbsfc_076_dml_residual_exact_route_conformance "
          "[--plan-import-only]");
  const bool plan_import_only = argc == 2;
  ConfigureMemoryFixture();
  for (const auto& row : kCases) {
    if (plan_import_only && row.operation_id != "dml.plan_import_rows") {
      continue;
    }
    RequireRegistryEvidence(row);
    RequireExactLowering(row, RunPipeline(row));
  }

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginEngineTransaction(path, database_uuid);
  SeedDmlTargetTableAndRows(context, !plan_import_only);
  const auto plan_context = AttachPlanStatementAuthority(context);
  const auto installed = api::LoadSblrExecutorAvailabilitySnapshot(
      plan_context, PlanAvailabilityIdentity());
  Require(installed.ok && installed.snapshot.installed &&
              installed.snapshot.generation != 0,
          "SBSFC-076 plan executor availability bootstrap failed");
  const auto current = api::LoadCurrentSblrExecutorAvailabilitySnapshot(
      plan_context, PlanAvailabilityIdentity());
  Require(current.ok && current.snapshot.installed &&
              current.snapshot.generation == installed.snapshot.generation,
          "SBSFC-076 current plan executor availability missing");
  for (const auto& row : kCases) {
    if (plan_import_only && row.operation_id != "dml.plan_import_rows") {
      continue;
    }
    RequireEngineDispatch(row.operation_id == "dml.plan_import_rows"
                              ? plan_context
                              : context,
                          row);
  }
  RemoveDatabaseArtifacts(path);

  std::cout << "plan_import_public_abi_proof="
            << kPlanImportPublicAbiProofTarget << '\n';
  if (plan_import_only) {
    std::cout << "sbsql_sbsfc_076_dml_residual_exact_route_conformance."
                 "plan_import=passed\n";
    return EXIT_SUCCESS;
  }
  std::cout << "sbsql_sbsfc_076_dml_residual_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
