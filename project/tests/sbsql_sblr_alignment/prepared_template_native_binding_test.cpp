// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/sblr_prepared_template.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace s = scratchbird::engine::sblr;
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  using scratchbird::tests::FixtureUuid;
  a::EngineApiRequest request;
  request.target_database.uuid = FixtureUuid(1157, 1);
  request.target_object.uuid = FixtureUuid(1157, 2);
  request.target_object.uuid.bytes[10] = 0; request.target_object.uuid.bytes[15] = 255;
  request.bound_object_identity.object_uuid = request.target_object.uuid;
  a::EngineColumnDefinition column;
  column.requested_column_uuid = FixtureUuid(1157, 3);
  column.descriptor.descriptor_uuid = FixtureUuid(1157, 4);
  column.descriptor.canonical_type_name = "uuid";
  request.columns = {column};
  auto dependencies = s::DependenciesFromRequest(request);
  Check(dependencies.size() == 3);
  Check(std::count(dependencies.begin(), dependencies.end(), request.target_object.uuid) == 1);
  Check(std::count(dependencies.begin(), dependencies.end(), column.requested_column_uuid) == 1);
  auto slots = s::DescriptorSlotsFromRequest(request);
  Check(slots.size() == 1 && slots[0].stable_name == "column:0" &&
        slots[0].descriptor.descriptor_uuid == column.descriptor.descriptor_uuid);
  request.columns.clear(); request.descriptors = {column.descriptor};
  slots = s::DescriptorSlotsFromRequest(request);
  Check(slots.size() == 1 && slots[0].stable_name == "descriptor:0" &&
        slots[0].descriptor.descriptor_uuid == column.descriptor.descriptor_uuid);
  auto offsets = s::FieldOffsetsFromSlots(slots);
  Check(offsets.size() == 1 && offsets[0].descriptor_slot == "descriptor:0");
  Check(s::IsCanonicalUuid(request.target_object.uuid) && !s::IsCanonicalUuid({}));
  auto invalid = request.target_object.uuid; invalid.bytes[6] = 0x40;
  Check(!s::IsCanonicalUuid(invalid));
}
