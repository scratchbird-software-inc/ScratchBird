#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "database_lifecycle_test_memory.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/insert_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/update_api.hpp"
#include "dml/transactional_index_provider.hpp"
#include "database_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::u64 MillisSeed() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, MillisSeed() + salt);
  Require(generated.ok(), "PFAR-012 UUID generation failed");
  return generated.value;
}

api::EngineUuid NewIdentity(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

struct Fixture {
  Fixture() = default;
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  std::filesystem::path dir;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid;
  api::EngineUuid table_uuid;
  api::EngineUuid index_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
  api::EngineRequestContext context;

  ~Fixture() {
    session.reset();
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineTypedValue IntValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.canonical_type_name = "int64";
  typed.encoded_value = std::move(value);
  return typed;
}

api::CrudTableRecord Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "pfar012_table";
  table.columns.push_back({"id", "canonical=int64"});
  table.columns.push_back({"name", "canonical=text"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture) {
  api::CrudIndexRecord index;
  index.creator_tx = fixture.context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "name";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  return index;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  auto context = fixture.owner_context;
  context.request_id = std::move(request_id);
  return context;
}

api::EngineRequestContext BeginContext(const Fixture& fixture,
                                       std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "PFAR-012 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void InitializeFixture(Fixture& fixture, std::string name, platform::u64 salt) {
  const auto database_identity = NewUuid(platform::UuidKind::database, salt + 1);
  const auto directory = std::filesystem::temp_directory_path() /
                ("scratchbird_pfar012_" + name + "_" +
                 uuid::UuidToString(database_identity.value));
  Require(std::filesystem::create_directory(directory),
          "PFAR-012 isolated fixture directory already exists");
  fixture.dir = directory;
  fixture.database_path = fixture.dir / "pfar012.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = database_identity;
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = MillisSeed() + salt + 3;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "PFAR-012 database create failed");

  fixture.database_uuid = created.state.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.owner_context.default_root_uuid = created.state.filespace_uuid.value;
  fixture.owner_context.resource_epoch = created.state.resource_seed_catalog.resource_epoch;
  Require(fixture.owner_context.resource_epoch != 0,
          "PFAR-012 actual resource catalog epoch missing");
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewIdentity(platform::UuidKind::object, salt + 20);
  fixture.context = BeginContext(fixture,
                                 "pfar-012-" + name + "-metadata");

  const auto table = scratchbird::tests::PublishMgaTableFixture(
      fixture.context, Table(fixture), {"int64", "text"}, {Index(fixture)});
  Require(!table.error, "PFAR-012 table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(fixture.context, Index(fixture));
  Require(!index.error, "PFAR-012 index metadata append failed");
  fixture.owner_context.current_schema_uuid = fixture.context.current_schema_uuid;
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(fixture.context);
  const auto read = api::LoadMgaRelationStorageDescriptor(fixture.context, fixture.table_uuid);
  Require(read.ok && read.descriptor.columns.size() == 2,
          "PFAR-012 published relation descriptor unavailable");
  const auto& descriptor = read.descriptor;
  Require(descriptor.primary_filespace_uuid == created.state.filespace_uuid.value &&
              descriptor.relation_uuid == fixture.table_uuid &&
              descriptor.schema_uuid == fixture.context.current_schema_uuid &&
              descriptor.relation_generation != 0,
          "PFAR-012 published relation owner cohort mismatch");
  const auto manifest = scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "PFAR-012 datatype catalog unavailable");
  const std::array types{scratchbird::core::datatypes::CanonicalTypeId::int64,
                         scratchbird::core::datatypes::CanonicalTypeId::character};
  for (std::size_t i = 0; i < types.size(); ++i) {
    const auto expected = scratchbird::core::datatypes::LookupDatatypeCatalogRow(
        manifest.manifest, types[i]);
    Require(expected.ok() && expected.manifest.descriptor_rows.size() == 1,
            "PFAR-012 expected datatype descriptor missing");
    const auto& column = descriptor.columns[i];
    const auto& value = column.value_descriptor;
    const auto& datatype = expected.manifest.descriptor_rows.front();
    const auto codec = scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
        fixture.context.datatype_catalog_snapshot_uuid, fixture.context.datatype_catalog_generation,
        fixture.context.datatype_registry_generation, datatype.descriptor_uuid.value,
        datatype.descriptor_epoch);
    Require(codec.ok && uuid::IsEngineIdentityUuid(column.column_uuid) &&
                uuid::IsEngineIdentityUuid(value.descriptor_uuid) &&
                column.column_uuid != value.descriptor_uuid && column.column_generation != 0 &&
                value.datatype_descriptor_uuid == datatype.descriptor_uuid.value &&
                value.datatype_descriptor_generation == datatype.descriptor_epoch &&
                value.type_uuid == codec.row.type_uuid,
            "PFAR-012 published native column/datatype binding mismatch");
  }
}

api::EngineRowValue Row(std::string id, std::string name) {
  api::EngineRowValue row;
  row.fields.push_back({"id", IntValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  return row;
}

std::vector<std::string> RuntimeOptions(platform::u64 data_pages, platform::u64 index_pages) {
  return {"page_allocation.runtime=enabled",
          "page_allocation.preallocate_data_pages=" + std::to_string(data_pages),
          "page_allocation.preallocate_index_pages=" + std::to_string(index_pages)};
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(item.evidence_id, id)) {
      return true;
    }
  }
  return false;
}

bool HasNonEmptyEvidenceKind(const std::vector<api::EngineEvidenceReference>& evidence,
                             std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && std::visit([](const auto& value) {
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>, api::EngineUuid>)
            return !value.is_nil();
          else return !value.empty();
        }, item.evidence_id)) {
      return true;
    }
  }
  return false;
}

