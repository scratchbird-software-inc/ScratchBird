// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::engine::internal_api::EngineDescriptor;
using scratchbird::engine::internal_api::EngineTypedValue;
using scratchbird::engine::internal_api::EngineValueState;
using scratchbird::core::datatypes::CanonicalTypeId;

DescriptorRuntimeDiagnostic OkDiagnostic() {
  return {};
}

DescriptorRuntimeDiagnostic ErrorDiagnostic(std::string code,
                                            std::string detail = {},
                                            std::size_t row = 0,
                                            std::size_t column = 0) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  diagnostic.row_index = row;
  diagnostic.column_index = column;
  return diagnostic;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool AsciiEqualFold(const std::string_view left,
                    const std::string_view lowercase_right) {
  if (left.size() != lowercase_right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (static_cast<char>(std::tolower(
            static_cast<unsigned char>(left[index]))) !=
        lowercase_right[index]) {
      return false;
    }
  }
  return true;
}

CanonicalTypeId CanonicalDescriptorTypeId(const EngineDescriptor& descriptor) {
  std::string_view type = descriptor.canonical_type_name;
  if (type.starts_with("sb.")) { type.remove_prefix(3); }
  if (type == "integer64") return CanonicalTypeId::int64;
  if (type == "bool") return CanonicalTypeId::boolean;
  if (type == "float8") return CanonicalTypeId::real64;
  if (type == "timestamp_tz") return CanonicalTypeId::timestamp;
  if (type == "blob" || type == "bytes") return CanonicalTypeId::binary;
  if (type == "uuidv7") return CanonicalTypeId::uuid;
  if (AsciiEqualFold(type, "string") || AsciiEqualFold(type, "varchar") ||
      AsciiEqualFold(type, "char") || AsciiEqualFold(type, "text")) {
    return CanonicalTypeId::character;
  }
  if (AsciiEqualFold(type, "integer") || AsciiEqualFold(type, "int")) {
    return CanonicalTypeId::int32;
  }
  if (AsciiEqualFold(type, "bigint")) return CanonicalTypeId::int64;
  if (AsciiEqualFold(type, "smallint")) return CanonicalTypeId::int16;
  if (AsciiEqualFold(type, "double") ||
      AsciiEqualFold(type, "double_precision") ||
      AsciiEqualFold(type, "double precision")) {
    return CanonicalTypeId::real64;
  }
  if (AsciiEqualFold(type, "float")) return CanonicalTypeId::real32;
  if (AsciiEqualFold(type, "set")) return CanonicalTypeId::set_value;
  for (const auto& builtin :
       scratchbird::core::datatypes::BuiltinDatatypeDescriptors()) {
    if (AsciiEqualFold(type, builtin.stable_name) ||
        AsciiEqualFold(
            type,
            scratchbird::core::datatypes::CanonicalTypeName(
                builtin.type_id))) {
      return builtin.type_id;
    }
  }
  return CanonicalTypeId::unknown;
}

bool IsBoundedSignedIntegerTypeId(const CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::int8 ||
         type_id == CanonicalTypeId::int16 ||
         type_id == CanonicalTypeId::int32 ||
         type_id == CanonicalTypeId::int64;
}

bool IsInt64Type(const EngineDescriptor& descriptor) {
  // QOW-SOURCE-PACKET7-RANGE-SAFE-SIGNED-INTEGER-DESCRIPTOR-V1
  // The descriptor runtime carries bounded signed integers in int64_t, but
  // canonical family and width authority remain with sb_core_datatypes.
  return IsBoundedSignedIntegerTypeId(CanonicalDescriptorTypeId(descriptor));
}

bool IsBoolType(const EngineDescriptor& descriptor) {
  const auto type = std::string_view(descriptor.canonical_type_name);
  return AsciiEqualFold(type, "bool") || AsciiEqualFold(type, "boolean") ||
         AsciiEqualFold(type, "sb.boolean");
}

bool IsReal64Type(const EngineDescriptor& descriptor) {
  const auto type = std::string_view(descriptor.canonical_type_name);
  return AsciiEqualFold(type, "real64") || AsciiEqualFold(type, "double") ||
         AsciiEqualFold(type, "float8") ||
         AsciiEqualFold(type, "sb.real64");
}

bool IsTextType(const EngineDescriptor& descriptor) {
  const auto type = std::string_view(descriptor.canonical_type_name);
  return AsciiEqualFold(type, "text") || AsciiEqualFold(type, "varchar") ||
         AsciiEqualFold(type, "string") || AsciiEqualFold(type, "sb.text") ||
         AsciiEqualFold(type, "timestamp") ||
         AsciiEqualFold(type, "timestamp_tz") ||
         AsciiEqualFold(type, "date") || AsciiEqualFold(type, "time");
}

bool IsBinaryType(const EngineDescriptor& descriptor) {
  const auto type = std::string_view(descriptor.canonical_type_name);
  return AsciiEqualFold(type, "blob") || AsciiEqualFold(type, "binary") ||
         AsciiEqualFold(type, "bytes");
}

bool IsUuidType(const EngineDescriptor& descriptor) {
  const auto type = std::string_view(descriptor.canonical_type_name);
  return AsciiEqualFold(type, "uuid") || AsciiEqualFold(type, "uuidv7");
}

bool IsOpaqueEncodedType(const EngineDescriptor& descriptor) {
  const auto type = std::string_view(descriptor.canonical_type_name);
  return IsBinaryType(descriptor) || IsUuidType(descriptor) ||
         AsciiEqualFold(type, "vector") ||
         AsciiEqualFold(type, "document") || AsciiEqualFold(type, "json") ||
         AsciiEqualFold(type, "graph") || AsciiEqualFold(type, "search") ||
         AsciiEqualFold(type, "geometry") ||
         AsciiEqualFold(type, "geography") || AsciiEqualFold(type, "point") ||
         AsciiEqualFold(type, "shape") || AsciiEqualFold(type, "raster") ||
         AsciiEqualFold(type, "columnar_segment");
}

bool IsKnownScalarType(const EngineDescriptor& descriptor) {
  switch (CanonicalDescriptorTypeId(descriptor)) {
    case CanonicalTypeId::boolean:
    case CanonicalTypeId::int8:
    case CanonicalTypeId::int16:
    case CanonicalTypeId::int32:
    case CanonicalTypeId::int64:
    case CanonicalTypeId::int128:
    case CanonicalTypeId::uint8:
    case CanonicalTypeId::uint16:
    case CanonicalTypeId::uint32:
    case CanonicalTypeId::uint64:
    case CanonicalTypeId::uint128:
    case CanonicalTypeId::real32:
    case CanonicalTypeId::real64:
    case CanonicalTypeId::real128:
    case CanonicalTypeId::decimal:
    case CanonicalTypeId::decimal_float:
    case CanonicalTypeId::uuid:
    case CanonicalTypeId::character:
    case CanonicalTypeId::binary:
    case CanonicalTypeId::date:
    case CanonicalTypeId::time:
    case CanonicalTypeId::timestamp:
    case CanonicalTypeId::interval:
      return true;
    default:
      return IsOpaqueEncodedType(descriptor);
  }
}

bool RequiresExpandedScalarValidation(const EngineDescriptor& descriptor) {
  switch (CanonicalDescriptorTypeId(descriptor)) {
    case CanonicalTypeId::int128:
    case CanonicalTypeId::uint8:
    case CanonicalTypeId::uint16:
    case CanonicalTypeId::uint32:
    case CanonicalTypeId::uint64:
    case CanonicalTypeId::uint128:
    case CanonicalTypeId::real32:
    case CanonicalTypeId::real128:
    case CanonicalTypeId::decimal:
    case CanonicalTypeId::decimal_float:
    case CanonicalTypeId::uuid:
    case CanonicalTypeId::character:
    case CanonicalTypeId::binary:
    case CanonicalTypeId::date:
    case CanonicalTypeId::time:
    case CanonicalTypeId::timestamp:
    case CanonicalTypeId::interval:
      return true;
    default:
      return false;
  }
}

