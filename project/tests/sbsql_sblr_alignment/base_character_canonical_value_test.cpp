// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "canonical_utf8.hpp"
#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_layout.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "datatype_storage_identity.hpp"
#include "disk_device.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace dt = scratchbird::core::datatypes;
namespace disk = scratchbird::storage::disk;
namespace platform = scratchbird::core::platform;
namespace fs = std::filesystem;

namespace {

unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const std::string& reason) {
  ++checks;
  if (!condition) {
    ++failures;
    if (failures <= 80) std::cerr << "FAIL: " << reason << '\n';
  }
}

template <typename LeftUuid, typename RightUuid>
bool SameUuidBytes(const LeftUuid& left, const RightUuid& right) {
  return std::equal(std::begin(left.bytes), std::end(left.bytes),
                    std::begin(right.bytes), std::end(right.bytes));
}

std::string DiagnosticDetail(const platform::DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      const auto* value = argument.text();
      return value == nullptr ? std::string{} : *value;
    }
  }
  return {};
}

template <typename Result>
bool RejectedAs(const Result& result, std::string_view code,
                std::string_view detail) {
  return !result.ok() && result.diagnostic.diagnostic_code == code &&
      DiagnosticDetail(result.diagnostic) == detail;
}

constexpr std::size_t kMaximumBytes = 16u * 1024u * 1024u;
constexpr platform::Uuid kSnapshotV1{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x01}};
constexpr platform::Uuid kSnapshotV2{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x02}};
constexpr platform::Uuid kSnapshotV3{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x03}};
constexpr platform::Uuid kSnapshotV4{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x04}};
constexpr platform::Uuid kSnapshotV5{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x05}};
constexpr platform::Uuid kDescriptorUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x18}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x19}};
constexpr platform::Uuid kCodecUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x1a}};
constexpr std::string_view kCodecId = "datatype.text.utf8.v1";
constexpr std::string_view kRepresentation =
    "exact_well_formed_UTF8_scalar_sequence_without_implicit_normalization";

std::vector<platform::byte> Payload(std::string_view bytes) {
  return {bytes.begin(), bytes.end()};
}

scratchbird::engine::ExecutionTypeDescriptor DescriptorFor(
    dt::CanonicalTypeId type_id) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type_id);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto result =
      dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  return result.ok() ? result.descriptor
                     : scratchbird::engine::ExecutionTypeDescriptor{};
}

scratchbird::engine::ExecutionTypeDescriptor CharacterDescriptor() {
  return DescriptorFor(dt::CanonicalTypeId::character);
}

dt::DatatypeOperationValue Present(
    std::string bytes = std::string{"A\xc3\xa9\0\xf0\x9f\x99\x82", 8}) {
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::character,
                                   std::move(bytes), false};
  value.descriptor = CharacterDescriptor();
  return value;
}

dt::DatatypeOperationValue TypedNull() {
  auto descriptor = CharacterDescriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::character, {}, true};
  value.descriptor = descriptor;
  return value;
}

dt::DatatypeTextSeedAuthority BinaryTextSeed() {
  dt::DatatypeTextSeedAuthority seed;
  seed.active = true;
  seed.database_uuid = kSnapshotV3;
  seed.charset_uuid = kSnapshotV4;
  seed.collation_uuid = kSnapshotV5;
  seed.resource_epoch = 7;
  seed.collation_epoch = 11;
  seed.comparison_profile =
      scratchbird::core::resources::CollationProfile::utf8_binary;
  seed.seed_pack_name = "character-component-fixture";
  seed.seed_pack_version = "1";
  seed.charset_name = "UTF-8";
  seed.collation_name = "UTF8_BINARY";
  return seed;
}

std::string EncodeScalar(std::uint32_t scalar) {
  std::string bytes;
  if (scalar <= 0x7fu) {
    bytes.push_back(static_cast<char>(scalar));
  } else if (scalar <= 0x7ffu) {
    bytes.push_back(static_cast<char>(0xc0u | (scalar >> 6u)));
    bytes.push_back(static_cast<char>(0x80u | (scalar & 0x3fu)));
  } else if (scalar <= 0xffffu) {
    bytes.push_back(static_cast<char>(0xe0u | (scalar >> 12u)));
    bytes.push_back(static_cast<char>(0x80u | ((scalar >> 6u) & 0x3fu)));
    bytes.push_back(static_cast<char>(0x80u | (scalar & 0x3fu)));
  } else {
    bytes.push_back(static_cast<char>(0xf0u | (scalar >> 18u)));
    bytes.push_back(static_cast<char>(0x80u | ((scalar >> 12u) & 0x3fu)));
    bytes.push_back(static_cast<char>(0x80u | ((scalar >> 6u) & 0x3fu)));
    bytes.push_back(static_cast<char>(0x80u | (scalar & 0x3fu)));
  }
  return bytes;
}

