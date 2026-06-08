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
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSql = "CREATE DOMAIN email_address AS TEXT;";
constexpr std::string_view kOperationId = "ddl.create_domain";
constexpr std::string_view kOpcode = "SBLR_DDL_CREATE_DOMAIN";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSurfaceId = "SBSQL-8E675F371A9C";
constexpr std::string_view kSurfaceName = "create_domain_stmt";
constexpr std::string_view kFixtureId = "SBSQL-SURFACE-6933B79A0959";
constexpr std::string_view kDomainUuid = "019f0000-0000-7000-8000-0000008e6701";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-0000008e6702";

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
  session.session_uuid = "019f0000-0000-7000-8000-0000008e6801";
  session.connection_uuid = "019f0000-0000-7000-8000-0000008e6802";
  session.database_uuid = "019f0000-0000-7000-8000-0000008e6803";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 54;
  session.security_policy_epoch = 55;
  session.descriptor_epoch = 56;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-0000008e6804";
  config.bundle_contract_id = "sbp_sbsql@create-domain-route-test";
  config.build_id = "sbsql-create-domain-route-test";
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
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(kSurfaceId);
  Require(registry_row != nullptr, "CREATE DOMAIN generated registry row missing");
  Require(registry_row->canonical_name == kSurfaceName,
          "CREATE DOMAIN generated registry canonical name drifted");
  Require(registry_row->surface_kind == "grammar_production",
          "CREATE DOMAIN generated registry kind drifted");
  Require(registry_row->family == "ddl_catalog",
          "CREATE DOMAIN generated registry family drifted");
  Require(registry_row->source_status == "native_now",
          "CREATE DOMAIN generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "CREATE DOMAIN generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == kFamily,
          "CREATE DOMAIN generated registry SBLR family drifted");
  Require(registry_row->validation_fixture_id == kFixtureId,
          "CREATE DOMAIN generated registry fixture id drifted");
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "CREATE DOMAIN CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE DOMAIN AST failed");
  Require(artifacts.bound.bound, "CREATE DOMAIN bind failed");
  Require(artifacts.verifier.admitted, "CREATE DOMAIN verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "CREATE DOMAIN operation family mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "CREATE DOMAIN operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "CREATE DOMAIN engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "CREATE DOMAIN SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "CREATE DOMAIN catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_domain_api_required"),
          "CREATE DOMAIN engine DDL authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "CREATE DOMAIN MGA catalog authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "CREATE DOMAIN parser no-storage authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "CREATE DOMAIN parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "CREATE DOMAIN lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "CREATE DOMAIN lowering allowed donor/file effects");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"create_domain_ddl\""),
          "CREATE DOMAIN payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_authority\":\"sys.catalog.domain\""),
          "CREATE DOMAIN payload missing catalog authority");
  Require(Contains(artifacts.envelope.payload, "\"target_object_kind\":\"domain\""),
          "CREATE DOMAIN payload missing target object kind");
  Require(Contains(artifacts.envelope.payload, "\"domain_name_parts\":1"),
          "CREATE DOMAIN payload missing name-part evidence");
  Require(Contains(artifacts.envelope.payload, "\"base_canonical_type_name\":\"text\""),
          "CREATE DOMAIN payload missing base type evidence");
  Require(Contains(artifacts.envelope.payload, "\"base_encoded_descriptor\":\"type=text\""),
          "CREATE DOMAIN payload missing encoded descriptor evidence");
  Require(Contains(artifacts.envelope.payload, "\"base_descriptor_embedded\":true"),
          "CREATE DOMAIN payload missing descriptor transport evidence");
  Require(Contains(artifacts.envelope.payload, "\"domain_constraints_included\":false"),
          "CREATE DOMAIN payload overclaimed constraint support");
  Require(Contains(artifacts.envelope.payload, "\"domain_methods_included\":false"),
          "CREATE DOMAIN payload overclaimed method support");
  Require(Contains(artifacts.envelope.payload, kSurfaceId),
          "CREATE DOMAIN payload missing row-identifiable surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "CREATE DOMAIN payload did not prove no name text authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "CREATE DOMAIN payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "CREATE DOMAIN payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, "email_address") &&
              !Contains(artifacts.envelope.payload, std::string(kSql)),
          "CREATE DOMAIN payload embedded SQL text or identifier names as authority");
  Require(!Contains(artifacts.envelope.payload, "donor"),
          "CREATE DOMAIN payload carried donor authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "CREATE DOMAIN payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected CREATE DOMAIN exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for CREATE DOMAIN");
  Require(admission.operation_id == kOperationId,
          "server admission CREATE DOMAIN operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission CREATE DOMAIN operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(kOperationId);
  Require(opcode_entry != nullptr, "CREATE DOMAIN opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode,
          "CREATE DOMAIN opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "CREATE DOMAIN opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "CREATE DOMAIN opcode registry transaction context drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_create_domain_exact_route_" + std::to_string(CurrentUnixMillis()) +
          ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.domain_events",
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810580000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810580001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810580002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CREATE DOMAIN engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-create-domain-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-0000008e6901";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-0000008e6902";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-8E675F371A9C");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.create_domain.exact_route.transaction.begin");
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

api::EngineApiRequest EngineCreateDomainApiRequest() {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::string(kDomainUuid);
  request.target_object.object_kind = "domain";
  request.localized_names.push_back({"en", "primary", "", "email_address", true});
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor = "type=text";
  request.descriptors.push_back(std::move(descriptor));
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
  const auto context = BeginEngineTransaction(path, database_uuid);

  const sblr::SblrDispatchRequest create_domain_request{
      context,
      EngineEnvelope(kOperationId, kOpcode,
                     "trace.create_domain.exact_route.SBSQL-8E675F371A9C"),
      EngineCreateDomainApiRequest()};
  const auto result = sblr::DispatchSblrOperation(create_domain_request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept CREATE DOMAIN");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineCreateDomain did not return success");
  Require(result.api_result.operation_id == kOperationId,
          "EngineCreateDomain returned wrong operation id");
  Require(result.api_result.primary_object.object_kind == "domain",
          "EngineCreateDomain did not return domain primary object");
  Require(result.api_result.primary_object.uuid.canonical == kDomainUuid,
          "EngineCreateDomain returned wrong domain UUID");
  Require(HasEvidence(result.api_result, "domain_event", "domain_create"),
          "EngineCreateDomain missing domain create event evidence");
  Require(HasEvidence(result.api_result, "validation_hook", "engine_metadata"),
          "EngineCreateDomain missing validation hook evidence");
  Require(!result.api_result.catalog_row_uuid.canonical.empty(),
          "EngineCreateDomain missing catalog row UUID evidence");
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_create_domain_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
