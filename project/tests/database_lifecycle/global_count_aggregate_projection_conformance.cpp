// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/global_aggregate_view.hpp"
#include "catalog/name_resolution_api.hpp"
#include "ddl/create_api.hpp"
#include "dml/global_aggregate_projection.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
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
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

std::uint64_t NowMillis() {
  static std::uint64_t sequence = 0;
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         (++sequence * 1000);
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind,
                                std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, salt);
  Require(generated.ok(), "global aggregate UUID generation failed");
  return generated.value;
}

std::string NewUuid(platform::UuidKind kind, std::uint64_t salt) {
  return uuid::UuidToString(NewTypedUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string principal_uuid;
  std::string values_table_uuid;
  std::string nulls_table_uuid;
  std::string expression_table_uuid;
  std::string schema_uuid;
  std::uint64_t salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

Fixture CreateFixture() {
  Fixture fixture;
  fixture.salt = NowMillis();
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_global_count_aggregate_" +
                       std::to_string(fixture.salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "global_count.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid =
      NewTypedUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid =
      NewTypedUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = fixture.salt + 3;
  create.page_size = 8192;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "global aggregate database creation failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.principal_uuid =
      NewUuid(platform::UuidKind::principal, fixture.salt + 4);
  fixture.values_table_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 5);
  fixture.nulls_table_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 6);
  fixture.expression_table_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 7);
  fixture.schema_uuid =
      NewUuid(platform::UuidKind::schema, fixture.salt + 8);
  return fixture;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::uint64_t ordinal,
                                std::string isolation = "read_committed") {
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id =
      "global-aggregate-begin-" + std::to_string(ordinal);
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid.canonical = fixture.database_uuid;
  begin.context.principal_uuid.canonical = fixture.principal_uuid;
  begin.context.session_uuid.canonical =
      NewUuid(platform::UuidKind::object, fixture.salt + 100 + ordinal);
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = 1;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = std::move(isolation);
  const auto begun = api::EngineBeginTransaction(begin);
  RequireOk(begun, "global aggregate transaction begin failed");

  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  RequireOk(api::EngineCommitTransaction(commit),
            "global aggregate transaction commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  RequireOk(api::EngineRollbackTransaction(rollback),
            "global aggregate transaction rollback failed");
}

api::CrudTableRecord Table(const api::EngineRequestContext& context,
                           std::string table_uuid,
                           std::string name) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = std::move(table_uuid);
  table.default_name = std::move(name);
  table.columns.push_back(
      {"value", "canonical=int64;precision=64;scale=0;nullable=true"});
  return table;
}

api::CrudTableRecord Int32Table(const api::EngineRequestContext& context,
                                std::string table_uuid,
                                std::string name) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = std::move(table_uuid);
  table.default_name = std::move(name);
  table.columns.push_back(
      {"value", "canonical=int32;precision=32;scale=0;nullable=true"});
  return table;
}

api::CrudTableRecord FirebirdIntegerTable(
    const api::EngineRequestContext& context,
    std::string table_uuid,
    std::string name) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = std::move(table_uuid);
  table.default_name = std::move(name);
  table.columns.push_back({"value", "type=integer;nullable=false"});
  return table;
}

api::EngineLocalizedName Name(std::string value) {
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

api::EngineTypedValue Int64Value(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int64";
  typed.descriptor.encoded_descriptor =
      "canonical=int64;precision=64;scale=0;nullable=true";
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineTypedValue Int32Value(std::int32_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int32";
  typed.descriptor.encoded_descriptor =
      "canonical=int32;precision=32;scale=0;nullable=true";
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineTypedValue FirebirdIntegerValue(std::int32_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "canonical_type_descriptor";
  typed.descriptor.canonical_type_name = "integer";
  typed.descriptor.encoded_descriptor = "type=integer;nullable=false";
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineTypedValue Int32LiteralValue(std::int32_t value) {
  api::EngineTypedValue typed;
  typed.descriptor =
      api::EngineGlobalAggregateExpressionInt32LiteralDescriptor();
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineTypedValue NullInt64Value() {
  auto typed = Int64Value(0);
  typed.encoded_value.clear();
  typed.is_null = true;
  typed.state = api::EngineValueState::sql_null;
  return typed;
}

api::EngineRowValue Row(std::uint64_t ordinal,
                        api::EngineTypedValue value) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical =
      NewUuid(platform::UuidKind::row, NowMillis() + ordinal);
  row.fields.push_back({"value", std::move(value)});
  return row;
}

void Insert(const api::EngineRequestContext& context,
            const std::string& table_uuid,
            std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest insert;
  insert.context = context;
  insert.context.request_id = "global-aggregate-insert";
  insert.target_table.uuid.canonical = table_uuid;
  insert.target_table.object_kind = "table";
  insert.input_rows = std::move(rows);
  insert.estimated_row_count = insert.input_rows.size();
  const auto inserted = api::EngineInsertRows(insert);
  RequireOk(inserted, "global aggregate row insert failed");
  Require(inserted.inserted_count == insert.estimated_row_count,
          "global aggregate row insert count drifted");
}

api::MgaRelationColumnStorageDescriptor ValueColumn(
    const api::EngineRequestContext& context,
    const std::string& table_uuid) {
  const auto loaded =
      api::LoadMgaRelationStorageDescriptor(context, table_uuid);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':'
              << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "global aggregate relation descriptor load failed");
  Require(loaded.descriptor.columns.size() == 1,
          "global aggregate relation descriptor column count drifted");
  return loaded.descriptor.columns.front();
}

api::EngineGlobalAggregateProjection CountStar(std::string alias) {
  api::EngineGlobalAggregateProjection projection;
  projection.operation = api::EngineGlobalAggregateOperation::count_star;
  projection.aggregate_function_uuid.canonical =
      std::string(api::EngineGlobalAggregateCountFunctionUuid());
  projection.output_alias = std::move(alias);
  projection.result_descriptor =
      api::EngineGlobalAggregateCountResultDescriptor();
  return projection;
}

api::EngineGlobalAggregateProjection CountField(
    api::EngineGlobalAggregateOperation operation,
    std::string alias,
    const api::MgaRelationColumnStorageDescriptor& column) {
  api::EngineGlobalAggregateProjection projection;
  projection.operation = operation;
  projection.aggregate_function_uuid.canonical =
      std::string(api::EngineGlobalAggregateCountFunctionUuid());
  projection.source_field.column_uuid = column.column_uuid;
  projection.source_field.value_descriptor = column.value_descriptor;
  projection.output_alias = std::move(alias);
  projection.result_descriptor =
      api::EngineGlobalAggregateCountResultDescriptor();
  return projection;
}

api::EngineGlobalAggregateProjection AvgField(
    api::EngineGlobalAggregateOperation operation,
    std::string alias,
    const api::MgaRelationColumnStorageDescriptor& column) {
  api::EngineGlobalAggregateProjection projection;
  projection.operation = operation;
  projection.aggregate_function_uuid.canonical =
      std::string(api::EngineGlobalAggregateAvgFunctionUuid());
  projection.source_field.column_uuid = column.column_uuid;
  projection.source_field.value_descriptor = column.value_descriptor;
  projection.output_alias = std::move(alias);
  const std::string& type = column.value_descriptor.canonical_type_name;
  projection.result_descriptor =
      (type == "int32" || type == "int64" || type == "integer" ||
       type == "bigint")
          ? api::EngineGlobalAggregateAvgIntegerResultDescriptor()
          : api::EngineGlobalAggregateAvgRealResultDescriptor();
  return projection;
}

api::EngineSelectRowsRequest AggregateRequest(
    const api::EngineRequestContext& context,
    const std::string& table_uuid,
    const api::MgaRelationColumnStorageDescriptor& column) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.context.request_id = "global-aggregate-select";
  request.source_object.uuid.canonical = table_uuid;
  request.source_object.object_kind = "table";
  const auto descriptor =
      api::LoadMgaRelationStorageDescriptor(context, table_uuid);
  Require(descriptor.ok,
          "global aggregate request relation descriptor load failed");
  request.global_aggregate_projection.relation_uuid =
      descriptor.descriptor.relation_uuid;
  request.global_aggregate_projection.relation_descriptor_uuid =
      descriptor.descriptor.descriptor_uuid;
  request.global_aggregate_projection.relation_descriptor_generation =
      descriptor.descriptor.descriptor_generation;
  request.global_aggregate_projection.outputs.push_back(
      CountStar("count_all"));
  request.global_aggregate_projection.outputs.push_back(CountField(
      api::EngineGlobalAggregateOperation::count_non_null_field,
      "count_value",
      column));
  request.global_aggregate_projection.outputs.push_back(CountField(
      api::EngineGlobalAggregateOperation::count_distinct_field,
      "count_distinct_value",
      column));
  return request;
}

api::EngineSelectRowsRequest AvgRequest(
    const api::EngineRequestContext& context,
    const std::string& table_uuid,
    const api::MgaRelationColumnStorageDescriptor& column) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.context.request_id = "global-avg-select";
  request.source_object.uuid.canonical = table_uuid;
  request.source_object.object_kind = "table";
  const auto descriptor =
      api::LoadMgaRelationStorageDescriptor(context, table_uuid);
  Require(descriptor.ok,
          "global AVG request relation descriptor load failed");
  request.global_aggregate_projection.relation_uuid =
      descriptor.descriptor.relation_uuid;
  request.global_aggregate_projection.relation_descriptor_uuid =
      descriptor.descriptor.descriptor_uuid;
  request.global_aggregate_projection.relation_descriptor_generation =
      descriptor.descriptor.descriptor_generation;
  request.global_aggregate_projection.outputs.push_back(AvgField(
      api::EngineGlobalAggregateOperation::avg_field,
      "avg_value",
      column));
  request.global_aggregate_projection.outputs.push_back(AvgField(
      api::EngineGlobalAggregateOperation::avg_distinct_field,
      "avg_distinct_value",
      column));
  return request;
}

std::string EvidenceValue(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return evidence.evidence_id;
  }
  return {};
}

