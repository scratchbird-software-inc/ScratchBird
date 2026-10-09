// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/global_aggregate_projection.hpp"

#include "api_diagnostics.hpp"
#include "canonical_aggregate_registry.hpp"
#include "crud_support/crud_store.hpp"
#include "datatype_operations.hpp"
#include "datatype_catalog_manifest.hpp"
#include "descriptor_value_runtime.hpp"
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "mga_relation_store/mga_binary_fields.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace dt = scratchbird::core::datatypes;

constexpr std::string_view kOperation = "dml.global_aggregate_projection";
constexpr std::size_t kMaximumProjectionCount = 4096;

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic AggregateDiagnostic(std::string detail) {
  return MakeInvalidRequestDiagnostic(
      std::string(kOperation), std::move(detail));
}

std::string DistinctValueKey(const EngineUuid& descriptor_uuid,
                             std::string_view canonical_type,
                             std::string_view value) {
  std::string bytes(reinterpret_cast<const char*>(descriptor_uuid.bytes.data()), 16);
  AppendBinaryString(&bytes, canonical_type);
  AppendBinaryString(&bytes, value);
  return bytes;
}

bool DescriptorEmpty(const EngineDescriptor& descriptor) {
  return descriptor.descriptor_uuid.is_nil() &&
         descriptor.descriptor_kind.empty() &&
         descriptor.canonical_type_name.empty() &&
         descriptor.encoded_descriptor.empty();
}

bool DescriptorExactlyMatches(const EngineDescriptor& left,
                              const EngineDescriptor& right) {
  return left == right;
}

bool TypedValueIsEmpty(const EngineTypedValue& value) {
  return DescriptorEmpty(value.descriptor) && value.encoded_value.empty() &&
         value.binary_value.empty() && !value.is_null &&
         value.state == EngineValueState::value;
}

bool SafeOutputAlias(std::string_view alias) {
  if (alias.empty() || alias.size() > 1024) return false;
  for (const unsigned char ch : alias) {
    if (ch == 0 || ch == '\n' || ch == '\r' || ch == '\t') return false;
  }
  return true;
}

bool FieldOperation(EngineGlobalAggregateOperation operation) {
  return operation ==
             EngineGlobalAggregateOperation::count_non_null_field ||
         operation ==
             EngineGlobalAggregateOperation::count_distinct_field ||
         operation == EngineGlobalAggregateOperation::avg_field ||
         operation == EngineGlobalAggregateOperation::avg_distinct_field;
}

bool CountOperation(EngineGlobalAggregateOperation operation) {
  return operation == EngineGlobalAggregateOperation::count_star ||
         operation == EngineGlobalAggregateOperation::count_non_null_field ||
         operation == EngineGlobalAggregateOperation::count_distinct_field;
}

bool AvgOperation(EngineGlobalAggregateOperation operation) {
  return operation == EngineGlobalAggregateOperation::avg_field ||
         operation == EngineGlobalAggregateOperation::avg_distinct_field;
}

bool DistinctOperation(EngineGlobalAggregateOperation operation) {
  return operation == EngineGlobalAggregateOperation::count_distinct_field ||
         operation == EngineGlobalAggregateOperation::avg_distinct_field;
}

bool DirectInputExpression(
    const EngineGlobalAggregateInputExpression& expression) {
  return expression.kind ==
             EngineGlobalAggregateInputExpressionKind::direct_field &&
         TypedValueIsEmpty(expression.int32_literal) &&
         DescriptorEmpty(expression.result_descriptor);
}

const MgaRelationColumnStorageDescriptor* FindColumnByUuid(
    const MgaRelationStorageDescriptor& descriptor,
    const EngineUuid& column_uuid,
    bool* duplicate) {
  const MgaRelationColumnStorageDescriptor* found = nullptr;
  if (duplicate != nullptr) *duplicate = false;
  for (const auto& column : descriptor.columns) {
    if (column.column_uuid != column_uuid) continue;
    if (found != nullptr) {
      if (duplicate != nullptr) *duplicate = true;
      return nullptr;
    }
    found = &column;
  }
  return found;
}

struct StoredFieldValueLookup {
  bool found = false;
  bool duplicate = false;
  EngineValueState state = EngineValueState::missing;
  std::string_view value;
};

StoredFieldValueLookup StoredFieldValueExact(
    const CrudRowVersionRecord& row,
    std::string_view field_name_key) {
  StoredFieldValueLookup lookup;
  for (const auto& [name, value] : row.values) {
    if (name != field_name_key) continue;
    if (lookup.found) {
      lookup.duplicate = true;
      continue;
    }
    lookup.found = true;
    lookup.state = value.state;
    lookup.value = value.bytes;
  }
  return lookup;
}