std::optional<std::string_view> DescriptorField(
    const std::string_view descriptor,
    const std::string_view key) {
  std::optional<std::string_view> value;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const auto field = descriptor.substr(start, end - start);
    if (field.size() > key.size() && field[key.size()] == '=' &&
        field.substr(0, key.size()) == key) {
      if (value.has_value()) return std::nullopt;
      value = field.substr(key.size() + 1);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return value;
}

bool DescriptorU32(const EngineDescriptor& descriptor,
                   const std::string_view key,
                   std::uint32_t* value) {
  if (value == nullptr) return false;
  const auto field = DescriptorField(descriptor.encoded_descriptor, key);
  if (!field.has_value() || field->empty() ||
      (field->size() > 1 && field->front() == '0')) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (const auto ch : *field) {
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10u + static_cast<unsigned>(ch - '0');
    if (parsed > std::numeric_limits<std::uint32_t>::max()) return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ValidateExpandedScalarEncoding(const EngineDescriptor& descriptor,
                                    const std::string& encoded_value,
                                    std::string* detail) {
  namespace dt = scratchbird::core::datatypes;
  if (detail == nullptr) return false;
  detail->clear();
  const auto type_id = CanonicalDescriptorTypeId(descriptor);
  const bool extended_numeric =
      type_id == CanonicalTypeId::decimal ||
      type_id == CanonicalTypeId::decimal_float ||
      type_id == CanonicalTypeId::int128 ||
      type_id == CanonicalTypeId::real128;
  if (extended_numeric) {
    dt::DatatypeNumericOperationRequest request;
    request.operation = dt::DatatypeNumericOperationKind::canonicalize;
    request.type_id = type_id;
    request.left.type_id = type_id;
    request.left.encoded_value = encoded_value;
    if (type_id == CanonicalTypeId::decimal ||
        type_id == CanonicalTypeId::decimal_float) {
      if (!DescriptorU32(descriptor, "precision", &request.context.precision) ||
          !DescriptorU32(descriptor, "scale", &request.context.scale)) {
        *detail = "numeric descriptor precision or scale is unresolved";
        return false;
      }
    } else {
      std::uint32_t width = 0;
      if (!DescriptorU32(descriptor, "width", &width) || width != 128) {
        *detail = "128-bit numeric descriptor width is invalid";
        return false;
      }
      request.context.precision = 38;
      request.context.scale = 0;
    }
    const auto canonical = dt::ApplyNumericOperation(request);
    if (!canonical.ok()) {
      *detail = canonical.diagnostic.diagnostic_code;
      return false;
    }
    return true;
  }
  dt::DatatypeCastRequest request;
  request.value.type_id = type_id;
  request.value.encoded_value = encoded_value;
  request.target_type_id = type_id;
  request.explicit_cast = true;
  const auto canonical = dt::CastDatatypeValue(request);
  if (!canonical.ok()) {
    *detail = canonical.diagnostic.diagnostic_code;
    return false;
  }
  return true;
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

bool SameCanonicalDescriptor(const EngineDescriptor& left,
                             const EngineDescriptor& right) {
  return left.descriptor_uuid.canonical == right.descriptor_uuid.canonical &&
         left.descriptor_kind == right.descriptor_kind &&
         left.canonical_type_name == right.canonical_type_name &&
         left.encoded_descriptor == right.encoded_descriptor;
}

bool ParseInt64Strict(const std::string& text, std::int64_t* out) {
  if (out == nullptr || text.empty()) { return false; }
  const char* begin = text.data();
  const char* end = begin + text.size();
  bool explicit_positive = false;
  if (*begin == '+') {
    explicit_positive = true;
    ++begin;
    if (begin == end) return false;
  }
  std::int64_t parsed = 0;
  const auto conversion = std::from_chars(begin, end, parsed, 10);
  if (conversion.ec != std::errc{} || conversion.ptr != end ||
      (explicit_positive && parsed < 0)) {
    return false;
  }
  *out = parsed;
  return true;
}

bool ParseBoundedSignedIntegerStrict(const EngineDescriptor& descriptor,
                                     const std::string& text,
                                     std::int64_t* out) {
  const auto type_id = CanonicalDescriptorTypeId(descriptor);
  if (!IsBoundedSignedIntegerTypeId(type_id)) { return false; }

  std::int64_t parsed = 0;
  if (!ParseInt64Strict(text, &parsed)) return false;
  switch (type_id) {
    case CanonicalTypeId::int8:
      if (parsed < std::numeric_limits<std::int8_t>::min() ||
          parsed > std::numeric_limits<std::int8_t>::max()) {
        return false;
      }
      break;
    case CanonicalTypeId::int16:
      if (parsed < std::numeric_limits<std::int16_t>::min() ||
          parsed > std::numeric_limits<std::int16_t>::max()) {
        return false;
      }
      break;
    case CanonicalTypeId::int32:
      if (parsed < std::numeric_limits<std::int32_t>::min() ||
          parsed > std::numeric_limits<std::int32_t>::max()) {
        return false;
      }
      break;
    case CanonicalTypeId::int64:
      break;
    default:
      return false;
  }
  *out = parsed;
  return true;
}

bool ParseReal64Strict(const std::string& text, double* out) {
  if (out == nullptr || text.empty()) { return false; }
  char* end = nullptr;
  const double parsed = std::strtod(text.c_str(), &end);
  if (end == nullptr || end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

std::string FormatReal64(double value) {
  if (value == 0.0) return "0";
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

bool ParseFixedWidthNumber(const std::string& text, std::size_t offset, std::size_t width, std::int64_t* out) {
  if (out == nullptr || offset > text.size() || width > text.size() - offset) {
    return false;
  }
  std::int64_t parsed = 0;
  for (std::size_t i = 0; i < width; ++i) {
    const char c = text[offset + i];
    if (c < '0' || c > '9') { return false; }
    parsed = (parsed * 10) + static_cast<std::int64_t>(c - '0');
  }
  *out = parsed;
  return true;
}

void SetDiagnostic(DescriptorRuntimeDiagnostic* out, DescriptorRuntimeDiagnostic diagnostic) {
  if (out != nullptr) { *out = std::move(diagnostic); }
}

std::vector<std::string> RowKey(const DescriptorTuple& tuple) {
  std::vector<std::string> key;
  key.reserve(tuple.values.size());
  for (const auto& value : tuple.values) {
    std::string field;
    const auto append = [&](const std::string_view component) {
      const auto size = static_cast<std::uint64_t>(component.size());
      for (unsigned shift = 0; shift < 64; shift += 8) {
        field.push_back(static_cast<char>(size >> shift));
      }
      field.append(component);
    };
    append(value.descriptor.descriptor_uuid.canonical);
    append(value.descriptor.descriptor_kind);
    append(value.descriptor.canonical_type_name);
    append(value.descriptor.encoded_descriptor);
    field.push_back(static_cast<char>(value.state));
    field.push_back(value.is_null ? 1 : 0);
    std::string canonical_payload = value.encoded_value;
    std::string_view auxiliary_binary;
    if (value.state == EngineValueState::value && !value.is_null) {
      if (IsInt64Type(value.descriptor)) {
        const auto decoded = DecodeInt64Value(value);
        if (decoded.ok()) canonical_payload = std::to_string(decoded.value);
      } else if (IsBoolType(value.descriptor)) {
        const auto decoded = DecodeBoolValue(value);
        if (decoded.ok()) canonical_payload = decoded.value ? "true" : "false";
      } else if (IsReal64Type(value.descriptor)) {
        const auto decoded = DecodeReal64Value(value);
        if (decoded.ok()) canonical_payload = FormatReal64(decoded.value);
      } else if (IsBinaryType(value.descriptor) &&
                 !value.binary_value.empty()) {
        canonical_payload.assign(
            reinterpret_cast<const char*>(value.binary_value.data()),
            value.binary_value.size());
      } else if (!value.binary_value.empty()) {
        auxiliary_binary = std::string_view(
            reinterpret_cast<const char*>(value.binary_value.data()),
            value.binary_value.size());
      }
    }
    append(canonical_payload);
    append(auxiliary_binary);
    key.push_back(std::move(field));
  }
  return key;
}

bool SameDescriptorShape(const DescriptorBatch& left, const DescriptorBatch& right) {
  if (left.columns.size() != right.columns.size()) { return false; }
  for (std::size_t i = 0; i < left.columns.size(); ++i) {
    if (left.columns[i].stable_name != right.columns[i].stable_name ||
        left.columns[i].nullable != right.columns[i].nullable ||
        !DescriptorMatches(left.columns[i].descriptor, right.columns[i].descriptor)) {
      return false;
    }
  }
  return true;
}

bool DescriptorFamiliesEqual(const EngineDescriptor& left, const EngineDescriptor& right) {
  return (IsInt64Type(left) && IsInt64Type(right)) ||
         (IsReal64Type(left) && IsReal64Type(right)) ||
         (IsTextType(left) && IsTextType(right)) ||
         (IsBoolType(left) && IsBoolType(right));
}

std::optional<std::string> EqualityKeyForValue(const EngineTypedValue& value,
                                               const EngineDescriptor& descriptor,
                                               std::size_t row,
                                               std::size_t column,
                                               DescriptorRuntimeDiagnostic* diagnostic) {
  if (value.state == EngineValueState::sql_null) {
    if (!value.is_null || !value.encoded_value.empty() ||
        !value.binary_value.empty()) {
      SetDiagnostic(diagnostic, ErrorDiagnostic(
          "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
          "equality key received malformed SQL NULL", row, column));
    }
    return std::nullopt;
  }
  if (value.state != EngineValueState::value || value.is_null) {
    SetDiagnostic(diagnostic, ErrorDiagnostic(
        "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
        "equality key received a non-value sentinel", row, column));
    return std::nullopt;
  }
  if (IsInt64Type(descriptor)) {
    std::int64_t parsed = 0;
    if (!ParseBoundedSignedIntegerStrict(descriptor,
                                         value.encoded_value,
                                         &parsed)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_INT64_DECODE_FAILED", value.encoded_value, row, column));
      return std::nullopt;
    }
    return "i:" + std::to_string(parsed);
  }
  if (IsReal64Type(descriptor)) {
    double parsed = 0.0;
    if (!ParseReal64Strict(value.encoded_value, &parsed)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_REAL64_DECODE_FAILED", value.encoded_value, row, column));
      return std::nullopt;
    }
    return "r:" + FormatReal64(parsed);
  }
  if (IsBoolType(descriptor)) {
    const std::string text = LowerAscii(value.encoded_value);
    if (text == "true" || text == "1") return "b:1";
    if (text == "false" || text == "0") return "b:0";
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_BOOL_DECODE_FAILED", value.encoded_value, row, column));
    return std::nullopt;
  }
  if (IsTextType(descriptor)) { return "t:" + value.encoded_value; }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED",
                                            descriptor.canonical_type_name,
                                            row,
                                            column));
  return std::nullopt;
}

bool DescriptorValueGreaterThan(const EngineTypedValue& value,
                                const EngineTypedValue& bound,
                                const EngineDescriptor& descriptor,
                                std::size_t row,
                                std::size_t column,
                                DescriptorRuntimeDiagnostic* diagnostic,
                                bool* out) {
  if (out == nullptr) return false;
  *out = false;
  if (value.state == EngineValueState::sql_null ||
      bound.state == EngineValueState::sql_null) {
    const auto well_formed_null = [](const EngineTypedValue& operand) {
      return operand.state != EngineValueState::sql_null ||
             (operand.is_null && operand.encoded_value.empty() &&
              operand.binary_value.empty());
    };
    if (!well_formed_null(value) || !well_formed_null(bound)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic(
          "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
          "comparison received malformed SQL NULL", row, column));
      return false;
    }
    return true;
  }
  if (value.state != EngineValueState::value ||
      bound.state != EngineValueState::value || value.is_null ||
      bound.is_null) {
    SetDiagnostic(diagnostic, ErrorDiagnostic(
        "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
        "comparison received a non-value sentinel", row, column));
    return false;
  }
  if (IsInt64Type(descriptor)) {
    std::int64_t lhs = 0;
    std::int64_t rhs = 0;
    if (!ParseBoundedSignedIntegerStrict(descriptor,
                                         value.encoded_value,
                                         &lhs) ||
        !ParseBoundedSignedIntegerStrict(descriptor,
                                         bound.encoded_value,
                                         &rhs)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_INT64_DECODE_FAILED", value.encoded_value, row, column));
      return false;
    }
    *out = lhs > rhs;
    return true;
  }
  if (IsReal64Type(descriptor)) {
    double lhs = 0.0;
    double rhs = 0.0;
    if (!ParseReal64Strict(value.encoded_value, &lhs) ||
        !ParseReal64Strict(bound.encoded_value, &rhs)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_REAL64_DECODE_FAILED", value.encoded_value, row, column));
      return false;
    }
    *out = lhs > rhs;
    return true;
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED",
                                            descriptor.canonical_type_name,
                                            row,
                                            column));
  return false;
}

bool CanonicalDerivedDescriptorShapesMatch(
    const std::string_view left, const std::string_view right,
    bool* left_nullable, bool* right_nullable) {
  if (left_nullable == nullptr || right_nullable == nullptr) return false;
  bool left_found = false;
  bool right_found = false;
  std::size_t left_offset = 0;
  std::size_t right_offset = 0;
  const auto next_field = [](const std::string_view encoded,
                             std::size_t* offset, bool* found_nullable,
                             bool* nullable, std::string_view* normalized,
                             bool* done) {
    if (*offset > encoded.size()) {
      *done = true;
      return true;
    }
    *done = false;
    const auto end = encoded.find(';', *offset);
    auto field = encoded.substr(
        *offset, end == std::string_view::npos ? std::string_view::npos
                                               : end - *offset);
    *offset = end == std::string_view::npos ? encoded.size() + 1 : end + 1;
    if (field.empty()) return false;
    if (field.starts_with("nullability=") ||
        field.starts_with("nullable=")) {
      if (*found_nullable) return false;
      *found_nullable = true;
      const bool long_form = field.starts_with("nullability=");
      const auto value = field.substr(
          long_form ? std::string_view("nullability=").size()
                    : std::string_view("nullable=").size());
      if (value == (long_form ? "nullable" : "true")) {
        *nullable = true;
      } else if (value == (long_form ? "non_null" : "false")) {
        *nullable = false;
      } else {
        return false;
      }
      *normalized = "nullability=*";
      return true;
    }
    *normalized = field;
    return true;
  };
  while (true) {
    std::string_view left_field;
    std::string_view right_field;
    bool left_done = false;
    bool right_done = false;
    if (!next_field(left, &left_offset, &left_found, left_nullable,
                    &left_field, &left_done) ||
        !next_field(right, &right_offset, &right_found, right_nullable,
                    &right_field, &right_done) ||
        left_done != right_done) {
      return false;
    }
    if (left_done) break;
    if (left_field != right_field) return false;
  }
  return left_found && right_found;
}

}  // namespace

bool IsCanonicalBoundedSignedIntegerDescriptor(
    const EngineDescriptor& descriptor) {
  return IsInt64Type(descriptor);
}

EngineDescriptor MakeExecutorDescriptor(std::string canonical_type_name, std::string encoded_descriptor) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "executor.scalar";
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor = encoded_descriptor.empty()
                                      ? "canonical_type=" + descriptor.canonical_type_name
                                      : std::move(encoded_descriptor);
  return descriptor;
}

EngineTypedValue MakeExecutorValue(const EngineDescriptor& descriptor,
                                   std::string encoded_value,
                                   bool is_null) {
  EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded_value);
  value.is_null = is_null;
  value.state = is_null
                    ? scratchbird::engine::internal_api::EngineValueState::sql_null
                    : scratchbird::engine::internal_api::EngineValueState::value;
  return value;
}

