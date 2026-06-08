// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <cmath>
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

SblrValue TextValue(std::string descriptor,
                    std::string input,
                    std::string charset = "UTF-8",
                    std::string collation = "unicode_root") {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.text_value = std::move(input);
  value.encoded_value = value.text_value;
  value.charset_name = std::move(charset);
  value.collation_name = std::move(collation);
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

SblrValue Real64Value(double input) {
  SblrValue value;
  value.descriptor_id = "real64";
  value.payload_kind = SblrValuePayloadKind::real64;
  value.is_null = false;
  value.has_real64_value = true;
  value.real64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrResult Run(const FunctionRegistry& registry,
               std::string function_id,
               std::vector<SblrValue> values) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-032-scalar-utility-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-032-scalar-utility-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.local_transaction_id = 32032;
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

bool ExpectInt64(std::string_view case_id, const SblrResult& result, std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "int64" || !value.has_int64_value ||
      value.int64_value != expected) {
    std::cerr << case_id << ": expected int64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectReal64(std::string_view case_id, const SblrResult& result, double expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "real64" || !value.has_real64_value ||
      std::abs(value.real64_value - expected) > 0.000001) {
    std::cerr << case_id << ": expected real64 " << expected << ", got "
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

  ok = ExpectReal64("SBSFC032-atan2d-quadrant",
                    Run(registry, "sb.scalar.atan2d", {Real64Value(1.0), Real64Value(0.0)}),
                    90.0) && ok;
  ok = ExpectNull("SBSFC032-atan2d-null",
                  Run(registry, "sb.scalar.atan2d", {NullValue("real64"), Real64Value(0.0)}),
                  "real64") && ok;

  ok = ExpectInvalidInput("SBSFC032-collation-for-bare-invalid",
                          Run(registry, "sb.scalar.collation_for", {})) && ok;
  ok = ExpectText("SBSFC032-collation-for-text",
                  Run(registry, "sb.scalar.collation_for",
                      {TextValue("character", "abc", "UTF-8", "unicode_ci")}),
                  "character", "unicode_ci") && ok;

  ok = ExpectInvalidInput("SBSFC032-descriptor-of-bare-invalid",
                          Run(registry, "sb.scalar.descriptor_of", {})) && ok;
  ok = ExpectText("SBSFC032-descriptor-of-expr",
                  Run(registry, "sb.scalar.descriptor_of",
                      {TextValue("character", "abc", "UTF-8", "unicode_ci")}),
                  "json_document",
                  "{\"descriptor_id\":\"character\",\"payload_kind\":\"text\",\"is_null\":false,"
                  "\"charset_name\":\"UTF-8\",\"collation_name\":\"unicode_ci\"}") && ok;

  ok = ExpectText("SBSFC032-pg-typeof-bare-unknown",
                  Run(registry, "sb.scalar.pg_typeof", {}),
                  "character", "unknown") && ok;
  ok = ExpectText("SBSFC032-pg-typeof-expr",
                  Run(registry, "sb.scalar.pg_typeof", {NullValue("character")}),
                  "character", "character") && ok;

  ok = ExpectInvalidInput("SBSFC032-safe-cast-bare-invalid",
                          Run(registry, "sb.scalar.safe_cast", {})) && ok;
  ok = ExpectInt64("SBSFC032-safe-cast-expr-as-type",
                   Run(registry, "sb.scalar.safe_cast",
                       {TextValue("character", "123"), TextValue("character", "int64")}),
                   123) && ok;
  ok = ExpectNull("SBSFC032-safe-cast-failure-null",
                  Run(registry, "sb.scalar.safe_cast",
                      {TextValue("character", "bad"), TextValue("character", "int64")}),
                  "int64") && ok;

  ok = ExpectInvalidInput("SBSFC032-try-cast-bare-invalid",
                          Run(registry, "sb.scalar.try_cast", {})) && ok;
  ok = ExpectNull("SBSFC032-try-cast-expr-as-type-null-on-failure",
                  Run(registry, "sb.scalar.try_cast",
                      {TextValue("character", "bad"), TextValue("character", "int64")}),
                  "int64") && ok;

  ok = ExpectInvalidInput("SBSFC032-similar-to-escape-bare-invalid",
                          Run(registry, "sb.scalar.similar_to_escape", {})) && ok;
  ok = ExpectText("SBSFC032-similar-to-escape-text",
                  Run(registry, "sb.scalar.similar_to_escape",
                      {TextValue("character", "a%b_c\\d")}),
                  "character", "a\\%b\\_c\\\\d") && ok;

  ok = ExpectInvalidInput("SBSFC032-value-state-bare-invalid",
                          Run(registry, "sb.scalar.value_state", {})) && ok;
  ok = ExpectText("SBSFC032-value-state-any",
                  Run(registry, "sb.scalar.value_state", {TextValue("character", "abc")}),
                  "character", "text_value") && ok;
  ok = ExpectText("SBSFC032-value-state-null",
                  Run(registry, "sb.scalar.value_state", {NullValue("character")}),
                  "character", "sql_null") && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "sbsql_sbsfc_032_scalar_utility_conversion_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
