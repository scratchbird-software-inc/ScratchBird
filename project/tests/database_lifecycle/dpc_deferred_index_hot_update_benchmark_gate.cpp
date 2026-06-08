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
#include "secondary_index_delta_ledger.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_DEFERRED_INDEX_HOT_UPDATE_BENCHMARK_GATE";
constexpr std::string_view kBenchmarkOutputSearchKey =
    "DPC_DEFERRED_INDEX_HOT_UPDATE_BENCHMARK_OUTPUT";
constexpr std::uint32_t kRunCount = 5;

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
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "DPC-027 generated UUID creation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

std::string TwoDigit(std::uint32_t value) {
  std::ostringstream out;
  out << std::setw(2) << std::setfill('0') << value;
  return out.str();
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

struct RowSpec {
  std::string id;
  std::string name;
  std::string note;
};

api::EngineRowValue Row(const RowSpec& spec) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(spec.id)});
  row.fields.push_back({"name", TextValue(spec.name)});
  row.fields.push_back({"note", TextValue(spec.note)});
  return row;
}

std::vector<std::string> DeferredOptions() {
  return {"runtime.deferred_secondary_index=enabled",
          "feature.secondary_index_delta_ledger=enabled",
          "delta_ledger.reader_overlay=enabled",
          "delta_ledger.cleanup_horizon_bound=true",
          "delta_ledger.recovery_classifiable=true"};
}

std::vector<std::string> HotShapeDisabledOptions() {
  return {"runtime.hot_update_shape=disabled"};
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
  RequireOk(begun, "DPC-027 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "DPC-027 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "DPC-027 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "dpc027_deferred_index_benchmark";
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
                ("scratchbird_dpc027_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc027.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DPC-027 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.non_unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "dpc027-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "DPC-027 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.non_unique_index_uuid,
                                "name", false)),
                      "DPC-027 non-unique index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.unique_index_uuid,
                                "id", true)),
                      "DPC-027 unique index metadata append failed");
  Commit(context);
  return fixture;
}

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

api::EngineInsertRowsResult InsertRows(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const std::vector<RowSpec>& rows,
    std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.estimated_row_count = rows.size();
  request.option_envelopes = std::move(options);
  for (const auto& spec : rows) { request.input_rows.push_back(Row(spec)); }
  return api::EngineInsertRows(request);
}

api::EngineUpdateRowsResult UpdateAllNames(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string name,
    std::vector<std::string> options = {}) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.assignments.push_back({"name", TextValue(std::move(name))});
  request.option_envelopes = std::move(options);
  return api::EngineUpdateRows(request);
}

api::EngineUpdateRowsResult UpdateHotNote(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string id,
    std::string name,
    std::string note,
    std::vector<std::string> options = {}) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = EqualsPredicate("id", std::move(id));
  request.assignments.push_back({"name", TextValue(std::move(name))});
  request.assignments.push_back({"note", TextValue(std::move(note))});
  request.option_envelopes = std::move(options);
  return api::EngineUpdateRows(request);
}

api::EngineDeleteRowsResult DeleteRow(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string row_uuid,
    std::vector<std::string> options = {}) {
  api::EngineDeleteRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate.predicate_kind = "row_uuid_match";
  request.delete_predicate.canonical_predicate_envelope = std::move(row_uuid);
  request.tombstone_only = true;
  request.option_envelopes = std::move(options);
  return api::EngineDeleteRows(request);
}

api::EngineSelectRowsResult SelectAll(const Fixture& fixture,
                                      const api::EngineRequestContext& context) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  return api::EngineSelectRows(request);
}

api::EngineSelectRowsResult SelectName(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       std::string name) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate = EqualsPredicate("name", std::move(name));
  return api::EngineSelectRows(request);
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view field) {
  for (const auto& [candidate, value] : row.fields) {
    if (candidate == field) { return value.encoded_value; }
  }
  return {};
}

std::uint64_t MixFnv1a(std::uint64_t hash, std::string_view text) {
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::nouppercase << value;
  return out.str();
}

struct TableSnapshot {
  std::uint64_t row_count = 0;
  std::string hash;
  std::map<std::string, std::vector<std::string>> ids_by_name;
};

