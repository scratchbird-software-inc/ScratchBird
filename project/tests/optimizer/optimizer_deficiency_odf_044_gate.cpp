// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "memory.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "crud_support/bound_ordered_index_key.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "dml/update_api.hpp"
#include "dml/transactional_index_provider.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sorted_bulk_index_build.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  // Unwind live fixtures so failing regressions do not retain their databases.
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::u64 UniqueSeed() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, UniqueSeed() + salt);
  Require(generated.ok(), "ODF-044 UUID generation failed");
  return generated.value;
}

platform::Uuid NewNativeUuid(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  platform::Uuid database_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
  platform::Uuid table_uuid;
  platform::Uuid id_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    session.reset();
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  typed.setState(api::EngineValueState::value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string city) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"city", TextValue(std::move(city))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextFields(item.evidence_id) == id) {
      return true;
    }
  }
  return false;
}

bool EvidenceKindContains(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view token) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind &&
        scratchbird::tests::EvidenceTextFields(item.evidence_id).find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void AssertNoRuntimeDocLeaks(const std::vector<api::EngineEvidenceReference>& evidence) {
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans", "execution_plan", "findings", "contracts", "references"};
  for (const auto& item : evidence) {
    for (const auto token : forbidden) {
      Require(item.evidence_kind.find(token) == std::string::npos &&
                  scratchbird::tests::EvidenceTextFields(item.evidence_id).find(token) == std::string::npos,
              "ODF-044 runtime evidence leaked documentation token");
    }
  }
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context = fixture.owner_context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-044 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request), "ODF-044 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf044_sorted_bulk";
  const auto charset = api::LookupEngineResourceDescriptorByName(context, "UTF8", "charset");
  const auto collation = api::LookupEngineResourceDescriptorByName(context, "SB_UTF8_BINARY", "collation");
  Require(charset.ok && charset.resource_descriptor.present &&
              collation.ok && collation.resource_descriptor.present &&
              collation.resource_descriptor.parent_resource_uuid == charset.resource_descriptor.resource_uuid,
          "ODF-044 real UTF8 resource binding unavailable");
  api::CatalogColumnMetadata attributes;
  attributes.text = {{"canonical", "character"}, {"not_null", "true"},
                     {"character_length", "256"}};
  attributes.identities = {{"charset_uuid", charset.resource_descriptor.resource_uuid},
                          {"collation_uuid", collation.resource_descriptor.resource_uuid}};
  std::string encoded;
  Require(api::EncodeCatalogColumnMetadata(attributes, &encoded),
          "ODF-044 bound text column encoding failed");
  table.columns.push_back({"id", encoded});
  // The key-state tests below explicitly exercise NULL city values. Publish
  // that actual nullable column; the primary id remains nonnullable.
  attributes.text.erase("not_null");
  attributes.text["nullable"] = "true";
  Require(api::EncodeCatalogColumnMetadata(attributes, &encoded),
          "ODF-044 nullable city metadata encoding failed");
  table.columns.push_back({"city", encoded});
  return table;
}

api::CrudIndexRecord IdIndex(const Fixture& fixture,
                             const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.id_index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt, bool with_index) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf044_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf044.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-044 database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.table_uuid = NewNativeUuid(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewNativeUuid(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "odf044-metadata");
  RequireDiagnosticOk(scratchbird::tests::PublishMgaTableFixture(metadata, Table(fixture, metadata),
                          {"character", "character"}, with_index ? std::vector<api::CrudIndexRecord>{IdIndex(fixture, metadata)} : std::vector<api::CrudIndexRecord>{}),
                      "ODF-044 table metadata append failed");
  if (with_index) {
    RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata, IdIndex(fixture, metadata)),
                        "ODF-044 index metadata append failed");
  }
  Commit(metadata);
  fixture.owner_context.current_schema_uuid = metadata.current_schema_uuid;
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "odf044-session"));
  return fixture;
}

scratchbird::tests::FixtureEngineRequest<api::EngineExecuteImportRowsRequest> ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    bool sorted_build) {
  scratchbird::tests::FixtureEngineRequest<api::EngineExecuteImportRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes.push_back("copy_append_batching=enabled");
  if (sorted_build) {
    request.option_envelopes.push_back("sorted_bulk_index_build=enabled");
  }
  return request;
}

