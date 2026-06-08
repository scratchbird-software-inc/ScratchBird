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
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

SblrValue TextValue(std::string descriptor, std::string input) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue TextValue(std::string input) {
  return TextValue("character", std::move(input));
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

scratchbird::engine::sblr::SblrResult Run(const FunctionRegistry& registry,
                                          std::string function_id,
                                          std::vector<SblrValue> values) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-058-expression-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-058-expression-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.current_timestamp = "2026-05-19T12:00:00";
  request.context.sblr_context.statement_timestamp = "2026-05-19T12:00:00";
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
  if (value.is_null || value.descriptor_id != descriptor || value.encoded_value != expected) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
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

bool ExpectReal64(std::string_view case_id,
                  const scratchbird::engine::sblr::SblrResult& result,
                  double expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_real64_value || value.descriptor_id != "real64" ||
      std::abs(value.real64_value - expected) > 0.000001) {
    std::cerr << case_id << ": expected real64 " << expected << ", got "
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

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectInt64("SBSFC058-position-regex",
                   Run(registry, "sb.scalar.position_regex", {TextValue("[0-9]+"), TextValue("abc-123-def")}),
                   5) && ok;
  ok = ExpectText("SBSFC058-age-signature",
                  Run(registry, "sb.temporal.age",
                      {TextValue("timestamp", "2026-05-18T14:23:45"),
                       TextValue("timestamp", "2026-05-17T14:23:45")}),
                  "interval", "PT86400S") && ok;
  ok = ExpectInt64("SBSFC058-epoch",
                   Run(registry, "sb.temporal.epoch", {TextValue("timestamp", "1970-01-02T00:00:05")}),
                   86405) && ok;
  ok = ExpectText("SBSFC058-date-add-bare",
                  Run(registry, "sb.temporal.date_add", {TextValue("date", "2026-05-18"), TextValue("interval", "P2D")}),
                  "date", "2026-05-20") && ok;
  ok = ExpectInt64("SBSFC058-currval-bare-setup",
                   Run(registry, "sb.scalar.nextval", {TextValue("SBSFC058_seq_curr_bare")}),
                   1) && ok;
  ok = ExpectInt64("SBSFC058-currval-bare",
                   Run(registry, "sb.scalar.currval", {TextValue("SBSFC058_seq_curr_bare")}),
                   1) && ok;
  ok = ExpectText("SBSFC058-timezone-signature",
                  Run(registry, "sb.temporal.timezone", {TextValue("+02:00"), TextValue("timestamp", "2026-05-18T14:00:00")}),
                  "timestamp_tz", "2026-05-18T12:00:00Z") && ok;
  ok = ExpectText("SBSFC058-age-bare",
                  Run(registry, "sb.temporal.age",
                      {TextValue("timestamp", "2026-05-18T14:23:45"),
                       TextValue("timestamp", "2026-05-17T14:23:45")}),
                  "interval", "PT86400S") && ok;
  ok = ExpectText("SBSFC058-regexp-split-to-table-bare",
                  Run(registry, "sb.scalar.regexp_split_to_table", {TextValue("a,b;c"), TextValue("[,;]")}),
                  "array", "[\"a\",\"b\",\"c\"]") && ok;
  ok = ExpectText("SBSFC058-day-name",
                  Run(registry, "sb.temporal.day_name", {TextValue("date", "2026-05-18")}),
                  "character", "Monday") && ok;
  ok = ExpectInt64("SBSFC058-age-in-years",
                   Run(registry, "sb.temporal.age_in_years",
                       {TextValue("date", "2026-05-18"), TextValue("date", "2024-05-18")}),
                   2) && ok;
  ok = ExpectText("SBSFC058-next-day-bare",
                  Run(registry, "sb.temporal.next_day", {TextValue("date", "2026-05-18"), TextValue("Friday")}),
                  "date", "2026-05-22") && ok;
  ok = ExpectText("SBSFC058-date-bin-bare",
                  Run(registry, "sb.temporal.date_bin",
                      {TextValue("interval", "PT15M"),
                       TextValue("timestamp", "2026-05-18T14:23:45"),
                       TextValue("timestamp", "2026-05-18T14:00:00")}),
                  "timestamp", "2026-05-18T14:15:00") && ok;
  ok = ExpectInt64("SBSFC058-gen-id",
                   Run(registry, "sb.scalar.gen_id", {TextValue("SBSFC058_gen"), Int64Value(10)}),
                   1) && ok;
  ok = ExpectInt64("SBSFC058-currval-signature-setup",
                   Run(registry, "sb.scalar.nextval", {TextValue("SBSFC058_seq_curr_sig")}),
                   1) && ok;
  ok = ExpectInt64("SBSFC058-currval-signature",
                   Run(registry, "sb.scalar.currval", {TextValue("SBSFC058_seq_curr_sig")}),
                   1) && ok;
  ok = ExpectInt64("SBSFC058-nextval-bare",
                   Run(registry, "sb.scalar.nextval", {TextValue("SBSFC058_seq_next_bare")}),
                   1) && ok;
  ok = ExpectText("SBSFC058-date-bin-signature",
                  Run(registry, "sb.temporal.date_bin",
                      {TextValue("interval", "PT15M"),
                       TextValue("timestamp", "2026-05-18T14:23:45"),
                       TextValue("timestamp", "2026-05-18T14:00:00")}),
                  "timestamp", "2026-05-18T14:15:00") && ok;
  ok = ExpectInt64("SBSFC058-occurrences-regex-signature",
                   Run(registry, "sb.scalar.occurrences_regex", {TextValue("a."), TextValue("abcabc")}),
                   2) && ok;
  ok = ExpectText("SBSFC058-timezone-bare",
                  Run(registry, "sb.temporal.timezone", {TextValue("+02:00"), TextValue("timestamp", "2026-05-18T14:00:00")}),
                  "timestamp_tz", "2026-05-18T12:00:00Z") && ok;
  ok = ExpectInt64("SBSFC058-age-in-days",
                   Run(registry, "sb.temporal.age_in_days",
                       {TextValue("date", "2026-05-18"), TextValue("date", "2026-05-15")}),
                   3) && ok;
  ok = ExpectText("SBSFC058-translate-regex-signature",
                  Run(registry, "sb.scalar.translate_regex", {TextValue("a"), TextValue("banana"), TextValue("X"), TextValue("all")}),
                  "character", "bXnXnX") && ok;
  ok = ExpectText("SBSFC058-to-char-signature",
                  Run(registry, "sb.scalar.to_char",
                      {TextValue("timestamp", "2026-05-18T14:23:45"), TextValue("YYYY-MM-DD HH24:MI:SS Day")}),
                  "character", "2026-05-18 14:23:45 Monday") && ok;
  ok = ExpectText("SBSFC058-regexp-matches-bare",
                  Run(registry, "sb.scalar.regexp_matches", {TextValue("abc-123"), TextValue("([a-z]+)-([0-9]+)")}),
                  "array", "[\"abc\",\"123\"]") && ok;
  ok = ExpectText("SBSFC058-from-unixtime-bare",
                  Run(registry, "sb.temporal.from_unixtime", {Int64Value(86405)}),
                  "timestamp_tz", "1970-01-02T00:00:05Z") && ok;
  ok = ExpectNull("SBSFC058-lastval",
                  Run(registry, "sb.scalar.lastval", {}),
                  "int64") && ok;
  ok = ExpectText("SBSFC058-date-add-signature",
                  Run(registry, "sb.temporal.date_add", {TextValue("date", "2026-05-18"), TextValue("interval", "P2D")}),
                  "date", "2026-05-20") && ok;
  ok = ExpectText("SBSFC058-to-date-signature",
                  Run(registry, "sb.scalar.to_date", {TextValue("2026/05/18"), TextValue("YYYY/MM/DD")}),
                  "date", "2026-05-18") && ok;
  ok = ExpectInt64("SBSFC058-setval-signature",
                   Run(registry, "sb.scalar.setval",
                       {TextValue("SBSFC058_seq_set_sig"), Int64Value(41), BoolValue(false)}),
                   41) && ok;
  ok = ExpectInt64("SBSFC058-age-in-months",
                   Run(registry, "sb.temporal.age_in_months",
                       {TextValue("date", "2026-05-18"), TextValue("date", "2026-03-18")}),
                   2) && ok;
  ok = ExpectText("SBSFC058-month-name",
                  Run(registry, "sb.temporal.month_name", {TextValue("date", "2026-05-18")}),
                  "character", "May") && ok;
  ok = ExpectInt64("SBSFC058-date-diff-bare",
                   Run(registry, "sb.temporal.date_diff",
                       {TextValue("day"), TextValue("date", "2026-05-15"), TextValue("date", "2026-05-18")}),
                   3) && ok;
  ok = ExpectInt64("SBSFC058-setval-bare",
                   Run(registry, "sb.scalar.setval", {TextValue("SBSFC058_seq_set_bare"), Int64Value(7)}),
                   7) && ok;
  ok = ExpectReal64("SBSFC058-to-number",
                    Run(registry, "sb.scalar.to_number", {TextValue("1,234.50"), TextValue("9G999D99")}),
                    1234.5) && ok;
  ok = ExpectText("SBSFC058-to-timestamp-bare",
                  Run(registry, "sb.scalar.to_timestamp", {TextValue("2026-05-18 14:23:45"), TextValue("YYYY-MM-DD HH24:MI:SS")}),
                  "timestamp", "2026-05-18T14:23:45") && ok;
  ok = ExpectInt64("SBSFC058-occurrences-regex-bare",
                   Run(registry, "sb.scalar.occurrences_regex", {TextValue("a."), TextValue("abcabc")}),
                   2) && ok;
  ok = ExpectReal64("SBSFC058-months-between-bare",
                    Run(registry, "sb.temporal.months_between",
                        {TextValue("date", "2026-05-18"), TextValue("date", "2026-03-18")}),
                    2.0) && ok;
  ok = ExpectText("SBSFC058-next-day-signature",
                  Run(registry, "sb.temporal.next_day", {TextValue("date", "2026-05-18"), TextValue("Friday")}),
                  "date", "2026-05-22") && ok;
  ok = ExpectReal64("SBSFC058-months-between-signature",
                    Run(registry, "sb.temporal.months_between",
                        {TextValue("date", "2026-05-18"), TextValue("date", "2026-03-18")}),
                    2.0) && ok;
  ok = ExpectText("SBSFC058-substring-regex-bare",
                  Run(registry, "sb.scalar.substring_regex",
                      {TextValue("([a-z])([0-9]+)"), TextValue("a1 b22 c333"), Int64Value(2), Int64Value(2)}),
                  "character", "22") && ok;
  ok = ExpectInt64("SBSFC058-date-diff-signature",
                   Run(registry, "sb.temporal.date_diff",
                       {TextValue("day"), TextValue("date", "2026-05-15"), TextValue("date", "2026-05-18")}),
                   3) && ok;
  ok = ExpectText("SBSFC058-to-date-bare",
                  Run(registry, "sb.scalar.to_date", {TextValue("2026/05/18"), TextValue("YYYY/MM/DD")}),
                  "date", "2026-05-18") && ok;
  ok = ExpectText("SBSFC058-regexp-split-to-table-signature",
                  Run(registry, "sb.scalar.regexp_split_to_table", {TextValue("a,b;c"), TextValue("[,;]")}),
                  "array", "[\"a\",\"b\",\"c\"]") && ok;
  ok = ExpectText("SBSFC058-make-interval-signature",
                  Run(registry, "sb.temporal.make_interval",
                      {Int64Value(1), Int64Value(2), Int64Value(0), Int64Value(3),
                       Int64Value(4), Int64Value(5), Int64Value(6)}),
                  "interval", "P1Y2M3DT4H5M6S") && ok;
  ok = ExpectInt64("SBSFC058-nextval-signature",
                   Run(registry, "sb.scalar.nextval", {TextValue("SBSFC058_seq_next_sig")}),
                   1) && ok;
  ok = ExpectText("SBSFC058-to-timestamp-signature",
                  Run(registry, "sb.scalar.to_timestamp", {TextValue("2026-05-18 14:23:45"), TextValue("YYYY-MM-DD HH24:MI:SS")}),
                  "timestamp", "2026-05-18T14:23:45") && ok;
  ok = ExpectText("SBSFC058-regexp-matches-signature",
                  Run(registry, "sb.scalar.regexp_matches", {TextValue("abc-123"), TextValue("([a-z]+)-([0-9]+)")}),
                  "array", "[\"abc\",\"123\"]") && ok;
  ok = ExpectText("SBSFC058-translate-regex-bare",
                  Run(registry, "sb.scalar.translate_regex", {TextValue("a"), TextValue("banana"), TextValue("X"), TextValue("all")}),
                  "character", "bXnXnX") && ok;
  ok = ExpectText("SBSFC058-to-char-bare",
                  Run(registry, "sb.scalar.to_char",
                      {TextValue("timestamp", "2026-05-18T14:23:45"), TextValue("YYYY-MM-DD HH24:MI:SS Day")}),
                  "character", "2026-05-18 14:23:45 Monday") && ok;
  ok = ExpectText("SBSFC058-make-interval-bare",
                  Run(registry, "sb.temporal.make_interval", {}),
                  "interval", "P0D") && ok;
  ok = ExpectText("SBSFC058-from-unixtime-signature",
                  Run(registry, "sb.temporal.from_unixtime", {Int64Value(86405)}),
                  "timestamp_tz", "1970-01-02T00:00:05Z") && ok;
  ok = ExpectText("SBSFC058-substring-regex-signature",
                  Run(registry, "sb.scalar.substring_regex",
                      {TextValue("([a-z])([0-9]+)"), TextValue("a1 b22 c333"), Int64Value(2), Int64Value(2)}),
                  "character", "22") && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_058_expression_runtime_function_conformance=passed\n";
  return 0;
}
