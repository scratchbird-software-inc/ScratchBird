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
#include "query/plan_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
  if (!condition) Fail(message);
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

template <typename TResult>
bool HasDiagnosticDetail(const TResult& result, std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(detail) != std::string::npos) return true;
  }
  return false;
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "DPC-065 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

api::EngineTypedValue Int64Value(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int64";
  typed.descriptor.encoded_descriptor = "canonical=int64";
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineRowValue Row(std::int64_t id, std::int64_t payload) {
  api::EngineRowValue row;
  row.fields.push_back({"bucket", Int64Value(id % 8)});
  row.fields.push_back({"id", Int64Value(id)});
  row.fields.push_back({"payload", Int64Value(payload)});
  return row;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) return true;
  }
  return false;
}

std::int64_t FieldI64(const api::EngineRowValue& row, std::size_t index) {
  Require(index < row.fields.size(), "DPC-065 field index out of range");
  return std::stoll(row.fields[index].second.encoded_value);
}

std::int64_t FieldI64ByName(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& [field_name, typed] : row.fields) {
    if (field_name == name) { return std::stoll(typed.encoded_value); }
  }
  Fail("DPC-065 named field missing");
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string left_table_uuid;
  std::string right_table_uuid;
  std::string window_table_uuid;
  api::EngineRequestContext context;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, 1000);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, 1001);
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

api::EngineRequestContext Begin(Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "DPC-065 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const std::string& table_uuid,
                           std::string default_name) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = table_uuid;
  table.default_name = std::move(default_name);
  table.columns.push_back({"id", "canonical=int64"});
  table.columns.push_back({"payload", "canonical=int64"});
  table.columns.push_back({"bucket", "canonical=int64"});
  return table;
}

void InsertRows(Fixture& fixture,
                const std::string& table_uuid,
                std::int64_t row_count,
                bool duplicate_keys) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(row_count));
  for (std::int64_t i = 0; i < row_count; ++i) {
    const std::int64_t id = duplicate_keys ? (i % 128) + 1 : i + 1;
    rows.push_back(Row(id, 1000 + i));
  }

  api::EngineInsertRowsRequest insert;
  insert.context = fixture.context;
  insert.context.request_id = "dpc065-query-relation-insert";
  insert.target_table.uuid.canonical = table_uuid;
  insert.target_table.object_kind = "table";
  insert.input_rows = std::move(rows);
  insert.estimated_row_count = insert.input_rows.size();
  const auto inserted = api::EngineInsertRows(insert);
  RequireOk(inserted, "DPC-065 fixture insert failed");
  Require(inserted.inserted_count == static_cast<api::EngineApiU64>(row_count),
          "DPC-065 fixture insert count mismatch");
}

api::EnginePredicateEnvelope ComparisonPredicate(std::string kind,
                                                 std::int64_t bound) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = std::move(kind);
  predicate.canonical_predicate_envelope = "id";
  predicate.bound_values.push_back(Int64Value(bound));
  return predicate;
}

api::EngineSelectRowsResult SelectComparison(Fixture& fixture,
                                             const std::string& table_uuid,
                                             std::string kind,
                                             std::int64_t bound) {
  api::EngineSelectRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = "dpc065-neutral-comparison-select-" + kind;
  request.source_object.uuid.canonical = table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate = ComparisonPredicate(std::move(kind), bound);
  return api::EngineSelectRows(request);
}

api::EngineUpdateRowsResult UpdateComparison(Fixture& fixture,
                                             const std::string& table_uuid,
                                             std::string kind,
                                             std::int64_t bound,
                                             std::string assignment_column,
                                             std::int64_t assignment_value) {
  api::EngineUpdateRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = "dpc065-neutral-comparison-update-" + kind;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = ComparisonPredicate(std::move(kind), bound);
  request.assignments.push_back(
      {std::move(assignment_column), Int64Value(assignment_value)});
  return api::EngineUpdateRows(request);
}

