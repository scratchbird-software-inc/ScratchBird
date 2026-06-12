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
#include <vector>

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

engine::ExecutionTypeDescriptor Descriptor(std::uint8_t seed,
                                           std::string_view name) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 32;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  descriptor.length = 128;
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::length);
  return descriptor;
}

engine::ExecutionTypeDescriptor DomainDescriptor(std::uint8_t seed,
                                                 std::string_view name,
                                                 std::uint8_t domain_seed) {
  auto descriptor = Descriptor(seed, name);
  descriptor.domain_uuid = Uuid(domain_seed);
  descriptor.domain_stack = {Uuid(domain_seed), Uuid(domain_seed + 1)};
  descriptor.modifier_flags |=
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_stack);
  return descriptor;
}

engine::ResultColumnDescriptor Column(std::uint32_t ordinal,
                                      std::uint8_t seed,
                                      std::string_view name) {
  return {ordinal,
          Descriptor(seed, name),
          std::string(name),
          std::string(name),
          std::string(name),
          true};
}

engine::ExecutionRelationDescriptor Relation(engine::ExecutionRelationKind kind,
                                             std::uint8_t seed,
                                             std::string_view name) {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(seed);
  relation.descriptor_epoch = 32;
  relation.relation_kind = kind;
  relation.stable_name = std::string(name);
  relation.columns.push_back(Column(0, seed + 1, "payload"));
  relation.snapshot_uuid = Uuid(seed + 2);
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(seed + 3);
  relation.memory_policy_uuid = Uuid(seed + 4);
  relation.memory_policy_epoch = 32;
  return relation;
}

engine::ExecutionRoutineParameterDescriptor ScalarParameter(
    std::uint32_t ordinal,
    std::string_view name,
    std::uint8_t seed) {
  engine::ExecutionRoutineParameterDescriptor parameter;
  parameter.ordinal = ordinal;
  parameter.stable_name = std::string(name);
  parameter.direction = engine::ExecutionRoutineParameterDirection::in;
  parameter.parameter_kind = engine::ExecutionRoutineParameterKind::scalar;
  parameter.scalar_descriptor = Descriptor(seed, name);
  return parameter;
}

engine::ExecutionRoutineParameterDescriptor RelationParameter(
    std::uint32_t ordinal,
    std::string_view name,
    engine::ExecutionRoutineParameterKind parameter_kind,
    engine::ExecutionRelationKind relation_kind,
    std::uint8_t seed) {
  engine::ExecutionRoutineParameterDescriptor parameter;
  parameter.ordinal = ordinal;
  parameter.stable_name = std::string(name);
  parameter.direction = engine::ExecutionRoutineParameterDirection::in;
  parameter.parameter_kind = parameter_kind;
  parameter.relation_descriptor = Relation(relation_kind, seed, name);
  return parameter;
}

engine::ExecutionRoutineSignatureDescriptor ValidSignature() {
  engine::ExecutionRoutineSignatureDescriptor signature;
  signature.routine_signature_uuid = Uuid(0x60);
  signature.signature_epoch = 32;
  signature.routine_uuid = Uuid(0x61);
  signature.routine_kind = engine::ExecutionRoutineKind::procedure;
  signature.stable_name = "edr032.polymorphic.signature";
  signature.parameter_descriptors.push_back(
      ScalarParameter(0, "left_value", 0x10));
  signature.parameter_descriptors.push_back(
      ScalarParameter(1, "right_value", 0x20));
  signature.parameter_descriptors.push_back(RelationParameter(
      2, "cursor_value", engine::ExecutionRoutineParameterKind::cursor,
      engine::ExecutionRelationKind::cursor, 0x70));
  signature.parameter_descriptors.push_back(RelationParameter(
      3, "row_value", engine::ExecutionRoutineParameterKind::rowset,
      engine::ExecutionRelationKind::rowset, 0x80));
  signature.parameter_descriptors.push_back(RelationParameter(
      4, "table_value", engine::ExecutionRoutineParameterKind::table_value,
      engine::ExecutionRelationKind::table_value, 0x90));
  return signature;
}

