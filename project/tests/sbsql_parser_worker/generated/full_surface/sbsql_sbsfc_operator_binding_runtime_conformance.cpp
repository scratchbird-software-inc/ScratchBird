// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_operator_runtime.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::engine::sblr::EvaluateSblrStringOperator;
using scratchbird::engine::sblr::EvaluateSblrArithmetic;
using scratchbird::engine::sblr::EvaluateSblrCollectionOperator;
using scratchbird::engine::sblr::EvaluateSblrComparison;
using scratchbird::engine::sblr::EvaluateSblrDocumentOperator;
using scratchbird::engine::sblr::EvaluateSblrUnaryArithmetic;
using scratchbird::engine::sblr::LookupSblrOperator;
using scratchbird::engine::sblr::MakeSblrTruthValue;
using scratchbird::engine::sblr::SblrExecutionContext;
using scratchbird::engine::sblr::SblrResult;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrTruthValue;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;
using scratchbird::engine::sblr::SblrAnd;
using scratchbird::engine::sblr::SblrNot;
using scratchbird::engine::sblr::SblrOr;

SblrExecutionContext Context() {
  SblrExecutionContext context;
  context.database_uuid = "SBSFC-operator-binding-runtime-db";
  context.transaction_uuid = "SBSFC-operator-binding-runtime-tx";
  context.transaction_context_present = true;
  return context;
}

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

SblrValue TextValue(std::string descriptor,
                    std::string text,
                    std::string charset = {},
                    std::string collation = {}) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(text);
  value.text_value = value.encoded_value;
  value.charset_name = std::move(charset);
  value.collation_name = std::move(collation);
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

SblrValue BinaryValue(std::vector<std::uint8_t> bytes) {
  SblrValue value;
  value.descriptor_id = "binary";
  value.payload_kind = SblrValuePayloadKind::binary;
  value.is_null = false;
  value.binary_value = std::move(bytes);
  return value;
}

bool HasDiagnostic(const SblrResult& result, std::string_view id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id) return true;
  }
  return false;
}

bool ExpectRegistryEntry(std::string_view case_id,
                         std::string_view operator_id,
                         std::string_view symbol) {
  const auto* entry = LookupSblrOperator(operator_id);
  if (entry == nullptr || entry->symbol != symbol) {
    std::cerr << case_id << ": missing operator registry entry " << operator_id << "\n";
    return false;
  }
  return true;
}

bool ExpectNoRegistryEntry(std::string_view case_id, std::string_view operator_id) {
  if (LookupSblrOperator(operator_id) != nullptr) {
    std::cerr << case_id << ": unexpected operator registry entry " << operator_id << "\n";
    return false;
  }
  return true;
}

bool ExpectOkScalar(const SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1 || result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected one successful non-mutating scalar result\n";
    return false;
  }
  return true;
}

