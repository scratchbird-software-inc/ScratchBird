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
  descriptor.descriptor_epoch = 37;
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

engine::ExecutionTypeDescriptor DomainTypeDescriptor(
    std::uint8_t seed,
    std::string_view name,
    const engine::Uuid& domain_uuid) {
  auto descriptor = TypeDescriptor(seed, name);
  descriptor.domain_uuid = domain_uuid;
  descriptor.domain_stack.push_back(domain_uuid);
  descriptor.modifier_flags |=
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_stack);
  return descriptor;
}

engine::ExecutionDomainDescriptor ValidDomain(
    engine::ExecutionDomainKind kind,
    const engine::Uuid& domain_uuid,
    const engine::Uuid& donor_profile_uuid,
    const engine::Uuid& donor_mapping_uuid) {
  engine::ExecutionDomainDescriptor descriptor;
  descriptor.domain_uuid = domain_uuid;
  descriptor.domain_descriptor_uuid = Uuid(0x21);
  descriptor.descriptor_epoch = 37;
  descriptor.domain_version = 1;
  descriptor.stable_name = "edr037.postgresql.citext.domain";
  descriptor.domain_kind = kind;
  descriptor.value_descriptor =
      DomainTypeDescriptor(0x22, "edr037.domain.value", domain_uuid);
  descriptor.storage_descriptor = TypeDescriptor(0x23, "text");
  descriptor.domain_stack = {domain_uuid};
  descriptor.security_policy_uuid = Uuid(0x24);
  descriptor.security_policy_epoch = 37;
  descriptor.storage_codec_uuid = Uuid(0x25);
  descriptor.storage_codec_epoch = 37;
  descriptor.cast_policy_uuid = Uuid(0x26);
  descriptor.cast_policy_epoch = 37;
  descriptor.operation_policy_uuid = Uuid(0x27);
  descriptor.operation_policy_epoch = 37;
  descriptor.element_addressing_policy_uuid = Uuid(0x28);
  descriptor.element_addressing_policy_epoch = 37;

  if (kind == engine::ExecutionDomainKind::donor_compatibility) {
    descriptor.storage_codec = engine::ExecutionDomainStorageCodec::donor_native;
    descriptor.cast_policy =
        engine::ExecutionDomainCastPolicy::donor_compatibility;
    descriptor.donor_metadata.present = true;
    descriptor.donor_metadata.donor_profile_uuid = donor_profile_uuid;
    descriptor.donor_metadata.donor_mapping_uuid = donor_mapping_uuid;
    descriptor.donor_metadata.donor_family = "postgresql";
    descriptor.donor_metadata.donor_type_name = "citext";
  }
  if (kind == engine::ExecutionDomainKind::opaque) {
    descriptor.storage_codec =
        engine::ExecutionDomainStorageCodec::external_locator;
    descriptor.element_addressing_policy =
        engine::ExecutionDomainElementAddressingPolicy::opaque_accessor;
  }
  return descriptor;
}

engine::DomainCastRuleDescriptor ValidCastRule(
    const engine::Uuid& source_domain_uuid,
    const engine::Uuid& target_domain_uuid) {
  engine::DomainCastRuleDescriptor descriptor;
  descriptor.cast_rule_uuid = Uuid(0x40);
  descriptor.cast_policy_uuid = Uuid(0x41);
  descriptor.descriptor_epoch = 37;
  descriptor.cast_policy_epoch = 37;
  descriptor.stable_name = "edr037.postgresql.citext.cast";
  descriptor.cast_kind = engine::DomainCastRuleKind::donor_compatibility;
  descriptor.source_domain_uuid = source_domain_uuid;
  descriptor.target_domain_uuid = target_domain_uuid;
  descriptor.source_descriptor =
      DomainTypeDescriptor(0x42, "edr037.cast.source", source_domain_uuid);
  descriptor.target_descriptor =
      DomainTypeDescriptor(0x43, "edr037.cast.target", target_domain_uuid);
  descriptor.source_descriptor_uuid =
      descriptor.source_descriptor.descriptor_uuid;
  descriptor.target_descriptor_uuid =
      descriptor.target_descriptor.descriptor_uuid;
  descriptor.cost_class = engine::DomainCostClass::donor;
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::donor_native;
  return descriptor;
}

