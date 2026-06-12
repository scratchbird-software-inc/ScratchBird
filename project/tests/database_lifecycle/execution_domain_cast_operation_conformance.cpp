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
  descriptor.descriptor_epoch = 26;
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

engine::ExecutionTypeDescriptor DomainTypeDescriptor(std::uint8_t seed,
                                                     std::string_view name,
                                                     engine::Uuid domain_uuid) {
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

engine::DomainCppUdrOperationHook CppHook(bool index_safe = false) {
  engine::DomainCppUdrOperationHook hook;
  hook.present = true;
  hook.library_uuid = Uuid(0xa0);
  hook.mapping_descriptor_uuid = Uuid(0xa1);
  hook.mapping_descriptor_epoch = 26;
  hook.entrypoint_symbol = "sb_edr026_operation";
  hook.index_safe = index_safe;
  return hook;
}

engine::DomainCastRuleDescriptor ValidCast() {
  engine::DomainCastRuleDescriptor descriptor;
  descriptor.cast_rule_uuid = Uuid(0x10);
  descriptor.cast_policy_uuid = Uuid(0x11);
  descriptor.descriptor_epoch = 26;
  descriptor.cast_policy_epoch = 26;
  descriptor.stable_name = "edr026.cast";
  descriptor.source_domain_uuid = Uuid(0x20);
  descriptor.target_domain_uuid = Uuid(0x30);
  descriptor.source_descriptor =
      DomainTypeDescriptor(0x40, "edr026.source", descriptor.source_domain_uuid);
  descriptor.target_descriptor =
      DomainTypeDescriptor(0x50, "edr026.target", descriptor.target_domain_uuid);
  descriptor.source_descriptor_uuid =
      descriptor.source_descriptor.descriptor_uuid;
  descriptor.target_descriptor_uuid =
      descriptor.target_descriptor.descriptor_uuid;
  return descriptor;
}

engine::DomainOperationOperandDescriptor Operand(std::uint8_t seed,
                                                engine::Uuid domain_uuid) {
  engine::DomainOperationOperandDescriptor operand;
  operand.domain_uuid = domain_uuid;
  operand.descriptor =
      DomainTypeDescriptor(seed, "edr026.operand", operand.domain_uuid);
  operand.operand_descriptor_uuid = operand.descriptor.descriptor_uuid;
  return operand;
}

engine::DomainOperationDescriptor ValidOperation(
    engine::DomainOperationKind kind = engine::DomainOperationKind::comparison) {
  engine::DomainOperationDescriptor descriptor;
  descriptor.operation_uuid = Uuid(0x60);
  descriptor.operation_policy_uuid = Uuid(0x61);
  descriptor.domain_uuid = Uuid(0x62);
  descriptor.descriptor_epoch = 26;
  descriptor.operation_policy_epoch = 26;
  descriptor.stable_name = "edr026.operation";
  descriptor.operation_kind = kind;
  descriptor.min_arity = 2;
  descriptor.max_arity = 2;
  descriptor.operands.push_back(Operand(0x70, descriptor.domain_uuid));
  descriptor.operands.push_back(Operand(0x80, descriptor.domain_uuid));
  descriptor.result_domain_uuid = Uuid(0x90);
  descriptor.result_descriptor =
      DomainTypeDescriptor(0x91, "edr026.result", descriptor.result_domain_uuid);
  descriptor.result_descriptor_uuid =
      descriptor.result_descriptor.descriptor_uuid;

  if (kind == engine::DomainOperationKind::accessor ||
      kind == engine::DomainOperationKind::constructor) {
    descriptor.min_arity = 1;
    descriptor.max_arity = 2;
  }
  if (kind == engine::DomainOperationKind::aggregate) {
    descriptor.min_arity = 1;
    descriptor.max_arity = 2;
  }
  return descriptor;
}

void RequireCastStatus(const engine::DomainCastRuleDescriptor& descriptor,
                       engine::DomainCastRuleStatus expected,
                       std::string_view message) {
  const auto result = engine::ValidateDomainCastRuleDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-026 domain cast status mismatch");
}

void RequireCastDescriptorStatus(
    const engine::DomainCastRuleDescriptor& descriptor,
    engine::DomainCastRuleStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result = engine::ValidateDomainCastRuleDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-026 domain cast status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "EDR-026 domain cast nested descriptor status mismatch");
}

