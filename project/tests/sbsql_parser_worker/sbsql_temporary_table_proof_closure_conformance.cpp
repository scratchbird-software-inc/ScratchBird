// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "backup_archive/backup_archive_api.hpp"
#include "binder/binder.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/sys_information_projection.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/merge_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "lowering/lowering.hpp"
#include "management/support_bundle_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include "../release/public_release_authz_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kDatabasePathName =
    "sbsql_temporary_table_proof_closure_conformance.sbdb";
constexpr std::string_view kRecoveryDatabasePathName =
    "sbsql_temporary_table_recovery_conformance.sbdb";
constexpr std::string_view kBackupDatabasePathName =
    "sbsql_temporary_table_backup_exclusion_conformance.sbdb";
constexpr std::string_view kTableUuid =
    "019f0000-0000-7000-8000-000000440101";
constexpr std::string_view kColumnUuid =
    "019f0000-0000-7000-8000-000000440102";
constexpr std::string_view kDurableShadowTableUuid =
    "019f0000-0000-7000-8000-000000440301";
constexpr std::string_view kTemporaryShadowTableUuid =
    "019f0000-0000-7000-8000-000000440302";
constexpr std::string_view kTemporaryPrivateOnlyTableUuid =
    "019f0000-0000-7000-8000-000000440303";
constexpr std::string_view kRollbackTableUuid =
    "019f0000-0000-7000-8000-000000440304";
constexpr std::string_view kRollbackCreatedTableUuid =
    "019f0000-0000-7000-8000-000000440305";
constexpr std::string_view kTemporaryIndexedTableUuid =
    "019f0000-0000-7000-8000-000000440307";
constexpr std::string_view kTemporaryLargeValueTableUuid =
    "019f0000-0000-7000-8000-000000440308";
constexpr std::string_view kTemporaryIndexUuid =
    "019f0000-0000-7000-8000-000000440601";
constexpr std::string_view kTemporaryDropTableUuid =
    "019f0000-0000-7000-8000-000000440309";
constexpr std::string_view kTemporaryLargeValueIdColumnUuid =
    "019f0000-0000-7000-8000-000000440701";
constexpr std::string_view kTemporaryLargeValuePayloadColumnUuid =
    "019f0000-0000-7000-8000-000000440702";
constexpr std::string_view kTemporaryRecoveryGlobalTableUuid =
    "019f0000-0000-7000-8000-000000440309";
constexpr std::string_view kTemporaryRecoveryPrivateTableUuid =
    "019f0000-0000-7000-8000-000000440310";
constexpr std::string_view kTemporaryRecoveryColumnUuid =
    "019f0000-0000-7000-8000-000000440801";
constexpr std::string_view kBackupDurableTableUuid =
    "019f0000-0000-7000-8000-000000440321";
constexpr std::string_view kBackupDurableColumnUuid =
    "019f0000-0000-7000-8000-000000440822";
constexpr std::string_view kBackupGlobalTempTableUuid =
    "019f0000-0000-7000-8000-000000440323";
constexpr std::string_view kBackupGlobalTempColumnUuid =
    "019f0000-0000-7000-8000-000000440824";
constexpr std::string_view kBackupPrivateTempTableUuid =
    "019f0000-0000-7000-8000-000000440325";
constexpr std::string_view kBackupPrivateTempColumnUuid =
    "019f0000-0000-7000-8000-000000440826";
constexpr std::string_view kBackupTempIndexUuid =
    "019f0000-0000-7000-8000-000000440827";
constexpr std::string_view kBackupDurableRowUuid =
    "019f0000-0000-7000-8000-000000440831";
constexpr std::string_view kBackupGlobalTempRowUuid =
    "019f0000-0000-7000-8000-000000440832";
constexpr std::string_view kBackupPrivateTempRowUuid =
    "019f0000-0000-7000-8000-000000440833";

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

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

std::string HexEncode(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(text.size() * 2);
  for (unsigned char c : text) {
    encoded.push_back(kHex[(c >> 4) & 0x0f]);
    encoded.push_back(kHex[c & 0x0f]);
  }
  return encoded;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view fragment) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        evidence.evidence_id.find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasDiagnosticCode(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string FirstDetail(const api::EngineApiResult& result) {
  if (result.diagnostics.empty()) return {};
  return result.diagnostics.front().detail;
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

void PrintApiDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

std::string ApiResultText(const api::EngineApiResult& result) {
  std::ostringstream out;
  out << result.operation_id << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    out << diagnostic.code << ':' << diagnostic.message_key << ':'
        << diagnostic.detail << '\n';
  }
  for (const auto& evidence : result.evidence) {
    out << evidence.evidence_kind << ':' << evidence.evidence_id << '\n';
  }
  for (const auto& row : result.result_shape.rows) {
    out << row.requested_row_uuid.canonical << '\n';
    for (const auto& [field_name, value] : row.fields) {
      out << field_name << '=' << value.encoded_value << '\n';
    }
  }
  return out.str();
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000440001";
  config.bundle_contract_id = "sbp_sbsql@temporary-table-proof";
  config.build_id = "sbsql-temporary-table-proof-closure";
  return config;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000440003";
  session.connection_uuid = "019f0000-0000-7000-8000-000000440004";
  session.database_uuid = "019f0000-0000-7000-8000-000000440005";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 440;
  session.security_policy_epoch = 441;
  session.descriptor_epoch = 442;
  return session;
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

PipelineArtifacts RunPipelineWithResolvedObjectUuids(
    std::string_view sql,
    const std::vector<std::string>& resolved_object_uuids) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            resolved_object_uuids);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireAcceptedTemporarySql(std::string_view sql,
                                 std::string_view scope,
                                 std::string_view on_commit,
                                 bool clause_present) {
  const auto artifacts = RunPipeline(sql);
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "TEMP-TABLE-GATE-001 CST rejected supported shape");
  Require(!artifacts.ast.messages.has_errors(), "TEMP-TABLE-GATE-001 AST rejected supported shape");
  Require(artifacts.bound.bound, "TEMP-TABLE-GATE-001 bind rejected supported shape");
  Require(artifacts.verifier.admitted, "TEMP-TABLE-GATE-002 verifier rejected temporary table route");
  Require(artifacts.envelope.operation_id == "ddl.create_table",
          "TEMP-TABLE-GATE-002 operation id drifted");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DDL_CREATE_TABLE",
          "TEMP-TABLE-GATE-002 opcode drifted");
  Require(!artifacts.envelope.parser_executes_sql,
          "TEMP-TABLE-GATE-002 parser claimed SQL execution authority");
  Require(!artifacts.envelope.real_file_effects,
          "TEMP-TABLE-GATE-002 parser claimed file/storage side effects");
  Require(Contains(artifacts.envelope.payload, "\"temporary_table\":true"),
          "TEMP-TABLE-GATE-002 payload missing temporary table flag");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"temporary_scope\":\"") + std::string(scope) + "\""),
          "TEMP-TABLE-GATE-002 payload missing temporary scope");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"temporary_metadata_scope\":\"") +
                       std::string(scope) + "\""),
          "TEMP-TABLE-GATE-002 payload missing metadata scope");
  Require(Contains(artifacts.envelope.payload, "\"temporary_data_scope\":\"session\""),
          "TEMP-TABLE-GATE-002 payload missing temporary data scope");
  Require(Contains(artifacts.envelope.payload, "\"temporary_session_uuid_required\":true"),
          "TEMP-TABLE-GATE-002 payload missing session UUID requirement");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"on_commit_action\":\"") + std::string(on_commit) + "\""),
          "TEMP-TABLE-GATE-002 payload missing on-commit action");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"temporary_on_commit_clause_present\":") +
                       (clause_present ? "true" : "false")),
          "TEMP-TABLE-GATE-002 payload on-commit clause evidence drifted");
  Require(Contains(artifacts.envelope.payload, "SBSQL-D9366D02D5DE") &&
              Contains(artifacts.envelope.payload, "SBSQL-60FF8D790ABF") &&
              Contains(artifacts.envelope.payload, "SBSQL-D57F1C8B14EF"),
          "TEMP-TABLE-GATE-002 persistence surface evidence missing");
  Require(!Contains(artifacts.envelope.payload, std::string(sql)),
          "TEMP-TABLE-GATE-002 payload embedded SQL text as authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "TEMP-TABLE-GATE-003 server admission rejected route");
  Require(admission.requires_public_abi_dispatch,
          "TEMP-TABLE-GATE-003 server admission skipped public ABI dispatch");
  Require(admission.operation_id == "ddl.create_table",
          "TEMP-TABLE-GATE-003 server admission operation drifted");
}

void RequireRejectedTemporarySql(std::string_view sql) {
  const auto artifacts = RunPipeline(sql);
  Require(!artifacts.verifier.admitted || artifacts.envelope.messages.has_errors(),
          "TEMP-TABLE-GATE-001 unsupported temporary shape was admitted");
}

void RequireDropTableExactLoweringForTemporaryCleanup() {
  const auto artifacts = RunPipelineWithResolvedObjectUuids(
      "DROP TABLE session_customer;",
      {std::string(kTableUuid), "019f0000-0000-7000-8000-000000440012"});
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(),
          "TEMP-TABLE-GATE-015 DROP TABLE CST failed");
  Require(!artifacts.ast.messages.has_errors(),
          "TEMP-TABLE-GATE-015 DROP TABLE AST failed");
  Require(artifacts.bound.bound,
          "TEMP-TABLE-GATE-015 DROP TABLE bind failed");
  Require(artifacts.verifier.admitted,
          "TEMP-TABLE-GATE-015 DROP TABLE verifier rejected exact route");
  Require(artifacts.envelope.operation_id == "ddl.drop_object",
          "TEMP-TABLE-GATE-015 DROP TABLE operation id drifted");
  Require(artifacts.envelope.engine_api_operation_id == "ddl.drop_object",
          "TEMP-TABLE-GATE-015 DROP TABLE engine API operation id drifted");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DDL_DROP_OBJECT",
          "TEMP-TABLE-GATE-015 DROP TABLE opcode drifted");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_drop_object_api_required"),
          "TEMP-TABLE-GATE-015 DROP TABLE API authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.name_registry_retirement_required"),
          "TEMP-TABLE-GATE-015 DROP TABLE name registry authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "TEMP-TABLE-GATE-015 DROP TABLE parser no-SQL authority missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"drop_object_ddl\""),
          "TEMP-TABLE-GATE-015 DROP TABLE missing drop-object payload");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_authority\":\"sys.catalog.table\""),
          "TEMP-TABLE-GATE-015 DROP TABLE missing table catalog authority");
  Require(Contains(artifacts.envelope.payload,
                   "\"target_object_kind\":\"table\""),
          "TEMP-TABLE-GATE-015 DROP TABLE missing target table kind");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"drop_target_uuid\":\"") +
                       std::string(kTableUuid) + "\""),
          "TEMP-TABLE-GATE-015 DROP TABLE missing target UUID");
  Require(Contains(artifacts.envelope.payload,
                   "\"name_registry_retirement_required\":true"),
          "TEMP-TABLE-GATE-015 DROP TABLE missing retirement evidence");
  Require(Contains(artifacts.envelope.payload,
                   "\"parser_executes_sql\":false"),
          "TEMP-TABLE-GATE-015 DROP TABLE parser SQL execution drifted");
  Require(Contains(artifacts.envelope.payload,
                   "\"target_name_text_included\":false") &&
              Contains(artifacts.envelope.payload,
                       "\"sql_text_included\":false"),
          "TEMP-TABLE-GATE-015 DROP TABLE carried SQL/name text authority");
  Require(!Contains(artifacts.envelope.payload, "session_customer") &&
              !Contains(artifacts.envelope.payload, "DROP TABLE"),
          "TEMP-TABLE-GATE-015 DROP TABLE embedded SQL/name text as authority");
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() / std::string(kDatabasePathName);
}

