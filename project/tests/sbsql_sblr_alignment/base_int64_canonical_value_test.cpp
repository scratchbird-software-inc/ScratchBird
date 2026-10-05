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

scratchbird::engine::ExecutionTypeDescriptor Int64Descriptor() {
  return DescriptorFor(dt::CanonicalTypeId::int64);
}

dt::DatatypeOperationValue Int64(std::int64_t value) {
  std::string encoded;
  if (!dt::EncodeCanonicalInt64Value(value, &encoded)) return {};
  dt::DatatypeOperationValue result{
      dt::CanonicalTypeId::int64, std::move(encoded), false};
  static const auto descriptor = Int64Descriptor();
  result.descriptor = descriptor;
  return result;
}

std::vector<platform::byte> Payload(const dt::DatatypeOperationValue& value) {
  return {value.encoded_value.begin(), value.encoded_value.end()};
}

void ExactIdentity() {
  constexpr platform::Uuid descriptor_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x11}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x12}};

  const auto descriptor = Int64Descriptor();
  Check(std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                   std::end(descriptor.descriptor_uuid.bytes),
                   descriptor_uuid.bytes.begin()) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.bit_width == 64,
        "exact int64 UUID descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_snapshot_uuid == dt::kDatatypeCohortV5 &&
        row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::int64);
  });
  Check(found != rows.end() && found->descriptor_uuid == descriptor_uuid &&
            found->type_uuid == type_uuid && found->codec_uuid.is_nil() &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.int64.le.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 8 &&
            found->canonical_value_maximum_bytes == 8 &&
            found->canonical_value_exact_bytes == 8 && !found->null_supported &&
            found->byte_order_code == 2 && found->signed_code &&
            found->representation_code == 2 &&
            found->canonical_byte_order.empty() &&
            found->canonical_representation.empty(),
        "exact int64 type-codec tuple records absent codec UUID and literal NULL support");

  for (const auto cohort : std::array{
           std::tuple{dt::kDatatypeCohortV1, 1ULL, 1ULL},
           std::tuple{dt::kDatatypeCohortV2, 2ULL, 2ULL},
           std::tuple{dt::kDatatypeCohortV3, 3ULL, 3ULL},
           std::tuple{dt::kDatatypeCohortV4, 4ULL, 4ULL},
           std::tuple{dt::kDatatypeCohortV5, 5ULL, 5ULL}}) {
    const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
        std::get<0>(cohort), std::get<1>(cohort), std::get<2>(cohort),
        descriptor_uuid, 1);
    Check(admitted.ok && admitted.row.type_uuid == type_uuid &&
              admitted.row.codec_uuid.is_nil(),
          "int64 exact identity survives admitted cohort inheritance");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV4, 5, 5, descriptor_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 2).ok,
        "int64 identity rejects crossed generations");
  const auto parsed_provisional = scratchbird::core::uuid::ParseUuid(
      "67000000-696e-7436-b400-000000000000");
  Check(parsed_provisional.ok(), "provisional int64 fixture UUID parses");
  const auto provisional = parsed_provisional.value;
  dt::DatatypeStorageIdentityV1 storage_identity;
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, provisional, 1).ok &&
            !dt::LookupDatatypeStorageIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, provisional, 1,
             &storage_identity) &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, type_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, platform::Uuid{}, 1).ok,
        "normal int64 lookup rejects provisional, wrong-role, and nil identities");

  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 1,
            &storage_identity) &&
            storage_identity.descriptor_uuid == descriptor_uuid &&
            storage_identity.type_uuid == type_uuid &&
            storage_identity.type_id == dt::CanonicalTypeId::int64 &&
            storage_identity.codec.has_value() &&
            storage_identity.codec->codec_uuid.is_nil(),
        "int64 storage identity preserves exact descriptor/type/codec fields");
  const auto layout = dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::int64);
  Check(layout.ok() &&
            layout.layout.storage_class == dt::DatatypeStorageClass::inline_fixed &&
            layout.layout.encoding == dt::DatatypeBinaryEncoding::twos_complement_little_endian &&
            layout.layout.inline_bytes == 8 && layout.layout.alignment_bytes == 8,
        "int64 storage layout is fixed signed LE8");
}

std::array<platform::byte, 8> OracleBytes(std::int64_t value) {
  const auto raw = std::bit_cast<std::uint64_t>(value);
  std::array<platform::byte, 8> bytes{};
  for (unsigned byte = 0; byte < 8; ++byte) {
    bytes[byte] = static_cast<platform::byte>((raw >> (byte * 8u)) & 0xffu);
  }
  return bytes;
}

