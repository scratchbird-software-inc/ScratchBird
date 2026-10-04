// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_physical_encoding.hpp"
#include "datatype_date.hpp"
#include "datatype_time.hpp"
#include "datatype_timestamp.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {

namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

dt::DatatypePhysicalValue PhysicalValue(dt::CanonicalTypeId type_id,
                                        dt::DatatypePhysicalValueState state,
                                        std::vector<platform::byte> payload) {
  dt::DatatypePhysicalValue value;
  value.type_id = type_id;
  value.state = state;
  value.payload = std::move(payload);
  return value;
}

std::vector<platform::byte> Payload(std::size_t size, platform::byte seed) {
  std::vector<platform::byte> payload(size);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<platform::byte>(seed + index);
  }
  return payload;
}

void RoundTrip(const dt::DatatypePhysicalValue& value) {
  const auto encoded = dt::EncodeDatatypePhysicalValue(value);
  if (!encoded.ok()) {
    std::cerr << encoded.diagnostic.diagnostic_code << '\n';
  }
  Require(encoded.ok(), "MDF-013 physical value did not encode");
  const auto decoded = dt::DecodeDatatypePhysicalValue(encoded.bytes.data(),
                                                       encoded.bytes.size());
  Require(decoded.ok(), "MDF-013 physical value did not decode");
  Require(decoded.value.type_id == value.type_id,
          "MDF-013 decoded type id mismatch");
  Require(decoded.value.state == value.state,
          "MDF-013 decoded value state mismatch");
  Require(decoded.value.payload == value.payload,
          "MDF-013 decoded payload mismatch");
}

void TestEveryCanonicalDatatypePhysicalRoundTrip() {
  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    // base.bit_string is context-sensitive and has no raw generic semantic
    // path. Its exact V3 receipt/profile adapter is exercised below.
    if (descriptor.type_id == dt::CanonicalTypeId::bit_string ||
        descriptor.type_id == dt::CanonicalTypeId::date ||
        descriptor.type_id == dt::CanonicalTypeId::time ||
        descriptor.type_id == dt::CanonicalTypeId::timestamp) continue;
    const auto layout = dt::LookupDatatypeStorageLayout(descriptor.type_id);
    Require(layout.ok(), "MDF-013 missing storage layout");
    RoundTrip(dt::SampleDatatypePhysicalValueForLayout(layout.layout));

    dt::DatatypePhysicalValue null_value;
    null_value.type_id = descriptor.type_id;
    null_value.state = dt::DatatypePhysicalValueState::sql_null;
    RoundTrip(null_value);
  }
}

std::shared_ptr<const dt::DateValidatedProfileHandleV3> DateProfile() {
  const auto result =
      dt::BuildCurrentDateValidatedProfileHandleV3(dt::kDatatypeCohortV9);
  Require(result.ok(), "MDF-013 current date profile did not resolve");
  return std::make_shared<const dt::DateValidatedProfileHandleV3>(
      result.profile);
}

std::shared_ptr<const dt::TimeValidatedProfileHandleV3> TimeProfile() {
  const auto result =
      dt::BuildCurrentTimeValidatedProfileHandleV3(dt::kDatatypeCohortV9);
  Require(result.ok(), "MDF-013 current time profile did not resolve");
  return std::make_shared<const dt::TimeValidatedProfileHandleV3>(
      result.profile);
}

std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> TimestampProfile() {
  const auto result =
      dt::BuildCurrentTimestampValidatedProfileHandleV3(dt::kDatatypeCohortV9);
  Require(result.ok(), "MDF-013 current timestamp profile did not resolve");
  return std::make_shared<const dt::TimestampValidatedProfileHandleV3>(
      result.profile);
}

void TestD708TemporalProfileReceiptsAreRefusedAsCurrent() {
  const auto date =
      dt::BuildCurrentDateValidatedProfileHandleV3(dt::kDatatypeCohortV8);
  const auto time =
      dt::BuildCurrentTimeValidatedProfileHandleV3(dt::kDatatypeCohortV8);
  const auto timestamp =
      dt::BuildCurrentTimestampValidatedProfileHandleV3(dt::kDatatypeCohortV8);
  Require(!date.ok() && !time.ok() && !timestamp.ok() &&
              date.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              time.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              timestamp.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "MDF-013 admitted historical d708 temporal receipt as current");
}

