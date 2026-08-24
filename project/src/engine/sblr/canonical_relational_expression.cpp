// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_relational_expression.hpp"
#include "hash_digest.hpp"

#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "query/expression_api.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace {

std::string UpperAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](const unsigned char byte) {
    return static_cast<char>(std::toupper(byte));
  });
  return value;
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto byte = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(byte) || std::isupper(byte)) return false;
  }
  return true;
}

bool IsNumericType(const dt::CanonicalTypeId type_id) {
  switch (type_id) {
    case dt::CanonicalTypeId::int8:
    case dt::CanonicalTypeId::int16:
    case dt::CanonicalTypeId::int32:
    case dt::CanonicalTypeId::int64:
    case dt::CanonicalTypeId::int128:
    case dt::CanonicalTypeId::real32:
    case dt::CanonicalTypeId::real64:
    case dt::CanonicalTypeId::real128:
    case dt::CanonicalTypeId::decimal:
    case dt::CanonicalTypeId::decimal_float:
      return true;
    default:
      return false;
  }
}

bool IsTemporalType(const dt::CanonicalTypeId type_id) {
  return type_id == dt::CanonicalTypeId::date ||
         type_id == dt::CanonicalTypeId::time ||
         type_id == dt::CanonicalTypeId::timestamp ||
         type_id == dt::CanonicalTypeId::interval;
}

bool ParseSpatialPointCoordinate(const std::string_view payload,
                                 double* coordinate) {
  if (coordinate == nullptr || payload.empty()) return false;
  double parsed = 0.0;
  const auto result = std::from_chars(
      payload.data(), payload.data() + payload.size(), parsed,
      std::chars_format::general);
  if (result.ec != std::errc{} ||
      result.ptr != payload.data() + payload.size() ||
      !std::isfinite(parsed)) {
    return false;
  }
  *coordinate = parsed == 0.0 ? 0.0 : parsed;
  return true;
}

std::vector<std::uint8_t> EncodeSpatialPoint2d(const double x,
                                               const double y) {
  std::vector<std::uint8_t> encoded(24, 0);
  encoded[0] = 'S';
  encoded[1] = 'B';
  encoded[2] = 'P';
  encoded[3] = '1';
  encoded[4] = 1;
  encoded[5] = 2;
  const auto encode_coordinate = [&](const double coordinate,
                                     const std::size_t offset) {
    const auto bits = std::bit_cast<std::uint64_t>(coordinate);
    for (std::size_t index = 0; index < 8; ++index) {
      encoded[offset + index] = static_cast<std::uint8_t>(
          bits >> ((7 - index) * 8));
    }
  };
  encode_coordinate(x, 8);
  encode_coordinate(y, 16);
  return encoded;
}

bool LiteralKindAdmitsType(const api::RelationalLiteralKind literal_kind,
                           const std::string_view type_name) {
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      std::string(type_name));
  switch (literal_kind) {
    case api::RelationalLiteralKind::kNumeric:
      return IsNumericType(type_id);
    case api::RelationalLiteralKind::kString:
      return type_id == dt::CanonicalTypeId::character;
    case api::RelationalLiteralKind::kBinary:
      return type_id == dt::CanonicalTypeId::binary;
    case api::RelationalLiteralKind::kTemporal:
      return IsTemporalType(type_id);
    case api::RelationalLiteralKind::kUuid:
      return type_id == dt::CanonicalTypeId::uuid;
    case api::RelationalLiteralKind::kBoolean:
      return type_id == dt::CanonicalTypeId::boolean;
    case api::RelationalLiteralKind::kNull:
      return true;
    default:
      return false;
  }
}

std::string BoundLiteralType(
    const api::RelationalExpressionRecord& expression,
    const api::RelationalTypeDescriptor& descriptor,
    const std::optional<std::string_view> expected_type) {
  if (!expression.literal_kind.has_value() ||
      (!expression.literal_or_parameter_ref.has_value() &&
       !expression.literal_typed_value_v1.has_value())) {
    return {};
  }
  const auto kind = *expression.literal_kind;
  if (expected_type.has_value() &&
      LiteralKindAdmitsType(kind, *expected_type)) {
    return std::string(*expected_type);
  }
  if(expression.literal_typed_value_v1.has_value()){
    return kind==api::RelationalLiteralKind::kNumeric?"bigint":std::string{};
  }
  const auto& payload = *expression.literal_or_parameter_ref;
  switch (kind) {
    case api::RelationalLiteralKind::kNumeric:
      if (descriptor.precision.has_value() || descriptor.scale.has_value()) {
        return descriptor.precision.has_value() && descriptor.scale.has_value()
                   ? "decimal"
                   : std::string{};
      }
      if (payload.find_first_of(".eE") != std::string::npos) {
        return "real64";
      }
      if (descriptor.width.has_value()) {
        switch (*descriptor.width) {
          case 8:
            return "int8";
          case 16:
            return "int16";
          case 32:
            return "int32";
          case 64:
            return "int64";
          case 128:
            return "int128";
          default:
            return {};
        }
      }
      return "int64";
    case api::RelationalLiteralKind::kString:
      return "text";
    case api::RelationalLiteralKind::kBinary:
      return "binary";
    case api::RelationalLiteralKind::kUuid:
      return "uuid";
    case api::RelationalLiteralKind::kBoolean:
      return "boolean";
    case api::RelationalLiteralKind::kNull:
      return "null";
    case api::RelationalLiteralKind::kTemporal:
      if (descriptor.timezone_profile_id.has_value()) {
        if (*descriptor.timezone_profile_id == "time_timezone_profile") {
          return "time";
        }
        if (*descriptor.timezone_profile_id ==
            "timestamp_timezone_profile") {
          return "timestamp";
        }
        return {};
      }
      if (!payload.empty() && payload.front() == 'P') return "interval";
      if (payload.size() == 10 && payload[4] == '-' && payload[7] == '-') {
        return "date";
      }
      if (payload.find('T') != std::string::npos ||
          (payload.size() > 10 && payload[4] == '-' &&
           payload.find(' ') != std::string::npos)) {
        return "timestamp";
      }
      if (payload.find(':') != std::string::npos) return "time";
      return {};
    default:
      return {};
  }
}

std::string DescriptorField(const std::string& descriptor,
                            const std::string& key) {
  const std::string prefix = key + "=";
  std::string value;
  bool found = false;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
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

bool DescriptorU32(const api::EngineDescriptor& descriptor,
                   const std::string& key,
                   std::uint32_t* value) {
  if (value == nullptr) return false;
  const auto field = DescriptorField(descriptor.encoded_descriptor, key);
  if (field.empty()) return false;
  const auto [end, error] = std::from_chars(
      field.data(), field.data() + field.size(), *value);
  return error == std::errc{} && end == field.data() + field.size();
}

bool NumericContextForDescriptor(
    const api::EngineDescriptor& descriptor,
    dt::DatatypeNumericContext* context,
    std::string* refusal_detail) {
  if (context == nullptr || refusal_detail == nullptr) return false;
  *context = {};
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      descriptor.canonical_type_name);
  if (!IsNumericType(type_id)) {
    *refusal_detail = "arithmetic descriptor is not a canonical numeric type";
    return false;
  }
  context->rounding = dt::DatatypeRoundingMode::half_even;
  context->allow_special_values =
      type_id == dt::CanonicalTypeId::decimal_float;
  if (type_id == dt::CanonicalTypeId::decimal ||
      type_id == dt::CanonicalTypeId::decimal_float) {
    if (!DescriptorU32(descriptor, "precision", &context->precision) ||
        !DescriptorU32(descriptor, "scale", &context->scale)) {
      *refusal_detail =
          "arithmetic descriptor precision or scale is unresolved";
      return false;
    }
    return true;
  }
  context->precision =
      type_id == dt::CanonicalTypeId::int128 ||
              type_id == dt::CanonicalTypeId::real128
          ? 38
          : 19;
  context->scale = 0;
  return true;
}

bool CanonicalizeLiteralPayload(const std::string_view type_name,
                                const api::EngineDescriptor& descriptor,
                                const std::string& payload,
                                std::string* canonical_payload,
                                std::string* refusal_detail) {
  if (canonical_payload == nullptr || refusal_detail == nullptr) return false;
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      std::string(type_name));
  if (type_id == dt::CanonicalTypeId::unknown) {
    *refusal_detail = "literal canonical datatype is unknown";
    return false;
  }
  const bool extended_numeric =
      type_id == dt::CanonicalTypeId::decimal ||
      type_id == dt::CanonicalTypeId::decimal_float ||
      type_id == dt::CanonicalTypeId::int128 ||
      type_id == dt::CanonicalTypeId::real128;
  if (extended_numeric) {
    dt::DatatypeNumericOperationRequest request;
    request.operation = dt::DatatypeNumericOperationKind::canonicalize;
    request.type_id = type_id;
    request.left.type_id = type_id;
    request.left.encoded_value = payload;
    if (type_id == dt::CanonicalTypeId::decimal ||
        type_id == dt::CanonicalTypeId::decimal_float) {
      if (!DescriptorU32(descriptor, "precision", &request.context.precision) ||
          !DescriptorU32(descriptor, "scale", &request.context.scale)) {
        *refusal_detail =
            "numeric literal descriptor precision or scale is unresolved";
        return false;
      }
    } else {
      std::uint32_t width = 0;
      if (!DescriptorU32(descriptor, "width", &width) || width != 128) {
        *refusal_detail = "128-bit numeric literal width is invalid";
        return false;
      }
      request.context.precision = 38;
      request.context.scale = 0;
    }
    const auto canonical = dt::ApplyNumericOperation(request);
    if (!canonical.ok()) {
      *refusal_detail = canonical.diagnostic.diagnostic_code.empty()
                            ? "numeric literal is out of range"
                            : canonical.diagnostic.diagnostic_code;
      return false;
    }
    *canonical_payload = canonical.value.encoded_value;
    return true;
  }
  dt::DatatypeCastRequest request;
  request.value.type_id = type_id;
  request.value.encoded_value = payload;
  request.target_type_id = type_id;
  request.explicit_cast = true;
  const auto canonical = dt::CastDatatypeValue(request);
  if (!canonical.ok()) {
    *refusal_detail = canonical.diagnostic.diagnostic_code.empty()
                          ? "literal payload is invalid for its descriptor"
                          : canonical.diagnostic.diagnostic_code;
    return false;
  }
  *canonical_payload = canonical.value.encoded_value;
  return true;
}

bool TruthFromValue(const api::EngineTypedValue& value,
                    api::EngineSqlTruthValue* truth,
                    std::string* refusal_detail) {
  return api::QowCanonicalTruthFromTypedValueV1(
      value, truth, refusal_detail);
}

bool IsComparisonOperator(const std::string_view operation) {
  return operation == "=" || operation == "<>" || operation == "!=" ||
         operation == "<" || operation == "<=" || operation == ">" ||
         operation == ">=" || operation == "IS DISTINCT FROM" ||
         operation == "IS NOT DISTINCT FROM";
}

bool IsAdmittedComparisonType(const std::string_view type_name) {
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      std::string(type_name));
  return type_id == dt::CanonicalTypeId::boolean ||
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
         type_id == dt::CanonicalTypeId::character ||
         type_id == dt::CanonicalTypeId::binary ||
         type_id == dt::CanonicalTypeId::bit_string ||
         type_id == dt::CanonicalTypeId::date ||
         type_id == dt::CanonicalTypeId::time ||
         type_id == dt::CanonicalTypeId::timestamp ||
         type_id == dt::CanonicalTypeId::interval;
}

bool SameCanonicalType(const std::string_view left,
                       const std::string_view right) {
  const auto left_id = dt::CanonicalTypeIdFromStableName(std::string(left));
  return left_id != dt::CanonicalTypeId::unknown &&
         left_id == dt::CanonicalTypeIdFromStableName(std::string(right));
}

bool IsBoundedSignedIntegerType(const std::string_view type_name) {
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      std::string(type_name));
  return type_id == dt::CanonicalTypeId::int8 ||
         type_id == dt::CanonicalTypeId::int16 ||
         type_id == dt::CanonicalTypeId::int32 ||
         type_id == dt::CanonicalTypeId::int64;
}

unsigned BoundedSignedIntegerTypeRank(const std::string_view type_name) {
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      std::string(type_name));
  switch (type_id) {
    case dt::CanonicalTypeId::int8: return 1;
    case dt::CanonicalTypeId::int16: return 2;
    case dt::CanonicalTypeId::int32: return 3;
    case dt::CanonicalTypeId::int64: return 4;
    default: return 0;
  }
}

bool LosslessBoundedSignedComparisonTypes(
    const std::string_view left, const std::string_view right,
    std::string* common_type) {
  if (common_type == nullptr) return false;
  const auto left_rank = BoundedSignedIntegerTypeRank(left);
  const auto right_rank = BoundedSignedIntegerTypeRank(right);
  if (left_rank == 0 || right_rank == 0) return false;
  *common_type = std::string(left_rank >= right_rank ? left : right);
  return true;
}

bool PromoteBoundedSignedComparisonValues(
    api::EngineTypedValue* left, api::EngineTypedValue* right,
    std::string* refusal_detail) {
  if (left == nullptr || right == nullptr || refusal_detail == nullptr) {
    return false;
  }
  if (SameCanonicalType(left->descriptor.canonical_type_name,
                        right->descriptor.canonical_type_name)) {
    return true;
  }
  std::string common_type;
  if (!LosslessBoundedSignedComparisonTypes(
          left->descriptor.canonical_type_name,
          right->descriptor.canonical_type_name, &common_type)) {
    *refusal_detail =
        "comparison operands have no lossless signed-integer promotion";
    return false;
  }
  const auto target_type = dt::CanonicalTypeIdFromStableName(common_type);
  const auto left_rank =
      BoundedSignedIntegerTypeRank(left->descriptor.canonical_type_name);
  const auto right_rank =
      BoundedSignedIntegerTypeRank(right->descriptor.canonical_type_name);
  const auto& target_descriptor =
      left_rank >= right_rank ? left->descriptor : right->descriptor;
  const auto promote = [&](api::EngineTypedValue* value) {
    const auto source_type = dt::CanonicalTypeIdFromStableName(
        value->descriptor.canonical_type_name);
    if (source_type == target_type) return true;
    if (value->isSqlNull()) {
      if (!value->encoded_value.empty() || !value->binary_value.empty()) {
        return false;
      }
      value->descriptor = target_descriptor;
      return true;
    }
    if (value->state != api::EngineValueState::value || value->is_null ||
        value->encoded_value.empty() || !value->binary_value.empty()) {
      return false;
    }
    dt::DatatypeCastRequest request;
    request.value.type_id = source_type;
    request.value.encoded_value = value->encoded_value;
    request.target_type_id = target_type;
    request.explicit_cast = false;
    const auto widened = dt::CastDatatypeValue(request);
    if (!widened.ok() || widened.value.is_null) return false;
    value->descriptor = target_descriptor;
    value->encoded_value = widened.value.encoded_value;
    value->binary_value.clear();
    value->is_null = false;
    value->state = api::EngineValueState::value;
    return true;
  };
  if (!promote(left) || !promote(right)) {
    *refusal_detail =
        "lossless signed-integer comparison promotion failed";
    return false;
  }
  return true;
}

