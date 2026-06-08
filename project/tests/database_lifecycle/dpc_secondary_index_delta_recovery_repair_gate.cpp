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

constexpr std::string_view kRecoverySearchKey =
    "DPC_SECONDARY_INDEX_DELTA_RECOVERY_REPAIR";
constexpr std::string_view kGateSearchKey =
    "DPC_SECONDARY_INDEX_DELTA_RECOVERY_REPAIR_GATE";

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
  Require(generated.ok(), "DPC-025 generated UUID creation failed");
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

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id,
                                bool read_only = false) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  if (read_only) {
    request.transaction_policy_profile.encoded_profiles.push_back(
        "transaction_read_mode:read_only");
  }
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "DPC-025 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  context.read_only_mode = read_only;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request), "DPC-025 commit failed");
}

api::EngineRollbackTransactionResult RollbackResult(
    const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  return api::EngineRollbackTransaction(request);
}

void Rollback(const api::EngineRequestContext& context) {
  RequireOk(RollbackResult(context), "DPC-025 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "dpc025_delta_recovery_repair";
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
                ("scratchbird_dpc025_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc025.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DPC-025 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.non_unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "dpc025-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "DPC-025 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.non_unique_index_uuid,
                                "name", false)),
                      "DPC-025 non-unique index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.unique_index_uuid,
                                "id", true)),
                      "DPC-025 unique index metadata append failed");
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

std::string SeedCommittedRow(Fixture& fixture,
                             std::string id,
                             std::string name,
                             bool deferred) {
  auto context = Begin(fixture, "dpc025-seed");
  const auto inserted = InsertRow(fixture,
                                  context,
                                  std::move(id),
                                  std::move(name),
                                  deferred ? DeferredOptions()
                                           : std::vector<std::string>{});
  RequireOk(inserted, "DPC-025 seed insert failed");
  Require(inserted.row_uuids.size() == 1, "DPC-025 seed row UUID missing");
  const std::string row_uuid = inserted.row_uuids.front().canonical;
  Commit(context);
  return row_uuid;
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

std::filesystem::path LedgerPath(const Fixture& fixture) {
  return fixture.database_path.string() + ".sb.mga_secondary_index_delta_ledger";
}

idx::PersistentSecondaryIndexDeltaLedger LoadLedger(const Fixture& fixture) {
  const auto loaded = api::LoadMgaSecondaryIndexDeltaLedger(
      BaseContext(fixture, "dpc025-load-ledger"));
  Require(loaded.ok, "DPC-025 ledger load failed");
  return loaded.ledger;
}

void WriteLedger(const Fixture& fixture,
                 const idx::PersistentSecondaryIndexDeltaLedger& ledger) {
  const auto encoded = idx::EncodePersistentSecondaryIndexDeltaLedger(
      ledger,
      idx::SecondaryIndexDeltaLedgerLimits{1024, 1024 * 1024});
  Require(encoded.ok(), "DPC-025 ledger encode failed");
  std::ofstream out(LedgerPath(fixture), std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(encoded.bytes.data()),
            static_cast<std::streamsize>(encoded.bytes.size()));
  out.flush();
  Require(static_cast<bool>(out), "DPC-025 ledger write failed");
}

void CorruptLedgerImage(const Fixture& fixture) {
  std::ofstream out(LedgerPath(fixture), std::ios::binary | std::ios::trunc);
  out << "DPC025_CORRUPT_DELTA_LEDGER_IMAGE";
  out.flush();
  Require(static_cast<bool>(out), "DPC-025 corrupt ledger write failed");
}

api::MgaSecondaryIndexDeltaRecoveryRepairRequest RecoveryRequest(
    const Fixture& fixture,
    bool repair_enabled = false) {
  api::MgaSecondaryIndexDeltaRecoveryRepairRequest request;
  request.index_uuid = fixture.non_unique_index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.max_records_to_scan = 16;
  request.repair_enabled = repair_enabled;
  request.require_authoritative_base = true;
  return request;
}

api::MgaIndexedRowsLookupResult DirectIndexedLookup(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string column,
    std::string value) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "DPC-025 direct lookup state load failed");
  api::CrudState state = api::BuildCrudCompatibilityStateFromMga(loaded.state);
  return api::IndexedMgaRowsForPredicateForContext(
      state,
      fixture.table_uuid,
      EqualsPredicate(std::move(column), std::move(value)),
      context,
      0);
}

