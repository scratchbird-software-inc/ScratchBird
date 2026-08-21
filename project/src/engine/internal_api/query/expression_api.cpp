// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/expression_api.hpp"

#include "datatype_operations.hpp"
#include "datatype_temporal_wire.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

#if !defined(SCRATCHBIRD_QOW_TYPED_SCALAR_DESCRIPTOR_CONTRACT_ONLY) && \
    !defined(SCRATCHBIRD_TYPED_SCALAR_DESCRIPTOR_CONTRACT_ONLY)
#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_advanced_family.hpp"
#include "datatype_document.hpp"
#include "domain_support/domain_store.hpp"
#include "security/security_model.hpp"

#include <sstream>
#endif

#ifndef SCRATCHBIRD_TYPED_SCALAR_DESCRIPTOR_CONTRACT_EXTERNAL

namespace scratchbird::engine::internal_api {
namespace {

template <typename Real>
bool QowApplyCanonicalBoundedRealV1(
    const std::string& left_encoded,
    const std::string& right_encoded,
    const scratchbird::core::datatypes::DatatypeNumericOperationKind operation,
    const bool allow_special_values,
    std::string* computed_encoded,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (computed_encoded == nullptr || refusal_detail == nullptr) return false;
  const auto parse = [&](const std::string& encoded, Real* value) {
    if (value == nullptr || encoded.empty()) return false;
    const auto [end, error] = std::from_chars(
        encoded.data(), encoded.data() + encoded.size(), *value,
        std::chars_format::general);
    return error == std::errc{} && end == encoded.data() + encoded.size() &&
           (allow_special_values || std::isfinite(*value));
  };
  Real left = 0;
  Real right = 0;
  if (!parse(left_encoded, &left) || !parse(right_encoded, &right)) {
    *refusal_detail = "canonical bounded-real operand encoding is invalid";
    return false;
  }
  Real computed = 0;
  switch (operation) {
    case dt::DatatypeNumericOperationKind::add:
      computed = left + right;
      break;
    case dt::DatatypeNumericOperationKind::subtract:
      computed = left - right;
      break;
    case dt::DatatypeNumericOperationKind::multiply:
      computed = left * right;
      break;
    case dt::DatatypeNumericOperationKind::divide:
      if (right == static_cast<Real>(0)) {
        *refusal_detail = "canonical bounded-real division by zero";
        return false;
      }
      computed = left / right;
      break;
    default:
      *refusal_detail = "canonical bounded-real numeric operation is unsupported";
      return false;
  }
  if (!allow_special_values && !std::isfinite(computed)) {
    *refusal_detail = "canonical bounded-real arithmetic overflow";
    return false;
  }
  std::array<char, 128> buffer{};
  const auto [end, error] = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(), computed,
      std::chars_format::general, std::numeric_limits<Real>::max_digits10);
  if (error != std::errc{}) {
    *refusal_detail = "canonical bounded-real result encoding failed";
    return false;
  }
  computed_encoded->assign(buffer.data(), end);
  return true;
}

bool QowCanonicalUuidV1(const std::string_view value) {
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

std::string QowCanonicalDescriptorFieldV1(const std::string& descriptor,
                                          const std::string& key) {
  const std::string prefix = key + "=";
  std::string value;
  bool found = false;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const std::size_t end = descriptor.find(';', start);
    const auto field = descriptor.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (field.rfind(prefix, 0) == 0) {
      if (found) return {};
      value = field.substr(prefix.size());
      found = true;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return value;
}

const char* QowCanonicalExpressionDiagnosticIdV1(
    const EngineCanonicalExpressionOperation operation) noexcept {
  switch (operation) {
    case EngineCanonicalExpressionOperation::identity:
      return "QOW-DIAG-RCP024-IDENTITY-REFUSAL-V1";
    case EngineCanonicalExpressionOperation::consume_truth:
      return "QOW-DIAG-RCP024-CONSUMER-TRUTH-REFUSAL-V1";
    case EngineCanonicalExpressionOperation::numeric_add:
    case EngineCanonicalExpressionOperation::numeric_subtract:
    case EngineCanonicalExpressionOperation::numeric_multiply:
    case EngineCanonicalExpressionOperation::numeric_divide:
    case EngineCanonicalExpressionOperation::numeric_modulo:
      return "QOW-DIAG-RCP024-NUMERIC-REFUSAL-V1";
    case EngineCanonicalExpressionOperation::text_concat:
    case EngineCanonicalExpressionOperation::like:
    case EngineCanonicalExpressionOperation::ilike:
      return "QOW-DIAG-RCP024-TEXT-REFUSAL-V1";
    case EngineCanonicalExpressionOperation::equal:
    case EngineCanonicalExpressionOperation::not_equal:
    case EngineCanonicalExpressionOperation::less_than:
    case EngineCanonicalExpressionOperation::less_than_or_equal:
    case EngineCanonicalExpressionOperation::greater_than:
    case EngineCanonicalExpressionOperation::greater_than_or_equal:
    case EngineCanonicalExpressionOperation::is_distinct_from:
    case EngineCanonicalExpressionOperation::is_not_distinct_from:
      return "QOW-DIAG-RCP024-COMPARISON-REFUSAL-V1";
    case EngineCanonicalExpressionOperation::is_null:
    case EngineCanonicalExpressionOperation::is_not_null:
    case EngineCanonicalExpressionOperation::logical_not:
    case EngineCanonicalExpressionOperation::logical_and:
    case EngineCanonicalExpressionOperation::logical_or:
    case EngineCanonicalExpressionOperation::logical_xor:
      return "QOW-DIAG-RCP024-3VL-REFUSAL-V1";
    case EngineCanonicalExpressionOperation::explicit_cast:
    case EngineCanonicalExpressionOperation::implicit_cast:
      return "QOW-DIAG-RCP024-CAST-REFUSAL-V1";
    case EngineCanonicalExpressionOperation::scalar_function:
      return "QOW-DIAG-RCP024-FUNCTION-REFUSAL-V1";
    case EngineCanonicalExpressionOperation::unspecified:
      return "QOW-DIAG-RCP024-OPERATION-UNBOUND-V1";
  }
  return "QOW-DIAG-RCP024-OPERATION-INVALID-V1";
}

char QowFoldLikeAsciiV1(const char value, const bool case_insensitive) {
  return case_insensitive
             ? static_cast<char>(
                   std::tolower(static_cast<unsigned char>(value)))
             : value;
}

struct QowLikeMatchResultV1 {
  bool valid = false;
  bool matched = false;
};

QowLikeMatchResultV1 QowLikeMatchV1(const std::string_view value,
                                    const std::string_view pattern,
                                    const bool case_insensitive) {
  QowLikeMatchResultV1 result;
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    if (pattern[index] != '\\') continue;
    if (index + 1 >= pattern.size()) return result;
    ++index;
  }
  std::size_t value_index = 0;
  std::size_t pattern_index = 0;
  std::size_t star_pattern = std::string_view::npos;
  std::size_t star_value = std::string_view::npos;
  while (value_index < value.size()) {
    if (pattern_index < pattern.size() && pattern[pattern_index] == '\\') {
      if (pattern_index + 1 >= pattern.size()) return result;
      if (QowFoldLikeAsciiV1(value[value_index], case_insensitive) ==
          QowFoldLikeAsciiV1(pattern[pattern_index + 1], case_insensitive)) {
        pattern_index += 2;
        ++value_index;
        continue;
      }
    } else if (pattern_index < pattern.size() &&
               pattern[pattern_index] == '_') {
      ++pattern_index;
      ++value_index;
      continue;
    } else if (pattern_index < pattern.size() &&
               pattern[pattern_index] == '%') {
      star_pattern = pattern_index++;
      star_value = value_index;
      continue;
    } else if (pattern_index < pattern.size() &&
               QowFoldLikeAsciiV1(value[value_index], case_insensitive) ==
                   QowFoldLikeAsciiV1(pattern[pattern_index],
                                      case_insensitive)) {
      ++pattern_index;
      ++value_index;
      continue;
    }
    if (star_pattern == std::string_view::npos) {
      result.valid = true;
      return result;
    }
    pattern_index = star_pattern + 1;
    value_index = ++star_value;
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '%') {
    ++pattern_index;
  }
  if (pattern_index < pattern.size() && pattern[pattern_index] == '\\' &&
      pattern_index + 1 >= pattern.size()) {
    return result;
  }
  result.valid = true;
  result.matched = pattern_index == pattern.size();
  return result;
}

}  // namespace

bool QowCanonicalDescriptorU32FieldV1(const std::string& descriptor,
                                      const std::string& key,
                                      std::uint32_t* value) {
  if (value == nullptr) return false;
  const std::string field = QowCanonicalDescriptorFieldV1(descriptor, key);
  if (field.empty() || (field.size() > 1 && field.front() == '0')) return false;
  std::uint64_t parsed = 0;
  for (const char ch : field) {
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10u + static_cast<unsigned>(ch - '0');
    if (parsed > std::numeric_limits<std::uint32_t>::max()) return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

// QOW-SOURCE-QRY-008-DESC-V1
bool QowCanonicalDescriptorIdentityV1(const EngineDescriptor& descriptor) {
  return QowCanonicalUuidV1(descriptor.descriptor_uuid.canonical) &&
         !descriptor.descriptor_kind.empty() &&
         !descriptor.canonical_type_name.empty() &&
         !descriptor.encoded_descriptor.empty();
}

scratchbird::core::datatypes::CanonicalTypeId
QowCanonicalTypeFromDescriptorV1(const EngineDescriptor& descriptor) {
  namespace dt = scratchbird::core::datatypes;
  if (descriptor.descriptor_kind == "domain") {
    const auto base_type = dt::CanonicalTypeIdFromStableName(
        QowCanonicalDescriptorFieldV1(descriptor.encoded_descriptor,
                                      "base_type"));
    if (base_type != dt::CanonicalTypeId::unknown) return base_type;
  }
  return dt::CanonicalTypeIdFromStableName(descriptor.canonical_type_name);
}

EngineTypedValue QowPreserveCanonicalDescriptorAfterScalarV1(
    const EngineDescriptor& result_descriptor,
    EngineTypedValue computed_value) {
  if (QowCanonicalDescriptorIdentityV1(result_descriptor) &&
      result_descriptor.canonical_type_name ==
          computed_value.descriptor.canonical_type_name) {
    computed_value.descriptor = result_descriptor;
  }
  return computed_value;
}

// QOW-SOURCE-QRY-008-NULL-V1
bool QowCanonicalSqlNullStateV1(const EngineTypedValue& value) {
  return value.state == EngineValueState::sql_null &&
         value.encoded_value.empty() &&
         value.binary_value.empty();
}

EngineTypedValue QowPropagateSqlNullAfterScalarV1(
    const EngineDescriptor& result_descriptor,
    EngineTypedValue computed_value) {
  if (!computed_value.isSqlNull()) return computed_value;
  computed_value.descriptor = result_descriptor;
  computed_value.encoded_value.clear();
  computed_value.binary_value.clear();
  computed_value.is_null = true;
  computed_value.state = EngineValueState::sql_null;
  return computed_value;
}

// QOW-SOURCE-QRY-008-COERCE-V1
bool QowApplyCanonicalDescriptorCoercionV1(
    const EngineTypedValue& input_value,
    const EngineDescriptor& target_descriptor,
    const bool explicit_cast,
    EngineTypedValue* output_value,
    std::string* cast_category,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (output_value == nullptr || cast_category == nullptr ||
      refusal_detail == nullptr) {
    return false;
  }
  *output_value = EngineTypedValue{};
  output_value->state = EngineValueState::error;
  cast_category->clear();
  refusal_detail->clear();
  if (!QowCanonicalDescriptorIdentityV1(input_value.descriptor) ||
      !QowCanonicalDescriptorIdentityV1(target_descriptor) ||
      input_value.descriptor.descriptor_kind != "scalar" ||
      target_descriptor.descriptor_kind != "scalar" ||
      (input_value.isSqlNull() &&
       !QowCanonicalSqlNullStateV1(input_value))) {
    *refusal_detail = "canonical coercion descriptors or value state are invalid";
    return false;
  }
  const auto source_type = dt::CanonicalTypeIdFromStableName(
      input_value.descriptor.canonical_type_name);
  const auto target_type = dt::CanonicalTypeIdFromStableName(
      target_descriptor.canonical_type_name);
  if (source_type == dt::CanonicalTypeId::unknown ||
      target_type == dt::CanonicalTypeId::unknown) {
    *refusal_detail = "canonical coercion type is unknown";
    return false;
  }
  dt::DatatypeCastRequest request;
  request.value.type_id = source_type;
  request.value.encoded_value = input_value.encoded_value;
  request.value.is_null = input_value.isSqlNull();
  request.target_type_id = target_type;
  request.explicit_cast = explicit_cast;
  const auto cast = dt::CastDatatypeValue(request);
  if (!cast.ok()) {
    *refusal_detail = cast.diagnostic.diagnostic_code.empty()
                          ? "canonical descriptor coercion refused"
                          : cast.diagnostic.diagnostic_code;
    return false;
  }
  output_value->descriptor = target_descriptor;
  output_value->encoded_value = cast.value.encoded_value;
  output_value->binary_value.clear();
  output_value->is_null = cast.value.is_null;
  output_value->state = cast.value.is_null ? EngineValueState::sql_null
                                           : EngineValueState::value;
  *cast_category = dt::DatatypeCastCategoryName(cast.category);
  return true;
}

// QOW-SOURCE-QRY-028-V1
bool QowPreserveInvalidDescriptorStateAndCoerceV1(
    const EngineTypedValue& input_value,
    const EngineDescriptor& target_descriptor,
    const bool explicit_cast,
    EngineTypedValue* output_value,
    std::string* cast_category,
    std::string* refusal_reason,
    std::string* refusal_detail) {
  if (output_value == nullptr || cast_category == nullptr ||
      refusal_reason == nullptr || refusal_detail == nullptr) {
    return false;
  }
  *output_value = input_value;
  cast_category->clear();
  refusal_reason->clear();
  refusal_detail->clear();
  const auto refuse = [&](std::string reason, std::string detail) {
    *output_value = input_value;
    cast_category->clear();
    *refusal_reason = std::move(reason);
    *refusal_detail = std::move(detail);
    return false;
  };

  const bool canonical_value_state =
      (input_value.state == EngineValueState::value && !input_value.is_null) ||
      (input_value.state == EngineValueState::sql_null && input_value.is_null &&
       input_value.encoded_value.empty() && input_value.binary_value.empty());
  if (!canonical_value_state) {
    return refuse("invalid_descriptor_state",
                  "descriptor coercion cannot replace an invalid value state");
  }
  if (!QowCanonicalDescriptorIdentityV1(input_value.descriptor) ||
      !QowCanonicalDescriptorIdentityV1(target_descriptor) ||
      input_value.descriptor.descriptor_kind != "scalar" ||
      target_descriptor.descriptor_kind != "scalar") {
    return refuse("descriptor_invalid",
                  "descriptor coercion requires canonical scalar descriptors");
  }
  if (input_value.state == EngineValueState::sql_null &&
      QowCanonicalDescriptorFieldV1(
          target_descriptor.encoded_descriptor, "nullability") ==
          "non_null") {
    return refuse("unsupported_descriptor_coercion",
                  "SQL NULL cannot be coerced to a non-NULL descriptor");
  }

  EngineTypedValue coerced;
  std::string category;
  std::string detail;
  if (!QowApplyCanonicalDescriptorCoercionV1(
          input_value, target_descriptor, explicit_cast, &coerced, &category,
          &detail)) {
    return refuse("unsupported_descriptor_coercion",
                  detail.empty() ? "descriptor coercion is unsupported"
                                 : std::move(detail));
  }
  *output_value = std::move(coerced);
  *cast_category = std::move(category);
  return true;
}

// QOW-SOURCE-QRY-008-COLLATION-V1
bool QowCompareCanonicalCollatedScalarsV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    const std::string& collation_uuid,
    const EngineApiU64 resource_epoch,
    const EngineApiU64 collation_epoch,
    const scratchbird::core::datatypes::DatatypeTextSeedAuthority& text_seed,
    int* comparison,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (comparison == nullptr || refusal_detail == nullptr) return false;
  *comparison = 0;
  refusal_detail->clear();
  const std::string left_collation = QowCanonicalDescriptorFieldV1(
      left_value.descriptor.encoded_descriptor, "collation_uuid");
  const std::string right_collation = QowCanonicalDescriptorFieldV1(
      right_value.descriptor.encoded_descriptor, "collation_uuid");
  if (!QowCanonicalDescriptorIdentityV1(left_value.descriptor) ||
      !QowCanonicalDescriptorIdentityV1(right_value.descriptor) ||
      left_value.descriptor.descriptor_kind != "scalar" ||
      right_value.descriptor.descriptor_kind != "scalar" ||
      dt::CanonicalTypeIdFromStableName(
          left_value.descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::character ||
      dt::CanonicalTypeIdFromStableName(
          right_value.descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::character ||
      !QowCanonicalUuidV1(collation_uuid) || left_collation != collation_uuid ||
      right_collation != collation_uuid) {
    *refusal_detail =
        "canonical character descriptors do not share the bound collation UUID";
    return false;
  }
  if (resource_epoch == 0 || collation_epoch == 0 || !text_seed.active ||
      text_seed.seed_pack_name.empty() || text_seed.seed_pack_version.empty() ||
      text_seed.charset_name.empty() || text_seed.collation_name.empty()) {
    *refusal_detail = "bound collation resource authority is incomplete";
    return false;
  }
  if (left_value.isSqlNull() || right_value.isSqlNull()) {
    *refusal_detail =
        "SQL NULL comparison requires the shared three-valued predicate seam";
    return false;
  }
  if (left_value.state != EngineValueState::value ||
      right_value.state != EngineValueState::value || left_value.is_null ||
      right_value.is_null) {
    *refusal_detail = "collation comparison requires two non-NULL value states";
    return false;
  }
  dt::DatatypeComparisonRequest request;
  request.left.type_id = dt::CanonicalTypeId::character;
  request.left.encoded_value = left_value.encoded_value;
  request.right.type_id = dt::CanonicalTypeId::character;
  request.right.encoded_value = right_value.encoded_value;
  request.case_insensitive_character_compare =
      text_seed.collation_case_insensitive;
  request.text_seed = text_seed;
  const auto compared = dt::CompareDatatypeValues(request);
  if (!compared.ok()) {
    *refusal_detail = compared.diagnostic.diagnostic_code.empty()
                          ? "canonical collation comparison refused"
                          : compared.diagnostic.diagnostic_code;
    return false;
  }
  *comparison = compared.comparison;
  return true;
}

// QOW-SOURCE-QRY-008-TIMEZONE-V1
bool QowNormalizeCanonicalTimezoneScalarV1(
    const EngineTypedValue& input_value,
    const scratchbird::core::datatypes::TimezoneSeedAuthority& timezone_seed,
    const EngineApiU64 resource_epoch,
    const EngineApiU64 timezone_epoch,
    EngineTypedValue* output_value,
    std::string* timezone_identifier,
    int* timezone_offset_minutes,
    bool* used_timezone_seed,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (output_value == nullptr || timezone_identifier == nullptr ||
      timezone_offset_minutes == nullptr || used_timezone_seed == nullptr ||
      refusal_detail == nullptr) {
    return false;
  }
  *output_value = EngineTypedValue{};
  output_value->state = EngineValueState::error;
  timezone_identifier->clear();
  *timezone_offset_minutes = 0;
  *used_timezone_seed = false;
  refusal_detail->clear();
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      input_value.descriptor.canonical_type_name);
  const std::string timezone_profile = QowCanonicalDescriptorFieldV1(
      input_value.descriptor.encoded_descriptor, "timezone_profile_id");
  const bool profile_matches_type =
      (type_id == dt::CanonicalTypeId::timestamp &&
       timezone_profile == "timestamp_timezone_profile") ||
      (type_id == dt::CanonicalTypeId::time &&
       timezone_profile == "time_timezone_profile");
  if (!QowCanonicalDescriptorIdentityV1(input_value.descriptor) ||
      input_value.descriptor.descriptor_kind != "scalar" ||
      !profile_matches_type) {
    *refusal_detail =
        "canonical temporal descriptor has no matching timezone profile";
    return false;
  }
  if (resource_epoch == 0 || timezone_epoch == 0 || !timezone_seed.active ||
      timezone_seed.seed_pack_name.empty() ||
      timezone_seed.seed_pack_version.empty() ||
      timezone_seed.content_hash.empty() ||
      timezone_seed.timezone_records == 0 ||
      timezone_seed.timezone_names.empty()) {
    *refusal_detail = "bound timezone seed authority is incomplete";
    return false;
  }
  if (input_value.isSqlNull()) {
    if (!QowCanonicalSqlNullStateV1(input_value)) {
      *refusal_detail = "SQL NULL temporal scalar carries substitute payload";
      return false;
    }
    *output_value = QowPropagateSqlNullAfterScalarV1(
        input_value.descriptor, input_value);
    return true;
  }
  if (input_value.state != EngineValueState::value || input_value.is_null) {
    *refusal_detail = "timezone normalization requires a value or SQL NULL";
    return false;
  }
  dt::ReferenceTemporalWireProfileRequest request;
  request.reference_engine = "scratchbird_native";
  request.reference_type_or_family = input_value.descriptor.canonical_type_name;
  request.wire_profile = timezone_profile;
  request.encoded_value = input_value.encoded_value;
  request.timezone_seed = timezone_seed;
  const auto normalized = dt::ValidateReferenceTemporalWireProfile(request);
  if (!normalized.ok() || normalized.canonical_type_id != type_id) {
    *refusal_detail = normalized.diagnostic.diagnostic_code.empty()
                          ? "canonical timezone normalization refused"
                          : normalized.diagnostic.diagnostic_code;
    return false;
  }
  output_value->descriptor = input_value.descriptor;
  output_value->encoded_value = normalized.normalized_value;
  output_value->binary_value.clear();
  output_value->is_null = false;
  output_value->state = EngineValueState::value;
  *timezone_identifier = normalized.timezone_identifier;
  *timezone_offset_minutes = normalized.timezone_offset_minutes;
  *used_timezone_seed = normalized.used_timezone_seed;
  return true;
}

// QOW-SOURCE-QRY-008-OVERFLOW-V1
bool QowApplyCanonicalNumericScalarV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    const EngineDescriptor& result_descriptor,
    const scratchbird::core::datatypes::DatatypeNumericOperationKind operation,
    const scratchbird::core::datatypes::DatatypeNumericContext& context,
    EngineTypedValue* output_value,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (output_value == nullptr || refusal_detail == nullptr) return false;
  *output_value = EngineTypedValue{};
  output_value->state = EngineValueState::error;
  refusal_detail->clear();
  std::uint32_t descriptor_precision = 0;
  std::uint32_t descriptor_scale = 0;
  std::uint32_t descriptor_width = 0;
  const auto left_type = dt::CanonicalTypeIdFromStableName(
      left_value.descriptor.canonical_type_name);
  const auto right_type = dt::CanonicalTypeIdFromStableName(
      right_value.descriptor.canonical_type_name);
  const auto result_type = dt::CanonicalTypeIdFromStableName(
      result_descriptor.canonical_type_name);
  const bool decimal_context =
      result_type == dt::CanonicalTypeId::decimal ||
      result_type == dt::CanonicalTypeId::decimal_float;
  const bool fixed_128_context =
      result_type == dt::CanonicalTypeId::int128 ||
      result_type == dt::CanonicalTypeId::uint128 ||
      result_type == dt::CanonicalTypeId::real128;
  const bool bounded_signed_context =
      result_type == dt::CanonicalTypeId::int8 ||
      result_type == dt::CanonicalTypeId::int16 ||
      result_type == dt::CanonicalTypeId::int32 ||
      result_type == dt::CanonicalTypeId::int64;
  const bool bounded_real_context =
      result_type == dt::CanonicalTypeId::real32 ||
      result_type == dt::CanonicalTypeId::real64;
  const bool descriptor_context_matches =
      (decimal_context &&
       QowCanonicalDescriptorU32FieldV1(
           result_descriptor.encoded_descriptor, "precision",
           &descriptor_precision) &&
       QowCanonicalDescriptorU32FieldV1(
           result_descriptor.encoded_descriptor, "scale", &descriptor_scale) &&
       descriptor_precision == context.precision &&
       descriptor_scale == context.scale) ||
      (fixed_128_context &&
       QowCanonicalDescriptorU32FieldV1(
           result_descriptor.encoded_descriptor, "width", &descriptor_width) &&
       descriptor_width == 128 && context.precision == 38 &&
       context.scale == 0) ||
      ((bounded_signed_context || bounded_real_context) &&
       context.scale == 0);
  if (!QowCanonicalDescriptorIdentityV1(left_value.descriptor) ||
      !QowCanonicalDescriptorIdentityV1(right_value.descriptor) ||
      !QowCanonicalDescriptorIdentityV1(result_descriptor) ||
      left_value.descriptor.descriptor_kind != "scalar" ||
      right_value.descriptor.descriptor_kind != "scalar" ||
      result_descriptor.descriptor_kind != "scalar" ||
      left_type != result_type || right_type != result_type ||
      (!decimal_context && !fixed_128_context && !bounded_signed_context &&
       !bounded_real_context) ||
      operation == dt::DatatypeNumericOperationKind::compare ||
      !descriptor_context_matches) {
    *refusal_detail =
        "canonical numeric descriptors or precision/scale context are invalid";
    return false;
  }
  if ((left_value.isSqlNull() &&
       !QowCanonicalSqlNullStateV1(left_value)) ||
      (right_value.isSqlNull() &&
       !QowCanonicalSqlNullStateV1(right_value))) {
    *refusal_detail = "SQL NULL numeric scalar carries substitute payload";
    return false;
  }
  if ((!left_value.isSqlNull() &&
       (left_value.state != EngineValueState::value || left_value.is_null)) ||
      (!right_value.isSqlNull() &&
       (right_value.state != EngineValueState::value || right_value.is_null))) {
    *refusal_detail = "numeric operation requires value or SQL NULL states";
    return false;
  }
  if (bounded_signed_context) {
    if (left_value.isSqlNull() || right_value.isSqlNull()) {
      output_value->descriptor = result_descriptor;
      output_value->encoded_value.clear();
      output_value->binary_value.clear();
      output_value->is_null = true;
      output_value->state = EngineValueState::sql_null;
      return true;
    }
    const auto parse = [](const std::string& encoded, std::int64_t* value) {
      if (value == nullptr || encoded.empty()) return false;
      const auto [end, error] = std::from_chars(
          encoded.data(), encoded.data() + encoded.size(), *value);
      return error == std::errc{} && end == encoded.data() + encoded.size();
    };
    std::int64_t left = 0;
    std::int64_t right = 0;
    const auto canonical_integer = [&](const EngineTypedValue& value,
                                       std::int64_t* decoded) {
      if (!parse(value.encoded_value, decoded)) return false;
      dt::DatatypeCastRequest canonical_request;
      canonical_request.value.type_id = result_type;
      canonical_request.value.encoded_value = value.encoded_value;
      canonical_request.target_type_id = result_type;
      canonical_request.explicit_cast = true;
      const auto canonical = dt::CastDatatypeValue(canonical_request);
      return canonical.ok() &&
             canonical.value.encoded_value == value.encoded_value;
    };
    if (!canonical_integer(left_value, &left) ||
        !canonical_integer(right_value, &right)) {
      *refusal_detail =
          std::string("canonical ") + dt::CanonicalTypeName(result_type) +
          " operand encoding is invalid or out of range";
      return false;
    }
    std::int64_t computed = 0;
    bool overflow = false;
    switch (operation) {
      case dt::DatatypeNumericOperationKind::add:
        overflow = __builtin_add_overflow(left, right, &computed);
        break;
      case dt::DatatypeNumericOperationKind::subtract:
        overflow = __builtin_sub_overflow(left, right, &computed);
        break;
      case dt::DatatypeNumericOperationKind::multiply:
        overflow = __builtin_mul_overflow(left, right, &computed);
        break;
      case dt::DatatypeNumericOperationKind::divide:
        if (right == 0) {
          *refusal_detail = "canonical int64 division by zero";
          return false;
        }
        if (left == std::numeric_limits<std::int64_t>::min() && right == -1) {
          overflow = true;
        } else {
          computed = left / right;
        }
        break;
      default:
        *refusal_detail =
            std::string("canonical ") + dt::CanonicalTypeName(result_type) +
            " numeric operation is unsupported";
        return false;
    }
    if (overflow) {
      *refusal_detail =
          std::string("canonical ") + dt::CanonicalTypeName(result_type) +
          " arithmetic overflow";
      return false;
    }
    dt::DatatypeCastRequest result_request;
    result_request.value.type_id = result_type;
    result_request.value.encoded_value = std::to_string(computed);
    result_request.target_type_id = result_type;
    result_request.explicit_cast = true;
    const auto canonical_result = dt::CastDatatypeValue(result_request);
    if (!canonical_result.ok()) {
      *refusal_detail =
          std::string("canonical ") + dt::CanonicalTypeName(result_type) +
          " arithmetic overflow";
      return false;
    }
    output_value->descriptor = result_descriptor;
    output_value->encoded_value = canonical_result.value.encoded_value;
    output_value->binary_value.clear();
    output_value->is_null = false;
    output_value->state = EngineValueState::value;
    return true;
  }
  if (bounded_real_context) {
    if (left_value.isSqlNull() || right_value.isSqlNull()) {
      output_value->descriptor = result_descriptor;
      output_value->encoded_value.clear();
      output_value->binary_value.clear();
      output_value->is_null = true;
      output_value->state = EngineValueState::sql_null;
      return true;
    }
    std::string computed;
    const bool accepted =
        result_type == dt::CanonicalTypeId::real32
            ? QowApplyCanonicalBoundedRealV1<float>(
                  left_value.encoded_value, right_value.encoded_value,
                  operation, context.allow_special_values, &computed,
                  refusal_detail)
            : QowApplyCanonicalBoundedRealV1<double>(
                  left_value.encoded_value, right_value.encoded_value,
                  operation, context.allow_special_values, &computed,
                  refusal_detail);
    if (!accepted) return false;
    output_value->descriptor = result_descriptor;
    output_value->encoded_value = std::move(computed);
    output_value->binary_value.clear();
    output_value->is_null = false;
    output_value->state = EngineValueState::value;
    return true;
  }
  dt::DatatypeNumericOperationRequest numeric_request;
  numeric_request.operation = operation;
  numeric_request.type_id = result_type;
  numeric_request.left.type_id = left_type;
  numeric_request.left.encoded_value = left_value.encoded_value;
  numeric_request.left.is_null = left_value.isSqlNull();
  numeric_request.right.type_id = right_type;
  numeric_request.right.encoded_value = right_value.encoded_value;
  numeric_request.right.is_null = right_value.isSqlNull();
  numeric_request.context = context;
  const auto numeric_result = dt::ApplyNumericOperation(numeric_request);
  if (!numeric_result.ok()) {
    for (const auto& argument : numeric_result.diagnostic.arguments) {
      if (argument.key == "detail" && !argument.value.empty()) {
        *refusal_detail = argument.value;
        break;
      }
    }
    if (refusal_detail->empty()) {
      *refusal_detail = numeric_result.diagnostic.diagnostic_code.empty()
                            ? "canonical numeric operation refused"
                            : numeric_result.diagnostic.diagnostic_code;
    }
    return false;
  }
  output_value->descriptor = result_descriptor;
  output_value->encoded_value = numeric_result.value.encoded_value;
  output_value->binary_value.clear();
  output_value->is_null = numeric_result.value.is_null;
  output_value->state = numeric_result.value.is_null
                            ? EngineValueState::sql_null
                            : EngineValueState::value;
  *output_value = QowPropagateSqlNullAfterScalarV1(
      result_descriptor, std::move(*output_value));
  return true;
}

// QOW-SOURCE-QRY-017-V1
const char* EngineSqlTruthValueName(const EngineSqlTruthValue value) noexcept {
  switch (value) {
    case EngineSqlTruthValue::unspecified:
      return "unspecified";
    case EngineSqlTruthValue::false_value:
      return "false";
    case EngineSqlTruthValue::true_value:
      return "true";
    case EngineSqlTruthValue::unknown:
      return "unknown";
  }
  return "invalid";
}

bool QowCanonicalTruthValueV1(const EngineSqlTruthValue value) noexcept {
  return value == EngineSqlTruthValue::false_value ||
         value == EngineSqlTruthValue::true_value ||
         value == EngineSqlTruthValue::unknown;
}

EngineSqlTruthValue QowSqlNotV1(const EngineSqlTruthValue value) noexcept {
  if (value == EngineSqlTruthValue::true_value) {
    return EngineSqlTruthValue::false_value;
  }
  if (value == EngineSqlTruthValue::false_value) {
    return EngineSqlTruthValue::true_value;
  }
  return EngineSqlTruthValue::unknown;
}

EngineSqlTruthValue QowSqlAndV1(const EngineSqlTruthValue left,
                                const EngineSqlTruthValue right) noexcept {
  if (left == EngineSqlTruthValue::false_value ||
      right == EngineSqlTruthValue::false_value) {
    return EngineSqlTruthValue::false_value;
  }
  if (left == EngineSqlTruthValue::true_value) return right;
  if (right == EngineSqlTruthValue::true_value) return left;
  return EngineSqlTruthValue::unknown;
}

EngineSqlTruthValue QowSqlOrV1(const EngineSqlTruthValue left,
                               const EngineSqlTruthValue right) noexcept {
  if (left == EngineSqlTruthValue::true_value ||
      right == EngineSqlTruthValue::true_value) {
    return EngineSqlTruthValue::true_value;
  }
  if (left == EngineSqlTruthValue::false_value) return right;
  if (right == EngineSqlTruthValue::false_value) return left;
  return EngineSqlTruthValue::unknown;
}

bool QowEvaluateCanonicalComparisonTruthV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    const int comparison,
    const EngineComparisonPredicateOperator operation,
    EngineSqlTruthValue* truth_value,
    std::string* refusal_detail) {
  if (truth_value == nullptr || refusal_detail == nullptr) return false;
  *truth_value = EngineSqlTruthValue::unknown;
  refusal_detail->clear();
  if (!QowCanonicalDescriptorIdentityV1(left_value.descriptor) ||
      !QowCanonicalDescriptorIdentityV1(right_value.descriptor) ||
      left_value.descriptor.descriptor_kind != "scalar" ||
      right_value.descriptor.descriptor_kind != "scalar" ||
      scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
          left_value.descriptor.canonical_type_name) !=
          scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
              right_value.descriptor.canonical_type_name)) {
    *refusal_detail =
        "comparison operands do not share a canonical scalar type";
    return false;
  }
  const auto comparison_type =
      scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
          left_value.descriptor.canonical_type_name);
  if (comparison_type ==
      scratchbird::core::datatypes::CanonicalTypeId::unknown) {
    *refusal_detail = "comparison operand type is unknown";
    return false;
  }
  if (comparison_type ==
      scratchbird::core::datatypes::CanonicalTypeId::character) {
    const std::string left_collation = QowCanonicalDescriptorFieldV1(
        left_value.descriptor.encoded_descriptor, "collation_uuid");
    const std::string right_collation = QowCanonicalDescriptorFieldV1(
        right_value.descriptor.encoded_descriptor, "collation_uuid");
    if (!QowCanonicalUuidV1(left_collation) ||
        left_collation != right_collation) {
      *refusal_detail =
          "character comparison descriptors have mismatched collation authority";
      return false;
    }
  }
  if ((left_value.isSqlNull() &&
       !QowCanonicalSqlNullStateV1(left_value)) ||
      (right_value.isSqlNull() &&
       !QowCanonicalSqlNullStateV1(right_value))) {
    *refusal_detail = "SQL NULL predicate operand carries substitute payload";
    return false;
  }
  switch (operation) {
    case EngineComparisonPredicateOperator::equal:
    case EngineComparisonPredicateOperator::not_equal:
    case EngineComparisonPredicateOperator::less_than:
    case EngineComparisonPredicateOperator::less_than_or_equal:
    case EngineComparisonPredicateOperator::greater_than:
    case EngineComparisonPredicateOperator::greater_than_or_equal:
      break;
    default:
      *refusal_detail = "comparison predicate operator is not bound";
      return false;
  }
  if (left_value.isSqlNull() || right_value.isSqlNull()) {
    *truth_value = EngineSqlTruthValue::unknown;
    return true;
  }
  if (left_value.state != EngineValueState::value || left_value.is_null ||
      right_value.state != EngineValueState::value || right_value.is_null) {
    *refusal_detail =
        "comparison requires value or canonical SQL NULL operand states";
    return false;
  }
  if (comparison < -1 || comparison > 1) {
    *refusal_detail = "typed comparator returned a noncanonical ordering";
    return false;
  }
  bool result = false;
  switch (operation) {
    case EngineComparisonPredicateOperator::equal:
      result = comparison == 0;
      break;
    case EngineComparisonPredicateOperator::not_equal:
      result = comparison != 0;
      break;
    case EngineComparisonPredicateOperator::less_than:
      result = comparison < 0;
      break;
    case EngineComparisonPredicateOperator::less_than_or_equal:
      result = comparison <= 0;
      break;
    case EngineComparisonPredicateOperator::greater_than:
      result = comparison > 0;
      break;
    case EngineComparisonPredicateOperator::greater_than_or_equal:
      result = comparison >= 0;
      break;
    default:
      return false;
  }
  *truth_value = result ? EngineSqlTruthValue::true_value
                        : EngineSqlTruthValue::false_value;
  return true;
}

