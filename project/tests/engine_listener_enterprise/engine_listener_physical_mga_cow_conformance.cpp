// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "datatype_binary.hpp"
#include "memory.hpp"
#include "physical_mga_cow_store.hpp"
#include "runtime_platform.hpp"
#include "startup_state.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace database = scratchbird::storage::database;
namespace datatypes = scratchbird::core::datatypes;
namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::UuidKind;
using platform::u64;

constexpr u64 kBaseMillis = 1770000000000ull;
constexpr platform::u32 kPageSize = 8192;
constexpr u64 kRowPageNumber = database::kCatalogOverflowFirstPageNumber + 512;

bool Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

void PrintDiagnostic(const platform::DiagnosticRecord& diagnostic) {
  if (!diagnostic.diagnostic_code.empty()) {
    std::cerr << diagnostic.diagnostic_code << ':' << diagnostic.message_key << '\n';
  }
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  if (!generated.ok()) {
    std::cerr << "uuid generation failed\n";
    std::exit(EXIT_FAILURE);
  }
  return generated.value;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "engine_listener_physical_mga_cow_conformance";
  policy.hard_limit_bytes = 8 * 1024 * 1024;
  policy.soft_limit_bytes = 8 * 1024 * 1024;
  policy.per_context_limit_bytes = 8 * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 8 * 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(),
          "engine_listener_physical_mga_cow_conformance");
  if (!configured.ok()) {
    PrintDiagnostic(configured.diagnostic);
  }
  return Require(configured.ok(),
                 "ELER-020 memory fixture should configure") &&
         Require(configured.fixture_mode,
                 "ELER-020 memory fixture should run in fixture mode");
}

datatypes::DatatypeBinaryValue TextValue(std::string_view text) {
  datatypes::DatatypeBinaryValue value;
  value.type_id = datatypes::CanonicalTypeId::character;
  value.payload.assign(text.begin(), text.end());
  return value;
}

std::vector<scratchbird::storage::page::RowDataCell> Cells(std::string_view text) {
  return {{1, TextValue(text)}};
}

std::filesystem::path TempRoot() {
  std::string scope = std::filesystem::current_path().filename().string();
  if (scope.empty()) {
    scope = "default";
  }
#ifdef _WIN32
  const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long long>(::getpid());
#endif
  auto root = std::filesystem::temp_directory_path() /
              ("scratchbird_engine_listener_physical_mga_cow_" + scope +
               "_" + std::to_string(pid));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  return root;
}

void RemoveRoot(const std::filesystem::path& root) {
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid relation_uuid;
  TypedUuid row_uuid;
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.root = TempRoot();
  fixture.database_path = fixture.root / "eler020_physical_mga_cow.sbdb";
  fixture.database_uuid = MakeUuid(UuidKind::database, 10);
  fixture.filespace_uuid = MakeUuid(UuidKind::filespace, 11);
  fixture.relation_uuid = MakeUuid(UuidKind::object, 12);
  fixture.row_uuid = MakeUuid(UuidKind::row, 13);

  database::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis + 100;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = database::CreateDatabaseFile(create);
  if (!created.ok()) {
    PrintDiagnostic(created.diagnostic);
  }
  Require(created.ok(), "ELER-020 fixture database should create");
  return fixture;
}

database::PhysicalMgaCowMutationRequest Mutation(const Fixture& fixture,
                                                 database::PhysicalMgaCowMutationKind kind,
                                                 u64 txn_offset,
                                                 u64 begin_millis,
                                                 std::string_view payload) {
  database::PhysicalMgaCowMutationRequest request;
  request.database_path = fixture.database_path.string();
  request.relation_uuid = fixture.relation_uuid;
  request.row_uuid = fixture.row_uuid;
  request.transaction_uuid = MakeUuid(UuidKind::transaction, txn_offset);
  request.kind = kind;
  request.page_number = kRowPageNumber;
  request.begin_unix_epoch_millis = begin_millis;
  request.stable_slot_id = 77;
  if (kind != database::PhysicalMgaCowMutationKind::delete_row) {
    request.cells = Cells(payload);
  }
  return request;
}

database::PhysicalMgaCowReadRequest LatestRead(const Fixture& fixture) {
  database::PhysicalMgaCowReadRequest request;
  request.database_path = fixture.database_path.string();
  request.relation_uuid = fixture.relation_uuid;
  request.page_number = kRowPageNumber;
  request.use_latest_committed_snapshot = true;
  return request;
}

database::PhysicalMgaCowReadRequest SnapshotRead(const Fixture& fixture, u64 visible_through) {
  auto request = LatestRead(fixture);
  request.use_latest_committed_snapshot = false;
  request.visibility_snapshot.visible_through_local_transaction_id = visible_through;
  request.visibility_snapshot.visible_through_local_transaction_id_is_boundary = true;
  request.visibility_snapshot.allow_reader_own_uncommitted = false;
  return request;
}

bool ContainsEvidence(const std::vector<std::string>& evidence,
                      std::string_view expected) {
  for (const std::string& value : evidence) {
    if (value == expected) {
      return true;
    }
  }
  return false;
}

