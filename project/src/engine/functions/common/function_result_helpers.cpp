// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_result_helpers.hpp"

#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace scratchbird::engine::functions {

std::string FormatReal64(double value) {
  std::ostringstream encoded;
  encoded << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return encoded.str();
}

scratchbird::engine::sblr::SblrValue MakeNullValue(std::string descriptor_id) {
  scratchbird::engine::sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.payload_kind = scratchbird::engine::sblr::SblrValuePayloadKind::none;
  value.is_null = true;
  return value;
}

scratchbird::engine::sblr::SblrValue MakeTextValue(std::string descriptor_id, std::string value_text) {
  scratchbird::engine::sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.text_value = std::move(value_text);
  value.encoded_value = value.text_value;
  value.payload_kind = value.descriptor_id == "uuid" ? scratchbird::engine::sblr::SblrValuePayloadKind::uuid_text :
                       (value.descriptor_id == "timestamp" ||
                        value.descriptor_id == "timestamp_tz" ||
                        value.descriptor_id == "timestamp_epoch_ms" ||
                        value.descriptor_id == "date" ||
                        value.descriptor_id == "time")
                           ? scratchbird::engine::sblr::SblrValuePayloadKind::temporal_text
                           : scratchbird::engine::sblr::SblrValuePayloadKind::text;
  value.is_null = false;
  return value;
}

scratchbird::engine::sblr::SblrValue MakeInt64Value(std::string descriptor_id, std::int64_t int_value) {
  scratchbird::engine::sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.int64_value = int_value;
  value.has_int64_value = true;
  value.payload_kind = scratchbird::engine::sblr::SblrValuePayloadKind::signed_integer;
  value.encoded_value = std::to_string(int_value);
  value.text_value = std::to_string(int_value);
  value.is_null = false;
  return value;
}

scratchbird::engine::sblr::SblrValue MakeUint64Value(std::string descriptor_id, std::uint64_t uint_value) {
  scratchbird::engine::sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.uint64_value = uint_value;
  value.has_uint64_value = true;
  value.payload_kind = scratchbird::engine::sblr::SblrValuePayloadKind::unsigned_integer;
  value.encoded_value = std::to_string(uint_value);
  value.text_value = value.encoded_value;
  value.is_null = false;
  return value;
}

scratchbird::engine::sblr::SblrValue MakeReal64Value(std::string descriptor_id, double real_value) {
  scratchbird::engine::sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.real64_value = real_value;
  value.has_real64_value = true;
  value.payload_kind = scratchbird::engine::sblr::SblrValuePayloadKind::real64;
  value.encoded_value = FormatReal64(real_value);
  value.text_value = value.encoded_value;
  value.is_null = false;
  return value;
}

scratchbird::engine::sblr::SblrValue MakeEncodedNumericValue(std::string descriptor_id, std::string encoded_text) {
  scratchbird::engine::sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.encoded_value = std::move(encoded_text);
  value.text_value = value.encoded_value;
  value.payload_kind = scratchbird::engine::sblr::SblrValuePayloadKind::high_precision_numeric_text;
  value.is_null = false;
  return value;
}

scratchbird::engine::sblr::SblrValue MakeBinaryValue(std::string descriptor_id, std::vector<std::uint8_t> bytes) {
  scratchbird::engine::sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.binary_value = std::move(bytes);
  value.payload_kind = scratchbird::engine::sblr::SblrValuePayloadKind::binary;
  value.is_null = false;
  return value;
}

bool IsSqlNull(const scratchbird::engine::sblr::SblrValue& value) { return value.is_null; }

std::string ValueAsText(const scratchbird::engine::sblr::SblrValue& value) {
  if (value.is_null) return {};
  if (value.has_int64_value) return std::to_string(value.int64_value);
  if (value.has_uint64_value) return std::to_string(value.uint64_value);
  if (value.has_real64_value) return FormatReal64(value.real64_value);
  if (!value.encoded_value.empty()) return value.encoded_value;
  return value.text_value;
}

FunctionCallResult RefuseFunctionWithDiagnostic(const FunctionCallRequest& request,
                                                scratchbird::engine::sblr::SblrStatusCode status,
                                                std::string diagnostic_id,
                                                std::string detail) {
  auto diagnostic = scratchbird::engine::sblr::MakeSblrRefusalDiagnostic(
      std::move(diagnostic_id), request.context.sblr_context, std::move(detail));
  diagnostic.fields.push_back({"function_id", request.context.function_id});
  diagnostic.fields.push_back({"function_uuid", request.context.function_uuid});
  diagnostic.fields.push_back({"package_name", request.context.package_name});
  FunctionCallResult out;
  out.result = scratchbird::engine::sblr::MakeSblrFailure(
      status, request.context.function_id, std::move(diagnostic));
  return out;
}

FunctionCallResult RefuseFunctionForGate(const FunctionCallRequest& request,
                                         const FunctionGateDecision& decision) {
  return RefuseFunctionWithDiagnostic(request, decision.status, decision.diagnostic_id, decision.detail);
}

FunctionCallResult RefuseFunctionInvalidInput(const FunctionCallRequest& request, std::string detail) {
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
                                      "SB_DIAG_FUNCTION_INVALID_INPUT",
                                      std::move(detail));
}

FunctionCallResult RefuseFunctionOverflow(const FunctionCallRequest& request, std::string detail) {
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
                                      "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW",
                                      std::move(detail));
}

FunctionCallResult RefuseFunctionNumericDomain(const FunctionCallRequest& request, std::string detail) {
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
                                      "SB_DIAG_FUNCTION_NUMERIC_DOMAIN",
                                      std::move(detail));
}

FunctionCallResult RefuseFunctionNumericDivisionByZero(const FunctionCallRequest& request, std::string detail) {
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::execution_failed,
                                      "SB_DIAG_FUNCTION_NUMERIC_DIVISION_BY_ZERO",
                                      std::move(detail));
}

}  // namespace scratchbird::engine::functions
