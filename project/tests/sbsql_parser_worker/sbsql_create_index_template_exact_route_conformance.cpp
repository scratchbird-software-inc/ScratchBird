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

constexpr std::string_view kCreateSql =
    "CREATE OR REPLACE INDEX TEMPLATE replay_template "
    "INDEX_PATTERNS ('replay-*') COMPOSED_OF (replay_component) "
    "PRIORITY 10 VERSION 1 _META JSON '{}' TEMPLATE JSON '{}';";
constexpr std::string_view kDropSql = "DROP COMPONENT TEMPLATE replay_component;";
constexpr std::string_view kOperationId = "ddl.create_index_template";
constexpr std::string_view kOpcode = "SBLR_DDL_CREATE_INDEX_TEMPLATE";
constexpr std::string_view kDropOperationId = "ddl.drop_object";
constexpr std::string_view kDropOpcode = "SBLR_DDL_DROP_OBJECT";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSurfaceId = "SBSQL-07D017E18394";
constexpr std::string_view kSurfaceName = "create_index_template_stmt";
constexpr std::string_view kFixtureId = "SBSQL-SURFACE-75BBCC82D24F";
constexpr std::string_view kIndexTemplateUuid = "019f0000-0000-7000-8000-000000d07e01";
constexpr std::string_view kComponentTemplateUuid = "019f0000-0000-7000-8000-000000d07e02";

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

bool HasDiagnosticDetailContaining(const api::EngineApiResult& result, std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (Contains(diagnostic.detail, detail)) return true;
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
  session.session_uuid = "019f0000-0000-7000-8000-000000d07e11";
  session.connection_uuid = "019f0000-0000-7000-8000-000000d07e12";
  session.database_uuid = "019f0000-0000-7000-8000-000000d07e13";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 67;
  session.security_policy_epoch = 68;
  session.descriptor_epoch = 69;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000d07e14";
  config.bundle_contract_id = "sbp_sbsql@index-template-route-test";
  config.build_id = "sbsql-index-template-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql, std::vector<std::string> resolved = {}) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, resolved);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(kSurfaceId);
  Require(registry_row != nullptr, "CREATE INDEX TEMPLATE generated registry row missing");
  Require(registry_row->canonical_name == kSurfaceName,
          "CREATE INDEX TEMPLATE generated registry canonical name drifted");
  Require(registry_row->surface_kind == "grammar_production",
          "CREATE INDEX TEMPLATE generated registry kind drifted");
  Require(registry_row->family == "ddl_catalog",
          "CREATE INDEX TEMPLATE generated registry family drifted");
  Require(registry_row->source_status == "native_now",
          "CREATE INDEX TEMPLATE generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "CREATE INDEX TEMPLATE generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == kFamily,
          "CREATE INDEX TEMPLATE generated registry SBLR family drifted");
  Require(registry_row->validation_fixture_id == kFixtureId,
          "CREATE INDEX TEMPLATE generated registry fixture id drifted");
}

void RequireCreateLowering(const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "CREATE INDEX TEMPLATE CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE INDEX TEMPLATE AST failed");
  Require(artifacts.bound.bound, "CREATE INDEX TEMPLATE bind failed");
  Require(artifacts.verifier.admitted, "CREATE INDEX TEMPLATE verifier rejected exact route");
  Require(artifacts.envelope.operation_id == kOperationId,
          "CREATE INDEX TEMPLATE operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "CREATE INDEX TEMPLATE engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "CREATE INDEX TEMPLATE opcode mismatch");
  Require(artifacts.envelope.operation_family == kFamily,
          "CREATE INDEX TEMPLATE operation family mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "CREATE INDEX TEMPLATE catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_index_template_api_required"),
          "CREATE INDEX TEMPLATE API authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "CREATE INDEX TEMPLATE MGA catalog authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "CREATE INDEX TEMPLATE parser no-storage authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "CREATE INDEX TEMPLATE parser no-SQL-execution authority step missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"create_index_template_ddl\""),
          "CREATE INDEX TEMPLATE payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_authority\":\"sys.catalog.index_template\""),
          "CREATE INDEX TEMPLATE payload missing index template catalog authority");
  Require(Contains(artifacts.envelope.payload, "\"index_template_pattern_count\":1"),
          "CREATE INDEX TEMPLATE payload missing pattern-count evidence");
  Require(Contains(artifacts.envelope.payload, "\"index_template_composed_of_count\":1"),
          "CREATE INDEX TEMPLATE payload missing composed-of evidence");
  Require(Contains(artifacts.envelope.payload, "\"template_document_present\":true"),
          "CREATE INDEX TEMPLATE payload missing template document presence");
  Require(Contains(artifacts.envelope.payload, "\"template_document_embedded\":false"),
          "CREATE INDEX TEMPLATE payload embedded template document authority");
  Require(Contains(artifacts.envelope.payload, kSurfaceId),
          "CREATE INDEX TEMPLATE payload missing row-identifiable surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "CREATE INDEX TEMPLATE payload did not prove no name text authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "CREATE INDEX TEMPLATE payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "CREATE INDEX TEMPLATE payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, "replay_template") &&
              !Contains(artifacts.envelope.payload, "replay_component") &&
              !Contains(artifacts.envelope.payload, "replay-*") &&
              !Contains(artifacts.envelope.payload, std::string(kCreateSql)),
          "CREATE INDEX TEMPLATE payload embedded SQL text, template text, or identifier names as authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "CREATE INDEX TEMPLATE payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "CREATE INDEX TEMPLATE payload carried WAL/recovery authority");
}