void RequireCounts(const api::EngineSelectRowsResult& result,
                   std::int64_t expected_star,
                   std::int64_t expected_non_null,
                   std::int64_t expected_distinct,
                   std::string_view message) {
  RequireOk(result, message);
  Require(result.visible_count == 1 &&
              result.result_shape.rows.size() == 1 &&
              result.result_shape.columns.size() == 3,
          "global aggregate did not return exactly one three-column row");
  const auto& row = result.result_shape.rows.front();
  Require(row.fields.size() == 3,
          "global aggregate result row width drifted");
  const std::vector<std::string> aliases{
      "count_all", "count_value", "count_distinct_value"};
  const std::vector<std::int64_t> expected{
      expected_star, expected_non_null, expected_distinct};
  const auto canonical = api::EngineGlobalAggregateCountResultDescriptor();
  for (std::size_t index = 0; index < row.fields.size(); ++index) {
    Require(row.fields[index].first == aliases[index],
            "global aggregate output alias drifted");
    const auto& value = row.fields[index].second;
    Require(value.descriptor.descriptor_kind ==
                canonical.descriptor_kind &&
                value.descriptor.canonical_type_name ==
                    canonical.canonical_type_name &&
                value.descriptor.encoded_descriptor ==
                    canonical.encoded_descriptor &&
                result.result_shape.columns[index].encoded_descriptor ==
                    canonical.encoded_descriptor,
            "global aggregate count descriptor drifted");
    Require(!value.isSqlNull(),
            "global aggregate count unexpectedly returned SQL NULL");
    Require(value.encoded_value == std::to_string(expected[index]),
            "global aggregate count value drifted");
  }
  Require(EvidenceValue(result, "global_aggregate_relation_scan") ==
              "one_mga_visible_scan",
          "global aggregate one-scan evidence missing");
  Require(EvidenceValue(result, "global_aggregate_visible_rows_scanned") ==
              std::to_string(expected_star),
          "global aggregate visible scan count drifted");
  Require(EvidenceValue(result, "global_aggregate_function_uuid") ==
              api::EngineGlobalAggregateCountFunctionUuid(),
          "global aggregate canonical COUNT UUID evidence missing");
}

void RequireAvgs(const api::EngineSelectRowsResult& result,
                 std::optional<std::int64_t> expected_avg,
                 std::optional<std::int64_t> expected_distinct,
                 std::uint64_t expected_scanned,
                 std::string_view message) {
  RequireOk(result, message);
  Require(result.visible_count == 1 &&
              result.result_shape.rows.size() == 1 &&
              result.result_shape.columns.size() == 2 &&
              result.result_shape.rows.front().fields.size() == 2,
          "global AVG did not return exactly one two-column row");
  const auto canonical =
      api::EngineGlobalAggregateAvgIntegerResultDescriptor();
  const std::vector<std::string> aliases{
      "avg_value", "avg_distinct_value"};
  const std::vector<std::optional<std::int64_t>> expected{
      expected_avg, expected_distinct};
  const auto& fields = result.result_shape.rows.front().fields;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    Require(fields[index].first == aliases[index] &&
                fields[index].second.descriptor.descriptor_kind ==
                    canonical.descriptor_kind &&
                fields[index].second.descriptor.canonical_type_name ==
                    canonical.canonical_type_name &&
                fields[index].second.descriptor.encoded_descriptor ==
                    canonical.encoded_descriptor &&
                result.result_shape.columns[index].encoded_descriptor ==
                    canonical.encoded_descriptor,
            "global AVG nullable int64 descriptor drifted");
    if (!expected[index]) {
      Require(fields[index].second.isSqlNull() &&
                  fields[index].second.encoded_value.empty(),
              "empty/all-NULL AVG did not return SQL NULL");
    } else {
      Require(!fields[index].second.isSqlNull() &&
                  fields[index].second.encoded_value ==
                      std::to_string(*expected[index]),
              "global AVG integer final value drifted");
    }
  }
  Require(EvidenceValue(result, "global_aggregate_relation_scan") ==
              "one_mga_visible_scan" &&
              EvidenceValue(result,
                            "global_aggregate_visible_rows_scanned") ==
                  std::to_string(expected_scanned) &&
              EvidenceValue(result, "global_aggregate_function_uuid") ==
                  api::EngineGlobalAggregateAvgFunctionUuid(),
          "global AVG MGA scan/UUID evidence drifted");
}

void RequireRejected(const api::EngineSelectRowsResult& result,
                     std::string_view detail,
                     std::string_view message) {
  Require(!result.ok && !result.diagnostics.empty(), message);
  const std::string expected =
      "dml.global_aggregate_projection:" + std::string(detail);
  if (result.diagnostics.front().detail != expected) {
    std::cerr << "expected refusal=" << expected
              << " actual refusal="
              << result.diagnostics.front().detail << '\n';
  }
  Require(result.diagnostics.front().detail == expected,
          "global aggregate refusal detail drifted");
  Require(result.result_shape.rows.empty(),
          "global aggregate refusal produced rows");
}

void RequireExecutionRejectedBeforeScan(
    const api::EngineGlobalAggregateExecutionResult& result,
    std::string_view detail,
    std::string_view message) {
  Require(!result.ok && result.diagnostic.error &&
              result.result_shape.rows.empty() &&
              result.scanned_visible_row_count == 0,
          message);
  const std::string expected =
      "dml.global_aggregate_projection:" + std::string(detail);
  Require(result.diagnostic.detail == expected,
          "bound global aggregate refusal detail drifted");
}

bool HasDiagnosticDetail(const api::EngineApiResult& result,
                         std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail == detail ||
        diagnostic.detail.find(detail) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result,
                           std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

api::EngineCreateViewRequest GlobalAggregateViewRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& relation,
    std::string view_name,
    std::int32_t literal,
    std::string view_uuid = {},
    bool create_or_alter = false) {
  Require(relation.columns.size() == 1,
          "global aggregate view source descriptor width drifted");
  api::EngineCreateViewRequest request;
  request.context = context;
  request.context.request_id = create_or_alter
                                   ? "global-aggregate-view-alter"
                                   : "global-aggregate-view-create";
  request.operation_id = create_or_alter
                             ? "ddl.create_or_alter_view"
                             : "ddl.create_view";
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::move(view_uuid);
  request.target_object.object_kind = "view";
  request.localized_names.push_back(Name(std::move(view_name)));
  request.related_objects.push_back(
      {{fixture.expression_table_uuid}, "table"});
  api::EngineColumnDefinition source_column;
  source_column.requested_column_uuid = relation.columns.front().column_uuid;
  source_column.descriptor = relation.columns.front().value_descriptor;
  source_column.ordinal = 0;
  source_column.nullable = true;
  request.columns.push_back(std::move(source_column));
  request.assignments.push_back(
      {"int32_literal", Int32LiteralValue(literal)});
  request.descriptors.push_back(
      api::EngineGlobalAggregateExpressionInt64ResultDescriptor());
  request.descriptors.push_back(
      api::EngineGlobalAggregateAvgIntegerResultDescriptor());
  request.projection.canonical_projection_envelopes.push_back(
      api::kEngineGlobalAggregateViewInt32MultiplyV1);
  request.option_envelopes = {
      std::string("view_query_shape:") +
          api::kEngineGlobalAggregateViewMarkerV1,
      "source_relation_descriptor_uuid:" +
          relation.descriptor_uuid.canonical,
      "source_relation_descriptor_generation:" +
          std::to_string(relation.descriptor_generation),
      "aggregate_function_uuid:" +
          std::string(api::EngineGlobalAggregateAvgFunctionUuid()),
      "aggregate_result_alias:AVG_RESULT"};
  if (create_or_alter) {
    request.option_envelopes.push_back("create_or_alter:true");
  }
  return request;
}