engine::PolymorphicRoutineBindingSlot Slot(
    const engine::ExecutionRoutineParameterDescriptor& parameter,
    engine::PolymorphicRoutineTypeKind polymorphic_kind,
    engine::Uuid group_uuid) {
  engine::PolymorphicRoutineBindingSlot slot;
  slot.ordinal = parameter.ordinal;
  slot.stable_name = parameter.stable_name;
  slot.parameter_kind = parameter.parameter_kind;
  slot.polymorphic_kind = polymorphic_kind;
  slot.binding_group_uuid = group_uuid;
  slot.scalar_descriptor = parameter.scalar_descriptor;
  slot.relation_descriptor = parameter.relation_descriptor;
  return slot;
}

engine::PolymorphicRoutineBindingDescriptor ValidBinding(
    const engine::ExecutionRoutineSignatureDescriptor& signature) {
  engine::PolymorphicRoutineBindingDescriptor binding;
  binding.binding_uuid = Uuid(0xa0);
  binding.routine_signature_uuid = signature.routine_signature_uuid;
  binding.binding_epoch = 32;
  binding.signature_epoch = signature.signature_epoch;
  binding.stable_name = "edr032.polymorphic.binding";
  binding.slots.push_back(Slot(signature.parameter_descriptors[0],
                               engine::PolymorphicRoutineTypeKind::any,
                               Uuid(0xb0)));
  binding.slots.push_back(Slot(signature.parameter_descriptors[1],
                               engine::PolymorphicRoutineTypeKind::anyelement,
                               Uuid(0xb0)));
  binding.slots.push_back(Slot(signature.parameter_descriptors[2],
                               engine::PolymorphicRoutineTypeKind::anycursor,
                               Uuid(0xb1)));
  binding.slots.push_back(Slot(signature.parameter_descriptors[3],
                               engine::PolymorphicRoutineTypeKind::anyrow,
                               Uuid(0xb2)));
  binding.slots.push_back(Slot(signature.parameter_descriptors[4],
                               engine::PolymorphicRoutineTypeKind::anytable,
                               Uuid(0xb3)));
  binding.slots[1].scalar_descriptor = binding.slots[0].scalar_descriptor;
  return binding;
}

void PrintMismatch(engine::PolymorphicRoutineBindingStatus expected,
                   engine::PolymorphicRoutineBindingStatus actual) {
  std::cerr << "expected="
            << engine::PolymorphicRoutineBindingStatusName(expected)
            << " actual=" << engine::PolymorphicRoutineBindingStatusName(actual)
            << '\n';
}

void RequireStatus(
    const engine::ExecutionRoutineSignatureDescriptor& signature,
    const engine::PolymorphicRoutineBindingDescriptor& binding,
    engine::PolymorphicRoutineBindingStatus expected,
    std::string_view message) {
  const auto result =
      engine::ValidatePolymorphicRoutineBinding(signature, binding);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-032 polymorphic routine binding status mismatch");
  }
}

void RequireScalarStatus(
    const engine::ExecutionRoutineSignatureDescriptor& signature,
    const engine::PolymorphicRoutineBindingDescriptor& binding,
    engine::PolymorphicRoutineBindingStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result =
      engine::ValidatePolymorphicRoutineBinding(signature, binding);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-032 polymorphic scalar status mismatch");
  }
  Require(result.scalar_descriptor_status == descriptor_status,
          "EDR-032 scalar descriptor diagnostic mismatch");
}

void RequireRelationStatus(
    const engine::ExecutionRoutineSignatureDescriptor& signature,
    const engine::PolymorphicRoutineBindingDescriptor& binding,
    engine::PolymorphicRoutineBindingStatus expected,
    engine::ExecutionRelationDescriptorStatus relation_status,
    std::string_view message) {
  const auto result =
      engine::ValidatePolymorphicRoutineBinding(signature, binding);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-032 polymorphic relation status mismatch");
  }
  Require(result.relation_status == relation_status,
          "EDR-032 relation descriptor diagnostic mismatch");
}