engine::DomainOperationDescriptor ValidOperation(
    const engine::Uuid& domain_uuid) {
  engine::DomainOperationDescriptor descriptor;
  descriptor.operation_uuid = Uuid(0x50);
  descriptor.operation_policy_uuid = Uuid(0x51);
  descriptor.domain_uuid = domain_uuid;
  descriptor.descriptor_epoch = 37;
  descriptor.operation_policy_epoch = 37;
  descriptor.stable_name = "edr037.postgresql.citext.equals";
  descriptor.operation_kind = engine::DomainOperationKind::comparison;
  descriptor.min_arity = 1;
  descriptor.max_arity = 1;
  engine::DomainOperationOperandDescriptor operand;
  operand.domain_uuid = domain_uuid;
  operand.descriptor =
      DomainTypeDescriptor(0x52, "edr037.operation.operand", domain_uuid);
  operand.operand_descriptor_uuid = operand.descriptor.descriptor_uuid;
  descriptor.operands.push_back(operand);
  descriptor.result_domain_uuid = domain_uuid;
  descriptor.result_descriptor =
      DomainTypeDescriptor(0x53, "edr037.operation.result", domain_uuid);
  descriptor.result_descriptor_uuid =
      descriptor.result_descriptor.descriptor_uuid;
  descriptor.cost_class = engine::DomainCostClass::donor;
  descriptor.index_eligibility = engine::DomainIndexEligibility::equality;
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::donor_native;
  return descriptor;
}

engine::DonorTypeMappingDescriptor ValidMapping() {
  engine::DonorTypeMappingDescriptor descriptor;
  descriptor.mapping_uuid = Uuid(0x10);
  descriptor.donor_profile_uuid = Uuid(0x11);
  descriptor.mapping_epoch = 37;
  descriptor.donor_family = "postgresql";
  descriptor.donor_type_name = "citext";
  descriptor.representation_class =
      engine::DonorTypeRepresentationClass::domain_compatibility;
  descriptor.canonical_descriptor = TypeDescriptor(0x12, "postgresql.citext");
  descriptor.canonical_descriptor_uuid =
      descriptor.canonical_descriptor.descriptor_uuid;
  descriptor.domain_descriptor_present = true;
  descriptor.domain_uuid = Uuid(0x20);
  descriptor.domain_descriptor = ValidDomain(
      engine::ExecutionDomainKind::donor_compatibility,
      descriptor.domain_uuid, descriptor.donor_profile_uuid,
      descriptor.mapping_uuid);
  descriptor.cast_rule_present = true;
  descriptor.cast_rule_descriptor =
      ValidCastRule(descriptor.domain_uuid, Uuid(0x30));
  descriptor.cast_rule_uuid = descriptor.cast_rule_descriptor.cast_rule_uuid;
  descriptor.operation_descriptor_present = true;
  descriptor.operation_descriptor = ValidOperation(descriptor.domain_uuid);
  descriptor.operation_uuid = descriptor.operation_descriptor.operation_uuid;
  return descriptor;
}

engine::ExecutionTypeCapabilityDescriptor ValidCapability(
    const engine::DonorTypeMappingDescriptor& mapping) {
  engine::ExecutionTypeCapabilityDescriptor descriptor;
  descriptor.capability_uuid = Uuid(0x70);
  descriptor.mapping_uuid = mapping.mapping_uuid;
  descriptor.donor_profile_uuid = mapping.donor_profile_uuid;
  descriptor.canonical_descriptor_uuid = mapping.canonical_descriptor_uuid;
  descriptor.capability_epoch = 37;
  descriptor.stable_name = "edr037.postgresql.citext.capability";
  descriptor.donor_superiority_matrix_entry_present = true;
  return descriptor;
}