bool QowEvaluateCanonicalNullPredicateV1(
    const EngineTypedValue& value,
    const bool negate,
    EngineSqlTruthValue* truth_value,
    std::string* refusal_detail) {
  if (truth_value == nullptr || refusal_detail == nullptr) return false;
  *truth_value = EngineSqlTruthValue::unknown;
  refusal_detail->clear();
  if (!QowCanonicalDescriptorIdentityV1(value.descriptor) ||
      value.descriptor.descriptor_kind != "scalar" ||
      scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
          value.descriptor.canonical_type_name) ==
          scratchbird::core::datatypes::CanonicalTypeId::unknown ||
      (value.isSqlNull() && !QowCanonicalSqlNullStateV1(value))) {
    *refusal_detail = "IS NULL operand is not a canonical scalar state";
    return false;
  }
  if (!value.isSqlNull() &&
      (value.state != EngineValueState::value || value.is_null)) {
    *refusal_detail = "IS NULL cannot consume a non-value runtime sentinel";
    return false;
  }
  const bool is_null = value.isSqlNull();
  *truth_value = (negate ? !is_null : is_null)
                     ? EngineSqlTruthValue::true_value
                     : EngineSqlTruthValue::false_value;
  return true;
}

bool QowCompareCanonicalNonCollatedScalarsV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    int* comparison,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (comparison == nullptr || refusal_detail == nullptr) return false;
  *comparison = 0;
  refusal_detail->clear();
  if (!QowCanonicalDescriptorIdentityV1(left_value.descriptor) ||
      !QowCanonicalDescriptorIdentityV1(right_value.descriptor) ||
      left_value.descriptor.descriptor_kind != "scalar" ||
      right_value.descriptor.descriptor_kind != "scalar" ||
      dt::CanonicalTypeIdFromStableName(
          left_value.descriptor.canonical_type_name) !=
          dt::CanonicalTypeIdFromStableName(
              right_value.descriptor.canonical_type_name) ||
      left_value.state != EngineValueState::value || left_value.is_null ||
      right_value.state != EngineValueState::value || right_value.is_null) {
    *refusal_detail =
        "non-NULL comparison operands are not canonical scalar values";
    return false;
  }
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      left_value.descriptor.canonical_type_name);
  const bool supported =
      type_id == dt::CanonicalTypeId::boolean ||
      (type_id >= dt::CanonicalTypeId::int8 &&
       type_id <= dt::CanonicalTypeId::int128) ||
      (type_id >= dt::CanonicalTypeId::uint8 &&
       type_id <= dt::CanonicalTypeId::uint128) ||
      (type_id >= dt::CanonicalTypeId::bfloat16 &&
       type_id <= dt::CanonicalTypeId::decimal_float) ||
      type_id == dt::CanonicalTypeId::uuid ||
      type_id == dt::CanonicalTypeId::ip_address ||
      type_id == dt::CanonicalTypeId::network_prefix ||
      type_id == dt::CanonicalTypeId::mac_address ||
      type_id == dt::CanonicalTypeId::binary ||
      type_id == dt::CanonicalTypeId::bit_string ||
      type_id == dt::CanonicalTypeId::date ||
      type_id == dt::CanonicalTypeId::time ||
      type_id == dt::CanonicalTypeId::timestamp ||
      type_id == dt::CanonicalTypeId::interval;
  if (!supported) {
    *refusal_detail =
        type_id == dt::CanonicalTypeId::character
            ? "character comparison requires catalog-bound collation authority"
            : "canonical scalar comparison type is not enabled at this seam";
    return false;
  }
  const bool runtime_numeric =
      type_id == dt::CanonicalTypeId::decimal ||
      type_id == dt::CanonicalTypeId::decimal_float ||
      type_id == dt::CanonicalTypeId::int128 ||
      type_id == dt::CanonicalTypeId::uint128 ||
      type_id == dt::CanonicalTypeId::real128;
  if (runtime_numeric) {
    if (type_id == dt::CanonicalTypeId::int128 ||
        type_id == dt::CanonicalTypeId::uint128 ||
        type_id == dt::CanonicalTypeId::real128) {
      std::uint32_t left_width = 0;
      std::uint32_t right_width = 0;
      if (!QowCanonicalDescriptorU32FieldV1(
              left_value.descriptor.encoded_descriptor, "width",
              &left_width) ||
          !QowCanonicalDescriptorU32FieldV1(
              right_value.descriptor.encoded_descriptor, "width",
              &right_width) ||
          left_width != 128 || right_width != 128) {
        *refusal_detail =
            "128-bit comparison descriptor width is invalid";
        return false;
      }
    }
    dt::DatatypeNumericOperationRequest request;
    request.operation = dt::DatatypeNumericOperationKind::compare;
    request.type_id = type_id;
    request.left.type_id = type_id;
    request.left.encoded_value = left_value.encoded_value;
    request.right.type_id = type_id;
    request.right.encoded_value = right_value.encoded_value;
    request.context.precision = 38;
    request.context.scale = 0;
    if (type_id == dt::CanonicalTypeId::decimal ||
        type_id == dt::CanonicalTypeId::decimal_float) {
      std::uint32_t left_precision = 0;
      std::uint32_t left_scale = 0;
      std::uint32_t right_precision = 0;
      std::uint32_t right_scale = 0;
      if (!QowCanonicalDescriptorU32FieldV1(
              left_value.descriptor.encoded_descriptor, "precision",
              &left_precision) ||
          !QowCanonicalDescriptorU32FieldV1(
              left_value.descriptor.encoded_descriptor, "scale",
              &left_scale) ||
          !QowCanonicalDescriptorU32FieldV1(
              right_value.descriptor.encoded_descriptor, "precision",
              &right_precision) ||
          !QowCanonicalDescriptorU32FieldV1(
              right_value.descriptor.encoded_descriptor, "scale",
              &right_scale)) {
        *refusal_detail =
            "numeric comparison descriptor precision or scale is invalid";
        return false;
      }
      request.context.precision =
          std::max(left_precision, right_precision);
      request.context.scale = std::max(left_scale, right_scale);
    }
    const auto numeric = dt::ApplyNumericOperation(request);
    if (!numeric.ok()) {
      *refusal_detail = numeric.diagnostic.diagnostic_code.empty()
                            ? "canonical numeric comparison refused"
                            : numeric.diagnostic.diagnostic_code;
      return false;
    }
    *comparison =
        numeric.comparison < 0 ? -1 : (numeric.comparison > 0 ? 1 : 0);
    return true;
  }
  const auto validate = [type_id](const EngineTypedValue& value) {
    dt::DatatypeCastRequest request;
    request.value.type_id = type_id;
    request.value.encoded_value = value.encoded_value;
    request.target_type_id = type_id;
    request.explicit_cast = true;
    return dt::CastDatatypeValue(request);
  };
  const auto left_checked = validate(left_value);
  const auto right_checked = validate(right_value);
  if (!left_checked.ok() || !right_checked.ok()) {
    *refusal_detail = "canonical comparison operand encoding is invalid";
    return false;
  }
  dt::DatatypeComparisonRequest request;
  request.left.type_id = type_id;
  request.left.encoded_value = left_checked.value.encoded_value;
  request.right.type_id = type_id;
  request.right.encoded_value = right_checked.value.encoded_value;
  const auto compared = dt::CompareDatatypeValues(request);
  if (!compared.ok()) {
    *refusal_detail = compared.diagnostic.diagnostic_code.empty()
                          ? "canonical scalar comparison refused"
                          : compared.diagnostic.diagnostic_code;
    return false;
  }
  *comparison = compared.comparison < 0 ? -1 : (compared.comparison > 0 ? 1 : 0);
  return true;
}