platform::u64 EvidenceU64(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return static_cast<platform::u64>(std::stoull(std::get<std::string>(item.evidence_id)));
    }
  }
  return 0;
}

std::size_t EvidenceCount(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  std::size_t count = 0;
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      ++count;
    }
  }
  return count;
}

std::size_t EvidenceIndex(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view id) {
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    if (evidence[index].evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(evidence[index].evidence_id, id)) {
      return index;
    }
  }
  return evidence.size();
}

api::MgaRelationStoreState LoadedState(const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "PFAR-012 MGA relation store load failed");
  return loaded.state;
}

void RequireIndexHistory(const Fixture& fixture, const api::MgaRelationStoreState& state,
                         std::size_t memberships, std::size_t retirements) {
  std::size_t actual_memberships = 0;
  std::size_t actual_retirements = 0;
  for (const auto& entry : state.index_entries) {
    Require(entry.index_uuid == fixture.index_uuid && entry.table_uuid == fixture.table_uuid &&
                entry.creator_tx == fixture.context.local_transaction_id &&
                uuid::IsEngineIdentityUuid(entry.row_uuid) &&
                uuid::IsEngineIdentityUuid(entry.version_uuid),
            "PFAR-012 index history has a foreign owner or malformed native identity");
    if (entry.entry_kind == "insert" || entry.entry_kind == "exact") ++actual_memberships;
    else if (entry.entry_kind == "retire") ++actual_retirements;
    else Fail("PFAR-012 unexpected index mutation kind");
  }
  // An indexed-key UPDATE retains both membership versions AND an explicit
  // old-key retirement. The old fixture counted only the two memberships.
  Require(actual_memberships == memberships && actual_retirements == retirements &&
              state.index_entries.size() == memberships + retirements,
          "PFAR-012 exact membership/retirement history count mismatch");
}

void RequireIndexLookup(const Fixture& fixture, const std::string& key, std::size_t count) {
  scratchbird::tests::FixtureEngineStatement statement(*fixture.session, fixture.context);
  const auto state = api::BuildCrudCompatibilityStateFromMga(LoadedState(statement.context));
  const auto index = std::find_if(state.indexes.begin(), state.indexes.end(),
      [&](const auto& candidate) { return candidate.index_uuid == fixture.index_uuid; });
  Require(index != state.indexes.end(), "PFAR-012 published index missing");
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = "name";
  predicate.bound_values.push_back(TextValue(key));
  api::MgaTransactionalIndexProvider provider(statement.context, nullptr);
  const auto validated = provider.ValidateAgainstRelation(state, *index);
  const auto lookup = provider.ResolveVisibleEntry(state, *index, predicate, 0);
  Require(validated.ok && lookup.ok && lookup.rows.size() == count,
          "PFAR-012 actual index lookup lost a row or retained a retired key");
  for (const auto& row : lookup.rows)
    Require(api::CrudFieldValue(row.values, "name") == key,
            "PFAR-012 index returned the wrong row value");
}

scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> InsertRequest(Fixture& fixture,
                                           std::string request_id,
                                           std::vector<std::string> options) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(
      *fixture.session, fixture.context);
  request.context.request_id = std::move(request_id);
  request.target_table.uuid = fixture.table_uuid;
  request.target_schema.uuid = fixture.context.current_schema_uuid;
  request.estimated_row_count = 1;
  request.input_rows.push_back(Row("1", "alpha"));
  request.option_envelopes = std::move(options);
  return request;
}

scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> MultiInsertRequest(Fixture& fixture,
                                                std::string request_id,
                                                std::vector<std::string> options,
                                                int row_count) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(
      *fixture.session, fixture.context);
  request.context.request_id = std::move(request_id);
  request.target_table.uuid = fixture.table_uuid;
  request.target_schema.uuid = fixture.context.current_schema_uuid;
  request.estimated_row_count = static_cast<api::EngineApiU64>(row_count);
  request.option_envelopes = std::move(options);
  request.input_rows.reserve(static_cast<std::size_t>(row_count));
  for (int row = 0; row < row_count; ++row) {
    request.input_rows.push_back(Row(std::to_string(row + 1),
                                     "batch_" + std::to_string(row + 1)));
  }
  return request;
}

scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> UpdateRequest(Fixture& fixture,
                                           std::string request_id,
                                           std::vector<std::string> options) {
  scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> request(
      *fixture.session, fixture.context);
  request.context.request_id = std::move(request_id);
  request.target_table.uuid = fixture.table_uuid;
  request.update_predicate.predicate_kind = "column_equals";
  request.update_predicate.canonical_predicate_envelope = "id";
  request.update_predicate.bound_values.push_back(IntValue("1"));
  request.assignments.push_back({"name", TextValue("bravo")});
  request.option_envelopes = std::move(options);
  return request;
}

scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> UpdateAllRequest(Fixture& fixture,
                                              std::string request_id,
                                              std::vector<std::string> options) {
  scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> request(
      *fixture.session, fixture.context);
  request.context.request_id = std::move(request_id);
  request.target_table.uuid = fixture.table_uuid;
  request.assignments.push_back({"name", TextValue("batch_updated")});
  request.option_envelopes = std::move(options);
  return request;
}

scratchbird::tests::FixtureEngineRequest<api::EngineDeleteRowsRequest> DeleteAllRequest(
    Fixture& fixture, std::string request_id) {
  scratchbird::tests::FixtureEngineRequest<api::EngineDeleteRowsRequest> request(
      *fixture.session, fixture.context);
  request.context.request_id = std::move(request_id);
  request.target_table.uuid = fixture.table_uuid;
  return request;
}