DescriptorBatch MakeDescriptorBatch(std::vector<ExecutorColumnDescriptor> columns,
                                    std::vector<DescriptorTuple> rows) {
  DescriptorBatch batch;
  batch.columns = std::move(columns);
  batch.rows = std::move(rows);
  return batch;
}

std::string DescriptorFingerprint(const std::vector<ExecutorColumnDescriptor>& columns) {
  std::ostringstream out;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i != 0) { out << '|'; }
    out << columns[i].stable_name << ':'
        << columns[i].descriptor.descriptor_kind << ':'
        << columns[i].descriptor.canonical_type_name << ':'
        << columns[i].descriptor.encoded_descriptor << ':'
        << (columns[i].nullable ? 'N' : 'R');
  }
  return out.str();
}

bool DescriptorMatches(const EngineDescriptor& expected, const EngineDescriptor& actual) {
  if (!expected.descriptor_uuid.canonical.empty() || !actual.descriptor_uuid.canonical.empty()) {
    return expected.descriptor_uuid.canonical == actual.descriptor_uuid.canonical;
  }
  if (!expected.encoded_descriptor.empty() || !actual.encoded_descriptor.empty()) {
    return expected.encoded_descriptor == actual.encoded_descriptor;
  }
  return LowerAscii(expected.canonical_type_name) == LowerAscii(actual.canonical_type_name);
}

bool CanonicalDerivedDescriptorTypeMatches(
    const EngineDescriptor& input, const bool input_nullable,
    const EngineDescriptor& output, const bool expected_output_nullable) {
  bool encoded_input_nullable = false;
  bool encoded_output_nullable = false;
  return input.descriptor_kind == output.descriptor_kind &&
         input.canonical_type_name == output.canonical_type_name &&
         CanonicalDerivedDescriptorShapesMatch(
             input.encoded_descriptor, output.encoded_descriptor,
             &encoded_input_nullable, &encoded_output_nullable) &&
         input_nullable == encoded_input_nullable &&
         expected_output_nullable == encoded_output_nullable;
}

bool DeriveCanonicalNullableDescriptorEncoding(
    EngineDescriptor* descriptor) {
  if (descriptor == nullptr || descriptor->encoded_descriptor.empty()) {
    return false;
  }
  bool nullability_carrier_seen = false;
  std::size_t nullable_value_offset = std::string::npos;
  std::size_t nullable_value_width = 0;
  std::string_view nullable_replacement;
  std::size_t offset = 0;
  while (offset <= descriptor->encoded_descriptor.size()) {
    const auto separator = descriptor->encoded_descriptor.find(';', offset);
    const auto end = separator == std::string::npos
                         ? descriptor->encoded_descriptor.size()
                         : separator;
    const std::string_view field(
        descriptor->encoded_descriptor.data() + offset, end - offset);
    if (field.empty()) return false;
    if (field.starts_with("nullability=")) {
      const auto value = field.substr(std::string_view("nullability=").size());
      if (nullability_carrier_seen ||
          (value != "nullable" && value != "non_null")) {
        return false;
      }
      nullable_value_offset =
          offset + std::string_view("nullability=").size();
      nullable_value_width = value.size();
      nullable_replacement = "nullable";
      nullability_carrier_seen = true;
    } else if (field.starts_with("nullable=")) {
      const auto value = field.substr(std::string_view("nullable=").size());
      if (nullability_carrier_seen ||
          (value != "true" && value != "false")) {
        return false;
      }
      nullable_value_offset = offset + std::string_view("nullable=").size();
      nullable_value_width = value.size();
      nullable_replacement = "true";
      nullability_carrier_seen = true;
    }
    if (separator == std::string::npos) break;
    offset = separator + 1;
  }
  if (!nullability_carrier_seen || nullable_value_offset == std::string::npos ||
      nullable_replacement.size() > nullable_value_width) {
    return false;
  }
  // Both canonical rewrites are same-size or shrinking. Rewriting the
  // already-owned carrier in place avoids a second live descriptor encoding
  // during outer-join NULL-extension.
  descriptor->encoded_descriptor.replace(
      nullable_value_offset, nullable_value_width, nullable_replacement);
  return true;
}

DescriptorRuntimeDiagnostic ValidateDescriptorBatch(
    const DescriptorBatch& batch,
    const DescriptorCancellationProbe cancellation_requested,
    const void* cancellation_context,
    bool* cancellation_observed) {
  if (cancellation_observed != nullptr) *cancellation_observed = false;
  const auto poll_cancellation = [&](const std::size_t row,
                                     const std::size_t column)
      -> std::optional<DescriptorRuntimeDiagnostic> {
    if (!cancellation_requested) return std::nullopt;
    try {
      if (!cancellation_requested(cancellation_context)) return std::nullopt;
      if (cancellation_observed != nullptr) *cancellation_observed = true;
      return ErrorDiagnostic("SB_MODEL_EXECUTION_CANCELLED_V1",
                             "descriptor-batch validation was cancelled",
                             row, column);
    } catch (const std::exception&) {
      return ErrorDiagnostic(
          "SB_MODEL_COORDINATOR_LEG_FAILED_V1",
          "descriptor-batch cancellation probe threw a standard exception",
          row, column);
    } catch (...) {
      return ErrorDiagnostic(
          "SB_MODEL_COORDINATOR_LEG_FAILED_V1",
          "descriptor-batch cancellation probe threw a non-standard exception",
          row, column);
    }
  };
  if (const auto cancelled = poll_cancellation(0, 0);
      cancelled.has_value()) {
    return *cancelled;
  }
  if (batch.columns.empty()) {
    return ErrorDiagnostic("SB_EXECUTOR_DESCRIPTOR_REQUIRED", "column descriptor vector is empty");
  }
  for (std::size_t column = 0; column < batch.columns.size(); ++column) {
    if (const auto cancelled = poll_cancellation(0, column);
        cancelled.has_value()) {
      return *cancelled;
    }
    const auto& descriptor = batch.columns[column].descriptor;
    if (descriptor.canonical_type_name.empty() || descriptor.descriptor_kind.empty()) {
      return ErrorDiagnostic("SB_EXECUTOR_DESCRIPTOR_INVALID", "descriptor kind and canonical type are required", 0, column);
    }
    if (!IsKnownScalarType(descriptor)) {
      return ErrorDiagnostic("SB_EXECUTOR_DESCRIPTOR_TYPE_UNSUPPORTED", descriptor.canonical_type_name, 0, column);
    }
  }
  for (std::size_t row = 0; row < batch.rows.size(); ++row) {
    if (const auto cancelled = poll_cancellation(row, 0);
        cancelled.has_value()) {
      return *cancelled;
    }
    if (batch.rows[row].values.size() != batch.columns.size()) {
      return ErrorDiagnostic("SB_EXECUTOR_ROW_WIDTH_MISMATCH", "row width does not match descriptor width", row, 0);
    }
    for (std::size_t column = 0; column < batch.columns.size(); ++column) {
      if (const auto cancelled = poll_cancellation(row, column);
          cancelled.has_value()) {
        return *cancelled;
      }
      const auto& value = batch.rows[row].values[column];
      const auto& expected = batch.columns[column];
      if (!DescriptorMatches(expected.descriptor, value.descriptor)) {
        return ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", expected.stable_name, row, column);
      }
      if (value.state ==
          scratchbird::engine::internal_api::EngineValueState::sql_null) {
        if (!value.is_null || !value.encoded_value.empty() ||
            !value.binary_value.empty() || !expected.nullable) {
          return ErrorDiagnostic(
              "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
              "SQL NULL state, payload, or nullability is malformed", row,
              column);
        }
        continue;
      }
      if (value.state !=
              scratchbird::engine::internal_api::EngineValueState::value ||
          value.is_null) {
        return ErrorDiagnostic(
            "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
            "non-value sentinel or legacy NULL flag reached operator", row,
            column);
      }
      if (!value.binary_value.empty() &&
          (IsInt64Type(expected.descriptor) ||
           IsBoolType(expected.descriptor) ||
           IsReal64Type(expected.descriptor) ||
           IsTextType(expected.descriptor))) {
        return ErrorDiagnostic(
            "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
            "encoded scalar value carries an auxiliary binary payload", row,
            column);
      }
      if (IsInt64Type(expected.descriptor)) {
        std::int64_t ignored = 0;
        if (!ParseBoundedSignedIntegerStrict(expected.descriptor,
                                             value.encoded_value,
                                             &ignored)) {
          return ErrorDiagnostic("SB_EXECUTOR_INT64_DECODE_FAILED", value.encoded_value, row, column);
        }
      } else if (IsBoolType(expected.descriptor)) {
        const auto text = std::string_view(value.encoded_value);
        if (!AsciiEqualFold(text, "true") &&
            !AsciiEqualFold(text, "false") && text != "1" && text != "0") {
          return ErrorDiagnostic("SB_EXECUTOR_BOOL_DECODE_FAILED", value.encoded_value, row, column);
        }
      } else if (IsReal64Type(expected.descriptor)) {
        double ignored = 0.0;
        if (!ParseReal64Strict(value.encoded_value, &ignored)) {
          return ErrorDiagnostic("SB_EXECUTOR_REAL64_DECODE_FAILED", value.encoded_value, row, column);
        }
      } else if (RequiresExpandedScalarValidation(expected.descriptor)) {
        std::string detail;
        if (!ValidateExpandedScalarEncoding(expected.descriptor,
                                            value.encoded_value, &detail)) {
          return ErrorDiagnostic(
              "QOW-DIAG-QRY-008-RUNTIME-BREADTH-REFUSAL-V1",
              detail.empty() ? value.encoded_value : std::move(detail), row,
              column);
        }
      }
    }
  }
  return OkDiagnostic();
}

DescriptorRuntimeDiagnostic ValidateDescriptorBatch(
    const DescriptorBatch& batch,
    const std::function<bool()>& cancellation_requested,
    bool* cancellation_observed) {
  const auto probe = [](const void* context) {
    return (*static_cast<const std::function<bool()>*>(context))();
  };
  const DescriptorCancellationProbe callback =
      cancellation_requested ? +probe : nullptr;
  return ValidateDescriptorBatch(
      batch, callback,
      cancellation_requested ? &cancellation_requested : nullptr,
      cancellation_observed);
}

DescriptorRuntimeDiagnostic ValidateDescriptorBatch(
    const DescriptorBatch& batch) {
  return ValidateDescriptorBatch(batch, nullptr, nullptr, nullptr);
}