std::optional<dt::CanonicalTypeId> AdmittedIntegerDistinctType(
    const EngineDescriptor& descriptor) {
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      descriptor.canonical_type_name);
  if (type_id == dt::CanonicalTypeId::int32 ||
      type_id == dt::CanonicalTypeId::int64) {
    return type_id;
  }
  return std::nullopt;
}

enum class AvgInputKind : std::uint8_t {
  unsupported = 0,
  integer = 1,
  real64 = 2,
};

AvgInputKind AdmittedAvgInputKind(const EngineDescriptor& descriptor) {
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      descriptor.canonical_type_name);
  if (type_id == dt::CanonicalTypeId::int32 ||
      type_id == dt::CanonicalTypeId::int64) {
    return AvgInputKind::integer;
  }
  if (type_id == dt::CanonicalTypeId::real64) {
    return AvgInputKind::real64;
  }
  return AvgInputKind::unsupported;
}

bool CanonicalIntegerValue(const EngineDescriptor& descriptor,
                           std::string_view encoded,
                           std::int64_t* value,
                           std::string* canonical_text,
                           std::string* error_detail) {
  const auto type_id = AdmittedIntegerDistinctType(descriptor);
  if (!type_id) {
    if (error_detail != nullptr) {
      *error_detail = "global_aggregate_avg_type_unsupported";
    }
    return false;
  }
  const std::size_t width = *type_id == dt::CanonicalTypeId::int32 ? 4 : 8;
  if (encoded.size() != width) {
    if (error_detail != nullptr) {
      *error_detail = "global_aggregate_integer_value_invalid";
    }
    return false;
  }
  std::uint64_t bits = 0;
  for (std::size_t i = 0; i < width; ++i)
    bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(encoded[i])) << (8 * i);
  const std::int64_t parsed = width == 4
      ? static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits)))
      : std::bit_cast<std::int64_t>(bits);
  if (value != nullptr) *value = parsed;
  if (canonical_text != nullptr) {
    canonical_text->assign(encoded);
  }
  if (error_detail != nullptr) error_detail->clear();
  return true;
}

bool CanonicalInt32Literal(
    const EngineGlobalAggregateInputExpression& expression,
    std::int32_t* value,
    std::string* error_detail) {
  if (expression.kind != EngineGlobalAggregateInputExpressionKind::
                             int32_literal_times_int32_field_to_int64 ||
      !DescriptorExactlyMatches(
          expression.int32_literal.descriptor,
          EngineGlobalAggregateExpressionInt32LiteralDescriptor()) ||
      !DescriptorExactlyMatches(
          expression.result_descriptor,
          EngineGlobalAggregateExpressionInt64ResultDescriptor()) ||
      expression.int32_literal.isSqlNull() ||
      expression.int32_literal.state != EngineValueState::value) {
    if (error_detail != nullptr) {
      *error_detail = "global_aggregate_input_expression_descriptor_invalid";
    }
    return false;
  }

  std::int64_t parsed = 0;
  std::string canonical;
  if (!expression.int32_literal.encoded_value.empty() ||
      expression.int32_literal.binary_value.size() != 4 ||
      !CanonicalIntegerValue(expression.int32_literal.descriptor,
                             std::string_view(reinterpret_cast<const char*>(
                                 expression.int32_literal.binary_value.data()), 4),
                             &parsed,
                             &canonical,
                             error_detail) ||
      parsed < std::numeric_limits<std::int32_t>::min() ||
      parsed > std::numeric_limits<std::int32_t>::max()) {
    if (error_detail != nullptr) {
      *error_detail = "global_aggregate_expression_literal_int32_invalid";
    }
    return false;
  }
  if (value != nullptr) *value = static_cast<std::int32_t>(parsed);
  if (error_detail != nullptr) error_detail->clear();
  return true;
}

bool CanonicalIntegerDistinctKey(const EngineDescriptor& descriptor,
                                 std::string_view encoded,
                                 std::string* key,
                                 std::string* error_detail) {
  if (key == nullptr) return false;
  const auto type_id = AdmittedIntegerDistinctType(descriptor);
  if (!type_id) {
    if (error_detail != nullptr) {
      *error_detail = "global_aggregate_distinct_type_unsupported";
    }
    return false;
  }
  std::int64_t parsed = 0;
  std::string canonical;
  if (!CanonicalIntegerValue(
          descriptor, encoded, &parsed, &canonical, error_detail)) {
    if (error_detail != nullptr) {
      *error_detail = "global_aggregate_distinct_integer_value_invalid";
    }
    return false;
  }
  (void)parsed;
  *key = DistinctValueKey(descriptor.descriptor_uuid,
                          dt::CanonicalTypeName(*type_id), canonical);
  if (error_detail != nullptr) error_detail->clear();
  return true;
}

