#include "../../../support/binary_uuid_fixture.hpp"
#include "../../../support/engine_statement_fixture.hpp"
#include "../../../support/database_fixture_cleanup.hpp"
#include "../../../database_lifecycle/database_lifecycle_test_memory.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dispatch/function_dispatch.hpp"
#include "query/projection_api.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_dispatch.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "canonical_projection_test_envelope.hpp"

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
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

using scratchbird::engine::functions::BuildStandardFunctionSeedPackage;
using scratchbird::engine::functions::DispatchFunctionCall;
using scratchbird::engine::functions::FunctionArgument;
using scratchbird::engine::functions::FunctionCallRequest;
using scratchbird::engine::functions::FunctionRegistry;
using sblr::SblrResult;
using sblr::SblrValue;
using sblr::SblrValuePayloadKind;

// Context-echo cases below supply these through the function dispatch context
// directly; they read back what the caller stated and touch no database. The
// projection and savepoint cases use a real created database and a real begun
// transaction instead, so they must not reuse these constants.
constexpr std::uint64_t kCurrentTxid = 31031;
constexpr std::uint64_t kSnapshotVisibleThrough = 31030;

SblrValue NullValue(std::string descriptor = {}) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

SblrValue Int64Value(std::int64_t input) {
  SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = SblrValuePayloadKind::signed_integer;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrResult Run(const FunctionRegistry& registry,
               std::string function_id,
               std::vector<SblrValue> values,
               bool transaction_context_present = true) {
  FunctionCallRequest request;
  // The fixture resolves its symbolic test case through the published seed
  // registry; executable dispatch receives the registry's binary identity.
  if (const auto* entry = registry.Lookup(function_id))
    request.context.function_uuid = entry->function_uuid;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = scratchbird::tests::FixtureUuid(1156, 25);
  if (transaction_context_present) {
    request.context.sblr_context.transaction_uuid = scratchbird::tests::FixtureUuid(1156, 26);
    request.context.sblr_context.transaction_context_present = true;
    request.context.sblr_context.local_transaction_id = kCurrentTxid;
    request.context.sblr_context.snapshot_visible_through_local_transaction_id =
        kSnapshotVisibleThrough;
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1 ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected one successful non-mutating scalar result\n";
    return false;
  }
  return true;
}

bool ExpectUint64(std::string_view case_id, const SblrResult& result, std::uint64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "uint64" || !value.has_uint64_value ||
      value.uint64_value != expected) {
    std::cerr << case_id << ": expected uint64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBoolean(std::string_view case_id, const SblrResult& result, bool expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  const std::int64_t expected_int = expected ? 1 : 0;
  if (value.is_null || value.descriptor_id != "boolean" || !value.has_int64_value ||
      value.int64_value != expected_int) {
    std::cerr << case_id << ": expected boolean " << expected_int << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id, const SblrResult& result, std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "character" || value.encoded_value != expected) {
    std::cerr << case_id << ": expected character " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id, const SblrResult& result, std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << ", got "
              << value.descriptor_id << "\n";
    return false;
  }
  return true;
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path TempDatabasePath() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("sbsfc031_txid_surface_runtime_" + std::to_string(stamp) + ".sbdb");
}

void CleanupDatabase(const std::filesystem::path& path) {
  scratchbird::tests::RemoveDatabaseFixtureArtifacts(path);
}

// The savepoint API admits a mutation only against the database's own current
// transaction inventory, so the savepoint cases need a real created database
// and a real EngineBeginTransaction. A fabricated path with a guessed
// transaction UUID owns no native transaction and is refused -- correctly.
db::DatabaseCreateConfig CreateFixtureDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1789810431000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1789810431001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1789810431002;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "SBSFC031 database create failed");
  return create;
}

api::EngineRequestContext BeginTransaction(const db::DatabaseCreateConfig& create) {
  api::EngineBeginTransactionRequest begin;
  begin.context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  begin.context.request_id = "sbsfc031-txid-surface";
  begin.context.session_uuid = scratchbird::tests::FixtureUuid(1156, 24);
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = 1;
  begin.context.name_resolution_epoch = 1;
  const auto begun = api::EngineBeginTransaction(begin);
  for (const auto& diagnostic : begun.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(begun.ok, "SBSFC031 transaction.begin failed");
  Require(begun.local_transaction_id != 0, "SBSFC031 transaction.begin returned no local id");
  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

sblr::SblrOperationEnvelope ProjectionEnvelope(std::string function_id,
                                               std::vector<api::EngineProjectionFunctionArgument> arguments) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "SBSFC031-transaction-context-projection");
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
  return scratchbird::tests::sbsql::CanonicalizeProjectionEnvelopeForTest(
      std::move(envelope));
}

