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

engine::EnumSetLabelDescriptor Label(std::uint8_t seed,
                                     std::uint32_t ordinal,
                                     std::string_view name) {
  engine::EnumSetLabelDescriptor label;
  label.label_uuid = Uuid(seed);
  label.ordinal = ordinal;
  label.stable_name = std::string(name);
  label.canonical_rendering = std::string(name);
  label.donor_rendering_name = std::string(name);
  return label;
}

engine::EnumSetRepresentationDescriptor ValidEnum() {
  engine::EnumSetRepresentationDescriptor descriptor;
  descriptor.enum_set_uuid = Uuid(0x10);
  descriptor.representation_descriptor_uuid = Uuid(0x11);
  descriptor.domain_uuid = Uuid(0x12);
  descriptor.descriptor_epoch = 28;
  descriptor.stable_name = "edr028.status_enum";
  descriptor.labels.push_back(Label(0x20, 0, "pending"));
  descriptor.labels.push_back(Label(0x30, 1, "active"));
  descriptor.labels.push_back(Label(0x40, 2, "closed"));
  return descriptor;
}

engine::EnumSetRepresentationDescriptor ValidSet() {
  auto descriptor = ValidEnum();
  descriptor.representation_kind =
      engine::EnumSetRepresentationKind::set_collection;
  descriptor.storage_kind = engine::EnumSetStorageKind::bitset;
  descriptor.stable_name = "edr028.flag_set";
  return descriptor;
}

void RequireStatus(const engine::EnumSetRepresentationDescriptor& descriptor,
                   engine::EnumSetRepresentationStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateEnumSetRepresentationDescriptor(descriptor);
  Require(!result.ok(), message);
  if (result.status != expected) {
    std::cerr << "expected="
              << engine::EnumSetRepresentationStatusName(expected)
              << " actual="
              << engine::EnumSetRepresentationStatusName(result.status)
              << '\n';
    Fail("EDR-028 enum/set representation status mismatch");
  }
}

void TestValidProfiles() {
  auto descriptor = ValidEnum();
  Require(engine::ValidateEnumSetRepresentationDescriptor(descriptor).ok(),
          "EDR-028 rejected valid ordinal enum");

  descriptor = ValidEnum();
  descriptor.storage_kind = engine::EnumSetStorageKind::string;
  descriptor.unknown_label_policy =
      engine::EnumSetUnknownLabelPolicy::map_to_unknown_label;
  descriptor.unknown_label_uuid = descriptor.labels.back().label_uuid;
  descriptor.labels.front().aliases = {"queued", "waiting"};
  Require(engine::ValidateEnumSetRepresentationDescriptor(descriptor).ok(),
          "EDR-028 rejected valid string enum with unknown label mapping");

  descriptor = ValidEnum();
  descriptor.storage_kind = engine::EnumSetStorageKind::donor_native;
  descriptor.unknown_label_policy =
      engine::EnumSetUnknownLabelPolicy::donor_compatibility;
  descriptor.donor_profile_uuid = Uuid(0x50);
  Require(engine::ValidateEnumSetRepresentationDescriptor(descriptor).ok(),
          "EDR-028 rejected valid donor enum rendering");

  descriptor = ValidSet();
  Require(engine::ValidateEnumSetRepresentationDescriptor(descriptor).ok(),
          "EDR-028 rejected valid bitset set representation");

  descriptor = ValidSet();
  descriptor.storage_kind = engine::EnumSetStorageKind::list;
  descriptor.ordered = false;
  Require(engine::ValidateEnumSetRepresentationDescriptor(descriptor).ok(),
          "EDR-028 rejected valid unordered list set representation");
}

void TestIdentityFailures() {
  auto descriptor = ValidEnum();
  descriptor.enum_set_uuid = {};
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::enum_set_uuid_required,
                "EDR-028 accepted enum/set without UUID");

  descriptor = ValidEnum();
  descriptor.representation_descriptor_uuid = {};
  RequireStatus(
      descriptor,
      engine::EnumSetRepresentationStatus::
          representation_descriptor_uuid_required,
      "EDR-028 accepted enum/set without descriptor UUID");

  descriptor = ValidEnum();
  descriptor.domain_uuid = {};
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::domain_uuid_required,
                "EDR-028 accepted enum/set without domain UUID");

  descriptor = ValidEnum();
  descriptor.descriptor_epoch = 0;
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::descriptor_epoch_required,
                "EDR-028 accepted enum/set without descriptor epoch");

  descriptor = ValidEnum();
  descriptor.stable_name.clear();
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::stable_name_required,
                "EDR-028 accepted enum/set without stable name");

  descriptor = ValidEnum();
  descriptor.descriptor_authoritative = false;
  RequireStatus(
      descriptor,
      engine::EnumSetRepresentationStatus::descriptor_not_authoritative,
      "EDR-028 accepted non-authoritative enum/set descriptor");

  descriptor = ValidEnum();
  descriptor.parser_independent = false;
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::descriptor_parser_dependent,
                "EDR-028 accepted parser-dependent enum/set descriptor");
}

