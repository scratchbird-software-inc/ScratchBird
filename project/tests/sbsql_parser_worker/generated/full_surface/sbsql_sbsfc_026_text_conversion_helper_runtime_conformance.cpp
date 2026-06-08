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
  request.context.sblr_context.database_uuid = "SBSFC-026-text-conversion-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-026-text-conversion-runtime-tx";
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
    std::cerr << case_id << ": expected " << descriptor << " " << expected << ", got "
              << value.descriptor_id << " " << value.text_value << "\n";
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
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << ", got "
              << value.descriptor_id << "\n";
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

  ok = ExpectText("convert_using_utf8",
                  Run(registry, "sb.scalar.convert", {TextValue("hello"), TextValue("UTF8")}),
                  "character",
                  "hello") && ok;
  ok = ExpectText("convert_call_utf8",
                  Run(registry, "sb.scalar.convert", {TextValue("surface"), TextValue("utf-8")}),
                  "character",
                  "surface") && ok;
  ok = ExpectNull("convert_null",
                  Run(registry, "sb.scalar.convert", {NullValue("character"), TextValue("utf8")}),
                  "character") && ok;
  ok = ExpectFailure("convert_bad_charset",
                     Run(registry, "sb.scalar.convert", {TextValue("hello"), TextValue("latin1")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("array_to_string_base",
                  Run(registry, "sb.scalar.array_to_string",
                      {TextValue("[\"red\",\"blue\"]"), TextValue("|")}),
                  "character",
                  "red|blue") && ok;
  ok = ExpectText("array_to_string_null_string",
                  Run(registry, "sb.scalar.array_to_string",
                      {TextValue("[\"a\",null,\"b\"]"), TextValue("-"), TextValue("<null>")}),
                  "character",
                  "a-<null>-b") && ok;
  ok = ExpectNull("array_to_string_null",
                  Run(registry, "sb.scalar.array_to_string", {NullValue("array"), TextValue("|")}),
                  "character") && ok;
  ok = ExpectFailure("array_to_string_bad_array",
                     Run(registry, "sb.scalar.array_to_string", {TextValue("not-json"), TextValue("|")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("to_ascii_base",
                  Run(registry, "sb.scalar.to_ascii", {TextValue("plain ASCII")}),
                  "character",
                  "plain ASCII") && ok;
  ok = ExpectText("to_ascii_encoding",
                  Run(registry, "sb.scalar.to_ascii", {TextValue("caf\xc3\xa9"), TextValue("UTF8")}),
                  "character",
                  "cafe") && ok;
  ok = ExpectNull("to_ascii_null",
                  Run(registry, "sb.scalar.to_ascii", {NullValue("character")}),
                  "character") && ok;
  ok = ExpectFailure("to_ascii_bad_encoding",
                     Run(registry, "sb.scalar.to_ascii", {TextValue("caf\xc3\xa9"), TextValue("latin1")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectText("format_base",
                  Run(registry, "sb.scalar.format", {TextValue("Hello %s"), TextValue("world")}),
                  "character",
                  "Hello world") && ok;
  ok = ExpectText("format_template_args",
                  Run(registry, "sb.scalar.format",
                      {TextValue("ident=%I literal=%L %%"), TextValue("Mixed Case"), TextValue("O'Brien")}),
                  "character",
                  "ident=\"Mixed Case\" literal='O''Brien' %") && ok;
  ok = ExpectNull("format_null",
                  Run(registry, "sb.scalar.format", {NullValue("character"), TextValue("ignored")}),
                  "character") && ok;
  ok = ExpectFailure("format_incomplete_directive",
                     Run(registry, "sb.scalar.format", {TextValue("bad %")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_026_text_conversion_helper_runtime_conformance=passed\n";
  return 0;
}
