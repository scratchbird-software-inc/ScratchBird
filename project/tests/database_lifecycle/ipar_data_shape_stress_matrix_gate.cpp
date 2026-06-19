// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/insert_physical_integration.hpp"
#include "filespace_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "overflow_persistence.hpp"
#include "page_selection.hpp"
#include "strict_bulk_load_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dml = scratchbird::engine::internal_api::dml;
namespace filespace = scratchbird::storage::filespace;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

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
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
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
      uuid::GenerateEngineIdentityV7(kind, 1917000000000ull + salt);
  Require(generated.ok(), "IPAR-P7-09 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

api::EngineTypedValue Value(std::string canonical_type, std::string encoded) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = std::move(canonical_type);
  typed.descriptor.encoded_descriptor =
      "canonical=" + typed.descriptor.canonical_type_name;
  typed.encoded_value = std::move(encoded);
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineTypedValue NullValue(std::string canonical_type) {
  auto typed = Value(std::move(canonical_type), "");
  typed.setState(api::EngineValueState::sql_null);
  return typed;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) { return true; }
  }
  return false;
}

bool HasEvidenceKind(const std::vector<api::EngineEvidenceReference>& evidence,
                     std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) { return true; }
  }
  return false;
}

api::EngineApiU64 EvidenceU64(
    const std::vector<api::EngineEvidenceReference>& evidence,
    std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind != kind) { continue; }
    try {
      return static_cast<api::EngineApiU64>(std::stoull(item.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::string table_uuid;
  std::string id_index_uuid;
  std::string sort_index_uuid;
  std::string tag_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    if (!dir.empty()) {
      std::error_code ignored;
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
  context.current_schema_uuid.canonical = fixture.schema_uuid;
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
  RequireOk(begun, "IPAR-P7-09 begin transaction failed");
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
  RequireOk(committed, "IPAR-P7-09 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  RequireOk(rolled_back, "IPAR-P7-09 rollback failed");
}

api::CrudTableRecord TableRecord(const Fixture& fixture,
                                 const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_data_shape_stress";
  table.columns.push_back({"id", "canonical=character;primary_key=true;not_null=true"});
  table.columns.push_back({"shape", "canonical=character;not_null=true"});
  table.columns.push_back({"seq_i64", "canonical=int64"});
  table.columns.push_back({"unsigned_u64", "canonical=uint64"});
  table.columns.push_back({"decimal_text", "canonical=decimal"});
  table.columns.push_back({"bool_flag", "canonical=boolean"});
  table.columns.push_back({"sort_key", "canonical=character"});
  table.columns.push_back({"tag", "canonical=character"});
  table.columns.push_back({"payload", "canonical=character"});
  table.columns.push_back({"nullable_note", "canonical=character"});
  return table;
}

api::CrudIndexRecord IndexRecord(const Fixture& fixture,
                                 const api::EngineRequestContext& context,
                                 std::string uuid_text,
                                 std::string name,
                                 std::string column,
                                 bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(uuid_text);
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column);
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = std::move(name);
  index.unique = unique;
  index.key_envelopes.push_back(index.column_name);
  if (unique) { index.key_envelopes.push_back("unique"); }
  return index;
}

Fixture MakeFixture(platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_data_shape_stress_" +
                 std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_data_shape_stress.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = 1917000000000ull + salt + 3;
  create.page_size = 8192;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR-P7-09 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);
  fixture.sort_index_uuid = NewUuidText(platform::UuidKind::object, salt + 13);
  fixture.tag_index_uuid = NewUuidText(platform::UuidKind::object, salt + 14);

  auto metadata = Begin(fixture, "ipar-p7-09-metadata");
  const auto table = api::AppendMgaTableMetadata(metadata, TableRecord(fixture, metadata));
  Require(!table.error, "IPAR-P7-09 table metadata append failed");
  const auto id_index = api::AppendMgaIndexMetadata(
      metadata,
      IndexRecord(fixture,
                  metadata,
                  fixture.id_index_uuid,
                  "ipar_data_shape_id_uidx",
                  "id",
                  true));
  Require(!id_index.error, "IPAR-P7-09 id index metadata append failed");
  const auto sort_index = api::AppendMgaIndexMetadata(
      metadata,
      IndexRecord(fixture,
                  metadata,
                  fixture.sort_index_uuid,
                  "ipar_data_shape_sort_idx",
                  "sort_key",
                  false));
  Require(!sort_index.error, "IPAR-P7-09 sort index metadata append failed");
  const auto tag_index = api::AppendMgaIndexMetadata(
      metadata,
      IndexRecord(fixture,
                  metadata,
                  fixture.tag_index_uuid,
                  "ipar_data_shape_tag_idx",
                  "tag",
                  false));
  Require(!tag_index.error, "IPAR-P7-09 tag index metadata append failed");
  Commit(metadata);
  return fixture;
}

std::string FixedWidth(platform::u64 value, std::size_t width) {
  std::string text = std::to_string(value);
  if (text.size() < width) { text.insert(text.begin(), width - text.size(), '0'); }
  return text;
}

api::EngineRowValue Row(std::string id,
                        std::string shape,
                        platform::u64 ordinal,
                        std::string sort_key,
                        std::string tag,
                        std::string payload,
                        bool nullable_note_is_null) {
  api::EngineRowValue row;
  row.fields.push_back({"id", Value("character", std::move(id))});
  row.fields.push_back({"shape", Value("character", std::move(shape))});
  row.fields.push_back({"seq_i64", Value("int64", std::to_string(static_cast<std::int64_t>(ordinal)))});
  row.fields.push_back({"unsigned_u64", Value("uint64", std::to_string(ordinal * 17 + 11))});
  row.fields.push_back({"decimal_text", Value("decimal", std::to_string(ordinal) + ".125")});
  row.fields.push_back({"bool_flag", Value("boolean", (ordinal % 2 == 0) ? "true" : "false")});
  row.fields.push_back({"sort_key", Value("character", std::move(sort_key))});
  row.fields.push_back({"tag", Value("character", std::move(tag))});
  row.fields.push_back({"payload", Value("character", std::move(payload))});
  row.fields.push_back({"nullable_note",
                        nullable_note_is_null
                            ? NullValue("character")
                            : Value("character", "note-" + std::to_string(ordinal))});
  return row;
}

std::vector<api::EngineRowValue> BuildShapeRows(std::string_view shape,
                                                platform::u64 start,
                                                std::size_t count,
                                                std::size_t payload_bytes) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(count);
  platform::u64 random_state = 0x9e3779b97f4a7c15ull ^ start;
  for (std::size_t index = 0; index < count; ++index) {
    random_state ^= random_state << 7;
    random_state ^= random_state >> 9;
    random_state ^= random_state << 8;
    const platform::u64 ordinal = start + static_cast<platform::u64>(index);
    std::string sort_key;
    std::string tag;
    std::size_t payload_size = payload_bytes;
    bool note_null = false;
    if (shape == "sequential") {
      sort_key = "seq-" + FixedWidth(index, 6);
      tag = "seq-" + std::to_string(index % 16);
    } else if (shape == "random") {
      sort_key = "rnd-" + FixedWidth(random_state % 1000000, 6);
      tag = "rnd-" + std::to_string(random_state % 31);
    } else if (shape == "hot_range") {
      sort_key = "hot-" + FixedWidth(index % 8, 3);
      tag = "hot-range";
    } else if (shape == "duplicate_heavy") {
      sort_key = "dup-" + FixedWidth(index % 4, 3);
      tag = "same-tag";
    } else if (shape == "null_heavy") {
      sort_key = "null-" + FixedWidth(index % 9, 3);
      tag = (index % 3 == 0) ? "nullable-a" : "nullable-b";
      note_null = index % 2 == 0;
      payload_size = (index % 4 == 0) ? 0 : payload_bytes;
    } else {
      sort_key = std::string(shape) + "-" + FixedWidth(index, 5);
      tag = std::string(shape) + "-" + std::to_string(index % 11);
    }
    rows.push_back(Row(std::string(shape) + "-" + FixedWidth(ordinal, 8),
                       std::string(shape),
                       ordinal,
                       std::move(sort_key),
                       std::move(tag),
                       std::string(payload_size,
                                   static_cast<char>('A' + (ordinal % 26))),
                       note_null));
  }
  return rows;
}

std::vector<std::string> DemandOptions() {
  return {"page_allocation.runtime=enabled",
          "dml_demand_hints=enabled",
          "dml_demand_hints.max_pages=512",
          "dml_demand_hints.available_capacity_pages=512",
          "page_allocation.row_pages_per_mutation=128",
          "page_allocation.index_pages_per_mutation=128",
          "page_allocation.preallocate_index_pages=128"};
}

api::EngineInsertRowsRequest InsertRequest(
    const Fixture& fixture,
    api::EngineRequestContext context,
    std::string request_id,
    std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = std::move(context);
  request.context.request_id = std::move(request_id);
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id =
      request.context.catalog_generation_id;
  request.bound_object_identity.security_epoch = request.context.security_epoch;
  request.bound_object_identity.resource_epoch = request.context.resource_epoch;
  request.estimated_row_count = static_cast<api::EngineApiU64>(rows.size());
  request.input_rows = std::move(rows);
  request.option_envelopes = DemandOptions();
  return request;
}

api::EngineExecuteImportRowsRequest CopyRequest(
    const Fixture& fixture,
    api::EngineRequestContext context,
    std::string request_id,
    std::vector<api::EngineRowValue> rows) {
  api::EngineExecuteImportRowsRequest request;
  request.context = std::move(context);
  request.context.request_id = std::move(request_id);
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = DemandOptions();
  request.option_envelopes.push_back("copy_append_batching=enabled");
  request.option_envelopes.push_back("copy_append_batch_rows=16");
  return request;
}

void VerifyDmlEvidence(const api::EngineApiResult& result,
                       bool require_large_value) {
  Require(result.dml_summary.rows_changed > 0,
          "IPAR-P7-09 DML summary did not record changed rows");
  Require(result.dml_summary.index_probes > 0,
          "IPAR-P7-09 indexed shape did not record index probes");
  Require(result.dml_summary.preallocation_granted_pages > 0,
          "IPAR-P7-09 shape did not receive preallocated pages");
  Require(HasEvidence(result.evidence,
                      "dml_demand_hint_decision",
                      "accepted"),
          "IPAR-P7-09 demand hint was not accepted");
  if (require_large_value) {
    Require(HasEvidence(result.evidence,
                        "mga_large_value_batch_writer",
                        "window"),
            "IPAR-P7-09 large-value shape did not use batch writer");
    Require(EvidenceU64(result.evidence,
                        "insert_large_value_batch_chunks") > 0,
            "IPAR-P7-09 large-value shape emitted no chunk evidence");
  }
}

void InsertShape(Fixture& fixture,
                 std::string_view shape,
                 platform::u64 start,
                 std::size_t count,
                 std::size_t payload_bytes,
                 bool require_large_value,
                 api::EngineApiU64* expected_rows) {
  auto context = Begin(fixture, "ipar-p7-09-" + std::string(shape));
  auto request = InsertRequest(fixture,
                               context,
                               "ipar-p7-09-insert-" + std::string(shape),
                               BuildShapeRows(shape, start, count, payload_bytes));
  const auto inserted = api::EngineInsertRows(request);
  RequireOk(inserted, "IPAR-P7-09 shape insert failed");
  Require(inserted.inserted_count == count,
          "IPAR-P7-09 shape inserted row count mismatch");
  VerifyDmlEvidence(inserted, require_large_value);
  Commit(context);
  *expected_rows += static_cast<api::EngineApiU64>(count);
}

void CopyShape(Fixture& fixture,
               std::string_view shape,
               platform::u64 start,
               std::size_t count,
               api::EngineApiU64* expected_rows) {
  auto context = Begin(fixture, "ipar-p7-09-copy-" + std::string(shape));
  const auto copied = api::EngineExecuteImportRows(CopyRequest(
      fixture,
      context,
      "ipar-p7-09-copy-request-" + std::string(shape),
      BuildShapeRows(shape, start, count, 96)));
  RequireOk(copied, "IPAR-P7-09 COPY shape failed");
  Require(copied.accepted_rows == count && copied.inserted_rows == count,
          "IPAR-P7-09 COPY shape row count mismatch");
  Require(HasEvidence(copied.evidence, "import_execution", "direct_physical"),
          "IPAR-P7-09 COPY shape did not use direct physical lane");
  VerifyDmlEvidence(copied, false);
  Commit(context);
  *expected_rows += static_cast<api::EngineApiU64>(count);
}

void VerifyDuplicateUniqueRefusal(Fixture& fixture) {
  auto seed_context = Begin(fixture, "ipar-p7-09-duplicate-seed");
  auto seed_request = InsertRequest(
      fixture,
      seed_context,
      "ipar-p7-09-duplicate-seed-request",
      {Row("duplicate-unique-key",
           "duplicate_unique",
           880000,
           "dup-key",
           "duplicate",
           "seed",
           false)});
  const auto seeded = api::EngineInsertRows(seed_request);
  RequireOk(seeded, "IPAR-P7-09 duplicate seed insert failed");
  Commit(seed_context);

  const auto reopened =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  if (!reopened.ok()) {
    std::cerr << reopened.diagnostic.diagnostic_code << ':'
              << reopened.diagnostic.message_key << '\n';
  }
  Require(reopened.ok(), "IPAR-P7-09 duplicate probe reopen failed");

  auto duplicate_context = Begin(fixture, "ipar-p7-09-duplicate-refusal");
  auto duplicate_request = InsertRequest(
      fixture,
      duplicate_context,
      "ipar-p7-09-duplicate-refusal-request",
      {Row("duplicate-unique-key",
           "duplicate_unique",
           880001,
           "dup-key",
           "duplicate",
           "duplicate",
           false)});
  const auto duplicate = api::EngineInsertRows(duplicate_request);
  Require(!duplicate.ok,
          "IPAR-P7-09 duplicate unique key was unexpectedly admitted");
  if (!HasEvidence(duplicate.evidence,
                   "insert_unique_probe_candidate_source",
                   "persisted_unique_index_physical_probe")) {
    for (const auto& item : duplicate.evidence) {
      std::cerr << item.evidence_kind << '=' << item.evidence_id << '\n';
    }
  }
  Require(HasEvidence(duplicate.evidence,
                      "insert_unique_probe_candidate_source",
                      "persisted_unique_index_physical_probe"),
          "IPAR-P7-09 duplicate path did not use physical unique probe");
  Require(HasEvidence(duplicate.evidence,
                      "physical_unique_index_probe_path",
                      "mga_persisted_index_entry_lookup"),
          "IPAR-P7-09 duplicate path did not use physical lookup");
  Require(EvidenceU64(duplicate.evidence,
                      "unique_index_physical_probe_hits") == 1,
          "IPAR-P7-09 duplicate path did not record one physical probe hit");
  Rollback(duplicate_context);
}

void VerifyCommittedRowsAfterReopen(const Fixture& fixture,
                                    api::EngineApiU64 expected_rows) {
  const auto opened =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ':'
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "IPAR-P7-09 reopen failed");

  auto context = Begin(fixture, "ipar-p7-09-readback");
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.message_key
              << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "IPAR-P7-09 relation-state load failed");
  api::EngineApiU64 visible_rows = 0;
  api::EngineApiU64 index_entries = 0;
  for (const auto& row : loaded.state.row_versions) {
    if (row.table_uuid == fixture.table_uuid && !row.deleted) {
      ++visible_rows;
    }
  }
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.table_uuid == fixture.table_uuid) { ++index_entries; }
  }
  Require(visible_rows == expected_rows,
          "IPAR-P7-09 committed data-shape row count mismatch after reopen");
  Require(index_entries >= expected_rows,
          "IPAR-P7-09 committed data-shape index entries missing after reopen");
  Rollback(context);
}