void TestDateStructuralBoundaryAndComposedAuthority() {
  const std::array<platform::byte, 4> epoch{{0, 0, 0, 0}};
  const dt::DatatypePhysicalValueView structural{
      dt::CanonicalTypeId::date,
      dt::DatatypePhysicalValueState::value,
      epoch.data(), epoch.size()};
  std::array<platform::byte, 28> frame{};
  const auto encoded = dt::EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      structural, frame.data(), frame.size());
  Require(encoded.ok() && encoded.bytes_written == frame.size(),
          "MDF-013 date structural SBDPV encode failed");
  const auto structural_decode =
      dt::DecodeDatatypePhysicalStructuralValueViewNoAlloc(
          frame.data(), frame.size());
  Require(structural_decode.ok() &&
              structural_decode.value.type_id == dt::CanonicalTypeId::date &&
              structural_decode.value.payload_bytes == epoch.size(),
          "MDF-013 date structural SBDPV decode failed");

  const auto raw_encode = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::date, dt::DatatypePhysicalValueState::value,
       {epoch.begin(), epoch.end()}});
  const auto raw_decode =
      dt::DecodeDatatypePhysicalValue(frame.data(), frame.size());
  Require(!raw_encode.ok() && !raw_decode.ok() &&
              raw_encode.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING" &&
              raw_decode.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "MDF-013 raw date SBDPV semantic path was not refused");

  const auto profile = DateProfile();
  const dt::DateOwnedValueV3 value{
      profile, dt::DateValueStateV3::value, 0};
  const auto composed =
      dt::EncodeDateSbdpvComposedV3(value, false);
  Require(composed.ok() && composed.bytes ==
              std::vector<platform::byte>(frame.begin(), frame.end()),
          "MDF-013 composed date SBDPV bytes differ from structural envelope");
  const auto decoded = dt::DecodeDateSbdpvComposedNoAllocV3(
      *profile, false, composed.bytes);
  Require(decoded.ok() && decoded.value.profile == profile.get() &&
              decoded.value.state == dt::DateValueStateV3::value &&
              decoded.value.day == 0,
          "MDF-013 composed date SBDPV did not preserve typed epoch");

  const auto malformed = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::date, dt::DatatypePhysicalValueState::value,
       {0, 0, 0}});
  Require(!malformed.ok() && malformed.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "MDF-013 malformed raw date component precedence mismatch");
  const auto dirty_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::date, dt::DatatypePhysicalValueState::sql_null,
       {0}});
  Require(!dirty_null.ok() && dirty_null.diagnostic.diagnostic_code ==
              "DATATYPE.NULL_STATE.INVALID",
          "MDF-013 raw date dirty NULL precedence mismatch");
}

void TestTimeStructuralBoundaryAndRawRefusal() {
  const std::array<platform::byte, 8> midnight{};
  const dt::DatatypePhysicalValueView structural{
      dt::CanonicalTypeId::time,
      dt::DatatypePhysicalValueState::value,
      midnight.data(), midnight.size()};
  std::array<platform::byte, 32> frame{};
  const auto encoded = dt::EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      structural, frame.data(), frame.size());
  Require(encoded.ok() && encoded.bytes_written == frame.size(),
          "MDF-013 time structural SBDPV encode failed");
  const auto structural_decode =
      dt::DecodeDatatypePhysicalStructuralValueViewNoAlloc(
          frame.data(), frame.size());
  Require(structural_decode.ok() &&
              structural_decode.value.type_id == dt::CanonicalTypeId::time &&
              structural_decode.value.payload_bytes == midnight.size(),
          "MDF-013 time structural SBDPV decode failed");

  const auto raw_encode = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::time, dt::DatatypePhysicalValueState::value,
       {midnight.begin(), midnight.end()}});
  const auto raw_decode =
      dt::DecodeDatatypePhysicalValue(frame.data(), frame.size());
  Require(!raw_encode.ok() && !raw_decode.ok() &&
              raw_encode.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING" &&
              raw_decode.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "MDF-013 raw time SBDPV semantic path was not refused");

  const auto profile = TimeProfile();
  const dt::TimeOwnedValueV3 value{
      profile, dt::TimeValueStateV3::value, 0};
  const auto composed = dt::EncodeTimeSbdpvComposedV3(value, false);
  Require(composed.ok() && composed.bytes ==
              std::vector<platform::byte>(frame.begin(), frame.end()),
          "MDF-013 composed time SBDPV bytes differ from structural envelope");
  const auto decoded = dt::DecodeTimeSbdpvComposedNoAllocV3(
      *profile, false, composed.bytes);
  Require(decoded.ok() && decoded.value.profile == profile.get() &&
              decoded.value.state == dt::TimeValueStateV3::value &&
              decoded.value.nanoseconds_since_midnight == 0,
          "MDF-013 composed time SBDPV did not preserve typed midnight");

  const auto malformed = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::time, dt::DatatypePhysicalValueState::value,
       {0, 0, 0, 0, 0, 0, 0}});
  Require(!malformed.ok() && malformed.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "MDF-013 malformed raw time component precedence mismatch");
  std::vector<platform::byte> out_of_range(8);
  std::uint64_t scalar = 86'400'000'000'000ull;
  for (std::size_t index = 0; index < out_of_range.size(); ++index) {
    out_of_range[index] = static_cast<platform::byte>(scalar & 0xffu);
    scalar >>= 8;
  }
  const auto invalid_range = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::time, dt::DatatypePhysicalValueState::value,
       out_of_range});
  Require(!invalid_range.ok() && invalid_range.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "MDF-013 out-of-range raw time component was admitted");
  const auto dirty_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::time, dt::DatatypePhysicalValueState::sql_null,
       {0}});
  Require(!dirty_null.ok() && dirty_null.diagnostic.diagnostic_code ==
              "DATATYPE.NULL_STATE.INVALID",
          "MDF-013 raw time dirty NULL precedence mismatch");
}

