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

scratchbird::engine::ExecutionTypeDescriptor Int32Descriptor() {
  return DescriptorFor(dt::CanonicalTypeId::int32);
}

dt::DatatypeOperationValue Int32(std::int64_t value) {
  std::string encoded;
  if (!dt::EncodeCanonicalInt32Value(value, &encoded)) return {};
  return {dt::CanonicalTypeId::int32, std::move(encoded), false};
}

std::vector<platform::byte> Payload(const dt::DatatypeOperationValue& value) {
  return {value.encoded_value.begin(), value.encoded_value.end()};
}

void ExactIdentity() {
  constexpr platform::Uuid descriptor_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x16}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x17}};

  const auto descriptor = Int32Descriptor();
  Check(std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                   std::end(descriptor.descriptor_uuid.bytes),
                   descriptor_uuid.bytes.begin()) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.stable_name == "int32" && descriptor.bit_width == 32,
        "exact int32 descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_snapshot_uuid == dt::kDatatypeCohortV5 &&
        row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::int32);
  });
  Check(found != rows.end() && found->descriptor_uuid == descriptor_uuid &&
            found->type_uuid == type_uuid && found->codec_uuid.is_nil() &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.int32.le.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 4 &&
            found->canonical_value_maximum_bytes == 4 &&
            found->canonical_value_exact_bytes == 4 && found->null_supported &&
            found->byte_order_code == 2 && found->signed_code &&
            found->representation_code == 2 &&
            found->canonical_byte_order.empty() &&
            found->canonical_representation.empty(),
        "exact int32 type-codec tuple records the missing codec UUID");

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
          "int32 exact identity survives admitted cohort inheritance");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV4, 5, 5, descriptor_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 2).ok,
        "int32 identity rejects crossed generations");
  const auto parsed_provisional = scratchbird::core::uuid::ParseUuid(
      "66000000-696e-7433-b200-000000000000");
  Check(parsed_provisional.ok(), "provisional int32 fixture UUID parses");
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
        "normal int32 lookup rejects provisional, wrong-role, and nil identities");
}