std::string TypedUuidText(const scratchbird::core::platform::TypedUuid& uuid) {
  if (!uuid.valid()) return {};
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < uuid.value.bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
    out << std::setw(2)
        << static_cast<unsigned>(uuid.value.bytes[index]);
  }
  return out.str();
}

bool SameDescriptor(const api::EngineDescriptor& left,
                    const api::EngineDescriptor& right) {
  return left.descriptor_uuid.canonical == right.descriptor_uuid.canonical &&
         left.descriptor_kind == right.descriptor_kind &&
         left.canonical_type_name == right.canonical_type_name &&
         left.encoded_descriptor == right.encoded_descriptor;
}

bool SamePersistedRowDescriptor(
    const api::RelationalTypeDescriptor& bound,
    const api::EngineDescriptor& actual,
    const std::optional<api::RelationalNullability>
        effective_nullability = std::nullopt) {
  if (!api::QowCanonicalDescriptorIdentityV1(actual) ||
      actual.descriptor_uuid.canonical != bound.descriptor_uuid ||
      actual.descriptor_kind != "scalar") {
    return false;
  }

  static const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  std::string bound_type_name;
  if (core_manifest.ok()) {
    const auto row = std::ranges::find_if(
        core_manifest.manifest.descriptor_rows, [&](const auto& candidate) {
          return TypedUuidText(candidate.descriptor_uuid) == bound.type_uuid;
        });
    if (row != core_manifest.manifest.descriptor_rows.end()) {
      bound_type_name = row->stable_name;
    } else {
      const auto int64_row = std::ranges::find_if(
          core_manifest.manifest.descriptor_rows, [](const auto& candidate) {
            return candidate.stable_name == "int64";
          });
      if (int64_row != core_manifest.manifest.descriptor_rows.end()) {
        const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
            "019d0000-0000-7000-8000-00000000d701",
            core_manifest.manifest.catalog_epoch, 1,
            TypedUuidText(int64_row->descriptor_uuid),
            int64_row->descriptor_epoch);
        if (identity.ok && identity.row.type_uuid == bound.type_uuid) {
          bound_type_name = int64_row->stable_name;
        }
      }
    }
  }
  if (bound_type_name.empty() ||
      !SameCanonicalType(bound_type_name, actual.canonical_type_name)) {
    return false;
  }

  bool canonical_seen = false;
  bool type_uuid_seen = false;
  bool nullability_seen = false;
  bool charset_seen = false;
  bool collation_seen = false;
  bool timezone_seen = false;
  bool width_seen = false;
  bool precision_seen = false;
  bool scale_seen = false;
  const auto expected_nullability =
      effective_nullability.value_or(bound.nullability);

  const auto exact_string_optional = [](const std::string_view value,
                                        const std::optional<std::string>& bound_value,
                                        bool* seen) {
    if (seen == nullptr || *seen || !bound_value.has_value() || value.empty() ||
        value != *bound_value) {
      return false;
    }
    *seen = true;
    return true;
  };
  const auto exact_u32_optional = [](const std::string_view value,
                                     const std::optional<std::uint32_t> bound_value,
                                     bool* seen) {
    if (seen == nullptr || *seen || !bound_value.has_value() ||
        value != std::to_string(*bound_value)) {
      return false;
    }
    *seen = true;
    return true;
  };

  std::size_t start = 0;
  while (start < actual.encoded_descriptor.size()) {
    const auto end = actual.encoded_descriptor.find(';', start);
    const auto field = std::string_view(actual.encoded_descriptor).substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    const auto separator = field.find('=');
    if (field.empty() || separator == std::string_view::npos || separator == 0 ||
        field.find('=', separator + 1) != std::string_view::npos) {
      return false;
    }
    const auto key = field.substr(0, separator);
    const auto value = field.substr(separator + 1);

    if (key == "canonical" || key == "type") {
      if (canonical_seen || value.empty() ||
          value != actual.canonical_type_name) {
        return false;
      }
      canonical_seen = true;
    } else if (key == "type_uuid") {
      if (type_uuid_seen || value.empty() || value != bound.type_uuid) {
        return false;
      }
      type_uuid_seen = true;
    } else if (key == "nullability" || key == "nullable") {
      if (nullability_seen) return false;
      if (key == "nullability") {
        if ((expected_nullability == api::RelationalNullability::kNullable &&
             value != "nullable") ||
            (expected_nullability == api::RelationalNullability::kNonNull &&
             value != "non_null")) {
          return false;
        }
      } else if ((expected_nullability ==
                      api::RelationalNullability::kNullable &&
                  value != "true") ||
                 (expected_nullability ==
                      api::RelationalNullability::kNonNull &&
                  value != "false")) {
        return false;
      }
      nullability_seen = true;
    } else if (key == "collation_uuid") {
      if (!exact_string_optional(value, bound.collation_uuid,
                                 &collation_seen)) {
        return false;
      }
    } else if (key == "charset_uuid") {
      if (charset_seen || !IsCanonicalUuid(value)) return false;
      charset_seen = true;
    } else if (key == "timezone_profile_id") {
      if (!exact_string_optional(value, bound.timezone_profile_id,
                                 &timezone_seen)) {
        return false;
      }
    } else if (key == "width" || key == "character_length") {
      if (!exact_u32_optional(value, bound.width, &width_seen)) return false;
    } else if (key == "precision") {
      if (!exact_u32_optional(value, bound.precision, &precision_seen)) {
        return false;
      }
    } else if (key == "scale") {
      if (!exact_u32_optional(value, bound.scale, &scale_seen)) return false;
    } else {
      return false;
    }

    if (end == std::string::npos) break;
    start = end + 1;
    if (start == actual.encoded_descriptor.size()) return false;
  }

  return (canonical_seen || type_uuid_seen) &&
         collation_seen == bound.collation_uuid.has_value() &&
         timezone_seen == bound.timezone_profile_id.has_value() &&
         width_seen == bound.width.has_value() &&
         precision_seen == bound.precision.has_value() &&
         scale_seen == bound.scale.has_value();
}

}  // namespace

