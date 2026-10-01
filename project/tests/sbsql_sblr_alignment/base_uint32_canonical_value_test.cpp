// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "datatype_storage_identity.hpp"
#include "disk_device.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <tuple>
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

void Check(bool ok, const std::string& reason) {
  ++checks;
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << reason << '\n';
  }
}

void CheckCastRefused(const dt::DatatypeCastResult& result,
                      std::string_view diagnostic_code,
                      const std::string& reason) {
  Check(!result.ok() && result.diagnostic.diagnostic_code == diagnostic_code &&
            result.value.type_id == dt::CanonicalTypeId::unknown &&
            !result.value.is_null && result.value.encoded_value.empty(),
        reason);
}

scratchbird::engine::Uuid FixtureV7Uuid(std::uint8_t seed) {
  scratchbird::engine::Uuid uuid{};
  for (std::size_t index = 0; index < 16; ++index) {
    uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0fu) | 0x70u);
  uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

scratchbird::engine::ExecutionTypeDescriptor DescriptorFor(
    dt::CanonicalTypeId type_id) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto row = dt::LookupDatatypeCatalogRow(
      manifest.manifest, type_id);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto descriptor = dt::LookupExecutionTypeDescriptorFromCatalog(
      type_id, metadata);
  return descriptor.ok()
      ? descriptor.descriptor
      : scratchbird::engine::ExecutionTypeDescriptor{};
}

scratchbird::engine::ExecutionTypeDescriptor Uint32Descriptor() {
  return DescriptorFor(dt::CanonicalTypeId::uint32);
}

dt::DatatypeOperationValue Uint32(std::uint64_t value) {
  std::string encoded;
  if (!dt::EncodeCanonicalUint32Value(value, &encoded)) return {};
  return {dt::CanonicalTypeId::uint32, std::move(encoded), false};
}

std::vector<platform::byte> Payload(const dt::DatatypeOperationValue& value) {
  return {value.encoded_value.begin(), value.encoded_value.end()};
}

void ExactIdentity() {
  constexpr platform::Uuid descriptor_uuid{{
      0x7a,0,0,0,0x75,0x69,0x7e,0x74,0xb3,0x32,0,0,0,0,0,0}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x08}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x09}};

  const auto descriptor = Uint32Descriptor();
  Check(std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                   std::end(descriptor.descriptor_uuid.bytes),
                   descriptor_uuid.bytes.begin()) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.stable_name == "uint32" && descriptor.bit_width == 32,
        "exact uint32 descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_snapshot_uuid == dt::kDatatypeCohortV5 &&
        row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::uint32);
  });
  Check(found != rows.end() && found->descriptor_uuid == descriptor_uuid &&
            found->type_uuid == type_uuid && found->codec_uuid == codec_uuid &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.uint32.le.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 4 &&
            found->canonical_value_maximum_bytes == 4 &&
            found->canonical_value_exact_bytes == 4 && found->null_supported &&
            found->canonical_byte_order == "little_endian" &&
            found->canonical_representation == "unsigned_integer",
        "exact uint32 type-codec tuple");

  for (const auto cohort : std::array{
           std::tuple{dt::kDatatypeCohortV4, 4ULL, 4ULL},
           std::tuple{dt::kDatatypeCohortV5, 5ULL, 5ULL}}) {
    const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
        std::get<0>(cohort), std::get<1>(cohort), std::get<2>(cohort),
        descriptor_uuid, 1);
    Check(admitted.ok && admitted.row.type_uuid == type_uuid &&
              admitted.row.codec_uuid == codec_uuid,
          "uint32 exact identity survives admitted cohort inheritance");
  }
  for (const auto cohort : std::array{
           std::tuple{dt::kDatatypeCohortV1, 1ULL, 1ULL},
           std::tuple{dt::kDatatypeCohortV2, 2ULL, 2ULL},
           std::tuple{dt::kDatatypeCohortV3, 3ULL, 3ULL}}) {
    Check(!dt::LookupDatatypeTypeCodecIdentityV1(
               std::get<0>(cohort), std::get<1>(cohort), std::get<2>(cohort),
               descriptor_uuid, 1).ok,
          "uint32 identity rejects cohorts that predate its admission");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV4, 5, 5, descriptor_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 2).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, type_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, platform::Uuid{}, 1).ok,
        "uint32 identity rejects crossed generations and wrong-role identities");

  dt::DatatypeStorageIdentityV1 storage_identity;
  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 1,
            &storage_identity) &&
            storage_identity.descriptor_uuid == descriptor_uuid &&
            storage_identity.type_uuid == type_uuid &&
            storage_identity.type_id == dt::CanonicalTypeId::uint32 &&
            storage_identity.codec.has_value() &&
            storage_identity.codec->codec_uuid == codec_uuid,
        "uint32 storage identity preserves the exact descriptor/type/codec cohort");

  const auto layout = dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::uint32);
  Check(layout.ok() &&
            layout.layout.storage_class == dt::DatatypeStorageClass::inline_fixed &&
            layout.layout.encoding == dt::DatatypeBinaryEncoding::unsigned_little_endian &&
            layout.layout.inline_bytes == 4 && layout.layout.alignment_bytes == 4,
        "uint32 storage layout is fixed unsigned LE4");

}

