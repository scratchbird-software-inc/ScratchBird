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

engine::ExecutionTypeDescriptor TypeDescriptor(std::uint8_t seed,
                                               std::string_view name) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 24;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  descriptor.length = 255;
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::length);
  return descriptor;
}

engine::ExecutionDomainConstraintDescriptor Constraint(std::uint8_t seed) {
  engine::ExecutionDomainConstraintDescriptor constraint;
  constraint.constraint_uuid = Uuid(seed);
  constraint.constraint_epoch = 24;
  constraint.constraint_kind = engine::ExecutionDomainConstraintKind::check;
  constraint.stable_name = "edr024.check.nonempty";
  constraint.canonical_envelope_present = true;
  return constraint;
}

engine::ExecutionDomainDescriptor ValidDomain(
    engine::ExecutionDomainKind kind = engine::ExecutionDomainKind::scalar_alias) {
  engine::ExecutionDomainDescriptor descriptor;
  descriptor.domain_uuid = Uuid(0x10);
  descriptor.domain_descriptor_uuid = Uuid(0x20);
  descriptor.descriptor_epoch = 24;
  descriptor.domain_version = 1;
  descriptor.stable_name = "edr024.domain";
  descriptor.domain_kind = kind;
  descriptor.value_descriptor = TypeDescriptor(0x30, "edr024.domain.value");
  descriptor.value_descriptor.domain_uuid = descriptor.domain_uuid;
  descriptor.value_descriptor.domain_stack.push_back(descriptor.domain_uuid);
  descriptor.value_descriptor.security_policy_uuid = Uuid(0x40);
  descriptor.value_descriptor.modifier_flags |=
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_stack) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::security_policy_uuid);
  descriptor.storage_descriptor = TypeDescriptor(0x50, "text");
  descriptor.domain_stack = {descriptor.domain_uuid};
  descriptor.security_policy_uuid = Uuid(0x60);
  descriptor.security_policy_epoch = 24;
  descriptor.storage_codec_uuid = Uuid(0x70);
  descriptor.storage_codec_epoch = 24;
  descriptor.cast_policy_uuid = Uuid(0x80);
  descriptor.cast_policy_epoch = 24;
  descriptor.operation_policy_uuid = Uuid(0x90);
  descriptor.operation_policy_epoch = 24;
  descriptor.element_addressing_policy_uuid = Uuid(0xa0);
  descriptor.element_addressing_policy_epoch = 24;

  if (kind == engine::ExecutionDomainKind::constrained_scalar) {
    descriptor.constraints.push_back(Constraint(0xb0));
  }
  if (kind == engine::ExecutionDomainKind::composite ||
      kind == engine::ExecutionDomainKind::container) {
    descriptor.element_addressing_policy =
        engine::ExecutionDomainElementAddressingPolicy::canonical_path;
  }
  if (kind == engine::ExecutionDomainKind::opaque) {
    descriptor.element_addressing_policy =
        engine::ExecutionDomainElementAddressingPolicy::opaque_accessor;
    descriptor.storage_codec = engine::ExecutionDomainStorageCodec::external_locator;
  }
  if (kind == engine::ExecutionDomainKind::reference_compatibility) {
    descriptor.storage_codec = engine::ExecutionDomainStorageCodec::reference_native;
    descriptor.cast_policy = engine::ExecutionDomainCastPolicy::reference_compatibility;
    descriptor.reference_metadata.present = true;
    descriptor.reference_metadata.reference_profile_uuid = Uuid(0xc0);
    descriptor.reference_metadata.reference_mapping_uuid = Uuid(0xd0);
    descriptor.reference_metadata.reference_family = "postgresql";
    descriptor.reference_metadata.reference_type_name = "citext";
  }
  return descriptor;
}

void RequireStatus(const engine::ExecutionDomainDescriptor& descriptor,
                   engine::ExecutionDomainDescriptorStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionDomainDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-024 domain descriptor validation status mismatch");
}