CanonicalPredicateLogicalMemoryBound
BoundCanonicalRowPredicateLogicalMemoryV1(
    const api::TypedRelationalDag& dag,
    const std::uint32_t root_expression_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const std::vector<CanonicalPredicateRowValueEnvelope>& row_values,
    const api::EngineCanonicalExpressionConsumer consumer,
    const std::function<bool()>& abort_requested) {
  CanonicalPredicateLogicalMemoryBound result;
  result.dag_expression_count = dag.expressions.size();
  result.dag_descriptor_count = dag.descriptors.size();
  result.row_slot_count = row_binding.slots.size();
  const auto cancelled = [&] {
    if (!abort_requested || !abort_requested()) return false;
    result.detail = "predicate logical-memory preflight was cancelled";
    return true;
  };
  const auto add = [](const std::uint64_t left, const std::uint64_t right,
                      std::uint64_t* output) {
    if (output == nullptr ||
        right > std::numeric_limits<std::uint64_t>::max() - left) {
      return false;
    }
    *output = left + right;
    return true;
  };
  const auto multiply = [](const std::uint64_t left,
                           const std::uint64_t right,
                           std::uint64_t* output) {
    if (output == nullptr ||
        (left != 0 &&
         right > std::numeric_limits<std::uint64_t>::max() / left)) {
      return false;
    }
    *output = left * right;
    return true;
  };
  const auto account = [&](std::uint64_t* total,
                           const std::uint64_t bytes) {
    return add(*total, bytes, total);
  };
  const auto account_count = [&](std::uint64_t* total,
                                 const std::size_t count,
                                 const std::size_t bytes) {
    std::uint64_t product = 0;
    return multiply(count, bytes, &product) && account(total, product);
  };
  const auto account_string = [&](std::uint64_t* total,
                                  const std::string_view value) {
    return account(total, value.size()) && account(total, 1);
  };
  const auto descriptor_dynamic_bytes = [&](const api::EngineDescriptor& d,
                                            std::uint64_t* bytes) {
    *bytes = 0;
    return account_string(bytes, d.descriptor_uuid.canonical) &&
           account_string(bytes, d.descriptor_kind) &&
           account_string(bytes, d.canonical_type_name) &&
           account_string(bytes, d.encoded_descriptor);
  };
  const auto value_envelope_bytes = [&](
                                        const CanonicalPredicateRowValueEnvelope&
                                            envelope,
                                        std::uint64_t* bytes) {
    std::uint64_t dynamic = 0;
    *bytes = sizeof(api::EngineTypedValue);
    return descriptor_dynamic_bytes(envelope.descriptor, &dynamic) &&
           account(bytes, dynamic) &&
           account(bytes, envelope.maximum_encoded_value_bytes) &&
           account(bytes, envelope.maximum_binary_value_bytes);
  };

  std::unordered_map<std::uint32_t,
                     const api::RelationalExpressionRecord*>
      expressions;
  expressions.reserve(dag.expressions.size());
  for (const auto& expression : dag.expressions) {
    if (cancelled()) return result;
    if (expression.expression_id == 0 ||
        !expressions.emplace(expression.expression_id, &expression).second) {
      result.detail = "predicate expression identity is not unique";
      return result;
    }
  }
  std::unordered_map<std::uint32_t,
                     const api::RelationalTypeDescriptor*>
      descriptors;
  descriptors.reserve(dag.descriptors.size());
  for (const auto& descriptor : dag.descriptors) {
    if (cancelled()) return result;
    if (descriptor.descriptor_id == 0 ||
        !descriptors.emplace(descriptor.descriptor_id, &descriptor).second) {
      result.detail = "predicate descriptor identity is not unique";
      return result;
    }
  }
  if (!expressions.contains(root_expression_id) ||
      row_binding.row_descriptor_ids.empty() ||
      row_binding.row_descriptor_ids.size() != row_values.size() ||
      (!row_binding.row_nullable.empty() &&
       row_binding.row_nullable.size() !=
           row_binding.row_descriptor_ids.size())) {
    result.detail =
        "predicate root or descriptor-exact row envelope is absent";
    return result;
  }
  if (consumer != api::EngineCanonicalExpressionConsumer::filter &&
      consumer != api::EngineCanonicalExpressionConsumer::aggregate &&
      consumer != api::EngineCanonicalExpressionConsumer::join &&
      consumer != api::EngineCanonicalExpressionConsumer::projection) {
    result.detail = "predicate expression consumer is not canonical";
    return result;
  }

  const auto build_expected_descriptor = [&](
      const api::RelationalTypeDescriptor& source,
      const api::RelationalNullability effective_nullability,
      const std::string_view type_name, api::EngineDescriptor* descriptor) {
    if (descriptor == nullptr || type_name.empty() || type_name == "null") {
      return false;
    }
    descriptor->descriptor_uuid.canonical = source.descriptor_uuid;
    descriptor->descriptor_kind = "scalar";
    descriptor->canonical_type_name = type_name;
    const char* nullability = "unknown";
    if (effective_nullability == api::RelationalNullability::kNonNull) {
      nullability = "non_null";
    } else if (effective_nullability ==
               api::RelationalNullability::kNullable) {
      nullability = "nullable";
    }
    descriptor->encoded_descriptor =
        "type_uuid=" + source.type_uuid + ";nullability=" + nullability;
    if (source.collation_uuid.has_value()) {
      descriptor->encoded_descriptor +=
          ";collation_uuid=" + *source.collation_uuid;
    }
    if (source.timezone_profile_id.has_value()) {
      descriptor->encoded_descriptor +=
          ";timezone_profile_id=" + *source.timezone_profile_id;
    }
    if (source.width.has_value()) {
      descriptor->encoded_descriptor +=
          ";width=" + std::to_string(*source.width);
    }
    if (source.precision.has_value()) {
      descriptor->encoded_descriptor +=
          ";precision=" + std::to_string(*source.precision);
    }
    if (source.scale.has_value()) {
      descriptor->encoded_descriptor +=
          ";scale=" + std::to_string(*source.scale);
    }
    return true;
  };

  std::uint64_t resident = sizeof(CanonicalRelationalExpressionRuntime);
  if (!account_count(
          &resident, dag.expressions.size(),
          sizeof(std::pair<const std::uint32_t,
                           const api::RelationalExpressionRecord*>)) ||
      !account_count(
          &resident, dag.descriptors.size(),
          sizeof(std::pair<const std::uint32_t,
                           const api::RelationalTypeDescriptor*>))) {
    result.detail = "predicate runtime resident-memory bound overflowed";
    return result;
  }

  std::unordered_set<std::uint32_t> row_descriptor_ids;
  std::uint64_t maximum_expected_descriptor = 0;
  for (std::size_t ordinal = 0;
       ordinal < row_binding.row_descriptor_ids.size(); ++ordinal) {
    if (cancelled()) return result;
    const auto descriptor_id = row_binding.row_descriptor_ids[ordinal];
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t expected_descriptor_bytes = 0;
    const auto descriptor = descriptors.find(descriptor_id);
    api::EngineDescriptor expected_descriptor;
    if (descriptor_id == 0 || descriptor == descriptors.end() ||
        !row_descriptor_ids.insert(descriptor_id).second ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown) {
      result.detail =
          "predicate row descriptor envelope is unresolved or ambiguous";
      return result;
    }
    const auto effective_nullability =
        row_binding.row_nullable.empty()
            ? descriptor->second->nullability
            : (row_binding.row_nullable[ordinal]
                   ? api::RelationalNullability::kNullable
                   : api::RelationalNullability::kNonNull);
    if (
        row_values[ordinal].descriptor.canonical_type_name.empty() ||
        !build_expected_descriptor(
            *descriptor->second, effective_nullability,
            row_values[ordinal].descriptor.canonical_type_name,
            &expected_descriptor) ||
        (!SameDescriptor(expected_descriptor,
                         row_values[ordinal].descriptor) &&
         !SamePersistedRowDescriptor(*descriptor->second,
                                     row_values[ordinal].descriptor,
                                     effective_nullability)) ||
        !descriptor_dynamic_bytes(row_values[ordinal].descriptor,
                                  &descriptor_bytes) ||
        !descriptor_dynamic_bytes(expected_descriptor,
                                  &expected_descriptor_bytes)) {
      result.detail =
          "predicate row descriptor envelope is unresolved or ambiguous";
      return result;
    }
    std::uint64_t expected_bytes = sizeof(api::EngineDescriptor);
    if (!account(&expected_bytes,
                 std::max(descriptor_bytes, expected_descriptor_bytes))) {
      result.detail = "predicate expected descriptor bound overflowed";
      return result;
    }
    maximum_expected_descriptor =
        std::max(maximum_expected_descriptor, expected_bytes);
  }

  std::unordered_map<std::uint32_t,
                     const CanonicalPredicateRowValueEnvelope*>
      bound_values;
  std::unordered_map<std::size_t,
                     CanonicalRelationalExpressionRowSlotKind>
      bound_ordinals;
  std::unordered_set<std::uint32_t> bound_expression_ids;
  for (const auto& slot : row_binding.slots) {
    if (cancelled()) return result;
    const auto expression = expressions.find(slot.expression_id);
    const auto [ordinal, inserted_ordinal] =
        bound_ordinals.emplace(slot.row_ordinal, slot.slot_kind);
    if (slot.expression_id == 0 ||
        !bound_expression_ids.insert(slot.expression_id).second ||
        (!inserted_ordinal &&
         (ordinal->second !=
              CanonicalRelationalExpressionRowSlotKind::input_identifier ||
          slot.slot_kind !=
              CanonicalRelationalExpressionRowSlotKind::input_identifier)) ||
        expression == expressions.end() || slot.descriptor_id == 0 ||
        expression->second->result_descriptor_id != slot.descriptor_id ||
        slot.row_ordinal >= row_values.size() ||
        row_binding.row_descriptor_ids[slot.row_ordinal] !=
            slot.descriptor_id) {
      result.detail = "predicate row slot binding is not descriptor-exact";
      return result;
    }
    const auto& record = *expression->second;
    const bool materialized_function =
        record.expression_kind == api::RelationalExpressionKind::kFunctionCall &&
        record.function_uuid.has_value() &&
        IsCanonicalUuid(*record.function_uuid) &&
        !record.bound_name_uuid.has_value() &&
        !record.literal_kind.has_value() &&
        !record.operator_name.has_value() &&
        !record.literal_or_parameter_ref.has_value();
    const bool grouping_key =
        consumer == api::EngineCanonicalExpressionConsumer::aggregate &&
        record.expression_kind == api::RelationalExpressionKind::kIdentifier &&
        record.child_expression_ids.empty() &&
        record.bound_name_uuid.has_value() &&
        IsCanonicalUuid(*record.bound_name_uuid) &&
        !record.function_uuid.has_value() && !record.literal_kind.has_value() &&
        !record.operator_name.has_value() &&
        !record.literal_or_parameter_ref.has_value();
    const bool input_identifier =
        (consumer == api::EngineCanonicalExpressionConsumer::join ||
         consumer == api::EngineCanonicalExpressionConsumer::filter ||
         consumer == api::EngineCanonicalExpressionConsumer::projection) &&
        record.expression_kind == api::RelationalExpressionKind::kIdentifier &&
        record.child_expression_ids.empty() &&
        record.bound_name_uuid.has_value() &&
        IsCanonicalUuid(*record.bound_name_uuid) &&
        !record.function_uuid.has_value() && !record.literal_kind.has_value() &&
        !record.operator_name.has_value() &&
        !record.literal_or_parameter_ref.has_value();
    const bool exact_kind =
        (slot.slot_kind ==
             CanonicalRelationalExpressionRowSlotKind::materialized_function &&
         materialized_function) ||
        (slot.slot_kind ==
             CanonicalRelationalExpressionRowSlotKind::grouping_key &&
         grouping_key) ||
        (slot.slot_kind ==
             CanonicalRelationalExpressionRowSlotKind::input_identifier &&
         input_identifier);
    if (!exact_kind ||
        !bound_values
             .emplace(slot.expression_id, &row_values[slot.row_ordinal])
             .second) {
      result.detail =
          "predicate row slot kind does not match its bound expression";
      return result;
    }
  }
  result.unique_row_ordinal_count = bound_ordinals.size();

  static const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  std::unordered_map<std::uint32_t, std::string> descriptor_type_names;
  const auto resolve_core_type = [&](const std::uint32_t descriptor_id,
                                     std::string* type_name) {
    const auto descriptor = descriptors.find(descriptor_id);
    if (descriptor == descriptors.end() ||
        !IsCanonicalUuid(descriptor->second->type_uuid) ||
        !core_manifest.ok()) {
      return false;
    }
    const auto row = std::ranges::find_if(
        core_manifest.manifest.descriptor_rows, [&](const auto& candidate) {
          return TypedUuidText(candidate.descriptor_uuid) ==
                 descriptor->second->type_uuid;
        });
    if (row == core_manifest.manifest.descriptor_rows.end()) return false;
    *type_name = row->stable_name;
    return true;
  };
  const auto descriptor_value_bytes = [&](const std::uint32_t descriptor_id,
                                          const std::uint64_t payload_bytes,
                                          std::uint64_t* bytes) {
    const auto descriptor = descriptors.find(descriptor_id);
    if (descriptor == descriptors.end()) return false;
    std::string type_name;
    if (!resolve_core_type(descriptor_id, &type_name)) {
      result.callbacks.descriptor_type_resolution_may_execute = true;
      result.core_bound_complete = false;
      return false;
    }
    descriptor_type_names.emplace(descriptor_id, type_name);
    api::EngineDescriptor built;
    if (!build_expected_descriptor(*descriptor->second,
                                   descriptor->second->nullability,
                                   type_name, &built)) {
      return false;
    }
    std::uint64_t dynamic = 0;
    *bytes = sizeof(api::EngineTypedValue);
    return descriptor_dynamic_bytes(built, &dynamic) &&
           account(bytes, dynamic) && account(bytes, payload_bytes);
  };

  struct EvaluationBound {
    std::uint64_t output_value_bytes{0};
    std::uint64_t peak_live_value_bytes{0};
    std::size_t maximum_depth{0};
    bool may_be_non_null{false};
    std::string canonical_type_name;
  };
  std::unordered_map<std::uint32_t, EvaluationBound> memo;
  std::unordered_set<std::uint32_t> visiting;
  std::unordered_set<std::uint32_t> reachable_descriptors;
  std::size_t reachable_edges = 0;
  std::function<bool(std::uint32_t, EvaluationBound*)> bound_expression;
  bound_expression = [&](const std::uint32_t expression_id,
                         EvaluationBound* output) {
    if (cancelled()) return false;
    const auto cached = memo.find(expression_id);
    if (cached != memo.end()) {
      *output = cached->second;
      return true;
    }
    if (!visiting.insert(expression_id).second) {
      result.detail = "predicate expression graph is cyclic";
      return false;
    }
    const auto leave = [&](const bool ok) {
      visiting.erase(expression_id);
      return ok;
    };
    const auto expression = expressions.find(expression_id);
    if (expression == expressions.end() ||
        !descriptors.contains(expression->second->result_descriptor_id)) {
      result.detail = "predicate expression or result descriptor is absent";
      return leave(false);
    }
    const auto& record = *expression->second;
    reachable_descriptors.insert(record.result_descriptor_id);
    EvaluationBound computed;
    const auto slot = bound_values.find(expression_id);
    if (slot != bound_values.end()) {
      if (!value_envelope_bytes(*slot->second,
                                &computed.output_value_bytes)) {
        result.detail = "predicate row value envelope overflowed";
        return leave(false);
      }
      computed.peak_live_value_bytes = computed.output_value_bytes;
      computed.maximum_depth = 1;
      computed.may_be_non_null = slot->second->any_non_null;
      computed.canonical_type_name =
          slot->second->descriptor.canonical_type_name;
      memo.emplace(expression_id, computed);
      *output = computed;
      return leave(true);
    }

    const bool has_function = record.function_uuid.has_value();
    const bool has_name = record.bound_name_uuid.has_value();
    const bool has_literal = record.literal_kind.has_value();
    const bool has_operator = record.operator_name.has_value();
    const bool has_payload = record.literal_or_parameter_ref.has_value();
    bool exact_shape = false;
    switch (record.expression_kind) {
      case api::RelationalExpressionKind::kLiteral:
        exact_shape = record.child_expression_ids.empty() && has_literal &&
                      has_payload && !has_function && !has_name &&
                      !has_operator;
        break;
      case api::RelationalExpressionKind::kParenthesized:
        exact_shape = record.child_expression_ids.size() == 1 &&
                      !has_function && !has_name && !has_literal &&
                      !has_operator && !has_payload;
        break;
      case api::RelationalExpressionKind::kUnary:
        exact_shape = record.child_expression_ids.size() == 1 &&
                      has_operator && !has_function && !has_name &&
                      !has_literal && !has_payload;
        break;
      case api::RelationalExpressionKind::kBinary:
        exact_shape = record.child_expression_ids.size() == 2 &&
                      has_operator && !has_function && !has_name &&
                      !has_literal && !has_payload;
        break;
      case api::RelationalExpressionKind::kParameter:
      case api::RelationalExpressionKind::kIdentifier:
      case api::RelationalExpressionKind::kFunctionCall:
        exact_shape = false;
        break;
    }
    if (!exact_shape) {
      if (record.expression_kind ==
              api::RelationalExpressionKind::kFunctionCall &&
          record.function_uuid.has_value()) {
        result.callbacks.function_evaluator_may_execute = true;
      }
      result.detail =
          "predicate expression is malformed or lacks a prepared row slot";
      return leave(false);
    }

    if (record.child_expression_ids.size() >
        std::numeric_limits<std::size_t>::max() - reachable_edges) {
      result.detail = "predicate reachable edge count overflowed";
      return leave(false);
    }
    reachable_edges += record.child_expression_ids.size();
    const auto declared_extent = [&] {
      const auto& descriptor =
          *descriptors.at(record.result_descriptor_id);
      return static_cast<std::uint64_t>(descriptor.width.value_or(0)) +
             static_cast<std::uint64_t>(descriptor.precision.value_or(0)) +
             static_cast<std::uint64_t>(descriptor.scale.value_or(0)) + 64;
    }();
    const auto finish_leaf = [&](const std::uint64_t payload,
                                 const bool may_be_non_null) {
      if (!descriptor_value_bytes(record.result_descriptor_id, payload,
                                  &computed.output_value_bytes)) {
        return false;
      }
      computed.peak_live_value_bytes = computed.output_value_bytes;
      computed.maximum_depth = 1;
      computed.may_be_non_null = may_be_non_null;
      const auto name = descriptor_type_names.find(record.result_descriptor_id);
      if (name != descriptor_type_names.end()) {
        computed.canonical_type_name = name->second;
      }
      return true;
    };
    const auto finish_unary = [&](const EvaluationBound& child,
                                  const std::uint64_t payload,
                                  const std::uint64_t copies) {
      if (!descriptor_value_bytes(record.result_descriptor_id, payload,
                                  &computed.output_value_bytes)) {
        return false;
      }
      std::uint64_t frame = 0;
      if (!add(child.output_value_bytes, computed.output_value_bytes, &frame) ||
          !multiply(frame, copies, &frame)) {
        return false;
      }
      computed.peak_live_value_bytes =
          std::max(child.peak_live_value_bytes, frame);
      if (child.maximum_depth == std::numeric_limits<std::size_t>::max()) {
        return false;
      }
      computed.maximum_depth = child.maximum_depth + 1;
      computed.may_be_non_null = child.may_be_non_null;
      const auto name = descriptor_type_names.find(record.result_descriptor_id);
      if (name != descriptor_type_names.end()) {
        computed.canonical_type_name = name->second;
      }
      return true;
    };
    const auto finish_binary = [&](const EvaluationBound& left,
                                   const EvaluationBound& right,
                                   const std::uint64_t payload,
                                   const std::uint64_t copies) {
      if (!descriptor_value_bytes(record.result_descriptor_id, payload,
                                  &computed.output_value_bytes)) {
        return false;
      }
      std::uint64_t right_phase = 0;
      std::uint64_t frame = 0;
      if (!add(left.output_value_bytes, right.peak_live_value_bytes,
               &right_phase) ||
          !add(left.output_value_bytes, right.output_value_bytes, &frame) ||
          !add(frame, computed.output_value_bytes, &frame) ||
          !multiply(frame, copies, &frame)) {
        return false;
      }
      computed.peak_live_value_bytes = std::max(
          {left.peak_live_value_bytes, right_phase, frame});
      const auto child_depth =
          std::max(left.maximum_depth, right.maximum_depth);
      if (child_depth == std::numeric_limits<std::size_t>::max()) {
        return false;
      }
      computed.maximum_depth = child_depth + 1;
      computed.may_be_non_null =
          left.may_be_non_null || right.may_be_non_null;
      const auto name = descriptor_type_names.find(record.result_descriptor_id);
      if (name != descriptor_type_names.end()) {
        computed.canonical_type_name = name->second;
      }
      return true;
    };

    bool bounded = false;
    switch (record.expression_kind) {
      case api::RelationalExpressionKind::kLiteral: {
        std::uint64_t payload = declared_extent;
        bounded = account(&payload,
                          record.literal_or_parameter_ref->size()) &&
                  finish_leaf(
                      payload,
                      *record.literal_kind !=
                          api::RelationalLiteralKind::kNull);
        if (bounded &&
            !multiply(computed.output_value_bytes, 4,
                      &computed.peak_live_value_bytes)) {
          bounded = false;
        }
        if (bounded &&
            !LiteralKindAdmitsType(*record.literal_kind,
                                   computed.canonical_type_name)) {
          bounded = false;
        }
        if (bounded &&
            *record.literal_kind == api::RelationalLiteralKind::kUuid &&
            !IsCanonicalUuid(*record.literal_or_parameter_ref)) {
          bounded = false;
        }
        if (bounded &&
            *record.literal_kind == api::RelationalLiteralKind::kBoolean) {
          const auto boolean =
              UpperAscii(*record.literal_or_parameter_ref);
          bounded = boolean == "TRUE" || boolean == "FALSE";
        }
        break;
      }
      case api::RelationalExpressionKind::kParenthesized: {
        EvaluationBound child;
        bounded = bound_expression(record.child_expression_ids.front(),
                                   &child) &&
                  finish_unary(child, child.output_value_bytes, 3) &&
                  SameCanonicalType(child.canonical_type_name,
                                    computed.canonical_type_name);
        break;
      }
      case api::RelationalExpressionKind::kUnary: {
        const auto operation = UpperAscii(*record.operator_name);
        if (operation != "+" && operation != "-" && operation != "NOT") {
          break;
        }
        EvaluationBound child;
        std::uint64_t payload = declared_extent;
        bounded = bound_expression(record.child_expression_ids.front(),
                                   &child) &&
                  account(&payload, child.output_value_bytes) &&
                  finish_unary(child, payload, 4);
        const auto child_record =
            expressions.at(record.child_expression_ids.front());
        const bool null_marker =
            operation == "NOT" &&
            child_record->expression_kind ==
                api::RelationalExpressionKind::kLiteral &&
            child_record->literal_kind == api::RelationalLiteralKind::kNull;
        if (bounded && null_marker) {
          // IS NOT NULL carries this unary node as a syntax marker; the
          // canonical evaluator does not execute it as boolean NOT.
        } else if (bounded && operation == "NOT") {
          bounded =
              dt::CanonicalTypeIdFromStableName(child.canonical_type_name) ==
                  dt::CanonicalTypeId::boolean &&
              dt::CanonicalTypeIdFromStableName(
                  computed.canonical_type_name) ==
                  dt::CanonicalTypeId::boolean;
        } else if (bounded) {
          bounded = IsNumericType(dt::CanonicalTypeIdFromStableName(
                        child.canonical_type_name)) &&
                    SameCanonicalType(child.canonical_type_name,
                                      computed.canonical_type_name);
        }
        break;
      }
      case api::RelationalExpressionKind::kBinary: {
        const auto operation = UpperAscii(*record.operator_name);
        const bool arithmetic = operation == "+" || operation == "-" ||
                                operation == "*" || operation == "/" ||
                                operation == "%";
        const bool logical = operation == "AND" || operation == "OR" ||
                             operation == "XOR";
        const bool concat = operation == "||";
        const bool match = operation == "LIKE" || operation == "ILIKE";
        const bool comparison = IsComparisonOperator(operation) ||
                                operation == "IS";
        if (!arithmetic && !logical && !concat && !match && !comparison) {
          break;
        }
        EvaluationBound left;
        EvaluationBound right;
        std::uint64_t payload = declared_extent;
        if (!bound_expression(record.child_expression_ids[0], &left) ||
            !bound_expression(record.child_expression_ids[1], &right)) {
          break;
        }
        if (logical || match || comparison) {
          if (!account(&payload, 8)) break;
        } else if (!account(&payload, left.output_value_bytes) ||
                   !account(&payload, right.output_value_bytes)) {
          break;
        }
        if (match ||
            (IsComparisonOperator(operation) &&
             (dt::CanonicalTypeIdFromStableName(left.canonical_type_name) ==
                  dt::CanonicalTypeId::character ||
              dt::CanonicalTypeIdFromStableName(right.canonical_type_name) ==
                  dt::CanonicalTypeId::character))) {
          result.callbacks.collation_comparison_may_execute = true;
        }
        const auto left_descriptor = descriptors.at(
            expressions.at(record.child_expression_ids[0])
                ->result_descriptor_id);
        const auto right_descriptor = descriptors.at(
            expressions.at(record.child_expression_ids[1])
                ->result_descriptor_id);
        if (IsComparisonOperator(operation) && left.may_be_non_null &&
            right.may_be_non_null &&
            (left_descriptor->timezone_profile_id.has_value() ||
             right_descriptor->timezone_profile_id.has_value())) {
          result.callbacks.timezone_normalization_may_execute = true;
        }
        if (match || IsComparisonOperator(operation)) {
          std::uint64_t handoff = 0;
          if (!add(left.output_value_bytes, right.output_value_bytes,
                   &handoff)) {
            break;
          }
          result.callback_handoff_peak_bytes =
              std::max(result.callback_handoff_peak_bytes, handoff);
        }
        bounded = finish_binary(left, right, payload, match ? 6 : 4);
        if (!bounded) break;
        const auto left_type =
            dt::CanonicalTypeIdFromStableName(left.canonical_type_name);
        const auto right_type =
            dt::CanonicalTypeIdFromStableName(right.canonical_type_name);
        const auto result_type =
            dt::CanonicalTypeIdFromStableName(computed.canonical_type_name);
        if (arithmetic) {
          bounded = IsNumericType(left_type) &&
                    IsNumericType(right_type) &&
                    SameCanonicalType(left.canonical_type_name,
                                      right.canonical_type_name) &&
                    SameCanonicalType(left.canonical_type_name,
                                      computed.canonical_type_name) &&
                    (operation != "%" ||
                     IsBoundedSignedIntegerType(left.canonical_type_name));
        } else if (logical) {
          bounded = left_type == dt::CanonicalTypeId::boolean &&
                    right_type == dt::CanonicalTypeId::boolean &&
                    result_type == dt::CanonicalTypeId::boolean;
        } else if (concat) {
          bounded = left_type == dt::CanonicalTypeId::character &&
                    right_type == dt::CanonicalTypeId::character &&
                    result_type == dt::CanonicalTypeId::character;
        } else if (match) {
          bounded = left_type == dt::CanonicalTypeId::character &&
                    right_type == dt::CanonicalTypeId::character &&
                    result_type == dt::CanonicalTypeId::boolean;
        } else if (operation == "IS") {
          const auto right_expression =
              expressions.at(record.child_expression_ids[1]);
          bool null_right =
              right_expression->expression_kind ==
                  api::RelationalExpressionKind::kLiteral &&
              right_expression->literal_kind ==
                  api::RelationalLiteralKind::kNull;
          if (!null_right &&
              right_expression->expression_kind ==
                  api::RelationalExpressionKind::kUnary &&
              right_expression->operator_name.has_value() &&
              UpperAscii(*right_expression->operator_name) == "NOT" &&
              right_expression->child_expression_ids.size() == 1) {
            const auto null_child = expressions.find(
                right_expression->child_expression_ids.front());
            null_right =
                null_child != expressions.end() &&
                null_child->second->expression_kind ==
                    api::RelationalExpressionKind::kLiteral &&
                null_child->second->literal_kind ==
                    api::RelationalLiteralKind::kNull;
          }
          bounded = null_right &&
                    left.canonical_type_name != "null" &&
                    left_type != dt::CanonicalTypeId::unknown &&
                    result_type == dt::CanonicalTypeId::boolean;
        } else {
          std::string common_type;
          const bool exact_type_match =
              SameCanonicalType(left.canonical_type_name,
                                right.canonical_type_name);
          const bool lossless_signed_promotion =
              LosslessBoundedSignedComparisonTypes(
                  left.canonical_type_name, right.canonical_type_name,
                  &common_type);
          bounded = result_type == dt::CanonicalTypeId::boolean &&
                    (exact_type_match || lossless_signed_promotion) &&
                    IsAdmittedComparisonType(
                        exact_type_match ? left.canonical_type_name
                                         : common_type);
        }
        break;
      }
      case api::RelationalExpressionKind::kParameter:
      case api::RelationalExpressionKind::kIdentifier:
      case api::RelationalExpressionKind::kFunctionCall:
        break;
    }
    if (!bounded) {
      if (record.expression_kind ==
              api::RelationalExpressionKind::kFunctionCall &&
          record.function_uuid.has_value()) {
        result.callbacks.function_evaluator_may_execute = true;
      }
      if (result.detail.empty()) {
        result.detail =
            "predicate is outside prepared row-expression admission or its "
            "logical-memory bound overflowed";
      }
      return leave(false);
    }
    memo.emplace(expression_id, computed);
    *output = computed;
    return leave(true);
  };

  EvaluationBound root;
  result.core_bound_complete = true;
  if (!bound_expression(root_expression_id, &root)) return result;
  if (dt::CanonicalTypeIdFromStableName(root.canonical_type_name) !=
      dt::CanonicalTypeId::boolean) {
    result.detail = "predicate root type is not canonical boolean";
    return result;
  }
  result.reachable_expression_count = memo.size();
  result.reachable_descriptor_count = reachable_descriptors.size();
  result.reachable_edge_count = reachable_edges;
  result.maximum_expression_depth = root.maximum_depth;
  for (const auto& [expression_id, ignored] : bound_values) {
    (void)ignored;
    if (!memo.contains(expression_id)) {
      result.detail = "predicate row slot is outside the reachable graph";
      return result;
    }
  }

  for (const auto descriptor_id : reachable_descriptors) {
    std::string type_name;
    const auto bound_slot = std::ranges::find_if(
        row_binding.slots, [&](const auto& slot) {
          return slot.descriptor_id == descriptor_id;
        });
    if (bound_slot != row_binding.slots.end()) {
      type_name = row_values[bound_slot->row_ordinal]
                      .descriptor.canonical_type_name;
    } else if (!resolve_core_type(descriptor_id, &type_name)) {
      result.callbacks.descriptor_type_resolution_may_execute = true;
      result.core_bound_complete = false;
      continue;
    }
    std::uint64_t entry_bytes =
        sizeof(std::pair<const std::uint32_t, std::string>);
    if (!account_string(&entry_bytes, type_name) ||
        !account(&resident, entry_bytes)) {
      result.detail = "predicate descriptor-type cache bound overflowed";
      return result;
    }
  }
  result.runtime_resident_structural_bytes = resident;

  std::uint64_t pending_entries = 0;
  if (!add(reachable_edges, 1, &pending_entries)) {
    result.detail = "predicate pending carrier bound overflowed";
    return result;
  }
  std::uint64_t row_descriptor_resident =
      sizeof(std::unordered_set<std::uint32_t>);
  std::uint64_t prepare_descriptors = 0;
  std::uint64_t prepare_binding =
      sizeof(std::unordered_map<
          std::uint32_t, const api::EngineTypedValue*>) +
      sizeof(std::unordered_map<
          std::size_t, CanonicalRelationalExpressionRowSlotKind>) +
      sizeof(std::unordered_set<std::uint32_t>) +
      sizeof(std::unordered_set<std::uint32_t>) +
      sizeof(std::vector<std::uint32_t>);
  if (!account_count(&row_descriptor_resident, row_descriptor_ids.size(),
                     sizeof(std::uint32_t)) ||
      !add(row_descriptor_resident, maximum_expected_descriptor,
           &prepare_descriptors) ||
      !account(&prepare_descriptors, sizeof(std::string)) ||
      !account_count(
          &prepare_binding, bound_values.size(),
          sizeof(std::pair<const std::uint32_t,
                           const api::EngineTypedValue*>)) ||
      !account_count(
          &prepare_binding, bound_ordinals.size(),
          sizeof(std::pair<const std::size_t,
                           CanonicalRelationalExpressionRowSlotKind>)) ||
      !account_count(&prepare_binding, bound_expression_ids.size(),
                     sizeof(std::uint32_t)) ||
      !account_count(&prepare_binding, memo.size(),
                     sizeof(std::uint32_t)) ||
      !account_count(&prepare_binding, pending_entries,
                     sizeof(std::uint32_t)) ||
      !account(&prepare_binding, row_descriptor_resident)) {
    result.detail = "predicate row-binding carrier bound overflowed";
    return result;
  }
  result.prepare_row_binding_peak_structural_bytes =
      std::max(prepare_descriptors, prepare_binding);
  result.active_row_binding_resident_bytes =
      sizeof(std::unordered_map<
          std::uint32_t, const api::EngineTypedValue*>);
  if (!account_count(
          &result.active_row_binding_resident_bytes, bound_values.size(),
          sizeof(std::pair<const std::uint32_t,
                           const api::EngineTypedValue*>))) {
    result.detail = "predicate active row-binding bound overflowed";
    return result;
  }
  result.evaluation_peak_value_bytes = root.peak_live_value_bytes;
  std::size_t maximum_transient_string_bytes = 1;
  std::uint64_t maximum_transient_descriptor_dynamic_bytes = 0;
  for (const auto& [expression_id, ignored] : memo) {
    (void)ignored;
    const auto& record = *expressions.at(expression_id);
    if (record.operator_name.has_value()) {
      maximum_transient_string_bytes =
          std::max(maximum_transient_string_bytes,
                   record.operator_name->size());
    }
    const auto type_name =
        descriptor_type_names.find(record.result_descriptor_id);
    if (type_name != descriptor_type_names.end()) {
      maximum_transient_string_bytes =
          std::max(maximum_transient_string_bytes,
                   type_name->second.size());
    }
    api::EngineDescriptor transient_descriptor;
    const auto slot = bound_values.find(expression_id);
    if (slot != bound_values.end()) {
      transient_descriptor = slot->second->descriptor;
    } else if (!build_expected_descriptor(
                   *descriptors.at(record.result_descriptor_id),
                   descriptors.at(record.result_descriptor_id)->nullability,
                   memo.at(expression_id).canonical_type_name,
                   &transient_descriptor)) {
      result.detail = "predicate transient descriptor is unresolved";
      return result;
    }
    std::uint64_t descriptor_dynamic = 0;
    if (!descriptor_dynamic_bytes(transient_descriptor,
                                  &descriptor_dynamic)) {
      result.detail = "predicate transient descriptor bound overflowed";
      return result;
    }
    maximum_transient_descriptor_dynamic_bytes =
        std::max(maximum_transient_descriptor_dynamic_bytes,
                 descriptor_dynamic);
  }
  std::uint64_t per_frame_structural_bytes =
      sizeof(api::EngineCanonicalExpressionEvaluationRequest) +
      sizeof(api::EngineCanonicalExpressionEvaluationResult) +
      4 * sizeof(api::EngineTypedValue) +
      2 * sizeof(api::EngineDescriptor) + 6 * sizeof(std::string);
  std::uint64_t per_frame_dynamic_string_bytes = 0;
  std::uint64_t string_with_terminator = 0;
  std::uint64_t per_frame_dynamic_descriptor_bytes = 0;
  if (!add(maximum_transient_string_bytes, 1,
           &string_with_terminator) ||
      !multiply(string_with_terminator, 6,
                &per_frame_dynamic_string_bytes) ||
      !multiply(maximum_transient_descriptor_dynamic_bytes, 4,
                &per_frame_dynamic_descriptor_bytes) ||
      !account(&per_frame_structural_bytes,
               per_frame_dynamic_string_bytes) ||
      !account(&per_frame_structural_bytes,
               per_frame_dynamic_descriptor_bytes) ||
      !multiply(root.maximum_depth, per_frame_structural_bytes,
                &result.evaluation_peak_transient_structural_bytes) ||
      !account_count(&result.evaluation_peak_transient_structural_bytes,
                     root.maximum_depth, sizeof(std::uint32_t))) {
    result.detail = "predicate evaluation stack bound overflowed";
    return result;
  }
  std::uint64_t evaluation_phase = 0;
  if (!add(result.active_row_binding_resident_bytes,
           result.evaluation_peak_value_bytes, &evaluation_phase) ||
      !account(&evaluation_phase,
               result.evaluation_peak_transient_structural_bytes) ||
      !account(&evaluation_phase, result.callback_handoff_peak_bytes)) {
    result.detail = "predicate evaluation phase bound overflowed";
    return result;
  }
  std::uint64_t transient_peak =
      std::max(result.prepare_row_binding_peak_structural_bytes,
               evaluation_phase);
  if (!add(result.runtime_resident_structural_bytes, transient_peak,
           &result.core_expression_peak_bytes)) {
    result.detail = "predicate core expression peak overflowed";
    return result;
  }
  result.callback_memory_complete =
      !result.callbacks.descriptor_type_resolution_may_execute &&
      !result.callbacks.collation_comparison_may_execute &&
      !result.callbacks.timezone_normalization_may_execute &&
      !result.callbacks.function_evaluator_may_execute;
  result.core_bound_exact = false;
  result.ok = true;
  result.detail.clear();
  return result;
}

