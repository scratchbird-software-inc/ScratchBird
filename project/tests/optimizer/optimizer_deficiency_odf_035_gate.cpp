// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"
#include "database_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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
  if (!condition) {
    Fail(message);
  }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
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
  Require(generated.ok(), "ODF-035 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::string current;
  std::istringstream in(line);
  while (std::getline(in, current, '\t')) {
    fields.push_back(current);
  }
  if (!line.empty() && line.back() == '\t') {
    fields.emplace_back();
  }
  return fields;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

platform::u64 ParseU64(const std::string& value) {
  return static_cast<platform::u64>(std::stoull(value));
}

struct AllocationRecord {
  std::string stream_kind;
  platform::u64 first = 0;
  platform::u64 count = 0;
  platform::u64 next = 0;
  bool bootstrapped = false;
  std::string raw_line;
};

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string name_index_uuid;
  std::string city_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) {
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

std::filesystem::path AllocatorPath(const Fixture& fixture) {
  return fixture.database_path.string() + ".sb.mga_event_sequence_allocator";
}

std::filesystem::path RowStorePath(const Fixture& fixture) {
  return fixture.database_path.string() + ".sb.mga_row_versions";
}

std::filesystem::path IndexStorePath(const Fixture& fixture) {
  return fixture.database_path.string() + ".sb.mga_index_entries";
}

std::vector<AllocationRecord> Allocations(const Fixture& fixture,
                                          std::string_view stream_kind) {
  std::vector<AllocationRecord> records;
  for (const auto& line : ReadLines(AllocatorPath(fixture))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 9 || fields[0] != "SBMGAEVSEQ1" ||
        fields[1] != "RANGE" || fields[2] != stream_kind) {
      continue;
    }
    AllocationRecord record;
    record.stream_kind = fields[2];
    record.first = ParseU64(fields[4]);
    record.count = ParseU64(fields[5]);
    record.next = ParseU64(fields[6]);
    record.bootstrapped = fields[8] == "1";
    record.raw_line = line;
    records.push_back(std::move(record));
  }
  return records;
}

void AssertNoDocumentationRuntimeTokens(const Fixture& fixture) {
  std::vector<std::string> bodies = ReadLines(AllocatorPath(fixture));
  for (const auto& line : ReadLines(RowStorePath(fixture))) {
    bodies.push_back(line);
  }
  for (const auto& line : ReadLines(IndexStorePath(fixture))) {
    bodies.push_back(line);
  }
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans",
      "execution_plan",
      "findings",
      "audit",
      "contracts",
      "references"};
  for (const auto& body : bodies) {
    for (const auto token : forbidden) {
      Require(body.find(token) == std::string::npos,
              "ODF-035 runtime append evidence leaked documentation token");
    }
  }
}

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
  context.catalog_generation_id = 10;
  context.security_epoch = 20;
  context.resource_epoch = 30;
  context.name_resolution_epoch = 40;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-035 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.salt = 35000;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf035_" + std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf035.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + fixture.salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-035 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 10);
  fixture.name_index_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 11);
  fixture.city_index_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 12);
  return fixture;
}

api::CrudIndexRecord NonUniqueIndex(const Fixture& fixture,
                                    const api::EngineRequestContext& context,
                                    const std::string& index_uuid,
                                    std::string column_name) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column_name);
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = false;
  index.key_envelopes.push_back(index.column_name);
  return index;
}

api::CrudRowVersionRecord RowRecord(const Fixture& fixture,
                                    const api::EngineRequestContext& context,
                                    platform::u64 salt,
                                    std::string id,
                                    std::string name,
                                    std::string city) {
  api::CrudRowVersionRecord row;
  row.creator_tx = context.local_transaction_id;
  row.table_uuid = fixture.table_uuid;
  row.row_uuid = NewUuidText(platform::UuidKind::row, fixture.salt + 200 + salt);
  row.version_uuid = NewUuidText(platform::UuidKind::row, fixture.salt + 300 + salt);
  row.values.push_back({"id", std::move(id)});
  row.values.push_back({"name", std::move(name)});
  row.values.push_back({"city", std::move(city)});
  return row;
}

api::MgaIndexEntryRowInput IndexRow(const api::CrudRowVersionRecord& row) {
  api::MgaIndexEntryRowInput input;
  input.row_uuid = row.row_uuid;
  input.version_uuid = row.version_uuid;
  input.values = row.values;
  return input;
}

