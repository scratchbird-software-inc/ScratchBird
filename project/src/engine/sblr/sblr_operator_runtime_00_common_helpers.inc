// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_operator_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
#include "sblr_operator_runtime.hpp"

#include "datatype_advanced_family.hpp"
#include "datatype_operations.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace {
namespace datatypes = scratchbird::core::datatypes;

bool IsOperator(std::string_view operator_id, std::string_view normalized, std::string_view legacy) {
  return operator_id == normalized || operator_id == legacy;
}

bool IsAnyOperator(std::string_view operator_id,
                   std::string_view normalized,
                   std::string_view legacy,
                   std::string_view legacy_alt) {
  return operator_id == normalized || operator_id == legacy || operator_id == legacy_alt;
}

SblrValue NullValue(std::string descriptor_id) {
  SblrValue out;
  out.descriptor_id = std::move(descriptor_id);
  out.payload_kind = SblrValuePayloadKind::none;
  out.is_null = true;
  return out;
}

SblrValue Int64Value(std::int64_t value) {
  SblrValue out;
  out.descriptor_id = "int64";
  out.is_null = false;
  out.has_int64_value = true;
  out.int64_value = value;
  out.payload_kind = SblrValuePayloadKind::signed_integer;
  out.encoded_value = std::to_string(value);
  out.text_value = std::to_string(value);
  return out;
}

SblrValue RealTextValue(std::string descriptor_id, long double value) {
  std::ostringstream stream;
  stream << std::setprecision(36) << value;
  SblrValue out;
  out.descriptor_id = std::move(descriptor_id);
  out.is_null = false;
  out.payload_kind = SblrValuePayloadKind::high_precision_numeric_text;
  out.encoded_value = stream.str();
  out.text_value = out.encoded_value;
  return out;
}

SblrValue TemporalValue(std::string descriptor_id, std::string encoded_value) {
  SblrValue out;
  out.descriptor_id = std::move(descriptor_id);
  out.is_null = false;
  out.payload_kind = SblrValuePayloadKind::temporal_text;
  out.encoded_value = std::move(encoded_value);
  out.text_value = out.encoded_value;
  return out;
}

SblrValue EncodedNumericValue(std::string descriptor_id, std::string encoded_value) {
  SblrValue out;
  out.descriptor_id = std::move(descriptor_id);
  out.is_null = false;
  out.payload_kind = SblrValuePayloadKind::high_precision_numeric_text;
  out.encoded_value = std::move(encoded_value);
  out.text_value = out.encoded_value;
  return out;
}

SblrValue TextValue(std::string value) {
  SblrValue out;
  out.descriptor_id = "text";
  out.is_null = false;
  out.payload_kind = SblrValuePayloadKind::text;
  out.encoded_value = value;
  out.text_value = std::move(value);
  return out;
}

SblrValue TextValueWithMetadata(std::string value, const SblrValue& left, const SblrValue& right) {
  SblrValue out = TextValue(std::move(value));
  if (!left.descriptor_id.empty() && left.descriptor_id != "text") {
    out.descriptor_id = left.descriptor_id;
  } else if (!right.descriptor_id.empty() && right.descriptor_id != "text") {
    out.descriptor_id = right.descriptor_id;
  }
  out.charset_name = !left.charset_name.empty() ? left.charset_name : right.charset_name;
  out.collation_name = !left.collation_name.empty() ? left.collation_name : right.collation_name;
  return out;
}

SblrValue BinaryValue(std::vector<std::uint8_t> value) {
  SblrValue out;
  out.descriptor_id = "binary";
  out.is_null = false;
  out.payload_kind = SblrValuePayloadKind::binary;
  out.binary_value = std::move(value);
  return out;
}

SblrValue BoolValue(SblrTruthValue value) {
  SblrValue out;
  out.descriptor_id = "boolean";
  out.payload_kind = SblrValuePayloadKind::boolean;
  out.is_null = value == SblrTruthValue::unknown;
  out.has_int64_value = value != SblrTruthValue::unknown;
  out.int64_value = value == SblrTruthValue::true_value ? 1 : 0;
  out.encoded_value = value == SblrTruthValue::true_value ? "TRUE" :
                      value == SblrTruthValue::false_value ? "FALSE" : "UNKNOWN";
  out.text_value = value == SblrTruthValue::true_value ? "TRUE" :
                   value == SblrTruthValue::false_value ? "FALSE" : "UNKNOWN";
  return out;
}

std::string ComparableText(const SblrValue& value) {
  if (value.has_int64_value) return std::to_string(value.int64_value);
  if (value.has_uint64_value) return std::to_string(value.uint64_value);
  if (value.has_real64_value) return std::to_string(value.real64_value);
  if (!value.encoded_value.empty()) return value.encoded_value;
  return value.text_value;
}

SblrResult ScalarResult(std::string_view operator_id, SblrValue value) {
  SblrResult out = MakeSblrSuccess(std::string(operator_id));
  out.scalar_values.push_back(std::move(value));
  return out;
}

SblrResult OperatorDiagnosticFailure(std::string_view operator_id,
                                     const SblrExecutionContext& context,
                                     std::string diagnostic_id,
                                     std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic(std::move(diagnostic_id), context, std::move(detail));
  diagnostic.fields.push_back({"operator_id", std::string(operator_id)});
  return MakeSblrFailure(SblrStatusCode::execution_failed, std::string(operator_id), std::move(diagnostic));
}