bool CanonicalRelationalComparisonAuthorityRequiredV1(
    const api::EngineDescriptor& left,
    const api::EngineDescriptor& right) {
  return dt::CanonicalTypeIdFromStableName(left.canonical_type_name) ==
             dt::CanonicalTypeId::character ||
         dt::CanonicalTypeIdFromStableName(right.canonical_type_name) ==
             dt::CanonicalTypeId::character ||
         left.encoded_descriptor.find("timezone_profile_id=") !=
             std::string::npos ||
         right.encoded_descriptor.find("timezone_profile_id=") !=
             std::string::npos;
}

bool BindCanonicalRelationalComparisonAuthorityV1(
    const api::EngineTypedValue& left,
    const api::EngineTypedValue& right,
    const CanonicalRelationalExpressionRuntimeServices& services,
    std::optional<int>* precomputed_comparison,
    std::string* refusal_detail) {
  if (precomputed_comparison == nullptr || refusal_detail == nullptr) {
    return false;
  }
  precomputed_comparison->reset();
  if (!CanonicalRelationalComparisonAuthorityRequiredV1(
          left.descriptor, right.descriptor) ||
      left.isSqlNull() ||
      right.isSqlNull()) {
    return true;
  }
  if (!services.comparison_evaluator) {
    *refusal_detail =
        "QOW-DIAG-RCP024-COMPARISON-AUTHORITY-REFUSAL-V1:descriptor-bound "
        "comparison authority is unavailable";
    return false;
  }
  int comparison = 0;
  std::string diagnostic_id;
  if (!services.comparison_evaluator(
          left, right, &comparison, &diagnostic_id, refusal_detail)) {
    *refusal_detail =
        (diagnostic_id.empty()
             ? "QOW-DIAG-RCP024-COMPARISON-AUTHORITY-REFUSAL-V1"
             : diagnostic_id) +
        ":" + *refusal_detail;
    return false;
  }
  *precomputed_comparison = comparison;
  return true;
}