std::uint64_t OracleBinaryChecksum(
    const std::vector<platform::byte>& payload) {
  std::uint64_t value = 1469598103934665603ull;
  for (const auto byte : payload) {
    value ^= byte;
    value *= 1099511628211ull;
  }
  return value;
}

std::vector<platform::byte> OracleBinaryFrame(
    const std::vector<platform::byte>& payload, bool is_null = false) {
  std::vector<platform::byte> frame(32 + payload.size(), 0);
  const std::array<platform::byte, 8> magic{{'S','B','D','V','A','L','0','1'}};
  std::copy(magic.begin(), magic.end(), frame.begin());
  platform::StoreLittle32(frame.data() + 8,
      static_cast<std::uint32_t>(dt::CanonicalTypeId::character));
  platform::StoreLittle16(frame.data() + 12, is_null ? 1 : 0);
  platform::StoreLittle16(frame.data() + 14, 32);
  platform::StoreLittle32(frame.data() + 16,
      static_cast<std::uint32_t>(payload.size()));
  platform::StoreLittle64(frame.data() + 24, OracleBinaryChecksum(payload));
  std::copy(payload.begin(), payload.end(), frame.begin() + 32);
  return frame;
}

std::uint32_t OraclePhysicalChecksum(
    dt::DatatypePhysicalValueState state,
    const std::vector<platform::byte>& payload) {
  std::uint32_t value = 2166136261u;
  auto mix = [&value](std::uint32_t next) {
    value ^= next;
    value *= 16777619u;
  };
  mix(static_cast<std::uint32_t>(dt::CanonicalTypeId::character));
  mix(static_cast<std::uint32_t>(state));
  for (const auto byte : payload) mix(byte);
  return value;
}

std::vector<platform::byte> OraclePhysicalFrame(
    dt::DatatypePhysicalValueState state,
    const std::vector<platform::byte>& payload) {
  std::vector<platform::byte> frame(24 + payload.size(), 0);
  const std::array<platform::byte, 8> magic{{'S','B','D','P','V','0','0','1'}};
  std::copy(magic.begin(), magic.end(), frame.begin());
  platform::StoreLittle32(frame.data() + 8,
      static_cast<std::uint32_t>(dt::CanonicalTypeId::character));
  platform::StoreLittle16(frame.data() + 12,
      static_cast<std::uint16_t>(state));
  platform::StoreLittle32(frame.data() + 16,
      static_cast<std::uint32_t>(payload.size()));
  platform::StoreLittle32(frame.data() + 20,
      OraclePhysicalChecksum(state, payload));
  std::copy(payload.begin(), payload.end(), frame.begin() + 24);
  return frame;
}

void ExactIdentityAndCohorts() {
  const auto descriptor = CharacterDescriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::character),
        "character execution descriptor has the exact catalog identity");

  struct Cohort { platform::Uuid snapshot; std::uint64_t generation; };
  const std::array cohorts{
      Cohort{kSnapshotV1, 1}, Cohort{kSnapshotV2, 2},
      Cohort{kSnapshotV3, 3}, Cohort{kSnapshotV4, 4},
      Cohort{kSnapshotV5, 5}};
  for (const auto& cohort : cohorts) {
    const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
        cohort.snapshot, cohort.generation, cohort.generation,
        kDescriptorUuid, 1);
    Check(admitted.ok &&
              dt::IsExactCanonicalTextTypeCodecIdentityV1(admitted.row) &&
              SameUuidBytes(admitted.row.descriptor_uuid, kDescriptorUuid) &&
              SameUuidBytes(admitted.row.type_uuid, kTypeUuid) &&
              SameUuidBytes(admitted.row.codec_uuid, kCodecUuid) &&
              admitted.row.descriptor_generation == 1 &&
              admitted.row.type_generation == 1 &&
              admitted.row.codec_id == kCodecId &&
              admitted.row.codec_version == 1 &&
              admitted.row.codec_generation == 1 &&
              admitted.row.canonical_value_bytes == 0 &&
              admitted.row.canonical_value_minimum_bytes == 0 &&
              admitted.row.canonical_value_maximum_bytes == kMaximumBytes &&
              admitted.row.canonical_value_exact_bytes == 0 &&
              admitted.row.canonical_value_exact_zero_is_width_marker &&
              admitted.row.canonical_byte_order == "byte_sequence" &&
              admitted.row.canonical_representation == kRepresentation &&
              admitted.row.canonical_charset == "UTF-8" &&
              admitted.row.shortest_form_utf8_required &&
              !admitted.row.implicit_normalization_allowed &&
              admitted.row.descriptor_bound_collation_required &&
              admitted.row.empty_value_distinct_from_sql_null &&
              admitted.row.sql_null_requires_zero_payload &&
              admitted.row.variable_width_storage_without_truncation,
          "every exact V1-V5 cohort preserves the canonical character tuple");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV5, 4, 5, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV5, 5, 4, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV5, 5, 5, kDescriptorUuid, 2).ok,
        "mixed cohort and descriptor generations refuse character identity");

  dt::DatatypeStorageIdentityV1 storage;
  const auto layout =
      dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::character);
  Check(dt::LookupDatatypeStorageIdentityV1(
            kSnapshotV5, 5, 5, kDescriptorUuid, 1, &storage) &&
            SameUuidBytes(storage.type_uuid, kTypeUuid) &&
            storage.type_id == dt::CanonicalTypeId::character &&
            storage.codec.has_value() &&
            SameUuidBytes(storage.codec->codec_uuid, kCodecUuid) &&
            layout.ok() &&
            layout.layout.storage_class ==
                dt::DatatypeStorageClass::inline_variable &&
            layout.layout.encoding ==
                dt::DatatypeBinaryEncoding::utf8_or_descriptor_charset_bytes &&
            layout.layout.inline_bytes == 0 &&
            layout.layout.alignment_bytes == 1 &&
            layout.layout.requires_descriptor &&
            layout.layout.requires_charset &&
            layout.layout.requires_collation &&
            layout.layout.may_overflow_to_toast,
        "character storage identity and variable layout remain descriptor bound");
}

