// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_storage_identity.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <source_location>
#include <utility>

namespace allocation_probe {
thread_local bool enabled = false;
thread_local bool failure_triggered = false;
thread_local std::size_t allocation_index = 0;
thread_local std::size_t fail_at = std::numeric_limits<std::size_t>::max();
}

void* operator new(std::size_t bytes) {
  if (allocation_probe::enabled) {
    const auto index = allocation_probe::allocation_index++;
    if (index == allocation_probe::fail_at) {
      allocation_probe::enabled = false;
      allocation_probe::failure_triggered = true;
      throw std::bad_alloc();
    }
  }
  if (void* value = std::malloc(bytes == 0 ? 1 : bytes)) return value;
  throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;

int main() try {
  auto require = [](bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error("storage binding oracle failed at line " + std::to_string(where.line()));
  };
  const p::Uuid int64_descriptor{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x11}};
  const p::Uuid int64_type{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x12}};
  const p::Uuid blob_descriptor{{0xf4,0x01,0,0,0x62,0x6c,0x7f,0x62,0x80,0,0,0,0,0,0,0}};
  const p::Uuid date_descriptor{{0x90,0x01,0,0,0x64,0x61,0x74,0x65,0x80,0,0,0,0,0,0,0}};
  const p::Uuid date_type{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x1c}};
  const p::Uuid date_codec{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x1d}};
  const p::Uuid time_descriptor{{0x91,0x01,0,0,0x74,0x69,0x7d,0x65,0x80,0,0,0,0,0,0,0}};
  const p::Uuid time_type{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x1e}};
  const p::Uuid time_codec{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x1f}};
  const p::Uuid timestamp_descriptor{{0x92,0x01,0,0,0x74,0x69,0x7d,0x65,0xb3,0x74,0x61,0x6d,0x70,0,0,0}};
  const p::Uuid timestamp_type{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x20}};
  const p::Uuid historical_timestamp_codec{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x21}};
  const p::Uuid current_timestamp_codec{{0x01,0xa1,0x04,0xf5,0xfb,0x16,0x75,0,0x93,0xe8,0x4a,0x99,0xc8,0x6d,0x11,0x30}};
  const p::Uuid unknown{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xff,0xff}};
  dt::DatatypeStorageIdentityV1 out;
  require(dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,int64_descriptor,1,&out));
  require(out.descriptor_uuid==int64_descriptor && out.descriptor_generation==1 &&
          out.type_uuid==int64_type && out.type_id==dt::CanonicalTypeId::int64);
  require(dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,blob_descriptor,1,&out));
  require(out.descriptor_uuid==blob_descriptor && out.descriptor_generation==1 &&
          out.type_uuid==blob_descriptor && out.type_id==dt::CanonicalTypeId::blob);
  // Storage recognition must not add an unimplemented literal codec.
  require(!dt::LookupDatatypeTypeCodecIdentityV1(dt::kDatatypeCohortV4,4,4,blob_descriptor,1).ok);
  const auto prior = out;
  unsigned rejected = 0;
  for (const auto& descriptor : {int64_descriptor, blob_descriptor}) {
    for (const auto generation : {0ULL, 2ULL, 5ULL, ~0ULL}) {
      require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,descriptor,generation,&out));
      ++rejected;
    }
    for (const auto generation : {0ULL, 1ULL, 3ULL, 5ULL, ~0ULL}) {
      require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,generation,4,descriptor,1,&out));
      require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,generation,descriptor,1,&out));
      rejected += 2;
    }
    for (const auto& snapshot : {p::Uuid{}, unknown, dt::kDatatypeCohortV3}) {
      require(!dt::LookupDatatypeStorageIdentityV1(snapshot,4,4,descriptor,1,&out));
      ++rejected;
    }
    require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,descriptor,1,nullptr));
    ++rejected;
  }
  for (const auto& descriptor : {p::Uuid{}, unknown}) {
    require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,descriptor,1,&out));
    ++rejected;
  }
  require(out.descriptor_uuid==prior.descriptor_uuid && out.descriptor_generation==prior.descriptor_generation &&
          out.type_uuid==prior.type_uuid && out.type_id==prior.type_id);
  // Immutable predecessor codec receipts remain exact, not upgraded implicitly.
  require(dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV3,3,3,int64_descriptor,1,&out));
  require(out.type_uuid==int64_type && out.type_id==dt::CanonicalTypeId::int64);
  require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV3,3,3,blob_descriptor,1,&out));
  // D704-D709 remain exact historical date storage identities. D710 is the
  // current policy-bearing V3 receipt for semantic publication.
  dt::DatatypeStorageIdentityV3 out_v3;
  // Every predecessor retains its exact storage-only BLOB identity. No row
  // acquires the D711 codec merely because the current catalog advanced.
  const p::Uuid current_blob_descriptor{{0x01,0x6f,0xd1,0xd3,0x0d,0xaf,0x59,0x67,
                                        0xb4,0xd7,0x07,0xfe,0x85,0x9a,0x41,0x8e}};
  for (unsigned generation = 4; generation <= 10; ++generation) {
    const p::Uuid snapshots[] = {dt::kDatatypeCohortV4, dt::kDatatypeCohortV5,
        dt::kDatatypeCohortV6, dt::kDatatypeCohortV7, dt::kDatatypeCohortV8,
        dt::kDatatypeCohortV9, dt::kDatatypeCohortV10};
    const auto& snapshot = snapshots[generation - 4];
    require(dt::LookupDatatypeStorageIdentityV3(snapshot, generation, generation,
                                                blob_descriptor, 1, &out_v3));
    require(out_v3.descriptor_uuid == blob_descriptor && out_v3.descriptor_generation == 1 &&
            out_v3.type_uuid == blob_descriptor && out_v3.type_id == dt::CanonicalTypeId::blob &&
            !out_v3.codec);
    const auto unchanged = [&] {
      require(out_v3.descriptor_uuid == blob_descriptor && out_v3.descriptor_generation == 1 &&
              out_v3.type_uuid == blob_descriptor && out_v3.type_id == dt::CanonicalTypeId::blob &&
              !out_v3.codec);
    };
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto changed = blob_descriptor;
      changed.bytes[bit / 8] ^= static_cast<p::byte>(1u << (bit % 8));
      require(!dt::LookupDatatypeStorageIdentityV3(snapshot, generation, generation,
                                                   changed, 1, &out_v3));
      unchanged();
    }
    require(!dt::LookupDatatypeStorageIdentityV3(snapshot, generation, generation,
                                                 current_blob_descriptor, 1, &out_v3));
    unchanged();
    require(!dt::LookupDatatypeStorageIdentityV3(snapshot, generation, generation,
                                                 blob_descriptor, 2, &out_v3));
    unchanged();
  }
  require(!dt::LookupDatatypeStorageIdentityV3(dt::kDatatypeCohortV11,11,11,
                                               blob_descriptor,1,&out_v3));
  require(out_v3.descriptor_uuid == blob_descriptor && out_v3.descriptor_generation == 1 &&
          out_v3.type_uuid == blob_descriptor && out_v3.type_id == dt::CanonicalTypeId::blob && !out_v3.codec);
  const p::Uuid current_blob_type{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7b,0x29,
                                  0xb6,0x79,0x2a,0xb3,0x75,0x5b,0x37,0xd2}};
  require(dt::LookupDatatypeStorageIdentityV3(dt::kDatatypeCohortV11,11,11,
                                              current_blob_descriptor,1,&out_v3));
  require(out_v3.descriptor_uuid == current_blob_descriptor && out_v3.descriptor_generation == 1 &&
          out_v3.type_uuid == current_blob_type && out_v3.type_id == dt::CanonicalTypeId::blob &&
          out_v3.codec && dt::IsExactCanonicalBlobTypeCodecIdentityV3(*out_v3.codec));
  for (unsigned generation = 4; generation <= 8; ++generation) {
    auto snapshot = dt::kDatatypeCohortV8;
    snapshot.bytes.back() = static_cast<p::byte>(generation);
    require(dt::LookupDatatypeStorageIdentityV3(
        snapshot, generation, generation, date_descriptor, 1, &out_v3));
    require(out_v3.descriptor_uuid == date_descriptor &&
            out_v3.type_uuid == date_type &&
            out_v3.type_id == dt::CanonicalTypeId::date && out_v3.codec &&
            out_v3.codec->legacy_fields.codec_uuid == date_codec);
  }
  require(dt::LookupDatatypeStorageIdentityV3(
      dt::kDatatypeCohortV10,10,10,date_descriptor,1,&out_v3));
  require(out_v3.descriptor_uuid == date_descriptor &&
          out_v3.type_uuid == date_type &&
          out_v3.type_id == dt::CanonicalTypeId::date && out_v3.codec &&
          out_v3.codec->legacy_fields.codec_uuid == date_codec &&
          dt::IsExactCanonicalDateTypeCodecIdentityV3(*out_v3.codec));
  require(!dt::LookupDatatypeStorageIdentityV1(
      dt::kDatatypeCohortV10,10,10,date_descriptor,1,&out));

  // Measure the lookup copy, then fail the first allocation in the following
  // V3-to-V1 projection. The storage output must remain entirely unchanged.
  allocation_probe::allocation_index = 0;
  allocation_probe::fail_at = std::numeric_limits<std::size_t>::max();
  allocation_probe::failure_triggered = false;
  allocation_probe::enabled = true;
  const auto measured_lookup = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV8,8,8,date_descriptor,1);
  allocation_probe::enabled = false;
  require(measured_lookup.ok && allocation_probe::allocation_index > 0);
  const auto lookup_allocation_count = allocation_probe::allocation_index;

  out = {unknown, 99, unknown, dt::CanonicalTypeId::unknown, std::nullopt};
  allocation_probe::allocation_index = 0;
  allocation_probe::fail_at = lookup_allocation_count;
  allocation_probe::failure_triggered = false;
  allocation_probe::enabled = true;
  const bool projection_failure_published =
      dt::LookupDatatypeStorageIdentityV1(
          dt::kDatatypeCohortV8,8,8,date_descriptor,1,&out);
  allocation_probe::enabled = false;
  require(allocation_probe::failure_triggered &&
          !projection_failure_published && out.descriptor_uuid == unknown &&
          out.descriptor_generation == 99 && out.type_uuid == unknown &&
          out.type_id == dt::CanonicalTypeId::unknown && !out.codec);

  require(dt::LookupDatatypeStorageIdentityV1(
      dt::kDatatypeCohortV8,8,8,date_descriptor,1,&out));
  require(out.descriptor_uuid == date_descriptor && out.type_uuid == date_type &&
          out.type_id == dt::CanonicalTypeId::date && out.codec &&
          out.codec->codec_uuid == date_codec);
  const auto current_date = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV10,10,10,date_descriptor,1);
  require(current_date.ok &&
          dt::IsExactCanonicalDateTypeCodecIdentityV3(current_date.row));
  const auto historical_date = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV8,8,8,date_descriptor,1);
  require(historical_date.ok &&
          !dt::IsExactCanonicalDateTypeCodecIdentityV3(historical_date.row));
  require(dt::LookupDatatypeStorageIdentityV3(
      dt::kDatatypeCohortV10,10,10,time_descriptor,1,&out_v3));
  require(out_v3.descriptor_uuid == time_descriptor &&
          out_v3.type_uuid == time_type &&
          out_v3.type_id == dt::CanonicalTypeId::time && out_v3.codec &&
          out_v3.codec->legacy_fields.codec_uuid == time_codec &&
          dt::IsExactCanonicalTimeTypeCodecIdentityV3(*out_v3.codec));
  const auto current_time = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV10,10,10,time_descriptor,1);
  require(current_time.ok &&
          dt::IsExactCanonicalTimeTypeCodecIdentityV3(current_time.row));
  const auto historical_time = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV8,8,8,time_descriptor,1);
  require(historical_time.ok &&
          !dt::IsExactCanonicalTimeTypeCodecIdentityV3(historical_time.row));
  const auto current_timestamp = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV10,10,10,timestamp_descriptor,1);
  require(current_timestamp.ok &&
          dt::IsExactCanonicalTimestampTypeCodecIdentityV3(
              current_timestamp.row) &&
          current_timestamp.row.legacy_fields.descriptor_uuid ==
              timestamp_descriptor &&
          current_timestamp.row.legacy_fields.type_uuid == timestamp_type &&
          current_timestamp.row.legacy_fields.codec_uuid ==
              current_timestamp_codec &&
          current_timestamp.row.legacy_fields.codec_id ==
              "datatype.timestamp.civil_tuple.le.v1" &&
          current_timestamp.row.legacy_fields.canonical_byte_order ==
              "little_endian" &&
          current_timestamp.row.legacy_fields.canonical_representation ==
              "i64_local_civil_seconds_u32_nanoseconds_u32_reserved_zero" &&
          current_timestamp.row.descriptor_policy.uuid ==
              p::Uuid{{0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x0a}} &&
          current_timestamp.row.descriptor_policy.generation == 1 &&
          current_timestamp.row.canonicalization_policy.uuid ==
              p::Uuid{{0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x0b}} &&
          current_timestamp.row.canonicalization_policy.generation == 1 &&
          current_timestamp.row.ordering_policy.uuid ==
              p::Uuid{{0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x0c}} &&
          current_timestamp.row.ordering_policy.generation == 1 &&
          current_timestamp.row.hash_policy.uuid ==
              p::Uuid{{0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x0d}} &&
          current_timestamp.row.hash_policy.generation == 1 &&
          current_timestamp.row.operation_policy.uuid ==
              p::Uuid{{0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x0e}} &&
          current_timestamp.row.operation_policy.generation == 1);
  require(dt::LookupDatatypeStorageIdentityV3(
      dt::kDatatypeCohortV10,10,10,timestamp_descriptor,1,&out_v3));
  require(out_v3.descriptor_uuid == timestamp_descriptor &&
          out_v3.descriptor_generation == 1 &&
          out_v3.type_uuid == timestamp_type &&
          out_v3.type_id == dt::CanonicalTypeId::timestamp && out_v3.codec &&
          out_v3.codec->legacy_fields.codec_uuid == current_timestamp_codec &&
          dt::IsExactCanonicalTimestampTypeCodecIdentityV3(*out_v3.codec));
  require(!dt::LookupDatatypeStorageIdentityV1(
      dt::kDatatypeCohortV10,10,10,timestamp_descriptor,1,&out));

  for (unsigned generation = 4; generation <= 8; ++generation) {
    auto snapshot = dt::kDatatypeCohortV8;
    snapshot.bytes.back() = static_cast<p::byte>(generation);
    const auto historical_timestamp =
        dt::LookupDatatypeTypeCodecIdentityV3(
            snapshot,generation,generation,timestamp_descriptor,1);
    require(historical_timestamp.ok &&
            historical_timestamp.row.legacy_fields.type_uuid ==
                timestamp_type &&
            historical_timestamp.row.legacy_fields.codec_uuid ==
                historical_timestamp_codec &&
            historical_timestamp.row.legacy_fields.codec_id ==
                "datatype.timestamp.utc_tuple.le.v1" &&
            !dt::IsExactCanonicalTimestampTypeCodecIdentityV3(
                historical_timestamp.row));
    require(dt::LookupDatatypeStorageIdentityV3(
        snapshot,generation,generation,timestamp_descriptor,1,&out_v3));
    require(out_v3.type_uuid == timestamp_type && out_v3.codec &&
            out_v3.codec->legacy_fields.codec_uuid ==
                historical_timestamp_codec);
  }

  const auto current_timestamp_storage = out_v3;
  for (const auto& receipt :
       {std::pair{dt::kDatatypeCohortV10, std::pair{9ULL,10ULL}},
        std::pair{dt::kDatatypeCohortV10, std::pair{10ULL,9ULL}},
        std::pair{dt::kDatatypeCohortV9, std::pair{10ULL,10ULL}},
        std::pair{dt::kDatatypeCohortV9, std::pair{9ULL,10ULL}},
        std::pair{dt::kDatatypeCohortV9, std::pair{10ULL,9ULL}},
        std::pair{dt::kDatatypeCohortV10, std::pair{9ULL,9ULL}}}) {
    require(!dt::LookupDatatypeStorageIdentityV3(
        receipt.first,receipt.second.first,receipt.second.second,
        timestamp_descriptor,1,&out_v3));
    require(out_v3.descriptor_uuid ==
                current_timestamp_storage.descriptor_uuid &&
            out_v3.descriptor_generation ==
                current_timestamp_storage.descriptor_generation &&
            out_v3.type_uuid == current_timestamp_storage.type_uuid &&
            out_v3.type_id == current_timestamp_storage.type_id &&
            out_v3.codec.has_value() ==
                current_timestamp_storage.codec.has_value() &&
            out_v3.codec->legacy_fields.codec_uuid ==
                current_timestamp_storage.codec->legacy_fields.codec_uuid);
  }

  auto relabelled_historical = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV8,8,8,timestamp_descriptor,1).row;
  relabelled_historical.legacy_fields.codec_id =
      "datatype.timestamp.civil_tuple.le.v1";
  require(!dt::IsExactCanonicalTimestampTypeCodecIdentityV3(
      relabelled_historical));
  relabelled_historical.legacy_fields.catalog_snapshot_uuid =
      dt::kDatatypeCohortV10;
  relabelled_historical.legacy_fields.catalog_generation = 10;
  relabelled_historical.legacy_fields.registry_generation = 10;
  require(!dt::IsExactCanonicalTimestampTypeCodecIdentityV3(
      relabelled_historical));
  // Storage-only manifest identities remain available under the current
  // admitted receipt without acquiring a fabricated value codec.
  require(dt::LookupDatatypeStorageIdentityV3(
      dt::kDatatypeCohortV10,10,10,blob_descriptor,1,&out_v3));
  require(out_v3.type_uuid == blob_descriptor && !out_v3.codec);
  std::cout << "datatype_storage_identity_conformance=passed rejected=" << rejected+1 << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