bool QowMaterializeCanonicalTruthValueV1(
    const EngineSqlTruthValue truth_value,
    const EngineDescriptor& result_descriptor,
    EngineTypedValue* output_value,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (output_value == nullptr || refusal_detail == nullptr) return false;
  *output_value = EngineTypedValue{};
  output_value->state = EngineValueState::error;
  refusal_detail->clear();
  if (!QowCanonicalTruthValueV1(truth_value) ||
      !QowCanonicalDescriptorIdentityV1(result_descriptor) ||
      result_descriptor.descriptor_kind != "scalar" ||
      dt::CanonicalTypeIdFromStableName(
          result_descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::boolean) {
    *refusal_detail = "predicate result descriptor is not canonical boolean";
    return false;
  }
  const std::string nullability = QowCanonicalDescriptorFieldV1(
      result_descriptor.encoded_descriptor, "nullability");
  if ((nullability != "nullable" && nullability != "non_null" &&
       nullability != "unknown") ||
      (truth_value == EngineSqlTruthValue::unknown &&
       nullability == "non_null")) {
    *refusal_detail =
        "predicate truth state contradicts the bound result nullability";
    return false;
  }
  output_value->descriptor = result_descriptor;
  output_value->binary_value.clear();
  if (truth_value == EngineSqlTruthValue::unknown) {
    output_value->encoded_value.clear();
    output_value->is_null = true;
    output_value->state = EngineValueState::sql_null;
  } else {
    output_value->encoded_value =
        truth_value == EngineSqlTruthValue::true_value ? "true" : "false";
    output_value->is_null = false;
    output_value->state = EngineValueState::value;
  }
  return true;
}

bool QowCanonicalTruthFromTypedValueV1(
    const EngineTypedValue& value,
    EngineSqlTruthValue* truth,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (truth == nullptr || refusal_detail == nullptr) return false;
  *truth = EngineSqlTruthValue::unspecified;
  refusal_detail->clear();
  if (!QowCanonicalDescriptorIdentityV1(value.descriptor) ||
      value.descriptor.descriptor_kind != "scalar" ||
      dt::CanonicalTypeIdFromStableName(
          value.descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::boolean) {
    *refusal_detail = "expression truth operand is not canonical boolean";
    return false;
  }
  if (value.isSqlNull()) {
    if (!QowCanonicalSqlNullStateV1(value)) {
      *refusal_detail = "SQL NULL boolean carries substitute payload";
      return false;
    }
    *truth = EngineSqlTruthValue::unknown;
    return true;
  }
  if (value.state != EngineValueState::value || value.is_null ||
      !value.binary_value.empty() ||
      (value.encoded_value != "true" && value.encoded_value != "false")) {
    *refusal_detail = "expression truth operand has a noncanonical payload";
    return false;
  }
  *truth = value.encoded_value == "true"
               ? EngineSqlTruthValue::true_value
               : EngineSqlTruthValue::false_value;
  return true;
}

bool QowCanonicalExpressionConsumerPassesV1(
    const EngineCanonicalExpressionConsumer consumer,
    const EngineSqlTruthValue truth_value,
    bool* passes,
    std::string* refusal_detail) {
  if (passes == nullptr || refusal_detail == nullptr) return false;
  *passes = false;
  refusal_detail->clear();
  const bool canonical_consumer =
      consumer == EngineCanonicalExpressionConsumer::filter ||
      consumer == EngineCanonicalExpressionConsumer::projection ||
      consumer == EngineCanonicalExpressionConsumer::join ||
      consumer == EngineCanonicalExpressionConsumer::aggregate ||
      consumer == EngineCanonicalExpressionConsumer::window ||
      consumer == EngineCanonicalExpressionConsumer::subquery;
  if (!canonical_consumer || !QowCanonicalTruthValueV1(truth_value)) {
    *refusal_detail = "canonical expression consumer or truth value is not bound";
    return false;
  }
  *passes = truth_value == EngineSqlTruthValue::true_value;
  return true;
}

// RCP-023-SOURCE-CANONICAL-TYPED-EXPRESSION-RUNTIME-V1
bool QowEvaluateCanonicalTypedExpressionV1(
    const EngineCanonicalExpressionEvaluationRequest& request,
    EngineCanonicalExpressionEvaluationResult* result,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (result == nullptr || refusal_detail == nullptr) return false;
  *result = EngineCanonicalExpressionEvaluationResult{};
  result->value.state = EngineValueState::error;
  result->diagnostic_id =
      QowCanonicalExpressionDiagnosticIdV1(request.operation);
  refusal_detail->clear();
  const bool canonical_consumer =
      request.consumer == EngineCanonicalExpressionConsumer::filter ||
      request.consumer == EngineCanonicalExpressionConsumer::projection ||
      request.consumer == EngineCanonicalExpressionConsumer::join ||
      request.consumer == EngineCanonicalExpressionConsumer::aggregate ||
      request.consumer == EngineCanonicalExpressionConsumer::window ||
      request.consumer == EngineCanonicalExpressionConsumer::subquery;
  if (!canonical_consumer) {
    *refusal_detail = "canonical expression consumer is not bound";
    return false;
  }

  const auto materialize_truth = [&](const EngineSqlTruthValue truth) {
    result->truth = truth;
    if (!QowCanonicalExpressionConsumerPassesV1(
            request.consumer, truth, &result->passes_consumer,
            refusal_detail)) {
      return false;
    }
    return QowMaterializeCanonicalTruthValueV1(
        truth, request.result_descriptor, &result->value, refusal_detail);
  };
  const auto canonical_value_state = [](const EngineTypedValue& value) {
    return (value.state == EngineValueState::value && !value.is_null) ||
           (value.state == EngineValueState::sql_null && value.is_null &&
            value.encoded_value.empty() && value.binary_value.empty());
  };

  switch (request.operation) {
    case EngineCanonicalExpressionOperation::identity: {
      if (!QowCanonicalDescriptorIdentityV1(request.left_value.descriptor) ||
          !QowCanonicalDescriptorIdentityV1(request.result_descriptor) ||
          request.left_value.descriptor.descriptor_kind != "scalar" ||
          request.result_descriptor.descriptor_kind != "scalar" ||
          request.left_value.descriptor.canonical_type_name !=
              request.result_descriptor.canonical_type_name ||
          !canonical_value_state(request.left_value)) {
        *refusal_detail =
            "identity expression requires one descriptor-compatible canonical value";
        return false;
      }
      if (request.left_value.isSqlNull() &&
          QowCanonicalDescriptorFieldV1(
              request.result_descriptor.encoded_descriptor,
              "nullability") == "non_null") {
        *refusal_detail =
            "identity expression SQL NULL contradicts result nullability";
        return false;
      }
      result->value = request.left_value;
      result->value.descriptor = request.result_descriptor;
      result->value = QowPropagateSqlNullAfterScalarV1(
          request.result_descriptor, std::move(result->value));
      return true;
    }
    case EngineCanonicalExpressionOperation::consume_truth:
      if (!QowCanonicalTruthValueV1(request.input_truth)) {
        *refusal_detail = "canonical expression truth input is not bound";
        return false;
      }
      return materialize_truth(request.input_truth);
    case EngineCanonicalExpressionOperation::explicit_cast:
    case EngineCanonicalExpressionOperation::implicit_cast: {
      std::string cast_category;
      return QowApplyCanonicalDescriptorCoercionV1(
          request.left_value, request.result_descriptor,
          request.operation ==
              EngineCanonicalExpressionOperation::explicit_cast,
          &result->value, &cast_category, refusal_detail);
    }
    case EngineCanonicalExpressionOperation::numeric_add:
    case EngineCanonicalExpressionOperation::numeric_subtract:
    case EngineCanonicalExpressionOperation::numeric_multiply:
    case EngineCanonicalExpressionOperation::numeric_divide: {
      auto operation = dt::DatatypeNumericOperationKind::add;
      if (request.operation ==
          EngineCanonicalExpressionOperation::numeric_subtract) {
        operation = dt::DatatypeNumericOperationKind::subtract;
      } else if (request.operation ==
                 EngineCanonicalExpressionOperation::numeric_multiply) {
        operation = dt::DatatypeNumericOperationKind::multiply;
      } else if (request.operation ==
                 EngineCanonicalExpressionOperation::numeric_divide) {
        operation = dt::DatatypeNumericOperationKind::divide;
      }
      return QowApplyCanonicalNumericScalarV1(
          request.left_value, request.right_value, request.result_descriptor,
          operation, request.numeric_context, &result->value,
          refusal_detail);
    }
    case EngineCanonicalExpressionOperation::numeric_modulo: {
      const auto result_type = dt::CanonicalTypeIdFromStableName(
          request.result_descriptor.canonical_type_name);
      const bool bounded_signed =
          result_type == dt::CanonicalTypeId::int8 ||
          result_type == dt::CanonicalTypeId::int16 ||
          result_type == dt::CanonicalTypeId::int32 ||
          result_type == dt::CanonicalTypeId::int64;
      const auto canonical_integer = [&](const EngineTypedValue& value) {
        return QowCanonicalDescriptorIdentityV1(value.descriptor) &&
               value.descriptor.descriptor_kind == "scalar" &&
               dt::CanonicalTypeIdFromStableName(
                   value.descriptor.canonical_type_name) == result_type &&
               ((value.state == EngineValueState::value && !value.is_null) ||
                (value.state == EngineValueState::sql_null && value.is_null &&
                 value.encoded_value.empty() &&
                 value.binary_value.empty()));
      };
      if (!bounded_signed || !canonical_integer(request.left_value) ||
          !canonical_integer(request.right_value) ||
          !QowCanonicalDescriptorIdentityV1(request.result_descriptor) ||
          request.result_descriptor.descriptor_kind != "scalar") {
        *refusal_detail =
            "canonical modulo requires descriptor-exact bounded signed "
            "integer operands";
        return false;
      }
      result->value.descriptor = request.result_descriptor;
      if (request.left_value.isSqlNull() ||
          request.right_value.isSqlNull()) {
        result->value.setState(EngineValueState::sql_null);
        result->value = QowPropagateSqlNullAfterScalarV1(
            request.result_descriptor, std::move(result->value));
        return true;
      }
      const auto parse = [](const std::string& encoded,
                            std::int64_t* value) {
        if (value == nullptr || encoded.empty()) return false;
        const auto [end, error] = std::from_chars(
            encoded.data(), encoded.data() + encoded.size(), *value);
        return error == std::errc{} &&
               end == encoded.data() + encoded.size();
      };
      std::int64_t left = 0;
      std::int64_t right = 0;
      const auto parse_canonical = [&](const EngineTypedValue& value,
                                       std::int64_t* decoded) {
        if (!parse(value.encoded_value, decoded)) return false;
        dt::DatatypeCastRequest canonical_request;
        canonical_request.value.type_id = result_type;
        canonical_request.value.encoded_value = value.encoded_value;
        canonical_request.target_type_id = result_type;
        canonical_request.explicit_cast = true;
        const auto canonical = dt::CastDatatypeValue(canonical_request);
        return canonical.ok() &&
               canonical.value.encoded_value == value.encoded_value;
      };
      if (!parse_canonical(request.left_value, &left) ||
          !parse_canonical(request.right_value, &right)) {
        *refusal_detail =
            "canonical modulo operand encoding is invalid or out of range";
        return false;
      }
      if (right == 0) {
        *refusal_detail =
            std::string("canonical ") + dt::CanonicalTypeName(result_type) +
            " modulo by zero";
        return false;
      }
      const std::int64_t remainder =
          left == std::numeric_limits<std::int64_t>::min() && right == -1
              ? 0
              : left % right;
      result->value.encoded_value = std::to_string(remainder);
      result->value.setState(EngineValueState::value);
      return true;
    }
    case EngineCanonicalExpressionOperation::text_concat: {
      const auto left_collation = QowCanonicalDescriptorFieldV1(
          request.left_value.descriptor.encoded_descriptor,
          "collation_uuid");
      const auto right_collation = QowCanonicalDescriptorFieldV1(
          request.right_value.descriptor.encoded_descriptor,
          "collation_uuid");
      const auto result_collation = QowCanonicalDescriptorFieldV1(
          request.result_descriptor.encoded_descriptor,
          "collation_uuid");
      if (!QowCanonicalDescriptorIdentityV1(request.left_value.descriptor) ||
          !QowCanonicalDescriptorIdentityV1(request.right_value.descriptor) ||
          !QowCanonicalDescriptorIdentityV1(request.result_descriptor) ||
          dt::CanonicalTypeIdFromStableName(
              request.left_value.descriptor.canonical_type_name) !=
              dt::CanonicalTypeId::character ||
          dt::CanonicalTypeIdFromStableName(
              request.right_value.descriptor.canonical_type_name) !=
              dt::CanonicalTypeId::character ||
          dt::CanonicalTypeIdFromStableName(
              request.result_descriptor.canonical_type_name) !=
              dt::CanonicalTypeId::character ||
          !QowCanonicalUuidV1(left_collation) ||
          left_collation != right_collation ||
          left_collation != result_collation ||
          !canonical_value_state(request.left_value) ||
          !canonical_value_state(request.right_value)) {
        *refusal_detail =
            "text concatenation operands or result descriptor are invalid";
        return false;
      }
      result->value.descriptor = request.result_descriptor;
      if (request.left_value.isSqlNull() ||
          request.right_value.isSqlNull()) {
        result->value.setState(EngineValueState::sql_null);
        result->value = QowPropagateSqlNullAfterScalarV1(
            request.result_descriptor, std::move(result->value));
      } else {
        result->value.encoded_value =
            request.left_value.encoded_value +
            request.right_value.encoded_value;
        result->value.setState(EngineValueState::value);
      }
      return true;
    }
    case EngineCanonicalExpressionOperation::like:
    case EngineCanonicalExpressionOperation::ilike: {
      const auto canonical_text = [&](const EngineTypedValue& value) {
        return QowCanonicalDescriptorIdentityV1(value.descriptor) &&
               value.descriptor.descriptor_kind == "scalar" &&
               dt::CanonicalTypeIdFromStableName(
                   value.descriptor.canonical_type_name) ==
                   dt::CanonicalTypeId::character &&
               ((value.state == EngineValueState::value && !value.is_null) ||
                (value.state == EngineValueState::sql_null && value.is_null &&
                 value.encoded_value.empty() &&
                 value.binary_value.empty()));
      };
      const auto left_collation = QowCanonicalDescriptorFieldV1(
          request.left_value.descriptor.encoded_descriptor,
          "collation_uuid");
      const auto right_collation = QowCanonicalDescriptorFieldV1(
          request.right_value.descriptor.encoded_descriptor,
          "collation_uuid");
      if (!canonical_text(request.left_value) ||
          !canonical_text(request.right_value) ||
          !QowCanonicalUuidV1(left_collation) ||
          left_collation != right_collation ||
          !request.bound_text_authority) {
        *refusal_detail =
            "LIKE operands lack one descriptor-bound collation authority";
        return false;
      }
      if (request.left_value.isSqlNull() ||
          request.right_value.isSqlNull()) {
        return materialize_truth(EngineSqlTruthValue::unknown);
      }
      const auto matched = QowLikeMatchV1(
          request.left_value.encoded_value,
          request.right_value.encoded_value,
          request.operation == EngineCanonicalExpressionOperation::ilike);
      if (!matched.valid) {
        *refusal_detail = "LIKE pattern has an invalid escape sequence";
        return false;
      }
      return materialize_truth(
          matched.matched ? EngineSqlTruthValue::true_value
                          : EngineSqlTruthValue::false_value);
    }
    case EngineCanonicalExpressionOperation::is_null:
    case EngineCanonicalExpressionOperation::is_not_null: {
      EngineSqlTruthValue truth = EngineSqlTruthValue::unspecified;
      if (!QowEvaluateCanonicalNullPredicateV1(
              request.left_value,
              request.operation ==
                  EngineCanonicalExpressionOperation::is_not_null,
              &truth, refusal_detail)) {
        return false;
      }
      return materialize_truth(truth);
    }
    case EngineCanonicalExpressionOperation::logical_not:
    case EngineCanonicalExpressionOperation::logical_and:
    case EngineCanonicalExpressionOperation::logical_or:
    case EngineCanonicalExpressionOperation::logical_xor: {
      EngineSqlTruthValue left = EngineSqlTruthValue::unspecified;
      if (!QowCanonicalTruthFromTypedValueV1(
              request.left_value, &left, refusal_detail)) {
        return false;
      }
      EngineSqlTruthValue truth = left;
      if (request.operation ==
          EngineCanonicalExpressionOperation::logical_not) {
        truth = QowSqlNotV1(left);
      } else {
        EngineSqlTruthValue right = EngineSqlTruthValue::unspecified;
        if (!QowCanonicalTruthFromTypedValueV1(
                request.right_value, &right, refusal_detail)) {
          return false;
        }
        if (request.operation ==
            EngineCanonicalExpressionOperation::logical_and) {
          truth = QowSqlAndV1(left, right);
        } else if (request.operation ==
                   EngineCanonicalExpressionOperation::logical_or) {
          truth = QowSqlOrV1(left, right);
        } else {
          truth = left == EngineSqlTruthValue::unknown ||
                          right == EngineSqlTruthValue::unknown
                      ? EngineSqlTruthValue::unknown
                      : (left != right ? EngineSqlTruthValue::true_value
                                       : EngineSqlTruthValue::false_value);
        }
      }
      return materialize_truth(truth);
    }
    case EngineCanonicalExpressionOperation::equal:
    case EngineCanonicalExpressionOperation::not_equal:
    case EngineCanonicalExpressionOperation::less_than:
    case EngineCanonicalExpressionOperation::less_than_or_equal:
    case EngineCanonicalExpressionOperation::greater_than:
    case EngineCanonicalExpressionOperation::greater_than_or_equal:
    case EngineCanonicalExpressionOperation::is_distinct_from:
    case EngineCanonicalExpressionOperation::is_not_distinct_from: {
      const bool distinct_predicate =
          request.operation ==
              EngineCanonicalExpressionOperation::is_distinct_from ||
          request.operation ==
              EngineCanonicalExpressionOperation::is_not_distinct_from;
      if (distinct_predicate &&
          (request.left_value.isSqlNull() ||
           request.right_value.isSqlNull())) {
        EngineSqlTruthValue ignored_truth = EngineSqlTruthValue::unspecified;
        if (!canonical_value_state(request.left_value) ||
            !canonical_value_state(request.right_value) ||
            !QowEvaluateCanonicalComparisonTruthV1(
                request.left_value, request.right_value, 0,
                EngineComparisonPredicateOperator::equal, &ignored_truth,
                refusal_detail)) {
          return false;
        }
        const bool distinct = request.left_value.isSqlNull() !=
                              request.right_value.isSqlNull();
        const bool selected =
            request.operation ==
                    EngineCanonicalExpressionOperation::is_distinct_from
                ? distinct
                : !distinct;
        return materialize_truth(
            selected ? EngineSqlTruthValue::true_value
                     : EngineSqlTruthValue::false_value);
      }
      int comparison = 0;
      if (request.precomputed_comparison.has_value()) {
        comparison = *request.precomputed_comparison;
        if (comparison < -1 || comparison > 1) {
          *refusal_detail =
              "precomputed expression comparison is noncanonical";
          return false;
        }
      } else if (!request.left_value.isSqlNull() &&
                 !request.right_value.isSqlNull() &&
                 !QowCompareCanonicalNonCollatedScalarsV1(
                     request.left_value, request.right_value, &comparison,
                     refusal_detail)) {
        return false;
      }
      EngineComparisonPredicateOperator predicate =
          EngineComparisonPredicateOperator::equal;
      if (request.operation ==
          EngineCanonicalExpressionOperation::not_equal) {
        predicate = EngineComparisonPredicateOperator::not_equal;
      } else if (request.operation ==
                 EngineCanonicalExpressionOperation::less_than) {
        predicate = EngineComparisonPredicateOperator::less_than;
      } else if (request.operation ==
                 EngineCanonicalExpressionOperation::less_than_or_equal) {
        predicate = EngineComparisonPredicateOperator::less_than_or_equal;
      } else if (request.operation ==
                 EngineCanonicalExpressionOperation::greater_than) {
        predicate = EngineComparisonPredicateOperator::greater_than;
      } else if (request.operation ==
                 EngineCanonicalExpressionOperation::greater_than_or_equal) {
        predicate = EngineComparisonPredicateOperator::greater_than_or_equal;
      }
      if (distinct_predicate) {
        const bool distinct = comparison != 0;
        const bool selected =
            request.operation ==
                    EngineCanonicalExpressionOperation::is_distinct_from
                ? distinct
                : !distinct;
        result->comparison = comparison;
        return materialize_truth(
            selected ? EngineSqlTruthValue::true_value
                     : EngineSqlTruthValue::false_value);
      }
      EngineSqlTruthValue truth = EngineSqlTruthValue::unspecified;
      if (!QowEvaluateCanonicalComparisonTruthV1(
              request.left_value, request.right_value, comparison, predicate,
              &truth, refusal_detail)) {
        return false;
      }
      result->comparison = comparison;
      return materialize_truth(truth);
    }
    case EngineCanonicalExpressionOperation::scalar_function: {
      if (!request.precomputed_value.has_value()) {
        *refusal_detail =
            "canonical function operation has no bound runtime result";
        return false;
      }
      const auto& function_value = *request.precomputed_value;
      const auto source_type = dt::CanonicalTypeIdFromStableName(
          function_value.descriptor.canonical_type_name);
      const auto result_type = dt::CanonicalTypeIdFromStableName(
          request.result_descriptor.canonical_type_name);
      if (!QowCanonicalDescriptorIdentityV1(function_value.descriptor) ||
          function_value.descriptor.descriptor_kind != "scalar" ||
          !QowCanonicalDescriptorIdentityV1(request.result_descriptor) ||
          request.result_descriptor.descriptor_kind != "scalar" ||
          source_type == dt::CanonicalTypeId::unknown ||
          result_type == dt::CanonicalTypeId::unknown ||
          (function_value.isSqlNull() &&
           !QowCanonicalSqlNullStateV1(function_value)) ||
          (!function_value.isSqlNull() &&
           (function_value.state != EngineValueState::value ||
            function_value.is_null))) {
        *refusal_detail =
            "bound function result or target descriptor is invalid";
        return false;
      }
      dt::DatatypeCastRequest cast_request;
      cast_request.value.type_id = source_type;
      cast_request.value.encoded_value = function_value.encoded_value;
      cast_request.value.is_null = function_value.isSqlNull();
      cast_request.target_type_id = result_type;
      cast_request.explicit_cast = false;
      const auto cast = dt::CastDatatypeValue(cast_request);
      if (!cast.ok()) {
        *refusal_detail = cast.diagnostic.diagnostic_code.empty()
                              ? "bound function result coercion refused"
                              : cast.diagnostic.diagnostic_code;
        return false;
      }
      result->value.descriptor = request.result_descriptor;
      result->value.encoded_value = cast.value.encoded_value;
      if (source_type == result_type && !cast.value.is_null) {
        result->value.binary_value = function_value.binary_value;
      } else {
        result->value.binary_value.clear();
      }
      result->value.setState(cast.value.is_null
                                 ? EngineValueState::sql_null
                                 : EngineValueState::value);
      return true;
    }
    case EngineCanonicalExpressionOperation::unspecified:
      *refusal_detail = "canonical expression operation is not bound";
      return false;
  }
  *refusal_detail = "canonical expression operation is invalid";
  return false;
}

// QOW-SOURCE-QRY-025-V1
bool QowBindCanonicalExpressionReferenceV1(
    const EngineBindExpressionRequest& request,
    EngineObjectReference* bound_reference,
    EngineDescriptor* bound_descriptor,
    std::string* refusal_reason,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (bound_reference == nullptr || bound_descriptor == nullptr ||
      refusal_reason == nullptr || refusal_detail == nullptr) {
    return false;
  }
  *bound_reference = EngineObjectReference{};
  *bound_descriptor = EngineDescriptor{};
  refusal_reason->clear();
  refusal_detail->clear();
  const auto refuse = [&](std::string reason, std::string detail) {
    *refusal_reason = std::move(reason);
    *refusal_detail = std::move(detail);
    return false;
  };

  const auto& reference = request.sql_object_reference;
  const auto& identity = request.bound_object_identity;
  const auto& context = request.context;
  if (reference.expected_object_type.empty() ||
      reference.object_name.raw_text.empty() ||
      (reference.object_name.normalized_lookup_key.empty() &&
       reference.object_name.exact_lookup_key.empty()) ||
      reference.object_name.source_span.empty()) {
    return refuse("malformed_reference",
                  "typed expression reference is incomplete");
  }
  if (!request.related_objects.empty()) {
    return refuse("ambiguous_reference",
                  "typed expression reference has multiple resolution candidates");
  }
  if (!QowCanonicalUuidV1(identity.object_uuid.canonical) ||
      identity.resolved_object_type.empty() ||
      !QowCanonicalUuidV1(identity.resolved_schema_uuid.canonical)) {
    return refuse("unresolved_reference",
                  "typed expression reference has no canonical object binding");
  }
  if (identity.resolved_object_type != reference.expected_object_type) {
    return refuse("ill_typed_reference",
                  "resolved object kind does not match the bound reference kind");
  }
  if (!context.statement_metadata_snapshot_engine_owned ||
      !QowCanonicalUuidV1(context.statement_metadata_snapshot_uuid.canonical) ||
      context.catalog_generation_id == 0 || context.security_epoch == 0 ||
      context.resource_epoch == 0 || identity.catalog_generation_id == 0 ||
      identity.security_epoch == 0 || identity.resource_epoch == 0 ||
      identity.catalog_generation_id != context.catalog_generation_id ||
      identity.security_epoch != context.security_epoch ||
      identity.resource_epoch != context.resource_epoch) {
    return refuse("stale_reference",
                  "resolved expression reference is outside the engine statement metadata snapshot");
  }
  if (request.descriptors.size() != 1 ||
      !QowCanonicalDescriptorIdentityV1(request.descriptors.front()) ||
      dt::CanonicalTypeIdFromStableName(
          request.descriptors.front().canonical_type_name) ==
          dt::CanonicalTypeId::unknown) {
    return refuse("ill_typed_reference",
                  "typed expression reference requires one canonical descriptor");
  }

  const auto& authorization = context.authorization_context;
  if (!context.security_context_present || !authorization.present ||
      authorization.principal_uuid.canonical !=
          context.principal_uuid.canonical ||
      authorization.security_epoch != context.security_epoch ||
      authorization.catalog_generation_id != context.catalog_generation_id ||
      authorization.policy_epoch == 0) {
    return refuse("unauthorized_reference",
                  "engine-owned authorization context is required");
  }
  const auto effective_subject = [&](const EngineUuid& subject_uuid,
                                     const std::string& subject_kind) {
    for (const auto& subject : authorization.effective_subjects) {
      if (subject.subject_uuid.canonical == subject_uuid.canonical &&
          subject.subject_kind == subject_kind) {
        return true;
      }
    }
    return false;
  };
  const auto target_matches = [&](const EngineUuid& target_uuid) {
    return target_uuid.canonical.empty() ||
           target_uuid.canonical == identity.object_uuid.canonical;
  };
  bool select_allowed = false;
  for (const auto& grant : authorization.grants) {
    if (grant.right != "SELECT" || !target_matches(grant.target_uuid) ||
        !effective_subject(grant.subject_uuid, grant.subject_kind)) {
      continue;
    }
    if (grant.deny) {
      return refuse("unauthorized_reference",
                    "SELECT authorization is explicitly denied");
    }
    select_allowed = true;
  }
  for (const auto& policy : authorization.policies) {
    if ((!policy.right.empty() && policy.right != "SELECT") ||
        !target_matches(policy.target_uuid) ||
        !effective_subject(policy.subject_uuid, policy.subject_kind)) {
      continue;
    }
    if (policy.deny) {
      return refuse("unauthorized_reference",
                    "SELECT authorization is denied by bound policy");
    }
  }
  if (!select_allowed) {
    return refuse("unauthorized_reference",
                  "SELECT authorization was not materialized for the resolved object");
  }

  bound_reference->uuid = identity.object_uuid;
  bound_reference->object_kind = identity.resolved_object_type;
  *bound_descriptor = request.descriptors.front();
  return true;
}

}  // namespace scratchbird::engine::internal_api

#endif  // SCRATCHBIRD_TYPED_SCALAR_DESCRIPTOR_CONTRACT_EXTERNAL

#if !defined(SCRATCHBIRD_QOW_TYPED_SCALAR_DESCRIPTOR_CONTRACT_ONLY) && \
    !defined(SCRATCHBIRD_TYPED_SCALAR_DESCRIPTOR_CONTRACT_ONLY)

namespace scratchbird::engine::internal_api {
namespace {

namespace dt = scratchbird::core::datatypes;

template <typename TResult>
TResult ApiSuccess(const EngineRequestContext& context, std::string operation_id) {
  TResult result;
  result.ok = true;
  result.operation_id = std::move(operation_id);
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  return result;
}

template <typename TResult>
TResult ApiFailure(const EngineRequestContext& context, std::string operation_id, EngineApiDiagnostic diagnostic) {
  TResult result;
  result.ok = false;
  result.operation_id = std::move(operation_id);
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

EngineApiDiagnostic DatatypeDiagnosticToApi(const std::string& operation_id,
                                            const dt::DiagnosticRecord& diagnostic) {
  std::string detail;
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      detail = argument.value;
      break;
    }
  }
  return MakeInvalidRequestDiagnostic(operation_id, detail.empty() ? diagnostic.diagnostic_code : detail);
}

EngineTypedValue RequestInputValue(const EngineApiRequest& request, const EngineTypedValue& typed_input) {
  if (!typed_input.descriptor.canonical_type_name.empty() || !typed_input.encoded_value.empty() || typed_input.is_null) {
    return typed_input;
  }
  if (!request.rows.empty() && !request.rows.front().fields.empty()) { return request.rows.front().fields.front().second; }
  if (!request.predicate.bound_values.empty()) { return request.predicate.bound_values.front(); }
  EngineTypedValue value;
  if (!request.descriptors.empty()) { value.descriptor = request.descriptors.front(); }
  return value;
}

EngineTypedValue RequestSecondValue(const EngineApiRequest& request, const EngineTypedValue& typed_input) {
  if (!typed_input.descriptor.canonical_type_name.empty() || !typed_input.encoded_value.empty() || typed_input.is_null) {
    return typed_input;
  }
  if (request.predicate.bound_values.size() >= 2) { return request.predicate.bound_values[1]; }
  if (!request.rows.empty() && request.rows.front().fields.size() >= 2) { return request.rows.front().fields[1].second; }
  EngineTypedValue value;
  if (request.descriptors.size() >= 2) { value.descriptor = request.descriptors[1]; }
  return value;
}

EngineDescriptor RequestTargetDescriptor(const EngineApiRequest& request, const EngineDescriptor& target_descriptor) {
  if (!target_descriptor.canonical_type_name.empty() || !target_descriptor.descriptor_uuid.canonical.empty()) {
    return target_descriptor;
  }
  if (request.descriptors.size() >= 2) { return request.descriptors[1]; }
  if (!request.descriptors.empty()) { return request.descriptors.front(); }
  return {};
}

EngineDescriptor RequestPrimaryDescriptor(const EngineApiRequest& request, const EngineDescriptor& descriptor) {
  if (!descriptor.canonical_type_name.empty() || !descriptor.descriptor_uuid.canonical.empty() ||
      !descriptor.encoded_descriptor.empty()) {
    return descriptor;
  }
  if (!request.descriptors.empty()) { return request.descriptors.front(); }
  if (!request.rows.empty() && !request.rows.front().fields.empty()) {
    return request.rows.front().fields.front().second.descriptor;
  }
  if (!request.predicate.bound_values.empty()) { return request.predicate.bound_values.front().descriptor; }
  return {};
}

std::string DescriptorField(const std::string& descriptor, const std::string& key) {
  const std::string prefix = key + "=";
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const std::size_t end = descriptor.find(';', start);
    const std::string field = descriptor.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (field.rfind(prefix, 0) == 0) { return field.substr(prefix.size()); }
    if (end == std::string::npos) { break; }
    start = end + 1;
  }
  return {};
}

dt::CanonicalTypeId TypeFromDescriptor(const EngineDescriptor& descriptor) {
  return QowCanonicalTypeFromDescriptorV1(descriptor);
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) { return option.substr(prefix.size()); }
  }
  return {};
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