CanonicalRelationalExpressionRuntime::CanonicalRelationalExpressionRuntime(
    const api::TypedRelationalDag& dag,
    CanonicalRelationalExpressionRuntimeServices services)
    : services_(std::move(services)) {
  for (const auto& descriptor : dag.descriptors) {
    descriptors_.emplace(descriptor.descriptor_id, &descriptor);
  }
  for (const auto& expression : dag.expressions) {
    expressions_.emplace(expression.expression_id, &expression);
  }
}

bool CanonicalRelationalExpressionRuntime::PrepareRowBinding(
    const std::uint32_t root_expression_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const CanonicalRelationalExpressionRowView row_values,
    const api::EngineCanonicalExpressionConsumer consumer,
    ActiveRowBinding* prepared,
    std::string* refusal_detail) const {
  if (prepared == nullptr || refusal_detail == nullptr) return false;
  *prepared = {};
  if (!expressions_.contains(root_expression_id)) {
    *refusal_detail =
        "materialized row expression root is absent from the bound graph";
    return false;
  }
  if (row_binding.row_descriptor_ids.empty() ||
      row_binding.row_descriptor_ids.size() != row_values.size() ||
      (!row_binding.row_nullable.empty() &&
       row_binding.row_nullable.size() !=
           row_binding.row_descriptor_ids.size())) {
    *refusal_detail =
        "materialized row width differs from its bound descriptor width";
    return false;
  }
  const auto row_value_at = [&](const std::size_t ordinal)
      -> const api::EngineTypedValue* {
    if (ordinal < row_values.first.size()) {
      return &row_values.first[ordinal];
    }
    const auto second_ordinal = ordinal - row_values.first.size();
    return second_ordinal < row_values.second.size()
               ? &row_values.second[second_ordinal]
               : nullptr;
  };
  for (std::size_t ordinal = 0;
       ordinal < row_binding.row_descriptor_ids.size(); ++ordinal) {
    const auto descriptor_id = row_binding.row_descriptor_ids[ordinal];
    const auto descriptor = descriptors_.find(descriptor_id);
    bool duplicate_descriptor = false;
    for (std::size_t prior = 0; prior < ordinal; ++prior) {
      if (row_binding.row_descriptor_ids[prior] == descriptor_id) {
        duplicate_descriptor = true;
        break;
      }
    }
    if (descriptor_id == 0 || descriptor == descriptors_.end() ||
        duplicate_descriptor ||
        descriptor->second->nullability == api::RelationalNullability::kUnknown) {
      *refusal_detail =
          "materialized row descriptor identity is unresolved or ambiguous";
      return false;
    }

    const auto* value = row_value_at(ordinal);
    if (value == nullptr) {
      *refusal_detail = "materialized row value ordinal is absent";
      return false;
    }
    if (value->descriptor.canonical_type_name.empty()) {
      *refusal_detail = "materialized row value type is unresolved";
      return false;
    }
    const auto effective_nullability =
        row_binding.row_nullable.empty()
            ? descriptor->second->nullability
            : (row_binding.row_nullable[ordinal]
                   ? api::RelationalNullability::kNullable
                   : api::RelationalNullability::kNonNull);
    if (!SamePersistedRowDescriptor(*descriptor->second, value->descriptor,
                                    effective_nullability)) {
      *refusal_detail =
          "materialized row value lost its full canonical descriptor identity:" +
          std::string("identity=") +
          (api::QowCanonicalDescriptorIdentityV1(value->descriptor) ? "1"
                                                                    : "0") +
          ":uuid=" +
          (value->descriptor.descriptor_uuid.canonical ==
                   descriptor->second->descriptor_uuid
               ? "1"
               : "0") +
          ":scalar=" +
          (value->descriptor.descriptor_kind == "scalar" ? "1" : "0") +
          ":type=" + value->descriptor.canonical_type_name +
          ":collation=" +
          (descriptor->second->collation_uuid.has_value() ? "1" : "0") +
          ":timezone=" +
          (descriptor->second->timezone_profile_id.has_value() ? "1" : "0") +
          ":width=" +
          (descriptor->second->width.has_value() ? "1" : "0") +
          ":precision=" +
          (descriptor->second->precision.has_value() ? "1" : "0") +
          ":scale=" +
          (descriptor->second->scale.has_value() ? "1" : "0");
      return false;
    }
    if (value->state == api::EngineValueState::sql_null) {
      if (!value->is_null || !value->encoded_value.empty() ||
          !value->binary_value.empty() ||
          effective_nullability != api::RelationalNullability::kNullable) {
        *refusal_detail =
            "materialized row SQL NULL is malformed or non-nullable";
        return false;
      }
    } else if (value->state != api::EngineValueState::value || value->is_null) {
      *refusal_detail =
          "materialized row contains a non-value runtime sentinel";
      return false;
    }
  }
  for (std::size_t slot_ordinal = 0;
       slot_ordinal < row_binding.slots.size(); ++slot_ordinal) {
    const auto& slot = row_binding.slots[slot_ordinal];
    bool duplicate_expression = false;
    bool incompatible_duplicate_ordinal = false;
    for (std::size_t prior = 0; prior < slot_ordinal; ++prior) {
      const auto& prior_slot = row_binding.slots[prior];
      duplicate_expression = duplicate_expression ||
                             prior_slot.expression_id == slot.expression_id;
      if (prior_slot.row_ordinal == slot.row_ordinal &&
          (prior_slot.slot_kind !=
               CanonicalRelationalExpressionRowSlotKind::input_identifier ||
           slot.slot_kind !=
               CanonicalRelationalExpressionRowSlotKind::input_identifier)) {
        incompatible_duplicate_ordinal = true;
      }
    }
    if (duplicate_expression) {
      *refusal_detail = "materialized row slot expression is duplicated";
      return false;
    }
    if (incompatible_duplicate_ordinal) {
      *refusal_detail = "materialized row slot ordinal is duplicated";
      return false;
    }
    const auto expression = expressions_.find(slot.expression_id);
    if (slot.expression_id == 0 || expression == expressions_.end() ||
        slot.descriptor_id == 0 ||
        expression->second->result_descriptor_id != slot.descriptor_id ||
        slot.row_ordinal >= row_values.size() ||
        row_binding.row_descriptor_ids[slot.row_ordinal] !=
            slot.descriptor_id) {
      *refusal_detail =
          "materialized row slot identity, descriptor, or ordinal is invalid";
      return false;
    }
    const auto& bound_expression = *expression->second;
    const bool exact_materialized_function =
        bound_expression.expression_kind ==
            api::RelationalExpressionKind::kFunctionCall &&
        bound_expression.function_uuid.has_value() &&
        IsCanonicalUuid(*bound_expression.function_uuid) &&
        !bound_expression.bound_name_uuid.has_value() &&
        !bound_expression.literal_kind.has_value() &&
        !bound_expression.operator_name.has_value() &&
        !bound_expression.literal_or_parameter_ref.has_value();
    const bool exact_grouping_key =
        consumer == api::EngineCanonicalExpressionConsumer::aggregate &&
        bound_expression.expression_kind ==
            api::RelationalExpressionKind::kIdentifier &&
        bound_expression.child_expression_ids.empty() &&
        bound_expression.bound_name_uuid.has_value() &&
        IsCanonicalUuid(*bound_expression.bound_name_uuid) &&
        !bound_expression.function_uuid.has_value() &&
        !bound_expression.literal_kind.has_value() &&
        !bound_expression.operator_name.has_value() &&
        !bound_expression.literal_or_parameter_ref.has_value();
    const bool exact_input_identifier =
        (consumer == api::EngineCanonicalExpressionConsumer::join ||
         consumer == api::EngineCanonicalExpressionConsumer::filter ||
         consumer == api::EngineCanonicalExpressionConsumer::projection) &&
        bound_expression.expression_kind ==
            api::RelationalExpressionKind::kIdentifier &&
        bound_expression.child_expression_ids.empty() &&
        bound_expression.bound_name_uuid.has_value() &&
        IsCanonicalUuid(*bound_expression.bound_name_uuid) &&
        !bound_expression.function_uuid.has_value() &&
        !bound_expression.literal_kind.has_value() &&
        !bound_expression.operator_name.has_value() &&
        !bound_expression.literal_or_parameter_ref.has_value();
    const bool exact_slot_kind =
        (slot.slot_kind ==
             CanonicalRelationalExpressionRowSlotKind::materialized_function &&
         exact_materialized_function) ||
        (slot.slot_kind ==
             CanonicalRelationalExpressionRowSlotKind::grouping_key &&
         exact_grouping_key) ||
        (slot.slot_kind ==
             CanonicalRelationalExpressionRowSlotKind::input_identifier &&
         exact_input_identifier);
    if (!exact_slot_kind) {
      *refusal_detail =
          "materialized row slot kind does not match its exact bound expression";
      return false;
    }
  }
  const auto bound_slot = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(row_binding.slots, [&](const auto& slot) {
      return slot.expression_id == expression_id;
    });
  };
  std::size_t graph_visits = 0;
  std::size_t maximum_graph_visits = 0;
  if (expressions_.empty() ||
      expressions_.size() >
          std::numeric_limits<std::size_t>::max() / expressions_.size()) {
    *refusal_detail = "materialized row expression graph bound overflowed";
    return false;
  }
  maximum_graph_visits = expressions_.size() * expressions_.size();
  const auto validate_reachable = [&](auto&& self,
                                      const std::uint32_t expression_id,
                                      const std::size_t depth) -> bool {
    if (depth > expressions_.size() ||
        ++graph_visits > maximum_graph_visits) {
      *refusal_detail =
          "materialized row expression graph is cyclic or exceeds its "
          "finite visit ceiling";
      return false;
    }
    const auto expression = expressions_.find(expression_id);
    if (expression == expressions_.end()) {
      *refusal_detail =
          "materialized row expression has a dangling expression child";
      return false;
    }
    if (bound_slot(expression_id) != row_binding.slots.end()) return true;
    const auto& record = *expression->second;
    const bool has_function = record.function_uuid.has_value();
    const bool has_name = record.bound_name_uuid.has_value();
    const bool has_literal = record.literal_kind.has_value();
    const bool has_operator = record.operator_name.has_value();
    const bool has_payload = record.literal_or_parameter_ref.has_value();
    const bool has_typed_literal = record.literal_typed_value_v1.has_value();
    const bool has_typed_parameter =
        record.parameter_typed_value_v1.has_value();
    bool exact_shape = false;
    switch (record.expression_kind) {
      case api::RelationalExpressionKind::kLiteral:
        exact_shape = record.child_expression_ids.empty() && has_literal &&
                      (has_payload != has_typed_literal) && !has_function &&
                      !has_name && !has_operator;
        break;
      case api::RelationalExpressionKind::kParenthesized:
        exact_shape = record.child_expression_ids.size() == 1 &&
                      !has_function && !has_name && !has_literal &&
                      !has_operator && !has_payload;
        break;
      case api::RelationalExpressionKind::kUnary:
        exact_shape = record.child_expression_ids.size() == 1 &&
                      has_operator && !has_function && !has_name &&
                      !has_literal && !has_payload;
        break;
      case api::RelationalExpressionKind::kBinary:
        exact_shape = record.child_expression_ids.size() == 2 &&
                      has_operator && !has_function && !has_name &&
                      !has_literal && !has_payload;
        break;
      case api::RelationalExpressionKind::kParameter:
        exact_shape = record.child_expression_ids.empty() &&
                      (has_payload != has_typed_parameter) && !has_literal &&
                      !has_typed_literal && !has_function && !has_name &&
                      !has_operator;
        break;
      case api::RelationalExpressionKind::kIdentifier:
      case api::RelationalExpressionKind::kFunctionCall:
        exact_shape = false;
        break;
    }
    if (!exact_shape) {
      *refusal_detail =
          "reachable row expression is malformed or lacks a prepared slot";
      return false;
    }
    for (const auto child : record.child_expression_ids) {
      if (!self(self, child, depth + 1)) return false;
    }
    return true;
  };
  if (!validate_reachable(validate_reachable, root_expression_id, 1)) {
    return false;
  }
  const auto reaches_slot = [&](auto&& self,
                                const std::uint32_t expression_id,
                                const std::uint32_t target,
                                const std::size_t depth,
                                std::size_t* visits) -> bool {
    if (depth > expressions_.size() ||
        ++*visits > maximum_graph_visits) {
      return false;
    }
    if (expression_id == target) return true;
    const auto expression = expressions_.find(expression_id);
    if (expression == expressions_.end()) return false;
    for (const auto child : expression->second->child_expression_ids) {
      if (self(self, child, target, depth + 1, visits)) return true;
    }
    return false;
  };
  for (const auto& slot : row_binding.slots) {
    std::size_t visits = 0;
    if (!reaches_slot(reaches_slot, root_expression_id, slot.expression_id,
                      1, &visits)) {
      *refusal_detail =
          "materialized row slot is outside the expression graph";
      return false;
    }
  }
  prepared->binding = &row_binding;
  prepared->values = row_values;
  return true;
}

const api::EngineTypedValue*
CanonicalRelationalExpressionRuntime::ActiveRowValue(
    const std::uint32_t expression_id) const {
  if (active_row_binding_ == nullptr ||
      active_row_binding_->binding == nullptr) {
    return nullptr;
  }
  const auto slot = std::ranges::find_if(
      active_row_binding_->binding->slots, [&](const auto& candidate) {
        return candidate.expression_id == expression_id;
      });
  if (slot == active_row_binding_->binding->slots.end()) return nullptr;
  if (slot->row_ordinal < active_row_binding_->values.first.size()) {
    return &active_row_binding_->values.first[slot->row_ordinal];
  }
  const auto second_ordinal =
      slot->row_ordinal - active_row_binding_->values.first.size();
  return second_ordinal < active_row_binding_->values.second.size()
             ? &active_row_binding_->values.second[second_ordinal]
             : nullptr;
}

