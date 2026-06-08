// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dispatch/function_dispatch.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/projection_api.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_dispatch.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace functions = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
using sblr::SblrValue;
using sblr::SblrValuePayloadKind;

constexpr const char* kDatabaseUuid = "019f4400-0000-7000-8000-000000000001";
constexpr const char* kSessionUuid = "019f4400-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f4400-0000-7000-8000-000000000003";
constexpr const char* kTableUuid = "019f4400-0000-7000-8000-000000000101";
constexpr const char* kIndexUuid = "019f4400-0000-7000-8000-000000000102";
constexpr const char* kRowA = "019f4400-0000-7000-8000-000000000201";
constexpr const char* kRowB = "019f4400-0000-7000-8000-000000000202";
constexpr const char* kVersionA = "019f4400-0000-7000-8000-000000000301";
constexpr const char* kVersionB = "019f4400-0000-7000-8000-000000000302";
constexpr const char* kUnknownTable = "019f4400-0000-7000-8000-000000000999";
constexpr std::uint64_t kExpectedVisibleRows = 2;
constexpr std::uint64_t kExpectedRowStoreBytes = 728;
constexpr std::uint64_t kExpectedTableSizeWithIndexes = 1538;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path TempDatabasePath() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("sbsfc044_catalog_statistics_" + std::to_string(stamp) + ".sbdb");
}