void RepresentationAndCodecs() {
  struct Vector {
    std::int64_t value;
    std::array<platform::byte, 4> bytes;
    std::array<platform::byte, 5> key;
  };
  constexpr std::array<Vector, 16> vectors{{
      {-2147483648LL, {0x00,0x00,0x00,0x80}, {0x01,0x00,0x00,0x00,0x00}},
      {-65536, {0x00,0x00,0xff,0xff}, {0x01,0x7f,0xff,0x00,0x00}},
      {-32768, {0x00,0x80,0xff,0xff}, {0x01,0x7f,0xff,0x80,0x00}},
      {-2, {0xfe,0xff,0xff,0xff}, {0x01,0x7f,0xff,0xff,0xfe}},
      {-1, {0xff,0xff,0xff,0xff}, {0x01,0x7f,0xff,0xff,0xff}},
      {0, {0x00,0x00,0x00,0x00}, {0x01,0x80,0x00,0x00,0x00}},
      {1, {0x01,0x00,0x00,0x00}, {0x01,0x80,0x00,0x00,0x01}},
      {127, {0x7f,0x00,0x00,0x00}, {0x01,0x80,0x00,0x00,0x7f}},
      {128, {0x80,0x00,0x00,0x00}, {0x01,0x80,0x00,0x00,0x80}},
      {255, {0xff,0x00,0x00,0x00}, {0x01,0x80,0x00,0x00,0xff}},
      {256, {0x00,0x01,0x00,0x00}, {0x01,0x80,0x00,0x01,0x00}},
      {32767, {0xff,0x7f,0x00,0x00}, {0x01,0x80,0x00,0x7f,0xff}},
      {32768, {0x00,0x80,0x00,0x00}, {0x01,0x80,0x00,0x80,0x00}},
      {65535, {0xff,0xff,0x00,0x00}, {0x01,0x80,0x00,0xff,0xff}},
      {65536, {0x00,0x00,0x01,0x00}, {0x01,0x80,0x01,0x00,0x00}},
      {2147483647LL, {0xff,0xff,0xff,0x7f}, {0x01,0xff,0xff,0xff,0xff}}}};

  std::string previous_key;
  for (std::size_t index = 0; index < vectors.size(); ++index) {
    const auto& vector = vectors[index];
    const auto value = Int32(vector.value);
    const std::vector<platform::byte> expected(vector.bytes.begin(),
                                               vector.bytes.end());
    std::string encoded;
    std::int64_t decoded_number = 0;
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::int32, false, false, expected});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int32,
         dt::DatatypePhysicalValueState::value, expected});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto comparison = dt::CompareDatatypeValues({value, value});
    const auto key = dt::MakeDatatypeSortKey({value});
    const std::vector<platform::byte> actual_key(
        key.sort_key.begin(), key.sort_key.end());
    Check(dt::EncodeCanonicalInt32Value(vector.value, &encoded) &&
              encoded == value.encoded_value &&
              Payload(value) == expected &&
              dt::DecodeCanonicalInt32Value(encoded, &decoded_number) &&
              decoded_number == vector.value && binary.ok() &&
              binary_back.ok() && binary_back.value.payload == expected &&
              physical.ok() && physical_back.ok() &&
              physical_back.value.payload == expected && comparison.ok() &&
              comparison.comparison == 0 && key.ok() &&
              actual_key == std::vector<platform::byte>(
                  vector.key.begin(), vector.key.end()),
          "int32 canonical, binary, physical, comparison, and key vector");
    if (index != 0) {
      const auto ordered = dt::CompareDatatypeValues(
          {Int32(vectors[index - 1].value), value});
      Check(ordered.ok() && ordered.comparison == -1 &&
                previous_key < key.sort_key,
            "int32 signed comparison and key orders adjacent vectors");
    }
    previous_key = key.sort_key;
  }

  std::uint32_t state = 0x6d2b79f5u;
  std::int64_t previous = std::numeric_limits<std::int32_t>::min();
  previous_key = dt::MakeDatatypeSortKey({Int32(previous)}).sort_key;
  for (unsigned index = 0; index < 100000; ++index) {
    state = state * 1664525u + 1013904223u;
    const auto candidate = state <= 0x7fffffffu
        ? static_cast<std::int64_t>(state)
        : static_cast<std::int64_t>(state) - (std::int64_t{1} << 32u);
    std::string encoded;
    std::int64_t decoded = 0;
    const auto key = dt::MakeDatatypeSortKey({Int32(candidate)});
    Check(dt::EncodeCanonicalInt32Value(candidate, &encoded) &&
              dt::DecodeCanonicalInt32Value(encoded, &decoded) &&
              decoded == candidate && key.ok(),
          "deterministic int32 domain sample round-trips");
    const auto prior = Int32(previous);
    const auto current = Int32(candidate);
    const auto compare = dt::CompareDatatypeValues({prior, current});
    Check(compare.ok() && compare.comparison ==
              (previous < candidate ? -1 : previous > candidate ? 1 : 0) &&
              ((previous_key < key.sort_key) == (previous < candidate)) &&
              ((previous_key > key.sort_key) == (previous > candidate)),
          "deterministic int32 sample comparison equals sort-key order");
    previous = candidate;
    previous_key = key.sort_key;
  }

  std::string unchanged = "sentinel";
  std::int64_t unchanged_number = 0x1122334455667788LL;
  Check(!dt::EncodeCanonicalInt32Value(-2147483649LL, &unchanged) &&
            unchanged == "sentinel" &&
            !dt::EncodeCanonicalInt32Value(2147483648LL, &unchanged) &&
            unchanged == "sentinel" &&
            !dt::EncodeCanonicalInt32Value(0, nullptr),
        "int32 mathematical encoder fails atomically outside range");
  for (const auto width : {0u, 1u, 2u, 3u, 5u, 32u}) {
    const std::string malformed(width, '\0');
    unchanged_number = 0x1122334455667788LL;
    Check(!dt::DecodeCanonicalInt32Value(malformed, &unchanged_number) &&
              unchanged_number == 0x1122334455667788LL,
          "int32 mathematical decoder rejects malformed width atomically");
    const std::vector<platform::byte> bad(width, 0);
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::int32, false, false, bad}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::int32,
                dt::DatatypePhysicalValueState::value, bad}).ok(),
          "int32 codecs reject non-four-byte present payload");
    const dt::DatatypeOperationValue operation{
        dt::CanonicalTypeId::int32, malformed, false};
    Check(!dt::CompareDatatypeValues({operation, Int32(0)}).ok() &&
              !dt::MakeDatatypeSortKey({operation}).ok() &&
              !dt::SerializeDatatypeValue({operation}).ok() &&
              !dt::HashDatatypeValue({operation}).ok() &&
              !dt::RenderDatatypeValueForDisplay({operation}).ok(),
          "int32 operation surfaces reject malformed carrier widths");
  }
  Check(!dt::DecodeCanonicalInt32Value(std::string(4, '\0'), nullptr),
        "int32 decoder refuses a null output pointer");

  std::int64_t text_like = 0;
  Check(dt::DecodeCanonicalInt32Value(std::string{"1234", 4}, &text_like) &&
            text_like == 875770417,
        "four text-like bytes remain native int32 0x34333231");

  dt::DatatypeExtractRequest extract;
  extract.value = {dt::CanonicalTypeId::date, "2026-09-30", false};
  extract.field = "year";
  extract.result_descriptor = Int32Descriptor();
  Check(!dt::ExtractDatatypeField(extract).ok(),
        "temporal extraction refuses until an exact int32 result profile exists");

  for (const auto& vector : vectors) {
    const auto input = Int32(vector.value);
    const auto serialized = dt::SerializeDatatypeValue({input});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::int32;
    request.serialized_value = serialized.serialized_value;
    const auto restored = dt::DeserializeDatatypeValue(request);
    Check(serialized.ok() && restored.ok() &&
              restored.value.type_id == dt::CanonicalTypeId::int32 &&
              !restored.value.is_null &&
              restored.value.encoded_value == input.encoded_value,
          "generic value serialization preserves int32 native bytes");
  }
  for (const char* bad : {"SBDV1;type=int32;state=value;payload=",
                          "SBDV1;type=int32;state=value;payload=00",
                          "SBDV1;type=int32;state=value;payload=000000",
                          "SBDV1;type=int32;state=value;payload=0000000000"}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::int32;
    request.serialized_value = bad;
    Check(!dt::DeserializeDatatypeValue(request).ok(),
          "generic value deserializer rejects malformed int32 widths");
  }
}

