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
#include "sblr_opcode_registry.hpp"
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

constexpr std::string_view kSql =
    "CREATE STATISTICS stats_customer_ndistinct (NDISTINCT) ON id FROM replay_target;";
constexpr std::string_view kOperationId = "ddl.create_statistics";
constexpr std::string_view kOpcode = "SBLR_DDL_CREATE_STATISTICS";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSurfaceId = "SBSQL-442E76222244";
constexpr std::string_view kSurfaceName = "create_statistics_stmt";
constexpr std::string_view kFixtureId = "SBSQL-SURFACE-E00E98016322";
constexpr std::string_view kTableUuid = "019f0000-0000-7000-8000-000000442001";
constexpr std::string_view kStatisticsUuid = "019f0000-0000-7000-8000-000000442002";

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

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << "  " << field.name << '=' << field.value << '\n';
    }
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000442101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000442102";
  session.database_uuid = "019f0000-0000-7000-8000-000000442103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 44;
  session.security_policy_epoch = 45;
  session.descriptor_epoch = 46;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000442104";
  config.bundle_contract_id = "sbp_sbsql@create-statistics-route-test";
  config.build_id = "sbsql-create-statistics-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline() {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(kSql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound =
      BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session,
              {std::string(kTableUuid)});
  if (artifacts.bound.resolved_object_uuids.empty()) {
    artifacts.bound.resolved_object_uuids.push_back(std::string(kTableUuid));
  }
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(kSurfaceId);
  Require(registry_row != nullptr, "CREATE STATISTICS generated registry row missing");
  Require(registry_row->canonical_name == kSurfaceName,
          "CREATE STATISTICS generated registry canonical name drifted");
  Require(registry_row->surface_kind == "grammar_production",
          "CREATE STATISTICS generated registry kind drifted");
  Require(registry_row->family == "ddl_catalog",
          "CREATE STATISTICS generated registry family drifted");
  Require(registry_row->source_status == "native_now",
          "CREATE STATISTICS generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "CREATE STATISTICS generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == kFamily,
          "CREATE STATISTICS generated registry SBLR family drifted");
  Require(registry_row->parser_handler_key == "parser.statement_family.ddl_catalog",
          "CREATE STATISTICS generated registry parser handler drifted");
  Require(registry_row->lowering_handler_key ==
              "lowering.sblr_family.sblr_catalog_mutation_v3",
          "CREATE STATISTICS generated registry lowering handler drifted");
  Require(registry_row->server_admission_key == "server.admission.sblr_catalog_mutation_v3",
          "CREATE STATISTICS generated registry server admission drifted");
  Require(registry_row->engine_rule_key == "engine.rule.sblr_catalog_mutation_v3",
          "CREATE STATISTICS generated registry engine rule drifted");
  Require(registry_row->validation_fixture_id == kFixtureId,
          "CREATE STATISTICS generated registry fixture id drifted");
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "CREATE STATISTICS CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE STATISTICS AST failed");
  Require(artifacts.bound.bound, "CREATE STATISTICS bind failed");
  Require(artifacts.verifier.admitted, "CREATE STATISTICS verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "CREATE STATISTICS operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "CREATE STATISTICS SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "CREATE STATISTICS operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "CREATE STATISTICS engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "CREATE STATISTICS SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "CREATE STATISTICS catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_statistics_api_required"),
          "CREATE STATISTICS engine DDL authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "CREATE STATISTICS MGA catalog authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "CREATE STATISTICS parser no-storage authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "CREATE STATISTICS parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "CREATE STATISTICS lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "CREATE STATISTICS lowering allowed donor/file effects");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"create_statistics_ddl\""),
          "CREATE STATISTICS payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_authority\":\"sys.optimizer.statistics_descriptor\""),
          "CREATE STATISTICS payload missing statistics descriptor authority");
  Require(Contains(artifacts.envelope.payload,
                   "\"statistics_target_uuid\":\"019f0000-0000-7000-8000-000000442001\""),
          "CREATE STATISTICS payload missing table UUID evidence");
  Require(Contains(artifacts.envelope.payload, "\"statistics_kind\":\"ndistinct\""),
          "CREATE STATISTICS payload missing statistics kind evidence");
  Require(Contains(artifacts.envelope.payload, "\"statistics_expression_count\":1"),
          "CREATE STATISTICS payload missing expression count evidence");
  Require(Contains(artifacts.envelope.payload, kSurfaceId),
          "CREATE STATISTICS payload missing row-identifiable surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"SBSQL-5D191798949E\""),
          "CREATE STATISTICS payload missing statistics kind syntax evidence");
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "CREATE STATISTICS payload did not prove no name text authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "CREATE STATISTICS payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "CREATE STATISTICS payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, "stats_customer_ndistinct") &&
              !Contains(artifacts.envelope.payload, "replay_target") &&
              !Contains(artifacts.envelope.payload, std::string(kSql)),
          "CREATE STATISTICS payload embedded SQL text or identifier names as authority");
  Require(!Contains(artifacts.envelope.payload, "donor"),
          "CREATE STATISTICS payload carried donor authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "CREATE STATISTICS payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected CREATE STATISTICS exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for CREATE STATISTICS");
  Require(admission.operation_id == kOperationId,
          "server admission CREATE STATISTICS operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission CREATE STATISTICS operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(kOperationId);
  Require(opcode_entry != nullptr, "CREATE STATISTICS opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode,
          "CREATE STATISTICS opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "CREATE STATISTICS opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "CREATE STATISTICS opcode registry transaction context drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_create_statistics_exact_route_" + std::to_string(CurrentUnixMillis()) +
          ".sbdb");
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810442000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810442001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810442002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CREATE STATISTICS engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-create-statistics-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000442202";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000442203";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000442204";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-442E76222244");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.create_statistics.exact_route.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "transaction begin envelope did not validate");
  Require(result.accepted, "transaction begin dispatch did not accept");
  Require(result.api_result.ok, "transaction begin did not return success");
  Require(result.api_result.local_transaction_id != 0,
          "transaction begin did not return local transaction id");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  return context;
}

