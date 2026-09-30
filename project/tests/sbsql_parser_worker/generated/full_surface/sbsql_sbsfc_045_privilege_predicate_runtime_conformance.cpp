#include "../../../support/binary_uuid_fixture.hpp"
#include "../../../support/published_mga_table_fixture.hpp"
#include "../../../support/engine_statement_fixture.hpp"
#include "../../../support/sb_test_temp_compat.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "dispatch/function_dispatch.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/projection_api.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_dispatch.hpp"
#include "canonical_projection_test_envelope.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
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

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    throw std::runtime_error(std::string(message));
  }
}

class OwnedTempDirectory {
 public:
  OwnedTempDirectory() {
    const auto pattern = (std::filesystem::temp_directory_path() / "sbsfc045_XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const auto* created = mkdtemp(name.data());
    if (!created) throw std::runtime_error("SBSFC045 temporary directory creation failed");
    path_ = created;
  }
  ~OwnedTempDirectory() {
    if (path_.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error) std::cerr << "SBSFC045 fixture cleanup failed: " << error.message() << '\n';
  }
  OwnedTempDirectory(const OwnedTempDirectory&) = delete;
  OwnedTempDirectory& operator=(const OwnedTempDirectory&) = delete;
  std::filesystem::path database_path() const { return path_ / "privileges.sbdb"; }
  void Cleanup() {
    std::filesystem::remove_all(path_);
    path_.clear();
  }
 private:
  std::filesystem::path path_;
};

api::EngineRequestContext CreateCredentialedDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1789810450000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1789810450001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1789810450002;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = false;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "SBSFC045 database create failed");
  auto context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  const auto request_uuid = uuid::IssueRuntimeIdentityV7();
  Require(request_uuid.has_value(), "SBSFC045 request identity issuance failed");
  context.request_id.assign(reinterpret_cast<const char*>(request_uuid->bytes.data()),
                            request_uuid->bytes.size());
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000002");
  context.current_schema_uuid = scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000004");
  return context;
}

