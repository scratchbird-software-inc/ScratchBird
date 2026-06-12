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
                                               bool nullable = true) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 30;
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

engine::ContainerFieldDescriptor Field(std::uint8_t seed,
                                       std::uint32_t ordinal,
                                       std::string_view name) {
  engine::ContainerFieldDescriptor field;
  field.field_uuid = Uuid(seed);
  field.ordinal = ordinal;
  field.stable_name = std::string(name);
  field.element_descriptor = TypeDescriptor(seed + 1, name);
  field.element_descriptor_uuid = field.element_descriptor.descriptor_uuid;
  return field;
}

engine::ContainerRepresentationDescriptor ValidArray() {
  engine::ContainerRepresentationDescriptor descriptor;
  descriptor.container_uuid = Uuid(0x10);
  descriptor.representation_descriptor_uuid = Uuid(0x11);
  descriptor.element_descriptor = TypeDescriptor(0x12, "edr030.element");
  descriptor.element_descriptor_uuid = descriptor.element_descriptor.descriptor_uuid;
  descriptor.descriptor_epoch = 30;
  descriptor.stable_name = "edr030.array";
  descriptor.representation_kind = engine::ContainerRepresentationKind::array;
  descriptor.storage_kind = engine::ContainerStorageKind::dense;
  descriptor.dimensions.push_back({1, 3});
  descriptor.dimensions.push_back({0, 2});
  descriptor.element_count = 6;
  descriptor.null_bitmap_present = true;
  descriptor.null_bitmap_bits = descriptor.element_count;
  return descriptor;
}

engine::ContainerRepresentationDescriptor ValidList() {
  auto descriptor = ValidArray();
  descriptor.stable_name = "edr030.list";
  descriptor.representation_kind = engine::ContainerRepresentationKind::list;
  descriptor.dimensions = {{0, 4}};
  descriptor.element_count = 4;
  descriptor.null_bitmap_bits = descriptor.element_count;
  return descriptor;
}

engine::ContainerRepresentationDescriptor ValidMap() {
  auto descriptor = ValidArray();
  descriptor.stable_name = "edr030.map";
  descriptor.representation_kind = engine::ContainerRepresentationKind::map;
  descriptor.storage_kind = engine::ContainerStorageKind::ordered_map;
  descriptor.dimensions.clear();
  descriptor.element_count = 3;
  descriptor.null_bitmap_bits = descriptor.element_count;
  descriptor.map_key_descriptor = TypeDescriptor(0x20, "edr030.key", false);
  descriptor.map_key_descriptor_uuid =
      descriptor.map_key_descriptor.descriptor_uuid;
  return descriptor;
}

engine::ContainerRepresentationDescriptor ValidRow() {
  auto descriptor = ValidArray();
  descriptor.stable_name = "edr030.row";
  descriptor.representation_kind = engine::ContainerRepresentationKind::row;
  descriptor.storage_kind = engine::ContainerStorageKind::row_tuple;
  descriptor.dimensions.clear();
  descriptor.element_count = 0;
  descriptor.null_bitmap_present = false;
  descriptor.null_bitmap_bits = 0;
  descriptor.fields.push_back(Field(0x30, 0, "first_name"));
  descriptor.fields.push_back(Field(0x40, 1, "last_name"));
  return descriptor;
}

void RequireStatus(const engine::ContainerRepresentationDescriptor& descriptor,
                   engine::ContainerRepresentationStatus expected,
                   std::string_view message) {
  const auto result =
      engine::ValidateContainerRepresentationDescriptor(descriptor);
  Require(!result.ok(), message);
  if (result.status != expected) {
    std::cerr << "expected="
              << engine::ContainerRepresentationStatusName(expected)
              << " actual="
              << engine::ContainerRepresentationStatusName(result.status)
              << '\n';
    Fail("EDR-030 container representation status mismatch");
  }
}

void RequireDescriptorStatus(
    const engine::ContainerRepresentationDescriptor& descriptor,
    engine::ContainerRepresentationStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result =
      engine::ValidateContainerRepresentationDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-030 container representation status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "EDR-030 nested descriptor status mismatch");
}

void TestValidProfiles() {
  Require(engine::ValidateContainerRepresentationDescriptor(ValidArray()).ok(),
          "EDR-030 rejected valid dense array");
  Require(engine::ValidateContainerRepresentationDescriptor(ValidList()).ok(),
          "EDR-030 rejected valid dense list");
  Require(engine::ValidateContainerRepresentationDescriptor(ValidMap()).ok(),
          "EDR-030 rejected valid ordered map");
  Require(engine::ValidateContainerRepresentationDescriptor(ValidRow()).ok(),
          "EDR-030 rejected valid row tuple");

  auto descriptor = ValidArray();
  descriptor.storage_kind = engine::ContainerStorageKind::sparse;
  descriptor.sparse = true;
  Require(engine::ValidateContainerRepresentationDescriptor(descriptor).ok(),
          "EDR-030 rejected valid sparse array");

  descriptor = ValidMap();
  descriptor.storage_kind = engine::ContainerStorageKind::hash_map;
  descriptor.ordered = false;
  Require(engine::ValidateContainerRepresentationDescriptor(descriptor).ok(),
          "EDR-030 rejected valid hash map");

  descriptor = ValidRow();
  descriptor.representation_kind =
      engine::ContainerRepresentationKind::composite;
  descriptor.stable_name = "edr030.composite";
  Require(engine::ValidateContainerRepresentationDescriptor(descriptor).ok(),
          "EDR-030 rejected valid composite tuple");
}