void TestValidBindings() {
  const auto signature = ValidSignature();
  auto binding = ValidBinding(signature);
  Require(engine::ValidatePolymorphicRoutineBinding(signature, binding).ok(),
          "EDR-032 rejected valid ANY/ANYELEMENT/ANYCURSOR/ANYROW/ANYTABLE");

  binding = ValidBinding(signature);
  binding.slots[0].match_policy =
      engine::PolymorphicRoutineBindingMatchPolicy::same_domain_stack;
  binding.slots[1].match_policy =
      engine::PolymorphicRoutineBindingMatchPolicy::same_domain_stack;
  binding.slots[0].scalar_descriptor =
      DomainDescriptor(0xc0, "domain_left", 0xd0);
  binding.slots[1].scalar_descriptor =
      DomainDescriptor(0xc1, "domain_right", 0xd0);
  Require(engine::ValidatePolymorphicRoutineBinding(signature, binding).ok(),
          "EDR-032 rejected same-domain-stack polymorphic binding");

  binding = ValidBinding(signature);
  binding.slots[0].polymorphic_kind =
      engine::PolymorphicRoutineTypeKind::concrete;
  binding.slots[0].binding_group_uuid = {};
  Require(engine::ValidatePolymorphicRoutineBinding(signature, binding).ok(),
          "EDR-032 rejected concrete scalar binding slot");
}

void TestBindingIdentityFailures() {
  const auto signature = ValidSignature();
  auto binding = ValidBinding(signature);
  binding.binding_uuid = {};
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::binding_uuid_required,
                "EDR-032 accepted binding without UUID");

  binding = ValidBinding(signature);
  binding.routine_signature_uuid = {};
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::
                    routine_signature_uuid_required,
                "EDR-032 accepted binding without routine signature UUID");

  binding = ValidBinding(signature);
  binding.binding_epoch = 0;
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::binding_epoch_required,
                "EDR-032 accepted binding without epoch");

  binding = ValidBinding(signature);
  binding.signature_epoch = 0;
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::signature_epoch_required,
                "EDR-032 accepted binding without signature epoch");

  binding = ValidBinding(signature);
  binding.stable_name.clear();
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::stable_name_required,
                "EDR-032 accepted binding without stable name");

  binding = ValidBinding(signature);
  binding.descriptor_authoritative = false;
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::
                    descriptor_not_authoritative,
                "EDR-032 accepted non-authoritative binding");

  binding = ValidBinding(signature);
  binding.parser_independent = false;
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::
                    descriptor_parser_dependent,
                "EDR-032 accepted parser-dependent binding");

  auto invalid_signature = signature;
  invalid_signature.routine_signature_uuid = {};
  binding = ValidBinding(signature);
  RequireStatus(invalid_signature, binding,
                engine::PolymorphicRoutineBindingStatus::
                    signature_descriptor_invalid,
                "EDR-032 accepted invalid routine signature");

  binding = ValidBinding(signature);
  binding.routine_signature_uuid = Uuid(0xe0);
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::signature_uuid_mismatch,
                "EDR-032 accepted wrong routine signature UUID");

  binding = ValidBinding(signature);
  binding.signature_epoch += 1;
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::signature_epoch_mismatch,
                "EDR-032 accepted wrong routine signature epoch");

  binding = ValidBinding(signature);
  binding.slots.pop_back();
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::slot_count_mismatch,
                "EDR-032 accepted missing polymorphic binding slot");
}

void TestSlotFailures() {
  const auto signature = ValidSignature();
  auto binding = ValidBinding(signature);
  binding.slots[1].ordinal = 9;
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::slot_ordinal_mismatch,
                "EDR-032 accepted non-canonical binding slot ordinal");

  binding = ValidBinding(signature);
  binding.slots[1].stable_name = "wrong_name";
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::slot_name_mismatch,
                "EDR-032 accepted binding slot name mismatch");

  binding = ValidBinding(signature);
  binding.slots[0].parameter_kind =
      engine::ExecutionRoutineParameterKind::cursor;
  RequireStatus(
      signature, binding,
      engine::PolymorphicRoutineBindingStatus::slot_parameter_kind_mismatch,
      "EDR-032 accepted binding slot parameter kind mismatch");

  binding = ValidBinding(signature);
  binding.slots[0].polymorphic_kind =
      static_cast<engine::PolymorphicRoutineTypeKind>(0xff);
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::polymorphic_kind_invalid,
                "EDR-032 accepted invalid polymorphic type kind");

  binding = ValidBinding(signature);
  binding.slots[0].match_policy =
      static_cast<engine::PolymorphicRoutineBindingMatchPolicy>(0xff);
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::binding_policy_invalid,
                "EDR-032 accepted invalid polymorphic binding policy");

  binding = ValidBinding(signature);
  binding.slots[0].polymorphic_kind =
      engine::PolymorphicRoutineTypeKind::anycursor;
  RequireStatus(
      signature, binding,
      engine::PolymorphicRoutineBindingStatus::polymorphic_kind_incompatible,
      "EDR-032 accepted ANYCURSOR for scalar parameter");

  binding = ValidBinding(signature);
  binding.slots[0].binding_group_uuid = {};
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::
                    binding_group_uuid_required,
                "EDR-032 accepted polymorphic slot without group UUID");
}