api::EngineResolveNameRequest ResolveViewRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string view_name) {
  api::EngineResolveNameRequest request;
  request.context = context;
  request.context.request_id = "global-aggregate-view-resolve";
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_schema.object_kind = "schema";
  request.target_object.object_kind = "view";
  request.localized_names.push_back(Name(view_name));
  request.sql_object_reference.expected_object_type = "view";
  request.sql_object_reference.object_name.raw_text = std::move(view_name);
  request.sql_object_reference.object_name.identifier_profile_uuid =
      context.identifier_profile_uuid;
  return request;
}

api::EngineSelectRowsRequest GlobalAggregateViewSelectRequest(
    const api::EngineRequestContext& context,
    const api::EngineResolveNameResult& resolved) {
  Require(resolved.ok && resolved.semantic_projection.present,
          "global aggregate view semantic projection required");
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.context.request_id = "global-aggregate-view-select";
  request.source_object.uuid = resolved.bound_object_identity.object_uuid;
  request.source_object.object_kind = "view";
  request.select_projection.canonical_projection_envelopes.push_back(
      api::kEngineGlobalAggregateViewMarkerV1);
  request.descriptors.push_back(
      resolved.semantic_projection.projection_descriptor);
  return request;
}

void RequireGlobalAggregateViewValue(
    const api::EngineSelectRowsResult& result,
    std::string_view expected,
    std::uint64_t expected_generation,
    std::string_view message) {
  RequireOk(result, message);
  const auto canonical =
      api::EngineGlobalAggregateAvgIntegerResultDescriptor();
  Require(result.visible_count == 1 &&
              result.result_shape.rows.size() == 1 &&
              result.result_shape.rows.front().fields.size() == 1 &&
              result.result_shape.columns.size() == 1,
          "global aggregate view result shape drifted");
  const auto& field = result.result_shape.rows.front().fields.front();
  Require(field.first == "AVG_RESULT" && !field.second.isSqlNull() &&
              field.second.encoded_value == expected &&
              field.second.descriptor.descriptor_kind ==
                  canonical.descriptor_kind &&
              field.second.descriptor.canonical_type_name ==
                  canonical.canonical_type_name &&
              field.second.descriptor.encoded_descriptor ==
                  canonical.encoded_descriptor,
          "global aggregate view exact AVG value/descriptor drifted");
  Require(EvidenceValue(result, "global_aggregate_relation_scan") ==
                  "one_mga_visible_scan" &&
              EvidenceValue(result,
                            "global_aggregate_visible_rows_scanned") == "4" &&
              EvidenceValue(result, "global_aggregate_view_marker") ==
                  api::kEngineGlobalAggregateViewMarkerV1 &&
              EvidenceValue(
                  result,
                  "global_aggregate_view_descriptor_generation") ==
                  std::to_string(expected_generation) &&
              EvidenceValue(result, "global_aggregate_view_parser_sql") ==
                  "false",
          "global aggregate view one-scan/authority evidence drifted");
}

api::EngineGlobalAggregateProjection CheckedInt32MultiplyAvg(
    const api::MgaRelationColumnStorageDescriptor& column,
    std::string literal) {
  api::EngineGlobalAggregateProjection projection;
  projection.operation = api::EngineGlobalAggregateOperation::avg_field;
  projection.aggregate_function_uuid.canonical =
      std::string(api::EngineGlobalAggregateAvgFunctionUuid());
  projection.source_field.column_uuid = column.column_uuid;
  projection.source_field.value_descriptor = column.value_descriptor;
  projection.input_expression.kind =
      api::EngineGlobalAggregateInputExpressionKind::
          int32_literal_times_int32_field_to_int64;
  projection.input_expression.int32_literal.descriptor =
      api::EngineGlobalAggregateExpressionInt32LiteralDescriptor();
  projection.input_expression.int32_literal.encoded_value =
      std::move(literal);
  projection.input_expression.result_descriptor =
      api::EngineGlobalAggregateExpressionInt64ResultDescriptor();
  projection.output_alias = "AVG_RESULT";
  projection.result_descriptor =
      api::EngineGlobalAggregateAvgIntegerResultDescriptor();
  return projection;
}

void TestBoundIdentityAndTypedIntegerDistinct(
    const api::EngineRequestContext& context,
    const std::string& table_uuid,
    const api::MgaRelationColumnStorageDescriptor& column) {
  const auto loaded =
      api::LoadMgaRelationStorageDescriptor(context, table_uuid);
  Require(loaded.ok, "typed DISTINCT relation descriptor load failed");
  const auto request = AggregateRequest(context, table_uuid, column);
  const auto bound = api::BindGlobalAggregateProjectionEnvelope(
      request.global_aggregate_projection, loaded.descriptor);
  Require(bound.ok && bound.outputs.size() == 3 &&
              bound.outputs[1].source_field.column_uuid.canonical ==
                  column.column_uuid.canonical &&
              bound.outputs[1].source_field.value_descriptor.descriptor_uuid
                      .canonical ==
                  column.value_descriptor.descriptor_uuid.canonical,
          "bound aggregate dropped the field UUID or descriptor");

  auto row = [](std::string field_name, std::string value) {
    api::CrudRowVersionRecord record;
    record.values.push_back({std::move(field_name), std::move(value)});
    return record;
  };
  const std::vector<api::CrudRowVersionRecord> typed_rows{
      row("value", "1"),
      row("value", "01"),
      row("value", "+1"),
      row("value", "0"),
      row("value", "-0"),
      row("value", "<NULL>")};
  const auto typed = api::ExecuteGlobalAggregateProjection(
      bound.outputs, loaded.descriptor, typed_rows);
  Require(typed.ok && typed.scanned_visible_row_count == 6 &&
              typed.result_shape.rows.size() == 1 &&
              typed.result_shape.rows[0].fields.size() == 3 &&
              typed.result_shape.rows[0].fields[0].second.encoded_value ==
                  "6" &&
              typed.result_shape.rows[0].fields[1].second.encoded_value ==
                  "5" &&
              typed.result_shape.rows[0].fields[2].second.encoded_value ==
                  "2",
          "typed int64 DISTINCT did not canonicalize equivalent encodings");

  auto int32_relation = loaded.descriptor;
  int32_relation.columns[0].value_descriptor.canonical_type_name = "int32";
  int32_relation.columns[0].value_descriptor.encoded_descriptor =
      "canonical=int32;precision=32;scale=0;nullable=true";
  auto int32_outputs = bound.outputs;
  int32_outputs[1].source_field.value_descriptor =
      int32_relation.columns[0].value_descriptor;
  int32_outputs[2].source_field.value_descriptor =
      int32_relation.columns[0].value_descriptor;
  const std::vector<api::CrudRowVersionRecord> int32_rows{
      row("value", "+0001"),
      row("value", "1"),
      row("value", "-0"),
      row("value", "0"),
      row("value", "2147483647"),
      row("value", "02147483647"),
      row("value", "-2147483648"),
      row("value", "-02147483648")};
  const auto typed_int32 = api::ExecuteGlobalAggregateProjection(
      int32_outputs, int32_relation, int32_rows);
  Require(typed_int32.ok &&
              typed_int32.result_shape.rows.size() == 1 &&
              typed_int32.result_shape.rows[0].fields[2]
                  .second.encoded_value == "4",
          "typed int32 DISTINCT canonicalization or range boundary drifted");

  auto firebird_integer_relation = loaded.descriptor;
  firebird_integer_relation.columns[0].value_descriptor
      .canonical_type_name = "integer";
  firebird_integer_relation.columns[0].value_descriptor
      .encoded_descriptor = "type=integer";
  auto firebird_integer_outputs = bound.outputs;
  firebird_integer_outputs[1].source_field.value_descriptor =
      firebird_integer_relation.columns[0].value_descriptor;
  firebird_integer_outputs[2].source_field.value_descriptor =
      firebird_integer_relation.columns[0].value_descriptor;
  const auto firebird_integer = api::ExecuteGlobalAggregateProjection(
      firebird_integer_outputs,
      firebird_integer_relation,
      {row("value", "0"),
       row("value", "00"),
       row("value", "1"),
       row("value", "+01")});
  Require(firebird_integer.ok &&
              firebird_integer.result_shape.rows.size() == 1 &&
              firebird_integer.result_shape.rows[0].fields[2]
                      .second.encoded_value == "2",
          "persisted Firebird INTEGER descriptor was not admitted as int32");

  const std::vector<api::CrudRowVersionRecord> invalid_integer_row{
      row("value", "1x")};
  const auto invalid_integer = api::ExecuteGlobalAggregateProjection(
      bound.outputs, loaded.descriptor, invalid_integer_row);
  Require(!invalid_integer.ok && invalid_integer.result_shape.rows.empty() &&
              invalid_integer.scanned_visible_row_count == 1 &&
              invalid_integer.diagnostic.detail ==
                  "dml.global_aggregate_projection:"
                  "global_aggregate_distinct_integer_value_invalid",
          "typed DISTINCT accepted invalid integer syntax");

  const std::vector<api::CrudRowVersionRecord> out_of_range_row{
      row("value", "9223372036854775808")};
  const auto out_of_range = api::ExecuteGlobalAggregateProjection(
      bound.outputs, loaded.descriptor, out_of_range_row);
  Require(!out_of_range.ok && out_of_range.result_shape.rows.empty() &&
              out_of_range.scanned_visible_row_count == 1 &&
              out_of_range.diagnostic.detail ==
                  "dml.global_aggregate_projection:"
                  "global_aggregate_distinct_integer_value_invalid",
          "typed DISTINCT accepted an out-of-range int64 value");

  api::CrudRowVersionRecord duplicate_key_row;
  duplicate_key_row.values.push_back({"value", "1"});
  duplicate_key_row.values.push_back({"value", "2"});
  const auto duplicate_key = api::ExecuteGlobalAggregateProjection(
      bound.outputs, loaded.descriptor, {duplicate_key_row});
  Require(!duplicate_key.ok && duplicate_key.result_shape.rows.empty() &&
              duplicate_key.scanned_visible_row_count == 1 &&
              duplicate_key.diagnostic.detail ==
                  "dml.global_aggregate_projection:"
                  "global_aggregate_source_field_duplicate_in_visible_row",
          "bound aggregate execution accepted duplicate exact field keys");

  auto uuid_drift = bound.outputs;
  uuid_drift[1].source_field.column_uuid.canonical =
      NewUuid(platform::UuidKind::object, NowMillis());
  RequireExecutionRejectedBeforeScan(
      api::ExecuteGlobalAggregateProjection(
          uuid_drift, loaded.descriptor, typed_rows),
      "bound_global_aggregate_source_field_not_found",
      "bound aggregate execution accepted a drifted field UUID");

  auto descriptor_drift = bound.outputs;
  descriptor_drift[2].source_field.value_descriptor.encoded_descriptor +=
      ";drift=true";
  RequireExecutionRejectedBeforeScan(
      api::ExecuteGlobalAggregateProjection(
          descriptor_drift, loaded.descriptor, typed_rows),
      "bound_global_aggregate_source_field_descriptor_mismatch",
      "bound aggregate execution accepted a drifted field descriptor");

  auto unsupported_relation = loaded.descriptor;
  unsupported_relation.columns[0].value_descriptor.canonical_type_name =
      "text";
  unsupported_relation.columns[0].value_descriptor.encoded_descriptor =
      "canonical=text;nullable=true";
  auto unsupported_outputs = bound.outputs;
  unsupported_outputs[1].source_field.value_descriptor =
      unsupported_relation.columns[0].value_descriptor;
  unsupported_outputs[2].source_field.value_descriptor =
      unsupported_relation.columns[0].value_descriptor;
  RequireExecutionRejectedBeforeScan(
      api::ExecuteGlobalAggregateProjection(
          unsupported_outputs, unsupported_relation, typed_rows),
      "global_aggregate_distinct_type_unsupported",
      "bound aggregate execution accepted unsupported DISTINCT typing");

  const std::vector<api::CrudRowVersionRecord> folded_name_row{
      row("VALUE", "1")};
  const auto folded = api::ExecuteGlobalAggregateProjection(
      bound.outputs, loaded.descriptor, folded_name_row);
  Require(!folded.ok && folded.result_shape.rows.empty() &&
              folded.scanned_visible_row_count == 1 &&
              folded.diagnostic.detail ==
                  "dml.global_aggregate_projection:"
                  "global_aggregate_source_field_missing_from_visible_row",
          "bound aggregate execution used a case-folded field-name fallback");
}

