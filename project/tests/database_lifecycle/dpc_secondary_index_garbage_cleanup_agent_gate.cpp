// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
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
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT_GATE";

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
  Require(generated.ok(), "DPC-033 generated UUID creation failed");
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
    if (!dir.empty()) {
      std::filesystem::remove_all(dir, ignored);
    }
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
  RequireOk(begun, "DPC-033 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "DPC-033 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "DPC-033 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "dpc033_index_garbage_cleanup";
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
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_dpc033_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc033.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DPC-033 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.non_unique_index_uuid =
      NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "dpc033-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "DPC-033 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture,
                                context,
                                fixture.non_unique_index_uuid,
                                "name",
                                false)),
                      "DPC-033 non-unique index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture,
                                context,
                                fixture.unique_index_uuid,
                                "id",
                                true)),
                      "DPC-033 unique index metadata append failed");
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

api::EngineSelectRowsResult SelectEquals(const Fixture& fixture,
                                         const api::EngineRequestContext& context,
                                         std::string column,
                                         std::string value) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate.predicate_kind = "column_equals";
  request.select_predicate.canonical_predicate_envelope = std::move(column);
  request.select_predicate.bound_values.push_back(TextValue(std::move(value)));
  return api::EngineSelectRows(request);
}

void SeedCommittedRow(Fixture& fixture,
                      std::string id,
                      std::string name,
                      bool deferred) {
  auto context = Begin(fixture, "dpc033-seed");
  RequireOk(InsertRow(fixture,
                      context,
                      std::move(id),
                      std::move(name),
                      deferred ? DeferredOptions() : std::vector<std::string>{}),
            "DPC-033 seed insert failed");
  Commit(context);
}

idx::PersistentSecondaryIndexDeltaLedger LoadLedger(const Fixture& fixture) {
  const auto loaded = api::LoadMgaSecondaryIndexDeltaLedger(
      BaseContext(fixture, "dpc033-load-ledger"));
  Require(loaded.ok, "DPC-033 ledger load failed");
  return loaded.ledger;
}

platform::u64 MaxLedgerLocalTransactionId(
    const idx::PersistentSecondaryIndexDeltaLedger& ledger) {
  platform::u64 max_tx = 0;
  for (const auto& record : ledger.records) {
    max_tx = std::max(max_tx, record.delta.local_transaction_id);
  }
  return max_tx;
}

api::MgaSecondaryIndexDeltaMergeAgentRequest MergeRequest(
    const Fixture& fixture,
    platform::u64 horizon) {
  api::MgaSecondaryIndexDeltaMergeAgentRequest request;
  request.index_uuid = fixture.non_unique_index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  request.cleanup_horizon_authoritative = true;
  request.max_records_to_scan = 32;
  request.max_records_to_merge = 32;
  return request;
}

api::MgaSecondaryIndexGarbageCleanupRequest CleanupRequest(
    const Fixture& fixture) {
  api::MgaSecondaryIndexGarbageCleanupRequest request;
  request.index_uuid = fixture.non_unique_index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.max_records_to_scan = 32;
  request.max_records_to_clean = 32;
  return request;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind && entry.evidence_id == value) {
      return true;
    }
  }
  return false;
}

void RequireCleanupDecision(
    const api::MgaSecondaryIndexGarbageCleanupResult& result,
    std::string_view decision,
    std::string_view diagnostic_code) {
  Require(result.decision == decision, "DPC-033 cleanup decision mismatch");
  Require(HasEvidence(result.evidence,
                      "mga_secondary_index_garbage_cleanup.decision",
                      decision),
          "DPC-033 cleanup decision evidence missing");
  Require(HasEvidence(result.evidence,
                      "mga_secondary_index_garbage_cleanup.diagnostic_code",
                      diagnostic_code),
          "DPC-033 cleanup diagnostic evidence missing");
  Require(HasEvidence(result.evidence,
                      "mga_secondary_index_garbage_cleanup.cleanup_horizon_service",
                      "dpc030_authoritative_cleanup_horizon_v1"),
          "DPC-033 DPC-030 horizon evidence missing");
  Require(HasEvidence(result.evidence,
                      "mga_secondary_index_garbage_cleanup.parser_finality_authority",
                      "false"),
          "DPC-033 parser authority evidence missing");
}

