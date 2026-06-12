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
  descriptor.descriptor_epoch = 25;
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

engine::ExecutionTypeDescriptor RootDescriptor(engine::Uuid domain_uuid) {
  auto descriptor = TypeDescriptor(0x20, "edr025.domain.root");
  descriptor.domain_uuid = domain_uuid;
  descriptor.domain_stack.push_back(domain_uuid);
  descriptor.modifier_flags |=
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_stack);
  return descriptor;
}

engine::DomainElementPathSegment Segment(
    engine::DomainElementPathSegmentKind kind,
    std::string_view token,
    engine::Uuid element_descriptor_uuid) {
  engine::DomainElementPathSegment segment;
  segment.segment_kind = kind;
  segment.canonical_token = std::string(token);
  segment.element_descriptor_uuid = element_descriptor_uuid;

  switch (kind) {
    case engine::DomainElementPathSegmentKind::field_uuid:
      segment.field_uuid = Uuid(0x40);
      break;
    case engine::DomainElementPathSegmentKind::field_ordinal:
      segment.field_ordinal = 3;
      segment.field_ordinal_present = true;
      break;
    case engine::DomainElementPathSegmentKind::array_index:
    case engine::DomainElementPathSegmentKind::list_index:
      segment.ordinal_index = 0;
      segment.ordinal_index_present = true;
      break;
    case engine::DomainElementPathSegmentKind::map_key:
      segment.key_descriptor = TypeDescriptor(0x50, "edr025.map.key");
      segment.key_payload = {'k'};
      break;
    case engine::DomainElementPathSegmentKind::variant_tag:
      segment.variant_tag_uuid = Uuid(0x60);
      break;
    case engine::DomainElementPathSegmentKind::range_lower_bound:
    case engine::DomainElementPathSegmentKind::range_upper_bound:
      break;
    case engine::DomainElementPathSegmentKind::set_member:
      segment.key_descriptor = TypeDescriptor(0x70, "edr025.set.member");
      segment.key_payload = {'m'};
      break;
    case engine::DomainElementPathSegmentKind::opaque_accessor:
      segment.opaque_accessor_uuid = Uuid(0x80);
      break;
    case engine::DomainElementPathSegmentKind::reserved_document_pointer:
      segment.reserved_document_pointer = "/profile/name";
      break;
  }
  return segment;
}

engine::DomainElementPath ValidPath(
    engine::DomainElementPathSegmentKind kind =
        engine::DomainElementPathSegmentKind::field_uuid) {
  engine::DomainElementPath path;
  path.path_uuid = Uuid(0x10);
  path.domain_uuid = Uuid(0x11);
  path.path_descriptor_uuid = Uuid(0x12);
  path.element_descriptor_uuid = Uuid(0x13);
  path.descriptor_epoch = 25;
  path.root_descriptor = RootDescriptor(path.domain_uuid);
  path.element_descriptor = TypeDescriptor(0x13, "edr025.element");
  path.segments.push_back(
      Segment(kind, "segment", path.element_descriptor_uuid));
  path.canonical_path = "/segment";
  return path;
}

void RequireStatus(const engine::DomainElementPath& path,
                   engine::DomainElementPathStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateDomainElementPath(path);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-025 domain element path status mismatch");
}

void RequireDescriptorStatus(const engine::DomainElementPath& path,
                             engine::DomainElementPathStatus expected,
                             engine::ExecutionDataPacketStatus descriptor_status,
                             std::string_view message) {
  const auto result = engine::ValidateDomainElementPath(path);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-025 domain element path status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "EDR-025 nested descriptor status mismatch");
}

void TestValidSegmentProfiles() {
  const engine::DomainElementPathSegmentKind kinds[] = {
      engine::DomainElementPathSegmentKind::field_uuid,
      engine::DomainElementPathSegmentKind::field_ordinal,
      engine::DomainElementPathSegmentKind::array_index,
      engine::DomainElementPathSegmentKind::list_index,
      engine::DomainElementPathSegmentKind::map_key,
      engine::DomainElementPathSegmentKind::variant_tag,
      engine::DomainElementPathSegmentKind::range_lower_bound,
      engine::DomainElementPathSegmentKind::range_upper_bound,
      engine::DomainElementPathSegmentKind::set_member,
      engine::DomainElementPathSegmentKind::opaque_accessor,
      engine::DomainElementPathSegmentKind::reserved_document_pointer};
  for (const auto kind : kinds) {
    Require(engine::ValidateDomainElementPath(ValidPath(kind)).ok(),
            "EDR-025 rejected valid domain element path segment kind");
  }

  auto path = ValidPath();
  const auto final_descriptor_uuid = Uuid(0x91);
  path.element_descriptor_uuid = final_descriptor_uuid;
  path.element_descriptor = TypeDescriptor(0x91, "edr025.nested.element");
  path.segments.clear();
  path.segments.push_back(
      Segment(engine::DomainElementPathSegmentKind::field_uuid, "field",
              Uuid(0x90)));
  path.segments.push_back(
      Segment(engine::DomainElementPathSegmentKind::array_index, "array0",
              final_descriptor_uuid));
  path.canonical_path = "/field/array0";
  Require(engine::ValidateDomainElementPath(path).ok(),
          "EDR-025 rejected valid nested canonical element path");
}