bool CanonicalReal64Value(const EngineDescriptor& descriptor,
                          std::string_view encoded,
                          double* value,
                          std::string* key,
                          std::string* error_detail) {
  if (AdmittedAvgInputKind(descriptor) != AvgInputKind::real64 ||
      encoded.size() != 8) {
    if (error_detail) *error_detail = "global_aggregate_real64_value_invalid";
    return false;
  }
  std::uint64_t bits = 0;
  for (unsigned i = 0; i < 8; ++i)
    bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(encoded[i])) << (8 * i);
  double parsed = std::bit_cast<double>(bits);
  if (!std::isfinite(parsed)) {
    if (error_detail) *error_detail = "global_aggregate_real64_value_invalid";
    return false;
  }
  if (value) *value = parsed;
  // SQL equality identifies both zero signs; all other finite binary64
  // representations are already canonical. No decimal formatting or parsing.
  if (key) {
    const std::string canonical = parsed == 0.0 ? std::string(8, '\0')
                                               : std::string(encoded);
    *key = DistinctValueKey(descriptor.descriptor_uuid, "real64", canonical);
  }
  if (error_detail) error_detail->clear();
  return true;
}

std::vector<std::uint8_t> NativeIntegerBytes(std::int64_t value) {
  std::vector<std::uint8_t> bytes(8);
  const auto bits = std::bit_cast<std::uint64_t>(value);
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = static_cast<std::uint8_t>(bits >> (8 * i));
  return bytes;
}
EngineTypedValue CountValue(std::uint64_t value) {
  EngineTypedValue typed;
  typed.descriptor = EngineGlobalAggregateCountResultDescriptor();
  typed.binary_value = NativeIntegerBytes(static_cast<std::int64_t>(value));
  typed.is_null = false;
  typed.state = EngineValueState::value;
  return typed;
}

EngineTypedValue NullAggregateValue(const EngineDescriptor& descriptor) {
  EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.is_null = true;
  typed.state = EngineValueState::sql_null;
  return typed;
}

EngineTypedValue AvgIntegerValue(std::int64_t value) {
  EngineTypedValue typed;
  typed.descriptor = EngineGlobalAggregateAvgIntegerResultDescriptor();
  typed.binary_value = NativeIntegerBytes(value);
  typed.is_null = false;
  typed.state = EngineValueState::value;
  return typed;
}

std::optional<EngineTypedValue> AvgReal64Value(double value) {
  if (!std::isfinite(value)) return std::nullopt;
  EngineTypedValue typed;
  typed.descriptor = EngineGlobalAggregateAvgRealResultDescriptor();
  typed.binary_value = NativeIntegerBytes(std::bit_cast<std::int64_t>(value));
  typed.is_null = false;
  typed.state = EngineValueState::value;
  return typed;
}

using I128 = __int128_t;
using U128 = __uint128_t;
constexpr U128 kU128Max = ~static_cast<U128>(0);
constexpr I128 kI128Max = static_cast<I128>(kU128Max >> 1u);
constexpr I128 kI128Min = -kI128Max - 1;

bool CheckedAddInteger(I128 left, std::int64_t right, I128* out) {
  if (out == nullptr) return false;
  const I128 widened = static_cast<I128>(right);
  if ((widened > 0 && left > kI128Max - widened) ||
      (widened < 0 && left < kI128Min - widened)) {
    return false;
  }
  *out = left + widened;
  return true;
}

bool CheckedMultiplyInt32ToInt64(std::int64_t field_value,
                                 std::int32_t literal,
                                 std::int64_t* out) {
  if (out == nullptr ||
      field_value < std::numeric_limits<std::int32_t>::min() ||
      field_value > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  const I128 product = static_cast<I128>(field_value) *
                       static_cast<I128>(literal);
  if (product < static_cast<I128>(std::numeric_limits<std::int64_t>::min()) ||
      product > static_cast<I128>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  *out = static_cast<std::int64_t>(product);
  return true;
}

struct AggregateState {
  std::uint64_t count = 0;
  std::unordered_set<std::string> canonical_distinct_keys;
  I128 integer_sum = 0;
  double real_sum = 0.0;
  std::uint64_t avg_value_count = 0;
};

}  // namespace

EngineDescriptor EngineGlobalAggregateCountResultDescriptor() {
  return scratchbird::engine::executor::MakeExecutorDescriptor(
      "int64", "nullability=non_null");
}

EngineUuid EngineGlobalAggregateCountFunctionUuid() {
  const auto* entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::count);
  return entry == nullptr ? EngineUuid{} : entry->function_uuid;
}

EngineDescriptor EngineGlobalAggregateAvgIntegerResultDescriptor() {
  return scratchbird::engine::executor::MakeExecutorDescriptor(
      "int64", "nullability=nullable");
}

EngineDescriptor EngineGlobalAggregateAvgRealResultDescriptor() {
  return scratchbird::engine::executor::MakeExecutorDescriptor(
      "real64", "nullability=nullable");
}

