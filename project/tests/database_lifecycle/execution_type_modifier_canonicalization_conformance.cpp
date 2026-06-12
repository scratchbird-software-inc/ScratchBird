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

engine::ExecutionTypeDescriptor TypeDescriptor(
    std::uint8_t seed,
    std::string_view name,
    engine::ExecutionTypeFamily family = engine::ExecutionTypeFamily::character,
    engine::ExecutionTypeWidthClass width_class =
        engine::ExecutionTypeWidthClass::variable) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 27;
  descriptor.canonical_type_id = seed;
  descriptor.family = family;
  descriptor.width_class = width_class;
  descriptor.stable_name = std::string(name);
  return descriptor;
}

engine::ExecutionTypeModifierCanonicalizationResult Canonicalize(
    const engine::ExecutionTypeDescriptor& descriptor) {
  engine::ExecutionTypeModifierCanonicalizationInput input;
  input.descriptor = descriptor;
  return engine::CanonicalizeExecutionTypeModifiers(input);
}

void RequireStatus(
    const engine::ExecutionTypeModifierCanonicalizationInput& input,
    engine::ExecutionTypeModifierCanonicalizationStatus expected,
    std::string_view message) {
  const auto result = engine::CanonicalizeExecutionTypeModifiers(input);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-027 modifier canonicalization status mismatch");
}

void RequireDescriptorStatus(
    const engine::ExecutionTypeModifierCanonicalizationInput& input,
    engine::ExecutionTypeModifierCanonicalizationStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result = engine::CanonicalizeExecutionTypeModifiers(input);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-027 modifier canonicalization status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "EDR-027 modifier nested descriptor status mismatch");
}

void TestEquivalentCanonicalKeys() {
  auto first = TypeDescriptor(0x10, "numeric-a",
                              engine::ExecutionTypeFamily::decimal,
                              engine::ExecutionTypeWidthClass::fixed);
  first.precision = 10;
  auto second = first;
  second.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::precision) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::scale);
  const auto first_result = Canonicalize(first);
  const auto second_result = Canonicalize(second);
  Require(first_result.ok(), "EDR-027 rejected valid numeric modifier");
  Require(second_result.ok(), "EDR-027 rejected equivalent numeric modifier");
  Require(first_result.canonical_form.canonical_modifier_key ==
              second_result.canonical_form.canonical_modifier_key,
          "EDR-027 produced different keys for equivalent numeric modifiers");

  auto text = TypeDescriptor(0x20, "text-a");
  text.length = 128;
  text.charset_uuid = Uuid(0x30);
  text.collation_uuid = Uuid(0x40);
  engine::ExecutionTypeModifierCanonicalizationInput text_input;
  text_input.descriptor = text;
  text_input.donor_modifiers = {{"  ZONE-MODE ", " UTF 8 "},
                                {"Unsigned_Int", " YES "}};
  auto text_equivalent = text_input;
  text_equivalent.descriptor.modifier_flags = 0xffffu;
  text_equivalent.donor_modifiers = {{"unsigned int", "yes"},
                                     {"zone_mode", "utf-8"}};
  const auto text_result =
      engine::CanonicalizeExecutionTypeModifiers(text_input);
  const auto text_equivalent_result =
      engine::CanonicalizeExecutionTypeModifiers(text_equivalent);
  Require(text_result.ok(), "EDR-027 rejected valid text modifiers");
  Require(text_equivalent_result.ok(),
          "EDR-027 rejected equivalent donor modifiers");
  Require(text_result.canonical_form.descriptor_identity_digest ==
              text_equivalent_result.canonical_form.descriptor_identity_digest,
          "EDR-027 produced different descriptor hashes for equivalent text");
}

