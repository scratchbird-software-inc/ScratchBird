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

constexpr const char* kSessionUuid = "019f4500-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f4500-0000-7000-8000-000000000003";
constexpr const char* kSchemaUuid = "019f4500-0000-7000-8000-000000000004";
constexpr const char* kStatementUuid = "019f4500-0000-7000-8000-000000000005";
constexpr const char* kTableUuid = "019f4500-0000-7000-8000-000000000101";
constexpr const char* kUnknownTableUuid = "019f4500-0000-7000-8000-000000000999";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path TempDatabasePath() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("sbsfc045_privilege_predicates_" + std::to_string(stamp) + ".sbdb");
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1789810450000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1789810450001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1789810450002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "SBSFC045 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                      const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsfc045-privilege-predicates";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.current_schema_uuid.canonical = kSchemaUuid;
  context.statement_uuid.canonical = kStatementUuid;
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
  Require(begun.ok, "SBSFC045 transaction.begin failed");
  Require(begun.local_transaction_id != 0, "SBSFC045 transaction.begin returned no local id");
  auto context = BaseContext(path, database_uuid);
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void SeedPrivilegeFixture(const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.table_uuid = kTableUuid;
  table.default_name = "sbsfc045_privilege_target";
  table.columns = {{"id", "type=int64"}, {"note", "type=character"}};
  const auto diagnostic = api::AppendMgaTableMetadata(context, table);
  Require(!diagnostic.error, "SBSFC045 table metadata append failed");
}

SblrValue TextValue(std::string descriptor, std::string text) {
  SblrValue value;
  const bool is_uuid = descriptor == "uuid";
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = is_uuid ? SblrValuePayloadKind::uuid_text
                               : SblrValuePayloadKind::text;
  value.is_null = false;
  value.text_value = std::move(text);
  value.encoded_value = value.text_value;
  return value;
}

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

