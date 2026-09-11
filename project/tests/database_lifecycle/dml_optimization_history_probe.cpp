// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Real engine API probe. This is NOT proof of SQL ON CONFLICT transport.
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "dml/insert_api.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "dml/transactional_index_provider.hpp"
#include "dml/transactional_relation_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
constexpr auto database_id = "019f3900-0000-7000-8000-000000000001";
constexpr auto table_id = "019f3900-0000-7000-8000-000000000101";
constexpr auto index_id = "019f3900-0000-7000-8000-000000000201";

void Require(bool ok, const std::string& detail) {
  if (!ok) throw std::runtime_error(detail);
}
void PrintDiagnostic(const char* phase, const api::EngineApiDiagnostic& diagnostic) {
  std::cout << phase << '\t' << diagnostic.code << '\t' << diagnostic.message_key
            << '\t' << diagnostic.detail << '\t' << diagnostic.error << '\n';
  for (const auto& field : diagnostic.fields)
    std::cout << phase << "_field\t" << field.key << '\t' << field.value << '\n';
}
template<class Result> void RequireOk(const Result& result, const char* operation) {
  if (!result.ok) {
    for (const auto& d : result.diagnostics) std::cerr << d.code << ':' << d.detail << '\n';
    throw std::runtime_error(operation);
  }
}
api::EngineRequestContext Begin(const std::string& path, const char* role,
                               const char* isolation = "read_committed") {
  api::EngineBeginTransactionRequest request;
  request.context.database_path = path;
  request.context.database_uuid.canonical = database_id;
  request.context.request_id = role;
  request.context.trust_mode = api::EngineTrustMode::server_isolated;
  request.context.principal_uuid.canonical = "019f3900-0000-7000-8000-000000000301";
  request.context.session_uuid.canonical = "019f3900-0000-7000-8000-000000000302";
  request.context.current_schema_uuid.canonical = "019f3900-0000-7000-8000-000000000303";
  request.context.default_root_uuid.canonical = "019f3900-0000-7000-8000-000000000304";
  request.context.security_context_present = true;
  request.context.catalog_generation_id = 1;
  request.context.security_epoch = 1;
  request.context.resource_epoch = 1;
  request.context.name_resolution_epoch = 1;
  request.isolation_level = isolation;
  auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "begin");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.transaction_isolation_level = begun.isolation_level;
  context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
  return context;
}
void End(const api::EngineRequestContext& context, bool commit) {
  if (commit) {
    api::EngineCommitTransactionRequest request;
    request.context = context;
    RequireOk(api::EngineCommitTransaction(request), "commit");
  } else {
    api::EngineRollbackTransactionRequest request;
    request.context = context;
    RequireOk(api::EngineRollbackTransaction(request), "rollback");
  }
}
api::EngineTypedValue Text(std::string value) {
  api::EngineTypedValue result;
  result.descriptor.descriptor_kind = "scalar";
  result.descriptor.canonical_type_name = "text";
  result.encoded_value = std::move(value);
  return result;
}
api::EngineInsertRowsResult Insert(const api::EngineRequestContext& context,
                                  std::string key, std::string value,
                                  std::string action = {}) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = table_id;
  request.target_table.object_kind = "table";
  api::EngineRowValue row;
  row.fields = {{"id", Text(std::move(key))}, {"payload", Text(std::move(value))}};
  request.input_rows.push_back(std::move(row));
  request.on_conflict_action = action;
  if (!action.empty()) request.conflict_target_column = "id";
  if (action == "do_update") request.conflict_update_columns = {"payload"};
  return api::EngineInsertRows(request);
}
using Rows = std::map<std::string, std::string>;
void Observe(const api::EngineRequestContext& context, const char* phase, const Rows& expected) {
  api::TransactionalRelationStore store(context);
  auto loaded = store.LoadConstraintScope(table_id);
  Require(loaded.ok, "scoped load failed");
  const auto state = store.BuildReadView(&loaded);
  Rows rows;
  const auto visible_rows = api::VisibleMgaRowsForContext(state, table_id, context);
  std::map<std::string, std::string> live_keys;
  for (const auto& row : visible_rows) {
    Require(rows.emplace(api::CrudFieldValue(row.values, "id"),
                         api::CrudFieldValue(row.values, "payload")).second,
            "duplicate visible key");
    live_keys.emplace(row.row_uuid, api::CrudFieldValue(row.values, "id"));
  }
  Require(rows == expected, std::string(phase) + ": wrong rows");
  const auto indexes = api::VisibleMgaIndexesForTable(state, table_id, context.local_transaction_id);
  Require(indexes.size() == 1, "index missing");
  api::MgaTransactionalIndexProvider provider(context, nullptr);
  const auto validation = provider.ValidateAgainstRelation(state, indexes.front());
  Require(validation.ok, std::string(phase) + ": durable index disagrees with rows");
  // ValidateAgainstRelation checks missing memberships; its count is derived
  // from rows. Independently enumerate MGA-visible, row-rechecked memberships
  // and probe every historical key for visible ghosts. Payload-only UPDATE
  // legitimately retains its prior index locator when the indexed key is
  // unchanged (PrepareTransactionalIndexVersionMutation); the locator need
  // not carry the newest row version UUID.
  std::set<std::pair<std::string, std::string>> memberships, expected_memberships;
  std::set<std::string> probe_keys{"absent-key"};
  for (const auto& row : visible_rows) {
    const auto key = api::CrudFieldValue(row.values, "id");
    expected_memberships.emplace(key, row.row_uuid);
    probe_keys.insert(key);
  }
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != index_id || entry.table_uuid != table_id) continue;
    const auto key = api::CrudIndexEntryLogicalKey(indexes.front(), entry);
    probe_keys.insert(key);
    const auto row = live_keys.find(entry.row_uuid);
    if (row != live_keys.end() && row->second == key &&
        (entry.entry_kind == "exact" || entry.entry_kind == "insert" || entry.entry_kind == "rebuild") &&
        api::MgaCreatorVisible(state, entry.creator_tx, entry.event_sequence, context.local_transaction_id)) {
      memberships.emplace(key, entry.row_uuid);
    }
  }
  Require(memberships == expected_memberships, std::string(phase) + ": wrong visible index memberships");
  for (const auto& key : probe_keys) {
    api::EnginePredicateEnvelope predicate;
    predicate.predicate_kind = "column_equals";
    predicate.canonical_predicate_envelope = "id";
    predicate.bound_values.push_back(Text(key));
    const auto lookup = provider.ResolveVisibleEntry(state, indexes.front(), predicate, 0);
    Require(lookup.ok && lookup.rows.size() == expected.count(key), std::string(phase) + ": index probe count");
    if (!lookup.rows.empty()) {
      Require(api::CrudFieldValue(lookup.rows.front().values, "id") == key &&
              api::CrudFieldValue(lookup.rows.front().values, "payload") == expected.at(key),
              std::string(phase) + ": index probe values");
    }
  }
  for (const auto& [key, value] : expected) {
    // The provider validates the full relation above. Print exact logical
    // membership, not a digest or random row/version identity.
    std::cout << phase << '\t' << key << '\t' << value << '\n';
  }
  std::cout << phase << "\tindex_entries\t" << memberships.size() << '\n';
}
void Create(const std::string& path) {
  db::DatabaseCreateConfig create;
  create.path = path;
  create.database_uuid = uuid::ParseTypedUuid(UuidKind::database, database_id).value;
  create.filespace_uuid = uuid::ParseTypedUuid(UuidKind::filespace,
      "019f3900-0000-7000-8000-000000000002").value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779800001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  Require(db::CreateDatabaseFile(create).ok(), "create database");
  auto context = Begin(path, "seed-metadata");
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = table_id;
  table.default_name = "optimization_history";
  table.columns = {{"id", "canonical=text;nullable=false"}, {"payload", "canonical=text"}};
  Require(!api::AppendMgaTableMetadata(context, table).error, "table metadata");
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = index_id;
  index.table_uuid = table_id;
  index.column_name = "id";
  index.key_envelopes = {"id", "unique"};
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.default_name = "optimization_key";
  Require(!api::AppendMgaIndexMetadata(context, index).error, "index metadata");
  End(context, true);
  context = Begin(path, "seed-row");
  RequireOk(Insert(context, "6", "baseline"), "seed insert");
  End(context, true);
}
void History(const std::string& path, const std::string& ids_path) {
  std::ofstream ids(ids_path);
  Require(bool(ids), "open transaction correspondence");
  const Rows baseline{{"6", "baseline"}};
  const Rows changed{{"6", "changed"}, {"7", "new"}};
  auto reader = Begin(path, "old-reader", "snapshot");
  auto writer = Begin(path, "conflict-writer");
  ids << "old-reader\t" << reader.local_transaction_id << "\tcommitted\n";
  ids << "writer\t" << writer.local_transaction_id << "\tcommitted\n";
  Observe(reader, "snapshot_before", baseline);
  auto inserted = Insert(writer, "7", "new", "do_nothing");
  RequireOk(inserted, "unmatched conflict insert");
  Require(inserted.inserted_count == 1 && inserted.updated_count == 0 && inserted.skipped_count == 0,
          "unmatched conflict affected counts");
  auto skipped = Insert(writer, "6", "ignored", "do_nothing");
  RequireOk(skipped, "matched conflict skip");
  Require(skipped.inserted_count == 0 && skipped.updated_count == 0 && skipped.skipped_count == 1,
          "matched conflict skip counts");
  auto updated = Insert(writer, "6", "changed", "do_update");
  RequireOk(updated, "matched conflict update");
  Require(updated.inserted_count == 0 && updated.updated_count == 1 && updated.skipped_count == 0,
          "matched conflict update counts");
  Observe(writer, "writer_before_commit", changed);
  Observe(reader, "snapshot_during_writer", baseline);
  End(writer, true);
  Observe(reader, "snapshot_after_commit", baseline);
  End(reader, true);
  auto rewind = Begin(path, "rewind-writer");
  ids << "rewind\t" << rewind.local_transaction_id << "\trolled_back\n";
  Require(!api::CreateMgaSavepointMarker(rewind, "upsert_sp").error, "savepoint");
  RequireOk(Insert(rewind, "6", "discarded", "do_update"), "rewind upsert");
  RequireOk(Insert(rewind, "8", "discarded", "do_nothing"), "rewind insert");
  Require(!api::RollbackToMgaSavepointMarker(rewind, "upsert_sp").error, "rewind");
  Observe(rewind, "after_rewind", changed);
  Require(!api::ReleaseMgaSavepointMarker(rewind, "upsert_sp").error, "release");
  End(rewind, false);
  ids.flush();
  Require(bool(ids), "write transaction correspondence");
}
api::EngineExecuteNativeBulkIngestResult Bulk(const api::EngineRequestContext& context,
                                            const Rows& rows) {
  api::EngineExecuteNativeBulkIngestRequest request;
  request.context = context;
  request.target_table.uuid.canonical = table_id;
  request.target_table.object_kind = "table";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  for (const auto& [key, value] : rows) {
    api::EngineRowValue row;
    row.fields = {{"id", Text(key)}, {"payload", Text(value)}};
    request.canonical_rows.push_back(std::move(row));
  }
  request.estimated_row_count = request.canonical_rows.size();
  return api::EngineExecuteNativeBulkIngest(request);
}
void BulkHistory(const std::string& path, const std::string& ids_path) {
  auto reader = Begin(path, "bulk-snapshot", "snapshot");
  auto writer = Begin(path, "bulk-writer");
  std::ofstream ids(ids_path);
  ids << "old-reader\t" << reader.local_transaction_id << "\tcommitted\n"
      << "writer\t" << writer.local_transaction_id << "\tcommitted\n";
  Require(bool(ids), "bulk transaction correspondence");
  const auto bulk = [&](const Rows& rows) {
    return Bulk(writer, rows);
  };
  const Rows changed{{"6", "baseline"}, {"9", "nine"}, {"10", "ten"}};
  const auto inserted = bulk({{"9", "nine"}, {"10", "ten"}});
  RequireOk(inserted, "bulk insert");
  Require(inserted.inserted_rows == 2 && inserted.accepted_rows == 2 && inserted.rejected_rows == 0,
          "bulk affected counts");
  Observe(writer, "bulk_before_commit", changed);
  Observe(reader, "snapshot_during_bulk", {{"6", "baseline"}});
  const auto refused = bulk({{"11", "prefix"}, {"6", "collision"}, {"12", "suffix"}});
  Require(!refused.ok && !refused.diagnostics.empty(), "bulk duplicate accepted");
  Require(refused.diagnostics.front().code == "CLI.CONSTRAINT_UNIQUE_VIOLATION", "bulk wrong duplicate diagnostic");
  Require(refused.accepted_rows == 0 && refused.inserted_rows == 0,
          "failed bulk reported accepted mutations");
  std::cout << "bulk_refusal_counts\t" << refused.accepted_rows << '\t'
            << refused.inserted_rows << '\t' << refused.rejected_rows << '\n';
  for (const auto& d : refused.diagnostics) {
    PrintDiagnostic("bulk_refusal", d);
  }
  Observe(writer, "bulk_after_refusal", changed);
  Require(!api::CreateMgaSavepointMarker(writer, "bulk_sp").error, "bulk savepoint");
  RequireOk(bulk({{"11", "reused"}}), "bulk key reuse");
  Require(!api::RollbackToMgaSavepointMarker(writer, "bulk_sp").error, "bulk rewind");
  Require(!api::ReleaseMgaSavepointMarker(writer, "bulk_sp").error, "bulk release");
  Observe(writer, "bulk_after_rewind", changed);
  End(writer, true);
  Observe(reader, "snapshot_after_bulk_commit", {{"6", "baseline"}});
  End(reader, true);
}
// Fixed mt19937 outputs plus integer modulo keep the recorded seed portable;
// no implementation-defined standard-library probability distribution is used.
// The map is an independent expected-value model, not a storage backend.
void RandomHistory(const std::string& path, const std::string& ids_path,
                   std::uint32_t seed) {
  std::mt19937 random(seed);
  Rows committed{{"6", "baseline"}};
  std::ofstream ids(ids_path);
  Require(bool(ids), "random transaction correspondence");
  std::cout << "seed\t" << seed << '\n';
  for (int epoch = 0; epoch < 3; ++epoch) {
    auto reader = Begin(path, "random-snapshot", "snapshot");
    auto writer = Begin(path, "random-writer");
    const bool commit = epoch != 1;
    ids << "old-reader\t" << reader.local_transaction_id << "\tcommitted\n"
        << "writer-" << epoch << '\t' << writer.local_transaction_id << '\t'
        << (commit ? "committed" : "rolled_back") << '\n';
    Rows model = committed;
    const auto anchor = std::to_string(20000 + epoch);
    auto inserted = Insert(writer, anchor, "anchor", "do_nothing");
    RequireOk(inserted, "random anchor");
    Require(inserted.inserted_count == 1, "random anchor count");
    model[anchor] = "anchor";
    for (const auto base : {30000, 40000}) {
      const auto key = std::to_string(base + epoch);
      const auto warm = Insert(writer, key, "warm");
      RequireOk(warm, "random warm-cache prefix");
      Require(warm.inserted_count == 1, "random warm-cache count");
      model[key] = "warm";
    }
    const Rows warm_bulk{{std::to_string(50000 + epoch), "warm-bulk"},
                         {std::to_string(60000 + epoch), "warm-bulk"}};
    const auto bulk_prefix = Bulk(writer, warm_bulk);
    RequireOk(bulk_prefix, "random successful bulk prefix");
    Require(bulk_prefix.inserted_rows == 2 && bulk_prefix.accepted_rows == 2 &&
            bulk_prefix.rejected_rows == 0, "random successful bulk prefix counts");
    model.insert(warm_bulk.begin(), warm_bulk.end());
    Rows saved;
    for (int step = 0; step < 24; ++step) {
      if (step == 6) {
        Require(!api::CreateMgaSavepointMarker(writer, "random_sp").error, "random savepoint");
        saved = model;
      }
      if (step == 12) {
        Require(!api::RollbackToMgaSavepointMarker(writer, "random_sp").error, "random rewind");
        Require(!api::ReleaseMgaSavepointMarker(writer, "random_sp").error, "random release");
        model = saved;
      }
      const auto key = std::to_string(6 + random() % 7);
      const auto value = "v" + std::to_string(random());
      const auto action = random() % 4;
      std::cout << "action\t" << epoch << '\t' << step << '\t'
                << action << '\t' << key << '\t' << value << std::endl;
      if (action == 3) {
        const Rows batch{{key, value}, {std::to_string(100 + step), "bulk-" + value}};
        bool conflict = false;
        for (const auto& [candidate, _] : batch) conflict |= model.contains(candidate);
        const auto result = Bulk(writer, batch);
        if (conflict) {
          Require(!result.ok && !result.diagnostics.empty(), "random bulk conflict accepted");
          Require(result.diagnostics.front().code == "CLI.CONSTRAINT_UNIQUE_VIOLATION",
                  "random bulk conflict diagnostic");
          Require(result.accepted_rows == 0 && result.inserted_rows == 0,
                  "random failed bulk reported mutations");
          std::cout << "bulk_refusal_counts\t" << result.accepted_rows << '\t'
                    << result.inserted_rows << '\t' << result.rejected_rows << '\n';
          for (const auto& d : result.diagnostics)
            PrintDiagnostic("refusal", d);
        } else {
          RequireOk(result, "random bulk");
          Require(result.inserted_rows == batch.size() && result.accepted_rows == batch.size() &&
                  result.rejected_rows == 0, "random bulk counts");
          model.insert(batch.begin(), batch.end());
          std::cout << "bulk_counts\t" << result.inserted_rows << '\t'
                    << result.accepted_rows << '\t' << result.rejected_rows << '\n';
        }
      } else {
        const bool present = model.contains(key);
        const std::string conflict_action = action == 0 ? "" : action == 1 ? "do_nothing" : "do_update";
        const auto result = Insert(writer, key, value, conflict_action);
        if (present && action == 0) {
          Require(!result.ok && !result.diagnostics.empty(), "random duplicate accepted");
          Require(result.diagnostics.front().code == "CLI.CONSTRAINT_UNIQUE_VIOLATION",
                  "random duplicate diagnostic");
          Require(result.inserted_count == 0 && result.updated_count == 0 && result.skipped_count == 0,
                  "random failed INSERT reported mutations");
          std::cout << "insert_refusal_counts\t" << result.inserted_count << '\t'
                    << result.updated_count << '\t' << result.skipped_count << '\n';
          for (const auto& d : result.diagnostics)
            PrintDiagnostic("refusal", d);
        } else {
          RequireOk(result, "random insert/upsert");
          Require(result.inserted_count == (present ? 0U : 1U) &&
                  result.updated_count == (present && action == 2 ? 1U : 0U) &&
                  result.skipped_count == (present && action == 1 ? 1U : 0U), "random insert counts");
          if (!present || action == 2) model[key] = value;
          std::cout << "insert_counts\t" << result.inserted_count << '\t'
                    << result.updated_count << '\t' << result.skipped_count << '\n';
        }
      }
      Observe(writer, "random_writer", model);
      Observe(reader, "random_snapshot", committed);
    }
    End(writer, commit);
    Observe(reader, "snapshot_after_finality", committed);
    End(reader, true);
    if (commit) committed = model;
  }
  std::ofstream expected(ids_path + ".expected");
  for (const auto& [key, value] : committed) expected << key << '\t' << value << '\n';
  expected.flush();
  ids.flush();
  Require(bool(expected) && bool(ids), "random expected state/correspondence write");
}
void CrashHistory(const std::string& path, const std::string& ids_path, bool committed) {
  auto writer = Begin(path, "crash-writer");
  auto inserted = Insert(writer, "8", "crash-value");
  RequireOk(inserted, "crash insert");
  Require(inserted.inserted_count == 1, "crash mutation affected count");
  if (committed) End(writer, true);
  {
    std::ofstream ids(ids_path);
    ids << "writer\t" << writer.local_transaction_id << '\t'
        << (committed ? "committed" : "rolled_back") << '\n';
    ids.flush();
    Require(bool(ids), "crash boundary correspondence");
  }
  // The engine operation and actual selected executor completed. Kill this
  // process without detach/destructors; native inventory owns classification.
#ifndef _WIN32
  std::raise(SIGKILL);
#endif
  std::_Exit(137);
}
int main(int argc, char** argv) {
  try {
    const auto memory = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
        scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "dml_optimization_history_probe");
    Require(memory.ok() && memory.fixture_mode, "memory fixture configuration");
    Require(argc >= 3, "usage: probe create|history|observe database [transaction-ids]");
    const std::string mode(argv[1]);
    if (mode == "create") Create(argv[2]);
    else if (mode == "history" && argc == 4) History(argv[2], argv[3]);
    else if (mode == "bulk" && argc == 4) BulkHistory(argv[2], argv[3]);
    else if (mode == "random" && argc == 5) RandomHistory(argv[2], argv[3], std::stoul(argv[4]));
    else if ((mode == "crash_before_commit" || mode == "crash_after_commit") && argc == 4) {
      CrashHistory(argv[2], argv[3], mode == "crash_after_commit");
    }
    else if (mode == "observe" || mode == "observe_bulk" || mode == "observe_crash_before_commit" || mode == "observe_crash_after_commit" || (mode == "observe_random" && argc == 4)) {
      db::DatabaseOpenConfig open;
      open.path = argv[2];
      const auto reopened = db::OpenDatabaseFile(open);
      if (!reopened.ok()) std::cerr << reopened.diagnostic.diagnostic_code << ':'
                                   << reopened.diagnostic.message_key << '\n';
      Require(reopened.ok(), "native database recovery/open");
      auto context = Begin(argv[2], "reopen-reader");
      Rows expected = mode == "observe" ? Rows{{"6", "changed"}, {"7", "new"}} :
          mode == "observe_bulk" ? Rows{{"6", "baseline"}, {"9", "nine"}, {"10", "ten"}} :
          mode == "observe_crash_after_commit" ? Rows{{"6", "baseline"}, {"8", "crash-value"}} :
          Rows{{"6", "baseline"}};
      if (mode == "observe_random") {
        std::ifstream input(argv[3]);
        Require(bool(input), "open randomized expected rows");
        expected.clear();
        std::string line;
        while (std::getline(input, line)) {
          const auto tab = line.find('\t');
          Require(tab != std::string::npos && expected.emplace(line.substr(0, tab), line.substr(tab + 1)).second,
                  "invalid randomized expected row");
        }
        Require(input.eof() && !expected.empty(), "read randomized expected rows");
      }
      Observe(context, "reopen", expected);
      End(context, true);
    } else throw std::runtime_error("invalid mode");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
