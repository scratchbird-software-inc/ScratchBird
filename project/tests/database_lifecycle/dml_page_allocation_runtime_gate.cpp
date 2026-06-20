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
#include "database_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
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

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string index_uuid;
  api::EngineRequestContext context;

  ~Fixture() {
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.canonical_type_name = "character";
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
  table.columns.push_back({"name", "canonical=character"});
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

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      const std::string& database_uuid,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_path = database_path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, 100);
  context.session_uuid.canonical = "session-pfar-012";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext BeginContext(const std::filesystem::path& database_path,
                                       const std::string& database_uuid,
                                       std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(database_path, database_uuid, std::move(request_id));
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

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_pfar012_" + name + "_" + std::to_string(MillisSeed() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "pfar012.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = MillisSeed() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "PFAR-012 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 20);
  fixture.context = BeginContext(fixture.database_path,
                                 fixture.database_uuid,
                                 "pfar-012-" + name + "-metadata");

  const auto table = api::AppendMgaTableMetadata(fixture.context, Table(fixture));
  Require(!table.error, "PFAR-012 table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(fixture.context, Index(fixture));
  Require(!index.error, "PFAR-012 index metadata append failed");
  return fixture;
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
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool HasNonEmptyEvidenceKind(const std::vector<api::EngineEvidenceReference>& evidence,
                             std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && !item.evidence_id.empty()) {
      return true;
    }
  }
  return false;
}

platform::u64 EvidenceU64(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return static_cast<platform::u64>(std::stoull(item.evidence_id));
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
    if (evidence[index].evidence_kind == kind && evidence[index].evidence_id == id) {
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

api::EngineInsertRowsRequest InsertRequest(Fixture& fixture,
                                           std::string request_id,
                                           std::vector<std::string> options) {
  api::EngineInsertRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_schema.uuid.canonical = NewUuidText(platform::UuidKind::schema, 300);
  request.estimated_row_count = 1;
  request.input_rows.push_back(Row("1", "alpha"));
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineInsertRowsRequest MultiInsertRequest(Fixture& fixture,
                                                std::string request_id,
                                                std::vector<std::string> options,
                                                int row_count) {
  api::EngineInsertRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_schema.uuid.canonical = NewUuidText(platform::UuidKind::schema, 350);
  request.estimated_row_count = static_cast<api::EngineApiU64>(row_count);
  request.option_envelopes = std::move(options);
  request.input_rows.reserve(static_cast<std::size_t>(row_count));
  for (int row = 0; row < row_count; ++row) {
    request.input_rows.push_back(Row(std::to_string(row + 1),
                                     "batch_" + std::to_string(row + 1)));
  }
  return request;
}

api::EngineUpdateRowsRequest UpdateRequest(Fixture& fixture,
                                           std::string request_id,
                                           std::vector<std::string> options) {
  api::EngineUpdateRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.update_predicate.predicate_kind = "column_equals";
  request.update_predicate.canonical_predicate_envelope = "id";
  request.update_predicate.bound_values.push_back(IntValue("1"));
  request.assignments.push_back({"name", TextValue("bravo")});
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineUpdateRowsRequest UpdateAllRequest(Fixture& fixture,
                                              std::string request_id,
                                              std::vector<std::string> options) {
  api::EngineUpdateRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.assignments.push_back({"name", TextValue("batch_updated")});
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineDeleteRowsRequest DeleteAllRequest(Fixture& fixture, std::string request_id) {
  api::EngineDeleteRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.target_table.uuid.canonical = fixture.table_uuid;
  return request;
}

void TestInsertAndUpdateRuntimeAllocationSuccess() {
  auto fixture = MakeFixture("success", 1000);
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
  Require(state.index_entries.size() == 2, "PFAR-012 updated index entry missing");
}

void TestInsertAllocationRefusalLeavesNoMutation() {
  auto fixture = MakeFixture("insert_refusal", 2000);
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
  auto fixture = MakeFixture("update_refusal", 3000);
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
  auto fixture = MakeFixture("batch_reservations", 4000);
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
  Require(state.index_entries.size() == 8, "PFAR-012 batch update index entry count mismatch");

  const auto deleted = api::EngineDeleteRows(DeleteAllRequest(fixture, "pfar-012-batch-delete"));
  Require(deleted.ok, "PFAR-012 batch delete failed");
  Require(deleted.deleted_count == 4, "PFAR-012 batch delete count mismatch");
  state = LoadedState(fixture.context);
  Require(state.row_versions.size() == 12, "PFAR-012 batch delete tombstone count mismatch");
}

}  // namespace

int main() {
  TestInsertAndUpdateRuntimeAllocationSuccess();
  TestInsertAllocationRefusalLeavesNoMutation();
  TestUpdateAllocationRefusalLeavesNoMutation();
  TestBatchDmlUsesStatementSizedRuntimeReservations();
  return EXIT_SUCCESS;
}
