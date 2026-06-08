// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_result_helpers.hpp"
#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_runtime.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scratchbird::engine::functions::BuildStandardFunctionSeedPackage;
using scratchbird::engine::functions::DispatchFunctionCall;
using scratchbird::engine::functions::FunctionArgument;
using scratchbird::engine::functions::FunctionCallRequest;
using scratchbird::engine::functions::MakeInt64Value;
using scratchbird::engine::functions::MakeNullValue;
using scratchbird::engine::functions::MakeTextValue;
using scratchbird::engine::sblr::PopSblrFrame;
using scratchbird::engine::sblr::PushSblrFrame;
using scratchbird::engine::sblr::SblrFrame;
using scratchbird::engine::sblr::SblrFrameStack;
using scratchbird::engine::sblr::SblrResult;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;

struct Failure {
  std::string check;
  std::string detail;
};

scratchbird::engine::sblr::SblrExecutionContext MakeContext() {
  scratchbird::engine::sblr::SblrExecutionContext context;
  context.cluster_uuid = "018f0000-0000-7000-8000-00000000c125";
  context.node_uuid = "018f0000-0000-7000-8000-00000000e125";
  context.database_uuid = "018f0000-0000-7000-8000-00000000d125";
  context.transaction_uuid = "018f0000-0000-7000-8000-00000000f125";
  context.local_transaction_id = 125;
  context.statement_uuid = "018f0000-0000-7000-8000-00000000a125";
  context.user_uuid = "018f0000-0000-7000-8000-00000000b125";
  context.current_role_uuid = "018f0000-0000-7000-8000-00000000b126";
  context.current_schema_uuid = "018f0000-0000-7000-8000-00000000b127";
  context.session_uuid = "018f0000-0000-7000-8000-00000000b128";
  context.attachment_uuid = "018f0000-0000-7000-8000-00000000b129";
  context.statement_timestamp = "2026-05-03T17:30:00Z";
  context.transaction_timestamp = "2026-05-03T17:29:00Z";
  context.current_timestamp = "2026-05-03T17:30:30Z";
  context.current_monotonic_ns = "125000000";
  context.deterministic_random_u64 = 424242;
  context.deterministic_random_u64_present = true;
  context.deterministic_uuid_text = "018f0000-0000-7000-8000-00000000d777";
  context.current_sqlstate = "00000";
  context.current_diagnostic_id = "SB_DIAG_OK";
  context.last_identity_value = "9001";
  context.last_identity_value_present = true;
  context.last_row_count = 42;
  context.last_row_count_present = true;
  context.parser_profile_uuid = "018f0000-0000-7000-8000-00000000c126";
  context.client_protocol_uuid = "018f0000-0000-7000-8000-00000000c127";
  context.security_snapshot_uuid = "018f0000-0000-7000-8000-00000000c128";
  context.security_context_present = true;
  context.transaction_context_present = true;
  return context;
}

FunctionCallRequest MakeRequest(std::string function_id, std::vector<SblrValue> values) {
  FunctionCallRequest request;
  request.context.sblr_context = MakeContext();
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  std::uint32_t ordinal = 0;
  for (auto& value : values) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(++ordinal), std::move(value)});
  }
  return request;
}

SblrValue MakeBool(bool value) {
  SblrValue out;
  out.descriptor_id = "BOOLEAN";
  out.payload_kind = scratchbird::engine::sblr::SblrValuePayloadKind::boolean;
  out.is_null = false;
  out.has_int64_value = true;
  out.int64_value = value ? 1 : 0;
  out.text_value = value ? "true" : "false";
  return out;
}

bool ScalarTextEquals(const SblrResult& result, std::string_view expected) {
  return result.status == SblrStatusCode::ok && result.diagnostics.empty() &&
         result.scalar_values.size() == 1 && result.scalar_values[0].text_value == expected;
}

bool ScalarIntEquals(const SblrResult& result, std::int64_t expected) {
  return result.status == SblrStatusCode::ok && result.diagnostics.empty() &&
         result.scalar_values.size() == 1 && result.scalar_values[0].has_int64_value &&
         result.scalar_values[0].int64_value == expected;
}

bool ScalarUintEquals(const SblrResult& result, std::uint64_t expected) {
  return result.status == SblrStatusCode::ok && result.diagnostics.empty() &&
         result.scalar_values.size() == 1 && result.scalar_values[0].has_uint64_value &&
         result.scalar_values[0].uint64_value == expected;
}

bool ScalarIsNull(const SblrResult& result) {
  return result.status == SblrStatusCode::ok && result.diagnostics.empty() &&
         result.scalar_values.size() == 1 && result.scalar_values[0].is_null;
}

bool HasDiagnostic(const SblrResult& result, std::string_view diagnostic_id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == diagnostic_id) return true;
  }
  return false;
}

void AddFailure(std::vector<Failure>* failures, std::string check, std::string detail) {
  failures->push_back(Failure{std::move(check), std::move(detail)});
}

