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
  context.request_id = "sbsql-v3-dml-probe";
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

EngineColumnDefinition Column(std::string name, std::string type, std::uint32_t ordinal) {
  EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back({"en", "default", name, name, true});
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  return column;
}

EngineTypedValue Value(std::string value) {
  EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  return typed;
}

EngineRowValue Row(std::string row_uuid, std::string id, std::string name, std::string qty) {
  EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", Value(std::move(id))});
  row.fields.push_back({"name", Value(std::move(name))});
  row.fields.push_back({"qty", Value(std::move(qty))});
  return row;
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

bool HasOneName(const EngineSelectRowsResult& result, const std::string& name) {
  return result.ok && result.visible_count == 1 && result.result_shape.rows.size() == 1 &&
         FieldValue(result.result_shape.rows.front(), "name") == name;
}

EngineSelectRowsResult SelectAll(const EngineRequestContext& context, const EngineObjectReference& table) {
  EngineSelectRowsRequest request;
  request.context = context;
  request.source_object = table;
  return EngineSelectRows(request);
}

EngineSelectRowsResult SelectById(const EngineRequestContext& context, const EngineObjectReference& table, const std::string& id) {
  EngineSelectRowsRequest request;
  request.context = context;
  request.source_object = table;
  request.select_predicate.predicate_kind = "column_equals";
  request.select_predicate.canonical_predicate_envelope = "id";
  request.select_predicate.bound_values.push_back(Value(id));
  return EngineSelectRows(request);
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_dml_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = Base(args);
  const auto setup_tx = Begin(base);
  const auto setup = Tx(base, setup_tx);

  EngineCreateTableRequest create;
  create.context = setup;
  create.table_names.push_back({"en", "default", "inventory", "inventory", true});
  create.table_columns.push_back(Column("id", "text", 1));
  create.table_columns.push_back(Column("name", "text", 2));
  create.table_columns.push_back(Column("qty", "int32", 3));
  const auto table_result = EngineCreateTable(create);

  EngineCreateIndexRequest index;
  index.context = setup;
  index.target_object = table_result.table_object;
  EngineIndexDefinition index_definition;
  index_definition.names.push_back({"en", "default", "inventory_id_idx", "inventory_id_idx", true});
  index_definition.index_kind = "btree_unique";
  index_definition.key_envelopes.push_back("id");
  index.indexes.push_back(index_definition);
  const auto index_result = EngineCreateIndex(index);

  EngineInsertRowsRequest insert;
  insert.context = setup;
  insert.target_table = table_result.table_object;
  insert.input_rows.push_back(Row("00000000-0000-7000-8000-000000000903", "1", "widget", "7"));
  const auto insert_result = EngineInsertRows(insert);
  const bool insert_visible_in_tx = HasOneName(SelectAll(setup, table_result.table_object), "widget");
  const bool setup_commit = Commit(setup);

  const auto read_tx = Begin(base);
  const auto read = Tx(base, read_tx);
  const auto indexed_select = SelectById(read, table_result.table_object, "1");
  const bool committed_visible_and_indexed = HasOneName(indexed_select, "widget") && HasEvidence(indexed_select, "index_lookup");
  const bool read_commit = Commit(read);

  const auto update_tx = Begin(base);
  const auto update_context = Tx(base, update_tx);
  EngineUpdateRowsRequest update;
  update.context = update_context;
  update.target_table = table_result.table_object;
  update.update_predicate.predicate_kind = "row_uuid_match";
  update.update_predicate.canonical_predicate_envelope = "00000000-0000-7000-8000-000000000903";
  update.assignments.push_back({"name", Value("widget-updated")});
  const auto update_result = EngineUpdateRows(update);
  const bool update_visible = update_result.ok && update_result.updated_count == 1 &&
                              HasOneName(SelectAll(update_context, table_result.table_object), "widget-updated");
  const bool update_commit = Commit(update_context);

  const auto merge_insert_tx = Begin(base);
  const auto merge_insert_context = Tx(base, merge_insert_tx);
  EngineMergeRowsRequest merge_insert;
  merge_insert.context = merge_insert_context;
  merge_insert.target_object = table_result.table_object;
  merge_insert.predicate.predicate_kind = "row_uuid_match";
  merge_insert.predicate.canonical_predicate_envelope = "00000000-0000-7000-8000-000000000904";
  merge_insert.rows.push_back(Row("00000000-0000-7000-8000-000000000904", "2", "gadget", "4"));
  const auto merge_insert_result = EngineMergeRows(merge_insert);
  const bool merge_insert_ok = merge_insert_result.ok && merge_insert_result.inserted_count == 1 && merge_insert_result.merged_count == 1 &&
                               HasEvidence(merge_insert_result, "merge_action");
  const bool merge_insert_commit = Commit(merge_insert_context);

  const auto merge_update_tx = Begin(base);
  const auto merge_update_context = Tx(base, merge_update_tx);
  EngineMergeRowsRequest merge_update;
  merge_update.context = merge_update_context;
  merge_update.target_object = table_result.table_object;
  merge_update.predicate.predicate_kind = "row_uuid_match";
  merge_update.predicate.canonical_predicate_envelope = "00000000-0000-7000-8000-000000000904";
  merge_update.rows.push_back(Row("00000000-0000-7000-8000-000000000904", "2", "gadget-updated", "5"));
  const auto merge_update_result = EngineMergeRows(merge_update);
  const bool merge_update_ok = merge_update_result.ok && merge_update_result.updated_count == 1 && merge_update_result.merged_count == 1;
  const bool merge_update_commit = Commit(merge_update_context);

  const auto delete_tx = Begin(base);
  const auto delete_context = Tx(base, delete_tx);
  EngineDeleteRowsRequest del;
  del.context = delete_context;
  del.target_table = table_result.table_object;
  del.delete_predicate.predicate_kind = "row_uuid_match";
  del.delete_predicate.canonical_predicate_envelope = "00000000-0000-7000-8000-000000000903";
  const auto delete_result = EngineDeleteRows(del);
  const bool delete_commit = delete_result.ok && delete_result.deleted_count == 1 && Commit(delete_context);

  const auto final_tx = Begin(base);
  const auto final_context = Tx(base, final_tx);
  const auto final_all = SelectAll(final_context, table_result.table_object);
  const auto final_gadget = SelectById(final_context, table_result.table_object, "2");
  const bool final_state = final_all.ok && final_all.visible_count == 1 && HasOneName(final_gadget, "gadget-updated");
  const bool final_commit = Commit(final_context);

  const bool ok = setup_tx.ok && table_result.ok && index_result.ok && insert_result.ok && insert_visible_in_tx &&
                  setup_commit && read_tx.ok && committed_visible_and_indexed && read_commit && update_tx.ok &&
                  update_visible && update_commit && merge_insert_ok && merge_insert_commit && merge_update_ok &&
                  merge_update_commit && delete_commit && final_state && final_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("committed_visible_and_indexed", committed_visible_and_indexed, true);
  PrintBool("merge_insert_ok", merge_insert_ok, true);
  PrintBool("merge_update_ok", merge_update_ok, true);
  PrintBool("final_state", final_state, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
