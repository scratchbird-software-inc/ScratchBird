// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "query/projection_api.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_dispatch.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

using scratchbird::engine::functions::BuildStandardFunctionSeedPackage;
using scratchbird::engine::functions::DispatchFunctionCall;
using scratchbird::engine::functions::FunctionArgument;
using scratchbird::engine::functions::FunctionCallRequest;
using scratchbird::engine::functions::FunctionRegistry;
using sblr::SblrResult;
using sblr::SblrValue;
using sblr::SblrValuePayloadKind;

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
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-031-txid-runtime-db";
  if (transaction_context_present) {
    request.context.sblr_context.transaction_uuid = "SBSFC-031-txid-runtime-tx";
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

api::EngineRequestContext EngineContext(const std::string& database_path) {
  api::EngineRequestContext context;
  context.database_path = database_path;
  context.security_context_present = true;
  context.local_transaction_id = kCurrentTxid;
  context.snapshot_visible_through_local_transaction_id = kSnapshotVisibleThrough;
  context.transaction_uuid.canonical = "SBSFC-031-txid-runtime-tx";
  context.transaction_isolation_level = "read_committed";
  return context;
}

std::string TempDatabasePath() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() /
          ("sbsfc031_txid_surface_runtime_" + std::to_string(stamp) + ".sbdb")).string();
}

void CleanupMgaSidecars(const std::string& database_path) {
  std::filesystem::remove(database_path + ".sb.mga_savepoints");
  std::filesystem::remove(database_path + ".sb.mga_row_versions");
  std::filesystem::remove(database_path + ".sb.mga_relation_metadata");
  std::filesystem::remove(database_path + ".sb.mga_index_entries");
  std::filesystem::remove(database_path + ".sb.mga_relation_descriptors");
  std::filesystem::remove(database_path + ".sb.mga_large_values");
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
  return envelope;
}

sblr::SblrOperationEnvelope SavepointEnvelope(std::string operation_id,
                                              std::string opcode,
                                              std::string savepoint_name) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         "SBSFC031-savepoint-marker");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back({"text", "savepoint_name", std::move(savepoint_name)});
  return envelope;
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
  if (value->is_null || value->descriptor.canonical_type_name != "uint64" ||
      value->encoded_value != std::to_string(expected)) {
    std::cerr << case_id << ": expected projected uint64 " << expected << ", got "
              << value->descriptor.canonical_type_name << " " << value->encoded_value << "\n";
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
  if (value->is_null || value->descriptor.canonical_type_name != "boolean" ||
      value->encoded_value != expected_text) {
    std::cerr << case_id << ": expected projected boolean " << expected_text << ", got "
              << value->descriptor.canonical_type_name << " " << value->encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectSavepointDispatchOk(std::string_view case_id,
                               const sblr::SblrDispatchResult& result) {
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api ||
      !result.api_result.ok) {
    std::cerr << case_id << ": savepoint dispatch failed\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << "  envelope diagnostic " << diagnostic.code << ": "
                << diagnostic.message << "\n";
    }
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << "  api diagnostic " << diagnostic.code << ": "
                << diagnostic.detail << "\n";
    }
    return false;
  }
  return true;
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

  const auto database_path = TempDatabasePath();
  CleanupMgaSidecars(database_path);
  const auto context = EngineContext(database_path);

  ok = ExpectProjectionUint64(
           "SBSFC031-mga-snapshot-id-projection-context",
           sblr::DispatchSblrOperation({context,
                                        ProjectionEnvelope("sb.scalar.mga_snapshot_id", {}),
                                        api::EngineApiRequest{}}),
           kSnapshotVisibleThrough) && ok;
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
  ok = ExpectSavepointDispatchOk(
           "SBSFC031-savepoint-create-live",
           sblr::DispatchSblrOperation({context,
                                        SavepointEnvelope("transaction.create_savepoint",
                                                          "SBLR_TRANSACTION_CREATE_SAVEPOINT",
                                                          "sp_live"),
                                        api::EngineApiRequest{}})) && ok;
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
  ok = ExpectSavepointDispatchOk(
           "SBSFC031-savepoint-release-live",
           sblr::DispatchSblrOperation({context,
                                        SavepointEnvelope("transaction.release_savepoint",
                                                          "SBLR_TRANSACTION_RELEASE_SAVEPOINT",
                                                          "sp_live"),
                                        api::EngineApiRequest{}})) && ok;
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
  CleanupMgaSidecars(database_path);

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_031_txid_surface_runtime_conformance=passed\n";
  return 0;
}
