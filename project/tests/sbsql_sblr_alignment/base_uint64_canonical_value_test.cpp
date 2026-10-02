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
#include "uuid.hpp"

#include <algorithm>
#include <bit>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
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

scratchbird::engine::ExecutionTypeDescriptor Uint64Descriptor() {
  return DescriptorFor(dt::CanonicalTypeId::uint64);
}

dt::DatatypeOperationValue Uint64(std::uint64_t value) {
  std::string encoded;
  if (!dt::EncodeCanonicalUint64Value(value, &encoded)) return {};
  dt::DatatypeOperationValue result{
      dt::CanonicalTypeId::uint64, std::move(encoded), false};
  static const auto descriptor = Uint64Descriptor();
  result.descriptor = descriptor;
  return result;
}

std::vector<platform::byte> Payload(const dt::DatatypeOperationValue& value) {
  return {value.encoded_value.begin(), value.encoded_value.end()};
}

void ExactIdentity() {
  constexpr platform::Uuid descriptor_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x3a}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x3b}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x3c}};

  const auto descriptor = Uint64Descriptor();
  Check(std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                   std::end(descriptor.descriptor_uuid.bytes),
                   descriptor_uuid.bytes.begin()) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.stable_name == "uint64" &&
            descriptor.bit_width == 64,
        "exact uint64 UUID descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_snapshot_uuid == dt::kDatatypeCohortV5 &&
        row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::uint64);
  });
  Check(found != rows.end() && found->descriptor_uuid == descriptor_uuid &&
            found->type_uuid == type_uuid && found->codec_uuid == codec_uuid &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.uint64.le.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 8 &&
            found->canonical_value_maximum_bytes == 8 &&
            found->canonical_value_exact_bytes == 8 && found->null_supported &&
            !found->signed_code &&
            found->canonical_byte_order == "little_endian" &&
            found->canonical_representation == "unsigned_64_bit_integer",
        "exact uint64 type-codec tuple");

  for (const auto cohort : std::array{
           std::tuple{dt::kDatatypeCohortV2, 2ULL, 2ULL},
           std::tuple{dt::kDatatypeCohortV3, 3ULL, 3ULL},
           std::tuple{dt::kDatatypeCohortV4, 4ULL, 4ULL},
           std::tuple{dt::kDatatypeCohortV5, 5ULL, 5ULL}}) {
    const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
        std::get<0>(cohort), std::get<1>(cohort), std::get<2>(cohort),
        descriptor_uuid, 1);
    Check(admitted.ok && admitted.row.type_uuid == type_uuid &&
              admitted.row.codec_uuid == codec_uuid,
          "uint64 exact identity survives admitted cohort inheritance");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV1, 1, 1, descriptor_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV2, 3, 3, descriptor_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 2).ok,
        "uint64 identity rejects V1 and crossed generations");
  dt::DatatypeStorageIdentityV1 storage_identity;
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, type_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, platform::Uuid{}, 1).ok,
        "normal uint64 lookup rejects wrong-role and nil identities");

  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 1,
            &storage_identity) &&
            storage_identity.descriptor_uuid == descriptor_uuid &&
            storage_identity.type_uuid == type_uuid &&
            storage_identity.type_id == dt::CanonicalTypeId::uint64 &&
            storage_identity.codec.has_value() &&
            storage_identity.codec->codec_uuid == codec_uuid,
        "uint64 storage identity preserves exact descriptor/type/codec fields");
  const auto layout = dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::uint64);
  Check(layout.ok() &&
            layout.layout.storage_class == dt::DatatypeStorageClass::inline_fixed &&
            layout.layout.encoding == dt::DatatypeBinaryEncoding::unsigned_little_endian &&
            layout.layout.inline_bytes == 8 && layout.layout.alignment_bytes == 8,
        "uint64 storage layout is fixed unsigned LE8");
}

std::array<platform::byte, 8> OracleBytes(std::uint64_t value) {
  std::array<platform::byte, 8> bytes{};
  for (unsigned byte = 0; byte < 8; ++byte) {
    bytes[byte] = static_cast<platform::byte>((value >> (byte * 8u)) & 0xffu);
  }
  return bytes;
}

std::string OracleKey(std::uint64_t value) {
  std::string key(9, '\0');
  key[0] = '\1';
  for (unsigned byte = 0; byte < 8; ++byte) {
    const unsigned shift = (7u - byte) * 8u;
    const auto octet = static_cast<unsigned char>((value >> shift) & 0xffu);
    key[byte + 1] = static_cast<char>(octet);
  }
  return key;
}

