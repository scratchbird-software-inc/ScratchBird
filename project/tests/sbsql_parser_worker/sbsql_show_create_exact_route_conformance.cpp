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
#include "ddl/create_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;

#ifndef SB_SBSFC021_SEED_PACK_ROOT
#define SB_SBSFC021_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr std::string_view kSql = "SHOW CREATE TABLE replay_target";
constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000000901";
constexpr std::string_view kOperationId = "catalog.get_descriptor";
constexpr std::string_view kOpcode = "SBLR_CATALOG_GET_DESCRIPTOR";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kDatabasePath = "/tmp/sbsql_show_create_exact_route_conformance.sbdb";

struct ShowCreateRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
};

constexpr std::array<ShowCreateRowEvidence, 2> kShowCreateRows{{
    {"SBSQL-A424141B1639", "show_create", "SBSQL-SURFACE-1C1F6D6BFA03"},
    {"SBSQL-E8B6AB62C4F9", "show_create_target", "SBSQL-SURFACE-E4B270436333"},
}};

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

bool ApiResultHasEvidence(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return true;
  }
  return false;
}

bool ApiResultHasField(const api::EngineApiResult& result,
                       std::string_view name,
                       std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == name && field.second.encoded_value == value) return true;
    }
  }
  return false;
}

void PrintApiDiagnostics(const api::EngineApiResult& result,
                         std::string_view operation) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << operation << ' ' << diagnostic.code << ':'
              << diagnostic.message_key << ':' << diagnostic.detail << '\n';
  }
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_show_create_exact_route_conformance";
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_show_create_exact_route_conformance");
  Require(configured.ok(), "SHOW CREATE memory fixture configuration failed");
  Require(configured.fixture_mode, "SHOW CREATE memory fixture mode was not active");
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000000701";
  session.connection_uuid = "019f0000-0000-7000-8000-000000000702";
  session.database_uuid = "019f0000-0000-7000-8000-000000000703";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000000704";
  config.bundle_contract_id = "sbp_sbsql@show-create-route-test";
  config.build_id = "sbsql-show-create-route-test";
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
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(kTargetUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

api::EngineLocalizedName Name(std::string name) {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "primary";
  localized.name = std::move(name);
  localized.default_name = true;
  return localized;
}

api::EngineColumnDefinition Column(std::uint32_t ordinal,
                                   std::string name,
                                   std::string type) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.encoded_descriptor =
      std::string("type=") + column.descriptor.canonical_type_name;
  column.nullable = true;
  return column;
}

api::EngineRequestContext BaseEngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-show-create-exact-route";
  context.database_path = std::string(kDatabasePath);
  context.security_context_present = true;
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000000801";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000000802";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000000803";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000000804";
  context.cluster_uuid.canonical = "019f0000-0000-7000-8000-000000000805";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000000806";
  context.catalog_generation_id = 7;
  context.security_epoch = 11;
  context.resource_epoch = 13;
  context.name_resolution_epoch = 17;
  context.trace_tags.push_back("right:OBS_CATALOG_DESCRIPTOR_READ");
  return context;
}

void RemoveEngineFiles() {
  const std::filesystem::path path(kDatabasePath);
  std::filesystem::remove(path);
  for (const auto suffix : {
           ".sb.crud_events",
           ".sb.mga_row_versions",
           ".sb.mga_relation_metadata",
           ".sb.mga_index_entries",
           ".sb.mga_relation_descriptors",
           ".sb.mga_large_values",
           ".sb.mga_savepoints",
           ".sb.api_events",
           ".sb.catalog_object_events",
           ".sb.domain_events",
       }) {
    std::filesystem::remove(std::filesystem::path(std::string(kDatabasePath) + suffix));
  }
}