void BulkImportTextConverter() {
  struct Accepted {
    std::string_view text;
    std::int64_t value;
  };
  constexpr std::array<Accepted, 9> accepted{{
      {"-2147483648", -2147483648LL},
      {"-65536", -65536},
      {"-1", -1},
      {"0", 0},
      {"1", 1},
      {"127", 127},
      {"32768", 32768},
      {"65536", 65536},
      {"2147483647", 2147483647LL},
  }};
  for (const auto& vector : accepted) {
    std::string actual;
    std::string expected;
    Check(dt::EncodeCanonicalInt32BulkImportTextV1(vector.text, &actual) &&
              dt::EncodeCanonicalInt32Value(vector.value, &expected) &&
              actual == expected,
          "int32 bulk-import text converter emits canonical LE4");
  }

  constexpr std::array<std::string_view, 23> rejected{{
      "", "+0", "+1", "-0", "00", "01", "-00", "-01", " 0",
      "0 ", "\t0", "0\n", "1_000", "1,000", "1.0", "1e0", "--1",
      "2147483648", "-2147483649", "4294967295", "abc", "0x1", "\xc2\xb9",
  }};
  for (const auto text : rejected) {
    std::string output = "unchanged";
    Check(!dt::EncodeCanonicalInt32BulkImportTextV1(text, &output) &&
              output == "unchanged",
          "int32 bulk-import text converter rejects noncanonical input atomically");
  }
  Check(!dt::EncodeCanonicalInt32BulkImportTextV1("0", nullptr),
        "int32 bulk-import text converter rejects a null output");
}

void StructuredPropertyPartitions() {
  const auto verify = [](std::uint32_t raw, bool have_previous,
                         std::int64_t previous_value,
                         const std::string& previous_key) {
    const std::int64_t expected_value = raw <= 0x7fffffffu
        ? static_cast<std::int64_t>(raw)
        : static_cast<std::int64_t>(raw) - (std::int64_t{1} << 32u);
    const std::string expected_bytes{
        static_cast<char>(raw & 0xffu),
        static_cast<char>((raw >> 8u) & 0xffu),
        static_cast<char>((raw >> 16u) & 0xffu),
        static_cast<char>((raw >> 24u) & 0xffu)};
    const std::string expected_key{
        '\x01',
        static_cast<char>(((raw >> 24u) & 0xffu) ^ 0x80u),
        static_cast<char>((raw >> 16u) & 0xffu),
        static_cast<char>((raw >> 8u) & 0xffu),
        static_cast<char>(raw & 0xffu)};

    std::string encoded;
    std::int64_t decoded = 0;
    const bool encoded_ok =
        dt::EncodeCanonicalInt32Value(expected_value, &encoded);
    const dt::DatatypeOperationValue value{
        dt::CanonicalTypeId::int32, encoded, false};
    const std::vector<platform::byte> payload(expected_bytes.begin(),
                                              expected_bytes.end());
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::int32, false, false, payload});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int32,
         dt::DatatypePhysicalValueState::value, payload});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto equal = dt::CompareDatatypeValues({value, value});
    const auto key = dt::MakeDatatypeSortKey({value});
    bool ordered = true;
    if (have_previous) {
      const auto previous = Int32(previous_value);
      const auto forward = dt::CompareDatatypeValues({previous, value});
      const auto reverse = dt::CompareDatatypeValues({value, previous});
      ordered = previous_value < expected_value && forward.ok() &&
          forward.comparison == -1 && reverse.ok() &&
          reverse.comparison == 1 && previous_key < key.sort_key;
    }
    Check(encoded_ok && encoded == expected_bytes &&
              dt::DecodeCanonicalInt32Value(encoded, &decoded) &&
              decoded == expected_value && binary.ok() && binary_back.ok() &&
              binary_back.value.payload == payload && physical.ok() &&
              physical_back.ok() && physical_back.value.payload == payload &&
              equal.ok() && equal.comparison == 0 && key.ok() &&
              key.sort_key == expected_key && ordered,
          "structured int32 partition agrees with independent byte/order oracle");
    return std::pair{expected_value, expected_key};
  };

  for (const std::uint32_t high : {0x0000u, 0x7fffu, 0x8000u, 0xffffu}) {
    bool have_previous = false;
    std::int64_t previous_value = 0;
    std::string previous_key;
    for (std::uint32_t low = 0; low <= 0xffffu; ++low) {
      const auto observed = verify((high << 16u) | low, have_previous,
                                   previous_value, previous_key);
      previous_value = observed.first;
      previous_key = observed.second;
      have_previous = true;
    }
  }
  for (const std::uint32_t low :
       {0x0000u, 0x0001u, 0x7fffu, 0x8000u, 0xffffu}) {
    bool have_previous = false;
    std::int64_t previous_value = 0;
    std::string previous_key;
    for (std::uint32_t ordinal = 0; ordinal <= 0xffffu; ++ordinal) {
      const std::uint32_t high = ordinal < 0x8000u
          ? ordinal + 0x8000u
          : ordinal - 0x8000u;
      const auto observed = verify((high << 16u) | low, have_previous,
                                   previous_value, previous_key);
      previous_value = observed.first;
      previous_key = observed.second;
      have_previous = true;
    }
  }
}

