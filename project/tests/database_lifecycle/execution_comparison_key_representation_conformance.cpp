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

engine::ExecutionTypeDescriptor Descriptor(
    std::uint8_t seed,
    std::string_view name,
    engine::ExecutionTypeFamily family) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 33;
  descriptor.canonical_type_id = seed;
  descriptor.family = family;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  descriptor.length = 128;
  return descriptor;
}

engine::ExecutionComparisonKeyDescriptor Key(
    std::uint8_t seed,
    engine::ExecutionComparisonKeyKind kind,
    engine::ExecutionTypeDescriptor descriptor) {
  engine::ExecutionComparisonKeyDescriptor key;
  key.comparison_key_uuid = Uuid(seed);
  key.type_descriptor_uuid = descriptor.descriptor_uuid;
  key.descriptor_epoch = descriptor.descriptor_epoch;
  key.stable_name = "edr033.comparison.key";
  key.key_kind = kind;
  key.type_descriptor = std::move(descriptor);
  key.canonical_payload = {0x10, 0x20, 0x30};
  return key;
}

engine::ExecutionComparisonKeyDescriptor NumericKey() {
  return Key(0x10, engine::ExecutionComparisonKeyKind::numeric,
             Descriptor(0x20, "numeric_key", engine::ExecutionTypeFamily::decimal));
}

engine::ExecutionComparisonKeyDescriptor TemporalKey() {
  auto descriptor =
      Descriptor(0x21, "temporal_key", engine::ExecutionTypeFamily::temporal);
  descriptor.timezone_uuid = Uuid(0x70);
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::timezone_uuid);
  auto key = Key(0x11, engine::ExecutionComparisonKeyKind::temporal, descriptor);
  key.timezone_uuid = descriptor.timezone_uuid;
  return key;
}

engine::ExecutionComparisonKeyDescriptor TextKey() {
  auto descriptor =
      Descriptor(0x22, "text_key", engine::ExecutionTypeFamily::character);
  descriptor.collation_uuid = Uuid(0x80);
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::collation_uuid);
  auto key =
      Key(0x12, engine::ExecutionComparisonKeyKind::text_collation, descriptor);
  key.collation_uuid = descriptor.collation_uuid;
  return key;
}

engine::ExecutionComparisonKeyDescriptor DomainKey() {
  auto descriptor =
      Descriptor(0x23, "domain_key", engine::ExecutionTypeFamily::character);
  descriptor.domain_uuid = Uuid(0x90);
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid);
  auto key = Key(0x13, engine::ExecutionComparisonKeyKind::domain, descriptor);
  key.domain_uuid = descriptor.domain_uuid;
  return key;
}

engine::ExecutionComparisonKeyDescriptor DonorKey() {
  auto key = Key(0x14, engine::ExecutionComparisonKeyKind::donor_compatible,
                 Descriptor(0x24, "donor_key",
                            engine::ExecutionTypeFamily::opaque));
  key.donor_profile_name = "edr033.donor.compat";
  key.lossy = true;
  key.requires_recheck = true;
  return key;
}

void PrintMismatch(engine::ExecutionComparisonKeyStatus expected,
                   engine::ExecutionComparisonKeyStatus actual) {
  std::cerr << "expected=" << engine::ExecutionComparisonKeyStatusName(expected)
            << " actual=" << engine::ExecutionComparisonKeyStatusName(actual)
            << '\n';
}

void RequireStatus(const engine::ExecutionComparisonKeyDescriptor& key,
                   engine::ExecutionComparisonKeyStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionComparisonKeyDescriptor(key);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-033 comparison key status mismatch");
  }
}

void RequireDescriptorStatus(
    const engine::ExecutionComparisonKeyDescriptor& key,
    engine::ExecutionComparisonKeyStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result = engine::ValidateExecutionComparisonKeyDescriptor(key);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-033 comparison key descriptor status mismatch");
  }
  Require(result.descriptor_status == descriptor_status,
          "EDR-033 comparison key nested descriptor diagnostic mismatch");
}

void TestValidComparisonKeys() {
  Require(engine::ValidateExecutionComparisonKeyDescriptor(NumericKey()).ok(),
          "EDR-033 rejected valid numeric comparison key");
  Require(engine::ValidateExecutionComparisonKeyDescriptor(TemporalKey()).ok(),
          "EDR-033 rejected valid temporal comparison key");
  Require(engine::ValidateExecutionComparisonKeyDescriptor(TextKey()).ok(),
          "EDR-033 rejected valid collation comparison key");
  Require(engine::ValidateExecutionComparisonKeyDescriptor(DomainKey()).ok(),
          "EDR-033 rejected valid domain comparison key");
  Require(engine::ValidateExecutionComparisonKeyDescriptor(DonorKey()).ok(),
          "EDR-033 rejected valid donor-compatible comparison key");

  auto null_key = NumericKey();
  null_key.value_state = engine::ExecutionValueState::sql_null;
  null_key.canonical_payload.clear();
  null_key.null_ordering = engine::ExecutionComparisonNullOrdering::nulls_first;
  Require(engine::ValidateExecutionComparisonKeyDescriptor(null_key).ok(),
          "EDR-033 rejected valid null comparison key");

  auto descending = NumericKey();
  descending.sort_direction =
      engine::ExecutionComparisonSortDirection::descending;
  Require(engine::ValidateExecutionComparisonKeyDescriptor(descending).ok(),
          "EDR-033 rejected valid descending comparison key");
}