std::string OracleKey(std::int64_t value) {
  const auto raw = std::bit_cast<std::uint64_t>(value);
  std::string key(9, '\0');
  key[0] = '\1';
  for (unsigned byte = 0; byte < 8; ++byte) {
    const unsigned shift = (7u - byte) * 8u;
    auto octet = static_cast<unsigned char>((raw >> shift) & 0xffu);
    if (byte == 0) octet ^= 0x80u;
    key[byte + 1] = static_cast<char>(octet);
  }
  return key;
}

void RepresentationAndCodecs() {
  constexpr std::array<std::int64_t, 24> vectors{{
      std::numeric_limits<std::int64_t>::min(),
      -281474976710656LL, -4294967296LL, -2147483648LL, -65536LL,
      -32768LL, -2LL, -1LL, 0LL, 1LL, 127LL, 128LL, 255LL, 256LL,
      32767LL, 32768LL, 65535LL, 65536LL, 2147483647LL, 2147483648LL,
      4294967295LL, 4294967296LL, 281474976710656LL,
      std::numeric_limits<std::int64_t>::max()}};

  std::string previous_key;
  for (std::size_t index = 0; index < vectors.size(); ++index) {
    const auto number = vectors[index];
    const auto value = Int64(number);
    const auto bytes = OracleBytes(number);
    const std::vector<platform::byte> expected(bytes.begin(), bytes.end());
    std::string encoded;
    std::int64_t decoded = 0;
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::int64, false, false, expected});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int64,
         dt::DatatypePhysicalValueState::value, expected});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(), physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto comparison = dt::CompareDatatypeValues({value, value});
    const auto key = dt::MakeDatatypeSortKey({value});
    Check(dt::EncodeCanonicalInt64Value(number, &encoded) &&
              encoded == value.encoded_value && Payload(value) == expected &&
              dt::DecodeCanonicalInt64Value(encoded, &decoded) &&
              decoded == number && binary.ok() && binary_back.ok() &&
              binary_back.value.payload == expected && physical.ok() &&
              physical_back.ok() && physical_back.value.payload == expected &&
              comparison.ok() && comparison.comparison == 0 && key.ok() &&
              key.sort_key == OracleKey(number),
          "int64 canonical, binary, physical, comparison, and key vector");
    if (index != 0) {
      const auto ordered = dt::CompareDatatypeValues(
          {Int64(vectors[index - 1]), value});
      Check(ordered.ok() && ordered.comparison == -1 &&
                previous_key < key.sort_key,
            "int64 signed comparison and key order adjacent vectors");
    }
    previous_key = key.sort_key;
  }

  std::uint64_t state = 0x9e3779b97f4a7c15ULL;
  std::int64_t previous = std::numeric_limits<std::int64_t>::min();
  previous_key = dt::MakeDatatypeSortKey({Int64(previous)}).sort_key;
  for (unsigned index = 0; index < 100000; ++index) {
    state ^= state >> 12u;
    state ^= state << 25u;
    state ^= state >> 27u;
    state *= 2685821657736338717ULL;
    const auto candidate = std::bit_cast<std::int64_t>(state);
    std::string encoded;
    std::int64_t decoded = 0;
    const auto key = dt::MakeDatatypeSortKey({Int64(candidate)});
    const auto compare = dt::CompareDatatypeValues({Int64(previous), Int64(candidate)});
    Check(dt::EncodeCanonicalInt64Value(candidate, &encoded) &&
              dt::DecodeCanonicalInt64Value(encoded, &decoded) &&
              decoded == candidate && key.ok() && compare.ok() &&
              compare.comparison == (previous < candidate ? -1 : previous > candidate ? 1 : 0) &&
              ((previous_key < key.sort_key) == (previous < candidate)) &&
              ((previous_key > key.sort_key) == (previous > candidate)),
          "deterministic int64 sample preserves value/comparison/key order");
    previous = candidate;
    previous_key = key.sort_key;
  }

  std::string unchanged = "sentinel";
  Check(dt::EncodeCanonicalInt64Value(std::numeric_limits<std::int64_t>::min(), &unchanged) &&
            unchanged == std::string(7, '\0') + std::string(1, static_cast<char>(0x80)) &&
            dt::EncodeCanonicalInt64Value(std::numeric_limits<std::int64_t>::max(), &unchanged) &&
            unchanged == std::string(7, static_cast<char>(0xff)) + std::string(1, static_cast<char>(0x7f)) &&
            !dt::EncodeCanonicalInt64Value(0, nullptr),
        "int64 mathematical encoder covers its full typed domain and rejects null output");
  for (const auto width : {0u,1u,2u,3u,4u,5u,6u,7u,9u,32u}) {
    const std::string malformed(width, '\0');
    std::int64_t unchanged_number = 0x1122334455667788LL;
    Check(!dt::DecodeCanonicalInt64Value(malformed, &unchanged_number) &&
              unchanged_number == 0x1122334455667788LL,
          "int64 mathematical decoder rejects malformed width atomically");
    const std::vector<platform::byte> bad(width, 0);
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::int64, false, false, bad}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::int64,
                dt::DatatypePhysicalValueState::value, bad}).ok(),
          "int64 codecs reject non-eight-byte present payload");
    const dt::DatatypeOperationValue operation{
        dt::CanonicalTypeId::int64, malformed, false};
    dt::DatatypeCastRequest malformed_identity;
    malformed_identity.value = operation;
    malformed_identity.target_type_id = dt::CanonicalTypeId::int64;
    Check(!dt::CompareDatatypeValues({operation, Int64(0)}).ok() &&
              !dt::MakeDatatypeSortKey({operation}).ok() &&
              !dt::SerializeDatatypeValue({operation}).ok() &&
              !dt::HashDatatypeValue({operation}).ok() &&
              !dt::RenderDatatypeValueForDisplay({operation}).ok() &&
              !dt::CastDatatypeValue(malformed_identity).ok(),
          "int64 operation surfaces reject malformed carrier widths");
  }
  Check(!dt::DecodeCanonicalInt64Value(std::string(8, '\0'), nullptr),
        "int64 decoder refuses a null output pointer");

  dt::DatatypeExtractRequest extract;
  extract.value = {dt::CanonicalTypeId::interval, std::string(16, '\0'), false};
  extract.field = "months";
  extract.result_descriptor = Int64Descriptor();
  const auto interval_extract = dt::ExtractDatatypeField(extract);
  Check(!interval_extract.ok() &&
            interval_extract.diagnostic.diagnostic_code ==
                "CTI.INTERVAL.DESCRIPTOR_INVALID" &&
            interval_extract.value.type_id == dt::CanonicalTypeId::unknown &&
            !interval_extract.value.is_null &&
            interval_extract.value.encoded_value.empty(),
        "generic interval months extraction requires exact profile authority");

  std::int64_t text_like = 0;
  Check(dt::DecodeCanonicalInt64Value(std::string{"12345678", 8}, &text_like) &&
            text_like == 4050765991979987505LL,
        "eight text-like bytes remain native int64 0x3837363534333231");

  for (const auto number : vectors) {
    const auto input = Int64(number);
    const auto serialized = dt::SerializeDatatypeValue({input});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::int64;
    request.expected_descriptor = input.descriptor;
    request.serialized_value = serialized.serialized_value;
    const auto restored = dt::DeserializeDatatypeValue(request);
    Check(serialized.ok() && restored.ok() &&
              restored.value.type_id == dt::CanonicalTypeId::int64 &&
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
          "generic value serialization preserves int64 UUID and native bytes");
  }
  for (const char* bad : {"SBDV1;type=int64;state=value;payload=",
                          "SBDV1;type=int64;state=value;payload=00",
                          "SBDV1;type=int64;state=value;payload=00000000000000",
                          "SBDV1;type=int64;state=value;payload=000000000000000000"}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::int64;
    request.serialized_value = bad;
    Check(!dt::DeserializeDatatypeValue(request).ok(),
          "generic value deserializer rejects malformed int64 widths");
  }
}