std::filesystem::path TestRecoveryDatabasePath() {
  return std::filesystem::temp_directory_path() /
         std::string(kRecoveryDatabasePathName);
}

std::filesystem::path TestBackupDatabasePath() {
  return std::filesystem::temp_directory_path() /
         std::string(kBackupDatabasePathName);
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
                            ".sb.mga_relation_metadata",
                            ".sb.mga_relation_descriptors",
                            ".sb.mga_row_versions",
                            ".sb.mga_index_entries",
                            ".sb.mga_large_values",
                            ".sb.mga_savepoints",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779814400000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779814400001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779814400002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "TEMP-TABLE-GATE-004 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

std::string MinimalDatabaseFilespaceUuid() {
  const auto generated =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779814400001);
  Require(generated.ok(),
          "TEMP-TABLE-GATE-014 filespace UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid,
                                        std::string session_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-temporary-table-proof-closure";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = std::move(session_uuid);
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000440011";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000440012";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("right:DML_MUTATE");
  context.trace_tags.push_back("SBSQL-TEMPORARY-TABLE-PROOF-CLOSURE");
  return context;
}

api::EngineRequestContext BeginTransaction(api::EngineRequestContext context) {
  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  auto result = api::EngineBeginTransaction(begin);
  if (!result.ok) { PrintApiDiagnostics(result); }
  Require(result.ok && result.local_transaction_id != 0,
          "TEMP-TABLE-GATE-006 transaction begin failed");
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  return context;
}

api::EngineRequestContext BackupEngineContext(
    const std::filesystem::path& path,
    const std::string& database_uuid,
    std::string session_uuid,
    std::string_view request_id) {
  auto context = EngineContext(path, database_uuid, std::move(session_uuid));
  context.request_id = std::string(request_id);
  context.trace_tags.push_back("right:BACKUP_CREATE");
  context.trace_tags.push_back("right:BACKUP_CONTROL");
  scratchbird::tests::release::GrantMaterializedRights(
      &context, {"BACKUP_CREATE", "BACKUP_CONTROL"});
  return BeginTransaction(std::move(context));
}

api::EngineCreateTableRequest TemporaryCreateTableRequest(
    const api::EngineRequestContext& context,
    std::string_view on_commit = "delete_rows",
    std::string_view table_uuid = kTableUuid,
    std::string_view table_name = "session_customer",
    std::string_view column_uuid = kColumnUuid,
    std::string_view scope = "private") {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = context.current_schema_uuid.canonical;
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid.canonical = std::string(table_uuid);
  request.table_names.push_back({"en", "primary", "", std::string(table_name), true});
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical = std::string(column_uuid);
  column.names.push_back({"en", "primary", "", "id", true});
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "int";
  column.descriptor.encoded_descriptor = "type=int";
  column.ordinal = 0;
  column.nullable = true;
  request.table_columns.push_back(std::move(column));
  request.option_envelopes.push_back("temporary:true");
  request.option_envelopes.push_back(std::string("temporary_scope:") +
                                     std::string(scope));
  request.option_envelopes.push_back(std::string("on_commit:") + std::string(on_commit));
  return request;
}

api::EngineCreateTableRequest TemporaryPayloadCreateTableRequest(
    const api::EngineRequestContext& context,
    std::string_view on_commit,
    std::string_view table_uuid,
    std::string_view table_name,
    std::string_view scope) {
  auto request = TemporaryCreateTableRequest(context,
                                             on_commit,
                                             table_uuid,
                                             table_name,
                                             kTemporaryLargeValueIdColumnUuid,
                                             scope);
  api::EngineColumnDefinition payload;
  payload.requested_column_uuid.canonical =
      std::string(kTemporaryLargeValuePayloadColumnUuid);
  payload.names.push_back({"en", "primary", "", "payload", true});
  payload.descriptor.descriptor_kind = "scalar";
  payload.descriptor.canonical_type_name = "text";
  payload.descriptor.encoded_descriptor = "type=text";
  payload.ordinal = 1;
  payload.nullable = true;
  request.table_columns.push_back(std::move(payload));
  return request;
}

api::EngineCreateTableRequest DurableCreateTableRequest(
    const api::EngineRequestContext& context,
    std::string_view table_uuid,
    std::string_view table_name,
    std::string_view column_uuid) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = context.current_schema_uuid.canonical;
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid.canonical = std::string(table_uuid);
  request.table_names.push_back({"en", "primary", "", std::string(table_name), true});
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical = std::string(column_uuid);
  column.names.push_back({"en", "primary", "", "id", true});
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "int";
  column.descriptor.encoded_descriptor = "type=int";
  column.ordinal = 0;
  column.nullable = true;
  request.table_columns.push_back(std::move(column));
  return request;
}

api::EngineTypedValue IntValue(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int";
  typed.descriptor.encoded_descriptor = "type=int";
  typed.encoded_value = std::to_string(value);
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

api::EngineRowValue Row(std::string row_uuid, std::int64_t id) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", IntValue(id)});
  return row;
}

api::EngineRowValue PayloadRow(std::string row_uuid,
                               std::int64_t id,
                               std::string payload) {
  auto row = Row(std::move(row_uuid), id);
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

api::EngineInsertRowsResult InsertRow(const api::EngineRequestContext& context,
                                      std::string_view table_uuid,
                                      std::string row_uuid,
                                      std::int64_t id) {
  api::EngineInsertRowsRequest insert;
  insert.context = context;
  insert.target_table.uuid.canonical = std::string(table_uuid);
  insert.target_table.object_kind = "table";
  insert.input_rows = {Row(std::move(row_uuid), id)};
  insert.require_generated_row_uuid = false;
  return api::EngineInsertRows(insert);
}

api::EngineInsertRowsResult InsertPayloadRow(
    const api::EngineRequestContext& context,
    std::string_view table_uuid,
    std::string row_uuid,
    std::int64_t id,
    std::string payload,
    bool force_large_value) {
  api::EngineInsertRowsRequest insert;
  insert.context = context;
  insert.target_table.uuid.canonical = std::string(table_uuid);
  insert.target_table.object_kind = "table";
  insert.input_rows = {PayloadRow(std::move(row_uuid), id, std::move(payload))};
  insert.require_generated_row_uuid = false;
  if (force_large_value) {
    insert.option_envelopes.push_back("large_value.force_toast=true");
  }
  return api::EngineInsertRows(insert);
}

api::EngineInsertRowsResult InsertRow(const api::EngineRequestContext& context,
                                      std::int64_t id) {
  return InsertRow(context,
                   kTableUuid,
                   "019f0000-0000-7000-8000-000000440201",
                   id);
}

api::EngineSelectRowsResult SelectRows(const api::EngineRequestContext& context,
                                       std::string_view table_uuid) {
  api::EngineSelectRowsRequest select;
  select.context = context;
  select.source_object.uuid.canonical = std::string(table_uuid);
  select.source_object.object_kind = "table";
  return api::EngineSelectRows(select);
}

api::EngineSelectRowsResult SelectRows(const api::EngineRequestContext& context) {
  return SelectRows(context, kTableUuid);
}

api::EnginePredicateEnvelope IdPredicate(std::int64_t id) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = "id";
  predicate.bound_values.push_back(IntValue(id));
  return predicate;
}

api::EngineSelectRowsResult SelectRowsById(const api::EngineRequestContext& context,
                                           std::string_view table_uuid,
                                           std::int64_t id) {
  api::EngineSelectRowsRequest select;
  select.context = context;
  select.source_object.uuid.canonical = std::string(table_uuid);
  select.source_object.object_kind = "table";
  select.select_predicate = IdPredicate(id);
  return api::EngineSelectRows(select);
}

api::EngineIndexDefinition UniqueIdIndexDefinition(std::string_view index_uuid,
                                                   std::string_view index_name) {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = std::string(index_uuid);
  index.names.push_back({"en", "primary", "", std::string(index_name), true});
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

api::EngineCreateIndexResult CreateIndex(const api::EngineRequestContext& context,
                                         std::string_view table_uuid,
                                         api::EngineIndexDefinition index) {
  api::EngineCreateIndexRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::string(table_uuid);
  request.target_object.object_kind = "table";
  request.indexes.push_back(std::move(index));
  return api::EngineCreateIndex(request);
}

bool FirstSelectedRowUuidIs(const api::EngineSelectRowsResult& result,
                            std::string_view row_uuid) {
  return !result.result_shape.rows.empty() &&
         result.result_shape.rows.front().requested_row_uuid.canonical == row_uuid;
}

std::string FirstSelectedFieldValue(const api::EngineSelectRowsResult& result,
                                    std::string_view field_name) {
  if (result.result_shape.rows.empty()) { return {}; }
  for (const auto& [field, value] : result.result_shape.rows.front().fields) {
    if (field == field_name) { return value.encoded_value; }
  }
  return {};
}

std::uint64_t CountLargeValueSidecarLines(const std::filesystem::path& path,
                                          std::string_view kind,
                                          std::string_view table_uuid = {}) {
  std::ifstream input(path.string() + ".sb.mga_large_values", std::ios::binary);
  std::uint64_t count = 0;
  std::string line;
  const std::string kind_token = "\t" + std::string(kind) + "\t";
  while (std::getline(input, line)) {
    if (!Contains(line, kind_token)) { continue; }
    if (!table_uuid.empty() && !Contains(line, table_uuid)) { continue; }
    ++count;
  }
  return count;
}

void RequireTemporaryDmlRequiresSession(const api::EngineRequestContext& context,
                                        std::string_view table_uuid) {
  auto inserted = InsertRow(context,
                            table_uuid,
                            "019f0000-0000-7000-8000-000000440431",
                            31);
  Require(!inserted.ok &&
              FirstDetail(inserted) ==
                  "dml.insert_rows:temporary_table_requires_session_uuid",
          "TEMP-TABLE-GATE-016 sessionless temp insert did not fail closed");

  auto selected = SelectRows(context, table_uuid);
  Require(!selected.ok &&
              FirstDetail(selected) ==
                  "dml.select_rows:temporary_table_requires_session_uuid",
          "TEMP-TABLE-GATE-016 sessionless temp select did not fail closed");

  api::EngineUpdateRowsRequest update;
  update.context = context;
  update.target_table.uuid.canonical = std::string(table_uuid);
  update.target_table.object_kind = "table";
  update.assignments.push_back({"id", IntValue(32)});
  auto updated = api::EngineUpdateRows(update);
  Require(!updated.ok &&
              FirstDetail(updated) ==
                  "dml.update_rows:temporary_table_requires_session_uuid",
          "TEMP-TABLE-GATE-016 sessionless temp update did not fail closed");

  api::EngineDeleteRowsRequest delete_request;
  delete_request.context = context;
  delete_request.target_table.uuid.canonical = std::string(table_uuid);
  delete_request.target_table.object_kind = "table";
  auto deleted = api::EngineDeleteRows(delete_request);
  Require(!deleted.ok &&
              FirstDetail(deleted) ==
                  "dml.delete_rows:temporary_table_requires_session_uuid",
          "TEMP-TABLE-GATE-016 sessionless temp delete did not fail closed");

  api::EngineMergeRowsRequest merge;
  merge.context = context;
  merge.target_table.uuid.canonical = std::string(table_uuid);
  merge.target_table.object_kind = "table";
  merge.match_predicate.predicate_kind = "row_uuid_match";
  merge.insert_when_not_matched = true;
  merge.input_rows.push_back(
      Row("019f0000-0000-7000-8000-000000440432", 32));
  auto merged = api::EngineMergeRows(merge);
  Require(!merged.ok &&
              FirstDetail(merged) ==
                  "dml.merge_rows:temporary_table_requires_session_uuid",
          "TEMP-TABLE-GATE-016 sessionless temp merge did not fail closed");
}

api::EngineResolveNameRequest ResolveTableRequest(
    const api::EngineRequestContext& context,
    std::string_view table_name) {
  api::EngineResolveNameRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = context.current_schema_uuid.canonical;
  request.target_schema.object_kind = "schema";
  request.target_object.object_kind = "table";
  request.sql_object_reference.expected_object_type = "table";
  request.localized_names.push_back({"en", "primary", "", std::string(table_name), true});
  return request;
}

api::EngineCreateSavepointRequest CreateSavepointRequest(
    const api::EngineRequestContext& context,
    std::string_view savepoint_name) {
  api::EngineCreateSavepointRequest request;
  request.context = context;
  request.option_envelopes.push_back(std::string("savepoint_name:") +
                                     std::string(savepoint_name));
  return request;
}

api::EngineRollbackToSavepointRequest RollbackToSavepointRequest(
    const api::EngineRequestContext& context,
    std::string_view savepoint_name) {
  api::EngineRollbackToSavepointRequest request;
  request.context = context;
  request.option_envelopes.push_back(std::string("savepoint_name:") +
                                     std::string(savepoint_name));
  return request;
}

api::EngineCommitTransactionResult Commit(api::EngineRequestContext context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = std::move(context);
  return api::EngineCommitTransaction(commit);
}

api::EngineRollbackTransactionResult Rollback(api::EngineRequestContext context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = std::move(context);
  return api::EngineRollbackTransaction(rollback);
}

api::EngineCleanupTemporarySessionResult CleanupTemporarySession(
    api::EngineRequestContext context) {
  context.local_transaction_id = 0;
  context.transaction_uuid.canonical.clear();
  context.snapshot_visible_through_local_transaction_id = 0;
  api::EngineCleanupTemporarySessionRequest cleanup;
  cleanup.context = std::move(context);
  return api::EngineCleanupTemporarySessionState(cleanup);
}

std::string ProjectionField(const api::SysInformationProjectionRow& row,
                            std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) { return field.second; }
  }
  return {};
}

