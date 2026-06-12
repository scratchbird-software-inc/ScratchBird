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
                                               std::string_view name,
                                               bool nullable) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 31;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  descriptor.length = 255;
  descriptor.nullable_allowed = nullable;
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::length);
  return descriptor;
}

engine::VariantMemberDescriptor Member(std::uint8_t seed,
                                       std::uint32_t ordinal,
                                       std::string_view tag_name,
                                       bool nullable) {
  engine::VariantMemberDescriptor member;
  member.member_uuid = Uuid(seed);
  member.tag_uuid = Uuid(seed + 1);
  member.ordinal = ordinal;
  member.tag_name = std::string(tag_name);
  member.value_descriptor = TypeDescriptor(seed + 2, tag_name, nullable);
  member.value_descriptor_uuid = member.value_descriptor.descriptor_uuid;
  return member;
}

engine::VariantRepresentationDescriptor ValidRepresentation() {
  engine::VariantRepresentationDescriptor descriptor;
  descriptor.variant_uuid = Uuid(0x10);
  descriptor.representation_descriptor_uuid = Uuid(0x11);
  descriptor.tag_descriptor = TypeDescriptor(0x12, "edr031.tag", false);
  descriptor.tag_descriptor_uuid = descriptor.tag_descriptor.descriptor_uuid;
  descriptor.descriptor_epoch = 31;
  descriptor.stable_name = "edr031.variant";
  descriptor.representation_kind =
      engine::VariantRepresentationKind::tagged_union;
  descriptor.members.push_back(Member(0x20, 0, "text_value", true));
  descriptor.members.push_back(Member(0x30, 1, "binary_value", false));
  return descriptor;
}

engine::VariantActiveValueDescriptor ActiveValue(
    const engine::VariantRepresentationDescriptor& representation) {
  engine::VariantActiveValueDescriptor active;
  active.variant_uuid = representation.variant_uuid;
  active.representation_descriptor_uuid =
      representation.representation_descriptor_uuid;
  engine::VariantActiveMemberValue member;
  member.member_uuid = representation.members.front().member_uuid;
  member.tag_uuid = representation.members.front().tag_uuid;
  member.ordinal = representation.members.front().ordinal;
  member.value_descriptor_uuid =
      representation.members.front().value_descriptor_uuid;
  member.value_state = engine::ExecutionValueState::value;
  member.payload = {0x01, 0x02, 0x03};
  active.active_members.push_back(member);
  return active;
}

void PrintMismatch(engine::VariantRepresentationStatus expected,
                   engine::VariantRepresentationStatus actual) {
  std::cerr << "expected=" << engine::VariantRepresentationStatusName(expected)
            << " actual=" << engine::VariantRepresentationStatusName(actual)
            << '\n';
}

void RequireStatus(const engine::VariantRepresentationDescriptor& descriptor,
                   engine::VariantRepresentationStatus expected,
                   std::string_view message) {
  const auto result =
      engine::ValidateVariantRepresentationDescriptor(descriptor);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-031 variant representation status mismatch");
  }
}

void RequireDescriptorStatus(
    const engine::VariantRepresentationDescriptor& descriptor,
    engine::VariantRepresentationStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result =
      engine::ValidateVariantRepresentationDescriptor(descriptor);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-031 variant descriptor status mismatch");
  }
  Require(result.descriptor_status == descriptor_status,
          "EDR-031 nested descriptor status mismatch");
}

void RequireActiveStatus(
    const engine::VariantRepresentationDescriptor& representation,
    const engine::VariantActiveValueDescriptor& active,
    engine::VariantRepresentationStatus expected,
    std::string_view message) {
  const auto result =
      engine::ValidateVariantActiveValue(representation, active);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-031 active variant status mismatch");
  }
}

void TestValidProfiles() {
  const auto representation = ValidRepresentation();
  Require(engine::ValidateVariantRepresentationDescriptor(representation).ok(),
          "EDR-031 rejected valid tagged-union representation");
  Require(engine::ValidateVariantActiveValue(
              representation, ActiveValue(representation)).ok(),
          "EDR-031 rejected valid active tagged-union value");

  auto active = ActiveValue(representation);
  active.active_members.front().value_state = engine::ExecutionValueState::sql_null;
  active.active_members.front().payload.clear();
  Require(engine::ValidateVariantActiveValue(representation, active).ok(),
          "EDR-031 rejected nullable active member SQL null");

  active = ActiveValue(representation);
  active.active_members.front().value_state =
      engine::ExecutionValueState::protected_value;
  Require(engine::ValidateVariantActiveValue(representation, active).ok(),
          "EDR-031 rejected protected-value active member payload");
}