bool DescriptorLooksNumeric(std::string_view descriptor_id) {
  return descriptor_id == "int8" || descriptor_id == "int16" || descriptor_id == "int32" ||
         descriptor_id == "int64" || descriptor_id == "int128" || descriptor_id == "uint8" ||
         descriptor_id == "uint16" || descriptor_id == "uint32" || descriptor_id == "uint64" ||
         descriptor_id == "uint128" || descriptor_id == "real32" || descriptor_id == "real64" ||
         descriptor_id == "real128" || descriptor_id == "decimal" || descriptor_id == "decimal_float" ||
         descriptor_id == "numeric" || descriptor_id == "number";
}

bool DescriptorLooksTemporal(std::string_view descriptor_id) {
  return descriptor_id == "date" || descriptor_id == "time" || descriptor_id == "timestamp" ||
         descriptor_id == "timestamp_tz" || descriptor_id == "timestamp_epoch_ms";
}

bool DescriptorLooksInterval(std::string_view descriptor_id) {
  return descriptor_id == "interval";
}

bool IsNumericPayload(const SblrValue& value) {
  return value.has_int64_value || value.has_uint64_value || value.has_real64_value ||
         value.payload_kind == SblrValuePayloadKind::high_precision_numeric_text ||
         DescriptorLooksNumeric(value.descriptor_id);
}

bool IsTemporalPayload(const SblrValue& value) {
  return !DescriptorLooksInterval(value.descriptor_id) &&
         (value.payload_kind == SblrValuePayloadKind::temporal_text || DescriptorLooksTemporal(value.descriptor_id));
}

bool IsIntervalPayload(const SblrValue& value) {
  return DescriptorLooksInterval(value.descriptor_id);
}

bool NeedsDatatypeNumericBackend(const SblrValue& left, const SblrValue& right) {
  return left.payload_kind == SblrValuePayloadKind::high_precision_numeric_text ||
         right.payload_kind == SblrValuePayloadKind::high_precision_numeric_text ||
         left.descriptor_id == "decimal" || right.descriptor_id == "decimal" ||
         left.descriptor_id == "decimal_float" || right.descriptor_id == "decimal_float" ||
         left.descriptor_id == "real128" || right.descriptor_id == "real128" ||
         left.descriptor_id == "int128" || right.descriptor_id == "int128" ||
         left.descriptor_id == "uint128" || right.descriptor_id == "uint128";
}

std::string EncodedForDatatypeBackend(const SblrValue& value) {
  if (!value.encoded_value.empty()) return value.encoded_value;
  if (value.has_int64_value) return std::to_string(value.int64_value);
  if (value.has_uint64_value) return std::to_string(value.uint64_value);
  if (value.has_real64_value) return std::to_string(value.real64_value);
  return value.text_value;
}

datatypes::CanonicalTypeId NumericBackendType(const SblrValue& left, const SblrValue& right) {
  if (left.descriptor_id == "real128" || right.descriptor_id == "real128") {
    return datatypes::CanonicalTypeId::real128;
  }
  if (left.descriptor_id == "decimal_float" || right.descriptor_id == "decimal_float") {
    return datatypes::CanonicalTypeId::decimal_float;
  }
  return datatypes::CanonicalTypeId::decimal;
}

const char* DescriptorNameForNumericBackendType(datatypes::CanonicalTypeId type_id) {
  switch (type_id) {
    case datatypes::CanonicalTypeId::decimal: return "decimal";
    case datatypes::CanonicalTypeId::decimal_float: return "decimal_float";
    case datatypes::CanonicalTypeId::real128: return "real128";
    default: return "decimal";
  }
}

datatypes::DatatypeNumericOperationKind NumericBackendOperation(std::string_view operator_id) {
  if (IsAnyOperator(operator_id, "op_add", "operator.numeric.add", "operator.arithmetic.add")) {
    return datatypes::DatatypeNumericOperationKind::add;
  }
  if (IsAnyOperator(operator_id, "op_sub", "operator.numeric.subtract", "operator.arithmetic.subtract")) {
    return datatypes::DatatypeNumericOperationKind::subtract;
  }
  if (IsAnyOperator(operator_id, "op_mul", "operator.numeric.multiply", "operator.arithmetic.multiply")) {
    return datatypes::DatatypeNumericOperationKind::multiply;
  }
  if (IsAnyOperator(operator_id, "op_div", "operator.numeric.divide", "operator.arithmetic.divide")) {
    return datatypes::DatatypeNumericOperationKind::divide;
  }
  return datatypes::DatatypeNumericOperationKind::canonicalize;
}

SblrResult EvaluateDatatypeNumericArithmetic(std::string_view operator_id,
                                             const SblrValue& left,
                                             const SblrValue& right,
                                             const SblrExecutionContext& context) {
  const auto backend_type = NumericBackendType(left, right);
  datatypes::DatatypeNumericOperationRequest request;
  request.operation = NumericBackendOperation(operator_id);
  request.type_id = backend_type;
  request.left.type_id = backend_type;
  request.left.encoded_value = EncodedForDatatypeBackend(left);
  request.left.is_null = left.is_null;
