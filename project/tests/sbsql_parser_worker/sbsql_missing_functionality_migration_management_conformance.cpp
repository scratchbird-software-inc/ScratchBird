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
#include "memory.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
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

constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000004001";

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

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_missing_functionality_gate_004";
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
      MemoryPolicy(), "sbsql_missing_functionality_migration_management_conformance");
  Require(configured.ok(), "Gate 004 memory fixture configuration failed");
  Require(configured.fixture_mode, "Gate 004 memory fixture mode was not active");
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000004101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000004102";
  session.database_uuid = "019f0000-0000-7000-8000-000000004103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 41;
  session.security_policy_epoch = 42;
  session.descriptor_epoch = 43;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsql_missing_gate_004";
  config.parser_uuid = "019f0000-0000-7000-8000-000000004104";
  config.bundle_contract_id = "sbp_sbsql@sbsql-missing-gate-004";
  config.build_id = "sbsql-missing-functionality-gate-004";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

void PrintMessages(const PipelineArtifacts& artifacts) {
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
}

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

sblr::SblrOperationEnvelope EngineEnvelopeFromParser(const SblrEnvelope& parser_envelope) {
  auto engine_envelope = sblr::MakeSblrEnvelope(
      parser_envelope.engine_api_operation_id.empty() ? parser_envelope.operation_id
                                                      : parser_envelope.engine_api_operation_id,
      parser_envelope.sblr_opcode,
      parser_envelope.trace_key);
  engine_envelope.result_shape = parser_envelope.result_shape_key;
  engine_envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  engine_envelope.requires_security_context = true;
  engine_envelope.requires_transaction_context = true;
  engine_envelope.requires_cluster_authority = false;
  engine_envelope.contains_sql_text = false;
  engine_envelope.parser_resolved_names_to_uuids = true;
  for (const auto& operand : parser_envelope.operands) {
    engine_envelope.operands.push_back({operand.type, operand.name, operand.value});
  }
  return engine_envelope;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << "dispatch " << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << "api " << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_missing_gate_004_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events", ".sb.crud_events", ".sb.name_events",
                            ".sb.transaction_inventory", ".dirty.manifest",
                            ".recovery.evidence", ".sb.owner.lock",
                            ".sb.txn_publish"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810040000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810040001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810040002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "Gate 004 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid,
                                        std::string request_id,
                                        std::string session_uuid) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = std::move(session_uuid);
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000004202";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.catalog_generation_id = 1;
  context.security_epoch = 2;
  context.resource_epoch = 3;
  context.name_resolution_epoch = 4;
  context.trace_tags.push_back("right:MIGRATE_DATABASE");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid,
                                                 std::string request_id,
                                                 std::string session_uuid) {
  auto context = EngineContext(path, database_uuid, std::move(request_id),
                               std::move(session_uuid));
  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(begin);
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, "Gate 004 transaction begin failed");
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  return context;
}

void CommitEngineTransaction(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  const auto result = api::EngineCommitTransaction(commit);
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, "Gate 004 transaction commit failed");
}

std::string FirstRowValue(const api::EngineApiResult& result, std::string_view field_name) {
  if (result.result_shape.rows.empty()) return {};
  for (const auto& field : result.result_shape.rows.front().fields) {
    if (field.first == field_name) return field.second.encoded_value;
  }
  return {};
}

void RequireGeneratedRegistryRows() {
  const struct ExpectedRow {
    const char* row_id;
    const char* canonical_name;
  } expected_rows[] = {
      {"SBSQL-1A0000000001", "migrate_from_reference"},
      {"SBSQL-1A0000000002", "alter_migration"},
      {"SBSQL-1A0000000003", "show_migration"},
      {"SBSQL-1A0000000004", "show_migrations"},
  };
  for (const auto& expected : expected_rows) {
    const auto* row = FindGeneratedSurfaceRegistryRowById(expected.row_id);
    Require(row != nullptr, "Gate 004 migration registry row missing");
    Require(row->canonical_name == expected.canonical_name,
            "Gate 004 migration registry canonical name drifted");
    Require(row->family == "migration", "Gate 004 migration registry family drifted");
    Require(row->source_status == "native_now", "Gate 004 migration registry status drifted");
    Require(row->sblr_operation_family == "sblr.migration.operation.v3",
            "Gate 004 migration registry SBLR family drifted");
  }
}