void NullAndAbsentPolicies() {
  auto descriptor = Int32Descriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue null_value{
      dt::CanonicalTypeId::int32, {}, true};
  null_value.descriptor = descriptor;

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::int32, true, false, {}});
  const auto binary_back = binary_null.ok()
      ? dt::DecodeDatatypeBinaryValue(binary_null.encoded)
      : dt::DatatypeBinaryResult{};
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::int32,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto physical_back = physical_null.ok()
      ? dt::DecodeDatatypePhysicalValue(physical_null.bytes.data(),
                                        physical_null.bytes.size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(binary_null.ok() && binary_back.ok() && binary_back.value.is_null &&
            binary_back.value.payload.empty() && physical_null.ok() &&
            physical_back.ok() &&
            physical_back.value.type_id == dt::CanonicalTypeId::int32 &&
            physical_back.value.state ==
                dt::DatatypePhysicalValueState::sql_null &&
            physical_back.value.payload.empty(),
        "int32 typed NULL is external state with zero payload");
  Check(!dt::EncodeDatatypeBinaryValue(
             {dt::CanonicalTypeId::int32, true, false, {0,0,0,0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
             {dt::CanonicalTypeId::int32,
              dt::DatatypePhysicalValueState::sql_null, {0,0,0,0}}).ok(),
        "int32 codecs reject payload-bearing NULL");
  const auto present_zero = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::int32, false, false, {0,0,0,0}});
  Check(present_zero.ok() && present_zero.encoded != binary_null.encoded,
        "present int32 zero remains distinct from typed NULL");

  const auto serialized_null = dt::SerializeDatatypeValue({null_value});
  dt::DatatypeDeserializationRequest null_decode;
  null_decode.expected_type_id = dt::CanonicalTypeId::int32;
  null_decode.expected_descriptor = descriptor;
  null_decode.serialized_value = serialized_null.serialized_value;
  const auto restored_null = dt::DeserializeDatatypeValue(null_decode);
  Check(serialized_null.ok() && restored_null.ok() &&
            restored_null.value.type_id == dt::CanonicalTypeId::int32 &&
            restored_null.value.is_null &&
            restored_null.value.encoded_value.empty(),
        "generic value serialization preserves typed int32 NULL state");
  null_decode.serialized_value =
      "SBDV1;type=int32;state=null;payload=00000000";
  Check(!dt::DeserializeDatatypeValue(null_decode).ok(),
        "generic value deserialization rejects payload-bearing int32 NULL");

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = null_value;
    identity.target_type_id = dt::CanonicalTypeId::int32;
    identity.target_descriptor = descriptor;
    identity.context = context;
    identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto result = dt::CastDatatypeValue(identity);
    Check(result.ok() && result.category == dt::DatatypeCastCategory::identity &&
              result.value.is_null && result.value.encoded_value.empty(),
          "typed int32 NULL identity validates exact descriptor/state");

    identity.target_descriptor.security_policy_uuid = FixtureV7Uuid(0x90u);
    identity.target_descriptor.modifier_flags |=
        scratchbird::engine::ExecutionTypeModifierFlagBit(
            scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
    CheckCastRefused(
        dt::CastDatatypeValue(identity), "DATATYPE.DESCRIPTOR.INVALID",
        "typed int32 NULL identity rejects a mismatched descriptor before cast policy");

    auto mismatched_and_malformed = identity;
    mismatched_and_malformed.value.encoded_value = "payload";
    CheckCastRefused(
        dt::CastDatatypeValue(mismatched_and_malformed),
        "DATATYPE.DESCRIPTOR.INVALID",
        "typed int32 NULL descriptor mismatch precedes malformed NULL state");

    auto mismatched_and_nonnullable = identity;
    mismatched_and_nonnullable.target_descriptor.nullable_allowed = false;
    CheckCastRefused(
        dt::CastDatatypeValue(mismatched_and_nonnullable),
        "DATATYPE.DESCRIPTOR.INVALID",
        "typed int32 NULL descriptor mismatch precedes target nullability");

    auto mismatched_and_bad_context = identity;
    mismatched_and_bad_context.context =
        static_cast<dt::DatatypeCastContext>(0xffu);
    CheckCastRefused(
        dt::CastDatatypeValue(mismatched_and_bad_context),
        "DATATYPE.DESCRIPTOR.INVALID",
        "typed int32 NULL descriptor mismatch precedes cast context");

    auto malformed_state = identity;
    malformed_state.target_descriptor = descriptor;
    malformed_state.value.encoded_value = "payload";
    CheckCastRefused(
        dt::CastDatatypeValue(malformed_state), "DATATYPE.NULL_STATE.INVALID",
        "typed int32 NULL with equal descriptors reports malformed state");

    auto nonnullable_target = identity;
    nonnullable_target.target_descriptor = descriptor;
    nonnullable_target.target_descriptor.nullable_allowed = false;
    nonnullable_target.value.descriptor =
        nonnullable_target.target_descriptor;
    CheckCastRefused(
        dt::CastDatatypeValue(nonnullable_target),
        "DATATYPE.NULL_NOT_ADMITTED",
        "typed int32 NULL with equal descriptors reports target nullability");

    auto bad_context = identity;
    bad_context.target_descriptor = descriptor;
    bad_context.context = static_cast<dt::DatatypeCastContext>(0xffu);
    CheckCastRefused(
        dt::CastDatatypeValue(bad_context), "DATATYPE.CAST_FORBIDDEN",
        "typed int32 NULL with equal descriptors reports invalid cast context");
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
    contextual.target_type_id = dt::CanonicalTypeId::int32;
    contextual.target_descriptor = descriptor;
    contextual.context = context;
    contextual.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    const auto bound = dt::CastDatatypeValue(contextual);
    Check(bound.ok() && bound.value.type_id == dt::CanonicalTypeId::int32 &&
              bound.value.is_null && bound.value.encoded_value.empty(),
          "contextual NULL binds to an exact nullable int32 descriptor");

    contextual.target_descriptor.nullable_allowed = false;
    CheckCastRefused(dt::CastDatatypeValue(contextual),
                     "DATATYPE.NULL_NOT_ADMITTED",
                     "contextual NULL refuses a non-nullable int32 descriptor");
    contextual.target_descriptor = descriptor;
    ++contextual.target_descriptor.descriptor_epoch;
    CheckCastRefused(
        dt::CastDatatypeValue(contextual), "DATATYPE.DESCRIPTOR.INVALID",
        "contextual NULL refuses a mismatched int32 descriptor generation");

    dt::DatatypeCastRequest standalone_target;
    standalone_target.value = Int32(0);
    standalone_target.target_type_id = dt::CanonicalTypeId::null_type;
    standalone_target.context = context;
    standalone_target.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    CheckCastRefused(dt::CastDatatypeValue(standalone_target),
                     "DATATYPE.CAST_FORBIDDEN",
                     "present int32 cannot cast to the standalone NULL sentinel");
    standalone_target.value = null_value;
    CheckCastRefused(
        dt::CastDatatypeValue(standalone_target), "DATATYPE.CAST_FORBIDDEN",
        "typed int32 NULL cannot cast to the standalone NULL sentinel");
  }

  for (const auto& candidate : dt::BuiltinDatatypeDescriptors()) {
    const auto outgoing = candidate.type_id == dt::CanonicalTypeId::int32 ? dt::DatatypeCastCategory::identity :
        (candidate.type_id == dt::CanonicalTypeId::int64 || candidate.type_id == dt::CanonicalTypeId::real64)
            ? dt::DatatypeCastCategory::lossless_implicit : dt::DatatypeCastCategory::forbidden;
    const auto incoming = candidate.type_id == dt::CanonicalTypeId::int32 ? dt::DatatypeCastCategory::identity :
        candidate.type_id == dt::CanonicalTypeId::real64 ? dt::DatatypeCastCategory::lossy_explicit :
        dt::DatatypeCastCategory::forbidden;
    for (bool compatibility : {false, true}) {
      Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::int32, candidate.type_id, compatibility) == outgoing &&
            dt::ClassifyDatatypeCast(candidate.type_id, dt::CanonicalTypeId::int32, compatibility) == incoming,
            "int32 cast matrix includes exact REAL64 widening and checked narrowing categories");
    }
  }
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest present;
    present.value = Int32(42);
    present.target_type_id = dt::CanonicalTypeId::int64;
    present.context = context;
    present.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present int32 outgoing cast refuses in every context");

    present.target_type_id = dt::CanonicalTypeId::int32;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present int32 identity cast refuses in every context");

    present.value = {dt::CanonicalTypeId::int16,
                     std::string{'\x2a', '\0'}, false};
    present.target_type_id = dt::CanonicalTypeId::int32;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present int32 incoming cast refuses in every context");

  }
  const auto target_descriptor = DescriptorFor(dt::CanonicalTypeId::int64);
  dt::DatatypeCastRequest overflowing_reverse;
  overflowing_reverse.value = {dt::CanonicalTypeId::int64,
      std::string{'\0', '\0', '\0', static_cast<char>(0x80), '\0', '\0', '\0', '\0'},
      false, target_descriptor};
  overflowing_reverse.target_type_id = dt::CanonicalTypeId::int32;
  overflowing_reverse.target_descriptor = descriptor;
  overflowing_reverse.context = dt::DatatypeCastContext::explicit_cast;
  CheckCastRefused(dt::CastDatatypeValue(overflowing_reverse), "DATATYPE.CAST_FORBIDDEN",
                   "widening never admits overflowing reverse narrowing or truncation");
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    for (std::int64_t number : {-2147483648LL, -65536LL, -1LL, 0LL, 1LL, 65536LL, 2147483647LL}) {
      for (bool nullable : {false, true}) for (bool target_nullable : {false, true}) {
        dt::DatatypeCastRequest request;
        request.value = Int32(number);
        request.value.descriptor = descriptor;
        request.value.descriptor.nullable_allowed = nullable;
        request.target_type_id = dt::CanonicalTypeId::int64;
        request.target_descriptor = target_descriptor;
        request.target_descriptor.nullable_allowed = target_nullable;
        request.context = context;
        const auto widened = dt::CastDatatypeValue(request);
        std::string expected;
        const auto raw = static_cast<std::uint64_t>(number);
        for (unsigned byte = 0; byte < 8; ++byte) expected.push_back(static_cast<char>(raw >> (byte * 8)));
        Check(widened.ok() && widened.category == dt::DatatypeCastCategory::lossless_implicit &&
              !widened.value.is_null && widened.value.encoded_value == expected &&
              widened.value.type_id == dt::CanonicalTypeId::int64 &&
              widened.value.descriptor.nullable_allowed == target_nullable,
              "int32 widening preserves signed value and exact target nullability");
        request.target_type_id = dt::CanonicalTypeId::int32;
        request.target_descriptor = descriptor;
        request.target_descriptor.nullable_allowed = target_nullable;
        const auto identity = dt::CastDatatypeValue(request);
        Check(identity.ok() && identity.category == dt::DatatypeCastCategory::identity &&
              identity.value.encoded_value == request.value.encoded_value &&
              identity.value.descriptor.nullable_allowed == target_nullable,
              "bound int32 identity preserves all bits in every context");
      }
    }
    dt::DatatypeCastRequest request;
    request.value = null_value;
    request.target_type_id = dt::CanonicalTypeId::int64;
    request.target_descriptor = target_descriptor;
    request.context = context;
    for (const auto target : {dt::CanonicalTypeId::int32, dt::CanonicalTypeId::int64}) {
      for (unsigned mutation = 0; mutation < 9; ++mutation) {
        auto invalid = request;
        invalid.value = Int32(42);
        invalid.value.descriptor = descriptor;
        invalid.target_type_id = target;
        invalid.target_descriptor = target == dt::CanonicalTypeId::int32 ? descriptor : target_descriptor;
        switch (mutation) {
          case 0: invalid.value.descriptor = {}; break;
          case 1: ++invalid.value.descriptor.descriptor_epoch; break;
          case 2: invalid.target_descriptor = {}; break;
          case 3: ++invalid.target_descriptor.descriptor_epoch; break;
          case 4: invalid.target_descriptor.precision = 1; break;
          case 5: invalid.value.encoded_value.pop_back(); break;
          case 6: invalid.value.encoded_value.push_back(0); break;
          case 7: invalid.value.is_null = true; break;
          case 8: invalid.value.descriptor.precision = 1; break;
        }
        const auto refused = dt::CastDatatypeValue(invalid);
        Check(!refused.ok() && refused.value.type_id == dt::CanonicalTypeId::unknown &&
              refused.value.encoded_value.empty(), "PRESENT int32 cast rejects malformed descriptor/carrier without output");
      }
    }
    const auto widened_null = dt::CastDatatypeValue(request);
    Check(widened_null.ok() && widened_null.value.is_null && widened_null.value.encoded_value.empty() &&
          widened_null.value.type_id == dt::CanonicalTypeId::int64,
          "int32 typed NULL widens to admitted nullable int64");
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
      auto invalid = request;
      switch (mutation) {
        case 0: invalid.value.descriptor = {}; break;
        case 1: ++invalid.value.descriptor.descriptor_epoch; break;
        case 2: invalid.target_descriptor = {}; break;
        case 3: ++invalid.target_descriptor.descriptor_epoch; break;
        case 4: invalid.target_descriptor.precision = 1; break;
        case 5: invalid.value.encoded_value = "hidden NULL bytes"; break;
        case 6: invalid.target_descriptor.nullable_allowed = false; break;
      }
      const auto refused = dt::CastDatatypeValue(invalid);
      Check(!refused.ok() && refused.value.type_id == dt::CanonicalTypeId::unknown &&
            refused.value.encoded_value.empty(), "int32 widening never bypasses NULL/descriptor authority");
    }
  }
  const auto null_first = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_last});
  const auto first_compare = dt::CompareDatatypeValues(
      {null_value, Int32(0), dt::DatatypeNullOrdering::nulls_first});
  const auto last_compare = dt::CompareDatatypeValues(
      {null_value, Int32(2147483647), dt::DatatypeNullOrdering::nulls_last});
  Check(null_first.ok() && null_first.sort_key == std::string(1, '\0') &&
            null_last.ok() && null_last.sort_key == std::string(1, '\2'),
        "int32 keys honor explicit containing NULL placement");
  Check(!first_compare.ok() && !last_compare.ok() &&
            !dt::CompareDatatypeValues({null_value, null_value}).ok(),
        "int32 NULL comparison refuses without an owning operator policy");

  Check(!dt::HashDatatypeValue({Int32(42)}).ok() &&
            !dt::HashDatatypeValue({null_value}).ok() &&
            !dt::RenderDatatypeValueForDisplay({Int32(42)}).ok(),
        "undefined int32 hash and display policies refuse");
  const dt::DatatypeOperationValue signed_value{
      dt::CanonicalTypeId::int16, std::string{'\x2a', '\0'}, false};
  Check(!dt::CompareDatatypeValues({Int32(42), signed_value}).ok(),
        "mixed-width signed comparison refuses without a registered rule");
}