bool HasDomainRight(const EngineRequestContext& context,
                    const std::string& domain_uuid,
                    const std::string& right) {
  return context.security_context_present &&
         SecurityContextHasRight(context, right, domain_uuid);
}

bool TryParseBoolOption(const std::string& value, bool* out) {
  if (out == nullptr || value.empty()) return false;
  std::string lowered = value;
  for (char& c : lowered) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
    *out = true;
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
    *out = false;
    return true;
  }
  return false;
}

bool TryParseU32Option(const std::string& value, std::uint32_t* out) {
  if (out == nullptr || value.empty()) return false;
  std::uint32_t parsed = 0;
  for (const char c : value) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    const auto digit = static_cast<std::uint32_t>(c - '0');
    if (parsed > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
      return false;
    }
    parsed = (parsed * 10) + digit;
  }
  *out = parsed;
  return true;
}

std::string MethodBindingField(const std::string& binding, const std::string& key) {
  const std::string prefix = key + ":";
  for (const auto& part : Split(binding, ';')) {
    if (part.rfind(prefix, 0) == 0) { return part.substr(prefix.size()); }
  }
  return {};
}

std::string LowerValue(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

std::string UpperValue(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
  return value;
}

bool SetOperationKind(const std::string& operation,
                      dt::DatatypeSetOperationKind* out) {
  if (out == nullptr) return false;
  const std::string lowered = LowerValue(operation);
  if (lowered == "membership" || lowered == "contains") {
    *out = dt::DatatypeSetOperationKind::membership;
    return true;
  }
  if (lowered == "equals") {
    *out = dt::DatatypeSetOperationKind::equals;
    return true;
  }
  if (lowered == "subset") {
    *out = dt::DatatypeSetOperationKind::subset;
    return true;
  }
  if (lowered == "superset") {
    *out = dt::DatatypeSetOperationKind::superset;
    return true;
  }
  if (lowered == "cardinality") {
    *out = dt::DatatypeSetOperationKind::cardinality;
    return true;
  }
  return false;
}

bool NumericOperationKind(const std::string& operation, dt::DatatypeNumericOperationKind* out) {
  const std::string lowered = LowerValue(operation);
  if (lowered == "canonicalize" || lowered == "normalize" ||
      lowered == "operator.numeric.canonicalize") {
    *out = dt::DatatypeNumericOperationKind::canonicalize;
    return true;
  }
  if (lowered == "add" || lowered == "+" || lowered == "operator.numeric.add") {
    *out = dt::DatatypeNumericOperationKind::add;
    return true;
  }
  if (lowered == "subtract" || lowered == "sub" || lowered == "-" ||
      lowered == "operator.numeric.subtract") {
    *out = dt::DatatypeNumericOperationKind::subtract;
    return true;
  }
  if (lowered == "multiply" || lowered == "mul" || lowered == "*" ||
      lowered == "operator.numeric.multiply") {
    *out = dt::DatatypeNumericOperationKind::multiply;
    return true;
  }
  if (lowered == "divide" || lowered == "div" || lowered == "/" ||
      lowered == "operator.numeric.divide") {
    *out = dt::DatatypeNumericOperationKind::divide;
    return true;
  }
  if (lowered == "compare" || lowered == "cmp" || lowered == "<=>" ||
      lowered == "operator.numeric.compare") {
    *out = dt::DatatypeNumericOperationKind::compare;
    return true;
  }
  return false;
}

bool RoundingModeKind(const std::string& mode, dt::DatatypeRoundingMode* out) {
  const std::string lowered = LowerValue(mode.empty() ? "half_even" : mode);
  if (lowered == "half_even" || lowered == "half-even" || lowered == "bankers" ||
      lowered == "bankers_rounding") {
    *out = dt::DatatypeRoundingMode::half_even;
    return true;
  }
  if (lowered == "half_up" || lowered == "half-up" || lowered == "away_from_zero" ||
      lowered == "away-from-zero") {
    *out = dt::DatatypeRoundingMode::half_up;
    return true;
  }
  if (lowered == "truncate" || lowered == "trunc" || lowered == "toward_zero" ||
      lowered == "toward-zero") {
    *out = dt::DatatypeRoundingMode::truncate;
    return true;
  }
  return false;
}

bool AdvancedOperationKind(const std::string& operation, dt::AdvancedDatatypeOperationKind* out) {
  const std::string lowered = LowerValue(operation.empty() ? "validate" : operation);
  if (lowered == "validate" || lowered == "operator.advanced.validate") {
    *out = dt::AdvancedDatatypeOperationKind::validate;
    return true;
  }
  if (lowered == "compare" || lowered == "operator.advanced.compare") {
    *out = dt::AdvancedDatatypeOperationKind::compare;
    return true;
  }
  if (lowered == "hash" || lowered == "operator.advanced.hash") {
    *out = dt::AdvancedDatatypeOperationKind::hash;
    return true;
  }
  if (lowered == "contains" || lowered == "spatial_contains" || lowered == "spatial.contains" ||
      lowered == "operator.spatial.contains") {
    *out = dt::AdvancedDatatypeOperationKind::contains;
    return true;
  }
  if (lowered == "intersects" || lowered == "spatial_intersects" ||
      lowered == "spatial.intersects" || lowered == "operator.spatial.intersects") {
    *out = dt::AdvancedDatatypeOperationKind::intersects;
    return true;
  }
  if (lowered == "distance" || lowered == "spatial_distance" || lowered == "spatial.distance" ||
      lowered == "vector_distance" || lowered == "vector.distance" ||
      lowered == "operator.spatial.distance" || lowered == "operator.vector.distance") {
    *out = dt::AdvancedDatatypeOperationKind::distance;
    return true;
  }
  if (lowered == "nearest_neighbor" || lowered == "nearest-neighbor" ||
      lowered == "nearest_neighbour" || lowered == "vector_nearest_neighbor" ||
      lowered == "vector.nearest_neighbor" || lowered == "operator.vector.nearest_neighbor") {
    *out = dt::AdvancedDatatypeOperationKind::nearest_neighbor;
    return true;
  }
  if (lowered == "tokenize" || lowered == "search_tokenize" || lowered == "search.tokenize") {
    *out = dt::AdvancedDatatypeOperationKind::tokenize;
    return true;
  }
  if (lowered == "search_match" || lowered == "search-match" || lowered == "search.match" ||
      lowered == "operator.search.match") {
    *out = dt::AdvancedDatatypeOperationKind::search_match;
    return true;
  }
  if (lowered == "rank" || lowered == "search_rank" || lowered == "search.rank" ||
      lowered == "operator.search.rank") {
    *out = dt::AdvancedDatatypeOperationKind::rank;
    return true;
  }
  if (lowered == "graph_traverse" || lowered == "graph-traverse" ||
      lowered == "graph.traverse") {
    *out = dt::AdvancedDatatypeOperationKind::graph_traverse;
    return true;
  }
  if (lowered == "path_match" || lowered == "path-match" || lowered == "graph_path_match" ||
      lowered == "graph.path_match" || lowered == "operator.graph.path_match") {
    *out = dt::AdvancedDatatypeOperationKind::path_match;
    return true;
  }
  if (lowered == "append_point" || lowered == "append-point" ||
      lowered == "time_series_append_point" || lowered == "time_series.append_point") {
    *out = dt::AdvancedDatatypeOperationKind::append_point;
    return true;
  }
  if (lowered == "aggregate_window" || lowered == "aggregate-window" ||
      lowered == "time_series_aggregate_window" || lowered == "time_series.aggregate_window") {
    *out = dt::AdvancedDatatypeOperationKind::aggregate_window;
    return true;
  }
  if (lowered == "estimate" || lowered == "sketch_estimate" ||
      lowered == "sketch.estimate" || lowered == "operator.sketch.estimate") {
    *out = dt::AdvancedDatatypeOperationKind::estimate;
    return true;
  }
  if (lowered == "merge" || lowered == "sketch_merge" || lowered == "sketch.merge" ||
      lowered == "aggregate_state_merge" || lowered == "aggregate_state.merge" ||
      lowered == "operator.aggregate_state.merge") {
    *out = dt::AdvancedDatatypeOperationKind::merge;
    return true;
  }
  if (lowered == "resolve_locator" || lowered == "locator_resolve" ||
      lowered == "locator.resolve" || lowered == "operator.locator.resolve") {
    *out = dt::AdvancedDatatypeOperationKind::resolve_locator;
    return true;
  }
  return false;
}

bool AdvancedIndexKind(const std::string& index, dt::AdvancedDatatypeIndexKind* out) {
  const std::string lowered = LowerValue(index.empty() ? "none" : index);
  if (lowered == "none") { *out = dt::AdvancedDatatypeIndexKind::none; return true; }
  if (lowered == "btree") { *out = dt::AdvancedDatatypeIndexKind::btree; return true; }
  if (lowered == "rtree") { *out = dt::AdvancedDatatypeIndexKind::rtree; return true; }
  if (lowered == "geohash") { *out = dt::AdvancedDatatypeIndexKind::geohash; return true; }
  if (lowered == "inverted") { *out = dt::AdvancedDatatypeIndexKind::inverted; return true; }
  if (lowered == "hnsw") { *out = dt::AdvancedDatatypeIndexKind::hnsw; return true; }
  if (lowered == "ivfflat" || lowered == "ivf_flat") { *out = dt::AdvancedDatatypeIndexKind::ivfflat; return true; }
  if (lowered == "adjacency" || lowered == "graph_adjacency") { *out = dt::AdvancedDatatypeIndexKind::adjacency; return true; }
  if (lowered == "time_partition" || lowered == "time-partition") { *out = dt::AdvancedDatatypeIndexKind::time_partition; return true; }
  if (lowered == "sketch_summary" || lowered == "sketch-summary") { *out = dt::AdvancedDatatypeIndexKind::sketch_summary; return true; }
  if (lowered == "aggregate_state" || lowered == "aggregate-state") { *out = dt::AdvancedDatatypeIndexKind::aggregate_state; return true; }
  if (lowered == "locator_exact" || lowered == "locator-exact") { *out = dt::AdvancedDatatypeIndexKind::locator_exact; return true; }
  return false;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_EXPRESSION_API_BEHAVIOR
EngineBindExpressionResult EngineBindExpression(const EngineBindExpressionRequest& request) {
  const bool canonical_reference_requested =
      !request.sql_object_reference.expected_object_type.empty() ||
      !request.sql_object_reference.object_name.raw_text.empty() ||
      !request.sql_object_reference.object_name.normalized_lookup_key.empty() ||
      !request.bound_object_identity.object_uuid.canonical.empty() ||
      !request.related_objects.empty();
  if (!canonical_reference_requested) {
    return ApiFailure<EngineBindExpressionResult>(
        request.context, "query.bind_expression",
        MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-025-BIND-REFUSAL-V1",
            "engine.query.expression_binding_refused",
            "canonical expression reference binding input is required"));
  }
  const auto refuse = [&](std::string reason, std::string detail) {
    auto diagnostic = MakeEngineApiDiagnostic(
        "QOW-DIAG-QRY-025-REFUSAL-V1",
        "engine.query.expression_reference_binding_refused",
        std::move(detail));
    diagnostic.fields.push_back({"reason", std::move(reason)});
    return ApiFailure<EngineBindExpressionResult>(
        request.context, "query.bind_expression", std::move(diagnostic));
  };

  const auto& context = request.context;
  EngineObjectReference bound_reference;
  EngineDescriptor bound_descriptor;
  std::string refusal_reason;
  std::string refusal_detail;
  if (!QowBindCanonicalExpressionReferenceV1(
          request, &bound_reference, &bound_descriptor, &refusal_reason,
          &refusal_detail)) {
    return refuse(std::move(refusal_reason), std::move(refusal_detail));
  }
  const auto authorization = EvaluateMaterializedAuthorization(
      context, context.authorization_context, "SELECT",
      bound_reference.uuid.canonical);
  if (!authorization.authorized) {
    return refuse(
        "unauthorized_reference",
        "SELECT authorization was not materialized for the resolved object");
  }

  auto result = ApiSuccess<EngineBindExpressionResult>(
      context, "query.bind_expression");
  result.bound_reference = bound_reference;
  result.bound_identity = request.bound_object_identity;
  result.bound_descriptor = bound_descriptor;
  result.primary_object = std::move(bound_reference);
  result.result_shape.result_kind = "bound_expression_reference";
  result.result_shape.columns.push_back(std::move(bound_descriptor));
  result.evidence.push_back(
      {"query_binding", "canonical_expression_reference"});
  result.evidence.push_back(
      {"statement_metadata_snapshot",
       context.statement_metadata_snapshot_uuid.canonical});
  result.evidence.push_back(
      {"bound_object_uuid", result.primary_object.uuid.canonical});
  return result;
}

EngineCastValueResult EngineCastValue(const EngineCastValueRequest& request) {
  const EngineTypedValue input = RequestInputValue(request, request.input_value);
  const EngineDescriptor target = RequestTargetDescriptor(request, request.target_descriptor);
  if (target.canonical_type_name.empty()) {
    return ApiFailure<EngineCastValueResult>(
        request.context,
        "query.cast_value",
        MakeInvalidRequestDiagnostic("query.cast_value", "target_descriptor_required"));
  }
  const bool domain_descriptor_route =
      !DomainUuidFromDescriptor(input.descriptor).empty() ||
      !DomainUuidFromDescriptor(target).empty();
  const bool canonical_descriptor_route =
      !domain_descriptor_route &&
      (!input.descriptor.descriptor_uuid.canonical.empty() ||
       !target.descriptor_uuid.canonical.empty());
  if (canonical_descriptor_route) {
    EngineTypedValue coerced;
    std::string category;
    std::string refusal_detail;
    if (!QowApplyCanonicalDescriptorCoercionV1(
            input, target, request.explicit_cast, &coerced, &category,
            &refusal_detail)) {
      return ApiFailure<EngineCastValueResult>(
          request.context,
          "query.cast_value",
          MakeEngineApiDiagnostic(
              "QOW-DIAG-QRY-008-COERCE-REFUSAL-V1",
              "engine.query.typed_scalar_coercion_refused",
              std::move(refusal_detail)));
    }
    auto result =
        ApiSuccess<EngineCastValueResult>(request.context, "query.cast_value");
    result.value = std::move(coerced);
    result.cast_category = std::move(category);
    result.result_shape.result_kind = "typed_value";
    result.result_shape.columns.push_back(result.value.descriptor);
    result.evidence.push_back({"datatype_cast", result.cast_category});
    result.evidence.push_back(
        {"canonical_descriptor_identity",
         result.value.descriptor.descriptor_uuid.canonical});
    return result;
  }
  const auto source_type = TypeFromDescriptor(input.descriptor);
  const auto target_type = TypeFromDescriptor(target);
  if (source_type == dt::CanonicalTypeId::unknown ||
      target_type == dt::CanonicalTypeId::unknown) {
    return ApiFailure<EngineCastValueResult>(
        request.context,
        "query.cast_value",
        MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-028-DESC-REFUSAL-V1",
            "engine.query.invalid_descriptor_state_refused",
            "source and target descriptors must resolve to canonical types"));
  }
  dt::DatatypeCastRequest cast_request;
  cast_request.value.type_id = source_type;
  cast_request.value.encoded_value = input.encoded_value;
  cast_request.value.is_null = input.is_null;
  cast_request.target_type_id = target_type;
  cast_request.explicit_cast = request.explicit_cast;
  cast_request.reference_compatibility_profile = !request.compatibility_profile.names.empty();
  const auto cast = dt::CastDatatypeValue(cast_request);
  if (!cast.ok()) {
    return ApiFailure<EngineCastValueResult>(
        request.context,
        "query.cast_value",
        DatatypeDiagnosticToApi("query.cast_value", cast.diagnostic));
  }
  if (!DomainUuidFromDescriptor(target).empty()) {
    EngineTypedValue candidate;
    candidate.descriptor.descriptor_kind = "scalar";
    candidate.descriptor.canonical_type_name = dt::CanonicalTypeName(cast.value.type_id);
    candidate.encoded_value = cast.value.encoded_value;
    candidate.is_null = cast.value.is_null;
    candidate.state = cast.value.is_null ? EngineValueState::sql_null
                                         : EngineValueState::value;
    if (candidate.is_null) candidate.encoded_value.clear();
    const auto validation = ValidateDomainTypedValue(request.context,
                                                    target,
                                                    candidate,
                                                    request.context.local_transaction_id);
    if (!validation.ok) {
      return ApiFailure<EngineCastValueResult>(
          request.context,
          "query.cast_value",
          validation.diagnostic);
    }
    auto result = ApiSuccess<EngineCastValueResult>(request.context, "query.cast_value");
    result.value = validation.value;
    result.cast_category = dt::DatatypeCastCategoryName(dt::DatatypeCastCategory::base_to_domain);
    result.result_shape.result_kind = "typed_value";
    result.result_shape.columns.push_back(result.value.descriptor);
    result.evidence.push_back({"datatype_cast", result.cast_category});
    for (const auto& evidence : validation.evidence) { result.evidence.push_back(evidence); }
    return result;
  }
  auto result = ApiSuccess<EngineCastValueResult>(request.context, "query.cast_value");
  result.value.descriptor = target;
  result.value.encoded_value = cast.value.encoded_value;
  result.value.is_null = cast.value.is_null;
  result.value.state = cast.value.is_null ? EngineValueState::sql_null
                                          : EngineValueState::value;
  if (result.value.is_null) result.value.encoded_value.clear();
  result.cast_category = dt::DatatypeCastCategoryName(cast.category);
  result.result_shape.result_kind = "typed_value";
  result.result_shape.columns.push_back(target);
  result.evidence.push_back({"datatype_cast", result.cast_category});
  return result;
}