void MergeDeferredRows(Fixture& fixture) {
  const auto ledger_before = LoadLedger(fixture);
  const auto merge = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc033-merge"),
      MergeRequest(fixture, MaxLedgerLocalTransactionId(ledger_before)));
  Require(merge.ok, "DPC-033 merge setup failed");
  Require(merge.cleaned_count != 0, "DPC-033 merge setup cleaned no records");
}

void TestSuccessfulCleanupAndNoOp() {
  auto fixture = MakeFixture("success_noop", 1000);
  SeedCommittedRow(fixture, "id-sync", "alpha", false);
  SeedCommittedRow(fixture, "id-deferred", "alpha", true);
  MergeDeferredRows(fixture);

  auto reader = Begin(fixture, "dpc033-reader-before-cleanup");
  const auto before = SelectEquals(fixture, reader, "name", "alpha");
  RequireOk(before, "DPC-033 pre-cleanup select failed");
  Require(before.visible_count == 2, "DPC-033 pre-cleanup semantics changed");
  Rollback(reader);

  auto cleanup_context = Begin(fixture, "dpc033-cleanup-success");
  const auto cleaned = api::CleanupMgaSecondaryIndexGarbageForIndex(
      cleanup_context,
      CleanupRequest(fixture));
  Require(cleaned.ok, "DPC-033 cleanup success failed");
  RequireCleanupDecision(cleaned,
                         "success",
                         "INDEX_GARBAGE_CLEANUP.SUCCESS");
  Require(cleaned.cleaned_count == 1, "DPC-033 cleaned count mismatch");
  Require(cleaned.validation_before_ok && cleaned.validation_after_ok,
          "DPC-033 validation evidence missing on success");
  Rollback(cleanup_context);

  Require(LoadLedger(fixture).records.empty(),
          "DPC-033 merged-cleaned garbage was not removed");

  reader = Begin(fixture, "dpc033-reader-after-cleanup");
  const auto after = SelectEquals(fixture, reader, "name", "alpha");
  RequireOk(after, "DPC-033 post-cleanup select failed");
  Require(after.visible_count == 2, "DPC-033 post-cleanup semantics changed");
  Rollback(reader);

  cleanup_context = Begin(fixture, "dpc033-cleanup-noop");
  const auto noop = api::CleanupMgaSecondaryIndexGarbageForIndex(
      cleanup_context,
      CleanupRequest(fixture));
  Require(noop.ok, "DPC-033 no-op cleanup failed");
  RequireCleanupDecision(noop,
                         "no_op",
                         "INDEX_GARBAGE_CLEANUP.NO_OP");
  Rollback(cleanup_context);
}

void TestBudgetExhaustedCleansBoundedBatch() {
  auto fixture = MakeFixture("budget", 2000);
  SeedCommittedRow(fixture, "id-a", "alpha", true);
  SeedCommittedRow(fixture, "id-b", "alpha", true);
  MergeDeferredRows(fixture);

  auto cleanup_context = Begin(fixture, "dpc033-cleanup-budget");
  auto request = CleanupRequest(fixture);
  request.max_records_to_clean = 1;
  const auto budget = api::CleanupMgaSecondaryIndexGarbageForIndex(
      cleanup_context,
      request);
  Require(budget.ok, "DPC-033 budget cleanup failed");
  RequireCleanupDecision(budget,
                         "budget_exhausted",
                         "INDEX_GARBAGE_CLEANUP.BUDGET_EXHAUSTED");
  Require(budget.budget_exhausted, "DPC-033 budget flag missing");
  Require(budget.cleaned_count == 1, "DPC-033 budget cleanup count mismatch");
  Rollback(cleanup_context);

  Require(LoadLedger(fixture).records.size() == 1,
          "DPC-033 budget cleanup did not retain remainder");
}

