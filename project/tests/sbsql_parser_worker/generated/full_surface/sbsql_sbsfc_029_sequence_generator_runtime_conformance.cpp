// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::engine::functions::BuildStandardFunctionSeedPackage;
using scratchbird::engine::functions::DispatchFunctionCall;
using scratchbird::engine::functions::FunctionArgument;
using scratchbird::engine::functions::FunctionCallRequest;
using scratchbird::engine::functions::FunctionRegistry;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

SblrValue NullValue(std::string descriptor = {}) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

SblrValue TextValue(std::string input) {
  SblrValue value;
  value.descriptor_id = "character";
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
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

SblrValue BoolValue(bool input) {
  SblrValue value = Int64Value(input ? 1 : 0);
  value.descriptor_id = "boolean";
  value.payload_kind = SblrValuePayloadKind::boolean;
  return value;
}

bool HasDiagnostic(const scratchbird::engine::sblr::SblrResult& result, std::string_view id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id) return true;
  }
  return false;
}

scratchbird::engine::sblr::SblrResult Run(const FunctionRegistry& registry,
                                          std::string function_id,
                                          std::vector<SblrValue> values) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-029-sequence-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-029-sequence-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const scratchbird::engine::sblr::SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected one successful scalar result\n";
    return false;
  }
  return true;
}

bool ExpectInt64(std::string_view case_id,
                 const scratchbird::engine::sblr::SblrResult& result,
                 std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value || value.int64_value != expected || value.descriptor_id != "int64") {
    std::cerr << case_id << ": expected int64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << ", got "
              << value.descriptor_id << "\n";
    return false;
  }
  return true;
}

bool ExpectFailure(std::string_view case_id,
                   const scratchbird::engine::sblr::SblrResult& result,
                   std::string_view diagnostic_id) {
  if (result.status == SblrStatusCode::ok || !HasDiagnostic(result, diagnostic_id)) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectFailure("nextval_missing",
                     Run(registry, "sb.scalar.nextval", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectInt64("nextval_named_first",
                   Run(registry, "sb.scalar.nextval", {TextValue("SBSFC029_seq_next")}),
                   1) && ok;
  ok = ExpectInt64("nextval_named_second",
                   Run(registry, "sb.scalar.nextval", {TextValue("SBSFC029_seq_next")}),
                   2) && ok;
  ok = ExpectFailure("nextval_null_name",
                     Run(registry, "sb.scalar.nextval", {NullValue("character")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectFailure("currval_missing",
                     Run(registry, "sb.scalar.currval", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("currval_undefined",
                     Run(registry, "sb.scalar.currval", {TextValue("SBSFC029_seq_curr_undefined")}),
                     "SB_DIAG_SEQUENCE_CURRENT_UNDEFINED") && ok;
  ok = ExpectInt64("currval_seed_next",
                   Run(registry, "sb.scalar.nextval", {TextValue("SBSFC029_seq_curr")}),
                   1) && ok;
  ok = ExpectInt64("currval_named",
                   Run(registry, "sb.scalar.currval", {TextValue("SBSFC029_seq_curr")}),
                   1) && ok;

  ok = ExpectFailure("gen_id_missing",
                     Run(registry, "sb.scalar.gen_id", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectInt64("gen_id_increment_first",
                   Run(registry, "sb.scalar.gen_id", {TextValue("SBSFC029_gen"), Int64Value(10)}),
                   1) && ok;
  ok = ExpectInt64("gen_id_increment_second",
                   Run(registry, "sb.scalar.gen_id", {TextValue("SBSFC029_gen"), Int64Value(10)}),
                   11) && ok;
  ok = ExpectFailure("gen_id_zero_increment",
                     Run(registry, "sb.scalar.gen_id", {TextValue("SBSFC029_gen_zero"), Int64Value(0)}),
                     "SB_DIAG_SEQUENCE_INCREMENT_ZERO") && ok;

  ok = ExpectNull("lastval_null",
                  Run(registry, "sb.scalar.lastval", {}),
                  "int64") && ok;

  ok = ExpectFailure("setval_missing",
                     Run(registry, "sb.scalar.setval", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectInt64("setval_called_true",
                   Run(registry, "sb.scalar.setval", {TextValue("SBSFC029_seq_set_true"), Int64Value(40)}),
                   40) && ok;
  ok = ExpectInt64("setval_called_true_next",
                   Run(registry, "sb.scalar.nextval", {TextValue("SBSFC029_seq_set_true")}),
                   41) && ok;
  ok = ExpectInt64("setval_called_false",
                   Run(registry, "sb.scalar.setval", {TextValue("SBSFC029_seq_set_false"), Int64Value(41), BoolValue(false)}),
                   41) && ok;
  ok = ExpectInt64("setval_called_false_next",
                   Run(registry, "sb.scalar.nextval", {TextValue("SBSFC029_seq_set_false")}),
                   41) && ok;
  ok = ExpectNull("setval_null_called",
                  Run(registry, "sb.scalar.setval",
                      {TextValue("SBSFC029_seq_set_null"), Int64Value(7), NullValue("boolean")}),
                  "int64") && ok;
  ok = ExpectFailure("setval_null_name",
                     Run(registry, "sb.scalar.setval", {NullValue("character"), Int64Value(7)}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("setval_bad_boolean",
                     Run(registry, "sb.scalar.setval",
                         {TextValue("SBSFC029_seq_set_bad_bool"), Int64Value(7), TextValue("maybe")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_029_sequence_generator_runtime_conformance=passed\n";
  return 0;
}