int RunProbe() {
  const auto package = BuildStandardFunctionSeedPackage();
  std::vector<Failure> failures;

  auto call = [&](std::string function_id, std::vector<SblrValue> values) {
    return DispatchFunctionCall(package.registry, MakeRequest(std::move(function_id), std::move(values))).result;
  };

  if (!ScalarTextEquals(call("data.scalar.if",
                             {MakeBool(true), MakeTextValue("TEXT", "then-branch"), MakeTextValue("TEXT", "else-branch")}),
                        "then-branch")) {
    AddFailure(&failures, "data.scalar.if.true", "IF did not return the true branch");
  }
  if (!ScalarTextEquals(call("data.scalar.if",
                             {MakeBool(false), MakeTextValue("TEXT", "then-branch"), MakeTextValue("TEXT", "else-branch")}),
                        "else-branch")) {
    AddFailure(&failures, "data.scalar.if.false", "IF did not return the false branch");
  }
  if (!ScalarTextEquals(call("data.scalar.ifnull", {MakeNullValue("TEXT"), MakeTextValue("TEXT", "fallback")}),
                        "fallback")) {
    AddFailure(&failures, "data.scalar.ifnull", "IFNULL did not return fallback for NULL");
  }
  if (!ScalarTextEquals(call("data.scalar.coalesce",
                             {MakeNullValue("TEXT"), MakeTextValue("TEXT", "first"), MakeTextValue("TEXT", "second")}),
                        "first")) {
    AddFailure(&failures, "data.scalar.coalesce", "COALESCE did not return first non-NULL value");
  }
  if (!ScalarIsNull(call("data.scalar.nullif", {MakeTextValue("TEXT", "same"), MakeTextValue("TEXT", "same")}))) {
    AddFailure(&failures, "data.scalar.nullif", "NULLIF did not return NULL for equal values");
  }
  if (!ScalarIntEquals(call("data.scalar.cast", {MakeTextValue("TEXT", "42"), MakeTextValue("TEXT", "INT64")}), 42)) {
    AddFailure(&failures, "data.scalar.cast", "CAST(TEXT AS INT64) did not return 42");
  }
  if (!ScalarIntEquals(call("data.scalar.extract",
                            {MakeTextValue("TEXT", "minute"), MakeTextValue("TIMESTAMP", "2026-05-03T17:30:30Z")}),
                       30)) {
    AddFailure(&failures, "data.scalar.extract", "EXTRACT(MINUTE) did not return 30");
  }
  if (!ScalarTextEquals(call("data.scalar.current_user", {}), MakeContext().user_uuid)) {
    AddFailure(&failures, "data.scalar.current_user", "current_user did not return context user UUID");
  }
  if (!ScalarTextEquals(call("data.scalar.current_transaction", {}), MakeContext().transaction_uuid)) {
    AddFailure(&failures, "data.scalar.current_transaction", "current_transaction did not return context transaction UUID");
  }
  if (!ScalarTextEquals(call("data.scalar.now", {}), MakeContext().current_timestamp)) {
    AddFailure(&failures, "data.scalar.now", "now did not return injected current timestamp");
  }
  if (!ScalarUintEquals(call("data.scalar.row_count", {}), 42)) {
    AddFailure(&failures, "data.scalar.row_count", "row_count did not return injected last row count");
  }

  const auto overflow = call("data.scalar.abs", {MakeInt64Value("INT64", std::numeric_limits<std::int64_t>::min())});
  if (overflow.status == SblrStatusCode::ok || !HasDiagnostic(overflow, "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW")) {
    AddFailure(&failures, "data.scalar.abs.overflow", "ABS(INT64_MIN) did not report numeric overflow");
  }
  const auto invalid_cast = call("data.scalar.cast", {MakeTextValue("TEXT", "x"), MakeTextValue("TEXT", "INT64")});
  if (invalid_cast.status == SblrStatusCode::ok || !HasDiagnostic(invalid_cast, "SB_DIAG_FUNCTION_INVALID_INPUT")) {
    AddFailure(&failures, "data.scalar.cast.invalid", "Invalid INT64 cast did not report invalid input");
  }
  const auto invalid_extract = call("data.scalar.extract",
                                   {MakeTextValue("TEXT", "nonsense"), MakeTextValue("TIMESTAMP", "2026-05-03T17:30:30Z")});
  if (invalid_extract.status == SblrStatusCode::ok || !HasDiagnostic(invalid_extract, "SB_DIAG_FUNCTION_INVALID_INPUT")) {
    AddFailure(&failures, "data.scalar.extract.invalid", "Invalid EXTRACT field did not report invalid input");
  }

  SblrFrameStack stack;
  stack.max_depth = 1;
  SblrResult frame_failure;
  if (!PushSblrFrame(&stack, SblrFrame{.frame_uuid = "frame-1", .routine_object_uuid = "routine-1"}, &frame_failure)) {
    AddFailure(&failures, "sblr.frame.push", "Initial frame push failed");
  }
  if (PushSblrFrame(&stack, SblrFrame{.frame_uuid = "frame-2", .routine_object_uuid = "routine-2"}, &frame_failure) ||
      frame_failure.status != SblrStatusCode::resource_exhausted ||
      !HasDiagnostic(frame_failure, "SB_DIAG_SBLR_FRAME_DEPTH_EXCEEDED")) {
    AddFailure(&failures, "sblr.frame.depth", "Frame depth exhaustion was not reported deterministically");
  }
  if (!PopSblrFrame(&stack, &frame_failure)) {
    AddFailure(&failures, "sblr.frame.pop", "Frame pop failed");
  }
  if (PopSblrFrame(&stack, &frame_failure) ||
      frame_failure.status != SblrStatusCode::internal_error ||
      !HasDiagnostic(frame_failure, "SB_DIAG_SBLR_FRAME_UNDERFLOW")) {
    AddFailure(&failures, "sblr.frame.underflow", "Frame underflow was not reported deterministically");
  }

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (failures.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"checks\": 18,\n";
  std::cout << "  \"failures\": [";
  for (std::size_t i = 0; i < failures.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << "{\"check\":\"" << failures[i].check << "\",\"detail\":\"" << failures[i].detail << "\"}";
  }
  std::cout << "]\n";
  std::cout << "}\n";
  return failures.empty() ? 0 : 1;
}

}  // namespace

int main() {
  return RunProbe();
}
