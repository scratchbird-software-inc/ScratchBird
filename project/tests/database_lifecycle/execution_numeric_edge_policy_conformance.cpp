// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace engine = scratchbird::engine;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

engine::Uuid Uuid(std::uint8_t seed) {
  engine::Uuid uuid;
  for (std::size_t index = 0; index < 16; ++index) {
    uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return uuid;
}

engine::ExecutionTypeDescriptor NumericDescriptor(
    std::uint8_t seed,
    engine::ExecutionTypeFamily family = engine::ExecutionTypeFamily::decimal) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 34;
  descriptor.canonical_type_id = seed;
  descriptor.family = family;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = "edr034.numeric";
  descriptor.precision = 18;
  descriptor.scale = 4;
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::precision) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::scale);
  return descriptor;
}

engine::ExecutionNumericEdgePolicyDescriptor ValidPolicy() {
  auto descriptor = NumericDescriptor(0x20);
  engine::ExecutionNumericEdgePolicyDescriptor policy;
  policy.policy_uuid = Uuid(0x10);
  policy.type_descriptor_uuid = descriptor.descriptor_uuid;
  policy.descriptor_epoch = descriptor.descriptor_epoch;
  policy.stable_name = "edr034.numeric.edge.policy";
  policy.type_descriptor = descriptor;
  policy.rounding_mode = engine::ExecutionNumericRoundingMode::half_even;
  policy.overflow_policy = engine::ExecutionNumericOverflowPolicy::error;
  policy.nan_policy = engine::ExecutionNumericSpecialValuePolicy::reject;
  policy.infinity_policy = engine::ExecutionNumericSpecialValuePolicy::reject;
  policy.signed_zero_policy =
      engine::ExecutionNumericSignedZeroPolicy::canonicalize_positive;
  policy.precision = descriptor.precision;
  policy.scale = descriptor.scale;
  return policy;
}

void PrintMismatch(engine::ExecutionNumericEdgePolicyStatus expected,
                   engine::ExecutionNumericEdgePolicyStatus actual) {
  std::cerr << "expected="
            << engine::ExecutionNumericEdgePolicyStatusName(expected)
            << " actual=" << engine::ExecutionNumericEdgePolicyStatusName(actual)
            << '\n';
}

void RequireStatus(const engine::ExecutionNumericEdgePolicyDescriptor& policy,
                   engine::ExecutionNumericEdgePolicyStatus expected,
                   std::string_view message) {
  const auto result =
      engine::ValidateExecutionNumericEdgePolicyDescriptor(policy);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-034 numeric edge policy status mismatch");
  }
}

void RequireDescriptorStatus(
    const engine::ExecutionNumericEdgePolicyDescriptor& policy,
    engine::ExecutionNumericEdgePolicyStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result =
      engine::ValidateExecutionNumericEdgePolicyDescriptor(policy);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-034 numeric edge descriptor status mismatch");
  }
  Require(result.descriptor_status == descriptor_status,
          "EDR-034 numeric edge descriptor diagnostic mismatch");
}

void TestValidPolicies() {
  Require(engine::ValidateExecutionNumericEdgePolicyDescriptor(ValidPolicy()).ok(),
          "EDR-034 rejected valid exact numeric edge policy");

  auto policy = ValidPolicy();
  policy.rounding_mode = engine::ExecutionNumericRoundingMode::toward_zero;
  policy.overflow_policy = engine::ExecutionNumericOverflowPolicy::saturate;
  policy.signed_zero_policy = engine::ExecutionNumericSignedZeroPolicy::reject;
  Require(engine::ValidateExecutionNumericEdgePolicyDescriptor(policy).ok(),
          "EDR-034 rejected valid alternate native numeric edge policy");

  policy = ValidPolicy();
  policy.decimal_floating_context = true;
  policy.decimal_context_precision = 34;
  policy.nan_policy =
      engine::ExecutionNumericSpecialValuePolicy::allow_with_descriptor;
  policy.infinity_policy =
      engine::ExecutionNumericSpecialValuePolicy::allow_with_descriptor;
  policy.signed_zero_policy = engine::ExecutionNumericSignedZeroPolicy::preserve;
  Require(engine::ValidateExecutionNumericEdgePolicyDescriptor(policy).ok(),
          "EDR-034 rejected valid decimal floating special-value policy");

  policy = ValidPolicy();
  policy.decimal_floating_context = true;
  policy.decimal_context_precision = 34;
  policy.overflow_policy =
      engine::ExecutionNumericOverflowPolicy::donor_compatible;
  policy.nan_policy =
      engine::ExecutionNumericSpecialValuePolicy::donor_compatible;
  policy.donor_profile_name = "edr034.donor.numeric";
  policy.donor_difference_documented = true;
  Require(engine::ValidateExecutionNumericEdgePolicyDescriptor(policy).ok(),
          "EDR-034 rejected valid donor numeric edge policy");
}