api::EngineApiRequest EngineCreateTableApiRequest() {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = "019f0000-0000-7000-8000-000000442204";
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::string(kTableUuid);
  request.target_object.object_kind = "table";
  request.localized_names.push_back({"en", "primary", "", "replay_target", true});
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical = "019f0000-0000-7000-8000-000000442205";
  column.names.push_back({"en", "primary", "", "id", true});
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "int";
  column.descriptor.encoded_descriptor = "type=int";
  column.ordinal = 0;
  column.nullable = true;
  request.columns.push_back(std::move(column));
  return request;
}

api::EngineApiRequest EngineCreateStatisticsApiRequest() {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = "019f0000-0000-7000-8000-000000442204";
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::string(kStatisticsUuid);
  request.target_object.object_kind = "statistics";
  request.related_objects.push_back({{std::string(kTableUuid)}, "table"});
  request.localized_names.push_back({"en", "primary", "", "stats_customer_ndistinct", true});
  request.option_envelopes.push_back("statistics_kind:ndistinct");
  request.option_envelopes.push_back("statistics_expression_count:1");
  request.option_envelopes.push_back("statistics_expression:identity:id");
  return request;
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

void RequireEngineDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  auto context = BeginEngineTransaction(path, database_uuid);

  const sblr::SblrDispatchRequest create_table_request{
      context,
      EngineEnvelope("ddl.create_table",
                     "SBLR_DDL_CREATE_TABLE",
                     "trace.create_statistics.exact_route.create_table"),
      EngineCreateTableApiRequest()};
  const auto table_result = sblr::DispatchSblrOperation(create_table_request);
  Require(table_result.envelope_validated, "CREATE TABLE setup envelope did not validate");
  Require(table_result.accepted, "CREATE TABLE setup dispatch did not accept");
  Require(table_result.api_result.ok, "CREATE TABLE setup did not return success");

  const sblr::SblrDispatchRequest create_statistics_request{
      context,
      EngineEnvelope(kOperationId,
                     kOpcode,
                     "trace.create_statistics.exact_route.SBSQL-442E76222244"),
      EngineCreateStatisticsApiRequest()};
  const auto result = sblr::DispatchSblrOperation(create_statistics_request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept CREATE STATISTICS");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineCreateStatistics did not return success");
  Require(result.api_result.operation_id == kOperationId,
          "EngineCreateStatistics returned wrong operation id");
  Require(result.api_result.primary_object.object_kind == "statistics",
          "EngineCreateStatistics did not return statistics primary object");
  Require(result.api_result.primary_object.uuid.canonical == kStatisticsUuid,
          "EngineCreateStatistics returned wrong statistics UUID");
  Require(HasEvidence(result.api_result, "api_behavior_event", kOperationId),
          "EngineCreateStatistics missing API behavior event evidence");
  Require(HasEvidence(result.api_result, "statistics_descriptor_created", kStatisticsUuid),
          "EngineCreateStatistics missing descriptor-created evidence");
  Require(HasEvidence(result.api_result, "statistics_target_table", kTableUuid),
          "EngineCreateStatistics missing target table evidence");
  Require(HasEvidence(result.api_result, "statistics_kind", "ndistinct"),
          "EngineCreateStatistics missing statistics kind evidence");
  Require(HasEvidence(result.api_result, "statistics_expression_count", "1"),
          "EngineCreateStatistics missing expression count evidence");
  Require(HasEvidence(result.api_result, "mga_table_visibility_checked", kTableUuid),
          "EngineCreateStatistics missing MGA table visibility evidence");
  Require(HasEvidence(result.api_result, "ddl_catalog_route",
                      "sys.optimizer.statistics_descriptor"),
          "EngineCreateStatistics missing catalog route evidence");
  Require(HasEvidence(result.api_result, "name_registry", kStatisticsUuid),
          "EngineCreateStatistics missing name registry evidence");
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_create_statistics_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