void RepresentationAndCodecs() {
  constexpr std::array<std::uint64_t, 24> vectors{{
      0ULL, 1ULL, 2ULL, 127ULL, 128ULL, 255ULL, 256ULL,
      32767ULL, 32768ULL, 65535ULL, 65536ULL,
      2147483647ULL, 2147483648ULL,
      4294967295ULL, 4294967296ULL,
      281474976710655ULL, 281474976710656ULL,
      72057594037927935ULL, 72057594037927936ULL,
      9223372036854775807ULL, 9223372036854775808ULL,
      18446744073709551613ULL, 18446744073709551614ULL,
      std::numeric_limits<std::uint64_t>::max()}};

  std::string previous_key;
  for (std::size_t index = 0; index < vectors.size(); ++index) {
    const auto number = vectors[index];
    const auto value = Uint64(number);
    const auto bytes = OracleBytes(number);
    const std::vector<platform::byte> expected(bytes.begin(), bytes.end());
    std::string encoded;
    std::uint64_t decoded = 0;
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::uint64, false, false, expected});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint64,
         dt::DatatypePhysicalValueState::value, expected});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(), physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto comparison = dt::CompareDatatypeValues({value, value});
    const auto key = dt::MakeDatatypeSortKey({value});
    Check(dt::EncodeCanonicalUint64Value(number, &encoded) &&
              encoded == value.encoded_value && Payload(value) == expected &&
              dt::DecodeCanonicalUint64Value(encoded, &decoded) &&
              decoded == number && binary.ok() && binary_back.ok() &&
              binary_back.value.payload == expected && physical.ok() &&
              physical_back.ok() && physical_back.value.payload == expected &&
              comparison.ok() && comparison.comparison == 0 && key.ok() &&
              key.sort_key == OracleKey(number),
          "uint64 canonical, binary, physical, comparison, and key vector");
    if (index != 0) {
      const auto ordered = dt::CompareDatatypeValues(
          {Uint64(vectors[index - 1]), value});
      Check(ordered.ok() && ordered.comparison == -1 &&
                previous_key < key.sort_key,
            "uint64 comparison and key order adjacent boundary vectors");
    }
    previous_key = key.sort_key;
  }

  std::uint64_t state = 0x9e3779b97f4a7c15ULL;
  std::uint64_t previous = std::numeric_limits<std::uint64_t>::min();
  previous_key = dt::MakeDatatypeSortKey({Uint64(previous)}).sort_key;
  for (unsigned index = 0; index < 100000; ++index) {
    state ^= state >> 12u;
    state ^= state << 25u;
    state ^= state >> 27u;
    state *= 2685821657736338717ULL;
    const auto candidate = state;
    std::string encoded;
    std::uint64_t decoded = 0;
    const auto key = dt::MakeDatatypeSortKey({Uint64(candidate)});
    const auto compare = dt::CompareDatatypeValues({Uint64(previous), Uint64(candidate)});
    Check(dt::EncodeCanonicalUint64Value(candidate, &encoded) &&
              dt::DecodeCanonicalUint64Value(encoded, &decoded) &&
              decoded == candidate && key.ok() && compare.ok() &&
              compare.comparison == (previous < candidate ? -1 : previous > candidate ? 1 : 0) &&
              ((previous_key < key.sort_key) == (previous < candidate)) &&
              ((previous_key > key.sort_key) == (previous > candidate)),
          "deterministic uint64 sample preserves value/comparison/key order");
    previous = candidate;
    previous_key = key.sort_key;
  }

  std::string unchanged = "sentinel";
  Check(dt::EncodeCanonicalUint64Value(std::numeric_limits<std::uint64_t>::min(), &unchanged) &&
            unchanged == std::string(8, '\0') &&
            dt::EncodeCanonicalUint64Value(std::numeric_limits<std::uint64_t>::max(), &unchanged) &&
            unchanged == std::string(8, static_cast<char>(0xff)) &&
            !dt::EncodeCanonicalUint64Value(0, nullptr),
        "uint64 mathematical encoder covers its full typed domain and rejects null output");
  for (const auto width : {0u,1u,2u,3u,4u,5u,6u,7u,9u,32u}) {
    const std::string malformed(width, '\0');
    std::uint64_t unchanged_number = 0x1122334455667788LL;
    Check(!dt::DecodeCanonicalUint64Value(malformed, &unchanged_number) &&
              unchanged_number == 0x1122334455667788LL,
          "uint64 mathematical decoder rejects malformed width atomically");
    const std::vector<platform::byte> bad(width, 0);
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::uint64, false, false, bad}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::uint64,
                dt::DatatypePhysicalValueState::value, bad}).ok(),
          "uint64 codecs reject non-eight-byte present payload");
    const dt::DatatypeOperationValue operation{
        dt::CanonicalTypeId::uint64, malformed, false};
    Check(!dt::CompareDatatypeValues({operation, Uint64(0)}).ok() &&
              !dt::MakeDatatypeSortKey({operation}).ok() &&
              !dt::SerializeDatatypeValue({operation}).ok() &&
              !dt::HashDatatypeValue({operation}).ok() &&
              !dt::RenderDatatypeValueForDisplay({operation}).ok(),
          "uint64 operation surfaces reject malformed carrier widths");
  }
  Check(!dt::DecodeCanonicalUint64Value(std::string(8, '\0'), nullptr),
        "uint64 decoder refuses a null output pointer");

  dt::DatatypeExtractRequest extract;
  std::string uuidv7(16, '\0');
  uuidv7[6] = static_cast<char>(0x70);
  uuidv7[8] = static_cast<char>(0x80);
  extract.value = {dt::CanonicalTypeId::uuid, uuidv7, false};
  extract.value.descriptor = DescriptorFor(dt::CanonicalTypeId::uuid);
  extract.field = "uuidv7_unix_millis";
  extract.result_descriptor = Uint64Descriptor();
  const auto extract_result = dt::ExtractDatatypeField(extract);
  const auto* extract_detail = extract_result.diagnostic.arguments.size() == 1
      ? extract_result.diagnostic.arguments.front().text()
      : nullptr;
  Check(!extract_result.ok() &&
            extract_result.diagnostic.diagnostic_code ==
                "SB_DATATYPE_EXTRACT_REJECTED" &&
            extract_detail != nullptr &&
            *extract_detail == "uuid_extract_policy_unresolved" &&
            extract_result.value.type_id == dt::CanonicalTypeId::unknown &&
            !extract_result.value.is_null &&
            extract_result.value.encoded_value.empty(),
        "UUIDv7 extraction refuses without an identity/extraction policy");

  std::uint64_t text_like = 0;
  Check(dt::DecodeCanonicalUint64Value(std::string{"12345678", 8}, &text_like) &&
            text_like == 4050765991979987505ULL,
        "eight text-like bytes remain native uint64 0x3837363534333231");

  for (const auto number : vectors) {
    const auto input = Uint64(number);
    const auto serialized = dt::SerializeDatatypeValue({input});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::uint64;
    request.expected_descriptor = input.descriptor;
    request.serialized_value = serialized.serialized_value;
    const auto restored = dt::DeserializeDatatypeValue(request);
    Check(serialized.ok() && restored.ok() &&
              restored.value.type_id == dt::CanonicalTypeId::uint64 &&
              !restored.value.is_null &&
              restored.value.encoded_value == input.encoded_value &&
              std::equal(
                  std::begin(serialized.descriptor.descriptor_uuid.bytes),
                  std::end(serialized.descriptor.descriptor_uuid.bytes),
                  std::begin(input.descriptor.descriptor_uuid.bytes)) &&
              std::equal(
                  std::begin(restored.value.descriptor.descriptor_uuid.bytes),
                  std::end(restored.value.descriptor.descriptor_uuid.bytes),
                  std::begin(input.descriptor.descriptor_uuid.bytes)),
          "generic value serialization preserves uint64 UUID and native bytes");
  }
  for (const char* bad : {"SBDV1;type=uint64;state=value;payload=",
                          "SBDV1;type=uint64;state=value;payload=00",
                          "SBDV1;type=uint64;state=value;payload=00000000000000",
                          "SBDV1;type=uint64;state=value;payload=000000000000000000"}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::uint64;
    request.serialized_value = bad;
    Check(!dt::DeserializeDatatypeValue(request).ok(),
          "generic value deserializer rejects malformed uint64 widths");
  }
}