void RegisterCandidate(page::PageSelectionLedger* ledger,
                       platform::TypedUuid database_uuid,
                       platform::TypedUuid filespace_uuid,
                       platform::TypedUuid object_uuid,
                       std::string page_family,
                       platform::u64 seed) {
  page::InsertPageCandidate candidate;
  candidate.database_uuid = database_uuid;
  candidate.filespace_uuid = filespace_uuid;
  candidate.object_uuid = object_uuid;
  candidate.page_uuid = NewUuid(platform::UuidKind::page, seed + 5000);
  candidate.page_family = std::move(page_family);
  candidate.page_number = 100 + seed;
  candidate.page_generation = 1;
  candidate.free_bytes = 8192;
  const auto registered = page::RegisterInsertPageCandidate(ledger, candidate);
  Require(registered.ok(), "IPAR-P7-09 page candidate registration failed");
}

filespace::FilespaceDescriptor Descriptor(platform::TypedUuid database_uuid,
                                          platform::TypedUuid filespace_uuid,
                                          filespace::FilespaceRole role,
                                          platform::u64 seed) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = database_uuid;
  descriptor.filespace_uuid = filespace_uuid;
  descriptor.path = "ipar-p7-09-filespace-" + std::to_string(seed) + ".sbfs";
  descriptor.role = role;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.page_size = 8192;
  descriptor.generation = 10 + seed;
  descriptor.read_only = false;
  descriptor.active = true;
  descriptor.physical_filespace_id = static_cast<platform::u16>(seed);
  descriptor.total_pages = 256;
  descriptor.free_pages = 128;
  descriptor.preallocated_pages = 8;
  descriptor.allocation_root_page = 1;
  descriptor.header_generation = 1;
  descriptor.writer_identity_uuid =
      NewUuid(platform::UuidKind::object, seed + 6000);
  return descriptor;
}