api::EngineDeleteRowsResult DeleteComparison(Fixture& fixture,
                                             const std::string& table_uuid,
                                             std::string kind,
                                             std::int64_t bound) {
  api::EngineDeleteRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = "dpc065-neutral-comparison-delete-" + kind;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate = ComparisonPredicate(std::move(kind), bound);
  return api::EngineDeleteRows(request);
}

api::EngineSelectRowsResult SelectAll(Fixture& fixture,
                                      const std::string& table_uuid,
                                      std::string request_id) {
  api::EngineSelectRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.source_object.uuid.canonical = table_uuid;
  request.source_object.object_kind = "table";
  return api::EngineSelectRows(request);
}

api::EngineUpdateRowsResult UpdateComparisonWindow(
    Fixture& fixture,
    const std::string& table_uuid,
    api::EngineApiU64 limit,
    api::EngineApiU64 offset) {
  api::EngineUpdateRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = "dpc065-neutral-update-row-window";
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = ComparisonPredicate("column_greater", 0);
  request.assignments.push_back({"payload", Int64Value(-300)});
  request.limit = limit;
  request.offset = offset;
  return api::EngineUpdateRows(request);
}

api::EngineDeleteRowsResult DeleteComparisonWindow(
    Fixture& fixture,
    const std::string& table_uuid,
    api::EngineApiU64 limit,
    api::EngineApiU64 offset) {
  api::EngineDeleteRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = "dpc065-neutral-delete-row-window";
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate = ComparisonPredicate("column_greater", 0);
  request.limit = limit;
  request.offset = offset;
  return api::EngineDeleteRows(request);
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_dpc065_query_relation_" + std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc065_query_relation.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, 10);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, 11);
  create.creation_unix_epoch_millis = NowMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DPC-065 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.left_table_uuid = NewUuidText(platform::UuidKind::object, 20);
  fixture.right_table_uuid = NewUuidText(platform::UuidKind::object, 21);
  fixture.window_table_uuid = NewUuidText(platform::UuidKind::object, 22);
  fixture.context = Begin(fixture, "dpc065-query-relation-metadata");

  const auto left_table =
      api::AppendMgaTableMetadata(fixture.context,
                                  Table(fixture,
                                        fixture.left_table_uuid,
                                        "dpc065_query_left"));
  Require(!left_table.error, "DPC-065 left table metadata append failed");
  const auto right_table =
      api::AppendMgaTableMetadata(fixture.context,
                                  Table(fixture,
                                        fixture.right_table_uuid,
                                        "dpc065_query_right"));
  Require(!right_table.error, "DPC-065 right table metadata append failed");
  const auto window_table =
      api::AppendMgaTableMetadata(fixture.context,
                                  Table(fixture,
                                        fixture.window_table_uuid,
                                        "dpc065_mutation_window"));
  Require(!window_table.error, "DPC-065 window table metadata append failed");

  InsertRows(fixture, fixture.left_table_uuid, 128, false);
  InsertRows(fixture, fixture.right_table_uuid, 512, true);
  InsertRows(fixture, fixture.window_table_uuid, 10, false);
  return fixture;
}

}  // namespace