void TestTimestampStructuralBoundaryAndRawRefusal() {
  const std::array<platform::byte, 16> epoch{};
  const dt::DatatypePhysicalValueView structural{
      dt::CanonicalTypeId::timestamp,
      dt::DatatypePhysicalValueState::value,
      epoch.data(), epoch.size()};
  std::array<platform::byte, 40> frame{};
  const auto encoded = dt::EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      structural, frame.data(), frame.size());
  Require(encoded.ok() && encoded.bytes_written == frame.size(),
          "MDF-013 timestamp structural SBDPV encode failed");
  const auto structural_decode =
      dt::DecodeDatatypePhysicalStructuralValueViewNoAlloc(
          frame.data(), frame.size());
  Require(structural_decode.ok() &&
              structural_decode.value.type_id == dt::CanonicalTypeId::timestamp &&
              structural_decode.value.payload_bytes == epoch.size(),
          "MDF-013 timestamp structural SBDPV decode failed");

  const auto raw_encode = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::timestamp, dt::DatatypePhysicalValueState::value,
       {epoch.begin(), epoch.end()}});
  const auto raw_decode = dt::DecodeDatatypePhysicalValue(frame.data(), frame.size());
  Require(!raw_encode.ok() && !raw_decode.ok() &&
              raw_encode.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING" &&
              raw_decode.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "MDF-013 raw timestamp SBDPV semantic path was not refused");

  const auto profile = TimestampProfile();
  const dt::TimestampOwnedValueV3 value{
      profile, dt::TimestampValueStateV3::value, 0, 0};
  const auto composed = dt::EncodeTimestampSbdpvComposedV3(value, false);
  Require(composed.ok() && composed.bytes.size() == frame.size() &&
              std::equal(composed.bytes.begin(), composed.bytes.end(), frame.begin()),
          "MDF-013 composed timestamp SBDPV bytes differ from structural envelope");
  const auto decoded = dt::DecodeTimestampSbdpvComposedNoAllocV3(
      *profile, false, composed.bytes);
  Require(decoded.ok() && decoded.value.profile == profile.get() &&
              decoded.value.state == dt::TimestampValueStateV3::value &&
              decoded.value.civil_day == 0 &&
              decoded.value.nanoseconds_since_midnight == 0,
          "MDF-013 composed timestamp SBDPV did not preserve local-civil epoch");

  const auto malformed = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::timestamp, dt::DatatypePhysicalValueState::value,
       std::vector<platform::byte>(15)});
  Require(!malformed.ok() && malformed.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "MDF-013 malformed raw timestamp component precedence mismatch");
  std::vector<platform::byte> invalid_nanosecond(16);
  platform::StoreLittle32(invalid_nanosecond.data() + 8, 1'000'000'000u);
  const auto invalid = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::timestamp, dt::DatatypePhysicalValueState::value,
       invalid_nanosecond});
  Require(!invalid.ok() && invalid.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "MDF-013 invalid timestamp nanosecond component was admitted");
  const auto dirty_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::timestamp, dt::DatatypePhysicalValueState::sql_null,
       {0}});
  Require(!dirty_null.ok() && dirty_null.diagnostic.diagnostic_code ==
              "DATATYPE.NULL_STATE.INVALID",
          "MDF-013 raw timestamp dirty NULL precedence mismatch");
}

