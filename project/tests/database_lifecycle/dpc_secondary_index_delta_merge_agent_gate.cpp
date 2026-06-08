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

constexpr std::string_view kMergeSearchKey =
    "DPC_SECONDARY_INDEX_DELTA_MERGE_AGENT_GATE";

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
  Require(generated.ok(), "DPC-024 generated UUID creation failed");
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
  RequireOk(begun, "DPC-024 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "DPC-024 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "DPC-024 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "dpc024_delta_merge_agent";
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
                ("scratchbird_dpc024_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc024.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DPC-024 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.non_unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "dpc024-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "DPC-024 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.non_unique_index_uuid,
                                "name", false)),
                      "DPC-024 non-unique index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.unique_index_uuid,
                                "id", true)),
                      "DPC-024 unique index metadata append failed");
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

std::string SeedCommittedRow(Fixture& fixture,
                             std::string id,
                             std::string name,
                             bool deferred) {
  auto context = Begin(fixture, "dpc024-seed");
  const auto inserted = InsertRow(fixture,
                                  context,
                                  std::move(id),
                                  std::move(name),
                                  deferred ? DeferredOptions()
                                           : std::vector<std::string>{});
  RequireOk(inserted, "DPC-024 seed insert failed");
  Require(inserted.row_uuids.size() == 1, "DPC-024 seed row UUID missing");
  Commit(context);
  return inserted.row_uuids.front().canonical;
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

idx::PersistentSecondaryIndexDeltaLedger LoadLedger(const Fixture& fixture) {
  const auto loaded = api::LoadMgaSecondaryIndexDeltaLedger(BaseContext(fixture, "dpc024-load-ledger"));
  Require(loaded.ok, "DPC-024 ledger load failed");
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
    platform::u64 horizon,
    bool authoritative = true) {
  api::MgaSecondaryIndexDeltaMergeAgentRequest request;
  request.index_uuid = fixture.non_unique_index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  request.cleanup_horizon_authoritative = authoritative;
  request.max_records_to_scan = 16;
  request.max_records_to_merge = 16;
  return request;
}

bool HasMergedCleanedRecord(const idx::PersistentSecondaryIndexDeltaLedger& ledger) {
  for (const auto& record : ledger.records) {
    if (record.commit_state ==
        idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned) {
      return true;
    }
  }
  return false;
}

void ValidateAuthoritativeMergeDrainsIntoBase() {
  auto fixture = MakeFixture("authoritative_merge", 1000);
  (void)SeedCommittedRow(fixture, "id-base", "alpha", false);
  (void)SeedCommittedRow(fixture, "id-delta", "alpha", true);
  auto active_sync = Begin(fixture, "dpc024-active-sync");
  RequireOk(InsertRow(fixture, active_sync, "id-active-sync", "bravo", {}),
            "DPC-024 active synchronous insert failed");

  auto before_reader = Begin(fixture, "dpc024-before-reader");
  const auto before = SelectEquals(fixture, before_reader, "name", "alpha");
  RequireOk(before, "DPC-024 pre-merge select failed");
  Require(before.visible_count == 2, "DPC-024 pre-merge overlay did not see both rows");
  Require(HasEvidence(before.evidence, "mga_secondary_index_delta_overlay_used"),
          "DPC-024 pre-merge overlay evidence missing");
  Rollback(before_reader);

  const auto ledger_before = LoadLedger(fixture);
  const auto merge = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-merge"),
      MergeRequest(fixture, MaxLedgerLocalTransactionId(ledger_before)));
  Require(merge.ok, "DPC-024 authoritative merge failed");
  Require(merge.merged_count == 1, "DPC-024 merge count changed");
  Require(merge.cleaned_count == 1, "DPC-024 cleaned count changed");
  Require(EvidenceValue(merge.evidence, "mga_secondary_index_delta_merge_result") ==
              "successful_merge",
          "DPC-024 successful merge evidence missing");
  Require(HasMergedCleanedRecord(LoadLedger(fixture)),
          "DPC-024 merged ledger state was not retained deterministically");
  Commit(active_sync);

  auto after_reader = Begin(fixture, "dpc024-after-reader");
  const auto after = SelectEquals(fixture, after_reader, "name", "alpha");
  RequireOk(after, "DPC-024 post-merge select failed");
  Require(after.visible_count == 2, "DPC-024 post-merge base lookup lost rows");
  Require(!HasEvidence(after.evidence, "mga_secondary_index_delta_overlay_used"),
          "DPC-024 post-merge lookup still required overlay");
  Require(EvidenceValue(after.evidence, "mga_secondary_index_lookup_path") ==
              "non_unique_synchronous_no_delta",
          "DPC-024 post-merge lookup did not use base-only path");
  Rollback(after_reader);

  auto active_reader = Begin(fixture, "dpc024-active-sync-reader");
  const auto active_after = SelectEquals(fixture, active_reader, "name", "bravo");
  RequireOk(active_after, "DPC-024 active synchronous post-merge select failed");
  Require(active_after.visible_count == 1,
          "DPC-024 merge rewrite dropped an active synchronous index entry");
  Require(EvidenceValue(active_after.evidence, "mga_secondary_index_lookup_path") ==
              "non_unique_synchronous_no_delta",
          "DPC-024 active synchronous preserved entry did not use base index path");
  Rollback(active_reader);
}