void RequireMappingStatus(
    const engine::DonorTypeMappingDescriptor& descriptor,
    engine::DonorTypeMappingDescriptorStatus expected,
    std::string_view message) {
  const auto result = engine::ValidateDonorTypeMappingDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-037 donor mapping status mismatch");
}

void RequireMappingDescriptorStatus(
    const engine::DonorTypeMappingDescriptor& descriptor,
    engine::DonorTypeMappingDescriptorStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result = engine::ValidateDonorTypeMappingDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-037 donor mapping status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "EDR-037 donor mapping nested descriptor status mismatch");
}

void RequireMappingDomainStatus(
    const engine::DonorTypeMappingDescriptor& descriptor,
    engine::DonorTypeMappingDescriptorStatus expected,
    engine::ExecutionDomainDescriptorStatus domain_status,
    std::string_view message) {
  const auto result = engine::ValidateDonorTypeMappingDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-037 donor mapping status mismatch");
  Require(result.domain_status == domain_status,
          "EDR-037 donor mapping nested domain status mismatch");
}

void RequireCapabilityStatus(
    const engine::DonorTypeMappingDescriptor& mapping,
    const engine::ExecutionTypeCapabilityDescriptor& descriptor,
    engine::ExecutionTypeCapabilityDescriptorStatus expected,
    std::string_view message) {
  const auto result =
      engine::ValidateExecutionTypeCapabilityDescriptor(mapping, descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-037 type capability status mismatch");
}

void RequireCapabilityMappingStatus(
    const engine::DonorTypeMappingDescriptor& mapping,
    const engine::ExecutionTypeCapabilityDescriptor& descriptor,
    engine::DonorTypeMappingDescriptorStatus mapping_status,
    std::string_view message) {
  const auto result =
      engine::ValidateExecutionTypeCapabilityDescriptor(mapping, descriptor);
  Require(!result.ok(), message);
  Require(result.status ==
              engine::ExecutionTypeCapabilityDescriptorStatus::
                  mapping_descriptor_invalid,
          "EDR-037 type capability did not preserve mapping failure");
  Require(result.mapping_status == mapping_status,
          "EDR-037 type capability nested mapping status mismatch");
}

void TestValidDonorDomainCapability() {
  const auto mapping = ValidMapping();
  Require(engine::ValidateDonorTypeMappingDescriptor(mapping).ok(),
          "EDR-037 rejected valid donor type mapping");

  const auto capability = ValidCapability(mapping);
  Require(engine::ValidateExecutionTypeCapabilityDescriptor(mapping,
                                                            capability)
              .ok(),
          "EDR-037 rejected valid donor type capability");
  Require(engine::DonorTypeMappingDescriptorStatusName(
              engine::DonorTypeMappingDescriptorStatus::ok) == "ok",
          "EDR-037 donor mapping status names are not stable");
  Require(engine::ExecutionTypeCapabilityDescriptorStatusName(
              engine::ExecutionTypeCapabilityDescriptorStatus::ok) == "ok",
          "EDR-037 type capability status names are not stable");
}

void TestRefusedAndDeferredMappingsAreExplicit() {
  auto mapping = ValidMapping();
  mapping.representation_class = engine::DonorTypeRepresentationClass::refused;
  mapping.decision_reason = "donor type requires unsupported storage hook";
  mapping.canonical_descriptor_uuid = {};
  mapping.canonical_descriptor = {};
  mapping.domain_descriptor_present = false;
  mapping.domain_uuid = {};
  mapping.domain_descriptor = {};
  mapping.cast_rule_present = false;
  mapping.cast_rule_uuid = {};
  mapping.cast_rule_descriptor = {};
  mapping.operation_descriptor_present = false;
  mapping.operation_uuid = {};
  mapping.operation_descriptor = {};
  Require(engine::ValidateDonorTypeMappingDescriptor(mapping).ok(),
          "EDR-037 rejected explicit refused donor mapping");

  auto capability = ValidCapability(mapping);
  capability.canonical_descriptor_uuid = {};
  capability.literal_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.bind_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.inference_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.overload_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.cast_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.operation_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.donor_superiority_matrix_entry_present = false;
  Require(engine::ValidateExecutionTypeCapabilityDescriptor(mapping,
                                                            capability)
              .ok(),
          "EDR-037 rejected explicit refused donor capability");

  auto invalid_mapping = mapping;
  invalid_mapping.decision_reason.clear();
  RequireMappingStatus(
      invalid_mapping,
      engine::DonorTypeMappingDescriptorStatus::decision_reason_required,
      "EDR-037 accepted inactive donor mapping without decision reason");

  invalid_mapping = mapping;
  invalid_mapping.canonical_descriptor = TypeDescriptor(0x12, "citext");
  RequireMappingStatus(
      invalid_mapping,
      engine::DonorTypeMappingDescriptorStatus::
          inactive_mapping_has_execution_binding,
      "EDR-037 accepted refused donor mapping with execution binding");

  auto invalid_capability = capability;
  invalid_capability.literal_policy =
      engine::ExecutionTypeCapabilityState::supported;
  RequireCapabilityStatus(
      mapping, invalid_capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          active_capability_requires_supported_mapping,
      "EDR-037 accepted active capability against refused mapping");
}

void TestMappingValidationFailures() {
  auto mapping = ValidMapping();
  mapping.mapping_uuid = {};
  RequireMappingStatus(
      mapping, engine::DonorTypeMappingDescriptorStatus::mapping_uuid_required,
      "EDR-037 accepted donor mapping without mapping UUID");

  mapping = ValidMapping();
  mapping.canonical_descriptor.descriptor_epoch = 0;
  RequireMappingDescriptorStatus(
      mapping,
      engine::DonorTypeMappingDescriptorStatus::canonical_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
      "EDR-037 accepted invalid canonical descriptor");

  mapping = ValidMapping();
  mapping.domain_descriptor.donor_metadata.donor_type_name = "uuid";
  RequireMappingStatus(
      mapping,
      engine::DonorTypeMappingDescriptorStatus::
          domain_donor_metadata_mismatch,
      "EDR-037 accepted mismatched donor metadata");

  mapping = ValidMapping();
  mapping.domain_descriptor.domain_uuid = {};
  RequireMappingDomainStatus(
      mapping,
      engine::DonorTypeMappingDescriptorStatus::domain_descriptor_invalid,
      engine::ExecutionDomainDescriptorStatus::domain_uuid_required,
      "EDR-037 accepted invalid donor domain descriptor");

  mapping = ValidMapping();
  mapping.representation_class =
      engine::DonorTypeRepresentationClass::external_locator;
  mapping.external_locator_safe = false;
  RequireMappingStatus(
      mapping,
      engine::DonorTypeMappingDescriptorStatus::
          external_locator_safety_required,
      "EDR-037 accepted unsafe external locator mapping");

  mapping = ValidMapping();
  mapping.representation_class = engine::DonorTypeRepresentationClass::opaque_bridge;
  mapping.domain_descriptor = ValidDomain(engine::ExecutionDomainKind::opaque,
                                          mapping.domain_uuid,
                                          mapping.donor_profile_uuid,
                                          mapping.mapping_uuid);
  mapping.opaque_lifecycle_managed = false;
  RequireMappingStatus(
      mapping,
      engine::DonorTypeMappingDescriptorStatus::opaque_lifecycle_required,
      "EDR-037 accepted opaque donor mapping without lifecycle policy");
}

void TestCapabilityValidationFailures() {
  auto mapping = ValidMapping();
  auto capability = ValidCapability(mapping);
  capability.mapping_uuid = Uuid(0x99);
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::mapping_uuid_mismatch,
      "EDR-037 accepted capability with wrong mapping UUID");

  mapping = ValidMapping();
  mapping.donor_type_name.clear();
  capability = ValidCapability(ValidMapping());
  RequireCapabilityMappingStatus(
      mapping, capability,
      engine::DonorTypeMappingDescriptorStatus::donor_type_name_required,
      "EDR-037 did not preserve invalid donor mapping status");

  mapping = ValidMapping();
  mapping.cast_rule_present = false;
  capability = ValidCapability(mapping);
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          cast_capability_requires_cast_rule,
      "EDR-037 accepted cast capability without cast rule");

  mapping = ValidMapping();
  mapping.operation_descriptor_present = false;
  capability = ValidCapability(mapping);
  capability.cast_policy = engine::ExecutionTypeCapabilityState::refused;
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          operation_capability_requires_operation_descriptor,
      "EDR-037 accepted operation capability without operation descriptor");

  mapping = ValidMapping();
  capability = ValidCapability(mapping);
  capability.literal_policy =
      engine::ExecutionTypeCapabilityState::cpp_udr_bridge;
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          cpp_udr_mapping_descriptor_uuid_required,
      "EDR-037 accepted C++ UDR capability without mapping descriptor UUID");

  capability.cxx_udr_mapping_descriptor_uuid = Uuid(0x80);
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          cpp_udr_mapping_descriptor_epoch_required,
      "EDR-037 accepted C++ UDR capability without mapping descriptor epoch");

  capability = ValidCapability(mapping);
  capability.literal_policy =
      engine::ExecutionTypeCapabilityState::llvm_accelerated;
  capability.cxx_udr_mapping_descriptor_uuid = Uuid(0x80);
  capability.cxx_udr_mapping_descriptor_epoch = 37;
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          llvm_acceleration_requires_cpp_udr_bridge,
      "EDR-037 accepted LLVM acceleration without C++ UDR bridge");

  capability.cxx_udr_bridge_available = true;
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          llvm_acceleration_descriptor_uuid_required,
      "EDR-037 accepted LLVM acceleration without descriptor UUID");

  mapping = ValidMapping();
  mapping.representation_class =
      engine::DonorTypeRepresentationClass::external_locator;
  mapping.external_locator_safe = true;
  capability = ValidCapability(mapping);
  capability.external_locator_allowed = true;
  capability.external_locator_policy_safe = false;
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          external_locator_policy_required,
      "EDR-037 accepted external locator capability without safe policy");

  mapping = ValidMapping();
  capability = ValidCapability(mapping);
  capability.donor_superiority_matrix_entry_present = false;
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          donor_superiority_matrix_required,
      "EDR-037 accepted capability without donor superiority matrix evidence");
}

