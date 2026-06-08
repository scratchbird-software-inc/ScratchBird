// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ddl/create_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/merge_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "database_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  std::string seed_pack_root;
  std::uint64_t creation_millis = 0;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; }
    else if (key == "--seed-pack-root") { args->seed_pack_root = value; }
    else if (key == "--creation-ms") { args->creation_millis = static_cast<std::uint64_t>(std::stoull(value)); }
    else { return false; }
  }
  return !args->path.empty() && !args->seed_pack_root.empty() && args->creation_millis != 0;
}

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "engine-api-crud-probe";
  context.database_path = args.path;
  return context;
}

bool CreateProbeDatabase(const Args& args) {
  if (args.overwrite) {
    std::filesystem::remove(args.path + ".sb.mga_row_versions");
    std::filesystem::remove(args.path + ".sb.mga_relation_metadata");
    std::filesystem::remove(args.path + ".sb.mga_index_entries");
    std::filesystem::remove(args.path + ".sb.mga_large_values");
    std::filesystem::remove(args.path + ".sb.mga_savepoints");
    std::filesystem::remove(args.path + ".sb.mga_relation_descriptors");
  }
  const auto database_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::database,
                                                        args.creation_millis + 10);
  const auto filespace_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::filespace,
                                                        args.creation_millis + 11);
  if (!database_uuid.ok()) {
    std::cerr << database_uuid.diagnostic.diagnostic_code << ":" << database_uuid.diagnostic.message_key << "\n";
    return false;
  }
  if (!filespace_uuid.ok()) {
    std::cerr << filespace_uuid.diagnostic.diagnostic_code << ":" << filespace_uuid.diagnostic.message_key << "\n";
    return false;
  }
  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = args.path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = args.creation_millis;
  create.resource_seed_pack_root = args.seed_pack_root;
  create.require_resource_seed_pack = true;
  create.allow_overwrite = args.overwrite;
  const auto created = scratchbird::storage::database::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << "\n";
    return false;
  }
  return true;
}

EngineRequestContext TxContext(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  base.transaction_isolation_level = tx.isolation_level;
  base.snapshot_visible_through_local_transaction_id = tx.snapshot_visible_through_local_transaction_id;
  return base;
}

EngineBeginTransactionResult Begin(const EngineRequestContext& base) {
  EngineBeginTransactionRequest request;
  request.context = base;
  request.isolation_level = "read_committed";
  return EngineBeginTransaction(request);
}

bool Commit(const EngineRequestContext& tx_context) {
  EngineCommitTransactionRequest request;
  request.context = tx_context;
  return EngineCommitTransaction(request).ok;
}

bool Rollback(const EngineRequestContext& tx_context) {
  EngineRollbackTransactionRequest request;
  request.context = tx_context;
  return EngineRollbackTransaction(request).ok;
}

EngineColumnDefinition Column(std::string name, std::string type, std::uint32_t ordinal) {
  EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back({"en", "default", name, name, true});
  column.descriptor.canonical_type_name = type;
  return column;
}

EngineTypedValue Value(std::string value) {
  EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  return typed;
}

EngineRowValue PersonRow(std::string id, std::string name, std::string age) {
  EngineRowValue row;
  row.fields.push_back({"id", Value(std::move(id))});
  row.fields.push_back({"name", Value(std::move(name))});
  row.fields.push_back({"age", Value(std::move(age))});
  return row;
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

EngineSelectRowsResult SelectAll(const EngineRequestContext& tx_context, const std::string& table_uuid) {
  EngineSelectRowsRequest request;
  request.context = tx_context;
  request.source_object.uuid.canonical = table_uuid;
  request.source_object.object_kind = "table";
  return EngineSelectRows(request);
}

bool HasSingleName(const EngineSelectRowsResult& result, const std::string& expected_name) {
  return result.ok && result.visible_count == 1 && result.result_shape.rows.size() == 1 &&
         FieldValue(result.result_shape.rows.front(), "name") == expected_name;
}

bool HasNoRows(const EngineSelectRowsResult& result) {
  return result.ok && result.visible_count == 0 && result.result_shape.rows.empty();
}

std::string FirstDiagnosticKey(const EngineApiResult& result) {
  if (result.diagnostics.empty()) { return {}; }
  return result.diagnostics.front().message_key;
}

std::string FirstDiagnosticDetail(const EngineApiResult& result) {
  if (result.diagnostics.empty()) { return {}; }
  return result.diagnostics.front().detail;
}

bool FileContainsText(const std::string& path, const std::string& needle) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { return false; }
  const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return body.find(needle) != std::string::npos;
}