bool CanonicalRelationalExpressionRuntime::BindDescriptorType(
    const std::uint32_t descriptor_id,
    const std::string_view type_name,
    std::string* refusal_detail) {
  if (refusal_detail == nullptr || type_name.empty() || type_name == "null" ||
      !descriptors_.contains(descriptor_id)) {
    if (refusal_detail != nullptr) {
      *refusal_detail = "expression result descriptor type is unresolved";
    }
    return false;
  }
  const auto [entry, inserted] = descriptor_type_names_.emplace(
      descriptor_id, std::string(type_name));
  if (!inserted && !SameCanonicalType(entry->second, type_name)) {
    *refusal_detail = "expression descriptor has incompatible inferred types";
    return false;
  }
  return true;
}

bool CanonicalRelationalExpressionRuntime::ResolveDescriptorType(
    const api::RelationalTypeDescriptor& descriptor,
    std::string* canonical_type_name,
    std::string* refusal_detail) const {
  if (canonical_type_name == nullptr || refusal_detail == nullptr) {
    return false;
  }
  canonical_type_name->clear();
  if (!IsCanonicalUuid(descriptor.type_uuid)) {
    *refusal_detail = "expression descriptor type UUID is not canonical";
    return false;
  }
  static const auto core_manifest =
      dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (core_manifest.ok()) {
    const auto row = std::ranges::find_if(
        core_manifest.manifest.descriptor_rows, [&](const auto& candidate) {
          return TypedUuidText(candidate.descriptor_uuid) ==
                 descriptor.type_uuid;
        });
    if (row != core_manifest.manifest.descriptor_rows.end()) {
      *canonical_type_name = row->stable_name;
      return true;
    }
    // int64 has separate current descriptor and value-type identities. Resolve
    // the type UUID only through the exact current type/codec registry row;
    // never treat an arbitrary UUID as an alias for the core datatype.
    const auto int64_row = std::ranges::find_if(
        core_manifest.manifest.descriptor_rows, [](const auto& candidate) {
          return candidate.stable_name == "int64";
        });
    if (int64_row != core_manifest.manifest.descriptor_rows.end()) {
      const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
          "019d0000-0000-7000-8000-00000000d701",
          core_manifest.manifest.catalog_epoch, 1,
          TypedUuidText(int64_row->descriptor_uuid),
          int64_row->descriptor_epoch);
      if (identity.ok && identity.row.type_uuid == descriptor.type_uuid) {
        *canonical_type_name = int64_row->stable_name;
        return true;
      }
    }
  }
  if (services_.descriptor_type_resolver) {
    std::string diagnostic_id;
    if (services_.descriptor_type_resolver(
            descriptor.type_uuid, canonical_type_name, &diagnostic_id,
            refusal_detail) &&
        !canonical_type_name->empty() &&
        dt::CanonicalTypeIdFromStableName(*canonical_type_name) !=
            dt::CanonicalTypeId::unknown) {
      return true;
    }
    if (!diagnostic_id.empty()) {
      *refusal_detail = diagnostic_id + ":" + *refusal_detail;
    }
  }
  if (refusal_detail->empty()) {
    *refusal_detail =
        "expression descriptor type UUID has no current engine binding";
  }
  return false;
}

bool CanonicalRelationalExpressionRuntime::InferType(
    const std::uint32_t expression_id,
    const std::optional<std::string_view> expected_type,
    std::string* canonical_type_name,
    std::string* refusal_detail) {
  if (canonical_type_name == nullptr || refusal_detail == nullptr) return false;
  canonical_type_name->clear();
  refusal_detail->clear();
  return InferTypeInternal(expression_id, expected_type, canonical_type_name,
                           refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::InferTypeInternal(
    const std::uint32_t expression_id,
    const std::optional<std::string_view> expected_type,
    std::string* canonical_type_name,
    std::string* refusal_detail) {
  const auto found = expressions_.find(expression_id);
  if (found == expressions_.end()) {
    *refusal_detail = "expression handle is absent from the bound graph";
    return false;
  }
  if (!inference_stack_.insert(expression_id).second) {
    *refusal_detail = "expression graph cycle reached scalar runtime";
    return false;
  }
  const auto leave = [&](const bool result) {
    inference_stack_.erase(expression_id);
    return result;
  };
  const auto& expression = *found->second;
  const auto finish_type = [&](std::string type_name) {
    if (type_name == "null" && expected_type.has_value()) {
      type_name = std::string(*expected_type);
    }
    if (expected_type.has_value() &&
        !SameCanonicalType(type_name, *expected_type)) {
      *refusal_detail = "expression type contradicts its bound consumer type";
      return leave(false);
    }
    if (expected_type.has_value()) type_name = std::string(*expected_type);
    if (type_name != "null" &&
        !BindDescriptorType(expression.result_descriptor_id, type_name,
                            refusal_detail)) {
      return leave(false);
    }
    *canonical_type_name = std::move(type_name);
    return leave(true);
  };

  if (active_row_binding_ != nullptr) {
    const auto* materialized = ActiveRowValue(expression_id);
    if (materialized != nullptr) {
      return finish_type(materialized->descriptor.canonical_type_name);
    }
  }

  switch (expression.expression_kind) {
    case api::RelationalExpressionKind::kLiteral: {
      if (!expression.literal_kind.has_value() ||
          (expression.literal_or_parameter_ref.has_value() ==
           expression.literal_typed_value_v1.has_value())) {
        *refusal_detail = "literal expression payload is incomplete";
        return leave(false);
      }
      const auto descriptor = descriptors_.find(
          expression.result_descriptor_id);
      if (descriptor == descriptors_.end()) {
        *refusal_detail = "literal result descriptor is absent";
        return leave(false);
      }
      auto type_name = BoundLiteralType(
          expression, *descriptor->second, expected_type);
      if (!expected_type.has_value()) {
        std::string descriptor_type;
        std::string descriptor_detail;
        if (ResolveDescriptorType(*descriptor->second, &descriptor_type,
                                  &descriptor_detail) &&
            LiteralKindAdmitsType(*expression.literal_kind,
                                  descriptor_type)) {
          type_name = std::move(descriptor_type);
        }
      }
      if (type_name == "null" && !expected_type.has_value()) {
        std::string descriptor_type;
        std::string descriptor_detail;
        if (ResolveDescriptorType(*descriptor->second, &descriptor_type,
                                  &descriptor_detail)) {
          type_name = std::move(descriptor_type);
        }
      }
      if (type_name.empty()) {
        *refusal_detail =
            "literal type is outside the bound object-free scalar profile";
        return leave(false);
      }
      if (*expression.literal_kind == api::RelationalLiteralKind::kUuid &&
          !IsCanonicalUuid(*expression.literal_or_parameter_ref)) {
        *refusal_detail = "UUID literal is not canonical lowercase text";
        return leave(false);
      }
      if (*expression.literal_kind == api::RelationalLiteralKind::kBoolean) {
        const auto boolean = UpperAscii(*expression.literal_or_parameter_ref);
        if (boolean != "TRUE" && boolean != "FALSE") {
          *refusal_detail = "boolean literal payload is invalid";
          return leave(false);
        }
      }
      return finish_type(type_name);
    }
    case api::RelationalExpressionKind::kParenthesized: {
      std::string child_type;
      if (!InferTypeInternal(expression.child_expression_ids.front(),
                             expected_type, &child_type, refusal_detail)) {
        return leave(false);
      }
      return finish_type(std::move(child_type));
    }
    case api::RelationalExpressionKind::kUnary: {
      const auto operation = UpperAscii(*expression.operator_name);
      if (operation != "NOT" && operation != "+" && operation != "-") {
        *refusal_detail = "unary scalar operator is not admitted";
        return leave(false);
      }
      std::string child_type;
      const auto child_expected =
          operation == "NOT"
              ? std::optional<std::string_view>("boolean")
              : expected_type;
      if (!InferTypeInternal(expression.child_expression_ids.front(),
                             child_expected, &child_type, refusal_detail)) {
        return leave(false);
      }
      if (operation != "NOT" &&
          !IsNumericType(dt::CanonicalTypeIdFromStableName(child_type))) {
        *refusal_detail = "unary arithmetic operand is not numeric";
        return leave(false);
      }
      return finish_type(operation == "NOT" ? "boolean" : child_type);
    }
    case api::RelationalExpressionKind::kBinary: {
      const auto operation = UpperAscii(*expression.operator_name);
      if (operation == "+" || operation == "-" || operation == "*" ||
          operation == "/" || operation == "%") {
        std::string left_type;
        std::string right_type;
        if (!InferTypeInternal(expression.child_expression_ids[0],
                               std::nullopt, &left_type, refusal_detail) ||
            !InferTypeInternal(expression.child_expression_ids[1],
                               std::nullopt, &right_type, refusal_detail)) {
          return leave(false);
        }
        if (left_type == "null" && right_type == "null") {
          if (!expected_type.has_value() ||
              !IsNumericType(dt::CanonicalTypeIdFromStableName(
                  std::string(*expected_type)))) {
            *refusal_detail =
                "arithmetic operand descriptors have no bound numeric type";
            return leave(false);
          }
          left_type = std::string(*expected_type);
          right_type = left_type;
        } else {
          const std::string operand_type =
              left_type == "null" ? right_type : left_type;
          if (left_type == "null" &&
              !InferTypeInternal(expression.child_expression_ids[0],
                                 operand_type, &left_type, refusal_detail)) {
            return leave(false);
          }
          if (right_type == "null" &&
              !InferTypeInternal(expression.child_expression_ids[1],
                                 operand_type, &right_type, refusal_detail)) {
            return leave(false);
          }
        }
        if (!SameCanonicalType(left_type, right_type) ||
            !IsNumericType(dt::CanonicalTypeIdFromStableName(left_type)) ||
            (operation == "%" &&
             !IsBoundedSignedIntegerType(left_type))) {
          *refusal_detail =
              "arithmetic operands have incompatible or unsupported types";
          return leave(false);
        }
        return finish_type(left_type);
      }
      if (operation == "AND" || operation == "OR" || operation == "XOR") {
        std::string left_type;
        std::string right_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], "boolean",
                               &left_type, refusal_detail) ||
            !InferTypeInternal(expression.child_expression_ids[1], "boolean",
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        return finish_type("boolean");
      }
      if (operation == "||") {
        std::string left_type;
        std::string right_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], "text",
                               &left_type, refusal_detail) ||
            !InferTypeInternal(expression.child_expression_ids[1], "text",
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        return finish_type("text");
      }
      if (operation == "LIKE" || operation == "ILIKE") {
        std::string left_type;
        std::string right_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], "text",
                               &left_type, refusal_detail) ||
            !InferTypeInternal(expression.child_expression_ids[1], "text",
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        return finish_type("boolean");
      }
      if (operation == "IS") {
        bool negate = false;
        if (!IsNullPredicateRight(expression.child_expression_ids[1], &negate)) {
          *refusal_detail = "IS expression is not a bound NULL predicate";
          return leave(false);
        }
        std::string left_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], std::nullopt,
                               &left_type, refusal_detail) ||
            left_type == "null") {
          if (refusal_detail->empty()) {
            *refusal_detail = "IS NULL operand descriptor type is unresolved";
          }
          return leave(false);
        }
        return finish_type("boolean");
      }
      if (IsComparisonOperator(operation)) {
        std::string left_type;
        std::string right_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], std::nullopt,
                               &left_type, refusal_detail) ||
            !InferTypeInternal(expression.child_expression_ids[1], std::nullopt,
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        if (left_type == "null" && right_type == "null") {
          *refusal_detail = "comparison operand descriptors have no bound type";
          return leave(false);
        }
        std::string operand_type =
            left_type == "null" ? right_type : left_type;
        if (right_type != "null" &&
            !SameCanonicalType(right_type, operand_type) &&
            !LosslessBoundedSignedComparisonTypes(
                operand_type, right_type, &operand_type)) {
          *refusal_detail = "comparison operands have incompatible types";
          return leave(false);
        }
        if (!IsAdmittedComparisonType(operand_type)) {
          *refusal_detail =
              "comparison type is outside canonical scalar admission";
          return leave(false);
        }
        if (left_type == "null" &&
            !InferTypeInternal(expression.child_expression_ids[0], operand_type,
                               &left_type, refusal_detail)) {
          return leave(false);
        }
        if (right_type == "null" &&
            !InferTypeInternal(expression.child_expression_ids[1], operand_type,
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        return finish_type("boolean");
      }
      *refusal_detail = "binary scalar operator is not admitted";
      return leave(false);
    }
    case api::RelationalExpressionKind::kParameter: {
      if (!expression.parameter_typed_value_v1.has_value() ||
          expression.literal_or_parameter_ref.has_value() ||
          expression.literal_typed_value_v1.has_value() ||
          expression.literal_kind.has_value() ||
          !expression.child_expression_ids.empty() ||
          expression.function_uuid.has_value() ||
          expression.bound_name_uuid.has_value() ||
          expression.operator_name.has_value()) {
        *refusal_detail =
            "parameter expression lacks an engine-bound runtime value";
        return leave(false);
      }
      const auto descriptor =
          descriptors_.find(expression.result_descriptor_id);
      if (descriptor == descriptors_.end()) {
        *refusal_detail = "parameter result descriptor is absent";
        return leave(false);
      }
      std::string type_name;
      if (!ResolveDescriptorType(*descriptor->second, &type_name,
                                 refusal_detail)) {
        return leave(false);
      }
      return finish_type(std::move(type_name));
    }
    case api::RelationalExpressionKind::kIdentifier:
      *refusal_detail = "identifier expression requires an input row binding";
      return leave(false);
    case api::RelationalExpressionKind::kFunctionCall: {
      const bool spatial_point =
          !expression.function_uuid.has_value() &&
          expression.operator_name == "POINT" &&
          expression.child_expression_ids.size() == 2 &&
          !expression.bound_name_uuid.has_value() &&
          !expression.literal_kind.has_value() &&
          !expression.literal_or_parameter_ref.has_value();
      if (spatial_point) {
        for (const auto child_expression_id :
             expression.child_expression_ids) {
          std::string child_type;
          if (!InferTypeInternal(child_expression_id, std::nullopt,
                                 &child_type, refusal_detail) ||
              !IsNumericType(
                  dt::CanonicalTypeIdFromStableName(child_type))) {
            if (refusal_detail->empty()) {
              *refusal_detail =
                  "POINT coordinate is not a bound canonical numeric value";
            }
            return leave(false);
          }
        }
        const auto descriptor =
            descriptors_.find(expression.result_descriptor_id);
        std::string result_type;
        if (descriptor == descriptors_.end() ||
            !ResolveDescriptorType(*descriptor->second, &result_type,
                                   refusal_detail) ||
            result_type != "geometry") {
          if (refusal_detail->empty()) {
            *refusal_detail =
                "POINT result is not bound to the canonical geometry type";
          }
          return leave(false);
        }
        return finish_type(std::move(result_type));
      }
      if (!expression.function_uuid.has_value() ||
          !IsCanonicalUuid(*expression.function_uuid) ||
          expression.bound_name_uuid.has_value() ||
          expression.literal_kind.has_value() ||
          expression.operator_name.has_value() ||
          expression.literal_or_parameter_ref.has_value()) {
        *refusal_detail =
            "function expression identity or payload shape is invalid";
        return leave(false);
      }
      for (const auto child_expression_id :
           expression.child_expression_ids) {
        std::string child_type;
        if (!InferTypeInternal(child_expression_id, std::nullopt, &child_type,
                               refusal_detail) ||
            child_type.empty() || child_type == "null") {
          if (refusal_detail->empty()) {
            *refusal_detail =
                "function argument descriptor type is unresolved";
          }
          return leave(false);
        }
      }
      std::string result_type;
      if (expected_type.has_value()) {
        result_type = std::string(*expected_type);
      } else {
        const auto descriptor = descriptors_.find(
            expression.result_descriptor_id);
        if (descriptor == descriptors_.end() ||
            !ResolveDescriptorType(*descriptor->second, &result_type,
                                   refusal_detail)) {
          return leave(false);
        }
      }
      return finish_type(std::move(result_type));
    }
  }
  *refusal_detail = "expression kind is outside scalar runtime admission";
  return leave(false);
}