void HotAppendContextUsesOneStreamLifecyclePerStore(
    Fixture& fixture,
    const api::EngineRequestContext& context) {
  api::ClearMgaEventSequenceRangeCacheForTesting();
  const auto name_index =
      NonUniqueIndex(fixture, context, fixture.name_index_uuid, "name");
  const auto city_index =
      NonUniqueIndex(fixture, context, fixture.city_index_uuid, "city");

  std::vector<api::CrudRowVersionRecord> rows;
  rows.push_back(RowRecord(fixture, context, 1, "1", "ada", "london"));
  rows.push_back(RowRecord(fixture, context, 2, "2", "grace", "arlington"));
  rows.push_back(RowRecord(fixture, context, 3, "3", "katherine", "white-sulphur-springs"));

  api::MgaRelationHotAppendContext append_context(context);
  std::vector<platform::u64> row_sequences;
  RequireDiagnosticOk(append_context.AppendRowVersions(&rows, &row_sequences),
                      "ODF-035 hot row append failed");
  RequireDiagnosticOk(append_context.FlushRowVersions(),
                      "ODF-035 hot row flush failed");

  std::vector<api::MgaIndexEntryRowInput> index_rows;
  for (const auto& row : rows) {
    index_rows.push_back(IndexRow(row));
  }
  api::MgaIndexEntryAppendBatch name_batch;
  name_batch.index = name_index;
  name_batch.table_uuid = fixture.table_uuid;
  name_batch.rows = index_rows;
  api::MgaIndexEntryAppendBatch city_batch;
  city_batch.index = city_index;
  city_batch.table_uuid = fixture.table_uuid;
  city_batch.rows = index_rows;
  RequireDiagnosticOk(append_context.AppendIndexEntryBatches({name_batch, city_batch}),
                      "ODF-035 hot index append failed");
  RequireDiagnosticOk(append_context.FlushIndexEntries(),
                      "ODF-035 hot index flush failed");

  const auto& counters = append_context.counters();
  Require(counters.row_stream_opens == 1 && counters.row_stream_flushes == 1,
          "ODF-035 row stream was not opened/flushed exactly once");
  Require(counters.index_stream_opens == 1 && counters.index_stream_flushes == 1,
          "ODF-035 index stream was not opened/flushed exactly once");
  Require(counters.scoped_row_stream_opens == 1 &&
              counters.scoped_row_stream_flushes == 1,
          "ODF-035 scoped row stream was not opened/flushed exactly once");
  Require(counters.scoped_index_stream_opens == 1 &&
              counters.scoped_index_stream_flushes == 1,
          "ODF-035 scoped index stream was not opened/flushed exactly once");
  Require(counters.row_range_reservations == 1 &&
              counters.index_range_reservations == 1,
          "ODF-035 hot append did not reserve exactly one range per stream");
  Require(counters.row_versions_appended == 3 &&
              counters.index_entries_appended == 6,
          "ODF-035 hot append counters do not match batch shape");

  Require(row_sequences == std::vector<platform::u64>({1, 2, 3}),
          "ODF-035 row batch did not receive a contiguous event range");

  const auto row_allocs = Allocations(fixture, "row_versions");
  Require(row_allocs.size() == 1 && row_allocs[0].first == 1 &&
              row_allocs[0].count == 3 && row_allocs[0].next == 4 &&
              row_allocs[0].bootstrapped,
          "ODF-035 row allocator did not record one correct durable range");
  const auto index_allocs = Allocations(fixture, "index_entries");
  Require(index_allocs.size() == 1 && index_allocs[0].first == 1 &&
              index_allocs[0].count == 6 && index_allocs[0].next == 7 &&
              index_allocs[0].bootstrapped,
          "ODF-035 index allocator did not record one correct durable range");

  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "ODF-035 state reload failed");
  std::vector<platform::u64> loaded_row_sequences;
  for (const auto& row : loaded.state.row_versions) {
    loaded_row_sequences.push_back(row.event_sequence);
  }
  std::sort(loaded_row_sequences.begin(), loaded_row_sequences.end());
  Require(loaded_row_sequences == std::vector<platform::u64>({1, 2, 3}),
          "ODF-035 row store persisted unexpected sequences");
  std::vector<platform::u64> loaded_index_sequences;
  for (const auto& entry : loaded.state.index_entries) {
    loaded_index_sequences.push_back(entry.event_sequence);
  }
  std::sort(loaded_index_sequences.begin(), loaded_index_sequences.end());
  Require(loaded_index_sequences ==
              std::vector<platform::u64>({1, 2, 3, 4, 5, 6}),
          "ODF-035 index store persisted unexpected sequences");
  AssertNoDocumentationRuntimeTokens(fixture);
}

}  // namespace

int main() {
  auto fixture = MakeFixture();
  const auto context = Begin(fixture, "odf035");
  HotAppendContextUsesOneStreamLifecyclePerStore(fixture, context);
  return EXIT_SUCCESS;
}
