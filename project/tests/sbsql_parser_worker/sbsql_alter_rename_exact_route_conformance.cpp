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

constexpr std::string_view kRenameSql = "RENAME TABLE replay_target TO renamed_target;";
constexpr std::string_view kAlterSql = "ALTER TABLE replay_target RENAME TO renamed_target;";
constexpr std::string_view kOperationId = "ddl.alter_object";
constexpr std::string_view kOpcode = "SBLR_DDL_ALTER_OBJECT";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kAlterObjectSurfaceId = "SBSQL-472ECFA63673";
constexpr std::string_view kAlterStmtSurfaceId = "SBSQL-6824451E6988";
constexpr std::string_view kAlterActionSurfaceId = "SBSQL-CFDD65DE9EA6";
constexpr std::string_view kRenameSurfaceId = "SBSQL-58224DEE5BCA";
constexpr std::string_view kObjectClassSurfaceId = "SBSQL-5CCF87EB0C5C";
constexpr std::string_view kQualifiedNameSurfaceId = "SBSQL-ADEF20254494";
constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000a17e01";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000a17e02";

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

bool ResultPayloadContains(const api::EngineApiResult& result, std::string_view expected) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      if (name == "payload" && Contains(value.encoded_value, expected)) return true;
    }
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
  session.session_uuid = "019f0000-0000-7000-8000-000000a17e11";
  session.connection_uuid = "019f0000-0000-7000-8000-000000a17e12";
  session.database_uuid = "019f0000-0000-7000-8000-000000a17e13";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 91;
  session.security_policy_epoch = 92;
  session.descriptor_epoch = 93;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000a17e14";
  config.bundle_contract_id = "sbp_sbsql@alter-rename-route-test";
  config.build_id = "sbsql-alter-rename-route-test";
  return config;
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
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(kTargetUuid), std::string(kSchemaUuid)});
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
  RequireRegistryRow(kAlterObjectSurfaceId, "alter_object", "canonical_surface",
                     "ddl_catalog", kFamily);
  RequireRegistryRow(kAlterStmtSurfaceId, "alter_object_stmt", "grammar_production",
                     "ddl_catalog", kFamily);
  RequireRegistryRow(kAlterActionSurfaceId, "alter_action", "grammar_production",
                     "ddl_catalog", kFamily);
  RequireRegistryRow(kRenameSurfaceId, "rename_object_stmt", "grammar_production",
                     "ddl_catalog", kFamily);
  RequireRegistryRow(kObjectClassSurfaceId, "object_class", "grammar_production",
                     "general", "sblr.general.operation.v3");
  RequireRegistryRow(kQualifiedNameSurfaceId, "qualified_name", "grammar_production",
                     "general", "sblr.general.operation.v3");
}

