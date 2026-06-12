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
  descriptor.descriptor_epoch = 29;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::signed_integer;
  descriptor.width_class = engine::ExecutionTypeWidthClass::fixed;
  descriptor.stable_name = std::string(name);
  descriptor.bit_width = 64;
  return descriptor;
}

engine::RangeBoundDescriptor Bound(std::string_view token, bool inclusive) {
  engine::RangeBoundDescriptor bound;
  bound.bound_kind = engine::RangeBoundKind::finite;
  bound.inclusive = inclusive;
  bound.payload.assign(token.begin(), token.end());
  bound.canonical_token = std::string(token);
  return bound;
}

engine::RangeBoundDescriptor Unbounded() {
  engine::RangeBoundDescriptor bound;
  bound.bound_kind = engine::RangeBoundKind::unbounded;
  bound.inclusive = false;
  return bound;
}

engine::RangeSegmentDescriptor Segment(std::string_view lower,
                                       bool lower_inclusive,
                                       std::string_view upper,
                                       bool upper_inclusive) {
  engine::RangeSegmentDescriptor segment;
  segment.lower_bound = Bound(lower, lower_inclusive);
  segment.upper_bound = Bound(upper, upper_inclusive);
  return segment;
}

engine::RangeRepresentationDescriptor ValidRange() {
  engine::RangeRepresentationDescriptor descriptor;
  descriptor.range_uuid = Uuid(0x10);
  descriptor.representation_descriptor_uuid = Uuid(0x11);
  descriptor.subtype_descriptor_uuid = Uuid(0x12);
  descriptor.descriptor_epoch = 29;
  descriptor.stable_name = "edr029.range";
  descriptor.subtype_descriptor = TypeDescriptor(0x12, "int64");
  descriptor.segments.push_back(Segment("0001", true, "0010", false));
  return descriptor;
}

engine::RangeRepresentationDescriptor ValidMultirange() {
  auto descriptor = ValidRange();
  descriptor.representation_kind = engine::RangeRepresentationKind::multirange;
  descriptor.stable_name = "edr029.multirange";
  descriptor.segments.clear();
  descriptor.segments.push_back(Segment("0001", true, "0010", false));
  descriptor.segments.push_back(Segment("0020", true, "0030", false));
  return descriptor;
}

void RequireStatus(const engine::RangeRepresentationDescriptor& descriptor,
                   engine::RangeRepresentationStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateRangeRepresentationDescriptor(descriptor);
  Require(!result.ok(), message);
  if (result.status != expected) {
    std::cerr << "expected=" << engine::RangeRepresentationStatusName(expected)
              << " actual="
              << engine::RangeRepresentationStatusName(result.status) << '\n';
    Fail("EDR-029 range representation status mismatch");
  }
}

void RequireDescriptorStatus(
    const engine::RangeRepresentationDescriptor& descriptor,
    engine::RangeRepresentationStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result = engine::ValidateRangeRepresentationDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-029 range representation status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "EDR-029 range subtype descriptor status mismatch");
}

void TestValidProfiles() {
  Require(engine::ValidateRangeRepresentationDescriptor(ValidRange()).ok(),
          "EDR-029 rejected valid finite range");
  Require(engine::ValidateRangeRepresentationDescriptor(ValidMultirange()).ok(),
          "EDR-029 rejected valid multirange");

  auto descriptor = ValidRange();
  descriptor.segments.front().empty = true;
  descriptor.segments.front().lower_bound = {};
  descriptor.segments.front().upper_bound = {};
  Require(engine::ValidateRangeRepresentationDescriptor(descriptor).ok(),
          "EDR-029 rejected valid empty range");

  descriptor = ValidRange();
  descriptor.segments.front().lower_bound = Unbounded();
  descriptor.segments.front().upper_bound = Bound("0100", false);
  Require(engine::ValidateRangeRepresentationDescriptor(descriptor).ok(),
          "EDR-029 rejected valid lower-unbounded range");

  descriptor = ValidRange();
  descriptor.segments.front().lower_bound = Bound("0100", true);
  descriptor.segments.front().upper_bound = Unbounded();
  Require(engine::ValidateRangeRepresentationDescriptor(descriptor).ok(),
          "EDR-029 rejected valid upper-unbounded range");
}

void TestIdentityFailures() {
  auto descriptor = ValidRange();
  descriptor.range_uuid = {};
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::range_uuid_required,
                "EDR-029 accepted range without UUID");

  descriptor = ValidRange();
  descriptor.representation_descriptor_uuid = {};
  RequireStatus(
      descriptor,
      engine::RangeRepresentationStatus::representation_descriptor_uuid_required,
      "EDR-029 accepted range without descriptor UUID");

  descriptor = ValidRange();
  descriptor.subtype_descriptor_uuid = {};
  RequireStatus(
      descriptor,
      engine::RangeRepresentationStatus::subtype_descriptor_uuid_required,
      "EDR-029 accepted range without subtype descriptor UUID");

  descriptor = ValidRange();
  descriptor.descriptor_epoch = 0;
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::descriptor_epoch_required,
                "EDR-029 accepted range without descriptor epoch");

  descriptor = ValidRange();
  descriptor.stable_name.clear();
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::stable_name_required,
                "EDR-029 accepted range without stable name");

  descriptor = ValidRange();
  descriptor.descriptor_authoritative = false;
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::descriptor_not_authoritative,
                "EDR-029 accepted non-authoritative range descriptor");

  descriptor = ValidRange();
  descriptor.parser_independent = false;
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::descriptor_parser_dependent,
                "EDR-029 accepted parser-dependent range descriptor");
}

