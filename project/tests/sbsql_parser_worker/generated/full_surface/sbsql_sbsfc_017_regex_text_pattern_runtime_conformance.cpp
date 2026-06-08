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
  request.context.sblr_context.database_uuid = "SBSFC-017-regex-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-017-regex-runtime-tx";
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

bool ExpectText(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != descriptor || value.text_value != expected) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected << ", got "
              << value.descriptor_id << " " << value.text_value << "\n";
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

  ok = ExpectInt64("regexp_count_basic",
                   Run(registry, "sb.scalar.regexp_count", {TextValue("abcabc"), TextValue("a.")}),
                   2) && ok;
  ok = ExpectInt64("regexp_count_flag",
                   Run(registry, "sb.scalar.regexp_count", {TextValue("Alpha"), TextValue("^a"), TextValue("i")}),
                   1) && ok;
  ok = ExpectNull("regexp_count_null",
                  Run(registry, "sb.scalar.regexp_count", {NullValue("character"), TextValue("a")}),
                  "int64") && ok;
  ok = ExpectFailure("regexp_count_invalid",
                     Run(registry, "sb.scalar.regexp_count", {TextValue("abc"), TextValue("[")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectInt64("occurrences_regex_basic",
                   Run(registry, "sb.scalar.occurrences_regex", {TextValue("a."), TextValue("abcabc")}),
                   2) && ok;
  ok = ExpectInt64("occurrences_regex_flag",
                   Run(registry, "sb.scalar.occurrences_regex", {TextValue("^a"), TextValue("Alpha"), TextValue("i")}),
                   1) && ok;
  ok = ExpectNull("occurrences_regex_null",
                  Run(registry, "sb.scalar.occurrences_regex", {NullValue("character"), TextValue("abc")}),
                  "int64") && ok;
  ok = ExpectFailure("occurrences_regex_invalid",
                     Run(registry, "sb.scalar.occurrences_regex", {TextValue("["), TextValue("abc")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectInt64("position_regex_start",
                   Run(registry, "sb.scalar.position_regex",
                       {TextValue("[0-9]+"), TextValue("abc-123-def")}),
                   5) && ok;
  ok = ExpectInt64("position_regex_after_occurrence",
                   Run(registry, "sb.scalar.position_regex",
                       {TextValue("a"), TextValue("A-a"), Int64Value(2), TextValue("i"), TextValue("after")}),
                   4) && ok;
  ok = ExpectNull("position_regex_null",
                  Run(registry, "sb.scalar.position_regex", {TextValue("a"), NullValue("character")}),
                  "int64") && ok;
  ok = ExpectFailure("position_regex_invalid",
                     Run(registry, "sb.scalar.position_regex", {TextValue("["), TextValue("abc")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("regexp_match_captures",
                  Run(registry, "sb.scalar.regexp_match",
                      {TextValue("abc-123"), TextValue("([a-z]+)-([0-9]+)")}),
                  "array",
                  "[\"abc\",\"123\"]") && ok;
  ok = ExpectText("regexp_match_full",
                  Run(registry, "sb.scalar.regexp_match", {TextValue("Alpha"), TextValue("^a"), TextValue("i")}),
                  "array",
                  "[\"A\"]") && ok;
  ok = ExpectNull("regexp_match_null",
                  Run(registry, "sb.scalar.regexp_match", {TextValue("abc"), NullValue("character")}),
                  "array") && ok;
  ok = ExpectFailure("regexp_match_invalid",
                     Run(registry, "sb.scalar.regexp_match", {TextValue("abc"), TextValue("[")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("regexp_matches_same_array_payload_captures",
                  Run(registry, "sb.scalar.regexp_matches",
                      {TextValue("abc-123"), TextValue("([a-z]+)-([0-9]+)")}),
                  "array",
                  "[\"abc\",\"123\"]") && ok;
  ok = ExpectText("regexp_matches_same_array_payload_full_match",
                  Run(registry, "sb.scalar.regexp_matches",
                      {TextValue("Alpha"), TextValue("^a"), TextValue("i")}),
                  "array",
                  "[\"A\"]") && ok;
  ok = ExpectNull("regexp_matches_null",
                  Run(registry, "sb.scalar.regexp_matches", {NullValue("character"), TextValue("a")}),
                  "array") && ok;
  ok = ExpectFailure("regexp_matches_invalid",
                     Run(registry, "sb.scalar.regexp_matches", {TextValue("abc"), TextValue("[")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("regexp_replace_first",
                  Run(registry, "sb.scalar.regexp_replace",
                      {TextValue("abc123abc"), TextValue("abc"), TextValue("X")}),
                  "character",
                  "X123abc") && ok;
  ok = ExpectText("regexp_replace_global",
                  Run(registry, "sb.scalar.regexp_replace",
                      {TextValue("abc123abc"), TextValue("abc"), TextValue("X"), TextValue("g")}),
                  "character",
                  "X123X") && ok;
  ok = ExpectNull("regexp_replace_null",
                  Run(registry, "sb.scalar.regexp_replace",
                      {TextValue("abc"), TextValue("a"), NullValue("character")}),
                  "character") && ok;
  ok = ExpectFailure("regexp_replace_invalid",
                     Run(registry, "sb.scalar.regexp_replace",
                         {TextValue("abc"), TextValue("["), TextValue("x")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("regexp_split_basic",
                  Run(registry, "sb.scalar.regexp_split_to_array", {TextValue("a,b;c"), TextValue("[,;]")}),
                  "array",
                  "[\"a\",\"b\",\"c\"]") && ok;
  ok = ExpectText("regexp_split_nomatch",
                  Run(registry, "sb.scalar.regexp_split_to_array", {TextValue("abc"), TextValue(";")}),
                  "array",
                  "[\"abc\"]") && ok;
  ok = ExpectNull("regexp_split_null",
                  Run(registry, "sb.scalar.regexp_split_to_array", {NullValue("character"), TextValue("[,;]")}),
                  "array") && ok;
  ok = ExpectFailure("regexp_split_invalid",
                     Run(registry, "sb.scalar.regexp_split_to_array", {TextValue("abc"), TextValue("[")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("regexp_split_to_table_scalar_array_payload",
                  Run(registry, "sb.scalar.regexp_split_to_table", {TextValue("a,b;c"), TextValue("[,;]")}),
                  "array",
                  "[\"a\",\"b\",\"c\"]") && ok;
  ok = ExpectText("regexp_split_to_table_scalar_array_payload_nomatch",
                  Run(registry, "sb.scalar.regexp_split_to_table", {TextValue("abc"), TextValue(";")}),
                  "array",
                  "[\"abc\"]") && ok;
  ok = ExpectNull("regexp_split_to_table_null",
                  Run(registry, "sb.scalar.regexp_split_to_table", {NullValue("character"), TextValue("[,;]")}),
                  "array") && ok;
  ok = ExpectFailure("regexp_split_to_table_invalid",
                     Run(registry, "sb.scalar.regexp_split_to_table", {TextValue("abc"), TextValue("[")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("regexp_substr_basic",
                  Run(registry, "sb.scalar.regexp_substr", {TextValue("abc-123-def"), TextValue("[0-9]+")}),
                  "character",
                  "123") && ok;
  ok = ExpectText("regexp_substr_group",
                  Run(registry, "sb.scalar.regexp_substr",
                      {TextValue("a1 b22 c333"),
                       TextValue("([a-z])([0-9]+)"),
                       Int64Value(1),
                       Int64Value(2),
                       TextValue(""),
                       Int64Value(2)}),
                  "character",
                  "22") && ok;
  ok = ExpectNull("regexp_substr_null",
                  Run(registry, "sb.scalar.regexp_substr", {TextValue("abc"), NullValue("character")}),
                  "character") && ok;
  ok = ExpectFailure("regexp_substr_invalid",
                     Run(registry, "sb.scalar.regexp_substr", {TextValue("abc"), TextValue("[")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("substring_regex_basic",
                  Run(registry, "sb.scalar.substring_regex", {TextValue("[0-9]+"), TextValue("abc-123-def")}),
                  "character",
                  "123") && ok;
  ok = ExpectText("substring_regex_group",
                  Run(registry, "sb.scalar.substring_regex",
                      {TextValue("([a-z])([0-9]+)"),
                       TextValue("a1 b22 c333"),
                       Int64Value(2),
                       Int64Value(2),
                       TextValue("")}),
                  "character",
                  "22") && ok;
  ok = ExpectNull("substring_regex_null",
                  Run(registry, "sb.scalar.substring_regex", {TextValue("a"), NullValue("character")}),
                  "character") && ok;
  ok = ExpectFailure("substring_regex_invalid",
                     Run(registry, "sb.scalar.substring_regex", {TextValue("["), TextValue("abc")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("translate_regex_first",
                  Run(registry, "sb.scalar.translate_regex",
                      {TextValue("abc"), TextValue("abc123abc"), TextValue("X")}),
                  "character",
                  "X123abc") && ok;
  ok = ExpectText("translate_regex_all",
                  Run(registry, "sb.scalar.translate_regex",
                      {TextValue("abc"), TextValue("abc123abc"), TextValue("X"), TextValue("all"), TextValue("")}),
                  "character",
                  "X123X") && ok;
  ok = ExpectNull("translate_regex_null",
                  Run(registry, "sb.scalar.translate_regex",
                      {TextValue("abc"), TextValue("abc123abc"), NullValue("character")}),
                  "character") && ok;
  ok = ExpectFailure("translate_regex_invalid",
                     Run(registry, "sb.scalar.translate_regex",
                         {TextValue("["), TextValue("abc"), TextValue("x")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_017_regex_text_pattern_runtime_conformance=passed\n";
  return 0;
}