void RequireDescriptorStatus(
    const engine::ExecutionDomainDescriptor& descriptor,
    engine::ExecutionDomainDescriptorStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result = engine::ValidateExecutionDomainDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-024 domain descriptor validation status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "EDR-024 nested descriptor status mismatch");
}

void TestValidDomainProfiles() {
  const engine::ExecutionDomainKind valid_kinds[] = {
      engine::ExecutionDomainKind::scalar_alias,
      engine::ExecutionDomainKind::constrained_scalar,
      engine::ExecutionDomainKind::composite,
      engine::ExecutionDomainKind::container,
      engine::ExecutionDomainKind::opaque,
      engine::ExecutionDomainKind::reference_compatibility};
  for (const auto kind : valid_kinds) {
    Require(engine::ValidateExecutionDomainDescriptor(ValidDomain(kind)).ok(),
            "EDR-024 rejected valid domain descriptor profile");
  }

  auto descriptor = ValidDomain();
  descriptor.default_policy = engine::ExecutionDomainDefaultPolicy::literal;
  descriptor.default_value_state = engine::ExecutionValueState::value;
  descriptor.default_payload = {'o', 'k'};
  Require(engine::ValidateExecutionDomainDescriptor(descriptor).ok(),
          "EDR-024 rejected valid literal default");

  descriptor = ValidDomain();
  descriptor.default_policy = engine::ExecutionDomainDefaultPolicy::expression;
  descriptor.default_expression_uuid = Uuid(0xe0);
  Require(engine::ValidateExecutionDomainDescriptor(descriptor).ok(),
          "EDR-024 rejected valid expression default");

  descriptor = ValidDomain();
  descriptor.masking_policy = engine::ExecutionDomainMaskingPolicy::redacted;
  descriptor.masking_policy_uuid = Uuid(0xe1);
  descriptor.masking_policy_epoch = 24;
  Require(engine::ValidateExecutionDomainDescriptor(descriptor).ok(),
          "EDR-024 rejected valid masking policy evidence");
}

void TestIdentityAndAuthorityFailures() {
  auto descriptor = ValidDomain();
  descriptor.domain_uuid = {};
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::domain_uuid_required,
                "EDR-024 accepted domain without UUID");

  descriptor = ValidDomain();
  descriptor.domain_descriptor_uuid = {};
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::domain_descriptor_uuid_required,
      "EDR-024 accepted domain without descriptor UUID");

  descriptor = ValidDomain();
  descriptor.descriptor_epoch = 0;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::descriptor_epoch_required,
      "EDR-024 accepted domain without descriptor epoch");

  descriptor = ValidDomain();
  descriptor.domain_version = 0;
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::domain_version_required,
                "EDR-024 accepted domain without version");

  descriptor = ValidDomain();
  descriptor.stable_name.clear();
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::stable_name_required,
                "EDR-024 accepted domain without stable name");

  descriptor = ValidDomain();
  descriptor.descriptor_authoritative = false;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::descriptor_not_authoritative,
      "EDR-024 accepted non-authoritative domain descriptor");

  descriptor = ValidDomain();
  descriptor.parser_independent = false;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::descriptor_parser_dependent,
      "EDR-024 accepted parser-dependent domain descriptor");

  descriptor = ValidDomain();
  descriptor.domain_kind = static_cast<engine::ExecutionDomainKind>(0xff);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::domain_kind_invalid,
                "EDR-024 accepted invalid domain kind");
}