PipelineArtifacts RequireMigrationRoute(std::string_view sql,
                                        std::string_view operation_id,
                                        std::string_view opcode,
                                        std::string_view engine_api_function,
                                        std::string_view result_shape) {
  auto artifacts = RunPipeline(sql);
  PrintMessages(artifacts);
  Require(!artifacts.cst.messages.has_errors(), "Gate 004 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "Gate 004 AST failed");
  Require(artifacts.ast.family == StatementFamily::kMigration,
          "Gate 004 AST did not classify migration family");
  if (artifacts.ast.statement_parser_category != "migration") {
    std::cerr << "sql=" << sql
              << " category=" << artifacts.ast.statement_parser_category
              << " family=" << StatementFamilyName(artifacts.ast.family)
              << " registry=" << artifacts.ast.registry_family << '\n';
    Require(false, "Gate 004 AST parser category mismatch");
  }
  Require(artifacts.bound.bound, "Gate 004 bind failed");
  Require(artifacts.bound.requires_security_authority,
          "Gate 004 migration route missing security authority");
  Require(artifacts.bound.requires_transaction_authority,
          "Gate 004 migration route missing transaction authority");
  Require(artifacts.verifier.admitted, "Gate 004 verifier rejected migration route");
  Require(artifacts.envelope.operation_family == "sblr.migration.operation.v3",
          "Gate 004 migration route family mismatch");
  Require(artifacts.envelope.operation_id == operation_id,
          "Gate 004 migration route operation mismatch");
  Require(artifacts.envelope.sblr_opcode == opcode,
          "Gate 004 migration route opcode mismatch");
  Require(artifacts.envelope.engine_api_function == engine_api_function,
          "Gate 004 migration route engine API function mismatch");
  Require(artifacts.envelope.result_shape_key == result_shape,
          "Gate 004 migration result shape mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.migrate_database"),
          "Gate 004 migration right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.migration_management_api_required"),
          "Gate 004 migration API authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "Gate 004 parser no-SQL authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "Gate 004 parser no-finality authority missing");
  Require(HasValue(artifacts.envelope.policy_refs, "migration_control_policy") ||
              HasValue(artifacts.envelope.policy_refs, "migration_inspection_policy"),
          "Gate 004 migration policy ref missing");
  Require(!artifacts.envelope.real_file_effects,
          "Gate 004 parser envelope claimed file effects");
  Require(!artifacts.envelope.parser_executes_sql,
          "Gate 004 parser envelope claimed SQL execution");
  Require(!Contains(artifacts.envelope.payload, sql),
          "Gate 004 migration payload embedded source SQL text");
  Require(Contains(artifacts.envelope.payload, "\"wal_recovery_authority\":false"),
          "Gate 004 migration payload missing no-WAL proof");
  Require(!Contains(artifacts.envelope.payload, "\"wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "\"authoritative_wal_allowed\":true"),
          "Gate 004 migration payload carried WAL authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "Gate 004 migration payload missing no-SQL-text proof");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "Gate 004 server admission rejected migration route");
  Require(admission.requires_public_abi_dispatch,
          "Gate 004 server admission did not require public ABI dispatch");
  Require(admission.operation_id == operation_id,
          "Gate 004 server admission operation mismatch");

  const auto* registry_row = sblr::LookupSblrOperation(operation_id);
  Require(registry_row != nullptr, "Gate 004 opcode registry row missing");
  Require(registry_row->opcode == opcode, "Gate 004 opcode registry drifted");
  Require(registry_row->requires_security_context,
          "Gate 004 opcode registry must require security context");
  Require(registry_row->requires_transaction_context,
          "Gate 004 opcode registry must require transaction context");
  Require(!registry_row->requires_cluster_authority,
          "Gate 004 public migration route required private cluster authority");
  return artifacts;
}

sblr::SblrDispatchResult DispatchMigrationRoute(
    const api::EngineRequestContext& context,
    const PipelineArtifacts& artifacts) {
  auto result = sblr::DispatchSblrOperation(
      {context, EngineEnvelopeFromParser(artifacts.envelope), api::EngineApiRequest{}});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "Gate 004 engine envelope rejected");
  Require(result.accepted, "Gate 004 dispatch refused public migration route");
  Require(result.dispatched_to_api, "Gate 004 dispatch did not call engine API");
  return result;
}

void RequireNegativeParserShape(std::string_view sql, std::string_view detail) {
  const auto artifacts = RunPipeline(sql);
  const bool refused = artifacts.cst.messages.has_errors() ||
                       artifacts.ast.messages.has_errors() ||
                       artifacts.bound.messages.has_errors() ||
                       artifacts.envelope.messages.has_errors() ||
                       !artifacts.verifier.admitted;
  Require(refused, "Gate 004 invalid migration syntax was admitted");
  const std::string messages = RenderMessageVectorSet(artifacts.cst.messages) +
                               RenderMessageVectorSet(artifacts.ast.messages) +
                               RenderMessageVectorSet(artifacts.bound.messages) +
                               RenderMessageVectorSet(artifacts.envelope.messages) +
                               RenderMessageVectorSet(artifacts.verifier.messages);
  Require(Contains(messages, detail), "Gate 004 invalid migration diagnostic drifted");
}

