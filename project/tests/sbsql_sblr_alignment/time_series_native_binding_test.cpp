// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/nosql/time_series_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  auto id = scratchbird::tests::FixtureUuid(1130, 1);
  id.bytes[9] = 0; id.bytes[15] = 0xff;
  Check(a::CanonicalTimeSeriesUuid(id));
  auto invalid = id; invalid.bytes[8] = 0;
  Check(!a::CanonicalTimeSeriesUuid(invalid));
  std::uint64_t memory = 23;
  Check(a::CheckedTimeSeriesOwnedDynamicBytes(id, &memory));
  Check(memory == 23); // binary16 is inline, already in sizeof its owner
  a::EngineBoundTimeSeriesReadResultV1 result;
  result.descriptor_uuid = id;
  const auto initial = a::BoundTimeSeriesResultLogicalMemoryBytesV1(result);
  result.provider_uuid = id;
  Check(initial == a::BoundTimeSeriesResultLogicalMemoryBytesV1(result));
  a::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = id;
  descriptor.type_uuid = scratchbird::tests::FixtureUuid(1130, 2);
  descriptor.descriptor_kind = "canonical_type_descriptor";
  descriptor.canonical_type_name = "timestamp_tz";
  a::CatalogColumnMetadata metadata;
  metadata.identities = {{"type_uuid", descriptor.type_uuid}};
  metadata.text = {{"canonical", "timestamp_tz"}, {"nullable", "false"},
                   {"timezone_profile_id", "UTC"}};
  const auto admitted = [&] { return a::ExactTimeSeriesValueDescriptor(
      descriptor, "timestamp_tz", descriptor.type_uuid, id, nullptr); };
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor));
  Check(admitted());
  metadata.text["timezone_profile_id"] = "local";
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor));
  Check(!admitted());
  metadata.text.erase("timezone_profile_id");
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor));
  Check(admitted());
  metadata.identities["type_uuid"] = id;
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor));
  Check(!admitted());
  descriptor.encoded_descriptor = "canonical=timestamp_tz;type_uuid=018f0000-0000-7000-8000-000000000001;nullable=false";
  Check(!admitted());
}