void CreateAndOpenEngineDatabase() {
  api::EngineCreateLifecycleRequest create;
  create.context = BaseEngineContext();
  create.option_envelopes.push_back(std::string("resource_seed_pack_root:") +
                                    SB_SBSFC021_SEED_PACK_ROOT);
  const auto created = api::EngineCreateLifecycle(create);
  if (!created.ok) PrintApiDiagnostics(created, "lifecycle.create_database");
  Require(created.ok, "lifecycle.create_database failed while seeding SHOW CREATE fixture");

  api::EngineOpenLifecycleRequest open;
  open.context = BaseEngineContext();
  const auto opened = api::EngineOpenLifecycle(open);
  if (!opened.ok) PrintApiDiagnostics(opened, "lifecycle.open_database");
  Require(opened.ok, "lifecycle.open_database failed while seeding SHOW CREATE fixture");
}

api::EngineRequestContext SeedEngineTable() {
  RemoveEngineFiles();
  CreateAndOpenEngineDatabase();
  auto context = BaseEngineContext();
  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  Require(begun.ok, "transaction.begin failed while seeding SHOW CREATE fixture");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;

  api::EngineCreateTableRequest table;
  table.context = context;
  table.requested_table_uuid.canonical = std::string(kTargetUuid);
  table.target_schema.uuid.canonical = "019f0000-0000-7000-8000-000000000902";
  table.target_schema.object_kind = "schema";
  table.table_names.push_back(Name("replay_target"));
  table.table_columns.push_back(Column(0, "id", "int64"));
  const auto created = api::EngineCreateTable(table);
  Require(created.ok, "ddl.create_table failed while seeding SHOW CREATE fixture");
  return context;
}

void RequireRegistryEvidence() {
  for (const auto& row : kShowCreateRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SHOW CREATE registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SHOW CREATE registry canonical name mismatch");
    Require(registry_row->surface_kind == "grammar_production",
            "SHOW CREATE registry surface kind mismatch");
    Require(registry_row->family == "ddl_catalog",
            "SHOW CREATE registry family mismatch");
    Require(registry_row->source_status == "native_now",
            "SHOW CREATE registry source status mismatch");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SHOW CREATE registry cluster scope mismatch");
    Require(registry_row->sblr_operation_family == kFamily,
            "SHOW CREATE registry SBLR family mismatch");
    Require(registry_row->parser_handler_key == "parser.statement_family.ddl_catalog",
            "SHOW CREATE registry parser handler mismatch");
    Require(registry_row->lowering_handler_key ==
                "lowering.sblr_family.sblr_catalog_mutation_v3",
            "SHOW CREATE registry lowering handler mismatch");
    Require(registry_row->server_admission_key ==
                "server.admission.sblr_catalog_mutation_v3",
            "SHOW CREATE registry server admission mismatch");
    Require(registry_row->engine_rule_key == "engine.rule.sblr_catalog_mutation_v3",
            "SHOW CREATE registry engine rule mismatch");
    Require(registry_row->validation_fixture_id == row.validation_fixture_id,
            "SHOW CREATE registry validation fixture mismatch");
  }
}