void TestDescriptorAndStackFailures() {
  auto descriptor = ValidDomain();
  descriptor.value_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::value_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-024 accepted invalid value descriptor");

  descriptor = ValidDomain();
  descriptor.value_descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    value_descriptor_domain_flag_required,
                "EDR-024 accepted value descriptor without domain flag");

  descriptor = ValidDomain();
  descriptor.value_descriptor.domain_uuid = Uuid(0x11);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    value_descriptor_domain_uuid_mismatch,
                "EDR-024 accepted mismatched value descriptor domain UUID");

  descriptor = ValidDomain();
  descriptor.storage_descriptor.descriptor_epoch = 0;
  RequireDescriptorStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::storage_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
      "EDR-024 accepted invalid storage descriptor");

  descriptor = ValidDomain();
  descriptor.domain_stack.clear();
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::domain_stack_required,
                "EDR-024 accepted empty domain stack");

  descriptor = ValidDomain();
  for (std::uint8_t seed = 1; seed <= 32; ++seed) {
    descriptor.domain_stack.push_back(Uuid(seed));
  }
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::domain_stack_too_deep,
                "EDR-024 accepted overly deep domain stack");

  descriptor = ValidDomain();
  descriptor.domain_stack.push_back({});
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    domain_stack_entry_required,
                "EDR-024 accepted nil domain stack entry");

  descriptor = ValidDomain();
  descriptor.domain_stack.front() = Uuid(0x12);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    domain_stack_current_uuid_mismatch,
                "EDR-024 accepted domain stack without current UUID first");

  descriptor = ValidDomain();
  descriptor.domain_stack.push_back(descriptor.domain_uuid);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::domain_stack_duplicate,
                "EDR-024 accepted duplicate domain stack UUID");

  descriptor = ValidDomain();
  descriptor.value_descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_stack);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    value_descriptor_domain_stack_flag_required,
                "EDR-024 accepted value descriptor without stack flag");

  descriptor = ValidDomain();
  descriptor.value_descriptor.domain_stack.push_back(Uuid(0x13));
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    value_descriptor_domain_stack_mismatch,
                "EDR-024 accepted mismatched value descriptor domain stack");

  descriptor = ValidDomain();
  descriptor.storage_descriptor.modifier_flags |=
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid);
  descriptor.storage_descriptor.domain_uuid = descriptor.domain_uuid;
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::storage_descriptor_cycle,
                "EDR-024 accepted storage descriptor cycle");
}

void TestDefaultFailures() {
  auto descriptor = ValidDomain();
  descriptor.default_policy =
      static_cast<engine::ExecutionDomainDefaultPolicy>(0xff);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::default_policy_invalid,
                "EDR-024 accepted invalid default policy");

  descriptor = ValidDomain();
  descriptor.default_policy = engine::ExecutionDomainDefaultPolicy::literal;
  descriptor.default_value_state = static_cast<engine::ExecutionValueState>(0xff);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    default_literal_state_invalid,
                "EDR-024 accepted invalid literal default state");

  descriptor = ValidDomain();
  descriptor.default_policy = engine::ExecutionDomainDefaultPolicy::literal;
  descriptor.default_value_state = engine::ExecutionValueState::value;
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    default_literal_payload_required,
                "EDR-024 accepted payload-bearing default without payload");

  descriptor = ValidDomain();
  descriptor.default_payload = {'x'};
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    default_payload_not_allowed,
                "EDR-024 accepted payload with no default policy");

  descriptor = ValidDomain();
  descriptor.default_policy = engine::ExecutionDomainDefaultPolicy::expression;
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    default_expression_required,
                "EDR-024 accepted expression default without expression UUID");

  descriptor = ValidDomain();
  descriptor.default_expression_uuid = Uuid(0xe2);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    default_expression_unexpected,
                "EDR-024 accepted expression UUID without expression policy");

  descriptor = ValidDomain();
  descriptor.default_policy = engine::ExecutionDomainDefaultPolicy::literal;
  descriptor.default_value_state = engine::ExecutionValueState::sql_null;
  descriptor.value_descriptor.nullable_allowed = false;
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    default_nullability_violation,
                "EDR-024 accepted null default for non-null domain");
}

