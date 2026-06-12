// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "descriptor_value_runtime.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "query/plan_api.hpp"
#include "sblr_aggregate_window_runtime.hpp"
#include "sblr_dispatch.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace exec = scratchbird::engine::executor;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000090901";
constexpr std::string_view kTableUuid = "019f0000-0000-7000-8000-000000090902";
constexpr std::string_view kIndexUuid = "019f0000-0000-7000-8000-000000090903";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_query_pivot_having_closure_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.domain_events",
                            ".sb.name_events",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock",
                            ".sb.mga_row_versions",
                            ".sb.mga_relation_metadata",
                            ".sb.mga_index_entries",
                            ".sb.mga_relation_descriptors",
                            ".sb.mga_large_values",
                            ".sb.mga_savepoints"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810909000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810909001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810909002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "query pivot/having closure database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-query-pivot-having-closure";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000090911";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000090912";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("right:DML_ROUTE_TEST");
  context.trace_tags.push_back("query_pivot_having_closure");
  return context;
}

api::EngineRequestContext BeginTransaction(api::EngineRequestContext context) {
  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(begin);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.ok, "transaction.begin failed for query pivot/having closure");
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  return context;
}

api::EngineDescriptor Descriptor(std::string type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue TypedValue(std::string type, std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = Descriptor(std::move(type));
  value.encoded_value = std::move(encoded);
  value.is_null = false;
  return value;
}

api::EngineTypedValue IntValue(std::int64_t value) {
  return TypedValue("int64", std::to_string(value));
}

api::EngineTypedValue TextValue(std::string value) {
  return TypedValue("text", std::move(value));
}

api::EngineRowValue Row(std::vector<std::pair<std::string, api::EngineTypedValue>> fields) {
  api::EngineRowValue row;
  row.fields = std::move(fields);
  return row;
}

api::EngineRowValue RelationRow(std::string uuid_text,
                                std::vector<std::pair<std::string, api::EngineTypedValue>> fields) {
  api::EngineRowValue row = Row(std::move(fields));
  row.requested_row_uuid.canonical = std::move(uuid_text);
  return row;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

std::string FirstDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field,
                       std::size_t row_index = 0) {
  if (row_index >= result.result_shape.rows.size()) { return {}; }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

std::string ResultValueAt(const api::EngineResultShape& shape,
                          std::size_t row_index,
                          std::size_t column_index) {
  Require(row_index < shape.rows.size(), "result row index out of range");
  Require(column_index < shape.rows[row_index].fields.size(), "result column index out of range");
  return shape.rows[row_index].fields[column_index].second.encoded_value;
}

api::EngineQueryRelation SalesRelation() {
  api::EngineQueryRelation relation;
  relation.relation_name = "sales";
  relation.descriptor_digest = "sales_descriptor";
  relation.rows = {
      Row({{"region", TextValue("north")}, {"channel", TextValue("retail")}, {"amount", IntValue(10)}}),
      Row({{"region", TextValue("north")}, {"channel", TextValue("retail")}, {"amount", IntValue(20)}}),
      Row({{"region", TextValue("north")}, {"channel", TextValue("wholesale")}, {"amount", IntValue(5)}}),
      Row({{"region", TextValue("south")}, {"channel", TextValue("retail")}, {"amount", IntValue(7)}}),
      Row({{"region", TextValue("south")}, {"channel", TextValue("wholesale")}, {"amount", IntValue(9)}}),
      Row({{"region", TextValue("south")}, {"channel", TextValue("wholesale")}, {"amount", IntValue(9)}}),
  };
  return relation;
}

api::EnginePlanOperationRequest BasePlanRequest() {
  api::EnginePlanOperationRequest request;
  request.context.security_context_present = true;
  request.execute = true;
  request.relations.push_back(SalesRelation());
  return request;
}

void RequirePivotAggregates() {
  struct Expected {
    std::string function;
    std::vector<std::vector<std::string>> rows;
  };
  const std::vector<Expected> cases = {
      {"sum", {{"north", "30", "5"}, {"south", "7", "18"}}},
      {"count", {{"north", "2", "1"}, {"south", "1", "2"}}},
      {"min", {{"north", "10", "5"}, {"south", "7", "9"}}},
      {"max", {{"north", "20", "5"}, {"south", "7", "9"}}},
      {"avg", {{"north", "15", "5"}, {"south", "7", "9"}}},
      {"count_distinct", {{"north", "2", "1"}, {"south", "1", "1"}}},
  };
  for (const auto& item : cases) {
    auto request = BasePlanRequest();
    request.query_operation = "pivot";
    request.option_envelopes = {
        "pivot_group_field:region",
        "pivot_for_field:channel",
        "pivot_value_field:amount",
        "pivot_in_values:retail,wholesale",
        "pivot_aggregate_function:" + item.function,
    };
    const auto result = api::EnginePlanOperation(request);
    if (!result.ok) { std::cerr << item.function << ':' << FirstDetail(result) << '\n'; }
    Require(result.ok, "supported pivot aggregate refused");
    Require(HasEvidence(result, "query_pivot_aggregate", item.function),
            "pivot aggregate evidence drifted");
    Require(result.result_shape.rows.size() == item.rows.size(),
            "pivot output row count drifted");
    for (std::size_t row = 0; row < item.rows.size(); ++row) {
      for (std::size_t column = 0; column < item.rows[row].size(); ++column) {
        Require(ResultValueAt(result.result_shape, row, column) == item.rows[row][column],
                "pivot aggregate result drifted");
      }
    }
  }

  auto unsupported = BasePlanRequest();
  unsupported.query_operation = "pivot";
  unsupported.option_envelopes = {
      "pivot_group_field:region",
      "pivot_for_field:channel",
      "pivot_value_field:amount",
      "pivot_in_values:retail",
      "pivot_aggregate_function:median",
  };
  const auto rejected = api::EnginePlanOperation(unsupported);
  Require(!rejected.ok && FirstDetail(rejected) == "query.plan_operation:query_plan_pivot_aggregate_unsupported",
          "unsupported pivot aggregate diagnostic drifted");
}

api::EngineQueryRelation HavingRelation() {
  api::EngineQueryRelation relation;
  relation.relation_name = "having_input";
  relation.descriptor_digest = "having_input_descriptor";
  relation.rows = {
      Row({{"grp", IntValue(1)}, {"amount", IntValue(3)}}),
      Row({{"grp", IntValue(1)}, {"amount", IntValue(4)}}),
      Row({{"grp", IntValue(2)}, {"amount", IntValue(10)}}),
      Row({{"grp", IntValue(3)}, {"amount", IntValue(1)}}),
  };
  return relation;
}

void RequireHavingPredicates() {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
      {"aggregate_gt", {"2"}},
      {"aggregate_gte", {"1", "2"}},
      {"aggregate_lt", {"3"}},
      {"aggregate_lte", {"1", "3"}},
      {"aggregate_eq", {"1"}},
      {"aggregate_ne", {"2", "3"}},
  };
  for (const auto& [predicate, expected_groups] : cases) {
    api::EnginePlanOperationRequest request;
    request.context.security_context_present = true;
    request.execute = true;
    request.query_operation = "aggregate";
    request.aggregate_function = "sum";
    request.group_key_column = 0;
    request.aggregate_value_column = 1;
    request.relations.push_back(HavingRelation());
    request.option_envelopes = {
        "having_predicate:" + predicate,
        "having_threshold:7",
        "having_value_column:1",
    };
    const auto result = api::EnginePlanOperation(request);
    if (!result.ok) { std::cerr << predicate << ':' << FirstDetail(result) << '\n'; }
    Require(result.ok, "supported aggregate HAVING predicate refused");
    Require(HasEvidence(result, "query_aggregate_having_predicate", predicate),
            "HAVING predicate evidence drifted");
    Require(result.result_shape.rows.size() == expected_groups.size(),
            "HAVING output row count drifted");
    for (std::size_t i = 0; i < expected_groups.size(); ++i) {
      Require(ResultValueAt(result.result_shape, i, 0) == expected_groups[i],
              "HAVING group result drifted");
    }
  }

  api::EnginePlanOperationRequest unsupported;
  unsupported.context.security_context_present = true;
  unsupported.execute = true;
  unsupported.query_operation = "aggregate";
  unsupported.relations.push_back(HavingRelation());
  unsupported.option_envelopes = {
      "having_predicate:aggregate_like",
      "having_threshold:7",
  };
  const auto rejected = api::EnginePlanOperation(unsupported);
  Require(!rejected.ok && FirstDetail(rejected) == "query.plan_operation:query_plan_aggregate_having_predicate_unsupported",
          "unsupported HAVING predicate diagnostic drifted");
}

void AddOptionOperand(sblr::SblrOperationEnvelope* envelope,
                      std::string name,
                      std::string value) {
  envelope->operands.push_back({"option", std::move(name), std::move(value)});
}

void AddRowFieldOperand(sblr::SblrOperationEnvelope* envelope,
                        std::string row_uuid,
                        std::string field_name,
                        std::string type,
                        std::string value) {
  envelope->operands.push_back({"row_field:" + std::move(type),
                                std::move(row_uuid) + "|" + std::move(field_name),
                                std::move(value)});
}

sblr::SblrOperationEnvelope QueryPlanEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.cbq009.query.plan_operation");
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  return envelope;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

void RequireSblrQueryPlanDispatch() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  auto envelope = QueryPlanEnvelope();
  AddOptionOperand(&envelope, "execute", "true");
  AddOptionOperand(&envelope, "query_operation", "aggregate");
  AddOptionOperand(&envelope, "group_key_column", "0");
  AddOptionOperand(&envelope, "aggregate_value_column", "1");
  AddOptionOperand(&envelope, "aggregate_function", "sum");
  AddOptionOperand(&envelope, "having_predicate", "aggregate_gte");
  AddOptionOperand(&envelope, "having_threshold", "7");
  AddRowFieldOperand(&envelope, "relation-0-row-a", "grp", "int64", "1");
  AddRowFieldOperand(&envelope, "relation-0-row-a", "amount", "int64", "3");
  AddRowFieldOperand(&envelope, "relation-0-row-b", "grp", "int64", "1");
  AddRowFieldOperand(&envelope, "relation-0-row-b", "amount", "int64", "4");
  AddRowFieldOperand(&envelope, "relation-0-row-c", "grp", "int64", "2");
  AddRowFieldOperand(&envelope, "relation-0-row-c", "amount", "int64", "10");
  const auto dispatched = sblr::DispatchSblrOperation({context, envelope, api::EngineApiRequest{}});
  if (!dispatched.api_result.ok) { PrintDispatchDiagnostics(dispatched); }
  Require(dispatched.envelope_validated && dispatched.accepted && dispatched.dispatched_to_api,
          "SBLR query.plan_operation dispatch did not reach engine API");
  Require(dispatched.api_result.ok, "SBLR query.plan_operation aggregate/HAVING route failed");
  Require(HasEvidence(dispatched.api_result, "query_aggregate_having_predicate", "aggregate_gte"),
          "SBLR query.plan_operation HAVING evidence drifted");
}

