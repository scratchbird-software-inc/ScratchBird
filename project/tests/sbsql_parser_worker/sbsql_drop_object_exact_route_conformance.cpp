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

constexpr std::string_view kOperationId = "ddl.drop_object";
constexpr std::string_view kOpcode = "SBLR_DDL_DROP_OBJECT";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kDropObjectSurfaceId = "SBSQL-40CAFAB37942";
constexpr std::string_view kDropObjectStmtSurfaceId = "SBSQL-CFFCCDEF6AC4";
constexpr std::string_view kObjectClassSurfaceId = "SBSQL-5CCF87EB0C5C";
constexpr std::string_view kQualifiedNameSurfaceId = "SBSQL-ADEF20254494";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000d200ff";

struct DropCase {
  std::string_view sql;
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view object_kind;
  std::string_view catalog_authority;
  std::string_view uuid;
};

const DropCase kDropCases[] = {
    {"DROP FILESPACE replay_filespace;", "SBSQL-1E702FF60BA0", "drop_filespace_stmt",
     "filespace", "sys.catalog.filespace", "019f0000-0000-7000-8000-000000d20001"},
    {"DROP POLICY replay_policy;", "SBSQL-25CE560681AB", "drop_policy_stmt",
     "policy", "sys.security.policy", "019f0000-0000-7000-8000-000000d20002"},
    {"DROP PRINCIPAL replay_principal;", "SBSQL-EF85496DB350", "drop_principal_stmt",
     "principal", "sys.security.principal", "019f0000-0000-7000-8000-000000d20003"},
    {"DROP ROUTINE replay_routine;", "SBSQL-66E94DC7813A", "drop_routine_stmt",
     "routine", "sys.catalog.routine", "019f0000-0000-7000-8000-000000d20004"},
    {"DROP SCHEDULE replay_schedule;", "SBSQL-B039B7B8F5C4", "drop_schedule_stmt",
     "schedule", "sys.scheduler.schedule", "019f0000-0000-7000-8000-000000d20005"},
    {"DROP JOB replay_job;", "SBSQL-D64BD9DCA318", "drop_job_stmt",
     "job", "sys.scheduler.job", "019f0000-0000-7000-8000-000000d20006"},
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_drop_object_exact_route_conformance";
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
      MemoryPolicy(), "sbsql_drop_object_exact_route_conformance");
  Require(configured.ok(), "DROP object memory fixture configuration failed");
  Require(configured.fixture_mode,
          "DROP object memory fixture mode was not active");
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
  session.session_uuid = "019f0000-0000-7000-8000-000000d20111";
  session.connection_uuid = "019f0000-0000-7000-8000-000000d20112";
  session.database_uuid = "019f0000-0000-7000-8000-000000d20113";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 101;
  session.security_policy_epoch = 102;
  session.descriptor_epoch = 103;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000d20114";
  config.bundle_contract_id = "sbp_sbsql@drop-object-route-test";
  config.build_id = "sbsql-drop-object-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(const DropCase& drop_case) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(drop_case.sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(drop_case.uuid), std::string(kSchemaUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryRow(std::string_view surface_id,
                        std::string_view canonical_name,
                        std::string_view surface_kind,
                        std::string_view family,
                        std::string_view sblr_family) {
  const auto* row = FindGeneratedSurfaceRegistryRowById(surface_id);
  Require(row != nullptr, "generated registry row missing");
  Require(row->canonical_name == canonical_name, "generated registry canonical name drifted");
  Require(row->surface_kind == surface_kind, "generated registry surface kind drifted");
  Require(row->family == family, "generated registry family drifted");
  Require(row->source_status == "native_now", "generated registry status drifted");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "generated registry cluster scope drifted");
  Require(row->sblr_operation_family == sblr_family,
          "generated registry SBLR family drifted");
}

void RequireRegistryEvidence() {
  RequireRegistryRow(kDropObjectSurfaceId, "drop_object", "canonical_surface",
                     "ddl_catalog", kFamily);
  RequireRegistryRow(kDropObjectStmtSurfaceId, "drop_object_stmt", "grammar_production",
                     "ddl_catalog", kFamily);
  RequireRegistryRow(kObjectClassSurfaceId, "object_class", "grammar_production",
                     "general", "sblr.general.operation.v3");
  RequireRegistryRow(kQualifiedNameSurfaceId, "qualified_name", "grammar_production",
                     "general", "sblr.general.operation.v3");
  for (const auto& drop_case : kDropCases) {
    RequireRegistryRow(drop_case.surface_id, drop_case.canonical_name,
                       "grammar_production", "ddl_catalog", kFamily);
  }
}