bool ProjectionContainsFieldValue(const api::SysInformationProjectionResult& result,
                                  std::string_view field_name,
                                  std::string_view value) {
  for (const auto& row : result.rows) {
    if (ProjectionField(row, field_name) == value) { return true; }
  }
  return false;
}

const api::SysInformationProjectionRow* ProjectionFindRowByField(
    const api::SysInformationProjectionResult& result,
    std::string_view field_name,
    std::string_view value) {
  for (const auto& row : result.rows) {
    if (ProjectionField(row, field_name) == value) { return &row; }
  }
  return nullptr;
}

api::SysInformationProjectionContext TemporaryInformationContext(
    std::string session_uuid) {
  api::SysInformationProjectionContext context;
  context.catalog_display_name = "TempProofDB";
  context.session_language = "en";
  context.default_language = "en";
  context.session_uuid = std::move(session_uuid);
  context.visible_catalog_generation_id = 100;
  context.strict_mode = false;
  context.cluster_authority_available = false;
  return context;
}

std::vector<api::SysInformationCatalogObjectSource>
TemporaryInformationCatalogObjects() {
  return {
      {.object_uuid = "schema-temp-proof",
       .object_class = "schema",
       .schema_uuid = "",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "gtt-catalog-customer",
       .object_class = "table",
       .schema_uuid = "schema-temp-proof",
       .temporary = true,
       .temporary_scope = "global",
       .temporary_session_uuid = "019f0000-0000-7000-8000-000000440131",
       .on_commit_action = "delete_rows",
       .catalog_generation_id = 4,
       .created_local_transaction_id = 4},
      {.object_uuid = "private-catalog-customer",
       .object_class = "table",
       .schema_uuid = "schema-temp-proof",
       .temporary = true,
       .temporary_scope = "private",
       .temporary_session_uuid = "019f0000-0000-7000-8000-000000440131",
       .on_commit_action = "preserve_rows",
       .catalog_generation_id = 5,
       .created_local_transaction_id = 5},
      {.object_uuid = "malformed-catalog-temp",
       .object_class = "table",
       .schema_uuid = "schema-temp-proof",
       .temporary_scope = "private",
       .temporary_session_uuid = "019f0000-0000-7000-8000-000000440131",
       .on_commit_action = "preserve_rows",
       .catalog_generation_id = 6,
       .created_local_transaction_id = 6},
  };
}

std::vector<api::SysInformationResolverNameSource>
TemporaryInformationResolverNames() {
  return {
      {.object_uuid = "schema-temp-proof",
       .object_class = "schema",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "app",
       .catalog_generation_id = 1},
      {.object_uuid = "gtt-catalog-customer",
       .object_class = "table",
       .scope_uuid = "schema-temp-proof",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "gtt_catalog_customer",
       .catalog_generation_id = 4},
      {.object_uuid = "private-catalog-customer",
       .object_class = "table",
       .scope_uuid = "schema-temp-proof",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "private_catalog_customer",
       .catalog_generation_id = 5},
      {.object_uuid = "malformed-catalog-temp",
       .object_class = "table",
       .scope_uuid = "schema-temp-proof",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "malformed_catalog_temp",
       .catalog_generation_id = 6},
  };
}

std::vector<api::SysInformationColumnSource> TemporaryInformationColumns() {
  return {
      {.relation_object_uuid = "gtt-catalog-customer",
       .schema_uuid = "schema-temp-proof",
       .column_name = "id",
       .ordinal_position = 1,
       .datatype_name = "int",
       .is_nullable = "YES",
       .catalog_generation_id = 4},
      {.relation_object_uuid = "private-catalog-customer",
       .schema_uuid = "schema-temp-proof",
       .column_name = "private_id",
       .ordinal_position = 1,
       .datatype_name = "int",
       .is_nullable = "YES",
       .catalog_generation_id = 5},
      {.relation_object_uuid = "malformed-catalog-temp",
       .schema_uuid = "schema-temp-proof",
       .column_name = "malformed_id",
       .ordinal_position = 1,
       .datatype_name = "int",
       .is_nullable = "YES",
       .catalog_generation_id = 6},
  };
}

void RequireTemporaryInformationProjectionVisibility() {
  const auto objects = TemporaryInformationCatalogObjects();
  const auto names = TemporaryInformationResolverNames();
  const auto columns = TemporaryInformationColumns();
  const auto owner_context = TemporaryInformationContext(
      "019f0000-0000-7000-8000-000000440131");
  const auto other_context = TemporaryInformationContext(
      "019f0000-0000-7000-8000-000000440132");
  const auto sessionless_context = TemporaryInformationContext("");

  auto tables = api::BuildSysInformationProjection(
      "sys.information.tables",
      owner_context,
      objects,
      names);
  Require(tables.ok,
          "TEMP-TABLE-GATE-013 owner sys.information.tables projection failed");
  const auto* global_row = ProjectionFindRowByField(
      tables,
      "table_name",
      "gtt_catalog_customer");
  Require(global_row != nullptr,
          "TEMP-TABLE-GATE-013 owner cannot see GTT metadata");
  Require(ProjectionField(*global_row, "table_type") == "GLOBAL TEMPORARY",
          "TEMP-TABLE-GATE-013 GTT table_type was not global temporary");
  Require(ProjectionField(*global_row, "commit_action") == "DELETE ROWS",
          "TEMP-TABLE-GATE-013 GTT commit action was not delete rows");
  const auto* private_row = ProjectionFindRowByField(
      tables,
      "table_name",
      "private_catalog_customer");
  Require(private_row != nullptr,
          "TEMP-TABLE-GATE-013 owner cannot see private temp metadata");
  Require(ProjectionField(*private_row, "table_type") == "LOCAL TEMPORARY",
          "TEMP-TABLE-GATE-013 private temp table_type was not local temporary");
  Require(ProjectionField(*private_row, "commit_action") == "PRESERVE ROWS",
          "TEMP-TABLE-GATE-013 private temp commit action was not preserve rows");
  Require(!ProjectionContainsFieldValue(tables, "table_name", "malformed_catalog_temp"),
          "TEMP-TABLE-GATE-013 malformed temp descriptor did not fail closed");

  tables = api::BuildSysInformationProjection(
      "sys.information.tables",
      other_context,
      objects,
      names);
  Require(tables.ok,
          "TEMP-TABLE-GATE-013 other sys.information.tables projection failed");
  Require(ProjectionContainsFieldValue(tables, "table_name", "gtt_catalog_customer"),
          "TEMP-TABLE-GATE-013 GTT metadata not visible cross-session");
  Require(!ProjectionContainsFieldValue(tables, "table_name", "private_catalog_customer"),
          "TEMP-TABLE-GATE-013 private temp metadata leaked cross-session");
  Require(!ProjectionContainsFieldValue(tables, "table_name", "malformed_catalog_temp"),
          "TEMP-TABLE-GATE-013 malformed temp descriptor leaked cross-session");

  tables = api::BuildSysInformationProjection(
      "sys.information.tables",
      sessionless_context,
      objects,
      names);
  Require(tables.ok,
          "TEMP-TABLE-GATE-013 sessionless sys.information.tables projection failed");
  Require(ProjectionContainsFieldValue(tables, "table_name", "gtt_catalog_customer"),
          "TEMP-TABLE-GATE-013 sessionless view did not see durable GTT metadata");
  Require(!ProjectionContainsFieldValue(tables, "table_name", "private_catalog_customer"),
          "TEMP-TABLE-GATE-013 sessionless view saw private temp metadata");

  auto projected_columns = api::BuildSysInformationProjection(
      "sys.information.columns",
      owner_context,
      objects,
      names,
      {},
      {},
      columns);
  Require(projected_columns.ok,
          "TEMP-TABLE-GATE-013 owner sys.information.columns projection failed");
  Require(ProjectionContainsFieldValue(projected_columns, "column_name", "id"),
          "TEMP-TABLE-GATE-013 owner did not see GTT column metadata");
  Require(ProjectionContainsFieldValue(projected_columns, "column_name", "private_id"),
          "TEMP-TABLE-GATE-013 owner did not see private temp column metadata");
  Require(!ProjectionContainsFieldValue(projected_columns, "column_name", "malformed_id"),
          "TEMP-TABLE-GATE-013 owner saw malformed temp column metadata");

  projected_columns = api::BuildSysInformationProjection(
      "sys.information.columns",
      other_context,
      objects,
      names,
      {},
      {},
      columns);
  Require(projected_columns.ok,
          "TEMP-TABLE-GATE-013 other sys.information.columns projection failed");
  Require(ProjectionContainsFieldValue(projected_columns, "column_name", "id"),
          "TEMP-TABLE-GATE-013 other session did not see GTT column metadata");
  Require(!ProjectionContainsFieldValue(projected_columns, "column_name", "private_id"),
          "TEMP-TABLE-GATE-013 private temp column metadata leaked cross-session");

  auto relations = api::BuildSysInformationProjection(
      "sys.catalog_readable.relations",
      owner_context,
      objects,
      names);
  Require(relations.ok,
          "TEMP-TABLE-GATE-013 owner catalog_readable.relations projection failed");
  Require(ProjectionContainsFieldValue(relations,
                                       "relation_path",
                                       "app.gtt_catalog_customer"),
          "TEMP-TABLE-GATE-013 owner did not see GTT catalog relation");
  Require(ProjectionContainsFieldValue(relations,
                                       "relation_path",
                                       "app.private_catalog_customer"),
          "TEMP-TABLE-GATE-013 owner did not see private temp catalog relation");

  relations = api::BuildSysInformationProjection(
      "sys.catalog_readable.relations",
      other_context,
      objects,
      names);
  Require(relations.ok,
          "TEMP-TABLE-GATE-013 other catalog_readable.relations projection failed");
  Require(ProjectionContainsFieldValue(relations,
                                       "relation_path",
                                       "app.gtt_catalog_customer"),
          "TEMP-TABLE-GATE-013 other session did not see GTT catalog relation");
  Require(!ProjectionContainsFieldValue(relations,
                                        "relation_path",
                                        "app.private_catalog_customer"),
          "TEMP-TABLE-GATE-013 private temp catalog relation leaked cross-session");

  auto object_names = api::BuildSysInformationProjection(
      "sys.catalog_readable.object_names",
      other_context,
      objects,
      names);
  Require(object_names.ok,
          "TEMP-TABLE-GATE-013 object_names projection failed");
  Require(ProjectionContainsFieldValue(object_names,
                                       "object_name",
                                       "gtt_catalog_customer"),
          "TEMP-TABLE-GATE-013 object_names omitted GTT metadata");
  Require(!ProjectionContainsFieldValue(object_names,
                                        "object_name",
                                        "private_catalog_customer"),
          "TEMP-TABLE-GATE-013 object_names leaked private temp metadata");
}

