// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
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
  Require(generated.ok(), "IPAR relation-state UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
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

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return item.evidence_id;
    }
  }
  return {};
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view detail) {
  const std::string needle(detail);
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail == needle ||
        diagnostic.detail.find(needle) != std::string::npos ||
        diagnostic.code.find(needle) != std::string::npos ||
        diagnostic.message_key.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void DumpDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string target_table_uuid;
  std::string child_table_uuid;
  std::string unrelated_table_uuid;
  std::string target_index_uuid;
  std::string schema_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
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

api::EngineRowValue Row(std::string value, std::string note = {}) {
  api::EngineRowValue row;
  row.fields.push_back({"payload", TextValue(std::move(value))});
  if (!note.empty()) {
    row.fields.push_back({"note", TextValue(std::move(note))});
  }
  return row;
}

api::CrudTableRecord Table(std::string table_uuid, std::string name,
                           std::uint64_t creator_tx,
                           bool primary_key = false) {
  api::CrudTableRecord table;
  table.creator_tx = creator_tx;
  table.table_uuid = std::move(table_uuid);
  table.default_name = std::move(name);
  table.columns.push_back({"payload",
                           primary_key ? "canonical=character;primary_key=true"
                                       : "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudTableRecord ChildTable(const Fixture& fixture, std::uint64_t creator_tx) {
  api::CrudTableRecord table;
  table.creator_tx = creator_tx;
  table.table_uuid = fixture.child_table_uuid;
  table.default_name = "ipar_relation_state_child";
  table.columns.push_back({"payload",
                           "canonical=character;referenced_table_uuid=" +
                               fixture.target_table_uuid + ";referenced_column=payload"});
  return table;
}

api::CrudIndexRecord UniquePayloadIndex(const Fixture& fixture,
                                        std::uint64_t creator_tx) {
  api::CrudIndexRecord index;
  index.creator_tx = creator_tx;
  index.index_uuid = fixture.target_index_uuid;
  index.table_uuid = fixture.target_table_uuid;
  index.column_name = "payload";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "ipar_relation_state_payload_uidx";
  index.unique = true;
  index.key_envelopes.push_back("payload");
  index.key_envelopes.push_back("unique");
  return index;
}

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, fixture.salt + 101);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, fixture.salt + 102);
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "IPAR relation-state begin transaction failed");
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
  const auto committed = api::EngineCommitTransaction(request);
  if (!committed.ok) {
    for (const auto& diagnostic : committed.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(committed.ok, "IPAR relation-state commit failed");
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.salt = MillisSeed();
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_relation_state_" +
                 std::to_string(fixture.salt) + "_" +
                 std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_relation_state.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = MillisSeed() + 3;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR relation-state database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::schema, fixture.salt + 10);
  fixture.target_table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 20);
  fixture.child_table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 21);
  fixture.target_index_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 22);
  fixture.unrelated_table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 30);

  auto metadata = Begin(fixture, "ipar-relation-state-metadata");
  const auto target = api::AppendMgaTableMetadata(
      metadata,
      Table(fixture.target_table_uuid, "ipar_relation_state_target",
            metadata.local_transaction_id, true));
  Require(!target.error, "IPAR relation-state target metadata append failed");
  const auto child = api::AppendMgaTableMetadata(
      metadata,
      ChildTable(fixture, metadata.local_transaction_id));
  Require(!child.error, "IPAR relation-state child metadata append failed");
  const auto target_index = api::AppendMgaIndexMetadata(
      metadata,
      UniquePayloadIndex(fixture, metadata.local_transaction_id));
  Require(!target_index.error, "IPAR relation-state target index metadata append failed");
  const auto unrelated = api::AppendMgaTableMetadata(
      metadata,
      Table(fixture.unrelated_table_uuid, "ipar_relation_state_unrelated",
            metadata.local_transaction_id));
  Require(!unrelated.error, "IPAR relation-state unrelated metadata append failed");
  Commit(metadata);
  return fixture;
}

api::EngineInsertRowsResult InsertInto(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       const std::string& table_uuid,
                                       std::string payload,
                                       std::vector<std::string> options = {},
                                       std::string note = {}) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = table_uuid;
  request.target_object.object_kind = "table";
  request.estimated_row_count = 1;
  request.input_rows.push_back(Row(std::move(payload), std::move(note)));
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

void RequireInsertOk(const api::EngineInsertRowsResult& result,
                     std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, message);
}