void TestDescriptorIdentityFailures() {
  auto descriptor = ValidRepresentation();
  descriptor.variant_uuid = {};
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::variant_uuid_required,
                "EDR-031 accepted variant without UUID");

  descriptor = ValidRepresentation();
  descriptor.representation_descriptor_uuid = {};
  RequireStatus(
      descriptor,
      engine::VariantRepresentationStatus::
          representation_descriptor_uuid_required,
      "EDR-031 accepted variant without representation descriptor UUID");

  descriptor = ValidRepresentation();
  descriptor.tag_descriptor_uuid = {};
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::tag_descriptor_uuid_required,
                "EDR-031 accepted variant without tag descriptor UUID");

  descriptor = ValidRepresentation();
  descriptor.descriptor_epoch = 0;
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::descriptor_epoch_required,
                "EDR-031 accepted variant without descriptor epoch");

  descriptor = ValidRepresentation();
  descriptor.stable_name.clear();
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::stable_name_required,
                "EDR-031 accepted variant without stable name");

  descriptor = ValidRepresentation();
  descriptor.descriptor_authoritative = false;
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::descriptor_not_authoritative,
                "EDR-031 accepted non-authoritative variant");

  descriptor = ValidRepresentation();
  descriptor.parser_independent = false;
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::descriptor_parser_dependent,
                "EDR-031 accepted parser-dependent variant");

  descriptor = ValidRepresentation();
  descriptor.representation_kind =
      static_cast<engine::VariantRepresentationKind>(0xff);
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::representation_kind_invalid,
                "EDR-031 accepted invalid variant representation kind");
}

void TestTagDescriptorFailures() {
  auto descriptor = ValidRepresentation();
  descriptor.tag_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      descriptor, engine::VariantRepresentationStatus::tag_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-031 accepted invalid tag descriptor");

  descriptor = ValidRepresentation();
  descriptor.tag_descriptor_uuid = Uuid(0x90);
  RequireStatus(
      descriptor,
      engine::VariantRepresentationStatus::tag_descriptor_uuid_mismatch,
      "EDR-031 accepted mismatched tag descriptor UUID");

  descriptor = ValidRepresentation();
  descriptor.tag_descriptor.nullable_allowed = true;
  RequireStatus(
      descriptor,
      engine::VariantRepresentationStatus::tag_descriptor_nullable_not_allowed,
      "EDR-031 accepted nullable variant tag descriptor");
}

void TestMemberFailures() {
  auto descriptor = ValidRepresentation();
  descriptor.members.clear();
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::members_required,
                "EDR-031 accepted variant without allowed members");

  descriptor = ValidRepresentation();
  descriptor.members.resize(engine::kVariantRepresentationMaxMembers + 1);
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::member_count_exceeds_limit,
                "EDR-031 accepted excessive variant members");

  descriptor = ValidRepresentation();
  descriptor.members[0].member_uuid = {};
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::member_uuid_required,
                "EDR-031 accepted member without UUID");

  descriptor = ValidRepresentation();
  descriptor.members[0].tag_uuid = {};
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::member_tag_uuid_required,
                "EDR-031 accepted member without tag UUID");

  descriptor = ValidRepresentation();
  descriptor.members[0].tag_name.clear();
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::member_tag_name_required,
                "EDR-031 accepted member without tag name");

  descriptor = ValidRepresentation();
  descriptor.members[1].ordinal = 0;
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::member_ordinal_mismatch,
                "EDR-031 accepted member with unstable ordinal");

  descriptor = ValidRepresentation();
  descriptor.members[0].value_descriptor_uuid = {};
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::
                    member_descriptor_uuid_required,
                "EDR-031 accepted member without value descriptor UUID");

  descriptor = ValidRepresentation();
  descriptor.members[0].value_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      descriptor, engine::VariantRepresentationStatus::member_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-031 accepted invalid member value descriptor");

  descriptor = ValidRepresentation();
  descriptor.members[0].value_descriptor_uuid = Uuid(0x91);
  RequireStatus(
      descriptor,
      engine::VariantRepresentationStatus::member_descriptor_uuid_mismatch,
      "EDR-031 accepted mismatched member value descriptor UUID");

  descriptor = ValidRepresentation();
  descriptor.members[1].member_uuid = descriptor.members[0].member_uuid;
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::member_uuid_duplicate,
                "EDR-031 accepted duplicate member UUID");

  descriptor = ValidRepresentation();
  descriptor.members[1].tag_uuid = descriptor.members[0].tag_uuid;
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::member_tag_uuid_duplicate,
                "EDR-031 accepted duplicate member tag UUID");

  descriptor = ValidRepresentation();
  descriptor.members[1].tag_name = "TEXT_VALUE";
  RequireStatus(descriptor,
                engine::VariantRepresentationStatus::member_tag_name_duplicate,
                "EDR-031 accepted duplicate canonical member tag name");
}