void RequireOperationStatus(
    const engine::DomainOperationDescriptor& descriptor,
    engine::DomainOperationDescriptorStatus expected,
    std::string_view message) {
  const auto result = engine::ValidateDomainOperationDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-026 domain operation status mismatch");
}

void RequireOperationDescriptorStatus(
    const engine::DomainOperationDescriptor& descriptor,
    engine::DomainOperationDescriptorStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result = engine::ValidateDomainOperationDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-026 domain operation status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "EDR-026 domain operation nested descriptor status mismatch");
}

void TestValidCastProfiles() {
  const engine::DomainCastRuleKind valid_cast_kinds[] = {
      engine::DomainCastRuleKind::implicit,
      engine::DomainCastRuleKind::assignment,
      engine::DomainCastRuleKind::explicit_only,
      engine::DomainCastRuleKind::donor_compatibility};
  for (const auto kind : valid_cast_kinds) {
    auto descriptor = ValidCast();
    descriptor.cast_kind = kind;
    Require(engine::ValidateDomainCastRuleDescriptor(descriptor).ok(),
            "EDR-026 rejected valid cast rule kind");
  }

  auto descriptor = ValidCast();
  descriptor.cast_kind = engine::DomainCastRuleKind::prohibited;
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::refused;
  Require(engine::ValidateDomainCastRuleDescriptor(descriptor).ok(),
          "EDR-026 rejected valid prohibited cast descriptor");

  descriptor = ValidCast();
  descriptor.null_policy = engine::DomainNullPolicy::substitute_default;
  descriptor.null_substitution_payload = {'n'};
  descriptor.missing_policy = engine::DomainMissingPolicy::substitute_default;
  descriptor.missing_substitution_payload = {'m'};
  descriptor.security_policy = engine::DomainSecurityPolicy::explicit_policy;
  descriptor.security_policy_uuid = Uuid(0xb0);
  descriptor.security_policy_epoch = 26;
  descriptor.collation_policy = engine::DomainResourcePolicy::explicit_resource;
  descriptor.collation_uuid = Uuid(0xb1);
  descriptor.collation_epoch = 26;
  descriptor.timezone_policy = engine::DomainResourcePolicy::explicit_resource;
  descriptor.timezone_uuid = Uuid(0xb2);
  descriptor.timezone_epoch = 26;
  descriptor.index_eligibility = engine::DomainIndexEligibility::equality;
  Require(engine::ValidateDomainCastRuleDescriptor(descriptor).ok(),
          "EDR-026 rejected valid cast policy descriptor");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook(true);
  descriptor.index_eligibility = engine::DomainIndexEligibility::equality;
  Require(engine::ValidateDomainCastRuleDescriptor(descriptor).ok(),
          "EDR-026 rejected valid C++ UDR cast descriptor");
}