void StructuredPropertyPartitions() {
  const auto verify = [](std::uint64_t raw, bool have_previous,
                         std::int64_t previous_value,
                         const std::string& previous_key) {
    const auto expected_value = std::bit_cast<std::int64_t>(raw);
    const auto expected_array = OracleBytes(expected_value);
    const std::string expected_bytes(expected_array.begin(), expected_array.end());
    const std::string expected_key = OracleKey(expected_value);
    std::string encoded;
    std::int64_t decoded = 0;
    const auto value = Int64(expected_value);
    const std::vector<platform::byte> payload(expected_array.begin(), expected_array.end());
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::int64, false, false, payload});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int64,
         dt::DatatypePhysicalValueState::value, payload});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(), physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto equal = dt::CompareDatatypeValues({value, value});
    const auto key = dt::MakeDatatypeSortKey({value});
    bool ordered = true;
    if (have_previous) {
      const auto forward = dt::CompareDatatypeValues({Int64(previous_value), value});
      const auto reverse = dt::CompareDatatypeValues({value, Int64(previous_value)});
      ordered = previous_value < expected_value && forward.ok() &&
          forward.comparison == -1 && reverse.ok() && reverse.comparison == 1 &&
          previous_key < key.sort_key;
    }
    Check(dt::EncodeCanonicalInt64Value(expected_value, &encoded) &&
              encoded == expected_bytes &&
              dt::DecodeCanonicalInt64Value(encoded, &decoded) &&
              decoded == expected_value && binary.ok() && binary_back.ok() &&
              binary_back.value.payload == payload && physical.ok() &&
              physical_back.ok() && physical_back.value.payload == payload &&
              equal.ok() && equal.comparison == 0 && key.ok() &&
              key.sort_key == expected_key && ordered,
          "structured int64 partition agrees with independent byte/order oracle");
    return std::pair{expected_value, expected_key};
  };

  constexpr std::array<std::uint64_t,4> anchors{{
      0x8000000000000000ULL, 0x81234567000089abULL,
      0x0123000089abcdefULL, 0x7fff012345670000ULL}};
  for (unsigned word = 0; word < 4; ++word) {
    bool have_previous = false;
    std::int64_t previous_value = 0;
    std::string previous_key;
    const auto mask = ~(0xffffULL << (word * 16u));
    for (std::uint64_t ordinal = 0; ordinal <= 0xffffULL; ++ordinal) {
      const std::uint64_t part = word == 3
          ? (ordinal < 0x8000ULL ? ordinal + 0x8000ULL : ordinal - 0x8000ULL)
          : ordinal;
      const auto raw = (anchors[word] & mask) | (part << (word * 16u));
      const auto observed = verify(raw, have_previous, previous_value, previous_key);
      previous_value = observed.first;
      previous_key = observed.second;
      have_previous = true;
    }
  }
}