EngineCompareScalarValuesResult EngineCompareScalarValues(
    const EngineCompareScalarValuesRequest& request) {
  const auto& context = request.borrowed_context == nullptr
                            ? request.context
                            : *request.borrowed_context;
  EngineTypedValue owned_left;
  EngineTypedValue owned_right;
  const auto& left = request.borrowed_left_value == nullptr
                         ? (owned_left =
                                RequestInputValue(request, request.left_value))
                         : *request.borrowed_left_value;
  const auto& right = request.borrowed_right_value == nullptr
                          ? (owned_right = RequestSecondValue(
                                 request, request.right_value))
                          : *request.borrowed_right_value;
  const std::string left_collation = DescriptorField(
      left.descriptor.encoded_descriptor, "collation_uuid");
  const std::string right_collation = DescriptorField(
      right.descriptor.encoded_descriptor, "collation_uuid");
  auto refuse = [&](std::string detail) {
    return ApiFailure<EngineCompareScalarValuesResult>(
        context,
        "query.compare_scalar_values",
        MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-008-COLLATION-REFUSAL-V1",
            "engine.query.typed_scalar_collation_refused",
            std::move(detail)));
  };
  if (left_collation.empty() || left_collation != right_collation) {
    return refuse("bound scalar collation UUID is absent or mismatched");
  }
  EngineUuid collation_uuid;
  collation_uuid.canonical = left_collation;
  const auto resolved = LookupEngineResourceDescriptorByUuid(
      context, collation_uuid, "collation");
  if (!resolved.ok || !resolved.resource_descriptor.present ||
      resolved.resource_descriptor.resource_uuid.canonical != left_collation) {
    const std::string detail = resolved.diagnostic.code.empty()
                                   ? "bound collation resource was not resolved"
                                   : resolved.diagnostic.code;
    return refuse(detail);
  }
  scratchbird::core::datatypes::DatatypeTextSeedAuthority text_seed;
  text_seed.active = true;
  text_seed.seed_pack_name = resolved.resource_descriptor.seed_pack_name;
  text_seed.seed_pack_version = resolved.resource_descriptor.seed_pack_version;
  text_seed.charset_name =
      resolved.resource_descriptor.parent_canonical_name;
  text_seed.collation_name = resolved.resource_descriptor.canonical_name;
  text_seed.collation_case_insensitive =
      resolved.resource_descriptor.case_insensitive;
  text_seed.collation_accent_insensitive =
      resolved.resource_descriptor.accent_insensitive;
  if (left.isSqlNull() || right.isSqlNull()) {
    EngineSqlTruthValue truth = EngineSqlTruthValue::unknown;
    std::string refusal_detail;
    if (!QowEvaluateCanonicalComparisonTruthV1(
            left, right, 0, EngineComparisonPredicateOperator::equal,
            &truth, &refusal_detail) ||
        truth != EngineSqlTruthValue::unknown) {
      return refuse(std::move(refusal_detail));
    }
    auto result = ApiSuccess<EngineCompareScalarValuesResult>(
        context, "query.compare_scalar_values");
    result.comparison = 0;
    result.collation_uuid = std::move(collation_uuid);
    result.collation_epoch = resolved.resource_descriptor.family_epoch;
    result.evidence.push_back(
        {"canonical_collation_identity", left_collation});
    result.evidence.push_back(
        {"collation_epoch", std::to_string(result.collation_epoch)});
    result.evidence.push_back(
        {"sql_truth_value", EngineSqlTruthValueName(truth)});
    return result;
  }
  int comparison = 0;
  std::string refusal_detail;
  if (!QowCompareCanonicalCollatedScalarsV1(
          left, right, left_collation,
          resolved.resource_descriptor.resource_epoch,
          resolved.resource_descriptor.family_epoch, text_seed, &comparison,
          &refusal_detail)) {
    return refuse(std::move(refusal_detail));
  }
  auto result = ApiSuccess<EngineCompareScalarValuesResult>(
      context, "query.compare_scalar_values");
  result.comparison = comparison;
  result.collation_uuid = std::move(collation_uuid);
  result.collation_epoch = resolved.resource_descriptor.family_epoch;
  result.evidence.push_back(
      {"canonical_collation_identity", left_collation});
  result.evidence.push_back(
      {"collation_epoch", std::to_string(result.collation_epoch)});
  return result;
}

