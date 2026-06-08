// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
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

  InsertRows(fixture, fixture.left_table_uuid, 128, false);
  InsertRows(fixture, fixture.right_table_uuid, 512, true);
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

  return EXIT_SUCCESS;
}