Fixture CopyFixtureSidecars(const Fixture& source, std::string name, platform::u64 salt) {
  Fixture restored;
  restored.salt = salt;
  restored.dir = std::filesystem::temp_directory_path() /
                 ("scratchbird_dpc025_" + name + "_" +
                  std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(restored.dir);
  restored.database_path = restored.dir / "dpc025_restored.sbdb";
  restored.database_uuid = source.database_uuid;
  restored.table_uuid = source.table_uuid;
  restored.non_unique_index_uuid = source.non_unique_index_uuid;
  restored.unique_index_uuid = source.unique_index_uuid;

  const std::string source_prefix = source.database_path.filename().string();
  const std::string restore_prefix = restored.database_path.filename().string();
  for (const auto& entry : std::filesystem::directory_iterator(source.dir)) {
    if (!entry.is_regular_file()) { continue; }
    const std::string filename = entry.path().filename().string();
    if (filename.rfind(source_prefix, 0) != 0) { continue; }
    const auto target = restored.dir /
        (restore_prefix + filename.substr(source_prefix.size()));
    std::filesystem::copy_file(entry.path(),
                               target,
                               std::filesystem::copy_options::overwrite_existing);
  }
  Require(std::filesystem::exists(restored.database_path),
          "DPC-025 restored database image missing");
  return restored;
}

void ValidatePrecommitReopenIgnoredAndRepairClassified() {
  auto fixture = MakeFixture("precommit_reopen", 1000);
  auto writer = Begin(fixture, "dpc025-precommit-writer");
  RequireOk(InsertRow(fixture, writer, "id-precommit", "alpha", DeferredOptions()),
            "DPC-025 precommit deferred insert failed");

  auto fresh_reader = Begin(fixture, "dpc025-precommit-fresh-reader");
  const auto selected = SelectEquals(fixture, fresh_reader, "name", "alpha");
  RequireOk(selected, "DPC-025 precommit fresh select failed");
  Require(selected.visible_count == 0,
          "DPC-025 precommit crash/reopen delta became visible");
  Require(!HasEvidence(selected.evidence, "mga_secondary_index_delta_overlay_used"),
          "DPC-025 precommit other transaction required visible overlay");
  Rollback(fresh_reader);

  const auto recovery = api::ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex(
      BaseContext(fixture, "dpc025-precommit-recovery"),
      RecoveryRequest(fixture, true));
  Require(recovery.ok, "DPC-025 precommit recovery validation refused");
  Require(!recovery.repaired && recovery.retained_count == 1,
          "DPC-025 active precommit delta was not retained under MGA authority");
  Require(recovery.recovery_class == "has_uncommitted_precommit_delta",
          "DPC-025 precommit recovery class changed");

  Rollback(writer);
}

void ValidateCommittedPremergeReopenOverlayOrRefusal() {
  auto fixture = MakeFixture("committed_premerge", 2000);
  (void)SeedCommittedRow(fixture, "id-delta", "alpha", true);

  auto fresh_reader = Begin(fixture, "dpc025-committed-reader");
  const auto selected = SelectEquals(fixture, fresh_reader, "name", "alpha");
  RequireOk(selected, "DPC-025 committed premerge select failed");
  Require(selected.visible_count == 1,
          "DPC-025 committed premerge delta was not visible after reopen");
  Require(HasEvidence(selected.evidence, "mga_secondary_index_delta_overlay_used"),
          "DPC-025 committed premerge lookup omitted overlay evidence");
  Rollback(fresh_reader);

  const auto recovery = api::ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex(
      BaseContext(fixture, "dpc025-committed-recovery"),
      RecoveryRequest(fixture, false));
  Require(recovery.ok, "DPC-025 committed premerge recovery validation refused");
  Require(recovery.committed_premerge_count == 1,
          "DPC-025 committed premerge recovery count changed");
  Require(recovery.recovery_class == "committed_premerge_requires_overlay_merge",
          "DPC-025 committed premerge recovery class changed");
}

void ValidateMergedCleanedReopenUsesBasePath() {
  auto fixture = MakeFixture("merged_cleaned", 3000);
  (void)SeedCommittedRow(fixture, "id-base", "alpha", false);
  (void)SeedCommittedRow(fixture, "id-delta", "alpha", true);
  const auto ledger = LoadLedger(fixture);
  platform::u64 horizon = 0;
  for (const auto& record : ledger.records) {
    horizon = std::max(horizon, record.delta.local_transaction_id);
  }
  auto merge_request = api::MgaSecondaryIndexDeltaMergeAgentRequest{};
  merge_request.index_uuid = fixture.non_unique_index_uuid;
  merge_request.table_uuid = fixture.table_uuid;
  merge_request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  merge_request.cleanup_horizon_authoritative = true;
  merge_request.max_records_to_scan = 16;
  merge_request.max_records_to_merge = 16;
  const auto merged = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "dpc025-merge"),
      merge_request);
  Require(merged.ok, "DPC-025 merge setup failed");

  auto fresh_reader = Begin(fixture, "dpc025-merged-reader");
  const auto selected = SelectEquals(fixture, fresh_reader, "name", "alpha");
  RequireOk(selected, "DPC-025 merged cleaned select failed");
  Require(selected.visible_count == 2,
          "DPC-025 merged cleaned base lookup lost rows");
  Require(!HasEvidence(selected.evidence, "mga_secondary_index_delta_overlay_used"),
          "DPC-025 merged cleaned lookup still used overlay");
  Require(EvidenceValue(selected.evidence, "mga_secondary_index_lookup_path") ==
              "non_unique_synchronous_no_delta",
          "DPC-025 merged cleaned lookup did not use base path");
  Rollback(fresh_reader);

  const auto recovery = api::ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex(
      BaseContext(fixture, "dpc025-merged-recovery"),
      RecoveryRequest(fixture, false));
  Require(recovery.ok, "DPC-025 merged cleaned recovery validation refused");
  Require(recovery.merged_cleaned_count == 1,
          "DPC-025 merged cleaned recovery count changed");
}