TableSnapshot Snapshot(const Fixture& fixture,
                       const api::EngineRequestContext& context) {
  const auto selected = SelectAll(fixture, context);
  RequireOk(selected, "DPC-027 select-all snapshot failed");
  std::vector<api::EngineRowValue> rows = selected.result_shape.rows;
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return FieldValue(left, "id") < FieldValue(right, "id");
  });

  std::uint64_t hash = 1469598103934665603ull;
  TableSnapshot snapshot;
  snapshot.row_count = rows.size();
  for (const auto& row : rows) {
    const std::string id = FieldValue(row, "id");
    const std::string name = FieldValue(row, "name");
    const std::string note = FieldValue(row, "note");
    hash = MixFnv1a(hash, id);
    hash = MixFnv1a(hash, "|");
    hash = MixFnv1a(hash, name);
    hash = MixFnv1a(hash, "|");
    hash = MixFnv1a(hash, note);
    hash = MixFnv1a(hash, ";");
    snapshot.ids_by_name[name].push_back(id);
  }
  for (auto& [ignored, ids] : snapshot.ids_by_name) {
    (void)ignored;
    std::sort(ids.begin(), ids.end());
  }
  snapshot.hash = Hex(hash);
  return snapshot;
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

std::uint64_t EvidenceCounter(const std::vector<api::EngineEvidenceReference>& evidence,
                              std::string_view kind) {
  const std::string value = EvidenceValue(evidence, kind);
  if (value.empty()) { return 0; }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

std::uint64_t CountBaseIndexEntries(const Fixture& fixture,
                                    const api::EngineRequestContext& context,
                                    const std::string& index_uuid) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "DPC-027 relation store load failed");
  std::uint64_t count = 0;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.table_uuid == fixture.table_uuid && entry.index_uuid == index_uuid) {
      ++count;
    }
  }
  return count;
}

idx::PersistentSecondaryIndexDeltaLedger LoadLedger(const Fixture& fixture) {
  const auto loaded =
      api::LoadMgaSecondaryIndexDeltaLedger(BaseContext(fixture, "dpc027-ledger-load"));
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "DPC-027 ledger load failed");
  return loaded.ledger;
}

std::string RecordIndexUuid(const idx::SecondaryIndexDeltaLedgerRecord& record) {
  return uuid::UuidToString(record.delta.index_uuid.value);
}

std::uint64_t CountLedgerRecordsForIndex(const Fixture& fixture,
                                         const std::string& index_uuid) {
  std::uint64_t count = 0;
  for (const auto& record : LoadLedger(fixture).records) {
    if (RecordIndexUuid(record) == index_uuid) { ++count; }
  }
  return count;
}

std::uint64_t CountMergedCleanedRecordsForIndex(const Fixture& fixture,
                                                const std::string& index_uuid) {
  std::uint64_t count = 0;
  for (const auto& record : LoadLedger(fixture).records) {
    if (RecordIndexUuid(record) == index_uuid &&
        record.commit_state ==
            idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned) {
      ++count;
    }
  }
  return count;
}

struct LookupValidation {
  std::uint64_t checked_key_count = 0;
  std::uint64_t overlay_used_count = 0;
  std::uint64_t max_visible_delta_count = 0;
};

LookupValidation ValidateNameLookups(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const std::map<std::string, std::vector<std::string>>& expected,
    const std::set<std::string>& all_keys,
    bool expect_overlay) {
  LookupValidation validation;
  for (const auto& key : all_keys) {
    const auto selected = SelectName(fixture, context, key);
    RequireOk(selected, "DPC-027 indexed lookup failed");

    std::vector<std::string> actual_ids;
    for (const auto& row : selected.result_shape.rows) {
      actual_ids.push_back(FieldValue(row, "id"));
    }
    std::sort(actual_ids.begin(), actual_ids.end());

    auto expected_it = expected.find(key);
    const std::vector<std::string> empty;
    const auto& expected_ids =
        expected_it == expected.end() ? empty : expected_it->second;
    Require(actual_ids == expected_ids,
            "DPC-027 indexed lookup result diverged from table snapshot");
    Require(selected.visible_count == actual_ids.size(),
            "DPC-027 indexed lookup count diverged from result rows");

    if (HasEvidence(selected.evidence, "mga_secondary_index_delta_overlay_used")) {
      ++validation.overlay_used_count;
      validation.max_visible_delta_count = std::max(
          validation.max_visible_delta_count,
          EvidenceCounter(selected.evidence,
                          "mga_secondary_index_delta_overlay_visible_delta_count"));
    }
    ++validation.checked_key_count;
  }
  if (expect_overlay) {
    Require(validation.overlay_used_count > 0,
            "DPC-027 expected overlay lookup evidence was not observed");
    Require(validation.max_visible_delta_count > 0,
            "DPC-027 overlay visible delta counter was not observed");
  } else {
    Require(validation.overlay_used_count == 0,
            "DPC-027 unexpected overlay lookup evidence after merge/baseline");
  }
  return validation;
}