void TestIdentityFailures() {
  auto policy = ValidPolicy();
  policy.policy_uuid = {};
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::policy_uuid_required,
                "EDR-034 accepted policy without UUID");

  policy = ValidPolicy();
  policy.type_descriptor_uuid = {};
  RequireStatus(
      policy,
      engine::ExecutionNumericEdgePolicyStatus::type_descriptor_uuid_required,
      "EDR-034 accepted policy without type descriptor UUID");

  policy = ValidPolicy();
  policy.descriptor_epoch = 0;
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::
                    descriptor_epoch_required,
                "EDR-034 accepted policy without descriptor epoch");

  policy = ValidPolicy();
  policy.stable_name.clear();
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::stable_name_required,
                "EDR-034 accepted policy without stable name");

  policy = ValidPolicy();
  policy.descriptor_authoritative = false;
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::
                    descriptor_not_authoritative,
                "EDR-034 accepted non-authoritative policy");

  policy = ValidPolicy();
  policy.parser_independent = false;
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::
                    descriptor_parser_dependent,
                "EDR-034 accepted parser-dependent policy");
}

void TestEnumAndDescriptorFailures() {
  auto policy = ValidPolicy();
  policy.rounding_mode = static_cast<engine::ExecutionNumericRoundingMode>(0xff);
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::rounding_mode_invalid,
                "EDR-034 accepted invalid rounding mode");

  policy = ValidPolicy();
  policy.overflow_policy =
      static_cast<engine::ExecutionNumericOverflowPolicy>(0xff);
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::overflow_policy_invalid,
                "EDR-034 accepted invalid overflow policy");

  policy = ValidPolicy();
  policy.nan_policy =
      static_cast<engine::ExecutionNumericSpecialValuePolicy>(0xff);
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::nan_policy_invalid,
                "EDR-034 accepted invalid NaN policy");

  policy = ValidPolicy();
  policy.infinity_policy =
      static_cast<engine::ExecutionNumericSpecialValuePolicy>(0xff);
  RequireStatus(
      policy, engine::ExecutionNumericEdgePolicyStatus::infinity_policy_invalid,
      "EDR-034 accepted invalid infinity policy");

  policy = ValidPolicy();
  policy.signed_zero_policy =
      static_cast<engine::ExecutionNumericSignedZeroPolicy>(0xff);
  RequireStatus(
      policy,
      engine::ExecutionNumericEdgePolicyStatus::signed_zero_policy_invalid,
      "EDR-034 accepted invalid signed-zero policy");

  policy = ValidPolicy();
  policy.type_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      policy, engine::ExecutionNumericEdgePolicyStatus::type_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-034 accepted invalid numeric type descriptor");

  policy = ValidPolicy();
  policy.type_descriptor_uuid = Uuid(0xe0);
  RequireStatus(
      policy,
      engine::ExecutionNumericEdgePolicyStatus::type_descriptor_uuid_mismatch,
      "EDR-034 accepted mismatched type descriptor UUID");

  policy = ValidPolicy();
  policy.type_descriptor.descriptor_epoch += 1;
  RequireStatus(
      policy,
      engine::ExecutionNumericEdgePolicyStatus::type_descriptor_epoch_mismatch,
      "EDR-034 accepted mismatched type descriptor epoch");
}

void TestNumericPolicyFailures() {
  auto policy = ValidPolicy();
  policy.type_descriptor.family = engine::ExecutionTypeFamily::character;
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::numeric_family_required,
                "EDR-034 accepted non-numeric edge policy");

  policy = ValidPolicy();
  policy.precision += 1;
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::precision_mismatch,
                "EDR-034 accepted precision mismatch");

  policy = ValidPolicy();
  policy.scale += 1;
  RequireStatus(policy, engine::ExecutionNumericEdgePolicyStatus::scale_mismatch,
                "EDR-034 accepted scale mismatch");

  policy = ValidPolicy();
  policy.type_descriptor.modifier_flags = 0;
  policy.precision = 4;
  policy.scale = 5;
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::scale_exceeds_precision,
                "EDR-034 accepted scale greater than precision");

  policy = ValidPolicy();
  policy.nan_policy =
      engine::ExecutionNumericSpecialValuePolicy::allow_with_descriptor;
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::decimal_context_required,
                "EDR-034 accepted NaN policy without decimal context");

  policy = ValidPolicy();
  policy.type_descriptor =
      NumericDescriptor(0x21, engine::ExecutionTypeFamily::unsigned_integer);
  policy.type_descriptor_uuid = policy.type_descriptor.descriptor_uuid;
  policy.signed_zero_policy = engine::ExecutionNumericSignedZeroPolicy::preserve;
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::signed_zero_not_supported,
                "EDR-034 accepted signed zero for unsigned integer");
}

void TestDonorFailures() {
  auto policy = ValidPolicy();
  policy.overflow_policy =
      engine::ExecutionNumericOverflowPolicy::donor_compatible;
  RequireStatus(policy,
                engine::ExecutionNumericEdgePolicyStatus::donor_profile_required,
                "EDR-034 accepted donor numeric policy without donor profile");

  policy = ValidPolicy();
  policy.overflow_policy =
      engine::ExecutionNumericOverflowPolicy::donor_compatible;
  policy.donor_profile_name = "edr034.donor.numeric";
  RequireStatus(
      policy, engine::ExecutionNumericEdgePolicyStatus::donor_difference_required,
      "EDR-034 accepted donor numeric policy without documented differences");
}

}  // namespace

int main() {
  TestValidPolicies();
  TestIdentityFailures();
  TestEnumAndDescriptorFailures();
  TestNumericPolicyFailures();
  TestDonorFailures();
  return EXIT_SUCCESS;
}