void RequireSecurityRefusal(const PipelineArtifacts& artifacts,
                            const api::EngineRequestContext& context) {
  auto insecure = context;
  insecure.security_context_present = false;
  auto result = sblr::DispatchSblrOperation(
      {insecure, EngineEnvelopeFromParser(artifacts.envelope), api::EngineApiRequest{}});
  Require(result.envelope_validated, "Gate 004 insecure envelope was not validated");
  Require(!result.accepted, "Gate 004 insecure route was accepted");
  Require(!result.api_result.ok, "Gate 004 insecure migration route succeeded");
  Require(HasDiagnostic(result.api_result, "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED"),
          "Gate 004 security-context diagnostic missing");
}

std::string RequireMigrationEnginePath(const std::filesystem::path& path,
                                       const std::string& database_uuid) {
  auto context = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-004-migration",
      "019f0000-0000-7000-8000-000000004301");

  const auto begin = RequireMigrationRoute(
      "MIGRATE FROM REFERENCE postgres WITH PACKAGE pg_compat_pack;",
      "op.migration.begin_from_reference",
      "SBLR_MIGRATION_BEGIN_FROM_REFERENCE",
      "EngineBeginMigration",
      "rs.migration.status.v1");
  Require(HasValue(begin.envelope.descriptor_refs, "sys.migration.context"),
          "Gate 004 begin migration descriptor missing");
  Require(Contains(begin.envelope.payload, "\"reference_profile\":\"postgres\""),
          "Gate 004 begin migration reference profile operand missing");
  Require(Contains(begin.envelope.payload, "\"reference_package\":\"pg_compat_pack\""),
          "Gate 004 begin migration reference package operand missing");
  RequireSecurityRefusal(begin, context);
  auto begin_result = DispatchMigrationRoute(context, begin);
  Require(begin_result.api_result.ok, "Gate 004 begin migration API failed");
  Require(begin_result.api_result.result_shape.result_kind == "rs.migration.status.v1",
          "Gate 004 begin migration result shape drifted");
  Require(HasEvidence(begin_result.api_result, "engine_api_function", "EngineBeginMigration"),
          "Gate 004 begin migration API evidence missing");
  Require(HasEvidence(begin_result.api_result, "reference_profile", "postgres"),
          "Gate 004 begin migration reference profile evidence missing");
  Require(HasEvidence(begin_result.api_result, "reference_storage_authority_accepted", "false"),
          "Gate 004 begin migration accepted reference storage authority");
  Require(HasEvidence(begin_result.api_result, "reference_finality_accepted", "false"),
          "Gate 004 begin migration accepted reference finality");
  Require(HasEvidence(begin_result.api_result, "private_cluster_execution", "false"),
          "Gate 004 begin migration entered private cluster execution");
  Require(HasEvidence(begin_result.api_result, "wal_recovery_authority", "false"),
          "Gate 004 begin migration carried WAL authority");
  const std::string migration_uuid = begin_result.api_result.primary_object.uuid.canonical;
  Require(!migration_uuid.empty(), "Gate 004 begin migration did not persist UUID");
  Require(FirstRowValue(begin_result.api_result, "state") == "prepared",
          "Gate 004 begin migration state drifted");

  const auto alter = RequireMigrationRoute(
      "ALTER MIGRATION " + migration_uuid + " START;",
      "op.migration.alter",
      "SBLR_MIGRATION_ALTER",
      "EngineAlterMigration",
      "rs.migration.status.v1");
  Require(Contains(alter.envelope.payload, "\"migration_action\":\"start\""),
          "Gate 004 alter migration action operand missing");
  auto alter_result = DispatchMigrationRoute(context, alter);
  Require(alter_result.api_result.ok, "Gate 004 alter migration API failed");
  Require(HasEvidence(alter_result.api_result, "engine_api_function", "EngineAlterMigration"),
          "Gate 004 alter migration API evidence missing");
  Require(FirstRowValue(alter_result.api_result, "state") == "running",
          "Gate 004 alter migration state drifted");

  const auto show = RequireMigrationRoute(
      "SHOW MIGRATION " + migration_uuid + ";",
      "op.show.migration",
      "SBLR_SHOW_MIGRATION",
      "EngineShowMigration",
      "rs.migration.status.v1");
  auto show_result = DispatchMigrationRoute(context, show);
  Require(show_result.api_result.ok, "Gate 004 show migration API failed");
  Require(show_result.api_result.result_shape.result_kind == "rs.migration.status.v1",
          "Gate 004 show migration result shape drifted");
  Require(HasEvidence(show_result.api_result, "engine_api_function", "EngineShowMigration"),
          "Gate 004 show migration API evidence missing");
  Require(FirstRowValue(show_result.api_result, "state") == "running",
          "Gate 004 show migration did not observe running state");
  Require(FirstRowValue(show_result.api_result, "migration_uuid") == migration_uuid,
          "Gate 004 show migration UUID drifted");

  const auto show_all = RequireMigrationRoute(
      "SHOW MIGRATIONS;",
      "op.show.migrations",
      "SBLR_SHOW_MIGRATIONS",
      "EngineShowMigrations",
      "rs.migration.list.v1");
  auto show_all_result = DispatchMigrationRoute(context, show_all);
  Require(show_all_result.api_result.ok, "Gate 004 show migrations API failed");
  Require(show_all_result.api_result.result_shape.result_kind == "rs.migration.list.v1",
          "Gate 004 show migrations result shape drifted");
  Require(HasEvidence(show_all_result.api_result, "engine_api_function", "EngineShowMigrations"),
          "Gate 004 show migrations API evidence missing");
  Require(!show_all_result.api_result.result_shape.rows.empty(),
          "Gate 004 show migrations returned no rows");
  Require(FirstRowValue(show_all_result.api_result, "migration_uuid") == migration_uuid,
          "Gate 004 show migrations did not list migration UUID");
  Require(FirstRowValue(show_all_result.api_result, "reference_storage_authority_accepted") == "false",
          "Gate 004 show migrations row accepted reference storage authority");
  Require(FirstRowValue(show_all_result.api_result, "reference_finality_accepted") == "false",
          "Gate 004 show migrations row accepted reference finality");

  CommitEngineTransaction(context);
  return migration_uuid;
}