void Utf8ClassesAndAtomicDecoder() {
  const std::vector<std::pair<std::string, std::uint64_t>> valid{
      {"", 0},
      {std::string{"A\0Z", 3}, 3},
      {"\x7f", 1},
      {"\xc2\x80", 1},
      {"\xdf\xbf", 1},
      {"\xe0\xa0\x80", 1},
      {"\xed\x9f\xbf", 1},
      {"\xee\x80\x80", 1},
      {"\xef\xb7\x90", 1},
      {"\xef\xbf\xbf", 1},
      {"\xf0\x90\x80\x80", 1},
      {"\xf4\x8f\xbf\xbf", 1},
      {"e\xcc\x81", 2},
      {"\xc3\xa9", 1},
      {"A\xc3\xa9\xf0\x9f\x99\x82", 3}};
  for (const auto& [bytes, expected_scalars] : valid) {
    std::uint64_t scalars = 99;
    Check(dt::ValidateCanonicalUtf8(
              reinterpret_cast<const std::uint8_t*>(bytes.data()),
              bytes.size(), &scalars) && scalars == expected_scalars,
          "canonical UTF-8 accepts every authorized scalar class");
  }

  std::uint64_t exhaustive_scalars = 0;
  std::uint64_t one_byte_scalars = 0;
  std::uint64_t two_byte_scalars = 0;
  for (std::uint32_t scalar = 0; scalar <= 0x10ffffu; ++scalar) {
    if (scalar >= 0xd800u && scalar <= 0xdfffu) continue;
    const auto bytes = EncodeScalar(scalar);
    std::uint64_t scalar_count = 99;
    std::size_t offset = 0;
    std::uint32_t decoded = 0xffffffffu;
    Check(dt::ValidateCanonicalUtf8(
              reinterpret_cast<const std::uint8_t*>(bytes.data()),
              bytes.size(), &scalar_count) && scalar_count == 1 &&
              dt::DecodeCanonicalUtf8Scalar(
                  reinterpret_cast<const std::uint8_t*>(bytes.data()),
                  bytes.size(), &offset, &decoded) &&
              offset == bytes.size() && decoded == scalar,
          "every Unicode scalar has one exact shortest-form UTF-8 encoding");
    if (bytes.size() == 1) ++one_byte_scalars;
    if (bytes.size() == 2) ++two_byte_scalars;
    ++exhaustive_scalars;
  }
  Check(exhaustive_scalars == 1'112'064 && one_byte_scalars == 128 &&
            two_byte_scalars == 1'920,
        "exhaustive UTF-8 sweep covers all scalars and every one/two-byte sequence");

  const std::vector<std::string> invalid{
      std::string{"\x80", 1}, std::string{"\xbf", 1},
      std::string{"\xc0\x80", 2}, std::string{"\xc1\xbf", 2},
      std::string{"\xc2", 1}, std::string{"\xe0\x80\x80", 3},
      std::string{"\xe2\x82", 2}, std::string{"\xed\xa0\x80", 3},
      std::string{"\xed\xbf\xbf", 3}, std::string{"\xf0\x80\x80\x80", 4},
      std::string{"\xf4\x90\x80\x80", 4},
      std::string{"\xf5\x80\x80\x80", 4}, std::string{"\xff", 1},
      std::string{"\xe2\x28\xa1", 3}, std::string{"A\x80", 2}};
  for (const auto& bytes : invalid) {
    std::uint64_t scalars = 99;
    Check(!dt::ValidateCanonicalUtf8(
              reinterpret_cast<const std::uint8_t*>(bytes.data()),
              bytes.size(), &scalars) && scalars == 0,
          "canonical UTF-8 rejects malformed input and clears partial count");
  }
  std::size_t offset = 1;
  std::uint32_t scalar = 0x12345678u;
  const std::string truncated = "A\xe2\x82";
  Check(!dt::DecodeCanonicalUtf8Scalar(
            reinterpret_cast<const std::uint8_t*>(truncated.data()),
            truncated.size(), &offset, &scalar) &&
            offset == 1 && scalar == 0x12345678u,
        "scalar decode failure leaves caller state unchanged");

  for (unsigned byte = 0x80; byte <= 0xc1; ++byte) {
    const std::array<std::uint8_t, 1> encoded{{
        static_cast<std::uint8_t>(byte)}};
    std::uint64_t scalar_count = 99;
    Check(!dt::ValidateCanonicalUtf8(encoded.data(), encoded.size(),
                                     &scalar_count) &&
              scalar_count == 0,
          "every lone continuation and overlong lead byte is rejected");
  }
  for (unsigned byte = 0xf5; byte <= 0xff; ++byte) {
    const std::array<std::uint8_t, 1> encoded{{
        static_cast<std::uint8_t>(byte)}};
    std::uint64_t scalar_count = 99;
    Check(!dt::ValidateCanonicalUtf8(encoded.data(), encoded.size(),
                                     &scalar_count) &&
              scalar_count == 0,
          "every out-of-range UTF-8 lead byte is rejected");
  }
}