void NullAndAbsentPolicies() {
  auto descriptor = Int64Descriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue null_value{
      dt::CanonicalTypeId::int64, {}, true};
  null_value.descriptor = descriptor;

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::int64, true, false, {}});
  const auto binary_back = binary_null.ok()
      ? dt::DecodeDatatypeBinaryValue(binary_null.encoded)
      : dt::DatatypeBinaryResult{};
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::int64,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto physical_back = physical_null.ok()
      ? dt::DecodeDatatypePhysicalValue(physical_null.bytes.data(),
                                        physical_null.bytes.size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(binary_null.ok() && binary_back.ok() && binary_back.value.is_null &&
            binary_back.value.payload.empty() && physical_null.ok() &&
            physical_back.ok() &&
            physical_back.value.type_id == dt::CanonicalTypeId::int64 &&
            physical_back.value.state ==
                dt::DatatypePhysicalValueState::sql_null &&
            physical_back.value.payload.empty(),
        "int64 typed NULL is external state with zero payload");
  Check(!dt::EncodeDatatypeBinaryValue(
             {dt::CanonicalTypeId::int64, true, false, {0,0,0,0,0,0,0,0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
             {dt::CanonicalTypeId::int64,
              dt::DatatypePhysicalValueState::sql_null, {0,0,0,0,0,0,0,0}}).ok(),
        "int64 codecs reject payload-bearing NULL");
  const auto present_zero = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::int64, false, false, {0,0,0,0,0,0,0,0}});
  Check(present_zero.ok() && present_zero.encoded != binary_null.encoded,
        "present int64 zero remains distinct from typed NULL");

  const auto serialized_null = dt::SerializeDatatypeValue({null_value});
  dt::DatatypeDeserializationRequest null_decode;
  null_decode.expected_type_id = dt::CanonicalTypeId::int64;
  null_decode.expected_descriptor = descriptor;
  null_decode.serialized_value = serialized_null.serialized_value;
  const auto restored_null = dt::DeserializeDatatypeValue(null_decode);
  Check(serialized_null.ok() && restored_null.ok() &&
            restored_null.value.type_id == dt::CanonicalTypeId::int64 &&
            restored_null.value.is_null &&
            restored_null.value.encoded_value.empty(),
        "generic value serialization preserves typed int64 NULL state");
  null_decode.serialized_value =
      "SBDV1;type=int64;state=null;payload=0000000000000000";
  Check(!dt::DeserializeDatatypeValue(null_decode).ok(),
        "generic value deserialization rejects payload-bearing int64 NULL");

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = null_value;
    identity.target_type_id = dt::CanonicalTypeId::int64;
    identity.target_descriptor = descriptor;
    identity.context = context;
    identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto result = dt::CastDatatypeValue(identity);
    Check(result.ok() && result.category == dt::DatatypeCastCategory::identity &&
              result.value.is_null && result.value.encoded_value.empty(),
          "typed int64 NULL identity validates exact descriptor/state");

    identity.target_descriptor.security_policy_uuid = FixtureV7Uuid(0x90u);
    identity.target_descriptor.modifier_flags |=
        scratchbird::engine::ExecutionTypeModifierFlagBit(
            scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
    CheckCastRefused(
        dt::CastDatatypeValue(identity), "DATATYPE.DESCRIPTOR.INVALID",
        "typed int64 NULL identity rejects a mismatched descriptor before cast policy");

    auto mismatched_and_malformed = identity;
    mismatched_and_malformed.value.encoded_value = "payload";
    CheckCastRefused(
        dt::CastDatatypeValue(mismatched_and_malformed),
        "DATATYPE.DESCRIPTOR.INVALID",
        "typed int64 NULL descriptor mismatch precedes malformed NULL state");

    auto mismatched_and_nonnullable = identity;
    mismatched_and_nonnullable.target_descriptor.nullable_allowed = false;
    CheckCastRefused(
        dt::CastDatatypeValue(mismatched_and_nonnullable),
        "DATATYPE.DESCRIPTOR.INVALID",
        "typed int64 NULL descriptor mismatch precedes target nullability");

    auto mismatched_and_bad_context = identity;
    mismatched_and_bad_context.context =
        static_cast<dt::DatatypeCastContext>(0xffu);
    CheckCastRefused(
        dt::CastDatatypeValue(mismatched_and_bad_context),
        "DATATYPE.DESCRIPTOR.INVALID",
        "typed int64 NULL descriptor mismatch precedes cast context");

    auto malformed_state = identity;
    malformed_state.target_descriptor = descriptor;
    malformed_state.value.encoded_value = "payload";
    CheckCastRefused(
        dt::CastDatatypeValue(malformed_state), "DATATYPE.NULL_STATE.INVALID",
        "typed int64 NULL with equal descriptors reports malformed state");

    auto nonnullable_target = identity;
    nonnullable_target.target_descriptor = descriptor;
    nonnullable_target.target_descriptor.nullable_allowed = false;
    nonnullable_target.value.descriptor =
        nonnullable_target.target_descriptor;
    CheckCastRefused(
        dt::CastDatatypeValue(nonnullable_target),
        "DATATYPE.NULL_NOT_ADMITTED",
        "typed int64 NULL with equal descriptors reports target nullability");

    auto bad_context = identity;
    bad_context.target_descriptor = descriptor;
    bad_context.context = static_cast<dt::DatatypeCastContext>(0xffu);
    CheckCastRefused(
        dt::CastDatatypeValue(bad_context), "DATATYPE.CAST_FORBIDDEN",
        "typed int64 NULL with equal descriptors reports invalid cast context");
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
    contextual.target_type_id = dt::CanonicalTypeId::int64;
    contextual.target_descriptor = descriptor;
    contextual.context = context;
    contextual.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    const auto bound = dt::CastDatatypeValue(contextual);
    Check(bound.ok() && bound.value.type_id == dt::CanonicalTypeId::int64 &&
              bound.value.is_null && bound.value.encoded_value.empty(),
          "contextual NULL binds to an exact nullable int64 descriptor");

    contextual.target_descriptor.nullable_allowed = false;
    CheckCastRefused(dt::CastDatatypeValue(contextual),
                     "DATATYPE.NULL_NOT_ADMITTED",
                     "contextual NULL refuses a non-nullable int64 descriptor");
    contextual.target_descriptor = descriptor;
    ++contextual.target_descriptor.descriptor_epoch;
    CheckCastRefused(
        dt::CastDatatypeValue(contextual), "DATATYPE.DESCRIPTOR.INVALID",
        "contextual NULL refuses a mismatched int64 descriptor generation");

    dt::DatatypeCastRequest standalone_target;
    standalone_target.value = Int64(0);
    standalone_target.target_type_id = dt::CanonicalTypeId::null_type;
    standalone_target.context = context;
    standalone_target.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    CheckCastRefused(dt::CastDatatypeValue(standalone_target),
                     "DATATYPE.CAST_FORBIDDEN",
                     "present int64 cannot cast to the standalone NULL sentinel");
    standalone_target.value = null_value;
    CheckCastRefused(
        dt::CastDatatypeValue(standalone_target), "DATATYPE.CAST_FORBIDDEN",
        "typed int64 NULL cannot cast to the standalone NULL sentinel");
  }

  auto bigint_alias = descriptor;
  bigint_alias.stable_name = "bigint";
  auto localized_alias = descriptor;
  localized_alias.stable_name = "entier_64";
  auto left_alias = Int64(-1);
  auto right_alias = Int64(1);
  left_alias.descriptor = bigint_alias;
  right_alias.descriptor = localized_alias;
  const auto alias_comparison =
      dt::CompareDatatypeValues({left_alias, right_alias});
  Check(alias_comparison.ok() && alias_comparison.comparison == -1,
        "int64 aliases preserve UUID-bound comparison identity");

  auto alias_null = null_value;
  alias_null.descriptor = bigint_alias;
  dt::DatatypeCastRequest alias_identity;
  alias_identity.value = alias_null;
  alias_identity.target_type_id = dt::CanonicalTypeId::int64;
  alias_identity.target_descriptor = localized_alias;
  const auto alias_cast = dt::CastDatatypeValue(alias_identity);
  Check(alias_cast.ok() && alias_cast.value.is_null &&
            alias_cast.value.descriptor.stable_name == "bigint",
        "int64 aliases preserve UUID-bound typed NULL identity");

  const dt::DatatypeOperationValue descriptorless{
      dt::CanonicalTypeId::int64, std::string(8, '\0'), false};
  Check(!dt::CompareDatatypeValues({descriptorless, descriptorless}).ok(),
        "int64 comparison refuses values without UUID-bound descriptors");
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "bigint";
  Check(!dt::CompareDatatypeValues({label_only, label_only}).ok() &&
            !dt::SerializeDatatypeValue({label_only}).ok(),
        "int64 label-only pseudo-descriptor cannot replace UUID identity");

  for (const auto& candidate : dt::BuiltinDatatypeDescriptors()) {
    const auto expected = candidate.type_id == dt::CanonicalTypeId::int64
        ? dt::DatatypeCastCategory::identity
        : dt::DatatypeCastCategory::forbidden;
    Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::int64,
                                   candidate.type_id) == expected &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::int64) == expected,
          "only the registered int64 identity cast is admitted");
  }
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest present;
    present.value = Int64(42);
    present.target_type_id = dt::CanonicalTypeId::int128;
    present.context = context;
    present.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present int64 outgoing cast refuses in every context");
    present.reference_compatibility_profile = true;
    Check(!dt::CastDatatypeValue(present).ok(),
          "compatibility mode cannot infer an int64 outgoing cast");
    present.reference_compatibility_profile = false;

    present.target_type_id = dt::CanonicalTypeId::int64;
    const auto identity = dt::CastDatatypeValue(present);
    Check(identity.ok() &&
              identity.category == dt::DatatypeCastCategory::identity &&
              identity.value.type_id == dt::CanonicalTypeId::int64 &&
              !identity.value.is_null &&
              identity.value.encoded_value == present.value.encoded_value &&
              std::equal(identity.value.descriptor.descriptor_uuid.bytes,
                         identity.value.descriptor.descriptor_uuid.bytes + 16,
                         present.value.descriptor.descriptor_uuid.bytes) &&
              identity.value.descriptor.stable_name ==
                  present.value.descriptor.stable_name,
          "present int64 identity preserves native bytes and source descriptor in every context");

    present.value = {dt::CanonicalTypeId::int16,
                     std::string{'\x2a', '\0'}, false};
    present.target_type_id = dt::CanonicalTypeId::int64;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present int64 incoming cast refuses in every context");

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
                     "cross-type typed int64 NULL refuses without cast policy");

    auto int16_descriptor = DescriptorFor(dt::CanonicalTypeId::int16);
    int16_descriptor.nullable_allowed = true;
    cross_type_null.value = {dt::CanonicalTypeId::int16, {}, true};
    cross_type_null.value.descriptor = int16_descriptor;
    cross_type_null.target_type_id = dt::CanonicalTypeId::int64;
    cross_type_null.target_descriptor = descriptor;
    CheckCastRefused(dt::CastDatatypeValue(cross_type_null),
                     "DATATYPE.CAST_FORBIDDEN",
                     "incoming typed NULL to int64 refuses without cast policy");

  }

  dt::DatatypeCastRequest labeled_identity;
  labeled_identity.value = Int64(-42);
  labeled_identity.target_type_id = dt::CanonicalTypeId::int64;
  labeled_identity.target_descriptor = labeled_identity.value.descriptor;
  labeled_identity.target_descriptor.stable_name = "localized-bigint";
  const auto labeled = dt::CastDatatypeValue(labeled_identity);
  Check(labeled.ok() &&
            labeled.category == dt::DatatypeCastCategory::identity &&
            labeled.value.encoded_value == labeled_identity.value.encoded_value &&
            std::equal(labeled.value.descriptor.descriptor_uuid.bytes,
                       labeled.value.descriptor.descriptor_uuid.bytes + 16,
                       labeled_identity.target_descriptor.descriptor_uuid.bytes) &&
            labeled.value.descriptor.stable_name == "localized-bigint",
        "int64 identity accepts label aliases for one UUID-bound descriptor");

  auto mismatched_identity = labeled_identity;
  mismatched_identity.target_descriptor.nullable_allowed =
      !mismatched_identity.value.descriptor.nullable_allowed;
  CheckCastRefused(dt::CastDatatypeValue(mismatched_identity),
                   "DATATYPE.DESCRIPTOR.INVALID",
                   "int64 identity rejects unequal UUID-bound descriptor metadata");

  dt::DatatypeNumericOperationRequest numeric;
  numeric.type_id = dt::CanonicalTypeId::int64;
  numeric.operation = dt::DatatypeNumericOperationKind::add;
  numeric.left = Int64(1);
  numeric.right = Int64(2);
  numeric.result_descriptor = descriptor;
  const auto numeric_result = dt::ApplyNumericOperation(numeric);
  Check(!numeric_result.ok() &&
            numeric_result.diagnostic.diagnostic_code ==
                "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
        "int64 arithmetic refuses until its numeric operation profile exists");

  auto mismatched_present = Int64(1);
  mismatched_present.descriptor.security_policy_uuid = FixtureV7Uuid(0xb0u);
  mismatched_present.descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  Check(!dt::CompareDatatypeValues({Int64(0), mismatched_present}).ok(),
        "int64 comparison rejects unequal UUID-bound descriptor metadata");
  Check(!dt::CompareDatatypeValues({descriptorless, Int64(0)}).ok() &&
            !dt::CompareDatatypeValues({Int64(0), descriptorless}).ok(),
        "int64 comparison rejects either missing descriptor side");
  const auto null_first = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_last});
  const auto first_compare = dt::CompareDatatypeValues(
      {null_value, Int64(0), dt::DatatypeNullOrdering::nulls_first});
  const auto last_compare = dt::CompareDatatypeValues(
      {null_value, Int64(std::numeric_limits<std::int64_t>::max()), dt::DatatypeNullOrdering::nulls_last});
  Check(null_first.ok() && null_first.sort_key == std::string(1, '\0') &&
            null_last.ok() && null_last.sort_key == std::string(1, '\2'),
        "int64 keys honor explicit containing NULL placement");
  Check(!first_compare.ok() && !last_compare.ok() &&
            !dt::CompareDatatypeValues({null_value, null_value}).ok(),
        "int64 NULL comparison refuses without an owning operator policy");

  Check(!dt::HashDatatypeValue({Int64(42)}).ok() &&
            !dt::HashDatatypeValue({null_value}).ok() &&
            !dt::RenderDatatypeValueForDisplay({Int64(42)}).ok(),
        "undefined int64 hash and display policies refuse");
  const dt::DatatypeOperationValue signed_value{
      dt::CanonicalTypeId::int16, std::string{'\x2a', '\0'}, false};
  Check(!dt::CompareDatatypeValues({Int64(42), signed_value}).ok(),
        "mixed-width signed comparison refuses without a registered rule");
}