bool CanonicalRelationalExpressionRuntime::BuildDescriptor(
    const std::uint32_t descriptor_id,
    const std::string_view type_name,
    api::EngineDescriptor* descriptor,
    std::string* refusal_detail) const {
  if (descriptor == nullptr || refusal_detail == nullptr) return false;
  const auto found = descriptors_.find(descriptor_id);
  if (found == descriptors_.end() || type_name.empty() || type_name == "null") {
    *refusal_detail = "scalar result descriptor is unresolved";
    return false;
  }
  const auto& source = *found->second;
  descriptor->descriptor_uuid.canonical = source.descriptor_uuid;
  descriptor->descriptor_kind = "scalar";
  descriptor->canonical_type_name = std::string(type_name);
  const char* nullability = "unknown";
  if (source.nullability == api::RelationalNullability::kNonNull) {
    nullability = "non_null";
  } else if (source.nullability == api::RelationalNullability::kNullable) {
    nullability = "nullable";
  }
  descriptor->encoded_descriptor =
      "type_uuid=" + source.type_uuid + ";nullability=" + nullability;
  if (source.collation_uuid.has_value()) {
    descriptor->encoded_descriptor += ";collation_uuid=" + *source.collation_uuid;
  }
  if (source.timezone_profile_id.has_value()) {
    descriptor->encoded_descriptor +=
        ";timezone_profile_id=" + *source.timezone_profile_id;
  }
  if (source.width.has_value()) {
    descriptor->encoded_descriptor += ";width=" + std::to_string(*source.width);
  }
  if (source.precision.has_value()) {
    descriptor->encoded_descriptor +=
        ";precision=" + std::to_string(*source.precision);
  }
  if (source.scale.has_value()) {
    descriptor->encoded_descriptor += ";scale=" + std::to_string(*source.scale);
  }
  return true;
}

bool CanonicalRelationalExpressionRuntime::FinishValue(
    const std::uint32_t descriptor_id,
    api::EngineTypedValue value,
    api::EngineTypedValue* output,
    std::string* refusal_detail) const {
  const auto descriptor = descriptors_.find(descriptor_id);
  if (output == nullptr || refusal_detail == nullptr ||
      descriptor == descriptors_.end()) {
    return false;
  }
  if (value.isSqlNull() &&
      descriptor->second->nullability == api::RelationalNullability::kNonNull) {
    *refusal_detail = "SQL NULL contradicts a non-null expression descriptor";
    return false;
  }
  if (!value.isSqlNull() &&
      (value.state != api::EngineValueState::value || value.is_null)) {
    *refusal_detail = "scalar expression produced a non-value runtime sentinel";
    return false;
  }
  *output = std::move(value);
  return true;
}

bool CanonicalRelationalExpressionRuntime::IsNullPredicateRight(
    const std::uint32_t expression_id,
    bool* negate) const {
  if (negate == nullptr) return false;
  *negate = false;
  const auto found = expressions_.find(expression_id);
  if (found == expressions_.end()) return false;
  const auto& expression = *found->second;
  if (expression.expression_kind == api::RelationalExpressionKind::kLiteral &&
      expression.literal_kind == api::RelationalLiteralKind::kNull) {
    return true;
  }
  if (expression.expression_kind != api::RelationalExpressionKind::kUnary ||
      UpperAscii(*expression.operator_name) != "NOT") {
    return false;
  }
  const auto child = expressions_.find(expression.child_expression_ids.front());
  if (child == expressions_.end() ||
      child->second->expression_kind !=
          api::RelationalExpressionKind::kLiteral ||
      child->second->literal_kind != api::RelationalLiteralKind::kNull) {
    return false;
  }
  *negate = true;
  return true;
}