void TestHorizonBlocked() {
  auto fixture = MakeFixture("horizon", 3000);
  auto old_active = Begin(fixture, "dpc033-old-active-blocker");
  SeedCommittedRow(fixture, "id-blocked", "alpha", true);
  MergeDeferredRows(fixture);

  auto cleanup_context = Begin(fixture, "dpc033-cleanup-horizon-blocked");
  const auto blocked = api::CleanupMgaSecondaryIndexGarbageForIndex(
      cleanup_context,
      CleanupRequest(fixture));
  Require(blocked.ok, "DPC-033 horizon-blocked cleanup should be safe no-op");
  RequireCleanupDecision(blocked,
                         "horizon_blocked",
                         "INDEX_GARBAGE_CLEANUP.HORIZON_BLOCKED");
  Require(blocked.horizon_blocked, "DPC-033 horizon blocked flag missing");
  Require(blocked.cleaned_count == 0, "DPC-033 horizon block cleaned a record");
  Rollback(cleanup_context);
  Rollback(old_active);
}

void TestValidationFailureAndNonAuthoritativeRefusal() {
  auto fixture = MakeFixture("refusals", 4000);
  SeedCommittedRow(fixture, "id-refuse", "alpha", true);
  MergeDeferredRows(fixture);

  {
    std::ofstream out(fixture.database_path.string() + ".sb.mga_index_entries",
                      std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(out), "DPC-033 could not corrupt index sidecar");
  }

  auto cleanup_context = Begin(fixture, "dpc033-cleanup-validation-refused");
  const auto refused = api::CleanupMgaSecondaryIndexGarbageForIndex(
      cleanup_context,
      CleanupRequest(fixture));
  Require(!refused.ok, "DPC-033 validation failure was accepted");
  Require(refused.decision == "validation_refused",
          "DPC-033 validation refusal decision mismatch");
  Require(refused.diagnostic.code == "INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
          "DPC-033 validation refusal diagnostic mismatch");
  Rollback(cleanup_context);

  auto non_authoritative_fixture = MakeFixture("non_authoritative", 5000);
  SeedCommittedRow(non_authoritative_fixture, "id-na", "alpha", true);
  MergeDeferredRows(non_authoritative_fixture);
  cleanup_context = Begin(non_authoritative_fixture, "dpc033-cleanup-na");
  auto non_authoritative = CleanupRequest(non_authoritative_fixture);
  non_authoritative.inventory_authoritative = false;
  const auto na = api::CleanupMgaSecondaryIndexGarbageForIndex(
      cleanup_context,
      non_authoritative);
  Require(!na.ok, "DPC-033 non-authoritative cleanup was accepted");
  RequireCleanupDecision(na,
                         "refused_non_authoritative",
                         "INDEX_GARBAGE_CLEANUP.NON_AUTHORITATIVE_REFUSAL");
  Rollback(cleanup_context);
}

void TestUniqueIndexRefusal() {
  auto fixture = MakeFixture("unique", 6000);
  SeedCommittedRow(fixture, "id-unique", "alpha", false);
  auto cleanup_context = Begin(fixture, "dpc033-cleanup-unique");
  auto request = CleanupRequest(fixture);
  request.index_uuid = fixture.unique_index_uuid;
  const auto unique = api::CleanupMgaSecondaryIndexGarbageForIndex(
      cleanup_context,
      request);
  Require(!unique.ok, "DPC-033 unique-index cleanup was accepted");
  Require(unique.diagnostic.code == "INDEX_GARBAGE_CLEANUP.UNIQUE_INDEX_REFUSED",
          "DPC-033 unique-index refusal diagnostic mismatch");
  Rollback(cleanup_context);
}

}  // namespace

int main() {
  std::cout << kGateSearchKey << '\n';
  TestSuccessfulCleanupAndNoOp();
  TestBudgetExhaustedCleansBoundedBatch();
  TestHorizonBlocked();
  TestValidationFailureAndNonAuthoritativeRefusal();
  TestUniqueIndexRefusal();
  return EXIT_SUCCESS;
}