api::EngineRequestContext BeginTransaction(const api::EngineRequestContext& owner) {
  api::EngineBeginTransactionRequest begin;
  begin.context = owner;
  const auto begun = api::EngineBeginTransaction(begin);
  for (const auto& diagnostic : begun.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(begun.ok, "SBSFC045 transaction.begin failed");
  Require(begun.local_transaction_id != 0, "SBSFC045 transaction.begin returned no local id");
  auto context = owner;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void SeedPrivilegeFixture(api::EngineRequestContext& context) {
  api::EngineCatalogCreateObjectRequest schema;
  schema.context = context;
  schema.target_object.uuid = context.current_schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back({"en", "primary", "", "current_schema", true});
  const auto published = api::EngineCatalogCreateObject(schema);
  for (const auto& diagnostic : published.diagnostics)
    if (!published.ok) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Require(published.ok, "SBSFC045 schema catalog publication failed");
  api::CrudTableRecord table;
  table.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101");
  table.default_name = "sbsfc045_privilege_target";
  table.columns = {{"id", "type=int64"}, {"note", "type=character"}};
  const auto diagnostic = scratchbird::tests::PublishMgaTableFixture(
      context, table, {"int64", "character"});
  Require(!diagnostic.error, "SBSFC045 table metadata append failed");
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
  out.database_uuid = context.database_uuid;
  out.current_schema_uuid = context.current_schema_uuid;
  out.session_uuid = context.session_uuid;
  out.user_uuid = context.principal_uuid;
  out.statement_uuid = context.statement_uuid;
  out.transaction_uuid = context.transaction_uuid;
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
  // The fixture resolves its symbolic test case through the published seed
  // registry; executable dispatch receives the registry's binary identity.
  if (const auto* entry = registry.Lookup(function_id))
    request.context.function_uuid = entry->function_uuid;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context = SblrContextFromEngine(context);
  request.context.engine_request_context = &context;
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
    if (arguments[index].type_name == "uuid" && !arguments[index].is_null) {
      Require(arguments[index].encoded_value.empty(), "UUID projection fixture has a text mirror");
      envelope.operands.push_back(scratchbird::tests::sbsql::UuidProjectionOperandForTest(
          prefix + "value", arguments[index].binary_value));
    } else {
      envelope.operands.push_back({"text", prefix + "value", arguments[index].encoded_value});
    }
    envelope.operands.push_back({"text", prefix + "is_null", arguments[index].is_null ? "true" : "false"});
  }
  return scratchbird::tests::sbsql::CanonicalizeProjectionEnvelopeForTest(
      std::move(envelope));
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

int RunFixture(const std::filesystem::path& database_path) {
  const auto memory = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(),
      "sbsfc045-credentialed-privilege-fixture");
  Require(memory.ok() && memory.fixture_mode,
          "SBSFC045 explicit fixture memory configuration failed");
  const auto owner = CreateCredentialedDatabase(database_path);
  scratchbird::tests::FixtureEngineSession session(owner);
  auto context = BeginTransaction(owner);
  SeedPrivilegeFixture(context);
  scratchbird::tests::FixtureEngineStatement statement(session, context);
  context = statement.context;
  Require(uuid::IsEngineIdentityUuid(context.statement_uuid),
          "SBSFC045 retained statement has no engine-issued UUIDv7 identity");

  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  // A UUID argument is data, not a replacement for the authenticated user.
  // Keep it binary and distinct from the principal read from the real catalog.
  const auto foreign_principal = scratchbird::tests::FixtureUuidLiteral(
      "019f4500-0000-7000-8000-000000000003");
  Require(foreign_principal != context.principal_uuid,
          "SBSFC045 foreign-principal fixture unexpectedly matches the owner");
  const auto foreign = sblr::MakeSblrUuidValue(foreign_principal);
  const auto table_value = sblr::MakeSblrUuidValue(
      scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101"));
  ok = ExpectBoolean("SBSFC045-has-table-privilege-foreign-principal",
      RunFunction(registry, context, "sb.scalar.has_table_privilege",
          {foreign, table_value, TextValue("character", "SELECT")}), false) && ok;
  ok = ExpectBoolean("SBSFC045-has-column-privilege-foreign-principal",
      RunFunction(registry, context, "sb.scalar.has_column_privilege",
          {foreign, table_value, TextValue("character", "id"),
           TextValue("character", "SELECT")}), false) && ok;
  ok = ExpectBoolean("SBSFC045-has-function-privilege-foreign-principal",
      RunFunction(registry, context, "sb.scalar.has_function_privilege",
          {foreign, TextValue("character", "has_function_privilege"),
           TextValue("character", "EXECUTE")}), false) && ok;
  ok = ExpectBoolean("SBSFC045-has-schema-privilege-foreign-principal",
      RunFunction(registry, context, "sb.scalar.has_schema_privilege",
          {foreign, TextValue("character", "current_schema"),
           TextValue("character", "USAGE")}), false) && ok;

  ok = ExpectBoolean("SBSFC045-has-table-privilege-current-owner",
                     RunFunction(registry, context, "sb.scalar.has_table_privilege",
                                 {scratchbird::engine::sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101")),
                                  TextValue("character", "SELECT")}),
                     true) && ok;
  ok = ExpectBoolean("SBSFC045-has-table-privilege-optional-user",
                     RunFunction(registry, context, "sb.scalar.has_table_privilege",
                                 {scratchbird::engine::sblr::MakeSblrUuidValue(context.principal_uuid),
                                  scratchbird::engine::sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101")),
                                  TextValue("character", "UPDATE")}),
                     true) && ok;
  ok = ExpectNull("SBSFC045-has-table-privilege-null",
                  RunFunction(registry, context, "sb.scalar.has_table_privilege",
                              {NullValue("uuid"), TextValue("character", "SELECT")}),
                  "boolean") && ok;
  ok = ExpectBoolean("SBSFC045-has-table-privilege-unknown",
                     RunFunction(registry, context, "sb.scalar.has_table_privilege",
                                 {scratchbird::engine::sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000999")),
                                  TextValue("character", "SELECT")}),
                     false) && ok;

  ok = ExpectBoolean("SBSFC045-has-column-privilege-current-owner",
                     RunFunction(registry, context, "sb.scalar.has_column_privilege",
                                 {scratchbird::engine::sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101")),
                                  TextValue("character", "id"),
                                  TextValue("character", "SELECT")}),
                     true) && ok;
  ok = ExpectBoolean("SBSFC045-has-column-privilege-optional-user",
                     RunFunction(registry, context, "sb.scalar.has_column_privilege",
                                 {scratchbird::engine::sblr::MakeSblrUuidValue(context.principal_uuid),
                                  scratchbird::engine::sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101")),
                                  TextValue("character", "note"),
                                  TextValue("character", "UPDATE")}),
                     true) && ok;
  ok = ExpectNull("SBSFC045-has-column-privilege-null",
                  RunFunction(registry, context, "sb.scalar.has_column_privilege",
                              {scratchbird::engine::sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101")),
                               NullValue("character"),
                               TextValue("character", "SELECT")}),
                  "boolean") && ok;
  ok = ExpectBoolean("SBSFC045-has-column-privilege-unknown-column",
                     RunFunction(registry, context, "sb.scalar.has_column_privilege",
                                 {scratchbird::engine::sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101")),
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
                                 {scratchbird::engine::sblr::MakeSblrUuidValue(context.principal_uuid),
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
                                 {scratchbird::engine::sblr::MakeSblrUuidValue(context.principal_uuid),
                                  scratchbird::engine::sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000004")),
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
                                                           {scratchbird::tests::sbsql::UuidProjectionArgumentForTest("table_uuid", scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101")),
                                                            api::EngineProjectionFunctionArgument{
                                                                "privilege", "character", "SELECT", false}}),
                                        api::EngineApiRequest{}}),
           true) && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC045-has-column-privilege-projection",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.has_column_privilege",
                                                           {scratchbird::tests::sbsql::UuidProjectionArgumentForTest("table_uuid", scratchbird::tests::FixtureUuidLiteral("019f4500-0000-7000-8000-000000000101")),
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

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_045_privilege_predicate_runtime_conformance=passed\n";
  return 0;
}

int main() {
  try {
    OwnedTempDirectory temporary;
    const auto result = RunFixture(temporary.database_path());
    temporary.Cleanup();
    return result;
  } catch (const std::exception& error) {
    std::cerr << "SBSFC045 fixture failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
