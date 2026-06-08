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

constexpr const char* kSessionUuid = "019f4600-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f4600-0000-7000-8000-000000000003";
constexpr std::uint64_t kBackendPid = 314159;
constexpr std::uint64_t kUnknownCancelPid = 42424242;
constexpr std::uint64_t kUnknownTerminatePid = 42424243;
constexpr std::uint64_t kTerminablePid = 42424244;

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

sblr::SblrExecutionContext BaseSblrContext() {
  sblr::SblrExecutionContext context;
  context.session_uuid = kSessionUuid;
  context.user_uuid = kPrincipalUuid;
  context.application_name = "sbsfc046-session-admin";
  context.security_context_present = true;
  context.backend_process_id = kBackendPid;
  context.session_runtime_state->terminable_backend_pids.push_back(kTerminablePid);
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

bool ExpectPositiveUint64(std::string_view case_id,
                          const sblr::SblrResult& result) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected successful scalar result\n";
    return false;
  }
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "uint64" || !value.has_uint64_value ||
      value.uint64_value == 0) {
    std::cerr << case_id << ": expected positive uint64, got "
              << value.descriptor_id << " " << value.encoded_value << '\n';
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

bool ContainsPid(const std::vector<std::uint64_t>& pids, std::uint64_t pid) {
  return std::find(pids.begin(), pids.end(), pid) != pids.end();
}

sblr::SblrOperationEnvelope ProjectionEnvelope(
    std::string function_id,
    std::vector<api::EngineProjectionFunctionArgument> arguments) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "SBSFC046-session-admin-projection");
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
  context.request_id = "sbsfc046-session-admin-projection";
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.application_name = "sbsfc046-session-admin";
  context.security_context_present = true;
  return context;
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
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  auto context = BaseSblrContext();
  bool ok = true;

  ok = ExpectText("SBSFC046-set-config-marker",
                  RunFunction(registry, context, "sb.scalar.set_config", {}),
                  "session_config.local_context.route") && ok;
  ok = ExpectText("SBSFC046-current-setting-timezone-default",
                  RunFunction(registry, context, "sb.scalar.current_setting",
                              {TextValue("character", "timezone")}),
                  "UTC") && ok;
  ok = ExpectText("SBSFC046-set-config-timezone",
                  RunFunction(registry, context, "sb.scalar.set_config_name_value_is_local",
                              {TextValue("character", "timezone"),
                               TextValue("character", "America/Toronto"),
                               BooleanValue(true)}),
                  "America/Toronto") && ok;
  ok = ExpectText("SBSFC046-current-setting-timezone-applied",
                  RunFunction(registry, context, "sb.scalar.current_setting",
                              {TextValue("character", "time zone")}),
                  "America/Toronto") && ok;
  ok = ExpectText("SBSFC046-set-config-temp-buffers",
                  RunFunction(registry, context, "sb.scalar.set_config_name_value_is_local",
                              {TextValue("character", "temp_buffers"),
                               TextValue("character", "16777216"),
                               BooleanValue(true)}),
                  "16777216") && ok;
  ok = ExpectUint64("SBSFC046-temp-buffers-applied",
                    RunFunction(registry, context, "sb.scalar.temp_buffers", {}),
                    16777216) && ok;
  ok = ExpectUint64("SBSFC046-pid-context",
                    RunFunction(registry, context, "sb.scalar.pid", {}),
                    kBackendPid) && ok;

  ok = ExpectBoolean("SBSFC046-pg-cancel-backend-marker",
                     RunFunction(registry, context, "sb.scalar.pg_cancel_backend", {}),
                     false) && ok;
  ok = ExpectBoolean("SBSFC046-pg-cancel-backend-unknown",
                     RunFunction(registry, context, "sb.scalar.pg_cancel_backend_pid",
                                 {Int64Value(kUnknownCancelPid)}),
                     false) && ok;
  ok = ExpectBoolean("SBSFC046-pg-cancel-backend-current",
                     RunFunction(registry, context, "sb.scalar.pg_cancel_backend_pid",
                                 {Int64Value(kBackendPid)}),
                     true) && ok;
  ok = (ContainsPid(context.session_runtime_state->cancel_requested_backend_pids, kBackendPid) && ok);

  ok = ExpectBoolean("SBSFC046-pg-terminate-backend-marker",
                     RunFunction(registry, context, "sb.scalar.pg_terminate_backend", {}),
                     false) && ok;
  ok = ExpectBoolean("SBSFC046-pg-terminate-backend-unknown",
                     RunFunction(registry, context, "sb.scalar.pg_terminate_backend_pid",
                                 {Int64Value(kUnknownTerminatePid)}),
                     false) && ok;
  ok = ExpectBoolean("SBSFC046-pg-terminate-backend-self-blocked",
                     RunFunction(registry, context, "sb.scalar.pg_terminate_backend_pid",
                                 {Int64Value(kBackendPid)}),
                     false) && ok;
  ok = ExpectBoolean("SBSFC046-pg-terminate-backend-controlled",
                     RunFunction(registry, context, "sb.scalar.pg_terminate_backend_pid",
                                 {Int64Value(kTerminablePid)}),
                     true) && ok;
  ok = (ContainsPid(context.session_runtime_state->terminate_requested_backend_pids,
                    kTerminablePid) && ok);
  ok = (!context.session_runtime_state->backend_control_evidence.empty() && ok);

  ok = ExpectProjectionUint64(
           "SBSFC046-temp-buffers-projection",
           sblr::DispatchSblrOperation({ProjectionContext(),
                                        ProjectionEnvelope("sb.scalar.temp_buffers", {}),
                                        api::EngineApiRequest{}}),
           8388608) && ok;
  ok = ExpectPositiveUint64(
           "SBSFC046-pid-process-fallback",
           RunFunction(registry, sblr::SblrExecutionContext{}, "sb.scalar.pid", {})) && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_046_session_admin_runtime_conformance=passed\n";
  return 0;
}