void RepresentationAndCodecs() {
  struct Vector {
    std::uint64_t value;
    std::array<platform::byte, 4> bytes;
    std::array<platform::byte, 5> key;
  };
  constexpr std::array<Vector, 14> vectors{{
      {0ULL, {0x00,0x00,0x00,0x00}, {0x01,0x00,0x00,0x00,0x00}},
      {1ULL, {0x01,0x00,0x00,0x00}, {0x01,0x00,0x00,0x00,0x01}},
      {127ULL, {0x7f,0x00,0x00,0x00}, {0x01,0x00,0x00,0x00,0x7f}},
      {128ULL, {0x80,0x00,0x00,0x00}, {0x01,0x00,0x00,0x00,0x80}},
      {255ULL, {0xff,0x00,0x00,0x00}, {0x01,0x00,0x00,0x00,0xff}},
      {256ULL, {0x00,0x01,0x00,0x00}, {0x01,0x00,0x00,0x01,0x00}},
      {32767ULL, {0xff,0x7f,0x00,0x00}, {0x01,0x00,0x00,0x7f,0xff}},
      {32768ULL, {0x00,0x80,0x00,0x00}, {0x01,0x00,0x00,0x80,0x00}},
      {65535ULL, {0xff,0xff,0x00,0x00}, {0x01,0x00,0x00,0xff,0xff}},
      {65536ULL, {0x00,0x00,0x01,0x00}, {0x01,0x00,0x01,0x00,0x00}},
      {2147483647ULL, {0xff,0xff,0xff,0x7f}, {0x01,0x7f,0xff,0xff,0xff}},
      {2147483648ULL, {0x00,0x00,0x00,0x80}, {0x01,0x80,0x00,0x00,0x00}},
      {4294967294ULL, {0xfe,0xff,0xff,0xff}, {0x01,0xff,0xff,0xff,0xfe}},
      {4294967295ULL, {0xff,0xff,0xff,0xff}, {0x01,0xff,0xff,0xff,0xff}}}};

  std::string previous_key;
  for (std::size_t index = 0; index < vectors.size(); ++index) {
    const auto& vector = vectors[index];
    const auto value = Uint32(vector.value);
    const std::vector<platform::byte> expected(vector.bytes.begin(),
                                               vector.bytes.end());
    std::string encoded;
    std::uint64_t decoded = 0;
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::uint32, false, false, expected});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint32,
         dt::DatatypePhysicalValueState::value, expected});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto comparison = dt::CompareDatatypeValues({value, value});
    const auto key = dt::MakeDatatypeSortKey({value});
    const std::vector<platform::byte> actual_key(
        key.sort_key.begin(), key.sort_key.end());
    Check(dt::EncodeCanonicalUint32Value(vector.value, &encoded) &&
              encoded == value.encoded_value && Payload(value) == expected &&
              dt::DecodeCanonicalUint32Value(encoded, &decoded) &&
              decoded == vector.value && binary.ok() && binary_back.ok() &&
              binary_back.value.type_id == dt::CanonicalTypeId::uint32 &&
              binary_back.value.payload == expected && physical.ok() &&
              physical_back.ok() &&
              physical_back.value.type_id == dt::CanonicalTypeId::uint32 &&
              physical_back.value.payload == expected && comparison.ok() &&
              comparison.comparison == 0 && key.ok() &&
              actual_key == std::vector<platform::byte>(
                  vector.key.begin(), vector.key.end()),
          "uint32 canonical, binary, physical, comparison, and key vector");
    if (index != 0) {
      const auto ordered = dt::CompareDatatypeValues(
          {Uint32(vectors[index - 1].value), value});
      Check(ordered.ok() && ordered.comparison == -1 &&
                previous_key < key.sort_key,
            "uint32 comparison and key order adjacent boundary vectors");
    }
    previous_key = key.sort_key;
  }

  std::string unchanged = "sentinel";
  Check(!dt::EncodeCanonicalUint32Value(4294967296ULL, &unchanged) &&
            unchanged == "sentinel" &&
            !dt::EncodeCanonicalUint32Value(
                static_cast<std::uint64_t>(std::int64_t{-1}), &unchanged) &&
            unchanged == "sentinel" &&
            !dt::EncodeCanonicalUint32Value(0, nullptr),
        "uint32 encoder rejects overflow, negative-origin wrap, and null output atomically");
  for (const auto width : {0u, 1u, 2u, 3u, 5u, 32u}) {
    const std::string malformed(width, '\0');
    std::uint64_t unchanged_number = 0x1122334455667788ULL;
    Check(!dt::DecodeCanonicalUint32Value(malformed, &unchanged_number) &&
              unchanged_number == 0x1122334455667788ULL,
          "uint32 mathematical decoder rejects malformed width atomically");
    const std::vector<platform::byte> bad(width, 0);
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::uint32, false, false, bad}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::uint32,
                dt::DatatypePhysicalValueState::value, bad}).ok(),
          "uint32 codecs reject non-four-byte present payload");
    const dt::DatatypeOperationValue operation{
        dt::CanonicalTypeId::uint32, malformed, false};
    Check(!dt::CompareDatatypeValues({operation, Uint32(0)}).ok() &&
              !dt::MakeDatatypeSortKey({operation}).ok() &&
              !dt::SerializeDatatypeValue({operation}).ok() &&
              !dt::HashDatatypeValue({operation}).ok() &&
              !dt::RenderDatatypeValueForDisplay({operation}).ok(),
          "uint32 operation surfaces reject malformed carrier widths");
  }
  Check(!dt::DecodeCanonicalUint32Value(std::string(4, '\0'), nullptr),
        "uint32 decoder refuses a null output pointer");

  std::uint64_t text_like = 0;
  Check(dt::DecodeCanonicalUint32Value(std::string{"1234", 4}, &text_like) &&
            text_like == 875770417ULL,
        "four text-like bytes remain native uint32 0x34333231");

  for (const auto& vector : vectors) {
    const auto input = Uint32(vector.value);
    const auto serialized = dt::SerializeDatatypeValue({input});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::uint32;
    request.serialized_value = serialized.serialized_value;
    const auto restored = dt::DeserializeDatatypeValue(request);
    Check(serialized.ok() && restored.ok() &&
              restored.value.type_id == dt::CanonicalTypeId::uint32 &&
              !restored.value.is_null &&
              restored.value.encoded_value == input.encoded_value,
          "generic value serialization preserves uint32 native bytes");
  }
  for (const char* bad : {"SBDV1;type=uint32;state=value;payload=",
                          "SBDV1;type=uint32;state=value;payload=00",
                          "SBDV1;type=uint32;state=value;payload=000000",
                          "SBDV1;type=uint32;state=value;payload=0000000000"}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::uint32;
    request.serialized_value = bad;
    Check(!dt::DeserializeDatatypeValue(request).ok(),
          "generic value deserializer rejects malformed uint32 widths");
  }
}