void CoreBuilderSortsAndRefusesUniqueDuplicates() {
  auto index_uuid = NewUuid(platform::UuidKind::object, 44000);
  auto table_uuid = NewUuid(platform::UuidKind::object, 44001);
  idx::SortedBulkIndexBuildRequest request;
  request.metadata.index_uuid = index_uuid;
  request.metadata.table_uuid = table_uuid;
  request.metadata.family = idx::IndexFamily::btree;
  request.metadata.family_name = "btree";
  request.metadata.semantic_profile = "rowstore_scalar_btree_v1";
  request.metadata.unique = false;
  request.metadata.rebuild = true;
  request.rows.push_back({"b",
                          NewUuid(platform::UuidKind::row, 44002).value,
                          NewUuid(platform::UuidKind::row, 44003).value,
                          "b",
                          0});
  request.rows.push_back({"a",
                          NewUuid(platform::UuidKind::row, 44004).value,
                          NewUuid(platform::UuidKind::row, 44005).value,
                          "a",
                          1});
  const auto built = idx::BuildSortedExactBulkIndex(request);
  Require(built.ok(), "ODF-044 core builder refused valid input");
  Require(built.bottom_up_build_selected,
          "ODF-044 core builder did not select bottom-up build");
  Require(built.entries.size() == 2 &&
              built.entries[0].encoded_key == "a" &&
              built.entries[1].encoded_key == "b",
          "ODF-044 core builder did not sort exact entries");
  Require(built.sorted_key_run_count == 2,
          "ODF-044 sorted key run counter drifted");
  Require(built.leaf_generation_count == 1 &&
              built.root_generation_count == 1,
          "ODF-044 deterministic leaf/root evidence drifted");

  request.metadata.unique = true;
  request.rows[1].encoded_key = "b";
  const auto duplicate = idx::BuildSortedExactBulkIndex(request);
  Require(!duplicate.ok(), "ODF-044 unique duplicate run was accepted");
  Require(duplicate.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-UNIQUE-DUPLICATE-BATCH",
          "ODF-044 duplicate diagnostic drifted");
}