void TestSubtypeAndShapeFailures() {
  auto descriptor = ValidRange();
  descriptor.representation_kind =
      static_cast<engine::RangeRepresentationKind>(0xff);
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::representation_kind_invalid,
                "EDR-029 accepted invalid range kind");

  descriptor = ValidRange();
  descriptor.subtype_descriptor.descriptor_uuid = {};
  RequireDescriptorStatus(
      descriptor, engine::RangeRepresentationStatus::subtype_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
      "EDR-029 accepted invalid subtype descriptor");

  descriptor = ValidRange();
  descriptor.subtype_descriptor_uuid = Uuid(0x90);
  RequireStatus(
      descriptor,
      engine::RangeRepresentationStatus::subtype_descriptor_uuid_mismatch,
      "EDR-029 accepted mismatched subtype descriptor UUID");

  descriptor = ValidRange();
  descriptor.segments.clear();
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::segments_required,
                "EDR-029 accepted range without segments");

  descriptor = ValidRange();
  descriptor.segments.assign(engine::kRangeRepresentationMaxSegments + 1,
                             Segment("0001", true, "0002", false));
  descriptor.representation_kind = engine::RangeRepresentationKind::multirange;
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::segment_count_exceeds_limit,
                "EDR-029 accepted too many range segments");

  descriptor = ValidRange();
  descriptor.segments.push_back(Segment("0020", true, "0030", false));
  RequireStatus(
      descriptor,
      engine::RangeRepresentationStatus::single_range_segment_count_invalid,
      "EDR-029 accepted multiple segments for single range");

  descriptor = ValidRange();
  descriptor.empty_distinct_from_null = false;
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::empty_range_not_distinct,
                "EDR-029 accepted empty range collapsed with null");

  descriptor = ValidRange();
  descriptor.canonicalized = false;
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::canonicalization_required,
                "EDR-029 accepted non-canonical range descriptor");
}

void TestBoundFailures() {
  auto descriptor = ValidRange();
  descriptor.segments.front().lower_bound.bound_kind =
      static_cast<engine::RangeBoundKind>(0xff);
  RequireStatus(descriptor, engine::RangeRepresentationStatus::bound_kind_invalid,
                "EDR-029 accepted invalid bound kind");

  descriptor = ValidRange();
  descriptor.segments.front().lower_bound.payload.clear();
  RequireStatus(
      descriptor,
      engine::RangeRepresentationStatus::finite_bound_payload_required,
      "EDR-029 accepted finite bound without payload");

  descriptor = ValidRange();
  descriptor.segments.front().lower_bound.canonical_token.clear();
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::finite_bound_token_required,
                "EDR-029 accepted finite bound without canonical token");

  descriptor = ValidRange();
  descriptor.segments.front().lower_bound = Unbounded();
  descriptor.segments.front().lower_bound.payload = {'x'};
  RequireStatus(
      descriptor,
      engine::RangeRepresentationStatus::unbounded_bound_payload_not_allowed,
      "EDR-029 accepted payload on unbounded range");

  descriptor = ValidRange();
  descriptor.segments.front().lower_bound = Unbounded();
  descriptor.segments.front().lower_bound.inclusive = true;
  RequireStatus(
      descriptor,
      engine::RangeRepresentationStatus::unbounded_bound_inclusive_not_allowed,
      "EDR-029 accepted inclusive unbounded range");

  descriptor = ValidRange();
  descriptor.segments.front().lower_bound.value_state =
      engine::ExecutionValueState::sql_null;
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::bound_value_state_invalid,
                "EDR-029 accepted null finite bound state");

  descriptor = ValidRange();
  descriptor.segments.front().empty = true;
  RequireStatus(
      descriptor,
      engine::RangeRepresentationStatus::empty_segment_bound_payload_not_allowed,
      "EDR-029 accepted empty segment with bound payload");
}

void TestCanonicalOrderingFailures() {
  auto descriptor = ValidRange();
  descriptor.segments.front().lower_bound = Bound("0020", true);
  descriptor.segments.front().upper_bound = Bound("0010", false);
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::segment_order_invalid,
                "EDR-029 accepted descending finite range");

  descriptor = ValidRange();
  descriptor.segments.front().lower_bound = Bound("0010", true);
  descriptor.segments.front().upper_bound = Bound("0010", true);
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::segment_order_invalid,
                "EDR-029 accepted inclusive zero-width range");

  descriptor = ValidMultirange();
  descriptor.segments[1] = Segment("0005", true, "0015", false);
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::segment_order_invalid,
                "EDR-029 accepted overlapping multirange segments");

  descriptor = ValidMultirange();
  descriptor.segments[1] = Segment("0010", true, "0020", false);
  RequireStatus(
      descriptor,
      engine::RangeRepresentationStatus::segment_overlap_or_adjacency,
      "EDR-029 accepted adjacent multirange segment without merge");

  descriptor = ValidMultirange();
  descriptor.segments.front().upper_bound = Unbounded();
  RequireStatus(descriptor,
                engine::RangeRepresentationStatus::segment_order_invalid,
                "EDR-029 accepted segment after upper-unbounded segment");
}

}  // namespace

int main() {
  TestValidProfiles();
  TestIdentityFailures();
  TestSubtypeAndShapeFailures();
  TestBoundFailures();
  TestCanonicalOrderingFailures();
  return EXIT_SUCCESS;
}