EngineNormalizeTimezoneScalarResult EngineNormalizeTimezoneScalar(
    const EngineNormalizeTimezoneScalarRequest& request) {
  const auto& context = request.borrowed_context == nullptr
                            ? request.context
                            : *request.borrowed_context;
  EngineTypedValue owned_input;
  const auto& input = request.borrowed_input_value == nullptr
                          ? (owned_input = RequestInputValue(
                                 request, request.input_value))
                          : *request.borrowed_input_value;
  const auto resolved = LookupEngineTimezoneSeedAuthority(context);
  auto refuse = [&](std::string detail) {
    return ApiFailure<EngineNormalizeTimezoneScalarResult>(
        context,
        "query.normalize_timezone_scalar",
        MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-008-TIMEZONE-REFUSAL-V1",
            "engine.query.typed_scalar_timezone_refused",
            std::move(detail)));
  };
  if (!resolved.ok) {
    return refuse(resolved.diagnostic.code.empty()
                      ? "bound timezone seed authority was not resolved"
                      : resolved.diagnostic.code);
  }
  scratchbird::core::datatypes::TimezoneSeedAuthority timezone_seed;
  timezone_seed.active = resolved.authority.active;
  timezone_seed.seed_pack_name = resolved.authority.seed_pack_name;
  timezone_seed.seed_pack_version = resolved.authority.seed_pack_version;
  timezone_seed.content_hash = resolved.authority.content_hash;
  timezone_seed.timezone_records = resolved.authority.timezone_records;
  timezone_seed.timezone_transition_records =
      resolved.authority.timezone_transition_records;
  timezone_seed.timezone_leap_second_records =
      resolved.authority.timezone_leap_second_records;
  timezone_seed.timezone_names = resolved.authority.timezone_names;
  EngineTypedValue output;
  std::string timezone_identifier;
  int timezone_offset_minutes = 0;
  bool used_timezone_seed = false;
  std::string refusal_detail;
  if (!QowNormalizeCanonicalTimezoneScalarV1(
          input, timezone_seed, resolved.authority.resource_epoch,
          resolved.authority.timezone_epoch, &output, &timezone_identifier,
          &timezone_offset_minutes, &used_timezone_seed, &refusal_detail)) {
    return refuse(std::move(refusal_detail));
  }
  dt::ReferenceTemporalWireProfileResult comparable;
  if (!input.isSqlNull()) {
    dt::ReferenceTemporalWireProfileRequest comparable_request;
    comparable_request.reference_engine = "scratchbird_native";
    comparable_request.reference_type_or_family =
        input.descriptor.canonical_type_name;
    comparable_request.wire_profile = DescriptorField(
        input.descriptor.encoded_descriptor, "timezone_profile_id");
    comparable_request.encoded_value = input.encoded_value;
    comparable_request.timezone_seed = timezone_seed;
    comparable = dt::ValidateReferenceTemporalWireProfile(
        comparable_request);
    if (!comparable.ok()) {
      return refuse(
          comparable.diagnostic.diagnostic_code.empty()
              ? "canonical timezone comparable-key binding refused"
              : comparable.diagnostic.diagnostic_code);
    }
  }
  auto result = ApiSuccess<EngineNormalizeTimezoneScalarResult>(
      context, "query.normalize_timezone_scalar");
  result.value = std::move(output);
  result.timezone_identifier = std::move(timezone_identifier);
  result.timezone_offset_minutes = timezone_offset_minutes;
  result.used_timezone_seed = used_timezone_seed;
  result.comparable_utc_key_available =
      comparable.comparable_utc_key_available;
  result.comparable_utc_whole_seconds =
      comparable.comparable_utc_whole_seconds;
  result.comparable_fractional_picoseconds =
      comparable.comparable_fractional_picoseconds;
  result.timezone_epoch = resolved.authority.timezone_epoch;
  result.result_shape.result_kind = "typed_value";
  result.result_shape.columns.push_back(result.value.descriptor);
  result.evidence.push_back(
      {"timezone_seed_version", resolved.authority.seed_pack_version});
  result.evidence.push_back(
      {"timezone_epoch", std::to_string(result.timezone_epoch)});
  return result;
}