void CleanupDatabase(const std::filesystem::path& path) {
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".sb.api_events");
  std::filesystem::remove(path.string() + ".sb.mga_row_versions");
  std::filesystem::remove(path.string() + ".sb.mga_relation_metadata");
  std::filesystem::remove(path.string() + ".sb.mga_index_entries");
  std::filesystem::remove(path.string() + ".sb.mga_relation_descriptors");
  std::filesystem::remove(path.string() + ".sb.mga_large_values");
  std::filesystem::remove(path.string() + ".sb.mga_savepoints");
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1789810444000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1789810444001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1789810444002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "SBSFC044 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                      const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsfc044-catalog-statistics";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext BeginTransaction(const std::filesystem::path& path,
                                           const std::string& database_uuid) {
  api::EngineBeginTransactionRequest begin;
  begin.context = BaseContext(path, database_uuid);
  const auto begun = api::EngineBeginTransaction(begin);
  for (const auto& diagnostic : begun.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(begun.ok, "SBSFC044 transaction.begin failed");
  Require(begun.local_transaction_id != 0, "SBSFC044 transaction.begin returned no local id");
  auto context = BaseContext(path, database_uuid);
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void SeedCatalogStatisticsFixture(const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.table_uuid = kTableUuid;
  table.default_name = "sbsfc044_catalog_stats";
  table.columns = {{"id", "type=int64"}, {"note", "type=character"}};
  auto diagnostic = api::AppendMgaTableMetadata(context, table);
  Require(!diagnostic.error, "SBSFC044 table metadata append failed");

  api::CrudIndexRecord index;
  index.index_uuid = kIndexUuid;
  index.table_uuid = kTableUuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "sbsfc044_catalog_stats_id_idx";
  index.key_envelopes = {"id"};
  diagnostic = api::AppendMgaIndexMetadata(context, index);
  Require(!diagnostic.error, "SBSFC044 index metadata append failed");

  api::CrudRowVersionRecord row_a;
  row_a.creator_tx = context.local_transaction_id;
  row_a.table_uuid = kTableUuid;
  row_a.row_uuid = kRowA;
  row_a.version_uuid = kVersionA;
  row_a.values = {{"id", "1"}, {"note", "alpha"}};
  diagnostic = api::AppendMgaRowVersion(context, row_a, nullptr);
  Require(!diagnostic.error, "SBSFC044 row A append failed");
  diagnostic = api::AppendMgaIndexEntriesForIndex(context, index, kRowA, kVersionA, row_a.values);
  Require(!diagnostic.error, "SBSFC044 row A index append failed");

  api::CrudRowVersionRecord row_b;
  row_b.creator_tx = context.local_transaction_id;
  row_b.table_uuid = kTableUuid;
  row_b.row_uuid = kRowB;
  row_b.version_uuid = kVersionB;
  row_b.values = {{"id", "2"}, {"note", "bravo"}};
  diagnostic = api::AppendMgaRowVersion(context, row_b, nullptr);
  Require(!diagnostic.error, "SBSFC044 row B append failed");
  diagnostic = api::AppendMgaIndexEntriesForIndex(context, index, kRowB, kVersionB, row_b.values);
  Require(!diagnostic.error, "SBSFC044 row B index append failed");
}

SblrValue TextValue(std::string descriptor, std::string text) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.text_value = std::move(text);
  value.encoded_value = value.text_value;
  return value;
}

SblrValue BooleanValue(bool input) {
  SblrValue value;
  value.descriptor_id = "boolean";
  value.payload_kind = SblrValuePayloadKind::boolean;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = input ? 1 : 0;
  value.encoded_value = input ? "1" : "0";
  value.text_value = value.encoded_value;
  return value;
}

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

scratchbird::engine::sblr::SblrExecutionContext SblrContextFromEngine(
    const api::EngineRequestContext& context) {
  scratchbird::engine::sblr::SblrExecutionContext out;
  out.database_path = context.database_path;
  out.database_uuid = context.database_uuid.canonical;
  out.session_uuid = context.session_uuid.canonical;
  out.user_uuid = context.principal_uuid.canonical;
  out.transaction_uuid = context.transaction_uuid.canonical;
  out.local_transaction_id = context.local_transaction_id;
  out.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  out.transaction_isolation_level = context.transaction_isolation_level;
  out.transaction_context_present = true;
  out.security_context_present = true;
  return out;
}

sblr::SblrResult RunFunction(const functions::FunctionRegistry& registry,
                             const api::EngineRequestContext& context,
                             std::string function_id,
                             std::vector<SblrValue> values) {
  functions::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context = SblrContextFromEngine(context);
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(functions::FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return functions::DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectUint64(std::string_view case_id,
                  const sblr::SblrResult& result,
                  std::uint64_t expected) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected successful scalar result\n";
    return false;
  }
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "uint64" || !value.has_uint64_value ||
      value.uint64_value != expected) {
    std::cerr << case_id << ": expected uint64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << '\n';
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const sblr::SblrResult& result,
                std::string_view descriptor) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected successful scalar result\n";
    return false;
  }
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected NULL " << descriptor << ", got "
              << value.descriptor_id << '\n';
    return false;
  }
  return true;
}

sblr::SblrOperationEnvelope ProjectionEnvelope(
    std::string function_id,
    std::vector<api::EngineProjectionFunctionArgument> arguments) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "SBSFC044-catalog-statistics-projection");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_0_function_id", std::move(function_id)});
  envelope.operands.push_back({"text", "projection_0_function_arg_count", std::to_string(arguments.size())});
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto prefix = "projection_0_arg_" + std::to_string(index) + "_";
    envelope.operands.push_back({"text", prefix + "name", arguments[index].name});
    envelope.operands.push_back({"text", prefix + "type", arguments[index].type_name});
    envelope.operands.push_back({"text", prefix + "value", arguments[index].encoded_value});
    envelope.operands.push_back({"text", prefix + "is_null", arguments[index].is_null ? "true" : "false"});
  }
  return envelope;
}