void StructuredPropertyPartitions() {
  const auto verify = [](std::uint32_t raw, bool have_previous,
                         std::uint32_t previous_value,
                         const std::string& previous_key) {
    const std::string expected_bytes{
        static_cast<char>(raw & 0xffu),
        static_cast<char>((raw >> 8u) & 0xffu),
        static_cast<char>((raw >> 16u) & 0xffu),
        static_cast<char>((raw >> 24u) & 0xffu)};
    const std::string expected_key{
        '\x01',
        static_cast<char>((raw >> 24u) & 0xffu),
        static_cast<char>((raw >> 16u) & 0xffu),
        static_cast<char>((raw >> 8u) & 0xffu),
        static_cast<char>(raw & 0xffu)};
    std::string encoded;
    std::uint64_t decoded = 0;
    const bool encoded_ok = dt::EncodeCanonicalUint32Value(raw, &encoded);
    const dt::DatatypeOperationValue value{
        dt::CanonicalTypeId::uint32, encoded, false};
    const std::vector<platform::byte> payload(expected_bytes.begin(),
                                              expected_bytes.end());
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::uint32, false, false, payload});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint32,
         dt::DatatypePhysicalValueState::value, payload});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto equal = dt::CompareDatatypeValues({value, value});
    const auto key = dt::MakeDatatypeSortKey({value});
    bool ordered = true;
    if (have_previous) {
      const auto previous = Uint32(previous_value);
      const auto forward = dt::CompareDatatypeValues({previous, value});
      const auto reverse = dt::CompareDatatypeValues({value, previous});
      ordered = previous_value < raw && forward.ok() &&
          forward.comparison == -1 && reverse.ok() && reverse.comparison == 1 &&
          previous_key < key.sort_key;
    }
    Check(encoded_ok && encoded == expected_bytes &&
              dt::DecodeCanonicalUint32Value(encoded, &decoded) &&
              decoded == raw && binary.ok() && binary_back.ok() &&
              binary_back.value.payload == payload && physical.ok() &&
              physical_back.ok() && physical_back.value.payload == payload &&
              equal.ok() && equal.comparison == 0 && key.ok() &&
              key.sort_key == expected_key && ordered,
          "structured uint32 partition agrees with independent byte/order oracle");
    return expected_key;
  };

  for (const std::uint32_t high : {0x0000u, 0x7fffu, 0x8000u, 0xffffu}) {
    bool have_previous = false;
    std::uint32_t previous = 0;
    std::string previous_key;
    for (std::uint32_t low = 0; low <= 0xffffu; ++low) {
      const std::uint32_t raw = (high << 16u) | low;
      previous_key = verify(raw, have_previous, previous, previous_key);
      previous = raw;
      have_previous = true;
    }
  }
  for (const std::uint32_t low :
       {0x0000u, 0x0001u, 0x7fffu, 0x8000u, 0xffffu}) {
    bool have_previous = false;
    std::uint32_t previous = 0;
    std::string previous_key;
    for (std::uint32_t high = 0; high <= 0xffffu; ++high) {
      const std::uint32_t raw = (high << 16u) | low;
      previous_key = verify(raw, have_previous, previous, previous_key);
      previous = raw;
      have_previous = true;
    }
  }
}