EngineDescriptor EngineGlobalAggregateExpressionInt32LiteralDescriptor() {
  return scratchbird::engine::executor::MakeExecutorDescriptor(
      "int32", "nullability=non_null");
}

EngineDescriptor EngineGlobalAggregateExpressionInt64ResultDescriptor() {
  return scratchbird::engine::executor::MakeExecutorDescriptor(
      "int64", "nullability=nullable");
}

EngineUuid EngineGlobalAggregateAvgFunctionUuid() {
  const auto* entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::avg);
  return entry == nullptr ? EngineUuid{} : entry->function_uuid;
}

EngineApiDiagnostic ValidateGlobalAggregateProjectionEnvelope(
    const EngineGlobalAggregateProjectionEnvelope& envelope) {
  if (envelope.relation_uuid.is_nil()) {
    return AggregateDiagnostic("global_aggregate_relation_uuid_required");
  }
  if (envelope.relation_descriptor_uuid.is_nil()) {
    return AggregateDiagnostic(
        "global_aggregate_relation_descriptor_uuid_required");
  }
  if (envelope.relation_descriptor_generation == 0) {
    return AggregateDiagnostic(
        "global_aggregate_relation_descriptor_generation_required");
  }
  if (envelope.outputs.empty()) {
    return AggregateDiagnostic("global_aggregate_output_required");
  }
  if (envelope.outputs.size() > kMaximumProjectionCount) {
    return AggregateDiagnostic("global_aggregate_output_count_exceeded");
  }

  const EngineUuid& envelope_function_uuid =
      envelope.outputs.front().aggregate_function_uuid;
  const EngineUuid count_function_uuid =
      EngineGlobalAggregateCountFunctionUuid();
  const EngineUuid avg_function_uuid =
      EngineGlobalAggregateAvgFunctionUuid();
  if (count_function_uuid.is_nil() || avg_function_uuid.is_nil()) {
    return AggregateDiagnostic("global_aggregate_registry_unavailable");
  }
  const bool count_envelope = envelope_function_uuid == count_function_uuid;
  const bool avg_envelope = envelope_function_uuid == avg_function_uuid;
  if (!count_envelope && !avg_envelope) {
    return AggregateDiagnostic("global_aggregate_function_uuid_invalid");
  }
  std::unordered_set<std::string> output_aliases;
  for (const auto& output : envelope.outputs) {
    if (output.aggregate_function_uuid != envelope_function_uuid) {
      return AggregateDiagnostic(
          "global_aggregate_function_uuid_mixed");
    }
    if (!CountOperation(output.operation) && !AvgOperation(output.operation)) {
      return AggregateDiagnostic("global_aggregate_operation_invalid");
    }
    if ((count_envelope && !CountOperation(output.operation)) ||
        (avg_envelope && !AvgOperation(output.operation))) {
      return AggregateDiagnostic("global_aggregate_operation_family_mixed");
    }
    if (!SafeOutputAlias(output.output_alias)) {
      return AggregateDiagnostic("global_aggregate_output_alias_invalid");
    }
    if (!output_aliases.emplace(output.output_alias).second) {
      return AggregateDiagnostic("global_aggregate_output_alias_ambiguous");
    }
    const bool checked_int32_multiply =
        output.input_expression.kind ==
        EngineGlobalAggregateInputExpressionKind::
            int32_literal_times_int32_field_to_int64;
    if (!checked_int32_multiply &&
        !DirectInputExpression(output.input_expression)) {
      return AggregateDiagnostic(
          "global_aggregate_input_expression_invalid");
    }
    if (checked_int32_multiply) {
      std::string expression_error;
      if (!avg_envelope ||
          output.operation != EngineGlobalAggregateOperation::avg_field ||
          AdmittedAvgInputKind(output.source_field.value_descriptor) !=
              AvgInputKind::integer ||
          dt::CanonicalTypeIdFromStableName(
              output.source_field.value_descriptor.canonical_type_name) !=
              dt::CanonicalTypeId::int32 ||
          !CanonicalInt32Literal(output.input_expression,
                                 nullptr,
                                 &expression_error)) {
        return AggregateDiagnostic(
            expression_error.empty()
                ? "global_aggregate_input_expression_invalid"
                : std::move(expression_error));
      }
    }
    if (output.operation == EngineGlobalAggregateOperation::count_star) {
      if (!output.source_field.column_uuid.is_nil() ||
          !DescriptorEmpty(output.source_field.value_descriptor)) {
        return AggregateDiagnostic(
            "global_aggregate_count_star_forbids_field_binding");
      }
      if (!DescriptorExactlyMatches(
              output.result_descriptor,
              EngineGlobalAggregateCountResultDescriptor())) {
        return AggregateDiagnostic(
            "global_aggregate_result_descriptor_invalid");
      }
      continue;
    }

    if (!FieldOperation(output.operation) ||
        output.source_field.column_uuid.is_nil()) {
      return AggregateDiagnostic(
          "global_aggregate_source_field_uuid_required");
    }
    if (output.source_field.value_descriptor.descriptor_uuid.is_nil() ||
        output.source_field.value_descriptor.descriptor_kind.empty() ||
        output.source_field.value_descriptor.canonical_type_name.empty() ||
        output.source_field.value_descriptor.encoded_descriptor.empty()) {
      return AggregateDiagnostic(
          "global_aggregate_source_field_descriptor_required");
    }
    EngineDescriptor expected_result;
    if (count_envelope) {
      expected_result = EngineGlobalAggregateCountResultDescriptor();
    } else {
      const AvgInputKind input_kind =
          AdmittedAvgInputKind(output.source_field.value_descriptor);
      if (input_kind == AvgInputKind::unsupported) {
        return AggregateDiagnostic("global_aggregate_avg_type_unsupported");
      }
      expected_result =
          input_kind == AvgInputKind::integer
              ? EngineGlobalAggregateAvgIntegerResultDescriptor()
              : EngineGlobalAggregateAvgRealResultDescriptor();
    }
    if (!DescriptorExactlyMatches(output.result_descriptor,
                                  expected_result)) {
      return AggregateDiagnostic("global_aggregate_result_descriptor_invalid");
    }
  }
  return OkDiagnostic();
}