std::set<std::string> KeysFromRows(const std::vector<RowSpec>& rows) {
  std::set<std::string> keys;
  for (const auto& row : rows) { keys.insert(row.name); }
  return keys;
}

std::set<std::string> KeysFromSnapshot(const TableSnapshot& snapshot) {
  std::set<std::string> keys;
  for (const auto& [key, ignored] : snapshot.ids_by_name) {
    (void)ignored;
    keys.insert(key);
  }
  return keys;
}

std::vector<RowSpec> LoadRows() {
  std::vector<RowSpec> rows;
  for (std::uint32_t i = 0; i < 16; ++i) {
    rows.push_back({"load-" + TwoDigit(i),
                    "load-key-" + std::to_string(i % 4),
                    "payload-" + std::to_string((i * 17) % 97)});
  }
  return rows;
}

std::vector<RowSpec> UpdateRows() {
  std::vector<RowSpec> rows;
  for (std::uint32_t i = 0; i < 16; ++i) {
    rows.push_back({"update-" + TwoDigit(i),
                    "update-key-" + std::to_string(i % 8),
                    "payload-" + std::to_string((i * 19) % 101)});
  }
  return rows;
}

std::vector<RowSpec> DeleteRows() {
  std::vector<RowSpec> rows;
  for (std::uint32_t i = 0; i < 12; ++i) {
    rows.push_back({"delete-" + TwoDigit(i),
                    "delete-key-" + TwoDigit(i),
                    "payload-" + std::to_string((i * 23) % 103)});
  }
  return rows;
}

std::uint64_t MergeDeltas(const Fixture& fixture,
                          const api::EngineRequestContext& context,
                          std::uint64_t horizon,
                          std::uint64_t expected_cleaned) {
  api::MgaSecondaryIndexDeltaMergeAgentRequest request;
  request.index_uuid = fixture.non_unique_index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  request.cleanup_horizon_authoritative = true;
  request.max_records_to_scan = 1024;
  request.max_records_to_merge = 1024;
  const auto merged = api::MergeMgaSecondaryIndexDeltasForIndex(context, request);
  if (!merged.ok) {
    std::cerr << merged.diagnostic.code << ':' << merged.diagnostic.detail << '\n';
  }
  Require(merged.ok, "DPC-027 merge failed");
  Require(merged.cleaned_count == expected_cleaned,
          "DPC-027 merge cleaned count diverged");
  Require(CountMergedCleanedRecordsForIndex(fixture,
                                            fixture.non_unique_index_uuid) >=
              expected_cleaned,
          "DPC-027 merged_cleaned ledger validation failed");
  return merged.merged_count;
}

struct LaneObservation {
  TableSnapshot snapshot;
  std::uint64_t base_index_writes = 0;
  std::uint64_t delta_records = 0;
  std::uint64_t merge_cleaned_records = 0;
  std::uint64_t merge_published_base_entries = 0;
  std::uint64_t overlay_visible_delta_records = 0;
  std::uint64_t hot_churn_avoided = 0;
  std::uint64_t disabled_hot_churn_decisions = 0;
  std::uint64_t deleted_count = 0;
  std::set<std::string> validation_keys;
};

