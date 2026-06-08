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
  Require(generated.ok(), "ODF-034 UUID generation failed");
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
  std::string route;
  bool bootstrapped = false;
  std::string raw_line;
};

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string index_uuid;
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

std::filesystem::path DeltaLedgerPath(const Fixture& fixture) {
  return fixture.database_path.string() + ".sb.mga_secondary_index_delta_ledger";
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
    record.route = fields[7];
    record.bootstrapped = fields[8] == "1";
    record.raw_line = line;
    records.push_back(std::move(record));
  }
  return records;
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

void AssertNoDocumentationRuntimeTokens(
    const Fixture& fixture,
    const std::vector<api::EngineEvidenceReference>& evidence) {
  std::vector<std::string> bodies = ReadLines(AllocatorPath(fixture));
  for (const auto& entry : evidence) {
    bodies.push_back(entry.evidence_kind + "=" + entry.evidence_id);
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
              "ODF-034 runtime allocator evidence leaked documentation token");
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
  RequireOk(begun, "ODF-034 begin transaction failed");
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
  fixture.salt = 34000;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf034_" + std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf034.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + fixture.salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-034 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 11);
  return fixture;
}

api::CrudIndexRecord NonUniqueIndex(const Fixture& fixture,
                                    const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "name";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = false;
  index.key_envelopes.push_back("name");
  return index;
}

api::CrudRowVersionRecord RowRecord(const Fixture& fixture,
                                    const api::EngineRequestContext& context,
                                    platform::u64 salt,
                                    std::string id,
                                    std::string name) {
  api::CrudRowVersionRecord row;
  row.creator_tx = context.local_transaction_id;
  row.table_uuid = fixture.table_uuid;
  row.row_uuid = NewUuidText(platform::UuidKind::row, fixture.salt + 200 + salt);
  row.version_uuid = NewUuidText(platform::UuidKind::row, fixture.salt + 300 + salt);
  row.values.push_back({"id", std::move(id)});
  row.values.push_back({"name", std::move(name)});
  return row;
}

api::MgaIndexEntryRowInput IndexRow(platform::u64 salt,
                                    std::string name) {
  api::MgaIndexEntryRowInput row;
  row.row_uuid = NewUuidText(platform::UuidKind::row, 50000 + salt);
  row.version_uuid = NewUuidText(platform::UuidKind::row, 51000 + salt);
  row.values.push_back({"id", std::to_string(salt)});
  row.values.push_back({"name", std::move(name)});
  return row;
}

api::MgaSecondaryIndexDeltaLedgerEntryInput DeltaInput(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const api::CrudIndexRecord& index,
    platform::u64 salt,
    std::string name) {
  api::MgaSecondaryIndexDeltaLedgerEntryInput input;
  input.index = index;
  input.table_uuid = fixture.table_uuid;
  input.row_uuid = NewUuidText(platform::UuidKind::row, fixture.salt + 600 + salt);
  input.version_uuid = NewUuidText(platform::UuidKind::row, fixture.salt + 700 + salt);
  input.values.push_back({"id", std::to_string(salt)});
  input.values.push_back({"name", std::move(name)});
  input.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::insert;
  input.cleanup_horizon_token =
      "mga-cleanup-horizon-" + std::to_string(context.local_transaction_id);
  return input;
}

void RowBatchesUseDurableRanges(Fixture& fixture,
                                const api::EngineRequestContext& context) {
  std::vector<api::CrudRowVersionRecord> rows;
  rows.push_back(RowRecord(fixture, context, 1, "1", "ada"));
  rows.push_back(RowRecord(fixture, context, 2, "2", "grace"));
  rows.push_back(RowRecord(fixture, context, 3, "3", "katherine"));
  std::vector<platform::u64> sequences;
  RequireDiagnosticOk(api::AppendMgaRowVersions(context, &rows, &sequences),
                      "ODF-034 row batch append failed");
  Require(sequences.size() == 3 && sequences[0] == 1 && sequences[1] == 2 &&
              sequences[2] == 3,
          "ODF-034 row batch did not receive contiguous event sequence range");

  auto row_allocs = Allocations(fixture, "row_versions");
  Require(row_allocs.size() == 1,
          "ODF-034 row batch did not reserve exactly one durable range");
  Require(row_allocs[0].first == 1 && row_allocs[0].count == 3 &&
              row_allocs[0].next == 4 && row_allocs[0].bootstrapped,
          "ODF-034 row batch durable range fields are wrong");

  const auto hole = api::ReserveMgaRowEventSequenceRangeForTesting(context, 4);
  Require(hole.ok && hole.first == 4 && hole.count == 4 && hole.next == 8,
          "ODF-034 test row hole reservation failed");
  api::ClearMgaEventSequenceRangeCacheForTesting();

  std::vector<api::CrudRowVersionRecord> post_reset_rows;
  post_reset_rows.push_back(RowRecord(fixture, context, 4, "4", "dorothy"));
  sequences.clear();
  RequireDiagnosticOk(api::AppendMgaRowVersions(context, &post_reset_rows, &sequences),
                      "ODF-034 post-reset row append failed");
  Require(sequences.size() == 1 && sequences[0] == 8,
          "ODF-034 durable row allocator state was not reloaded after cache reset");

  row_allocs = Allocations(fixture, "row_versions");
  Require(row_allocs.size() == 3,
          "ODF-034 row allocator durable record count mismatch");
  Require(row_allocs.back().first == 8 && row_allocs.back().count == 1 &&
              row_allocs.back().next == 9 &&
              row_allocs.back().route == "durable_allocator_state",
          "ODF-034 post-reset row allocation did not use durable allocator state");

  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "ODF-034 row state reload failed");
  std::vector<platform::u64> observed;
  for (const auto& row : loaded.state.row_versions) {
    observed.push_back(row.event_sequence);
  }
  std::sort(observed.begin(), observed.end());
  Require(observed == std::vector<platform::u64>({1, 2, 3, 8}),
          "ODF-034 row store persisted unexpected event sequences");
  Require(std::filesystem::exists(RowStorePath(fixture)),
          "ODF-034 row store path was not created");
}

