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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace functions = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;
using sblr::SblrValue;
using sblr::SblrValuePayloadKind;

constexpr const char* kSessionUuid = "019f4700-0000-7000-8000-000000000002";
constexpr const char* kOtherSessionUuid = "019f4700-0000-7000-8000-000000000099";
constexpr const char* kPrincipalUuid = "019f4700-0000-7000-8000-000000000003";
constexpr std::uint64_t kBackendPid = 271828;
constexpr std::int64_t kBlockingLockKey = 4242;
constexpr std::int64_t kTryLockKey = 7777;
constexpr std::int64_t kOtherOwnerLockKey = 8888;
constexpr std::int64_t kProjectionLockKey = 5150;

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

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::none;
  value.is_null = true;
  return value;
}

sblr::SblrExecutionContext BaseSblrContext() {
  sblr::SblrExecutionContext context;
  context.session_uuid = kSessionUuid;
  context.user_uuid = kPrincipalUuid;
  context.application_name = "sbsfc047-advisory-lock";
  context.security_context_present = true;
  context.backend_process_id = kBackendPid;
  return context;
}

sblr::SblrResult RunFunction(const functions::FunctionRegistry& registry,
                             const sblr::SblrExecutionContext& context,
                             std::string function_id,
                             std::vector<SblrValue> values) {
  functions::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context = context;
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(
        functions::FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return functions::DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectText(std::string_view case_id,
                const sblr::SblrResult& result,
                std::string_view expected) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected successful scalar result\n";
    return false;
  }
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "character" ||
      value.text_value != expected) {
    std::cerr << case_id << ": expected character " << expected << ", got "
              << value.descriptor_id << " " << value.text_value << '\n';
    return false;
  }
  return true;
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
    std::cerr << case_id << ": expected null " << descriptor << ", got "
              << value.descriptor_id << " " << value.encoded_value << '\n';
    return false;
  }
  return true;
}

const sblr::SblrSessionAdvisoryLockEntry* FindLockEntry(
    const sblr::SblrExecutionContext& context,
    std::int64_t key) {
  if (!context.session_runtime_state) return nullptr;
  for (const auto& entry : context.session_runtime_state->advisory_lock_entries) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

bool ContainsEvidence(const sblr::SblrExecutionContext& context,
                      std::string_view fragment) {
  if (!context.session_runtime_state) return false;
  return std::any_of(
      context.session_runtime_state->advisory_lock_evidence.begin(),
      context.session_runtime_state->advisory_lock_evidence.end(),
      [fragment](const std::string& evidence) {
        return evidence.find(fragment) != std::string::npos;
      });
}

sblr::SblrOperationEnvelope ProjectionEnvelope(
    std::string function_id,
    std::vector<api::EngineProjectionFunctionArgument> arguments) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "SBSFC047-advisory-lock-projection");
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

api::EngineRequestContext ProjectionContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsfc047-advisory-lock-projection";
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.application_name = "sbsfc047-advisory-lock";
  context.security_context_present = true;
  return context;
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
  const auto expected_text = expected ? "1" : "0";
  if (value.is_null || value.descriptor.canonical_type_name != "boolean" ||
      value.encoded_value != expected_text) {
    std::cerr << case_id << ": expected projected boolean " << expected_text << ", got "
              << value.descriptor.canonical_type_name << " " << value.encoded_value << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  auto context = BaseSblrContext();
  bool ok = true;

  ok = ExpectText("SBSFC047-pg-advisory-lock-marker",
                  RunFunction(registry, context, "sb.scalar.pg_advisory_lock", {}),
                  "session_advisory_lock.local_context.route") && ok;
  ok = ContainsEvidence(context, "pg_advisory_lock.marker:no_key") && ok;

  ok = ExpectBoolean("SBSFC047-pg-advisory-lock-acquire",
                     RunFunction(registry, context, "sb.scalar.pg_advisory_lock_key",
                                 {Int64Value(kBlockingLockKey)}),
                     true) && ok;
  ok = ExpectBoolean("SBSFC047-pg-advisory-lock-reentrant",
                     RunFunction(registry, context, "sb.scalar.pg_advisory_lock_key",
                                 {Int64Value(kBlockingLockKey)}),
                     true) && ok;
  const auto* blocking_entry = FindLockEntry(context, kBlockingLockKey);
  ok = (blocking_entry != nullptr &&
        blocking_entry->owner_session_uuid == kSessionUuid &&
        blocking_entry->acquisition_count == 2) && ok;

  ok = ExpectNull("SBSFC047-pg-advisory-lock-null-key",
                  RunFunction(registry, context, "sb.scalar.pg_advisory_lock_key",
                              {NullValue("int64")}),
                  "boolean") && ok;

  ok = ExpectBoolean("SBSFC047-pg-try-advisory-lock-marker",
                     RunFunction(registry, context, "sb.scalar.pg_try_advisory_lock", {}),
                     false) && ok;
  ok = ExpectBoolean("SBSFC047-pg-try-advisory-lock-acquire",
                     RunFunction(registry, context, "sb.scalar.pg_try_advisory_lock_key",
                                 {Int64Value(kTryLockKey)}),
                     true) && ok;
  ok = ExpectBoolean("SBSFC047-pg-try-advisory-lock-owned",
                     RunFunction(registry, context, "sb.scalar.pg_try_advisory_lock_key",
                                 {Int64Value(kTryLockKey)}),
                     true) && ok;

  context.session_runtime_state->advisory_lock_entries.push_back(
      sblr::SblrSessionAdvisoryLockEntry{kOtherOwnerLockKey, kOtherSessionUuid, 1});
  ok = ExpectBoolean("SBSFC047-pg-try-advisory-lock-other-owner",
                     RunFunction(registry, context, "sb.scalar.pg_try_advisory_lock_key",
                                 {Int64Value(kOtherOwnerLockKey)}),
                     false) && ok;
  const auto* other_owner_entry = FindLockEntry(context, kOtherOwnerLockKey);
  ok = (other_owner_entry != nullptr &&
        other_owner_entry->owner_session_uuid == kOtherSessionUuid &&
        other_owner_entry->acquisition_count == 1) && ok;
  ok = ContainsEvidence(context, "pg_try_advisory_lock.other_owner:") && ok;

  ok = ExpectProjectionBoolean(
           "SBSFC047-pg-try-advisory-lock-projection",
           sblr::DispatchSblrOperation(
               {ProjectionContext(),
                ProjectionEnvelope(
                    "sb.scalar.pg_try_advisory_lock_key",
                    {api::EngineProjectionFunctionArgument{
                        "key", "int64", std::to_string(kProjectionLockKey), false}}),
                api::EngineApiRequest{}}),
           true) && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_047_advisory_lock_runtime_conformance=passed\n";
  return 0;
}