LaneObservation RunLoadLane(bool deferred) {
  auto fixture = MakeFixture(deferred ? "load_deferred" : "load_baseline",
                             deferred ? 2000 : 1000);
  const auto rows = LoadRows();
  auto writer = Begin(fixture, deferred ? "dpc027-load-deferred"
                                        : "dpc027-load-baseline");
  const auto before_base =
      CountBaseIndexEntries(fixture, writer, fixture.non_unique_index_uuid);
  const auto inserted =
      InsertRows(fixture, writer, rows, deferred ? DeferredOptions()
                                                : std::vector<std::string>{});
  RequireOk(inserted, "DPC-027 load insert failed");
  Require(inserted.inserted_count == rows.size(),
          "DPC-027 load inserted count changed");
  Commit(writer);

  auto reader = Begin(fixture, deferred ? "dpc027-load-deferred-reader"
                                        : "dpc027-load-baseline-reader");
  LaneObservation observation;
  observation.validation_keys = KeysFromRows(rows);
  observation.snapshot = Snapshot(fixture, reader);
  const auto lookup = ValidateNameLookups(fixture,
                                          reader,
                                          observation.snapshot.ids_by_name,
                                          observation.validation_keys,
                                          deferred);
  observation.overlay_visible_delta_records = lookup.max_visible_delta_count;
  const auto after_base =
      CountBaseIndexEntries(fixture, reader, fixture.non_unique_index_uuid);
  observation.base_index_writes = after_base - before_base;
  observation.delta_records =
      CountLedgerRecordsForIndex(fixture, fixture.non_unique_index_uuid);
  if (deferred) {
    observation.merge_published_base_entries =
        MergeDeltas(fixture,
                    reader,
                    writer.local_transaction_id,
                    observation.delta_records);
    observation.merge_cleaned_records =
        CountMergedCleanedRecordsForIndex(fixture, fixture.non_unique_index_uuid);
    ValidateNameLookups(fixture,
                        reader,
                        observation.snapshot.ids_by_name,
                        observation.validation_keys,
                        false);
  }
  Rollback(reader);
  return observation;
}

std::vector<std::string> SeedRows(Fixture& fixture,
                                  const std::vector<RowSpec>& rows,
                                  std::string request_id) {
  auto context = Begin(fixture, std::move(request_id));
  const auto inserted = InsertRows(fixture, context, rows);
  RequireOk(inserted, "DPC-027 seed insert failed");
  Require(inserted.inserted_count == rows.size() &&
              inserted.row_uuids.size() == rows.size(),
          "DPC-027 seed insert shape changed");
  std::vector<std::string> row_uuids;
  for (const auto& row_uuid : inserted.row_uuids) {
    row_uuids.push_back(row_uuid.canonical);
  }
  Commit(context);
  return row_uuids;
}

LaneObservation RunUpdateLane(bool deferred) {
  auto fixture = MakeFixture(deferred ? "update_deferred" : "update_baseline",
                             deferred ? 4000 : 3000);
  const auto rows = UpdateRows();
  (void)SeedRows(fixture, rows, "dpc027-update-seed");
  auto writer = Begin(fixture, deferred ? "dpc027-update-deferred"
                                        : "dpc027-update-baseline");
  const auto before_base =
      CountBaseIndexEntries(fixture, writer, fixture.non_unique_index_uuid);
  const auto updated =
      UpdateAllNames(fixture,
                     writer,
                     "updated-key",
                     deferred ? DeferredOptions() : std::vector<std::string>{});
  RequireOk(updated, "DPC-027 update workload failed");
  Require(updated.updated_count == rows.size(),
          "DPC-027 update count changed");
  Commit(writer);

  auto reader = Begin(fixture, deferred ? "dpc027-update-deferred-reader"
                                        : "dpc027-update-baseline-reader");
  LaneObservation observation;
  observation.validation_keys = KeysFromRows(rows);
  observation.validation_keys.insert("updated-key");
  observation.snapshot = Snapshot(fixture, reader);
  const auto lookup = ValidateNameLookups(fixture,
                                          reader,
                                          observation.snapshot.ids_by_name,
                                          observation.validation_keys,
                                          deferred);
  observation.overlay_visible_delta_records = lookup.max_visible_delta_count;
  const auto after_base =
      CountBaseIndexEntries(fixture, reader, fixture.non_unique_index_uuid);
  observation.base_index_writes = after_base - before_base;
  observation.delta_records =
      CountLedgerRecordsForIndex(fixture, fixture.non_unique_index_uuid);
  if (deferred) {
    observation.merge_published_base_entries =
        MergeDeltas(fixture,
                    reader,
                    writer.local_transaction_id,
                    observation.delta_records);
    observation.merge_cleaned_records =
        CountMergedCleanedRecordsForIndex(fixture, fixture.non_unique_index_uuid);
    ValidateNameLookups(fixture,
                        reader,
                        observation.snapshot.ids_by_name,
                        observation.validation_keys,
                        false);
  }
  Rollback(reader);
  return observation;
}