sblr::SblrExecutionContext SblrContextFromEngine(
    const api::EngineRequestContext& context) {
  sblr::SblrExecutionContext out;
  out.database_path = context.database_path;
  out.database_uuid = context.database_uuid.canonical;
  out.current_schema_uuid = context.current_schema_uuid.canonical;
  out.session_uuid = context.session_uuid.canonical;
  out.user_uuid = context.principal_uuid.canonical;
  out.statement_uuid = context.statement_uuid.canonical;
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
    request.arguments.push_back(
        functions::FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return functions::DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectBoolean(std::string_view case_id,
                   const sblr::SblrResult& result,
                   bool expected) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected successful scalar result\n";
    return false;
  }
  const auto& value = result.scalar_values.front();
  const auto expected_int = expected ? 1 : 0;
  if (value.is_null || value.descriptor_id != "boolean" || !value.has_int64_value ||
      value.int64_value != expected_int) {
    std::cerr << case_id << ": expected boolean " << expected_int << ", got "
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
                                         "SBSFC045-privilege-predicate-projection");
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

bool ExpectProjectionBoolean(std::string_view case_id,
                             const sblr::SblrDispatchResult& result,
                             bool expected) {
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
  const std::string expected_value = expected ? "1" : "0";
  if (value.is_null || value.descriptor.canonical_type_name != "boolean" ||
      value.encoded_value != expected_value) {
    std::cerr << case_id << ": expected projected boolean " << expected_value << ", got "
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
  SeedPrivilegeFixture(context);

  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectBoolean("SBSFC045-has-table-privilege-current-owner",
                     RunFunction(registry, context, "sb.scalar.has_table_privilege",
                                 {TextValue("uuid", kTableUuid),
                                  TextValue("character", "SELECT")}),
                     true) && ok;
  ok = ExpectBoolean("SBSFC045-has-table-privilege-optional-user",
                     RunFunction(registry, context, "sb.scalar.has_table_privilege",
                                 {TextValue("uuid", kPrincipalUuid),
                                  TextValue("uuid", kTableUuid),
                                  TextValue("character", "UPDATE")}),
                     true) && ok;
  ok = ExpectNull("SBSFC045-has-table-privilege-null",
                  RunFunction(registry, context, "sb.scalar.has_table_privilege",
                              {NullValue("uuid"), TextValue("character", "SELECT")}),
                  "boolean") && ok;
  ok = ExpectBoolean("SBSFC045-has-table-privilege-unknown",
                     RunFunction(registry, context, "sb.scalar.has_table_privilege",
                                 {TextValue("uuid", kUnknownTableUuid),
                                  TextValue("character", "SELECT")}),
                     false) && ok;

  ok = ExpectBoolean("SBSFC045-has-column-privilege-current-owner",
                     RunFunction(registry, context, "sb.scalar.has_column_privilege",
                                 {TextValue("uuid", kTableUuid),
                                  TextValue("character", "id"),
                                  TextValue("character", "SELECT")}),
                     true) && ok;
  ok = ExpectBoolean("SBSFC045-has-column-privilege-optional-user",
                     RunFunction(registry, context, "sb.scalar.has_column_privilege",
                                 {TextValue("uuid", kPrincipalUuid),
                                  TextValue("uuid", kTableUuid),
                                  TextValue("character", "note"),
                                  TextValue("character", "UPDATE")}),
                     true) && ok;
  ok = ExpectNull("SBSFC045-has-column-privilege-null",
                  RunFunction(registry, context, "sb.scalar.has_column_privilege",
                              {TextValue("uuid", kTableUuid),
                               NullValue("character"),
                               TextValue("character", "SELECT")}),
                  "boolean") && ok;
  ok = ExpectBoolean("SBSFC045-has-column-privilege-unknown-column",
                     RunFunction(registry, context, "sb.scalar.has_column_privilege",
                                 {TextValue("uuid", kTableUuid),
                                  TextValue("character", "missing_column"),
                                  TextValue("character", "SELECT")}),
                     false) && ok;

  ok = ExpectBoolean("SBSFC045-has-function-privilege-current-owner",
                     RunFunction(registry, context, "sb.scalar.has_function_privilege",
                                 {TextValue("character", "has_function_privilege"),
                                  TextValue("character", "EXECUTE")}),
                     true) && ok;
  ok = ExpectBoolean("SBSFC045-has-function-privilege-optional-user",
                     RunFunction(registry, context, "sb.scalar.has_function_privilege",
                                 {TextValue("uuid", kPrincipalUuid),
                                  TextValue("character", "sb.scalar.has_table_privilege"),
                                  TextValue("character", "EXECUTE")}),
                     true) && ok;
  ok = ExpectNull("SBSFC045-has-function-privilege-null",
                  RunFunction(registry, context, "sb.scalar.has_function_privilege",
                              {NullValue("character"), TextValue("character", "EXECUTE")}),
                  "boolean") && ok;
  ok = ExpectBoolean("SBSFC045-has-function-privilege-unknown",
                     RunFunction(registry, context, "sb.scalar.has_function_privilege",
                                 {TextValue("character", "missing_function"),
                                  TextValue("character", "EXECUTE")}),
                     false) && ok;

  ok = ExpectBoolean("SBSFC045-has-schema-privilege-current-owner",
                     RunFunction(registry, context, "sb.scalar.has_schema_privilege",
                                 {TextValue("character", "current_schema"),
                                  TextValue("character", "USAGE")}),
                     true) && ok;
  ok = ExpectBoolean("SBSFC045-has-schema-privilege-optional-user",
                     RunFunction(registry, context, "sb.scalar.has_schema_privilege",
                                 {TextValue("uuid", kPrincipalUuid),
                                  TextValue("uuid", kSchemaUuid),
                                  TextValue("character", "CREATE")}),
                     true) && ok;
  ok = ExpectNull("SBSFC045-has-schema-privilege-null",
                  RunFunction(registry, context, "sb.scalar.has_schema_privilege",
                              {TextValue("character", "current_schema"),
                               NullValue("character")}),
                  "boolean") && ok;
  ok = ExpectBoolean("SBSFC045-has-schema-privilege-unknown",
                     RunFunction(registry, context, "sb.scalar.has_schema_privilege",
                                 {TextValue("character", "missing_schema"),
                                  TextValue("character", "USAGE")}),
                     false) && ok;

  ok = ExpectProjectionBoolean(
           "SBSFC045-has-table-privilege-projection",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.has_table_privilege",
                                                           {api::EngineProjectionFunctionArgument{
                                                                "table_uuid", "uuid", kTableUuid, false},
                                                            api::EngineProjectionFunctionArgument{
                                                                "privilege", "character", "SELECT", false}}),
                                        api::EngineApiRequest{}}),
           true) && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC045-has-column-privilege-projection",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.has_column_privilege",
                                                           {api::EngineProjectionFunctionArgument{
                                                                "table_uuid", "uuid", kTableUuid, false},
                                                            api::EngineProjectionFunctionArgument{
                                                                "column_name", "character", "id", false},
                                                            api::EngineProjectionFunctionArgument{
                                                                "privilege", "character", "SELECT", false}}),
                                        api::EngineApiRequest{}}),
           true) && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC045-has-function-privilege-projection",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.has_function_privilege",
                                                           {api::EngineProjectionFunctionArgument{
                                                                "function_name", "character", "has_table_privilege", false},
                                                            api::EngineProjectionFunctionArgument{
                                                                "privilege", "character", "EXECUTE", false}}),
                                        api::EngineApiRequest{}}),
           true) && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC045-has-schema-privilege-projection",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.has_schema_privilege",
                                                           {api::EngineProjectionFunctionArgument{
                                                                "schema_name", "character", "current_schema", false},
                                                            api::EngineProjectionFunctionArgument{
                                                                "privilege", "character", "USAGE", false}}),
                                        api::EngineApiRequest{}}),
           true) && ok;

  CleanupDatabase(database_path);
  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_045_privilege_predicate_runtime_conformance=passed\n";
  return 0;
}