void TestAvgTypedFinalizationAndRefusals(
    const api::EngineRequestContext& context,
    const std::string& table_uuid,
    const api::MgaRelationColumnStorageDescriptor& column) {
  const auto loaded =
      api::LoadMgaRelationStorageDescriptor(context, table_uuid);
  Require(loaded.ok, "typed AVG relation descriptor load failed");
  const auto request = AvgRequest(context, table_uuid, column);
  const auto bound = api::BindGlobalAggregateProjectionEnvelope(
      request.global_aggregate_projection, loaded.descriptor);
  Require(bound.ok && bound.outputs.size() == 2 &&
              bound.outputs[0].aggregate_function_uuid.canonical ==
                  api::EngineGlobalAggregateAvgFunctionUuid() &&
              bound.outputs[0].source_field.column_uuid.canonical ==
                  column.column_uuid.canonical &&
              bound.outputs[0].result_descriptor.encoded_descriptor ==
                  api::EngineGlobalAggregateAvgIntegerResultDescriptor()
                      .encoded_descriptor,
          "bound AVG dropped its function, field, or result descriptor");

  auto row = [](std::string value) {
    api::CrudRowVersionRecord record;
    record.values.push_back({"value", std::move(value)});
    return record;
  };
  auto require_integer = [&](std::vector<api::CrudRowVersionRecord> rows,
                             std::optional<std::int64_t> expected_avg,
                             std::optional<std::int64_t> expected_distinct,
                             std::string_view message) {
    const auto executed = api::ExecuteGlobalAggregateProjection(
        bound.outputs, loaded.descriptor, rows);
    Require(executed.ok && executed.result_shape.rows.size() == 1 &&
                executed.result_shape.rows.front().fields.size() == 2 &&
                executed.scanned_visible_row_count == rows.size(),
            message);
    const auto& fields = executed.result_shape.rows.front().fields;
    const std::vector<std::optional<std::int64_t>> expected{
        expected_avg, expected_distinct};
    for (std::size_t index = 0; index < expected.size(); ++index) {
      if (expected[index]) {
        Require(!fields[index].second.isSqlNull() &&
                    fields[index].second.encoded_value ==
                        std::to_string(*expected[index]),
                "typed integer AVG value drifted");
      } else {
        Require(fields[index].second.isSqlNull() &&
                    fields[index].second.encoded_value.empty(),
                "typed integer AVG NULL finality drifted");
      }
    }
  };

  // Exact Firebird QA direct-relation inputs test_01..05 and test_07..08.
  require_integer({row("5")}, 5, 5, "AVG test_01 finalization failed");
  require_integer({row("5"), row("6")}, 5, 5,
                  "AVG test_02 finalization failed");
  require_integer({row("5"), row("5"), row("6")}, 5, 5,
                  "AVG test_03 finalization failed");
  require_integer({row("5"), row("6"), row("6")}, 5, 5,
                  "AVG test_04 finalization failed");
  require_integer({row("5"), row("5"), row("7")}, 5, 6,
                  "AVG test_05 DISTINCT finalization failed");
  require_integer({row("12"), row("13"), row("14"), row("<NULL>")},
                  13, 13, "AVG test_07 NULL-elision failed");
  require_integer({row("<NULL>")}, std::nullopt, std::nullopt,
                  "AVG test_08 all-NULL finalization failed");
  require_integer({}, std::nullopt, std::nullopt,
                  "empty AVG finalization failed");
  require_integer({row("-5"), row("-6")}, -5, -5,
                  "negative integer AVG did not truncate toward zero");
  require_integer(
      {row("9223372036854775807"), row("9223372036854775807")},
      std::numeric_limits<std::int64_t>::max(),
      std::numeric_limits<std::int64_t>::max(),
      "integer AVG did not use a widened checked accumulator");

  auto int32_relation = loaded.descriptor;
  int32_relation.columns[0].value_descriptor.canonical_type_name = "int32";
  int32_relation.columns[0].value_descriptor.encoded_descriptor =
      "canonical=int32;precision=32;scale=0;nullable=true";
  auto int32_outputs = bound.outputs;
  for (auto& output : int32_outputs) {
    output.source_field.value_descriptor =
        int32_relation.columns[0].value_descriptor;
  }
  const auto int32 = api::ExecuteGlobalAggregateProjection(
      int32_outputs,
      int32_relation,
      {row("2147483647"), row("-2147483648")});
  Require(int32.ok &&
              int32.result_shape.rows[0].fields[0].second.encoded_value ==
                  "0",
          "int32 AVG boundary/truncation drifted");

  auto real_relation = loaded.descriptor;
  real_relation.columns[0].value_descriptor.canonical_type_name = "real64";
  real_relation.columns[0].value_descriptor.encoded_descriptor =
      "canonical=real64;precision=64;nullable=true";
  auto real_envelope = request.global_aggregate_projection;
  for (auto& output : real_envelope.outputs) {
    output.source_field.value_descriptor =
        real_relation.columns[0].value_descriptor;
    output.result_descriptor =
        api::EngineGlobalAggregateAvgRealResultDescriptor();
  }
  const auto real_bound = api::BindGlobalAggregateProjectionEnvelope(
      real_envelope, real_relation);
  Require(real_bound.ok && real_bound.outputs.size() == 2,
          "real64 AVG binding failed");
  const auto qa_real = api::ExecuteGlobalAggregateProjection(
      real_bound.outputs, real_relation, {row("5.123456789")});
  Require(qa_real.ok && qa_real.result_shape.rows.size() == 1 &&
              qa_real.result_shape.rows[0].fields.size() == 2 &&
              std::abs(std::stod(qa_real.result_shape.rows[0]
                                     .fields[0]
                                     .second.encoded_value) -
                       5.123456789) < 1.0e-15,
          "AVG test_09 finite real64 finalization failed");
  const auto rounded_each_step = api::ExecuteGlobalAggregateProjection(
      real_bound.outputs,
      real_relation,
      {row("1e16"), row("1"), row("-1e16")});
  Require(rounded_each_step.ok &&
              rounded_each_step.result_shape.rows.size() == 1 &&
              rounded_each_step.result_shape.rows[0].fields.size() == 2 &&
              rounded_each_step.result_shape.rows[0]
                      .fields[0]
                      .second.encoded_value == "0",
          "real64 AVG did not round each addition as binary64");
  const auto binary64_overflow = api::ExecuteGlobalAggregateProjection(
      real_bound.outputs,
      real_relation,
      {row("1.7976931348623157e+308"),
       row("1.7976931348623157e+308")});
  Require(!binary64_overflow.ok &&
              binary64_overflow.result_shape.rows.empty() &&
              binary64_overflow.scanned_visible_row_count == 2 &&
              binary64_overflow.diagnostic.detail ==
                  "dml.global_aggregate_projection:"
                  "global_aggregate_avg_real64_accumulator_nonfinite",
          "real64 AVG did not reject binary64 intermediate overflow");
  const auto zero_distinct = api::ExecuteGlobalAggregateProjection(
      {real_bound.outputs[1]},
      real_relation,
      {row("0"), row("-0"), row("2")});
  Require(zero_distinct.ok &&
              zero_distinct.result_shape.rows[0]
                      .fields[0]
                      .second.encoded_value == "1",
          "real64 AVG DISTINCT did not canonicalize signed zero");
  for (const std::string_view invalid : {"nan", "inf", "-inf", "1x"}) {
    const auto rejected = api::ExecuteGlobalAggregateProjection(
        real_bound.outputs, real_relation, {row(std::string(invalid))});
    Require(!rejected.ok && rejected.result_shape.rows.empty() &&
                rejected.diagnostic.detail ==
                    "dml.global_aggregate_projection:"
                    "global_aggregate_real64_value_invalid",
            "real64 AVG accepted a non-finite or malformed value");
  }

  for (const std::string_view rejected_uuid : {
           "019dffbb-f000-7fd3-b228-03bf40871b10",
           "019dffbb-f000-710f-9410-919aad901ae2"}) {
    auto rejected = request;
    for (auto& output : rejected.global_aggregate_projection.outputs) {
      output.aggregate_function_uuid.canonical = rejected_uuid;
    }
    RequireRejected(api::EngineSelectRows(rejected),
                    "global_aggregate_function_uuid_invalid",
                    "engine accepted a rejected AVG surface UUID");
  }

  auto mixed = request;
  auto& mixed_output = mixed.global_aggregate_projection.outputs.back();
  mixed_output.operation =
      api::EngineGlobalAggregateOperation::count_non_null_field;
  mixed_output.aggregate_function_uuid.canonical =
      std::string(api::EngineGlobalAggregateCountFunctionUuid());
  mixed_output.result_descriptor =
      api::EngineGlobalAggregateCountResultDescriptor();
  RequireRejected(api::EngineSelectRows(mixed),
                  "global_aggregate_function_uuid_mixed",
                  "engine accepted mixed COUNT/AVG output identities");

}

