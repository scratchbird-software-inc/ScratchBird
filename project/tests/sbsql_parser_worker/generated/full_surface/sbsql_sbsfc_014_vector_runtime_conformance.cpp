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
  request.context.sblr_context.database_uuid = "SBSFC-014-vector-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-014-vector-runtime-tx";
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
                std::string_view expected,
                std::string_view descriptor = {}) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.text_value != expected || (!descriptor.empty() && value.descriptor_id != descriptor)) {
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

bool ExpectNull(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << "\n";
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

  ok = ExpectText("SBSQL-CD9843963529 VECTOR",
                  Run(registry, "sb.vector.vector", {Int64Value(1), Real64Value(2.5), Int64Value(-3)}),
                  "[1,2.5,-3]", "dense_vector") && ok;
  ok = ExpectText("SBSQL-DBCE1186E9CE vector(array<numeric>)",
                  Run(registry, "sb.vector.vector", {TextValue("dense_vector", "[1,2.5,-3]")}),
                  "[1,2.5,-3]", "dense_vector") && ok;

  ok = ExpectInt64("SBSQL-4115F7B3F459 vector_dims",
                   Run(registry, "sb.vector.vector_dims", {TextValue("dense_vector", "[1, 2, 3]")}), 3) && ok;
  ok = ExpectInt64("SBSQL-C82CD2448FC0 vector_dims(vector)",
                   Run(registry, "sb.vector.vector_dims", {TextValue("dense_vector", "[1, 2, 3]")}), 3) && ok;

  ok = ExpectReal64("SBSQL-5BA020DF9977 vector_norm",
                    Run(registry, "sb.vector.vector_norm", {TextValue("dense_vector", "[3,4]")}), 5.0) && ok;
  ok = ExpectReal64("SBSQL-FEB5761FAF29 vector_norm(vector)",
                    Run(registry, "sb.vector.vector_norm", {TextValue("dense_vector", "[3,4]")}), 5.0) && ok;

  ok = ExpectText("SBSQL-09ABD3B9C0D4 vector_sum",
                  Run(registry, "sb.vector.vector_sum",
                      {TextValue("dense_vector", "[1,2,3]"), TextValue("dense_vector", "[4,5,6]")}),
                  "[5,7,9]", "dense_vector") && ok;
  ok = ExpectText("SBSQL-6EBD4C7CB915 vector_sum(vector)",
                  Run(registry, "sb.vector.vector_sum",
                      {TextValue("dense_vector", "[1,2,3]"), TextValue("dense_vector", "[4,5,6]")}),
                  "[5,7,9]", "dense_vector") && ok;
  ok = ExpectText("SBSQL-6A0DD02E0445 vector_avg",
                  Run(registry, "sb.vector.vector_avg",
                      {TextValue("dense_vector", "[1,2,3]"), TextValue("dense_vector", "[4,5,6]")}),
                  "[2.5,3.5,4.5]", "dense_vector") && ok;
  ok = ExpectText("SBSQL-BFD7D5BD1995 vector_avg(vector)",
                  Run(registry, "sb.vector.vector_avg",
                      {TextValue("dense_vector", "[1,2,3]"), TextValue("dense_vector", "[4,5,6]")}),
                  "[2.5,3.5,4.5]", "dense_vector") && ok;
  ok = ExpectText("SBSFC014-legacy-vector-aggregate-sum",
                  Run(registry, "sb.fn.vector.vector_aggregate.aggregate_sum",
                      {TextValue("dense_vector", "[1,2]"), TextValue("dense_vector", "[3,4]")}),
                  "[4,6]", "dense_vector") && ok;
  ok = ExpectText("SBSFC014-legacy-vector-aggregate-avg",
                  Run(registry, "sb.fn.vector.vector_aggregate.aggregate_avg",
                      {TextValue("dense_vector", "[1,2]"), TextValue("dense_vector", "[3,4]")}),
                  "[2,3]", "dense_vector") && ok;

  ok = ExpectReal64("SBSQL-9D2789A81562 l2_distance",
                    Run(registry, "sb.vector.l2_distance", {TextValue("dense_vector", "[1,2]"), TextValue("dense_vector", "[4,6]")}),
                    5.0) && ok;
  ok = ExpectReal64("SBSQL-0A79694FE93C l2_distance(vector,vector)",
                    Run(registry, "sb.vector.l2_distance", {TextValue("dense_vector", "[1,2]"), TextValue("dense_vector", "[4,6]")}),
                    5.0) && ok;

  ok = ExpectReal64("SBSQL-B706AC22E3F0 cosine_distance",
                    Run(registry, "sb.vector.cosine_distance", {TextValue("dense_vector", "[1,0]"), TextValue("dense_vector", "[0,1]")}),
                    1.0) && ok;
  ok = ExpectReal64("SBSQL-A9D992B92872 cosine_distance(vector,vector)",
                    Run(registry, "sb.vector.cosine_distance", {TextValue("dense_vector", "[1,0]"), TextValue("dense_vector", "[0,1]")}),
                    1.0) && ok;

  ok = ExpectReal64("SBSQL-B04FAE2CB645 inner_product",
                    Run(registry, "sb.vector.inner_product", {TextValue("dense_vector", "[1,2,3]"), TextValue("dense_vector", "[4,5,6]")}),
                    32.0) && ok;
  ok = ExpectReal64("SBSQL-BD65B1E34D96 inner_product(vector,vector)",
                    Run(registry, "sb.vector.inner_product", {TextValue("dense_vector", "[1,2,3]"), TextValue("dense_vector", "[4,5,6]")}),
                    32.0) && ok;

  ok = ExpectReal64("SBSQL-F57618A8B0BB negative_inner_product",
                    Run(registry, "sb.vector.negative_inner_product", {TextValue("dense_vector", "[1,2,3]"), TextValue("dense_vector", "[4,5,6]")}),
                    -32.0) && ok;
  ok = ExpectReal64("SBSQL-5E97D501D992 negative_inner_product(vector,vector)",
                    Run(registry, "sb.vector.negative_inner_product", {TextValue("dense_vector", "[1,2,3]"), TextValue("dense_vector", "[4,5,6]")}),
                    -32.0) && ok;

  ok = ExpectInt64("SBSQL-0B74FC4EAF28 hamming_distance",
                   Run(registry, "sb.vector.hamming_distance", {TextValue("bit_vector", "B'10101'"), TextValue("bit_vector", "10011")}),
                   2) && ok;
  ok = ExpectInt64("SBSQL-4711C0478840 hamming_distance(bit_vector,bit_vector)",
                   Run(registry, "sb.vector.hamming_distance", {TextValue("bit_vector", "B'10101'"), TextValue("bit_vector", "10011")}),
                   2) && ok;

  ok = ExpectText("SBSQL-5E85E51AD1BB vector_l2_normalize",
                  Run(registry, "sb.vector.vector_l2_normalize", {TextValue("dense_vector", "[3,4]")}),
                  "[0.6,0.8]", "dense_vector") && ok;
  ok = ExpectText("SBSQL-F5CEBB882D6C vector_l2_normalize(vector)",
                  Run(registry, "sb.vector.vector_l2_normalize", {TextValue("dense_vector", "[3,4]")}),
                  "[0.6,0.8]", "dense_vector") && ok;

  ok = ExpectText("SBSQL-29F6F6A31027 subvector",
                  Run(registry, "sb.vector.subvector", {TextValue("dense_vector", "[9,8,7,6]"), Int64Value(2), Int64Value(2)}),
                  "[8,7]", "dense_vector") && ok;
  ok = ExpectText("SBSQL-609AA171BBEC subvector(vector,start,length)",
                  Run(registry, "sb.vector.subvector", {TextValue("dense_vector", "[9,8,7,6]"), Int64Value(2), Int64Value(2)}),
                  "[8,7]", "dense_vector") && ok;

  ok = ExpectText("SBSQL-3BC5399D0F15 vector_cast_int8",
                  Run(registry, "sb.vector.vector_cast_int8", {TextValue("dense_vector", "[-129.2,-1.5,0.49,2.5,300]")}),
                  "[-128,-2,0,3,127]", "int8_vector") && ok;
  ok = ExpectText("SBSQL-0D938B14D724 vector_cast_int8(vector)",
                  Run(registry, "sb.vector.vector_cast_int8", {TextValue("dense_vector", "[-129.2,-1.5,0.49,2.5,300]")}),
                  "[-128,-2,0,3,127]", "int8_vector") && ok;

  ok = ExpectText("SBSQL-94114FAB3F1C vector_cast_float16",
                  Run(registry, "sb.vector.vector_cast_float16", {TextValue("dense_vector", "[1,0.3333,65504]")}),
                  "[1,0.333251953125,65504]", "float16_vector") && ok;
  ok = ExpectText("SBSQL-5EAE338151D1 vector_cast_float16(vector)",
                  Run(registry, "sb.vector.vector_cast_float16", {TextValue("dense_vector", "[1,0.3333,65504]")}),
                  "[1,0.333251953125,65504]", "float16_vector") && ok;

  ok = ExpectNull("SBSFC014-vector-null", Run(registry, "sb.vector.vector", {NullValue("real64")}), "dense_vector") && ok;
  ok = ExpectNull("SBSFC014-vector-dims-null", Run(registry, "sb.vector.vector_dims", {NullValue("dense_vector")}), "int64") && ok;
  ok = ExpectNull("SBSFC014-vector-sum-null",
                  Run(registry, "sb.vector.vector_sum", {NullValue("dense_vector"), TextValue("dense_vector", "[1]")}),
                  "dense_vector") && ok;
  ok = ExpectNull("SBSFC014-vector-avg-null",
                  Run(registry, "sb.vector.vector_avg", {NullValue("dense_vector"), TextValue("dense_vector", "[1]")}),
                  "dense_vector") && ok;
  ok = ExpectNull("SBSFC014-l2-distance-null", Run(registry, "sb.vector.l2_distance", {NullValue("dense_vector"), TextValue("dense_vector", "[1]")}), "real64") && ok;
  ok = ExpectNull("SBSFC014-vector-cast-float16-null", Run(registry, "sb.vector.vector_cast_float16", {NullValue("dense_vector")}), "float16_vector") && ok;

  ok = ExpectFailure("SBSFC014-vector-empty-refusal",
                     Run(registry, "sb.vector.vector", {}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("SBSFC014-l2-distance-dimension-refusal",
                     Run(registry, "sb.vector.l2_distance", {TextValue("dense_vector", "[1,2]"), TextValue("dense_vector", "[1]")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("SBSFC014-vector-sum-dimension-refusal",
                     Run(registry, "sb.vector.vector_sum", {TextValue("dense_vector", "[1,2]"), TextValue("dense_vector", "[1]")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("SBSFC014-vector-avg-dimension-refusal",
                     Run(registry, "sb.vector.vector_avg", {TextValue("dense_vector", "[1,2]"), TextValue("dense_vector", "[1]")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("SBSFC014-cosine-distance-zero-refusal",
                     Run(registry, "sb.vector.cosine_distance", {TextValue("dense_vector", "[0,0]"), TextValue("dense_vector", "[1,0]")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("SBSFC014-vector-l2-normalize-zero-refusal",
                     Run(registry, "sb.vector.vector_l2_normalize", {TextValue("dense_vector", "[0,0]")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("SBSFC014-hamming-distance-dimension-refusal",
                     Run(registry, "sb.vector.hamming_distance", {TextValue("bit_vector", "101"), TextValue("bit_vector", "10")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("SBSFC014-subvector-range-refusal",
                     Run(registry, "sb.vector.subvector", {TextValue("dense_vector", "[1,2]"), Int64Value(2), Int64Value(2)}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectFailure("SBSFC014-vector-cast-float16-overflow-refusal",
                     Run(registry, "sb.vector.vector_cast_float16", {TextValue("dense_vector", "[65505]")}),
                     "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW") && ok;

  return ok ? 0 : 1;
}