void TestConstraintFailures() {
  auto descriptor =
      ValidDomain(engine::ExecutionDomainKind::constrained_scalar);
  descriptor.constraints.clear();
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::constraint_required,
                "EDR-024 accepted constrained domain without constraint");

  descriptor = ValidDomain();
  descriptor.constraints.assign(engine::kExecutionDomainConstraintMaxCount + 1,
                                Constraint(0xb0));
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::
                    constraint_count_exceeds_limit,
                "EDR-024 accepted too many constraints");

  descriptor = ValidDomain();
  descriptor.constraints.push_back(Constraint(0xb1));
  descriptor.constraints.front().constraint_uuid = {};
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::constraint_uuid_required,
                "EDR-024 accepted constraint without UUID");

  descriptor = ValidDomain();
  descriptor.constraints.push_back(Constraint(0xb2));
  descriptor.constraints.front().constraint_epoch = 0;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::constraint_epoch_required,
      "EDR-024 accepted constraint without epoch");

  descriptor = ValidDomain();
  descriptor.constraints.push_back(Constraint(0xb3));
  descriptor.constraints.front().constraint_kind =
      static_cast<engine::ExecutionDomainConstraintKind>(0xff);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::constraint_kind_invalid,
                "EDR-024 accepted invalid constraint kind");

  descriptor = ValidDomain();
  descriptor.constraints.push_back(Constraint(0xb4));
  descriptor.constraints.front().stable_name.clear();
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::constraint_stable_name_required,
      "EDR-024 accepted constraint without stable name");

  descriptor = ValidDomain();
  descriptor.constraints.push_back(Constraint(0xb5));
  descriptor.constraints.front().canonical_envelope_present = false;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::constraint_envelope_required,
      "EDR-024 accepted enforced constraint without canonical envelope");

  descriptor = ValidDomain();
  descriptor.constraints.push_back(Constraint(0xb6));
  descriptor.constraints.front().parser_independent = false;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::constraint_parser_dependent,
      "EDR-024 accepted parser-dependent constraint");
}

void TestPolicyFailures() {
  auto descriptor = ValidDomain();
  descriptor.security_policy =
      static_cast<engine::ExecutionDomainSecurityPolicy>(0xff);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::security_policy_invalid,
                "EDR-024 accepted invalid security policy");

  descriptor = ValidDomain();
  descriptor.security_policy_uuid = {};
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::security_policy_uuid_required,
      "EDR-024 accepted missing security policy UUID");

  descriptor = ValidDomain();
  descriptor.security_policy_epoch = 0;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::security_policy_epoch_required,
      "EDR-024 accepted missing security policy epoch");

  descriptor = ValidDomain();
  descriptor.masking_policy =
      static_cast<engine::ExecutionDomainMaskingPolicy>(0xff);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::masking_policy_invalid,
                "EDR-024 accepted invalid masking policy");

  descriptor = ValidDomain();
  descriptor.masking_policy = engine::ExecutionDomainMaskingPolicy::fixed_text;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::masking_policy_uuid_required,
      "EDR-024 accepted missing masking policy UUID");

  descriptor = ValidDomain();
  descriptor.masking_policy = engine::ExecutionDomainMaskingPolicy::fixed_text;
  descriptor.masking_policy_uuid = Uuid(0xe3);
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::masking_policy_epoch_required,
      "EDR-024 accepted missing masking policy epoch");

  descriptor = ValidDomain();
  descriptor.storage_codec =
      static_cast<engine::ExecutionDomainStorageCodec>(0xff);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::storage_codec_invalid,
                "EDR-024 accepted invalid storage codec");

  descriptor = ValidDomain();
  descriptor.storage_codec_uuid = {};
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::storage_codec_uuid_required,
      "EDR-024 accepted missing storage codec UUID");

  descriptor = ValidDomain();
  descriptor.storage_codec_epoch = 0;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::storage_codec_epoch_required,
      "EDR-024 accepted missing storage codec epoch");

  descriptor = ValidDomain();
  descriptor.cast_policy = static_cast<engine::ExecutionDomainCastPolicy>(0xff);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::cast_policy_invalid,
                "EDR-024 accepted invalid cast policy");

  descriptor = ValidDomain();
  descriptor.cast_policy_uuid = {};
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::cast_policy_uuid_required,
                "EDR-024 accepted missing cast policy UUID");

  descriptor = ValidDomain();
  descriptor.cast_policy_epoch = 0;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::cast_policy_epoch_required,
      "EDR-024 accepted missing cast policy epoch");

  descriptor = ValidDomain();
  descriptor.operation_policy =
      static_cast<engine::ExecutionDomainOperationPolicy>(0xff);
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::operation_policy_invalid,
                "EDR-024 accepted invalid operation policy");

  descriptor = ValidDomain();
  descriptor.operation_policy_uuid = {};
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::operation_policy_uuid_required,
      "EDR-024 accepted missing operation policy UUID");

  descriptor = ValidDomain();
  descriptor.operation_policy_epoch = 0;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::operation_policy_epoch_required,
      "EDR-024 accepted missing operation policy epoch");

  descriptor = ValidDomain();
  descriptor.element_addressing_policy =
      static_cast<engine::ExecutionDomainElementAddressingPolicy>(0xff);
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::
          element_addressing_policy_invalid,
      "EDR-024 accepted invalid element addressing policy");

  descriptor = ValidDomain();
  descriptor.element_addressing_policy_uuid = {};
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::
          element_addressing_policy_uuid_required,
      "EDR-024 accepted missing element addressing policy UUID");

  descriptor = ValidDomain();
  descriptor.element_addressing_policy_epoch = 0;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::
          element_addressing_policy_epoch_required,
      "EDR-024 accepted missing element addressing policy epoch");

  descriptor = ValidDomain(engine::ExecutionDomainKind::composite);
  descriptor.element_addressing_policy =
      engine::ExecutionDomainElementAddressingPolicy::scalar_value;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::
          compound_element_addressing_required,
      "EDR-024 accepted compound domain without compound element addressing");

  descriptor = ValidDomain(engine::ExecutionDomainKind::opaque);
  descriptor.element_addressing_policy =
      engine::ExecutionDomainElementAddressingPolicy::scalar_value;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::opaque_element_addressing_required,
      "EDR-024 accepted opaque domain without opaque accessor policy");
}

