// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "../support/native_catalog_column_fixture.hpp"
#include "../support/durable_authorization_fixture.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "ast/ast.hpp"
#include "canonical_sblr_admission_test_helper.hpp"
#include "binder/binder.hpp"
#include "catalog/schema_tree_api.hpp"
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

#include "../database_lifecycle/credentialed_database_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <stdexcept>
#include <utility>

namespace scratchbird::engine::internal_api {
template <typename... Args>
auto CheckedSchemaTreeRecords(Args&&... args) {
  EngineApiDiagnostic diagnostic;
  auto result = VisibleSchemaTreeRecords(std::forward<Args>(args)..., diagnostic);
  if (diagnostic.error) throw std::runtime_error(diagnostic.code + ":" + diagnostic.detail);
  return result;
}
}  // namespace scratchbird::engine::internal_api

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;

#ifndef SB_SBSFC021_SEED_PACK_ROOT
#define SB_SBSFC021_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr std::string_view kSql = "SHOW CREATE TABLE replay_target";
constexpr std::string_view kOperationId = "catalog.get_descriptor";
constexpr std::string_view kOpcode = "SBLR_CATALOG_GET_DESCRIPTOR";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kCanonicalAdmissionFamily = "sblr.catalog.introspect.v3";
const std::filesystem::path kFixtureDirectory = [] {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    auto path = std::filesystem::temp_directory_path() /
        ("sb_show_create_" + std::to_string(stamp) + "_" + std::to_string(attempt));
    std::error_code error;
    if (std::filesystem::create_directory(path, error)) return path;
    if (error) throw std::runtime_error("SHOW CREATE fixture directory creation failed");
  }
  throw std::runtime_error("SHOW CREATE fixture directory collision");
}();
const std::string kDatabasePath = (kFixtureDirectory / "database.sbdb").string();

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

bool HasBinaryTarget(const SblrEnvelope& envelope, const api::EngineUuid& expected) {
  const scratchbird::parser::sbsql::SblrOperand* selected = nullptr;
  for (const auto& operand : envelope.operands) {
    if (operand.name != "target_object_uuid") continue;
    if (selected) return false;
    selected = &operand;
  }
  return selected && selected->type == "uuid" && selected->value.empty() &&
         selected->canonical_value_kind == static_cast<std::uint16_t>(sblr::SblrValueKind::uuid_ref) &&
         selected->canonical_value_body.size() == 16 &&
         std::equal(selected->canonical_value_body.begin(), selected->canonical_value_body.end(),
                    expected.bytes.begin());
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
  session.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000701");
  session.connection_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000702");
  session.database_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000703");
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000704");
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
                            {scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000901")});
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
  namespace dt = scratchbird::core::datatypes;
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "SHOW CREATE datatype manifest unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest,
      dt::CanonicalTypeIdFromStableName(column.descriptor.canonical_type_name));
  Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
          "SHOW CREATE datatype row unavailable");
  const auto& datatype = row.manifest.descriptor_rows.front();
  const auto binding = dt::LookupDatatypeTypeCodecIdentityV1(
      api::kBootstrapDatatypeCatalogUuid, api::kBootstrapDatatypeCatalogGeneration,
      api::kBootstrapDatatypeRegistryGeneration, datatype.descriptor_uuid.value,
      datatype.descriptor_epoch);
  Require(binding.ok, "SHOW CREATE datatype codec unavailable");
  column.requested_column_uuid = scratchbird::tests::FixtureUuid(2015, 10 + ordinal);
  column.descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(2015, 20 + ordinal);
  column.descriptor.datatype_descriptor_uuid = binding.row.descriptor_uuid;
  column.descriptor.datatype_descriptor_generation = binding.row.descriptor_generation;
  column.descriptor.type_uuid = binding.row.type_uuid;
  column.descriptor.encoded_descriptor = scratchbird::tests::NativeCatalogColumnFixture(
      {{{"type", column.descriptor.canonical_type_name}, {"nullable", "true"}},
       {{"datatype_descriptor_uuid", binding.row.descriptor_uuid}, {"type_uuid", binding.row.type_uuid}}});
  column.nullable = true;
  return column;
}