void RequireDropLowering(const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(artifacts.bound.bound, "DROP INDEX TEMPLATE bind failed");
  Require(artifacts.verifier.admitted, "DROP INDEX TEMPLATE verifier rejected exact route");
  Require(artifacts.envelope.operation_id == kDropOperationId,
          "DROP INDEX TEMPLATE operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kDropOpcode,
          "DROP INDEX TEMPLATE opcode mismatch");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"drop_index_template_ddl\""),
          "DROP INDEX TEMPLATE payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_authority\":\"sys.catalog.component_template\""),
          "DROP COMPONENT TEMPLATE payload missing component template catalog authority");
  Require(Contains(artifacts.envelope.payload,
                   "\"target_object_uuid\":\"019f0000-0000-7000-8000-000000d07e02\""),
          "DROP INDEX TEMPLATE payload missing resolved target UUID");
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "DROP INDEX TEMPLATE payload did not prove no name text authority");
  Require(!Contains(artifacts.envelope.payload, "replay_component") &&
              !Contains(artifacts.envelope.payload, std::string(kDropSql)),
          "DROP INDEX TEMPLATE payload embedded SQL text or identifier names as authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope,
                            std::string_view operation_id,
                            std::string_view opcode) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected INDEX TEMPLATE exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for INDEX TEMPLATE");
  Require(admission.operation_id == operation_id,
          "server admission INDEX TEMPLATE operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission INDEX TEMPLATE operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(std::string(operation_id));
  Require(opcode_entry != nullptr, "INDEX TEMPLATE opcode registry row missing");
  Require(opcode_entry->opcode == opcode,
          "INDEX TEMPLATE opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "INDEX TEMPLATE opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "INDEX TEMPLATE opcode registry transaction context drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_create_index_template_exact_route_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1780107801000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1780107801001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1780107801002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CREATE INDEX TEMPLATE engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-create-index-template-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000d07e21";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000d07e22";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-07D017E18394");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.index_template.exact_route.transaction.begin");
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

api::EngineApiRequest EngineCreateIndexTemplateApiRequest(std::string_view uuid_text,
                                                          std::string_view object_kind,
                                                          std::string_view name) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(uuid_text);
  request.target_object.object_kind = std::string(object_kind);
  request.localized_names.push_back({"en", "primary", "", std::string(name), true});
  request.option_envelopes.push_back("index_template_kind:" + std::string(object_kind));
  request.option_envelopes.push_back("template_document_present:true");
  request.option_envelopes.push_back("template_document_hash:sha256:test-fixture-descriptor");
  request.option_envelopes.push_back("index_template_pattern_count:1");
  return request;
}