void IndexBatchUsesOneDurableRange(Fixture& fixture,
                                   const api::EngineRequestContext& context,
                                   const api::CrudIndexRecord& index) {
  std::vector<api::MgaIndexEntryRowInput> rows;
  rows.push_back(IndexRow(1, "ada"));
  rows.push_back(IndexRow(2, "grace"));
  RequireDiagnosticOk(api::AppendMgaIndexEntriesForRowsWithIndexes(
                          context, {index}, fixture.table_uuid, rows),
                      "ODF-034 index batch append failed");

  const auto index_allocs = Allocations(fixture, "index_entries");
  Require(index_allocs.size() == 1,
          "ODF-034 index batch did not reserve exactly one durable range");
  Require(index_allocs[0].first == 1 && index_allocs[0].count == 2 &&
              index_allocs[0].next == 3 && index_allocs[0].bootstrapped,
          "ODF-034 index durable range fields are wrong");

  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "ODF-034 index state reload failed");
  std::vector<platform::u64> observed;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.index_uuid == fixture.index_uuid) {
      observed.push_back(entry.event_sequence);
    }
  }
  std::sort(observed.begin(), observed.end());
  Require(observed == std::vector<platform::u64>({1, 2}),
          "ODF-034 index batch did not persist contiguous event sequences");
  Require(std::filesystem::exists(IndexStorePath(fixture)),
          "ODF-034 index store path was not created");
}

std::vector<api::EngineEvidenceReference> DeltaLedgerUsesDurableRanges(
    Fixture& fixture,
    const api::EngineRequestContext& context,
    const api::CrudIndexRecord& index) {
  std::vector<api::EngineEvidenceReference> evidence;
  RequireDiagnosticOk(api::AppendMgaSecondaryIndexDeltaLedgerEntries(
                          context,
                          {DeltaInput(fixture, context, index, 1, "ada"),
                           DeltaInput(fixture, context, index, 2, "grace")},
                          &evidence),
                      "ODF-034 delta ledger batch append failed");
  Require(HasEvidence(evidence, "mga_event_sequence_allocator_stream",
                      "secondary_index_delta_ledger"),
          "ODF-034 delta ledger missing allocator stream evidence");
  Require(HasEvidence(evidence, "mga_event_sequence_allocator_first", "1") &&
              HasEvidence(evidence, "mga_event_sequence_allocator_count", "2") &&
              HasEvidence(evidence, "mga_event_sequence_allocator_next", "3"),
          "ODF-034 delta ledger range evidence is wrong");

  auto delta_allocs = Allocations(fixture, "secondary_index_delta_ledger");
  Require(delta_allocs.size() == 1,
          "ODF-034 delta ledger did not reserve exactly one durable range");
  Require(delta_allocs[0].first == 1 && delta_allocs[0].count == 2 &&
              delta_allocs[0].next == 3 && delta_allocs[0].bootstrapped,
          "ODF-034 delta durable range fields are wrong");

  api::ClearMgaEventSequenceRangeCacheForTesting();
  std::vector<api::EngineEvidenceReference> post_reset_evidence;
  RequireDiagnosticOk(api::AppendMgaSecondaryIndexDeltaLedgerEntries(
                          context,
                          {DeltaInput(fixture, context, index, 3, "dorothy")},
                          &post_reset_evidence),
                      "ODF-034 post-reset delta append failed");
  evidence.insert(evidence.end(),
                  post_reset_evidence.begin(),
                  post_reset_evidence.end());

  delta_allocs = Allocations(fixture, "secondary_index_delta_ledger");
  Require(delta_allocs.size() == 2,
          "ODF-034 delta allocator durable record count mismatch");
  Require(delta_allocs.back().first == 3 && delta_allocs.back().count == 1 &&
              delta_allocs.back().next == 4 &&
              delta_allocs.back().route == "durable_allocator_state",
          "ODF-034 post-reset delta allocation did not use durable state");

  const auto loaded = api::LoadMgaSecondaryIndexDeltaLedger(context);
  Require(loaded.ok, "ODF-034 delta ledger reload failed");
  Require(loaded.ledger.records.size() == 3,
          "ODF-034 delta ledger record count did not match reserved ranges");
  Require(std::filesystem::exists(DeltaLedgerPath(fixture)),
          "ODF-034 delta ledger path was not created");
  return evidence;
}

}  // namespace

int main() {
  auto fixture = MakeFixture();
  const auto context = Begin(fixture, "odf034");
  const auto index = NonUniqueIndex(fixture, context);

  RowBatchesUseDurableRanges(fixture, context);
  IndexBatchUsesOneDurableRange(fixture, context, index);
  const auto delta_evidence =
      DeltaLedgerUsesDurableRanges(fixture, context, index);
  AssertNoDocumentationRuntimeTokens(fixture, delta_evidence);
  return EXIT_SUCCESS;
}
