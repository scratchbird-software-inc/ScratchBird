// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "families/data_scalar_functions.hpp"
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

SblrValue TextValue(std::string input) {
  SblrValue value;
  value.descriptor_id = "character";
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue UuidValue(std::string input) {
  SblrValue value = TextValue(std::move(input));
  value.descriptor_id = "uuid";
  return value;
}

SblrValue BinaryValue(std::vector<std::uint8_t> input, std::string descriptor = "binary") {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::binary;
  value.is_null = false;
  value.binary_value = std::move(input);
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

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
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
                                          std::vector<SblrValue> values,
                                          bool dependency_available = true) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = dependency_available;
  request.context.sblr_context.database_uuid = "SBSFC-011-text-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-011-text-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

scratchbird::engine::sblr::SblrResult RunDataScalarEntrypoint(std::string function_id,
                                                              std::vector<SblrValue> values) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-011-text-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-011-text-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchDataScalarFunction(request).result;
}

bool ExpectOkScalar(const scratchbird::engine::sblr::SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected one successful scalar result\n";
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

bool ExpectUint64(std::string_view case_id,
                  const scratchbird::engine::sblr::SblrResult& result,
                  std::uint64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_uint64_value || value.uint64_value != expected ||
      value.descriptor_id != "uint64") {
    std::cerr << case_id << ": expected uint64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
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
  if (value.is_null || !value.has_real64_value || value.descriptor_id != "real64" ||
      std::fabs(value.real64_value - expected) > epsilon) {
    std::cerr << case_id << ": expected real64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBool(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                bool expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "boolean" || !value.has_int64_value ||
      (value.int64_value != 0) != expected) {
    std::cerr << case_id << ": expected boolean " << (expected ? "true" : "false")
              << ", got " << value.descriptor_id << " " << value.encoded_value << "\n";
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
    std::cerr << case_id << ": expected character " << expected << ", got "
              << value.descriptor_id << " " << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectTextDescriptor(std::string_view case_id,
                          const scratchbird::engine::sblr::SblrResult& result,
                          std::string_view descriptor,
                          std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.text_value != expected || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected << ", got "
              << value.descriptor_id << " " << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBinary(std::string_view case_id,
                  const scratchbird::engine::sblr::SblrResult& result,
                  const std::vector<std::uint8_t>& expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "binary" || value.binary_value != expected) {
    std::cerr << case_id << ": expected binary length " << expected.size() << ", got "
              << value.descriptor_id << " length " << value.binary_value.size() << "\n";
    return false;
  }
  return true;
}

bool ExpectBinaryDescriptor(std::string_view case_id,
                            const scratchbird::engine::sblr::SblrResult& result,
                            std::string_view descriptor,
                            const std::vector<std::uint8_t>& expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != descriptor || value.binary_value != expected) {
    std::cerr << case_id << ": expected " << descriptor << " binary length "
              << expected.size() << ", got " << value.descriptor_id << " length "
              << value.binary_value.size() << "\n";
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
                   SblrStatusCode status,
                   std::string_view diagnostic_id) {
  if (result.status != status || !HasDiagnostic(result, diagnostic_id)) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << "\n";
    return false;
  }
  if (!result.scalar_values.empty()) {
    std::cerr << case_id << ": refusal unexpectedly returned scalar values\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectInt64("SBSQL-B81F04905CD1-sb-scalar-length-alias",
                   Run(registry, "sb.scalar.length", {TextValue("surface")}),
                   7) && ok;
  ok = ExpectInt64("SBSQL-E116CF62E9D0-length-alias",
                   Run(registry, "sb.scalar.length", {TextValue("alias")}),
                   5) && ok;
  ok = ExpectNull("SBSQL-B81F04905CD1-sb-scalar-length-null",
                  Run(registry, "sb.scalar.length", {NullValue("character")}),
                  "int64") && ok;
  ok = ExpectText("SBSQL-781AA5628408-lower-alias",
                  Run(registry, "sb.scalar.lower", {TextValue("MIXED")}),
                  "mixed") && ok;
  ok = ExpectText("SBSQL-E557D9462210-lower-text-alias",
                  Run(registry, "sb.scalar.lower", {TextValue("ALPHA")}),
                  "alpha") && ok;
  ok = ExpectNull("SBSQL-781AA5628408-lower-null",
                  Run(registry, "sb.scalar.lower", {NullValue("character")}),
                  "character") && ok;
  ok = ExpectText("SBSQL-20053F5AC863-upper-text-alias",
                  Run(registry, "sb.scalar.upper", {TextValue("beta")}),
                  "BETA") && ok;
  ok = ExpectText("SBSQL-259E28B6EF67-upper-alias",
                  Run(registry, "sb.scalar.upper", {TextValue("alias")}),
                  "ALIAS") && ok;
  ok = ExpectNull("SBSQL-20053F5AC863-upper-null",
                  Run(registry, "sb.scalar.upper", {NullValue("character")}),
                  "character") && ok;
  ok = ExpectInt64("SBSQL-906EA0F406D1-sb-scalar-octet-length",
                   Run(registry, "sb.scalar.octet_length", {TextValue("abc")}),
                   3) && ok;
  ok = ExpectNull("SBSQL-906EA0F406D1-sb-scalar-octet-length-null",
                  Run(registry, "sb.scalar.octet_length", {NullValue("character")}),
                  "int64") && ok;
  ok = ExpectText("SBSQL-578CF964D127-sb-scalar-replace",
                  Run(registry, "sb.scalar.replace", {TextValue("one two"), TextValue("two"), TextValue("2")}),
                  "one 2") && ok;
  ok = ExpectText("SBSQL-40C2AF720E24-sb-scalar-concat",
                  Run(registry, "sb.scalar.concat", {TextValue("left"), TextValue("right")}),
                  "leftright") && ok;
  ok = ExpectText("SBSQL-E6121A0124D4-concat-args",
                  Run(registry, "sb.scalar.concat", {TextValue("left"), TextValue("-"), TextValue("right")}),
                  "left-right") && ok;
  ok = ExpectText("SBSQL-40C2AF720E24-sb-scalar-concat-null",
                  Run(registry, "sb.scalar.concat", {NullValue("character"), TextValue("right")}),
                  "right") && ok;
  ok = ExpectText("SBSQL-54CEE6DA3EEC-sb-operator-concat",
                  Run(registry, "sb.scalar.concat", {TextValue("left"), TextValue("right")}),
                  "leftright") && ok;
  ok = ExpectText("SBSQL-54CEE6DA3EEC-sb-operator-concat-null",
                  Run(registry, "sb.scalar.concat", {NullValue("character"), TextValue("right")}),
                  "right") && ok;
  ok = ExpectText("SBSQL-EE28E52F3E30-left-alias",
                  Run(registry, "sb.scalar.left", {TextValue("abcdef"), Int64Value(2)}),
                  "ab") && ok;
  ok = ExpectText("SBSQL-824EDB98BFE8-right-alias",
                  Run(registry, "sb.scalar.right", {TextValue("abcdef"), Int64Value(2)}),
                  "ef") && ok;
  ok = ExpectNull("SBSQL-EE28E52F3E30-left-null",
                  Run(registry, "sb.scalar.left", {NullValue("character"), Int64Value(2)}),
                  "character") && ok;
  ok = ExpectText("SBSQL-01F01C7A290B-special-form-coalesce-alias",
                  Run(registry, "sb.scalar.coalesce", {NullValue("character"), TextValue("first")}),
                  "first") && ok;
  ok = ExpectText("SBSQL-352FFF5468A6-sb-scalar-coalesce-alias",
                  Run(registry, "sb.scalar.coalesce", {NullValue("character"), TextValue("first")}),
                  "first") && ok;
  ok = ExpectText("SBSQL-DC524A31C9B5-coalesce-args-alias",
                  Run(registry, "sb.scalar.coalesce", {NullValue("character"), TextValue("first"), TextValue("second")}),
                  "first") && ok;
  ok = ExpectNull("SBSQL-01F01C7A290B-special-form-coalesce-null",
                  Run(registry, "sb.scalar.coalesce", {NullValue("character"), NullValue("character")}),
                  "character") && ok;
  ok = ExpectText("SBSQL-03A27CAE8A45-sb-scalar-nullif-alias",
                  Run(registry, "sb.scalar.nullif", {TextValue("x"), TextValue("y")}),
                  "x") && ok;
  ok = ExpectNull("SBSQL-03A27CAE8A45-sb-scalar-nullif-null",
                  Run(registry, "sb.scalar.nullif", {TextValue("same"), TextValue("same")}),
                  "character") && ok;
  ok = ExpectText("SBSQL-48AD6AFE9BCD-sb-scalar-ifnull-alias",
                  Run(registry, "sb.scalar.ifnull", {NullValue("character"), TextValue("fallback")}),
                  "fallback") && ok;
  ok = ExpectText("SBSQL-83EEED02BD93-ifnull-signature-alias",
                  Run(registry, "sb.scalar.ifnull", {NullValue("character"), TextValue("fallback")}),
                  "fallback") && ok;
  ok = ExpectNull("SBSQL-48AD6AFE9BCD-sb-scalar-ifnull-null",
                  Run(registry, "sb.scalar.ifnull", {NullValue("character"), NullValue("character")}),
                  "character") && ok;
  ok = ExpectInt64("SBSQL-00439AD179E6-position-text-alias",
                   Run(registry, "sb.scalar.position", {TextValue("lo"), TextValue("hello")}),
                   4) && ok;
  ok = ExpectInt64("SBSQL-7C500BA126F0-position-string-alias",
                   Run(registry, "sb.scalar.position", {TextValue("lo"), TextValue("hello")}),
                   4) && ok;
  ok = ExpectInt64("SBSQL-D95E853515DC-position-bare-alias",
                   Run(registry, "sb.scalar.position", {TextValue("lo"), TextValue("hello")}),
                   4) && ok;
  ok = ExpectInt64("SBSQL-1B39A3C25D5B-position-bit-string",
                   Run(registry,
                       "sb.scalar.position",
                       {BinaryValue({0xf0}, "bit_string"), BinaryValue({0x0f, 0xf0}, "bit_string")}),
                   9) && ok;
  ok = ExpectNull("SBSQL-1B39A3C25D5B-position-bit-string-null",
                  Run(registry, "sb.scalar.position", {NullValue("bit_string"), BinaryValue({0x0f}, "bit_string")}),
                  "int64") && ok;
  ok = ExpectNull("SBSQL-00439AD179E6-position-text-null",
                  Run(registry, "sb.scalar.position", {NullValue("character"), TextValue("hello")}),
                  "int64") && ok;
  ok = ExpectText("SBSQL-99635CAA083A-substring-bare-alias",
                  Run(registry, "sb.scalar.substring", {TextValue("hello world"), Int64Value(7), Int64Value(5)}),
                  "world") && ok;
  ok = ExpectText("SBSQL-9ECB29BCFF5A-substring-signature-alias",
                  Run(registry, "sb.scalar.substring", {TextValue("hello world"), Int64Value(7), Int64Value(5)}),
                  "world") && ok;
  ok = ExpectText("SBSQL-E1285814D20C-sb-scalar-substring-alias",
                  Run(registry, "sb.scalar.substring", {TextValue("hello world"), Int64Value(7), Int64Value(5)}),
                  "world") && ok;
  ok = ExpectBinaryDescriptor("SBSQL-29624CFE2736-substring-bit-string",
                              Run(registry,
                                  "sb.scalar.substring",
                                  {BinaryValue({0x0f, 0xf0}, "bit_string"), Int64Value(4), Int64Value(8)}),
                              "bit_string",
                              {0x7f}) && ok;
  ok = ExpectNull("SBSQL-29624CFE2736-substring-bit-string-null",
                  Run(registry, "sb.scalar.substring", {NullValue("bit_string"), Int64Value(4), Int64Value(8)}),
                  "bit_string") && ok;
  ok = ExpectNull("SBSQL-99635CAA083A-substring-null",
                  Run(registry, "sb.scalar.substring", {NullValue("character"), Int64Value(7), Int64Value(5)}),
                  "character") && ok;
  ok = ExpectText("SBSQL-6CD48E00F9BD-trim-special-alias",
                  Run(registry, "sb.scalar.trim", {TextValue("  trim  ")}),
                  "trim") && ok;
  ok = ExpectText("SBSQL-D1914EB51362-sb-scalar-trim-alias",
                  Run(registry, "sb.scalar.trim", {TextValue("  trim  ")}),
                  "trim") && ok;
  ok = ExpectNull("SBSQL-6CD48E00F9BD-trim-null",
                  Run(registry, "sb.scalar.trim", {NullValue("character")}),
                  "character") && ok;
  ok = ExpectText("SBSFC011-btrim-pos/SBSQL-12FB359A9976",
                  Run(registry, "sb.scalar.btrim", {TextValue("  hello  ")}),
                  "hello") && ok;
  ok = ExpectText("SBSFC011-btrim-empty/SBSQL-12FB359A9976",
                  Run(registry, "sb.scalar.btrim", {TextValue("")}),
                  "") && ok;
  ok = ExpectNull("SBSFC011-btrim-null/SBSQL-12FB359A9976",
                  Run(registry, "sb.scalar.btrim", {NullValue("character")}),
                  "character") && ok;
  ok = ExpectText("SBSFC011-btrim-chars/SBSQL-234E59C1DF96",
                  Run(registry,
                      "sb.scalar.btrim",
                      {TextValue("abcabcfooabcabc"), TextValue("abc")}),
                  "foo") && ok;
  ok = ExpectNull("SBSFC011-btrim-chars-null/SBSQL-234E59C1DF96",
                  Run(registry,
                      "sb.scalar.btrim",
                      {TextValue("hello"), NullValue("character")}),
                  "character") && ok;
  ok = ExpectText("SBSFC011-ltrim-pos/SBSQL-521BAEB2E8A4",
                  Run(registry, "sb.scalar.ltrim", {TextValue("  hello")}),
                  "hello") && ok;
  ok = ExpectNull("SBSFC011-ltrim-null/SBSQL-521BAEB2E8A4",
                  Run(registry, "sb.scalar.ltrim", {NullValue("character")}),
                  "character") && ok;
  ok = ExpectText("SBSFC011-ltrim-chars/SBSQL-185F244A9CA1",
                  Run(registry,
                      "sb.scalar.ltrim",
                      {TextValue("xyzhelloxyz"), TextValue("xyz")}),
                  "helloxyz") && ok;
  ok = ExpectNull("SBSFC011-ltrim-chars-null/SBSQL-185F244A9CA1",
                  Run(registry,
                      "sb.scalar.ltrim",
                      {NullValue("character"), TextValue("xyz")}),
                  "character") && ok;
  ok = ExpectText("SBSFC011-rtrim-pos/SBSQL-4B68123DAFB1",
                  Run(registry, "sb.scalar.rtrim", {TextValue("hello  ")}),
                  "hello") && ok;
  ok = ExpectNull("SBSFC011-rtrim-null/SBSQL-4B68123DAFB1",
                  Run(registry, "sb.scalar.rtrim", {NullValue("character")}),
                  "character") && ok;
  ok = ExpectText("SBSFC011-rtrim-chars/SBSQL-74418A7B7349",
                  Run(registry,
                      "sb.scalar.rtrim",
                      {TextValue("xyzhelloxyz"), TextValue("xyz")}),
                  "xyzhello") && ok;
  ok = ExpectNull("SBSFC011-rtrim-chars-null/SBSQL-74418A7B7349",
                  Run(registry,
                      "sb.scalar.rtrim",
                      {NullValue("character"), TextValue("xyz")}),
                  "character") && ok;
  ok = ExpectText("SBSQL-7456C956A880-sb-scalar-overlay-alias",
                  Run(registry, "sb.scalar.overlay", {TextValue("hello world"), TextValue("SBSQL"), Int64Value(7), Int64Value(5)}),
                  "hello SBSQL") && ok;
  ok = ExpectNull("SBSQL-7456C956A880-sb-scalar-overlay-null",
                  Run(registry, "sb.scalar.overlay", {NullValue("character"), TextValue("SBSQL"), Int64Value(7), Int64Value(5)}),
                  "character") && ok;

  ok = ExpectInt64("SBSQL-F77648609C4D-bit_length-text",
                   Run(registry, "sb.scalar.bit_length", {TextValue("hello")}),
                   40) && ok;
  ok = ExpectNull("SBSQL-F77648609C4D-bit_length-null",
                  Run(registry, "sb.scalar.bit_length", {NullValue("character")}),
                  "int64") && ok;
  ok = ExpectInt64("SBSQL-256568E3617A-bit_length-binary",
                   Run(registry, "sb.scalar.bit_length", {BinaryValue({0x00, 0xff, 0x10})}),
                   24) && ok;

  ok = ExpectInt64("SBSQL-71F2FC01DDD0-char_length-text",
                   Run(registry, "sb.scalar.char_length", {TextValue("surface")}),
                   7) && ok;
  ok = ExpectNull("SBSQL-71F2FC01DDD0-char_length-null",
                  Run(registry, "sb.scalar.char_length", {NullValue("character")}),
                  "int64") && ok;

  ok = ExpectText("SBSQL-0112477D19BE-chr-integer",
                  Run(registry, "sb.scalar.chr", {Int64Value(90)}),
                  "Z") && ok;
  ok = ExpectFailure("SBSQL-0112477D19BE-chr-overrange",
                     Run(registry, "sb.scalar.chr", {Int64Value(128)}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectInt64("SBSQL-515E6D2B9239-octet_length-text",
                   Run(registry, "sb.scalar.octet_length", {TextValue("hello")}),
                   5) && ok;
  ok = ExpectNull("SBSQL-515E6D2B9239-octet_length-null",
                  Run(registry, "sb.scalar.octet_length", {NullValue("character")}),
                  "int64") && ok;

  ok = ExpectText("SBSQL-9DBB48B8B778-replace-string-from-to",
                  Run(registry,
                      "sb.scalar.replace",
                      {TextValue("alpha beta beta"), TextValue("beta"), TextValue("B")}),
                  "alpha B B") && ok;
  ok = ExpectNull("SBSQL-9DBB48B8B778-replace-null",
                  Run(registry,
                      "sb.scalar.replace",
                      {TextValue("alpha"), NullValue("character"), TextValue("B")}),
                  "character") && ok;

  ok = ExpectText("SBSQL-DA800099AD68-reverse-text",
                  Run(registry, "sb.scalar.reverse", {TextValue("drawer")}),
                  "reward") && ok;
  ok = ExpectNull("SBSQL-DA800099AD68-reverse-null",
                  Run(registry, "sb.scalar.reverse", {NullValue("character")}),
                  "character") && ok;

  ok = ExpectText("SBSQL-4C44F5CE32D7-initcap-text",
                  Run(registry, "sb.scalar.initcap", {TextValue("hello WORLD-from sb")}),
                  "Hello World-From Sb") && ok;
  ok = ExpectNull("SBSQL-6B0797D68FD6-initcap-null",
                  Run(registry, "sb.scalar.initcap", {NullValue("character")}),
                  "character") && ok;

  ok = ExpectText("SBSQL-98ED9587403E-translate-text",
                  Run(registry, "sb.scalar.translate", {TextValue("banana"), TextValue("an"), TextValue("ox")}),
                  "boxoxo") && ok;
  ok = ExpectText("SBSQL-A32440FF2F9A-translate-delete",
                  Run(registry, "sb.scalar.translate", {TextValue("banana"), TextValue("an"), TextValue("o")}),
                  "booo") && ok;
  ok = ExpectNull("SBSQL-A32440FF2F9A-translate-null",
                  Run(registry, "sb.scalar.translate", {NullValue("character"), TextValue("an"), TextValue("ox")}),
                  "character") && ok;

  ok = ExpectInt64("SBSQL-D19550197C5C-unicode-ascii",
                   Run(registry, "sb.scalar.unicode", {TextValue("A")}),
                   65) && ok;
  ok = ExpectInt64("SBSQL-B3D36C023507-unicode-utf8",
                   Run(registry, "sb.scalar.unicode", {TextValue("\xe2\x82\xac")}),
                   8364) && ok;
  ok = ExpectNull("SBSQL-B3D36C023507-unicode-null",
                  Run(registry, "sb.scalar.unicode", {NullValue("character")}),
                  "int64") && ok;

  ok = ExpectReal64("SBSQL-0639AB5CE559-to-numeric-text",
                    Run(registry, "sb.scalar.to_numeric", {TextValue("42.5")}),
                    42.5) && ok;
  ok = ExpectReal64("SBSQL-252B904F2E7F-to-number",
                    Run(registry, "sb.scalar.to_number", {TextValue("7.25")}),
                    7.25) && ok;
  ok = ExpectInt64("SBSQL-191DB8E6ABA8-to-integer-text",
                   Run(registry, "sb.scalar.to_integer", {TextValue("-42")}),
                   -42) && ok;
  ok = ExpectInt64("SBSQL-8C986B530E74-to-bigint-text",
                   Run(registry, "sb.scalar.to_bigint", {TextValue("9223372036854775807")}),
                   9223372036854775807LL) && ok;
  ok = ExpectReal64("SBSQL-C9CD8B811AAB-to-double-text",
                    Run(registry, "sb.scalar.to_double", {TextValue("2.5")}),
                    2.5) && ok;
  ok = ExpectReal64("SBSQL-956C90741B66-to-real-text",
                    Run(registry, "sb.scalar.to_real", {TextValue("1.25")}),
                    1.25) && ok;
  ok = ExpectBool("SBSQL-2EDA1021123C-to-boolean-text",
                  Run(registry, "sb.scalar.to_boolean", {TextValue("yes")}),
                  true) && ok;
  ok = ExpectBool("SBSQL-6482F3BE89E6-to-boolean-integer",
                  Run(registry, "sb.scalar.to_boolean", {Int64Value(0)}),
                  false) && ok;
  ok = ExpectNull("SBSQL-0639AB5CE559-to-numeric-null",
                  Run(registry, "sb.scalar.to_numeric", {NullValue("character")}),
                  "real64") && ok;
  ok = ExpectFailure("SBSQL-191DB8E6ABA8-to-integer-refusal",
                     Run(registry, "sb.scalar.to_integer", {TextValue("42.5")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectBool("SBSQL-2B7642A86CBE-is-ascii",
                  Run(registry, "sb.scalar.is_ascii", {TextValue("abc123")}),
                  true) && ok;
  ok = ExpectBool("SBSQL-26FBAF525BD9-is-alphanumeric",
                  Run(registry, "sb.scalar.is_alphanumeric", {TextValue("abc123")}),
                  true) && ok;
  ok = ExpectBool("SBSQL-391856B3E122-is-space",
                  Run(registry, "sb.scalar.is_space", {TextValue(" \t\n")}),
                  true) && ok;
  ok = ExpectBool("SBSQL-C712D580096E-is-digit",
                  Run(registry, "sb.scalar.is_digit", {TextValue("12345")}),
                  true) && ok;
  ok = ExpectBool("SBSQL-C712D580096E-is-digit-negative",
                  Run(registry, "sb.scalar.is_digit", {TextValue("12x")}),
                  false) && ok;

  ok = ExpectText("SBSQL-1FD9B625C623-quote-ident-safe",
                  Run(registry, "sb.scalar.quote_ident", {TextValue("safe_name")}),
                  "safe_name") && ok;
  ok = ExpectText("SBSQL-AB62652D4770-quote-ident-space",
                  Run(registry, "sb.scalar.quote_ident", {TextValue("needs quote")}),
                  "\"needs quote\"") && ok;
  ok = ExpectText("SBSQL-54771F85ED42-quote-literal",
                  Run(registry, "sb.scalar.quote_literal", {TextValue("O'Reilly")}),
                  "'O''Reilly'") && ok;
  ok = ExpectNull("SBSQL-9436487D5B25-quote-literal-null",
                  Run(registry, "sb.scalar.quote_literal", {NullValue("character")}),
                  "character") && ok;
  ok = ExpectText("SBSQL-1F2E6C01431D-quote-nullable-null",
                  Run(registry, "sb.scalar.quote_nullable", {NullValue("character")}),
                  "NULL") && ok;
  ok = ExpectText("SBSQL-E3CF13905BAE-quote-nullable-value",
                  Run(registry, "sb.scalar.quote_nullable", {TextValue("bird")}),
                  "'bird'") && ok;

  ok = ExpectInt64("SBSQL-0AC9FBC2D994-bit-set",
                   Run(registry, "sb.scalar.bit_set", {Int64Value(8), Int64Value(1)}),
                   10) && ok;
  ok = ExpectBool("SBSQL-66E1D07CC7F3-bit-test",
                  Run(registry, "sb.scalar.bit_test", {Int64Value(8), Int64Value(3)}),
                  true) && ok;
  ok = ExpectInt64("SBSQL-519C5DE43A06-bit-clear",
                   Run(registry, "sb.scalar.bit_clear", {Int64Value(10), Int64Value(1)}),
                   8) && ok;
  ok = ExpectInt64("SBSQL-3F52BDC34961-bit-toggle",
                   Run(registry, "sb.scalar.bit_toggle", {Int64Value(8), Int64Value(1)}),
                   10) && ok;
  ok = ExpectFailure("SBSQL-0AC9FBC2D994-bit-position-refusal",
                     Run(registry, "sb.scalar.bit_set", {Int64Value(1), Int64Value(64)}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectUint64("SBSQL-2E02B10A7469-crc32",
                    Run(registry, "sb.scalar.crc32", {TextValue("123456789")}),
                    3421780262ULL) && ok;
  ok = ExpectUint64("SBSQL-94EA9F3D10BC-crc32c",
                    Run(registry, "sb.scalar.crc32c", {TextValue("123456789")}),
                    3808858755ULL) && ok;
  ok = ExpectBinary("SBSQL-FB769B72C9A2-to-bytes-utf8",
                    Run(registry, "sb.scalar.to_bytes", {TextValue("AZ"), TextValue("utf8")}),
                    {0x41, 0x5a}) && ok;
  ok = ExpectText("SBSQL-E17C7BED97C0-from-bytes-utf8",
                  Run(registry, "sb.scalar.from_bytes", {BinaryValue({0x41, 0x5a}), TextValue("utf-8")}),
                  "AZ") && ok;
  ok = ExpectFailure("SBSQL-FB769B72C9A2-to-bytes-unsupported-encoding",
                     Run(registry, "sb.scalar.to_bytes", {TextValue("AZ"), TextValue("latin1")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  for (const auto* algorithm : {"md5", "sha1", "sha224", "sha256", "sha384", "sha512"}) {
    ok = ExpectFailure(algorithm,
                       Run(registry, "sb.fn.data.scalar.sb_crypto_digest", {TextValue("hello"), TextValue(algorithm)}),
                       SblrStatusCode::dependency_unavailable,
                       "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE") && ok;
  }

  ok = ExpectFailure("SBSQL-FFBA0CA4527A-sha224-refuses",
                     Run(registry, "sb.scalar.sha224", {TextValue("hello")}),
                     SblrStatusCode::dependency_unavailable,
                     "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE") && ok;
  ok = ExpectFailure("SBSQL-76B2598F41C1-sha384-refuses",
                     Run(registry, "sb.scalar.sha384", {TextValue("hello")}),
                     SblrStatusCode::dependency_unavailable,
                     "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE") && ok;

  ok = ExpectInt64("SBSQL-209CB0E77FAC-ascii-text",
                   Run(registry, "sb.scalar.ascii", {TextValue("Zebra")}),
                   90) && ok;
  ok = ExpectNull("SBSQL-209CB0E77FAC-ascii-null",
                  Run(registry, "sb.scalar.ascii", {NullValue("character")}),
                  "int64") && ok;

  ok = ExpectText("SBSQL-5F92FDEEE7E3-left-positive",
                  Run(registry, "sb.scalar.left", {TextValue("abcdef"), Int64Value(2)}),
                  "ab") && ok;
  ok = ExpectText("SBSQL-5F92FDEEE7E3-left-zero",
                  Run(registry, "sb.scalar.left", {TextValue("abcdef"), Int64Value(0)}),
                  "") && ok;
  ok = ExpectText("SBSQL-5F92FDEEE7E3-left-negative",
                  Run(registry, "sb.scalar.left", {TextValue("abcdef"), Int64Value(-2)}),
                  "abcd") && ok;
  ok = ExpectNull("SBSQL-5F92FDEEE7E3-left-null",
                  Run(registry, "sb.scalar.left", {NullValue("character"), Int64Value(2)}),
                  "character") && ok;

  ok = ExpectText("SBSQL-837F23FAC0D1-right-positive",
                  Run(registry, "sb.scalar.right", {TextValue("abcdef"), Int64Value(2)}),
                  "ef") && ok;
  ok = ExpectText("SBSQL-837F23FAC0D1-right-zero",
                  Run(registry, "sb.scalar.right", {TextValue("abcdef"), Int64Value(0)}),
                  "") && ok;
  ok = ExpectText("SBSQL-837F23FAC0D1-right-negative",
                  Run(registry, "sb.scalar.right", {TextValue("abcdef"), Int64Value(-2)}),
                  "cdef") && ok;
  ok = ExpectNull("SBSQL-837F23FAC0D1-right-null",
                  Run(registry, "sb.scalar.right", {TextValue("abcdef"), NullValue("int64")}),
                  "character") && ok;

  ok = ExpectText("SBSQL-558F06614D96-encode-hex",
                  Run(registry, "sb.scalar.encode", {BinaryValue({0x00, 0xff, 0x10}), TextValue("hex")}),
                  "00ff10") && ok;
  ok = ExpectText("SBSQL-558F06614D96-encode-base64",
                  Run(registry, "sb.scalar.encode", {BinaryValue({'h', 'i'}), TextValue("base64")}),
                  "aGk=") && ok;
  ok = ExpectText("SBSQL-558F06614D96-encode-escape",
                  Run(registry, "sb.scalar.encode", {BinaryValue({0x00, '\\', 'A', '\n'}), TextValue("escape")}),
                  "\\000\\\\A\\012") && ok;
  ok = ExpectFailure("SBSQL-558F06614D96-encode-invalid-format",
                     Run(registry, "sb.scalar.encode", {BinaryValue({0x00}), TextValue("rot13")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectNull("SBSQL-558F06614D96-encode-null",
                  Run(registry, "sb.scalar.encode", {NullValue("binary"), TextValue("hex")}),
                  "character") && ok;
  ok = ExpectText("SBSQL-D69F87D8A712-encode-bare-alias-hex",
                  RunDataScalarEntrypoint("encode", {BinaryValue({0x00, 0xff, 0x10}), TextValue("hex")}),
                  "00ff10") && ok;
  ok = ExpectText("SBSQL-D69F87D8A712-encode-bare-alias-base64",
                  RunDataScalarEntrypoint("encode", {BinaryValue({'h', 'i'}), TextValue("base64")}),
                  "aGk=") && ok;
  ok = ExpectText("SBSQL-D69F87D8A712-encode-bare-alias-escape",
                  RunDataScalarEntrypoint("encode", {BinaryValue({0x00, '\\', 'A', '\n'}), TextValue("escape")}),
                  "\\000\\\\A\\012") && ok;
  ok = ExpectFailure("SBSQL-D69F87D8A712-encode-bare-alias-invalid-format",
                     RunDataScalarEntrypoint("encode", {BinaryValue({0x00}), TextValue("rot13")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectNull("SBSQL-D69F87D8A712-encode-bare-alias-null",
                  RunDataScalarEntrypoint("encode", {NullValue("binary"), TextValue("hex")}),
                  "character") && ok;

  ok = ExpectBinary("SBSQL-5AD47A23FC8C-decode-hex",
                    Run(registry, "sb.scalar.decode", {TextValue("00ff10"), TextValue("hex")}),
                    {0x00, 0xff, 0x10}) && ok;
  ok = ExpectBinary("SBSQL-5AD47A23FC8C-decode-base64",
                    Run(registry, "sb.scalar.decode", {TextValue("aGk="), TextValue("base64")}),
                    {'h', 'i'}) && ok;
  ok = ExpectBinary("SBSQL-5AD47A23FC8C-decode-escape",
                    Run(registry, "sb.scalar.decode", {TextValue("\\000\\\\A\\012"), TextValue("escape")}),
                    {0x00, '\\', 'A', '\n'}) && ok;
  ok = ExpectFailure("SBSQL-5AD47A23FC8C-decode-invalid-format",
                     Run(registry, "sb.scalar.decode", {TextValue("00"), TextValue("rot13")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectNull("SBSQL-5AD47A23FC8C-decode-null",
                  Run(registry, "sb.scalar.decode", {NullValue("character"), TextValue("hex")}),
                  "binary") && ok;

  ok = ExpectBinary("SBSQL-9AD044845D29-decode-bare-alias-hex",
                    RunDataScalarEntrypoint("decode", {TextValue("00ff10"), TextValue("hex")}),
                    {0x00, 0xff, 0x10}) && ok;
  ok = ExpectBinary("SBSQL-9AD044845D29-decode-bare-alias-base64",
                    RunDataScalarEntrypoint("decode", {TextValue("aGk="), TextValue("base64")}),
                    {'h', 'i'}) && ok;
  ok = ExpectFailure("SBSQL-9AD044845D29-decode-bare-alias-invalid-format",
                     RunDataScalarEntrypoint("decode", {TextValue("00"), TextValue("rot13")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectNull("SBSQL-9AD044845D29-decode-bare-alias-null",
                  RunDataScalarEntrypoint("decode", {NullValue("character"), TextValue("hex")}),
                  "binary") && ok;

  ok = ExpectText("SBSQL-9436D973AB0D-oracle-decode-match",
                  Run(registry,
                      "sb.scalar.oracle_decode",
                      {Int64Value(2), Int64Value(1), TextValue("one"), Int64Value(2), TextValue("two"), TextValue("other")}),
                  "two") && ok;
  ok = ExpectText("SBSQL-9436D973AB0D-oracle-decode-default",
                  Run(registry,
                      "sb.scalar.oracle_decode",
                      {Int64Value(3), Int64Value(1), TextValue("one"), Int64Value(2), TextValue("two"), TextValue("other")}),
                  "other") && ok;
  ok = ExpectNull("SBSQL-9436D973AB0D-oracle-decode-no-default-null",
                  Run(registry,
                      "sb.scalar.oracle_decode",
                      {Int64Value(3), Int64Value(1), TextValue("one"), Int64Value(2), TextValue("two")}),
                  "character") && ok;
  ok = ExpectText("SBSQL-9436D973AB0D-oracle-decode-null-equality",
                  Run(registry,
                      "sb.scalar.oracle_decode",
                      {NullValue("character"), NullValue("character"), TextValue("null-hit"), TextValue("miss")}),
                  "null-hit") && ok;
  ok = ExpectFailure("SBSQL-9436D973AB0D-oracle-decode-arity",
                     Run(registry, "sb.scalar.oracle_decode", {Int64Value(1), Int64Value(1)}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectTextDescriptor("SBSQL-6250A4C72894-uuid-from-string-valid",
                            Run(registry, "sb.scalar.uuid_from_string", {TextValue("550e8400-e29b-41d4-a716-446655440000")}),
                            "uuid",
                            "550e8400-e29b-41d4-a716-446655440000") && ok;
  ok = ExpectTextDescriptor("SBSQL-81D063680A39-uuid-from-string-uppercase",
                            Run(registry, "sb.scalar.uuid_from_string", {TextValue("550E8400-E29B-41D4-A716-446655440000")}),
                            "uuid",
                            "550e8400-e29b-41d4-a716-446655440000") && ok;
  ok = ExpectFailure("SBSQL-6250A4C72894-uuid-from-string-invalid",
                     Run(registry, "sb.scalar.uuid_from_string", {TextValue("550e8400e29b41d4a716446655440000")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectNull("SBSQL-81D063680A39-uuid-from-string-null",
                  Run(registry, "sb.scalar.uuid_from_string", {NullValue("character")}),
                  "uuid") && ok;

  ok = ExpectText("SBSQL-051F74FD6FF7-uuid-to-string-valid",
                  Run(registry, "sb.scalar.uuid_to_string", {UuidValue("550e8400-e29b-41d4-a716-446655440000")}),
                  "550e8400-e29b-41d4-a716-446655440000") && ok;
  ok = ExpectText("SBSQL-B260A8B5877E-uuid-to-string-uppercase",
                  Run(registry, "sb.scalar.uuid_to_string", {UuidValue("550E8400-E29B-41D4-A716-446655440000")}),
                  "550e8400-e29b-41d4-a716-446655440000") && ok;
  ok = ExpectFailure("SBSQL-051F74FD6FF7-uuid-to-string-invalid",
                     Run(registry, "sb.scalar.uuid_to_string", {UuidValue("not-a-uuid")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectNull("SBSQL-B260A8B5877E-uuid-to-string-null",
                  Run(registry, "sb.scalar.uuid_to_string", {NullValue("uuid")}),
                  "character") && ok;

  ok = ExpectUint64("SBSQL-8681F20399C3-digest-fnv64",
                    Run(registry, "sb.scalar.digest", {TextValue("hello"), TextValue("fnv64")}),
                    25347132070217633ull) && ok;
  ok = ExpectFailure("SBSQL-8681F20399C3-digest-md5-refuses",
                     Run(registry, "sb.scalar.digest", {TextValue("hello"), TextValue("md5")}),
                     SblrStatusCode::dependency_unavailable,
                     "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE") && ok;
  ok = ExpectNull("SBSQL-8681F20399C3-digest-null",
                  Run(registry, "sb.scalar.digest", {NullValue("character"), TextValue("fnv64")}),
                  "character") && ok;
  ok = ExpectUint64("SBSQL-303C4BE9B001-digest-bare-alias-fnv64",
                    RunDataScalarEntrypoint("digest", {TextValue("hello"), TextValue("fnv64")}),
                    25347132070217633ull) && ok;
  ok = ExpectUint64("SBSQL-303C4BE9B001-digest-bare-alias-hash64",
                    RunDataScalarEntrypoint("digest", {TextValue("hello"), TextValue("hash64")}),
                    25347132070217633ull) && ok;
  ok = ExpectUint64("SBSQL-303C4BE9B001-digest-bare-alias-checksum64",
                    RunDataScalarEntrypoint("digest", {TextValue("hello"), TextValue("checksum64")}),
                    25347132070217633ull) && ok;
  ok = ExpectFailure("SBSQL-303C4BE9B001-digest-bare-alias-md5-refuses",
                     RunDataScalarEntrypoint("digest", {TextValue("hello"), TextValue("md5")}),
                     SblrStatusCode::dependency_unavailable,
                     "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE") && ok;
  ok = ExpectNull("SBSQL-303C4BE9B001-digest-bare-alias-null",
                  RunDataScalarEntrypoint("digest", {NullValue("character"), TextValue("fnv64")}),
                  "character") && ok;

  ok = ExpectText("SBSQL-C1D95CE89815-default-charset",
                  Run(registry, "sb.scalar.default_charset", {}),
                  "UTF-8") && ok;
  ok = ExpectText("SBSQL-4A0E859F75C6-default-collation",
                  Run(registry, "sb.scalar.default_collation", {}),
                  "unicode_root") && ok;
  ok = ExpectText("SBSQL-75DC9CE7072D-comparison-collation-resolution",
                  Run(registry, "sb.scalar.comparison_collation_resolution", {}),
                  "descriptor_collation_explicit_collate_override") && ok;
  ok = ExpectText("SBSQL-3688A280B569-keyword-case-rule",
                  Run(registry, "sb.scalar.keyword_case_rule", {}),
                  "case_insensitive") && ok;
  ok = ExpectText("SBSQL-E2364705F97A-quoted-identifier-case-rule",
                  Run(registry, "sb.scalar.quoted_identifier_case_rule", {}),
                  "spelling_preserving_not_identity") && ok;
  ok = ExpectText("SBSQL-C5C60A1F17D1-unquoted-identifier-case-rule",
                  Run(registry, "sb.scalar.unquoted_identifier_case_rule", {}),
                  "case_preserving_case_insensitive") && ok;
  ok = ExpectText("SBSQL-DED44DC1AF64-unicode-root",
                  Run(registry, "sb.scalar.unicode_root", {}),
                  "unicode_root") && ok;
  ok = ExpectFailure("SBSQL-C1D95CE89815-default-charset-arity",
                     Run(registry, "sb.scalar.default_charset", {TextValue("unexpected")}),
                     SblrStatusCode::execution_failed,
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  return ok ? 0 : 1;
}