void TestCastIdentityFailures() {
  auto descriptor = ValidCast();
  descriptor.cast_rule_uuid = {};
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::cast_rule_uuid_required,
                    "EDR-026 accepted cast without rule UUID");

  descriptor = ValidCast();
  descriptor.cast_policy_uuid = {};
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::cast_policy_uuid_required,
                    "EDR-026 accepted cast without policy UUID");

  descriptor = ValidCast();
  descriptor.descriptor_epoch = 0;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::descriptor_epoch_required,
                    "EDR-026 accepted cast without descriptor epoch");

  descriptor = ValidCast();
  descriptor.cast_policy_epoch = 0;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::cast_policy_epoch_required,
                    "EDR-026 accepted cast without policy epoch");

  descriptor = ValidCast();
  descriptor.stable_name.clear();
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::stable_name_required,
                    "EDR-026 accepted cast without stable name");

  descriptor = ValidCast();
  descriptor.descriptor_authoritative = false;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::descriptor_not_authoritative,
                    "EDR-026 accepted non-authoritative cast");

  descriptor = ValidCast();
  descriptor.parser_independent = false;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::descriptor_parser_dependent,
                    "EDR-026 accepted parser-dependent cast");

  descriptor = ValidCast();
  descriptor.source_domain_uuid = {};
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::source_domain_uuid_required,
                    "EDR-026 accepted cast without source domain UUID");

  descriptor = ValidCast();
  descriptor.target_domain_uuid = {};
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::target_domain_uuid_required,
                    "EDR-026 accepted cast without target domain UUID");

  descriptor = ValidCast();
  descriptor.source_descriptor_uuid = {};
  RequireCastStatus(
      descriptor, engine::DomainCastRuleStatus::source_descriptor_uuid_required,
      "EDR-026 accepted cast without source descriptor UUID");

  descriptor = ValidCast();
  descriptor.target_descriptor_uuid = {};
  RequireCastStatus(
      descriptor, engine::DomainCastRuleStatus::target_descriptor_uuid_required,
      "EDR-026 accepted cast without target descriptor UUID");
}

void TestCastDescriptorFailures() {
  auto descriptor = ValidCast();
  descriptor.source_descriptor.descriptor_uuid = {};
  RequireCastDescriptorStatus(
      descriptor, engine::DomainCastRuleStatus::source_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-026 accepted invalid source descriptor");

  descriptor = ValidCast();
  descriptor.target_descriptor.descriptor_epoch = 0;
  RequireCastDescriptorStatus(
      descriptor, engine::DomainCastRuleStatus::target_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
      "EDR-026 accepted invalid target descriptor");

  descriptor = ValidCast();
  descriptor.source_descriptor_uuid = Uuid(0xc0);
  RequireCastStatus(
      descriptor, engine::DomainCastRuleStatus::source_descriptor_uuid_mismatch,
      "EDR-026 accepted mismatched source descriptor UUID");

  descriptor = ValidCast();
  descriptor.target_descriptor_uuid = Uuid(0xc1);
  RequireCastStatus(
      descriptor, engine::DomainCastRuleStatus::target_descriptor_uuid_mismatch,
      "EDR-026 accepted mismatched target descriptor UUID");

  descriptor = ValidCast();
  descriptor.source_descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid);
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::source_descriptor_domain_flag_required,
      "EDR-026 accepted source descriptor without domain flag");

  descriptor = ValidCast();
  descriptor.target_descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid);
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::target_descriptor_domain_flag_required,
      "EDR-026 accepted target descriptor without domain flag");

  descriptor = ValidCast();
  descriptor.source_descriptor.domain_uuid = Uuid(0xc2);
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::source_descriptor_domain_uuid_mismatch,
      "EDR-026 accepted source descriptor with wrong domain UUID");

  descriptor = ValidCast();
  descriptor.target_descriptor.domain_uuid = Uuid(0xc3);
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::target_descriptor_domain_uuid_mismatch,
      "EDR-026 accepted target descriptor with wrong domain UUID");
}

