#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/published_ddl_table_fixture.hpp"
#include "database_lifecycle_test_memory.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "physical_mga_cow_store.hpp"
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
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 TimeSeed() {
  return static_cast<platform::u64>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + salt);
  Require(generated.ok(), "IPAR-P3-01 UUID generation failed");
  return generated.value;
}

api::EngineUuid NewIdentity(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid;
  api::EngineUuid schema_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
  api::EngineUuid table_uuid;
  api::EngineUuid index_uuid;
  platform::u64 salt = 0;
  platform::u64 page_size = 0;

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
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-id-" + std::to_string(index + 1),
                       prefix + "-note-" + std::to_string(index + 1)));
  }
  return rows;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  auto context = fixture.owner_context;
  context.request_id = std::move(request_id);
  context.current_schema_uuid = fixture.schema_uuid;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "IPAR-P3-01 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request),
            "IPAR-P3-01 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "IPAR-P3-01 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_physical_row_page_append";
  table.columns.push_back({"id", "canonical=character;primary_key=true;not_null=true"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_p301_" + name + "_" +
                 std::to_string(TimeSeed() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_physical_row_page_append.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = 1900000000000ull + salt + 3;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR-P3-01 database create failed");
  fixture.page_size = created.state.header.page_size;

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, salt + 10);

  auto metadata = Begin(fixture, "ipar-p301-metadata");
  const auto table = scratchbird::tests::PublishDdlTableFixture(
      metadata, Table(fixture, metadata), {"character", "character"});
  Require(table.table_uuid == fixture.table_uuid && table.columns.size() == 2,
          "IPAR-P3-01 native table publication changed identity or shape");
  api::CatalogColumnMetadata primary;
  Require(api::DecodeCatalogColumnMetadata(table.columns.front().second, &primary) &&
              primary.identities.contains("candidate_key_constraint_uuid") &&
              primary.identities.contains("support_uuid"),
          "IPAR-P3-01 native primary-key constraint and support identities missing");
  fixture.index_uuid = primary.identities.at("support_uuid");
  const auto published = api::LoadMgaRelationStoreState(metadata);
  Require(published.ok, "IPAR-P3-01 published primary-key index unreadable");
  bool exact_support = false;
  for (const auto& index : published.state.relation_metadata.indexes) {
    exact_support = exact_support ||
        (index.index_uuid == fixture.index_uuid &&
         index.table_uuid == fixture.table_uuid && index.unique &&
         index.family == api::kCrudIndexFamilyBtree && index.column_name == "id");
  }
  Require(exact_support, "IPAR-P3-01 primary-key backing index is not published");
  fixture.schema_uuid = metadata.current_schema_uuid;
  Commit(metadata);
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "native-ipar-session"));
  return fixture;
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

std::size_t EvidenceCount(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view id) {
  std::size_t count = 0;
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(item.evidence_id, id)) {
      ++count;
    }
  }
  return count;
}

std::string EvidenceId(const std::vector<api::EngineEvidenceReference>& evidence,
                       std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return scratchbird::tests::EvidenceTextFields(item.evidence_id);
    }
  }
  return {};
}

scratchbird::tests::FixtureEngineRequest<api::EngineExecuteImportRowsRequest> ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    platform::u64 page_number,
    platform::u64 rows_per_page,
    platform::u64 copy_batch_rows) {
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
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes.push_back("copy_append_batching=enabled");
  request.option_envelopes.push_back("copy_append_batch_rows=" +
                                     std::to_string(copy_batch_rows));
  request.option_envelopes.push_back("physical_mga_cow=required");
  request.option_envelopes.push_back("physical_mga_cow.page_number=" +
                                     std::to_string(page_number));
  request.option_envelopes.push_back("physical_mga_cow.rows_per_page=" +
                                     std::to_string(rows_per_page));
  return request;
}

api::EngineApiU64 SelectCount(const Fixture& fixture) {
  auto context = Begin(fixture, "ipar-p301-select");
  scratchbird::tests::FixtureEngineRequest<api::EngineSelectRowsRequest> request(
      *fixture.session, context);
  request.source_object.uuid = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "IPAR-P3-01 select failed");
  Rollback(context);
  return selected.visible_count;
}

platform::TypedUuid RelationUuid(const Fixture& fixture) {
  Require(uuid::IsEngineIdentityUuid(fixture.table_uuid),
          "IPAR-P3-01 invalid table identity");
  return {platform::UuidKind::object, fixture.table_uuid};
}