void RequireEngineRefusals(const std::filesystem::path& path,
                           const std::string& database_uuid) {
  auto no_session = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440020"));
  no_session.session_uuid.canonical.clear();
  auto request = TemporaryCreateTableRequest(no_session);
  auto result = api::EngineCreateTable(request);
  Require(!result.ok && FirstDetail(result) == "ddl.create_table:temporary_table_requires_session_uuid",
          "TEMP-TABLE-GATE-004 missing session UUID did not fail closed");

  auto session = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440021"));
  request = TemporaryCreateTableRequest(session);
  request.option_envelopes = {"temporary:true", "temporary_scope:cluster"};
  result = api::EngineCreateTable(request);
  Require(!result.ok && HasDiagnosticCode(result, "SB-MGA-TEMPORARY-SCOPE-UNSUPPORTED"),
          "TEMP-TABLE-GATE-016 unsupported temporary scope did not fail closed");

  request = TemporaryCreateTableRequest(session);
  request.option_envelopes = {"temporary:true", "temporary_scope:private", "on_commit:drop"};
  result = api::EngineCreateTable(request);
  Require(!result.ok &&
              HasDiagnosticCode(result, "SB-MGA-TEMPORARY-ON-COMMIT-UNSUPPORTED"),
          "TEMP-TABLE-GATE-016 invalid on-commit action did not fail closed");
}

void RequireEngineTemporaryDmlAndCommit(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  auto owner = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440031"));
  auto create = api::EngineCreateTable(TemporaryCreateTableRequest(owner, "delete_rows"));
  if (!create.ok) { PrintApiDiagnostics(create); }
  Require(create.ok, "TEMP-TABLE-GATE-004 create temporary table failed");
  Require(HasEvidence(create, "temporary_object_scope", "private"),
          "TEMP-TABLE-GATE-004 create evidence missing private scope");
  Require(HasEvidence(create, "temporary_on_commit", "delete_rows"),
          "TEMP-TABLE-GATE-004 create evidence missing on-commit action");

  auto insert = InsertRow(owner, 7);
  if (!insert.ok) { PrintApiDiagnostics(insert); }
  Require(insert.ok && insert.inserted_count == 1,
          "TEMP-TABLE-GATE-006 owner-session insert failed");
  auto selected = SelectRows(owner);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 1,
          "TEMP-TABLE-GATE-006 owner session cannot see temporary row");

  auto other = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440032"));
  insert = InsertRow(other, 9);
  Require(!insert.ok && FirstDetail(insert) == "dml.insert_rows:target_table_not_visible",
          "TEMP-TABLE-GATE-006 cross-session insert did not fail closed");
  selected = SelectRows(other);
  Require(!selected.ok && FirstDetail(selected) == "dml.select_rows:source_table_not_visible",
          "TEMP-TABLE-GATE-006 cross-session select did not fail closed");

  api::EngineCommitTransactionRequest commit;
  commit.context = owner;
  auto committed = api::EngineCommitTransaction(commit);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-007 commit failed");
  Require(HasEvidence(committed, "temporary_on_commit_deleted_rows", "1"),
          "TEMP-TABLE-GATE-007 commit missing delete-rows cleanup evidence");

  owner.local_transaction_id = 0;
  owner.transaction_uuid.canonical.clear();
  owner = BeginTransaction(owner);
  selected = SelectRows(owner);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 0,
          "TEMP-TABLE-GATE-007 rows survived ON COMMIT DELETE ROWS");

  auto preserve = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440033"));
  auto preserve_request = TemporaryCreateTableRequest(preserve, "preserve_rows");
  preserve_request.requested_table_uuid.canonical =
      "019f0000-0000-7000-8000-000000440103";
  preserve_request.table_names.front().name = "preserve_customer";
  auto preserve_create = api::EngineCreateTable(preserve_request);
  if (!preserve_create.ok) { PrintApiDiagnostics(preserve_create); }
  Require(preserve_create.ok, "TEMP-TABLE-GATE-008 create preserve table failed");
  api::EngineInsertRowsRequest preserve_insert;
  preserve_insert.context = preserve;
  preserve_insert.target_table.uuid.canonical = preserve_request.requested_table_uuid.canonical;
  preserve_insert.target_table.object_kind = "table";
  preserve_insert.input_rows = {
      Row("019f0000-0000-7000-8000-000000440202", 11)};
  preserve_insert.require_generated_row_uuid = false;
  auto preserve_inserted = api::EngineInsertRows(preserve_insert);
  if (!preserve_inserted.ok) { PrintApiDiagnostics(preserve_inserted); }
  Require(preserve_inserted.ok && preserve_inserted.inserted_count == 1,
          "TEMP-TABLE-GATE-008 preserve insert failed");
  commit.context = preserve;
  committed = api::EngineCommitTransaction(commit);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-008 preserve commit failed");
  Require(HasEvidence(committed, "temporary_on_commit_deleted_rows", "0"),
          "TEMP-TABLE-GATE-008 preserve commit deleted rows");
  preserve.local_transaction_id = 0;
  preserve.transaction_uuid.canonical.clear();
  preserve = BeginTransaction(preserve);
  api::EngineSelectRowsRequest preserve_select;
  preserve_select.context = preserve;
  preserve_select.source_object.uuid.canonical =
      preserve_request.requested_table_uuid.canonical;
  preserve_select.source_object.object_kind = "table";
  auto preserve_selected = api::EngineSelectRows(preserve_select);
  if (!preserve_selected.ok) { PrintApiDiagnostics(preserve_selected); }
  Require(preserve_selected.ok && preserve_selected.visible_count == 1,
          "TEMP-TABLE-GATE-008 preserve rows were not visible after commit");
  committed = Commit(preserve);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok,
          "TEMP-TABLE-GATE-008 preserve post-read commit failed");
  auto cleaned = CleanupTemporarySession(preserve);
  if (!cleaned.ok) { PrintApiDiagnostics(cleaned); }
  Require(cleaned.ok,
          "TEMP-TABLE-GATE-008 private temp session cleanup failed");
  Require(HasEvidence(cleaned, "temporary_session_cleanup_deleted_rows", "1"),
          "TEMP-TABLE-GATE-008 private temp session cleanup did not delete row");
  Require(HasEvidence(cleaned,
                      "temporary_session_cleanup_retired_private_metadata",
                      "1"),
          "TEMP-TABLE-GATE-008 private temp cleanup did not retire metadata");
  preserve.local_transaction_id = 0;
  preserve.transaction_uuid.canonical.clear();
  preserve = BeginTransaction(preserve);
  preserve_select.context = preserve;
  preserve_selected = api::EngineSelectRows(preserve_select);
  Require(!preserve_selected.ok &&
              FirstDetail(preserve_selected) ==
                  "dml.select_rows:source_table_not_visible",
          "TEMP-TABLE-GATE-008 private temp table visible after session cleanup");
  auto preserve_resolved = api::EngineResolveName(
      ResolveTableRequest(preserve, "preserve_customer"));
  Require(!preserve_resolved.ok,
          "TEMP-TABLE-GATE-008 private temp name visible after session cleanup");
}

void RequireEngineTemporaryDropRetiresMetadata(
    const std::filesystem::path& path,
    const std::string& database_uuid) {
  auto owner = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440034"));
  auto request = TemporaryCreateTableRequest(owner,
                                             "preserve_rows",
                                             kTemporaryDropTableUuid,
                                             "drop_customer",
                                             "019f0000-0000-7000-8000-000000440104",
                                             "private");
  auto created = api::EngineCreateTable(request);
  if (!created.ok) { PrintApiDiagnostics(created); }
  Require(created.ok, "TEMP-TABLE-GATE-015 temp drop target create failed");
  auto committed = Commit(owner);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-015 temp drop target commit failed");

  owner.local_transaction_id = 0;
  owner.transaction_uuid.canonical.clear();
  owner = BeginTransaction(owner);
  auto resolved = api::EngineResolveName(ResolveTableRequest(owner, "drop_customer"));
  if (!resolved.ok) { PrintApiDiagnostics(resolved); }
  Require(resolved.ok &&
              resolved.primary_object.uuid.canonical ==
                  std::string(kTemporaryDropTableUuid),
          "TEMP-TABLE-GATE-015 temp drop target was not visible before drop");

  api::EngineDropObjectRequest drop;
  drop.context = owner;
  drop.target_object.uuid.canonical = std::string(kTemporaryDropTableUuid);
  drop.target_object.object_kind = "table";
  auto dropped = api::EngineDropObject(drop);
  if (!dropped.ok) { PrintApiDiagnostics(dropped); }
  Require(dropped.ok, "TEMP-TABLE-GATE-015 temp drop failed");
  Require(HasEvidence(dropped, "temporary_metadata_retired", "true"),
          "TEMP-TABLE-GATE-015 temp drop did not retire metadata");
  Require(HasEvidence(dropped, "name_registry_retired",
                      std::string(kTemporaryDropTableUuid)),
          "TEMP-TABLE-GATE-015 temp drop did not retire name registry entry");

  resolved = api::EngineResolveName(ResolveTableRequest(owner, "drop_customer"));
  Require(!resolved.ok,
          "TEMP-TABLE-GATE-015 temp name visible inside drop transaction");
  auto selected = SelectRows(owner, kTemporaryDropTableUuid);
  Require(!selected.ok &&
              FirstDetail(selected) ==
                  "dml.select_rows:source_table_not_visible",
          "TEMP-TABLE-GATE-015 temp table visible inside drop transaction");

  auto rolled_back = Rollback(owner);
  if (!rolled_back.ok) { PrintApiDiagnostics(rolled_back); }
  Require(rolled_back.ok, "TEMP-TABLE-GATE-015 temp drop rollback failed");

  owner.local_transaction_id = 0;
  owner.transaction_uuid.canonical.clear();
  owner = BeginTransaction(owner);
  resolved = api::EngineResolveName(ResolveTableRequest(owner, "drop_customer"));
  if (!resolved.ok) { PrintApiDiagnostics(resolved); }
  Require(resolved.ok &&
              resolved.primary_object.uuid.canonical ==
                  std::string(kTemporaryDropTableUuid),
          "TEMP-TABLE-GATE-015 temp drop rollback did not restore metadata");

  drop.context = owner;
  dropped = api::EngineDropObject(drop);
  if (!dropped.ok) { PrintApiDiagnostics(dropped); }
  Require(dropped.ok, "TEMP-TABLE-GATE-015 committed temp drop failed");
  committed = Commit(owner);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-015 committed temp drop finality failed");

  owner.local_transaction_id = 0;
  owner.transaction_uuid.canonical.clear();
  owner = BeginTransaction(owner);
  resolved = api::EngineResolveName(ResolveTableRequest(owner, "drop_customer"));
  Require(!resolved.ok,
          "TEMP-TABLE-GATE-015 temp name visible after committed drop");
  selected = SelectRows(owner, kTemporaryDropTableUuid);
  Require(!selected.ok &&
              FirstDetail(selected) ==
                  "dml.select_rows:source_table_not_visible",
          "TEMP-TABLE-GATE-015 temp table visible after committed drop");
}