void RequireParserLoweringAndAdmission() {
  const auto artifacts = RunPipeline();
  Require(artifacts.bound.bound, "SHOW CREATE did not bind with resolved table UUID");
  Require(artifacts.bound.requires_name_resolution,
          "SHOW CREATE did not require server name resolution");
  Require(artifacts.bound.operation_family == kFamily,
          "SHOW CREATE bound operation family mismatch");
  Require(HasValue(artifacts.bound.required_rights, "right.observe"),
          "SHOW CREATE bound right.observe missing");
  Require(!HasValue(artifacts.bound.required_rights, "right.catalog_mutate"),
          "SHOW CREATE incorrectly requested catalog mutation right");
  Require(artifacts.envelope.operation_id == kOperationId,
          "SHOW CREATE operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "SHOW CREATE engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "SHOW CREATE SBLR opcode mismatch");
  Require(artifacts.envelope.operation_family == kFamily,
          "SHOW CREATE envelope operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "SHOW CREATE envelope operation key mismatch");
  Require(HasValue(artifacts.envelope.resolved_object_uuids, kTargetUuid),
          "SHOW CREATE envelope missing resolved target UUID");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.catalog_descriptor_api_required"),
          "SHOW CREATE missing catalog descriptor API authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.resolve_name_registry_public"),
          "SHOW CREATE missing public name registry authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SHOW CREATE missing no SQL execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SHOW CREATE missing no storage/finality authority");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.catalog.object_descriptor"),
          "SHOW CREATE missing object descriptor ref");
  Require(Contains(artifacts.envelope.payload, "\"catalog_envelope_kind\":\"show_create\""),
          "SHOW CREATE payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload, "\"catalog_read_only\":true"),
          "SHOW CREATE payload did not prove read-only descriptor access");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") +
                       std::string(kTargetUuid) + "\""),
          "SHOW CREATE payload missing target UUID");
  Require(Contains(artifacts.envelope.payload, "\"show_create_target_kind\":\"table\""),
          "SHOW CREATE payload missing table target kind");
  Require(Contains(artifacts.envelope.payload, "SBSQL-A424141B1639") &&
              Contains(artifacts.envelope.payload, "SBSQL-E8B6AB62C4F9"),
          "SHOW CREATE payload missing row surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "SHOW CREATE payload did not suppress name text");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SHOW CREATE payload did not suppress SQL text");
  Require(Contains(artifacts.envelope.payload,
                   "\"parser_renders_create_statement\":false"),
          "SHOW CREATE payload did not prove engine-side rendering");
  Require(!Contains(artifacts.envelope.payload, "replay_target"),
          "SHOW CREATE payload embedded source object name");
  Require(!Contains(artifacts.envelope.payload, std::string(kSql)),
          "SHOW CREATE payload embedded source SQL");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\"") &&
              !Contains(artifacts.envelope.payload, "\"sql_text\":"),
          "SHOW CREATE payload embedded source text fields");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SHOW CREATE payload carried WAL/recovery authority");
  Require(artifacts.verifier.admitted,
          "SHOW CREATE verifier rejected the exact route");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected SHOW CREATE route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SHOW CREATE");
  Require(admission.operation_id == kOperationId,
          "server admission SHOW CREATE operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission SHOW CREATE family mismatch");
}

void RequireEngineDispatch() {
  const auto context = SeedEngineTable();
  auto engine_envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                                std::string(kOpcode),
                                                "trace.show_create.exact_route");
  engine_envelope.requires_security_context = true;
  engine_envelope.requires_transaction_context = false;
  engine_envelope.requires_cluster_authority = false;
  engine_envelope.contains_sql_text = false;
  engine_envelope.parser_resolved_names_to_uuids = true;

  api::EngineApiRequest api_request;
  api_request.target_object.uuid.canonical = std::string(kTargetUuid);
  api_request.target_object.object_kind = "table";
  const sblr::SblrDispatchRequest request{context, engine_envelope, api_request};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(result.envelope_validated, "SHOW CREATE engine envelope did not validate");
  Require(result.accepted, "SHOW CREATE engine dispatch was not accepted");
  Require(result.dispatched_to_api, "SHOW CREATE did not dispatch to engine API");
  Require(result.api_result.ok, "catalog.get_descriptor returned a diagnostic");
  Require(result.api_result.operation_id == kOperationId,
          "catalog.get_descriptor returned wrong operation id");
  Require(result.api_result.primary_object.uuid.canonical == kTargetUuid,
          "catalog.get_descriptor returned wrong primary object");
  Require(result.api_result.result_shape.result_kind == "descriptor",
          "catalog.get_descriptor did not preserve descriptor result kind");
  Require(ApiResultHasEvidence(result.api_result, "table_descriptor_lookup"),
          "catalog.get_descriptor missing table descriptor evidence");
  Require(ApiResultHasEvidence(result.api_result, "show_create_statement"),
          "catalog.get_descriptor missing SHOW CREATE evidence");
  Require(ApiResultHasField(result.api_result,
                            "create_statement",
                            "CREATE TABLE replay_target (id int64)"),
          "catalog.get_descriptor did not render expected CREATE TABLE statement");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RequireRegistryEvidence();
  RequireParserLoweringAndAdmission();
  RequireEngineDispatch();
  RemoveEngineFiles();
  return EXIT_SUCCESS;
}