void TestOpaqueBridgeCapability() {
  auto mapping = ValidMapping();
  mapping.representation_class =
      engine::DonorTypeRepresentationClass::opaque_bridge;
  mapping.domain_descriptor =
      ValidDomain(engine::ExecutionDomainKind::opaque, mapping.domain_uuid,
                  mapping.donor_profile_uuid, mapping.mapping_uuid);
  mapping.opaque_lifecycle_managed = true;
  Require(engine::ValidateDonorTypeMappingDescriptor(mapping).ok(),
          "EDR-037 rejected valid opaque bridge mapping");

  auto capability = ValidCapability(mapping);
  capability.opaque_lifecycle_managed = true;
  Require(engine::ValidateExecutionTypeCapabilityDescriptor(mapping,
                                                            capability)
              .ok(),
          "EDR-037 rejected valid opaque bridge capability");

  capability.opaque_lifecycle_managed = false;
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          opaque_lifecycle_required,
      "EDR-037 accepted opaque bridge capability without lifecycle evidence");
}

}  // namespace

int main() {
  TestValidDonorDomainCapability();
  TestRefusedAndDeferredMappingsAreExplicit();
  TestMappingValidationFailures();
  TestCapabilityValidationFailures();
  TestOpaqueBridgeCapability();
  std::cout << "execution_donor_type_capability_conformance=passed\n";
  return EXIT_SUCCESS;
}