void TestBitStringStructuralBoundaryAndRawRefusal() {
  const std::array<platform::byte, 5> component{{1, 0, 0, 0, 0x80}};
  const dt::DatatypePhysicalValueView structural{
      dt::CanonicalTypeId::bit_string,
      dt::DatatypePhysicalValueState::value,
      component.data(), component.size()};
  std::array<platform::byte, 29> frame{};
  const auto encoded = dt::EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      structural, frame.data(), frame.size());
  Require(encoded.ok() && encoded.bytes_written == frame.size(),
          "MDF-013 bit structural SBDPV encode failed");
  const auto decoded = dt::DecodeDatatypePhysicalStructuralValueViewNoAlloc(
      frame.data(), frame.size());
  Require(decoded.ok() &&
              decoded.value.type_id == dt::CanonicalTypeId::bit_string &&
              decoded.value.state == dt::DatatypePhysicalValueState::value &&
              decoded.value.payload_data == frame.data() + 24 &&
              decoded.value.payload_bytes == component.size(),
          "MDF-013 bit structural SBDPV borrowed decode mismatch");

  const auto raw_encode = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::bit_string,
       dt::DatatypePhysicalValueState::value,
       {component.begin(), component.end()}});
  const auto raw_decode = dt::DecodeDatatypePhysicalValue(
      frame.data(), frame.size());
  Require(!raw_encode.ok() && !raw_decode.ok() &&
              raw_encode.diagnostic.diagnostic_code ==
                  "CTB.BIT.SERIALIZATION_PROFILE_MISSING" &&
              raw_decode.diagnostic.diagnostic_code ==
                  "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
          "MDF-013 generic raw bit semantic path was not refused");
  const auto malformed_raw=dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::bit_string,
       dt::DatatypePhysicalValueState::value,{}});
  Require(!malformed_raw.ok()&&malformed_raw.diagnostic.diagnostic_code==
              "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
          "MDF-013 raw bit payload was interpreted before profile refusal");
  const auto invalid_state=dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::bit_string,
       dt::DatatypePhysicalValueState::overflow_root,{0x80}});
  Require(!invalid_state.ok()&&invalid_state.diagnostic.diagnostic_code==
              "SB-DATATYPE-PHYSICAL-PAYLOAD-REFUSED",
          "MDF-013 raw bit invalid state precedence mismatch");
  const auto dirty_null=dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::bit_string,
       dt::DatatypePhysicalValueState::sql_null,{0x80}});
  Require(!dirty_null.ok()&&dirty_null.diagnostic.diagnostic_code==
              "DATATYPE.NULL_STATE.INVALID",
          "MDF-013 raw bit dirty NULL precedence mismatch");

  auto corrupt = frame;
  corrupt.back() ^= 1;
  const auto integrity = dt::DecodeDatatypePhysicalStructuralValueViewNoAlloc(
      corrupt.data(), corrupt.size());
  Require(!integrity.ok() && integrity.diagnostic.diagnostic_code ==
              "SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH",
          "MDF-013 structural bit corruption diagnostic mismatch");
}

void TestOverflowLocatorOpaqueAndProtectedStates() {
  RoundTrip(PhysicalValue(dt::CanonicalTypeId::blob,
                          dt::DatatypePhysicalValueState::overflow_root,
                          Payload(24, 0x20)));
  RoundTrip(PhysicalValue(dt::CanonicalTypeId::document,
                          dt::DatatypePhysicalValueState::overflow_chunk,
                          Payload(64, 0x30)));
  RoundTrip(PhysicalValue(dt::CanonicalTypeId::lob_locator,
                          dt::DatatypePhysicalValueState::locator_handle,
                          Payload(16, 0x40)));
  RoundTrip(PhysicalValue(dt::CanonicalTypeId::opaque_extension,
                          dt::DatatypePhysicalValueState::opaque_handle,
                          Payload(16, 0x50)));
  RoundTrip(PhysicalValue(dt::CanonicalTypeId::uuid,
                          dt::DatatypePhysicalValueState::protected_chunk_root,
                          Payload(32, 0x60)));
}