void DirectBulkUsesSortedExactIndexBuild() {
  auto fixture = MakeFixture("direct", 44100, true);
  auto context = Begin(fixture, "odf044-direct");
  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    context,
                    {Row("004", "oslo"),
                     Row("001", "zurich"),
                     Row("003", "quito"),
                     Row("002", "berlin")},
                    true));
  RequireOk(imported, "ODF-044 direct sorted bulk import failed");
  Require(imported.inserted_rows == 4,
          "ODF-044 direct sorted row count drifted");
  Require(HasEvidence(imported.evidence,
                      "sorted_bulk_index_build_selected",
                      "true"),
          "ODF-044 sorted build selected evidence missing");
  Require(HasEvidence(imported.evidence,
                      "sorted_bulk_index_exact_append",
                      "mga_index_append_path"),
          "ODF-044 exact append path evidence missing");
  Require(HasEvidence(imported.evidence,
                      "sorted_bulk_index_uniqueness_proof",
                      "sorted_duplicate_runs_absent"),
          "ODF-044 uniqueness proof evidence missing");
  Require(HasEvidence(imported.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-044 MGA authority evidence missing");
  AssertNoRuntimeDocLeaks(imported.evidence);
  Commit(context);
}

void CreateIndexBackfillsWithSortedExactBuild() {
  auto fixture = MakeFixture("ddl", 44200, false);
  auto insert_context = Begin(fixture, "odf044-ddl-load");
  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    insert_context,
                    {Row("001", "zurich"),
                     Row("002", "oslo"),
                     Row("003", "berlin")},
                    false));
  RequireOk(imported, "ODF-044 preload import failed");
  Commit(insert_context);

  auto ddl_context = Begin(fixture, "odf044-create-index");
  const platform::Uuid city_index_uuid =
      NewNativeUuid(platform::UuidKind::object, 44250);
  scratchbird::tests::FixtureEngineRequest<api::EngineCreateIndexRequest> request(
      *fixture.session, ddl_context);
  request.target_object.uuid = fixture.table_uuid;
  request.target_object.object_kind = "table";
  api::EngineIndexDefinition definition;
  definition.requested_index_uuid = city_index_uuid;
  definition.physical_profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  definition.key_envelopes.push_back("city");
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "canonical";
  name.name = "odf044_city_idx";
  name.default_name = true;
  definition.names.push_back(name);
  request.indexes.push_back(definition);

  const auto created = api::EngineCreateIndex(request);
  RequireOk(created, "ODF-044 create index failed");
  Require(HasEvidence(created.evidence,
                      "sorted_bulk_index_build_route",
                      "ddl.create_index"),
          "ODF-044 DDL sorted route evidence missing");
  Require(HasEvidence(created.evidence,
                      "sorted_bulk_index_root_publish_fence",
                      "mga_index_append_path_after_bottom_up_root_generation"),
          "ODF-044 root publish fence evidence missing");
  Require(HasEvidence(created.evidence,
                      "sorted_bulk_index_retail_append_bypass",
                      "bottom_up_build_selected"),
          "ODF-044 retail append bypass evidence missing");
  AssertNoRuntimeDocLeaks(created.evidence);

  const auto loaded = api::LoadMgaRelationStoreState(ddl_context);
  Require(loaded.ok, "ODF-044 relation store reload failed");
  std::vector<std::string> keys;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.index_uuid == city_index_uuid) {
      Require(entry.key_value.starts_with("SBKOBIN:SBKO"),
              "DDL sorted build did not retain a physical ordered key");
      keys.push_back(entry.payload_value);
    }
  }
  Require(keys.size() == 3, "ODF-044 DDL exact index entry count drifted");
  Require(keys[0] == std::string("SBCLKEY2\x01\x00\x00\x00\x00\x06\x00\x00\x00" "berlin", 23) && keys[1] == std::string("SBCLKEY2\x01\x00\x00\x00\x00\x04\x00\x00\x00" "oslo", 21) && keys[2] == std::string("SBCLKEY2\x01\x00\x00\x00\x00\x06\x00\x00\x00" "zurich", 23),
          "ODF-044 DDL exact index entries were not sorted");
  std::string ddl_oslo_key;
  for (const auto& entry : loaded.state.index_entries)
    if (entry.index_uuid == city_index_uuid && entry.payload_value == keys[1])
      ddl_oslo_key = entry.key_value;
  Require(!ddl_oslo_key.empty(), "ODF-044 DDL equality control missing");
  Commit(ddl_context);

  auto append_context = Begin(fixture, "odf044-after-ddl-import");
  RequireOk(api::EngineExecuteImportRows(ImportRequest(
      fixture, append_context, {Row("004", "oslo")}, false)),
      "ODF-044 post-DDL typed import failed");
  const auto appended = api::LoadMgaRelationStoreState(append_context);
  Require(appended.ok, "ODF-044 post-DDL index reload failed");
  unsigned oslo_entries = 0;
  for (const auto& entry : appended.state.index_entries) {
    if (entry.index_uuid != city_index_uuid || entry.payload_value != keys[1]) continue;
    ++oslo_entries;
    Require(entry.key_value == ddl_oslo_key,
            "ODF-044 DDL and later typed import encoded unequal keys for the same bound value");
  }
  Require(oslo_entries == 2, "ODF-044 post-DDL import omitted its index membership");
  Commit(append_context);

  // Independent v2 packet: one row, two text columns, no NULLs. This drives
  // the native packet lane instead of materializing canonical_rows first.
  auto native_context = Begin(fixture, "odf044-after-ddl-native");
  scratchbird::tests::FixtureEngineRequest<api::EngineExecuteNativeBulkIngestRequest> native(
      *fixture.session, native_context);
  native.target_table = request.target_object;
  native.import_policy.reject_mode = "fail_fast";
  native.import_policy.reject_payload_policy = "diagnostic_only";
  native.import_policy.resume_policy = "fail_closed";
  native.checkpoint_policy.checkpoint_mode = "disabled";
  auto& packet = native.native_row_packet;
  packet.present = true;
  packet.version = 2;
  packet.row_count = 1;
  packet.column_count = 2;
  packet.field_order = {"id", "city"};
  packet.column_type_tags = {1, 1};
  packet.packet_bytes = {'S','B','N','R',2,0,0,0,1,0,0,0,0,0,0,0,
      2,0,0,0,1,1,2,0,0,0,'i','d',4,0,0,0,'c','i','t','y',
      0,3,0,0,0,'0','0','5',4,0,0,0,'o','s','l','o'};
  packet.row_offsets = {36};
  packet.row_sizes = {16};
  const auto native_result = api::EngineExecuteNativeBulkIngest(native);
  RequireOk(native_result, "ODF-044 post-DDL native packet append failed");
  Require(native_result.inserted_rows == 1 && native_result.rejected_rows == 0,
          "ODF-044 native packet append did not insert exactly one row");
  Commit(native_context);

  auto sorted_context = Begin(fixture, "odf044-after-ddl-sorted");
  const auto sorted_result = api::EngineExecuteImportRows(ImportRequest(
      fixture, sorted_context, {Row("006", "oslo")}, true));
  RequireOk(sorted_result, "ODF-044 post-DDL sorted append failed");
  Require(sorted_result.inserted_rows == 1, "ODF-044 sorted append count drifted");
  Commit(sorted_context);

  auto update_context = Begin(fixture, "odf044-after-ddl-update");
  scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> update(
      *fixture.session, update_context);
  update.target_table = request.target_object;
  update.update_predicate.predicate_kind = "column_equals";
  update.update_predicate.canonical_predicate_envelope = "id";
  update.update_predicate.bound_values.push_back(TextValue("003"));
  update.assignments.push_back({"city", TextValue("oslo")});
  const auto updated = api::EngineUpdateRows(update);
  RequireOk(updated, "ODF-044 post-DDL UPDATE failed");
  Require(updated.matched_count == 1 && updated.updated_count == 1,
          "ODF-044 UPDATE did not change exactly its selected row");
  Commit(update_context);

  auto rebuild_context = Begin(fixture, "odf044-after-ddl-rebuild");
  const auto before_rebuild = api::LoadMgaRelationStoreState(rebuild_context);
  Require(before_rebuild.ok, "ODF-044 pre-rebuild reload failed");
  const auto rebuild_state = api::BuildCrudCompatibilityStateFromMga(before_rebuild.state);
  oslo_entries = 0;
  for (const auto& entry : before_rebuild.state.index_entries) {
    if (entry.index_uuid != city_index_uuid || entry.payload_value != keys[1]) continue;
    ++oslo_entries;
    Require(entry.key_value == ddl_oslo_key,
            "ODF-044 native/sorted/UPDATE key differs from DDL for the same value");
  }
  Require(oslo_entries == 5, "ODF-044 cross-producer memberships missing");
  const auto found = std::find_if(rebuild_state.indexes.begin(),
      rebuild_state.indexes.end(), [&](const auto& index) {
        return index.index_uuid == city_index_uuid;
      });
  Require(found != rebuild_state.indexes.end(), "ODF-044 durable index missing");
  api::MgaRelationHotAppendContext rebuild_append(rebuild_context);
  api::MgaTransactionalIndexProvider provider(rebuild_context, &rebuild_append);
  const auto refused = provider.RebuildFromRelation(rebuild_state, *found, false);
  Require(!refused.ok && refused.diagnostic.code ==
              "INDEX.TRANSACTIONAL_PROVIDER.REBUILD_HORIZON_REQUIRED",
          "ODF-044 rebuild ignored the cleanup horizon");
  const auto rebuilt = provider.RebuildFromRelation(rebuild_state, *found, true);
  RequireDiagnosticOk(rebuilt.diagnostic, "ODF-044 index rebuild failed");
  Require(rebuilt.ok && rebuilt.rebuilt_entry_count == 6,
          "ODF-044 rebuild did not cover every current row");
  RequireDiagnosticOk(rebuild_append.FlushIndexEntries(), "ODF-044 rebuild flush failed");
  Commit(rebuild_context);

  auto reader_context = Begin(fixture, "odf044-after-rebuild-reader");
  const auto reopened = api::LoadMgaRelationStoreState(reader_context);
  Require(reopened.ok, "ODF-044 rebuilt index reload failed");
  unsigned rebuilt_oslo = 0;
  for (const auto& entry : reopened.state.index_entries) {
    if (entry.index_uuid != city_index_uuid || entry.payload_value != keys[1]) continue;
    Require(entry.key_value == ddl_oslo_key, "ODF-044 rebuild changed physical key encoding");
    if (entry.entry_kind == "rebuild" && entry.creator_tx == rebuild_context.local_transaction_id)
      ++rebuilt_oslo;
  }
  Require(rebuilt_oslo == 5, "ODF-044 rebuild omitted current Oslo memberships");
  const auto validated = api::MgaTransactionalIndexProvider(reader_context, nullptr)
      .ValidateAgainstRelation(api::BuildCrudCompatibilityStateFromMga(reopened.state), *found);
  RequireDiagnosticOk(validated.diagnostic, "ODF-044 rebuilt memberships failed validation");
  Require(validated.ok && validated.visible_entry_count == 6,
          "ODF-044 rebuilt index does not match visible relation");
  Commit(reader_context);
}