void TestInsertAndUpdateRuntimeAllocationSuccess() {
  Fixture fixture;
  InitializeFixture(fixture, "success", 1000);
  auto insert = InsertRequest(fixture,
                              "pfar-012-insert-success",
                              RuntimeOptions(4, 4));
  const auto inserted = api::EngineInsertRows(insert);
  Require(inserted.ok, "PFAR-012 insert with runtime allocation failed");
  Require(inserted.inserted_count == 1, "PFAR-012 insert count mismatch");
  Require(HasEvidence(inserted.evidence,
                      "row_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "PFAR-012 insert row allocation did not hit preallocated pool");
  Require(HasEvidence(inserted.evidence, "row_page_preallocated_inventory_consumed", "true"),
          "PFAR-012 insert row preallocated inventory was not consumed");
  Require(HasEvidence(inserted.evidence, "row_page_preallocation_inventory_authority",
                      "storage_page_allocation_lifecycle"),
          "PFAR-012 insert row inventory authority proof missing");
  Require(HasNonEmptyEvidenceKind(inserted.evidence, "index_page_allocation"),
          "PFAR-012 insert index allocation UUID was missing");
  Require(HasEvidence(inserted.evidence,
                      "index_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "PFAR-012 insert index allocation did not hit preallocated pool");
  Require(HasEvidence(inserted.evidence, "index_page_preallocated_inventory_consumed", "true"),
          "PFAR-012 insert index preallocated inventory was not consumed");
  Require(HasEvidence(inserted.evidence, "page_allocation_agent_finality_authority", "false"),
          "PFAR-012 insert incorrectly made page agent finality-authoritative");
  Require(EvidenceU64(inserted.evidence, "insert_hot_append_scoped_row_write_batches") == 1 &&
              EvidenceU64(inserted.evidence,
                          "insert_hot_append_scoped_row_write_tickets_issued") == 1 &&
              EvidenceU64(inserted.evidence,
                          "insert_hot_append_scoped_row_write_tickets_completed") == 1 &&
              EvidenceU64(inserted.evidence,
                          "insert_hot_append_scoped_row_write_worker_count") >= 1,
          "PFAR-012 insert row scoped write tickets did not complete");
  Require(EvidenceU64(inserted.evidence, "insert_hot_append_scoped_index_write_batches") == 1 &&
              EvidenceU64(inserted.evidence,
                          "insert_hot_append_scoped_index_write_tickets_issued") == 1 &&
              EvidenceU64(inserted.evidence,
                          "insert_hot_append_scoped_index_write_tickets_completed") == 1 &&
              EvidenceU64(inserted.evidence,
                          "insert_hot_append_scoped_index_write_worker_count") >= 1,
          "PFAR-012 insert index scoped write tickets did not complete");
  Require(EvidenceIndex(inserted.evidence, "index_page_allocation_source",
                        "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT") <
              EvidenceIndex(inserted.evidence, "mga_index_store", "row_insert"),
          "PFAR-012 index allocation evidence did not precede index append evidence");

  auto state = LoadedState(fixture.context);
  Require(state.row_versions.size() == 1, "PFAR-012 inserted row version missing");
  Require(state.index_entries.size() == 1, "PFAR-012 inserted index entry missing");

  auto update = UpdateRequest(fixture,
                              "pfar-012-update-success",
                              RuntimeOptions(4, 4));
  const auto updated = api::EngineUpdateRows(update);
  Require(updated.ok, "PFAR-012 update with runtime allocation failed");
  Require(updated.updated_count == 1, "PFAR-012 update count mismatch");
  Require(HasEvidence(updated.evidence,
                      "row_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "PFAR-012 update row allocation missing");
  Require(HasEvidence(updated.evidence, "row_page_preallocated_inventory_consumed", "true"),
          "PFAR-012 update row preallocated inventory was not consumed");
  Require(HasEvidence(updated.evidence,
                      "index_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "PFAR-012 update index allocation missing");
  Require(HasEvidence(updated.evidence, "index_page_preallocated_inventory_consumed", "true"),
          "PFAR-012 update index preallocated inventory was not consumed");
  state = LoadedState(fixture.context);
  Require(state.row_versions.size() == 2, "PFAR-012 updated row version missing");
  RequireIndexHistory(fixture, state, 2, 1);
  RequireIndexLookup(fixture, "alpha", 0);
  RequireIndexLookup(fixture, "bravo", 1);
}

void TestInsertAllocationRefusalLeavesNoMutation() {
  Fixture fixture;
  InitializeFixture(fixture, "insert_refusal", 2000);
  auto insert = InsertRequest(fixture,
                              "pfar-012-insert-refusal",
                              RuntimeOptions(1, 0));
  const auto refused = api::EngineInsertRows(insert);
  Require(!refused.ok, "PFAR-012 allocation-refused insert succeeded");
  Require(!refused.diagnostics.empty() &&
              refused.diagnostics.front().code ==
                  "SB-STORAGE-PAGE-ALLOCATION-INSUFFICIENT-FREE-SPACE",
          "PFAR-012 insert refusal diagnostic mismatch");
  Require(HasEvidence(refused.evidence,
                      "page_allocation_diagnostic",
                      "SB-STORAGE-PAGE-ALLOCATION-INSUFFICIENT-FREE-SPACE"),
          "PFAR-012 insert refusal allocation evidence missing");
  Require(HasEvidence(refused.evidence, "page_allocation_runtime_phase", "insert.index"),
          "PFAR-012 insert refusal did not exercise index allocation");

  const auto state = LoadedState(fixture.context);
  Require(state.row_versions.empty(), "PFAR-012 refused insert wrote row versions");
  Require(state.index_entries.empty(), "PFAR-012 refused insert wrote index entries");
}

void TestUpdateAllocationRefusalLeavesNoMutation() {
  Fixture fixture;
  InitializeFixture(fixture, "update_refusal", 3000);
  auto seed = InsertRequest(fixture, "pfar-012-update-seed", {});
  const auto seeded = api::EngineInsertRows(seed);
  Require(seeded.ok, "PFAR-012 seed insert failed");
  auto before = LoadedState(fixture.context);
  Require(before.row_versions.size() == 1, "PFAR-012 seed row version missing");
  Require(before.index_entries.size() == 1, "PFAR-012 seed index entry missing");

  auto update = UpdateRequest(fixture,
                              "pfar-012-update-refusal",
                              RuntimeOptions(1, 0));
  const auto refused = api::EngineUpdateRows(update);
  Require(!refused.ok, "PFAR-012 allocation-refused update succeeded");
  Require(!refused.diagnostics.empty() &&
              refused.diagnostics.front().code ==
                  "SB-STORAGE-PAGE-ALLOCATION-INSUFFICIENT-FREE-SPACE",
          "PFAR-012 update refusal diagnostic mismatch");
  Require(HasEvidence(refused.evidence,
                      "page_allocation_diagnostic",
                      "SB-STORAGE-PAGE-ALLOCATION-INSUFFICIENT-FREE-SPACE"),
          "PFAR-012 update refusal allocation evidence missing");
  Require(HasEvidence(refused.evidence, "page_allocation_runtime_phase", "update.index"),
          "PFAR-012 update refusal did not exercise index allocation");

  const auto after = LoadedState(fixture.context);
  Require(after.row_versions.size() == before.row_versions.size(),
          "PFAR-012 refused update wrote row versions");
  Require(after.index_entries.size() == before.index_entries.size(),
          "PFAR-012 refused update wrote index entries");
}

void TestBatchDmlUsesStatementSizedRuntimeReservations() {
  Fixture fixture;
  InitializeFixture(fixture, "batch_reservations", 4000);
  auto insert = MultiInsertRequest(fixture,
                                   "pfar-012-batch-insert",
                                   RuntimeOptions(8, 8),
                                   4);
  const auto inserted = api::EngineInsertRows(insert);
  Require(inserted.ok, "PFAR-012 batch insert failed");
  Require(inserted.inserted_count == 4, "PFAR-012 batch insert count mismatch");
  Require(EvidenceCount(inserted.evidence, "row_page_allocation_source") == 1,
          "PFAR-012 batch insert did not use one row allocation reservation");
  Require(EvidenceCount(inserted.evidence, "index_page_allocation_source") == 1,
          "PFAR-012 batch insert did not use one index allocation reservation");

  auto state = LoadedState(fixture.context);
  Require(state.row_versions.size() == 4, "PFAR-012 batch insert row version count mismatch");
  Require(state.index_entries.size() == 4, "PFAR-012 batch insert index entry count mismatch");

  auto update = UpdateAllRequest(fixture,
                                 "pfar-012-batch-update",
                                 RuntimeOptions(8, 8));
  const auto updated = api::EngineUpdateRows(update);
  Require(updated.ok, "PFAR-012 batch update failed");
  Require(updated.updated_count == 4, "PFAR-012 batch update count mismatch");
  Require(EvidenceCount(updated.evidence, "row_page_allocation_source") == 1,
          "PFAR-012 batch update did not use one row allocation reservation");
  Require(EvidenceCount(updated.evidence, "index_page_allocation_source") == 1,
          "PFAR-012 batch update did not use one index allocation reservation");

  state = LoadedState(fixture.context);
  Require(state.row_versions.size() == 8, "PFAR-012 batch update row version count mismatch");
  RequireIndexHistory(fixture, state, 8, 4);
  for (int row = 1; row <= 4; ++row)
    RequireIndexLookup(fixture, "batch_" + std::to_string(row), 0);
  RequireIndexLookup(fixture, "batch_updated", 4);

  const auto deleted = api::EngineDeleteRows(DeleteAllRequest(fixture, "pfar-012-batch-delete"));
  Require(deleted.ok, "PFAR-012 batch delete failed");
  Require(deleted.deleted_count == 4, "PFAR-012 batch delete count mismatch");
  state = LoadedState(fixture.context);
  Require(state.row_versions.size() == 12, "PFAR-012 batch delete tombstone count mismatch");
  RequireIndexLookup(fixture, "batch_updated", 0);
}

}  // namespace

int main() try {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "dml_page_allocation_runtime_gate");
  TestInsertAndUpdateRuntimeAllocationSuccess();
  TestInsertAllocationRefusalLeavesNoMutation();
  TestUpdateAllocationRefusalLeavesNoMutation();
  TestBatchDmlUsesStatementSizedRuntimeReservations();
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