void NullAndAbsentPolicies() {
  auto descriptor = Uint32Descriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue null_value{
      dt::CanonicalTypeId::uint32, {}, true};
  null_value.descriptor = descriptor;

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::uint32, true, false, {}});
  const auto binary_back = binary_null.ok()
      ? dt::DecodeDatatypeBinaryValue(binary_null.encoded)
      : dt::DatatypeBinaryResult{};
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::uint32,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto physical_back = physical_null.ok()
      ? dt::DecodeDatatypePhysicalValue(physical_null.bytes.data(),
                                        physical_null.bytes.size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(binary_null.ok() && binary_back.ok() && binary_back.value.is_null &&
            binary_back.value.payload.empty() && physical_null.ok() &&
            physical_back.ok() &&
            physical_back.value.type_id == dt::CanonicalTypeId::uint32 &&
            physical_back.value.state ==
                dt::DatatypePhysicalValueState::sql_null &&
            physical_back.value.payload.empty(),
        "uint32 typed NULL is external state with zero payload");
  Check(!dt::EncodeDatatypeBinaryValue(
             {dt::CanonicalTypeId::uint32, true, false, {0,0,0,0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
             {dt::CanonicalTypeId::uint32,
              dt::DatatypePhysicalValueState::sql_null, {0,0,0,0}}).ok(),
        "uint32 codecs reject payload-bearing NULL");
  const auto present_zero = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::uint32, false, false, {0,0,0,0}});
  Check(present_zero.ok() && present_zero.encoded != binary_null.encoded,
        "present uint32 zero remains distinct from typed NULL");

  const auto serialized_null = dt::SerializeDatatypeValue({null_value});
  dt::DatatypeDeserializationRequest null_decode;
  null_decode.expected_type_id = dt::CanonicalTypeId::uint32;
  null_decode.expected_descriptor = descriptor;
  null_decode.serialized_value = serialized_null.serialized_value;
  const auto restored_null = dt::DeserializeDatatypeValue(null_decode);
  Check(serialized_null.ok() && restored_null.ok() &&
            restored_null.value.type_id == dt::CanonicalTypeId::uint32 &&
            restored_null.value.is_null &&
            restored_null.value.encoded_value.empty(),
        "generic value serialization preserves typed uint32 NULL state");
  null_decode.serialized_value =
      "SBDV1;type=uint32;state=null;payload=00000000";
  Check(!dt::DeserializeDatatypeValue(null_decode).ok(),
        "generic value deserialization rejects payload-bearing uint32 NULL");

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = null_value;
    identity.target_type_id = dt::CanonicalTypeId::uint32;
    identity.target_descriptor = descriptor;
    identity.context = context;
    identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto result = dt::CastDatatypeValue(identity);
    Check(result.ok() && result.category == dt::DatatypeCastCategory::identity &&
              result.value.is_null && result.value.encoded_value.empty() &&
              std::equal(
                  std::begin(result.value.descriptor.descriptor_uuid.bytes),
                  std::end(result.value.descriptor.descriptor_uuid.bytes),
                  std::begin(descriptor.descriptor_uuid.bytes)),
          "typed uint32 NULL identity preserves exact descriptor/state");

    identity.target_descriptor.security_policy_uuid = FixtureV7Uuid(0x90u);
    identity.target_descriptor.modifier_flags |=
        scratchbird::engine::ExecutionTypeModifierFlagBit(
            scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
    CheckCastRefused(dt::CastDatatypeValue(identity),
                     "DATATYPE.DESCRIPTOR.INVALID",
                     "typed uint32 NULL identity rejects descriptor mismatch first");

    auto mismatched_and_malformed = identity;
    mismatched_and_malformed.value.encoded_value = "payload";
    CheckCastRefused(dt::CastDatatypeValue(mismatched_and_malformed),
                     "DATATYPE.DESCRIPTOR.INVALID",
                     "uint32 descriptor mismatch precedes malformed NULL state");

    auto mismatched_and_nonnullable = identity;
    mismatched_and_nonnullable.target_descriptor.nullable_allowed = false;
    CheckCastRefused(dt::CastDatatypeValue(mismatched_and_nonnullable),
                     "DATATYPE.DESCRIPTOR.INVALID",
                     "uint32 descriptor mismatch precedes target nullability");

    auto mismatched_and_bad_context = identity;
    mismatched_and_bad_context.context =
        static_cast<dt::DatatypeCastContext>(0xffu);
    CheckCastRefused(dt::CastDatatypeValue(mismatched_and_bad_context),
                     "DATATYPE.DESCRIPTOR.INVALID",
                     "uint32 descriptor mismatch precedes cast context");

    auto malformed_state = identity;
    malformed_state.target_descriptor = descriptor;
    malformed_state.value.encoded_value = "payload";
    CheckCastRefused(dt::CastDatatypeValue(malformed_state),
                     "DATATYPE.NULL_STATE.INVALID",
                     "typed uint32 NULL with equal descriptors reports malformed state");

    auto nonnullable_target = identity;
    nonnullable_target.target_descriptor = descriptor;
    nonnullable_target.target_descriptor.nullable_allowed = false;
    nonnullable_target.value.descriptor = nonnullable_target.target_descriptor;
    CheckCastRefused(dt::CastDatatypeValue(nonnullable_target),
                     "DATATYPE.NULL_NOT_ADMITTED",
                     "typed uint32 NULL with equal descriptors reports nullability");

    auto bad_context = identity;
    bad_context.target_descriptor = descriptor;
    bad_context.context = static_cast<dt::DatatypeCastContext>(0xffu);
    CheckCastRefused(dt::CastDatatypeValue(bad_context),
                     "DATATYPE.CAST_FORBIDDEN",
                     "typed uint32 NULL with equal descriptors reports cast context");

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::uint32;
    contextual.target_descriptor = descriptor;
    contextual.context = context;
    contextual.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    const auto bound = dt::CastDatatypeValue(contextual);
    Check(bound.ok() && bound.value.type_id == dt::CanonicalTypeId::uint32 &&
              bound.value.is_null && bound.value.encoded_value.empty() &&
              std::equal(
                  std::begin(bound.value.descriptor.descriptor_uuid.bytes),
                  std::end(bound.value.descriptor.descriptor_uuid.bytes),
                  std::begin(descriptor.descriptor_uuid.bytes)),
          "contextual NULL binds to exact nullable uint32 descriptor");

    contextual.target_descriptor.nullable_allowed = false;
    CheckCastRefused(dt::CastDatatypeValue(contextual),
                     "DATATYPE.NULL_NOT_ADMITTED",
                     "contextual NULL refuses non-nullable uint32 descriptor");
    contextual.target_descriptor = descriptor;
    ++contextual.target_descriptor.descriptor_epoch;
    CheckCastRefused(dt::CastDatatypeValue(contextual),
                     "DATATYPE.DESCRIPTOR.INVALID",
                     "contextual NULL refuses mismatched uint32 generation");

    dt::DatatypeCastRequest standalone_target;
    standalone_target.value = Uint32(0);
    standalone_target.target_type_id = dt::CanonicalTypeId::null_type;
    standalone_target.context = context;
    standalone_target.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    CheckCastRefused(dt::CastDatatypeValue(standalone_target),
                     "DATATYPE.CAST_FORBIDDEN",
                     "present uint32 cannot target standalone NULL sentinel");
    standalone_target.value = null_value;
    CheckCastRefused(dt::CastDatatypeValue(standalone_target),
                     "DATATYPE.CAST_FORBIDDEN",
                     "typed uint32 NULL cannot target standalone NULL sentinel");
  }

  for (const auto& candidate : dt::BuiltinDatatypeDescriptors()) {
    Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint32,
                                   candidate.type_id) ==
              dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::uint32) ==
              dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint32,
                                       candidate.type_id, true) ==
              dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::uint32, true) ==
              dt::DatatypeCastCategory::forbidden,
          "all present-value casts incident to uint32 refuse with or without compatibility profile");
  }
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest present;
    present.value = Uint32(42);
    present.target_type_id = dt::CanonicalTypeId::uint64;
    present.context = context;
    present.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present uint32 outgoing cast refuses in every context");
    present.target_type_id = dt::CanonicalTypeId::uint32;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present uint32 identity cast refuses in every context");
    present.value = {dt::CanonicalTypeId::uint16,
                     std::string{'\x2a', '\0'}, false};
    present.target_type_id = dt::CanonicalTypeId::uint32;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present uint32 incoming cast refuses in every context");
    present.reference_compatibility_profile = true;
    Check(!dt::CastDatatypeValue(present).ok(),
          "compatibility profile cannot bypass uint32 present-cast refusal");
  }

  const auto int32_descriptor = DescriptorFor(dt::CanonicalTypeId::int32);
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    for (const bool compatibility_profile : {false, true}) {
      dt::DatatypeCastRequest cross_concrete_null;
      cross_concrete_null.value = null_value;
      cross_concrete_null.target_type_id = dt::CanonicalTypeId::int32;
      cross_concrete_null.target_descriptor = int32_descriptor;
      cross_concrete_null.context = context;
      cross_concrete_null.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      cross_concrete_null.reference_compatibility_profile =
          compatibility_profile;
      CheckCastRefused(
          dt::CastDatatypeValue(cross_concrete_null),
          "DATATYPE.CAST_FORBIDDEN",
          "typed uint32 NULL cannot cross to int32 in any cast context/profile");

      cross_concrete_null.value = {
          dt::CanonicalTypeId::int32, {}, true, int32_descriptor};
      cross_concrete_null.target_type_id = dt::CanonicalTypeId::uint32;
      cross_concrete_null.target_descriptor = descriptor;
      CheckCastRefused(
          dt::CastDatatypeValue(cross_concrete_null),
          "DATATYPE.CAST_FORBIDDEN",
          "typed int32 NULL cannot cross to uint32 in any cast context/profile");
    }
  }

  for (const auto operation : {
           dt::DatatypeNumericOperationKind::canonicalize,
           dt::DatatypeNumericOperationKind::add,
           dt::DatatypeNumericOperationKind::subtract,
           dt::DatatypeNumericOperationKind::multiply,
           dt::DatatypeNumericOperationKind::divide,
           dt::DatatypeNumericOperationKind::compare}) {
    dt::DatatypeNumericOperationRequest numeric;
    numeric.operation = operation;
    numeric.type_id = dt::CanonicalTypeId::uint32;
    numeric.left = Uint32(6);
    numeric.right = Uint32(2);
    Check(!dt::ApplyNumericOperation(numeric).ok(),
          "uint32 numeric operation refuses until arithmetic policy exists");
  }

  auto described_left = Uint32(1);
  described_left.descriptor = descriptor;
  auto described_right = Uint32(2);
  described_right.descriptor = descriptor;
  Check(dt::CompareDatatypeValues({described_left, described_right}).ok(),
        "uint32 comparison admits the same execution descriptor");
  const auto descriptorless_right = Uint32(2);
  const auto one_sided_left =
      dt::CompareDatatypeValues({described_left, descriptorless_right});
  const auto one_sided_right =
      dt::CompareDatatypeValues({descriptorless_right, described_left});
  Check(!one_sided_left.ok() && !one_sided_right.ok() &&
            one_sided_left.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            one_sided_right.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID",
        "uint32 comparison rejects one-sided execution descriptor identity");
  described_right.descriptor.nullable_allowed = false;
  const auto descriptor_mismatch =
      dt::CompareDatatypeValues({described_left, described_right});
  dt::DatatypeComparisonRequest mismatch_and_bad_ordering{
      described_left, described_right,
      static_cast<dt::DatatypeNullOrdering>(0xffu)};
  const auto descriptor_precedence =
      dt::CompareDatatypeValues(mismatch_and_bad_ordering);
  Check(!descriptor_mismatch.ok() &&
            descriptor_mismatch.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            !descriptor_precedence.ok() &&
            descriptor_precedence.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID",
        "uint32 comparison rejects unequal valid execution descriptors");

  auto zero_with_descriptor = Uint32(0);
  zero_with_descriptor.descriptor = descriptor;
  auto max_with_descriptor = Uint32(4294967295ULL);
  max_with_descriptor.descriptor = descriptor;
  const auto null_first = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_last});
  const auto first_compare = dt::CompareDatatypeValues(
      {null_value, zero_with_descriptor, dt::DatatypeNullOrdering::nulls_first});
  const auto last_compare = dt::CompareDatatypeValues(
      {null_value, max_with_descriptor,
       dt::DatatypeNullOrdering::nulls_last});
  Check(null_first.ok() && null_first.sort_key == std::string(1, '\0') &&
            null_last.ok() && null_last.sort_key == std::string(1, '\2'),
        "uint32 keys honor explicit containing NULL placement");
  const auto null_pair =
      dt::CompareDatatypeValues({null_value, null_value});
  Check(!first_compare.ok() &&
            first_compare.diagnostic.diagnostic_code ==
                "SB_DATATYPE_COMPARISON_REJECTED" &&
            !last_compare.ok() &&
            last_compare.diagnostic.diagnostic_code ==
                "SB_DATATYPE_COMPARISON_REJECTED" &&
            !null_pair.ok() &&
            null_pair.diagnostic.diagnostic_code ==
                "SB_DATATYPE_COMPARISON_REJECTED",
        "uint32 NULL comparison refuses without owning operator policy");
  Check(!dt::HashDatatypeValue({Uint32(42)}).ok() &&
            !dt::HashDatatypeValue({null_value}).ok() &&
            !dt::RenderDatatypeValueForDisplay({Uint32(42)}).ok(),
        "undefined uint32 hash and display policies refuse");
  Check(!dt::CompareDatatypeValues({
             Uint32(42),
             {dt::CanonicalTypeId::int32,
              std::string{'\x2a', '\0', '\0', '\0'}, false}}).ok(),
        "mixed unsigned/signed comparison refuses without registered rule");
}