void OrderedKeyAuthorityAndValueStates() {
  auto fixture = MakeFixture("bound_keys", 44300, false);
  auto context = Begin(fixture, "odf044-bound-key-authority");
  auto index = IdIndex(fixture, context);
  index.key_envelopes = {"city"};
  index.column_name = "city";
  std::vector<api::bound_index_key::OrderedIndexColumn> columns;
  api::EngineApiDiagnostic diagnostic;
  Require(api::bound_index_key::BindOrderedIndexColumns(context, index, fixture.table_uuid,
                                                   &columns, &diagnostic),
          "ODF-044 real ordered-column binding failed");
  const auto key = [&](const std::vector<api::CrudStoredValue>& values,
                       const std::vector<api::bound_index_key::OrderedIndexColumn>& binding) {
    std::string encoded;
    bool is_null = false;
    const bool ok = api::bound_index_key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey(values),
                binding, &encoded, &is_null, &diagnostic);
    if (!ok) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Require(ok, "ODF-044 bound key encoding failed");
    Require(is_null == std::any_of(values.begin(), values.end(), [](const auto& value) {
              return value.isSqlNull(); }), "ODF-044 index NULL state was inferred from payload bytes");
    return encoded;
  };
  Require(key({api::CrudStoredValue::SqlNull()}, columns) < key({""}, columns) &&
              key({""}, columns) < key({"<NULL>"}, columns) &&
              key({"berlin"}, columns) < key({"oslo"}, columns),
          "ODF-044 character length/state framing controls physical order");
  auto compound = columns;
  compound.push_back(columns.front());
  Require(key({"aa", "z"}, compound) < key({"b", "a"}, compound) &&
              key({"same", "aa"}, compound) < key({"same", "b"}, compound),
          "ODF-044 compound component ordering drifted");
  auto invalid_context = context;
  ++invalid_context.datatype_catalog_generation;
  auto unchanged = columns;
  Require(!api::bound_index_key::BindOrderedIndexColumns(invalid_context, index, fixture.table_uuid,
                &unchanged, &diagnostic) && unchanged.size() == columns.size(),
          "ODF-044 stale datatype cohort was accepted");
  std::string output = "unchanged";
  bool null_key = false;
  auto unbound = columns;
  unbound.front().text_seed = {};
  Require(!api::bound_index_key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({"city"}),
              unbound, &output, &null_key, &diagnostic) && output == "unchanged",
          "ODF-044 missing collation authority silently used raw byte ordering");
  Require(!api::bound_index_key::EncodeOrderedIndexKey("city", columns, &output, &null_key, &diagnostic) &&
              output == "unchanged", "ODF-044 malformed logical key altered output");
  namespace dt = scratchbird::core::datatypes;
  const auto registry = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  for (const auto type : {dt::CanonicalTypeId::int32, dt::CanonicalTypeId::uuid,
                         dt::CanonicalTypeId::real128, dt::CanonicalTypeId::decimal,
                         dt::CanonicalTypeId::decimal_float}) {
    const auto row = std::find_if(registry.begin(), registry.end(), [&](const auto& candidate) {
      return candidate.canonical_binary_type_code == static_cast<std::uint32_t>(type);
    });
    Require(row != registry.end(), "ODF-044 canonical datatype fixture missing");
    api::bound_index_key::OrderedIndexColumn binding;
    Require(dt::LookupDatatypeStorageIdentityV1(context.datatype_catalog_snapshot_uuid,
                context.datatype_catalog_generation, context.datatype_registry_generation,
                row->descriptor_uuid, row->descriptor_generation, &binding.datatype),
            "ODF-044 native datatype fixture authority missing");
    binding.descriptor = {platform::UuidKind::object, row->descriptor_uuid};
    auto source = scratchbird::engine::executor::MakeExecutorDescriptor(
        dt::CanonicalTypeName(type), "nullability=nullable");
    std::string binding_detail;
    Require(api::bound_index_key::BuildOrderedColumnExecutionDescriptor(
                source, binding.datatype, true, &binding.execution_descriptor, &binding_detail),
            "ODF-044 native execution descriptor binding missing");
    if (type == dt::CanonicalTypeId::int32) {
      Require(key({std::string{"\xf6\xff\xff\xff", 4}}, {binding}) < key({std::string{"\x02\0\0\0", 4}}, {binding}) &&
                  key({std::string{"\x02\0\0\0", 4}}, {binding}) < key({std::string{"\x64\0\0\0", 4}}, {binding}),
              "ODF-044 integer physical order is lexical");
    } else if (type == dt::CanonicalTypeId::decimal) {
      // Independent coefficient/scale codec bytes for -1, 0, 0.1 and 1.
      std::string zero(24, '\0'); zero[1] = 1; zero[2] = 1;
      auto one = zero; one[4] = 1;
      auto negative = one; negative[0] = static_cast<char>(0x80);
      auto fraction = one; fraction[0] = 1;
      Require(key({negative}, {binding}) < key({zero}, {binding}) &&
                  key({zero}, {binding}) < key({fraction}, {binding}) &&
                  key({fraction}, {binding}) < key({one}, {binding}),
              "ODF-044 canonical decimal binary storage lost numeric ordering");
      Require(!api::bound_index_key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({"1.0"}),
                  {binding}, &output, &null_key, &diagnostic) && output == "unchanged",
              "ODF-044 bound decimal storage guessed a lexical carrier");
    } else if (type == dt::CanonicalTypeId::decimal_float) {
      std::string zero(16, '\0'), one(16, '\0');
      one[0]=1; one[14]=0x40; one[15]=0x30;
      auto negative=one; negative[15]=static_cast<char>(0xb0);
      auto equivalent=one; equivalent[0]=100; equivalent[14]=0x3c;
      auto negative_zero=zero; negative_zero[15]=static_cast<char>(0x80);
      Require(key({negative},{binding}) < key({zero},{binding}) &&
              key({zero},{binding}) < key({one},{binding}) &&
              key({one},{binding}) == key({equivalent},{binding}) &&
              key({zero},{binding}) == key({negative_zero},{binding}),
              "ODF-044 bound decimal128 numeric equivalence/order changed");
      for (const auto invalid : {0,1,2,3,4}) {
        auto changed=binding;
        if (invalid==0) changed.datatype.codec.reset();
        if (invalid==1) changed.datatype.codec->comparison_policy_uuid.bytes[0]^=1;
        if (invalid==2) ++changed.datatype.codec->numeric_context_generation;
        if (invalid==3) changed.datatype.codec->allow_special_values=false;
        if (invalid==4) changed.datatype.codec->comparison_profile="decimal128_ieee_total_order_v1";
        Require(!api::bound_index_key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({one}),
                    {changed}, &output, &null_key, &diagnostic) && output=="unchanged",
                "ODF-044 substituted decimal128 policy admitted or wrote output");
      }
      Require(!api::bound_index_key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({"1.0"}),
                  {binding}, &output, &null_key, &diagnostic) && output=="unchanged",
              "ODF-044 decimal128 storage guessed lexical input");
    } else if (type == dt::CanonicalTypeId::real128) {
      std::string positive(16, '\0'), negative(16, '\0'), zero(16, '\0');
      positive[14] = static_cast<char>(0xff); positive[15] = 0x3f;
      negative = positive; negative[15] = static_cast<char>(0xbf);
      Require(key({negative}, {binding}) < key({zero}, {binding}) &&
                  key({zero}, {binding}) < key({positive}, {binding}),
              "ODF-044 binary128 retained values were interpreted as decimal text");
      Require(!api::bound_index_key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({"1.0"}),
                  {binding}, &output, &null_key, &diagnostic) && output == "unchanged",
              "ODF-044 binary128 key accepted a noncanonical lexical carrier");
    } else {
      std::string nil(16, '\0'), user_v4(16, '\0'), maximum(16, static_cast<char>(0xff));
      user_v4[6] = 0x40; user_v4[8] = static_cast<char>(0x80);
      Require(key({nil}, {binding}) < key({user_v4}, {binding}) &&
                  key({user_v4}, {binding}) < key({maximum}, {binding}),
              "ODF-044 UUID user data was text encoded or constrained to system UUIDv7");
    }
  }
  // Publication must validate all bound key batches before a valid prefix can
  // reserve sequence numbers, queue jobs, or leave durable memberships.
  index.unique = false;
  api::MgaExactIndexEntryAppendBatch valid_batch;
  valid_batch.index = index;
  valid_batch.table_uuid = fixture.table_uuid;
  valid_batch.entry_kind = "insert";
  valid_batch.entries.push_back({api::EncodeStoredLogicalKey({"oslo"}), {},
      NewNativeUuid(platform::UuidKind::row, 44380),
      NewNativeUuid(platform::UuidKind::row, 44381)});
  std::vector<api::MgaExactIndexEntryAppendBatch> cohort(8, valid_batch);
  for (std::size_t ordinal = 0; ordinal < cohort.size(); ++ordinal) {
    cohort[ordinal].index.index_uuid = NewNativeUuid(platform::UuidKind::object, 44400 + ordinal);
    cohort[ordinal].entries.front().encoded_key = api::EncodeStoredLogicalKey(
        {ordinal % 2 ? api::CrudStoredValue("oslo") : api::CrudStoredValue("berlin")});
  }
  auto individual = cohort;
  for (auto& batch : individual)
    Require(api::bound_index_key::CanonicalizePublicationBatch(context, &batch, &diagnostic),
            "ODF-044 individually admitted publication binding failed");
  auto grouped = cohort;
  api::bound_index_key::PublicationBindingStatistics binding_statistics;
  Require(api::bound_index_key::CanonicalizePublicationBatches(
              context, &grouped, &diagnostic, &binding_statistics) &&
              binding_statistics.resource_lookups == 1 && binding_statistics.resource_reuses == 7,
          "ODF-044 publication cohort did not reuse its single admitted collation");
  for (std::size_t ordinal = 0; ordinal < cohort.size(); ++ordinal) {
    Require(grouped[ordinal].entries.front().encoded_key == individual[ordinal].entries.front().encoded_key &&
                grouped[ordinal].entries.front().payload_value == individual[ordinal].entries.front().payload_value &&
                grouped[ordinal].entries.front().row_uuid == cohort[ordinal].entries.front().row_uuid &&
                grouped[ordinal].entries.front().version_uuid == cohort[ordinal].entries.front().version_uuid,
            "ODF-044 grouped bindings changed key bytes or row/version identity");
  }
  const auto require_unmodified = [&](const auto& actual, const auto& expected) {
    Require(actual.size() == expected.size(), "ODF-044 refused cohort changed extent");
    for (std::size_t i = 0; i < expected.size(); ++i)
      Require(actual[i].entries.front().encoded_key == expected[i].entries.front().encoded_key &&
                  actual[i].entries.front().payload_value == expected[i].entries.front().payload_value,
              "ODF-044 refused cohort changed a valid prefix");
  };
  for (unsigned invalid = 0; invalid < 4; ++invalid) {
    auto foreign = context;
    if (invalid == 0) ++foreign.resource_epoch;
    if (invalid == 1) foreign.database_uuid = NewNativeUuid(platform::UuidKind::database, 44420);
    if (invalid == 2) foreign.transaction_uuid = NewNativeUuid(platform::UuidKind::transaction, 44421);
    if (invalid == 3) ++foreign.local_transaction_id;
    auto rejected = cohort;
    Require(!api::bound_index_key::CanonicalizePublicationBatches(
                foreign, &rejected, &diagnostic, &binding_statistics),
            "ODF-044 publication reused a binding across stale/foreign authority");
    require_unmodified(rejected, cohort);
  }
  auto malformed_cohort = cohort;
  malformed_cohort.back().entries.front().encoded_key = "not-a-logical-tuple";
  const auto malformed_before = malformed_cohort;
  Require(!api::bound_index_key::CanonicalizePublicationBatches(
              context, &malformed_cohort, &diagnostic, &binding_statistics) &&
              binding_statistics.resource_lookups == 1 && binding_statistics.resource_reuses == 7,
          "ODF-044 malformed later batch was accepted or lost bounded binding reuse");
  require_unmodified(malformed_cohort, malformed_before);
  auto malformed_single = cohort.front();
  malformed_single.entries.push_back(malformed_single.entries.front());
  malformed_single.entries.back().encoded_key = "not-a-logical-tuple";
  const auto single_before = malformed_single;
  Require(!api::bound_index_key::CanonicalizePublicationBatch(
              context, &malformed_single, &diagnostic) &&
              malformed_single.entries.front().encoded_key == single_before.entries.front().encoded_key &&
              malformed_single.entries.front().payload_value == single_before.entries.front().payload_value &&
              malformed_single.entries.back().encoded_key == single_before.entries.back().encoded_key,
          "ODF-044 refused single publication batch changed a valid entry prefix");
  auto fresh = cohort;
  Require(api::bound_index_key::CanonicalizePublicationBatches(
              context, &fresh, &diagnostic, &binding_statistics) &&
              binding_statistics.resource_lookups == 1 && binding_statistics.resource_reuses == 7,
          "ODF-044 next append borrowed bindings or failures from an earlier call");
  const auto before = api::LoadMgaRelationStoreState(context);
  Require(before.ok, "ODF-044 failure-atomic control snapshot unavailable");
  for (unsigned malformed = 0; malformed < 2; ++malformed) {
    auto invalid_batch = valid_batch;
    if (malformed == 0) invalid_batch.entries.front().encoded_key = "not-a-logical-tuple";
    else invalid_batch.table_uuid = NewNativeUuid(platform::UuidKind::object, 44382);
    api::MgaRelationHotAppendContext append(context);
    const auto refused = append.AppendExactIndexEntryBatches({valid_batch, invalid_batch});
    const auto expected_reason = malformed == 0
        ? "mga.index_store:sorted_index_logical_key_invalid"
        : "mga.index_store:ordered_index_table_identity_mismatch";
    Require(refused.error && refused.code == "SB_ENGINE_API_INVALID_REQUEST" &&
                refused.detail == expected_reason && append.counters().index_range_reservations == 0 &&
                append.counters().index_materialization_jobs_queued == 0,
            "ODF-044 failed bound-key batch published a valid prefix");
    RequireDiagnosticOk(append.FlushIndexEntries(), "ODF-044 refused batch retained queued work");
    const auto after = api::LoadMgaRelationStoreState(context);
    Require(after.ok && after.state.index_entries.size() == before.state.index_entries.size() &&
                after.state.max_index_event_sequence == before.state.max_index_event_sequence,
            "ODF-044 refused bound-key batch mutated durable memberships");
  }
  Commit(context);
  auto completed_transaction = cohort;
  Require(!api::bound_index_key::CanonicalizePublicationBatches(
              context, &completed_transaction, &diagnostic, &binding_statistics),
          "ODF-044 completed transaction borrowed an earlier active binding");
  require_unmodified(completed_transaction, cohort);
}

}  // namespace

int main() try {
  auto policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "odf044_statement_fixture";
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              policy, "odf044_statement_fixture").ok(),
          "ODF-044 memory policy configuration failed");
  CoreBuilderSortsAndRefusesUniqueDuplicates();
  DirectBulkUsesSortedExactIndexBuild();
  CreateIndexBackfillsWithSortedExactBuild();
  OrderedKeyAuthorityAndValueStates();
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
