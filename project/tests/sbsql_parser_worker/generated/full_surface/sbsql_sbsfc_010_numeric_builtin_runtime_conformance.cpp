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
#include <limits>
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
  request.context.sblr_context.database_uuid = "SBSFC-010-numeric-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-010-numeric-runtime-tx";
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

bool ExpectInt64(std::string_view case_id,
                 const scratchbird::engine::sblr::SblrResult& result,
                 std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value || value.descriptor_id != "int64" ||
      value.int64_value != expected) {
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
  const double pi = std::acos(-1.0);
  bool ok = true;

  ok = ExpectReal64("cot_pi_over_4",
                    Run(registry, "sb.scalar.cot", {Real64Value(0.7853981633974483)}),
                    1.0) && ok;
  ok = ExpectNull("cot_null", Run(registry, "sb.scalar.cot", {NullValue("real64")}), "real64") && ok;
  ok = ExpectFailure("cot_invalid_input",
                     Run(registry, "sb.scalar.cot", {TextValue("not_numeric")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectInt64("SBSQL-0086682C61B3-bit_count-positive",
                   Run(registry, "sb.scalar.bit_count", {Int64Value(15)}),
                   4) && ok;
  ok = ExpectInt64("SBSQL-0B1AB80FE276-bit_count-negative-two-complement",
                   Run(registry, "sb.scalar.bit_count", {Int64Value(-1)}),
                   64) && ok;
  ok = ExpectNull("SBSQL-0B1AB80FE276-bit_count-null",
                  Run(registry, "sb.scalar.bit_count", {NullValue("int64")}),
                  "int64") && ok;

  ok = ExpectReal64("SBSQL-489E752A14DC-cbrt-positive",
                    Run(registry, "sb.scalar.cbrt", {Real64Value(27.0)}),
                    3.0) && ok;
  ok = ExpectReal64("SBSQL-B0A110B28BF7-cbrt-negative",
                    Run(registry, "sb.scalar.cbrt", {Real64Value(-8.0)}),
                    -2.0) && ok;
  ok = ExpectNull("SBSQL-B0A110B28BF7-cbrt-null",
                  Run(registry, "sb.scalar.cbrt", {NullValue("real64")}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-F539CB3A4C05-degrees-pi",
                    Run(registry, "sb.scalar.degrees", {Real64Value(pi)}),
                    180.0) && ok;
  ok = ExpectReal64("SBSQL-0EC8CCC1D528-degrees-zero",
                    Run(registry, "sb.scalar.degrees", {Real64Value(0.0)}),
                    0.0) && ok;
  ok = ExpectNull("SBSQL-0EC8CCC1D528-degrees-null",
                  Run(registry, "sb.scalar.degrees", {NullValue("real64")}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-8DC7B150AE0F-radians-180",
                    Run(registry, "sb.scalar.radians", {Real64Value(180.0)}),
                    pi) && ok;
  ok = ExpectReal64("SBSQL-A9DAC3CABD67-radians-zero",
                    Run(registry, "sb.scalar.radians", {Real64Value(0.0)}),
                    0.0) && ok;
  ok = ExpectNull("SBSQL-A9DAC3CABD67-radians-null",
                  Run(registry, "sb.scalar.radians", {NullValue("real64")}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-4F60E36560E7-pi-positive",
                    Run(registry, "sb.scalar.pi", {}),
                    pi) && ok;
  ok = ExpectFailure("SBSQL-4F60E36560E7-pi-arity-refusal",
                     Run(registry, "sb.scalar.pi", {Int64Value(1)}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectInt64("SBSQL-815172E915D1-div-positive",
                   Run(registry, "sb.scalar.div", {Int64Value(7), Int64Value(3)}),
                   2) && ok;
  ok = ExpectInt64("SBSQL-6E72868332B5-div-negative",
                   Run(registry, "sb.scalar.div", {Int64Value(-7), Int64Value(3)}),
                   -2) && ok;
  ok = ExpectNull("SBSQL-6E72868332B5-div-null",
                  Run(registry, "sb.scalar.div", {NullValue("int64"), Int64Value(3)}),
                  "int64") && ok;
  ok = ExpectFailure("SBSQL-6E72868332B5-div-division-by-zero",
                     Run(registry, "sb.scalar.div", {Int64Value(7), Int64Value(0)}),
                     "SB_DIAG_FUNCTION_NUMERIC_DIVISION_BY_ZERO") && ok;
  ok = ExpectFailure("SBSQL-6E72868332B5-div-overflow",
                     Run(registry,
                         "sb.scalar.div",
                         {Int64Value(std::numeric_limits<std::int64_t>::min()), Int64Value(-1)}),
                     "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW") && ok;

  ok = ExpectInt64("SBSQL-B6863A70DD43-factorial-positive",
                   Run(registry, "sb.scalar.factorial", {Int64Value(5)}),
                   120) && ok;
  ok = ExpectInt64("SBSQL-B807EBE231F8-factorial-zero",
                   Run(registry, "sb.scalar.factorial", {Int64Value(0)}),
                   1) && ok;
  ok = ExpectNull("SBSQL-B807EBE231F8-factorial-null",
                  Run(registry, "sb.scalar.factorial", {NullValue("int64")}),
                  "int64") && ok;
  ok = ExpectFailure("SBSQL-B807EBE231F8-factorial-domain",
                     Run(registry, "sb.scalar.factorial", {Int64Value(-1)}),
                     "SB_DIAG_FUNCTION_NUMERIC_DOMAIN") && ok;
  ok = ExpectFailure("SBSQL-B807EBE231F8-factorial-overflow",
                     Run(registry, "sb.scalar.factorial", {Int64Value(21)}),
                     "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW") && ok;

  ok = ExpectInt64("SBSQL-C2B266AC888C-gcd-positive",
                   Run(registry, "sb.scalar.gcd", {Int64Value(48), Int64Value(18)}),
                   6) && ok;
  ok = ExpectInt64("SBSQL-8EF07027C636-gcd-negative",
                   Run(registry, "sb.scalar.gcd", {Int64Value(-48), Int64Value(18)}),
                   6) && ok;
  ok = ExpectNull("SBSQL-8EF07027C636-gcd-null",
                  Run(registry, "sb.scalar.gcd", {NullValue("int64"), Int64Value(18)}),
                  "int64") && ok;
  ok = ExpectFailure("SBSQL-8EF07027C636-gcd-overflow",
                     Run(registry,
                         "sb.scalar.gcd",
                         {Int64Value(std::numeric_limits<std::int64_t>::min()), Int64Value(0)}),
                     "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW") && ok;

  ok = ExpectInt64("SBSQL-747985526DA6-lcm-positive",
                   Run(registry, "sb.scalar.lcm", {Int64Value(21), Int64Value(6)}),
                   42) && ok;
  ok = ExpectInt64("SBSQL-07E21A7F4F84-lcm-zero",
                   Run(registry, "sb.scalar.lcm", {Int64Value(0), Int64Value(6)}),
                   0) && ok;
  ok = ExpectNull("SBSQL-07E21A7F4F84-lcm-null",
                  Run(registry, "sb.scalar.lcm", {NullValue("int64"), Int64Value(6)}),
                  "int64") && ok;
  ok = ExpectFailure("SBSQL-07E21A7F4F84-lcm-overflow",
                     Run(registry,
                         "sb.scalar.lcm",
                         {Int64Value(std::numeric_limits<std::int64_t>::max()), Int64Value(2)}),
                     "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW") && ok;

  ok = ExpectInt64("SBSQL-28C3EDBAFC8A-width_bucket-positive",
                   Run(registry,
                       "sb.scalar.width_bucket",
                       {Real64Value(5.0), Real64Value(0.0), Real64Value(10.0), Int64Value(5)}),
                   3) && ok;
  ok = ExpectInt64("SBSQL-E414655E27A3-width_bucket-underflow",
                   Run(registry,
                       "sb.scalar.width_bucket",
                       {Real64Value(-1.0), Real64Value(0.0), Real64Value(10.0), Int64Value(5)}),
                   0) && ok;
  ok = ExpectNull("SBSQL-E414655E27A3-width_bucket-null",
                  Run(registry,
                      "sb.scalar.width_bucket",
                      {NullValue("real64"), Real64Value(0.0), Real64Value(10.0), Int64Value(5)}),
                  "int64") && ok;
  ok = ExpectFailure("SBSQL-E414655E27A3-width_bucket-count-domain",
                     Run(registry,
                         "sb.scalar.width_bucket",
                         {Real64Value(5.0), Real64Value(0.0), Real64Value(10.0), Int64Value(0)}),
                     "SB_DIAG_FUNCTION_NUMERIC_DOMAIN") && ok;
  ok = ExpectFailure("SBSQL-E414655E27A3-width_bucket-bound-domain",
                     Run(registry,
                         "sb.scalar.width_bucket",
                         {Real64Value(5.0), Real64Value(1.0), Real64Value(1.0), Int64Value(5)}),
                     "SB_DIAG_FUNCTION_NUMERIC_DOMAIN") && ok;

  return ok ? 0 : 1;
}