void RequireExactLowering(const PipelineArtifacts& artifacts, bool standalone_rename) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "ALTER/RENAME CST failed");
  Require(!artifacts.ast.messages.has_errors(), "ALTER/RENAME AST failed");
  Require(artifacts.bound.bound, "ALTER/RENAME bind failed");
  Require(artifacts.verifier.admitted, "ALTER/RENAME verifier rejected exact route");
  Require(artifacts.envelope.operation_id == kOperationId, "ALTER/RENAME operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "ALTER/RENAME engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode, "ALTER/RENAME opcode mismatch");
  Require(artifacts.envelope.operation_family == kFamily,
          "ALTER/RENAME operation family mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "ALTER/RENAME catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_alter_object_api_required"),
          "ALTER/RENAME API authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.name_registry_update_required"),
          "ALTER/RENAME name registry authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "ALTER/RENAME MGA authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "ALTER/RENAME parser no-storage authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "ALTER/RENAME parser no-SQL-execution authority step missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"alter_object_rename_ddl\""),
          "ALTER/RENAME payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload, "\"catalog_authority\":\"sys.name_registry\""),
          "ALTER/RENAME payload missing name registry authority");
  Require(Contains(artifacts.envelope.payload, "\"target_object_kind\":\"table\""),
          "ALTER/RENAME payload missing target object kind");
  Require(Contains(artifacts.envelope.payload,
                   "\"target_object_uuid\":\"019f0000-0000-7000-8000-000000a17e01\""),
          "ALTER/RENAME payload missing target UUID");
  Require(Contains(artifacts.envelope.payload, "\"new_name\":\"renamed_target\""),
          "ALTER/RENAME payload missing new-name user payload");
  Require(Contains(artifacts.envelope.payload, "\"new_name_text_is_user_payload\":true"),
          "ALTER/RENAME payload did not classify new name as user payload");
  Require(Contains(artifacts.envelope.payload, kAlterObjectSurfaceId),
          "ALTER/RENAME payload missing canonical alter_object row evidence");
  if (standalone_rename) {
    Require(Contains(artifacts.envelope.payload, kRenameSurfaceId),
            "RENAME payload missing rename_object_stmt row evidence");
  } else {
    Require(Contains(artifacts.envelope.payload, kAlterStmtSurfaceId),
            "ALTER RENAME payload missing alter_object_stmt row evidence");
    Require(Contains(artifacts.envelope.payload, kAlterActionSurfaceId),
            "ALTER RENAME payload missing alter_action row evidence");
  }
  Require(Contains(artifacts.envelope.payload, kObjectClassSurfaceId),
          "ALTER/RENAME payload missing object_class row evidence");
  Require(Contains(artifacts.envelope.payload, kQualifiedNameSurfaceId),
          "ALTER/RENAME payload missing qualified_name row evidence");
  Require(Contains(artifacts.envelope.payload, "\"target_name_text_included\":false"),
          "ALTER/RENAME payload did not prove no target name text authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "ALTER/RENAME payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "ALTER/RENAME payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, "replay_target") &&
              !Contains(artifacts.envelope.payload, std::string(kRenameSql)) &&
              !Contains(artifacts.envelope.payload, std::string(kAlterSql)),
          "ALTER/RENAME payload embedded target identifier or SQL text as authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "ALTER/RENAME payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "ALTER/RENAME payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected ALTER/RENAME exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for ALTER/RENAME");
  Require(admission.operation_id == kOperationId,
          "server admission ALTER/RENAME operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission ALTER/RENAME operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(std::string(kOperationId));
  Require(opcode_entry != nullptr, "ALTER/RENAME opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode, "ALTER/RENAME opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "ALTER/RENAME opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "ALTER/RENAME opcode registry transaction context drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_alter_rename_exact_route_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1780107902000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1780107902001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1780107902002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "ALTER/RENAME engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-alter-rename-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000a17e21";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000a17e22";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-472ECFA63673");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.alter_rename.exact_route.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "transaction begin envelope did not validate");
  Require(result.accepted, "transaction begin dispatch did not accept");
  Require(result.api_result.ok, "transaction begin did not return success");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  return context;
}

api::EngineApiRequest EngineAlterApiRequest() {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "table";
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.localized_names.push_back({"en", "primary", "", "renamed_target", true});
  request.option_envelopes.push_back("rename_target_kind:table");
  request.option_envelopes.push_back("rename_new_name:renamed_target");
  return request;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.alter_rename.exact_route.SBSQL-472ECFA63673");
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
  const auto context = BeginEngineTransaction(path, database_uuid);

  const sblr::SblrDispatchRequest alter_request{context, EngineEnvelope(), EngineAlterApiRequest()};
  const auto result = sblr::DispatchSblrOperation(alter_request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept ALTER/RENAME");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineAlterObject did not return success");
  Require(result.api_result.operation_id == kOperationId,
          "EngineAlterObject returned wrong operation id");
  Require(result.api_result.primary_object.object_kind == "object_alteration",
          "EngineAlterObject did not return object_alteration primary object");
  Require(result.api_result.primary_object.uuid.canonical == kTargetUuid,
          "EngineAlterObject returned wrong target UUID");
  Require(HasEvidence(result.api_result, "api_behavior_event", kOperationId),
          "EngineAlterObject missing API behavior event evidence");
  Require(HasEvidence(result.api_result, "object_alteration", kTargetUuid),
          "EngineAlterObject missing object alteration evidence");
  Require(HasEvidence(result.api_result, "name_registry", kTargetUuid),
          "EngineAlterObject missing name registry update evidence");
  Require(ResultPayloadContains(result.api_result, "localized_name_count=1"),
          "EngineAlterObject did not persist localized name payload");
  Require(ResultPayloadContains(result.api_result, "renamed_target"),
          "EngineAlterObject did not persist renamed target payload");
  Require(!result.api_result.catalog_row_uuid.canonical.empty(),
          "EngineAlterObject missing catalog row UUID evidence");
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto rename_artifacts = RunPipeline(kRenameSql);
  RequireExactLowering(rename_artifacts, true);
  RequireServerAdmission(rename_artifacts.envelope);
  const auto alter_artifacts = RunPipeline(kAlterSql);
  RequireExactLowering(alter_artifacts, false);
  RequireServerAdmission(alter_artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_alter_rename_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