void RequireMigrationReopenPath(const std::filesystem::path& path,
                                const std::string& database_uuid,
                                const std::string& migration_uuid) {
  db::DatabaseOpenConfig open;
  open.path = path.string();
  const auto opened = db::OpenDatabaseFile(open);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ':'
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "Gate 004 database did not reopen after migration commit");
  Require(opened.state.local_transaction_inventory_present,
          "Gate 004 reopen did not load local transaction inventory");

  auto context = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-004-migration-reopen",
      "019f0000-0000-7000-8000-000000004302");

  const auto show = RequireMigrationRoute(
      "SHOW MIGRATION " + migration_uuid + ";",
      "op.show.migration",
      "SBLR_SHOW_MIGRATION",
      "EngineShowMigration",
      "rs.migration.status.v1");
  auto show_result = DispatchMigrationRoute(context, show);
  Require(show_result.api_result.ok, "Gate 004 reopen show migration API failed");
  Require(FirstRowValue(show_result.api_result, "migration_uuid") == migration_uuid,
          "Gate 004 reopen show migration UUID drifted");
  Require(FirstRowValue(show_result.api_result, "state") == "running",
          "Gate 004 reopen show migration did not preserve running state");
  Require(FirstRowValue(show_result.api_result, "reference_storage_authority_accepted") == "false",
          "Gate 004 reopen show migration accepted reference storage authority");
  Require(FirstRowValue(show_result.api_result, "reference_finality_accepted") == "false",
          "Gate 004 reopen show migration accepted reference finality");

  const auto show_all = RequireMigrationRoute(
      "SHOW MIGRATIONS;",
      "op.show.migrations",
      "SBLR_SHOW_MIGRATIONS",
      "EngineShowMigrations",
      "rs.migration.list.v1");
  auto show_all_result = DispatchMigrationRoute(context, show_all);
  Require(show_all_result.api_result.ok, "Gate 004 reopen show migrations API failed");
  Require(!show_all_result.api_result.result_shape.rows.empty(),
          "Gate 004 reopen show migrations returned no rows");
  Require(FirstRowValue(show_all_result.api_result, "migration_uuid") == migration_uuid,
          "Gate 004 reopen show migrations did not list migration UUID");
  Require(FirstRowValue(show_all_result.api_result, "state") == "running",
          "Gate 004 reopen show migrations did not preserve running state");

  CommitEngineTransaction(context);
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RequireGeneratedRegistryRows();
  RequireNegativeParserShape("MIGRATE FROM REFERENCE postgres;",
                             "migrate_from_reference_requires_reference_profile_with_package_ref");
  RequireNegativeParserShape("ALTER MIGRATION migration_1 STOP;",
                             "alter_migration_requires_ref_and_start_pause_resume_abort_or_finalize");

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto migration_uuid = RequireMigrationEnginePath(path, database_uuid);
  RequireMigrationReopenPath(path, database_uuid, migration_uuid);
  RemoveDatabaseArtifacts(path);
  return EXIT_SUCCESS;
}
