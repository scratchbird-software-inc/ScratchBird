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

SblrValue BitStringValue(std::vector<std::uint8_t> bytes) {
  SblrValue value;
  value.descriptor_id = "bit_string";
  value.payload_kind = SblrValuePayloadKind::binary;
  value.binary_value = std::move(bytes);
  value.is_null = false;
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
  request.context.sblr_context.database_uuid = "SBSFC-034-text-trigram-bit-string-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-034-text-trigram-bit-string-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.local_transaction_id = 34034;
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

bool ExpectBinary(std::string_view case_id,
                  const SblrResult& result,
                  std::string_view descriptor,
                  const std::vector<std::uint8_t>& expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != descriptor ||
      value.payload_kind != SblrValuePayloadKind::binary ||
      value.binary_value != expected) {
    std::cerr << case_id << ": expected binary descriptor " << descriptor
              << " with " << expected.size() << " byte(s), got "
              << value.descriptor_id << " with " << value.binary_value.size() << " byte(s)\n";
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

  ok = ExpectInt64("SBSFC034-bit-string-position",
                   Run(registry, "sb.scalar.bit_string_position",
                       {BitStringValue({0xf0}), BitStringValue({0x0f, 0xf0})}),
                   9) && ok;
  ok = ExpectBinary("SBSFC034-bit-string-substring",
                    Run(registry, "sb.scalar.bit_string_substring",
                        {BitStringValue({0x0f, 0xf0}), Int64Value(5), Int64Value(8)}),
                    "bit_string", {0xff}) && ok;

  ok = ExpectInvalidInput("SBSFC034-show-trgm-bare-invalid",
                          Run(registry, "sb.scalar.show_trgm", {})) && ok;
  ok = ExpectText("SBSFC034-show-trgm-text",
                  Run(registry, "sb.scalar.show_trgm", {TextValue("character", "cat")}),
                  "array", R"(["  c"," ca","cat","at "])") && ok;
  ok = ExpectNull("SBSFC034-show-trgm-null",
                  Run(registry, "sb.scalar.show_trgm", {NullValue("character")}),
                  "array") && ok;

  ok = ExpectText(
           "SBSFC034-pg-trgm-capability",
           Run(registry, "sb.scalar.pg_trgm", {}),
           "json_document",
           "{\"capability\":\"pg_trgm\","
           "\"provider\":\"scratchbird-native-scalar\","
           "\"available\":true,"
           "\"mutable\":false,"
           "\"functions\":[\"similarity\",\"word_similarity\",\"show_trgm\"]}") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