void TestIdentityFailures() {
  auto key = NumericKey();
  key.comparison_key_uuid = {};
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::
                    comparison_key_uuid_required,
                "EDR-033 accepted comparison key without UUID");

  key = NumericKey();
  key.type_descriptor_uuid = {};
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::
                    type_descriptor_uuid_required,
                "EDR-033 accepted comparison key without type UUID");

  key = NumericKey();
  key.descriptor_epoch = 0;
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::descriptor_epoch_required,
                "EDR-033 accepted comparison key without descriptor epoch");

  key = NumericKey();
  key.stable_name.clear();
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::stable_name_required,
                "EDR-033 accepted comparison key without stable name");

  key = NumericKey();
  key.descriptor_authoritative = false;
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::descriptor_not_authoritative,
                "EDR-033 accepted non-authoritative comparison key");

  key = NumericKey();
  key.parser_independent = false;
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::descriptor_parser_dependent,
                "EDR-033 accepted parser-dependent comparison key");
}

void TestShapeFailures() {
  auto key = NumericKey();
  key.key_kind = static_cast<engine::ExecutionComparisonKeyKind>(0xff);
  RequireStatus(key, engine::ExecutionComparisonKeyStatus::key_kind_invalid,
                "EDR-033 accepted invalid comparison key kind");

  key = NumericKey();
  key.sort_direction =
      static_cast<engine::ExecutionComparisonSortDirection>(0xff);
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::sort_direction_invalid,
                "EDR-033 accepted invalid sort direction");

  key = NumericKey();
  key.null_ordering =
      static_cast<engine::ExecutionComparisonNullOrdering>(0xff);
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::null_ordering_invalid,
                "EDR-033 accepted invalid null ordering");

  key = NumericKey();
  key.type_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      key, engine::ExecutionComparisonKeyStatus::type_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-033 accepted invalid type descriptor");

  key = NumericKey();
  key.type_descriptor_uuid = Uuid(0xf0);
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::
                    type_descriptor_uuid_mismatch,
                "EDR-033 accepted mismatched type descriptor UUID");

  key = NumericKey();
  key.type_descriptor.descriptor_epoch += 1;
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::
                    type_descriptor_epoch_mismatch,
                "EDR-033 accepted mismatched type descriptor epoch");
}

void TestValueStateFailures() {
  auto key = NumericKey();
  key.value_state = engine::ExecutionValueState::missing;
  RequireStatus(key, engine::ExecutionComparisonKeyStatus::value_state_invalid,
                "EDR-033 accepted missing comparison value state");

  key = NumericKey();
  key.value_state = engine::ExecutionValueState::sql_null;
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::null_payload_not_allowed,
                "EDR-033 accepted null comparison key with payload");

  key = NumericKey();
  key.canonical_payload.clear();
  RequireStatus(key, engine::ExecutionComparisonKeyStatus::payload_required,
                "EDR-033 accepted non-null comparison key without payload");
}

void TestFamilyAndResourceFailures() {
  auto key = NumericKey();
  key.type_descriptor.family = engine::ExecutionTypeFamily::character;
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::numeric_family_required,
                "EDR-033 accepted non-numeric numeric comparison key");

  key = TemporalKey();
  key.type_descriptor.family = engine::ExecutionTypeFamily::character;
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::temporal_family_required,
                "EDR-033 accepted non-temporal temporal comparison key");

  key = TemporalKey();
  key.timezone_uuid = {};
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::timezone_uuid_required,
                "EDR-033 accepted temporal key without timezone UUID");

  key = TemporalKey();
  key.timezone_uuid = Uuid(0xf1);
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::timezone_uuid_mismatch,
                "EDR-033 accepted temporal key timezone mismatch");

  key = TextKey();
  key.type_descriptor.family = engine::ExecutionTypeFamily::binary;
  RequireStatus(key, engine::ExecutionComparisonKeyStatus::text_family_required,
                "EDR-033 accepted non-text collation key");

  key = TextKey();
  key.collation_uuid = {};
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::collation_uuid_required,
                "EDR-033 accepted collation key without collation UUID");

  key = TextKey();
  key.collation_uuid = Uuid(0xf2);
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::collation_uuid_mismatch,
                "EDR-033 accepted collation UUID mismatch");

  key = DomainKey();
  key.type_descriptor.modifier_flags = 0;
  RequireStatus(key, engine::ExecutionComparisonKeyStatus::domain_flag_required,
                "EDR-033 accepted domain key without domain flag");

  key = DomainKey();
  key.domain_uuid = {};
  RequireStatus(key, engine::ExecutionComparisonKeyStatus::domain_uuid_required,
                "EDR-033 accepted domain key without domain UUID");

  key = DomainKey();
  key.domain_uuid = Uuid(0xf3);
  RequireStatus(key, engine::ExecutionComparisonKeyStatus::domain_uuid_mismatch,
                "EDR-033 accepted domain UUID mismatch");

  key = DonorKey();
  key.donor_profile_name.clear();
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::donor_profile_required,
                "EDR-033 accepted donor key without donor profile");

  key = DonorKey();
  key.requires_recheck = false;
  RequireStatus(key,
                engine::ExecutionComparisonKeyStatus::donor_recheck_required,
                "EDR-033 accepted lossy donor key without recheck");
}

}  // namespace

int main() {
  TestValidComparisonKeys();
  TestIdentityFailures();
  TestShapeFailures();
  TestValueStateFailures();
  TestFamilyAndResourceFailures();
  return EXIT_SUCCESS;
}