void TestInvalidBindingsFailClosed(
    const api::EngineRequestContext& context,
    const std::string& table_uuid,
    const api::MgaRelationColumnStorageDescriptor& column) {
  auto invalid_operation = AggregateRequest(context, table_uuid, column);
  invalid_operation.global_aggregate_projection.outputs.front().operation =
      static_cast<api::EngineGlobalAggregateOperation>(255);
  RequireRejected(api::EngineSelectRows(invalid_operation),
                  "global_aggregate_operation_invalid",
                  "global aggregate accepted an unknown operation");

  for (const auto legacy_uuid : {
           std::string("019dffbb-f000-7613-a71e-84b03ef18e1d"),
           std::string("019dffbb-f000-7293-b215-aa84d8693576")}) {
    auto invalid_function = AggregateRequest(context, table_uuid, column);
    invalid_function.global_aggregate_projection.outputs.front()
        .aggregate_function_uuid.canonical = legacy_uuid;
    RequireRejected(api::EngineSelectRows(invalid_function),
                    "global_aggregate_function_uuid_invalid",
                    "global aggregate accepted a non-canonical COUNT UUID");
  }

  auto invalid_result = AggregateRequest(context, table_uuid, column);
  invalid_result.global_aggregate_projection.outputs.front()
      .result_descriptor.encoded_descriptor = "canonical=int128";
  RequireRejected(api::EngineSelectRows(invalid_result),
                  "global_aggregate_result_descriptor_invalid",
                  "global aggregate accepted an invalid result descriptor");

  auto missing_field = AggregateRequest(context, table_uuid, column);
  missing_field.global_aggregate_projection.outputs[1]
      .source_field.column_uuid.canonical =
      NewUuid(platform::UuidKind::object, NowMillis());
  RequireRejected(api::EngineSelectRows(missing_field),
                  "global_aggregate_source_field_not_found",
                  "global aggregate accepted an unknown field UUID");

  auto mismatched_descriptor = AggregateRequest(context, table_uuid, column);
  mismatched_descriptor.global_aggregate_projection.outputs[2]
      .source_field.value_descriptor.encoded_descriptor += ";drift=true";
  RequireRejected(api::EngineSelectRows(mismatched_descriptor),
                  "global_aggregate_source_field_descriptor_mismatch",
                  "global aggregate accepted a mismatched field descriptor");

  auto star_with_field = AggregateRequest(context, table_uuid, column);
  star_with_field.global_aggregate_projection.outputs.front().source_field =
      star_with_field.global_aggregate_projection.outputs[1].source_field;
  RequireRejected(api::EngineSelectRows(star_with_field),
                  "global_aggregate_count_star_forbids_field_binding",
                  "global aggregate accepted COUNT_STAR with a field");

  auto relation_mismatch = AggregateRequest(context, table_uuid, column);
  relation_mismatch.global_aggregate_projection.relation_uuid.canonical =
      NewUuid(platform::UuidKind::object, NowMillis());
  RequireRejected(api::EngineSelectRows(relation_mismatch),
                  "global_aggregate_relation_uuid_mismatch",
                  "global aggregate accepted a mismatched relation UUID");

  auto descriptor_mismatch = AggregateRequest(context, table_uuid, column);
  descriptor_mismatch.global_aggregate_projection.relation_descriptor_uuid
      .canonical = NewUuid(platform::UuidKind::object, NowMillis());
  RequireRejected(api::EngineSelectRows(descriptor_mismatch),
                  "global_aggregate_relation_descriptor_uuid_mismatch",
                  "global aggregate accepted a mismatched relation descriptor UUID");

  auto generation_mismatch = AggregateRequest(context, table_uuid, column);
  ++generation_mismatch.global_aggregate_projection
        .relation_descriptor_generation;
  RequireRejected(api::EngineSelectRows(generation_mismatch),
                  "global_aggregate_relation_descriptor_generation_mismatch",
                  "global aggregate accepted a mismatched descriptor generation");

  auto conflicting_projection = AggregateRequest(context, table_uuid, column);
  conflicting_projection.option_envelopes.push_back(
      "result_projection:any_legacy_projection");
  RequireRejected(api::EngineSelectRows(conflicting_projection),
                  "global_aggregate_conflicting_select_shape",
                  "global aggregate accepted a conflicting projection");
}

void TestLegacyCountProjectionPreserved(
    const api::EngineRequestContext& context,
    const std::string& table_uuid,
    std::int64_t expected) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = table_uuid;
  request.source_object.object_kind = "table";
  request.option_envelopes.push_back("result_projection:count");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "legacy dml.select_rows count projection failed");
  Require(selected.visible_count == 1 &&
              selected.result_shape.rows.size() == 1 &&
              selected.result_shape.rows.front().fields.size() == 1 &&
              selected.result_shape.rows.front()
                      .fields.front()
                      .second.encoded_value == std::to_string(expected),
          "legacy dml.select_rows COUNT(*) behavior regressed");
}

void TestNeutralSblrTransport(
    const api::EngineRequestContext& context) {
  auto envelope = sblr::MakeSblrEnvelope(
      "dml.select_rows", "SBLR_DML_SELECT_ROWS",
      "SBLR-RETIRED-GLOBAL-AGGREGATE-ROOT-REFUSAL");
  envelope.opcode_code = 0;
  envelope.parser_package_uuid = context.session_uuid.canonical;
  envelope.registry_snapshot_uuid = context.database_uuid.canonical;
  envelope.requires_transaction_context = true;

  sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = std::move(envelope);
  const auto refused = sblr::DispatchSblrOperation(std::move(request));
  Require(!refused.envelope_validated && !refused.accepted &&
              !refused.dispatched_to_api && !refused.api_result.ok &&
              refused.api_result.result_shape.rows.empty() &&
              HasDispatchDiagnostic(
                  refused,
                  "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH"),
          "retired code-zero SBLR_DML_SELECT_ROWS root was not refused before dispatch");
  Require(EvidenceValue(refused.api_result,
                        "global_aggregate_relation_scan").empty(),
          "retired aggregate root reached the MGA relation scan");
}