void TestIdentityFailures() {
  auto descriptor = ValidArray();
  descriptor.container_uuid = {};
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::container_uuid_required,
                "EDR-030 accepted container without UUID");

  descriptor = ValidArray();
  descriptor.representation_descriptor_uuid = {};
  RequireStatus(
      descriptor,
      engine::ContainerRepresentationStatus::
          representation_descriptor_uuid_required,
      "EDR-030 accepted container without descriptor UUID");

  descriptor = ValidArray();
  descriptor.element_descriptor_uuid = {};
  RequireStatus(
      descriptor,
      engine::ContainerRepresentationStatus::element_descriptor_uuid_required,
      "EDR-030 accepted container without element descriptor UUID");

  descriptor = ValidArray();
  descriptor.descriptor_epoch = 0;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::descriptor_epoch_required,
                "EDR-030 accepted container without descriptor epoch");

  descriptor = ValidArray();
  descriptor.stable_name.clear();
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::stable_name_required,
                "EDR-030 accepted container without stable name");

  descriptor = ValidArray();
  descriptor.descriptor_authoritative = false;
  RequireStatus(
      descriptor,
      engine::ContainerRepresentationStatus::descriptor_not_authoritative,
      "EDR-030 accepted non-authoritative container");

  descriptor = ValidArray();
  descriptor.parser_independent = false;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::descriptor_parser_dependent,
                "EDR-030 accepted parser-dependent container");
}

void TestElementAndShapeFailures() {
  auto descriptor = ValidArray();
  descriptor.representation_kind =
      static_cast<engine::ContainerRepresentationKind>(0xff);
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::representation_kind_invalid,
                "EDR-030 accepted invalid container kind");

  descriptor = ValidArray();
  descriptor.storage_kind = static_cast<engine::ContainerStorageKind>(0xff);
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::storage_kind_invalid,
                "EDR-030 accepted invalid storage kind");

  descriptor = ValidArray();
  descriptor.element_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      descriptor, engine::ContainerRepresentationStatus::element_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-030 accepted invalid element descriptor");

  descriptor = ValidArray();
  descriptor.element_descriptor_uuid = Uuid(0x90);
  RequireStatus(
      descriptor,
      engine::ContainerRepresentationStatus::element_descriptor_uuid_mismatch,
      "EDR-030 accepted mismatched element descriptor UUID");

  descriptor = ValidArray();
  descriptor.dimensions.clear();
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::dimensions_required,
                "EDR-030 accepted array without dimensions");

  descriptor = ValidArray();
  descriptor.dimensions.assign(engine::kContainerRepresentationMaxDimensions + 1,
                               {0, 1});
  RequireStatus(
      descriptor,
      engine::ContainerRepresentationStatus::dimension_count_exceeds_limit,
      "EDR-030 accepted too many dimensions");

  descriptor = ValidArray();
  descriptor.dimensions.front().length = 0;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::dimension_length_required,
                "EDR-030 accepted zero-length dimension");

  descriptor = ValidList();
  descriptor.dimensions.front().lower_bound = 1;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::list_dimension_invalid,
                "EDR-030 accepted list with non-zero lower bound");

  descriptor = ValidArray();
  descriptor.element_count = 0;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::element_count_required,
                "EDR-030 accepted array without element count");

  descriptor = ValidRow();
  descriptor.dimensions.push_back({0, 1});
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::dimensions_not_allowed,
                "EDR-030 accepted row with dimensions");

  descriptor = ValidArray();
  descriptor.fields.push_back(Field(0xa0, 0, "illegal"));
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::fields_not_allowed,
                "EDR-030 accepted array with row fields");
}

