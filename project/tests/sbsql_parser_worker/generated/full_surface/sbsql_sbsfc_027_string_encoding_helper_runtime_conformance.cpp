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

SblrValue TextValue(std::string input, std::string descriptor = "character") {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
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

SblrValue BinaryValue(std::vector<std::uint8_t> input) {
  SblrValue value;
  value.descriptor_id = "binary";
  value.payload_kind = SblrValuePayloadKind::binary;
  value.is_null = false;
  value.binary_value = std::move(input);
  return value;
}

std::string CafeUtf8() {
  std::string out = "caf";
  out.push_back(static_cast<char>(0xc3));
  out.push_back(static_cast<char>(0xa9));
  return out;
}

std::string BadUtf8Text() {
  std::string out;
  out.push_back(static_cast<char>(0xc3));
  out.push_back('(');
  return out;
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
  request.context.sblr_context.database_uuid = "SBSFC-027-string-encoding-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-027-string-encoding-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const scratchbird::engine::sblr::SblrResult& result, std::string_view case_id) {
  if (result.status != SblrStatusCode::ok || !result.diagnostics.empty() ||
      result.scalar_values.size() != 1 || !result.rows.empty()) {
    std::cerr << case_id << ": expected one successful scalar result\n";
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != descriptor || value.text_value != expected ||
      value.encoded_value != expected || value.payload_kind != SblrValuePayloadKind::text) {
    std::cerr << case_id << ": expected " << descriptor << " text value\n";
    return false;
  }
  return true;
}

bool ExpectBinary(std::string_view case_id,
                  const scratchbird::engine::sblr::SblrResult& result,
                  const std::vector<std::uint8_t>& expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "binary" ||
      value.payload_kind != SblrValuePayloadKind::binary ||
      value.binary_value != expected) {
    std::cerr << case_id << ": expected binary payload\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor ||
      value.payload_kind != SblrValuePayloadKind::none) {
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << "\n";
    return false;
  }
  return true;
}

