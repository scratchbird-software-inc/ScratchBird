#include "../../../support/binary_uuid_fixture.hpp"
#include "../../../support/engine_statement_fixture.hpp"
#include "../../../support/ordered_integer_key_oracle.hpp"
#include "../../../support/database_fixture_cleanup.hpp"
#include "../../../database_lifecycle/database_lifecycle_test_memory.hpp"
#include "../../../support/projection_uuid_literal_checks.hpp"
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
#include "canonical_projection_test_envelope.hpp"
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
constexpr char kTableUuid[] = "019f4400-0000-7000-8000-000000000101";
constexpr const char* kIndexUuid = "019f4400-0000-7000-8000-000000000102";
constexpr char kRowA[] = "019f4400-0000-7000-8000-000000000201";
constexpr char kRowB[] = "019f4400-0000-7000-8000-000000000202";
constexpr const char* kVersionA = "019f4400-0000-7000-8000-000000000301";
constexpr const char* kVersionB = "019f4400-0000-7000-8000-000000000302";
constexpr const char* kUnknownTable = "019f4400-0000-7000-8000-000000000999";
constexpr std::uint64_t kExpectedVisibleRows = 2;
// Independent accounting of the fixture's documented relation-size estimate:
// Identities occupy 16 bytes including nil. Retained named values carry a
// state octet; metadata attributes do not. SBCLKEY2 contains an 8-byte magic,
// 4-byte arity, 1-byte state, 4-byte length and the eight-byte LE id. SBVALS01 has
// a 12-byte header plus the length-prefixed name, state and value. These are
// retained metadata/value estimates, not page sizes or process allocations.
// Binary note metadata: SBMETA02 + schema + counts, two text fields
// (character_length=16777216 and type=character), two raw UUID references.
constexpr std::uint64_t kNoteMetadataBytes =
    8 + 4 + 9 + 4 + 4 + (4 + 16 + 4 + 8) + (4 + 4 + 4 + 9) +
    (4 + 12 + 16) + (4 + 14 + 16);
constexpr std::uint64_t kTableBytes = 96 + 2 * 16 + 22 + (8 + 2 + 10) + (8 + 4 + kNoteMetadataBytes);
constexpr std::uint64_t kRowBytes = 128 + 4 * 16 + (9 + 2 + 8) + (9 + 4 + 5);
constexpr std::uint64_t kIndexBytes = 128 + 2 * 16 + 2 + 5 + 24 + 29 + (8 + 2);
constexpr std::uint64_t kLogicalKeyBytes = 8 + 4 + 1 + 4 + 8;
// SBKOBIN: + SBKO + rank + scalar kind + typed UUID + generation +
// absent collation + present int64 payload (six escaped zero bytes) + end.
constexpr std::uint64_t kPhysicalKeyBytes = 8 + 4 + 1 + 4 + 17 + 8 + 1 + 1 + 8 + 6 + 2;
constexpr std::uint64_t kIndexPayloadBytes = kLogicalKeyBytes;
constexpr std::uint64_t kIndexEntryBytes =
    112 + 4 * 16 + 2 + 5 + 5 + kPhysicalKeyBytes + kIndexPayloadBytes;
constexpr std::uint64_t kExpectedRowStoreBytes = kTableBytes + 2 * kRowBytes;
constexpr std::uint64_t kExpectedTableSizeWithIndexes =
    kExpectedRowStoreBytes + kIndexBytes + 2 * kIndexEntryBytes;

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
  scratchbird::tests::RemoveDatabaseFixtureArtifacts(path);
}

db::DatabaseCreateConfig CreateFixtureDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1789810444000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1789810444001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1789810444002;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "SBSFC044 database create failed");
  return create;
}