void ValidateRepairUsesMgaInventoryAuthority() {
  auto promoted_fixture = MakeFixture("promote_precommit", 4000);
  (void)SeedCommittedRow(promoted_fixture, "id-promote", "alpha", true);
  auto promoted_ledger = LoadLedger(promoted_fixture);
  Require(promoted_ledger.records.size() == 1,
          "DPC-025 promote fixture ledger size changed");
  promoted_ledger.records[0].commit_state =
      idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted;
  promoted_ledger.records[0].delta.committed = false;
  WriteLedger(promoted_fixture, promoted_ledger);

  const auto promoted = api::ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex(
      BaseContext(promoted_fixture, "dpc025-promote-repair"),
      RecoveryRequest(promoted_fixture, true));
  Require(promoted.ok && promoted.repaired && promoted.promoted_count == 1,
          "DPC-025 committed MGA inventory did not promote precommit delta");
  const auto promoted_after = LoadLedger(promoted_fixture);
  Require(promoted_after.records[0].commit_state ==
              idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge,
          "DPC-025 promoted ledger state was not committed_premerge");

  auto pruned_fixture = MakeFixture("prune_rolledback", 5000);
  auto writer = Begin(pruned_fixture, "dpc025-prune-writer");
  RequireOk(InsertRow(pruned_fixture, writer, "id-prune", "bravo", DeferredOptions()),
            "DPC-025 prune setup insert failed");
  const auto stale_ledger = LoadLedger(pruned_fixture);
  Rollback(writer);
  WriteLedger(pruned_fixture, stale_ledger);

  const auto pruned = api::ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex(
      BaseContext(pruned_fixture, "dpc025-prune-repair"),
      RecoveryRequest(pruned_fixture, true));
  Require(pruned.ok && pruned.repaired && pruned.removed_count == 1,
          "DPC-025 rolled-back MGA inventory did not prune precommit delta");
  Require(LoadLedger(pruned_fixture).records.empty(),
          "DPC-025 stale rolled-back delta remained after repair");
}