void RequireEngineTemporaryNameScopesAndGttDataIsolation(
    const std::filesystem::path& path,
    const std::string& database_uuid) {
  constexpr std::string_view kShadowName = "shadow_customer";
  constexpr std::string_view kGlobalName = "global_work_customer";

  auto durable = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440041"));
  auto durable_create = api::EngineCreateTable(
      DurableCreateTableRequest(durable,
                                kDurableShadowTableUuid,
                                kShadowName,
                                "019f0000-0000-7000-8000-000000440401"));
  if (!durable_create.ok) { PrintApiDiagnostics(durable_create); }
  Require(durable_create.ok, "TEMP-TABLE-GATE-005 durable shadow target create failed");
  auto committed = Commit(durable);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-005 durable shadow target commit failed");

  auto owner = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440042"));
  auto private_create = api::EngineCreateTable(
      TemporaryCreateTableRequest(owner,
                                  "delete_rows",
                                  kTemporaryShadowTableUuid,
                                  kShadowName,
                                  "019f0000-0000-7000-8000-000000440402",
                                  "private"));
  if (!private_create.ok) { PrintApiDiagnostics(private_create); }
  Require(private_create.ok, "TEMP-TABLE-GATE-005 private temporary shadow create failed");
  Require(HasEvidence(private_create, "temporary_object_scope", "private"),
          "TEMP-TABLE-GATE-005 private temp metadata scope evidence missing");

  auto owner_resolve = api::EngineResolveName(ResolveTableRequest(owner, kShadowName));
  if (!owner_resolve.ok) { PrintApiDiagnostics(owner_resolve); }
  Require(owner_resolve.ok &&
              owner_resolve.primary_object.uuid.canonical ==
                  std::string(kTemporaryShadowTableUuid),
          "TEMP-TABLE-GATE-005 owner did not resolve private temp shadow");
  Require(HasEvidence(owner_resolve,
                      "temporary_name_resolution",
                      "session_visible_shadow"),
          "TEMP-TABLE-GATE-005 private temp shadow evidence missing");

  auto other = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440043"));
  auto other_resolve = api::EngineResolveName(ResolveTableRequest(other, kShadowName));
  if (!other_resolve.ok) { PrintApiDiagnostics(other_resolve); }
  Require(other_resolve.ok &&
              other_resolve.primary_object.uuid.canonical ==
                  std::string(kDurableShadowTableUuid),
          "TEMP-TABLE-GATE-005 other session saw private temp metadata");

  committed = Commit(owner);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-005 private temp metadata commit failed");
  owner.local_transaction_id = 0;
  owner.transaction_uuid.canonical.clear();
  owner = BeginTransaction(owner);
  owner_resolve = api::EngineResolveName(ResolveTableRequest(owner, kShadowName));
  if (!owner_resolve.ok) { PrintApiDiagnostics(owner_resolve); }
  Require(owner_resolve.ok &&
              owner_resolve.primary_object.uuid.canonical ==
                  std::string(kTemporaryShadowTableUuid),
          "TEMP-TABLE-GATE-005 owner private temp metadata did not survive session boundary");
  other_resolve = api::EngineResolveName(ResolveTableRequest(other, kShadowName));
  if (!other_resolve.ok) { PrintApiDiagnostics(other_resolve); }
  Require(other_resolve.ok &&
              other_resolve.primary_object.uuid.canonical ==
                  std::string(kDurableShadowTableUuid),
          "TEMP-TABLE-GATE-005 committed private temp metadata leaked to other session");

  auto global_create_tx = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440044"));
  auto global_create = api::EngineCreateTable(
      TemporaryCreateTableRequest(global_create_tx,
                                  "delete_rows",
                                  kTemporaryPrivateOnlyTableUuid,
                                  kGlobalName,
                                  "019f0000-0000-7000-8000-000000440403",
                                  "global"));
  if (!global_create.ok) { PrintApiDiagnostics(global_create); }
  Require(global_create.ok, "TEMP-TABLE-GATE-004 global temporary table create failed");
  Require(HasEvidence(global_create, "temporary_object_scope", "global"),
          "TEMP-TABLE-GATE-004 global metadata scope evidence missing");
  committed = Commit(global_create_tx);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-004 global temporary metadata commit failed");

  auto no_session = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440054"));
  no_session.session_uuid.canonical.clear();
  RequireTemporaryDmlRequiresSession(no_session, kTemporaryPrivateOnlyTableUuid);

  auto gtt_a = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440045"));
  auto gtt_b = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440046"));
  auto gtt_a_resolve = api::EngineResolveName(ResolveTableRequest(gtt_a, kGlobalName));
  auto gtt_b_resolve = api::EngineResolveName(ResolveTableRequest(gtt_b, kGlobalName));
  if (!gtt_a_resolve.ok) { PrintApiDiagnostics(gtt_a_resolve); }
  if (!gtt_b_resolve.ok) { PrintApiDiagnostics(gtt_b_resolve); }
  Require(gtt_a_resolve.ok && gtt_b_resolve.ok &&
              gtt_a_resolve.primary_object.uuid.canonical ==
                  std::string(kTemporaryPrivateOnlyTableUuid) &&
              gtt_b_resolve.primary_object.uuid.canonical ==
                  std::string(kTemporaryPrivateOnlyTableUuid),
          "TEMP-TABLE-GATE-005 global temporary metadata did not resolve cross-session");

  auto inserted_a = InsertRow(gtt_a,
                              kTemporaryPrivateOnlyTableUuid,
                              "019f0000-0000-7000-8000-000000440421",
                              21);
  if (!inserted_a.ok) { PrintApiDiagnostics(inserted_a); }
  Require(inserted_a.ok && inserted_a.inserted_count == 1,
          "TEMP-TABLE-GATE-006 GTT session A insert failed");
  auto selected_a = SelectRows(gtt_a, kTemporaryPrivateOnlyTableUuid);
  if (!selected_a.ok) { PrintApiDiagnostics(selected_a); }
  Require(selected_a.ok && selected_a.visible_count == 1,
          "TEMP-TABLE-GATE-006 GTT session A cannot see own row");
  auto selected_b = SelectRows(gtt_b, kTemporaryPrivateOnlyTableUuid);
  if (!selected_b.ok) { PrintApiDiagnostics(selected_b); }
  Require(selected_b.ok && selected_b.visible_count == 0,
          "TEMP-TABLE-GATE-006 GTT session B saw session A data");

  auto inserted_b = InsertRow(gtt_b,
                              kTemporaryPrivateOnlyTableUuid,
                              "019f0000-0000-7000-8000-000000440422",
                              22);
  if (!inserted_b.ok) { PrintApiDiagnostics(inserted_b); }
  Require(inserted_b.ok && inserted_b.inserted_count == 1,
          "TEMP-TABLE-GATE-006 GTT session B insert failed");
  selected_b = SelectRows(gtt_b, kTemporaryPrivateOnlyTableUuid);
  if (!selected_b.ok) { PrintApiDiagnostics(selected_b); }
  Require(selected_b.ok && selected_b.visible_count == 1,
          "TEMP-TABLE-GATE-006 GTT session B cannot see own row");
  selected_a = SelectRows(gtt_a, kTemporaryPrivateOnlyTableUuid);
  if (!selected_a.ok) { PrintApiDiagnostics(selected_a); }
  Require(selected_a.ok && selected_a.visible_count == 1,
          "TEMP-TABLE-GATE-006 GTT session A row count was polluted by session B");

  committed = Commit(gtt_a);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-007 GTT session A commit failed");
  Require(HasEvidence(committed, "temporary_on_commit_deleted_rows", "1"),
          "TEMP-TABLE-GATE-007 GTT session A did not clear temporary rows at commit");
  gtt_a.local_transaction_id = 0;
  gtt_a.transaction_uuid.canonical.clear();
  gtt_a = BeginTransaction(gtt_a);
  selected_a = SelectRows(gtt_a, kTemporaryPrivateOnlyTableUuid);
  if (!selected_a.ok) { PrintApiDiagnostics(selected_a); }
  Require(selected_a.ok && selected_a.visible_count == 0,
          "TEMP-TABLE-GATE-007 GTT session A data committed durably");
  selected_b = SelectRows(gtt_b, kTemporaryPrivateOnlyTableUuid);
  if (!selected_b.ok) { PrintApiDiagnostics(selected_b); }
  Require(selected_b.ok && selected_b.visible_count == 1,
          "TEMP-TABLE-GATE-006 GTT session B data was affected by session A commit");
}

void RequireEngineTemporaryRollbackAndSavepoints(
    const std::filesystem::path& path,
    const std::string& database_uuid) {
  auto context = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440051"));
  auto create = api::EngineCreateTable(
      TemporaryCreateTableRequest(context,
                                  "delete_rows",
                                  kRollbackTableUuid,
                                  "rollback_customer",
                                  "019f0000-0000-7000-8000-000000440501",
                                  "private"));
  if (!create.ok) { PrintApiDiagnostics(create); }
  Require(create.ok, "TEMP-TABLE-GATE-009 rollback temp table create failed");

  auto savepoint = api::EngineCreateSavepoint(
      CreateSavepointRequest(context, "after_temp_descriptor"));
  if (!savepoint.ok) { PrintApiDiagnostics(savepoint); }
  Require(savepoint.ok, "TEMP-TABLE-GATE-009 savepoint after descriptor failed");

  auto insert = InsertRow(context,
                          kRollbackTableUuid,
                          "019f0000-0000-7000-8000-000000440511",
                          51);
  if (!insert.ok) { PrintApiDiagnostics(insert); }
  Require(insert.ok && insert.inserted_count == 1,
          "TEMP-TABLE-GATE-009 temp insert before savepoint rollback failed");
  auto selected = SelectRows(context, kRollbackTableUuid);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 1,
          "TEMP-TABLE-GATE-009 temp row not visible before savepoint rollback");

  auto rollback_to = api::EngineRollbackToSavepoint(
      RollbackToSavepointRequest(context, "after_temp_descriptor"));
  if (!rollback_to.ok) { PrintApiDiagnostics(rollback_to); }
  Require(rollback_to.ok, "TEMP-TABLE-GATE-009 rollback to descriptor savepoint failed");
  selected = SelectRows(context, kRollbackTableUuid);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 0,
          "TEMP-TABLE-GATE-009 temp row survived savepoint rollback");
  auto resolved = api::EngineResolveName(
      ResolveTableRequest(context, "rollback_customer"));
  if (!resolved.ok) { PrintApiDiagnostics(resolved); }
  Require(resolved.ok &&
              resolved.primary_object.uuid.canonical == std::string(kRollbackTableUuid),
          "TEMP-TABLE-GATE-009 temp descriptor did not survive row savepoint rollback");

  auto descriptor_context = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440053"));
  savepoint = api::EngineCreateSavepoint(
      CreateSavepointRequest(descriptor_context, "before_temp_descriptor"));
  if (!savepoint.ok) { PrintApiDiagnostics(savepoint); }
  Require(savepoint.ok, "TEMP-TABLE-GATE-009 savepoint before descriptor failed");
  auto rollback_create = api::EngineCreateTable(
      TemporaryCreateTableRequest(descriptor_context,
                                  "delete_rows",
                                  kRollbackCreatedTableUuid,
                                  "rollback_created_customer",
                                  "019f0000-0000-7000-8000-000000440502",
                                  "private"));
  if (!rollback_create.ok) { PrintApiDiagnostics(rollback_create); }
  Require(rollback_create.ok, "TEMP-TABLE-GATE-009 create after savepoint failed");
  resolved = api::EngineResolveName(
      ResolveTableRequest(descriptor_context, "rollback_created_customer"));
  if (!resolved.ok) { PrintApiDiagnostics(resolved); }
  Require(resolved.ok &&
              resolved.primary_object.uuid.canonical ==
                  std::string(kRollbackCreatedTableUuid),
          "TEMP-TABLE-GATE-009 created temp descriptor did not resolve before rollback");
  rollback_to = api::EngineRollbackToSavepoint(
      RollbackToSavepointRequest(descriptor_context, "before_temp_descriptor"));
  if (!rollback_to.ok) { PrintApiDiagnostics(rollback_to); }
  Require(rollback_to.ok, "TEMP-TABLE-GATE-009 rollback before descriptor failed");
  selected = SelectRows(descriptor_context, kRollbackCreatedTableUuid);
  Require(!selected.ok &&
              FirstDetail(selected) == "dml.select_rows:source_table_not_visible",
          "TEMP-TABLE-GATE-009 rolled-back temp descriptor still visible by UUID");
  resolved = api::EngineResolveName(
      ResolveTableRequest(descriptor_context, "rollback_created_customer"));
  Require(!resolved.ok,
          "TEMP-TABLE-GATE-009 rolled-back temp descriptor still visible by name");

  auto full = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440052"));
  auto full_create = api::EngineCreateTable(
      TemporaryCreateTableRequest(full,
                                  "delete_rows",
                                  "019f0000-0000-7000-8000-000000440306",
                                  "full_rollback_temp",
                                  "019f0000-0000-7000-8000-000000440503",
                                  "private"));
  if (!full_create.ok) { PrintApiDiagnostics(full_create); }
  Require(full_create.ok, "TEMP-TABLE-GATE-009 full rollback temp create failed");
  auto rolled_back = Rollback(full);
  if (!rolled_back.ok) { PrintApiDiagnostics(rolled_back); }
  Require(rolled_back.ok, "TEMP-TABLE-GATE-009 full rollback failed");
  full.local_transaction_id = 0;
  full.transaction_uuid.canonical.clear();
  full = BeginTransaction(full);
  selected = SelectRows(full, "019f0000-0000-7000-8000-000000440306");
  Require(!selected.ok &&
              FirstDetail(selected) == "dml.select_rows:source_table_not_visible",
          "TEMP-TABLE-GATE-009 full-rolled-back temp table visible by UUID");
  resolved = api::EngineResolveName(ResolveTableRequest(full, "full_rollback_temp"));
  Require(!resolved.ok,
          "TEMP-TABLE-GATE-009 full-rolled-back temp table visible by name");
}

