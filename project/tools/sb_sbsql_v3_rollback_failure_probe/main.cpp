// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "transaction/transaction_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args { std::string path; bool overwrite = false; };

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { args->overwrite = true; continue; }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; } else { return false; }
  }
  return !args->path.empty();
}

EngineRequestContext Base(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "sbsql-v3-rollback-failure-probe";
  context.database_path = args.path;
  return context;
}

EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  return base;
}

EngineBeginTransactionResult Begin(const EngineRequestContext& base) {
  EngineBeginTransactionRequest request;
  request.context = base;
  request.isolation_level = "read_committed";
  return EngineBeginTransaction(request);
}

bool Commit(const EngineRequestContext& context) {
  EngineCommitTransactionRequest request;
  request.context = context;
  return EngineCommitTransaction(request).ok;
}

bool Rollback(const EngineRequestContext& context) {
  EngineRollbackTransactionRequest request;
  request.context = context;
  return EngineRollbackTransaction(request).ok;
}

EngineColumnDefinition Column(std::string name) {
  EngineColumnDefinition column;
  column.ordinal = 1;
  column.names.push_back({"en", "default", name, name, true});
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  return column;
}

EngineTypedValue Value(std::string value) {
  EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  return typed;
}

EngineRowValue Row(std::string id, std::string name) {
  EngineRowValue row;
  row.fields.push_back({"id", Value(std::move(id))});
  row.fields.push_back({"name", Value(std::move(name))});
  return row;
}

EngineSelectRowsResult SelectAll(const EngineRequestContext& context, const EngineObjectReference& table) {
  EngineSelectRowsRequest request;
  request.context = context;
  request.source_object = table;
  return EngineSelectRows(request);
}

bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_rollback_failure_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = Base(args);
  const auto setup_tx = Begin(base);
  const auto setup = Tx(base, setup_tx);
  EngineCreateTableRequest create;
  create.context = setup;
  create.table_names.push_back({"en", "default", "rollback_probe", "rollback_probe", true});
  create.table_columns.push_back(Column("id"));
  create.table_columns.push_back(Column("name"));
  const auto table = EngineCreateTable(create);
  EngineCreateIndexRequest index;
  index.context = setup;
  index.target_object = table.table_object;
  EngineIndexDefinition index_definition;
  index_definition.names.push_back({"en", "default", "rollback_probe_id_idx", "rollback_probe_id_idx", true});
  index_definition.index_kind = "btree_unique";
  index_definition.key_envelopes.push_back("id");
  index.indexes.push_back(index_definition);
  const auto index_result = EngineCreateIndex(index);
  const bool setup_commit = Commit(setup);

  const auto tx = Begin(base);
  const auto context = Tx(base, tx);
  EngineInsertRowsRequest good;
  good.context = context;
  good.target_table = table.table_object;
  good.input_rows.push_back(Row("1", "persist-only-if-committed"));
  const auto good_insert = EngineInsertRows(good);

  EngineInsertRowsRequest partial_fail;
  partial_fail.context = context;
  partial_fail.target_table = table.table_object;
  partial_fail.input_rows.push_back(Row("2", "partial-before-failure"));
  partial_fail.input_rows.push_back(Row("2", "duplicate-failure"));
  const auto partial_result = EngineInsertRows(partial_fail);
  const bool partial_failure_detected = !partial_result.ok && HasDiagnosticCode(partial_result, "SB_ENGINE_API_INVALID_REQUEST");
  const bool partial_visible_in_tx = SelectAll(context, table.table_object).visible_count == 2;
  const bool rolled_back = Rollback(context);

  const auto read_tx = Begin(base);
  const auto read_context = Tx(base, read_tx);
  const auto after_rollback = SelectAll(read_context, table.table_object);
  const bool rollback_hid_partial_work = after_rollback.ok && after_rollback.visible_count == 0;
  const bool read_commit = Commit(read_context);

  const bool ok = setup_tx.ok && table.ok && index_result.ok && setup_commit && tx.ok && good_insert.ok &&
                  partial_failure_detected && partial_visible_in_tx && rolled_back && read_tx.ok &&
                  rollback_hid_partial_work && read_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("partial_failure_detected", partial_failure_detected, true);
  PrintBool("partial_visible_in_tx", partial_visible_in_tx, true);
  PrintBool("rollback_hid_partial_work", rollback_hid_partial_work, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