EngineCompareCanonicalScalarValuesResult EngineCompareCanonicalScalarValues(
    const EngineCompareCanonicalScalarValuesRequest& request) {
  constexpr const char* kOperation = "query.compare_canonical_scalar_values";
  const auto& context = request.borrowed_context == nullptr
                            ? request.context
                            : *request.borrowed_context;
  EngineTypedValue owned_left;
  EngineTypedValue owned_right;
  const auto& left = request.borrowed_left_value == nullptr
                         ? (owned_left =
                                RequestInputValue(request, request.left_value))
                         : *request.borrowed_left_value;
  const auto& right = request.borrowed_right_value == nullptr
                          ? (owned_right = RequestSecondValue(
                                 request, request.right_value))
                          : *request.borrowed_right_value;
  const auto refuse = [&](std::string detail) {
    return ApiFailure<EngineCompareCanonicalScalarValuesResult>(
        context, kOperation,
        MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-008-COMPARISON-AUTHORITY-REFUSAL-V1",
            "engine.query.typed_scalar_comparison_refused",
            std::move(detail)));
  };
  const auto propagate_failure = [&](const EngineApiResult& failed) {
    EngineCompareCanonicalScalarValuesResult result;
    static_cast<EngineApiResult&>(result) = failed;
    result.operation_id = kOperation;
    return result;
  };

  const auto left_type = dt::CanonicalTypeIdFromStableName(
      left.descriptor.canonical_type_name);
  const auto right_type = dt::CanonicalTypeIdFromStableName(
      right.descriptor.canonical_type_name);
  const auto left_timezone_profile = DescriptorField(
      left.descriptor.encoded_descriptor, "timezone_profile_id");
  const auto right_timezone_profile = DescriptorField(
      right.descriptor.encoded_descriptor, "timezone_profile_id");
  if (left_type == dt::CanonicalTypeId::unknown ||
      left_type != right_type ||
      left_timezone_profile != right_timezone_profile) {
    return refuse(
        "comparison operands do not share one canonical scalar type and "
        "timezone profile");
  }

  if (left.isSqlNull() || right.isSqlNull()) {
    EngineSqlTruthValue truth = EngineSqlTruthValue::unspecified;
    std::string detail;
    if (!QowEvaluateCanonicalComparisonTruthV1(
            left, right, 0, EngineComparisonPredicateOperator::equal,
            &truth, &detail) || truth != EngineSqlTruthValue::unknown) {
      return refuse(detail.empty()
                        ? "canonical NULL comparison was not unknown"
                        : std::move(detail));
    }
    auto result = ApiSuccess<EngineCompareCanonicalScalarValuesResult>(
        context, kOperation);
    result.evidence.push_back(
        {"sql_truth_value", EngineSqlTruthValueName(truth)});
    return result;
  }

  if (left_type == dt::CanonicalTypeId::character) {
    EngineCompareScalarValuesRequest collated_request;
    collated_request.borrowed_context = &context;
    collated_request.borrowed_left_value = &left;
    collated_request.borrowed_right_value = &right;
    const auto collated = EngineCompareScalarValues(collated_request);
    if (!collated.ok) return propagate_failure(collated);
    if (collated.comparison < -1 || collated.comparison > 1) {
      return refuse(
          "collation authority returned a noncanonical scalar order");
    }
    auto result = ApiSuccess<EngineCompareCanonicalScalarValuesResult>(
        context, kOperation);
    result.comparison = collated.comparison;
    result.evidence = collated.evidence;
    return result;
  }

  if (!left_timezone_profile.empty()) {
    if (left_type != dt::CanonicalTypeId::time &&
        left_type != dt::CanonicalTypeId::timestamp) {
      return refuse(
          "timezone comparison operands do not share one supported temporal "
          "profile");
    }
    EngineNormalizeTimezoneScalarRequest left_request;
    left_request.borrowed_context = &context;
    left_request.borrowed_input_value = &left;
    const auto left_normalized =
        EngineNormalizeTimezoneScalar(left_request);
    if (!left_normalized.ok) return propagate_failure(left_normalized);
    EngineNormalizeTimezoneScalarRequest right_request;
    right_request.borrowed_context = &context;
    right_request.borrowed_input_value = &right;
    const auto right_normalized =
        EngineNormalizeTimezoneScalar(right_request);
    if (!right_normalized.ok) return propagate_failure(right_normalized);
    if (left_normalized.timezone_epoch != right_normalized.timezone_epoch ||
        !left_normalized.comparable_utc_key_available ||
        !right_normalized.comparable_utc_key_available) {
      return refuse(
          left_normalized.timezone_epoch != right_normalized.timezone_epoch
              ? "timezone comparison authority changed between operands"
              : "named-zone operands require a resolved transition instant");
    }
    const auto left_key = std::tie(
        left_normalized.comparable_utc_whole_seconds,
        left_normalized.comparable_fractional_picoseconds);
    const auto right_key = std::tie(
        right_normalized.comparable_utc_whole_seconds,
        right_normalized.comparable_fractional_picoseconds);
    auto result = ApiSuccess<EngineCompareCanonicalScalarValuesResult>(
        context, kOperation);
    result.comparison =
        left_key < right_key ? -1 : (right_key < left_key ? 1 : 0);
    result.evidence.push_back(
        {"timezone_epoch", std::to_string(left_normalized.timezone_epoch)});
    return result;
  }

  int comparison = 0;
  std::string detail;
  if (!QowCompareCanonicalNonCollatedScalarsV1(
          left, right, &comparison, &detail)) {
    return refuse(std::move(detail));
  }
  if (comparison < -1 || comparison > 1) {
    return refuse("canonical scalar comparator returned a noncanonical order");
  }
  auto result = ApiSuccess<EngineCompareCanonicalScalarValuesResult>(
      context, kOperation);
  result.comparison = comparison;
  result.evidence.push_back(
      {"comparison_authority", "canonical_noncollated"});
  return result;
}

EngineExtractValueResult EngineExtractValue(const EngineExtractValueRequest& request) {
  const EngineTypedValue input = RequestInputValue(request, request.input_value);
  const std::string field = !request.field.empty() ? request.field : OptionValue(request, "field:");
  if (field.empty()) {
    return ApiFailure<EngineExtractValueResult>(
        request.context,
        "query.extract_value",
        MakeInvalidRequestDiagnostic("query.extract_value", "extract_field_required"));
  }
  dt::DatatypeExtractRequest extract_request;
  extract_request.value.type_id = TypeFromDescriptor(input.descriptor);
  extract_request.value.encoded_value = input.encoded_value;
  extract_request.value.is_null = input.is_null;
  extract_request.field = field;
  const auto extracted = dt::ExtractDatatypeField(extract_request);
  if (!extracted.ok()) {
    return ApiFailure<EngineExtractValueResult>(
        request.context,
        "query.extract_value",
        DatatypeDiagnosticToApi("query.extract_value", extracted.diagnostic));
  }
  auto result = ApiSuccess<EngineExtractValueResult>(request.context, "query.extract_value");
  result.value.descriptor.canonical_type_name = dt::CanonicalTypeName(extracted.value.type_id);
  result.value.descriptor.descriptor_kind = "scalar";
  result.value.encoded_value = extracted.value.encoded_value;
  result.value.is_null = extracted.value.is_null;
  result.value.state = extracted.value.is_null ? EngineValueState::sql_null
                                               : EngineValueState::value;
  if (result.value.is_null) result.value.encoded_value.clear();
  result.result_shape.result_kind = "typed_value";
  result.result_shape.columns.push_back(result.value.descriptor);
  result.evidence.push_back({"datatype_extract", field});
  return result;
}

EngineSetOperationResult EngineSetOperation(const EngineSetOperationRequest& request) {
  const EngineTypedValue left = RequestInputValue(request, request.left_set);
  const EngineTypedValue right =
      RequestSecondValue(request, request.right_set_or_value);
  const std::string operation = !request.set_operation.empty() ? request.set_operation : OptionValue(request, "set_operation:");
  dt::DatatypeSetOperationRequest set_request;
  if (!SetOperationKind(operation, &set_request.operation)) {
    return ApiFailure<EngineSetOperationResult>(
        request.context,
        "query.set_operation",
        MakeInvalidRequestDiagnostic("query.set_operation",
                                     "set_operation_required_or_unsupported:" +
                                         operation));
  }
  if (request.descriptors.empty()) {
    return ApiFailure<EngineSetOperationResult>(
        request.context,
        "query.set_operation",
        MakeInvalidRequestDiagnostic("query.set_operation",
                                     "set_element_descriptor_required"));
  }
  set_request.descriptor.element_type_id =
      TypeFromDescriptor(request.descriptors.front());
  if (set_request.descriptor.element_type_id ==
      dt::CanonicalTypeId::unknown) {
    return ApiFailure<EngineSetOperationResult>(
        request.context,
        "query.set_operation",
        MakeInvalidRequestDiagnostic("query.set_operation",
                                     "set_element_descriptor_unresolved"));
  }
  if (left.descriptor.canonical_type_name.empty() ||
      right.descriptor.canonical_type_name.empty() ||
      (left.isSqlNull() && !QowCanonicalSqlNullStateV1(left)) ||
      (right.isSqlNull() && !QowCanonicalSqlNullStateV1(right)) ||
      (!left.isSqlNull() && left.state != EngineValueState::value) ||
      (!right.isSqlNull() && right.state != EngineValueState::value)) {
    return ApiFailure<EngineSetOperationResult>(
        request.context,
        "query.set_operation",
        MakeInvalidRequestDiagnostic("query.set_operation",
                                     "set_operand_descriptor_or_state_invalid"));
  }
  set_request.left_encoded_set = left.encoded_value;
  set_request.right_encoded_set_or_value = right.encoded_value;
  const auto set_result = dt::ApplySetOperation(set_request);
  if (!set_result.ok()) {
    return ApiFailure<EngineSetOperationResult>(
        request.context,
        "query.set_operation",
        DatatypeDiagnosticToApi("query.set_operation", set_result.diagnostic));
  }
  auto result = ApiSuccess<EngineSetOperationResult>(request.context, "query.set_operation");
  result.value.descriptor.canonical_type_name = dt::CanonicalTypeName(set_result.value.type_id);
  result.value.descriptor.descriptor_kind = "scalar";
  result.value.encoded_value = set_result.value.encoded_value;
  result.value.is_null = set_result.value.is_null;
  result.value.state = set_result.value.is_null ? EngineValueState::sql_null
                                                : EngineValueState::value;
  if (result.value.is_null) result.value.encoded_value.clear();
  result.result_shape.result_kind = "typed_value";
  result.result_shape.columns.push_back(result.value.descriptor);
  result.evidence.push_back({"datatype_set_operation", LowerValue(operation)});
  return result;
}

EngineApplyNumericOperationResult EngineApplyNumericOperation(const EngineApplyNumericOperationRequest& request) {
  const EngineTypedValue left = RequestInputValue(request, request.left_value);
  const EngineTypedValue right = RequestSecondValue(request, request.right_value);
  const EngineDescriptor result_descriptor =
      request.descriptors.empty() ? left.descriptor : request.descriptors.front();
  const bool canonical_descriptor_route =
      !left.descriptor.descriptor_uuid.canonical.empty() ||
      !right.descriptor.descriptor_uuid.canonical.empty() ||
      !result_descriptor.descriptor_uuid.canonical.empty();
  if (canonical_descriptor_route &&
      (!QowCanonicalDescriptorIdentityV1(left.descriptor) ||
       !QowCanonicalDescriptorIdentityV1(right.descriptor) ||
       !QowCanonicalDescriptorIdentityV1(result_descriptor))) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-008-DESC-REFUSAL-V1",
            "engine.query.typed_scalar_descriptor_refused",
            "canonical descriptor identity is missing or malformed"));
  }
  if (canonical_descriptor_route &&
      ((left.isSqlNull() && !QowCanonicalSqlNullStateV1(left)) ||
       (right.isSqlNull() && !QowCanonicalSqlNullStateV1(right)))) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-008-NULL-REFUSAL-V1",
            "engine.query.typed_scalar_null_refused",
            "SQL NULL scalar operands cannot carry substitute payload bytes"));
  }
  const std::string operation_text = !request.numeric_operation.empty()
      ? request.numeric_operation
      : OptionValue(request, "numeric_operation:");
  dt::DatatypeNumericOperationKind operation;
  if (!NumericOperationKind(operation_text, &operation)) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        MakeInvalidRequestDiagnostic("query.apply_numeric_operation", "numeric_operation_unsupported:" + operation_text));
  }
  const auto value_state_valid = [](const EngineTypedValue& value) {
    return value.isSqlNull()
               ? QowCanonicalSqlNullStateV1(value)
               : value.state == EngineValueState::value && !value.is_null;
  };
  if (left.descriptor.canonical_type_name.empty() ||
      result_descriptor.canonical_type_name.empty() ||
      !value_state_valid(left) ||
      (operation != dt::DatatypeNumericOperationKind::canonicalize &&
       (right.descriptor.canonical_type_name.empty() ||
        !value_state_valid(right)))) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        MakeInvalidRequestDiagnostic("query.apply_numeric_operation",
                                     "numeric_operand_descriptor_or_state_invalid"));
  }
  dt::DatatypeRoundingMode rounding;
  const std::string rounding_text = !request.rounding_mode.empty()
      ? request.rounding_mode
      : OptionValue(request, "rounding:");
  if (!RoundingModeKind(rounding_text, &rounding)) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        MakeInvalidRequestDiagnostic("query.apply_numeric_operation", "numeric_rounding_mode_unsupported:" + rounding_text));
  }

  dt::DatatypeNumericOperationRequest numeric_request;
  numeric_request.operation = operation;
  numeric_request.type_id = request.descriptors.empty() ? TypeFromDescriptor(left.descriptor) : TypeFromDescriptor(request.descriptors.front());
  numeric_request.left.type_id = TypeFromDescriptor(left.descriptor);
  numeric_request.left.encoded_value = left.encoded_value;
  numeric_request.left.is_null = left.isSqlNull();
  numeric_request.right.type_id = TypeFromDescriptor(right.descriptor);
  numeric_request.right.encoded_value = right.encoded_value;
  numeric_request.right.is_null = right.isSqlNull();
  if (numeric_request.type_id == dt::CanonicalTypeId::unknown ||
      numeric_request.left.type_id == dt::CanonicalTypeId::unknown ||
      (operation != dt::DatatypeNumericOperationKind::canonicalize &&
       numeric_request.right.type_id == dt::CanonicalTypeId::unknown)) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        MakeInvalidRequestDiagnostic("query.apply_numeric_operation",
                                     "numeric_operand_descriptor_unresolved"));
  }
  numeric_request.context.precision = request.precision;
  const std::string precision_text = OptionValue(request, "precision:");
  if (!precision_text.empty() &&
      !TryParseU32Option(precision_text,
                         &numeric_request.context.precision)) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        MakeInvalidRequestDiagnostic("query.apply_numeric_operation",
                                     "numeric_precision_invalid"));
  }
  numeric_request.context.scale = request.scale;
  const std::string scale_text = OptionValue(request, "scale:");
  if (!scale_text.empty() &&
      !TryParseU32Option(scale_text, &numeric_request.context.scale)) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        MakeInvalidRequestDiagnostic("query.apply_numeric_operation",
                                     "numeric_scale_invalid"));
  }
  numeric_request.context.rounding = rounding;
  numeric_request.context.allow_special_values = request.allow_special_values;
  const std::string allow_special_values_text =
      OptionValue(request, "allow_special_values:");
  if (!allow_special_values_text.empty() &&
      !TryParseBoolOption(allow_special_values_text,
                          &numeric_request.context.allow_special_values)) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        MakeInvalidRequestDiagnostic("query.apply_numeric_operation",
                                     "allow_special_values_invalid"));
  }

  if (canonical_descriptor_route) {
    const auto canonical_result_type = dt::CanonicalTypeIdFromStableName(
        result_descriptor.canonical_type_name);
    if (canonical_result_type == dt::CanonicalTypeId::decimal ||
        canonical_result_type == dt::CanonicalTypeId::decimal_float) {
      std::uint32_t bound_precision = 0;
      std::uint32_t bound_scale = 0;
      if (QowCanonicalDescriptorU32FieldV1(
              result_descriptor.encoded_descriptor, "precision",
              &bound_precision) &&
          QowCanonicalDescriptorU32FieldV1(
              result_descriptor.encoded_descriptor, "scale", &bound_scale)) {
        numeric_request.context.precision = bound_precision;
        numeric_request.context.scale = bound_scale;
      }
    } else if (canonical_result_type == dt::CanonicalTypeId::int128 ||
               canonical_result_type == dt::CanonicalTypeId::uint128 ||
               canonical_result_type == dt::CanonicalTypeId::real128) {
      numeric_request.context.precision = 38;
      numeric_request.context.scale = 0;
    }
    if (operation == dt::DatatypeNumericOperationKind::compare) {
      int comparison = 0;
      std::string refusal_detail;
      const auto left_comparison_type = dt::CanonicalTypeIdFromStableName(
          left.descriptor.canonical_type_name);
      const bool numeric_comparison_type =
          (left_comparison_type >= dt::CanonicalTypeId::int8 &&
           left_comparison_type <= dt::CanonicalTypeId::int128) ||
          (left_comparison_type >= dt::CanonicalTypeId::uint8 &&
           left_comparison_type <= dt::CanonicalTypeId::uint128) ||
          (left_comparison_type >= dt::CanonicalTypeId::bfloat16 &&
           left_comparison_type <= dt::CanonicalTypeId::decimal_float);
      if (!numeric_comparison_type) {
        refusal_detail =
            "numeric comparison requires a canonical numeric operand";
      }
      if (!refusal_detail.empty() ||
          (!left.isSqlNull() && !right.isSqlNull() &&
          !QowCompareCanonicalNonCollatedScalarsV1(
              left, right, &comparison, &refusal_detail))) {
        return ApiFailure<EngineApplyNumericOperationResult>(
            request.context,
            "query.apply_numeric_operation",
            MakeEngineApiDiagnostic(
                "QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                "engine.query.typed_predicate_refused",
                std::move(refusal_detail)));
      }
      EngineSqlTruthValue truth = EngineSqlTruthValue::unknown;
      if (!QowEvaluateCanonicalComparisonTruthV1(
              left, right, comparison,
              EngineComparisonPredicateOperator::equal, &truth,
              &refusal_detail)) {
        return ApiFailure<EngineApplyNumericOperationResult>(
            request.context,
            "query.apply_numeric_operation",
            MakeEngineApiDiagnostic(
                "QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                "engine.query.typed_predicate_refused",
                std::move(refusal_detail)));
      }
      EngineTypedValue output;
      if (!QowMaterializeCanonicalTruthValueV1(
              truth, result_descriptor, &output, &refusal_detail)) {
        return ApiFailure<EngineApplyNumericOperationResult>(
            request.context,
            "query.apply_numeric_operation",
            MakeEngineApiDiagnostic(
                "QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                "engine.query.typed_predicate_refused",
                std::move(refusal_detail)));
      }
      auto result = ApiSuccess<EngineApplyNumericOperationResult>(
          request.context, "query.apply_numeric_operation");
      result.value = std::move(output);
      result.comparison = comparison;
      result.result_shape.result_kind = "typed_value";
      result.result_shape.columns.push_back(result.value.descriptor);
      result.evidence.push_back(
          {"datatype_numeric_operation", "compare"});
      result.evidence.push_back(
          {"sql_truth_value", EngineSqlTruthValueName(truth)});
      return result;
    }
    EngineTypedValue output;
    std::string refusal_detail;
    if (!QowApplyCanonicalNumericScalarV1(
            left, right, result_descriptor, operation, numeric_request.context,
            &output, &refusal_detail)) {
      const bool overflow_refusal =
          refusal_detail.find("overflow") != std::string::npos ||
          refusal_detail.find("out_of_range") != std::string::npos;
      return ApiFailure<EngineApplyNumericOperationResult>(
          request.context,
          "query.apply_numeric_operation",
          MakeEngineApiDiagnostic(
              overflow_refusal
                  ? "QOW-DIAG-QRY-008-OVERFLOW-REFUSAL-V1"
                  : "QOW-DIAG-QRY-008-NUMERIC-REFUSAL-V1",
              "engine.query.typed_scalar_numeric_refused",
              std::move(refusal_detail)));
    }
    auto result = ApiSuccess<EngineApplyNumericOperationResult>(
        request.context, "query.apply_numeric_operation");
    result.value = std::move(output);
    result.result_shape.result_kind = "typed_value";
    result.result_shape.columns.push_back(result.value.descriptor);
    result.evidence.push_back(
        {"datatype_numeric_operation",
         dt::DatatypeNumericOperationKindName(numeric_request.operation)});
    result.evidence.push_back(
        {"canonical_numeric_descriptor",
         result.value.descriptor.descriptor_uuid.canonical});
    return result;
  }

  const auto numeric_result = dt::ApplyNumericOperation(numeric_request);
  if (!numeric_result.ok()) {
    return ApiFailure<EngineApplyNumericOperationResult>(
        request.context,
        "query.apply_numeric_operation",
        DatatypeDiagnosticToApi("query.apply_numeric_operation", numeric_result.diagnostic));
  }

  auto result = ApiSuccess<EngineApplyNumericOperationResult>(request.context, "query.apply_numeric_operation");
  result.value.descriptor.descriptor_kind = "scalar";
  result.value.descriptor.canonical_type_name = dt::CanonicalTypeName(numeric_result.value.type_id);
  result.value.encoded_value = numeric_result.value.encoded_value;
  result.value.is_null = numeric_result.value.is_null;
  result.value.state = numeric_result.value.is_null
                           ? EngineValueState::sql_null
                           : EngineValueState::value;
  result.comparison = numeric_result.comparison;
  result.result_shape.result_kind = "typed_value";
  result.result_shape.columns.push_back(result.value.descriptor);
  result.evidence.push_back({"datatype_numeric_operation",
                             dt::DatatypeNumericOperationKindName(numeric_request.operation)});
  return result;
}