void TestActiveValueFailures() {
  const auto representation = ValidRepresentation();

  auto active = ActiveValue(representation);
  active.descriptor_authoritative = false;
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::descriptor_not_authoritative,
      "EDR-031 accepted non-authoritative active variant value");

  active = ActiveValue(representation);
  active.parser_independent = false;
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::descriptor_parser_dependent,
      "EDR-031 accepted parser-dependent active variant value");

  active = ActiveValue(representation);
  active.variant_uuid = Uuid(0x92);
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_variant_uuid_mismatch,
      "EDR-031 accepted active value for a different variant UUID");

  active = ActiveValue(representation);
  active.representation_descriptor_uuid = Uuid(0x93);
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::
          active_representation_descriptor_uuid_mismatch,
      "EDR-031 accepted active value for a different representation UUID");

  active = ActiveValue(representation);
  active.active_members.clear();
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_value_count_invalid,
      "EDR-031 accepted no active member");

  active = ActiveValue(representation);
  active.active_members.push_back(active.active_members.front());
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_value_count_invalid,
      "EDR-031 accepted more than one active member");

  active = ActiveValue(representation);
  active.active_members.front().member_uuid = {};
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_member_uuid_required,
      "EDR-031 accepted active member without UUID");

  active = ActiveValue(representation);
  active.active_members.front().tag_uuid = {};
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_tag_uuid_required,
      "EDR-031 accepted active member without tag UUID");

  active = ActiveValue(representation);
  active.active_members.front().member_uuid = Uuid(0x94);
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_member_not_declared,
      "EDR-031 accepted undeclared active member UUID");

  active = ActiveValue(representation);
  active.active_members.front().tag_uuid = representation.members[1].tag_uuid;
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_tag_mismatch,
      "EDR-031 accepted active member tag mismatch");

  active = ActiveValue(representation);
  active.active_members.front().ordinal = 1;
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_ordinal_mismatch,
      "EDR-031 accepted active member ordinal mismatch");

  active = ActiveValue(representation);
  active.active_members.front().value_descriptor_uuid = Uuid(0x95);
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_descriptor_uuid_mismatch,
      "EDR-031 accepted active member descriptor UUID mismatch");

  active = ActiveValue(representation);
  active.active_members.front().value_state = engine::ExecutionValueState::missing;
  active.active_members.front().payload.clear();
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_value_state_invalid,
      "EDR-031 accepted missing as an active member value");

  active = ActiveValue(representation);
  active.active_members.front().member_uuid = representation.members[1].member_uuid;
  active.active_members.front().tag_uuid = representation.members[1].tag_uuid;
  active.active_members.front().ordinal = representation.members[1].ordinal;
  active.active_members.front().value_descriptor_uuid =
      representation.members[1].value_descriptor_uuid;
  active.active_members.front().value_state = engine::ExecutionValueState::sql_null;
  active.active_members.front().payload.clear();
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_value_null_not_allowed,
      "EDR-031 accepted SQL null for non-nullable active member");

  active = ActiveValue(representation);
  active.active_members.front().payload.clear();
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_payload_required,
      "EDR-031 accepted active value without payload");

  active = ActiveValue(representation);
  active.active_members.front().value_state = engine::ExecutionValueState::sql_null;
  active.active_members.front().payload = {0x7f};
  RequireActiveStatus(
      representation, active,
      engine::VariantRepresentationStatus::active_payload_not_allowed,
      "EDR-031 accepted SQL null active value with payload");
}

}  // namespace

int main() {
  TestValidProfiles();
  TestDescriptorIdentityFailures();
  TestTagDescriptorFailures();
  TestMemberFailures();
  TestActiveValueFailures();
  return EXIT_SUCCESS;
}