int main() {
  auto fixture = MakeFixture();

  api::EnginePlanOperationRequest request;
  request.context = fixture.context;
  request.context.request_id = "dpc065-query-relation-join";
  request.execute = true;
  request.query_operation = "inner_join";
  request.join_algorithm = "hash";
  request.target_object.uuid.canonical = fixture.left_table_uuid;
  request.target_object.object_kind = "table";
  api::EngineObjectReference right;
  right.uuid.canonical = fixture.right_table_uuid;
  right.object_kind = "table";
  request.related_objects.push_back(std::move(right));
  request.left_key_field = "id";
  request.right_key_field = "id";

  const auto joined = api::EnginePlanOperation(request);
  RequireOk(joined, "DPC-065 descriptor-cached CRUD join failed");
  Require(joined.output_row_count == 512,
          "DPC-065 descriptor-cached CRUD join output count mismatch");
  Require(HasEvidence(joined, "query_join_algorithm", "hash"),
          "DPC-065 descriptor-cached CRUD join did not use hash route");
  Require(joined.result_shape.columns.size() == 6,
          "DPC-065 descriptor-cached CRUD join width mismatch");
  for (const auto& descriptor : joined.result_shape.columns) {
    Require(descriptor.canonical_type_name == "int64",
            "DPC-065 descriptor-cached CRUD join lost int64 descriptor");
  }
  for (const auto& row : joined.result_shape.rows) {
    Require(row.fields.size() == 6,
            "DPC-065 descriptor-cached CRUD join row width mismatch");
    Require(row.fields[0].second.descriptor.canonical_type_name == "int64",
            "DPC-065 joined row left id descriptor mismatch");
    Require(row.fields[3].second.descriptor.canonical_type_name == "int64",
            "DPC-065 joined row right id descriptor mismatch");
    const std::int64_t left_id = FieldI64(row, 0);
    const std::int64_t left_payload = FieldI64(row, 1);
    const std::int64_t left_bucket = FieldI64(row, 2);
    const std::int64_t right_id = FieldI64(row, 3);
    const std::int64_t right_bucket = FieldI64(row, 5);
    Require(left_id == right_id,
            "DPC-065 descriptor-cached CRUD join did not emit id columns in table order");
    Require(left_payload >= 1000,
            "DPC-065 descriptor-cached CRUD join did not emit payload in table order");
    Require(left_bucket == (left_id % 8) && right_bucket == (right_id % 8),
            "DPC-065 descriptor-cached CRUD join did not emit bucket columns in table order");
  }

  request.query_operation = "count_all";
  request.related_objects.clear();
  request.left_key_field.clear();
  request.right_key_field.clear();
  const auto counted = api::EnginePlanOperation(request);
  RequireOk(counted, "DPC-065 descriptor-cached CRUD count failed");
  Require(counted.output_row_count == 1,
          "DPC-065 descriptor-cached CRUD count output shape mismatch");
  Require(counted.result_shape.rows.front().fields.front().second.encoded_value == "128",
          "DPC-065 descriptor-cached CRUD count value mismatch");

  api::EngineSelectRowsRequest projected_count;
  projected_count.context = fixture.context;
  projected_count.context.request_id = "dpc065-neutral-count-projection";
  projected_count.source_object.uuid.canonical = fixture.left_table_uuid;
  projected_count.source_object.object_kind = "table";
  projected_count.select_predicate = ComparisonPredicate("column_less", 4);
  projected_count.option_envelopes.push_back("result_projection:count");
  projected_count.option_envelopes.push_back(
      "actual_column_name:FIREBIRD_COUNT");
  const auto selected_count = api::EngineSelectRows(projected_count);
  RequireOk(selected_count, "DPC-065 neutral count projection failed");
  Require(selected_count.visible_count == 1 &&
              selected_count.result_shape.rows.size() == 1,
          "DPC-065 neutral count projection result cardinality mismatch");
  Require(FieldI64ByName(selected_count.result_shape.rows.front(),
                         "FIREBIRD_COUNT") == 3,
          "DPC-065 neutral count projection value mismatch");
  Require(HasEvidence(selected_count, "dml_result_projection", "count") &&
              HasEvidence(selected_count, "row_scan_predicate", "column_less"),
          "DPC-065 neutral count projection bypassed MGA-visible row scan");

  api::EngineSelectRowsRequest select;
  select.context = fixture.context;
  select.context.request_id = "dpc065-bounded-predicate-order-select";
  select.source_object.uuid.canonical = fixture.left_table_uuid;
  select.source_object.object_kind = "table";
  select.select_predicate.predicate_kind = "column_equals";
  select.select_predicate.canonical_predicate_envelope = "id";
  select.select_predicate.bound_values.push_back(Int64Value(1));
  select.select_ordering.canonical_ordering_envelopes.push_back("id:desc");
  select.limit = 1;

  const auto bounded = api::EngineSelectRows(select);
  RequireOk(bounded, "DPC-065 bounded descriptor predicate/order select failed");
  Require(bounded.visible_count == 1,
          "DPC-065 bounded descriptor predicate/order select row count mismatch");
  Require(HasEvidence(bounded,
                      "row_scan_predicate",
                      "column_equals:bounded_order_limit"),
          "DPC-065 bounded descriptor predicate/order select did not use bounded scan");
  Require(FieldI64ByName(bounded.result_shape.rows.front(), "id") == 1,
          "DPC-065 bounded descriptor predicate/order select returned wrong id");
  Require(FieldI64ByName(bounded.result_shape.rows.front(), "payload") == 1000,
          "DPC-065 bounded descriptor predicate/order select returned wrong payload");

  for (const auto& [kind, expected_count] :
       std::vector<std::pair<std::string, api::EngineApiU64>>{
           {"column_less", 3},
           {"column_less_equal", 4},
           {"column_not_equals", 127}}) {
    const auto compared = SelectComparison(fixture, fixture.left_table_uuid, kind, 4);
    RequireOk(compared, "DPC-065 neutral comparison select failed");
    Require(compared.visible_count == expected_count,
            "DPC-065 neutral comparison select row count mismatch");
    Require(HasEvidence(compared, "row_scan_predicate", kind),
            "DPC-065 neutral comparison select did not use visible row scan");
  }

  api::EngineSelectRowsRequest select_always_false;
  select_always_false.context = fixture.context;
  select_always_false.context.request_id = "dpc065-neutral-always-false-select";
  select_always_false.source_object.uuid.canonical = fixture.left_table_uuid;
  select_always_false.source_object.object_kind = "table";
  select_always_false.select_predicate.predicate_kind = "always_false";
  const auto selected_always_false = api::EngineSelectRows(select_always_false);
  RequireOk(selected_always_false, "DPC-065 neutral always_false select failed");
  Require(selected_always_false.visible_count == 0 &&
              selected_always_false.result_shape.rows.empty(),
          "DPC-065 neutral always_false select matched rows");
  Require(HasEvidence(selected_always_false, "row_scan_predicate", "always_false"),
          "DPC-065 neutral always_false select did not use table scan");

  api::EngineUpdateRowsRequest update_always_false;
  update_always_false.context = fixture.context;
  update_always_false.context.request_id = "dpc065-neutral-always-false-update";
  update_always_false.target_table.uuid.canonical = fixture.left_table_uuid;
  update_always_false.target_table.object_kind = "table";
  update_always_false.update_predicate.predicate_kind = "always_false";
  update_always_false.assignments.push_back({"payload", Int64Value(-1)});
  const auto updated_always_false = api::EngineUpdateRows(update_always_false);
  RequireOk(updated_always_false, "DPC-065 neutral always_false update failed");
  Require(updated_always_false.matched_count == 0 &&
              updated_always_false.updated_count == 0,
          "DPC-065 neutral always_false update mutated rows");
  Require(HasEvidence(updated_always_false,
                      "update_target_access_kind",
                      "table_scan"),
          "DPC-065 neutral always_false update did not use table scan");

  api::EngineDeleteRowsRequest delete_always_false;
  delete_always_false.context = fixture.context;
  delete_always_false.context.request_id = "dpc065-neutral-always-false-delete";
  delete_always_false.target_table.uuid.canonical = fixture.left_table_uuid;
  delete_always_false.target_table.object_kind = "table";
  delete_always_false.delete_predicate.predicate_kind = "always_false";
  const auto deleted_always_false = api::EngineDeleteRows(delete_always_false);
  RequireOk(deleted_always_false, "DPC-065 neutral always_false delete failed");
  Require(deleted_always_false.matched_count == 0 &&
              deleted_always_false.deleted_count == 0,
          "DPC-065 neutral always_false delete mutated rows");
  Require(HasEvidence(deleted_always_false,
                      "delete_target_access_kind",
                      "table_scan"),
          "DPC-065 neutral always_false delete did not use table scan");

  const auto update_less = UpdateComparison(fixture,
                                            fixture.left_table_uuid,
                                            "column_less",
                                            3,
                                            "payload",
                                            -100);
  RequireOk(update_less, "DPC-065 neutral column_less update failed");
  Require(update_less.matched_count == 2 && update_less.updated_count == 2,
          "DPC-065 neutral column_less update count mismatch");
  Require(HasEvidence(update_less, "update_target_access_kind", "table_scan"),
          "DPC-065 neutral column_less update did not use MGA-visible table scan");

  const auto update_less_equal = UpdateComparison(fixture,
                                                  fixture.left_table_uuid,
                                                  "column_less_equal",
                                                  4,
                                                  "payload",
                                                  -200);
  RequireOk(update_less_equal,
            "DPC-065 neutral column_less_equal update failed");
  Require(update_less_equal.matched_count == 4 &&
              update_less_equal.updated_count == 4,
          "DPC-065 neutral column_less_equal update count mismatch");
  Require(HasEvidence(update_less_equal,
                      "update_target_access_kind",
                      "table_scan"),
          "DPC-065 neutral column_less_equal update did not use MGA-visible table scan");

  const auto update_not_equals = UpdateComparison(fixture,
                                                  fixture.left_table_uuid,
                                                  "column_not_equals",
                                                  4,
                                                  "bucket",
                                                  99);
  RequireOk(update_not_equals,
            "DPC-065 neutral column_not_equals update failed");
  Require(update_not_equals.matched_count == 127 &&
              update_not_equals.updated_count == 127,
          "DPC-065 neutral column_not_equals update count mismatch");
  Require(HasEvidence(update_not_equals,
                      "update_target_access_kind",
                      "table_scan"),
          "DPC-065 neutral column_not_equals update did not use MGA-visible table scan");

  const auto window_baseline = SelectAll(fixture,
                                         fixture.window_table_uuid,
                                         "dpc065-neutral-window-baseline");
  RequireOk(window_baseline, "DPC-065 neutral mutation window baseline failed");
  Require(window_baseline.result_shape.rows.size() == 10,
          "DPC-065 neutral mutation window baseline row count mismatch");
  std::vector<std::string> stable_row_uuids;
  stable_row_uuids.reserve(window_baseline.result_shape.rows.size());
  for (const auto& row : window_baseline.result_shape.rows) {
    stable_row_uuids.push_back(row.requested_row_uuid.canonical);
  }

  const auto update_window = UpdateComparisonWindow(fixture,
                                                    fixture.window_table_uuid,
                                                    3,
                                                    2);
  RequireOk(update_window, "DPC-065 neutral update row window failed");
  Require(update_window.matched_count == 3 && update_window.updated_count == 3,
          "DPC-065 neutral update row window count mismatch");
  Require(HasEvidence(update_window, "update_target_access_kind", "table_scan") &&
              HasEvidence(update_window,
                          "mutation_row_window_order",
                          "mga_visible_row_uuid_ascending"),
          "DPC-065 neutral update row window did not preserve stable MGA scan order");
  Require(update_window.result_shape.rows.size() == 3,
          "DPC-065 neutral update row window result count mismatch");
  for (std::size_t index = 0; index < 3; ++index) {
    Require(update_window.result_shape.rows[index].requested_row_uuid.canonical ==
                stable_row_uuids[index + 2],
            "DPC-065 neutral update row window selected an unstable row");
  }

  const auto delete_window = DeleteComparisonWindow(fixture,
                                                    fixture.window_table_uuid,
                                                    2,
                                                    4);
  RequireOk(delete_window, "DPC-065 neutral delete row window failed");
  Require(delete_window.matched_count == 2 && delete_window.deleted_count == 2,
          "DPC-065 neutral delete row window count mismatch");
  Require(HasEvidence(delete_window, "delete_target_access_kind", "table_scan") &&
              HasEvidence(delete_window,
                          "mutation_row_window_order",
                          "mga_visible_row_uuid_ascending"),
          "DPC-065 neutral delete row window did not preserve stable MGA scan order");
  Require(delete_window.result_shape.rows.size() == 2,
          "DPC-065 neutral delete row window result count mismatch");
  for (std::size_t index = 0; index < 2; ++index) {
    Require(delete_window.result_shape.rows[index].requested_row_uuid.canonical ==
                stable_row_uuids[index + 4],
            "DPC-065 neutral delete row window selected an unstable row");
  }

  const auto window_remaining = SelectAll(fixture,
                                          fixture.window_table_uuid,
                                          "dpc065-neutral-window-remaining");
  RequireOk(window_remaining, "DPC-065 neutral mutation window remainder failed");
  Require(window_remaining.visible_count == 8,
          "DPC-065 neutral delete row window changed the wrong row count");

  const auto offset_without_limit =
      UpdateComparisonWindow(fixture, fixture.window_table_uuid, 0, 1);
  Require(!offset_without_limit.ok &&
              HasDiagnosticDetail(offset_without_limit,
                                  "mutation_row_window_offset_requires_limit"),
          "DPC-065 neutral mutation offset without limit did not fail closed");

  api::EngineDeleteRowsRequest conflicting_window;
  conflicting_window.context = fixture.context;
  conflicting_window.context.request_id = "dpc065-neutral-window-batch-conflict";
  conflicting_window.target_table.uuid.canonical = fixture.window_table_uuid;
  conflicting_window.target_table.object_kind = "table";
  conflicting_window.delete_predicate = ComparisonPredicate("column_greater", 0);
  conflicting_window.delete_surface_variant = "batch_delete";
  conflicting_window.batch_limit_rows = 1;
  conflicting_window.limit = 1;
  const auto conflict = api::EngineDeleteRows(conflicting_window);
  Require(!conflict.ok &&
              HasDiagnosticDetail(conflict,
                                  "mutation_row_window_conflicts_with_batch_limit"),
          "DPC-065 neutral batch/window conflict did not fail closed");

  const auto delete_less = DeleteComparison(fixture,
                                            fixture.right_table_uuid,
                                            "column_less",
                                            2);
  RequireOk(delete_less, "DPC-065 neutral column_less delete failed");
  Require(delete_less.matched_count == 4 && delete_less.deleted_count == 4,
          "DPC-065 neutral column_less delete count mismatch");
  Require(HasEvidence(delete_less, "delete_target_access_kind", "table_scan"),
          "DPC-065 neutral column_less delete did not use MGA-visible table scan");

  const auto delete_less_equal = DeleteComparison(fixture,
                                                  fixture.right_table_uuid,
                                                  "column_less_equal",
                                                  3);
  RequireOk(delete_less_equal,
            "DPC-065 neutral column_less_equal delete failed");
  Require(delete_less_equal.matched_count == 8 &&
              delete_less_equal.deleted_count == 8,
          "DPC-065 neutral column_less_equal delete count mismatch");
  Require(HasEvidence(delete_less_equal,
                      "delete_target_access_kind",
                      "table_scan"),
          "DPC-065 neutral column_less_equal delete did not use MGA-visible table scan");

  const auto delete_not_equals = DeleteComparison(fixture,
                                                  fixture.right_table_uuid,
                                                  "column_not_equals",
                                                  128);
  RequireOk(delete_not_equals,
            "DPC-065 neutral column_not_equals delete failed");
  Require(delete_not_equals.matched_count == 496 &&
              delete_not_equals.deleted_count == 496,
          "DPC-065 neutral column_not_equals delete count mismatch");
  Require(HasEvidence(delete_not_equals,
                      "delete_target_access_kind",
                      "table_scan"),
          "DPC-065 neutral column_not_equals delete did not use MGA-visible table scan");

  return EXIT_SUCCESS;
}