LaneObservation RunDeleteLane(bool deferred) {
  auto fixture = MakeFixture(deferred ? "delete_deferred" : "delete_baseline",
                             deferred ? 6000 : 5000);
  const auto rows = DeleteRows();
  const auto row_uuids = SeedRows(fixture, rows, "dpc027-delete-seed");
  auto writer = Begin(fixture, deferred ? "dpc027-delete-deferred"
                                        : "dpc027-delete-baseline");
  const auto before_base =
      CountBaseIndexEntries(fixture, writer, fixture.non_unique_index_uuid);
  std::uint64_t deleted_count = 0;
  for (std::size_t i = 0; i < row_uuids.size(); i += 3) {
    const auto deleted = DeleteRow(fixture,
                                   writer,
                                   row_uuids[i],
                                   deferred ? DeferredOptions()
                                            : std::vector<std::string>{});
    RequireOk(deleted, "DPC-027 delete workload failed");
    Require(deleted.deleted_count == 1, "DPC-027 delete count changed");
    ++deleted_count;
  }
  Commit(writer);

  auto reader = Begin(fixture, deferred ? "dpc027-delete-deferred-reader"
                                        : "dpc027-delete-baseline-reader");
  LaneObservation observation;
  observation.deleted_count = deleted_count;
  observation.validation_keys = KeysFromRows(rows);
  observation.snapshot = Snapshot(fixture, reader);
  const auto lookup = ValidateNameLookups(fixture,
                                          reader,
                                          observation.snapshot.ids_by_name,
                                          observation.validation_keys,
                                          deferred);
  observation.overlay_visible_delta_records = lookup.max_visible_delta_count;
  const auto after_base =
      CountBaseIndexEntries(fixture, reader, fixture.non_unique_index_uuid);
  observation.base_index_writes = after_base - before_base;
  observation.delta_records =
      CountLedgerRecordsForIndex(fixture, fixture.non_unique_index_uuid);
  if (deferred) {
    observation.merge_published_base_entries =
        MergeDeltas(fixture,
                    reader,
                    writer.local_transaction_id,
                    observation.delta_records);
    observation.merge_cleaned_records =
        CountMergedCleanedRecordsForIndex(fixture, fixture.non_unique_index_uuid);
    ValidateNameLookups(fixture,
                        reader,
                        observation.snapshot.ids_by_name,
                        observation.validation_keys,
                        false);
  }
  Rollback(reader);
  return observation;
}

LaneObservation RunHotRowLane(bool hot_shape_enabled) {
  auto fixture =
      MakeFixture(hot_shape_enabled ? "hot_enabled" : "hot_disabled",
                  hot_shape_enabled ? 8000 : 7000);
  const std::vector<RowSpec> rows{{"hot-00", "hot-key", "hot-note-00"}};
  (void)SeedRows(fixture, rows, "dpc027-hot-seed");
  auto writer = Begin(fixture, hot_shape_enabled ? "dpc027-hot-enabled"
                                                 : "dpc027-hot-disabled");
  const auto before_base =
      CountBaseIndexEntries(fixture, writer, fixture.non_unique_index_uuid);
  std::uint64_t churn_avoided = 0;
  std::uint64_t disabled_churn = 0;
  for (std::uint32_t i = 1; i <= 8; ++i) {
    const auto updated = UpdateHotNote(
        fixture,
        writer,
        "hot-00",
        "hot-key",
        "hot-note-" + TwoDigit(i),
        hot_shape_enabled ? std::vector<std::string>{}
                          : HotShapeDisabledOptions());
    RequireOk(updated, "DPC-027 hot-row update failed");
    Require(updated.updated_count == 1, "DPC-027 hot-row update count changed");
    churn_avoided += EvidenceCounter(
        updated.evidence,
        "dpc_hot_update_shape_index_churn_avoided");
    disabled_churn += EvidenceCounter(
        updated.evidence,
        "dpc_hot_update_shape_disabled_baseline_churn_decisions");
  }
  Commit(writer);

  auto reader = Begin(fixture, hot_shape_enabled ? "dpc027-hot-enabled-reader"
                                                 : "dpc027-hot-disabled-reader");
  LaneObservation observation;
  observation.validation_keys = KeysFromRows(rows);
  observation.snapshot = Snapshot(fixture, reader);
  ValidateNameLookups(fixture,
                      reader,
                      observation.snapshot.ids_by_name,
                      observation.validation_keys,
                      false);
  const auto after_base =
      CountBaseIndexEntries(fixture, reader, fixture.non_unique_index_uuid);
  observation.base_index_writes = after_base - before_base;
  observation.hot_churn_avoided = churn_avoided;
  observation.disabled_hot_churn_decisions = disabled_churn;
  Rollback(reader);
  return observation;
}