void TestMalformedPhysicalPayloadsAreRefused() {
  auto value = PhysicalValue(dt::CanonicalTypeId::int32,
                             dt::DatatypePhysicalValueState::value,
                             Payload(4, 0x10));
  auto encoded = dt::EncodeDatatypePhysicalValue(value);
  Require(encoded.ok(), "MDF-013 valid int32 payload did not encode");
  encoded.bytes[0] = 'X';
  auto decoded = dt::DecodeDatatypePhysicalValue(encoded.bytes.data(),
                                                 encoded.bytes.size());
  Require(!decoded.ok(), "MDF-013 accepted bad magic");
  Require(decoded.diagnostic.diagnostic_code == "SB-DATATYPE-PHYSICAL-BAD-MAGIC",
          "MDF-013 bad magic diagnostic mismatch");

  encoded = dt::EncodeDatatypePhysicalValue(value);
  encoded.bytes.back() ^= 0xff;
  decoded = dt::DecodeDatatypePhysicalValue(encoded.bytes.data(),
                                            encoded.bytes.size());
  Require(!decoded.ok(), "MDF-013 accepted checksum mismatch");
  Require(decoded.diagnostic.diagnostic_code ==
              "SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH",
          "MDF-013 checksum diagnostic mismatch");

  auto bad_null = PhysicalValue(dt::CanonicalTypeId::uuid,
                                dt::DatatypePhysicalValueState::sql_null,
                                Payload(1, 0x10));
  encoded = dt::EncodeDatatypePhysicalValue(bad_null);
  Require(!encoded.ok(), "MDF-013 accepted SQL null payload bytes");
  Require(encoded.diagnostic.diagnostic_code ==
              "DATATYPE.NULL_STATE.INVALID",
          "MDF-013 null payload diagnostic mismatch");

  auto bad_inline = PhysicalValue(dt::CanonicalTypeId::int64,
                                  dt::DatatypePhysicalValueState::value,
                                  Payload(7, 0x10));
  encoded = dt::EncodeDatatypePhysicalValue(bad_inline);
  Require(!encoded.ok(), "MDF-013 accepted wrong inline width");
}

void TestRestartPersistenceRoundTrip() {
  std::vector<platform::byte> image;
  std::size_t expected_count = 0;
  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    if (descriptor.type_id == dt::CanonicalTypeId::bit_string ||
        descriptor.type_id == dt::CanonicalTypeId::date ||
        descriptor.type_id == dt::CanonicalTypeId::time ||
        descriptor.type_id == dt::CanonicalTypeId::timestamp) continue;
    const auto layout = dt::LookupDatatypeStorageLayout(descriptor.type_id);
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        dt::SampleDatatypePhysicalValueForLayout(layout.layout));
    Require(encoded.ok(), "MDF-013 restart image encode failed");
    const std::uint32_t size = static_cast<std::uint32_t>(encoded.bytes.size());
    image.push_back(static_cast<platform::byte>(size & 0xffu));
    image.push_back(static_cast<platform::byte>((size >> 8) & 0xffu));
    image.push_back(static_cast<platform::byte>((size >> 16) & 0xffu));
    image.push_back(static_cast<platform::byte>((size >> 24) & 0xffu));
    image.insert(image.end(), encoded.bytes.begin(), encoded.bytes.end());
    ++expected_count;
  }

  const auto path = std::filesystem::temp_directory_path() /
                    "sb_mdf013_physical_encoding.bin";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
  }
  std::vector<platform::byte> restored(image.size());
  {
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char*>(restored.data()),
            static_cast<std::streamsize>(restored.size()));
  }
  std::filesystem::remove(path);
  Require(restored == image, "MDF-013 restart image changed on disk");

  std::size_t offset = 0;
  std::size_t decoded_count = 0;
  while (offset < restored.size()) {
    const std::uint32_t size =
        static_cast<std::uint32_t>(restored[offset]) |
        (static_cast<std::uint32_t>(restored[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(restored[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(restored[offset + 3]) << 24);
    offset += 4;
    const auto decoded = dt::DecodeDatatypePhysicalValue(restored.data() + offset,
                                                         size);
    Require(decoded.ok(), "MDF-013 persisted physical packet did not decode");
    offset += size;
    ++decoded_count;
  }
  Require(decoded_count == expected_count,
          "MDF-013 persisted row count mismatch");
}

}  // namespace

int main() {
  // MDF-013-CURRENT-CORE-DATATYPE-PHYSICAL-ENCODING
  // DEFER-DPE-ENCODER-DECODER
  // DEFER-DPE-IN-PAGE-LAYOUT
  // DEFER-DPE-OVERFLOW-LAYOUT
  TestEveryCanonicalDatatypePhysicalRoundTrip();
  TestBitStringStructuralBoundaryAndRawRefusal();
  TestDateStructuralBoundaryAndComposedAuthority();
  TestTimeStructuralBoundaryAndRawRefusal();
  TestTimestampStructuralBoundaryAndRawRefusal();
  TestD708TemporalProfileReceiptsAreRefusedAsCurrent();
  TestOverflowLocatorOpaqueAndProtectedStates();
  TestMalformedPhysicalPayloadsAreRefused();
  TestRestartPersistenceRoundTrip();
  std::cout << "current_core_datatype_physical_encoding_gate=passed\n";
  return EXIT_SUCCESS;
}