void TestCastPolicyFailures() {
  auto descriptor = ValidCast();
  descriptor.cast_kind = static_cast<engine::DomainCastRuleKind>(0xff);
  RequireCastStatus(descriptor, engine::DomainCastRuleStatus::cast_kind_invalid,
                    "EDR-026 accepted invalid cast kind");

  descriptor = ValidCast();
  descriptor.null_policy = static_cast<engine::DomainNullPolicy>(0xff);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::null_policy_invalid,
                    "EDR-026 accepted invalid null policy");

  descriptor = ValidCast();
  descriptor.missing_policy = static_cast<engine::DomainMissingPolicy>(0xff);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::missing_policy_invalid,
                    "EDR-026 accepted invalid missing policy");

  descriptor = ValidCast();
  descriptor.null_policy = engine::DomainNullPolicy::substitute_default;
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::null_substitution_payload_required,
      "EDR-026 accepted null substitution without payload");

  descriptor = ValidCast();
  descriptor.missing_policy = engine::DomainMissingPolicy::substitute_default;
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::missing_substitution_payload_required,
      "EDR-026 accepted missing substitution without payload");

  descriptor = ValidCast();
  descriptor.null_substitution_payload = {'n'};
  RequireCastStatus(
      descriptor, engine::DomainCastRuleStatus::substitution_payload_not_allowed,
      "EDR-026 accepted stray substitution payload");

  descriptor = ValidCast();
  descriptor.security_policy = static_cast<engine::DomainSecurityPolicy>(0xff);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::security_policy_invalid,
                    "EDR-026 accepted invalid security policy");

  descriptor = ValidCast();
  descriptor.security_policy = engine::DomainSecurityPolicy::explicit_policy;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::security_policy_uuid_required,
                    "EDR-026 accepted security policy without UUID");

  descriptor = ValidCast();
  descriptor.security_policy = engine::DomainSecurityPolicy::explicit_policy;
  descriptor.security_policy_uuid = Uuid(0xd0);
  RequireCastStatus(
      descriptor, engine::DomainCastRuleStatus::security_policy_epoch_required,
      "EDR-026 accepted security policy without epoch");

  descriptor = ValidCast();
  descriptor.collation_policy = static_cast<engine::DomainResourcePolicy>(0xff);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::collation_policy_invalid,
                    "EDR-026 accepted invalid collation policy");

  descriptor = ValidCast();
  descriptor.collation_policy = engine::DomainResourcePolicy::explicit_resource;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::collation_resource_required,
                    "EDR-026 accepted collation policy without resource");

  descriptor = ValidCast();
  descriptor.collation_policy = engine::DomainResourcePolicy::explicit_resource;
  descriptor.collation_uuid = Uuid(0xd1);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::collation_epoch_required,
                    "EDR-026 accepted collation policy without epoch");

  descriptor = ValidCast();
  descriptor.timezone_policy = static_cast<engine::DomainResourcePolicy>(0xff);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::timezone_policy_invalid,
                    "EDR-026 accepted invalid timezone policy");

  descriptor = ValidCast();
  descriptor.timezone_policy = engine::DomainResourcePolicy::explicit_resource;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::timezone_resource_required,
                    "EDR-026 accepted timezone policy without resource");

  descriptor = ValidCast();
  descriptor.timezone_policy = engine::DomainResourcePolicy::explicit_resource;
  descriptor.timezone_uuid = Uuid(0xd2);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::timezone_epoch_required,
                    "EDR-026 accepted timezone policy without epoch");

  descriptor = ValidCast();
  descriptor.determinism = static_cast<engine::DomainDeterminism>(0xff);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::determinism_invalid,
                    "EDR-026 accepted invalid determinism");

  descriptor = ValidCast();
  descriptor.cost_class = static_cast<engine::DomainCostClass>(0xff);
  RequireCastStatus(descriptor, engine::DomainCastRuleStatus::cost_class_invalid,
                    "EDR-026 accepted invalid cost class");

  descriptor = ValidCast();
  descriptor.estimated_cost = 0;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::estimated_cost_required,
                    "EDR-026 accepted zero cast cost");

  descriptor = ValidCast();
  descriptor.estimated_cost = engine::kDomainDescriptorPolicyMaxCost + 1;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::estimated_cost_exceeds_limit,
                    "EDR-026 accepted excessive cast cost");

  descriptor = ValidCast();
  descriptor.index_eligibility =
      static_cast<engine::DomainIndexEligibility>(0xff);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::index_eligibility_invalid,
                    "EDR-026 accepted invalid index eligibility");

  descriptor = ValidCast();
  descriptor.index_eligibility = engine::DomainIndexEligibility::equality;
  descriptor.determinism = engine::DomainDeterminism::volatile_state;
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::index_requires_deterministic_rule,
      "EDR-026 accepted volatile indexable cast");
}

