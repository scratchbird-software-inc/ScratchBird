// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../../support/binary_uuid_fixture.hpp"
#include "../../support/engine_statement_fixture.hpp"
#include "../../support/catalog_column_binding_fixture.hpp"
#include "../../support/catalog_text_binding_fixture.hpp"
#include <map>
#include <memory>
#include "api_types.hpp"
#include "ddl/create_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/merge_api.hpp"
#include "dml/select_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "memory.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include "../../database_lifecycle/credentialed_database_fixture.hpp"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;

namespace {

#ifndef SB_MISS008_SEED_PACK_ROOT
#define SB_MISS008_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr auto kDatabaseUuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000001");
constexpr auto kSchemaUuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000101");
constexpr auto kTableUuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000102");
constexpr auto kUniqueIdIndexUuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000103");

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  throw std::runtime_error(std::string(message));
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_missing_functionality_dml_upsert_variants_conformance";
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_missing_functionality_dml_upsert_variants_conformance");
  Require(configured.ok(), "MISS-008 memory fixture configuration failed");
  Require(configured.fixture_mode, "MISS-008 memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = (std::filesystem::temp_directory_path() /
                      "sb_miss008_dml.XXXXXX").string();
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || (std::holds_alternative<std::string>(evidence.evidence_id) && std::get<std::string>(evidence.evidence_id) == id))) {
      return true;
    }
  }
  return false;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineColumnDefinition Column(const api::EngineRequestContext& context, std::uint32_t ordinal, std::string name) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000300");
  Require(ordinal < 10, "fixture column ordinal out of range");
  column.requested_column_uuid.bytes[15] += ordinal;
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000400");
  column.descriptor.descriptor_uuid.bytes[15] += ordinal;
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  scratchbird::tests::BindFixtureColumnDatatype(context, scratchbird::core::datatypes::CanonicalTypeId::character, column);
  scratchbird::tests::BindFixtureUtf8BinaryTextResources(context, column);
  return column;
}

api::EngineIndexDefinition UniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000103");
  index.names.push_back(Name("miss008_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

api::EngineRowValue Row(api::EngineUuid row_uuid, std::string id, std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EnginePredicateEnvelope IdEquals(std::string id) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = "id";
  predicate.bound_values.push_back(TextValue(std::move(id)));
  return predicate;
}

api::EnginePredicateEnvelope IdIn(std::initializer_list<std::string_view> ids) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_in_list";
  predicate.canonical_predicate_envelope = "id";
  for (const auto id : ids) {
    predicate.bound_values.push_back(TextValue(std::string(id)));
  }
  return predicate;
}

api::EngineRequestContext fixture_owner;
std::map<api::EngineUuid, std::unique_ptr<scratchbird::tests::FixtureEngineSession>> fixture_sessions;
std::vector<std::unique_ptr<scratchbird::tests::FixtureEngineStatement>> fixture_statements;
struct OwnedFixtureRoot {
  std::filesystem::path path = MakeTempDir();
  ~OwnedFixtureRoot() {
    fixture_statements.clear(); fixture_sessions.clear();
    if (!path.empty()) { std::error_code error; std::filesystem::remove_all(path, error); }
  }
};
api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      api::EngineUuid session_uuid) {
  auto context = fixture_owner;
  Require(context.database_path == database_path.string(), "UPSERT bootstrap path mismatch");
  context.session_uuid = session_uuid;
  context.request_id = "miss008-dml-upsert-variants";
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  return context;
}

std::uint64_t EvidenceU64(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) continue;
    try {
      const auto* text = std::get_if<std::string>(&evidence.evidence_id);
      if (text == nullptr) return 0;
      return static_cast<std::uint64_t>(std::stoull(*text));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

api::EngineRequestContext BeginTransaction(const std::filesystem::path& database_path,
                                           api::EngineUuid session_uuid) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(database_path, session_uuid);
  auto begin = api::EngineBeginTransaction(request);
  Require(begin.ok, "MISS-008 transaction begin failed");
  auto context = BaseContext(database_path, session_uuid);
  context.local_transaction_id = begin.local_transaction_id;
  context.transaction_uuid = begin.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begin.snapshot_visible_through_local_transaction_id != 0
          ? begin.snapshot_visible_through_local_transaction_id
          : EvidenceU64(begin, "snapshot_visible_through_local_transaction_id");
  auto& session = fixture_sessions[context.session_uuid];
  if (!session) session = std::make_unique<scratchbird::tests::FixtureEngineSession>(context);
  fixture_statements.push_back(std::make_unique<scratchbird::tests::FixtureEngineStatement>(*session, context));
  return fixture_statements.back()->context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  auto commit = api::EngineCommitTransaction(request);
  Require(commit.ok, "MISS-008 commit failed");
}