bool CanonicalRelationalExpressionRuntime::Evaluate(
    const std::uint32_t expression_id,
    const std::string_view expected_type,
    api::EngineTypedValue* value,
    std::string* refusal_detail) {
  return EvaluateForConsumer(
      expression_id, expected_type,
      api::EngineCanonicalExpressionConsumer::projection, value,
      refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::EvaluateForConsumer(
    const std::uint32_t expression_id,
    const std::string_view expected_type,
    const api::EngineCanonicalExpressionConsumer consumer,
    api::EngineTypedValue* value,
    std::string* refusal_detail) {
  if (value == nullptr || refusal_detail == nullptr || expected_type.empty()) {
    return false;
  }
  *value = {};
  value->state = api::EngineValueState::error;
  refusal_detail->clear();
  if (consumer == api::EngineCanonicalExpressionConsumer::unspecified ||
      static_cast<std::uint8_t>(consumer) >
          static_cast<std::uint8_t>(
              api::EngineCanonicalExpressionConsumer::subquery)) {
    *refusal_detail = "canonical expression consumer is not bound";
    return false;
  }
  std::string inferred_type;
  if (!InferType(expression_id, expected_type, &inferred_type, refusal_detail)) {
    return false;
  }
  const auto previous_consumer = active_consumer_;
  active_consumer_ = consumer;
  const bool evaluated =
      EvaluateInternal(expression_id, expected_type, value, refusal_detail);
  active_consumer_ = previous_consumer;
  return evaluated;
}

bool CanonicalRelationalExpressionRuntime::InferTypeForConsumer(
    const std::uint32_t expression_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const std::vector<api::EngineTypedValue>& row_values,
    const api::EngineCanonicalExpressionConsumer consumer,
    std::string* canonical_type_name,
    std::string* refusal_detail) {
  if (canonical_type_name == nullptr || refusal_detail == nullptr) {
    return false;
  }
  canonical_type_name->clear();
  refusal_detail->clear();
  if (active_row_binding_ != nullptr) {
    *refusal_detail = "nested materialized row evaluation is not admitted";
    return false;
  }
  ActiveRowBinding prepared;
  if (!PrepareRowBinding(expression_id, row_binding,
                         CanonicalRelationalExpressionRowView{row_values, {}},
                         consumer,
                         &prepared, refusal_detail)) {
    return false;
  }
  const auto previous_consumer = active_consumer_;
  active_row_binding_ = &prepared;
  active_consumer_ = consumer;
  const bool inferred = InferType(expression_id, std::nullopt,
                                  canonical_type_name, refusal_detail);
  active_consumer_ = previous_consumer;
  active_row_binding_ = nullptr;
  return inferred;
}

bool CanonicalRelationalExpressionRuntime::EvaluateForConsumer(
    const std::uint32_t expression_id,
    const std::string_view expected_type,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const std::vector<api::EngineTypedValue>& row_values,
    const api::EngineCanonicalExpressionConsumer consumer,
    api::EngineTypedValue* value,
    std::string* refusal_detail) {
  return EvaluateForConsumer(
      expression_id, expected_type, row_binding,
      CanonicalRelationalExpressionRowView{row_values, {}}, consumer, value,
      refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::EvaluateForConsumer(
    const std::uint32_t expression_id,
    const std::string_view expected_type,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const CanonicalRelationalExpressionRowView row_values,
    const api::EngineCanonicalExpressionConsumer consumer,
    api::EngineTypedValue* value,
    std::string* refusal_detail) {
  if (value == nullptr || refusal_detail == nullptr) return false;
  if (active_row_binding_ != nullptr) {
    *refusal_detail = "nested materialized row evaluation is not admitted";
    return false;
  }
  ActiveRowBinding prepared;
  refusal_detail->clear();
  if (!PrepareRowBinding(expression_id, row_binding, row_values, consumer,
                         &prepared, refusal_detail)) {
    return false;
  }
  active_row_binding_ = &prepared;
  const bool evaluated = EvaluateForConsumer(
      expression_id, expected_type, consumer, value, refusal_detail);
  active_row_binding_ = nullptr;
  return evaluated;
}

bool CanonicalRelationalExpressionRuntime::EvaluatePredicate(
    const std::uint32_t expression_id,
    api::EngineSqlTruthValue* truth,
    std::string* refusal_detail) {
  return EvaluatePredicateForConsumer(
      expression_id, api::EngineCanonicalExpressionConsumer::filter, truth,
      refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::EvaluatePredicateForConsumer(
    const std::uint32_t expression_id,
    const api::EngineCanonicalExpressionConsumer consumer,
    api::EngineSqlTruthValue* truth,
    std::string* refusal_detail) {
  if (truth == nullptr || refusal_detail == nullptr) return false;
  api::EngineTypedValue value;
  if (!EvaluateForConsumer(expression_id, "boolean", consumer, &value,
                           refusal_detail)) {
    return false;
  }
  return TruthFromValue(value, truth, refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::EvaluatePredicate(
    const std::uint32_t expression_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const std::vector<api::EngineTypedValue>& row_values,
    api::EngineSqlTruthValue* truth,
    std::string* refusal_detail) {
  return EvaluatePredicateForConsumer(
      expression_id, row_binding, row_values,
      api::EngineCanonicalExpressionConsumer::aggregate, truth,
      refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::EvaluatePredicateForConsumer(
    const std::uint32_t expression_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const std::vector<api::EngineTypedValue>& row_values,
    const api::EngineCanonicalExpressionConsumer consumer,
    api::EngineSqlTruthValue* truth,
    std::string* refusal_detail) {
  return EvaluatePredicateForConsumer(
      expression_id, row_binding,
      CanonicalRelationalExpressionRowView{row_values, {}}, consumer, truth,
      refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::EvaluatePredicateForConsumer(
    const std::uint32_t expression_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const CanonicalRelationalExpressionRowView row_values,
    const api::EngineCanonicalExpressionConsumer consumer,
    api::EngineSqlTruthValue* truth,
    std::string* refusal_detail) {
  if (truth == nullptr || refusal_detail == nullptr) return false;
  api::EngineTypedValue value;
  if (!EvaluateForConsumer(expression_id, "boolean", row_binding, row_values,
                           consumer, &value, refusal_detail)) {
    return false;
  }
  return TruthFromValue(value, truth, refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::EvaluateInternal(
    const std::uint32_t expression_id,
    const std::string_view expected_type,
    api::EngineTypedValue* value,
    std::string* refusal_detail) {
  const auto& expression = *expressions_.at(expression_id);
  std::string inferred_type;
  if (!InferTypeInternal(expression_id, expected_type, &inferred_type,
                         refusal_detail)) {
    return false;
  }
  api::EngineDescriptor result_descriptor;
  if (!BuildDescriptor(expression.result_descriptor_id, inferred_type,
                       &result_descriptor, refusal_detail)) {
    return false;
  }
  const auto finish = [&](api::EngineTypedValue computed) {
    computed.descriptor = result_descriptor;
    return FinishValue(expression.result_descriptor_id, std::move(computed),
                       value, refusal_detail);
  };
  const auto evaluate_scalar = [&](const auto& scalar_request,
                                   auto* scalar_result) {
    if (api::QowEvaluateCanonicalTypedExpressionV1(
            scalar_request, scalar_result, refusal_detail)) {
      return true;
    }
    if (!scalar_result->diagnostic_id.empty()) {
      *refusal_detail = scalar_result->diagnostic_id + ":" +
                        *refusal_detail;
    }
    return false;
  };

  if (active_row_binding_ != nullptr) {
    const auto* materialized = ActiveRowValue(expression_id);
    if (materialized != nullptr) {
      return finish(*materialized);
    }
  }

  if (expression.expression_kind == api::RelationalExpressionKind::kLiteral) {
    api::EngineTypedValue literal;
    literal.descriptor = result_descriptor;
    if (*expression.literal_kind == api::RelationalLiteralKind::kNull) {
      literal.setState(api::EngineValueState::sql_null);
      return finish(std::move(literal));
    }
    literal.setState(api::EngineValueState::value);
    if(expression.literal_typed_value_v1.has_value()){
      const auto& typed=*expression.literal_typed_value_v1;
      const auto digest=scratchbird::core::hash::ComputeSha256Digest(
          typed.canonical_value_bytes);
      if(typed.value_state!="value"||typed.descriptor_generation==0||
         typed.descriptor_uuid!=result_descriptor.descriptor_uuid.canonical||
         !digest.ok()||digest.digest!=typed.canonical_value_sha256||
         typed.canonical_value_bytes.size()!=8){
        *refusal_detail="typed_value_v1 descriptor, state, bytes, or SHA differs";
        return false;
      }
      literal.binary_value=typed.canonical_value_bytes;
      return finish(std::move(literal));
    }
    std::string payload = *expression.literal_or_parameter_ref;
    if (*expression.literal_kind == api::RelationalLiteralKind::kBoolean) {
      payload = UpperAscii(payload) == "TRUE" ? "true" : "false";
    }
    if (!CanonicalizeLiteralPayload(inferred_type, result_descriptor, payload,
                                    &literal.encoded_value,
                                    refusal_detail)) {
      return false;
    }
    return finish(std::move(literal));
  }

  if (expression.expression_kind == api::RelationalExpressionKind::kParameter &&
      expression.parameter_typed_value_v1.has_value()) {
    if (!expression.child_expression_ids.empty() ||
        expression.literal_or_parameter_ref.has_value() ||
        expression.literal_typed_value_v1.has_value() ||
        expression.literal_kind.has_value() ||
        expression.function_uuid.has_value() ||
        expression.bound_name_uuid.has_value() ||
        expression.operator_name.has_value()) {
      *refusal_detail = "parameter expression shape is malformed";
      return false;
    }
    const auto& typed = *expression.parameter_typed_value_v1;
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        typed.canonical_value_bytes);
    if ((typed.value_state != "value" && typed.value_state != "null") ||
        typed.descriptor_generation == 0 ||
        typed.descriptor_uuid != result_descriptor.descriptor_uuid.canonical ||
        !digest.ok() || digest.digest != typed.canonical_value_sha256 ||
        (typed.value_state == "value" &&
         typed.canonical_value_bytes.size() != 8) ||
        (typed.value_state == "null" &&
         !typed.canonical_value_bytes.empty())) {
      *refusal_detail =
          "parameter typed_value_v1 descriptor, state, bytes, or SHA differs";
      return false;
    }
    api::EngineTypedValue parameter;
    parameter.descriptor = result_descriptor;
    if (typed.value_state == "null") {
      parameter.setState(api::EngineValueState::sql_null);
    } else {
      parameter.setState(api::EngineValueState::value);
      parameter.binary_value = typed.canonical_value_bytes;
    }
    return finish(std::move(parameter));
  }

  if (expression.expression_kind ==
      api::RelationalExpressionKind::kParenthesized) {
    api::EngineTypedValue child;
    if (!EvaluateInternal(expression.child_expression_ids.front(), inferred_type,
                          &child, refusal_detail)) {
      return false;
    }
    return finish(std::move(child));
  }

  if (expression.expression_kind == api::RelationalExpressionKind::kUnary) {
    const auto operation = UpperAscii(*expression.operator_name);
    api::EngineTypedValue child;
    const std::string_view child_type =
        operation == "NOT" ? std::string_view("boolean")
                           : std::string_view(inferred_type);
    if (!EvaluateInternal(expression.child_expression_ids.front(), child_type,
                          &child, refusal_detail)) {
      return false;
    }
    api::EngineCanonicalExpressionEvaluationRequest scalar_request;
    scalar_request.consumer = active_consumer_;
    scalar_request.left_value = child;
    scalar_request.result_descriptor = result_descriptor;
    api::EngineCanonicalExpressionEvaluationResult scalar_result;
    if (operation == "+") {
      scalar_request.operation =
          api::EngineCanonicalExpressionOperation::identity;
    } else if (operation == "-") {
      api::EngineTypedValue zero;
      zero.descriptor = child.descriptor;
      zero.encoded_value = "0";
      zero.setState(api::EngineValueState::value);
      scalar_request.operation =
          api::EngineCanonicalExpressionOperation::numeric_subtract;
      scalar_request.left_value = std::move(zero);
      scalar_request.right_value = child;
      if (!NumericContextForDescriptor(
              result_descriptor, &scalar_request.numeric_context,
              refusal_detail)) {
        return false;
      }
    } else {
      scalar_request.operation =
          api::EngineCanonicalExpressionOperation::logical_not;
    }
    if (!evaluate_scalar(scalar_request, &scalar_result)) {
      return false;
    }
    return finish(std::move(scalar_result.value));
  }

  if (expression.expression_kind ==
      api::RelationalExpressionKind::kFunctionCall) {
    if (!expression.function_uuid.has_value() &&
        expression.operator_name == "POINT" &&
        expression.child_expression_ids.size() == 2) {
      std::array<double, 2> coordinates{};
      for (std::size_t index = 0; index < coordinates.size(); ++index) {
        const auto child_expression_id =
            expression.child_expression_ids[index];
        std::string child_type;
        api::EngineTypedValue child;
        if (!InferTypeInternal(child_expression_id, std::nullopt,
                               &child_type, refusal_detail) ||
            !IsNumericType(
                dt::CanonicalTypeIdFromStableName(child_type)) ||
            !EvaluateInternal(child_expression_id, child_type, &child,
                              refusal_detail) ||
            child.isSqlNull() || !child.binary_value.empty() ||
            !ParseSpatialPointCoordinate(child.encoded_value,
                                         &coordinates[index])) {
          if (refusal_detail->empty()) {
            *refusal_detail =
                "SB_MODEL_SPATIAL_COORDINATE_INVALID_V1:POINT coordinate "
                "is not a finite canonical numeric value";
          }
          return false;
        }
      }
      api::EngineTypedValue point;
      point.descriptor = result_descriptor;
      point.binary_value = EncodeSpatialPoint2d(coordinates[0], coordinates[1]);
      point.setState(api::EngineValueState::value);
      return finish(std::move(point));
    }
    if (!services_.function_evaluator) {
      *refusal_detail =
          "QOW-DIAG-RCP024-FUNCTION-AUTHORITY-REFUSAL-V1:bound function "
          "runtime is unavailable";
      return false;
    }
    std::vector<api::EngineTypedValue> arguments;
    arguments.reserve(expression.child_expression_ids.size());
    for (const auto child_expression_id : expression.child_expression_ids) {
      std::string child_type;
      if (!InferTypeInternal(child_expression_id, std::nullopt, &child_type,
                             refusal_detail)) {
        return false;
      }
      api::EngineTypedValue argument;
      if (!EvaluateInternal(child_expression_id, child_type, &argument,
                            refusal_detail)) {
        return false;
      }
      arguments.push_back(std::move(argument));
    }
    api::EngineTypedValue computed;
    std::string diagnostic_id;
    if (!services_.function_evaluator(
            *expression.function_uuid, arguments, &computed, &diagnostic_id,
            refusal_detail)) {
      *refusal_detail =
          (diagnostic_id.empty()
               ? "QOW-DIAG-RCP024-FUNCTION-DISPATCH-REFUSAL-V1"
               : diagnostic_id) +
          ":" + *refusal_detail;
      return false;
    }
    if (computed.isSqlNull()) {
      if (!computed.encoded_value.empty() || !computed.binary_value.empty()) {
        *refusal_detail =
            "QOW-DIAG-RCP024-FUNCTION-RESULT-REFUSAL-V1:function SQL NULL "
            "carries substitute payload";
        return false;
      }
      computed.setState(api::EngineValueState::sql_null);
    }
    api::EngineCanonicalExpressionEvaluationRequest scalar_request;
    scalar_request.consumer = active_consumer_;
    scalar_request.operation =
        api::EngineCanonicalExpressionOperation::scalar_function;
    scalar_request.result_descriptor = result_descriptor;
    scalar_request.precomputed_value = std::move(computed);
    api::EngineCanonicalExpressionEvaluationResult scalar_result;
    if (!evaluate_scalar(scalar_request, &scalar_result)) {
      return false;
    }
    return finish(std::move(scalar_result.value));
  }

  if (expression.expression_kind != api::RelationalExpressionKind::kBinary) {
    *refusal_detail = "expression kind has no object-free evaluator";
    return false;
  }
  const auto operation = UpperAscii(*expression.operator_name);
  if (operation == "+" || operation == "-" || operation == "*" ||
      operation == "/" || operation == "%") {
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], inferred_type,
                          &left, refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], inferred_type,
                          &right, refusal_detail)) {
      return false;
    }
    api::EngineCanonicalExpressionOperation numeric_operation =
        api::EngineCanonicalExpressionOperation::numeric_add;
    if (operation == "-") {
      numeric_operation =
          api::EngineCanonicalExpressionOperation::numeric_subtract;
    } else if (operation == "*") {
      numeric_operation =
          api::EngineCanonicalExpressionOperation::numeric_multiply;
    } else if (operation == "/") {
      numeric_operation =
          api::EngineCanonicalExpressionOperation::numeric_divide;
    } else if (operation == "%") {
      numeric_operation =
          api::EngineCanonicalExpressionOperation::numeric_modulo;
    }
    api::EngineCanonicalExpressionEvaluationRequest scalar_request;
    scalar_request.consumer = active_consumer_;
    scalar_request.operation = numeric_operation;
    scalar_request.left_value = std::move(left);
    scalar_request.right_value = std::move(right);
    scalar_request.result_descriptor = result_descriptor;
    if (!NumericContextForDescriptor(
            result_descriptor, &scalar_request.numeric_context,
            refusal_detail)) {
      return false;
    }
    api::EngineCanonicalExpressionEvaluationResult scalar_result;
    if (!evaluate_scalar(scalar_request, &scalar_result)) {
      return false;
    }
    return finish(std::move(scalar_result.value));
  }

  if (operation == "AND" || operation == "OR" || operation == "XOR") {
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], "boolean", &left,
                          refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], "boolean", &right,
                          refusal_detail)) {
      return false;
    }
    api::EngineCanonicalExpressionEvaluationRequest scalar_request;
    scalar_request.consumer = active_consumer_;
    scalar_request.operation =
        operation == "AND"
            ? api::EngineCanonicalExpressionOperation::logical_and
            : operation == "OR"
                  ? api::EngineCanonicalExpressionOperation::logical_or
                  : api::EngineCanonicalExpressionOperation::logical_xor;
    scalar_request.left_value = std::move(left);
    scalar_request.right_value = std::move(right);
    scalar_request.result_descriptor = result_descriptor;
    api::EngineCanonicalExpressionEvaluationResult scalar_result;
    if (!evaluate_scalar(scalar_request, &scalar_result)) {
      return false;
    }
    return finish(std::move(scalar_result.value));
  }

  if (operation == "||") {
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], "text", &left,
                          refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], "text", &right,
                          refusal_detail)) {
      return false;
    }
    api::EngineCanonicalExpressionEvaluationRequest scalar_request;
    scalar_request.consumer = active_consumer_;
    scalar_request.operation =
        api::EngineCanonicalExpressionOperation::text_concat;
    scalar_request.left_value = std::move(left);
    scalar_request.right_value = std::move(right);
    scalar_request.result_descriptor = result_descriptor;
    api::EngineCanonicalExpressionEvaluationResult scalar_result;
    if (!evaluate_scalar(scalar_request, &scalar_result)) {
      return false;
    }
    return finish(std::move(scalar_result.value));
  }

  if (operation == "LIKE" || operation == "ILIKE") {
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], "text", &left,
                          refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], "text", &right,
                          refusal_detail)) {
      return false;
    }
    if (!services_.comparison_evaluator) {
      *refusal_detail =
          "QOW-DIAG-RCP024-COLLATION-AUTHORITY-REFUSAL-V1:LIKE requires "
          "engine-owned collation authority";
      return false;
    }
    auto authority_left = left;
    auto authority_right = right;
    if (authority_left.isSqlNull()) {
      authority_left.encoded_value.clear();
      authority_left.is_null = false;
      authority_left.setState(api::EngineValueState::value);
    }
    if (authority_right.isSqlNull()) {
      authority_right.encoded_value.clear();
      authority_right.is_null = false;
      authority_right.setState(api::EngineValueState::value);
    }
    int ignored_comparison = 0;
    std::string diagnostic_id;
    if (!services_.comparison_evaluator(
            authority_left, authority_right, &ignored_comparison,
            &diagnostic_id, refusal_detail)) {
      *refusal_detail =
          (diagnostic_id.empty()
               ? "QOW-DIAG-RCP024-COLLATION-AUTHORITY-REFUSAL-V1"
               : diagnostic_id) +
          ":" + *refusal_detail;
      return false;
    }
    api::EngineCanonicalExpressionEvaluationRequest scalar_request;
    scalar_request.consumer = active_consumer_;
    scalar_request.operation =
        operation == "LIKE"
            ? api::EngineCanonicalExpressionOperation::like
            : api::EngineCanonicalExpressionOperation::ilike;
    scalar_request.left_value = std::move(left);
    scalar_request.right_value = std::move(right);
    scalar_request.result_descriptor = result_descriptor;
    scalar_request.bound_text_authority = true;
    api::EngineCanonicalExpressionEvaluationResult scalar_result;
    if (!evaluate_scalar(scalar_request, &scalar_result)) {
      return false;
    }
    return finish(std::move(scalar_result.value));
  }

  if (operation == "IS") {
    bool negate = false;
    if (!IsNullPredicateRight(expression.child_expression_ids[1], &negate)) {
      return false;
    }
    std::string left_type;
    if (!InferTypeInternal(expression.child_expression_ids[0], std::nullopt,
                           &left_type, refusal_detail) ||
        left_type == "null") {
      return false;
    }
    api::EngineTypedValue left;
    if (!EvaluateInternal(expression.child_expression_ids[0], left_type, &left,
                          refusal_detail)) {
      return false;
    }
    api::EngineCanonicalExpressionEvaluationRequest scalar_request;
    scalar_request.consumer = active_consumer_;
    scalar_request.operation =
        negate ? api::EngineCanonicalExpressionOperation::is_not_null
               : api::EngineCanonicalExpressionOperation::is_null;
    scalar_request.left_value = std::move(left);
    scalar_request.result_descriptor = result_descriptor;
    api::EngineCanonicalExpressionEvaluationResult scalar_result;
    if (!evaluate_scalar(scalar_request, &scalar_result)) {
      return false;
    }
    return finish(std::move(scalar_result.value));
  }

  if (IsComparisonOperator(operation)) {
    std::string left_type;
    std::string right_type;
    if (!InferTypeInternal(expression.child_expression_ids[0], std::nullopt,
                           &left_type, refusal_detail) ||
        !InferTypeInternal(expression.child_expression_ids[1], std::nullopt,
                           &right_type, refusal_detail)) {
      return false;
    }
    const std::string operand_type =
        left_type == "null" ? right_type : left_type;
    const std::string_view left_expected =
        left_type == "null" ? std::string_view{operand_type}
                            : std::string_view{left_type};
    const std::string_view right_expected =
        right_type == "null" ? std::string_view{operand_type}
                             : std::string_view{right_type};
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], left_expected, &left,
                          refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], right_expected, &right,
                          refusal_detail)) {
      return false;
    }
    if (!PromoteBoundedSignedComparisonValues(
            &left, &right, refusal_detail) &&
        !SameCanonicalType(left.descriptor.canonical_type_name,
                           right.descriptor.canonical_type_name)) {
      return false;
    }
    api::EngineCanonicalExpressionOperation scalar_operation =
        api::EngineCanonicalExpressionOperation::equal;
    if (operation == "<>" || operation == "!=") {
      scalar_operation =
          api::EngineCanonicalExpressionOperation::not_equal;
    } else if (operation == "<") {
      scalar_operation = api::EngineCanonicalExpressionOperation::less_than;
    } else if (operation == "<=") {
      scalar_operation =
          api::EngineCanonicalExpressionOperation::less_than_or_equal;
    } else if (operation == ">") {
      scalar_operation =
          api::EngineCanonicalExpressionOperation::greater_than;
    } else if (operation == ">=") {
      scalar_operation =
          api::EngineCanonicalExpressionOperation::greater_than_or_equal;
    } else if (operation == "IS DISTINCT FROM") {
      scalar_operation =
          api::EngineCanonicalExpressionOperation::is_distinct_from;
    } else if (operation == "IS NOT DISTINCT FROM") {
      scalar_operation =
          api::EngineCanonicalExpressionOperation::is_not_distinct_from;
    }
    api::EngineCanonicalExpressionEvaluationRequest scalar_request;
    scalar_request.consumer = active_consumer_;
    scalar_request.operation = scalar_operation;
    scalar_request.left_value = std::move(left);
    scalar_request.right_value = std::move(right);
    scalar_request.result_descriptor = result_descriptor;
    if (!BindCanonicalRelationalComparisonAuthorityV1(
            scalar_request.left_value, scalar_request.right_value, services_,
            &scalar_request.precomputed_comparison, refusal_detail)) {
      return false;
    }
    api::EngineCanonicalExpressionEvaluationResult scalar_result;
    if (!evaluate_scalar(scalar_request, &scalar_result)) {
      return false;
    }
    return finish(std::move(scalar_result.value));
  }

  *refusal_detail = "binary scalar operator has no object-free evaluator";
  return false;
}

}  // namespace scratchbird::engine::sblr