void TestCastImplementationFailures() {
  auto descriptor = ValidCast();
  descriptor.implementation_kind =
      static_cast<engine::DomainDescriptorImplementationKind>(0xff);
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::implementation_kind_invalid,
                    "EDR-026 accepted invalid cast implementation kind");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::cpp_udr_hook_required,
                    "EDR-026 accepted C++ cast without hook");

  descriptor = ValidCast();
  descriptor.cpp_udr_hook = CppHook();
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::cpp_udr_hook_not_allowed,
                    "EDR-026 accepted stray C++ hook");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.library_uuid = {};
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::cpp_udr_library_uuid_required,
                    "EDR-026 accepted C++ hook without library UUID");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.entrypoint_symbol.clear();
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::cpp_udr_entrypoint_required,
                    "EDR-026 accepted C++ hook without entrypoint");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.mapping_descriptor_uuid = {};
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::cpp_udr_mapping_descriptor_uuid_required,
      "EDR-026 accepted C++ hook without mapping UUID");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.mapping_descriptor_epoch = 0;
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::cpp_udr_mapping_descriptor_epoch_required,
      "EDR-026 accepted C++ hook without mapping epoch");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.abi_major = 0;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::cpp_udr_abi_required,
                    "EDR-026 accepted C++ hook without ABI major");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.preserves_descriptors = false;
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::cpp_udr_descriptor_preservation_required,
      "EDR-026 accepted C++ hook that drops descriptors");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.parser_independent = false;
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::cpp_udr_parser_independent_required,
      "EDR-026 accepted parser-dependent C++ hook");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook(false);
  descriptor.index_eligibility = engine::DomainIndexEligibility::equality;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::cpp_udr_index_safety_required,
                    "EDR-026 accepted indexable C++ hook without proof");

  descriptor = ValidCast();
  descriptor.cast_kind = engine::DomainCastRuleKind::prohibited;
  RequireCastStatus(
      descriptor,
      engine::DomainCastRuleStatus::
          prohibited_cast_requires_refused_implementation,
      "EDR-026 accepted prohibited cast with executable implementation");

  descriptor = ValidCast();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::refused;
  RequireCastStatus(descriptor,
                    engine::DomainCastRuleStatus::refused_implementation_not_allowed,
                    "EDR-026 accepted refused implementation for active cast");
}

void TestValidOperationProfiles() {
  const engine::DomainOperationKind kinds[] = {
      engine::DomainOperationKind::comparison,
      engine::DomainOperationKind::arithmetic,
      engine::DomainOperationKind::containment,
      engine::DomainOperationKind::accessor,
      engine::DomainOperationKind::constructor,
      engine::DomainOperationKind::aggregate,
      engine::DomainOperationKind::user_defined};
  for (const auto kind : kinds) {
    Require(engine::ValidateDomainOperationDescriptor(
                ValidOperation(kind)).ok(),
            "EDR-026 rejected valid operation kind");
  }

  auto descriptor = ValidOperation();
  descriptor.null_policy = engine::DomainNullPolicy::substitute_default;
  descriptor.null_substitution_payload = {'n'};
  descriptor.missing_policy = engine::DomainMissingPolicy::substitute_default;
  descriptor.missing_substitution_payload = {'m'};
  descriptor.security_policy = engine::DomainSecurityPolicy::definer_rights;
  descriptor.security_policy_uuid = Uuid(0xe0);
  descriptor.security_policy_epoch = 26;
  descriptor.collation_policy = engine::DomainResourcePolicy::explicit_resource;
  descriptor.collation_uuid = Uuid(0xe1);
  descriptor.collation_epoch = 26;
  descriptor.timezone_policy = engine::DomainResourcePolicy::explicit_resource;
  descriptor.timezone_uuid = Uuid(0xe2);
  descriptor.timezone_epoch = 26;
  descriptor.index_eligibility = engine::DomainIndexEligibility::ordered;
  Require(engine::ValidateDomainOperationDescriptor(descriptor).ok(),
          "EDR-026 rejected valid operation policy descriptor");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook(true);
  descriptor.index_eligibility = engine::DomainIndexEligibility::equality;
  Require(engine::ValidateDomainOperationDescriptor(descriptor).ok(),
          "EDR-026 rejected valid C++ UDR operation descriptor");
}