db::PhysicalMgaCowReadResult ReadPage(const Fixture& fixture,
                                      platform::u64 page_number) {
  db::PhysicalMgaCowReadRequest request;
  request.database_path = fixture.database_path.string();
  request.relation_uuid = RelationUuid(fixture);
  request.page_number = page_number;
  request.use_latest_committed_snapshot = true;
  const auto read = db::ReadPhysicalMgaCowRows(request);
  if (!read.ok()) {
    std::cerr << read.diagnostic.diagnostic_code << ':'
              << read.diagnostic.message_key << '\n';
  }
  Require(read.ok(), "IPAR-P3-01 physical row page read failed");
  return read;
}

platform::u64 NextPhysicalAppendPage(const Fixture& fixture) {
  const auto size = std::filesystem::file_size(fixture.database_path);
  Require(fixture.page_size != 0 && size % fixture.page_size == 0,
          "IPAR-P3-01 database physical extent is not page-aligned");
  // The credentialed catalog occupies real pages. The COW writer initializes
  // fresh pages at the current end of this fixture-owned file; fixed page
  // numbers must not overwrite the native catalog or inventory.
  return size / fixture.page_size;
}

void VerifyCommittedPhysicalRows() {
  auto fixture = MakeFixture("commit", 301000 + TimeSeed() % 100000);
  auto context = Begin(fixture, "ipar-p301-commit-import");
  const auto first_page = NextPhysicalAppendPage(fixture);
  const auto imported = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      Rows("commit", 5),
      first_page,
      2,
      2));
  RequireOk(imported, "IPAR-P3-01 committed import failed");
  Require(imported.inserted_rows == 5 && imported.accepted_rows == 5,
          "IPAR-P3-01 committed import row count mismatch");
  Require(HasEvidence(imported.evidence,
                      "import_execution",
                      "direct_physical"),
          "IPAR-P3-01 import did not use direct physical lane");
  Require(HasEvidence(imported.evidence,
                      "direct_physical_bulk_row_page_writer",
                      "physical_mga_cow"),
          "IPAR-P3-01 physical row-page writer evidence missing");
  Require(EvidenceId(imported.evidence,
                     "direct_physical_bulk_row_page_written_rows") == "5",
          "IPAR-P3-01 physical row-page write count mismatch");
  Require(HasEvidence(imported.evidence,
                      "direct_physical_bulk_row_page_evidence",
                      "physical_mga_cow.existing_active_transaction_verified=true"),
          "IPAR-P3-01 existing transaction evidence missing");
  Commit(context);

  Require(SelectCount(fixture) == 5,
          "IPAR-P3-01 committed engine select count mismatch");
  const auto first = ReadPage(fixture, first_page);
  const auto second = ReadPage(fixture, first_page + 1);
  const auto third = ReadPage(fixture, first_page + 2);
  Require(first.visible_rows.size() == 2,
          "IPAR-P3-01 first page visible row count mismatch");
  Require(second.visible_rows.size() == 2,
          "IPAR-P3-01 second page visible row count mismatch");
  Require(third.visible_rows.size() == 1,
          "IPAR-P3-01 third page visible row count mismatch");
  Require(HasEvidence(imported.evidence,
                      "direct_physical_bulk_row_page_visibility_authority",
                      "durable_transaction_inventory"),
          "IPAR-P3-01 row-page visibility authority evidence missing");
}

void VerifyRolledBackPhysicalRows() {
  auto fixture = MakeFixture("rollback", 401000 + TimeSeed() % 100000);
  auto context = Begin(fixture, "ipar-p301-rollback-import");
  const auto first_page = NextPhysicalAppendPage(fixture);
  const auto imported = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      Rows("rollback", 3),
      first_page,
      4,
      2));
  RequireOk(imported, "IPAR-P3-01 rolled-back import failed");
  Require(imported.inserted_rows == 3 && imported.accepted_rows == 3,
          "IPAR-P3-01 rolled-back import row count mismatch");
  Require(EvidenceId(imported.evidence,
                     "direct_physical_bulk_row_page_written_rows") == "3",
          "IPAR-P3-01 rolled-back physical write count mismatch");
  Rollback(context);

  Require(SelectCount(fixture) == 0,
          "IPAR-P3-01 rolled-back engine select saw rows");
  const auto read = ReadPage(fixture, first_page);
  Require(read.visible_rows.empty(),
          "IPAR-P3-01 rolled-back physical read exposed rows");
  Require(read.rolled_back_version_count == 3,
          "IPAR-P3-01 rolled-back physical version count mismatch");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("ipar_physical_row_page_append_gate-native");
  VerifyCommittedPhysicalRows();
  VerifyRolledBackPhysicalRows();
  return EXIT_SUCCESS;
}
