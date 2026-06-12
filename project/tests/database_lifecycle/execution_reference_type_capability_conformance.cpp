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
    const engine::Uuid& reference_profile_uuid,
    const engine::Uuid& reference_mapping_uuid) {
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

  if (kind == engine::ExecutionDomainKind::reference_compatibility) {
    descriptor.storage_codec = engine::ExecutionDomainStorageCodec::reference_native;
    descriptor.cast_policy =
        engine::ExecutionDomainCastPolicy::reference_compatibility;
    descriptor.reference_metadata.present = true;
    descriptor.reference_metadata.reference_profile_uuid = reference_profile_uuid;
    descriptor.reference_metadata.reference_mapping_uuid = reference_mapping_uuid;
    descriptor.reference_metadata.reference_family = "postgresql";
    descriptor.reference_metadata.reference_type_name = "citext";
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
  descriptor.cast_kind = engine::DomainCastRuleKind::reference_compatibility;
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
  descriptor.cost_class = engine::DomainCostClass::reference;
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::reference_native;
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
  descriptor.cost_class = engine::DomainCostClass::reference;
  descriptor.index_eligibility = engine::DomainIndexEligibility::equality;
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::reference_native;
  return descriptor;
}

engine::ReferenceTypeMappingDescriptor ValidMapping() {
  engine::ReferenceTypeMappingDescriptor descriptor;
  descriptor.mapping_uuid = Uuid(0x10);
  descriptor.reference_profile_uuid = Uuid(0x11);
  descriptor.mapping_epoch = 37;
  descriptor.reference_family = "postgresql";
  descriptor.reference_type_name = "citext";
  descriptor.representation_class =
      engine::ReferenceTypeRepresentationClass::domain_compatibility;
  descriptor.canonical_descriptor = TypeDescriptor(0x12, "postgresql.citext");
  descriptor.canonical_descriptor_uuid =
      descriptor.canonical_descriptor.descriptor_uuid;
  descriptor.domain_descriptor_present = true;
  descriptor.domain_uuid = Uuid(0x20);
  descriptor.domain_descriptor = ValidDomain(
      engine::ExecutionDomainKind::reference_compatibility,
      descriptor.domain_uuid, descriptor.reference_profile_uuid,
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
    const engine::ReferenceTypeMappingDescriptor& mapping) {
  engine::ExecutionTypeCapabilityDescriptor descriptor;
  descriptor.capability_uuid = Uuid(0x70);
  descriptor.mapping_uuid = mapping.mapping_uuid;
  descriptor.reference_profile_uuid = mapping.reference_profile_uuid;
  descriptor.canonical_descriptor_uuid = mapping.canonical_descriptor_uuid;
  descriptor.capability_epoch = 37;
  descriptor.stable_name = "edr037.postgresql.citext.capability";
  descriptor.reference_superiority_matrix_entry_present = true;
  return descriptor;
}

void RequireMappingStatus(
    const engine::ReferenceTypeMappingDescriptor& descriptor,
    engine::ReferenceTypeMappingDescriptorStatus expected,
    std::string_view message) {
  const auto result = engine::ValidateReferenceTypeMappingDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-037 reference mapping status mismatch");
}

void RequireMappingDescriptorStatus(
    const engine::ReferenceTypeMappingDescriptor& descriptor,
    engine::ReferenceTypeMappingDescriptorStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result = engine::ValidateReferenceTypeMappingDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-037 reference mapping status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "EDR-037 reference mapping nested descriptor status mismatch");
}

void RequireMappingDomainStatus(
    const engine::ReferenceTypeMappingDescriptor& descriptor,
    engine::ReferenceTypeMappingDescriptorStatus expected,
    engine::ExecutionDomainDescriptorStatus domain_status,
    std::string_view message) {
  const auto result = engine::ValidateReferenceTypeMappingDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-037 reference mapping status mismatch");
  Require(result.domain_status == domain_status,
          "EDR-037 reference mapping nested domain status mismatch");
}

void RequireCapabilityStatus(
    const engine::ReferenceTypeMappingDescriptor& mapping,
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
    const engine::ReferenceTypeMappingDescriptor& mapping,
    const engine::ExecutionTypeCapabilityDescriptor& descriptor,
    engine::ReferenceTypeMappingDescriptorStatus mapping_status,
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

void TestValidReferenceDomainCapability() {
  const auto mapping = ValidMapping();
  Require(engine::ValidateReferenceTypeMappingDescriptor(mapping).ok(),
          "EDR-037 rejected valid reference type mapping");

  const auto capability = ValidCapability(mapping);
  Require(engine::ValidateExecutionTypeCapabilityDescriptor(mapping,
                                                            capability)
              .ok(),
          "EDR-037 rejected valid reference type capability");
  Require(engine::ReferenceTypeMappingDescriptorStatusName(
              engine::ReferenceTypeMappingDescriptorStatus::ok) == "ok",
          "EDR-037 reference mapping status names are not stable");
  Require(engine::ExecutionTypeCapabilityDescriptorStatusName(
              engine::ExecutionTypeCapabilityDescriptorStatus::ok) == "ok",
          "EDR-037 type capability status names are not stable");
}

void TestRefusedAndDeferredMappingsAreExplicit() {
  auto mapping = ValidMapping();
  mapping.representation_class = engine::ReferenceTypeRepresentationClass::refused;
  mapping.decision_reason = "reference type requires unsupported storage hook";
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
  Require(engine::ValidateReferenceTypeMappingDescriptor(mapping).ok(),
          "EDR-037 rejected explicit refused reference mapping");

  auto capability = ValidCapability(mapping);
  capability.canonical_descriptor_uuid = {};
  capability.literal_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.bind_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.inference_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.overload_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.cast_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.operation_policy = engine::ExecutionTypeCapabilityState::refused;
  capability.reference_superiority_matrix_entry_present = false;
  Require(engine::ValidateExecutionTypeCapabilityDescriptor(mapping,
                                                            capability)
              .ok(),
          "EDR-037 rejected explicit refused reference capability");

  auto invalid_mapping = mapping;
  invalid_mapping.decision_reason.clear();
  RequireMappingStatus(
      invalid_mapping,
      engine::ReferenceTypeMappingDescriptorStatus::decision_reason_required,
      "EDR-037 accepted inactive reference mapping without decision reason");

  invalid_mapping = mapping;
  invalid_mapping.canonical_descriptor = TypeDescriptor(0x12, "citext");
  RequireMappingStatus(
      invalid_mapping,
      engine::ReferenceTypeMappingDescriptorStatus::
          inactive_mapping_has_execution_binding,
      "EDR-037 accepted refused reference mapping with execution binding");

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
      mapping, engine::ReferenceTypeMappingDescriptorStatus::mapping_uuid_required,
      "EDR-037 accepted reference mapping without mapping UUID");

  mapping = ValidMapping();
  mapping.canonical_descriptor.descriptor_epoch = 0;
  RequireMappingDescriptorStatus(
      mapping,
      engine::ReferenceTypeMappingDescriptorStatus::canonical_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
      "EDR-037 accepted invalid canonical descriptor");

  mapping = ValidMapping();
  mapping.domain_descriptor.reference_metadata.reference_type_name = "uuid";
  RequireMappingStatus(
      mapping,
      engine::ReferenceTypeMappingDescriptorStatus::
          domain_reference_metadata_mismatch,
      "EDR-037 accepted mismatched reference metadata");

  mapping = ValidMapping();
  mapping.domain_descriptor.domain_uuid = {};
  RequireMappingDomainStatus(
      mapping,
      engine::ReferenceTypeMappingDescriptorStatus::domain_descriptor_invalid,
      engine::ExecutionDomainDescriptorStatus::domain_uuid_required,
      "EDR-037 accepted invalid reference domain descriptor");

  mapping = ValidMapping();
  mapping.representation_class =
      engine::ReferenceTypeRepresentationClass::external_locator;
  mapping.external_locator_safe = false;
  RequireMappingStatus(
      mapping,
      engine::ReferenceTypeMappingDescriptorStatus::
          external_locator_safety_required,
      "EDR-037 accepted unsafe external locator mapping");

  mapping = ValidMapping();
  mapping.representation_class = engine::ReferenceTypeRepresentationClass::opaque_bridge;
  mapping.domain_descriptor = ValidDomain(engine::ExecutionDomainKind::opaque,
                                          mapping.domain_uuid,
                                          mapping.reference_profile_uuid,
                                          mapping.mapping_uuid);
  mapping.opaque_lifecycle_managed = false;
  RequireMappingStatus(
      mapping,
      engine::ReferenceTypeMappingDescriptorStatus::opaque_lifecycle_required,
      "EDR-037 accepted opaque reference mapping without lifecycle policy");
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
  mapping.reference_type_name.clear();
  capability = ValidCapability(ValidMapping());
  RequireCapabilityMappingStatus(
      mapping, capability,
      engine::ReferenceTypeMappingDescriptorStatus::reference_type_name_required,
      "EDR-037 did not preserve invalid reference mapping status");

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
      engine::ReferenceTypeRepresentationClass::external_locator;
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
  capability.reference_superiority_matrix_entry_present = false;
  RequireCapabilityStatus(
      mapping, capability,
      engine::ExecutionTypeCapabilityDescriptorStatus::
          reference_superiority_matrix_required,
      "EDR-037 accepted capability without reference superiority matrix evidence");
}

void TestOpaqueBridgeCapability() {
  auto mapping = ValidMapping();
  mapping.representation_class =
      engine::ReferenceTypeRepresentationClass::opaque_bridge;
  mapping.domain_descriptor =
      ValidDomain(engine::ExecutionDomainKind::opaque, mapping.domain_uuid,
                  mapping.reference_profile_uuid, mapping.mapping_uuid);
  mapping.opaque_lifecycle_managed = true;
  Require(engine::ValidateReferenceTypeMappingDescriptor(mapping).ok(),
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
  TestValidReferenceDomainCapability();
  TestRefusedAndDeferredMappingsAreExplicit();
  TestMappingValidationFailures();
  TestCapabilityValidationFailures();
  TestOpaqueBridgeCapability();
  std::cout << "execution_reference_type_capability_conformance=passed\n";
  return EXIT_SUCCESS;
}