void RequireEngineTemporaryIndexAndConstraintIsolation(
    const std::filesystem::path& path,
    const std::string& database_uuid) {
  auto create_tx = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440061"));
  auto create = api::EngineCreateTable(
      TemporaryCreateTableRequest(create_tx,
                                  "preserve_rows",
                                  kTemporaryIndexedTableUuid,
                                  "indexed_global_work_customer",
                                  "019f0000-0000-7000-8000-000000440602",
                                  "global"));
  if (!create.ok) { PrintApiDiagnostics(create); }
  Require(create.ok, "TEMP-TABLE-GATE-010 indexed GTT create failed");
  auto committed = Commit(create_tx);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-010 indexed GTT metadata commit failed");

  constexpr std::string_view kRowA =
      "019f0000-0000-7000-8000-000000440611";
  constexpr std::string_view kRowB =
      "019f0000-0000-7000-8000-000000440612";
  constexpr std::string_view kRowB2 =
      "019f0000-0000-7000-8000-000000440613";
  constexpr std::string_view kRowC =
      "019f0000-0000-7000-8000-000000440614";

  auto session_a = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440062"));
  auto inserted_a = InsertRow(session_a, kTemporaryIndexedTableUuid,
                              std::string(kRowA), 71);
  if (!inserted_a.ok) { PrintApiDiagnostics(inserted_a); }
  Require(inserted_a.ok, "TEMP-TABLE-GATE-010 session A pre-index insert failed");
  committed = Commit(session_a);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-010 session A preserve commit failed");

  auto session_b = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440063"));
  auto inserted_b = InsertRow(session_b, kTemporaryIndexedTableUuid,
                              std::string(kRowB), 71);
  if (!inserted_b.ok) { PrintApiDiagnostics(inserted_b); }
  Require(inserted_b.ok,
          "TEMP-TABLE-GATE-010 same key in second GTT session failed before index");
  auto inserted_b2 = InsertRow(session_b, kTemporaryIndexedTableUuid,
                               std::string(kRowB2), 72);
  if (!inserted_b2.ok) { PrintApiDiagnostics(inserted_b2); }
  Require(inserted_b2.ok, "TEMP-TABLE-GATE-010 session B second row insert failed");
  committed = Commit(session_b);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-010 session B preserve commit failed");

  auto index_tx = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440062"));
  auto indexed = CreateIndex(
      index_tx,
      kTemporaryIndexedTableUuid,
      UniqueIdIndexDefinition(kTemporaryIndexUuid, "indexed_global_work_customer_id_uidx"));
  if (!indexed.ok) { PrintApiDiagnostics(indexed); }
  Require(indexed.ok,
          "TEMP-TABLE-GATE-010 GTT unique index rejected cross-session duplicates");
  Require(HasEvidence(indexed, "temporary_index_entry_scope", "global"),
          "TEMP-TABLE-GATE-010 GTT index build did not publish global temp scope");
  Require(HasEvidence(indexed, "temporary_index_build_rows", "3"),
          "TEMP-TABLE-GATE-010 GTT index build did not include all visible session rows");
  committed = Commit(index_tx);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-010 GTT unique index commit failed");

  session_a.local_transaction_id = 0;
  session_a.transaction_uuid.canonical.clear();
  session_a = BeginTransaction(session_a);
  auto selected = SelectRowsById(session_a, kTemporaryIndexedTableUuid, 71);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 1 &&
              FirstSelectedRowUuidIs(selected, kRowA),
          "TEMP-TABLE-GATE-010 indexed GTT read leaked or lost session A row");
  Require(EvidenceContains(selected,
                           "index_lookup",
                           std::string(kTemporaryIndexUuid) +
                               "|index_family=btree|index_profile=btree"),
          "TEMP-TABLE-GATE-010 session A select did not use GTT index");

  session_b.local_transaction_id = 0;
  session_b.transaction_uuid.canonical.clear();
  session_b = BeginTransaction(session_b);
  selected = SelectRowsById(session_b, kTemporaryIndexedTableUuid, 71);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 1 &&
              FirstSelectedRowUuidIs(selected, kRowB),
          "TEMP-TABLE-GATE-010 indexed GTT read leaked or lost session B row");
  selected = SelectRowsById(session_b, kTemporaryIndexedTableUuid, 72);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 1 &&
              FirstSelectedRowUuidIs(selected, kRowB2),
          "TEMP-TABLE-GATE-010 indexed GTT read missed second session B row");

  auto session_c = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440064"));
  auto inserted_c = InsertRow(session_c, kTemporaryIndexedTableUuid,
                              std::string(kRowC), 71);
  if (!inserted_c.ok) { PrintApiDiagnostics(inserted_c); }
  Require(inserted_c.ok,
          "TEMP-TABLE-GATE-010 GTT unique index enforced uniqueness globally");
  selected = SelectRowsById(session_c, kTemporaryIndexedTableUuid, 71);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 1 &&
              FirstSelectedRowUuidIs(selected, kRowC),
          "TEMP-TABLE-GATE-010 indexed GTT read missed session C same-key row");

  auto duplicate_b = InsertRow(session_b,
                               kTemporaryIndexedTableUuid,
                               "019f0000-0000-7000-8000-000000440615",
                               71);
  Require(!duplicate_b.ok && Contains(FirstDetail(duplicate_b), "duplicate_key"),
          "TEMP-TABLE-GATE-010 GTT unique index did not reject same-session insert duplicate");

  api::EngineUpdateRowsRequest update;
  update.context = session_b;
  update.target_table.uuid.canonical = std::string(kTemporaryIndexedTableUuid);
  update.target_table.object_kind = "table";
  update.update_predicate.predicate_kind = "row_uuid_match";
  update.update_predicate.canonical_predicate_envelope = std::string(kRowB2);
  update.assignments.push_back({"id", IntValue(71)});
  auto updated = api::EngineUpdateRows(update);
  Require(!updated.ok &&
              (Contains(FirstDetail(updated), "unique_index_duplicate") ||
               Contains(FirstDetail(updated), "duplicate_key")),
          "TEMP-TABLE-GATE-010 GTT unique index did not reject same-session update duplicate");

  committed = Commit(session_a);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok,
          "TEMP-TABLE-GATE-008 GTT preserve cleanup pre-commit failed");
  auto cleaned_a = CleanupTemporarySession(session_a);
  if (!cleaned_a.ok) { PrintApiDiagnostics(cleaned_a); }
  Require(cleaned_a.ok,
          "TEMP-TABLE-GATE-008 GTT session cleanup failed");
  Require(HasEvidence(cleaned_a, "temporary_session_cleanup_deleted_rows", "1"),
          "TEMP-TABLE-GATE-008 GTT session cleanup did not delete session A row");
  Require(HasEvidence(cleaned_a,
                      "temporary_session_cleanup_retired_private_metadata",
                      "0"),
          "TEMP-TABLE-GATE-008 GTT session cleanup retired shared metadata");
  session_a.local_transaction_id = 0;
  session_a.transaction_uuid.canonical.clear();
  session_a = BeginTransaction(session_a);
  selected = SelectRows(session_a, kTemporaryIndexedTableUuid);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 0,
          "TEMP-TABLE-GATE-008 GTT session A rows survived session cleanup");
  selected = SelectRowsById(session_a, kTemporaryIndexedTableUuid, 71);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 0,
          "TEMP-TABLE-GATE-010 cleaned GTT row remained reachable through index");
  auto resolved = api::EngineResolveName(
      ResolveTableRequest(session_a, "indexed_global_work_customer"));
  if (!resolved.ok) { PrintApiDiagnostics(resolved); }
  Require(resolved.ok &&
              resolved.primary_object.uuid.canonical ==
                  std::string(kTemporaryIndexedTableUuid),
          "TEMP-TABLE-GATE-008 GTT metadata disappeared after session cleanup");
  auto selected_b_after_cleanup = SelectRows(session_b, kTemporaryIndexedTableUuid);
  if (!selected_b_after_cleanup.ok) {
    PrintApiDiagnostics(selected_b_after_cleanup);
  }
  Require(selected_b_after_cleanup.ok &&
              selected_b_after_cleanup.visible_count == 2,
          "TEMP-TABLE-GATE-008 GTT session cleanup affected another session");
}