api::EngineApiRequest EngineDropObjectApiRequest(std::string_view uuid_text,
                                                 std::string_view object_kind) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(uuid_text);
  request.target_object.object_kind = std::string(object_kind);
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

  const sblr::SblrDispatchRequest missing_document_request{
      context,
      EngineEnvelope(kOperationId,
                     kOpcode,
                     "trace.index_template.exact_route.missing_document"),
      EngineDropObjectApiRequest(kIndexTemplateUuid, "index_template")};
  const auto missing_document = sblr::DispatchSblrOperation(missing_document_request);
  Require(missing_document.envelope_validated,
          "CREATE INDEX TEMPLATE missing document envelope did not validate");
  Require(missing_document.accepted,
          "CREATE INDEX TEMPLATE missing document dispatch did not accept");
  Require(!missing_document.api_result.ok,
          "CREATE INDEX TEMPLATE missing document unexpectedly succeeded");
  Require(HasDiagnosticDetailContaining(missing_document.api_result,
                                        "template_document_required"),
          "CREATE INDEX TEMPLATE missing document diagnostic did not prove descriptor requirement");

  const sblr::SblrDispatchRequest create_index_template_request{
      context,
      EngineEnvelope(kOperationId,
                     kOpcode,
                     "trace.index_template.exact_route.SBSQL-07D017E18394"),
      EngineCreateIndexTemplateApiRequest(kIndexTemplateUuid, "index_template", "replay_template")};
  const auto create_index_template = sblr::DispatchSblrOperation(create_index_template_request);
  Require(create_index_template.envelope_validated,
          "CREATE INDEX TEMPLATE engine envelope did not validate");
  Require(create_index_template.accepted,
          "CREATE INDEX TEMPLATE engine dispatch did not accept");
  Require(create_index_template.dispatched_to_api,
          "CREATE INDEX TEMPLATE engine dispatch did not route to internal API");
  Require(create_index_template.api_result.ok,
          "EngineCreateIndexTemplate did not return success");
  Require(create_index_template.api_result.operation_id == kOperationId,
          "EngineCreateIndexTemplate returned wrong operation id");
  Require(create_index_template.api_result.primary_object.object_kind == "index_template",
          "EngineCreateIndexTemplate returned wrong primary object kind");
  Require(create_index_template.api_result.primary_object.uuid.canonical == kIndexTemplateUuid,
          "EngineCreateIndexTemplate returned wrong index template UUID");
  Require(HasEvidence(create_index_template.api_result,
                      "index_template_descriptor_created",
                      kIndexTemplateUuid),
          "EngineCreateIndexTemplate missing descriptor evidence");
  Require(HasEvidence(create_index_template.api_result,
                      "template_document_bound",
                      "descriptor_payload"),
          "EngineCreateIndexTemplate missing template document binding evidence");
  Require(HasEvidence(create_index_template.api_result, "name_registry", kIndexTemplateUuid),
          "EngineCreateIndexTemplate missing name registry evidence");

  const sblr::SblrDispatchRequest create_component_template_request{
      context,
      EngineEnvelope(kOperationId,
                     kOpcode,
                     "trace.component_template.exact_route.SBSQL-07D017E18394"),
      EngineCreateIndexTemplateApiRequest(kComponentTemplateUuid,
                                          "component_template",
                                          "replay_component")};
  const auto create_component_template = sblr::DispatchSblrOperation(create_component_template_request);
  Require(create_component_template.api_result.ok,
          "EngineCreateIndexTemplate did not create component template");
  Require(create_component_template.api_result.primary_object.object_kind == "component_template",
          "EngineCreateIndexTemplate returned wrong component template kind");
  Require(HasEvidence(create_component_template.api_result,
                      "index_template_kind",
                      "component_template"),
          "EngineCreateIndexTemplate missing component template evidence");

  const sblr::SblrDispatchRequest drop_component_request{
      context,
      EngineEnvelope(kDropOperationId,
                     kDropOpcode,
                     "trace.component_template.exact_route.drop"),
      EngineDropObjectApiRequest(kComponentTemplateUuid, "component_template")};
  const auto drop_component = sblr::DispatchSblrOperation(drop_component_request);
  Require(drop_component.envelope_validated,
          "DROP COMPONENT TEMPLATE engine envelope did not validate");
  Require(drop_component.accepted,
          "DROP COMPONENT TEMPLATE engine dispatch did not accept");
  Require(drop_component.dispatched_to_api,
          "DROP COMPONENT TEMPLATE engine dispatch did not route to internal API");
  Require(drop_component.api_result.ok,
          "EngineDropObject did not return success for component template");
  Require(HasEvidence(drop_component.api_result,
                      "name_registry_retired",
                      kComponentTemplateUuid),
          "DROP COMPONENT TEMPLATE missing name registry retirement evidence");
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto create_artifacts = RunPipeline(kCreateSql);
  RequireCreateLowering(create_artifacts);
  RequireServerAdmission(create_artifacts.envelope, kOperationId, kOpcode);
  const auto drop_artifacts = RunPipeline(kDropSql, {std::string(kComponentTemplateUuid)});
  RequireDropLowering(drop_artifacts);
  RequireServerAdmission(drop_artifacts.envelope, kDropOperationId, kDropOpcode);
  RequireEngineDispatch();
  std::cout << "sbsql_create_index_template_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