api::EngineLocalizedName Name(std::string text) {
  return {"en", "primary", "", std::move(text), true};
}

api::EngineColumnDefinition Column(std::string name, std::string type, std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical =
      "019f0000-0000-7000-8000-000000090" + std::to_string(920 + ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor = Descriptor(std::move(type));
  column.ordinal = ordinal;
  column.nullable = true;
  return column;
}

api::EngineIndexDefinition UniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = std::string(kIndexUuid);
  index.names.push_back(Name("cbq009_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes = {"unique", "id"};
  return index;
}

void CreateDmlTable(const api::EngineRequestContext& context) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid.canonical = std::string(kTableUuid);
  request.table_names.push_back(Name("cbq009_rows"));
  request.table_columns.push_back(Column("id", "int64", 0));
  request.table_columns.push_back(Column("category", "text", 1));
  request.table_columns.push_back(Column("name", "text", 2));
  request.table_columns.push_back(Column("secret_payload", "opaque_extension", 3));
  request.table_indexes.push_back(UniqueIdIndex());
  const auto result = api::EngineCreateTable(request);
  if (!result.ok) { std::cerr << FirstDetail(result) << '\n'; }
  Require(result.ok, "CBQ-009 create table failed");
  Require(HasEvidence(result, "mga_relation_metadata", "table_create"),
          "CBQ-009 create table MGA evidence missing");
}

api::EngineRowValue DmlRow(std::string row_uuid,
                           std::int64_t id,
                           std::string category,
                           std::string name) {
  return RelationRow(std::move(row_uuid),
                     {{"id", IntValue(id)},
                      {"category", TextValue(std::move(category))},
                      {"name", TextValue(std::move(name))}});
}

api::EngineInsertRowsResult InsertRows(const api::EngineRequestContext& context,
                                       std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = std::string(kTableUuid);
  request.target_table.object_kind = "table";
  request.input_rows = std::move(rows);
  request.require_generated_row_uuid = false;
  return api::EngineInsertRows(request);
}

void RequireDmlRowScanAndConflict(const api::EngineRequestContext& context) {
  auto inserted = InsertRows(context,
                             {DmlRow("019f0000-0000-7000-8000-000000090a01", 1, "alpha", "Ada"),
                              DmlRow("019f0000-0000-7000-8000-000000090a02", 2, "beta", "Bea"),
                              DmlRow("019f0000-0000-7000-8000-000000090a03", 3, "gamma", "Cy")});
  if (!inserted.ok) { std::cerr << FirstDetail(inserted) << '\n'; }
  Require(inserted.ok && inserted.inserted_count == 3, "CBQ-009 seed insert failed");

  api::EngineSelectRowsRequest select;
  select.context = context;
  select.source_object.uuid.canonical = std::string(kTableUuid);
  select.source_object.object_kind = "table";
  select.select_predicate.predicate_kind = "column_in_list";
  select.select_predicate.canonical_predicate_envelope = "category";
  select.select_predicate.bound_values = {TextValue("alpha"), TextValue("gamma")};
  auto selected = api::EngineSelectRows(select);
  if (!selected.ok) { std::cerr << FirstDetail(selected) << '\n'; }
  Require(selected.ok && selected.visible_count == 2,
          "DML select supported predicate did not row-scan without index");
  Require(HasEvidence(selected, "row_scan_predicate", "column_in_list"),
          "DML select row-scan evidence missing");

  api::EngineSelectRowsRequest opaque_select;
  opaque_select.context = context;
  opaque_select.source_object.uuid.canonical = std::string(kTableUuid);
  opaque_select.source_object.object_kind = "table";
  opaque_select.select_predicate.predicate_kind = "column_equals";
  opaque_select.select_predicate.canonical_predicate_envelope = "secret_payload";
  opaque_select.select_predicate.bound_values = {TextValue("redacted")};
  selected = api::EngineSelectRows(opaque_select);
  Require(!selected.ok && FirstDetail(selected) == "dml.select_rows:opaque_column_comparison_denied",
          "opaque column comparison diagnostic drifted");

  api::EngineInsertRowsRequest conflict;
  conflict.context = context;
  conflict.target_table.uuid.canonical = std::string(kTableUuid);
  conflict.target_table.object_kind = "table";
  conflict.input_rows = {DmlRow("019f0000-0000-7000-8000-000000090a04", 1, "alpha", "Noop")};
  conflict.require_generated_row_uuid = false;
  conflict.on_conflict_action = "do_nothing";
  conflict.conflict_target_column = "id";
  auto conflict_result = api::EngineInsertRows(conflict);
  if (!conflict_result.ok) { std::cerr << FirstDetail(conflict_result) << '\n'; }
  Require(conflict_result.ok && conflict_result.skipped_count == 1,
          "ON CONFLICT DO NOTHING route failed");
  Require(HasEvidence(conflict_result, "on_conflict_action", "do_nothing"),
          "ON CONFLICT DO NOTHING evidence missing");

  conflict.input_rows = {DmlRow("019f0000-0000-7000-8000-000000090a05", 1, "alpha", "Grace")};
  conflict.on_conflict_action = "do_update";
  conflict.conflict_update_columns = {"name"};
  conflict_result = api::EngineInsertRows(conflict);
  if (!conflict_result.ok) { std::cerr << FirstDetail(conflict_result) << '\n'; }
  Require(conflict_result.ok && conflict_result.updated_count == 1,
          "ON CONFLICT DO UPDATE route failed");
  Require(HasEvidence(conflict_result, "on_conflict_action", "do_update"),
          "ON CONFLICT DO UPDATE evidence missing");
  Require(FieldValue(conflict_result, "name") == "Grace",
          "ON CONFLICT DO UPDATE returning row drifted");

  api::EngineInsertRowsRequest reference;
  reference.context = context;
  reference.target_table.uuid.canonical = std::string(kTableUuid);
  reference.target_table.object_kind = "table";
  reference.input_rows = {DmlRow("019f0000-0000-7000-8000-000000090a06", 4, "delta", "Reference")};
  reference.require_generated_row_uuid = false;
  reference.reference_unique_checks_relaxed = true;
  const auto reference_result = api::EngineInsertRows(reference);
  Require(!reference_result.ok && FirstDetail(reference_result) == "dml.insert_rows:reference_relaxer_requires_engine_policy",
          "reference relaxer refusal diagnostic drifted");
}

exec::ExecutorColumnDescriptor ExecColumn(std::string name, std::string type) {
  return {std::move(name), exec::MakeExecutorDescriptor(std::move(type)), true};
}

exec::DescriptorTuple ExecTuple(std::vector<api::EngineTypedValue> values) {
  return {std::move(values)};
}

void RequireDescriptorQueryRuntime() {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  const auto batch = exec::MakeDescriptorBatch(
      {ExecColumn("id", "int64"),
       ExecColumn("category", "text"),
       ExecColumn("score", "real64"),
       ExecColumn("flag", "boolean"),
       ExecColumn("object_uuid", "uuid")},
      {ExecTuple({exec::EncodeInt64Value(1), exec::EncodeTextValue("alpha"), exec::EncodeReal64Value(1.5), exec::EncodeBoolValue(true), exec::MakeExecutorValue(exec::MakeExecutorDescriptor("uuid"), "019f0000-0000-7000-8000-000000090b01", false)}),
       ExecTuple({exec::EncodeInt64Value(2), exec::EncodeTextValue("beta"), exec::EncodeReal64Value(3.0), exec::EncodeBoolValue(false), exec::MakeExecutorValue(exec::MakeExecutorDescriptor("uuid"), "019f0000-0000-7000-8000-000000090b02", false)}),
       ExecTuple({exec::EncodeInt64Value(3), exec::EncodeTextValue("alpha"), exec::EncodeReal64Value(4.5), exec::EncodeBoolValue(true), exec::MakeExecutorValue(exec::MakeExecutorDescriptor("uuid"), "019f0000-0000-7000-8000-000000090b03", false)})});

  auto filtered = exec::FilterDescriptorBatchByComparison(batch,
                                                          2,
                                                          exec::DescriptorComparisonOperator::kGreaterThan,
                                                          exec::EncodeReal64Value(2.0),
                                                          &diagnostic);
  Require(diagnostic.ok && filtered.rows.size() == 2,
          "descriptor real64 greater-than filter failed");

  filtered = exec::FilterDescriptorBatchByComparison(batch,
                                                     1,
                                                     exec::DescriptorComparisonOperator::kEqual,
                                                     exec::EncodeTextValue("alpha"),
                                                     &diagnostic);
  Require(diagnostic.ok && filtered.rows.size() == 2,
          "descriptor text equality filter failed");

  const auto right = exec::MakeDescriptorBatch(
      {ExecColumn("category", "text"), ExecColumn("label", "text")},
      {ExecTuple({exec::EncodeTextValue("alpha"), exec::EncodeTextValue("A")}),
       ExecTuple({exec::EncodeTextValue("beta"), exec::EncodeTextValue("B")})});
  auto joined = exec::JoinDescriptorBatchesOnEqual(batch, right, 1, 0, &diagnostic);
  Require(diagnostic.ok && joined.rows.size() == 3,
          "descriptor text equality join failed");

  auto counted = exec::AggregateDescriptorCountByKey(batch, 1, "row_count", &diagnostic);
  Require(diagnostic.ok && counted.rows.size() == 2,
          "descriptor text count-by-key aggregate failed");

  counted = exec::AggregateDescriptorCountByKey(batch, 2, "score_count", &diagnostic);
  Require(diagnostic.ok && counted.rows.size() == 3,
          "descriptor real64 count-by-key aggregate failed");

  counted = exec::AggregateDescriptorCountByKey(batch, 3, "flag_count", &diagnostic);
  Require(diagnostic.ok && counted.rows.size() == 2,
          "descriptor boolean count-by-key aggregate failed");

  filtered = exec::FilterDescriptorBatchByComparison(batch,
                                                     4,
                                                     exec::DescriptorComparisonOperator::kEqual,
                                                     exec::MakeExecutorValue(exec::MakeExecutorDescriptor("uuid"), "019f0000-0000-7000-8000-000000090b01", false),
                                                     &diagnostic);
  Require(!diagnostic.ok && diagnostic.diagnostic_code == "SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED",
          "descriptor opaque filter diagnostic drifted");

  joined = exec::JoinDescriptorBatchesOnEqual(batch, batch, 4, 4, &diagnostic);
  Require(!diagnostic.ok && diagnostic.diagnostic_code == "SB_EXECUTOR_JOIN_TYPE_UNSUPPORTED",
          "descriptor opaque join diagnostic drifted");

  counted = exec::AggregateDescriptorCountByKey(batch, 4, "opaque_count", &diagnostic);
  Require(!diagnostic.ok && diagnostic.diagnostic_code == "SB_EXECUTOR_AGGREGATE_TYPE_UNSUPPORTED",
          "descriptor opaque aggregate diagnostic drifted");
}

sblr::SblrExecutionContext SblrContext() {
  sblr::SblrExecutionContext context;
  context.database_uuid = "CBQ-009-QUERY-PIVOT-HAVING-CLOSURE-db";
  context.transaction_uuid = "CBQ-009-QUERY-PIVOT-HAVING-CLOSURE-tx";
  context.transaction_context_present = true;
  return context;
}

sblr::SblrValue SblrInt(std::int64_t value) {
  sblr::SblrValue out;
  out.descriptor_id = "int64";
  out.payload_kind = sblr::SblrValuePayloadKind::signed_integer;
  out.is_null = false;
  out.has_int64_value = true;
  out.int64_value = value;
  out.encoded_value = std::to_string(value);
  out.text_value = out.encoded_value;
  return out;
}

sblr::SblrValue SblrReal(double value) {
  sblr::SblrValue out;
  out.descriptor_id = "real64";
  out.payload_kind = sblr::SblrValuePayloadKind::real64;
  out.is_null = false;
  out.has_real64_value = true;
  out.real64_value = value;
  out.encoded_value = std::to_string(value);
  out.text_value = out.encoded_value;
  return out;
}

sblr::SblrValue SblrBool(bool value) {
  sblr::SblrValue out;
  out.descriptor_id = "boolean";
  out.payload_kind = sblr::SblrValuePayloadKind::boolean;
  out.is_null = false;
  out.has_int64_value = true;
  out.int64_value = value ? 1 : 0;
  out.encoded_value = value ? "true" : "false";
  out.text_value = out.encoded_value;
  return out;
}

sblr::SblrValue SblrText(std::string value) {
  sblr::SblrValue out;
  out.descriptor_id = "text";
  out.payload_kind = sblr::SblrValuePayloadKind::text;
  out.is_null = false;
  out.encoded_value = std::move(value);
  out.text_value = out.encoded_value;
  return out;
}

std::vector<sblr::SblrValue> AggregateInputs(std::string_view function_id) {
  if (function_id == "every" || function_id == "bool_or") {
    return {SblrBool(true)};
  }
  if (function_id.starts_with("bit_")) {
    return {SblrInt(7)};
  }
  if (function_id == "string_agg" || function_id == "array_agg" ||
      function_id == "binary_agg" || function_id == "json_agg" ||
      function_id == "approx_count_distinct" || function_id == "top_k") {
    return {SblrText("alpha"), SblrInt(3)};
  }
  if (function_id == "json_object_agg") {
    return {SblrText("k"), SblrText("v")};
  }
  if (function_id == "approx_quantile") {
    return {SblrReal(10.0), SblrReal(0.5)};
  }
  if (function_id == "corr" || function_id == "covar_pop" ||
      function_id == "covar_samp" || function_id.starts_with("regr_")) {
    return {SblrReal(10.0), SblrReal(2.0)};
  }
  return {SblrReal(10.0)};
}

void RequireSblrAggregateWindowRuntime() {
  const auto context = SblrContext();
  const std::vector<std::string> aggregate_ids = {
      "count", "sum", "avg", "min", "max", "every", "bool_or",
      "variance", "variance_pop", "stddev", "stddev_pop", "corr",
      "covar_pop", "covar_samp", "regr_avgx", "regr_avgy",
      "regr_count", "regr_intercept", "regr_r2", "regr_slope",
      "regr_sxx", "regr_sxy", "regr_syy", "bit_and", "bit_or",
      "bit_xor", "string_agg", "array_agg", "binary_agg",
      "json_agg", "json_object_agg", "approx_count_distinct",
      "approx_quantile", "top_k"};
  for (const auto& function_id : aggregate_ids) {
    Require(sblr::IsSblrAggregateFunctionSupported(function_id),
            "registered aggregate id did not resolve as supported");
    sblr::SblrAggregateWindowState state;
    auto result = sblr::InitializeSblrAggregateState(function_id,
                                                     "019f0000-0000-7000-8000-000000090c01",
                                                     "result_descriptor",
                                                     context,
                                                     &state);
    Require(result.ok(), "registered aggregate initialize failed");
    sblr::SblrAggregateUpdateRequest update;
    update.context = context;
    update.values = AggregateInputs(function_id);
    result = sblr::UpdateSblrAggregateState(&state, update);
    if (!result.ok()) {
      for (const auto& diagnostic : result.diagnostics) {
        std::cerr << function_id << ':' << diagnostic.diagnostic_id << ':' << diagnostic.detail << '\n';
      }
    }
    Require(result.ok(), "registered aggregate update failed");
    sblr::SblrAggregateFinalizeRequest finalize;
    finalize.context = context;
    result = sblr::FinalizeSblrAggregateState(state, finalize);
    Require(result.ok() && result.scalar_values.size() == 1,
            "registered aggregate finalize failed");
  }

  sblr::SblrAggregateWindowState unknown_state;
  auto unknown = sblr::InitializeSblrAggregateState("aggregate.teleport",
                                                   "019f0000-0000-7000-8000-000000090c02",
                                                   "result_descriptor",
                                                   context,
                                                   &unknown_state);
  Require(!unknown.ok() && !unknown.diagnostics.empty() &&
              unknown.diagnostics.front().diagnostic_id == "SB_DIAG_AGGREGATE_KIND_UNSUPPORTED",
          "unknown aggregate diagnostic drifted");

  const std::vector<std::string> window_ids = {
      "row_number", "rank", "dense_rank", "percent_rank", "cume_dist",
      "ntile", "lag", "lead", "first_value", "last_value",
      "nth_value", "aggregate_as_window"};
  for (const auto& function_id : window_ids) {
    Require(sblr::IsSblrWindowFunctionSupported(function_id),
            "registered window id did not resolve as supported");
    sblr::SblrWindowFrameRequest request;
    request.context = context;
    request.function_id = function_id;
    request.function_uuid = "019f0000-0000-7000-8000-000000090c03";
    request.rows = {{{SblrInt(10)}, 0}, {{SblrInt(20)}, 1}, {{SblrInt(20)}, 1}};
    request.current_row_index = 1;
    request.frame_start_index = 0;
    request.frame_end_exclusive = request.rows.size();
    request.offset = 1;
    request.ntile_bucket_count = 2;
    request.nth = 2;
    request.aggregate_function_id = "sum";
    request.aggregate_function_uuid = "019f0000-0000-7000-8000-000000090c04";
    request.aggregate_result_descriptor_id = "int64";
    const auto result = sblr::EvaluateSblrWindowFunction(request);
    if (!result.ok()) {
      for (const auto& diagnostic : result.diagnostics) {
        std::cerr << function_id << ':' << diagnostic.diagnostic_id << ':' << diagnostic.detail << '\n';
      }
    }
    Require(result.ok() && result.scalar_values.size() == 1,
            "registered window function failed");
  }

  sblr::SblrWindowFrameRequest bad_window;
  bad_window.context = context;
  bad_window.function_id = "window.teleport";
  bad_window.function_uuid = "019f0000-0000-7000-8000-000000090c05";
  const auto bad = sblr::EvaluateSblrWindowFunction(bad_window);
  Require(!bad.ok() && !bad.diagnostics.empty() &&
              bad.diagnostics.front().diagnostic_id == "SB_DIAG_WINDOW_FUNCTION_UNSUPPORTED",
          "unknown window diagnostic drifted");
}

}  // namespace

int main() {
  RequirePivotAggregates();
  RequireHavingPredicates();
  RequireSblrQueryPlanDispatch();
  RequireDescriptorQueryRuntime();
  RequireSblrAggregateWindowRuntime();

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginTransaction(EngineContext(path, database_uuid));
  CreateDmlTable(context);
  RequireDmlRowScanAndConflict(context);
  std::cout << "sbsql_query_pivot_having_closure_conformance=passed\n";
  return EXIT_SUCCESS;
}