template <std::size_t N>
void Append(std::vector<platform::byte>* output,
            const std::array<platform::byte, N>& bytes) {
  output->insert(output->end(), bytes.begin(), bytes.end());
}

void Persistence() {
  constexpr platform::Uuid descriptor_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x16}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x17}};
  constexpr platform::Uuid absent_codec_uuid{};
  constexpr std::array<platform::byte,28> value_min{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x66,0,0,0,1,0,0,0,
      4,0,0,0,0x48,0x1a,0x66,0x35,0,0,0,0x80}};
  constexpr std::array<platform::byte,28> value_minus_one{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x66,0,0,0,1,0,0,0,
      4,0,0,0,0x64,0x07,0x2e,0xda,0xff,0xff,0xff,0xff}};
  constexpr std::array<platform::byte,28> value_zero{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x66,0,0,0,1,0,0,0,
      4,0,0,0,0xc8,0x50,0x65,0xb5,0,0,0,0}};
  constexpr std::array<platform::byte,28> value_one{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x66,0,0,0,1,0,0,0,
      4,0,0,0,0xd9,0x8f,0x91,0x05,1,0,0,0}};
  constexpr std::array<platform::byte,28> value_max{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x66,0,0,0,1,0,0,0,
      4,0,0,0,0xe4,0x3d,0x2d,0x5a,0xff,0xff,0xff,0x7f}};
  constexpr std::array<platform::byte,24> sql_null{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x66,0,0,0,0,0,0,0,
      0,0,0,0,0xdb,0x55,0x22,0x05}};

  struct Oracle {
    std::int64_t value;
    const platform::byte* bytes;
    std::size_t size;
  };
  const std::array<Oracle,5> oracles{{
      {-2147483648LL,value_min.data(),value_min.size()},
      {-1,value_minus_one.data(),value_minus_one.size()},
      {0,value_zero.data(),value_zero.size()},
      {1,value_one.data(),value_one.size()},
      {2147483647LL,value_max.data(),value_max.size()}}};
  for (const auto& oracle : oracles) {
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int32, dt::DatatypePhysicalValueState::value,
         Payload(Int32(oracle.value))});
    Check(encoded.ok() && encoded.bytes.size() == oracle.size &&
              std::equal(encoded.bytes.begin(), encoded.bytes.end(), oracle.bytes),
          "production int32 physical frame matches independent hard-coded oracle");
  }
  const auto encoded_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::int32,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(encoded_null.ok() && encoded_null.bytes.size() == sql_null.size() &&
            std::equal(encoded_null.bytes.begin(), encoded_null.bytes.end(),
                       sql_null.begin()),
        "production int32 NULL frame matches independent hard-coded oracle");

  // This outer envelope is test-owned framing for the FileDevice reopen proof;
  // it is not a production disk format. The embedded SBDPV001 value frames are
  // the production datatype physical encoding under test.
  constexpr std::size_t test_owned_envelope_header_bytes = 120;
  std::vector<platform::byte> expected(test_owned_envelope_header_bytes, 0);
  const std::array<platform::byte,8> test_owned_magic{
      {'S','B','I','3','2','V','0','1'}};
  std::copy(test_owned_magic.begin(), test_owned_magic.end(), expected.begin());
  std::copy(descriptor_uuid.bytes.begin(), descriptor_uuid.bytes.end(),
            expected.begin() + 8);
  std::copy(type_uuid.bytes.begin(), type_uuid.bytes.end(), expected.begin() + 24);
  std::copy(absent_codec_uuid.bytes.begin(), absent_codec_uuid.bytes.end(),
            expected.begin() + 40);
  platform::StoreLittle32(expected.data() + 56, 1);
  platform::StoreLittle32(expected.data() + 60, 1);
  platform::StoreLittle32(expected.data() + 64, 0);
  platform::StoreLittle32(expected.data() + 68, 6);
  std::uint32_t offset = test_owned_envelope_header_bytes;
  for (unsigned index = 0; index < 6; ++index) {
    const std::uint32_t size = index == 5 ? 24 : 28;
    platform::StoreLittle32(expected.data() + 72 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 76 + index * 8, size);
    offset += size;
  }
  Append(&expected, value_min);
  Append(&expected, value_minus_one);
  Append(&expected, value_zero);
  Append(&expected, value_one);
  Append(&expected, value_max);
  Append(&expected, sql_null);

