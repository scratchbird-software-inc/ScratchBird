// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_relational_expression.hpp"

#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "query/expression_api.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <iomanip>
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
      !expression.literal_or_parameter_ref.has_value()) {
    return {};
  }
  const auto kind = *expression.literal_kind;
  if (expected_type.has_value() &&
      LiteralKindAdmitsType(kind, *expected_type)) {
    return std::string(*expected_type);
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

}  // namespace

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
    const std::vector<api::EngineTypedValue>& row_values,
    const api::EngineCanonicalExpressionConsumer consumer,
    ActiveRowBinding* prepared,
    std::string* refusal_detail) const {
  if (prepared == nullptr || refusal_detail == nullptr) return false;
  prepared->values_by_expression.clear();
  if (!expressions_.contains(root_expression_id)) {
    *refusal_detail =
        "materialized row predicate root is absent from the bound graph";
    return false;
  }
  if (row_binding.row_descriptor_ids.empty() ||
      row_binding.row_descriptor_ids.size() != row_values.size()) {
    *refusal_detail =
        "materialized row width differs from its bound descriptor width";
    return false;
  }

  std::unordered_set<std::uint32_t> row_descriptor_ids;
  for (std::size_t ordinal = 0;
       ordinal < row_binding.row_descriptor_ids.size(); ++ordinal) {
    const auto descriptor_id = row_binding.row_descriptor_ids[ordinal];
    const auto descriptor = descriptors_.find(descriptor_id);
    if (descriptor_id == 0 || descriptor == descriptors_.end() ||
        !row_descriptor_ids.insert(descriptor_id).second ||
        descriptor->second->nullability == api::RelationalNullability::kUnknown) {
      *refusal_detail =
          "materialized row descriptor identity is unresolved or ambiguous";
      return false;
    }

    const auto& value = row_values[ordinal];
    if (value.descriptor.canonical_type_name.empty()) {
      *refusal_detail = "materialized row value type is unresolved";
      return false;
    }
    api::EngineDescriptor expected_descriptor;
    std::string descriptor_detail;
    if (!BuildDescriptor(descriptor_id,
                         value.descriptor.canonical_type_name,
                         &expected_descriptor, &descriptor_detail) ||
        !SameDescriptor(expected_descriptor, value.descriptor)) {
      *refusal_detail =
          "materialized row value lost its full canonical descriptor identity";
      return false;
    }
    if (value.state == api::EngineValueState::sql_null) {
      if (!value.is_null || !value.encoded_value.empty() ||
          !value.binary_value.empty() ||
          descriptor->second->nullability !=
              api::RelationalNullability::kNullable) {
        *refusal_detail =
            "materialized row SQL NULL is malformed or non-nullable";
        return false;
      }
    } else if (value.state != api::EngineValueState::value || value.is_null) {
      *refusal_detail =
          "materialized row contains a non-value runtime sentinel";
      return false;
    }
  }

  std::unordered_map<std::size_t,
                     CanonicalRelationalExpressionRowSlotKind>
      bound_ordinals;
  std::unordered_set<std::uint32_t> bound_expression_ids;
  for (const auto& slot : row_binding.slots) {
    if (!bound_expression_ids.insert(slot.expression_id).second) {
      prepared->values_by_expression.clear();
      *refusal_detail = "materialized row slot expression is duplicated";
      return false;
    }
    const auto [bound_ordinal, inserted_ordinal] =
        bound_ordinals.emplace(slot.row_ordinal, slot.slot_kind);
    if (!inserted_ordinal &&
        (bound_ordinal->second !=
             CanonicalRelationalExpressionRowSlotKind::input_identifier ||
         slot.slot_kind !=
             CanonicalRelationalExpressionRowSlotKind::input_identifier)) {
      prepared->values_by_expression.clear();
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
      prepared->values_by_expression.clear();
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
         consumer == api::EngineCanonicalExpressionConsumer::filter) &&
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
      prepared->values_by_expression.clear();
      *refusal_detail =
          "materialized row slot kind does not match its exact bound expression";
      return false;
    }
    prepared->values_by_expression.emplace(slot.expression_id,
                                           &row_values[slot.row_ordinal]);
  }

  std::unordered_set<std::uint32_t> reachable;
  std::vector<std::uint32_t> pending{root_expression_id};
  while (!pending.empty()) {
    const auto expression_id = pending.back();
    pending.pop_back();
    if (!reachable.insert(expression_id).second) continue;
    const auto expression = expressions_.find(expression_id);
    if (expression == expressions_.end()) {
      prepared->values_by_expression.clear();
      *refusal_detail =
          "materialized row predicate has a dangling expression child";
      return false;
    }
    if (prepared->values_by_expression.contains(expression_id)) continue;
    const auto& record = *expression->second;
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
      prepared->values_by_expression.clear();
      *refusal_detail =
          "reachable predicate expression is malformed or lacks a prepared slot";
      return false;
    }
    pending.insert(pending.end(),
                   record.child_expression_ids.begin(),
                   record.child_expression_ids.end());
  }
  for (const auto& [expression_id, ignored] :
       prepared->values_by_expression) {
    (void)ignored;
    if (!reachable.contains(expression_id)) {
      prepared->values_by_expression.clear();
      *refusal_detail =
          "materialized row slot is outside the predicate graph";
      return false;
    }
  }
  return true;
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
    const auto materialized =
        active_row_binding_->values_by_expression.find(expression_id);
    if (materialized !=
        active_row_binding_->values_by_expression.end()) {
      return finish_type(
          materialized->second->descriptor.canonical_type_name);
    }
  }

  switch (expression.expression_kind) {
    case api::RelationalExpressionKind::kLiteral: {
      if (!expression.literal_kind.has_value() ||
          !expression.literal_or_parameter_ref.has_value()) {
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
        const std::string operand_type =
            left_type == "null" ? right_type : left_type;
        if (right_type != "null" &&
            !SameCanonicalType(right_type, operand_type)) {
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
    case api::RelationalExpressionKind::kParameter:
      *refusal_detail = "parameter expression lacks an engine-bound runtime value";
      return leave(false);
    case api::RelationalExpressionKind::kIdentifier:
      *refusal_detail = "identifier expression requires an input row binding";
      return leave(false);
    case api::RelationalExpressionKind::kFunctionCall: {
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
  if (truth == nullptr || refusal_detail == nullptr) return false;
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
  const bool evaluated = EvaluatePredicateForConsumer(
      expression_id, consumer, truth, refusal_detail);
  active_row_binding_ = nullptr;
  return evaluated;
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
    const auto materialized =
        active_row_binding_->values_by_expression.find(expression_id);
    if (materialized !=
        active_row_binding_->values_by_expression.end()) {
      return finish(*materialized->second);
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
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], operand_type, &left,
                          refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], operand_type, &right,
                          refusal_detail)) {
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
    const bool descriptor_bound_comparison =
        dt::CanonicalTypeIdFromStableName(operand_type) ==
            dt::CanonicalTypeId::character ||
        scalar_request.left_value.descriptor.encoded_descriptor.find(
            "timezone_profile_id=") != std::string::npos;
    if (descriptor_bound_comparison &&
        !scalar_request.left_value.isSqlNull() &&
        !scalar_request.right_value.isSqlNull()) {
      if (!services_.comparison_evaluator) {
        *refusal_detail =
            "QOW-DIAG-RCP024-COMPARISON-AUTHORITY-REFUSAL-V1:descriptor-bound "
            "comparison authority is unavailable";
        return false;
      }
      int comparison = 0;
      std::string diagnostic_id;
      if (!services_.comparison_evaluator(
              scalar_request.left_value, scalar_request.right_value,
              &comparison, &diagnostic_id, refusal_detail)) {
        *refusal_detail =
            (diagnostic_id.empty()
                 ? "QOW-DIAG-RCP024-COMPARISON-AUTHORITY-REFUSAL-V1"
                 : diagnostic_id) +
            ":" + *refusal_detail;
        return false;
      }
      scalar_request.precomputed_comparison = comparison;
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