void StructuredPropertyPartitions() {
  const auto verify = [](std::uint64_t raw, bool have_previous,
                         std::uint64_t previous_value,
                         const std::string& previous_key) {
    const auto expected_value = raw;
    const auto expected_array = OracleBytes(expected_value);
    const std::string expected_bytes(expected_array.begin(), expected_array.end());
    const std::string expected_key = OracleKey(expected_value);
    std::string encoded;
    std::uint64_t decoded = 0;
    const auto value = Uint64(expected_value);
    const std::vector<platform::byte> payload(expected_array.begin(), expected_array.end());
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::uint64, false, false, payload});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint64,
         dt::DatatypePhysicalValueState::value, payload});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(), physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto equal = dt::CompareDatatypeValues({value, value});
    const auto key = dt::MakeDatatypeSortKey({value});
    bool ordered = true;
    if (have_previous) {
      const auto forward = dt::CompareDatatypeValues({Uint64(previous_value), value});
      const auto reverse = dt::CompareDatatypeValues({value, Uint64(previous_value)});
      ordered = previous_value < expected_value && forward.ok() &&
          forward.comparison == -1 && reverse.ok() && reverse.comparison == 1 &&
          previous_key < key.sort_key;
    }
    Check(dt::EncodeCanonicalUint64Value(expected_value, &encoded) &&
              encoded == expected_bytes &&
              dt::DecodeCanonicalUint64Value(encoded, &decoded) &&
              decoded == expected_value && binary.ok() && binary_back.ok() &&
              binary_back.value.payload == payload && physical.ok() &&
              physical_back.ok() && physical_back.value.payload == payload &&
              equal.ok() && equal.comparison == 0 && key.ok() &&
              key.sort_key == expected_key && ordered,
          "structured uint64 partition agrees with independent byte/order oracle");
    return std::pair{expected_value, expected_key};
  };

  constexpr std::array<std::uint64_t,4> anchors{{
      0x0000000000000000ULL, 0x01234567000089abULL,
      0x8123000089abcdefULL, 0xffff012345670000ULL}};
  for (unsigned word = 0; word < 4; ++word) {
    bool have_previous = false;
    std::uint64_t previous_value = 0;
    std::string previous_key;
    const auto mask = ~(0xffffULL << (word * 16u));
    for (std::uint64_t ordinal = 0; ordinal <= 0xffffULL; ++ordinal) {
      const auto raw = (anchors[word] & mask) | (ordinal << (word * 16u));
      const auto observed = verify(raw, have_previous, previous_value, previous_key);
      previous_value = observed.first;
      previous_key = observed.second;
      have_previous = true;
    }
  }
}