std::optional<std::uint64_t> BoundDescriptorBatchValidationScratchMemoryBytes(
    const DescriptorBatch& batch,
    const std::function<bool()>& cancellation_requested,
    bool* cancellation_observed,
    bool* cancellation_probe_failed) {
  if (cancellation_observed != nullptr) *cancellation_observed = false;
  if (cancellation_probe_failed != nullptr) *cancellation_probe_failed = false;
  const auto cancelled = [&]() {
    if (!cancellation_requested) return false;
    try {
      if (!cancellation_requested()) return false;
    } catch (...) {
      if (cancellation_probe_failed != nullptr) {
        *cancellation_probe_failed = true;
      }
      return true;
    }
    if (cancellation_observed != nullptr) *cancellation_observed = true;
    return true;
  };
  // Expanded numeric validation passes through a finite chain of request,
  // canonicalization, backend, result, and diagnostic carriers. At most 32
  // simultaneous strings/cpp_int values can each own a source- or
  // exponent-expanded payload; cpp_int limbs require less than one byte per
  // decimal digit. The 128-byte-per-admitted-work-byte multiplier therefore
  // dominates all such carriers plus allocator headers. The fixed 64 KiB
  // covers carrier objects and all bounded diagnostic literals.
  constexpr std::uint64_t kFixedScratchBytes = 64 * 1024;
  constexpr std::uint64_t kDynamicExpansionFactor = 128;
  std::uint64_t maximum = kFixedScratchBytes;
  for (std::size_t column = 0; column < batch.columns.size(); ++column) {
    if (cancelled()) return std::nullopt;
    const auto& descriptor = batch.columns[column].descriptor;
    const auto type_id = CanonicalDescriptorTypeId(descriptor);
    std::uint64_t descriptor_bytes = 1;
    for (const auto* carrier : {&batch.columns[column].stable_name,
                                &descriptor.descriptor_uuid.canonical,
                                &descriptor.descriptor_kind,
                                &descriptor.canonical_type_name,
                                &descriptor.encoded_descriptor}) {
      if (carrier->size() >
          std::numeric_limits<std::uint64_t>::max() - descriptor_bytes) {
        return std::nullopt;
      }
      descriptor_bytes += static_cast<std::uint64_t>(carrier->size());
    }
    if (descriptor_bytes >
        (std::numeric_limits<std::uint64_t>::max() - kFixedScratchBytes) /
            kDynamicExpansionFactor) {
      return std::nullopt;
    }
    maximum = std::max(
        maximum, kFixedScratchBytes +
                     descriptor_bytes * kDynamicExpansionFactor);
    for (const auto& row : batch.rows) {
      if (cancelled()) return std::nullopt;
      if (column >= row.values.size()) continue;
      const auto& value = row.values[column];
      if (value.isSqlNull()) continue;
      std::uint64_t source_bytes = 1;
      const auto account_source = [&](const std::size_t size) {
        if (size > std::numeric_limits<std::uint64_t>::max() - source_bytes) {
          return false;
        }
        source_bytes += static_cast<std::uint64_t>(size);
        return true;
      };
      if (!account_source(value.encoded_value.size()) ||
          !account_source(value.binary_value.size()) ||
          !account_source(batch.columns[column].stable_name.size()) ||
          !account_source(descriptor.descriptor_uuid.canonical.size()) ||
          !account_source(descriptor.descriptor_kind.size()) ||
          !account_source(descriptor.canonical_type_name.size()) ||
          !account_source(descriptor.encoded_descriptor.size())) {
        return std::nullopt;
      }
      std::uint64_t admitted_work_bytes = source_bytes;
      const bool decimal_type = type_id == CanonicalTypeId::decimal ||
                                type_id == CanonicalTypeId::decimal_float;
      const bool exponent_numeric = decimal_type ||
                                    type_id == CanonicalTypeId::real128;
      if (exponent_numeric) {
        auto text = std::string_view(value.encoded_value);
        while (!text.empty() &&
               std::isspace(static_cast<unsigned char>(text.front()))) {
          text.remove_prefix(1);
        }
        while (!text.empty() &&
               std::isspace(static_cast<unsigned char>(text.back()))) {
          text.remove_suffix(1);
        }
        const auto exponent_marker = text.find_first_of("eE");
        if (exponent_marker != std::string_view::npos) {
          auto exponent_text = text.substr(exponent_marker + 1);
          bool negative_exponent = false;
          if (!exponent_text.empty() &&
              (exponent_text.front() == '+' ||
               exponent_text.front() == '-')) {
            negative_exponent = exponent_text.front() == '-';
            exponent_text.remove_prefix(1);
          }
          std::uint64_t exponent = 0;
          bool exponent_syntax_valid = !exponent_text.empty();
          for (const auto ch : exponent_text) {
            // Mirror sbl_numeric's finite exponent parser. Invalid syntax or
            // an exponent rejected before Pow10 needs only the source-sized
            // bound and must reach the typed validator for its diagnostic.
            if (ch < '0' || ch > '9' || exponent > 100000) {
              exponent_syntax_valid = false;
              break;
            }
            exponent = exponent * 10 + static_cast<unsigned>(ch - '0');
          }
          if (exponent_syntax_valid && !negative_exponent) {
            const auto mantissa = text.substr(0, exponent_marker);
            std::uint64_t coefficient_digits = 0;
            std::uint64_t fractional_digits = 0;
            bool decimal_point = false;
            bool nonzero_seen = false;
            bool mantissa_syntax_valid = !mantissa.empty();
            bool mantissa_digit_seen = false;
            std::size_t mantissa_offset = 0;
            if (mantissa_syntax_valid &&
                (mantissa.front() == '+' || mantissa.front() == '-')) {
              ++mantissa_offset;
              if (mantissa_offset == mantissa.size()) {
                mantissa_syntax_valid = false;
              }
            }
            for (; mantissa_syntax_valid &&
                   mantissa_offset < mantissa.size(); ++mantissa_offset) {
              const auto ch = mantissa[mantissa_offset];
              if (ch == '.') {
                if (decimal_point) {
                  mantissa_syntax_valid = false;
                  break;
                }
                decimal_point = true;
                continue;
              }
              if (ch < '0' || ch > '9') {
                mantissa_syntax_valid = false;
                break;
              }
              mantissa_digit_seen = true;
              if (decimal_point) ++fractional_digits;
              if (ch != '0') nonzero_seen = true;
              if (nonzero_seen) ++coefficient_digits;
            }
            mantissa_syntax_valid =
                mantissa_syntax_valid && mantissa_digit_seen;
            if (mantissa_syntax_valid) {
              if (coefficient_digits == 0) coefficient_digits = 1;
              const auto parse_expansion =
                  nonzero_seen && exponent >= fractional_digits
                      ? exponent - fractional_digits
                      : 0;
              if (parse_expansion >
                  std::numeric_limits<std::uint64_t>::max() -
                      coefficient_digits) {
                return std::nullopt;
              }
              const auto parsed_coefficient_digits =
                  coefficient_digits + parse_expansion;
              admitted_work_bytes = std::max(admitted_work_bytes,
                                             parsed_coefficient_digits);
              if (type_id == CanonicalTypeId::real128) {
                // Real128 canonicalization parses the decimal for
                // negative-zero detection. Only exponent beyond fractional
                // scale expands its cpp_int; long zero spellings remain
                // source-sized.
              } else {
                std::uint32_t precision = 0;
                std::uint32_t scale = 0;
                const bool descriptor_syntax_valid =
                    DescriptorU32(descriptor, "precision", &precision) &&
                    DescriptorU32(descriptor, "scale", &scale) &&
                    precision != 0 && precision <= 38 && scale <= precision;
                if (descriptor_syntax_valid) {
                  const auto parsed_scale = exponent >= fractional_digits
                                                ? 0
                                                : fractional_digits - exponent;
                  if (parsed_scale <= scale) {
                    const auto round_expansion = scale - parsed_scale;
                    if (round_expansion >
                        std::numeric_limits<std::uint64_t>::max() -
                            parsed_coefficient_digits) {
                      return std::nullopt;
                    }
                    admitted_work_bytes = std::max(
                        admitted_work_bytes,
                        parsed_coefficient_digits + round_expansion);
                  } else {
                    const auto drop = parsed_scale - scale;
                    // RoundToScale short-circuits when its divisor would be
                    // wider than the coefficient. Otherwise account the
                    // divisor, quotient, remainder, and a possible carry.
                    if (drop <= parsed_coefficient_digits) {
                      admitted_work_bytes = std::max(
                          admitted_work_bytes,
                          std::max(parsed_coefficient_digits, drop + 1));
                    }
                  }
                } else {
                  // Descriptor validation will provide the canonical error;
                  // it cannot reach numeric expansion with invalid context.
                }
              }
            }
          }
        }
      }
      if (admitted_work_bytes >
              (std::numeric_limits<std::uint64_t>::max() -
               kFixedScratchBytes) /
                  kDynamicExpansionFactor) {
        return std::nullopt;
      }
      const auto scratch =
          kFixedScratchBytes +
          admitted_work_bytes * kDynamicExpansionFactor;
      maximum = std::max(maximum, scratch);
    }
  }
  return maximum;
}

DescriptorRuntimeDiagnostic ValidateCanonicalDescriptorBatch(
    const DescriptorBatch& batch,
    const std::vector<std::uint32_t>& output_descriptor_ids,
    const DescriptorCancellationProbe cancellation_requested,
    const void* cancellation_context,
    bool* cancellation_observed) {
  if (cancellation_observed != nullptr) *cancellation_observed = false;
  const auto poll_cancellation = [&](const std::size_t row,
                                     const std::size_t column)
      -> std::optional<DescriptorRuntimeDiagnostic> {
    if (cancellation_requested == nullptr) return std::nullopt;
    try {
      if (!cancellation_requested(cancellation_context)) return std::nullopt;
      if (cancellation_observed != nullptr) *cancellation_observed = true;
      return ErrorDiagnostic("SB_MODEL_EXECUTION_CANCELLED_V1",
                             "descriptor-batch validation was cancelled",
                             row, column);
    } catch (const std::exception&) {
      return ErrorDiagnostic(
          "SB_MODEL_COORDINATOR_LEG_FAILED_V1",
          "descriptor-batch cancellation probe threw a standard exception",
          row, column);
    } catch (...) {
      return ErrorDiagnostic(
          "SB_MODEL_COORDINATOR_LEG_FAILED_V1",
          "descriptor-batch cancellation probe threw a non-standard exception",
          row, column);
    }
  };
  if (const auto cancelled = poll_cancellation(0, 0);
      cancelled.has_value()) {
    return *cancelled;
  }
  if (batch.columns.size() != output_descriptor_ids.size() ||
      batch.columns.empty()) {
    return ErrorDiagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                           "physical output descriptor width mismatch");
  }
  for (std::size_t column = 0; column < batch.columns.size(); ++column) {
    if (const auto cancelled = poll_cancellation(0, column);
        cancelled.has_value()) {
      return *cancelled;
    }
    const auto& bound_column = batch.columns[column];
    const auto& descriptor = bound_column.descriptor;
    bool duplicate_descriptor_id = false;
    for (std::size_t prior = 0; prior < column; ++prior) {
      if (batch.columns[prior].descriptor_id ==
          bound_column.descriptor_id) {
        duplicate_descriptor_id = true;
        break;
      }
    }
    if (bound_column.descriptor_id == 0 ||
        bound_column.descriptor_id != output_descriptor_ids[column] ||
        duplicate_descriptor_id ||
        bound_column.stable_name.empty() ||
        !IsCanonicalUuid(descriptor.descriptor_uuid.canonical) ||
        descriptor.descriptor_kind != "scalar" ||
        descriptor.canonical_type_name.empty() ||
        descriptor.encoded_descriptor.empty() ||
        !CanonicalDerivedDescriptorTypeMatches(
            descriptor, bound_column.nullable, descriptor,
            bound_column.nullable)) {
      return ErrorDiagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                             "canonical output descriptor or nullability is unresolved",
                             0, column);
    }
  }
  for (std::size_t row = 0; row < batch.rows.size(); ++row) {
    if (const auto cancelled = poll_cancellation(row, 0);
        cancelled.has_value()) {
      return *cancelled;
    }
    if (batch.rows[row].values.size() != batch.columns.size()) {
      return ErrorDiagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                             "typed row width does not match physical output",
                             row, 0);
    }
    for (std::size_t column = 0; column < batch.columns.size(); ++column) {
      if (const auto cancelled = poll_cancellation(row, column);
          cancelled.has_value()) {
        return *cancelled;
      }
      const auto& value = batch.rows[row].values[column];
      const auto& bound_column = batch.columns[column];
      if (!SameCanonicalDescriptor(value.descriptor,
                                   bound_column.descriptor)) {
        return ErrorDiagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                               "typed value lost its canonical descriptor",
                               row, column);
      }
      if (value.state ==
          scratchbird::engine::internal_api::EngineValueState::sql_null) {
        if (!value.is_null || !value.encoded_value.empty() ||
            !value.binary_value.empty() || !bound_column.nullable) {
          return ErrorDiagnostic(
              "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
              "canonical SQL NULL state is malformed or non-nullable", row,
              column);
        }
        continue;
      }
      if (value.state !=
              scratchbird::engine::internal_api::EngineValueState::value ||
          value.is_null) {
        return ErrorDiagnostic(
            "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
            "legacy NULL flag or non-value sentinel reached operator", row,
            column);
      }
    }
  }
  return OkDiagnostic();
}