EngineCanonicalizeDocumentValueResult EngineCanonicalizeDocumentValue(
    const EngineCanonicalizeDocumentValueRequest& request) {
  const EngineTypedValue input = RequestInputValue(request, request.input_value);
  dt::DocumentCanonicalizationRequest document_request;
  document_request.type_id = TypeFromDescriptor(input.descriptor);
  document_request.encoded_value = input.encoded_value;
  document_request.reference_profile = !request.reference_profile.empty()
      ? request.reference_profile
      : OptionValue(request, "document_reference_profile:");
  if (document_request.reference_profile.empty()) { document_request.reference_profile = OptionValue(request, "reference_profile:"); }
  document_request.allow_hstore_domain = request.allow_hstore_domain;
  const std::string allow_hstore_domain_text =
      OptionValue(request, "allow_hstore_domain:");
  if (!allow_hstore_domain_text.empty() &&
      !TryParseBoolOption(allow_hstore_domain_text,
                          &document_request.allow_hstore_domain)) {
    return ApiFailure<EngineCanonicalizeDocumentValueResult>(
        request.context,
        "query.canonicalize_document_value",
        MakeInvalidRequestDiagnostic("query.canonicalize_document_value",
                                     "allow_hstore_domain_invalid"));
  }

  const auto canonical = dt::CanonicalizeDocumentValue(document_request);
  if (!canonical.ok()) {
    return ApiFailure<EngineCanonicalizeDocumentValueResult>(
        request.context,
        "query.canonicalize_document_value",
        DatatypeDiagnosticToApi("query.canonicalize_document_value", canonical.diagnostic));
  }

  auto result = ApiSuccess<EngineCanonicalizeDocumentValueResult>(request.context, "query.canonicalize_document_value");
  result.value.descriptor.descriptor_kind = "scalar";
  result.value.descriptor.canonical_type_name = dt::CanonicalTypeName(canonical.canonical_type_id);
  result.value.encoded_value = canonical.canonical_value;
  result.canonical_format = canonical.canonical_format;
  result.result_shape.result_kind = "typed_value";
  result.result_shape.columns.push_back(result.value.descriptor);
  result.evidence.push_back({"datatype_document_canonical_format", canonical.canonical_format});
  return result;
}

EngineEvaluateAdvancedDatatypeFamilyResult EngineEvaluateAdvancedDatatypeFamily(
    const EngineEvaluateAdvancedDatatypeFamilyRequest& request) {
  const EngineDescriptor descriptor = RequestPrimaryDescriptor(request, request.descriptor);
  const std::string operation_text = !request.operation_kind.empty()
      ? request.operation_kind
      : OptionValue(request, "advanced_operation:");
  const std::string index_text = !request.index_kind.empty()
      ? request.index_kind
      : OptionValue(request, "advanced_index:");
  dt::AdvancedDatatypeOperationKind operation;
  if (!AdvancedOperationKind(operation_text, &operation)) {
    return ApiFailure<EngineEvaluateAdvancedDatatypeFamilyResult>(
        request.context,
        "query.evaluate_advanced_datatype_family",
        MakeInvalidRequestDiagnostic("query.evaluate_advanced_datatype_family",
                                     "advanced_operation_unsupported:" + operation_text));
  }
  dt::AdvancedDatatypeIndexKind index_kind;
  if (!AdvancedIndexKind(index_text, &index_kind)) {
    return ApiFailure<EngineEvaluateAdvancedDatatypeFamilyResult>(
        request.context,
        "query.evaluate_advanced_datatype_family",
        MakeInvalidRequestDiagnostic("query.evaluate_advanced_datatype_family",
                                     "advanced_index_unsupported:" + index_text));
  }

  dt::AdvancedDatatypeFamilyRequest advanced_request;
  advanced_request.type_id = dt::CanonicalTypeIdFromStableName(descriptor.canonical_type_name);
  advanced_request.operation = operation;
  advanced_request.index_kind = index_kind;
  advanced_request.descriptor_profile = !request.descriptor_profile.empty()
      ? request.descriptor_profile
      : OptionValue(request, "descriptor_profile:");
  advanced_request.vector_dimension = request.vector_dimension;
  const std::string vector_dimension_text =
      OptionValue(request, "vector_dimension:");
  if (!vector_dimension_text.empty() &&
      !TryParseU32Option(vector_dimension_text,
                         &advanced_request.vector_dimension)) {
    return ApiFailure<EngineEvaluateAdvancedDatatypeFamilyResult>(
        request.context,
        "query.evaluate_advanced_datatype_family",
        MakeInvalidRequestDiagnostic("query.evaluate_advanced_datatype_family",
                                     "vector_dimension_invalid"));
  }

  const auto evaluated = dt::EvaluateAdvancedDatatypeFamily(advanced_request);
  if (!evaluated.ok()) {
    return ApiFailure<EngineEvaluateAdvancedDatatypeFamilyResult>(
        request.context,
        "query.evaluate_advanced_datatype_family",
        DatatypeDiagnosticToApi("query.evaluate_advanced_datatype_family", evaluated.diagnostic));
  }

  auto result = ApiSuccess<EngineEvaluateAdvancedDatatypeFamilyResult>(
      request.context,
      "query.evaluate_advanced_datatype_family");
  result.family = dt::AdvancedDatatypeFamilyName(evaluated.family);
  result.descriptor_supported = evaluated.descriptor_supported;
  result.operation_supported = evaluated.operation_supported;
  result.index_supported = evaluated.index_supported;
  result.optimizer_admitted = evaluated.optimizer_admitted;
  result.compare_supported = evaluated.compare_supported;
  result.hash_supported = evaluated.hash_supported;
  result.canonical_descriptor_profile = evaluated.canonical_descriptor_profile;
  result.required_descriptor_fields = evaluated.required_descriptor_fields;
  result.compare_hash_refusal_detail = evaluated.compare_hash_refusal_detail;
  result.optimizer_support_path = evaluated.optimizer_support_path;
  result.result_shape.result_kind = "datatype_family_evaluation";
  EngineDescriptor family_descriptor;
  family_descriptor.descriptor_kind = "scalar";
  family_descriptor.canonical_type_name = "character";
  EngineDescriptor boolean_descriptor;
  boolean_descriptor.descriptor_kind = "scalar";
  boolean_descriptor.canonical_type_name = "boolean";
  result.result_shape.columns.push_back(family_descriptor);
  result.result_shape.columns.push_back(boolean_descriptor);
  result.result_shape.columns.push_back(boolean_descriptor);
  result.result_shape.columns.push_back(boolean_descriptor);
  result.result_shape.columns.push_back(boolean_descriptor);
  result.result_shape.columns.push_back(boolean_descriptor);
  result.result_shape.columns.push_back(family_descriptor);
  result.result_shape.columns.push_back(family_descriptor);
  EngineRowValue row;
  row.fields.push_back({"family", {family_descriptor, result.family, false}});
  row.fields.push_back({"operation_supported", {boolean_descriptor, result.operation_supported ? "true" : "false", false}});
  row.fields.push_back({"index_supported", {boolean_descriptor, result.index_supported ? "true" : "false", false}});
  row.fields.push_back({"optimizer_admitted", {boolean_descriptor, result.optimizer_admitted ? "true" : "false", false}});
  row.fields.push_back({"compare_supported", {boolean_descriptor, result.compare_supported ? "true" : "false", false}});
  row.fields.push_back({"hash_supported", {boolean_descriptor, result.hash_supported ? "true" : "false", false}});
  row.fields.push_back({"canonical_descriptor_profile", {family_descriptor, result.canonical_descriptor_profile, false}});
  row.fields.push_back({"optimizer_support_path", {family_descriptor, result.optimizer_support_path, false}});
  result.result_shape.rows.push_back(std::move(row));
  result.evidence.push_back({"advanced_family", result.family});
  result.evidence.push_back({"advanced_operation", dt::AdvancedDatatypeOperationKindName(operation)});
  result.evidence.push_back({"advanced_index", dt::AdvancedDatatypeIndexKindName(index_kind)});
  result.evidence.push_back({"advanced_canonical_descriptor_profile", result.canonical_descriptor_profile});
  result.evidence.push_back({"advanced_compare_hash_refusal", result.compare_hash_refusal_detail});
  result.evidence.push_back({"advanced_optimizer_support_path", result.optimizer_support_path});
  return result;
}

EngineValidateDomainValueResult EngineValidateDomainValue(const EngineValidateDomainValueRequest& request) {
  const EngineDescriptor descriptor = RequestTargetDescriptor(request, request.domain_descriptor);
  const EngineTypedValue input = RequestInputValue(request, request.input_value);
  const auto validation = ValidateDomainTypedValue(request.context,
                                                  descriptor,
                                                  input,
                                                  request.context.local_transaction_id);
  if (!validation.ok) {
    return ApiFailure<EngineValidateDomainValueResult>(
        request.context,
        "query.validate_domain_value",
        validation.diagnostic);
  }
  auto result = ApiSuccess<EngineValidateDomainValueResult>(request.context, "query.validate_domain_value");
  result.value = validation.value;
  result.result_shape.result_kind = "typed_value";
  result.result_shape.columns.push_back(result.value.descriptor);
  for (const auto& evidence : validation.evidence) { result.evidence.push_back(evidence); }
  return result;
}

EngineInvokeDomainMethodResult EngineInvokeDomainMethod(const EngineInvokeDomainMethodRequest& request) {
  const EngineDescriptor descriptor = RequestTargetDescriptor(request, request.domain_descriptor);
  const EngineTypedValue input = RequestInputValue(request, request.input_value);
  const std::string domain_uuid = DomainUuidFromDescriptor(descriptor);
  if (domain_uuid.empty()) {
    return ApiFailure<EngineInvokeDomainMethodResult>(
        request.context,
        "query.invoke_domain_method",
        MakeInvalidRequestDiagnostic("query.invoke_domain_method", "domain_uuid_required"));
  }
  const auto domain = FindVisibleDomain(request.context, domain_uuid, request.context.local_transaction_id);
  if (!domain) {
    return ApiFailure<EngineInvokeDomainMethodResult>(
        request.context,
        "query.invoke_domain_method",
        MakeInvalidRequestDiagnostic("query.invoke_domain_method", "domain_not_visible"));
  }
  const std::string method = !request.method_name.empty() ? request.method_name : OptionValue(request, "method:");
  if (method.empty()) {
    return ApiFailure<EngineInvokeDomainMethodResult>(
        request.context,
        "query.invoke_domain_method",
        MakeInvalidRequestDiagnostic("query.invoke_domain_method", "method_name_required"));
  }
  const std::string binding = domain->method_binding_envelope;
  if (binding.empty()) {
    return ApiFailure<EngineInvokeDomainMethodResult>(
        request.context,
        "query.invoke_domain_method",
        MakeInvalidRequestDiagnostic("query.invoke_domain_method", "domain_method_not_declared"));
  }
  const std::string required_right = MethodBindingField(binding, "require_right");
  if (!required_right.empty() && !HasDomainRight(request.context, domain_uuid, required_right)) {
    return ApiFailure<EngineInvokeDomainMethodResult>(
        request.context,
        "query.invoke_domain_method",
        MakeInvalidRequestDiagnostic("query.invoke_domain_method", "domain_method_right_denied:" + required_right));
  }
  const std::string builtin = MethodBindingField(binding, "builtin");
  const std::string udr = MethodBindingField(binding, "udr");
  if (!udr.empty()) {
    return ApiFailure<EngineInvokeDomainMethodResult>(
        request.context,
        "query.invoke_domain_method",
        MakeInvalidRequestDiagnostic("query.invoke_domain_method", "domain_method_udr_bridge_not_available"));
  }
  if (builtin.empty() || builtin != method) {
    return ApiFailure<EngineInvokeDomainMethodResult>(
        request.context,
        "query.invoke_domain_method",
        MakeInvalidRequestDiagnostic("query.invoke_domain_method", "domain_method_binding_not_found:" + method));
  }

  auto result = ApiSuccess<EngineInvokeDomainMethodResult>(request.context, "query.invoke_domain_method");
  result.value = input;
  result.value.descriptor = descriptor;
  if (method == "identity") {
    result.value = input;
    result.value.descriptor = descriptor;
  } else if (method == "length") {
    result.value.descriptor.descriptor_kind = "scalar";
    result.value.descriptor.canonical_type_name = "int64";
    result.value.descriptor.encoded_descriptor = "canonical=int64";
    result.value.encoded_value = std::to_string(input.encoded_value.size());
  } else if (method == "lower") {
    result.value.encoded_value = LowerValue(input.encoded_value);
  } else if (method == "upper") {
    result.value.encoded_value = UpperValue(input.encoded_value);
  } else {
    return ApiFailure<EngineInvokeDomainMethodResult>(
        request.context,
        "query.invoke_domain_method",
        MakeInvalidRequestDiagnostic("query.invoke_domain_method", "domain_method_builtin_not_supported:" + method));
  }
  result.result_shape.result_kind = "typed_value";
  result.result_shape.columns.push_back(result.value.descriptor);
  result.evidence.push_back({"domain_method", domain_uuid});
  result.evidence.push_back({"domain_method_builtin", method});
  return result;
}

}  // namespace scratchbird::engine::internal_api

#endif  // full internal API implementation