bool CorruptFirstOverflowChunkPayload(const std::string& path) {
  std::ifstream input(path + ".sb.mga_large_values", std::ios::binary);
  if (!input) { return false; }
  std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  const std::string marker = "SBMGA1\tLARGE_VALUE_CHUNK\t";
  const std::size_t line_start = body.find(marker);
  if (line_start == std::string::npos) { return false; }
  const std::size_t line_end = body.find('\n', line_start);
  if (line_end == std::string::npos) { return false; }
  std::size_t tab_count = 0;
  std::size_t payload_start = std::string::npos;
  for (std::size_t i = line_start; i < line_end; ++i) {
    if (body[i] != '\t') { continue; }
    ++tab_count;
    if (tab_count == 5) {
      payload_start = i + 1;
      break;
    }
  }
  if (payload_start == std::string::npos || payload_start >= line_end || body[payload_start] == '\t') {
    return false;
  }
  body[payload_start] = body[payload_start] == 'X' ? 'Y' : 'X';
  std::ofstream output(path + ".sb.mga_large_values", std::ios::binary | std::ios::trunc);
  if (!output) { return false; }
  output.write(body.data(), static_cast<std::streamsize>(body.size()));
  return static_cast<bool>(output);
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_engine_api_crud_probe --path PATH --seed-pack-root PATH --creation-ms MILLIS [--overwrite]\n";
    return 2;
  }
  if (!CreateProbeDatabase(args)) {
    return 1;
  }

  const auto base = BaseContext(args);
  const auto tx1 = Begin(base);
  const auto tx1_context = TxContext(base, tx1);

  EngineCreateTableRequest create;
  create.context = tx1_context;
  create.table_names.push_back({"en", "default", "person", "person", true});
  create.table_columns.push_back(Column("id", "text", 1));
  create.table_columns.push_back(Column("name", "text", 2));
  create.table_columns.push_back(Column("age", "int32", 3));
  const auto create_result = EngineCreateTable(create);
  const std::string table_uuid = create_result.table_object.uuid.canonical;

  EngineInsertRowsRequest insert;
  insert.context = tx1_context;
  insert.target_table = create_result.table_object;
  insert.input_rows.push_back(PersonRow("1", "Ada", "37"));
  const auto insert_result = EngineInsertRows(insert);
  const std::string row_uuid = insert_result.row_uuids.empty() ? std::string{} : insert_result.row_uuids.front().canonical;

  const auto select_in_tx = SelectAll(tx1_context, table_uuid);
  const bool create_insert_visible_in_tx = create_result.ok && insert_result.ok && HasSingleName(select_in_tx, "Ada");
  const bool commit_1 = Commit(tx1_context);

  const auto read_tx = Begin(base);
  const auto read_context = TxContext(base, read_tx);
  const auto select_after_reopen = SelectAll(read_context, table_uuid);
  const bool committed_visible_after_reopen = HasSingleName(select_after_reopen, "Ada");
  const bool commit_read = Commit(read_context);

  const auto update_tx = Begin(base);
  const auto update_context = TxContext(base, update_tx);
  EngineUpdateRowsRequest update;
  update.context = update_context;
  update.target_table = create_result.table_object;
  update.update_predicate.predicate_kind = "row_uuid_match";
  update.update_predicate.canonical_predicate_envelope = row_uuid;
  update.assignments.push_back({"name", Value("Grace")});
  const auto update_result = EngineUpdateRows(update);
  const auto select_updated_in_tx = SelectAll(update_context, table_uuid);
  const bool update_visible_in_tx = update_result.ok && update_result.updated_count == 1 && HasSingleName(select_updated_in_tx, "Grace");
  const bool rollback_update = Rollback(update_context);

  const auto read_after_rollback_tx = Begin(base);
  const auto read_after_rollback_context = TxContext(base, read_after_rollback_tx);
  const auto select_after_rollback = SelectAll(read_after_rollback_context, table_uuid);
  const bool rollback_hides_update = HasSingleName(select_after_rollback, "Ada");
  const bool commit_read_after_rollback = Commit(read_after_rollback_context);

  const auto delete_tx = Begin(base);
  const auto delete_context = TxContext(base, delete_tx);
  EngineDeleteRowsRequest del;
  del.context = delete_context;
  del.target_table = create_result.table_object;
  del.delete_predicate.predicate_kind = "row_uuid_match";
  del.delete_predicate.canonical_predicate_envelope = row_uuid;
  const auto delete_result = EngineDeleteRows(del);
  const bool delete_commit = delete_result.ok && delete_result.deleted_count == 1 && Commit(delete_context);

  const auto final_read_tx = Begin(base);
  const auto final_read_context = TxContext(base, final_read_tx);
  const auto select_after_delete = SelectAll(final_read_context, table_uuid);
  const bool delete_hides_row = HasNoRows(select_after_delete);
  const bool commit_final_read = Commit(final_read_context);

  const auto merge_tx = Begin(base);
  const auto merge_context = TxContext(base, merge_tx);
  EngineRowValue merge_row = PersonRow("3", "Linus", "51");
  merge_row.requested_row_uuid.canonical = "probe_merge_row_uuid";
  EngineMergeRowsRequest merge_insert;
  merge_insert.context = merge_context;
  merge_insert.target_table = create_result.table_object;
  merge_insert.match_predicate.predicate_kind = "row_uuid_match";
  merge_insert.input_rows.push_back(merge_row);
  const auto merge_insert_result = EngineMergeRows(merge_insert);
  const bool merge_insert_ok = merge_insert_result.ok &&
                               merge_insert_result.inserted_count == 1 &&
                               merge_insert_result.result_shape.rows.size() == 1 &&
                               FieldValue(merge_insert_result.result_shape.rows.front(), "name") == "Linus";
  EngineRowValue merge_update_row = PersonRow("3", "Margaret", "52");
  merge_update_row.requested_row_uuid.canonical = "probe_merge_row_uuid";
  EngineMergeRowsRequest merge_update;
  merge_update.context = merge_context;
  merge_update.target_table = create_result.table_object;
  merge_update.match_predicate.predicate_kind = "row_uuid_match";
  merge_update.input_rows.push_back(merge_update_row);
  const auto merge_update_result = EngineMergeRows(merge_update);
  const bool merge_update_ok = merge_update_result.ok &&
                               merge_update_result.matched_count == 1 &&
                               merge_update_result.updated_count == 1 &&
                               merge_update_result.result_shape.rows.size() == 1 &&
                               FieldValue(merge_update_result.result_shape.rows.front(), "name") == "Margaret";
  EngineDeleteRowsRequest delete_merge_row;
  delete_merge_row.context = merge_context;
  delete_merge_row.target_table = create_result.table_object;
  delete_merge_row.delete_predicate.predicate_kind = "row_uuid_match";
  delete_merge_row.delete_predicate.canonical_predicate_envelope = "probe_merge_row_uuid";
  const auto delete_merge_result = EngineDeleteRows(delete_merge_row);
  const bool merge_cleanup_ok = delete_merge_result.ok && delete_merge_result.deleted_count == 1 && Commit(merge_context);

  const std::string large_name_1(9000, 'x');
  const std::string large_name_2(11000, 'y');

  const auto large_insert_tx = Begin(base);
  const auto large_insert_context = TxContext(base, large_insert_tx);
  EngineInsertRowsRequest large_insert;
  large_insert.context = large_insert_context;
  large_insert.target_table = create_result.table_object;
  large_insert.input_rows.push_back(PersonRow("2", large_name_1, "1"));
  const auto large_insert_result = EngineInsertRows(large_insert);
  const std::string large_row_uuid =
      large_insert_result.row_uuids.empty() ? std::string{} : large_insert_result.row_uuids.front().canonical;
  const auto select_large_in_tx = SelectAll(large_insert_context, table_uuid);
  const bool large_insert_visible_in_tx =
      large_insert_result.ok && large_insert_result.inserted_count == 1 && HasSingleName(select_large_in_tx, large_name_1);
  const bool commit_large_insert = Commit(large_insert_context);

  const auto large_read_tx = Begin(base);
  const auto large_read_context = TxContext(base, large_read_tx);
  const auto select_large_after_reopen = SelectAll(large_read_context, table_uuid);
  const bool large_insert_visible_after_reopen = HasSingleName(select_large_after_reopen, large_name_1);
  const bool commit_large_read = Commit(large_read_context);

  const auto large_update_tx = Begin(base);
  const auto large_update_context = TxContext(base, large_update_tx);
  EngineUpdateRowsRequest large_update;
  large_update.context = large_update_context;
  large_update.target_table = create_result.table_object;
  large_update.update_predicate.predicate_kind = "row_uuid_match";
  large_update.update_predicate.canonical_predicate_envelope = large_row_uuid;
  large_update.assignments.push_back({"name", Value(large_name_2)});
  const auto large_update_result = EngineUpdateRows(large_update);
  const auto select_large_updated_in_tx = SelectAll(large_update_context, table_uuid);
  const bool large_update_visible_in_tx =
      large_update_result.ok && large_update_result.updated_count == 1 && HasSingleName(select_large_updated_in_tx, large_name_2);
  const bool commit_large_update = Commit(large_update_context);

  const auto large_after_update_tx = Begin(base);
  const auto large_after_update_context = TxContext(base, large_after_update_tx);
  const auto select_large_after_update = SelectAll(large_after_update_context, table_uuid);
  const bool large_update_visible_after_reopen = HasSingleName(select_large_after_update, large_name_2);
  const bool commit_large_after_update = Commit(large_after_update_context);

  const bool overflow_events_persisted =
      FileContainsText(args.path + ".sb.mga_large_values", "SBMGA1\tLARGE_VALUE\t") &&
      FileContainsText(args.path + ".sb.mga_large_values", "SBMGA1\tLARGE_VALUE_CHUNK\t");

  const bool corrupted_overflow_applied = CorruptFirstOverflowChunkPayload(args.path);
  const auto corrupt_tx = Begin(base);
  bool corrupted_overflow_fail_closed = false;
  std::string corrupted_overflow_diagnostic;
  if (!corrupt_tx.ok) {
    corrupted_overflow_fail_closed = true;
    corrupted_overflow_diagnostic =
        corrupt_tx.diagnostics.empty() ? std::string{} : corrupt_tx.diagnostics.front().message_key;
  } else {
    const auto corrupt_context = TxContext(base, corrupt_tx);
    const auto corrupt_select = SelectAll(corrupt_context, table_uuid);
    corrupted_overflow_fail_closed = !corrupt_select.ok;
    corrupted_overflow_diagnostic =
        corrupt_select.diagnostics.empty() ? std::string{} : corrupt_select.diagnostics.front().message_key;
  }

  const bool ok = tx1.ok && create_insert_visible_in_tx && commit_1 && committed_visible_after_reopen && commit_read &&
                  update_visible_in_tx && rollback_update && rollback_hides_update && commit_read_after_rollback &&
                  delete_commit && delete_hides_row && commit_final_read && merge_insert_ok && merge_update_ok &&
                  merge_cleanup_ok && large_insert_visible_in_tx &&
                  commit_large_insert && large_insert_visible_after_reopen && commit_large_read &&
                  large_update_visible_in_tx && commit_large_update && large_update_visible_after_reopen &&
                  commit_large_after_update && overflow_events_persisted && corrupted_overflow_applied &&
                  corrupted_overflow_fail_closed;

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"tx1_ok\": " << (tx1.ok ? "true" : "false") << ",\n";
  std::cout << "  \"tx1_diagnostic\": \"" << FirstDiagnosticKey(tx1) << "\",\n";
  std::cout << "  \"create_ok\": " << (create_result.ok ? "true" : "false") << ",\n";
  std::cout << "  \"create_diagnostic\": \"" << FirstDiagnosticKey(create_result) << "\",\n";
  std::cout << "  \"insert_ok\": " << (insert_result.ok ? "true" : "false") << ",\n";
  std::cout << "  \"insert_diagnostic\": \"" << FirstDiagnosticKey(insert_result) << "\",\n";
  std::cout << "  \"insert_detail\": \"" << FirstDiagnosticDetail(insert_result) << "\",\n";
  std::cout << "  \"select_in_tx_ok\": " << (select_in_tx.ok ? "true" : "false") << ",\n";
  std::cout << "  \"select_in_tx_diagnostic\": \"" << FirstDiagnosticKey(select_in_tx) << "\",\n";
  std::cout << "  \"select_in_tx_detail\": \"" << FirstDiagnosticDetail(select_in_tx) << "\",\n";
  std::cout << "  \"create_insert_visible_in_tx\": " << (create_insert_visible_in_tx ? "true" : "false") << ",\n";
  std::cout << "  \"committed_visible_after_reopen\": " << (committed_visible_after_reopen ? "true" : "false") << ",\n";
  std::cout << "  \"update_visible_in_tx\": " << (update_visible_in_tx ? "true" : "false") << ",\n";
  std::cout << "  \"rollback_hides_update\": " << (rollback_hides_update ? "true" : "false") << ",\n";
  std::cout << "  \"delete_hides_row\": " << (delete_hides_row ? "true" : "false") << ",\n";
  std::cout << "  \"merge_insert_ok\": " << (merge_insert_ok ? "true" : "false") << ",\n";
  std::cout << "  \"merge_update_ok\": " << (merge_update_ok ? "true" : "false") << ",\n";
  std::cout << "  \"merge_cleanup_ok\": " << (merge_cleanup_ok ? "true" : "false") << ",\n";
  std::cout << "  \"large_insert_visible_in_tx\": " << (large_insert_visible_in_tx ? "true" : "false") << ",\n";
  std::cout << "  \"large_insert_visible_after_reopen\": " << (large_insert_visible_after_reopen ? "true" : "false") << ",\n";
  std::cout << "  \"large_update_visible_in_tx\": " << (large_update_visible_in_tx ? "true" : "false") << ",\n";
  std::cout << "  \"large_update_visible_after_reopen\": " << (large_update_visible_after_reopen ? "true" : "false") << ",\n";
  std::cout << "  \"overflow_events_persisted\": " << (overflow_events_persisted ? "true" : "false") << ",\n";
  std::cout << "  \"corrupted_overflow_applied\": " << (corrupted_overflow_applied ? "true" : "false") << ",\n";
  std::cout << "  \"corrupted_overflow_fail_closed\": " << (corrupted_overflow_fail_closed ? "true" : "false") << ",\n";
  std::cout << "  \"corrupted_overflow_diagnostic\": \"" << corrupted_overflow_diagnostic << "\",\n";
  std::cout << "  \"table_uuid\": \"" << table_uuid << "\",\n";
  std::cout << "  \"row_uuid\": \"" << row_uuid << "\",\n";
  std::cout << "  \"large_row_uuid\": \"" << large_row_uuid << "\"\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