template <std::size_t N>
void Append(std::vector<platform::byte>* output,
            const std::array<platform::byte, N>& bytes) {
  output->insert(output->end(), bytes.begin(), bytes.end());
}

void Persistence() {
  constexpr platform::Uuid descriptor_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x11}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x12}};
  constexpr platform::Uuid absent_codec_uuid{};
  constexpr std::array<platform::byte,32> value_min{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x67,0,0,0,1,0,0,0,
      8,0,0,0,0x25,0x8f,0x04,0xc7,0,0,0,0,0,0,0,0x80}};
  constexpr std::array<platform::byte,32> value_minus_one{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x67,0,0,0,1,0,0,0,
      8,0,0,0,0x9d,0x37,0xd1,0x91,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}};
  constexpr std::array<platform::byte,32> value_zero{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x67,0,0,0,1,0,0,0,
      8,0,0,0,0xa5,0x58,0x05,0x47,0,0,0,0,0,0,0,0}};
  constexpr std::array<platform::byte,32> value_one{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x67,0,0,0,1,0,0,0,
      8,0,0,0,0x84,0xf9,0xa3,0xe9,1,0,0,0,0,0,0,0}};
  constexpr std::array<platform::byte,32> value_max{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x67,0,0,0,1,0,0,0,
      8,0,0,0,0x1d,0x01,0xd2,0x11,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f}};
  constexpr std::array<platform::byte,24> sql_null{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x67,0,0,0,0,0,0,0,
      0,0,0,0,0x72,0xdb,0x1f,0xdf}};

  struct Oracle { std::int64_t value; const platform::byte* bytes; std::size_t size; };
  const std::array<Oracle,5> oracles{{
      {std::numeric_limits<std::int64_t>::min(),value_min.data(),value_min.size()},
      {-1,value_minus_one.data(),value_minus_one.size()},
      {0,value_zero.data(),value_zero.size()},
      {1,value_one.data(),value_one.size()},
      {std::numeric_limits<std::int64_t>::max(),value_max.data(),value_max.size()}}};
  for (const auto& oracle : oracles) {
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int64, dt::DatatypePhysicalValueState::value,
         Payload(Int64(oracle.value))});
    Check(encoded.ok() && encoded.bytes.size() == oracle.size &&
              std::equal(encoded.bytes.begin(), encoded.bytes.end(), oracle.bytes),
          "production int64 physical frame matches independent hard-coded oracle");
  }
  const auto encoded_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::int64, dt::DatatypePhysicalValueState::sql_null, {}});
  Check(encoded_null.ok() && encoded_null.bytes.size() == sql_null.size() &&
            std::equal(encoded_null.bytes.begin(), encoded_null.bytes.end(), sql_null.begin()),
        "production int64 NULL frame matches independent hard-coded oracle");

  constexpr std::size_t test_owned_envelope_header_bytes = 120;
  std::vector<platform::byte> expected(test_owned_envelope_header_bytes, 0);
  const std::array<platform::byte,8> test_owned_magic{{'S','B','I','6','4','V','0','1'}};
  std::copy(test_owned_magic.begin(), test_owned_magic.end(), expected.begin());
  std::copy(descriptor_uuid.bytes.begin(), descriptor_uuid.bytes.end(), expected.begin() + 8);
  std::copy(type_uuid.bytes.begin(), type_uuid.bytes.end(), expected.begin() + 24);
  std::copy(absent_codec_uuid.bytes.begin(), absent_codec_uuid.bytes.end(), expected.begin() + 40);
  platform::StoreLittle32(expected.data() + 56, 1);
  platform::StoreLittle32(expected.data() + 60, 1);
  platform::StoreLittle32(expected.data() + 64, 0);
  platform::StoreLittle32(expected.data() + 68, 6);
  std::uint32_t offset = test_owned_envelope_header_bytes;
  for (unsigned index = 0; index < 6; ++index) {
    const std::uint32_t size = index == 5 ? 24 : 32;
    platform::StoreLittle32(expected.data() + 72 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 76 + index * 8, size);
    offset += size;
  }
  Append(&expected, value_min); Append(&expected, value_minus_one);
  Append(&expected, value_zero); Append(&expected, value_one);
  Append(&expected, value_max); Append(&expected, sql_null);