void NullAndAbsentPolicies() {
  auto descriptor = Uint64Descriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue null_value{
      dt::CanonicalTypeId::uint64, {}, true};
  null_value.descriptor = descriptor;

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::uint64, true, false, {}});
  const auto binary_back = binary_null.ok()
      ? dt::DecodeDatatypeBinaryValue(binary_null.encoded)
      : dt::DatatypeBinaryResult{};
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::uint64,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto physical_back = physical_null.ok()
      ? dt::DecodeDatatypePhysicalValue(physical_null.bytes.data(),
                                        physical_null.bytes.size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(binary_null.ok() && binary_back.ok() && binary_back.value.is_null &&
            binary_back.value.payload.empty() && physical_null.ok() &&
            physical_back.ok() &&
            physical_back.value.type_id == dt::CanonicalTypeId::uint64 &&
            physical_back.value.state ==
                dt::DatatypePhysicalValueState::sql_null &&
            physical_back.value.payload.empty(),
        "uint64 typed NULL is external state with zero payload");
  Check(!dt::EncodeDatatypeBinaryValue(
             {dt::CanonicalTypeId::uint64, true, false, {0,0,0,0,0,0,0,0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
             {dt::CanonicalTypeId::uint64,
              dt::DatatypePhysicalValueState::sql_null, {0,0,0,0,0,0,0,0}}).ok(),
        "uint64 codecs reject payload-bearing NULL");
  const auto present_zero = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::uint64, false, false, {0,0,0,0,0,0,0,0}});
  Check(present_zero.ok() && present_zero.encoded != binary_null.encoded,
        "present uint64 zero remains distinct from typed NULL");

  const auto serialized_null = dt::SerializeDatatypeValue({null_value});
  dt::DatatypeDeserializationRequest null_decode;
  null_decode.expected_type_id = dt::CanonicalTypeId::uint64;
  null_decode.expected_descriptor = descriptor;
  null_decode.serialized_value = serialized_null.serialized_value;
  const auto restored_null = dt::DeserializeDatatypeValue(null_decode);
  Check(serialized_null.ok() && restored_null.ok() &&
            restored_null.value.type_id == dt::CanonicalTypeId::uint64 &&
            restored_null.value.is_null &&
            restored_null.value.encoded_value.empty(),
        "generic value serialization preserves typed uint64 NULL state");
  null_decode.serialized_value =
      "SBDV1;type=uint64;state=null;payload=0000000000000000";
  Check(!dt::DeserializeDatatypeValue(null_decode).ok(),
        "generic value deserialization rejects payload-bearing uint64 NULL");

  auto uuid_descriptor = DescriptorFor(dt::CanonicalTypeId::uuid);
  uuid_descriptor.nullable_allowed = true;
  dt::DatatypeExtractRequest null_uuid_extract;
  null_uuid_extract.value = {dt::CanonicalTypeId::uuid, {}, true};
  null_uuid_extract.value.descriptor = uuid_descriptor;
  null_uuid_extract.field = "uuidv7_unix_millis";
  null_uuid_extract.result_descriptor = descriptor;
  const auto null_uuid_result = dt::ExtractDatatypeField(null_uuid_extract);
  const auto* null_uuid_detail =
      null_uuid_result.diagnostic.arguments.size() == 1
          ? null_uuid_result.diagnostic.arguments.front().text()
          : nullptr;
  Check(!null_uuid_result.ok() &&
            null_uuid_result.diagnostic.diagnostic_code ==
                "SB_DATATYPE_EXTRACT_REJECTED" &&
            null_uuid_detail != nullptr &&
            *null_uuid_detail == "uuid_extract_policy_unresolved" &&
            null_uuid_result.value.type_id == dt::CanonicalTypeId::unknown &&
            !null_uuid_result.value.is_null &&
            null_uuid_result.value.encoded_value.empty(),
        "typed-NULL UUIDv7 extraction refuses without an extraction policy");

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = null_value;
    identity.target_type_id = dt::CanonicalTypeId::uint64;
    identity.target_descriptor = descriptor;
    identity.context = context;
    identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto result = dt::CastDatatypeValue(identity);
    Check(result.ok() && result.category == dt::DatatypeCastCategory::identity &&
              result.value.is_null && result.value.encoded_value.empty(),
          "typed uint64 NULL identity validates exact descriptor/state");

    identity.target_descriptor.security_policy_uuid = FixtureV7Uuid(0x90u);
    identity.target_descriptor.modifier_flags |=
        scratchbird::engine::ExecutionTypeModifierFlagBit(
            scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
    CheckCastRefused(
        dt::CastDatatypeValue(identity), "DATATYPE.DESCRIPTOR.INVALID",
        "typed uint64 NULL identity rejects a mismatched descriptor before cast policy");

    auto mismatched_and_malformed = identity;
    mismatched_and_malformed.value.encoded_value = "payload";
    CheckCastRefused(
        dt::CastDatatypeValue(mismatched_and_malformed),
        "DATATYPE.DESCRIPTOR.INVALID",
        "typed uint64 NULL descriptor mismatch precedes malformed NULL state");

    auto mismatched_and_nonnullable = identity;
    mismatched_and_nonnullable.target_descriptor.nullable_allowed = false;
    CheckCastRefused(
        dt::CastDatatypeValue(mismatched_and_nonnullable),
        "DATATYPE.DESCRIPTOR.INVALID",
        "typed uint64 NULL descriptor mismatch precedes target nullability");

    auto mismatched_and_bad_context = identity;
    mismatched_and_bad_context.context =
        static_cast<dt::DatatypeCastContext>(0xffu);
    CheckCastRefused(
        dt::CastDatatypeValue(mismatched_and_bad_context),
        "DATATYPE.DESCRIPTOR.INVALID",
        "typed uint64 NULL descriptor mismatch precedes cast context");

    auto malformed_state = identity;
    malformed_state.target_descriptor = descriptor;
    malformed_state.value.encoded_value = "payload";
    CheckCastRefused(
        dt::CastDatatypeValue(malformed_state), "DATATYPE.NULL_STATE.INVALID",
        "typed uint64 NULL with equal descriptors reports malformed state");

    auto nonnullable_target = identity;
    nonnullable_target.target_descriptor = descriptor;
    nonnullable_target.target_descriptor.nullable_allowed = false;
    nonnullable_target.value.descriptor =
        nonnullable_target.target_descriptor;
    CheckCastRefused(
        dt::CastDatatypeValue(nonnullable_target),
        "DATATYPE.NULL_NOT_ADMITTED",
        "typed uint64 NULL with equal descriptors reports target nullability");

    auto bad_context = identity;
    bad_context.target_descriptor = descriptor;
    bad_context.context = static_cast<dt::DatatypeCastContext>(0xffu);
    CheckCastRefused(
        dt::CastDatatypeValue(bad_context), "DATATYPE.CAST_FORBIDDEN",
        "typed uint64 NULL with equal descriptors reports invalid cast context");
  }

  for (const auto sibling_type : {dt::CanonicalTypeId::int16,
                                  dt::CanonicalTypeId::uint16}) {
    auto sibling_descriptor = DescriptorFor(sibling_type);
    sibling_descriptor.nullable_allowed = true;
    dt::DatatypeOperationValue sibling_null{sibling_type, {}, true};
    sibling_null.descriptor = sibling_descriptor;
    auto mismatched_descriptor = sibling_descriptor;
    mismatched_descriptor.security_policy_uuid = FixtureV7Uuid(0xa0u);
    mismatched_descriptor.modifier_flags |=
        scratchbird::engine::ExecutionTypeModifierFlagBit(
            scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      dt::DatatypeCastRequest sibling_identity;
      sibling_identity.value = sibling_null;
      sibling_identity.target_type_id = sibling_type;
      sibling_identity.target_descriptor = mismatched_descriptor;
      sibling_identity.context = context;
      sibling_identity.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      CheckCastRefused(
          dt::CastDatatypeValue(sibling_identity),
          "DATATYPE.DESCRIPTOR.INVALID",
          "bounded typed NULL identity descriptor mismatch keeps descriptor precedence");
    }
  }
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::uint64;
    contextual.target_descriptor = descriptor;
    contextual.context = context;
    contextual.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    const auto bound = dt::CastDatatypeValue(contextual);
    Check(bound.ok() && bound.value.type_id == dt::CanonicalTypeId::uint64 &&
              bound.value.is_null && bound.value.encoded_value.empty(),
          "contextual NULL binds to an exact nullable uint64 descriptor");

    contextual.target_descriptor.nullable_allowed = false;
    CheckCastRefused(dt::CastDatatypeValue(contextual),
                     "DATATYPE.NULL_NOT_ADMITTED",
                     "contextual NULL refuses a non-nullable uint64 descriptor");
    contextual.target_descriptor = descriptor;
    ++contextual.target_descriptor.descriptor_epoch;
    CheckCastRefused(
        dt::CastDatatypeValue(contextual), "DATATYPE.DESCRIPTOR.INVALID",
        "contextual NULL refuses a mismatched uint64 descriptor generation");

    dt::DatatypeCastRequest standalone_target;
    standalone_target.value = Uint64(0);
    standalone_target.target_type_id = dt::CanonicalTypeId::null_type;
    standalone_target.context = context;
    standalone_target.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    CheckCastRefused(dt::CastDatatypeValue(standalone_target),
                     "DATATYPE.CAST_FORBIDDEN",
                     "present uint64 cannot cast to the standalone NULL sentinel");
    standalone_target.value = null_value;
    CheckCastRefused(
        dt::CastDatatypeValue(standalone_target), "DATATYPE.CAST_FORBIDDEN",
        "typed uint64 NULL cannot cast to the standalone NULL sentinel");
  }

  auto bigint_alias = descriptor;
  bigint_alias.stable_name = "unsigned bigint";
  auto localized_alias = descriptor;
  localized_alias.stable_name = "entier_non_signe_64";
  auto left_alias = Uint64(0);
  auto right_alias = Uint64(std::numeric_limits<std::uint64_t>::max());
  left_alias.descriptor = bigint_alias;
  right_alias.descriptor = localized_alias;
  const auto alias_comparison =
      dt::CompareDatatypeValues({left_alias, right_alias});
  Check(alias_comparison.ok() && alias_comparison.comparison == -1,
        "uint64 aliases preserve UUID-bound comparison identity");

  auto alias_null = null_value;
  alias_null.descriptor = bigint_alias;
  dt::DatatypeCastRequest alias_identity;
  alias_identity.value = alias_null;
  alias_identity.target_type_id = dt::CanonicalTypeId::uint64;
  alias_identity.target_descriptor = localized_alias;
  const auto alias_cast = dt::CastDatatypeValue(alias_identity);
  Check(alias_cast.ok() && alias_cast.value.is_null &&
            alias_cast.value.descriptor.stable_name == "unsigned bigint",
        "uint64 aliases preserve UUID-bound typed NULL identity");

  const dt::DatatypeOperationValue descriptorless{
      dt::CanonicalTypeId::uint64, std::string(8, '\0'), false};
  Check(!dt::CompareDatatypeValues({descriptorless, descriptorless}).ok(),
        "uint64 comparison refuses values without UUID-bound descriptors");
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "unsigned bigint";
  Check(!dt::CompareDatatypeValues({label_only, label_only}).ok() &&
            !dt::SerializeDatatypeValue({label_only}).ok(),
        "uint64 label-only pseudo-descriptor cannot replace UUID identity");

  for (const auto& candidate : dt::BuiltinDatatypeDescriptors()) {
    Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint64,
                                   candidate.type_id) ==
              dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::uint64) ==
              dt::DatatypeCastCategory::forbidden,
          "all registered present-value casts incident to uint64 refuse");
  }
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest present;
    present.value = Uint64(42);
    present.target_type_id = dt::CanonicalTypeId::int128;
    present.context = context;
    present.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present uint64 outgoing cast refuses in every context");
    present.reference_compatibility_profile = true;
    Check(!dt::CastDatatypeValue(present).ok(),
          "compatibility mode cannot infer an uint64 outgoing cast");
    present.reference_compatibility_profile = false;

    present.target_type_id = dt::CanonicalTypeId::uint64;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present uint64 identity cast refuses in every context");

    present.value = {dt::CanonicalTypeId::int16,
                     std::string{'\x2a', '\0'}, false};
    present.value.descriptor = DescriptorFor(dt::CanonicalTypeId::int16);
    present.target_type_id = dt::CanonicalTypeId::uint64;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present uint64 incoming cast refuses in every context");

    auto int128_descriptor = DescriptorFor(dt::CanonicalTypeId::int128);
    int128_descriptor.nullable_allowed = true;
    dt::DatatypeCastRequest cross_type_null;
    cross_type_null.value = null_value;
    cross_type_null.target_type_id = dt::CanonicalTypeId::int128;
    cross_type_null.target_descriptor = int128_descriptor;
    cross_type_null.context = context;
    cross_type_null.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    CheckCastRefused(dt::CastDatatypeValue(cross_type_null),
                     "DATATYPE.CAST_FORBIDDEN",
                     "cross-type typed uint64 NULL refuses without cast policy");

    auto int16_descriptor = DescriptorFor(dt::CanonicalTypeId::int16);
    int16_descriptor.nullable_allowed = true;
    cross_type_null.value = {dt::CanonicalTypeId::int16, {}, true};
    cross_type_null.value.descriptor = int16_descriptor;
    cross_type_null.target_type_id = dt::CanonicalTypeId::uint64;
    cross_type_null.target_descriptor = descriptor;
    CheckCastRefused(dt::CastDatatypeValue(cross_type_null),
                     "DATATYPE.CAST_FORBIDDEN",
                     "incoming typed NULL to uint64 refuses without cast policy");

  }

  dt::DatatypeNumericOperationRequest numeric;
  numeric.type_id = dt::CanonicalTypeId::uint64;
  numeric.operation = dt::DatatypeNumericOperationKind::add;
  numeric.left = Uint64(1);
  numeric.right = Uint64(2);
  numeric.result_descriptor = descriptor;
  const auto numeric_result = dt::ApplyNumericOperation(numeric);
  Check(!numeric_result.ok() &&
            numeric_result.diagnostic.diagnostic_code ==
                "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
        "uint64 arithmetic refuses until its numeric operation profile exists");

  auto mismatched_present = Uint64(1);
  mismatched_present.descriptor.security_policy_uuid = FixtureV7Uuid(0xb0u);
  mismatched_present.descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  Check(!dt::CompareDatatypeValues({Uint64(0), mismatched_present}).ok(),
        "uint64 comparison rejects unequal UUID-bound descriptor metadata");
  Check(!dt::CompareDatatypeValues({descriptorless, Uint64(0)}).ok() &&
            !dt::CompareDatatypeValues({Uint64(0), descriptorless}).ok(),
        "uint64 comparison rejects either missing descriptor side");
  const auto null_first = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_last});
  const auto first_compare = dt::CompareDatatypeValues(
      {null_value, Uint64(0), dt::DatatypeNullOrdering::nulls_first});
  const auto last_compare = dt::CompareDatatypeValues(
      {null_value, Uint64(std::numeric_limits<std::uint64_t>::max()), dt::DatatypeNullOrdering::nulls_last});
  Check(null_first.ok() && null_first.sort_key == std::string(1, '\0') &&
            null_last.ok() && null_last.sort_key == std::string(1, '\2'),
        "uint64 keys honor explicit containing NULL placement");
  Check(!first_compare.ok() && !last_compare.ok() &&
            !dt::CompareDatatypeValues({null_value, null_value}).ok(),
        "uint64 NULL comparison refuses without an owning operator policy");

  Check(!dt::HashDatatypeValue({Uint64(42)}).ok() &&
            !dt::HashDatatypeValue({null_value}).ok() &&
            !dt::RenderDatatypeValueForDisplay({Uint64(42)}).ok(),
        "undefined uint64 hash and display policies refuse");
  dt::DatatypeOperationValue signed_value{
      dt::CanonicalTypeId::int16, std::string{'\x2a', '\0'}, false};
  signed_value.descriptor = DescriptorFor(dt::CanonicalTypeId::int16);
  Check(!dt::CompareDatatypeValues({Uint64(42), signed_value}).ok(),
        "mixed-width signed comparison refuses without a registered rule");
}