api::EngineRequestContext BaseContext(const db::DatabaseCreateConfig& create) {
  auto context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  context.request_id = "sbsfc044-catalog-statistics";
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000002");
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext BeginTransaction(const db::DatabaseCreateConfig& create) {
  api::EngineBeginTransactionRequest begin;
  begin.context = BaseContext(create);
  const auto begun = api::EngineBeginTransaction(begin);
  for (const auto& diagnostic : begun.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(begun.ok, "SBSFC044 transaction.begin failed");
  Require(begun.local_transaction_id != 0, "SBSFC044 transaction.begin returned no local id");
  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void SeedCatalogStatisticsFixture(api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101");
  table.default_name = "sbsfc044_catalog_stats";
  table.columns = {{"id", "type=int64"}, {"note", "type=character"}};
  auto diagnostic = scratchbird::tests::PublishMgaTableFixture(context, table, {"int64", "character"});
  Require(!diagnostic.error, "SBSFC044 table metadata append failed");

  api::CrudIndexRecord index;
  index.index_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000102");
  index.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101");
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "sbsfc044_catalog_stats_id_idx";
  index.key_envelopes = {"id"};
  diagnostic = api::AppendMgaIndexMetadata(context, index);
  Require(!diagnostic.error, "SBSFC044 index metadata append failed");

  api::CrudRowVersionRecord row_a;
  row_a.creator_tx = context.local_transaction_id;
  row_a.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101");
  row_a.row_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000201");
  row_a.version_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000301");
  row_a.values = {{"id", std::string("\x01\0\0\0\0\0\0\0", 8)}, {"note", "alpha"}};
  diagnostic = api::AppendMgaRowVersion(context, row_a, nullptr);
  Require(!diagnostic.error, "SBSFC044 row A append failed");
  diagnostic = api::AppendMgaIndexEntriesForIndex(context, index, row_a.row_uuid, row_a.version_uuid, row_a.values);
  Require(!diagnostic.error, "SBSFC044 row A index append failed");

  api::CrudRowVersionRecord row_b;
  row_b.creator_tx = context.local_transaction_id;
  row_b.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101");
  row_b.row_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000202");
  row_b.version_uuid = scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000302");
  row_b.values = {{"id", std::string("\x02\0\0\0\0\0\0\0", 8)}, {"note", "bravo"}};
  diagnostic = api::AppendMgaRowVersion(context, row_b, nullptr);
  Require(!diagnostic.error, "SBSFC044 row B append failed");
  diagnostic = api::AppendMgaIndexEntriesForIndex(context, index, row_b.row_uuid, row_b.version_uuid, row_b.values);
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
  out.database_uuid = context.database_uuid;
  out.session_uuid = context.session_uuid;
  out.user_uuid = context.principal_uuid;
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
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("sbsfc044-catalog-statistics");
  const auto database_path = TempDatabasePath();
  CleanupDatabase(database_path);
  const auto database = CreateFixtureDatabase(database_path);
  auto context = BeginTransaction(database);
  SeedCatalogStatisticsFixture(context);
  const auto observed = api::LoadMgaRelationStoreState(context);
  Require(observed.ok, "SBSFC044 independent retained-state inspection failed");
  Require(observed.state.index_entries.size() == 2,
          "SBSFC044 retained index entry count drifted");
  bool seen_a = false, seen_b = false;
  const auto storage = api::LoadMgaRelationStorageDescriptor(context, scratchbird::tests::FixtureUuidLiteral(kTableUuid));
  Require(storage.ok && storage.descriptor.columns.size() == 2,
          "SBSFC044 bound column descriptor missing");
  const auto& datatype = storage.descriptor.columns.front().value_descriptor;
  for (const auto& entry : observed.state.index_entries) {
    const bool row_a = entry.row_uuid == scratchbird::tests::FixtureUuidLiteral(kRowA);
    const bool row_b = entry.row_uuid == scratchbird::tests::FixtureUuidLiteral(kRowB);
    Require((row_a && !seen_a) || (row_b && !seen_b), "SBSFC044 retained row identity drifted");
    seen_a = seen_a || row_a;
    seen_b = seen_b || row_b;
    const std::string value = row_a ? std::string("\x01\0\0\0\0\0\0\0", 8)
                                    : std::string("\x02\0\0\0\0\0\0\0", 8);
    const auto key = scratchbird::tests::ExpectedInt64OrderedIndexKey(
        datatype.datatype_descriptor_uuid, datatype.datatype_descriptor_generation, row_a ? 1 : 2);
    const std::string payload = std::string("SBCLKEY2\x01\x00\x00\x00\x00\x08\x00\x00\x00", 17) + value;
    Require(entry.column_name == "id" && entry.family == "btree" && entry.entry_kind == "exact" &&
                entry.key_value == key && entry.payload_value == payload &&
                key.size() == kPhysicalKeyBytes && payload.size() == kIndexPayloadBytes,
            "SBSFC044 independent retained index frame accounting drifted");
  }

  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = scratchbird::tests::CheckProjectionUuidLiteralCarriers();

  ok = ExpectUint64("SBSFC044-relation-row-estimate-catalog",
                    RunFunction(registry, context, "sb.scalar.relation_row_estimate", {}),
                    kExpectedVisibleRows) && ok;
  ok = ExpectUint64("SBSFC044-relation-row-estimate-table",
                    RunFunction(registry, context, "sb.scalar.relation_row_estimate",
                                {sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101"))}),
                    kExpectedVisibleRows) && ok;
  ok = ExpectNull("SBSFC044-relation-row-estimate-null",
                  RunFunction(registry, context, "sb.scalar.relation_row_estimate",
                              {NullValue("uuid")}),
                  "uint64") && ok;
  ok = ExpectNull("SBSFC044-relation-row-estimate-unknown",
                  RunFunction(registry, context, "sb.scalar.relation_row_estimate",
                              {sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000999"))}),
                  "uint64") && ok;
  ok = ExpectUint64("SBSFC044-table-size-catalog",
                    RunFunction(registry, context, "sb.scalar.table_size", {}),
                    kExpectedTableSizeWithIndexes) && ok;
  ok = ExpectUint64("SBSFC044-table-size-table-default",
                    RunFunction(registry, context, "sb.scalar.table_size",
                                {sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101"))}),
                    kExpectedTableSizeWithIndexes) && ok;
  ok = ExpectUint64("SBSFC044-table-size-table-no-indexes",
                    RunFunction(registry, context, "sb.scalar.table_size",
                                {sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101")), BooleanValue(false)}),
                    kExpectedRowStoreBytes) && ok;
  ok = ExpectNull("SBSFC044-table-size-null-table",
                  RunFunction(registry, context, "sb.scalar.table_size",
                              {NullValue("uuid")}),
                  "uint64") && ok;
  ok = ExpectNull("SBSFC044-table-size-null-include-indexes",
                  RunFunction(registry, context, "sb.scalar.table_size",
                              {sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101")), NullValue("boolean")}),
                  "uint64") && ok;
  ok = ExpectNull("SBSFC044-table-size-unknown",
                  RunFunction(registry, context, "sb.scalar.table_size",
                              {sblr::MakeSblrUuidValue(scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000999"))}),
                  "uint64") && ok;

  ok = ExpectProjectionUint64(
           "SBSFC044-relation-row-estimate-projection",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.relation_row_estimate",
                                                           {scratchbird::tests::sbsql::UuidProjectionArgumentForTest("table_uuid", scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101"))}),
                                        api::EngineApiRequest{}}),
           kExpectedVisibleRows) && ok;
  ok = ExpectProjectionUint64(
           "SBSFC044-table-size-projection-no-indexes",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.table_size",
                                                           {scratchbird::tests::sbsql::UuidProjectionArgumentForTest("table_uuid", scratchbird::tests::FixtureUuidLiteral("019f4400-0000-7000-8000-000000000101")),
                                                            api::EngineProjectionFunctionArgument{
                                                                "include_indexes", "boolean", "false", false}}),
                                        api::EngineApiRequest{}}),
           kExpectedRowStoreBytes) && ok;

  CleanupDatabase(database_path);
  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_044_catalog_statistics_runtime_conformance=passed\n";
  return 0;
}