void VerifyRelationStateLoadRoutes() {
  auto fixture = MakeFixture();

  auto seed = Begin(fixture, "ipar-relation-state-seed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.target_table_uuid, "target-1", {}, "seed"),
                  "IPAR relation-state target seed insert failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.child_table_uuid, "target-1"),
                  "IPAR relation-state child seed insert failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.unrelated_table_uuid, "unrelated-1"),
                  "IPAR relation-state unrelated seed insert 1 failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.unrelated_table_uuid, "unrelated-2"),
                  "IPAR relation-state unrelated seed insert 2 failed");
  Commit(seed);

  auto verify = Begin(fixture, "ipar-relation-state-verify");
  const auto diagnostic_full_load = api::LoadMgaRelationStoreState(verify);
  Require(diagnostic_full_load.ok,
          "IPAR relation-state diagnostic full loader failed");
  Require(diagnostic_full_load.full_state_load,
          "IPAR relation-state diagnostic loader did not mark full load");
  Require(!diagnostic_full_load.scoped_state_load,
          "IPAR relation-state diagnostic loader incorrectly marked scoped load");
  Require(diagnostic_full_load.row_versions_retained == 4,
          "IPAR relation-state diagnostic full loader did not retain all rows");

  const auto refused = InsertInto(
      fixture,
      verify,
      fixture.target_table_uuid,
      "target-refused",
      {"relation_state_load=full"});
  Require(!refused.ok,
          "IPAR relation-state mutation accepted caller-forced full load");
  if (!HasDiagnostic(refused, "relation_state_full_load_diagnostic_only")) {
    DumpDiagnostics(refused);
    Fail("IPAR relation-state full-load refusal diagnostic missing");
  }
  Require(HasEvidence(refused.evidence,
                      "relation_state_full_load_refused",
                      "diagnostic_only"),
          "IPAR relation-state full-load refusal evidence missing");
  Require(HasEvidence(refused.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state refusal incorrectly loaded full state");

  const auto normal = InsertInto(fixture,
                                 verify,
                                 fixture.target_table_uuid,
                                 "target-2");
  RequireInsertOk(normal, "IPAR relation-state normal scoped insert failed");
  Require(HasEvidence(normal.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state normal insert performed full load");
  Require(HasEvidence(normal.evidence, "relation_state_scoped_loads", "1"),
          "IPAR relation-state normal insert did not perform scoped load");
  Require(HasEvidence(normal.evidence,
                      "relation_state_load_reason",
                      "target_table_insert_scope"),
          "IPAR relation-state scoped reason evidence missing");
  Require(EvidenceValue(normal.evidence,
                        "mga_relation_state_row_versions_retained") == "2",
          "IPAR relation-state scoped loader did not retain only target and child row state");
  Require(EvidenceValue(normal.evidence,
                        "mga_relation_state_row_versions_scanned") == "2",
          "IPAR relation-state scoped loader scanned unrelated row state");
  Require(EvidenceValue(normal.evidence,
                        "mga_relation_state_index_entries_scanned") == "1",
          "IPAR relation-state scoped loader scanned unrelated index state");
  Require(HasEvidence(normal.evidence,
                      "mga_relation_state_scoped_physical_segments",
                      "true"),
          "IPAR relation-state scoped physical segment evidence missing");
  Require(HasEvidence(normal.evidence,
                      "mga_relation_state_scoped_physical_fallback",
                      "false"),
          "IPAR relation-state scoped loader used global fallback");

  const auto do_nothing = InsertInto(
      fixture,
      verify,
      fixture.target_table_uuid,
      "target-1",
      {"on_conflict_action:do_nothing", "conflict_target_column:payload"},
      "ignored");
  RequireInsertOk(do_nothing, "IPAR relation-state ON CONFLICT DO NOTHING failed");
  Require(do_nothing.skipped_count == 1,
          "IPAR relation-state ON CONFLICT DO NOTHING did not skip duplicate");
  Require(HasEvidence(do_nothing.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state ON CONFLICT DO NOTHING performed full load");
  Require(HasEvidence(do_nothing.evidence, "relation_state_scoped_loads", "1"),
          "IPAR relation-state ON CONFLICT DO NOTHING did not perform scoped load");
  Require(HasEvidence(do_nothing.evidence,
                      "mga_relation_state_scoped_physical_segments",
                      "true"),
          "IPAR relation-state ON CONFLICT DO NOTHING did not use scoped physical segments");
  Require(HasEvidence(do_nothing.evidence,
                      "mga_relation_state_scoped_physical_fallback",
                      "false"),
          "IPAR relation-state ON CONFLICT DO NOTHING used global fallback");

  const auto do_update = InsertInto(
      fixture,
      verify,
      fixture.target_table_uuid,
      "target-1",
      {"on_conflict_action:do_update",
       "conflict_target_column:payload",
       "on_conflict_update_column:note"},
      "updated");
  RequireInsertOk(do_update, "IPAR relation-state ON CONFLICT DO UPDATE failed");
  Require(do_update.updated_count == 1,
          "IPAR relation-state ON CONFLICT DO UPDATE did not update duplicate");
  Require(HasEvidence(do_update.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state ON CONFLICT DO UPDATE performed full load");
  Require(HasEvidence(do_update.evidence, "relation_state_scoped_loads", "1"),
          "IPAR relation-state ON CONFLICT DO UPDATE did not perform scoped load");
  Require(HasEvidence(do_update.evidence,
                      "relation_state_load_reason",
                      "target_table_insert_and_child_reference_scope"),
          "IPAR relation-state ON CONFLICT DO UPDATE reference-scope evidence missing");
  Require(EvidenceValue(do_update.evidence,
                        "mga_relation_state_row_versions_retained") == "3",
          "IPAR relation-state ON CONFLICT DO UPDATE retained unrelated row state");
  Require(EvidenceValue(do_update.evidence,
                        "mga_relation_state_row_versions_scanned") == "3",
          "IPAR relation-state ON CONFLICT DO UPDATE scanned unrelated row state");
  Require(HasEvidence(do_update.evidence,
                      "mga_relation_state_scoped_physical_segments",
                      "true"),
          "IPAR relation-state ON CONFLICT DO UPDATE did not use scoped physical segments");
  Require(HasEvidence(do_update.evidence,
                      "mga_relation_state_scoped_physical_fallback",
                      "false"),
          "IPAR relation-state ON CONFLICT DO UPDATE used global fallback");
  Commit(verify);
}

}  // namespace

int main() {
  VerifyRelationStateLoadRoutes();
  return EXIT_SUCCESS;
}