void ValidateCorruptRefusalAndReadOnlyRollbackBypass() {
  auto fixture = MakeFixture("corrupt_refusal", 6000);
  (void)SeedCommittedRow(fixture, "id-base", "alpha", false);
  CorruptLedgerImage(fixture);

  auto reader = Begin(fixture, "dpc025-corrupt-index-reader");
  const auto indexed = DirectIndexedLookup(fixture, reader, "name", "alpha");
  Require(indexed.index_refused,
          "DPC-025 corrupt ledger did not refuse visible index use");
  Require(indexed.diagnostic.code == "secondary_index_delta_ledger_corrupt_checksum",
          "DPC-025 corrupt ledger diagnostic changed");
  Require(EvidenceValue(indexed.evidence,
                        "mga_secondary_index_delta_overlay_refused_code") ==
              "secondary_index_delta_ledger_corrupt_checksum",
          "DPC-025 corrupt ledger refusal evidence changed");
  (void)reader;

  auto read_only = Begin(fixture, "dpc025-read-only-rollback", true);
  const auto rolled_back = RollbackResult(read_only);
  RequireOk(rolled_back,
            "DPC-025 read-only rollback was blocked by unrelated corrupt ledger");
  Require(EvidenceValue(rolled_back.evidence,
                        "read_only_rollback_delta_cleanup") ==
              "skipped_no_mutation_authority",
          "DPC-025 read-only rollback cleanup evidence missing");
}

void ValidateCopiedRestoredSidecarsPreserveOrFailClosed() {
  auto source = MakeFixture("backup_source", 7000);
  (void)SeedCommittedRow(source, "id-restored", "alpha", true);
  auto restored = CopyFixtureSidecars(source, "backup_restored", 7100);

  auto restored_reader = Begin(restored, "dpc025-restored-reader");
  const auto selected = SelectEquals(restored, restored_reader, "name", "alpha");
  RequireOk(selected, "DPC-025 restored sidecar select failed");
  Require(selected.visible_count == 1,
          "DPC-025 restored sidecar lost committed delta semantics");
  Require(HasEvidence(selected.evidence, "mga_secondary_index_delta_overlay_used"),
          "DPC-025 restored sidecar did not preserve overlay semantics");
  Rollback(restored_reader);

  CorruptLedgerImage(restored);
  auto fail_reader = Begin(restored, "dpc025-restored-corrupt-reader");
  const auto indexed = DirectIndexedLookup(restored, fail_reader, "name", "alpha");
  Require(indexed.index_refused,
          "DPC-025 restored corrupt sidecar did not fail closed for index use");
  Require(indexed.diagnostic.code == "secondary_index_delta_ledger_corrupt_checksum",
          "DPC-025 restored corrupt diagnostic changed");
  (void)fail_reader;
}

}  // namespace

int main() {
  Require(kRecoverySearchKey == "DPC_SECONDARY_INDEX_DELTA_RECOVERY_REPAIR",
          "DPC-025 recovery search key drifted");
  Require(kGateSearchKey == "DPC_SECONDARY_INDEX_DELTA_RECOVERY_REPAIR_GATE",
          "DPC-025 recovery gate search key drifted");

  ValidatePrecommitReopenIgnoredAndRepairClassified();
  ValidateCommittedPremergeReopenOverlayOrRefusal();
  ValidateMergedCleanedReopenUsesBasePath();
  ValidateRepairUsesMgaInventoryAuthority();
  ValidateCorruptRefusalAndReadOnlyRollbackBypass();
  ValidateCopiedRestoredSidecarsPreserveOrFailClosed();
  return EXIT_SUCCESS;
}