template <std::size_t N>
void Append(std::vector<platform::byte>* output,
            const std::array<platform::byte, N>& bytes) {
  output->insert(output->end(), bytes.begin(), bytes.end());
}

void Persistence() {
  constexpr platform::Uuid descriptor_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x3a}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x3b}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x3c}};
  constexpr std::array<platform::byte,32> value_zero{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7b,0,0,0,1,0,0,0,
      8,0,0,0,0xa1,0x2f,0xde,0xf5,0,0,0,0,0,0,0,0}};
  constexpr std::array<platform::byte,32> value_one{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7b,0,0,0,1,0,0,0,
      8,0,0,0,0x80,0xd0,0x7c,0x98,1,0,0,0,0,0,0,0}};
  constexpr std::array<platform::byte,32> value_mid{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7b,0,0,0,1,0,0,0,
      8,0,0,0,0x19,0x1f,0xc6,0x65,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f}};
  constexpr std::array<platform::byte,32> value_high{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7b,0,0,0,1,0,0,0,
      8,0,0,0,0x21,0xf9,0xde,0x75,0,0,0,0,0,0,0,0x80}};
  constexpr std::array<platform::byte,32> value_max{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7b,0,0,0,1,0,0,0,
      8,0,0,0,0x99,0xe8,0xc6,0xe5,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}};
  constexpr std::array<platform::byte,24> sql_null{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x7b,0,0,0,0,0,0,0,
      0,0,0,0,0xee,0x3e,0x65,0x07}};

  struct Oracle { std::uint64_t value; const platform::byte* bytes; std::size_t size; };
  const std::array<Oracle,5> oracles{{
      {0,value_zero.data(),value_zero.size()},
      {1,value_one.data(),value_one.size()},
      {0x7fffffffffffffffULL,value_mid.data(),value_mid.size()},
      {0x8000000000000000ULL,value_high.data(),value_high.size()},
      {std::numeric_limits<std::uint64_t>::max(),value_max.data(),value_max.size()}}};
  for (const auto& oracle : oracles) {
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint64, dt::DatatypePhysicalValueState::value,
         Payload(Uint64(oracle.value))});
    Check(encoded.ok() && encoded.bytes.size() == oracle.size &&
              std::equal(encoded.bytes.begin(), encoded.bytes.end(), oracle.bytes),
          "production uint64 physical frame matches independent hard-coded oracle");
  }
  const auto encoded_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::uint64, dt::DatatypePhysicalValueState::sql_null, {}});
  Check(encoded_null.ok() && encoded_null.bytes.size() == sql_null.size() &&
            std::equal(encoded_null.bytes.begin(), encoded_null.bytes.end(), sql_null.begin()),
        "production uint64 NULL frame matches independent hard-coded oracle");

  constexpr std::size_t test_owned_envelope_header_bytes = 120;
  std::vector<platform::byte> expected(test_owned_envelope_header_bytes, 0);
  const std::array<platform::byte,8> test_owned_magic{{'S','B','U','6','4','V','0','1'}};
  std::copy(test_owned_magic.begin(), test_owned_magic.end(), expected.begin());
  std::copy(descriptor_uuid.bytes.begin(), descriptor_uuid.bytes.end(), expected.begin() + 8);
  std::copy(type_uuid.bytes.begin(), type_uuid.bytes.end(), expected.begin() + 24);
  std::copy(codec_uuid.bytes.begin(), codec_uuid.bytes.end(), expected.begin() + 40);
  platform::StoreLittle32(expected.data() + 56, 1);
  platform::StoreLittle32(expected.data() + 60, 1);
  platform::StoreLittle32(expected.data() + 64, 1);
  platform::StoreLittle32(expected.data() + 68, 6);
  std::uint32_t offset = test_owned_envelope_header_bytes;
  for (unsigned index = 0; index < 6; ++index) {
    const std::uint32_t size = index == 5 ? 24 : 32;
    platform::StoreLittle32(expected.data() + 72 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 76 + index * 8, size);
    offset += size;
  }
  Append(&expected, value_zero); Append(&expected, value_one);
  Append(&expected, value_mid); Append(&expected, value_high);
  Append(&expected, value_max); Append(&expected, sql_null);

