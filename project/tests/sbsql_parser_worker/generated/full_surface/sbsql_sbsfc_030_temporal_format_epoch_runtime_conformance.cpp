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
  request.context.sblr_context.database_uuid = "SBSFC-030-temporal-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-030-temporal-runtime-tx";
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

  ok = ExpectFailure("day_name_missing",
                     Run(registry, "sb.temporal.day_name", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("day_name_date",
                  Run(registry, "sb.temporal.day_name", {TextValue("date", "2026-05-18")}),
                  "character",
                  "Monday") && ok;
  ok = ExpectNull("day_name_null",
                  Run(registry, "sb.temporal.day_name", {NullValue("date")}),
                  "character") && ok;

  ok = ExpectFailure("month_name_missing",
                     Run(registry, "sb.temporal.month_name", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("month_name_locale",
                  Run(registry, "sb.temporal.month_name", {TextValue("date", "2026-05-18"), TextValue("en_US")}),
                  "character",
                  "May") && ok;

  ok = ExpectFailure("epoch_missing",
                     Run(registry, "sb.temporal.epoch", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectInt64("epoch_timestamp",
                   Run(registry, "sb.temporal.epoch", {TextValue("timestamp", "1970-01-02T00:00:05")}),
                   86405) && ok;

  ok = ExpectFailure("from_unixtime_missing",
                     Run(registry, "sb.temporal.from_unixtime", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("from_unixtime_bigint",
                  Run(registry, "sb.temporal.from_unixtime", {Int64Value(86405)}),
                  "timestamp_tz",
                  "1970-01-02T00:00:05Z") && ok;

  ok = ExpectFailure("months_between_missing",
                     Run(registry, "sb.temporal.months_between", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectReal64("months_between_dates",
                    Run(registry, "sb.temporal.months_between",
                        {TextValue("date", "2026-05-18"), TextValue("date", "2026-03-18")}),
                    2.0) && ok;

  ok = ExpectFailure("next_day_missing",
                     Run(registry, "sb.temporal.next_day", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("next_day_friday",
                  Run(registry, "sb.temporal.next_day", {TextValue("date", "2026-05-18"), TextValue("Friday")}),
                  "date",
                  "2026-05-22") && ok;

  ok = ExpectFailure("to_char_missing",
                     Run(registry, "sb.scalar.to_char", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("to_char_temporal",
                  Run(registry, "sb.scalar.to_char",
                      {TextValue("timestamp", "2026-05-18T14:23:45"), TextValue("YYYY-MM-DD HH24:MI:SS Day")}),
                  "character",
                  "2026-05-18 14:23:45 Monday") && ok;
  ok = ExpectText("to_char_temporal_names",
                  Run(registry, "sb.scalar.to_char",
                      {TextValue("timestamp", "2026-05-18T14:23:45"), TextValue("Dy Mon DD, YYYY")}),
                  "character",
                  "Mon May 18, 2026") && ok;
  ok = ExpectText("to_char_numeric",
                  Run(registry, "sb.scalar.to_char", {Real64Value(1234.5), TextValue("FM9990D00")}),
                  "character",
                  "1234.50") && ok;

  ok = ExpectFailure("to_date_missing",
                     Run(registry, "sb.scalar.to_date", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("to_date_format",
                  Run(registry, "sb.scalar.to_date", {TextValue("18/05/2026"), TextValue("DD/MM/YYYY")}),
                  "date",
                  "2026-05-18") && ok;
  ok = ExpectText("to_date_month_name_format",
                  Run(registry, "sb.scalar.to_date", {TextValue("18-May-2026"), TextValue("DD-Mon-YYYY")}),
                  "date",
                  "2026-05-18") && ok;

  ok = ExpectReal64("to_number_format",
                    Run(registry, "sb.scalar.to_number", {TextValue("1,234.50"), TextValue("9G999D99")}),
                    1234.5) && ok;
  ok = ExpectReal64("to_number_decimal_comma_format",
                    Run(registry, "sb.scalar.to_number", {TextValue("1.234,50"), TextValue("9G999D99")}),
                    1234.5) && ok;

  ok = ExpectFailure("to_timestamp_missing",
                     Run(registry, "sb.scalar.to_timestamp", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("to_timestamp_format",
                  Run(registry, "sb.scalar.to_timestamp",
                      {TextValue("2026-05-18 14:23:45"), TextValue("YYYY-MM-DD HH24:MI:SS")}),
                  "timestamp",
                  "2026-05-18T14:23:45") && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_030_temporal_format_epoch_runtime_conformance=passed\n";
  return 0;
}