void TestValidModifierFamilies() {
  auto temporal = TypeDescriptor(0x50, "timestamp",
                                 engine::ExecutionTypeFamily::temporal,
                                 engine::ExecutionTypeWidthClass::fixed);
  temporal.timezone_uuid = Uuid(0x51);
  Require(Canonicalize(temporal).ok(),
          "EDR-027 rejected valid temporal timezone modifier");

  auto vector = TypeDescriptor(0x60, "vector",
                               engine::ExecutionTypeFamily::vector,
                               engine::ExecutionTypeWidthClass::variable);
  vector.vector_dimensions = 768;
  Require(Canonicalize(vector).ok(),
          "EDR-027 rejected valid vector dimensions");

  auto range = TypeDescriptor(0x70, "range",
                              engine::ExecutionTypeFamily::range,
                              engine::ExecutionTypeWidthClass::variable);
  range.element_descriptor_uuid = Uuid(0x71);
  Require(Canonicalize(range).ok(),
          "EDR-027 rejected valid range subtype descriptor");

  auto array = TypeDescriptor(0x80, "array",
                              engine::ExecutionTypeFamily::structured,
                              engine::ExecutionTypeWidthClass::variable);
  array.container_rank = 2;
  array.element_descriptor_uuid = Uuid(0x81);
  Require(Canonicalize(array).ok(),
          "EDR-027 rejected valid array/container shape");

  auto domain = TypeDescriptor(0x90, "domain");
  domain.length = 64;
  domain.domain_uuid = Uuid(0x91);
  const auto domain_result = Canonicalize(domain);
  Require(domain_result.ok(), "EDR-027 rejected valid domain modifier");
  Require(domain_result.canonical_form.descriptor.domain_stack.size() == 1,
          "EDR-027 did not canonicalize missing domain stack");
  Require(engine::ExecutionDataPacketUuidEquals(
              domain_result.canonical_form.descriptor.domain_stack.front(),
              domain.domain_uuid),
          "EDR-027 canonicalized domain stack with wrong UUID");
}

void TestInvalidNumericAndLengthModifiers() {
  engine::ExecutionTypeModifierCanonicalizationInput input;
  input.descriptor = TypeDescriptor(0x01, "bad");
  input.descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-027 accepted invalid descriptor");

  input.descriptor = TypeDescriptor(0x10, "decimal",
                                    engine::ExecutionTypeFamily::decimal,
                                    engine::ExecutionTypeWidthClass::fixed);
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::precision_required,
      "EDR-027 accepted decimal without precision");

  input.descriptor.precision = 4;
  input.descriptor.scale = 8;
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          scale_exceeds_precision,
      "EDR-027 accepted scale greater than precision");

  input.descriptor = TypeDescriptor(0x20, "integer",
                                    engine::ExecutionTypeFamily::signed_integer,
                                    engine::ExecutionTypeWidthClass::fixed);
  input.descriptor.precision = 3;
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::precision_not_allowed,
      "EDR-027 accepted precision on integer");

  input.descriptor = TypeDescriptor(0x21, "integer",
                                    engine::ExecutionTypeFamily::signed_integer,
                                    engine::ExecutionTypeWidthClass::fixed);
  input.descriptor.scale = 2;
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::scale_requires_precision,
      "EDR-027 accepted scale without precision");

  input.descriptor = TypeDescriptor(0x30, "char");
  RequireStatus(input,
                engine::ExecutionTypeModifierCanonicalizationStatus::
                    length_required,
                "EDR-027 accepted character without length");

  input.descriptor = TypeDescriptor(0x31, "integer",
                                    engine::ExecutionTypeFamily::signed_integer,
                                    engine::ExecutionTypeWidthClass::fixed);
  input.descriptor.length = 16;
  RequireStatus(input,
                engine::ExecutionTypeModifierCanonicalizationStatus::
                    length_not_allowed,
                "EDR-027 accepted length on integer");
}