api::EngineRequestContext BaseEngineContext(const api::EngineUuid& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-show-create-exact-route";
  context.database_path = std::string(kDatabasePath);
  context.security_context_present = true;
  context.database_uuid = database_uuid;
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000802");
  context.principal_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000803");
  context.node_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000804");
  context.cluster_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000805");
  context.statement_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000806");
  context.catalog_generation_id = 1;
  context.datatype_catalog_snapshot_uuid = api::kBootstrapDatatypeCatalogUuid;
  context.datatype_catalog_generation = api::kBootstrapDatatypeCatalogGeneration;
  context.datatype_registry_generation = api::kBootstrapDatatypeRegistryGeneration;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

void RemoveEngineFiles() {
  std::filesystem::remove_all(kFixtureDirectory);
}

api::EngineRequestContext CreateAndOpenEngineDatabase() {
  const auto created =
      scratchbird::tests::database_lifecycle::CreateCredentialedDatabaseFixture(
          std::filesystem::path(kDatabasePath), SB_SBSFC021_SEED_PACK_ROOT);
  if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << ':'
                                << created.diagnostic.message_key << '\n';
  Require(created.ok(), "credentialed fixture create failed while seeding SHOW CREATE");

  api::EngineOpenLifecycleRequest open;
  open.context = BaseEngineContext(
      created.state.database_uuid.value);
  open.context.principal_uuid = created.bootstrap_principal_uuid.value;
  open.context.default_root_uuid = created.state.filespace_uuid.value;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(open.context);
  const auto opened = api::EngineOpenLifecycle(open);
  if (!opened.ok) PrintApiDiagnostics(opened, "lifecycle.open_database");
  Require(opened.ok, "lifecycle.open_database failed while seeding SHOW CREATE fixture");
  return open.context;
}

api::EngineRequestContext SeedEngineTable() {
  auto context = CreateAndOpenEngineDatabase();
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
  table.requested_table_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000901");
  const auto visible_schemas = api::CheckedSchemaTreeRecords(
      context, context.local_transaction_id);
  Require(!visible_schemas.empty(),
          "credentialed SHOW CREATE fixture exposed no writable schema");
  table.target_schema.uuid = visible_schemas.front().schema_uuid;
  table.target_schema.object_kind = "schema";
  table.table_names.push_back(Name("replay_target"));
  table.table_columns.push_back(Column(0, "id", "int64"));
  const auto created = api::EngineCreateTable(table);
  if (!created.ok) PrintApiDiagnostics(created, "ddl.create_table");
  Require(created.ok, "ddl.create_table failed while seeding SHOW CREATE fixture");
  const auto epochs = api::LoadCatalogObjectLifecycleEpochState(context);
  Require(epochs.ok, "SHOW CREATE published catalog epochs unavailable");
  context.catalog_generation_id = epochs.state.metadata_epoch;
  context.name_resolution_epoch = epochs.state.name_resolution_epoch;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
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
  Require((std::find(artifacts.envelope.resolved_object_uuids.begin(), artifacts.envelope.resolved_object_uuids.end(), scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000901")) != artifacts.envelope.resolved_object_uuids.end()),
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
  Require(HasValue(artifacts.envelope.descriptor_requirements, "sys.catalog.object_descriptor"),
          "SHOW CREATE missing object descriptor ref");
  Require(Contains(artifacts.envelope.payload, "\"catalog_envelope_kind\":\"show_create\""),
          "SHOW CREATE payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload, "\"catalog_read_only\":true"),
          "SHOW CREATE payload did not prove read-only descriptor access");
  Require(HasBinaryTarget(artifacts.envelope,
              scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000901")) &&
              !Contains(artifacts.envelope.payload, "\"target_object_uuid\""),
          "SHOW CREATE must carry its exact target once as binary16, outside JSON text");
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
      scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(artifacts.envelope));
  Require(admission.admitted, "server admission rejected SHOW CREATE route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SHOW CREATE");
  Require(admission.operation_id == kOperationId,
          "server admission SHOW CREATE operation id mismatch");
  Require(admission.operation_family == kCanonicalAdmissionFamily,
          "server admission SHOW CREATE family mismatch");
}

void RequireEngineDispatch() {
  const auto context = SeedEngineTable();
  auto engine_envelope =
      scratchbird::test::sbsql::BuildCanonicalEngineSblrEnvelopeForTest(
          kOperationId, kOpcode, "trace.show_create.exact_route");
  engine_envelope.requires_security_context = true;
  engine_envelope.requires_transaction_context = false;
  engine_envelope.requires_cluster_authority = false;
  engine_envelope.contains_sql_text = false;
  engine_envelope.parser_resolved_names_to_uuids = true;

  api::EngineApiRequest api_request;
  api_request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000901");
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
  Require(result.api_result.primary_object.uuid == scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000901"),
          "catalog.get_descriptor returned wrong primary object");
  Require(result.api_result.result_shape.result_kind == "descriptor",
          "catalog.get_descriptor did not preserve descriptor result kind");
  Require(result.api_result.result_shape.columns.size() == 1,
          "SHOW CREATE descriptor shape must contain one table descriptor");
  std::vector<std::pair<std::string, std::string>> columns;
  const auto& encoded = result.api_result.result_shape.columns.front().encoded_descriptor;
  Require(api::DecodeMetadataPairs(encoded, &columns) && columns.size() == 1 &&
              columns.front().first == "id",
          "table descriptor must use complete binary column framing");
  api::CatalogColumnMetadata metadata;
  Require(api::DecodeCatalogColumnMetadata(columns.front().second, &metadata),
          "table descriptor lost binary column metadata");
  const auto expected_column = Column(0, "id", "int64");
  Require(api::BinaryCatalogUuid(metadata, "type_uuid") == expected_column.descriptor.type_uuid &&
              api::BinaryCatalogUuid(metadata, "datatype_descriptor_uuid") ==
                  expected_column.descriptor.datatype_descriptor_uuid,
          "table descriptor changed native datatype identities");
  auto damaged = encoded;
  damaged.back() ^= 1;
  const auto retained = columns;
  Require(!api::DecodeMetadataPairs(damaged, &columns) && columns == retained,
          "damaged table metadata was admitted or changed output");
  Require(!api::DecodeMetadataPairs(encoded + "trailing", &columns) && columns == retained,
          "table descriptor trailing data was admitted");
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