void LowerCodecsAndBoundaries() {
  const std::vector<std::string> values{
      "", std::string{"A\0Z", 3}, "\xc3\xa9", "e\xcc\x81",
      "\xef\xb7\x90", "\xf4\x8f\xbf\xbf",
      "A\xc3\xa9\xf0\x9f\x99\x82"};
  for (const auto& bytes : values) {
    const auto payload = Payload(bytes);
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::character, false, false, payload});
    const auto binary_decoded = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::character,
         dt::DatatypePhysicalValueState::value, payload});
    const auto physical_decoded = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(binary.ok() && binary.encoded == OracleBinaryFrame(payload) &&
              binary_decoded.ok() && binary_decoded.value.payload == payload &&
              !binary_decoded.value.is_null &&
              physical.ok() &&
              physical.bytes == OraclePhysicalFrame(
                  dt::DatatypePhysicalValueState::value, payload) &&
              physical_decoded.ok() &&
              physical_decoded.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              physical_decoded.value.payload == payload,
          "lower codecs preserve legal character bytes including empty PRESENT");
  }

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::character, true, false, {}});
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::character,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(binary_null.ok() &&
            binary_null.encoded == OracleBinaryFrame({}, true) &&
            physical_null.ok() &&
            physical_null.bytes == OraclePhysicalFrame(
                dt::DatatypePhysicalValueState::sql_null, {}) &&
            binary_null.encoded != OracleBinaryFrame({}, false) &&
            physical_null.bytes != OraclePhysicalFrame(
                dt::DatatypePhysicalValueState::value, {}),
        "empty PRESENT and SQL NULL have distinct lower envelopes");

  const std::string maximum(kMaximumBytes, 'a');
  const std::string over_limit(kMaximumBytes + 1, 'a');
  const auto maximum_payload = Payload(maximum);
  const auto over_payload = Payload(over_limit);
  const auto maximum_binary = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::character, false, false, maximum_payload});
  const auto maximum_physical = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::character,
       dt::DatatypePhysicalValueState::value, maximum_payload});
  Check(maximum_binary.ok() && maximum_physical.ok(),
        "exact 16 MiB canonical character payload is admitted by lower codecs");
  Check(!dt::EncodeDatatypeBinaryValue(
              {dt::CanonicalTypeId::character, false, false, over_payload}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
              {dt::CanonicalTypeId::character,
               dt::DatatypePhysicalValueState::value, over_payload}).ok() &&
            !dt::DecodeDatatypeBinaryValue(OracleBinaryFrame(over_payload)).ok() &&
            !dt::DecodeDatatypePhysicalValue(
              OraclePhysicalFrame(dt::DatatypePhysicalValueState::value,
                                  over_payload).data(),
              24 + over_payload.size()).ok(),
        "lower encoders and decoders reject character payload above 16 MiB");

  const std::vector<std::string> malformed{
      std::string{"\x80", 1}, std::string{"\xc0\x80", 2},
      std::string{"\xe2\x82", 2}, std::string{"\xed\xa0\x80", 3},
      std::string{"\xf4\x90\x80\x80", 4}};
  for (const auto& bytes : malformed) {
    const auto payload = Payload(bytes);
    const auto binary_frame = OracleBinaryFrame(payload);
    const auto physical_frame = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    Check(!dt::EncodeDatatypeBinaryValue(
              {dt::CanonicalTypeId::character, false, false, payload}).ok() &&
            !dt::DecodeDatatypeBinaryValue(binary_frame).ok() &&
            !dt::EncodeDatatypePhysicalValue(
              {dt::CanonicalTypeId::character,
               dt::DatatypePhysicalValueState::value, payload}).ok() &&
            !dt::DecodeDatatypePhysicalValue(
              physical_frame.data(), physical_frame.size()).ok(),
          "all lower character boundaries reject malformed UTF-8");
  }
}