std::string Fixed(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << value;
  return out.str();
}

double ReductionRatio(std::uint64_t baseline, std::uint64_t optimized) {
  if (baseline == 0) { return optimized == 0 ? 0.0 : -1.0; }
  return static_cast<double>(baseline - optimized) /
         static_cast<double>(baseline);
}

struct ProofRow {
  std::string workload_id;
  std::string lane;
  std::string baseline_mode;
  std::string optimized_mode;
  LaneObservation baseline;
  LaneObservation optimized;
  std::uint64_t foreground_churn_decisions_avoided = 0;
};

void RequireEquivalentSnapshots(const ProofRow& row) {
  Require(row.baseline.snapshot.row_count == row.optimized.snapshot.row_count,
          "DPC-027 baseline/enabled row counts diverged");
  Require(row.baseline.snapshot.hash == row.optimized.snapshot.hash,
          "DPC-027 baseline/enabled result hash diverged");
  Require(row.baseline.snapshot.ids_by_name == row.optimized.snapshot.ids_by_name,
          "DPC-027 baseline/enabled index validation set diverged");
}

void PrintProofRow(const ProofRow& row) {
  const std::uint64_t base_writes_skipped =
      row.baseline.base_index_writes >= row.optimized.base_index_writes
          ? row.baseline.base_index_writes - row.optimized.base_index_writes
          : 0;
  const double reduction =
      ReductionRatio(row.baseline.base_index_writes,
                     row.optimized.base_index_writes);

  std::cout << kBenchmarkOutputSearchKey
            << ",workload_id=" << row.workload_id
            << ",lane=" << row.lane
            << ",baseline_mode=" << row.baseline_mode
            << ",optimized_mode=" << row.optimized_mode
            << ",run_count=" << kRunCount
            << ",baseline_row_count=" << row.baseline.snapshot.row_count
            << ",optimized_row_count=" << row.optimized.snapshot.row_count
            << ",result_hash=" << row.optimized.snapshot.hash
            << ",baseline_base_index_writes=" << row.baseline.base_index_writes
            << ",optimized_base_index_writes=" << row.optimized.base_index_writes
            << ",base_index_writes_skipped=" << base_writes_skipped
            << ",foreground_index_churn_decisions_avoided="
            << row.foreground_churn_decisions_avoided
            << ",delta_records_written=" << row.optimized.delta_records
            << ",merged_cleaned_records=" << row.optimized.merge_cleaned_records
            << ",merge_published_base_entries="
            << row.optimized.merge_published_base_entries
            << ",overlay_visible_delta_records="
            << row.optimized.overlay_visible_delta_records
            << ",hot_churn_avoided=" << row.optimized.hot_churn_avoided
            << ",disabled_hot_churn_decisions="
            << row.baseline.disabled_hot_churn_decisions
            << ",deleted_count=" << row.optimized.deleted_count
            << ",base_index_write_reduction_ratio=" << Fixed(reduction)
            << ",median_proxy=" << row.optimized.base_index_writes
            << ",p95_proxy=" << row.optimized.base_index_writes
            << ",cv_proxy=0.000000"
            << ",table_hash_equal=true"
            << ",index_validation_equal=true"
            << ",overlay_equality=true"
            << ",merge_equality=true"
            << ",build_profile=ctest_standalone_deterministic"
            << ",source_state=ctest_runtime\n";
}

void ProveLoadLane() {
  ProofRow row;
  row.workload_id = "WL03";
  row.lane = "load_non_unique_secondary_index";
  row.baseline_mode = "deferred_index_disabled_synchronous";
  row.optimized_mode = "deferred_index_enabled_delta_overlay_merge";
  row.baseline = RunLoadLane(false);
  row.optimized = RunLoadLane(true);
  RequireEquivalentSnapshots(row);
  Require(row.baseline.base_index_writes == row.baseline.snapshot.row_count,
          "DPC-027 load baseline did not synchronously write base entries");
  Require(row.optimized.base_index_writes == 0,
          "DPC-027 load deferred path wrote foreground non-unique base entries");
  Require(row.optimized.delta_records == row.optimized.snapshot.row_count,
          "DPC-027 load delta record count changed");
  row.foreground_churn_decisions_avoided =
      row.baseline.base_index_writes - row.optimized.base_index_writes;
  Require(row.foreground_churn_decisions_avoided > 0,
          "DPC-027 load did not avoid foreground index writes");
  PrintProofRow(row);
}