void RequireEngineTemporaryLargeValueCleanup(
    const std::filesystem::path& path,
    const std::string& database_uuid) {
  auto create_tx = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440071"));
  auto create = api::EngineCreateTable(
      TemporaryPayloadCreateTableRequest(create_tx,
                                         "delete_rows",
                                         kTemporaryLargeValueTableUuid,
                                         "large_value_global_work",
                                         "global"));
  if (!create.ok) { PrintApiDiagnostics(create); }
  Require(create.ok, "TEMP-TABLE-GATE-011 large-value GTT create failed");
  auto committed = Commit(create_tx);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok,
          "TEMP-TABLE-GATE-011 large-value GTT metadata commit failed");

  const std::string payload =
      "temporary-large-value:" + std::string(5000, 'x') + ":end";
  constexpr std::string_view kLargeRowUuid =
      "019f0000-0000-7000-8000-000000440711";

  auto session_a = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440072"));
  auto inserted = InsertPayloadRow(session_a,
                                   kTemporaryLargeValueTableUuid,
                                   std::string(kLargeRowUuid),
                                   81,
                                   payload,
                                   true);
  if (!inserted.ok) { PrintApiDiagnostics(inserted); }
  Require(inserted.ok && inserted.inserted_count == 1,
          "TEMP-TABLE-GATE-011 large-value temp insert failed");
  Require(EvidenceContains(inserted, "mga_large_value_overflow", ""),
          "TEMP-TABLE-GATE-011 insert did not force large-value sidecar storage");
  Require(CountLargeValueSidecarLines(path,
                                      "LARGE_VALUE",
                                      kTemporaryLargeValueTableUuid) == 1,
          "TEMP-TABLE-GATE-011 sidecar missing large-value metadata");
  Require(CountLargeValueSidecarLines(path, "LARGE_VALUE_CHUNK") > 0,
          "TEMP-TABLE-GATE-011 sidecar missing large-value chunks");

  auto selected = SelectRows(session_a, kTemporaryLargeValueTableUuid);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 1 &&
              FirstSelectedRowUuidIs(selected, kLargeRowUuid) &&
              FirstSelectedFieldValue(selected, "payload") == payload,
          "TEMP-TABLE-GATE-011 owner session did not materialize large value");

  auto session_b = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440073"));
  selected = SelectRows(session_b, kTemporaryLargeValueTableUuid);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 0,
          "TEMP-TABLE-GATE-011 second session saw temp large-value row");

  committed = Commit(session_a);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok, "TEMP-TABLE-GATE-011 large-value temp commit failed");
  Require(HasEvidence(committed, "temporary_on_commit_deleted_rows", "1"),
          "TEMP-TABLE-GATE-011 commit did not delete temp row");
  Require(HasEvidence(committed,
                      "temporary_on_commit_reclaimed_large_values",
                      "1"),
          "TEMP-TABLE-GATE-011 commit did not reclaim temp large value");
  Require(CountLargeValueSidecarLines(path,
                                      "LARGE_VALUE_RECLAIMED",
                                      kTemporaryLargeValueTableUuid) == 1,
          "TEMP-TABLE-GATE-011 sidecar missing temp large-value reclaim marker");

  session_a.local_transaction_id = 0;
  session_a.transaction_uuid.canonical.clear();
  session_a = BeginTransaction(session_a);
  selected = SelectRows(session_a, kTemporaryLargeValueTableUuid);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 0,
          "TEMP-TABLE-GATE-011 temp large-value row survived commit cleanup");

  selected = SelectRows(session_b, kTemporaryLargeValueTableUuid);
  if (!selected.ok) { PrintApiDiagnostics(selected); }
  Require(selected.ok && selected.visible_count == 0,
          "TEMP-TABLE-GATE-011 reclaim leaked data to another session");
}

api::MgaTemporaryRecoveryClassificationResult ClassifyTemporaryRecovery(
    const std::filesystem::path& path,
    const std::string& database_uuid) {
  auto context = EngineContext(path,
                               database_uuid,
                               "019f0000-0000-7000-8000-000000440090");
  context.local_transaction_id = 0;
  context.transaction_uuid.canonical.clear();
  return api::ClassifyMgaTemporaryRecoveryState(context);
}

void AppendFencedTemporaryRowEvidence(const std::filesystem::path& path) {
  std::ofstream out(path.string() + ".sb.mga_row_versions",
                    std::ios::app | std::ios::binary);
  Require(static_cast<bool>(out),
          "TEMP-TABLE-GATE-012 could not append fenced row evidence");
  out << "SBMGA1\tROW_VERSION\t999999\t900\t"
      << kTemporaryRecoveryGlobalTableUuid
      << "\t019f0000-0000-7000-8000-000000440891"
      << "\t019f0000-0000-7000-8000-000000440892"
      << "\t0\t\t0\t\t019f0000-0000-7000-8000-000000440082\n";
  Require(static_cast<bool>(out),
          "TEMP-TABLE-GATE-012 fenced row evidence append failed");
}

void RequireEngineTemporaryCrashRestartClassification(
    const std::filesystem::path& path,
    const std::string& database_uuid) {
  auto classified = ClassifyTemporaryRecovery(path, database_uuid);
  if (classified.diagnostic.error) {
    std::cerr << classified.diagnostic.code << ':'
              << classified.diagnostic.detail << '\n';
  }
  Require(classified.ok && classified.classification == "old_state",
          "TEMP-TABLE-GATE-012 empty temporary recovery state was not old_state");

  auto create_active = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440081"));
  auto create = api::EngineCreateTable(
      TemporaryCreateTableRequest(create_active,
                                  "preserve_rows",
                                  kTemporaryRecoveryPrivateTableUuid,
                                  "recovery_private_work",
                                  kTemporaryRecoveryColumnUuid,
                                  "private"));
  if (!create.ok) { PrintApiDiagnostics(create); }
  Require(create.ok,
          "TEMP-TABLE-GATE-012 active private temp create failed");
  classified = ClassifyTemporaryRecovery(path, database_uuid);
  Require(classified.ok &&
              classified.classification == "recovery_required" &&
              classified.active_or_unresolved_event_count != 0 &&
              classified.write_admission_must_remain_fenced,
          "TEMP-TABLE-GATE-012 active create was not recovery-required");
  auto rolled_back = Rollback(create_active);
  if (!rolled_back.ok) { PrintApiDiagnostics(rolled_back); }
  Require(rolled_back.ok,
          "TEMP-TABLE-GATE-012 active create rollback failed");
  classified = ClassifyTemporaryRecovery(path, database_uuid);
  Require(classified.ok && classified.classification == "old_state" &&
              classified.rolled_back_event_count != 0,
          "TEMP-TABLE-GATE-012 rolled-back create was not old_state");

  auto global_create_tx = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440082"));
  create = api::EngineCreateTable(
      TemporaryCreateTableRequest(global_create_tx,
                                  "preserve_rows",
                                  kTemporaryRecoveryGlobalTableUuid,
                                  "recovery_global_work",
                                  kTemporaryRecoveryColumnUuid,
                                  "global"));
  if (!create.ok) { PrintApiDiagnostics(create); }
  Require(create.ok,
          "TEMP-TABLE-GATE-012 recovery GTT create failed");
  auto committed = Commit(global_create_tx);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok,
          "TEMP-TABLE-GATE-012 recovery GTT metadata commit failed");
  classified = ClassifyTemporaryRecovery(path, database_uuid);
  Require(classified.ok && classified.classification == "new_state" &&
              classified.durable_global_metadata_count == 1,
          "TEMP-TABLE-GATE-012 committed GTT metadata was not new_state");

  auto dml_active = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440082"));
  auto inserted = InsertRow(dml_active,
                            kTemporaryRecoveryGlobalTableUuid,
                            "019f0000-0000-7000-8000-000000440811",
                            91);
  if (!inserted.ok) { PrintApiDiagnostics(inserted); }
  Require(inserted.ok,
          "TEMP-TABLE-GATE-012 active GTT DML insert failed");
  classified = ClassifyTemporaryRecovery(path, database_uuid);
  Require(classified.ok &&
              classified.classification == "recovery_required" &&
              classified.active_or_unresolved_event_count != 0,
          "TEMP-TABLE-GATE-012 active GTT DML was not recovery-required");
  rolled_back = Rollback(dml_active);
  if (!rolled_back.ok) { PrintApiDiagnostics(rolled_back); }
  Require(rolled_back.ok,
          "TEMP-TABLE-GATE-012 active GTT DML rollback failed");

  auto preserve_tx = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440082"));
  inserted = InsertRow(preserve_tx,
                       kTemporaryRecoveryGlobalTableUuid,
                       "019f0000-0000-7000-8000-000000440812",
                       92);
  if (!inserted.ok) { PrintApiDiagnostics(inserted); }
  Require(inserted.ok,
          "TEMP-TABLE-GATE-012 committed GTT DML insert failed");
  committed = Commit(preserve_tx);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok,
          "TEMP-TABLE-GATE-012 committed GTT DML commit failed");
  classified = ClassifyTemporaryRecovery(path, database_uuid);
  Require(classified.ok &&
              classified.classification == "recovery_required" &&
              classified.orphaned_row_count == 1 &&
              classified.write_admission_must_remain_fenced,
          "TEMP-TABLE-GATE-012 committed orphaned GTT row was not recovery-required");
  auto cleaned = CleanupTemporarySession(preserve_tx);
  if (!cleaned.ok) { PrintApiDiagnostics(cleaned); }
  Require(cleaned.ok,
          "TEMP-TABLE-GATE-012 recovery GTT cleanup failed");
  classified = ClassifyTemporaryRecovery(path, database_uuid);
  Require(classified.ok &&
              classified.classification == "new_state" &&
              classified.cleaned_row_count != 0,
          "TEMP-TABLE-GATE-012 cleaned GTT state was not new_state");

  auto private_tx = BeginTransaction(
      EngineContext(path, database_uuid, "019f0000-0000-7000-8000-000000440083"));
  create = api::EngineCreateTable(
      TemporaryCreateTableRequest(private_tx,
                                  "preserve_rows",
                                  "019f0000-0000-7000-8000-000000440311",
                                  "recovery_private_orphan",
                                  "019f0000-0000-7000-8000-000000440802",
                                  "private"));
  if (!create.ok) { PrintApiDiagnostics(create); }
  Require(create.ok,
          "TEMP-TABLE-GATE-012 private temp orphan create failed");
  committed = Commit(private_tx);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok,
          "TEMP-TABLE-GATE-012 private temp orphan commit failed");
  classified = ClassifyTemporaryRecovery(path, database_uuid);
  Require(classified.ok &&
              classified.classification == "recovery_required" &&
              classified.orphaned_private_metadata_count != 0,
          "TEMP-TABLE-GATE-012 orphaned private metadata was not recovery-required");
  cleaned = CleanupTemporarySession(private_tx);
  if (!cleaned.ok) { PrintApiDiagnostics(cleaned); }
  Require(cleaned.ok,
          "TEMP-TABLE-GATE-012 private temp metadata cleanup failed");
  classified = ClassifyTemporaryRecovery(path, database_uuid);
  Require(classified.ok &&
              classified.classification == "new_state" &&
              classified.retired_private_metadata_count != 0,
          "TEMP-TABLE-GATE-012 retired private metadata was not new_state");

  AppendFencedTemporaryRowEvidence(path);
  classified = ClassifyTemporaryRecovery(path, database_uuid);
  Require(classified.ok &&
              classified.classification == "fenced" &&
              classified.fenced_event_count != 0 &&
              classified.write_admission_must_remain_fenced,
          "TEMP-TABLE-GATE-012 missing transaction authority did not fence restart");
}