void TestIdentityAndAuthorityFailures() {
  auto path = ValidPath();
  path.path_uuid = {};
  RequireStatus(path, engine::DomainElementPathStatus::path_uuid_required,
                "EDR-025 accepted path without UUID");

  path = ValidPath();
  path.domain_uuid = {};
  RequireStatus(path, engine::DomainElementPathStatus::domain_uuid_required,
                "EDR-025 accepted path without domain UUID");

  path = ValidPath();
  path.path_descriptor_uuid = {};
  RequireStatus(
      path, engine::DomainElementPathStatus::path_descriptor_uuid_required,
      "EDR-025 accepted path without path descriptor UUID");

  path = ValidPath();
  path.element_descriptor_uuid = {};
  RequireStatus(
      path, engine::DomainElementPathStatus::element_descriptor_uuid_required,
      "EDR-025 accepted path without element descriptor UUID");

  path = ValidPath();
  path.descriptor_epoch = 0;
  RequireStatus(path, engine::DomainElementPathStatus::descriptor_epoch_required,
                "EDR-025 accepted path without descriptor epoch");

  path = ValidPath();
  path.canonical_path.clear();
  RequireStatus(path, engine::DomainElementPathStatus::canonical_path_required,
                "EDR-025 accepted path without canonical path");

  path = ValidPath();
  path.descriptor_authoritative = false;
  RequireStatus(path,
                engine::DomainElementPathStatus::descriptor_not_authoritative,
                "EDR-025 accepted non-authoritative path descriptor");

  path = ValidPath();
  path.parser_independent = false;
  RequireStatus(path,
                engine::DomainElementPathStatus::descriptor_parser_dependent,
                "EDR-025 accepted parser-dependent path descriptor");
}

void TestDescriptorFailures() {
  auto path = ValidPath();
  path.root_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      path, engine::DomainElementPathStatus::root_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-025 accepted invalid root descriptor");

  path = ValidPath();
  path.root_descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid);
  RequireStatus(
      path,
      engine::DomainElementPathStatus::root_descriptor_domain_flag_required,
      "EDR-025 accepted root descriptor without domain flag");

  path = ValidPath();
  path.root_descriptor.domain_uuid = Uuid(0x14);
  RequireStatus(
      path,
      engine::DomainElementPathStatus::root_descriptor_domain_uuid_mismatch,
      "EDR-025 accepted root descriptor with mismatched domain UUID");

  path = ValidPath();
  path.element_descriptor.descriptor_epoch = 0;
  RequireDescriptorStatus(
      path, engine::DomainElementPathStatus::element_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
      "EDR-025 accepted invalid element descriptor");

  path = ValidPath();
  path.element_descriptor.descriptor_uuid = Uuid(0x15);
  RequireStatus(
      path,
      engine::DomainElementPathStatus::element_descriptor_uuid_mismatch,
      "EDR-025 accepted mismatched element descriptor UUID");
}