void VerifyMixedFilespacePlacementShape() {
  const auto database_uuid = NewUuid(platform::UuidKind::database, 900001);
  const auto transaction_uuid = NewUuid(platform::UuidKind::transaction, 900002);
  const auto object_uuid = NewUuid(platform::UuidKind::object, 900003);
  const auto data_filespace_uuid = NewUuid(platform::UuidKind::filespace, 900010);
  const auto index_filespace_uuid = NewUuid(platform::UuidKind::filespace, 900011);

  filespace::FilespaceRegistry registry;
  registry.filespaces.push_back(Descriptor(database_uuid,
                                           data_filespace_uuid,
                                           filespace::FilespaceRole::secondary_data,
                                           1));
  registry.filespaces.push_back(Descriptor(database_uuid,
                                           index_filespace_uuid,
                                           filespace::FilespaceRole::secondary_index,
                                           2));

  filespace::FilespacePlacementPolicy policy;
  policy.present = true;
  policy.require_explicit_binding = true;
  policy.default_preallocate_page_count = 2;
  policy.bindings.push_back({filespace::FilespaceObjectClass::exact_index,
                             "index",
                             index_filespace_uuid,
                             true,
                             4});

  page::PageReservationLedger reservation_ledger;
  page::PageSelectionLedger selection_ledger;
  filespace::FilespaceGrowthLedger growth_ledger;
  scratchbird::storage::page::OverflowLedger overflow_ledger;
  scratchbird::core::bulk_load::StrictBulkLoadLedger strict_bulk_ledger;
  dml::InsertPhysicalIntegrationContext context;
  context.page_reservation_ledger = &reservation_ledger;
  context.page_selection_ledger = &selection_ledger;
  context.filespace_growth_ledger = &growth_ledger;
  context.filespace_registry = &registry;
  context.overflow_ledger = &overflow_ledger;
  context.strict_bulk_load_ledger = &strict_bulk_ledger;

  RegisterCandidate(&selection_ledger,
                    database_uuid,
                    data_filespace_uuid,
                    object_uuid,
                    "index",
                    1);
  RegisterCandidate(&selection_ledger,
                    database_uuid,
                    index_filespace_uuid,
                    object_uuid,
                    "index",
                    2);

  dml::InsertPhysicalIntegrationRequest request;
  request.database_uuid = database_uuid;
  request.object_uuid = object_uuid;
  request.transaction_uuid = transaction_uuid;
  request.local_transaction_id = 990001;
  request.policy_uuid = NewUuid(platform::UuidKind::object, 900020);
  request.request_id = NewUuid(platform::UuidKind::object, 900021);
  request.page_family = "index";
  request.estimated_row_count = 96;
  request.estimated_payload_bytes = 8192;
  request.encoded_row_bytes = 128;
  request.page_size = 8192;
  request.placement_object_class = filespace::FilespaceObjectClass::exact_index;
  request.placement_policy = policy;
  request.require_placement_policy = true;
  request.require_placement_preallocation = true;
  request.placement_preallocation_pages = 4;

  const auto placed = dml::ExecuteInsertPhysicalIntegration(&context, request);
  Require(placed.ok(), "IPAR-P7-09 mixed-filespace placement failed");
  Require(placed.filespace_placement_resolved,
          "IPAR-P7-09 mixed-filespace placement was not resolved");
  Require(placed.resolved_filespace_uuid.value == index_filespace_uuid.value,
          "IPAR-P7-09 mixed-filespace placement selected wrong filespace");
  Require(selection_ledger.selections.size() == 1 &&
              selection_ledger.selections.front().filespace_uuid.value ==
                  index_filespace_uuid.value,
          "IPAR-P7-09 mixed-filespace selection ignored reservation filespace");
  Require(growth_ledger.preallocation_operations.size() == 1 &&
              growth_ledger.preallocation_operations.front().filespace_uuid.value ==
                  index_filespace_uuid.value,
          "IPAR-P7-09 mixed-filespace preallocation targeted wrong filespace");
}