EngineGlobalAggregateBindingResult BindGlobalAggregateProjectionEnvelope(
    const EngineGlobalAggregateProjectionEnvelope& envelope,
    const MgaRelationStorageDescriptor& relation_descriptor) {
  EngineGlobalAggregateBindingResult result;
  result.diagnostic = ValidateGlobalAggregateProjectionEnvelope(envelope);
  if (result.diagnostic.error) return result;

  const auto relation_validation =
      ValidateMgaRelationStorageDescriptor(relation_descriptor);
  if (relation_validation.error) {
    result.diagnostic = relation_validation;
    return result;
  }
  if (envelope.relation_uuid !=
      relation_descriptor.relation_uuid) {
    result.diagnostic =
        AggregateDiagnostic("global_aggregate_relation_uuid_mismatch");
    return result;
  }
  if (envelope.relation_descriptor_uuid !=
      relation_descriptor.descriptor_uuid) {
    result.diagnostic = AggregateDiagnostic(
        "global_aggregate_relation_descriptor_uuid_mismatch");
    return result;
  }
  if (envelope.relation_descriptor_generation !=
      relation_descriptor.descriptor_generation) {
    result.diagnostic = AggregateDiagnostic(
        "global_aggregate_relation_descriptor_generation_mismatch");
    return result;
  }

  result.outputs.reserve(envelope.outputs.size());
  for (const auto& output : envelope.outputs) {
    EngineBoundGlobalAggregateProjection bound;
    bound.operation = output.operation;
    bound.aggregate_function_uuid = output.aggregate_function_uuid;
    bound.output_alias = output.output_alias;
    bound.result_descriptor = output.result_descriptor;
    if (FieldOperation(output.operation)) {
      bool duplicate = false;
      const auto* column = FindColumnByUuid(
          relation_descriptor,
          output.source_field.column_uuid,
          &duplicate);
      if (duplicate) {
        result.outputs.clear();
        result.diagnostic = AggregateDiagnostic(
            "global_aggregate_source_field_uuid_ambiguous");
        return result;
      }
      if (column == nullptr) {
        result.outputs.clear();
        result.diagnostic = AggregateDiagnostic(
            "global_aggregate_source_field_not_found");
        return result;
      }
      if (!DescriptorExactlyMatches(
              output.source_field.value_descriptor,
              column->value_descriptor)) {
        result.outputs.clear();
        result.diagnostic = AggregateDiagnostic(
            "global_aggregate_source_field_descriptor_mismatch");
        return result;
      }
      bound.source_field = output.source_field;
    }
    bound.input_expression = output.input_expression;
    result.outputs.push_back(std::move(bound));
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineGlobalAggregateExecutionResult ExecuteGlobalAggregateProjection(
    const EngineRequestContext& context,
    const std::vector<EngineBoundGlobalAggregateProjection>& outputs,
    const MgaRelationStorageDescriptor& relation_descriptor,
    const std::vector<CrudRowVersionRecord>& visible_rows) {
  EngineGlobalAggregateExecutionResult result;
  if (outputs.empty()) {
    result.diagnostic =
        AggregateDiagnostic("bound_global_aggregate_output_required");
    return result;
  }

  const auto relation_validation =
      ValidateMgaRelationStorageDescriptor(relation_descriptor);
  if (relation_validation.error) {
    result.diagnostic = relation_validation;
    return result;
  }

  const EngineUuid& aggregate_function_uuid =
      outputs.front().aggregate_function_uuid;
  const EngineUuid count_function_uuid =
      EngineGlobalAggregateCountFunctionUuid();
  const EngineUuid avg_function_uuid =
      EngineGlobalAggregateAvgFunctionUuid();
  if (count_function_uuid.is_nil() || avg_function_uuid.is_nil()) {
    result.diagnostic =
        AggregateDiagnostic("bound_global_aggregate_registry_unavailable");
    return result;
  }
  const bool count_envelope = aggregate_function_uuid == count_function_uuid;
  const bool avg_envelope = aggregate_function_uuid == avg_function_uuid;
  if (!count_envelope && !avg_envelope) {
    result.diagnostic = AggregateDiagnostic(
        "bound_global_aggregate_function_uuid_invalid");
    return result;
  }

  std::vector<std::string> source_field_name_keys(outputs.size());
  std::vector<bool> source_nullable(outputs.size());
  std::vector<AvgInputKind> avg_input_kinds(
      outputs.size(), AvgInputKind::unsupported);
  std::vector<std::optional<std::int32_t>> expression_int32_literals(
      outputs.size());
  for (std::size_t index = 0; index < outputs.size(); ++index) {
    const auto& output = outputs[index];
    if (output.aggregate_function_uuid != aggregate_function_uuid) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_function_uuid_mixed");
      return result;
    }
    if (!CountOperation(output.operation) && !AvgOperation(output.operation)) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_operation_invalid");
      return result;
    }
    if ((count_envelope && !CountOperation(output.operation)) ||
        (avg_envelope && !AvgOperation(output.operation))) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_operation_family_mixed");
      return result;
    }
    const bool checked_int32_multiply =
        output.input_expression.kind ==
        EngineGlobalAggregateInputExpressionKind::
            int32_literal_times_int32_field_to_int64;
    if (!checked_int32_multiply &&
        !DirectInputExpression(output.input_expression)) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_input_expression_invalid");
      return result;
    }
    if (output.operation == EngineGlobalAggregateOperation::count_star) {
      if (!output.source_field.column_uuid.is_nil() ||
          !DescriptorEmpty(output.source_field.value_descriptor) ||
          !DescriptorExactlyMatches(
              output.result_descriptor,
              EngineGlobalAggregateCountResultDescriptor())) {
        result.diagnostic = AggregateDiagnostic(
            "bound_global_aggregate_count_star_field_identity_invalid");
        return result;
      }
      continue;
    }
    if (!FieldOperation(output.operation) ||
        output.source_field.column_uuid.is_nil()) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_source_field_uuid_required");
      return result;
    }
    bool duplicate = false;
    const auto* column = FindColumnByUuid(
        relation_descriptor,
        output.source_field.column_uuid,
        &duplicate);
    if (duplicate) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_source_field_uuid_ambiguous");
      return result;
    }
    if (column == nullptr) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_source_field_not_found");
      return result;
    }
    if (!DescriptorExactlyMatches(output.source_field.value_descriptor,
                                  column->value_descriptor)) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_source_field_descriptor_mismatch");
      return result;
    }
    if (column->canonical_name_key.empty()) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_source_field_name_key_required");
      return result;
    }
    const auto& source = column->value_descriptor;
    const auto binding = dt::LookupDatatypeTypeCodecIdentityV1(
        context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
        context.datatype_registry_generation, source.datatype_descriptor_uuid,
        source.datatype_descriptor_generation);
    const auto source_type = dt::CanonicalTypeIdFromStableName(source.canonical_type_name);
    if (!binding.ok || binding.row.type_uuid != source.type_uuid ||
        binding.row.canonical_binary_type_code != static_cast<std::uint32_t>(source_type) ||
        source.canonical_type_name != dt::CanonicalTypeName(source_type) ||
        ((source_type == dt::CanonicalTypeId::int32 ||
          source_type == dt::CanonicalTypeId::int64 ||
          source_type == dt::CanonicalTypeId::real64) &&
         binding.row.canonical_value_exact_bytes !=
             (source_type == dt::CanonicalTypeId::int32 ? 4 : 8))) {
      result.diagnostic = AggregateDiagnostic("global_aggregate_source_datatype_binding_invalid");
      return result;
    }
    source_nullable[index] = column->nullable;
    if (output.operation ==
            EngineGlobalAggregateOperation::count_distinct_field &&
        !AdmittedIntegerDistinctType(column->value_descriptor)) {
      result.diagnostic = AggregateDiagnostic(
          "global_aggregate_distinct_type_unsupported");
      return result;
    }
    if (count_envelope &&
        !DescriptorExactlyMatches(
            output.result_descriptor,
            EngineGlobalAggregateCountResultDescriptor())) {
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_result_descriptor_mismatch");
      return result;
    }
    if (avg_envelope) {
      avg_input_kinds[index] = AdmittedAvgInputKind(
          checked_int32_multiply
              ? output.input_expression.result_descriptor
              : column->value_descriptor);
      if (avg_input_kinds[index] == AvgInputKind::unsupported) {
        result.diagnostic = AggregateDiagnostic(
            "global_aggregate_avg_type_unsupported");
        return result;
      }
      if (checked_int32_multiply) {
        std::int32_t literal = 0;
        std::string expression_error;
        if (output.operation != EngineGlobalAggregateOperation::avg_field ||
            dt::CanonicalTypeIdFromStableName(
                column->value_descriptor.canonical_type_name) !=
                dt::CanonicalTypeId::int32 ||
            !CanonicalInt32Literal(output.input_expression,
                                   &literal,
                                   &expression_error)) {
          result.diagnostic = AggregateDiagnostic(
              expression_error.empty()
                  ? "bound_global_aggregate_input_expression_invalid"
                  : std::move(expression_error));
          return result;
        }
        expression_int32_literals[index] = literal;
      }
      const EngineDescriptor expected_result =
          avg_input_kinds[index] == AvgInputKind::integer
              ? EngineGlobalAggregateAvgIntegerResultDescriptor()
              : EngineGlobalAggregateAvgRealResultDescriptor();
      if (!DescriptorExactlyMatches(output.result_descriptor,
                                    expected_result)) {
        result.diagnostic = AggregateDiagnostic(
            "bound_global_aggregate_result_descriptor_mismatch");
        return result;
      }
    }
    source_field_name_keys[index] = column->canonical_name_key;
  }

  std::vector<AggregateState> states(outputs.size());
  for (const auto& row : visible_rows) {
    if (result.scanned_visible_row_count ==
        std::numeric_limits<std::uint64_t>::max()) {
      result.diagnostic = AggregateDiagnostic(
          "global_aggregate_visible_row_count_overflow");
      return result;
    }
    ++result.scanned_visible_row_count;

    for (std::size_t index = 0; index < outputs.size(); ++index) {
      const auto& output = outputs[index];
      auto& state = states[index];
      if (output.operation == EngineGlobalAggregateOperation::count_star) {
        if (state.count == std::numeric_limits<std::uint64_t>::max()) {
          result.diagnostic = AggregateDiagnostic(
              "global_aggregate_count_overflow");
          return result;
        }
        ++state.count;
        continue;
      }

      const auto value = StoredFieldValueExact(
          row, source_field_name_keys[index]);
      if (value.duplicate) {
        result.diagnostic = AggregateDiagnostic(
            "global_aggregate_source_field_duplicate_in_visible_row");
        return result;
      }
      if (!value.found) {
        result.diagnostic = AggregateDiagnostic(
            "global_aggregate_source_field_missing_from_visible_row");
        return result;
      }
      if (value.state == EngineValueState::sql_null && value.value.empty() &&
          source_nullable[index]) continue;
      if (value.state != EngineValueState::value) {
        result.diagnostic = AggregateDiagnostic("global_aggregate_source_value_state_invalid");
        return result;
      }

      if (output.operation ==
          EngineGlobalAggregateOperation::count_non_null_field) {
        if (state.count == std::numeric_limits<std::uint64_t>::max()) {
          result.diagnostic = AggregateDiagnostic(
              "global_aggregate_count_overflow");
          return result;
        }
        ++state.count;
      } else if (output.operation ==
                 EngineGlobalAggregateOperation::count_distinct_field) {
        std::string key;
        std::string error_detail;
        if (!CanonicalIntegerDistinctKey(
                output.source_field.value_descriptor,
                value.value,
                &key,
                &error_detail)) {
          result.diagnostic = AggregateDiagnostic(std::move(error_detail));
          return result;
        }
        state.canonical_distinct_keys.emplace(std::move(key));
      } else if (AvgOperation(output.operation)) {
        const bool needs_distinct = output.operation == EngineGlobalAggregateOperation::avg_distinct_field;
        std::string distinct_key;
        std::string error_detail;
        std::int64_t integer_value = 0;
        double real_value = 0.0;
        if (avg_input_kinds[index] == AvgInputKind::integer) {
          std::string canonical_text;
          if (!CanonicalIntegerValue(
                  output.source_field.value_descriptor,
                  value.value,
                  &integer_value,
                  needs_distinct ? &canonical_text : nullptr,
                  &error_detail)) {
            result.diagnostic = AggregateDiagnostic(std::move(error_detail));
            return result;
          }
          if (expression_int32_literals[index]) {
            std::int64_t product = 0;
            if (!CheckedMultiplyInt32ToInt64(
                    integer_value,
                    *expression_int32_literals[index],
                    &product)) {
              result.diagnostic = AggregateDiagnostic(
                  "global_aggregate_expression_int32_multiply_overflow");
              return result;
            }
            integer_value = product;
            if (needs_distinct) {
              const auto bytes = NativeIntegerBytes(integer_value);
              canonical_text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            }
          }
          if (needs_distinct)
            distinct_key = expression_int32_literals[index]
                ? DistinctValueKey({}, "expression:int64", canonical_text)
                : DistinctValueKey(output.source_field.value_descriptor.descriptor_uuid,
                    output.source_field.value_descriptor.canonical_type_name, canonical_text);
        } else if (avg_input_kinds[index] == AvgInputKind::real64) {
          if (!CanonicalReal64Value(
                  output.source_field.value_descriptor,
                  value.value,
                  &real_value,
                  needs_distinct ? &distinct_key : nullptr,
                  &error_detail)) {
            result.diagnostic = AggregateDiagnostic(std::move(error_detail));
            return result;
          }
        } else {
          result.diagnostic = AggregateDiagnostic(
              "global_aggregate_avg_type_unsupported");
          return result;
        }

        if (output.operation ==
                EngineGlobalAggregateOperation::avg_distinct_field &&
            !state.canonical_distinct_keys.emplace(distinct_key).second) {
          continue;
        }
        if (state.avg_value_count ==
            std::numeric_limits<std::uint64_t>::max()) {
          result.diagnostic = AggregateDiagnostic(
              "global_aggregate_avg_value_count_overflow");
          return result;
        }
        ++state.avg_value_count;
        if (avg_input_kinds[index] == AvgInputKind::integer) {
          I128 next = 0;
          if (!CheckedAddInteger(state.integer_sum, integer_value, &next)) {
            result.diagnostic = AggregateDiagnostic(
                "global_aggregate_avg_integer_accumulator_overflow");
            return result;
          }
          state.integer_sum = next;
        } else {
          // REAL64 AVG commits binary64 arithmetic at every accumulation
          // step. A wider accumulator would hide an intermediate overflow and
          // preserve low-order bits that this descriptor has already rounded.
          const double next = state.real_sum + real_value;
          if (!std::isfinite(next)) {
            result.diagnostic = AggregateDiagnostic(
                "global_aggregate_avg_real64_accumulator_nonfinite");
            return result;
          }
          state.real_sum = next;
        }
      } else {
        result.diagnostic = AggregateDiagnostic(
            "bound_global_aggregate_operation_invalid");
        return result;
      }
    }
  }

  result.result_shape.result_kind = "global_aggregate_projection_rowset";
  EngineRowValue row;
  for (std::size_t index = 0; index < outputs.size(); ++index) {
    const auto& output = outputs[index];
    auto& state = states[index];
    EngineTypedValue typed;
    if (CountOperation(output.operation)) {
      std::uint64_t value = state.count;
      if (output.operation ==
          EngineGlobalAggregateOperation::count_distinct_field) {
        value = static_cast<std::uint64_t>(
            state.canonical_distinct_keys.size());
      }
      if (value > static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
        result.result_shape = {};
        result.diagnostic = AggregateDiagnostic(
            "global_aggregate_int64_result_overflow");
        return result;
      }
      typed = CountValue(value);
    } else if (state.avg_value_count == 0) {
      typed = NullAggregateValue(output.result_descriptor);
    } else if (avg_input_kinds[index] == AvgInputKind::integer) {
      const I128 quotient =
          state.integer_sum / static_cast<I128>(state.avg_value_count);
      if (quotient < static_cast<I128>(
                         std::numeric_limits<std::int64_t>::min()) ||
          quotient > static_cast<I128>(
                         std::numeric_limits<std::int64_t>::max())) {
        result.result_shape = {};
        result.diagnostic = AggregateDiagnostic(
            "global_aggregate_avg_int64_result_overflow");
        return result;
      }
      typed = AvgIntegerValue(static_cast<std::int64_t>(quotient));
    } else if (avg_input_kinds[index] == AvgInputKind::real64) {
      const double averaged =
          state.real_sum / static_cast<double>(state.avg_value_count);
      auto encoded = AvgReal64Value(averaged);
      if (!std::isfinite(averaged) || !encoded) {
        result.result_shape = {};
        result.diagnostic = AggregateDiagnostic(
            "global_aggregate_avg_real64_result_nonfinite");
        return result;
      }
      typed = std::move(*encoded);
    } else {
      result.result_shape = {};
      result.diagnostic = AggregateDiagnostic(
          "bound_global_aggregate_operation_invalid");
      return result;
    }
    result.result_shape.columns.push_back(output.result_descriptor);
    row.fields.push_back({output.output_alias, std::move(typed)});
  }
  result.result_shape.rows.push_back(std::move(row));
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

}  // namespace scratchbird::engine::internal_api