template <std::size_t N>
void Append(std::vector<platform::byte>* output,
            const std::array<platform::byte, N>& bytes) {
  output->insert(output->end(), bytes.begin(), bytes.end());
}

void Persistence() {
  constexpr platform::Uuid descriptor_uuid{{
      0x7a,0,0,0,0x75,0x69,0x7e,0x74,0xb3,0x32,0,0,0,0,0,0}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x08}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x09}};
  constexpr std::array<platform::byte,28> value_zero{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7a,0,0,0,1,0,0,0,
      4,0,0,0,0x04,0x70,0xcc,0x4e,0,0,0,0}};
  constexpr std::array<platform::byte,28> value_one{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7a,0,0,0,1,0,0,0,
      4,0,0,0,0x15,0xaf,0xf8,0x9e,1,0,0,0}};
  constexpr std::array<platform::byte,28> value_mid{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7a,0,0,0,1,0,0,0,
      4,0,0,0,0x84,0x39,0xcd,0xce,0,0,0,0x80}};
  constexpr std::array<platform::byte,28> value_max{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7a,0,0,0,1,0,0,0,
      4,0,0,0,0xa0,0xc4,0xbd,0xe1,0xff,0xff,0xff,0xff}};
  constexpr std::array<platform::byte,24> sql_null{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7a,0,0,0,0,0,0,0,
      0,0,0,0,0x57,0xb9,0x67,0x2d}};

  struct Oracle {
    std::uint64_t value;
    const platform::byte* bytes;
    std::size_t size;
  };
  const std::array<Oracle,4> oracles{{
      {0ULL,value_zero.data(),value_zero.size()},
      {1ULL,value_one.data(),value_one.size()},
      {2147483648ULL,value_mid.data(),value_mid.size()},
      {4294967295ULL,value_max.data(),value_max.size()}}};
  for (const auto& oracle : oracles) {
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint32, dt::DatatypePhysicalValueState::value,
         Payload(Uint32(oracle.value))});
    Check(encoded.ok() && encoded.bytes.size() == oracle.size &&
              std::equal(encoded.bytes.begin(), encoded.bytes.end(), oracle.bytes),
          "production uint32 physical frame matches independent hard-coded oracle");
  }
  const auto encoded_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::uint32,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(encoded_null.ok() && encoded_null.bytes.size() == sql_null.size() &&
            std::equal(encoded_null.bytes.begin(), encoded_null.bytes.end(),
                       sql_null.begin()),
        "production uint32 NULL frame matches independent hard-coded oracle");

  constexpr std::size_t header_bytes = 112;
  std::vector<platform::byte> expected(header_bytes, 0);
  // This envelope belongs only to the component fixture. The embedded frames
  // above use the production physical codec; no table/page format is claimed.
  const std::array<platform::byte,8> magic{
      {'S','B','U','3','2','V','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(descriptor_uuid.bytes.begin(), descriptor_uuid.bytes.end(),
            expected.begin() + 8);
  std::copy(type_uuid.bytes.begin(), type_uuid.bytes.end(),
            expected.begin() + 24);
  std::copy(codec_uuid.bytes.begin(), codec_uuid.bytes.end(),
            expected.begin() + 40);
  platform::StoreLittle32(expected.data() + 56, 1);
  platform::StoreLittle32(expected.data() + 60, 1);
  platform::StoreLittle32(expected.data() + 64, 1);
  platform::StoreLittle32(expected.data() + 68, 5);
  std::uint32_t offset = header_bytes;
  for (unsigned index = 0; index < 5; ++index) {
    const std::uint32_t size = index == 4 ? 24 : 28;
    platform::StoreLittle32(expected.data() + 72 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 76 + index * 8, size);
    offset += size;
  }
  Append(&expected, value_zero);
  Append(&expected, value_one);
  Append(&expected, value_mid);
  Append(&expected, value_max);
  Append(&expected, sql_null);

#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-uint32-" + std::to_string(pid) + ".codec");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create uint32 persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact uint32 bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen uint32 persistence fixture with independent handle");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected &&
      std::equal(actual.begin() + 8, actual.begin() + 24,
                 descriptor_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 24, actual.begin() + 40,
                 type_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 40, actual.begin() + 56,
                 codec_uuid.bytes.begin()) &&
      platform::LoadLittle32(actual.data() + 56) == 1 &&
      platform::LoadLittle32(actual.data() + 60) == 1 &&
      platform::LoadLittle32(actual.data() + 64) == 1 &&
      platform::LoadLittle32(actual.data() + 68) == 5;
  for (unsigned index = 0; index < 5 && decoded_all; ++index) {
    const auto frame_offset =
        platform::LoadLittle32(actual.data() + 72 + index * 8);
    const auto frame_size =
        platform::LoadLittle32(actual.data() + 76 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::uint32 &&
        (index == 4
             ? decoded.value.state == dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state == dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Uint32(oracles[index].value)));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "test-owned envelope independently reopens exact uint32 identities and production frames");

  std::array<platform::byte,2> short_bytes{};
  const auto short_read = reader.ReadAt(
      actual.size() - 1, short_bytes.data(), short_bytes.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_bytes.size(),
        "uint32 persistence boundary refuses short read");
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only uint32 persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened uint32 fixture");

  for (const auto& oracle : oracles) {
    std::vector<platform::byte> corrupt(oracle.bytes, oracle.bytes + oracle.size);
    corrupt[20] ^= 0x01;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(oracle.bytes,
                                                oracle.size - 1).ok(),
          "uint32 physical decoder rejects checksum corruption and truncation");
  }
  auto corrupt_null = sql_null;
  corrupt_null[20] ^= 0x01;
  Check(!dt::DecodeDatatypePhysicalValue(
             corrupt_null.data(), corrupt_null.size()).ok() &&
            !dt::DecodeDatatypePhysicalValue(
             sql_null.data(), sql_null.size() - 1).ok(),
        "uint32 NULL physical frame rejects checksum corruption and truncation");
}

}  // namespace

int main() {
  ExactIdentity();
  RepresentationAndCodecs();
  StructuredPropertyPartitions();
  NullAndAbsentPolicies();
  Persistence();
  std::cout << "base uint32 checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