void TestStorageAndNullBitmapFailures() {
  auto descriptor = ValidArray();
  descriptor.storage_kind = engine::ContainerStorageKind::ordered_map;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::storage_kind_incompatible,
                "EDR-030 accepted map storage for array");

  descriptor = ValidArray();
  descriptor.storage_kind = engine::ContainerStorageKind::sparse;
  descriptor.sparse = false;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::sparse_flag_required,
                "EDR-030 accepted sparse storage without sparse flag");

  descriptor = ValidArray();
  descriptor.storage_kind = engine::ContainerStorageKind::dense;
  descriptor.sparse = true;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::sparse_flag_not_allowed,
                "EDR-030 accepted sparse flag for dense storage");

  descriptor = ValidArray();
  descriptor.null_bitmap_present = false;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::null_bitmap_required,
                "EDR-030 accepted nullable array without null bitmap");

  descriptor = ValidArray();
  descriptor.element_descriptor.nullable_allowed = false;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::null_bitmap_not_allowed,
                "EDR-030 accepted null bitmap for non-nullable element");

  descriptor = ValidArray();
  descriptor.null_bitmap_bits = descriptor.element_count - 1;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::null_bitmap_size_mismatch,
                "EDR-030 accepted null bitmap with wrong size");
}

void TestMapFailures() {
  auto descriptor = ValidMap();
  descriptor.storage_kind = engine::ContainerStorageKind::dense;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::storage_kind_incompatible,
                "EDR-030 accepted dense storage for map");

  descriptor = ValidMap();
  descriptor.map_key_descriptor_uuid = {};
  RequireStatus(
      descriptor,
      engine::ContainerRepresentationStatus::map_key_descriptor_uuid_required,
      "EDR-030 accepted map without key descriptor UUID");

  descriptor = ValidMap();
  descriptor.map_key_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      descriptor, engine::ContainerRepresentationStatus::map_key_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-030 accepted invalid map key descriptor");

  descriptor = ValidMap();
  descriptor.map_key_descriptor_uuid = Uuid(0xb0);
  RequireStatus(
      descriptor,
      engine::ContainerRepresentationStatus::map_key_descriptor_uuid_mismatch,
      "EDR-030 accepted mismatched map key descriptor UUID");

  descriptor = ValidMap();
  descriptor.map_key_descriptor.nullable_allowed = true;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::map_key_nullable_not_allowed,
                "EDR-030 accepted nullable map key descriptor");

  descriptor = ValidMap();
  descriptor.element_count = 0;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::element_count_required,
                "EDR-030 accepted map without element count");

  descriptor = ValidArray();
  descriptor.map_key_descriptor_uuid = Uuid(0xb1);
  RequireStatus(
      descriptor,
      engine::ContainerRepresentationStatus::map_key_descriptor_not_allowed,
      "EDR-030 accepted map key descriptor on non-map container");
}

void TestFieldFailures() {
  auto descriptor = ValidRow();
  descriptor.fields.clear();
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::fields_required,
                "EDR-030 accepted row without fields");

  descriptor = ValidRow();
  descriptor.fields.assign(engine::kContainerRepresentationMaxFields + 1,
                           Field(0x10, 0, "too_many"));
  for (std::size_t index = 0; index < descriptor.fields.size(); ++index) {
    descriptor.fields[index] =
        Field(static_cast<std::uint8_t>(0x10 + index),
              static_cast<std::uint32_t>(index),
              "field_" + std::to_string(index));
  }
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::field_count_exceeds_limit,
                "EDR-030 accepted too many fields");

  descriptor = ValidRow();
  descriptor.fields.front().field_uuid = {};
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::field_uuid_required,
                "EDR-030 accepted field without UUID");

  descriptor = ValidRow();
  descriptor.fields.front().stable_name.clear();
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::field_stable_name_required,
                "EDR-030 accepted field without stable name");

  descriptor = ValidRow();
  descriptor.fields.back().ordinal = 7;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::field_ordinal_mismatch,
                "EDR-030 accepted field ordinal gap");

  descriptor = ValidRow();
  descriptor.fields.back().field_uuid = descriptor.fields.front().field_uuid;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::field_uuid_duplicate,
                "EDR-030 accepted duplicate field UUID");

  descriptor = ValidRow();
  descriptor.fields.back().stable_name = descriptor.fields.front().stable_name;
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::field_name_duplicate,
                "EDR-030 accepted duplicate field name");

  descriptor = ValidRow();
  descriptor.fields.front().element_descriptor_uuid = {};
  RequireStatus(descriptor,
                engine::ContainerRepresentationStatus::field_descriptor_uuid_required,
                "EDR-030 accepted field without descriptor UUID");

  descriptor = ValidRow();
  descriptor.fields.front().element_descriptor.descriptor_epoch = 0;
  RequireDescriptorStatus(
      descriptor, engine::ContainerRepresentationStatus::field_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
      "EDR-030 accepted invalid field descriptor");

  descriptor = ValidRow();
  descriptor.fields.front().element_descriptor_uuid = Uuid(0xc0);
  RequireStatus(
      descriptor,
      engine::ContainerRepresentationStatus::field_descriptor_uuid_mismatch,
      "EDR-030 accepted mismatched field descriptor UUID");
}

}  // namespace

int main() {
  TestValidProfiles();
  TestIdentityFailures();
  TestElementAndShapeFailures();
  TestStorageAndNullBitmapFailures();
  TestMapFailures();
  TestFieldFailures();
  return EXIT_SUCCESS;
}