void TestPersistedGlobalAggregateView(Fixture& fixture) {
  auto metadata = Begin(fixture, 20);
  api::EngineCreateSchemaRequest schema;
  schema.context = metadata;
  schema.target_object.uuid.canonical = fixture.schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("aggregate_view_schema"));
  RequireOk(api::EngineCreateSchema(schema),
            "global aggregate view schema create failed");

  const auto table = FirebirdIntegerTable(
      metadata, fixture.expression_table_uuid,
      "aggregate_view_values");
  Require(!api::AppendMgaTableMetadata(metadata, table).error,
          "global aggregate view table metadata append failed");
  api::MgaRelationStorageDescriptor source_descriptor;
  Require(!api::EnsureMgaRelationStorageDescriptor(
               metadata, table, {}, &source_descriptor)
               .error,
          "global aggregate view source descriptor persistence failed");
  Require(source_descriptor.columns.size() == 1 &&
              source_descriptor.columns.front()
                      .value_descriptor.canonical_type_name == "integer" &&
              source_descriptor.columns.front()
                      .value_descriptor.encoded_descriptor ==
                  "type=integer;nullable=false",
          "global aggregate view source lost the persisted Firebird INTEGER descriptor");
  Commit(metadata);

  auto writer = Begin(fixture, 21);
  Insert(writer,
         fixture.expression_table_uuid,
         {Row(101, FirebirdIntegerValue(2100000000)),
          Row(102, FirebirdIntegerValue(2100000000)),
          Row(103, FirebirdIntegerValue(2100000000)),
          Row(104, FirebirdIntegerValue(2100000000))});
  Commit(writer);

  auto create = Begin(fixture, 22);
  const auto created = api::EngineCreateView(GlobalAggregateViewRequest(
      fixture, create, source_descriptor, "V_AVG_EXPRESSION", 2100000000));
  RequireOk(created, "persisted global aggregate view create failed");
  const std::string view_uuid = created.primary_object.uuid.canonical;
  Require(!view_uuid.empty() &&
              EvidenceValue(created, "global_aggregate_view_marker") ==
                  api::kEngineGlobalAggregateViewMarkerV1 &&
              EvidenceValue(created, "global_aggregate_view_parser_sql") ==
                  "false",
          "global aggregate view create authority evidence drifted");
  const auto own_descriptor =
      api::DescribeEngineGlobalAggregateView(create, view_uuid);
  if (own_descriptor.diagnostic.error || !own_descriptor.present ||
      own_descriptor.view_descriptor_generation != 1) {
    std::cerr << "own descriptor: error="
              << own_descriptor.diagnostic.error
              << " present=" << own_descriptor.present
              << " generation="
              << own_descriptor.view_descriptor_generation
              << " code=" << own_descriptor.diagnostic.code
              << " detail=" << own_descriptor.diagnostic.detail << '\n';
    const auto visible = api::FindVisibleApiBehaviorRecord(
        create, view_uuid, create.local_transaction_id);
    if (visible.has_value()) {
      std::cerr << "own descriptor record: operation="
                << visible->operation_id << " kind=" << visible->object_kind
                << " state=" << visible->state
                << " name=" << visible->default_name
                << " payload=" << visible->payload << '\n';
    }
  }
  Require(!own_descriptor.diagnostic.error && own_descriptor.present &&
              own_descriptor.view_descriptor_generation == 1,
          "creating transaction cannot see its own global aggregate view");

  auto precommit_reader = Begin(fixture, 23);
  const auto precommit_resolved = api::EngineResolveName(
      ResolveViewRequest(fixture, precommit_reader, "V_AVG_EXPRESSION"));
  Require(!precommit_resolved.ok &&
              !precommit_resolved.diagnostics.empty() &&
              precommit_resolved.diagnostics.front().code ==
                  "CATALOG.NAME.NOT_FOUND",
          "uncommitted global aggregate view escaped MGA visibility");
  Rollback(precommit_reader);
  Commit(create);

  auto rollback_create = Begin(fixture, 24);
  const auto rolled_back_created = api::EngineCreateView(
      GlobalAggregateViewRequest(fixture,
                                 rollback_create,
                                 source_descriptor,
                                 "V_AVG_ROLLBACK",
                                 2100000000));
  RequireOk(rolled_back_created,
            "rollback probe global aggregate view create failed");
  const auto rollback_own = api::DescribeEngineGlobalAggregateView(
      rollback_create, rolled_back_created.primary_object.uuid.canonical);
  Require(!rollback_own.diagnostic.error && rollback_own.present,
          "rollback probe view lacked own-transaction visibility");
  Rollback(rollback_create);
  auto rollback_reader = Begin(fixture, 25);
  const auto rolled_back_resolved = api::EngineResolveName(
      ResolveViewRequest(fixture, rollback_reader, "V_AVG_ROLLBACK"));
  Require(!rolled_back_resolved.ok &&
              !rolled_back_resolved.diagnostics.empty() &&
              rolled_back_resolved.diagnostics.front().code ==
                  "CATALOG.NAME.NOT_FOUND",
          "rolled-back global aggregate view remained name-visible");
  const auto rolled_back_descriptor =
      api::DescribeEngineGlobalAggregateView(
          rollback_reader,
          rolled_back_created.primary_object.uuid.canonical);
  Require(!rolled_back_descriptor.diagnostic.error &&
              !rolled_back_descriptor.present,
          "rolled-back global aggregate descriptor remained visible");
  Rollback(rollback_reader);

  auto baseline_reader = Begin(fixture, 26);
  const auto baseline = api::EngineResolveName(
      ResolveViewRequest(fixture, baseline_reader, "V_AVG_EXPRESSION"));
  RequireOk(baseline, "committed global aggregate view did not resolve");
  Require(baseline.bound_object_identity.object_uuid.canonical == view_uuid &&
              baseline.semantic_projection.present &&
              baseline.semantic_projection.marker ==
                  api::kEngineGlobalAggregateViewMarkerV1 &&
              baseline.semantic_projection.descriptor_generation == 1 &&
              baseline.semantic_projection.result_alias == "AVG_RESULT" &&
              baseline.semantic_projection.result_descriptor
                      .canonical_type_name == "int64" &&
              baseline.semantic_projection.projection_descriptor
                      .encoded_descriptor.find(
                          fixture.expression_table_uuid) == std::string::npos &&
              baseline.semantic_projection.projection_descriptor
                      .encoded_descriptor.find("2100000000") ==
                  std::string::npos,
          "name resolution leaked non-semantic view expansion state");
  RequireGlobalAggregateViewValue(
      api::EngineSelectRows(
          GlobalAggregateViewSelectRequest(baseline_reader, baseline)),
      "4410000000000000000",
      1,
      "persisted global aggregate view exact AVG failed");

  auto stale_source = GlobalAggregateViewRequest(fixture,
                                                 baseline_reader,
                                                 source_descriptor,
                                                 "V_STALE_SOURCE",
                                                 2100000000);
  for (auto& option : stale_source.option_envelopes) {
    if (option.rfind("source_relation_descriptor_generation:", 0) == 0) {
      option = "source_relation_descriptor_generation:" +
               std::to_string(source_descriptor.descriptor_generation + 1);
    }
  }
  const auto stale_source_result = api::EngineCreateView(stale_source);
  Require(!stale_source_result.ok &&
              HasDiagnosticDetail(
                  stale_source_result,
                  "global_aggregate_view_source_descriptor_stale"),
          "global aggregate view accepted a stale source generation");
  Rollback(baseline_reader);

  auto old_snapshot = Begin(fixture, 27, "snapshot");
  const auto snapshot_baseline = api::EngineResolveName(
      ResolveViewRequest(fixture, old_snapshot, "V_AVG_EXPRESSION"));
  RequireOk(snapshot_baseline,
            "old snapshot failed to bind baseline view descriptor");
  Require(snapshot_baseline.semantic_projection.descriptor_generation == 1,
          "old snapshot baseline descriptor generation drifted");

  auto rollback_alter = Begin(fixture, 28);
  const auto rolled_back_alter = api::EngineCreateView(
      GlobalAggregateViewRequest(fixture,
                                 rollback_alter,
                                 source_descriptor,
                                 "V_AVG_EXPRESSION",
                                 2000000000,
                                 view_uuid,
                                 true));
  RequireOk(rolled_back_alter,
            "CREATE OR ALTER rollback probe failed");
  const auto rollback_alter_descriptor =
      api::DescribeEngineGlobalAggregateView(rollback_alter, view_uuid);
  Require(!rollback_alter_descriptor.diagnostic.error &&
              rollback_alter_descriptor.present &&
              rollback_alter_descriptor.view_uuid.canonical == view_uuid &&
              rollback_alter_descriptor.view_descriptor_generation == 2 &&
              rollback_alter_descriptor.view_descriptor_uuid.canonical !=
                  own_descriptor.view_descriptor_uuid.canonical,
          "CREATE OR ALTER did not retain view UUID/new descriptor generation");
  const auto rollback_alter_resolved = api::EngineResolveName(
      ResolveViewRequest(fixture, rollback_alter, "V_AVG_EXPRESSION"));
  RequireOk(rollback_alter_resolved,
            "altering transaction failed to resolve its own descriptor");
  RequireGlobalAggregateViewValue(
      api::EngineSelectRows(GlobalAggregateViewSelectRequest(
          rollback_alter, rollback_alter_resolved)),
      "4200000000000000000",
      2,
      "altering transaction did not execute its own generation");

  auto stale_select =
      GlobalAggregateViewSelectRequest(rollback_alter, snapshot_baseline);
  const auto stale_select_result = api::EngineSelectRows(stale_select);
  Require(!stale_select_result.ok &&
              HasDiagnosticDetail(
                  stale_select_result,
                  "global_aggregate_view_descriptor_stale") &&
              EvidenceValue(stale_select_result,
                            "global_aggregate_relation_scan").empty(),
          "stale view generation reached the MGA row scan");
  RequireGlobalAggregateViewValue(
      api::EngineSelectRows(GlobalAggregateViewSelectRequest(
          old_snapshot, snapshot_baseline)),
      "4410000000000000000",
      1,
      "snapshot observed an active CREATE OR ALTER generation");
  Rollback(rollback_alter);

  auto post_rollback = Begin(fixture, 29);
  const auto post_rollback_resolved = api::EngineResolveName(
      ResolveViewRequest(fixture, post_rollback, "V_AVG_EXPRESSION"));
  RequireOk(post_rollback_resolved,
            "view did not resolve after rolled-back alter");
  Require(post_rollback_resolved.semantic_projection.descriptor_generation ==
              1 &&
              post_rollback_resolved.semantic_projection.projection_descriptor
                      .descriptor_uuid.canonical ==
                  own_descriptor.view_descriptor_uuid.canonical,
          "rolled-back alter changed the durable descriptor generation");
  RequireGlobalAggregateViewValue(
      api::EngineSelectRows(GlobalAggregateViewSelectRequest(
          post_rollback, post_rollback_resolved)),
      "4410000000000000000",
      1,
      "rolled-back alter changed persisted view execution");
  Rollback(post_rollback);

  auto commit_alter = Begin(fixture, 30);
  const auto committed_alter = api::EngineCreateView(
      GlobalAggregateViewRequest(fixture,
                                 commit_alter,
                                 source_descriptor,
                                 "V_AVG_EXPRESSION",
                                 2000000000,
                                 view_uuid,
                                 true));
  RequireOk(committed_alter, "committed CREATE OR ALTER failed");
  const auto committed_alter_descriptor =
      api::DescribeEngineGlobalAggregateView(commit_alter, view_uuid);
  Require(!committed_alter_descriptor.diagnostic.error &&
              committed_alter_descriptor.present &&
              committed_alter_descriptor.view_descriptor_generation == 2,
          "committed ALTER did not stage descriptor generation two");
  Commit(commit_alter);

  const auto old_snapshot_after_commit = api::EngineResolveName(
      ResolveViewRequest(fixture, old_snapshot, "V_AVG_EXPRESSION"));
  RequireOk(old_snapshot_after_commit,
            "old snapshot lost the view after committed ALTER");
  Require(old_snapshot_after_commit.semantic_projection
                  .descriptor_generation == 1 &&
              old_snapshot_after_commit.semantic_projection
                      .projection_descriptor.descriptor_uuid.canonical ==
                  own_descriptor.view_descriptor_uuid.canonical,
          "old snapshot selected the post-snapshot ALTER generation");
  RequireGlobalAggregateViewValue(
      api::EngineSelectRows(GlobalAggregateViewSelectRequest(
          old_snapshot, old_snapshot_after_commit)),
      "4410000000000000000",
      1,
      "old snapshot execution changed after committed ALTER");
  Rollback(old_snapshot);

  auto fresh_reader = Begin(fixture, 31);
  const auto fresh = api::EngineResolveName(
      ResolveViewRequest(fixture, fresh_reader, "V_AVG_EXPRESSION"));
  RequireOk(fresh, "fresh reader did not resolve committed ALTER");
  Require(fresh.bound_object_identity.object_uuid.canonical == view_uuid &&
              fresh.semantic_projection.descriptor_generation == 2 &&
              fresh.semantic_projection.projection_descriptor.descriptor_uuid
                      .canonical ==
                  committed_alter_descriptor.view_descriptor_uuid.canonical,
          "fresh reader did not select committed descriptor generation two");
  RequireGlobalAggregateViewValue(
      api::EngineSelectRows(
          GlobalAggregateViewSelectRequest(fresh_reader, fresh)),
      "4200000000000000000",
      2,
      "fresh reader did not execute committed ALTER generation");
  const auto fresh_stale = api::EngineSelectRows(
      GlobalAggregateViewSelectRequest(fresh_reader, snapshot_baseline));
  Require(!fresh_stale.ok &&
              HasDiagnosticDetail(fresh_stale,
                                  "global_aggregate_view_descriptor_stale") &&
              EvidenceValue(fresh_stale,
                            "global_aggregate_relation_scan").empty(),
          "fresh reader accepted stale semantic descriptor generation one");

  const std::string malformed_view_uuid =
      NewUuid(platform::UuidKind::object, NowMillis());
  api::ApiBehaviorRecord malformed_record;
  malformed_record.creator_tx = fresh_reader.local_transaction_id;
  malformed_record.operation_id = "ddl.create_view";
  malformed_record.object_uuid = malformed_view_uuid;
  malformed_record.object_kind = "view";
  malformed_record.default_name = "V_MALFORMED";
  malformed_record.state = "created";
  malformed_record.payload =
      "schema=" + fixture.schema_uuid + ";target=" + malformed_view_uuid +
      ";options=view_query_shape:" +
      api::kEngineGlobalAggregateViewMarkerV1 +
      ";view_descriptor_uuid:not-a-canonical-uuid"
      ";view_descriptor_generation:1"
      ";source_relation_uuid:" + source_descriptor.relation_uuid.canonical +
      ";source_relation_descriptor_uuid:" +
      source_descriptor.descriptor_uuid.canonical +
      ";source_relation_descriptor_generation:" +
      std::to_string(source_descriptor.descriptor_generation) +
      ";source_column_uuid:" +
      source_descriptor.columns.front().column_uuid.canonical +
      ";source_column_descriptor_uuid:" +
      source_descriptor.columns.front()
          .value_descriptor.descriptor_uuid.canonical +
      ";expression_kind:" +
      api::kEngineGlobalAggregateViewInt32MultiplyV1 +
      ";expression_literal_type:int32"
      ";expression_literal_value:2100000000"
      ";expression_result_type:int64"
      ";aggregate_function_uuid:" +
      std::string(api::EngineGlobalAggregateAvgFunctionUuid()) +
      ";aggregate_result_alias:AVG_RESULT"
      ";aggregate_result_type:int64_nullable";
  Require(!api::AppendApiBehaviorEvent(
               fresh_reader,
               api::MakeApiBehaviorRecordEvent(malformed_record))
               .error,
          "malformed descriptor probe append failed");
  const auto malformed = api::DescribeEngineGlobalAggregateView(
      fresh_reader, malformed_view_uuid);
  Require(malformed.diagnostic.error && !malformed.present &&
              malformed.diagnostic.detail.find(
                  "global_aggregate_view_descriptor_invalid") !=
                  std::string::npos,
          "malformed persisted descriptor reached semantic projection");

  api::EngineGlobalAggregateProjectionEnvelope invalid_literal;
  invalid_literal.relation_uuid = source_descriptor.relation_uuid;
  invalid_literal.relation_descriptor_uuid =
      source_descriptor.descriptor_uuid;
  invalid_literal.relation_descriptor_generation =
      source_descriptor.descriptor_generation;
  invalid_literal.outputs.push_back(CheckedInt32MultiplyAvg(
      source_descriptor.columns.front(), "2147483648"));
  api::EngineSelectRowsRequest invalid_literal_select;
  invalid_literal_select.context = fresh_reader;
  invalid_literal_select.source_object.uuid = source_descriptor.relation_uuid;
  invalid_literal_select.source_object.object_kind = "table";
  invalid_literal_select.global_aggregate_projection = invalid_literal;
  RequireRejected(api::EngineSelectRows(invalid_literal_select),
                  "global_aggregate_expression_literal_int32_invalid",
                  "out-of-range expression literal was accepted");

  api::EngineGlobalAggregateProjectionEnvelope checked;
  checked.relation_uuid = source_descriptor.relation_uuid;
  checked.relation_descriptor_uuid = source_descriptor.descriptor_uuid;
  checked.relation_descriptor_generation =
      source_descriptor.descriptor_generation;
  checked.outputs.push_back(CheckedInt32MultiplyAvg(
      source_descriptor.columns.front(), "2100000000"));
  const auto bound = api::BindGlobalAggregateProjectionEnvelope(
      checked, source_descriptor);
  Require(bound.ok && bound.outputs.size() == 1,
          "checked int32 multiply expression did not bind");
  api::CrudRowVersionRecord invalid_row;
  invalid_row.values.push_back(
      {source_descriptor.columns.front().canonical_name_key,
       "2147483648"});
  const auto invalid_execution = api::ExecuteGlobalAggregateProjection(
      bound.outputs, source_descriptor, {invalid_row});
  Require(!invalid_execution.ok && invalid_execution.diagnostic.error &&
              invalid_execution.scanned_visible_row_count == 1 &&
              invalid_execution.result_shape.rows.empty() &&
              invalid_execution.diagnostic.detail.find(
                  "global_aggregate_integer_value_invalid") !=
                  std::string::npos,
          "out-of-range int32 source did not fail checked arithmetic");
  Rollback(fresh_reader);
}

}  // namespace

