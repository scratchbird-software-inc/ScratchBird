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

bool HasDiagnosticDetail(const scratchbird::engine::sblr::SblrResult& result,
                         std::string_view id,
                         std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id && diagnostic.detail == detail) return true;
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
  request.context.sblr_context.database_uuid = "SBSFC-014-fuzzy-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-014-fuzzy-runtime-tx";
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
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.text_value != expected || value.descriptor_id != "character") {
    std::cerr << case_id << ": expected text " << expected << ", got " << value.text_value
              << " descriptor " << value.descriptor_id << "\n";
    return false;
  }
  return true;
}

bool ExpectInt64(std::string_view case_id,
                 const scratchbird::engine::sblr::SblrResult& result,
                 std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value || value.int64_value != expected) {
    std::cerr << case_id << ": expected int64 " << expected << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectReal64(std::string_view case_id,
                  const scratchbird::engine::sblr::SblrResult& result,
                  double expected,
                  double epsilon = 1e-12) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_real64_value || std::fabs(value.real64_value - expected) > epsilon) {
    std::cerr << case_id << ": expected real64 " << expected << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBoolean(std::string_view case_id,
                   const scratchbird::engine::sblr::SblrResult& result,
                   std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value || value.int64_value != expected ||
      value.descriptor_id != "boolean") {
    std::cerr << case_id << ": expected boolean " << expected << ", got "
              << value.encoded_value << " descriptor " << value.descriptor_id << "\n";
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

bool ExpectFailureDetail(std::string_view case_id,
                         const scratchbird::engine::sblr::SblrResult& result,
                         std::string_view diagnostic_id,
                         std::string_view detail) {
  if (result.status == SblrStatusCode::ok || !HasDiagnosticDetail(result, diagnostic_id, detail)) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << " detail " << detail << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectText("soundex_robert", Run(registry, "sb.scalar.soundex", {TextValue("Robert")}), "R163") && ok;
  ok = ExpectText("soundex_hello_world",
                  Run(registry, "sb.scalar.soundex", {TextValue("hello world!")}),
                  "H464") && ok;
  ok = ExpectText("soundex_empty", Run(registry, "sb.scalar.soundex", {TextValue("")}), "") && ok;
  ok = ExpectText("soundex_anne", Run(registry, "sb.scalar.soundex", {TextValue("Anne")}), "A500") && ok;
  ok = ExpectText("soundex_andrew", Run(registry, "sb.scalar.soundex", {TextValue("Andrew")}), "A536") && ok;
  ok = ExpectText("soundex_bare_surface", Run(registry, "sb.scalar.soundex", {TextValue("Robert")}), "R163") && ok;
  ok = ExpectText("metaphone_smith", Run(registry, "sb.scalar.metaphone", {TextValue("Smith")}), "SM0") && ok;
  ok = ExpectText("metaphone_bare_surface", Run(registry, "sb.scalar.metaphone", {TextValue("Smith")}), "SM0") && ok;
  ok = ExpectText("metaphone_bounded", Run(registry, "sb.scalar.metaphone", {TextValue("Smith"), Int64Value(2)}), "SM") && ok;
  ok = ExpectText("metaphone_gumbo_pg",
                  Run(registry, "sb.scalar.metaphone", {TextValue("GUMBO"), Int64Value(4)}),
                  "KM") && ok;
  ok = ExpectText("dmetaphone_gumbo_pg", Run(registry, "sb.scalar.dmetaphone", {TextValue("gumbo")}), "KMP") && ok;
  ok = ExpectText("dmetaphone_bare_surface", Run(registry, "sb.scalar.dmetaphone", {TextValue("gumbo")}), "KMP") && ok;
  ok = ExpectText("dmetaphone_alt_gumbo_pg", Run(registry, "sb.scalar.dmetaphone_alt", {TextValue("gumbo")}), "KMP") && ok;
  ok = ExpectText("dmetaphone_smith", Run(registry, "sb.scalar.dmetaphone", {TextValue("Smith")}), "SM0") && ok;
  ok = ExpectText("dmetaphone_alt_smith", Run(registry, "sb.scalar.dmetaphone_alt", {TextValue("Smith")}), "SMT") && ok;
  ok = ExpectText("dmetaphone_alt_bare_surface", Run(registry, "sb.scalar.dmetaphone_alt", {TextValue("Smith")}), "SMT") && ok;
  ok = ExpectInt64("levenshtein_kitten", Run(registry, "sb.scalar.levenshtein", {TextValue("kitten"), TextValue("sitting")}), 3) && ok;
  ok = ExpectInt64("levenshtein_bare_surface",
                   Run(registry, "sb.scalar.levenshtein", {TextValue("kitten"), TextValue("sitting")}),
                   3) && ok;
  ok = ExpectInt64("levenshtein_costs",
                   Run(registry, "sb.scalar.levenshtein",
                       {TextValue("abc"), TextValue("yabc"), Int64Value(2), Int64Value(3), Int64Value(4)}),
                   2) && ok;
  ok = ExpectInt64("levenshtein_le_exceeded",
                   Run(registry, "sb.scalar.levenshtein_le", {TextValue("kitten"), TextValue("sitting"), Int64Value(2)}),
                   3) && ok;
  ok = ExpectInt64("levenshtein_le_within",
                   Run(registry, "sb.scalar.levenshtein_le", {TextValue("kitten"), TextValue("sitten"), Int64Value(1)}),
                   1) && ok;
  ok = ExpectInt64("levenshtein_le_bare_surface",
                   Run(registry, "sb.scalar.levenshtein_le", {TextValue("kitten"), TextValue("sitten"), Int64Value(1)}),
                   1) && ok;
  ok = ExpectInt64("damerau_transpose",
                   Run(registry, "sb.scalar.damerau_levenshtein", {TextValue("CA"), TextValue("AC")}),
                   1) && ok;
  ok = ExpectInt64("damerau_bare_surface",
                   Run(registry, "sb.scalar.damerau_levenshtein", {TextValue("CA"), TextValue("AC")}),
                   1) && ok;
  ok = ExpectReal64("jaro_martha",
                    Run(registry, "sb.scalar.jaro_similarity", {TextValue("MARTHA"), TextValue("MARHTA")}),
                    0.9444444444444445) && ok;
  ok = ExpectReal64("jaro_bare_surface",
                    Run(registry, "sb.scalar.jaro_similarity", {TextValue("MARTHA"), TextValue("MARHTA")}),
                    0.9444444444444445) && ok;
  ok = ExpectReal64("jaro_winkler_martha",
                    Run(registry, "sb.scalar.jaro_winkler_similarity", {TextValue("MARTHA"), TextValue("MARHTA")}),
                    0.9611111111111111) && ok;
  ok = ExpectReal64("jaro_winkler_bare_surface",
                    Run(registry, "sb.scalar.jaro_winkler_similarity", {TextValue("MARTHA"), TextValue("MARHTA")}),
                    0.9611111111111111) && ok;
  ok = ExpectReal64("similarity_identical",
                    Run(registry, "sb.scalar.similarity", {TextValue("hello"), TextValue("hello")}),
                    1.0) && ok;
  ok = ExpectReal64("similarity_bare_surface",
                    Run(registry, "sb.scalar.similarity", {TextValue("hello"), TextValue("hello")}),
                    1.0) && ok;
  ok = ExpectReal64("word_similarity_extent",
                    Run(registry, "sb.scalar.word_similarity", {TextValue("hello"), TextValue("say hello today")}),
                    1.0) && ok;
  ok = ExpectReal64("word_similarity_bare_surface",
                    Run(registry, "sb.scalar.word_similarity", {TextValue("hello"), TextValue("say hello today")}),
                    1.0) && ok;
  ok = ExpectBoolean("regexp_like_qualified_match",
                     Run(registry, "sb.regex.match", {TextValue("Alpha"), TextValue("^a"), TextValue("i")}),
                     1) && ok;
  ok = ExpectBoolean("regexp_like_qualified_nomatch",
                     Run(registry, "sb.regex.match", {TextValue("Alpha"), TextValue("^z")}),
                     0) && ok;

  ok = ExpectNull("soundex_null", Run(registry, "sb.scalar.soundex", {NullValue("character")}), "character") && ok;
  ok = ExpectNull("levenshtein_null",
                  Run(registry, "sb.scalar.levenshtein", {NullValue("character"), TextValue("x")}),
                  "int64") && ok;
  ok = ExpectNull("jaro_null",
                  Run(registry, "sb.scalar.jaro_similarity", {TextValue("x"), NullValue("character")}),
                  "real64") && ok;
  ok = ExpectNull("regexp_like_qualified_null",
                  Run(registry, "sb.regex.match", {NullValue("character"), TextValue("^a")}),
                  "boolean") && ok;
  ok = ExpectFailure("levenshtein_bad_cost",
                     Run(registry, "sb.scalar.levenshtein",
                         {TextValue("a"), TextValue("b"), Int64Value(0), Int64Value(1), Int64Value(1)}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailureDetail("metaphone_bad_max_negative",
                           Run(registry, "sb.scalar.metaphone", {TextValue("Smith"), Int64Value(-1)}),
                           "SB_DIAG_FUNCTION_INVALID_INPUT",
                           "metaphone max_length must be a positive int64 in [1, 1024]") && ok;
  ok = ExpectFailureDetail("metaphone_bad_max_zero",
                           Run(registry, "sb.scalar.metaphone", {TextValue("Smith"), Int64Value(0)}),
                           "SB_DIAG_FUNCTION_INVALID_INPUT",
                           "metaphone max_length must be a positive int64 in [1, 1024]") && ok;
  ok = ExpectFailureDetail("regexp_like_qualified_invalid_pattern",
                           Run(registry, "sb.regex.match", {TextValue("Alpha"), TextValue("[unclosed")}),
                           "SB_DIAG_FUNCTION_INVALID_INPUT",
                           "regexp_like received an invalid regular expression pattern") && ok;

  return ok ? 0 : 1;
}
