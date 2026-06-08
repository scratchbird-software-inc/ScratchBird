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
#include <cstdlib>
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
using scratchbird::engine::sblr::SblrResult;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

SblrValue NullValue(std::string descriptor = {}) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::none;
  value.is_null = true;
  return value;
}

SblrValue TextValue(std::string descriptor, std::string input) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.text_value = std::move(input);
  value.encoded_value = value.text_value;
  value.charset_name = "UTF-8";
  value.collation_name = "unicode_root";
  value.is_null = false;
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
               std::vector<SblrValue> values = {}) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-035-range-scalar-helper-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-035-range-scalar-helper-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.local_transaction_id = 35035;
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

bool ExpectBool(std::string_view case_id, const SblrResult& result, bool expected) {
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

bool ExpectText(std::string_view case_id,
                const SblrResult& result,
                std::string_view descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != descriptor || value.encoded_value != expected) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected
              << ", got " << value.descriptor_id << " " << value.encoded_value << "\n";
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

bool ExpectInvalidInput(std::string_view case_id, const SblrResult& result) {
  if (result.ok() || result.status != SblrStatusCode::execution_failed ||
      result.diagnostics.empty() ||
      result.diagnostics.front().diagnostic_id != "SB_DIAG_FUNCTION_INVALID_INPUT" ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected SB_DIAG_FUNCTION_INVALID_INPUT refusal\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectBool("SBSFC035-range-contains-true",
                  Run(registry, "sb.scalar.range_contains",
                      {TextValue("character", "[1,5]"), TextValue("character", "[2,5)")}),
                  true) && ok;
  ok = ExpectBool("SBSFC035-range-contains-empty",
                  Run(registry, "sb.scalar.range_contains",
                      {TextValue("character", "[1,5)"), TextValue("character", "empty")}),
                  true) && ok;
  ok = ExpectBool("SBSFC035-range-contains-exclusive-false",
                  Run(registry, "sb.scalar.range_contains",
                      {TextValue("character", "(1,5]"), TextValue("character", "[1,2]")}),
                  false) && ok;
  ok = ExpectBool("SBSFC035-range-contains-element",
                  Run(registry, "sb.scalar.range_contains_element",
                      {TextValue("character", "[1,5)"), Int64Value(3)}),
                  true) && ok;
  ok = ExpectBool("SBSFC035-range-contains-element-exclusive-upper",
                  Run(registry, "sb.scalar.range_contains_element",
                      {TextValue("character", "[1,5)"), Int64Value(5)}),
                  false) && ok;
  ok = ExpectBool("SBSFC035-range-json-contains-element",
                  Run(registry, "sb.scalar.range_contains_element",
                      {TextValue("json_document", R"({"lower":1,"upper":5,"lower_inc":true,"upper_inc":true})"),
                       Int64Value(5)}),
                  true) && ok;
  ok = ExpectText("SBSFC035-range-lower",
                  Run(registry, "sb.scalar.range_lower", {TextValue("character", "[1,5)")}),
                  "character", "1") && ok;
  ok = ExpectNull("SBSFC035-range-lower-empty-null",
                  Run(registry, "sb.scalar.range_lower", {TextValue("character", "empty")}),
                  "character") && ok;
  ok = ExpectBool("SBSFC035-range-lower-inc",
                  Run(registry, "sb.scalar.range_lower_inc", {TextValue("character", "[1,5)")}),
                  true) && ok;
  ok = ExpectBool("SBSFC035-range-overlaps-boundary",
                  Run(registry, "sb.scalar.range_overlaps",
                      {TextValue("character", "[1,5]"), TextValue("character", "[5,8)")}),
                  true) && ok;
  ok = ExpectBool("SBSFC035-range-overlaps-boundary-exclusive",
                  Run(registry, "sb.scalar.range_overlaps",
                      {TextValue("character", "[1,5)"), TextValue("character", "[5,8)")}),
                  false) && ok;
  ok = ExpectBool("SBSFC035-range-strictly-left",
                  Run(registry, "sb.scalar.range_strictly_left",
                      {TextValue("character", "[1,5)"), TextValue("character", "[5,8)")}),
                  true) && ok;
  ok = ExpectBool("SBSFC035-range-strictly-right",
                  Run(registry, "sb.scalar.range_strictly_right",
                      {TextValue("character", "[5,8)"), TextValue("character", "[1,5)")}),
                  true) && ok;
  ok = ExpectText("SBSFC035-range-upper",
                  Run(registry, "sb.scalar.range_upper", {TextValue("character", "[1,5)")}),
                  "character", "5") && ok;
  ok = ExpectNull("SBSFC035-range-upper-unbounded-null",
                  Run(registry, "sb.scalar.range_upper", {TextValue("character", "[1,)")}),
                  "character") && ok;
  ok = ExpectBool("SBSFC035-range-upper-inc",
                  Run(registry, "sb.scalar.range_upper_inc", {TextValue("character", "[1,5)")}),
                  false) && ok;
  ok = ExpectNull("SBSFC035-range-upper-inc-null",
                  Run(registry, "sb.scalar.range_upper_inc", {NullValue("character")}),
                  "boolean") && ok;
  ok = ExpectInvalidInput("SBSFC035-range-invalid-descriptor",
                          Run(registry, "sb.scalar.range_overlaps",
                              {TextValue("character", "[1 5)"), TextValue("character", "[5,8)")})) && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