void NullAndLengths() {
  const auto value = Present(std::string{"A\0\xc3\xa9\xf0\x9f\x99\x82", 8});
  auto uint64_descriptor = DescriptorFor(dt::CanonicalTypeId::uint64);
  for (const auto& [field, expected] :
       std::array<std::pair<std::string_view, std::uint64_t>, 3>{{
           {"character_length", 4}, {"length", 4}, {"octet_length", 8}}}) {
    dt::DatatypeExtractRequest request;
    request.value = value;
    request.field = std::string(field);
    request.result_descriptor = uint64_descriptor;
    const auto result = dt::ExtractDatatypeField(request);
    std::uint64_t decoded = 0;
    Check(result.ok() && !result.value.is_null &&
              result.value.type_id == dt::CanonicalTypeId::uint64 &&
              dt::DecodeCanonicalUint64Value(result.value.encoded_value,
                                             &decoded) &&
              decoded == expected,
          "character length counts scalars while octet length counts bytes");
  }

  uint64_descriptor.nullable_allowed = true;
  dt::DatatypeExtractRequest null_length;
  null_length.value = TypedNull();
  null_length.field = "character_length";
  null_length.result_descriptor = uint64_descriptor;
  const auto null_result = dt::ExtractDatatypeField(null_length);
  Check(null_result.ok() && null_result.value.is_null &&
            null_result.value.type_id == dt::CanonicalTypeId::uint64 &&
            null_result.value.encoded_value.empty(),
        "character length propagates typed NULL without manufacturing zero");
}

void DescriptorAndGenericRules() {
  const auto present = Present("A\xc3\xa9");
  auto descriptorless = present;
  descriptorless.descriptor = {};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "text";
  auto wrong = present;
  wrong.descriptor = DescriptorFor(dt::CanonicalTypeId::binary);
  auto stale = present;
  ++stale.descriptor.descriptor_epoch;
  auto wrong_identity = present;
  wrong_identity.descriptor.descriptor_uuid.bytes[15] ^= 1u;
  auto wrong_family = present;
  wrong_family.descriptor.family =
      scratchbird::engine::ExecutionTypeFamily::binary;
  auto invalid_modifiers = present;
  invalid_modifiers.descriptor.length = 32;
  for (const auto& invalid : {descriptorless, label_only, wrong, stale,
                              wrong_identity, wrong_family,
                              invalid_modifiers}) {
    const auto serialized = dt::SerializeDatatypeValue({invalid});
    dt::DatatypeExtractRequest extract;
    extract.value = invalid;
    extract.field = "character_length";
    extract.result_descriptor = DescriptorFor(dt::CanonicalTypeId::uint64);
    const auto extracted = dt::ExtractDatatypeField(extract);
    Check(!serialized.ok() &&
              serialized.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              extracted.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              serialized.serialized_value.empty() &&
              extracted.value.encoded_value.empty(),
          "invalid character descriptors refuse extract and generic serialization");
  }

  auto parameterized = present;
  parameterized.descriptor.length = 32;
  parameterized.descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::length);
  auto domain_bound = present;
  std::copy(kSnapshotV3.bytes.begin(), kSnapshotV3.bytes.end(),
            std::begin(domain_bound.descriptor.domain_uuid.bytes));
  domain_bound.descriptor.domain_stack = {domain_bound.descriptor.domain_uuid};
  domain_bound.descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::domain_uuid) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::domain_stack);
  auto security_bound = present;
  std::copy(kSnapshotV4.bytes.begin(), kSnapshotV4.bytes.end(),
            std::begin(security_bound.descriptor.security_policy_uuid.bytes));
  security_bound.descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  auto charset_collation_bound = present;
  std::copy(kSnapshotV3.bytes.begin(), kSnapshotV3.bytes.end(),
            std::begin(charset_collation_bound.descriptor.charset_uuid.bytes));
  std::copy(kSnapshotV4.bytes.begin(), kSnapshotV4.bytes.end(),
            std::begin(charset_collation_bound.descriptor.collation_uuid.bytes));
  charset_collation_bound.descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::charset_uuid) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::collation_uuid);
  for (const auto& decorated : {parameterized, domain_bound, security_bound,
                                charset_collation_bound}) {
    dt::DatatypeExtractRequest extract;
    extract.value = decorated;
    extract.field = "character_length";
    extract.result_descriptor = DescriptorFor(dt::CanonicalTypeId::uint64);
    const auto extracted = dt::ExtractDatatypeField(extract);
    std::uint64_t scalar_count = 0;
    const auto serialized = dt::SerializeDatatypeValue({decorated});
    Check(extracted.ok() &&
              dt::DecodeCanonicalUint64Value(extracted.value.encoded_value,
                                             &scalar_count) &&
              scalar_count == 2,
          "extract accepts valid parameter, domain and security descriptor metadata");
    Check(RejectedAs(serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                     "character_serialization_policy_unresolved") &&
              serialized.serialized_value.empty(),
          "generic serialization validates decorated descriptors then refuses unresolved policy");
  }
}

