// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "query/plan_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace {

struct CheckState {
  std::size_t ok = 0;
  std::size_t fail = 0;
};

void Check(CheckState* state, const std::string& name, bool condition) {
  if (condition) {
    ++state->ok;
    return;
  }
  ++state->fail;
  std::cerr << "FAIL " << name << "\n";
}

api::EngineRequestContext BaseContext(const std::string& path) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "engine-api-query-executor-probe";
  context.database_path = path;
  return context;
}

bool CreateProbeDatabase(const std::string& path) {
  constexpr std::uint64_t kCreationMillis = 1770000000000ULL;
  const auto database_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::database,
                                                        kCreationMillis + 100);
  const auto filespace_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::filespace,
                                                        kCreationMillis + 101);
  if (!database_uuid.ok() || !filespace_uuid.ok()) { return false; }

  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = kCreationMillis;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  return scratchbird::storage::database::CreateDatabaseFile(create).ok();
}

api::EngineRequestContext TxContext(api::EngineRequestContext base, const api::EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  base.transaction_isolation_level = tx.isolation_level;
  base.snapshot_visible_through_local_transaction_id = tx.snapshot_visible_through_local_transaction_id;
  return base;
}

api::EngineLocalizedName Name(const std::string& value) { return {"en", "default", value, value, true}; }

api::EngineColumnDefinition Column(const std::string& name, const std::string& type, std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.names.push_back(Name(name));
  column.descriptor.canonical_type_name = type;
  column.ordinal = ordinal;
  return column;
}

api::EngineTypedValue Value(const std::string& value) {
  api::EngineTypedValue typed;
  typed.encoded_value = value;
  typed.is_null = false;
  return typed;
}

api::EngineRowValue NumericCrudRow(const std::string& c0, const std::string& c1) {
  api::EngineRowValue row;
  row.fields.push_back({"c0", Value(c0)});
  row.fields.push_back({"c1", Value(c1)});
  return row;
}

api::EngineRowValue NumericRow(std::initializer_list<std::int64_t> values) {
  api::EngineRowValue row;
  std::size_t index = 0;
  for (const auto value : values) { row.fields.push_back({"c" + std::to_string(index++), Value(std::to_string(value))}); }
  return row;
}

api::EngineQueryRelation Relation(const std::string& name, std::initializer_list<api::EngineRowValue> rows) {
  api::EngineQueryRelation relation;
  relation.relation_name = name;
  relation.descriptor_digest = name;
  relation.rows.assign(rows.begin(), rows.end());
  return relation;
}

std::string FieldValue(const api::EnginePlanOperationResult& result, std::size_t row_index, const std::string& field) {
  if (row_index >= result.result_shape.rows.size()) { return {}; }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

api::EnginePlanOperationResult Execute(api::EngineRequestContext context,
                                       std::string operation,
                                       std::vector<api::EngineQueryRelation> relations) {
  api::EnginePlanOperationRequest request;
  request.context = std::move(context);
  request.execute = true;
  request.query_operation = std::move(operation);
  request.relations = std::move(relations);
  return api::EnginePlanOperation(request);
}

}  // namespace