#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-int64-" + std::to_string(pid) + ".codec");
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove(path, error); } } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create int64 persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact int64 bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen int64 persistence fixture with an independent handle");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected &&
      std::equal(actual.begin() + 8, actual.begin() + 24, descriptor_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 24, actual.begin() + 40, type_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 40, actual.begin() + 56, absent_codec_uuid.bytes.begin()) &&
      platform::LoadLittle32(actual.data() + 56) == 1 &&
      platform::LoadLittle32(actual.data() + 60) == 1 &&
      platform::LoadLittle32(actual.data() + 64) == 0 &&
      platform::LoadLittle32(actual.data() + 68) == 6;
  for (unsigned index = 0; index < 6 && decoded_all; ++index) {
    const auto frame_offset = platform::LoadLittle32(actual.data() + 72 + index * 8);
    const auto frame_size = platform::LoadLittle32(actual.data() + 76 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() && decoded.value.type_id == dt::CanonicalTypeId::int64 &&
        (index == 5
             ? decoded.value.state == dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state == dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Int64(oracles[index].value)));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "independent reopen preserves exact identities and int64 frames");

  std::array<platform::byte,2> short_bytes{};
  const auto short_read = reader.ReadAt(actual.size() - 1, short_bytes.data(), short_bytes.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_bytes.size(),
        "int64 persistence boundary refuses a short read");
  const auto rejected_write = reader.WriteAt(0, test_owned_magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only int64 persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened int64 fixture");

  for (const auto& oracle : oracles) {
    std::vector<platform::byte> corrupt(oracle.bytes, oracle.bytes + oracle.size);
    corrupt[20] ^= 0x01;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(oracle.bytes, oracle.size - 1).ok(),
          "int64 physical decoder rejects checksum corruption and truncation");
  }
  auto corrupt_null = sql_null;
  corrupt_null[20] ^= 0x01;
  Check(!dt::DecodeDatatypePhysicalValue(corrupt_null.data(), corrupt_null.size()).ok() &&
            !dt::DecodeDatatypePhysicalValue(sql_null.data(), sql_null.size() - 1).ok(),
        "int64 NULL physical frame rejects checksum corruption and truncation");
}


}  // namespace

int main() {
  ExactIdentity();
  RepresentationAndCodecs();
  StructuredPropertyPartitions();
  NullAndAbsentPolicies();
  Persistence();
  std::cout << "base int64 checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