void TestOperationIdentityAndArityFailures() {
  auto descriptor = ValidOperation();
  descriptor.operation_uuid = {};
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operation_uuid_required,
      "EDR-026 accepted operation without UUID");

  descriptor = ValidOperation();
  descriptor.operation_policy_uuid = {};
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operation_policy_uuid_required,
      "EDR-026 accepted operation without policy UUID");

  descriptor = ValidOperation();
  descriptor.domain_uuid = {};
  RequireOperationStatus(
      descriptor, engine::DomainOperationDescriptorStatus::domain_uuid_required,
      "EDR-026 accepted operation without owner domain UUID");

  descriptor = ValidOperation();
  descriptor.descriptor_epoch = 0;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::descriptor_epoch_required,
      "EDR-026 accepted operation without descriptor epoch");

  descriptor = ValidOperation();
  descriptor.operation_policy_epoch = 0;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operation_policy_epoch_required,
      "EDR-026 accepted operation without policy epoch");

  descriptor = ValidOperation();
  descriptor.stable_name.clear();
  RequireOperationStatus(
      descriptor, engine::DomainOperationDescriptorStatus::stable_name_required,
      "EDR-026 accepted operation without stable name");

  descriptor = ValidOperation();
  descriptor.descriptor_authoritative = false;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::descriptor_not_authoritative,
      "EDR-026 accepted non-authoritative operation");

  descriptor = ValidOperation();
  descriptor.parser_independent = false;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::descriptor_parser_dependent,
      "EDR-026 accepted parser-dependent operation");

  descriptor = ValidOperation();
  descriptor.operation_kind = static_cast<engine::DomainOperationKind>(0xff);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operation_kind_invalid,
      "EDR-026 accepted invalid operation kind");

  descriptor = ValidOperation();
  descriptor.min_arity = 3;
  descriptor.max_arity = 2;
  RequireOperationStatus(descriptor,
                         engine::DomainOperationDescriptorStatus::arity_invalid,
                         "EDR-026 accepted invalid operation arity");

  descriptor = ValidOperation();
  descriptor.operands.assign(engine::kDomainOperationMaxOperands + 1,
                             Operand(0x70, descriptor.domain_uuid));
  descriptor.min_arity = 1;
  descriptor.max_arity = engine::kDomainOperationMaxOperands + 1;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operand_count_exceeds_limit,
      "EDR-026 accepted too many operands");

  descriptor = ValidOperation();
  descriptor.operands.pop_back();
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operand_count_out_of_range,
      "EDR-026 accepted too few operands");
}

void TestOperationDescriptorFailures() {
  auto descriptor = ValidOperation();
  descriptor.operands.front().operand_descriptor_uuid = {};
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operand_descriptor_uuid_required,
      "EDR-026 accepted operand without descriptor UUID");

  descriptor = ValidOperation();
  descriptor.operands.front().descriptor.descriptor_uuid = {};
  RequireOperationDescriptorStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operand_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-026 accepted invalid operand descriptor");

  descriptor = ValidOperation();
  descriptor.operands.front().operand_descriptor_uuid = Uuid(0xe3);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operand_descriptor_uuid_mismatch,
      "EDR-026 accepted mismatched operand descriptor UUID");

  descriptor = ValidOperation();
  descriptor.operands.front().descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operand_domain_flag_required,
      "EDR-026 accepted operand domain UUID without domain flag");

  descriptor = ValidOperation();
  descriptor.operands.front().descriptor.domain_uuid = Uuid(0xe4);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::operand_domain_uuid_mismatch,
      "EDR-026 accepted operand with wrong domain UUID");

  descriptor = ValidOperation();
  descriptor.result_descriptor_uuid = {};
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::result_descriptor_uuid_required,
      "EDR-026 accepted result without descriptor UUID");

  descriptor = ValidOperation();
  descriptor.result_domain_uuid = {};
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::result_domain_uuid_required,
      "EDR-026 accepted result without domain UUID");

  descriptor = ValidOperation();
  descriptor.result_descriptor.descriptor_epoch = 0;
  RequireOperationDescriptorStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::result_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
      "EDR-026 accepted invalid result descriptor");

  descriptor = ValidOperation();
  descriptor.result_descriptor_uuid = Uuid(0xe5);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::result_descriptor_uuid_mismatch,
      "EDR-026 accepted mismatched result descriptor UUID");

  descriptor = ValidOperation();
  descriptor.result_descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          result_descriptor_domain_flag_required,
      "EDR-026 accepted result descriptor without domain flag");

  descriptor = ValidOperation();
  descriptor.result_descriptor.domain_uuid = Uuid(0xe6);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          result_descriptor_domain_uuid_mismatch,
      "EDR-026 accepted result descriptor with wrong domain UUID");
}