#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-int32-" + std::to_string(pid) + ".codec");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create int32 persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact int32 bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen int32 persistence fixture with an independent handle");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected &&
      std::equal(actual.begin() + 8, actual.begin() + 24,
                 descriptor_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 24, actual.begin() + 40,
                 type_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 40, actual.begin() + 56,
                 absent_codec_uuid.bytes.begin()) &&
      platform::LoadLittle32(actual.data() + 56) == 1 &&
      platform::LoadLittle32(actual.data() + 60) == 1 &&
      platform::LoadLittle32(actual.data() + 64) == 0 &&
      platform::LoadLittle32(actual.data() + 68) == 6;
  for (unsigned index = 0; index < 6 && decoded_all; ++index) {
    const auto frame_offset =
        platform::LoadLittle32(actual.data() + 72 + index * 8);
    const auto frame_size =
        platform::LoadLittle32(actual.data() + 76 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::int32 &&
        (index == 5
             ? decoded.value.state == dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state == dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Int32(oracles[index].value)));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "independent reopen preserves exact identities and int32 frames");

  std::array<platform::byte,2> short_bytes{};
  const auto short_read = reader.ReadAt(
      actual.size() - 1, short_bytes.data(), short_bytes.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_bytes.size(),
        "int32 persistence boundary refuses a short read");
  const auto rejected_write = reader.WriteAt(0, test_owned_magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only int32 persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened int32 fixture");

  for (const auto& oracle : oracles) {
    std::vector<platform::byte> corrupt(oracle.bytes, oracle.bytes + oracle.size);
    corrupt[20] ^= 0x01;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(oracle.bytes,
                                                oracle.size - 1).ok(),
          "int32 physical decoder rejects checksum corruption and truncation");
  }
  auto corrupt_null = sql_null;
  corrupt_null[20] ^= 0x01;
  Check(!dt::DecodeDatatypePhysicalValue(
             corrupt_null.data(), corrupt_null.size()).ok() &&
            !dt::DecodeDatatypePhysicalValue(
             sql_null.data(), sql_null.size() - 1).ok(),
        "int32 NULL physical frame rejects checksum corruption and truncation");
}

}  // namespace

int main() {
  ExactIdentity();
  RepresentationAndCodecs();
  BulkImportTextConverter();
  StructuredPropertyPartitions();
  NullAndAbsentPolicies();
  Persistence();
  std::cout << "base int32 checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
