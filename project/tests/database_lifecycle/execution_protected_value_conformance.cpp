// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstddef>
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

engine::ExecutionTypeDescriptor Descriptor() {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(0x10);
  descriptor.descriptor_epoch = 13;
  descriptor.canonical_type_id = 0x10;
  descriptor.family = engine::ExecutionTypeFamily::binary;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = "protected-value-fixture";
  descriptor.security_policy_uuid = Uuid(0x40);
  descriptor.modifier_flags |=
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::security_policy_uuid);
  return descriptor;
}

engine::ExecutionProtectedValue ValidProtectedValue(
    engine::ExecutionProtectedValuePurpose purpose =
        engine::ExecutionProtectedValuePurpose::result_projection) {
  engine::ExecutionProtectedValue value;
  value.value_state = engine::ExecutionValueState::protected_value;
  value.descriptor = Descriptor();
  value.value_uuid = Uuid(0x30);
  value.security_policy_uuid = value.descriptor.security_policy_uuid;
  value.masking_policy_uuid = Uuid(0x50);
  value.redaction_policy_uuid = Uuid(0x60);
  value.protection_class = engine::ExecutionProtectionClass::redacted;
  value.purpose = purpose;
  value.redaction_token = "<redacted:protected_value>";
  value.cache_identity = "descriptor:protected-value-fixture:policy:v1";
  value.descriptor_preserved = true;
  value.raw_payload_present = false;
  value.redaction_applied = true;
  value.cache_identity_descriptor_bound = true;
  value.policy_authoritative = true;
  value.parser_independent = true;
  value.diagnostic_safe = true;
  return value;
}

void RequireOk(const engine::ExecutionProtectedValue& value,
               std::string_view message) {
  const auto result = engine::ValidateExecutionProtectedValue(value);
  Require(result.ok(), message);
}

void RequireStatus(
    const engine::ExecutionProtectedValue& value,
    engine::ExecutionProtectedValueValidationStatus expected,
    std::string_view message) {
  const auto result = engine::ValidateExecutionProtectedValue(value);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-010 protected value validation status mismatch");
}

void TestAcceptedProtectedValuePurposes() {
  RequireOk(ValidProtectedValue(
                engine::ExecutionProtectedValuePurpose::result_projection),
            "EDR-010 rejected redacted result projection");
  RequireOk(ValidProtectedValue(engine::ExecutionProtectedValuePurpose::diagnostic),
            "EDR-010 rejected diagnostic-safe protected value");
  RequireOk(ValidProtectedValue(engine::ExecutionProtectedValuePurpose::cache_key),
            "EDR-010 rejected descriptor-bound protected cache identity");

  auto storage = ValidProtectedValue(engine::ExecutionProtectedValuePurpose::storage);
  storage.raw_payload_present = true;
  storage.redaction_applied = false;
  storage.redaction_token.clear();
  RequireOk(storage, "EDR-010 rejected protected storage carrier");
}

void TestDescriptorAndPolicyFailures() {
  auto value = ValidProtectedValue();
  value.descriptor.descriptor_epoch = 0;
  const auto descriptor_result = engine::ValidateExecutionProtectedValue(value);
  Require(!descriptor_result.ok(), "EDR-010 accepted invalid descriptor");
  Require(descriptor_result.status ==
              engine::ExecutionProtectedValueValidationStatus::descriptor_invalid,
          "EDR-010 descriptor status mismatch");
  Require(descriptor_result.descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
          "EDR-010 descriptor diagnostic was not preserved");

  value = ValidProtectedValue();
  value.descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::security_policy_uuid);
  RequireStatus(
      value,
      engine::ExecutionProtectedValueValidationStatus::security_policy_uuid_missing,
      "EDR-010 accepted descriptor without security policy UUID flag");

  value = ValidProtectedValue();
  value.security_policy_uuid = Uuid(0x90);
  RequireStatus(
      value,
      engine::ExecutionProtectedValueValidationStatus::security_policy_uuid_missing,
      "EDR-010 accepted mismatched security policy UUID");

  value = ValidProtectedValue();
  value.policy_authoritative = false;
  RequireStatus(value,
                engine::ExecutionProtectedValueValidationStatus::
                    policy_not_authoritative,
                "EDR-010 accepted non-authoritative protection policy");

  value = ValidProtectedValue();
  value.parser_independent = false;
  RequireStatus(value,
                engine::ExecutionProtectedValueValidationStatus::parser_dependent,
                "EDR-010 accepted parser-dependent protected value");
}

void TestValueIdentityAndStateFailures() {
  auto value = ValidProtectedValue();
  value.value_uuid = {};
  RequireStatus(value,
                engine::ExecutionProtectedValueValidationStatus::value_uuid_required,
                "EDR-010 accepted protected value without value UUID");

  value = ValidProtectedValue();
  value.value_state = static_cast<engine::ExecutionValueState>(0xff);
  RequireStatus(value,
                engine::ExecutionProtectedValueValidationStatus::value_state_invalid,
                "EDR-010 accepted invalid value-state code");

  value = ValidProtectedValue();
  value.value_state = engine::ExecutionValueState::value;
  RequireStatus(
      value,
      engine::ExecutionProtectedValueValidationStatus::value_state_kind_mismatch,
      "EDR-010 accepted protected value without protected value-state");

  value = ValidProtectedValue();
  value.descriptor_preserved = false;
  RequireStatus(
      value,
      engine::ExecutionProtectedValueValidationStatus::descriptor_not_preserved,
      "EDR-010 accepted protected value without descriptor preservation");
}

void TestProjectionAndDiagnosticFailures() {
  auto value = ValidProtectedValue();
  value.raw_payload_present = true;
  RequireStatus(value,
                engine::ExecutionProtectedValueValidationStatus::raw_payload_leak,
                "EDR-010 accepted raw payload in projected protected value");

  value = ValidProtectedValue();
  value.redaction_applied = false;
  RequireStatus(value,
                engine::ExecutionProtectedValueValidationStatus::redaction_required,
                "EDR-010 accepted unredacted result projection");

  value = ValidProtectedValue();
  value.redaction_token.clear();
  RequireStatus(
      value,
      engine::ExecutionProtectedValueValidationStatus::redaction_token_required,
      "EDR-010 accepted redacted result without safe redaction token");

  value = ValidProtectedValue(engine::ExecutionProtectedValuePurpose::diagnostic);
  value.diagnostic_safe = false;
  RequireStatus(value,
                engine::ExecutionProtectedValueValidationStatus::diagnostic_not_safe,
                "EDR-010 accepted unsafe diagnostic protected value");
}

void TestCacheIdentityFailures() {
  auto value = ValidProtectedValue(engine::ExecutionProtectedValuePurpose::cache_key);
  value.cache_identity.clear();
  RequireStatus(
      value,
      engine::ExecutionProtectedValueValidationStatus::cache_identity_required,
      "EDR-010 accepted protected cache key without cache identity");

  value = ValidProtectedValue(engine::ExecutionProtectedValuePurpose::cache_key);
  value.cache_identity_descriptor_bound = false;
  RequireStatus(
      value,
      engine::ExecutionProtectedValueValidationStatus::
          cache_identity_not_descriptor_bound,
      "EDR-010 accepted protected cache identity not bound to descriptor");
}

}  // namespace

int main() {
  TestAcceptedProtectedValuePurposes();
  TestDescriptorAndPolicyFailures();
  TestValueIdentityAndStateFailures();
  TestProjectionAndDiagnosticFailures();
  TestCacheIdentityFailures();
  return EXIT_SUCCESS;
}