void TestOperationPolicyFailures() {
  auto descriptor = ValidOperation();
  descriptor.null_policy = static_cast<engine::DomainNullPolicy>(0xff);
  RequireOperationStatus(
      descriptor, engine::DomainOperationDescriptorStatus::null_policy_invalid,
      "EDR-026 accepted invalid operation null policy");

  descriptor = ValidOperation();
  descriptor.missing_policy = static_cast<engine::DomainMissingPolicy>(0xff);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::missing_policy_invalid,
      "EDR-026 accepted invalid operation missing policy");

  descriptor = ValidOperation();
  descriptor.null_policy = engine::DomainNullPolicy::substitute_default;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          null_substitution_payload_required,
      "EDR-026 accepted operation null substitution without payload");

  descriptor = ValidOperation();
  descriptor.missing_policy = engine::DomainMissingPolicy::substitute_default;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          missing_substitution_payload_required,
      "EDR-026 accepted operation missing substitution without payload");

  descriptor = ValidOperation();
  descriptor.missing_substitution_payload = {'m'};
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::substitution_payload_not_allowed,
      "EDR-026 accepted stray operation substitution payload");

  descriptor = ValidOperation();
  descriptor.security_policy = static_cast<engine::DomainSecurityPolicy>(0xff);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::security_policy_invalid,
      "EDR-026 accepted invalid operation security policy");

  descriptor = ValidOperation();
  descriptor.security_policy = engine::DomainSecurityPolicy::explicit_policy;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::security_policy_uuid_required,
      "EDR-026 accepted operation security policy without UUID");

  descriptor = ValidOperation();
  descriptor.security_policy = engine::DomainSecurityPolicy::explicit_policy;
  descriptor.security_policy_uuid = Uuid(0xe7);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::security_policy_epoch_required,
      "EDR-026 accepted operation security policy without epoch");

  descriptor = ValidOperation();
  descriptor.collation_policy = static_cast<engine::DomainResourcePolicy>(0xff);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::collation_policy_invalid,
      "EDR-026 accepted invalid operation collation policy");

  descriptor = ValidOperation();
  descriptor.collation_policy = engine::DomainResourcePolicy::explicit_resource;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::collation_resource_required,
      "EDR-026 accepted operation collation policy without resource");

  descriptor = ValidOperation();
  descriptor.collation_policy = engine::DomainResourcePolicy::explicit_resource;
  descriptor.collation_uuid = Uuid(0xe8);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::collation_epoch_required,
      "EDR-026 accepted operation collation policy without epoch");

  descriptor = ValidOperation();
  descriptor.timezone_policy = static_cast<engine::DomainResourcePolicy>(0xff);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::timezone_policy_invalid,
      "EDR-026 accepted invalid operation timezone policy");

  descriptor = ValidOperation();
  descriptor.timezone_policy = engine::DomainResourcePolicy::explicit_resource;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::timezone_resource_required,
      "EDR-026 accepted operation timezone policy without resource");

  descriptor = ValidOperation();
  descriptor.timezone_policy = engine::DomainResourcePolicy::explicit_resource;
  descriptor.timezone_uuid = Uuid(0xe9);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::timezone_epoch_required,
      "EDR-026 accepted operation timezone policy without epoch");

  descriptor = ValidOperation();
  descriptor.determinism = static_cast<engine::DomainDeterminism>(0xff);
  RequireOperationStatus(
      descriptor, engine::DomainOperationDescriptorStatus::determinism_invalid,
      "EDR-026 accepted invalid operation determinism");

  descriptor = ValidOperation();
  descriptor.cost_class = static_cast<engine::DomainCostClass>(0xff);
  RequireOperationStatus(
      descriptor, engine::DomainOperationDescriptorStatus::cost_class_invalid,
      "EDR-026 accepted invalid operation cost class");

  descriptor = ValidOperation();
  descriptor.estimated_cost = 0;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::estimated_cost_required,
      "EDR-026 accepted zero operation cost");

  descriptor = ValidOperation();
  descriptor.estimated_cost = engine::kDomainDescriptorPolicyMaxCost + 1;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::estimated_cost_exceeds_limit,
      "EDR-026 accepted excessive operation cost");

  descriptor = ValidOperation();
  descriptor.index_eligibility =
      static_cast<engine::DomainIndexEligibility>(0xff);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::index_eligibility_invalid,
      "EDR-026 accepted invalid operation index eligibility");

  descriptor = ValidOperation();
  descriptor.index_eligibility = engine::DomainIndexEligibility::ordered;
  descriptor.determinism = engine::DomainDeterminism::volatile_state;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          index_requires_deterministic_operation,
      "EDR-026 accepted volatile indexable operation");

  descriptor = ValidOperation();
  descriptor.index_eligibility = engine::DomainIndexEligibility::ordered;
  descriptor.has_side_effects = true;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          side_effecting_operation_not_indexable,
      "EDR-026 accepted side-effecting indexable operation");
}