void CreateSchemaAndTable(const std::filesystem::path& database_path) {
  auto context = BeginTransaction(database_path, scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000101"));

  api::EngineCreateSchemaRequest schema_request;
  schema_request.context = context;
  schema_request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000101");
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("miss008_schema"));
  auto schema = api::EngineCreateSchema(schema_request);
  Require(schema.ok, "MISS-008 schema create failed");

  api::EngineCreateTableRequest table_request;
  table_request.context = context;
  table_request.target_schema.uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000101");
  table_request.target_schema.object_kind = "schema";
  table_request.requested_table_uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000102");
  table_request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000102");
  table_request.target_object.object_kind = "table";
  table_request.table_names.push_back(Name("miss008_table"));
  table_request.table_columns.push_back(Column(context, 0, "id"));
  table_request.table_columns.push_back(Column(context, 1, "note"));
  table_request.table_indexes.push_back(UniqueIdIndex());
  auto table = api::EngineCreateTable(table_request);
  Require(table.ok, "MISS-008 table create failed");
  Commit(context);
}

void InsertRows(const api::EngineRequestContext& context,
                std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000102");
  request.target_table.object_kind = "table";
  request.input_rows = std::move(rows);
  auto inserted = api::EngineInsertRows(request);
  if (!inserted.ok) {
    for (const auto& diagnostic : inserted.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(inserted.ok, "MISS-008 insert rows failed");
  Require(HasEvidence(inserted, "audit_event", "data.dml_change"),
          "MISS-008 insert audit evidence missing");
  Require(HasEvidence(inserted, "dml_result_shape"),
          "MISS-008 insert result shape evidence missing");
}

void VerifyRights(const api::EngineRequestContext& context) {
  Require(api::SecurityContextHasRight(context, "INSERT", kTableUuid),
          "MISS-008 INSERT right not materialized");
  Require(api::SecurityContextHasRight(context, "UPDATE", kTableUuid),
          "MISS-008 UPDATE right not materialized");
  Require(api::SecurityContextHasRight(context, "DELETE", kTableUuid),
          "MISS-008 DELETE right not materialized");
  auto denied = context;
  denied.authorization_context.grants.clear();
  Require(api::SecurityContextHasRight(denied, "DELETE", kTableUuid),
          "MISS-008 actual bootstrap role authority was lost when explicit grants were empty");
  denied.authorization_context.engine_owned_bootstrap_role_uuid = {};
  Require(!api::SecurityContextHasRight(denied, "DELETE", kTableUuid),
          "MISS-008 empty grants unexpectedly authorized DELETE");
}

void VerifyUpsert(const api::EngineRequestContext& context) {
  api::EngineMergeRowsRequest request;
  request.context = context;
  request.target_table.uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000102");
  request.target_table.object_kind = "table";
  request.merge_surface_variant = "upsert";
  request.conflict_target_column = "id";
  request.on_conflict_action = "do_update";
  request.match_predicate.predicate_kind = "column_equals";
  request.match_predicate.canonical_predicate_envelope = "id";
  request.input_rows.push_back(Row(scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000301"),
                                   "1",
                                   "upsert-source"));
  request.input_rows.push_back(Row(scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000302"),
                                   "2",
                                   "upsert-inserted"));
  request.update_assignments.push_back({"note", TextValue("upsert-updated")});

  auto upserted = api::EngineMergeRows(request);
  Require(upserted.ok, "MISS-008 UPSERT merge failed");
  Require(upserted.matched_count == 1, "MISS-008 UPSERT matched count mismatch");
  Require(upserted.updated_count == 1, "MISS-008 UPSERT updated count mismatch");
  Require(upserted.inserted_count == 1, "MISS-008 UPSERT inserted count mismatch");
  Require(HasEvidence(upserted, "dml_surface_variant", "upsert"),
          "MISS-008 UPSERT surface evidence missing");
  Require(HasEvidence(upserted, "upsert_canonical_route", "dml.merge_rows"),
          "MISS-008 UPSERT canonical route evidence missing");
  Require(HasEvidence(upserted, "excluded_pseudo_relation",
                      "source_row_descriptor_bound"),
          "MISS-008 UPSERT EXCLUDED descriptor evidence missing");
  Require(HasEvidence(upserted, "merge_surface", "upsert_matched_update_or_insert"),
          "MISS-008 UPSERT merge surface evidence missing");
  Require(HasEvidence(upserted, "audit_event", "data.dml_change"),
          "MISS-008 UPSERT audit evidence missing");
}

void VerifyDeleteVariant(const api::EngineRequestContext& context,
                         std::string variant,
                         api::EnginePredicateEnvelope predicate,
                         std::string expected_evidence_kind,
                         std::string expected_evidence_id,
                         api::EngineApiU64 expected_deleted) {
  api::EngineDeleteRowsRequest request;
  request.context = context;
  request.target_table.uuid = scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000102");
  request.target_table.object_kind = "table";
  request.delete_surface_variant = std::move(variant);
  request.delete_predicate = std::move(predicate);
  if (request.delete_surface_variant == "batch_delete") {
    request.batch_on_column = "id";
    request.batch_limit_rows = expected_deleted;
  } else if (request.delete_surface_variant == "drop_series") {
    request.series_name = "series";
  }

  auto deleted = api::EngineDeleteRows(request);
  Require(deleted.ok, "MISS-008 delete variant failed");
  Require(deleted.deleted_count == expected_deleted,
          "MISS-008 delete variant deleted count mismatch");
  Require(HasEvidence(deleted, "dml_surface_variant", request.delete_surface_variant),
          "MISS-008 delete surface evidence missing");
  Require(HasEvidence(deleted, expected_evidence_kind, expected_evidence_id),
          "MISS-008 delete variant-specific evidence missing");
  Require(HasEvidence(deleted, "audit_event", "data.dml_change"),
          "MISS-008 delete audit evidence missing");
  Require(HasEvidence(deleted, "mga_row_version", "row_delete_tombstone"),
          "MISS-008 delete tombstone evidence missing");
}

}  // namespace

int main() try {
  ConfigureMemoryFixture();
  OwnedFixtureRoot owner;
  const auto work = owner.path;
  Require(!work.empty(), "MISS-008 failed to create temp directory");
  const auto database_path = work / "miss008.sbdb";

  namespace db = scratchbird::storage::database;
  namespace uuid = scratchbird::core::uuid;
  namespace platform = scratchbird::core::platform;
  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(platform::UuidKind::database, 1790638200000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(platform::UuidKind::filespace, 1790638200000).value;
  create.creation_unix_epoch_millis = 1790638200000;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = false;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "credentialed lifecycle fixture database create failed");
  fixture_owner = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  CreateSchemaAndTable(database_path);

  auto context = BeginTransaction(database_path, scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000201"));
  VerifyRights(context);
  InsertRows(context,
             {Row(scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000201"), "1", "seed"),
              Row(scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000211"), "b1", "batch-one"),
              Row(scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000212"), "b2", "batch-two"),
              Row(scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000221"), "e1", "erase-one"),
              Row(scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000231"), "s1", "series-one")});
  VerifyUpsert(context);
  VerifyDeleteVariant(context,
                      "batch_delete",
                      IdIn({"b1", "b2"}),
                      "delete_chunked_limit_applied",
                      "true",
                      1);
  VerifyDeleteVariant(context,
                      "erase",
                      IdEquals("e1"),
                      "erase_semantics",
                      "audit_safe_valid_time_close",
                      1);
  VerifyDeleteVariant(context,
                      "drop_series",
                      IdEquals("s1"),
                      "drop_series_semantics",
                      "series_tombstone",
                      1);
  Commit(context);

  fixture_statements.clear();
  fixture_sessions.clear();
  auto observer = BeginTransaction(database_path,
      scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000202"));
  api::EngineSelectRowsRequest select;
  select.context = observer;
  select.source_object.uuid = kTableUuid;
  select.source_object.object_kind = "table";
  const auto visible = api::EngineSelectRows(select);
  Require(visible.ok && visible.visible_count == 3 && visible.result_shape.rows.size() == 3,
          "MISS-008 independent session did not observe exact committed variant effects");
  std::map<std::string, std::pair<std::string, api::EngineUuid>> actual;
  for (const auto& row : visible.result_shape.rows) {
    std::string id, note;
    for (const auto& [name, value] : row.fields) {
      if (name == "id") { Require(!value.is_null, "MISS-008 committed key became null"); id=value.encoded_value; }
      if (name == "note") { Require(!value.is_null, "MISS-008 committed note became null"); note=value.encoded_value; }
    }
    Require(actual.emplace(id, std::make_pair(note, row.requested_row_uuid)).second,
            "MISS-008 independent session observed duplicate variant keys");
  }
  Require(actual.contains("1") && actual.at("1").first == "upsert-updated" &&
              actual.at("1").second == scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000201") &&
              actual.contains("2") && actual.at("2").first == "upsert-inserted" &&
              actual.at("2").second == scratchbird::tests::FixtureUuidLiteral("019f2800-0000-7000-8000-000000000302"),
          "MISS-008 committed UPSERT values or native lineage identities differ");
  Require(actual.contains("b1") != actual.contains("b2"),
          "MISS-008 bounded DELETE did not retain exactly one admitted batch row");
  Require((actual.contains("b1") && actual.at("b1").first == "batch-one") ||
              (actual.contains("b2") && actual.at("b2").first == "batch-two"),
          "MISS-008 retained batch row value changed");
  Require(!actual.contains("e1") && !actual.contains("s1"),
          "MISS-008 erased or dropped series row remained visible");
  Commit(observer);
  std::cout << "native_upsert_delete_variants_and_independent_poststate=passed\n";
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