void SemanticAndGenericSurfacesRefuse() {
  const auto present = Present("e\xcc\x81");
  const auto other = Present("\xc3\xa9");
  const auto missing_compare = dt::CompareDatatypeValues({present, other});
  const auto missing_key = dt::MakeDatatypeSortKey({present});
  const auto missing_hash = dt::HashDatatypeValue({present});

  auto crossed_seed = BinaryTextSeed();
  crossed_seed.collation_case_insensitive = true;
  dt::DatatypeComparisonRequest crossed_compare_request;
  crossed_compare_request.left = present;
  crossed_compare_request.right = other;
  crossed_compare_request.text_seed = crossed_seed;
  const auto crossed_compare =
      dt::CompareDatatypeValues(crossed_compare_request);
  dt::DatatypeSortKeyRequest crossed_key_request;
  crossed_key_request.value = present;
  crossed_key_request.text_seed = crossed_seed;
  const auto crossed_key = dt::MakeDatatypeSortKey(crossed_key_request);
  dt::DatatypeHashRequest crossed_hash_request;
  crossed_hash_request.value = present;
  crossed_hash_request.text_seed = crossed_seed;
  const auto crossed_hash = dt::HashDatatypeValue(crossed_hash_request);
  Check(!missing_compare.ok() && !missing_key.ok() && !missing_hash.ok() &&
            !crossed_compare.ok() && !crossed_key.ok() &&
            !crossed_hash.ok() && missing_compare.comparison == 0 &&
            crossed_compare.comparison == 0 && missing_key.sort_key.empty() &&
            crossed_key.sort_key.empty() &&
            missing_hash.stable_hash_hex.empty() &&
            crossed_hash.stable_hash_hex.empty(),
        "missing or crossed text seed authority refuses compare, sort and hash");

  const auto seed = BinaryTextSeed();
  dt::DatatypeComparisonRequest bound_compare_request;
  bound_compare_request.left = present;
  bound_compare_request.right = other;
  bound_compare_request.text_seed = seed;
  const auto bound_compare = dt::CompareDatatypeValues(bound_compare_request);
  dt::DatatypeSortKeyRequest first_key_request;
  first_key_request.value = present;
  first_key_request.text_seed = seed;
  auto second_key_request = first_key_request;
  second_key_request.value = other;
  const auto first_key = dt::MakeDatatypeSortKey(first_key_request);
  const auto second_key = dt::MakeDatatypeSortKey(second_key_request);
  dt::DatatypeHashRequest first_hash_request;
  first_hash_request.value = present;
  first_hash_request.text_seed = seed;
  auto second_hash_request = first_hash_request;
  second_hash_request.value = other;
  const auto first_hash = dt::HashDatatypeValue(first_hash_request);
  const auto second_hash = dt::HashDatatypeValue(second_hash_request);
  Check(bound_compare.ok() && bound_compare.comparison < 0 &&
            first_key.ok() && second_key.ok() &&
            first_key.sort_key < second_key.sort_key &&
            first_hash.ok() && second_hash.ok() &&
            first_hash.stable_hash_hex != second_hash.stable_hash_hex,
        "bound UTF8_BINARY seed permits the caller-authorized component primitive");

  const auto typed_null = TypedNull();
  const auto null_compare =
      dt::CompareDatatypeValues({typed_null, typed_null});
  const auto null_first = dt::MakeDatatypeSortKey({
      typed_null, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey({
      typed_null, dt::DatatypeNullOrdering::nulls_last});
  const auto null_hash = dt::HashDatatypeValue({typed_null});
  Check(null_compare.ok() && null_compare.comparison == 0 &&
            null_first.ok() && null_first.sort_key == "00:null" &&
            null_last.ok() && null_last.sort_key == "ff:null" &&
            null_hash.ok() && !null_hash.stable_hash_hex.empty(),
        "typed character NULL compare, sort and hash need no text seed");

  auto dirty_null = typed_null;
  dirty_null.encoded_value = std::string{"\xed\xa0\x80", 3};
  const auto dirty_compare =
      dt::CompareDatatypeValues({dirty_null, typed_null});
  const auto dirty_key = dt::MakeDatatypeSortKey({dirty_null});
  const auto dirty_hash = dt::HashDatatypeValue({dirty_null});
  Check(!dirty_compare.ok() && !dirty_key.ok() && !dirty_hash.ok() &&
            dirty_compare.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID" &&
            dirty_key.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID" &&
            dirty_hash.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID" &&
            dirty_compare.comparison == 0 && dirty_key.sort_key.empty() &&
            dirty_hash.stable_hash_hex.empty(),
        "typed character NULL payload is state-invalid and never interpreted as UTF-8");

  const auto serialized = dt::SerializeDatatypeValue({present});
  const auto null_serialized = dt::SerializeDatatypeValue({TypedNull()});
  Check(RejectedAs(serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                   "character_serialization_policy_unresolved") &&
            RejectedAs(null_serialized,
                       "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "character_serialization_policy_unresolved") &&
            serialized.serialized_value.empty() &&
            null_serialized.serialized_value.empty(),
        "unresolved generic character serialization refuses atomically");

  dt::DatatypeSetDescriptor descriptor;
  descriptor.element_type_id = dt::CanonicalTypeId::character;
  descriptor.element_descriptor = present.descriptor;
  const auto encoded = dt::EncodeSetValue(descriptor, {present});
  const auto empty = dt::EncodeSetValue(descriptor, {});
  Check(!encoded.ok() && !empty.ok() && encoded.encoded_set.empty() &&
            empty.encoded_set.empty(),
        "character set construction refuses without collation authority");
  for (const auto operation : {dt::DatatypeSetOperationKind::membership,
                               dt::DatatypeSetOperationKind::equals,
                               dt::DatatypeSetOperationKind::subset,
                               dt::DatatypeSetOperationKind::superset,
                               dt::DatatypeSetOperationKind::cardinality}) {
    dt::DatatypeSetOperationRequest request;
    request.operation = operation;
    request.descriptor = descriptor;
    request.left_encoded_set = "SBSET2;element=character;descriptor=invalid";
    request.right_value = present;
    request.right_encoded_set = request.left_encoded_set;
    const auto result = dt::ApplySetOperation(request);
    Check(!result.ok() && result.value.encoded_value.empty() &&
              result.encoded_set.empty(),
          "every character set operation refuses before publishing output");
  }

  const std::string valid_frame =
      "SBDV1;type=character;state=value;payload=41";
  dt::DatatypeDeserializationRequest descriptorless_restore;
  descriptorless_restore.expected_type_id = dt::CanonicalTypeId::character;
  descriptorless_restore.serialized_value = valid_frame;
  const auto descriptorless_result =
      dt::DeserializeDatatypeValue(descriptorless_restore);
  Check(RejectedAs(descriptorless_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "expected_descriptor_invalid") &&
            descriptorless_result.value.encoded_value.empty(),
        "name-framed character deserialization rejects a missing descriptor");

  auto exact_restore = descriptorless_restore;
  exact_restore.expected_descriptor = present.descriptor;
  const auto exact_result = dt::DeserializeDatatypeValue(exact_restore);
  Check(RejectedAs(exact_result, "SB_DATATYPE_DESERIALIZATION_REJECTED",
                   "character_deserialization_policy_unresolved") &&
            exact_result.value.encoded_value.empty(),
        "name-framed character deserialization validates identity then refuses policy");

  for (const auto& decorated : [&] {
         std::array<scratchbird::engine::ExecutionTypeDescriptor, 4> values;
         values[0] = present.descriptor;
         values[0].length = 32;
         values[0].modifier_flags |=
             scratchbird::engine::ExecutionTypeModifierFlagBit(
                 scratchbird::engine::ExecutionTypeModifierFlag::length);
         values[1] = present.descriptor;
         std::copy(kSnapshotV3.bytes.begin(), kSnapshotV3.bytes.end(),
                   std::begin(values[1].domain_uuid.bytes));
         values[1].domain_stack = {values[1].domain_uuid};
         values[1].modifier_flags |=
             scratchbird::engine::ExecutionTypeModifierFlagBit(
                 scratchbird::engine::ExecutionTypeModifierFlag::domain_uuid) |
             scratchbird::engine::ExecutionTypeModifierFlagBit(
                 scratchbird::engine::ExecutionTypeModifierFlag::domain_stack);
         values[2] = present.descriptor;
         std::copy(kSnapshotV4.bytes.begin(), kSnapshotV4.bytes.end(),
                   std::begin(values[2].security_policy_uuid.bytes));
         values[2].modifier_flags |=
             scratchbird::engine::ExecutionTypeModifierFlagBit(
                 scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
         values[3] = present.descriptor;
         std::copy(kSnapshotV3.bytes.begin(), kSnapshotV3.bytes.end(),
                   std::begin(values[3].charset_uuid.bytes));
         std::copy(kSnapshotV4.bytes.begin(), kSnapshotV4.bytes.end(),
                   std::begin(values[3].collation_uuid.bytes));
         values[3].modifier_flags |=
             scratchbird::engine::ExecutionTypeModifierFlagBit(
                 scratchbird::engine::ExecutionTypeModifierFlag::charset_uuid) |
             scratchbird::engine::ExecutionTypeModifierFlagBit(
                 scratchbird::engine::ExecutionTypeModifierFlag::collation_uuid);
         return values;
       }()) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::character;
    request.expected_descriptor = decorated;
    request.serialized_value = valid_frame;
    const auto result = dt::DeserializeDatatypeValue(request);
    Check(RejectedAs(result, "SB_DATATYPE_DESERIALIZATION_REJECTED",
                     "character_deserialization_policy_unresolved") &&
              result.value.encoded_value.empty(),
          "generic deserialization validates decorated descriptors then refuses unresolved policy");
  }

  dt::DatatypeDeserializationRequest malformed;
  malformed.expected_type_id = dt::CanonicalTypeId::character;
  malformed.expected_descriptor = present.descriptor;
  malformed.serialized_value =
      "SBDV1;type=character;state=value;payload=eda080";
  const auto malformed_result = dt::DeserializeDatatypeValue(malformed);
  Check(!malformed_result.ok() &&
            DiagnosticDetail(malformed_result.diagnostic) ==
                "character_utf8_invalid" &&
            malformed_result.value.encoded_value.empty(),
        "generic frame validates malformed UTF-8 before unresolved policy");
}

void FileDevicePersistence() {
  const std::vector<std::string> carriers{
      "", std::string{"A\0Z", 3}, "\xc3\xa9", "e\xcc\x81",
      "\xef\xb7\x90", "\xf4\x8f\xbf\xbf"};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto& carrier : carriers) {
    frames.push_back(OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, Payload(carrier)));
  }
  frames.push_back(OraclePhysicalFrame(
      dt::DatatypePhysicalValueState::sql_null, {}));

  constexpr std::size_t header_bytes = 160;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte, 8> magic{{'S','B','T','E','X','T','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(kDescriptorUuid.bytes.begin(), kDescriptorUuid.bytes.end(),
            expected.begin() + 8);
  std::copy(kTypeUuid.bytes.begin(), kTypeUuid.bytes.end(),
            expected.begin() + 24);
  std::copy(kCodecUuid.bytes.begin(), kCodecUuid.bytes.end(),
            expected.begin() + 40);
  std::copy(kSnapshotV5.bytes.begin(), kSnapshotV5.bytes.end(),
            expected.begin() + 56);
  platform::StoreLittle32(expected.data() + 72, 1);
  platform::StoreLittle32(expected.data() + 76, 1);
  platform::StoreLittle32(expected.data() + 80, 1);
  platform::StoreLittle32(expected.data() + 84, 5);
  platform::StoreLittle32(expected.data() + 88, 5);
  platform::StoreLittle32(expected.data() + 92,
                          static_cast<std::uint32_t>(frames.size()));
  std::uint32_t offset = header_bytes;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    platform::StoreLittle32(expected.data() + 96 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 100 + index * 8,
                            static_cast<std::uint32_t>(frames[index].size()));
    offset += static_cast<std::uint32_t>(frames[index].size());
  }
  for (const auto& frame : frames)
    expected.insert(expected.end(), frame.begin(), frame.end());

#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-character-" + std::to_string(pid) + ".carrier");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create test-owned character persistence envelope");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write sync and close exact character persistence envelope");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen character persistence envelope read-only");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected &&
      std::equal(kDescriptorUuid.bytes.begin(), kDescriptorUuid.bytes.end(),
                 actual.begin() + 8) &&
      std::equal(kTypeUuid.bytes.begin(), kTypeUuid.bytes.end(),
                 actual.begin() + 24) &&
      std::equal(kCodecUuid.bytes.begin(), kCodecUuid.bytes.end(),
                 actual.begin() + 40) &&
      std::equal(kSnapshotV5.bytes.begin(), kSnapshotV5.bytes.end(),
                 actual.begin() + 56) &&
      platform::LoadLittle32(actual.data() + 72) == 1 &&
      platform::LoadLittle32(actual.data() + 76) == 1 &&
      platform::LoadLittle32(actual.data() + 80) == 1 &&
      platform::LoadLittle32(actual.data() + 84) == 5 &&
      platform::LoadLittle32(actual.data() + 88) == 5;
  for (std::size_t index = 0; index < frames.size() && decoded_all; ++index) {
    const auto frame_offset =
        platform::LoadLittle32(actual.data() + 96 + index * 8);
    const auto frame_size =
        platform::LoadLittle32(actual.data() + 100 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::character &&
        (index + 1 == frames.size()
             ? decoded.value.state ==
                       dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state ==
                       dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(carriers[index]));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "FileDevice reopen preserves character UUID/generations, bytes, empty and NULL");

  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size() &&
            !rejected_write.ok() && rejected_write.bytes_transferred == 0 &&
            reader.Close().ok(),
        "character fixture detects short reads and read-only writes");

  auto corrupt = frames[1];
  corrupt[20] ^= 1u;
  Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
            !dt::DecodeDatatypePhysicalValue(
                frames[1].data(), frames[1].size() - 1).ok(),
        "character physical decoder rejects corruption and truncation");
}

}  // namespace

int main() {
  ExactIdentityAndCohorts();
  Utf8ClassesAndAtomicDecoder();
  LowerCodecsAndBoundaries();
  NullAndLengths();
  DescriptorAndGenericRules();
  SemanticAndGenericSurfacesRefuse();
  FileDevicePersistence();
  std::cout << "base character canonical value checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
