// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_physical_encoding.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    if (descriptor.type_id == dt::CanonicalTypeId::bit_string) continue;
    const auto layout = dt::LookupDatatypeStorageLayout(descriptor.type_id);
    Require(layout.ok(), "MDF-013 missing storage layout");
    RoundTrip(dt::SampleDatatypePhysicalValueForLayout(layout.layout));

    dt::DatatypePhysicalValue null_value;
    null_value.type_id = descriptor.type_id;
    null_value.state = dt::DatatypePhysicalValueState::sql_null;
    RoundTrip(null_value);
  }
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
    if (descriptor.type_id == dt::CanonicalTypeId::bit_string) continue;
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
  TestOverflowLocatorOpaqueAndProtectedStates();
  TestMalformedPhysicalPayloadsAreRefused();
  TestRestartPersistenceRoundTrip();
  std::cout << "current_core_datatype_physical_encoding_gate=passed\n";
  return EXIT_SUCCESS;
}