const scratchbird::storage::page::RowDataRecord* FindVersion(
    const database::PhysicalMgaCowReadResult& result,
    platform::u32 row_version) {
  for (const auto& row : result.row_page.rows) {
    if (row.row_version == row_version) {
      return &row;
    }
  }
  return nullptr;
}

bool EndToEndInsertUpdateDeleteRollbackReopen() {
  bool ok = true;
  ok = ConfigureMemoryFixture() && ok;
  if (!ok) {
    return false;
  }
  const Fixture fixture = MakeFixture();
  if (!std::filesystem::exists(fixture.database_path)) {
    RemoveRoot(fixture.root);
    return Require(false, "ELER-020 fixture database file should exist");
  }

  const auto insert = database::WritePhysicalMgaCowUnpublishedMutation(
      Mutation(fixture,
               database::PhysicalMgaCowMutationKind::insert,
               1000,
               kBaseMillis + 1000,
               "inserted"));
  if (!insert.ok()) {
    PrintDiagnostic(insert.diagnostic);
  }
  ok = Require(insert.ok(), "unpublished insert should write row page") && ok;
  ok = Require(insert.row_version.row_version == 1,
               "insert should allocate version 1") && ok;
  ok = Require(insert.mutation.phase ==
                   txn::CopyOnWriteMutationPhase::payload_written_unpublished,
               "insert should stop before inventory publish") && ok;
  ok = Require(ContainsEvidence(insert.evidence,
                                "physical_mga_cow.visibility_published_by_inventory=false"),
               "unpublished insert should not claim visibility") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }

  const auto before_commit = database::ReadPhysicalMgaCowRows(LatestRead(fixture));
  if (!before_commit.ok()) {
    PrintDiagnostic(before_commit.diagnostic);
  }
  ok = Require(before_commit.ok(), "unpublished insert should reopen for read") && ok;
  ok = Require(before_commit.visible_rows.empty(),
               "unpublished insert must not be visible after reopen") && ok;
  ok = Require(before_commit.wait_for_transaction_count == 1,
               "unpublished insert should wait on inventory finality") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }

  const auto insert_commit = database::FinalizePhysicalMgaCowTransaction(
      {fixture.database_path.string(),
       insert.transaction_entry.identity.local_id,
       database::PhysicalMgaCowFinalizeDecision::commit,
       kBaseMillis + 1100});
  if (!insert_commit.ok()) {
    PrintDiagnostic(insert_commit.diagnostic);
  }
  ok = Require(insert_commit.ok(), "insert commit should persist inventory") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }
  const u64 insert_txn = insert_commit.transaction_entry.identity.local_id.value;

  const auto after_insert = database::ReadPhysicalMgaCowRows(LatestRead(fixture));
  ok = Require(after_insert.ok(), "committed insert should reopen for read") && ok;
  ok = Require(after_insert.visible_rows.size() == 1,
               "committed insert should be visible") && ok;
  ok = Require(after_insert.visible_rows.front().row_version == 1,
               "committed insert visible version should be 1") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }

  const auto update = database::WritePhysicalMgaCowUnpublishedMutation(
      Mutation(fixture,
               database::PhysicalMgaCowMutationKind::update,
               2000,
               kBaseMillis + 2000,
               "updated"));
  if (!update.ok()) {
    PrintDiagnostic(update.diagnostic);
  }
  ok = Require(update.ok(), "unpublished update should write new version") && ok;
  ok = Require(update.row_version.previous_row_version == 1,
               "update should link to previous version") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }

  const auto update_unpublished = database::ReadPhysicalMgaCowRows(LatestRead(fixture));
  ok = Require(update_unpublished.ok(), "unpublished update should reopen") && ok;
  ok = Require(update_unpublished.visible_rows.size() == 1 &&
                   update_unpublished.visible_rows.front().row_version == 1,
               "unpublished update should leave old version visible") && ok;
  ok = Require(update_unpublished.wait_for_transaction_count == 1,
               "unpublished update should expose wait evidence") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }

  const auto update_commit = database::FinalizePhysicalMgaCowTransaction(
      {fixture.database_path.string(),
       update.transaction_entry.identity.local_id,
       database::PhysicalMgaCowFinalizeDecision::commit,
       kBaseMillis + 2100});
  ok = Require(update_commit.ok(), "update commit should persist inventory") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }
  const u64 update_txn = update_commit.transaction_entry.identity.local_id.value;

  const auto after_update = database::ReadPhysicalMgaCowRows(LatestRead(fixture));
  ok = Require(after_update.ok(), "committed update should reopen") && ok;
  ok = Require(after_update.visible_rows.size() == 1 &&
                   after_update.visible_rows.front().row_version == 2,
               "latest snapshot should see updated version") && ok;
  const auto old_snapshot = database::ReadPhysicalMgaCowRows(
      SnapshotRead(fixture, insert_txn));
  ok = Require(old_snapshot.ok(), "old snapshot should reopen") && ok;
  ok = Require(old_snapshot.visible_rows.size() == 1 &&
                   old_snapshot.visible_rows.front().row_version == 1,
               "old snapshot should retain inserted version") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }

  const auto rollback_update = database::WritePhysicalMgaCowUnpublishedMutation(
      Mutation(fixture,
               database::PhysicalMgaCowMutationKind::update,
               3000,
               kBaseMillis + 3000,
               "rolled back update"));
  ok = Require(rollback_update.ok(), "rollback candidate update should write") && ok;
  const auto rollback_done = database::FinalizePhysicalMgaCowTransaction(
      {fixture.database_path.string(),
       rollback_update.transaction_entry.identity.local_id,
       database::PhysicalMgaCowFinalizeDecision::rollback,
       kBaseMillis + 3100});
  ok = Require(rollback_done.ok(), "rollback should persist inventory") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }
  const auto after_rollback = database::ReadPhysicalMgaCowRows(LatestRead(fixture));
  ok = Require(after_rollback.ok(), "rolled back update should reopen") && ok;
  ok = Require(after_rollback.visible_rows.size() == 1 &&
                   after_rollback.visible_rows.front().row_version == 2,
               "rolled back update must not replace committed version") && ok;
  ok = Require(after_rollback.rolled_back_version_count >= 1,
               "rolled back version should be counted and skipped") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }

  const auto delete_row = database::WritePhysicalMgaCowUnpublishedMutation(
      Mutation(fixture,
               database::PhysicalMgaCowMutationKind::delete_row,
               4000,
               kBaseMillis + 4000,
               {}));
  if (!delete_row.ok()) {
    PrintDiagnostic(delete_row.diagnostic);
  }
  ok = Require(delete_row.ok(), "delete marker should write") && ok;
  ok = Require(delete_row.row_version.deleted,
               "delete mutation should create delete marker") && ok;
  ok = Require(delete_row.row_version.previous_row_version == 2,
               "delete marker should link to latest committed version") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }
  const auto delete_commit = database::FinalizePhysicalMgaCowTransaction(
      {fixture.database_path.string(),
       delete_row.transaction_entry.identity.local_id,
       database::PhysicalMgaCowFinalizeDecision::commit,
       kBaseMillis + 4100});
  ok = Require(delete_commit.ok(), "delete commit should persist inventory") && ok;
  if (!ok) {
    RemoveRoot(fixture.root);
    return false;
  }

  const auto after_delete = database::ReadPhysicalMgaCowRows(LatestRead(fixture));
  ok = Require(after_delete.ok(), "delete should reopen") && ok;
  ok = Require(after_delete.visible_rows.empty(),
               "latest snapshot should see row deleted") && ok;
  ok = Require(after_delete.visible_delete_marker_count == 1,
               "delete marker should suppress older visible row") && ok;
  const auto before_delete_snapshot = database::ReadPhysicalMgaCowRows(
      SnapshotRead(fixture, update_txn));
  ok = Require(before_delete_snapshot.ok(),
               "pre-delete snapshot should reopen") && ok;
  ok = Require(before_delete_snapshot.visible_rows.size() == 1 &&
                   before_delete_snapshot.visible_rows.front().row_version == 2,
               "pre-delete snapshot should retain updated version") && ok;

  const auto lineage = database::ReadPhysicalMgaCowRows(SnapshotRead(fixture, update_txn));
  const auto* version1 = FindVersion(lineage, 1);
  const auto* version2 = FindVersion(lineage, 2);
  const auto* version3 = FindVersion(lineage, 3);
  const auto* version4 = FindVersion(lineage, 4);
  ok = Require(version1 != nullptr && version2 != nullptr &&
                   version3 != nullptr && version4 != nullptr,
               "all physical row versions should reopen from the row page") && ok;
  if (version1 != nullptr && version2 != nullptr &&
      version3 != nullptr && version4 != nullptr) {
    ok = Require(version1->next_row_version == 2,
                 "version 1 should link forward to version 2") && ok;
    ok = Require(version2->previous_row_version == 1,
                 "version 2 should link backward to version 1") && ok;
    ok = Require(version3->previous_row_version == 2,
                 "rolled back version should keep backward lineage") && ok;
    ok = Require(version4->previous_row_version == 2,
                 "delete marker should link backward to committed base") && ok;
  }
  ok = Require(ContainsEvidence(after_delete.evidence,
                                "physical_mga_cow.row_page_finality_authority=false"),
               "row page must not become finality authority") && ok;
  ok = Require(ContainsEvidence(after_delete.evidence,
                                "physical_mga_cow.read_visibility_authority=durable_transaction_inventory"),
               "read proof should name durable inventory authority") && ok;

  RemoveRoot(fixture.root);
  return ok;
}

}  // namespace

int main() {
  const bool ok = EndToEndInsertUpdateDeleteRollbackReopen();
  if (!ok) {
    return EXIT_FAILURE;
  }
  std::cout << "engine_listener_physical_mga_cow_conformance=passed\n";
  return EXIT_SUCCESS;
}