#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-uint64-" + std::to_string(pid) + ".codec");
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove(path, error); } } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create uint64 persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact uint64 bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen uint64 persistence fixture with an independent handle");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected &&
      std::equal(actual.begin() + 8, actual.begin() + 24, descriptor_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 24, actual.begin() + 40, type_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 40, actual.begin() + 56, codec_uuid.bytes.begin()) &&
      platform::LoadLittle32(actual.data() + 56) == 1 &&
      platform::LoadLittle32(actual.data() + 60) == 1 &&
      platform::LoadLittle32(actual.data() + 64) == 1 &&
      platform::LoadLittle32(actual.data() + 68) == 6;
  for (unsigned index = 0; index < 6 && decoded_all; ++index) {
    const auto frame_offset = platform::LoadLittle32(actual.data() + 72 + index * 8);
    const auto frame_size = platform::LoadLittle32(actual.data() + 76 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() && decoded.value.type_id == dt::CanonicalTypeId::uint64 &&
        (index == 5
             ? decoded.value.state == dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state == dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Uint64(oracles[index].value)));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "independent reopen preserves exact identities and uint64 frames");

  std::array<platform::byte,2> short_bytes{};
  const auto short_read = reader.ReadAt(actual.size() - 1, short_bytes.data(), short_bytes.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_bytes.size(),
        "uint64 persistence boundary refuses a short read");
  const auto rejected_write = reader.WriteAt(0, test_owned_magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only uint64 persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened uint64 fixture");

  for (const auto& oracle : oracles) {
    std::vector<platform::byte> corrupt(oracle.bytes, oracle.bytes + oracle.size);
    corrupt[20] ^= 0x01;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(oracle.bytes, oracle.size - 1).ok(),
          "uint64 physical decoder rejects checksum corruption and truncation");
  }
  auto corrupt_null = sql_null;
  corrupt_null[20] ^= 0x01;
  Check(!dt::DecodeDatatypePhysicalValue(corrupt_null.data(), corrupt_null.size()).ok() &&
            !dt::DecodeDatatypePhysicalValue(sql_null.data(), sql_null.size() - 1).ok(),
        "uint64 NULL physical frame rejects checksum corruption and truncation");
}


}  // namespace

int main() {
  ExactIdentity();
  RepresentationAndCodecs();
  StructuredPropertyPartitions();
  NullAndAbsentPolicies();
  Persistence();
  std::cout << "base uint64 checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