void TestDescriptorFailures() {
  const auto signature = ValidSignature();
  auto binding = ValidBinding(signature);
  binding.slots[0].scalar_descriptor.descriptor_uuid = {};
  RequireScalarStatus(
      signature, binding,
      engine::PolymorphicRoutineBindingStatus::scalar_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-032 accepted invalid scalar binding descriptor");

  binding = ValidBinding(signature);
  binding.slots[2].relation_descriptor.relation_descriptor_uuid = {};
  RequireRelationStatus(
      signature, binding,
      engine::PolymorphicRoutineBindingStatus::relation_descriptor_invalid,
      engine::ExecutionRelationDescriptorStatus::descriptor_uuid_required,
      "EDR-032 accepted invalid relation binding descriptor");

  binding = ValidBinding(signature);
  binding.slots[2].relation_descriptor.relation_kind =
      engine::ExecutionRelationKind::rowset;
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::relation_kind_mismatch,
                "EDR-032 accepted ANYCURSOR bound to rowset relation");

  binding = ValidBinding(signature);
  binding.slots[0].polymorphic_kind =
      engine::PolymorphicRoutineTypeKind::concrete;
  binding.slots[0].binding_group_uuid = {};
  binding.slots[0].scalar_descriptor.descriptor_epoch += 1;
  RequireStatus(
      signature, binding,
      engine::PolymorphicRoutineBindingStatus::
          concrete_scalar_descriptor_mismatch,
      "EDR-032 accepted concrete scalar descriptor mismatch");

  binding = ValidBinding(signature);
  binding.slots[2].polymorphic_kind =
      engine::PolymorphicRoutineTypeKind::concrete;
  binding.slots[2].binding_group_uuid = {};
  binding.slots[2].relation_descriptor.descriptor_epoch += 1;
  RequireStatus(
      signature, binding,
      engine::PolymorphicRoutineBindingStatus::
          concrete_relation_descriptor_mismatch,
      "EDR-032 accepted concrete relation descriptor mismatch");
}

void TestBindingGroupFailures() {
  const auto signature = ValidSignature();
  auto binding = ValidBinding(signature);
  binding.slots[2].binding_group_uuid = binding.slots[0].binding_group_uuid;
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::
                    binding_group_kind_mismatch,
                "EDR-032 accepted mixed scalar/relation binding group");

  binding = ValidBinding(signature);
  binding.slots[1].match_policy =
      engine::PolymorphicRoutineBindingMatchPolicy::same_domain_stack;
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::
                    binding_group_policy_mismatch,
                "EDR-032 accepted mismatched binding group policy");

  binding = ValidBinding(signature);
  binding.slots[1].scalar_descriptor = Descriptor(0xc0, "different_type");
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::
                    binding_group_descriptor_mismatch,
                "EDR-032 accepted same-type group descriptor mismatch");

  binding = ValidBinding(signature);
  binding.slots[0].match_policy =
      engine::PolymorphicRoutineBindingMatchPolicy::same_domain_stack;
  binding.slots[1].match_policy =
      engine::PolymorphicRoutineBindingMatchPolicy::same_domain_stack;
  binding.slots[0].scalar_descriptor =
      DomainDescriptor(0xc0, "domain_left", 0xd0);
  binding.slots[1].scalar_descriptor =
      DomainDescriptor(0xc1, "domain_right", 0xd2);
  RequireStatus(signature, binding,
                engine::PolymorphicRoutineBindingStatus::
                    binding_group_domain_stack_mismatch,
                "EDR-032 accepted mismatched domain-stack group");
}

}  // namespace

int main() {
  TestValidBindings();
  TestBindingIdentityFailures();
  TestSlotFailures();
  TestDescriptorFailures();
  TestBindingGroupFailures();
  return EXIT_SUCCESS;
}