int main() {
  auto fixture = CreateFixture();

  auto metadata = Begin(fixture, 1);
  const auto values_table = Table(metadata,
                                  fixture.values_table_uuid,
                                  "global_aggregate_values");
  const auto nulls_table = Table(metadata,
                                 fixture.nulls_table_uuid,
                                 "global_aggregate_nulls");
  Require(!api::AppendMgaTableMetadata(metadata, values_table).error,
          "global aggregate values metadata append failed");
  Require(!api::AppendMgaTableMetadata(metadata, nulls_table).error,
          "global aggregate nulls metadata append failed");
  api::MgaRelationStorageDescriptor values_descriptor;
  api::MgaRelationStorageDescriptor nulls_descriptor;
  Require(!api::EnsureMgaRelationStorageDescriptor(
               metadata, values_table, {}, &values_descriptor)
               .error,
          "global aggregate values descriptor persistence failed");
  Require(!api::EnsureMgaRelationStorageDescriptor(
               metadata, nulls_table, {}, &nulls_descriptor)
               .error,
          "global aggregate nulls descriptor persistence failed");
  Commit(metadata);

  auto empty_reader = Begin(fixture, 2);
  const auto values_column =
      ValueColumn(empty_reader, fixture.values_table_uuid);
  RequireCounts(api::EngineSelectRows(AggregateRequest(
                    empty_reader,
                    fixture.values_table_uuid,
                    values_column)),
                0,
                0,
                0,
                "empty global aggregate projection failed");
  RequireAvgs(api::EngineSelectRows(AvgRequest(
                  empty_reader,
                  fixture.values_table_uuid,
                  values_column)),
              std::nullopt,
              std::nullopt,
              0,
              "empty global AVG projection failed");
  TestInvalidBindingsFailClosed(
      empty_reader, fixture.values_table_uuid, values_column);
  TestBoundIdentityAndTypedIntegerDistinct(
      empty_reader, fixture.values_table_uuid, values_column);
  TestAvgTypedFinalizationAndRefusals(
      empty_reader, fixture.values_table_uuid, values_column);
  Rollback(empty_reader);

  auto writer = Begin(fixture, 3);
  Insert(writer,
         fixture.values_table_uuid,
         {Row(1, Int64Value(0)),
          Row(2, Int64Value(0)),
          Row(3, NullInt64Value()),
          Row(4, NullInt64Value()),
          Row(5, NullInt64Value()),
          Row(6, Int64Value(1)),
          Row(7, Int64Value(1)),
          Row(8, Int64Value(1)),
          Row(9, Int64Value(1))});
  Insert(writer,
         fixture.nulls_table_uuid,
         {Row(10, NullInt64Value()),
          Row(11, NullInt64Value()),
          Row(12, NullInt64Value())});
  const auto writer_values_column =
      ValueColumn(writer, fixture.values_table_uuid);
  RequireCounts(api::EngineSelectRows(AggregateRequest(
                    writer,
                    fixture.values_table_uuid,
                    writer_values_column)),
                9,
                6,
                2,
                "writer global aggregate projection failed");
  RequireAvgs(api::EngineSelectRows(AvgRequest(
                  writer,
                  fixture.values_table_uuid,
                  writer_values_column)),
              0,
              0,
              9,
              "writer global AVG projection failed");
  TestNeutralSblrTransport(writer);
  const auto nulls_column =
      ValueColumn(writer, fixture.nulls_table_uuid);
  RequireCounts(api::EngineSelectRows(AggregateRequest(
                    writer,
                    fixture.nulls_table_uuid,
                    nulls_column)),
                3,
                0,
                0,
                "all-null global aggregate projection failed");
  RequireAvgs(api::EngineSelectRows(AvgRequest(
                  writer,
                  fixture.nulls_table_uuid,
                  nulls_column)),
              std::nullopt,
              std::nullopt,
              3,
              "all-null global AVG projection failed");
  Commit(writer);

  auto snapshot_reader = Begin(fixture, 4, "snapshot");
  const auto snapshot_column =
      ValueColumn(snapshot_reader, fixture.values_table_uuid);
  RequireCounts(api::EngineSelectRows(AggregateRequest(
                    snapshot_reader,
                    fixture.values_table_uuid,
                    snapshot_column)),
                9,
                6,
                2,
                "snapshot baseline aggregate projection failed");
  RequireAvgs(api::EngineSelectRows(AvgRequest(
                  snapshot_reader,
                  fixture.values_table_uuid,
                  snapshot_column)),
              0,
              0,
              9,
              "snapshot baseline AVG projection failed");
  TestLegacyCountProjectionPreserved(
      snapshot_reader, fixture.values_table_uuid, 9);

  auto rollback_writer = Begin(fixture, 5);
  Insert(rollback_writer,
         fixture.values_table_uuid,
         {Row(13, Int64Value(30))});
  const auto rollback_column =
      ValueColumn(rollback_writer, fixture.values_table_uuid);
  RequireCounts(api::EngineSelectRows(AggregateRequest(
                    rollback_writer,
                    fixture.values_table_uuid,
                    rollback_column)),
                10,
                7,
                3,
                "writer-own aggregate visibility failed");
  RequireAvgs(api::EngineSelectRows(AvgRequest(
                  rollback_writer,
                  fixture.values_table_uuid,
                  rollback_column)),
              4,
              10,
              10,
              "writer-own AVG visibility failed");
  RequireCounts(api::EngineSelectRows(AggregateRequest(
                    snapshot_reader,
                    fixture.values_table_uuid,
                    snapshot_column)),
                9,
                6,
                2,
                "snapshot observed another active writer");
  RequireAvgs(api::EngineSelectRows(AvgRequest(
                  snapshot_reader,
                  fixture.values_table_uuid,
                  snapshot_column)),
              0,
              0,
              9,
              "snapshot AVG observed another active writer");
  Rollback(rollback_writer);
  RequireCounts(api::EngineSelectRows(AggregateRequest(
                    snapshot_reader,
                    fixture.values_table_uuid,
                    snapshot_column)),
                9,
                6,
                2,
                "rolled-back row became aggregate-visible");
  RequireAvgs(api::EngineSelectRows(AvgRequest(
                  snapshot_reader,
                  fixture.values_table_uuid,
                  snapshot_column)),
              0,
              0,
              9,
              "rolled-back row became AVG-visible");

  auto commit_writer = Begin(fixture, 6);
  Insert(commit_writer,
         fixture.values_table_uuid,
         {Row(14, Int64Value(30))});
  Commit(commit_writer);
  RequireCounts(api::EngineSelectRows(AggregateRequest(
                    snapshot_reader,
                    fixture.values_table_uuid,
                    snapshot_column)),
                9,
                6,
                2,
                "stable snapshot changed after concurrent commit");
  RequireAvgs(api::EngineSelectRows(AvgRequest(
                  snapshot_reader,
                  fixture.values_table_uuid,
                  snapshot_column)),
              0,
              0,
              9,
              "stable AVG snapshot changed after concurrent commit");
  Rollback(snapshot_reader);

  auto fresh_reader = Begin(fixture, 7);
  const auto fresh_column =
      ValueColumn(fresh_reader, fixture.values_table_uuid);
  RequireCounts(api::EngineSelectRows(AggregateRequest(
                    fresh_reader,
                    fixture.values_table_uuid,
                    fresh_column)),
                10,
                7,
                3,
                "fresh reader did not observe committed aggregate input");
  RequireAvgs(api::EngineSelectRows(AvgRequest(
                  fresh_reader,
                  fixture.values_table_uuid,
                  fresh_column)),
              4,
              10,
              10,
              "fresh reader did not observe committed AVG input");
  TestLegacyCountProjectionPreserved(
      fresh_reader, fixture.values_table_uuid, 10);
  Rollback(fresh_reader);
  TestPersistedGlobalAggregateView(fixture);
  return EXIT_SUCCESS;
}