DescriptorRuntimeDiagnostic ValidateCanonicalDescriptorBatch(
    const DescriptorBatch& batch,
    const std::vector<std::uint32_t>& output_descriptor_ids,
    const std::function<bool()>& cancellation_requested,
    bool* cancellation_observed) {
  const auto probe = [](const void* context) {
    return (*static_cast<const std::function<bool()>*>(context))();
  };
  auto diagnostic = ValidateCanonicalDescriptorBatch(
      batch, output_descriptor_ids,
      cancellation_requested ? +probe : nullptr,
      cancellation_requested ? &cancellation_requested : nullptr,
      cancellation_observed);
  if (diagnostic.diagnostic_code == "SB_MODEL_EXECUTION_CANCELLED_V1") {
    diagnostic.diagnostic_code = "QOW-DIAG-QRY-012-JOIN-CANCELLED-V1";
  } else if (diagnostic.diagnostic_code ==
             "SB_MODEL_COORDINATOR_LEG_FAILED_V1") {
    diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-012-CANCELLATION-PROBE-V1";
  }
  return diagnostic;
}

std::optional<std::size_t> FindColumnByStableName(const DescriptorBatch& batch, const std::string& stable_name) {
  for (std::size_t i = 0; i < batch.columns.size(); ++i) {
    if (batch.columns[i].stable_name == stable_name) { return i; }
  }
  return std::nullopt;
}

// QOW-SOURCE-IAS-014-V1
// QOW-SOURCE-QRY-029-V1
// Reusable projection mechanics.  The production physical entry in
// projection_executor.cpp resolves the canonical DAG and descriptor handles
// before invoking this helper.
DescriptorBatch ProjectDescriptorBatch(const DescriptorBatch& input, const std::vector<std::size_t>& columns) {
  DescriptorBatch output;
  for (const auto column : columns) {
    if (column < input.columns.size()) { output.columns.push_back(input.columns[column]); }
  }
  output.rows.reserve(input.rows.size());
  for (const auto& row : input.rows) {
    DescriptorTuple projected;
    for (const auto column : columns) {
      if (column < row.values.size()) { projected.values.push_back(row.values[column]); }
    }
    output.rows.push_back(std::move(projected));
  }
  return output;
}