void TestSegmentShapeFailures() {
  auto path = ValidPath();
  path.segments.clear();
  RequireStatus(path, engine::DomainElementPathStatus::segments_required,
                "EDR-025 accepted path without segments");

  path = ValidPath();
  path.segments.assign(engine::kDomainElementPathMaxSegments + 1,
                       Segment(engine::DomainElementPathSegmentKind::field_uuid,
                               "segment", path.element_descriptor_uuid));
  RequireStatus(path,
                engine::DomainElementPathStatus::segment_count_exceeds_limit,
                "EDR-025 accepted too many path segments");

  path = ValidPath();
  path.segments.front().segment_kind =
      static_cast<engine::DomainElementPathSegmentKind>(0xff);
  RequireStatus(path, engine::DomainElementPathStatus::segment_kind_invalid,
                "EDR-025 accepted invalid segment kind");

  path = ValidPath();
  path.segments.front().canonical_token.clear();
  RequireStatus(
      path, engine::DomainElementPathStatus::segment_canonical_token_required,
      "EDR-025 accepted segment without canonical token");

  path = ValidPath();
  path.segments.front().element_descriptor_uuid = {};
  RequireStatus(
      path,
      engine::DomainElementPathStatus::segment_element_descriptor_uuid_required,
      "EDR-025 accepted segment without element descriptor UUID");

  path = ValidPath(engine::DomainElementPathSegmentKind::field_uuid);
  path.segments.front().field_uuid = {};
  RequireStatus(path, engine::DomainElementPathStatus::field_uuid_required,
                "EDR-025 accepted field UUID path without field UUID");

  path = ValidPath(engine::DomainElementPathSegmentKind::field_ordinal);
  path.segments.front().field_ordinal_present = false;
  RequireStatus(path, engine::DomainElementPathStatus::field_ordinal_required,
                "EDR-025 accepted field ordinal path without ordinal evidence");

  path = ValidPath(engine::DomainElementPathSegmentKind::array_index);
  path.segments.front().ordinal_index_present = false;
  RequireStatus(path, engine::DomainElementPathStatus::ordinal_index_required,
                "EDR-025 accepted array path without index evidence");

  path = ValidPath(engine::DomainElementPathSegmentKind::map_key);
  path.segments.front().key_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      path, engine::DomainElementPathStatus::map_key_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-025 accepted invalid map key descriptor");

  path = ValidPath(engine::DomainElementPathSegmentKind::map_key);
  path.segments.front().key_payload.clear();
  RequireStatus(path, engine::DomainElementPathStatus::map_key_payload_required,
                "EDR-025 accepted map key path without key payload");

  path = ValidPath(engine::DomainElementPathSegmentKind::variant_tag);
  path.segments.front().variant_tag_uuid = {};
  RequireStatus(
      path, engine::DomainElementPathStatus::variant_tag_uuid_required,
      "EDR-025 accepted variant path without tag UUID");

  path = ValidPath(engine::DomainElementPathSegmentKind::set_member);
  path.segments.front().key_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      path, engine::DomainElementPathStatus::set_member_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-025 accepted invalid set member descriptor");

  path = ValidPath(engine::DomainElementPathSegmentKind::set_member);
  path.segments.front().key_payload.clear();
  RequireStatus(path,
                engine::DomainElementPathStatus::set_member_payload_required,
                "EDR-025 accepted set member path without member payload");

  path = ValidPath(engine::DomainElementPathSegmentKind::opaque_accessor);
  path.segments.front().opaque_accessor_uuid = {};
  RequireStatus(
      path, engine::DomainElementPathStatus::opaque_accessor_uuid_required,
      "EDR-025 accepted opaque path without accessor UUID");

  path =
      ValidPath(engine::DomainElementPathSegmentKind::reserved_document_pointer);
  path.segments.front().reserved_document_pointer.clear();
  RequireStatus(path,
                engine::DomainElementPathStatus::
                    reserved_document_pointer_required,
                "EDR-025 accepted document path without pointer");
}

void TestPathIntegrityFailures() {
  auto path = ValidPath();
  path.segments.front().element_descriptor_uuid = Uuid(0x16);
  RequireStatus(
      path,
      engine::DomainElementPathStatus::final_element_descriptor_uuid_mismatch,
      "EDR-025 accepted final segment with mismatched element descriptor UUID");

  path = ValidPath();
  path.canonical_path = "/different";
  RequireStatus(path, engine::DomainElementPathStatus::canonical_path_mismatch,
                "EDR-025 accepted unstable canonical path");

  path = ValidPath();
  path.missing_value_state = engine::ExecutionValueState::sql_null;
  RequireStatus(path,
                engine::DomainElementPathStatus::missing_value_state_invalid,
                "EDR-025 accepted invalid missing value state");

  path = ValidPath();
  path.null_value_state = engine::ExecutionValueState::missing;
  RequireStatus(path, engine::DomainElementPathStatus::null_value_state_invalid,
                "EDR-025 accepted invalid null value state");

  path = ValidPath();
  path.missing_distinct_from_null = false;
  RequireStatus(path, engine::DomainElementPathStatus::missing_null_not_distinct,
                "EDR-025 accepted collapsed missing/null semantics");
}

}  // namespace

int main() {
  TestValidSegmentProfiles();
  TestIdentityAndAuthorityFailures();
  TestDescriptorFailures();
  TestSegmentShapeFailures();
  TestPathIntegrityFailures();
  return EXIT_SUCCESS;
}