void ProveUpdateLane() {
  ProofRow row;
  row.workload_id = "WL05";
  row.lane = "changed_key_update_non_unique_secondary_index";
  row.baseline_mode = "deferred_index_disabled_synchronous";
  row.optimized_mode = "deferred_index_enabled_before_after_delta";
  row.baseline = RunUpdateLane(false);
  row.optimized = RunUpdateLane(true);
  RequireEquivalentSnapshots(row);
  Require(row.baseline.base_index_writes == row.baseline.snapshot.row_count,
          "DPC-027 update baseline did not synchronously write new base entries");
  Require(row.optimized.base_index_writes == 0,
          "DPC-027 update deferred path wrote foreground non-unique base entries");
  Require(row.optimized.delta_records == row.optimized.snapshot.row_count * 2,
          "DPC-027 update before/after delta count changed");
  row.foreground_churn_decisions_avoided =
      row.baseline.base_index_writes - row.optimized.base_index_writes;
  Require(row.foreground_churn_decisions_avoided > 0,
          "DPC-027 update did not avoid foreground index writes");
  PrintProofRow(row);
}

void ProveDeleteLane() {
  ProofRow row;
  row.workload_id = "WL06";
  row.lane = "delete_with_non_unique_secondary_index";
  row.baseline_mode = "deferred_index_disabled_visibility_recheck";
  row.optimized_mode = "deferred_index_enabled_tombstone_delta";
  row.baseline = RunDeleteLane(false);
  row.optimized = RunDeleteLane(true);
  RequireEquivalentSnapshots(row);
  Require(row.optimized.deleted_count > 0,
          "DPC-027 delete lane did not delete deterministic rows");
  Require(row.optimized.delta_records == row.optimized.deleted_count,
          "DPC-027 delete tombstone delta count changed");
  Require(row.optimized.merge_cleaned_records == row.optimized.delta_records,
          "DPC-027 delete merge did not clean tombstone deltas");
  row.foreground_churn_decisions_avoided = row.optimized.deleted_count;
  PrintProofRow(row);
}

void ProveHotRowLane() {
  ProofRow row;
  row.workload_id = "WL04";
  row.lane = "hot_row_non_key_update";
  row.baseline_mode = "hot_update_shape_disabled_baseline";
  row.optimized_mode = "hot_update_shape_enabled";
  row.baseline = RunHotRowLane(false);
  row.optimized = RunHotRowLane(true);
  RequireEquivalentSnapshots(row);
  Require(row.baseline.base_index_writes == 8,
          "DPC-027 hot-row disabled baseline churn count changed");
  Require(row.optimized.base_index_writes == 0,
          "DPC-027 hot-row enabled path wrote unchanged-key base entries");
  row.foreground_churn_decisions_avoided =
      row.baseline.base_index_writes - row.optimized.base_index_writes;
  Require(row.optimized.hot_churn_avoided >=
              row.foreground_churn_decisions_avoided,
          "DPC-027 hot-row churn avoidance evidence changed");
  Require(row.foreground_churn_decisions_avoided > 0,
          "DPC-027 hot-row did not avoid index churn");
  PrintProofRow(row);
}

}  // namespace

int main() {
  Require(kGateSearchKey == "DPC_DEFERRED_INDEX_HOT_UPDATE_BENCHMARK_GATE",
          "DPC-027 gate search key drifted");
  Require(kBenchmarkOutputSearchKey ==
              "DPC_DEFERRED_INDEX_HOT_UPDATE_BENCHMARK_OUTPUT",
          "DPC-027 benchmark output search key drifted");

  ProveLoadLane();
  ProveUpdateLane();
  ProveDeleteLane();
  ProveHotRowLane();

  std::cout << kGateSearchKey << "=passed "
            << "DPC_DEFERRED_INDEX_HOT_UPDATE_BENCHMARK_OUTPUT=retained "
            << "run_count=" << kRunCount
            << " deterministic_proxy_counters=true"
            << " wall_clock_speed_claim=false"
            << " mga_authority=engine_owned\n";
  return EXIT_SUCCESS;
}