void ValidateHorizonRetainsFutureAndResourceRefuses() {
  auto fixture = MakeFixture("horizon_resource", 2000);
  (void)SeedCommittedRow(fixture, "id-one", "alpha", true);
  (void)SeedCommittedRow(fixture, "id-two", "bravo", true);

  const auto ledger = LoadLedger(fixture);
  Require(ledger.records.size() == 2, "DPC-024 resource fixture ledger size changed");
  auto throttled = MergeRequest(fixture, MaxLedgerLocalTransactionId(ledger));
  throttled.max_records_to_scan = 1;
  const auto refused = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-resource-refusal"),
      throttled);
  Require(!refused.ok, "DPC-024 resource governor accepted over-budget scan");
  Require(refused.diagnostic.code == "resource_governor_throttled",
          "DPC-024 resource governor diagnostic changed");

  const platform::u64 low_horizon =
      std::min(ledger.records[0].delta.local_transaction_id,
               ledger.records[1].delta.local_transaction_id);
  const auto partial = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-horizon-retain"),
      MergeRequest(fixture, low_horizon));
  Require(partial.ok, "DPC-024 bounded horizon merge failed");
  Require(partial.retained_count == 1, "DPC-024 above-horizon delta was not retained");
  Require(partial.cleaned_count == 1, "DPC-024 eligible delta was not cleaned");

  auto budget_fixture = MakeFixture("merge_budget", 2500);
  (void)SeedCommittedRow(budget_fixture, "id-a", "alpha", true);
  (void)SeedCommittedRow(budget_fixture, "id-b", "alpha", true);
  const auto budget_ledger = LoadLedger(budget_fixture);
  auto budgeted = MergeRequest(budget_fixture,
                               MaxLedgerLocalTransactionId(budget_ledger));
  budgeted.max_records_to_merge = 1;
  const auto partial_budget = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(budget_fixture, "dpc024-merge-budget"),
      budgeted);
  Require(partial_budget.ok, "DPC-024 merge-budget throttle refused all work");
  Require(partial_budget.throttled,
          "DPC-024 merge-budget throttle flag was not set");
  Require(partial_budget.throttle_or_refusal_reason == "resource_governor_throttled",
          "DPC-024 merge-budget throttle reason changed");
  Require(partial_budget.retained_count == 1,
          "DPC-024 merge-budget retained count changed");
  Require(partial_budget.cleaned_count == 1,
          "DPC-024 merge-budget cleaned count changed");
}

void ValidateRefusalDiagnostics() {
  auto fixture = MakeFixture("refusals", 3000);
  (void)SeedCommittedRow(fixture, "id-delta", "alpha", true);
  const auto horizon = MaxLedgerLocalTransactionId(LoadLedger(fixture));

  const auto no_authority = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-no-authority"),
      MergeRequest(fixture, horizon, false));
  Require(!no_authority.ok, "DPC-024 accepted non-authoritative horizon");
  Require(no_authority.diagnostic.code == "not_authoritative_horizon",
          "DPC-024 not-authoritative diagnostic changed");

  auto disabled = MergeRequest(fixture, horizon);
  disabled.merge_disabled = true;
  const auto disabled_refused = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-disabled"),
      disabled);
  Require(!disabled_refused.ok, "DPC-024 accepted disabled merge agent");
  Require(disabled_refused.diagnostic.code == "merge_agent_disabled",
          "DPC-024 disabled-agent diagnostic changed");

  auto zero_budget = MergeRequest(fixture, horizon);
  zero_budget.max_records_to_merge = 0;
  const auto zero_refused = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-zero-budget"),
      zero_budget);
  Require(!zero_refused.ok, "DPC-024 accepted zero merge budget");
  Require(zero_refused.throttled,
          "DPC-024 zero-budget throttle flag was not set");
  Require(zero_refused.diagnostic.code == "resource_governor_throttled",
          "DPC-024 zero-budget diagnostic changed");

  auto unique = MergeRequest(fixture, horizon);
  unique.index_uuid = fixture.unique_index_uuid;
  const auto unique_refused = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-unique"),
      unique);
  Require(!unique_refused.ok, "DPC-024 accepted unique-index delta merge");
  Require(unique_refused.diagnostic.code == "unique_index_delta_refused",
          "DPC-024 unique refusal diagnostic changed");

  const auto path = fixture.database_path.string() +
                    ".sb.mga_secondary_index_delta_ledger";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "DPC024_CORRUPT_LEDGER";
  out.flush();
  Require(static_cast<bool>(out), "DPC-024 corrupt ledger write failed");
  const auto corrupt = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc024-corrupt"),
      MergeRequest(fixture, horizon));
  Require(!corrupt.ok, "DPC-024 accepted corrupt ledger");
  Require(corrupt.diagnostic.code == "corrupt_ledger_refused",
          "DPC-024 corrupt ledger diagnostic changed");
}

}  // namespace

int main() {
  Require(kMergeSearchKey == "DPC_SECONDARY_INDEX_DELTA_MERGE_AGENT_GATE",
          "DPC-024 gate search key drifted");
  ValidateAuthoritativeMergeDrainsIntoBase();
  ValidateHorizonRetainsFutureAndResourceRefuses();
  ValidateRefusalDiagnostics();
  return EXIT_SUCCESS;
}