int main(int argc, char** argv) {
  std::string path = "/tmp/sb_engine_api_query_executor_probe.db";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--path" && i + 1 < argc) { path = argv[++i]; }
  }

  std::filesystem::remove(path);
  std::filesystem::remove(path + ".sb.owner.lock");

  CheckState checks;
  Check(&checks, "database_create", CreateProbeDatabase(path));

  const auto base = BaseContext(path);
  api::EngineBeginTransactionRequest begin;
  begin.context = base;
  begin.isolation_level = "read_committed";
  const auto tx = api::EngineBeginTransaction(begin);
  Check(&checks, "begin_transaction", tx.ok);
  const auto context = TxContext(base, tx);

  api::EngineCreateTableRequest create_table;
  create_table.context = context;
  create_table.table_names.push_back(Name("query_source"));
  create_table.table_columns.push_back(Column("c0", "int64", 1));
  create_table.table_columns.push_back(Column("c1", "int64", 2));
  const auto table = api::EngineCreateTable(create_table);
  Check(&checks, "create_table", table.ok && !table.primary_object.uuid.canonical.empty());

  api::EngineInsertRowsRequest insert;
  insert.context = context;
  insert.target_table = table.primary_object;
  insert.input_rows.push_back(NumericCrudRow("1", "10"));
  insert.input_rows.push_back(NumericCrudRow("2", "20"));
  const auto inserted = api::EngineInsertRows(insert);
  Check(&checks, "insert_rows", inserted.ok && inserted.inserted_count == 2);

  api::EnginePlanOperationRequest crud_scan;
  crud_scan.context = context;
  crud_scan.execute = true;
  crud_scan.query_operation = "scan";
  crud_scan.target_object = table.primary_object;
  const auto crud_scan_result = api::EnginePlanOperation(crud_scan);
  Check(&checks, "crud_backed_scan", crud_scan_result.ok && crud_scan_result.output_row_count == 2);

  const auto join = Execute(context,
                            "join",
                            {Relation("left", {NumericRow({1, 100}), NumericRow({2, 200})}),
                             Relation("right", {NumericRow({1, 9}), NumericRow({3, 8})})});
  Check(&checks, "hash_join", join.ok && join.output_row_count == 1);

  api::EnginePlanOperationRequest aggregate;
  aggregate.context = context;
  aggregate.execute = true;
  aggregate.query_operation = "aggregate";
  aggregate.relations.push_back(Relation("agg", {NumericRow({1, 10}), NumericRow({1, 20}), NumericRow({2, 5})}));
  aggregate.group_key_column = 0;
  aggregate.aggregate_value_column = 1;
  const auto aggregate_result = api::EnginePlanOperation(aggregate);
  Check(&checks, "grouped_sum_aggregate", aggregate_result.ok && aggregate_result.output_row_count == 2);

  const auto window = Execute(context, "window", {Relation("window", {NumericRow({2}), NumericRow({1}), NumericRow({3})})});
  Check(&checks, "row_number_window", window.ok && window.output_row_count == 3);

  api::EnginePlanOperationRequest setop;
  setop.context = context;
  setop.execute = true;
  setop.query_operation = "set_operation";
  setop.set_operation = "except";
  setop.relations.push_back(Relation("set_left", {NumericRow({1}), NumericRow({2})}));
  setop.relations.push_back(Relation("set_right", {NumericRow({2})}));
  const auto setop_result = api::EnginePlanOperation(setop);
  Check(&checks, "except_distinct", setop_result.ok && setop_result.output_row_count == 1 && FieldValue(setop_result, 0, "c0") == "1");

  const auto recursive_cte = Execute(context, "recursive_cte", {Relation("seed", {NumericRow({7})})});
  Check(&checks, "recursive_cte_materialized", recursive_cte.ok && recursive_cte.output_row_count == 1);

  const auto subquery = Execute(context, "correlated_subquery", {Relation("subquery", {NumericRow({42})})});
  Check(&checks, "correlated_subquery_first_value", subquery.ok && subquery.output_row_count == 1 && FieldValue(subquery, 0, "c0") == "42");

  api::EnginePlanOperationRequest pipeline;
  pipeline.context = context;
  pipeline.execute = true;
  pipeline.query_operation = "scan";
  pipeline.relations.push_back(Relation("pipeline", {NumericRow({3, 30}), NumericRow({1, 10}), NumericRow({2, 20})}));
  pipeline.order_column = 0;
  pipeline.option_envelopes.push_back("order_column:0");
  pipeline.projected_columns.push_back(1);
  pipeline.offset = 1;
  pipeline.limit = 1;
  const auto pipeline_result = api::EnginePlanOperation(pipeline);
  Check(&checks, "order_project_limit_offset", pipeline_result.ok && pipeline_result.output_row_count == 1 &&
                                               FieldValue(pipeline_result, 0, "c0") == "20");

  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  const auto committed = api::EngineCommitTransaction(commit);
  Check(&checks, "commit_transaction", committed.ok);

  const bool ok = checks.fail == 0 && checks.ok >= 12;
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"ok_count\": " << checks.ok << ",\n";
  std::cout << "  \"fail_count\": " << checks.fail << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