void RequireExactLowering(const DropCase& drop_case, const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "DROP object CST failed");
  Require(!artifacts.ast.messages.has_errors(), "DROP object AST failed");
  Require(artifacts.bound.bound, "DROP object bind failed");
  Require(artifacts.verifier.admitted, "DROP object verifier rejected exact route");
  Require(artifacts.envelope.operation_id == kOperationId, "DROP object operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "DROP object engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode, "DROP object opcode mismatch");
  Require(artifacts.envelope.operation_family == kFamily,
          "DROP object operation family mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "DROP object catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_drop_object_api_required"),
          "DROP object API authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.name_registry_retirement_required"),
          "DROP object name registry retirement authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "DROP object MGA authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "DROP object parser no-storage authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "DROP object parser no-SQL-execution authority step missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"drop_object_ddl\""),
          "DROP object payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload, drop_case.catalog_authority),
          "DROP object payload missing catalog authority");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_kind\":\"") +
                       std::string(drop_case.object_kind) + "\""),
          "DROP object payload missing target object kind");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"drop_target_uuid\":\"") +
                       std::string(drop_case.uuid) + "\""),
          "DROP object payload missing target UUID");
  Require(Contains(artifacts.envelope.payload, drop_case.surface_id),
          "DROP object payload missing row-specific surface evidence");
  Require(Contains(artifacts.envelope.payload, kDropObjectSurfaceId),
          "DROP object payload missing canonical drop_object evidence");
  Require(Contains(artifacts.envelope.payload, kDropObjectStmtSurfaceId),
          "DROP object payload missing drop_object_stmt evidence");
  Require(Contains(artifacts.envelope.payload, kObjectClassSurfaceId),
          "DROP object payload missing object_class evidence");
  Require(Contains(artifacts.envelope.payload, kQualifiedNameSurfaceId),
          "DROP object payload missing qualified_name evidence");
  Require(Contains(artifacts.envelope.payload, "\"target_name_text_included\":false"),
          "DROP object payload did not prove no target name text authority");
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "DROP object payload did not prove no name text authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "DROP object payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "DROP object payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, "replay_") &&
              !Contains(artifacts.envelope.payload, drop_case.sql),
          "DROP object payload embedded target identifier or SQL text as authority");
  Require(!Contains(artifacts.envelope.payload, "donor"),
          "DROP object payload carried donor authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "DROP object payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected DROP object exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for DROP object");
  Require(admission.operation_id == kOperationId,
          "server admission DROP object operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission DROP object operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(std::string(kOperationId));
  Require(opcode_entry != nullptr, "DROP object opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode, "DROP object opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "DROP object opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "DROP object opcode registry transaction context drifted");
}

std::filesystem::path TestDatabasePath() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("sbsql_drop_object_exact_route_" + std::to_string(now) + ".sbdb");
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
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1780108002000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1780108002001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1780108002002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DROP object engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-drop-object-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000d20121";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000d20122";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_family:drop_object");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.drop_object.exact_route.transaction.begin");
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

api::EngineApiRequest EngineDropObjectApiRequest(const DropCase& drop_case) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(drop_case.uuid);
  request.target_object.object_kind = std::string(drop_case.object_kind);
  return request;
}

sblr::SblrOperationEnvelope EngineEnvelope(std::string_view trace_key) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
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

  for (const auto& drop_case : kDropCases) {
    const std::string trace_key =
        "trace.drop_object.exact_route." + std::string(drop_case.surface_id);
    const sblr::SblrDispatchRequest request{
        context,
        EngineEnvelope(trace_key),
        EngineDropObjectApiRequest(drop_case)};
    const auto result = sblr::DispatchSblrOperation(request);
    Require(result.envelope_validated, "DROP object engine envelope did not validate");
    Require(result.accepted, "DROP object engine dispatch did not accept");
    Require(result.dispatched_to_api,
            "DROP object engine dispatch did not route to internal API");
    Require(result.api_result.ok, "EngineDropObject did not return success");
    Require(result.api_result.operation_id == kOperationId,
            "EngineDropObject returned wrong operation id");
    Require(result.api_result.primary_object.object_kind == drop_case.object_kind,
            "EngineDropObject returned wrong primary object kind");
    Require(result.api_result.primary_object.uuid.canonical == drop_case.uuid,
            "EngineDropObject returned wrong target UUID");
    Require(HasEvidence(result.api_result, drop_case.object_kind, drop_case.uuid),
            "EngineDropObject missing object-kind evidence");
    Require(HasEvidence(result.api_result, "name_registry_retired", drop_case.uuid),
            "EngineDropObject missing name registry retirement evidence");
  }

  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RequireRegistryEvidence();
  for (const auto& drop_case : kDropCases) {
    const auto artifacts = RunPipeline(drop_case);
    RequireExactLowering(drop_case, artifacts);
    RequireServerAdmission(artifacts.envelope);
  }
  RequireEngineDispatch();
  std::cout << "sbsql_drop_object_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