void RequireEngineTemporaryBackupAndDeltaExclusion(
    const std::filesystem::path& path,
    const std::string& database_uuid) {
  constexpr std::string_view kSessionUuid =
      "019f0000-0000-7000-8000-000000440141";
  const auto filespace_uuid = MinimalDatabaseFilespaceUuid();
  const std::filesystem::path logical_manifest =
      path.string() + ".temp_gate014.logical.backup";
  const std::filesystem::path delta_manifest =
      path.string() + ".temp_gate014.delta.backup";
  std::error_code ignored;
  std::filesystem::remove(logical_manifest, ignored);
  std::filesystem::remove(delta_manifest, ignored);

  auto source = BeginTransaction(
      EngineContext(path, database_uuid, std::string(kSessionUuid)));
  const auto source_tx = source.local_transaction_id;
  auto durable_create = api::EngineCreateTable(
      DurableCreateTableRequest(source,
                                kBackupDurableTableUuid,
                                "backup_durable_customer",
                                kBackupDurableColumnUuid));
  if (!durable_create.ok) { PrintApiDiagnostics(durable_create); }
  Require(durable_create.ok,
          "TEMP-TABLE-GATE-014 durable backup table create failed");

  auto global_create = api::EngineCreateTable(
      TemporaryCreateTableRequest(source,
                                  "preserve_rows",
                                  kBackupGlobalTempTableUuid,
                                  "backup_global_temp_customer",
                                  kBackupGlobalTempColumnUuid,
                                  "global"));
  if (!global_create.ok) { PrintApiDiagnostics(global_create); }
  Require(global_create.ok,
          "TEMP-TABLE-GATE-014 GTT backup table create failed");

  auto private_create = api::EngineCreateTable(
      TemporaryCreateTableRequest(source,
                                  "preserve_rows",
                                  kBackupPrivateTempTableUuid,
                                  "backup_private_temp_customer",
                                  kBackupPrivateTempColumnUuid,
                                  "private"));
  if (!private_create.ok) { PrintApiDiagnostics(private_create); }
  Require(private_create.ok,
          "TEMP-TABLE-GATE-014 private temp backup table create failed");

  auto temp_index = CreateIndex(
      source,
      kBackupGlobalTempTableUuid,
      UniqueIdIndexDefinition(kBackupTempIndexUuid,
                              "backup_global_temp_customer_id_uidx"));
  if (!temp_index.ok) { PrintApiDiagnostics(temp_index); }
  Require(temp_index.ok,
          "TEMP-TABLE-GATE-014 temporary backup index create failed");

  auto inserted = InsertRow(source,
                            kBackupDurableTableUuid,
                            std::string(kBackupDurableRowUuid),
                            141);
  if (!inserted.ok) { PrintApiDiagnostics(inserted); }
  Require(inserted.ok && inserted.inserted_count == 1,
          "TEMP-TABLE-GATE-014 durable backup row insert failed");
  inserted = InsertRow(source,
                       kBackupGlobalTempTableUuid,
                       std::string(kBackupGlobalTempRowUuid),
                       142);
  if (!inserted.ok) { PrintApiDiagnostics(inserted); }
  Require(inserted.ok && inserted.inserted_count == 1,
          "TEMP-TABLE-GATE-014 GTT backup row insert failed");
  inserted = InsertRow(source,
                       kBackupPrivateTempTableUuid,
                       std::string(kBackupPrivateTempRowUuid),
                       143);
  if (!inserted.ok) { PrintApiDiagnostics(inserted); }
  Require(inserted.ok && inserted.inserted_count == 1,
          "TEMP-TABLE-GATE-014 private temp backup row insert failed");

  auto committed = Commit(source);
  if (!committed.ok) { PrintApiDiagnostics(committed); }
  Require(committed.ok,
          "TEMP-TABLE-GATE-014 source transaction commit failed");

  auto backup_context = BackupEngineContext(path,
                                            database_uuid,
                                            std::string(kSessionUuid),
                                            "temp-gate014-logical-backup");
  api::EngineStartLogicalBackupRequest backup;
  backup.context = backup_context;
  backup.option_envelopes.push_back("target_uri:" +
                                    logical_manifest.string());
  backup.option_envelopes.push_back("filespace_uuid:" + filespace_uuid);
  auto backed_up = api::EngineStartLogicalBackup(backup);
  if (!backed_up.ok) { PrintApiDiagnostics(backed_up); }
  Require(backed_up.ok,
          "TEMP-TABLE-GATE-014 logical backup failed");
  Require(backed_up.table_count == 1 && backed_up.row_count == 1 &&
              backed_up.index_count == 0,
          "TEMP-TABLE-GATE-014 logical backup did not capture durable-only content");
  Require(HasEvidence(backed_up, "temporary_content_excluded", "true") &&
              HasEvidence(backed_up, "temporary_tables_excluded", "2") &&
              HasEvidence(backed_up, "temporary_rows_excluded", "2") &&
              HasEvidence(backed_up, "temporary_indexes_excluded", "1"),
          "TEMP-TABLE-GATE-014 logical backup missing temp exclusion evidence");
  auto logical_body = ReadFile(logical_manifest);
  Require(Contains(logical_body, HexEncode(kBackupDurableTableUuid)) &&
              Contains(logical_body, HexEncode(kBackupDurableRowUuid)),
          "TEMP-TABLE-GATE-014 logical backup omitted durable content");
  Require(!Contains(logical_body, HexEncode(kBackupGlobalTempTableUuid)) &&
              !Contains(logical_body, HexEncode(kBackupPrivateTempTableUuid)) &&
              !Contains(logical_body, HexEncode(kBackupTempIndexUuid)) &&
              !Contains(logical_body, HexEncode(kBackupGlobalTempRowUuid)) &&
              !Contains(logical_body, HexEncode(kBackupPrivateTempRowUuid)),
          "TEMP-TABLE-GATE-014 logical backup leaked temporary content");
  auto rolled_back = Rollback(backup_context);
  if (!rolled_back.ok) { PrintApiDiagnostics(rolled_back); }
  Require(rolled_back.ok,
          "TEMP-TABLE-GATE-014 logical backup context rollback failed");

  auto delta_context = BackupEngineContext(path,
                                           database_uuid,
                                           std::string(kSessionUuid),
                                           "temp-gate014-delta-package");
  api::EnginePackageDeltaStreamRequest delta;
  delta.context = delta_context;
  delta.option_envelopes.push_back("target_uri:" + delta_manifest.string());
  delta.option_envelopes.push_back("source_backup_uuid:" +
                                   backed_up.backup_uuid.canonical);
  delta.option_envelopes.push_back("filespace_uuid:" + filespace_uuid);
  delta.option_envelopes.push_back("start_transaction_id:" +
                                   std::to_string(source_tx));
  delta.option_envelopes.push_back("end_transaction_id:" +
                                   std::to_string(source_tx));
  auto packaged = api::EnginePackageDeltaStream(delta);
  if (!packaged.ok) { PrintApiDiagnostics(packaged); }
  Require(packaged.ok,
          "TEMP-TABLE-GATE-014 delta package failed");
  Require(packaged.table_count == 1 && packaged.row_count == 1,
          "TEMP-TABLE-GATE-014 delta package did not capture durable-only content");
  Require(HasEvidence(packaged, "temporary_content_excluded", "true") &&
              HasEvidence(packaged, "temporary_tables_excluded", "2") &&
              HasEvidence(packaged, "temporary_rows_excluded", "2") &&
              HasEvidence(packaged, "temporary_indexes_excluded", "1"),
          "TEMP-TABLE-GATE-014 delta package missing temp exclusion evidence");
  auto delta_body = ReadFile(delta_manifest);
  Require(Contains(delta_body, HexEncode(kBackupDurableTableUuid)) &&
              Contains(delta_body, HexEncode(kBackupDurableRowUuid)),
          "TEMP-TABLE-GATE-014 delta package omitted durable content");
  Require(!Contains(delta_body, HexEncode(kBackupGlobalTempTableUuid)) &&
              !Contains(delta_body, HexEncode(kBackupPrivateTempTableUuid)) &&
              !Contains(delta_body, HexEncode(kBackupTempIndexUuid)) &&
              !Contains(delta_body, HexEncode(kBackupGlobalTempRowUuid)) &&
              !Contains(delta_body, HexEncode(kBackupPrivateTempRowUuid)),
          "TEMP-TABLE-GATE-014 delta package leaked temporary content");
  rolled_back = Rollback(delta_context);
  if (!rolled_back.ok) { PrintApiDiagnostics(rolled_back); }
  Require(rolled_back.ok,
          "TEMP-TABLE-GATE-014 delta package context rollback failed");

  auto support_context = BackupEngineContext(path,
                                             database_uuid,
                                             std::string(kSessionUuid),
                                             "temp-gate014-support-bundle");
  api::EnginePrepareSupportBundleRequest support;
  support.context = support_context;
  support.option_envelopes.push_back("engine_authorized_support_export:true");
  support.option_envelopes.push_back(
      "retention_policy_ref:support.bundle.default_retention.v1");
  support.option_envelopes.push_back(
      "redaction_profile_ref:server.support_bundle.default_redaction.v1");
  auto prepared = api::EnginePrepareSupportBundle(support);
  if (!prepared.ok) { PrintApiDiagnostics(prepared); }
  Require(prepared.ok && prepared.redaction_applied &&
              prepared.forbidden_fields_absent,
          "TEMP-TABLE-GATE-014 support bundle preparation failed");
  const std::string support_text =
      prepared.support_bundle_json + "\n" + ApiResultText(prepared);
  Require(!Contains(support_text, kBackupGlobalTempTableUuid) &&
              !Contains(support_text, kBackupPrivateTempTableUuid) &&
              !Contains(support_text, kBackupGlobalTempRowUuid) &&
              !Contains(support_text, kBackupPrivateTempRowUuid) &&
              !Contains(support_text, HexEncode(kBackupGlobalTempTableUuid)) &&
              !Contains(support_text, HexEncode(kBackupPrivateTempTableUuid)) &&
              !Contains(support_text, HexEncode(kBackupGlobalTempRowUuid)) &&
              !Contains(support_text, HexEncode(kBackupPrivateTempRowUuid)),
          "TEMP-TABLE-GATE-014 support bundle leaked temporary content");
  rolled_back = Rollback(support_context);
  if (!rolled_back.ok) { PrintApiDiagnostics(rolled_back); }
  Require(rolled_back.ok,
          "TEMP-TABLE-GATE-014 support bundle context rollback failed");

  std::filesystem::remove(logical_manifest, ignored);
  std::filesystem::remove(delta_manifest, ignored);
}

}  // namespace

int main() {
  // SEARCH_KEY: SBSQL-TEMPORARY-TABLE-PROOF-CLOSURE
  // SEARCH_KEY: TEMP-TABLE-GATE-001 TEMP-TABLE-GATE-002 TEMP-TABLE-GATE-003
  // SEARCH_KEY: TEMP-TABLE-GATE-004 TEMP-TABLE-GATE-006 TEMP-TABLE-GATE-007
  // SEARCH_KEY: TEMP-TABLE-GATE-005 TEMP-TABLE-GATE-008 TEMP-TABLE-GATE-009
  // SEARCH_KEY: TEMP-TABLE-GATE-010 TEMP-TABLE-GATE-011 TEMP-TABLE-GATE-012
  // SEARCH_KEY: TEMP-TABLE-GATE-013 TEMP-TABLE-GATE-014 TEMP-TABLE-GATE-015
  // SEARCH_KEY: TEMP-TABLE-GATE-016
  RequireTemporaryInformationProjectionVisibility();
  RequireAcceptedTemporarySql("CREATE TEMPORARY TABLE customer (id int) ON COMMIT DELETE ROWS",
                              "private",
                              "delete_rows",
                              true);
  RequireAcceptedTemporarySql("CREATE TEMP TABLE customer (id int) ON COMMIT PRESERVE ROWS",
                              "private",
                              "preserve_rows",
                              true);
  RequireAcceptedTemporarySql("CREATE LOCAL TEMPORARY TABLE customer (id int)",
                              "private",
                              "delete_rows",
                              false);
  RequireAcceptedTemporarySql("CREATE GLOBAL TEMPORARY TABLE customer (id int)",
                              "global",
                              "delete_rows",
                              false);
  RequireRejectedTemporarySql("CREATE TEMPORARY TABLE customer (id int) ON COMMIT DROP");
  RequireDropTableExactLoweringForTemporaryCleanup();

  const auto recovery_path = TestRecoveryDatabasePath();
  RemoveDatabaseArtifacts(recovery_path);
  const auto recovery_database_uuid = CreateMinimalDatabase(recovery_path);
  RequireEngineTemporaryCrashRestartClassification(recovery_path,
                                                   recovery_database_uuid);
  RemoveDatabaseArtifacts(recovery_path);

  const auto backup_path = TestBackupDatabasePath();
  RemoveDatabaseArtifacts(backup_path);
  const auto backup_database_uuid = CreateMinimalDatabase(backup_path);
  RequireEngineTemporaryBackupAndDeltaExclusion(backup_path,
                                                backup_database_uuid);
  RemoveDatabaseArtifacts(backup_path);

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  RequireEngineRefusals(path, database_uuid);
  RequireEngineTemporaryDmlAndCommit(path, database_uuid);
  RequireEngineTemporaryDropRetiresMetadata(path, database_uuid);
  RequireEngineTemporaryNameScopesAndGttDataIsolation(path, database_uuid);
  RequireEngineTemporaryRollbackAndSavepoints(path, database_uuid);
  RequireEngineTemporaryIndexAndConstraintIsolation(path, database_uuid);
  RequireEngineTemporaryLargeValueCleanup(path, database_uuid);
  RemoveDatabaseArtifacts(path);

  std::cout << "sbsql_temporary_table_proof_closure_conformance=passed\n";
  return EXIT_SUCCESS;
}