void TestInvalidShapeAndResourceModifiers() {
  engine::ExecutionTypeModifierCanonicalizationInput input;
  input.descriptor = TypeDescriptor(0x40, "vector",
                                    engine::ExecutionTypeFamily::vector,
                                    engine::ExecutionTypeWidthClass::variable);
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          vector_dimensions_required,
      "EDR-027 accepted vector without dimensions");

  input.descriptor = TypeDescriptor(0x41, "integer",
                                    engine::ExecutionTypeFamily::signed_integer,
                                    engine::ExecutionTypeWidthClass::fixed);
  input.descriptor.vector_dimensions = 2;
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          vector_dimensions_not_allowed,
      "EDR-027 accepted vector dimensions on scalar");

  input.descriptor = TypeDescriptor(0x50, "array",
                                    engine::ExecutionTypeFamily::structured,
                                    engine::ExecutionTypeWidthClass::variable);
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          container_rank_required,
      "EDR-027 accepted container without rank");

  input.descriptor = TypeDescriptor(0x51, "integer",
                                    engine::ExecutionTypeFamily::signed_integer,
                                    engine::ExecutionTypeWidthClass::fixed);
  input.descriptor.container_rank = 1;
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          container_rank_not_allowed,
      "EDR-027 accepted container rank on scalar");

  input.descriptor = TypeDescriptor(0x52, "array",
                                    engine::ExecutionTypeFamily::structured,
                                    engine::ExecutionTypeWidthClass::variable);
  input.descriptor.container_rank = 1;
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::array_element_required,
      "EDR-027 accepted array without element descriptor");

  input.descriptor = TypeDescriptor(0x53, "array",
                                    engine::ExecutionTypeFamily::structured,
                                    engine::ExecutionTypeWidthClass::variable);
  input.descriptor.container_rank = 1;
  input.array_element_present = true;
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          element_descriptor_uuid_required,
      "EDR-027 accepted array element proof without descriptor UUID");

  input = {};
  input.descriptor = TypeDescriptor(0x60, "range",
                                    engine::ExecutionTypeFamily::range,
                                    engine::ExecutionTypeWidthClass::variable);
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::range_subtype_required,
      "EDR-027 accepted range without subtype descriptor");

  input.descriptor = TypeDescriptor(0x61, "range",
                                    engine::ExecutionTypeFamily::range,
                                    engine::ExecutionTypeWidthClass::variable);
  input.range_subtype_present = true;
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          element_descriptor_uuid_required,
      "EDR-027 accepted range subtype proof without descriptor UUID");

  input = {};
  input.descriptor = TypeDescriptor(0x70, "text");
  input.descriptor.length = 64;
  input.descriptor.collation_uuid = Uuid(0x71);
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          collation_requires_charset,
      "EDR-027 accepted collation without charset");

  input.descriptor = TypeDescriptor(0x72, "integer",
                                    engine::ExecutionTypeFamily::signed_integer,
                                    engine::ExecutionTypeWidthClass::fixed);
  input.descriptor.charset_uuid = Uuid(0x73);
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::charset_uuid_required,
      "EDR-027 accepted charset on non-character descriptor");

  input.descriptor = TypeDescriptor(0x74, "integer",
                                    engine::ExecutionTypeFamily::signed_integer,
                                    engine::ExecutionTypeWidthClass::fixed);
  input.descriptor.collation_uuid = Uuid(0x75);
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          collation_requires_character_family,
      "EDR-027 accepted collation on non-character descriptor");

  input.descriptor = TypeDescriptor(0x80, "timestamp",
                                    engine::ExecutionTypeFamily::temporal,
                                    engine::ExecutionTypeWidthClass::fixed);
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::timezone_uuid_required,
      "EDR-027 accepted temporal descriptor without timezone UUID");

  input.descriptor = TypeDescriptor(0x81, "integer",
                                    engine::ExecutionTypeFamily::signed_integer,
                                    engine::ExecutionTypeWidthClass::fixed);
  input.descriptor.timezone_uuid = Uuid(0x82);
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          timezone_requires_temporal_family,
      "EDR-027 accepted timezone on non-temporal descriptor");
}

void TestInvalidDomainAndDonorModifiers() {
  engine::ExecutionTypeModifierCanonicalizationInput input;
  input.descriptor = TypeDescriptor(0x90, "domain");
  input.descriptor.length = 64;
  input.descriptor.domain_stack.push_back(Uuid(0x91));
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          domain_stack_without_domain_uuid,
      "EDR-027 accepted domain stack without domain UUID");

  input.descriptor = TypeDescriptor(0x92, "domain");
  input.descriptor.length = 64;
  input.descriptor.domain_uuid = Uuid(0x93);
  input.descriptor.domain_stack.push_back(Uuid(0x94));
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          domain_stack_current_uuid_mismatch,
      "EDR-027 accepted mismatched current domain stack UUID");

  input = {};
  input.descriptor = TypeDescriptor(0xa0, "text");
  input.descriptor.length = 64;
  input.donor_modifiers = {{"", "value"}};
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          donor_modifier_name_required,
      "EDR-027 accepted donor modifier without name");

  input.donor_modifiers = {{"Unsigned Int", "YES"}, {"unsigned_int", "yes"}};
  RequireStatus(
      input,
      engine::ExecutionTypeModifierCanonicalizationStatus::
          donor_modifier_duplicate,
      "EDR-027 accepted duplicate canonical donor modifier");
}

}  // namespace

int main() {
  TestEquivalentCanonicalKeys();
  TestValidModifierFamilies();
  TestInvalidNumericAndLengthModifiers();
  TestInvalidShapeAndResourceModifiers();
  TestInvalidDomainAndDonorModifiers();
  return EXIT_SUCCESS;
}