DescriptorBatch FilterDescriptorInt64GreaterThan(const DescriptorBatch& input,
                                                 std::size_t column,
                                                 std::int64_t threshold,
                                                 DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "filter column out of range", 0, column));
    return {};
  }
  if (!IsInt64Type(input.columns[column].descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED", input.columns[column].stable_name, 0, column));
    return {};
  }
  DescriptorBatch output;
  output.columns = input.columns;
  for (std::size_t row_index = 0; row_index < input.rows.size(); ++row_index) {
    const auto decoded = DecodeInt64Value(input.rows[row_index].values[column]);
    if (!decoded.ok()) {
      auto diag = decoded.diagnostic;
      diag.row_index = row_index;
      diag.column_index = column;
      SetDiagnostic(diagnostic, std::move(diag));
      return {};
    }
    if (decoded.value > threshold) { output.rows.push_back(input.rows[row_index]); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch FilterDescriptorBatchByComparison(
    const DescriptorBatch& input,
    std::size_t column,
    DescriptorComparisonOperator op,
    const EngineTypedValue& bound_value,
    DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "filter column out of range", 0, column));
    return {};
  }
  const auto& descriptor = input.columns[column].descriptor;
  if (IsOpaqueEncodedType(descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED", input.columns[column].stable_name, 0, column));
    return {};
  }
  DescriptorBatch bound_batch;
  bound_batch.columns = {{"comparison_bound", bound_value.descriptor, true}};
  bound_batch.rows = {{{bound_value}}};
  const auto bound_validation = ValidateDescriptorBatch(bound_batch);
  if (!bound_validation.ok) {
    SetDiagnostic(diagnostic, bound_validation);
    return {};
  }
  if (bound_value.state != EngineValueState::sql_null &&
      !DescriptorFamiliesEqual(descriptor, bound_value.descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", input.columns[column].stable_name, 0, column));
    return {};
  }

  std::optional<std::string> bound_key;
  if (op == DescriptorComparisonOperator::kEqual) {
    bound_key = EqualityKeyForValue(bound_value, descriptor, 0, column, diagnostic);
    if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
  } else if (!IsInt64Type(descriptor) && !IsReal64Type(descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED", input.columns[column].stable_name, 0, column));
    return {};
  }

  DescriptorBatch output;
  output.columns = input.columns;
  for (std::size_t row_index = 0; row_index < input.rows.size(); ++row_index) {
    bool matches = false;
    if (op == DescriptorComparisonOperator::kEqual) {
      const auto key = EqualityKeyForValue(input.rows[row_index].values[column],
                                           descriptor,
                                           row_index,
                                           column,
                                           diagnostic);
      if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
      matches = key && bound_key && *key == *bound_key;
    } else {
      if (!DescriptorValueGreaterThan(input.rows[row_index].values[column],
                                      bound_value,
                                      descriptor,
                                      row_index,
                                      column,
                                      diagnostic,
                                      &matches)) {
        return {};
      }
    }
    if (matches) { output.rows.push_back(input.rows[row_index]); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch SortDescriptorBatchByColumn(const DescriptorBatch& input,
                                            std::size_t column,
                                            bool ascending,
                                            DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "sort column out of range", 0, column));
    return {};
  }
  DescriptorBatch output = input;
  if (IsInt64Type(input.columns[column].descriptor)) {
    std::stable_sort(output.rows.begin(), output.rows.end(), [&](const auto& lhs, const auto& rhs) {
      const auto l = DecodeInt64Value(lhs.values[column]);
      const auto r = DecodeInt64Value(rhs.values[column]);
      return ascending ? l.value < r.value : l.value > r.value;
    });
  } else {
    std::stable_sort(output.rows.begin(), output.rows.end(), [&](const auto& lhs, const auto& rhs) {
      const std::string& l = lhs.values[column].encoded_value;
      const std::string& r = rhs.values[column].encoded_value;
      return ascending ? l < r : l > r;
    });
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch LimitOffsetDescriptorBatch(const DescriptorBatch& input,
                                           std::size_t limit,
                                           std::size_t offset) {
  DescriptorBatch output;
  output.columns = input.columns;
  if (offset >= input.rows.size()) { return output; }
  const auto available = input.rows.size() - offset;
  const auto end = offset + std::min(limit, available);
  for (std::size_t i = offset; i < end; ++i) {
    output.rows.push_back(input.rows[i]);
  }
  return output;
}

DescriptorBatch SetUnionDistinctDescriptorBatch(const DescriptorBatch& left,
                                                const DescriptorBatch& right,
                                                DescriptorRuntimeDiagnostic* diagnostic) {
  const auto left_validation = ValidateDescriptorBatch(left);
  if (!left_validation.ok) {
    SetDiagnostic(diagnostic, left_validation);
    return {};
  }
  const auto right_validation = ValidateDescriptorBatch(right);
  if (!right_validation.ok) {
    SetDiagnostic(diagnostic, right_validation);
    return {};
  }
  if (!SameDescriptorShape(left, right)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_SETOP_DESCRIPTOR_MISMATCH"));
    return {};
  }
  DescriptorBatch output;
  output.columns = left.columns;
  std::set<std::vector<std::string>> seen;
  for (const auto& row : left.rows) {
    if (seen.insert(RowKey(row)).second) { output.rows.push_back(row); }
  }
  for (const auto& row : right.rows) {
    if (seen.insert(RowKey(row)).second) { output.rows.push_back(row); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch SetIntersectDistinctDescriptorBatch(const DescriptorBatch& left,
                                                    const DescriptorBatch& right,
                                                    DescriptorRuntimeDiagnostic* diagnostic) {
  const auto left_validation = ValidateDescriptorBatch(left);
  if (!left_validation.ok) {
    SetDiagnostic(diagnostic, left_validation);
    return {};
  }
  const auto right_validation = ValidateDescriptorBatch(right);
  if (!right_validation.ok) {
    SetDiagnostic(diagnostic, right_validation);
    return {};
  }
  if (!SameDescriptorShape(left, right)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_SETOP_DESCRIPTOR_MISMATCH"));
    return {};
  }
  DescriptorBatch output;
  output.columns = left.columns;
  std::set<std::vector<std::string>> right_keys;
  for (const auto& row : right.rows) { right_keys.insert(RowKey(row)); }
  std::set<std::vector<std::string>> emitted;
  for (const auto& row : left.rows) {
    const auto key = RowKey(row);
    if (right_keys.count(key) != 0 && emitted.insert(key).second) { output.rows.push_back(row); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch SetExceptDistinctDescriptorBatch(const DescriptorBatch& left,
                                                 const DescriptorBatch& right,
                                                 DescriptorRuntimeDiagnostic* diagnostic) {
  const auto left_validation = ValidateDescriptorBatch(left);
  if (!left_validation.ok) {
    SetDiagnostic(diagnostic, left_validation);
    return {};
  }
  const auto right_validation = ValidateDescriptorBatch(right);
  if (!right_validation.ok) {
    SetDiagnostic(diagnostic, right_validation);
    return {};
  }
  if (!SameDescriptorShape(left, right)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_SETOP_DESCRIPTOR_MISMATCH"));
    return {};
  }
  DescriptorBatch output;
  output.columns = left.columns;
  std::set<std::vector<std::string>> right_keys;
  for (const auto& row : right.rows) { right_keys.insert(RowKey(row)); }
  std::set<std::vector<std::string>> emitted;
  for (const auto& row : left.rows) {
    const auto key = RowKey(row);
    if (right_keys.count(key) == 0 && emitted.insert(key).second) { output.rows.push_back(row); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch JoinDescriptorBatchesOnInt64(const DescriptorBatch& left,
                                             const DescriptorBatch& right,
                                             std::size_t left_column,
                                             std::size_t right_column,
                                             DescriptorRuntimeDiagnostic* diagnostic) {
  const auto left_valid = ValidateDescriptorBatch(left);
  if (!left_valid.ok) {
    SetDiagnostic(diagnostic, left_valid);
    return {};
  }
  const auto right_valid = ValidateDescriptorBatch(right);
  if (!right_valid.ok) {
    SetDiagnostic(diagnostic, right_valid);
    return {};
  }
  if (left_column >= left.columns.size() || right_column >= right.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "join column out of range"));
    return {};
  }
  if (!IsInt64Type(left.columns[left_column].descriptor) || !IsInt64Type(right.columns[right_column].descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_JOIN_TYPE_UNSUPPORTED", "join keys must be int64"));
    return {};
  }

  DescriptorBatch output;
  output.columns.reserve(left.columns.size() + right.columns.size());
  output.columns.insert(output.columns.end(), left.columns.begin(), left.columns.end());
  output.columns.insert(output.columns.end(), right.columns.begin(), right.columns.end());

  for (std::size_t left_row = 0; left_row < left.rows.size(); ++left_row) {
    if (left.rows[left_row].values[left_column].is_null) { continue; }
    const auto left_key = DecodeInt64Value(left.rows[left_row].values[left_column]);
    if (!left_key.ok()) {
      auto diag = left_key.diagnostic;
      diag.row_index = left_row;
      diag.column_index = left_column;
      SetDiagnostic(diagnostic, std::move(diag));
      return {};
    }
    for (std::size_t right_row = 0; right_row < right.rows.size(); ++right_row) {
      if (right.rows[right_row].values[right_column].is_null) { continue; }
      const auto right_key = DecodeInt64Value(right.rows[right_row].values[right_column]);
      if (!right_key.ok()) {
        auto diag = right_key.diagnostic;
        diag.row_index = right_row;
        diag.column_index = right_column;
        SetDiagnostic(diagnostic, std::move(diag));
        return {};
      }
      if (left_key.value != right_key.value) { continue; }
      DescriptorTuple joined;
      joined.values.reserve(left.rows[left_row].values.size() + right.rows[right_row].values.size());
      joined.values.insert(joined.values.end(), left.rows[left_row].values.begin(), left.rows[left_row].values.end());
      joined.values.insert(joined.values.end(), right.rows[right_row].values.begin(), right.rows[right_row].values.end());
      output.rows.push_back(std::move(joined));
    }
  }

  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch JoinDescriptorBatchesOnEqual(const DescriptorBatch& left,
                                             const DescriptorBatch& right,
                                             std::size_t left_column,
                                             std::size_t right_column,
                                             DescriptorRuntimeDiagnostic* diagnostic) {
  const auto left_valid = ValidateDescriptorBatch(left);
  if (!left_valid.ok) {
    SetDiagnostic(diagnostic, left_valid);
    return {};
  }
  const auto right_valid = ValidateDescriptorBatch(right);
  if (!right_valid.ok) {
    SetDiagnostic(diagnostic, right_valid);
    return {};
  }
  if (left_column >= left.columns.size() || right_column >= right.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "join column out of range"));
    return {};
  }
  const auto& left_descriptor = left.columns[left_column].descriptor;
  const auto& right_descriptor = right.columns[right_column].descriptor;
  if (IsOpaqueEncodedType(left_descriptor) || IsOpaqueEncodedType(right_descriptor) ||
      !DescriptorFamiliesEqual(left_descriptor, right_descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_JOIN_TYPE_UNSUPPORTED", "join keys must be comparable core scalar descriptors"));
    return {};
  }

  DescriptorBatch output;
  output.columns.reserve(left.columns.size() + right.columns.size());
  output.columns.insert(output.columns.end(), left.columns.begin(), left.columns.end());
  output.columns.insert(output.columns.end(), right.columns.begin(), right.columns.end());

  std::multimap<std::string, const DescriptorTuple*> right_index;
  for (std::size_t right_row = 0; right_row < right.rows.size(); ++right_row) {
    const auto key = EqualityKeyForValue(right.rows[right_row].values[right_column],
                                         right_descriptor,
                                         right_row,
                                         right_column,
                                         diagnostic);
    if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
    if (key) { right_index.emplace(*key, &right.rows[right_row]); }
  }
  for (std::size_t left_row = 0; left_row < left.rows.size(); ++left_row) {
    const auto key = EqualityKeyForValue(left.rows[left_row].values[left_column],
                                         left_descriptor,
                                         left_row,
                                         left_column,
                                         diagnostic);
    if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
    if (!key) { continue; }
    const auto range = right_index.equal_range(*key);
    for (auto it = range.first; it != range.second; ++it) {
      DescriptorTuple joined;
      joined.values.reserve(left.rows[left_row].values.size() + it->second->values.size());
      joined.values.insert(joined.values.end(), left.rows[left_row].values.begin(), left.rows[left_row].values.end());
      joined.values.insert(joined.values.end(), it->second->values.begin(), it->second->values.end());
      output.rows.push_back(std::move(joined));
    }
  }

  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch AggregateDescriptorCountByInt64(const DescriptorBatch& input,
                                                std::size_t group_column,
                                                std::string count_stable_name,
                                                DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (group_column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "aggregate group column out of range", 0, group_column));
    return {};
  }
  if (!IsInt64Type(input.columns[group_column].descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_AGGREGATE_TYPE_UNSUPPORTED", input.columns[group_column].stable_name, 0, group_column));
    return {};
  }

  std::map<std::int64_t, std::int64_t> counts;
  for (std::size_t row = 0; row < input.rows.size(); ++row) {
    if (input.rows[row].values[group_column].is_null) { continue; }
    const auto decoded = DecodeInt64Value(input.rows[row].values[group_column]);
    if (!decoded.ok()) {
      auto diag = decoded.diagnostic;
      diag.row_index = row;
      diag.column_index = group_column;
      SetDiagnostic(diagnostic, std::move(diag));
      return {};
    }
    auto& count = counts[decoded.value];
    if (count == std::numeric_limits<std::int64_t>::max()) {
      SetDiagnostic(diagnostic, ErrorDiagnostic(
          "SB_EXECUTOR_NUMERIC_OVERFLOW", "aggregate count exceeds int64"));
      return {};
    }
    ++count;
  }

  DescriptorBatch output;
  output.columns = {input.columns[group_column], {std::move(count_stable_name), MakeExecutorDescriptor("int64"), false}};
  output.rows.reserve(counts.size());
  for (const auto& [group_value, count] : counts) {
    output.rows.push_back({{EncodeInt64Value(group_value), EncodeInt64Value(count)}});
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch AggregateDescriptorCountByKey(const DescriptorBatch& input,
                                              std::size_t group_column,
                                              std::string count_stable_name,
                                              DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (group_column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "aggregate group column out of range", 0, group_column));
    return {};
  }
  const auto& descriptor = input.columns[group_column].descriptor;
  if (IsOpaqueEncodedType(descriptor) ||
      (!IsInt64Type(descriptor) && !IsTextType(descriptor) &&
       !IsReal64Type(descriptor) && !IsBoolType(descriptor))) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_AGGREGATE_TYPE_UNSUPPORTED", input.columns[group_column].stable_name, 0, group_column));
    return {};
  }

  struct CountState {
    EngineTypedValue representative;
    std::int64_t count = 0;
  };
  std::map<std::string, CountState> counts;
  for (std::size_t row = 0; row < input.rows.size(); ++row) {
    const auto key = EqualityKeyForValue(input.rows[row].values[group_column],
                                         descriptor,
                                         row,
                                         group_column,
                                         diagnostic);
    if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
    if (!key) { continue; }
    auto& state = counts[*key];
    if (state.count == 0) { state.representative = input.rows[row].values[group_column]; }
    if (state.count == std::numeric_limits<std::int64_t>::max()) {
      SetDiagnostic(diagnostic, ErrorDiagnostic(
          "SB_EXECUTOR_NUMERIC_OVERFLOW", "aggregate count exceeds int64"));
      return {};
    }
    ++state.count;
  }

  DescriptorBatch output;
  output.columns = {input.columns[group_column], {std::move(count_stable_name), MakeExecutorDescriptor("int64"), false}};
  output.rows.reserve(counts.size());
  for (const auto& [key, state] : counts) {
    (void)key;
    output.rows.push_back({{state.representative, EncodeInt64Value(state.count)}});
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch WindowDescriptorRowNumberByInt64(const DescriptorBatch& input,
                                                 std::size_t order_column,
                                                 std::string row_number_stable_name,
                                                 bool ascending,
                                                 DescriptorRuntimeDiagnostic* diagnostic) {
  auto sorted = SortDescriptorBatchByColumn(input, order_column, ascending, diagnostic);
  if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
  if (sorted.rows.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    SetDiagnostic(diagnostic, ErrorDiagnostic(
        "SB_EXECUTOR_NUMERIC_OVERFLOW", "row number exceeds int64"));
    return {};
  }
  sorted.columns.push_back({std::move(row_number_stable_name), MakeExecutorDescriptor("int64"), false});
  for (std::size_t row = 0; row < sorted.rows.size(); ++row) {
    sorted.rows[row].values.push_back(EncodeInt64Value(static_cast<std::int64_t>(row + 1)));
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return sorted;
}

EngineTypedValue EvaluateDescriptorExpression(DescriptorExpressionOperator op,
                                              const EngineTypedValue& left,
                                              const EngineTypedValue& right,
                                              DescriptorRuntimeDiagnostic* diagnostic) {
  switch (op) {
    case DescriptorExpressionOperator::kInt64Add:
    case DescriptorExpressionOperator::kInt64Subtract:
    case DescriptorExpressionOperator::kInt64Multiply:
    case DescriptorExpressionOperator::kInt64Divide:
    case DescriptorExpressionOperator::kInt64Equal:
    case DescriptorExpressionOperator::kInt64GreaterThan: {
      const auto l = DecodeInt64Value(left);
      if (!l.ok()) {
        SetDiagnostic(diagnostic, l.diagnostic);
        return {};
      }
      const auto r = DecodeInt64Value(right);
      if (!r.ok()) {
        SetDiagnostic(diagnostic, r.diagnostic);
        return {};
      }
      switch (op) {
        case DescriptorExpressionOperator::kInt64Add: {
          const auto value = static_cast<__int128>(l.value) +
                             static_cast<__int128>(r.value);
          if (value < std::numeric_limits<std::int64_t>::min() ||
              value > std::numeric_limits<std::int64_t>::max()) {
            SetDiagnostic(diagnostic, ErrorDiagnostic(
                "SB_EXECUTOR_NUMERIC_OVERFLOW", "int64 add overflow"));
            return {};
          }
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeInt64Value(static_cast<std::int64_t>(value));
        }
        case DescriptorExpressionOperator::kInt64Subtract: {
          const auto value = static_cast<__int128>(l.value) -
                             static_cast<__int128>(r.value);
          if (value < std::numeric_limits<std::int64_t>::min() ||
              value > std::numeric_limits<std::int64_t>::max()) {
            SetDiagnostic(diagnostic, ErrorDiagnostic(
                "SB_EXECUTOR_NUMERIC_OVERFLOW",
                "int64 subtract overflow"));
            return {};
          }
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeInt64Value(static_cast<std::int64_t>(value));
        }
        case DescriptorExpressionOperator::kInt64Multiply: {
          const auto value = static_cast<__int128>(l.value) *
                             static_cast<__int128>(r.value);
          if (value < std::numeric_limits<std::int64_t>::min() ||
              value > std::numeric_limits<std::int64_t>::max()) {
            SetDiagnostic(diagnostic, ErrorDiagnostic(
                "SB_EXECUTOR_NUMERIC_OVERFLOW",
                "int64 multiply overflow"));
            return {};
          }
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeInt64Value(static_cast<std::int64_t>(value));
        }
        case DescriptorExpressionOperator::kInt64Divide:
          if (r.value == 0) {
            SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_DIVIDE_BY_ZERO", "int64 divide by zero"));
            return {};
          }
          if (l.value == std::numeric_limits<std::int64_t>::min() &&
              r.value == -1) {
            SetDiagnostic(diagnostic, ErrorDiagnostic(
                "SB_EXECUTOR_NUMERIC_OVERFLOW", "int64 divide overflow"));
            return {};
          }
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeInt64Value(l.value / r.value);
        case DescriptorExpressionOperator::kInt64Equal:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeBoolValue(l.value == r.value);
        case DescriptorExpressionOperator::kInt64GreaterThan:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeBoolValue(l.value > r.value);
        default:
          break;
      }
      break;
    }
    case DescriptorExpressionOperator::kBoolAnd:
    case DescriptorExpressionOperator::kBoolOr: {
      const auto l = DecodeBoolValue(left);
      if (!l.ok()) {
        SetDiagnostic(diagnostic, l.diagnostic);
        return {};
      }
      const auto r = DecodeBoolValue(right);
      if (!r.ok()) {
        SetDiagnostic(diagnostic, r.diagnostic);
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return EncodeBoolValue(op == DescriptorExpressionOperator::kBoolAnd ? (l.value && r.value) : (l.value || r.value));
    }
    case DescriptorExpressionOperator::kReal64Add:
    case DescriptorExpressionOperator::kReal64Subtract:
    case DescriptorExpressionOperator::kReal64Multiply:
    case DescriptorExpressionOperator::kReal64Divide:
    case DescriptorExpressionOperator::kReal64Equal:
    case DescriptorExpressionOperator::kReal64GreaterThan: {
      const auto l = DecodeReal64Value(left);
      if (!l.ok()) {
        SetDiagnostic(diagnostic, l.diagnostic);
        return {};
      }
      const auto r = DecodeReal64Value(right);
      if (!r.ok()) {
        SetDiagnostic(diagnostic, r.diagnostic);
        return {};
      }
      switch (op) {
        case DescriptorExpressionOperator::kReal64Add:
        case DescriptorExpressionOperator::kReal64Subtract:
        case DescriptorExpressionOperator::kReal64Multiply: {
          double value = 0.0;
          if (op == DescriptorExpressionOperator::kReal64Add) {
            value = l.value + r.value;
          } else if (op == DescriptorExpressionOperator::kReal64Subtract) {
            value = l.value - r.value;
          } else {
            value = l.value * r.value;
          }
          if (!std::isfinite(value)) {
            SetDiagnostic(diagnostic, ErrorDiagnostic(
                "SB_EXECUTOR_NUMERIC_OVERFLOW",
                "real64 arithmetic produced a non-finite value"));
            return {};
          }
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeReal64Value(value);
        }
        case DescriptorExpressionOperator::kReal64Divide:
          if (r.value == 0.0) {
            SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_DIVIDE_BY_ZERO", "real64 divide by zero"));
            return {};
          }
          if (!std::isfinite(l.value / r.value)) {
            SetDiagnostic(diagnostic, ErrorDiagnostic(
                "SB_EXECUTOR_NUMERIC_OVERFLOW",
                "real64 divide produced a non-finite value"));
            return {};
          }
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeReal64Value(l.value / r.value);
        case DescriptorExpressionOperator::kReal64Equal:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeBoolValue(l.value == r.value);
        case DescriptorExpressionOperator::kReal64GreaterThan:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeBoolValue(l.value > r.value);
        default:
          break;
      }
      break;
    }
    case DescriptorExpressionOperator::kTextConcat:
      if (left.state == EngineValueState::sql_null ||
          right.state == EngineValueState::sql_null) {
        const auto well_formed_null = [](const EngineTypedValue& value) {
          return value.state != EngineValueState::sql_null ||
                 (value.is_null && value.encoded_value.empty() &&
                  value.binary_value.empty());
        };
        if (!well_formed_null(left) || !well_formed_null(right)) {
          SetDiagnostic(diagnostic, ErrorDiagnostic(
              "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
              "text concatenation received malformed SQL NULL"));
          return {};
        }
        SetDiagnostic(diagnostic, OkDiagnostic());
        return MakeExecutorValue(MakeExecutorDescriptor("text"), {}, true);
      }
      if (left.state != EngineValueState::value ||
          right.state != EngineValueState::value || left.is_null ||
          right.is_null || !left.binary_value.empty() ||
          !right.binary_value.empty()) {
        SetDiagnostic(diagnostic, ErrorDiagnostic(
            "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
            "text concatenation received a non-value sentinel or auxiliary "
            "binary payload"));
        return {};
      }
      if (!IsTextType(left.descriptor) || !IsTextType(right.descriptor)) {
        SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH",
                                                  left.descriptor.canonical_type_name + "," + right.descriptor.canonical_type_name));
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return EncodeTextValue(left.encoded_value + right.encoded_value);
    case DescriptorExpressionOperator::kTextEqual:
      if (left.state == EngineValueState::sql_null ||
          right.state == EngineValueState::sql_null) {
        const auto well_formed_null = [](const EngineTypedValue& value) {
          return value.state != EngineValueState::sql_null ||
                 (value.is_null && value.encoded_value.empty() &&
                  value.binary_value.empty());
        };
        if (!well_formed_null(left) || !well_formed_null(right)) {
          SetDiagnostic(diagnostic, ErrorDiagnostic(
              "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
              "text equality received malformed SQL NULL"));
          return {};
        }
        SetDiagnostic(diagnostic, OkDiagnostic());
        return MakeExecutorValue(MakeExecutorDescriptor("boolean"), {}, true);
      }
      if (left.state != EngineValueState::value ||
          right.state != EngineValueState::value || left.is_null ||
          right.is_null || !left.binary_value.empty() ||
          !right.binary_value.empty()) {
        SetDiagnostic(diagnostic, ErrorDiagnostic(
            "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
            "text equality received a non-value sentinel or auxiliary binary "
            "payload"));
        return {};
      }
      if (!IsTextType(left.descriptor) || !IsTextType(right.descriptor)) {
        SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH",
                                                  left.descriptor.canonical_type_name + "," + right.descriptor.canonical_type_name));
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return EncodeBoolValue(left.encoded_value == right.encoded_value);
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXPRESSION_UNSUPPORTED", "descriptor expression operator unsupported"));
  return {};
}

EngineTypedValue EvaluateDescriptorCoalesce(const std::vector<EngineTypedValue>& values,
                                            DescriptorRuntimeDiagnostic* diagnostic) {
  if (values.empty()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_SPECIAL_FORM_ARGUMENT_REQUIRED", "coalesce requires at least one argument"));
    return {};
  }
  const auto& descriptor = values.front().descriptor;
  for (const auto& value : values) {
    if (!DescriptorMatches(descriptor, value.descriptor)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic(
          "SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH",
          "coalesce arguments do not share one bound descriptor"));
      return {};
    }
    if (value.state == EngineValueState::sql_null) {
      if (!value.is_null || !value.encoded_value.empty() ||
          !value.binary_value.empty()) {
        SetDiagnostic(diagnostic, ErrorDiagnostic(
            "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
            "coalesce received malformed SQL NULL"));
        return {};
      }
      continue;
    }
    if (value.state != EngineValueState::value || value.is_null) {
      SetDiagnostic(diagnostic, ErrorDiagnostic(
          "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
          "coalesce received a non-value sentinel"));
      return {};
    }
    {
      SetDiagnostic(diagnostic, OkDiagnostic());
      return value;
    }
  }
  EngineTypedValue null_value = values.front();
  null_value.is_null = true;
  null_value.state = EngineValueState::sql_null;
  null_value.encoded_value.clear();
  null_value.binary_value.clear();
  SetDiagnostic(diagnostic, OkDiagnostic());
  return null_value;
}

EngineTypedValue CastDescriptorValue(const EngineTypedValue& value,
                                     const EngineDescriptor& target_descriptor,
                                     DescriptorRuntimeDiagnostic* diagnostic) {
  if (!IsKnownScalarType(target_descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_TARGET_UNSUPPORTED", target_descriptor.canonical_type_name));
    return {};
  }
  if (!IsKnownScalarType(value.descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic(
        "SB_EXECUTOR_CAST_SOURCE_UNSUPPORTED",
        value.descriptor.canonical_type_name));
    return {};
  }
  DescriptorBatch source_batch;
  source_batch.columns = {{"cast_source", value.descriptor, true}};
  source_batch.rows = {{{value}}};
  const auto source_validation = ValidateDescriptorBatch(source_batch);
  if (!source_validation.ok) {
    SetDiagnostic(diagnostic, source_validation);
    return {};
  }
  if (value.state == EngineValueState::sql_null) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, {}, true);
  }
  if (DescriptorMatches(target_descriptor, value.descriptor)) {
    auto output = value;
    output.descriptor = target_descriptor;
    SetDiagnostic(diagnostic, OkDiagnostic());
    return output;
  }
  if (IsInt64Type(target_descriptor) && IsInt64Type(value.descriptor)) {
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      SetDiagnostic(diagnostic, decoded.diagnostic);
      return {};
    }
    std::int64_t target_value = 0;
    if (!ParseBoundedSignedIntegerStrict(target_descriptor,
                                         std::to_string(decoded.value),
                                         &target_value)) {
      SetDiagnostic(diagnostic,
                    ErrorDiagnostic("SB_EXECUTOR_CAST_FAILED",
                                    value.encoded_value));
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, std::to_string(target_value), false);
  }
  if (IsBoolType(target_descriptor) && IsBoolType(value.descriptor)) {
    const auto decoded = DecodeBoolValue(value);
    if (!decoded.ok()) {
      SetDiagnostic(diagnostic, decoded.diagnostic);
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, decoded.value ? "true" : "false", false);
  }
  if (IsReal64Type(target_descriptor) && IsReal64Type(value.descriptor)) {
    const auto decoded = DecodeReal64Value(value);
    if (!decoded.ok()) {
      SetDiagnostic(diagnostic, decoded.diagnostic);
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, FormatReal64(decoded.value), false);
  }
  if (IsTextType(target_descriptor)) {
    if (IsInt64Type(value.descriptor)) {
      const auto decoded = DecodeInt64Value(value);
      if (!decoded.ok()) {
        SetDiagnostic(diagnostic, decoded.diagnostic);
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, std::to_string(decoded.value), false);
    }
    if (IsBoolType(value.descriptor)) {
      const auto decoded = DecodeBoolValue(value);
      if (!decoded.ok()) {
        SetDiagnostic(diagnostic, decoded.diagnostic);
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, decoded.value ? "true" : "false", false);
    }
    if (IsReal64Type(value.descriptor)) {
      const auto decoded = DecodeReal64Value(value);
      if (!decoded.ok()) {
        SetDiagnostic(diagnostic, decoded.diagnostic);
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, FormatReal64(decoded.value), false);
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, value.encoded_value, false);
  }
  if (IsOpaqueEncodedType(target_descriptor) && IsTextType(value.descriptor)) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, value.encoded_value, false);
  }
  if (IsTextType(target_descriptor) && IsOpaqueEncodedType(value.descriptor)) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, value.encoded_value, false);
  }
  if (IsInt64Type(target_descriptor) && IsTextType(value.descriptor)) {
    std::int64_t parsed = 0;
    if (!ParseBoundedSignedIntegerStrict(target_descriptor,
                                         value.encoded_value,
                                         &parsed)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_FAILED", value.encoded_value));
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, std::to_string(parsed), false);
  }
  if (IsReal64Type(target_descriptor) && IsInt64Type(value.descriptor)) {
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      SetDiagnostic(diagnostic, decoded.diagnostic);
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, FormatReal64(static_cast<double>(decoded.value)), false);
  }
  if (IsReal64Type(target_descriptor) && IsTextType(value.descriptor)) {
    double parsed = 0.0;
    if (!ParseReal64Strict(value.encoded_value, &parsed)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_FAILED", value.encoded_value));
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, FormatReal64(parsed), false);
  }
  if (IsBoolType(target_descriptor) && IsTextType(value.descriptor)) {
    const std::string text = LowerAscii(value.encoded_value);
    if (text == "true" || text == "1") {
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, "true", false);
    }
    if (text == "false" || text == "0") {
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, "false", false);
    }
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_FAILED", value.encoded_value));
    return {};
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_UNSUPPORTED", value.descriptor.canonical_type_name + "->" + target_descriptor.canonical_type_name));
  return {};
}

EngineTypedValue ExtractDescriptorField(const EngineTypedValue& value,
                                        const std::string& field_name,
                                        DescriptorRuntimeDiagnostic* diagnostic) {
  DescriptorBatch source_batch;
  source_batch.columns = {{"extract_source", value.descriptor, true}};
  source_batch.rows = {{{value}}};
  const auto source_validation = ValidateDescriptorBatch(source_batch);
  if (!source_validation.ok) {
    SetDiagnostic(diagnostic, source_validation);
    return {};
  }
  if (value.state == EngineValueState::sql_null) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(MakeExecutorDescriptor("int64"), {}, true);
  }
  if (!IsTextType(value.descriptor)) {
    if (IsBinaryType(value.descriptor)) {
      const std::string field = LowerAscii(field_name);
      if (field == "octet_length" || field == "length") {
        const auto byte_count = value.binary_value.empty()
                                    ? value.encoded_value.size()
                                    : value.binary_value.size();
        SetDiagnostic(diagnostic, OkDiagnostic());
        return MakeExecutorValue(MakeExecutorDescriptor("uint64"),
                                 std::to_string(byte_count), false);
      }
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_FIELD_UNSUPPORTED", field_name));
      return {};
    }
    if (IsUuidType(value.descriptor)) {
      const std::string field = LowerAscii(field_name);
      if (field == "version" && value.encoded_value.size() == 36) {
        SetDiagnostic(diagnostic, OkDiagnostic());
        return MakeExecutorValue(MakeExecutorDescriptor("uint8"), std::string(1, value.encoded_value[14]), false);
      }
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_FIELD_UNSUPPORTED", field_name));
      return {};
    }
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_TYPE_UNSUPPORTED", value.descriptor.canonical_type_name));
    return {};
  }
  const std::string field = LowerAscii(field_name);
  if (field == "character_length" || field == "length" || field == "octet_length") {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(MakeExecutorDescriptor("uint64"), std::to_string(value.encoded_value.size()), false);
  }
  std::size_t offset = 0;
  std::size_t width = 0;
  if (field == "year") {
    offset = 0;
    width = 4;
  } else if (field == "month") {
    offset = 5;
    width = 2;
  } else if (field == "day") {
    offset = 8;
    width = 2;
  } else if (field == "hour") {
    offset = 11;
    width = 2;
  } else if (field == "minute") {
    offset = 14;
    width = 2;
  } else if (field == "second") {
    offset = 17;
    width = 2;
  } else {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_FIELD_UNSUPPORTED", field_name));
    return {};
  }
  std::int64_t parsed = 0;
  if (!ParseFixedWidthNumber(value.encoded_value, offset, width, &parsed)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_FAILED", value.encoded_value));
    return {};
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return EncodeInt64Value(parsed);
}

void SetDescriptorRuntimeVariable(DescriptorRuntimeSetScope* scope,
                                  std::string stable_name,
                                  EngineTypedValue value) {
  if (scope == nullptr) { return; }
  for (auto& variable : scope->variables) {
    if (variable.stable_name == stable_name) {
      variable.value = std::move(value);
      return;
    }
  }
  scope->variables.push_back({std::move(stable_name), std::move(value)});
}

std::optional<EngineTypedValue> GetDescriptorRuntimeVariable(const DescriptorRuntimeSetScope& scope,
                                                             const std::string& stable_name) {
  for (const auto& variable : scope.variables) {
    if (variable.stable_name == stable_name) { return variable.value; }
  }
  return std::nullopt;
}

DescriptorRuntimeDiagnostic ValidateDescriptorDomainValue(const DescriptorDomainPolicy& policy,
                                                          const EngineTypedValue& value) {
  if (policy.domain_stable_name.empty()) {
    return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_POLICY_INVALID", "domain stable name is required");
  }
  if (policy.base_descriptor.canonical_type_name.empty() || policy.base_descriptor.descriptor_kind.empty()) {
    return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_POLICY_INVALID", "domain base descriptor is required");
  }
  if (!DescriptorMatches(policy.base_descriptor, value.descriptor)) {
    return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_DESCRIPTOR_MISMATCH", policy.domain_stable_name);
  }
  DescriptorBatch value_batch;
  value_batch.columns = {
      {policy.domain_stable_name, policy.base_descriptor, policy.nullable}};
  value_batch.rows = {{{value}}};
  const auto value_validation = ValidateDescriptorBatch(value_batch);
  if (!value_validation.ok) return value_validation;
  if (value.state == EngineValueState::sql_null) return OkDiagnostic();
  if (IsInt64Type(policy.base_descriptor)) {
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) { return decoded.diagnostic; }
    if (policy.min_int64.has_value() && decoded.value < *policy.min_int64) {
      return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_MIN_VIOLATION", policy.domain_stable_name);
    }
    if (policy.max_int64.has_value() && decoded.value > *policy.max_int64) {
      return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_MAX_VIOLATION", policy.domain_stable_name);
    }
  }
  if (IsTextType(policy.base_descriptor) && policy.max_text_bytes.has_value() &&
      value.encoded_value.size() > *policy.max_text_bytes) {
    return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_TEXT_LENGTH_VIOLATION", policy.domain_stable_name);
  }
  return OkDiagnostic();
}

EngineTypedValue ApplyDescriptorDomainMask(const DescriptorDomainPolicy& policy,
                                           const EngineTypedValue& value,
                                           const std::string& security_token,
                                           DescriptorRuntimeDiagnostic* diagnostic) {
  const auto validation = ValidateDescriptorDomainValue(policy, value);
  if (!validation.ok) {
    SetDiagnostic(diagnostic, validation);
    return {};
  }
  if (policy.required_security_token.empty() || policy.required_security_token == security_token ||
      policy.mask_kind == DescriptorDomainMaskKind::kNone) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return value;
  }
  if (policy.mask_kind == DescriptorDomainMaskKind::kNull) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(policy.base_descriptor, {}, true);
  }
  if (policy.mask_kind == DescriptorDomainMaskKind::kFixedText) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(policy.base_descriptor, policy.fixed_mask_text, false);
  }
  if (policy.mask_kind == DescriptorDomainMaskKind::kRevealLast4) {
    std::string masked = value.encoded_value;
    if (masked.size() <= 4) {
      masked.assign(masked.size(), '*');
    } else {
      masked.replace(0, masked.size() - 4, masked.size() - 4, '*');
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(policy.base_descriptor, std::move(masked), false);
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_DOMAIN_MASK_UNSUPPORTED", policy.domain_stable_name));
  return {};
}

EngineTypedValue EvaluateDescriptorDomainMethod(const DescriptorDomainPolicy& policy,
                                                const std::string& method_name,
                                                const EngineTypedValue& value,
                                                const std::string& security_token,
                                                DescriptorRuntimeDiagnostic* diagnostic) {
  const std::string method = LowerAscii(method_name);
  if (method == "validate") {
    const auto validation = ValidateDescriptorDomainValue(policy, value);
    SetDiagnostic(diagnostic, OkDiagnostic());
    return EncodeBoolValue(validation.ok);
  }
  if (method == "mask") {
    return ApplyDescriptorDomainMask(policy, value, security_token, diagnostic);
  }
  if (method == "is_visible") {
    const auto validation = ValidateDescriptorDomainValue(policy, value);
    if (!validation.ok) {
      SetDiagnostic(diagnostic, validation);
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return EncodeBoolValue(policy.required_security_token.empty() || policy.required_security_token == security_token);
  }
  if (method == "base_type_name") {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return EncodeTextValue(policy.base_descriptor.canonical_type_name);
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_DOMAIN_METHOD_UNKNOWN", method_name));
  return {};
}

Int64DecodeResult DecodeInt64Value(const EngineTypedValue& value) {
  Int64DecodeResult result;
  if (value.state ==
      scratchbird::engine::internal_api::EngineValueState::sql_null) {
    if (!value.is_null || !value.encoded_value.empty() ||
        !value.binary_value.empty()) {
      result.diagnostic = ErrorDiagnostic(
          "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
          "int64 SQL NULL state is malformed");
      return result;
    }
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_NULL_VALUE", "int64 decode received NULL");
    return result;
  }
  if (value.state !=
          scratchbird::engine::internal_api::EngineValueState::value ||
      value.is_null ||
      (value.encoded_value.empty() == value.binary_value.empty())) {
    result.diagnostic = ErrorDiagnostic(
        "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
        "int64 decode received a non-value sentinel or ambiguous payload");
    return result;
  }
  if (!IsInt64Type(value.descriptor)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", value.descriptor.canonical_type_name);
    return result;
  }
  if (!value.binary_value.empty()) {
    if (CanonicalDescriptorTypeId(value.descriptor) !=
            CanonicalTypeId::int64 ||
        value.binary_value.size() != sizeof(std::int64_t)) {
      result.diagnostic = ErrorDiagnostic(
          "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
          "binary int64 payload is not the exact canonical width");
      return result;
    }
    std::uint64_t encoded = 0;
    for (std::size_t index = 0; index < value.binary_value.size(); ++index) {
      encoded |= static_cast<std::uint64_t>(value.binary_value[index])
                 << (index * 8U);
    }
    if ((encoded & (std::uint64_t{1} << 63U)) == 0) {
      result.value = static_cast<std::int64_t>(encoded);
    } else {
      result.value =
          -1 - static_cast<std::int64_t>(~encoded);
    }
    result.diagnostic = OkDiagnostic();
    return result;
  }
  if (!ParseBoundedSignedIntegerStrict(value.descriptor,
                                       value.encoded_value,
                                       &result.value)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_INT64_DECODE_FAILED", value.encoded_value);
    return result;
  }
  result.diagnostic = OkDiagnostic();
  return result;
}

BoolDecodeResult DecodeBoolValue(const EngineTypedValue& value) {
  BoolDecodeResult result;
  if (value.state ==
      scratchbird::engine::internal_api::EngineValueState::sql_null) {
    if (!value.is_null || !value.encoded_value.empty() ||
        !value.binary_value.empty()) {
      result.diagnostic = ErrorDiagnostic(
          "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
          "boolean SQL NULL state is malformed");
      return result;
    }
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_NULL_VALUE", "bool decode received NULL");
    return result;
  }
  if (value.state !=
          scratchbird::engine::internal_api::EngineValueState::value ||
      value.is_null || !value.binary_value.empty()) {
    result.diagnostic = ErrorDiagnostic(
        "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
        "boolean decode received a non-value sentinel or auxiliary binary "
        "payload");
    return result;
  }
  if (!IsBoolType(value.descriptor)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", value.descriptor.canonical_type_name);
    return result;
  }
  const std::string text = LowerAscii(value.encoded_value);
  if (text == "true" || text == "1") {
    result.value = true;
  } else if (text == "false" || text == "0") {
    result.value = false;
  } else {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_BOOL_DECODE_FAILED", value.encoded_value);
    return result;
  }
  result.diagnostic = OkDiagnostic();
  return result;
}

Real64DecodeResult DecodeReal64Value(const EngineTypedValue& value) {
  Real64DecodeResult result;
  if (value.state ==
      scratchbird::engine::internal_api::EngineValueState::sql_null) {
    if (!value.is_null || !value.encoded_value.empty() ||
        !value.binary_value.empty()) {
      result.diagnostic = ErrorDiagnostic(
          "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
          "real64 SQL NULL state is malformed");
      return result;
    }
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_NULL_VALUE", "real64 decode received NULL");
    return result;
  }
  if (value.state !=
          scratchbird::engine::internal_api::EngineValueState::value ||
      value.is_null || !value.binary_value.empty()) {
    result.diagnostic = ErrorDiagnostic(
        "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
        "real64 decode received a non-value sentinel or auxiliary binary "
        "payload");
    return result;
  }
  if (!IsReal64Type(value.descriptor)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", value.descriptor.canonical_type_name);
    return result;
  }
  if (!ParseReal64Strict(value.encoded_value, &result.value)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_REAL64_DECODE_FAILED", value.encoded_value);
    return result;
  }
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineTypedValue EncodeInt64Value(std::int64_t value) {
  return MakeExecutorValue(MakeExecutorDescriptor("int64"), std::to_string(value), false);
}

EngineTypedValue EncodeBoolValue(bool value) {
  return MakeExecutorValue(MakeExecutorDescriptor("boolean"), value ? "true" : "false", false);
}

EngineTypedValue EncodeReal64Value(double value) {
  return MakeExecutorValue(MakeExecutorDescriptor("real64"), FormatReal64(value), false);
}

EngineTypedValue EncodeTextValue(std::string value) {
  return MakeExecutorValue(MakeExecutorDescriptor("text"), std::move(value), false);
}

}  // namespace scratchbird::engine::executor