bool ExpectBool(std::string_view case_id, const SblrResult& result, bool expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  const std::int64_t expected_int = expected ? 1 : 0;
  if (value.is_null || value.payload_kind != SblrValuePayloadKind::boolean ||
      !value.has_int64_value || value.int64_value != expected_int) {
    std::cerr << case_id << ": expected boolean " << (expected ? "TRUE" : "FALSE")
              << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectInt64(std::string_view case_id, const SblrResult& result, std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.payload_kind != SblrValuePayloadKind::signed_integer ||
      !value.has_int64_value || value.int64_value != expected || value.descriptor_id != "int64") {
    std::cerr << case_id << ": expected int64 " << expected << ", got "
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

bool ExpectUnknown(std::string_view case_id, const SblrValue& value) {
  if (!value.is_null || value.descriptor_id != "boolean" || value.has_int64_value) {
    std::cerr << case_id << ": expected UNKNOWN boolean\n";
    return false;
  }
  return true;
}

bool ExpectTruthValue(std::string_view case_id, const SblrValue& value, bool expected) {
  const std::int64_t expected_int = expected ? 1 : 0;
  if (value.is_null || value.payload_kind != SblrValuePayloadKind::boolean ||
      !value.has_int64_value || value.int64_value != expected_int) {
    std::cerr << case_id << ": expected truth value " << (expected ? "TRUE" : "FALSE")
              << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

SblrTruthValue TruthFromResult(const SblrResult& result) {
  if (!result.ok() || result.scalar_values.empty()) return SblrTruthValue::unknown;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value) return SblrTruthValue::unknown;
  return value.int64_value == 0 ? SblrTruthValue::false_value : SblrTruthValue::true_value;
}

bool ExpectFailure(std::string_view case_id,
                   const SblrResult& result,
                   std::string_view diagnostic_id) {
  if (result.status == SblrStatusCode::ok || !HasDiagnostic(result, diagnostic_id) ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto context = Context();
  bool ok = true;

  ok = ExpectRegistryEntry("SBSFCOP-registry-like", "op_like", "LIKE") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-ilike", "op_ilike", "ILIKE") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-not", "op_not", "NOT") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-and", "op_and", "AND") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-or", "op_or", "OR") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-add", "op_add", "+") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-subtract", "op_sub", "-") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-unary-minus", "op_unary_minus", "UNARY_MINUS") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-multiply", "op_mul", "*") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-divide", "op_div", "/") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-modulo", "op_mod", "%") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-equal", "op_eq", "=") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-not-equal", "op_ne", "<>") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-less", "op_lt", "<") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-less-equal", "op_le", "<=") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-greater", "op_gt", ">") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-greater-equal", "op_ge", ">=") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-is-distinct", "op_is_distinct", "IS DISTINCT FROM") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-regex-match", "op_regex_match", "REGEX_MATCH") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-json-get", "op_json_get", "JSON_GET") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-json-get-text", "op_json_get_text", "JSON_GET_TEXT") && ok;
  ok = ExpectRegistryEntry("SBSFCOP-registry-array-contains", "op_array_contains", "ARRAY_CONTAINS") && ok;
  ok = ExpectNoRegistryEntry("SBSFCOP-no-direct-not-like", "op_not_like") && ok;

  ok = ExpectInt64("SBSFCOP-add-int64",
                   EvaluateSblrArithmetic("op_add", Int64Value(2), Int64Value(3), context),
                   5) && ok;
  ok = ExpectInt64("SBSFCOP-subtract-int64",
                   EvaluateSblrArithmetic("op_sub", Int64Value(7), Int64Value(5), context),
                   2) && ok;
  ok = ExpectInt64("SBSFCOP-multiply-int64",
                   EvaluateSblrArithmetic("op_mul", Int64Value(6), Int64Value(7), context),
                   42) && ok;
  ok = ExpectInt64("SBSFCOP-divide-int64",
                   EvaluateSblrArithmetic("op_div", Int64Value(8), Int64Value(2), context),
                   4) && ok;
  ok = ExpectInt64("SBSFCOP-modulo-int64",
                   EvaluateSblrArithmetic("op_mod", Int64Value(10), Int64Value(3), context),
                   1) && ok;
  const auto null_add = EvaluateSblrArithmetic("op_add", NullValue("int64"), Int64Value(3), context);
  if (ExpectOkScalar(null_add, "SBSFCOP-add-null")) {
    ok = (null_add.scalar_values.front().is_null &&
          null_add.scalar_values.front().descriptor_id == "int64") && ok;
  } else {
    ok = false;
  }
  ok = ExpectFailure("SBSFCOP-divide-by-zero",
                     EvaluateSblrArithmetic("op_div", Int64Value(8), Int64Value(0), context),
                     "SBLR.DIVIDE_BY_ZERO") && ok;
  ok = ExpectFailure("SBSFCOP-add-overflow",
                     EvaluateSblrArithmetic("op_add",
                                            Int64Value(INT64_MAX),
                                            Int64Value(1),
                                            context),
                     "SBLR.NUMERIC_OVERFLOW") && ok;
  ok = ExpectInt64("SBSFCOP-unary-minus-int64",
                   EvaluateSblrUnaryArithmetic("op_unary_minus", Int64Value(7), context),
                   -7) && ok;
  const auto null_unary = EvaluateSblrUnaryArithmetic("op_unary_minus", NullValue("int64"), context);
  if (ExpectOkScalar(null_unary, "SBSFCOP-unary-minus-null")) {
    ok = (null_unary.scalar_values.front().is_null &&
          null_unary.scalar_values.front().descriptor_id == "int64") && ok;
  } else {
    ok = false;
  }
  ok = ExpectFailure("SBSFCOP-unary-minus-overflow",
                     EvaluateSblrUnaryArithmetic("op_unary_minus",
                                                 Int64Value(INT64_MIN),
                                                 context),
                     "SBLR.NUMERIC_OVERFLOW") && ok;

  ok = ExpectBool("SBSFCOP-equal-int64",
                  EvaluateSblrComparison("op_eq", Int64Value(5), Int64Value(5), context),
                  true) && ok;
  ok = ExpectBool("SBSFCOP-not-equal-int64",
                  EvaluateSblrComparison("op_ne", Int64Value(5), Int64Value(6), context),
                  true) && ok;
  ok = ExpectBool("SBSFCOP-less-int64",
                  EvaluateSblrComparison("op_lt", Int64Value(4), Int64Value(9), context),
                  true) && ok;
  ok = ExpectBool("SBSFCOP-less-equal-int64",
                  EvaluateSblrComparison("op_le", Int64Value(4), Int64Value(4), context),
                  true) && ok;
  ok = ExpectBool("SBSFCOP-greater-int64",
                  EvaluateSblrComparison("op_gt", Int64Value(8), Int64Value(2), context),
                  true) && ok;
  ok = ExpectBool("SBSFCOP-greater-equal-int64",
                  EvaluateSblrComparison("op_ge", Int64Value(8), Int64Value(8), context),
                  true) && ok;
  const auto null_equal = EvaluateSblrComparison("op_eq", NullValue("int64"), Int64Value(5), context);
  if (ExpectOkScalar(null_equal, "SBSFCOP-equal-null")) {
    ok = ExpectUnknown("SBSFCOP-equal-null", null_equal.scalar_values.front()) && ok;
  } else {
    ok = false;
  }
  ok = ExpectBool("SBSFCOP-is-distinct-null",
                  EvaluateSblrComparison("op_is_distinct", NullValue("int64"), Int64Value(5), context),
                  true) && ok;
  ok = ExpectBool("SBSFCOP-is-distinct-both-null",
                  EvaluateSblrComparison("op_is_distinct", NullValue("int64"), NullValue("int64"), context),
                  false) && ok;
  ok = ExpectTruthValue("SBSFCOP-and-3vl-false",
                        MakeSblrTruthValue(SblrAnd(SblrTruthValue::unknown,
                                                   SblrTruthValue::false_value)),
                        false) && ok;
  ok = ExpectUnknown("SBSFCOP-and-3vl-unknown",
                     MakeSblrTruthValue(SblrAnd(SblrTruthValue::true_value,
                                                SblrTruthValue::unknown))) && ok;
  ok = ExpectTruthValue("SBSFCOP-or-3vl-true",
                        MakeSblrTruthValue(SblrOr(SblrTruthValue::unknown,
                                                  SblrTruthValue::true_value)),
                        true) && ok;
  ok = ExpectUnknown("SBSFCOP-or-3vl-unknown",
                     MakeSblrTruthValue(SblrOr(SblrTruthValue::false_value,
                                               SblrTruthValue::unknown))) && ok;

  const auto descriptor_like = EvaluateSblrStringOperator(
      "op_like",
      TextValue("varchar", "ScratchBird", "utf8", "und_ci"),
      TextValue("char", "scratch%", "utf8", "und_ci"),
      context);
  ok = ExpectBool("SBSFCOP-like-descriptor-collation", descriptor_like, true) && ok;
  ok = ExpectTruthValue("SBSFCOP-not-like-composed-false",
                        MakeSblrTruthValue(SblrNot(TruthFromResult(descriptor_like))),
                        false) && ok;

  const auto unmatched_like = EvaluateSblrStringOperator(
      "op_like", TextValue("text", "sparrow"), TextValue("text", "duck%"), context);
  ok = ExpectBool("SBSFCOP-like-negative", unmatched_like, false) && ok;
  ok = ExpectTruthValue("SBSFCOP-not-like-composed-true",
                        MakeSblrTruthValue(SblrNot(TruthFromResult(unmatched_like))),
                        true) && ok;

  const auto null_like = EvaluateSblrStringOperator(
      "op_like", NullValue("text"), TextValue("text", "bird%"), context);
  if (ExpectOkScalar(null_like, "SBSFCOP-like-null")) {
    ok = ExpectUnknown("SBSFCOP-like-null", null_like.scalar_values.front()) && ok;
    ok = ExpectUnknown("SBSFCOP-not-unknown",
                       MakeSblrTruthValue(SblrNot(TruthFromResult(null_like)))) && ok;
  } else {
    ok = false;
  }

  ok = ExpectFailure("SBSFCOP-like-invalid-escape",
                     EvaluateSblrStringOperator("op_like",
                                                TextValue("text", "abc"),
                                                TextValue("text", "abc\\"),
                                                context),
                     "SBLR.INVALID_PATTERN") && ok;
  ok = ExpectFailure("SBSFCOP-like-non-text-left",
                     EvaluateSblrStringOperator("op_like",
                                                Int64Value(7),
                                                TextValue("text", "7"),
                                                context),
                     "SBLR.DESCRIPTOR_MISMATCH") && ok;
  ok = ExpectFailure("SBSFCOP-like-binary-refusal",
                     EvaluateSblrStringOperator("op_like",
                                                BinaryValue({0x41}),
                                                BinaryValue({0x41}),
                                                context),
                     "SBLR.DESCRIPTOR_MISMATCH") && ok;
  ok = ExpectFailure("SBSFCOP-like-charset-refusal",
                     EvaluateSblrStringOperator("op_like",
                                                TextValue("varchar", "bird", "utf8", "und"),
                                                TextValue("varchar", "b%", "latin1", "und"),
                                                context),
                     "SBLR.CHARSET_COLLATION_MISMATCH") && ok;
  ok = ExpectBool("SBSFCOP-ilike-positive",
                  EvaluateSblrStringOperator("op_ilike",
                                             TextValue("text", "ScratchBird"),
                                             TextValue("text", "scratch%"),
                                             context),
                  true) && ok;
  ok = ExpectBool("SBSFCOP-ilike-negative",
                  EvaluateSblrStringOperator("op_ilike",
                                             TextValue("text", "sparrow"),
                                             TextValue("text", "DUCK%"),
                                             context),
                  false) && ok;
  const auto null_ilike = EvaluateSblrStringOperator(
      "op_ilike", NullValue("text"), TextValue("text", "bird%"), context);
  if (ExpectOkScalar(null_ilike, "SBSFCOP-ilike-null")) {
    ok = ExpectUnknown("SBSFCOP-ilike-null", null_ilike.scalar_values.front()) && ok;
  } else {
    ok = false;
  }
  ok = ExpectFailure("SBSFCOP-ilike-invalid-escape",
                     EvaluateSblrStringOperator("op_ilike",
                                                TextValue("text", "abc"),
                                                TextValue("text", "abc\\"),
                                                context),
                     "SBLR.INVALID_PATTERN") && ok;
  ok = ExpectFailure("SBSFCOP-ilike-non-text-left",
                     EvaluateSblrStringOperator("op_ilike",
                                                Int64Value(7),
                                                TextValue("text", "7"),
                                                context),
                     "SBLR.DESCRIPTOR_MISMATCH") && ok;
  ok = ExpectFailure("SBSFCOP-direct-not-like-refusal",
                     EvaluateSblrStringOperator("op_not_like",
                                                TextValue("text", "bird"),
                                                TextValue("text", "b%"),
                                                context),
                     "SB_DIAG_OPERATOR_INVALID_INPUT") && ok;
  ok = ExpectBool("SBSFCOP-regex-match-positive",
                  EvaluateSblrStringOperator("op_regex_match",
                                             TextValue("text", "Alpha"),
                                             TextValue("text", "^A"),
                                             context),
                  true) && ok;
  ok = ExpectFailure("SBSFCOP-regex-match-invalid-pattern",
                     EvaluateSblrStringOperator("op_regex_match",
                                                TextValue("text", "Alpha"),
                                                TextValue("text", "("),
                                                context),
                     "SBLR.INVALID_PATTERN") && ok;
  ok = ExpectText("SBSFCOP-json-get-document",
                  EvaluateSblrDocumentOperator("op_json_get",
                                               TextValue("json_document", "{\"a\":1}"),
                                               TextValue("text", "$.a"),
                                               context),
                  "json_document",
                  "1") && ok;
  ok = ExpectText("SBSFCOP-json-get-text-string",
                  EvaluateSblrDocumentOperator("op_json_get_text",
                                               TextValue("json_document", "{\"a\":\"bird\"}"),
                                               TextValue("text", "$.a"),
                                               context),
                  "text",
                  "bird") && ok;
  const auto missing_json_text = EvaluateSblrDocumentOperator(
      "op_json_get_text",
      TextValue("json_document", "{\"a\":\"bird\"}"),
      TextValue("text", "$.missing"),
      context);
  if (ExpectOkScalar(missing_json_text, "SBSFCOP-json-get-text-missing")) {
    ok = (missing_json_text.scalar_values.front().is_null &&
          missing_json_text.scalar_values.front().descriptor_id == "text") && ok;
  } else {
    ok = false;
  }
  ok = ExpectBool("SBSFCOP-array-contains-positive",
                  EvaluateSblrCollectionOperator("op_array_contains",
                                                 TextValue("array", "[1,2]"),
                                                 Int64Value(2),
                                                 context),
                  true) && ok;
  ok = ExpectBool("SBSFCOP-array-contains-negative",
                  EvaluateSblrCollectionOperator("op_array_contains",
                                                 TextValue("array", "[1,2]"),
                                                 Int64Value(3),
                                                 context),
                  false) && ok;

  return ok ? 0 : 1;
}