void VerifyDataShapeStressMatrix() {
  auto fixture = MakeFixture(TimeSeed());
  api::EngineApiU64 expected_rows = 0;

  InsertShape(fixture, "narrow", 1000, 64, 16, false, &expected_rows);
  InsertShape(fixture, "wide", 10000, 24, 2048, false, &expected_rows);
  InsertShape(fixture, "null_heavy", 20000, 40, 48, false, &expected_rows);
  InsertShape(fixture, "large_value", 30000, 12, 10000, true, &expected_rows);
  InsertShape(fixture, "sequential", 40000, 128, 64, false, &expected_rows);
  InsertShape(fixture, "random", 50000, 128, 64, false, &expected_rows);
  InsertShape(fixture, "duplicate_heavy", 60000, 128, 80, false, &expected_rows);
  InsertShape(fixture, "hot_range", 70000, 96, 64, false, &expected_rows);
  CopyShape(fixture, "copy_native", 80000, 40, &expected_rows);

  VerifyDuplicateUniqueRefusal(fixture);
  ++expected_rows;
  VerifyCommittedRowsAfterReopen(fixture, expected_rows);
  VerifyMixedFilespacePlacementShape();
}

}  // namespace

int main() {
  VerifyDataShapeStressMatrix();
  std::cout << "ipar_data_shape_stress_matrix_gate=passed\n";
  return EXIT_SUCCESS;
}