const api::EngineTypedValue* ProjectionField(const sblr::SblrDispatchResult& result,
                                             std::string_view case_id) {
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api ||
      !result.api_result.ok || result.api_result.result_shape.rows.size() != 1 ||
      result.api_result.result_shape.rows.front().fields.size() != 1) {
    std::cerr << case_id << ": expected one successful projected scalar field\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << "  envelope diagnostic " << diagnostic.code << ": "
                << diagnostic.message << "\n";
    }
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << "  api diagnostic " << diagnostic.code << ": "
                << diagnostic.detail << "\n";
    }
    return nullptr;
  }
  return &result.api_result.result_shape.rows.front().fields.front().second;
}

bool ExpectProjectionUint64(std::string_view case_id,
                            const sblr::SblrDispatchResult& result,
                            std::uint64_t expected) {
  const auto* value = ProjectionField(result, case_id);
  if (value == nullptr) return false;
  // Canonical scalars are carried as native bytes; encoded_value has no text
  // alternative for them, so asserting against it always read an empty field.
  std::uint64_t carried = 0;
  if (value->binary_value.size() == 8)
    for (unsigned i = 0; i < 8; ++i)
      carried |= static_cast<std::uint64_t>(value->binary_value[i]) << (8 * i);
  if (value->is_null || value->descriptor.canonical_type_name != "uint64" ||
      value->binary_value.size() != 8 || carried != expected) {
    std::cerr << case_id << ": expected projected uint64 " << expected << ", got "
              << value->descriptor.canonical_type_name << " carrier["
              << value->binary_value.size() << "]=" << carried << "\n";
    return false;
  }
  return true;
}