bool ExpectFailure(std::string_view case_id,
                   const scratchbird::engine::sblr::SblrResult& result,
                   SblrStatusCode status,
                   std::string_view diagnostic_id) {
  if (result.status != status || result.diagnostics.size() != 1 ||
      !HasDiagnostic(result, diagnostic_id)) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << "\n";
    return false;
  }
  if (!result.scalar_values.empty() || !result.rows.empty()) {
    std::cerr << case_id << ": refusal unexpectedly returned values\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectText("concat_ws_skip_nulls",
                  Run(registry, "sb.scalar.concat_ws",
                      {TextValue("|"), TextValue("red"), NullValue("character"), TextValue("blue")}),
                  "character",
                  "red|blue") && ok;
  ok = ExpectText("concat_ws_separator_only",
                  Run(registry, "sb.scalar.concat_ws", {TextValue("|")}),
                  "character",
                  "") && ok;
  ok = ExpectNull("concat_ws_null_separator",
                  Run(registry, "sb.scalar.concat_ws", {NullValue("character"), TextValue("red")}),
                  "character") && ok;
  ok = ExpectFailure("concat_ws_arity",
                     Run(registry, "sb.scalar.concat_ws", {}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("split_part_middle",
                  Run(registry, "sb.scalar.split_part",
                      {TextValue("alpha|beta|gamma"), TextValue("|"), Int64Value(2)}),
                  "character",
                  "beta") && ok;
  ok = ExpectText("split_part_beyond",
                  Run(registry, "sb.scalar.split_part",
                      {TextValue("alpha|beta"), TextValue("|"), Int64Value(4)}),
                  "character",
                  "") && ok;
  ok = ExpectText("split_part_empty_delimiter",
                  Run(registry, "sb.scalar.split_part",
                      {TextValue("whole"), TextValue(""), Int64Value(1)}),
                  "character",
                  "whole") && ok;
  ok = ExpectNull("split_part_null",
                  Run(registry, "sb.scalar.split_part",
                      {NullValue("character"), TextValue("|"), Int64Value(1)}),
                  "character") && ok;
  ok = ExpectFailure("split_part_non_positive",
                     Run(registry, "sb.scalar.split_part",
                         {TextValue("alpha|beta"), TextValue("|"), Int64Value(0)}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("string_to_array_delimiter",
                  Run(registry, "sb.scalar.string_to_array",
                      {TextValue("a|b"), TextValue("|")}),
                  "array",
                  "[\"a\",\"b\"]") && ok;
  ok = ExpectText("string_to_array_null_string",
                  Run(registry, "sb.scalar.string_to_array",
                      {TextValue("a|NULL|b"), TextValue("|"), TextValue("NULL")}),
                  "array",
                  "[\"a\",null,\"b\"]") && ok;
  ok = ExpectText("string_to_array_null_delimiter_utf8_chars",
                  Run(registry, "sb.scalar.string_to_array",
                      {TextValue(std::string("A") + CafeUtf8()), NullValue("character")}),
                  "array",
                  "[\"A\",\"c\",\"a\",\"f\",\"\xc3\xa9\"]") && ok;
  ok = ExpectText("string_to_array_empty_delimiter",
                  Run(registry, "sb.scalar.string_to_array",
                      {TextValue("abc"), TextValue("")}),
                  "array",
                  "[\"abc\"]") && ok;
  ok = ExpectNull("string_to_array_null_input",
                  Run(registry, "sb.scalar.string_to_array",
                      {NullValue("character"), TextValue("|")}),
                  "array") && ok;
  ok = ExpectFailure("string_to_array_invalid_utf8_char_split",
                     Run(registry, "sb.scalar.string_to_array",
                         {TextValue(BadUtf8Text()), NullValue("character")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectBinary("convert_to_ascii",
                    Run(registry, "sb.scalar.convert_to", {TextValue("Hi"), TextValue("UTF8")}),
                    {0x48, 0x69}) && ok;
  ok = ExpectBinary("convert_to_utf8",
                    Run(registry, "sb.scalar.convert_to", {TextValue(CafeUtf8()), TextValue("utf-8")}),
                    {0x63, 0x61, 0x66, 0xc3, 0xa9}) && ok;
  ok = ExpectNull("convert_to_null",
                  Run(registry, "sb.scalar.convert_to", {NullValue("character"), TextValue("UTF8")}),
                  "binary") && ok;
  ok = ExpectFailure("convert_to_bad_encoding",
                     Run(registry, "sb.scalar.convert_to", {TextValue("Hi"), TextValue("latin1")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("convert_to_invalid_utf8",
                     Run(registry, "sb.scalar.convert_to", {TextValue(BadUtf8Text()), TextValue("UTF8")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("convert_from_ascii",
                  Run(registry, "sb.scalar.convert_from", {BinaryValue({0x48, 0x69}), TextValue("UTF8")}),
                  "character",
                  "Hi") && ok;
  ok = ExpectText("convert_from_utf8",
                  Run(registry, "sb.scalar.convert_from",
                      {BinaryValue({0x63, 0x61, 0x66, 0xc3, 0xa9}), TextValue("utf-8")}),
                  "character",
                  CafeUtf8()) && ok;
  ok = ExpectNull("convert_from_null",
                  Run(registry, "sb.scalar.convert_from", {NullValue("binary"), TextValue("UTF8")}),
                  "character") && ok;
  ok = ExpectFailure("convert_from_non_binary",
                     Run(registry, "sb.scalar.convert_from", {TextValue("Hi"), TextValue("UTF8")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("convert_from_invalid_utf8",
                     Run(registry, "sb.scalar.convert_from", {BinaryValue({0xc3, 0x28}), TextValue("UTF8")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_027_string_encoding_helper_runtime_conformance=passed\n";
  return 0;
}