bool ExpectProjectionUint64(std::string_view case_id,
                            const sblr::SblrDispatchResult& result,
                            std::uint64_t expected) {
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api ||
      !result.api_result.ok || result.api_result.result_shape.rows.size() != 1 ||
      result.api_result.result_shape.rows.front().fields.size() != 1) {
    std::cerr << case_id << ": expected one projected scalar field\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << "  envelope " << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << "  api " << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    return false;
  }
  const auto& value = result.api_result.result_shape.rows.front().fields.front().second;
  if (value.is_null || value.descriptor.canonical_type_name != "uint64" ||
      value.encoded_value != std::to_string(expected)) {
    std::cerr << case_id << ": expected projected uint64 " << expected << ", got "
              << value.descriptor.canonical_type_name << " " << value.encoded_value << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto database_path = TempDatabasePath();
  CleanupDatabase(database_path);
  const auto database_uuid = CreateMinimalDatabase(database_path);
  auto context = BeginTransaction(database_path, database_uuid);
  SeedCatalogStatisticsFixture(context);

  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectUint64("SBSFC044-relation-row-estimate-catalog",
                    RunFunction(registry, context, "sb.scalar.relation_row_estimate", {}),
                    kExpectedVisibleRows) && ok;
  ok = ExpectUint64("SBSFC044-relation-row-estimate-table",
                    RunFunction(registry, context, "sb.scalar.relation_row_estimate",
                                {TextValue("uuid", kTableUuid)}),
                    kExpectedVisibleRows) && ok;
  ok = ExpectNull("SBSFC044-relation-row-estimate-null",
                  RunFunction(registry, context, "sb.scalar.relation_row_estimate",
                              {NullValue("uuid")}),
                  "uint64") && ok;
  ok = ExpectNull("SBSFC044-relation-row-estimate-unknown",
                  RunFunction(registry, context, "sb.scalar.relation_row_estimate",
                              {TextValue("uuid", kUnknownTable)}),
                  "uint64") && ok;
  ok = ExpectUint64("SBSFC044-table-size-catalog",
                    RunFunction(registry, context, "sb.scalar.table_size", {}),
                    kExpectedTableSizeWithIndexes) && ok;
  ok = ExpectUint64("SBSFC044-table-size-table-default",
                    RunFunction(registry, context, "sb.scalar.table_size",
                                {TextValue("uuid", kTableUuid)}),
                    kExpectedTableSizeWithIndexes) && ok;
  ok = ExpectUint64("SBSFC044-table-size-table-no-indexes",
                    RunFunction(registry, context, "sb.scalar.table_size",
                                {TextValue("uuid", kTableUuid), BooleanValue(false)}),
                    kExpectedRowStoreBytes) && ok;
  ok = ExpectNull("SBSFC044-table-size-null-table",
                  RunFunction(registry, context, "sb.scalar.table_size",
                              {NullValue("uuid")}),
                  "uint64") && ok;
  ok = ExpectNull("SBSFC044-table-size-null-include-indexes",
                  RunFunction(registry, context, "sb.scalar.table_size",
                              {TextValue("uuid", kTableUuid), NullValue("boolean")}),
                  "uint64") && ok;
  ok = ExpectNull("SBSFC044-table-size-unknown",
                  RunFunction(registry, context, "sb.scalar.table_size",
                              {TextValue("uuid", kUnknownTable)}),
                  "uint64") && ok;

  ok = ExpectProjectionUint64(
           "SBSFC044-relation-row-estimate-projection",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.relation_row_estimate",
                                                           {api::EngineProjectionFunctionArgument{
                                                               "table_uuid", "uuid", kTableUuid, false}}),
                                        api::EngineApiRequest{}}),
           kExpectedVisibleRows) && ok;
  ok = ExpectProjectionUint64(
           "SBSFC044-table-size-projection-no-indexes",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.table_size",
                                                           {api::EngineProjectionFunctionArgument{
                                                                "table_uuid", "uuid", kTableUuid, false},
                                                            api::EngineProjectionFunctionArgument{
                                                                "include_indexes", "boolean", "false", false}}),
                                        api::EngineApiRequest{}}),
           kExpectedRowStoreBytes) && ok;

  CleanupDatabase(database_path);
  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_044_catalog_statistics_runtime_conformance=passed\n";
  return 0;
}
