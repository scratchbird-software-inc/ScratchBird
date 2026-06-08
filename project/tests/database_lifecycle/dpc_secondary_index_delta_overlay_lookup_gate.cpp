// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/plan_api.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kLookupSearchKey =
    "DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP";
constexpr std::string_view kGateSearchKey =
    "DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "DPC-023 generated UUID creation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string name) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  row.fields.push_back({"note", TextValue("note")});
  return row;
}

std::vector<std::string> DeferredOptions() {
  return {"runtime.deferred_secondary_index=enabled",
          "feature.secondary_index_delta_ledger=enabled",
          "delta_ledger.reader_overlay=enabled",
          "delta_ledger.cleanup_horizon_bound=true",
          "delta_ledger.recovery_classifiable=true"};
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string non_unique_index_uuid;
  std::string unique_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
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
  RequireOk(begun, "DPC-023 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "DPC-023 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "DPC-023 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "dpc023_delta_overlay_lookup";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context,
                           std::string index_uuid,
                           std::string column,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column);
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  index.key_envelopes.push_back(index.column_name);
  if (unique) { index.key_envelopes.push_back("unique"); }
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_dpc023_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc023.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DPC-023 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.non_unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "dpc023-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "DPC-023 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.non_unique_index_uuid,
                                "name", false)),
                      "DPC-023 non-unique index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.unique_index_uuid,
                                "id", true)),
                      "DPC-023 unique index metadata append failed");
  Commit(context);
  return fixture;
}

api::EngineInsertRowsResult InsertRow(const Fixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id,
                                      std::string name,
                                      std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), std::move(name)));
  request.estimated_row_count = 1;
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

api::EngineUpdateRowsResult UpdateName(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       std::string id,
                                       std::string name) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate.predicate_kind = "column_equals";
  request.update_predicate.canonical_predicate_envelope = "id";
  request.update_predicate.bound_values.push_back(TextValue(std::move(id)));
  request.assignments.push_back({"name", TextValue(std::move(name))});
  request.option_envelopes = DeferredOptions();
  return api::EngineUpdateRows(request);
}

api::EngineDeleteRowsResult DeleteByRowUuid(const Fixture& fixture,
                                            const api::EngineRequestContext& context,
                                            std::string row_uuid) {
  api::EngineDeleteRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate.predicate_kind = "row_uuid_match";
  request.delete_predicate.canonical_predicate_envelope = std::move(row_uuid);
  request.tombstone_only = true;
  request.option_envelopes = DeferredOptions();
  return api::EngineDeleteRows(request);
}

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

api::EngineSelectRowsResult SelectEquals(const Fixture& fixture,
                                         const api::EngineRequestContext& context,
                                         std::string column,
                                         std::string value) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate = EqualsPredicate(std::move(column), std::move(value));
  return api::EngineSelectRows(request);
}

api::EnginePlanOperationResult PlanEquals(const Fixture& fixture,
                                          const api::EngineRequestContext& context,
                                          std::string column,
                                          std::string value) {
  api::EnginePlanOperationRequest request;
  request.context = context;
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.query_operation = "index_lookup";
  request.predicate = EqualsPredicate(std::move(column), std::move(value));
  return api::EnginePlanOperation(request);
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) { return true; }
  }
  return false;
}

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) { return entry.evidence_id; }
  }
  return {};
}

std::size_t CountRowsWithField(const api::EngineSelectRowsResult& result,
                               std::string_view field,
                               std::string_view value) {
  std::size_t count = 0;
  for (const auto& row : result.result_shape.rows) {
    for (const auto& candidate : row.fields) {
      if (candidate.first == field &&
          candidate.second.encoded_value == value) {
        ++count;
      }
    }
  }
  return count;
}

std::string SeedCommittedRow(Fixture& fixture,
                             std::string id,
                             std::string name,
                             bool deferred) {
  auto context = Begin(fixture, "dpc023-seed");
  const auto inserted = InsertRow(fixture,
                                  context,
                                  std::move(id),
                                  std::move(name),
                                  deferred ? DeferredOptions()
                                           : std::vector<std::string>{});
  RequireOk(inserted, "DPC-023 seed insert failed");
  Require(inserted.row_uuids.size() == 1, "DPC-023 seed row UUID missing");
  const std::string row_uuid = inserted.row_uuids.front().canonical;
  Commit(context);
  return row_uuid;
}

void CorruptLedgerImage(const Fixture& fixture) {
  const auto path = fixture.database_path.string() +
                    ".sb.mga_secondary_index_delta_ledger";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "DPC023_CORRUPT_LEDGER";
  out.flush();
  Require(static_cast<bool>(out), "DPC-023 corrupt ledger write failed");
}

