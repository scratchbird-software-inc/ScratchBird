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

scratchbird::engine::sblr::SblrResult Run(const FunctionRegistry& registry,
                                          std::string function_id,
                                          std::vector<SblrValue> values) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-059-expression-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-059-expression-runtime-tx";
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
  if (value.is_null || !value.has_int64_value || value.int64_value != expected ||
      value.descriptor_id != "int64") {
    std::cerr << case_id << ": expected int64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectText("SBSFC059-month-name-signature",
                  Run(registry, "sb.temporal.month_name",
                      {TextValue("date", "2026-05-18"), TextValue("en")}),
                  "character", "May") && ok;
  ok = ExpectText("SBSFC059-date-sub-signature",
                  Run(registry, "sb.temporal.date_sub",
                      {TextValue("date", "2026-05-18"), TextValue("interval", "P2D")}),
                  "date", "2026-05-16") && ok;
  ok = ExpectInt64("SBSFC059-epoch-signature",
                   Run(registry, "sb.temporal.epoch",
                       {TextValue("timestamp", "1970-01-02T00:00:05")}),
                   86405) && ok;
  ok = ExpectInt64("SBSFC059-gen-id-bare",
                   Run(registry, "sb.scalar.gen_id", {TextValue("SBSFC059_gen_bare"), Int64Value(5)}),
                   1) && ok;
  ok = ExpectText("SBSFC059-day-name-bare",
                  Run(registry, "sb.temporal.day_name",
                      {TextValue("date", "2026-05-18"), TextValue("C")}),
                  "character", "Monday") && ok;
  ok = ExpectText("SBSFC059-date-sub-bare",
                  Run(registry, "sb.temporal.date_sub",
                      {TextValue("timestamp", "2026-05-18T14:23:45"),
                       TextValue("interval", "PT3600S")}),
                  "timestamp", "2026-05-18T13:23:45") && ok;
  ok = ExpectInt64("SBSFC059-position-regex-bare",
                   Run(registry, "sb.scalar.position_regex",
                       {TextValue("[0-9]+"), TextValue("abc-123-def-45"), Int64Value(2),
                        TextValue(""), TextValue("AFTER")}),
                   15) && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_059_expression_runtime_function_cleanup_conformance=passed\n";
  return 0;
}
