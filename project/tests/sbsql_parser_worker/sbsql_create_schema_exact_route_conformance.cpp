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
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
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
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSql = "CREATE SCHEMA qa_schema;";
constexpr std::string_view kOperationId = "ddl.create_schema";
constexpr std::string_view kOpcode = "SBLR_DDL_CREATE_SCHEMA";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000023306";

struct CreateSchemaRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
};

constexpr std::array<CreateSchemaRowEvidence, 2> kCreateSchemaRows{{
    {"SBSQL-DE4B8AAF6326", "create_schema_stmt", "SBSQL-SURFACE-4F9512A05B14"},
    {"SBSQL-7BA0B928798B", "schema_name", "SBSQL-SURFACE-DF3A68E8CA6C"},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_create_schema_exact_route_conformance";
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
      MemoryPolicy(), "sbsql_create_schema_exact_route_conformance");
  Require(configured.ok(), "CREATE SCHEMA memory fixture configuration failed");
  Require(configured.fixture_mode,
          "CREATE SCHEMA memory fixture mode was not active");
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

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000023101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000023102";
  session.database_uuid = "019f0000-0000-7000-8000-000000023103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 23;
  session.security_policy_epoch = 24;
  session.descriptor_epoch = 25;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000023104";
  config.bundle_contract_id = "sbp_sbsql@create-schema-route-test";
  config.build_id = "sbsql-create-schema-route-test";
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
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  for (const auto& row : kCreateSchemaRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "CREATE SCHEMA generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "CREATE SCHEMA generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "CREATE SCHEMA generated registry kind drifted");
    Require(registry_row->family == "ddl_catalog",
            "CREATE SCHEMA generated registry family drifted");
    Require(registry_row->source_status == "native_now",
            "CREATE SCHEMA generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "CREATE SCHEMA generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == kFamily,
            "CREATE SCHEMA generated registry SBLR family drifted");
    Require(registry_row->parser_handler_key == "parser.statement_family.ddl_catalog",
            "CREATE SCHEMA generated registry parser handler drifted");
    Require(registry_row->lowering_handler_key ==
                "lowering.sblr_family.sblr_catalog_mutation_v3",
            "CREATE SCHEMA generated registry lowering handler drifted");
    Require(registry_row->server_admission_key == "server.admission.sblr_catalog_mutation_v3",
            "CREATE SCHEMA generated registry server admission drifted");
    Require(registry_row->engine_rule_key == "engine.rule.sblr_catalog_mutation_v3",
            "CREATE SCHEMA generated registry engine rule drifted");
    Require(registry_row->validation_fixture_id == row.validation_fixture_id,
            "CREATE SCHEMA generated registry fixture id drifted");
  }
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "CREATE SCHEMA CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE SCHEMA AST failed");
  Require(artifacts.bound.bound, "CREATE SCHEMA bind failed");
  Require(artifacts.verifier.admitted, "CREATE SCHEMA verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "CREATE SCHEMA operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "CREATE SCHEMA SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "CREATE SCHEMA operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "CREATE SCHEMA engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "CREATE SCHEMA SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "CREATE SCHEMA catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_schema_api_required"),
          "CREATE SCHEMA engine DDL authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "CREATE SCHEMA MGA catalog authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "CREATE SCHEMA parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "CREATE SCHEMA lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "CREATE SCHEMA lowering allowed reference/file effects");
  Require(Contains(artifacts.envelope.payload, "\"operation_id\":\"ddl.create_schema\""),
          "CREATE SCHEMA payload missing exact operation id");
  Require(Contains(artifacts.envelope.payload,
                   "\"sblr_operation\":\"SBLR_DDL_CREATE_SCHEMA\""),
          "CREATE SCHEMA payload missing exact SBLR opcode");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"create_schema_ddl\""),
          "CREATE SCHEMA payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload, "\"schema_name_parts\":1"),
          "CREATE SCHEMA payload missing schema name part evidence");
  for (const auto& row : kCreateSchemaRows) {
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            "CREATE SCHEMA payload missing row-identifiable surface evidence");
  }
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "CREATE SCHEMA payload did not prove no name text authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "CREATE SCHEMA payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "CREATE SCHEMA payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, "qa_schema") &&
              !Contains(artifacts.envelope.payload, kSql),
          "CREATE SCHEMA payload embedded SQL text or identifier names as authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "CREATE SCHEMA payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "CREATE SCHEMA payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected CREATE SCHEMA exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for CREATE SCHEMA");
  Require(admission.operation_id == kOperationId,
          "server admission CREATE SCHEMA operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission CREATE SCHEMA operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(kOperationId);
  Require(opcode_entry != nullptr, "CREATE SCHEMA opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode, "CREATE SCHEMA opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "CREATE SCHEMA opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "CREATE SCHEMA opcode registry transaction context drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_create_schema_exact_route_" + std::to_string(CurrentUnixMillis()) +
          ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.name_events",
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810233000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810233001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810233002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CREATE SCHEMA engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-create-schema-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000023202";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000023203";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-DE4B8AAF6326");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-7BA0B928798B");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.create_schema.exact_route.transaction.begin");
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

api::EngineApiRequest EngineCreateSchemaApiRequest() {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = "";
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::string(kSchemaUuid);
  request.target_object.object_kind = "schema";
  request.localized_names.push_back({"en", "primary", "", "qa_schema", true});
  return request;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.create_schema.exact_route.SBSQL-DE4B8AAF6326");
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
  const sblr::SblrDispatchRequest request{
      context,
      EngineEnvelope(),
      EngineCreateSchemaApiRequest()};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept CREATE SCHEMA");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineCreateSchema did not return success");
  Require(result.api_result.operation_id == kOperationId,
          "EngineCreateSchema returned wrong operation id");
  Require(result.api_result.primary_object.object_kind == "schema",
          "EngineCreateSchema did not return schema primary object");
  Require(result.api_result.primary_object.uuid.canonical == kSchemaUuid,
          "EngineCreateSchema returned wrong schema UUID");
  Require(HasEvidence(result.api_result, "api_behavior_event", "ddl.create_schema"),
          "EngineCreateSchema missing API behavior event evidence");
  Require(HasEvidence(result.api_result, "schema", kSchemaUuid),
          "EngineCreateSchema missing schema UUID evidence");
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_create_schema_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