void TestReferenceMetadataFailures() {
  auto descriptor =
      ValidDomain(engine::ExecutionDomainKind::reference_compatibility);
  descriptor.reference_metadata.present = false;
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::reference_metadata_required,
                "EDR-024 accepted reference domain without reference metadata");

  descriptor = ValidDomain(engine::ExecutionDomainKind::reference_compatibility);
  descriptor.reference_metadata.reference_profile_uuid = {};
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::reference_profile_uuid_required,
      "EDR-024 accepted reference metadata without profile UUID");

  descriptor = ValidDomain(engine::ExecutionDomainKind::reference_compatibility);
  descriptor.reference_metadata.reference_mapping_uuid = {};
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::reference_mapping_uuid_required,
      "EDR-024 accepted reference metadata without mapping UUID");

  descriptor = ValidDomain(engine::ExecutionDomainKind::reference_compatibility);
  descriptor.reference_metadata.reference_family.clear();
  RequireStatus(descriptor,
                engine::ExecutionDomainDescriptorStatus::reference_family_required,
                "EDR-024 accepted reference metadata without family");

  descriptor = ValidDomain(engine::ExecutionDomainKind::reference_compatibility);
  descriptor.reference_metadata.reference_type_name.clear();
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::reference_type_name_required,
      "EDR-024 accepted reference metadata without type name");

  descriptor = ValidDomain(engine::ExecutionDomainKind::reference_compatibility);
  descriptor.reference_metadata.descriptor_authoritative = false;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::
          reference_metadata_not_authoritative,
      "EDR-024 accepted non-authoritative reference metadata");

  descriptor = ValidDomain(engine::ExecutionDomainKind::reference_compatibility);
  descriptor.reference_metadata.parser_independent = false;
  RequireStatus(
      descriptor,
      engine::ExecutionDomainDescriptorStatus::
          reference_metadata_parser_dependent,
      "EDR-024 accepted parser-dependent reference metadata");
}

}  // namespace

int main() {
  TestValidDomainProfiles();
  TestIdentityAndAuthorityFailures();
  TestDescriptorAndStackFailures();
  TestDefaultFailures();
  TestConstraintFailures();
  TestPolicyFailures();
  TestReferenceMetadataFailures();
  return EXIT_SUCCESS;
}
