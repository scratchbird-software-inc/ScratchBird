// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "datatype_operations.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

namespace scratchbird::engine::internal_api {
bool QowApplyCanonicalNumericScalarV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    const EngineDescriptor& result_descriptor,
    scratchbird::core::datatypes::DatatypeNumericOperationKind operation,
    const scratchbird::core::datatypes::DatatypeNumericContext& context,
    EngineTypedValue* output_value,
    std::string* refusal_detail);
}  // namespace scratchbird::engine::internal_api

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

api::EngineDescriptor DecimalDescriptor(const std::string& descriptor_uuid,
                                        const std::uint32_t precision,
                                        const std::uint32_t scale) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000871;"
      "nullability=nullable;precision=" +
      std::to_string(precision) + ";scale=" + std::to_string(scale);
  return descriptor;
}

api::EngineTypedValue DecimalValue(const std::string& descriptor_uuid,
                                   const std::string& value) {
  api::EngineTypedValue typed;
  typed.descriptor = DecimalDescriptor(descriptor_uuid, 6, 2);
  typed.encoded_value = value;
  typed.state = api::EngineValueState::value;
  return typed;
}

dt::DatatypeNumericContext NumericContext(const std::uint32_t precision) {
  dt::DatatypeNumericContext context;
  context.precision = precision;
  context.scale = 2;
  context.rounding = dt::DatatypeRoundingMode::half_even;
  context.allow_special_values = false;
  return context;
}

api::EngineDescriptor Int128Descriptor(const std::string& descriptor_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int128";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000881;"
      "nullability=non_null;width=128";
  return descriptor;
}

api::EngineTypedValue Int128Value(const std::string& descriptor_uuid,
                                  const std::string& value) {
  api::EngineTypedValue typed;
  typed.descriptor = Int128Descriptor(descriptor_uuid);
  typed.encoded_value = value;
  typed.state = api::EngineValueState::value;
  return typed;
}

dt::DatatypeNumericContext Fixed128Context() {
  dt::DatatypeNumericContext context;
  context.precision = 38;
  context.scale = 0;
  context.rounding = dt::DatatypeRoundingMode::half_even;
  context.allow_special_values = false;
  return context;
}

bool ValidateAcceptedBoundary() {
  const auto left = DecimalValue(
      "019f0000-0000-7200-8000-000000000872", "999.99");
  const auto right = DecimalValue(
      "019f0000-0000-7200-8000-000000000873", "0.01");
  const auto result_descriptor = DecimalDescriptor(
      "019f0000-0000-7200-8000-000000000874", 6, 2);
  api::EngineTypedValue output;
  std::string refusal;
  const bool accepted = api::QowApplyCanonicalNumericScalarV1(
      left, right, result_descriptor, dt::DatatypeNumericOperationKind::add,
      NumericContext(6), &output, &refusal);
  bool passed = true;
  passed &= Require(accepted && refusal.empty(),
                    "in-range canonical decimal addition was refused");
  passed &= Require(
      output.encoded_value == "1000.00" &&
          output.descriptor.descriptor_uuid.canonical ==
              result_descriptor.descriptor_uuid.canonical &&
          output.state == api::EngineValueState::value,
      "in-range numeric result lost value or descriptor identity");
  return passed;
}

bool ValidateOverflowRefusal() {
  const auto left = DecimalValue(
      "019f0000-0000-7200-8000-000000000875", "999.99");
  const auto right = DecimalValue(
      "019f0000-0000-7200-8000-000000000876", "0.01");
  const auto result_descriptor = DecimalDescriptor(
      "019f0000-0000-7200-8000-000000000877", 5, 2);
  api::EngineTypedValue output;
  output.encoded_value = "substitute";
  std::string refusal;
  const bool accepted = api::QowApplyCanonicalNumericScalarV1(
      left, right, result_descriptor, dt::DatatypeNumericOperationKind::add,
      NumericContext(5), &output, &refusal);
  bool passed = true;
  passed &= Require(!accepted && !refusal.empty(),
                    "out-of-range decimal result was accepted");
  passed &= Require(output.state == api::EngineValueState::error &&
                        output.encoded_value.empty() &&
                        output.binary_value.empty(),
                    "overflow produced a wrapped or substitute payload");
  return passed;
}

bool ValidateContextAndNull() {
  const auto left = DecimalValue(
      "019f0000-0000-7200-8000-000000000878", "1.00");
  const auto right = DecimalValue(
      "019f0000-0000-7200-8000-000000000879", "2.00");
  const auto result_descriptor = DecimalDescriptor(
      "019f0000-0000-7200-8000-000000000880", 6, 2);
  api::EngineTypedValue output;
  std::string refusal;
  const bool mismatched_context = api::QowApplyCanonicalNumericScalarV1(
      left, right, result_descriptor, dt::DatatypeNumericOperationKind::add,
      NumericContext(5), &output, &refusal);

  auto null_left = left;
  null_left.state = api::EngineValueState::sql_null;
  null_left.is_null = false;
  null_left.encoded_value.clear();
  refusal.clear();
  const bool null_accepted = api::QowApplyCanonicalNumericScalarV1(
      null_left, right, result_descriptor,
      dt::DatatypeNumericOperationKind::add, NumericContext(6), &output,
      &refusal);

  bool passed = true;
  passed &= Require(!mismatched_context,
                    "precision context differing from descriptor was accepted");
  passed &= Require(null_accepted && output.isSqlNull() &&
                        output.encoded_value.empty() &&
                        output.descriptor.descriptor_uuid.canonical ==
                            result_descriptor.descriptor_uuid.canonical,
                    "canonical numeric SQL NULL was not propagated");
  return passed;
}

bool ValidateIntegerOverflowRefusal() {
  const auto left = Int128Value(
      "019f0000-0000-7200-8000-000000000882",
      "170141183460469231731687303715884105727");
  const auto right = Int128Value(
      "019f0000-0000-7200-8000-000000000883", "1");
  const auto result_descriptor = Int128Descriptor(
      "019f0000-0000-7200-8000-000000000884");
  api::EngineTypedValue output;
  std::string refusal;
  const bool accepted = api::QowApplyCanonicalNumericScalarV1(
      left, right, result_descriptor, dt::DatatypeNumericOperationKind::add,
      Fixed128Context(), &output, &refusal);
  bool passed = true;
  passed &= Require(!accepted && !refusal.empty(),
                    "int128 overflow was accepted");
  passed &= Require(output.state == api::EngineValueState::error &&
                        output.encoded_value.empty(),
                    "int128 overflow wrapped or produced a substitute");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-008-OVERFLOW-V1
int main() {
  bool passed = true;
  passed &= ValidateAcceptedBoundary();
  passed &= ValidateOverflowRefusal();
  passed &= ValidateContextAndNull();
  passed &= ValidateIntegerOverflowRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