void TestShapeFailures() {
  auto descriptor = ValidEnum();
  descriptor.representation_kind =
      static_cast<engine::EnumSetRepresentationKind>(0xff);
  RequireStatus(
      descriptor,
      engine::EnumSetRepresentationStatus::representation_kind_invalid,
      "EDR-028 accepted invalid representation kind");

  descriptor = ValidEnum();
  descriptor.storage_kind = static_cast<engine::EnumSetStorageKind>(0xff);
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::storage_kind_invalid,
                "EDR-028 accepted invalid storage kind");

  descriptor = ValidEnum();
  descriptor.unknown_label_policy =
      static_cast<engine::EnumSetUnknownLabelPolicy>(0xff);
  RequireStatus(
      descriptor,
      engine::EnumSetRepresentationStatus::unknown_label_policy_invalid,
      "EDR-028 accepted invalid unknown label policy");

  descriptor = ValidEnum();
  descriptor.labels.clear();
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::labels_required,
                "EDR-028 accepted enum/set without labels");

  descriptor = ValidEnum();
  descriptor.labels.assign(engine::kEnumSetRepresentationMaxLabels + 1,
                           Label(0x60, 0, "too_many"));
  for (std::size_t index = 0; index < descriptor.labels.size(); ++index) {
    descriptor.labels[index].label_uuid = Uuid(static_cast<std::uint8_t>(index));
    descriptor.labels[index].ordinal = static_cast<std::uint32_t>(index);
    descriptor.labels[index].stable_name = "label_" + std::to_string(index);
    descriptor.labels[index].canonical_rendering =
        descriptor.labels[index].stable_name;
  }
  RequireStatus(
      descriptor,
      engine::EnumSetRepresentationStatus::label_count_exceeds_limit,
      "EDR-028 accepted too many labels");

  descriptor = ValidSet();
  descriptor.labels.assign(engine::kEnumSetBitsetStorageMaxLabels + 1,
                           Label(0x70, 0, "bit"));
  for (std::size_t index = 0; index < descriptor.labels.size(); ++index) {
    descriptor.labels[index].label_uuid =
        Uuid(static_cast<std::uint8_t>(0x70 + index));
    descriptor.labels[index].ordinal = static_cast<std::uint32_t>(index);
    descriptor.labels[index].stable_name = "bit_" + std::to_string(index);
    descriptor.labels[index].canonical_rendering =
        descriptor.labels[index].stable_name;
  }
  RequireStatus(
      descriptor,
      engine::EnumSetRepresentationStatus::bitset_label_count_exceeds_limit,
      "EDR-028 accepted oversized bitset representation");

  descriptor = ValidEnum();
  descriptor.storage_kind = engine::EnumSetStorageKind::bitset;
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::enum_storage_kind_invalid,
                "EDR-028 accepted bitset storage for single enum");

  descriptor = ValidSet();
  descriptor.storage_kind = engine::EnumSetStorageKind::ordinal;
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::set_storage_kind_invalid,
                "EDR-028 accepted ordinal storage for set");

  descriptor = ValidSet();
  descriptor.ordered = false;
  descriptor.storage_kind = engine::EnumSetStorageKind::bitset;
  RequireStatus(
      descriptor,
      engine::EnumSetRepresentationStatus::unordered_set_requires_list_storage,
      "EDR-028 accepted unordered set without list storage");
}

void TestLabelFailures() {
  auto descriptor = ValidEnum();
  descriptor.labels.front().label_uuid = {};
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::label_uuid_required,
                "EDR-028 accepted label without UUID");

  descriptor = ValidEnum();
  descriptor.labels.front().stable_name.clear();
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::label_stable_name_required,
                "EDR-028 accepted label without stable name");

  descriptor = ValidEnum();
  descriptor.labels.front().canonical_rendering.clear();
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::label_rendering_required,
                "EDR-028 accepted label without rendering");

  descriptor = ValidEnum();
  descriptor.labels.back().ordinal = 7;
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::label_ordinal_mismatch,
                "EDR-028 accepted non-contiguous label ordinal");

  descriptor = ValidEnum();
  descriptor.labels.back().label_uuid = descriptor.labels.front().label_uuid;
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::label_uuid_duplicate,
                "EDR-028 accepted duplicate label UUID");

  descriptor = ValidEnum();
  descriptor.labels.back().stable_name = descriptor.labels.front().stable_name;
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::label_name_duplicate,
                "EDR-028 accepted duplicate label name");

  descriptor = ValidEnum();
  descriptor.labels.front().aliases = {"active"};
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::label_name_duplicate,
                "EDR-028 accepted alias colliding with label name");

  descriptor = ValidEnum();
  descriptor.labels.front().aliases = {""};
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::label_alias_duplicate,
                "EDR-028 accepted empty label alias");
}

void TestDonorAndUnknownPolicyFailures() {
  auto descriptor = ValidEnum();
  descriptor.storage_kind = engine::EnumSetStorageKind::donor_native;
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::donor_profile_uuid_required,
                "EDR-028 accepted donor rendering without profile UUID");

  descriptor = ValidEnum();
  descriptor.storage_kind = engine::EnumSetStorageKind::donor_native;
  descriptor.donor_profile_uuid = Uuid(0x90);
  descriptor.labels.front().donor_rendering_name.clear();
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::donor_rendering_required,
                "EDR-028 accepted donor rendering without label names");

  descriptor = ValidEnum();
  descriptor.unknown_label_policy =
      engine::EnumSetUnknownLabelPolicy::map_to_unknown_label;
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::unknown_label_uuid_required,
                "EDR-028 accepted unknown-label mapping without UUID");

  descriptor = ValidEnum();
  descriptor.unknown_label_policy =
      engine::EnumSetUnknownLabelPolicy::map_to_unknown_label;
  descriptor.unknown_label_uuid = Uuid(0x91);
  RequireStatus(descriptor,
                engine::EnumSetRepresentationStatus::unknown_label_uuid_not_found,
                "EDR-028 accepted unknown-label UUID outside label table");
}

}  // namespace

int main() {
  TestValidProfiles();
  TestIdentityFailures();
  TestShapeFailures();
  TestLabelFailures();
  TestDonorAndUnknownPolicyFailures();
  return EXIT_SUCCESS;
}