void ValidateCommittedInsertOverlayAndPlanning() {
  auto fixture = MakeFixture("insert_overlay", 1000);
  (void)SeedCommittedRow(fixture, "id-base", "alpha", false);
  (void)SeedCommittedRow(fixture, "id-delta", "alpha", true);

  auto reader = Begin(fixture, "dpc023-read-insert-overlay");
  const auto selected = SelectEquals(fixture, reader, "name", "alpha");
  RequireOk(selected, "DPC-023 committed delta select failed");
  Require(selected.visible_count == 2,
          "DPC-023 overlay did not return base plus committed delta rows");
  Require(HasEvidence(selected.evidence,
                      "mga_secondary_index_delta_overlay_used"),
          "DPC-023 select omitted overlay evidence");
  Require(EvidenceValue(selected.evidence,
                        "mga_secondary_index_delta_overlay_visible_delta_count") == "1",
          "DPC-023 overlay visible delta evidence changed");

  const auto plan = PlanEquals(fixture, reader, "name", "alpha");
  RequireOk(plan, "DPC-023 plan operation failed");
  Require(HasEvidence(plan.evidence,
                      "mga_secondary_index_delta_overlay_used"),
          "DPC-023 planner omitted overlay evidence");
  Rollback(reader);
}

void ValidateUpdateAndDeleteOverlay() {
  auto fixture = MakeFixture("update_delete_overlay", 2000);
  (void)SeedCommittedRow(fixture, "id-update", "alpha", false);
  const std::string delete_row_uuid =
      SeedCommittedRow(fixture, "id-delete", "alpha", false);

  auto update_context = Begin(fixture, "dpc023-update");
  RequireOk(UpdateName(fixture, update_context, "id-update", "bravo"),
            "DPC-023 deferred update failed");
  Commit(update_context);

  auto delete_context = Begin(fixture, "dpc023-delete");
  RequireOk(DeleteByRowUuid(fixture, delete_context, delete_row_uuid),
            "DPC-023 deferred delete failed");
  Commit(delete_context);

  auto reader = Begin(fixture, "dpc023-read-update-delete");
  const auto alpha = SelectEquals(fixture, reader, "name", "alpha");
  RequireOk(alpha, "DPC-023 alpha select failed");
  Require(CountRowsWithField(alpha, "id", "id-update") == 0,
          "DPC-023 update_before did not remove stale base alpha entry");
  Require(CountRowsWithField(alpha, "id", "id-delete") == 0,
          "DPC-023 delete tombstone did not suppress base alpha entry");

  const auto bravo = SelectEquals(fixture, reader, "name", "bravo");
  RequireOk(bravo, "DPC-023 bravo select failed");
  Require(bravo.visible_count == 1 &&
              CountRowsWithField(bravo, "id", "id-update") == 1,
          "DPC-023 update_after delta was not visible through overlay");
  Rollback(reader);
}

void ValidateOwnAndOtherTransactionVisibility() {
  auto fixture = MakeFixture("txn_visibility", 3000);
  auto writer = Begin(fixture, "dpc023-writer");
  RequireOk(InsertRow(fixture, writer, "id-own", "alpha", DeferredOptions()),
            "DPC-023 own deferred insert failed");

  const auto own = SelectEquals(fixture, writer, "name", "alpha");
  RequireOk(own, "DPC-023 own transaction select failed");
  Require(own.visible_count == 1 &&
              HasEvidence(own.evidence, "mga_secondary_index_delta_overlay_used"),
          "DPC-023 own transaction overlay visibility was not admitted safely");

  auto reader = Begin(fixture, "dpc023-other-reader");
  const auto other = SelectEquals(fixture, reader, "name", "alpha");
  RequireOk(other, "DPC-023 other transaction select failed");
  Require(other.visible_count == 0,
          "DPC-023 uncommitted other transaction delta became visible");
  Rollback(reader);
  Rollback(writer);
}

void ValidateUniqueIndexBypassAndCorruptFallback() {
  auto fixture = MakeFixture("unique_corrupt", 4000);
  (void)SeedCommittedRow(fixture, "id-unique", "alpha", true);

  auto reader = Begin(fixture, "dpc023-unique-reader");
  const auto unique = SelectEquals(fixture, reader, "id", "id-unique");
  RequireOk(unique, "DPC-023 unique index select failed");
  Require(unique.visible_count == 1,
          "DPC-023 unique synchronous lookup did not return row");
  Require(!HasEvidence(unique.evidence,
                       "mga_secondary_index_delta_overlay_used"),
          "DPC-023 unique index incorrectly routed through overlay");
  Rollback(reader);

  CorruptLedgerImage(fixture);
  auto fallback_reader = Begin(fixture, "dpc023-corrupt-reader");
  const auto fallback = SelectEquals(fixture, fallback_reader, "name", "alpha");
  RequireOk(fallback, "DPC-023 corrupt ledger row-scan fallback failed");
  Require(fallback.visible_count == 1,
          "DPC-023 corrupt ledger fallback did not preserve correct row result");
  Require(HasEvidence(fallback.evidence,
                      "mga_secondary_index_delta_overlay_refused"),
          "DPC-023 corrupt ledger refusal evidence missing");
  Require(HasEvidence(fallback.evidence, "row_scan_predicate"),
          "DPC-023 corrupt ledger did not expose row-scan fallback evidence");
  (void)fallback_reader;
}

}  // namespace

int main() {
  Require(kLookupSearchKey == "DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP",
          "DPC-023 lookup search key drifted");
  Require(kGateSearchKey == "DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP_GATE",
          "DPC-023 gate search key drifted");

  ValidateCommittedInsertOverlayAndPlanning();
  ValidateUpdateAndDeleteOverlay();
  ValidateOwnAndOtherTransactionVisibility();
  ValidateUniqueIndexBypassAndCorruptFallback();
  return EXIT_SUCCESS;
}