bool ExpectProjectionText(std::string_view case_id,
                          const sblr::SblrDispatchResult& result,
                          std::string_view expected) {
  const auto* value = ProjectionField(result, case_id);
  if (value == nullptr) return false;
  if (value->is_null || value->descriptor.canonical_type_name != "character" ||
      value->encoded_value != expected) {
    std::cerr << case_id << ": expected projected character " << expected << ", got "
              << value->descriptor.canonical_type_name << " " << value->encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectProjectionNull(std::string_view case_id,
                          const sblr::SblrDispatchResult& result,
                          std::string_view descriptor) {
  const auto* value = ProjectionField(result, case_id);
  if (value == nullptr) return false;
  if (!value->is_null || value->descriptor.canonical_type_name != descriptor) {
    std::cerr << case_id << ": expected projected NULL descriptor " << descriptor << ", got "
              << value->descriptor.canonical_type_name << "\n";
    return false;
  }
  return true;
}

bool ExpectProjectionBoolean(std::string_view case_id,
                             const sblr::SblrDispatchResult& result,
                             bool expected) {
  const auto* value = ProjectionField(result, case_id);
  if (value == nullptr) return false;
  const std::string expected_text = expected ? "1" : "0";
  // Canonical scalars are carried as native bytes; encoded_value has no text
  // alternative for them, so asserting against it always read an empty field.
  const unsigned expected_byte = expected ? 1u : 0u;
  if (value->is_null || value->descriptor.canonical_type_name != "boolean" ||
      value->binary_value.size() != 1 || value->binary_value[0] != expected_byte) {
    std::cerr << case_id << ": expected projected boolean " << expected_text << ", got "
              << value->descriptor.canonical_type_name << " carrier["
              << value->binary_value.size() << "]="
              << (value->binary_value.empty() ? -1 : int(value->binary_value[0])) << "\n";
    return false;
  }
  return true;
}

bool ExpectSavepointApiOk(std::string_view case_id,
                          const api::EngineApiResult& result) {
  if (!result.ok) {
    std::cerr << case_id << ": savepoint API setup failed\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << "  API diagnostic " << diagnostic.code << ": "
                << diagnostic.detail << "\n";
    }
    return false;
  }
  return true;
}

api::EngineCreateSavepointResult CreateSavepoint(
    const api::EngineRequestContext& context,
    std::string name) {
  api::EngineCreateSavepointRequest request;
  request.context = context;
  request.option_envelopes.push_back("savepoint_name:" + std::move(name));
  return api::EngineCreateSavepoint(request);
}

api::EngineReleaseSavepointResult ReleaseSavepoint(
    const api::EngineRequestContext& context,
    std::string name) {
  api::EngineReleaseSavepointRequest request;
  request.context = context;
  request.option_envelopes.push_back("savepoint_name:" + std::move(name));
  return api::EngineReleaseSavepoint(request);
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectUint64("SBSFC031-txid-current-context",
                    Run(registry, "sb.session.transaction_id", {}),
                    kCurrentTxid) && ok;
  ok = ExpectNull("SBSFC031-txid-current-no-context-null",
                  Run(registry, "sb.session.transaction_id", {}, false),
                  "uint64") && ok;
  ok = ExpectText("SBSFC031-txid-status-current-in-progress",
                  Run(registry, "sb.session.txid_status", {}),
                  "in_progress") && ok;
  ok = ExpectNull("SBSFC031-txid-status-no-context-null",
                  Run(registry, "sb.session.txid_status", {}, false),
                  "character") && ok;
  ok = ExpectText("SBSFC031-txid-status-bigint-in-progress",
                  Run(registry, "sb.session.txid_status", {Int64Value(kCurrentTxid)}),
                  "in_progress") && ok;
  ok = ExpectText("SBSFC031-txid-status-bigint-unknown",
                  Run(registry, "sb.session.txid_status", {Int64Value(99999)}),
                  "unknown") && ok;
  ok = ExpectNull("SBSFC031-txid-status-bigint-null",
                  Run(registry, "sb.session.txid_status", {NullValue("bigint")}),
                  "character") && ok;
  ok = ExpectUint64("SBSFC031-mga-snapshot-id-context",
                    Run(registry, "sb.scalar.mga_snapshot_id", {}),
                    kSnapshotVisibleThrough) && ok;
  ok = ExpectNull("SBSFC031-mga-snapshot-id-no-context-null",
                  Run(registry, "sb.scalar.mga_snapshot_id", {}, false),
                  "uint64") && ok;
  ok = ExpectText("SBSFC031-pg-xact-status-current-in-progress",
                  Run(registry, "sb.session.pg_xact_status", {}),
                  "in_progress") && ok;
  ok = ExpectNull("SBSFC031-pg-xact-status-no-context-null",
                  Run(registry, "sb.session.pg_xact_status", {}, false),
                  "character") && ok;
  ok = ExpectText("SBSFC031-pg-xact-status-bigint-in-progress",
                  Run(registry, "sb.session.pg_xact_status", {Int64Value(kCurrentTxid)}),
                  "in_progress") && ok;
  ok = ExpectText("SBSFC031-pg-xact-status-bigint-unknown",
                  Run(registry, "sb.session.pg_xact_status", {Int64Value(99999)}),
                  "unknown") && ok;
  ok = ExpectNull("SBSFC031-pg-xact-status-bigint-null",
                  Run(registry, "sb.session.pg_xact_status", {NullValue("bigint")}),
                  "character") && ok;
  ok = ExpectBoolean("SBSFC031-savepoint-active-direct-empty-false",
                     Run(registry, "sb.session.savepoint_active", {}),
                     false) && ok;
  ok = ExpectNull("SBSFC031-savepoint-active-name-null",
                  Run(registry, "sb.session.savepoint_active", {NullValue("character")}),
                  "boolean") && ok;

  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "sbsfc031-txid-surface");
  const auto database_path = TempDatabasePath();
  CleanupDatabase(database_path);
  const auto database = CreateFixtureDatabase(database_path);
  const auto context = BeginTransaction(database);

  // The snapshot watermark belongs to the transaction the engine actually
  // began, so expect the value it reported rather than a fixture constant.
  ok = ExpectProjectionUint64(
           "SBSFC031-mga-snapshot-id-projection-context",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.mga_snapshot_id", {}),
                                        api::EngineApiRequest{}}),
           context.snapshot_visible_through_local_transaction_id) && ok;
  ok = ExpectProjectionText(
           "SBSFC031-pg-xact-status-projection-context",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.session.pg_xact_status", {}),
                                        api::EngineApiRequest{}}),
           "in_progress") && ok;
  ok = ExpectProjectionNull(
           "SBSFC031-pg-xact-status-projection-null-argument",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.session.pg_xact_status",
                                                           {api::EngineProjectionFunctionArgument{
                                                               "txid", "bigint", "", true}}),
                                        api::EngineApiRequest{}}),
           "character") && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC031-savepoint-active-any-false",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.session.savepoint_active", {}),
                                        api::EngineApiRequest{}}),
           false) && ok;
  ok = ExpectSavepointApiOk(
           "SBSFC031-savepoint-create-live",
           CreateSavepoint(context, "sp_live")) && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC031-savepoint-active-any-true",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.session.savepoint_active", {}),
                                        api::EngineApiRequest{}}),
           true) && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC031-savepoint-active-name-true",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.session.savepoint_active",
                                                           {api::EngineProjectionFunctionArgument{
                                                               "name", "character", "sp_live", false}}),
                                        api::EngineApiRequest{}}),
           true) && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC031-savepoint-active-name-missing-false",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.session.savepoint_active",
                                                           {api::EngineProjectionFunctionArgument{
                                                               "name", "character", "sp_missing", false}}),
                                        api::EngineApiRequest{}}),
           false) && ok;
  ok = ExpectSavepointApiOk(
           "SBSFC031-savepoint-release-live",
           ReleaseSavepoint(context, "sp_live")) && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC031-savepoint-active-name-false-after-release",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.session.savepoint_active",
                                                           {api::EngineProjectionFunctionArgument{
                                                               "name", "character", "sp_live", false}}),
                                        api::EngineApiRequest{}}),
           false) && ok;
  ok = ExpectProjectionBoolean(
           "SBSFC031-savepoint-active-any-false-after-release",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.session.savepoint_active", {}),
                                        api::EngineApiRequest{}}),
           false) && ok;
  CleanupDatabase(database_path);

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_031_txid_surface_runtime_conformance=passed\n";
  return 0;
}