void TestOperationImplementationFailures() {
  auto descriptor = ValidOperation();
  descriptor.implementation_kind =
      static_cast<engine::DomainDescriptorImplementationKind>(0xff);
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::implementation_kind_invalid,
      "EDR-026 accepted invalid operation implementation kind");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::refused;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::refused_implementation_not_allowed,
      "EDR-026 accepted refused implementation for executable operation");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::cpp_udr_hook_required,
      "EDR-026 accepted C++ operation without hook");

  descriptor = ValidOperation();
  descriptor.cpp_udr_hook = CppHook();
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::cpp_udr_hook_not_allowed,
      "EDR-026 accepted stray C++ operation hook");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.library_uuid = {};
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::cpp_udr_library_uuid_required,
      "EDR-026 accepted C++ operation hook without library UUID");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.entrypoint_symbol.clear();
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::cpp_udr_entrypoint_required,
      "EDR-026 accepted C++ operation hook without entrypoint");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.mapping_descriptor_uuid = {};
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          cpp_udr_mapping_descriptor_uuid_required,
      "EDR-026 accepted C++ operation hook without mapping UUID");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.mapping_descriptor_epoch = 0;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          cpp_udr_mapping_descriptor_epoch_required,
      "EDR-026 accepted C++ operation hook without mapping epoch");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.abi_major = 0;
  RequireOperationStatus(
      descriptor, engine::DomainOperationDescriptorStatus::cpp_udr_abi_required,
      "EDR-026 accepted C++ operation hook without ABI major");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.preserves_descriptors = false;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          cpp_udr_descriptor_preservation_required,
      "EDR-026 accepted C++ operation hook that drops descriptors");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook();
  descriptor.cpp_udr_hook.parser_independent = false;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::
          cpp_udr_parser_independent_required,
      "EDR-026 accepted parser-dependent C++ operation hook");

  descriptor = ValidOperation();
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::cpp_udr;
  descriptor.cpp_udr_hook = CppHook(false);
  descriptor.index_eligibility = engine::DomainIndexEligibility::equality;
  RequireOperationStatus(
      descriptor,
      engine::DomainOperationDescriptorStatus::cpp_udr_index_safety_required,
      "EDR-026 accepted indexable C++ operation without proof");
}

}  // namespace

int main() {
  TestValidCastProfiles();
  TestCastIdentityFailures();
  TestCastDescriptorFailures();
  TestCastPolicyFailures();
  TestCastImplementationFailures();
  TestValidOperationProfiles();
  TestOperationIdentityAndArityFailures();
  TestOperationDescriptorFailures();
  TestOperationPolicyFailures();
  TestOperationImplementationFailures();
  return EXIT_SUCCESS;
}
