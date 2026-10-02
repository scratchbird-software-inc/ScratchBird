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
#include <string>
#include <string_view>
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

void Check(bool condition, const std::string& reason) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << reason << '\n';
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
      const auto* text = argument.text();
      return text == nullptr ? std::string{} : *text;
    }
  }
  return {};
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
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type_id);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto descriptor =
      dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  return descriptor.ok() ? descriptor.descriptor
                         : scratchbird::engine::ExecutionTypeDescriptor{};
}

scratchbird::engine::ExecutionTypeDescriptor Int128Descriptor() {
  return DescriptorFor(dt::CanonicalTypeId::int128);
}

struct Vector {
  const char* decimal;
  std::array<platform::byte, 16> bytes;
};

constexpr std::array<Vector, 9> kVectors{{
    {"-170141183460469231731687303715884105728",
     {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x80}},
    {"-18446744073709551616",
     {0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {"-9223372036854775808",
     {0,0,0,0,0,0,0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {"-2", {0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {"-1", {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {"0", {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    {"1", {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    {"18446744073709551616", {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0}},
    {"170141183460469231731687303715884105727",
     {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f}},
}};

std::string Bytes(const std::array<platform::byte, 16>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<platform::byte> Payload(std::string_view bytes) {
  return {bytes.begin(), bytes.end()};
}

std::string OracleSignedKeyPayload(std::string_view little_endian) {
  if (little_endian.size() != 16) return {};
  std::string key(little_endian.rbegin(), little_endian.rend());
  key[0] = static_cast<char>(
      static_cast<unsigned char>(key[0]) ^ 0x80u);
  return key;
}

dt::DatatypeOperationValue Int128(std::string_view decimal) {
  std::string encoded;
  if (!dt::EncodeCanonicalInt128Value(decimal, &encoded)) return {};
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::int128, std::move(encoded), false};
  static const auto descriptor = Int128Descriptor();
  value.descriptor = descriptor;
  return value;
}

void ExactIdentity() {
  constexpr platform::Uuid descriptor_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x14}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x15}};

  const auto descriptor = Int128Descriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, descriptor_uuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::int128) &&
            descriptor.bit_width == 128,
        "int128 exact UUID descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_snapshot_uuid == dt::kDatatypeCohortV5 &&
        row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::int128);
  });
  Check(found != rows.end() && found->descriptor_uuid == descriptor_uuid &&
            found->type_uuid == type_uuid && found->codec_uuid == platform::Uuid{} &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.int128.le.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 16 &&
            found->canonical_value_maximum_bytes == 16 &&
            found->canonical_value_exact_bytes == 16 && found->null_supported &&
            found->signed_code && found->byte_order_code == 2 &&
            found->representation_code == 2,
        "int128 exact type/codec tuple without invented codec UUID");

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
              admitted.row.codec_id == "datatype.int128.le.v1",
          "int128 V1 identity survives exact successor cohort inheritance");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 4, 5, descriptor_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, type_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 2).ok,
        "int128 lookup rejects crossed generations and wrong UUID roles");

  dt::DatatypeStorageIdentityV1 storage;
  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 1, &storage) &&
            storage.descriptor_uuid == descriptor_uuid &&
            storage.type_uuid == type_uuid &&
            storage.type_id == dt::CanonicalTypeId::int128 &&
            storage.codec.has_value() &&
            storage.codec->codec_id == "datatype.int128.le.v1" &&
            storage.codec->codec_uuid == platform::Uuid{},
        "int128 storage identity retains UUID tuple and absent codec UUID");
  const auto layout = dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::int128);
  Check(layout.ok() &&
            layout.layout.storage_class == dt::DatatypeStorageClass::inline_fixed &&
            layout.layout.encoding ==
                dt::DatatypeBinaryEncoding::twos_complement_little_endian &&
            layout.layout.inline_bytes == 16 &&
            layout.layout.alignment_bytes == 16,
        "int128 storage layout is fixed signed LE16");
}

void RepresentationAndCodecs() {
  for (const auto& vector : kVectors) {
    const auto expected = Bytes(vector.bytes);
    std::string encoded = "unchanged";
    std::string decoded = "unchanged";
    const auto value = Int128(vector.decimal);
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::int128, false, false, Payload(expected)});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int128,
         dt::DatatypePhysicalValueState::value, Payload(expected)});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto equal = dt::CompareDatatypeValues({value, value});
    const auto key = dt::MakeDatatypeSortKey({value});
    const auto display = dt::RenderDatatypeValueForDisplay({value});
    Check(dt::EncodeCanonicalInt128Value(vector.decimal, &encoded) &&
              encoded == expected && value.encoded_value == expected &&
              dt::DecodeCanonicalInt128Value(expected, &decoded) &&
              decoded == vector.decimal && binary.ok() && binary_back.ok() &&
              binary_back.value.payload == Payload(expected) && physical.ok() &&
              physical_back.ok() && physical_back.value.payload == Payload(expected) &&
              equal.ok() && equal.comparison == 0 && display.ok() &&
              display.display_value == vector.decimal && !key.ok() &&
              key.diagnostic.diagnostic_code == "SB_DATATYPE_SORT_KEY_REJECTED",
          "int128 exact LE16, production codecs, compare, display, and key refusal vector");
  }

  for (std::size_t left = 0; left < kVectors.size(); ++left) {
    for (std::size_t right = 0; right < kVectors.size(); ++right) {
      const int expected = left < right ? -1 : left > right ? 1 : 0;
      const auto compared = dt::CompareDatatypeValues(
          {Int128(kVectors[left].decimal), Int128(kVectors[right].decimal)});
      Check(compared.ok() && compared.comparison == expected,
            "int128 signed comparison follows mathematical vector order");
    }
  }

  std::uint64_t state = 0x9e3779b97f4a7c15ULL;
  const auto next = [&state]() {
    state ^= state >> 12u;
    state ^= state << 25u;
    state ^= state >> 27u;
    return state * 2685821657736338717ULL;
  };
  auto previous = Int128("0");
  auto previous_key = OracleSignedKeyPayload(previous.encoded_value);
  for (unsigned sample = 0; sample < 20000; ++sample) {
    std::string bytes(16, '\0');
    for (unsigned word = 0; word < 2; ++word) {
      const auto random = next();
      for (unsigned byte = 0; byte < 8; ++byte) {
        bytes[word * 8 + byte] =
            static_cast<char>((random >> (byte * 8u)) & 0xffu);
      }
    }
    std::string decimal;
    std::string reencoded;
    dt::DatatypeOperationValue current{
        dt::CanonicalTypeId::int128, bytes, false};
    current.descriptor = Int128Descriptor();
    const auto current_key = OracleSignedKeyPayload(bytes);
    const int expected = previous_key < current_key ? -1
        : previous_key > current_key ? 1 : 0;
    const auto compared = dt::CompareDatatypeValues({previous, current});
    Check(dt::DecodeCanonicalInt128Value(bytes, &decimal) &&
              dt::EncodeCanonicalInt128Value(decimal, &reencoded) &&
              reencoded == bytes && compared.ok() &&
              compared.comparison == expected,
          "deterministic int128 sample preserves all bits and signed order");
    previous = std::move(current);
    previous_key = current_key;
  }

  std::string unchanged = "sentinel";
  for (const auto invalid : {"", "+1", " 1", "1 ", "00", "-0",
                             "170141183460469231731687303715884105728",
                             "-170141183460469231731687303715884105729"}) {
    const auto before = unchanged;
    Check(!dt::EncodeCanonicalInt128Value(invalid, &unchanged) &&
              unchanged == before,
          "int128 encoder rejects noncanonical or out-of-range text atomically");
  }
  Check(!dt::EncodeCanonicalInt128Value("0", nullptr),
        "int128 encoder rejects null output");

  for (const auto width : {0u,1u,8u,15u,17u,32u}) {
    const std::string malformed(width, '\0');
    std::string decoded = "sentinel";
    const auto before = decoded;
    dt::DatatypeOperationValue operation{
        dt::CanonicalTypeId::int128, malformed, false};
    operation.descriptor = Int128Descriptor();
    const auto comparison =
        dt::CompareDatatypeValues({operation, Int128("0")});
    const auto sort_key = dt::MakeDatatypeSortKey({operation});
    const auto serialized = dt::SerializeDatatypeValue({operation});
    const auto hashed = dt::HashDatatypeValue({operation});
    const auto displayed = dt::RenderDatatypeValueForDisplay({operation});
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::int128, false, false, Payload(malformed)});
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int128,
         dt::DatatypePhysicalValueState::value, Payload(malformed)});
    Check(!dt::DecodeCanonicalInt128Value(malformed, &decoded) &&
              decoded == before &&
              !comparison.ok() && comparison.comparison == 0 &&
              !sort_key.ok() && sort_key.sort_key.empty() &&
              !serialized.ok() && serialized.serialized_value.empty() &&
              !hashed.ok() && hashed.stable_hash_hex.empty() &&
              !displayed.ok() && displayed.display_value.empty() &&
              !binary.ok() && binary.encoded.empty() &&
              !physical.ok() && physical.bytes.empty(),
          "int128 operations and codecs reject malformed widths atomically");
  }
  Check(!dt::DecodeCanonicalInt128Value(Bytes(kVectors[5].bytes), nullptr),
        "int128 decoder rejects null output");

  const std::string text_like = "1234567890123456";
  std::string mathematical;
  std::string reencoded;
  Check(dt::DecodeCanonicalInt128Value(text_like, &mathematical) &&
            dt::EncodeCanonicalInt128Value(mathematical, &reencoded) &&
            reencoded == text_like,
        "sixteen text-like bytes remain one native binary int128 value");

  for (const auto& vector : kVectors) {
    const auto input = Int128(vector.decimal);
    const auto serialized = dt::SerializeDatatypeValue({input});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::int128;
    request.expected_descriptor = serialized.descriptor;
    request.serialized_value = serialized.serialized_value;
    const auto restored = dt::DeserializeDatatypeValue(request);
    Check(serialized.ok() && restored.ok() &&
              restored.value.type_id == dt::CanonicalTypeId::int128 &&
              restored.value.encoded_value == input.encoded_value &&
              SameUuidBytes(serialized.descriptor.descriptor_uuid,
                            input.descriptor.descriptor_uuid) &&
              SameUuidBytes(restored.value.descriptor.descriptor_uuid,
                            input.descriptor.descriptor_uuid),
          "generic value frame plus descriptor sidecar preserve int128 LE16 value");
  }
}

void NumericAndCastAdapter() {
  auto descriptor = Int128Descriptor();
  const auto numeric = [&](dt::DatatypeNumericOperationKind operation,
                           std::string_view left,
                           std::string_view right = "0") {
    dt::DatatypeNumericOperationRequest request;
    request.operation = operation;
    request.type_id = dt::CanonicalTypeId::int128;
    request.left = Int128(left);
    request.right = Int128(right);
    request.result_descriptor = descriptor;
    return dt::ApplyNumericOperation(request);
  };
  auto result = numeric(dt::DatatypeNumericOperationKind::add,
                        "170141183460469231731687303715884105726", "1");
  std::string decoded;
  Check(result.ok() && result.value.encoded_value.size() == 16 &&
            dt::DecodeCanonicalInt128Value(result.value.encoded_value, &decoded) &&
            decoded == "170141183460469231731687303715884105727" &&
            SameUuidBytes(result.value.descriptor.descriptor_uuid,
                          descriptor.descriptor_uuid),
        "int128 addition returns canonical LE16 with UUID descriptor");
  result = numeric(dt::DatatypeNumericOperationKind::subtract, "-1", "1");
  Check(result.ok() &&
            dt::DecodeCanonicalInt128Value(result.value.encoded_value, &decoded) &&
            decoded == "-2",
        "int128 subtraction returns canonical LE16");
  result = numeric(dt::DatatypeNumericOperationKind::multiply,
                   "18446744073709551616", "2");
  Check(result.ok() &&
            dt::DecodeCanonicalInt128Value(result.value.encoded_value, &decoded) &&
            decoded == "36893488147419103232",
        "int128 multiplication returns canonical LE16");
  result = numeric(dt::DatatypeNumericOperationKind::divide, "-7", "2");
  Check(result.ok() &&
            dt::DecodeCanonicalInt128Value(result.value.encoded_value, &decoded) &&
            decoded == "-3",
        "int128 division preserves backend truncation and LE16 result");
  const auto addition_overflow = numeric(
      dt::DatatypeNumericOperationKind::add,
      "170141183460469231731687303715884105727", "1");
  const auto divide_by_zero = numeric(
      dt::DatatypeNumericOperationKind::divide, "1", "0");
  const auto division_overflow = numeric(
      dt::DatatypeNumericOperationKind::divide,
      "-170141183460469231731687303715884105728", "-1");
  Check(!addition_overflow.ok() &&
            addition_overflow.value.encoded_value.empty() &&
            addition_overflow.diagnostic.diagnostic_code ==
                "NUMERIC.INT128.OVERFLOW" &&
            addition_overflow.numeric_facts.overflow &&
            !divide_by_zero.ok() &&
            divide_by_zero.value.encoded_value.empty() &&
            divide_by_zero.diagnostic.diagnostic_code ==
                "SB_DATATYPE_NUMERIC_OPERATION_REJECTED" &&
            DiagnosticDetail(divide_by_zero.diagnostic) ==
                "numeric.int128_divide_by_zero" &&
            divide_by_zero.numeric_facts.divide_by_zero &&
            !division_overflow.ok() &&
            division_overflow.value.encoded_value.empty() &&
            division_overflow.diagnostic.diagnostic_code ==
                "NUMERIC.INT128.OVERFLOW" &&
            division_overflow.numeric_facts.overflow,
        "int128 overflow and division failures publish no value");

  dt::DatatypeNumericOperationRequest compare;
  compare.operation = dt::DatatypeNumericOperationKind::compare;
  compare.type_id = dt::CanonicalTypeId::int128;
  compare.left = Int128("-1");
  compare.right = Int128("1");
  compare.result_descriptor = DescriptorFor(dt::CanonicalTypeId::boolean);
  const auto compared = dt::ApplyNumericOperation(compare);
  Check(compared.ok() && compared.comparison < 0 &&
            compared.value.type_id == dt::CanonicalTypeId::boolean &&
            SameUuidBytes(compared.value.descriptor.descriptor_uuid,
                          compare.result_descriptor.descriptor_uuid),
        "int128 numeric compare accepts Boolean result descriptor");

  dt::DatatypeNumericOperationRequest malformed_numeric;
  malformed_numeric.operation = dt::DatatypeNumericOperationKind::add;
  malformed_numeric.type_id = dt::CanonicalTypeId::int128;
  malformed_numeric.left = Int128("1");
  malformed_numeric.right = Int128("2");
  malformed_numeric.result_descriptor = descriptor;
  malformed_numeric.left.encoded_value.resize(15);
  const auto malformed_left = dt::ApplyNumericOperation(malformed_numeric);
  malformed_numeric.left = Int128("1");
  malformed_numeric.right.encoded_value.resize(17, '\0');
  const auto malformed_right = dt::ApplyNumericOperation(malformed_numeric);
  Check(!malformed_left.ok() && malformed_left.value.encoded_value.empty() &&
            malformed_left.diagnostic.diagnostic_code ==
                "NUMERIC.ENCODING.NONCANONICAL" &&
            !malformed_right.ok() && malformed_right.value.encoded_value.empty() &&
            malformed_right.diagnostic.diagnostic_code ==
                "NUMERIC.ENCODING.NONCANONICAL",
        "int128 numeric adapter rejects malformed LE16 operands atomically");

  malformed_numeric.left = Int128("1");
  malformed_numeric.left.descriptor = {};
  malformed_numeric.right = Int128("2");
  const auto missing_descriptor =
      dt::ApplyNumericOperation(malformed_numeric);
  Check(!missing_descriptor.ok() &&
            missing_descriptor.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID",
        "int128 numeric adapter rejects missing UUID descriptor explicitly");

  malformed_numeric.left = {dt::CanonicalTypeId::character, "1", false};
  malformed_numeric.right = Int128("1");
  const auto type_mismatch = dt::ApplyNumericOperation(malformed_numeric);
  Check(!type_mismatch.ok() &&
            DiagnosticDetail(type_mismatch.diagnostic) ==
                "numeric_argument_type_mismatch",
        "int128 numeric type mismatch is independent of payload width");

  auto different_descriptor = descriptor;
  different_descriptor.security_policy_uuid = FixtureV7Uuid(0xd0);
  different_descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  dt::DatatypeNumericOperationRequest descriptor_mismatch;
  descriptor_mismatch.operation = dt::DatatypeNumericOperationKind::add;
  descriptor_mismatch.type_id = dt::CanonicalTypeId::int128;
  descriptor_mismatch.left = Int128("1");
  descriptor_mismatch.right = Int128("2");
  descriptor_mismatch.right.descriptor = different_descriptor;
  descriptor_mismatch.result_descriptor = descriptor;
  const auto mismatched_operand =
      dt::ApplyNumericOperation(descriptor_mismatch);
  descriptor_mismatch.right = Int128("2");
  descriptor_mismatch.result_descriptor = different_descriptor;
  const auto mismatched_result =
      dt::ApplyNumericOperation(descriptor_mismatch);
  Check(!mismatched_operand.ok() &&
            mismatched_operand.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            !mismatched_result.ok() &&
            mismatched_result.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID",
        "int128 arithmetic refuses operand or result descriptor substitution");

  descriptor_mismatch.left = Int128("1");
  descriptor_mismatch.right = Int128("2");
  descriptor_mismatch.left.descriptor.stable_name = "entier_signe_128";
  descriptor_mismatch.right.descriptor.stable_name = "signed wide integer";
  descriptor_mismatch.result_descriptor = descriptor;
  const auto alias_operands = dt::ApplyNumericOperation(descriptor_mismatch);
  Check(alias_operands.ok(),
        "int128 arithmetic identity ignores localized descriptor labels");

  dt::DatatypeCastRequest cast;
  cast.value = {dt::CanonicalTypeId::character,
                "170141183460469231731687303715884105727", false};
  cast.target_type_id = dt::CanonicalTypeId::int128;
  cast.context = dt::DatatypeCastContext::explicit_cast;
  cast.explicit_cast = true;
  cast.target_descriptor = descriptor;
  const auto casted = dt::CastDatatypeValue(cast);
  Check(casted.ok() && casted.value.encoded_value == Bytes(kVectors.back().bytes),
        "admitted character-to-int128 cast returns LE16");
  cast.value.encoded_value = "170141183460469231731687303715884105728";
  Check(!dt::CastDatatypeValue(cast).ok(),
        "character-to-int128 overflow cast refuses");

  dt::DatatypeCastRequest render;
  render.value = Int128("-18446744073709551616");
  render.target_type_id = dt::CanonicalTypeId::character;
  render.context = dt::DatatypeCastContext::explicit_cast;
  render.explicit_cast = true;
  const auto rendered = dt::CastDatatypeValue(render);
  Check(rendered.ok() && rendered.value.encoded_value == "-18446744073709551616",
        "admitted int128-to-character cast uses backend canonical text");

  dt::DatatypeCastRequest identity;
  identity.value = Int128("42");
  identity.value.descriptor.stable_name = "entier_signe_128";
  identity.target_type_id = dt::CanonicalTypeId::int128;
  identity.target_descriptor = identity.value.descriptor;
  identity.target_descriptor.stable_name = "signed wide integer";
  const auto identity_value = dt::CastDatatypeValue(identity);
  Check(identity_value.ok() &&
            identity_value.category == dt::DatatypeCastCategory::identity &&
            identity_value.value.descriptor.stable_name ==
                "entier_signe_128" &&
            identity_value.value.encoded_value == Int128("42").encoded_value,
        "int128 identity cast uses UUID metadata and preserves source alias");
  identity.target_descriptor.security_policy_uuid = FixtureV7Uuid(0xc0);
  identity.target_descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  Check(!dt::CastDatatypeValue(identity).ok(),
        "int128 identity cast refuses descriptor metadata substitution");

  dt::DatatypeCastRequest bfloat_cast;
  bfloat_cast.value = Int128("42");
  bfloat_cast.target_type_id = dt::CanonicalTypeId::bfloat16;
  bfloat_cast.context = dt::DatatypeCastContext::explicit_cast;
  bfloat_cast.explicit_cast = true;
  bfloat_cast.target_descriptor = DescriptorFor(dt::CanonicalTypeId::bfloat16);
  const auto bfloat_source_before = bfloat_cast.value.encoded_value;
  const auto bfloat_value = dt::CastDatatypeValue(bfloat_cast);
  Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::int128,
                                 dt::CanonicalTypeId::bfloat16) ==
                dt::DatatypeCastCategory::forbidden &&
            !bfloat_value.ok() &&
            bfloat_value.category == dt::DatatypeCastCategory::forbidden &&
            bfloat_value.value.type_id == dt::CanonicalTypeId::unknown &&
            bfloat_value.value.encoded_value.empty() &&
            !bfloat_value.value.is_null &&
            bfloat_cast.value.encoded_value == bfloat_source_before,
        "int128 PRESENT-to-bfloat16 cast refuses without publishing output");

  for (const auto target : {dt::CanonicalTypeId::real16,
                            dt::CanonicalTypeId::real32,
                            dt::CanonicalTypeId::real64,
                            dt::CanonicalTypeId::real128}) {
    dt::DatatypeCastRequest real_cast;
    real_cast.value = Int128("42");
    real_cast.target_type_id = target;
    real_cast.context = dt::DatatypeCastContext::explicit_cast;
    real_cast.explicit_cast = true;
    const auto real_value = dt::CastDatatypeValue(real_cast);
    Check(real_value.ok() && real_value.value.type_id == target &&
              real_value.value.encoded_value == "42",
          "admitted int128-to-real cast consumes decoded decimal boundary");
  }
  dt::DatatypeCastRequest real128_cast;
  real128_cast.value = Int128("42");
  real128_cast.target_type_id = dt::CanonicalTypeId::real128;
  real128_cast.context = dt::DatatypeCastContext::explicit_cast;
  real128_cast.explicit_cast = true;
  real128_cast.target_descriptor = DescriptorFor(dt::CanonicalTypeId::real128);
  const auto real128_value = dt::CastDatatypeValue(real128_cast);
  Check(real128_value.ok() &&
            SameUuidBytes(real128_value.value.descriptor.descriptor_uuid,
                          real128_cast.target_descriptor.descriptor_uuid),
        "int128-to-real128 cast preserves supplied target descriptor");

  dt::DatatypeCastRequest compatibility;
  compatibility.value = Int128("42");
  compatibility.target_type_id = dt::CanonicalTypeId::binary;
  compatibility.context = dt::DatatypeCastContext::explicit_cast;
  compatibility.explicit_cast = true;
  compatibility.reference_compatibility_profile = true;
  const auto compatibility_value = dt::CastDatatypeValue(compatibility);
  Check(compatibility_value.ok() &&
            compatibility_value.category ==
                dt::DatatypeCastCategory::reference_compatibility_explicit &&
            compatibility_value.value.encoded_value == "42",
        "existing reference conversion receives boundary text instead of LE16");
  Check(!dt::HashDatatypeValue({Int128("0")}).ok(),
        "int128 hash refuses without a descriptor-versioned profile");
}

void DescriptorAndNullState() {
  auto descriptor = Int128Descriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue null_value{dt::CanonicalTypeId::int128, {}, true};
  null_value.descriptor = descriptor;

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = null_value;
    identity.target_type_id = dt::CanonicalTypeId::int128;
    identity.target_descriptor = descriptor;
    identity.context = context;
    identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto bound = dt::CastDatatypeValue(identity);
    Check(bound.ok() && bound.value.is_null &&
              bound.value.encoded_value.empty() &&
              SameUuidBytes(bound.value.descriptor.descriptor_uuid,
                            descriptor.descriptor_uuid),
          "typed int128 NULL identity preserves UUID and zero payload");

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::int128;
    contextual.target_descriptor = descriptor;
    contextual.context = context;
    contextual.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto contextual_result = dt::CastDatatypeValue(contextual);
    Check(contextual_result.ok() && contextual_result.value.is_null &&
              contextual_result.value.type_id == dt::CanonicalTypeId::int128 &&
              contextual_result.value.encoded_value.empty() &&
              SameUuidBytes(contextual_result.value.descriptor.descriptor_uuid,
                            descriptor.descriptor_uuid),
          "contextual NULL binds exact int128 descriptor in every context");
  }

  auto alias_a = descriptor;
  auto alias_b = descriptor;
  alias_a.stable_name = "signed wide integer";
  alias_b.stable_name = "entier_signe_128";
  auto left = Int128("-1");
  auto right = Int128("1");
  left.descriptor = alias_a;
  right.descriptor = alias_b;
  const auto aliases = dt::CompareDatatypeValues({left, right});
  Check(aliases.ok() && aliases.comparison < 0,
        "localized int128 labels share UUID-bound comparison identity");

  const dt::DatatypeOperationValue descriptorless{
      dt::CanonicalTypeId::int128, Bytes(kVectors[5].bytes), false};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "int128";
  Check(!dt::CompareDatatypeValues({descriptorless, descriptorless}).ok() &&
            !dt::CompareDatatypeValues({descriptorless, Int128("0")}).ok() &&
            !dt::CompareDatatypeValues({Int128("0"), descriptorless}).ok() &&
            !dt::CompareDatatypeValues({label_only, label_only}).ok() &&
            !dt::SerializeDatatypeValue({label_only}).ok(),
        "missing or label-only int128 descriptors cannot replace UUID identity");

  auto mismatched = Int128("1");
  mismatched.descriptor.security_policy_uuid = FixtureV7Uuid(0xb0);
  mismatched.descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  Check(!dt::CompareDatatypeValues({Int128("0"), mismatched}).ok() &&
            !dt::CompareDatatypeValues({mismatched, Int128("0")}).ok(),
        "int128 comparison rejects unequal descriptor metadata both ways");

  const auto null_first = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_last});
  Check(null_first.ok() && null_first.sort_key == std::string(1, '\0') &&
            null_last.ok() && null_last.sort_key == std::string(1, '\2') &&
            !dt::CompareDatatypeValues({null_value, Int128("0")}).ok() &&
            !dt::HashDatatypeValue({null_value}).ok(),
        "int128 NULL keys are state-only while compare/hash policy refuses");

  dt::DatatypeNumericOperationRequest numeric_compare;
  numeric_compare.operation = dt::DatatypeNumericOperationKind::compare;
  numeric_compare.type_id = dt::CanonicalTypeId::int128;
  numeric_compare.left = null_value;
  numeric_compare.right = Int128("0");
  auto boolean_descriptor = DescriptorFor(dt::CanonicalTypeId::boolean);
  boolean_descriptor.nullable_allowed = true;
  numeric_compare.result_descriptor = boolean_descriptor;
  const auto numeric_null = dt::ApplyNumericOperation(numeric_compare);
  Check(!numeric_null.ok() &&
            DiagnosticDetail(numeric_null.diagnostic) ==
                "int128_null_comparison_policy_unresolved",
        "int128 numeric NULL comparison also refuses unresolved policy");
}

std::uint32_t OraclePhysicalChecksum(
    dt::DatatypePhysicalValueState state,
    const std::vector<platform::byte>& payload) {
  std::uint32_t value = 2166136261u;
  const auto mix = [&value](std::uint32_t next) {
    value ^= next;
    value *= 16777619u;
  };
  mix(static_cast<std::uint32_t>(dt::CanonicalTypeId::int128));
  mix(static_cast<std::uint32_t>(state));
  for (const auto byte : payload) mix(byte);
  return value;
}

std::vector<platform::byte> OraclePhysicalFrame(
    dt::DatatypePhysicalValueState state,
    const std::vector<platform::byte>& payload) {
  std::vector<platform::byte> frame(24 + payload.size(), 0);
  const std::array<platform::byte,8> magic{{'S','B','D','P','V','0','0','1'}};
  std::copy(magic.begin(), magic.end(), frame.begin());
  platform::StoreLittle32(frame.data() + 8,
      static_cast<std::uint32_t>(dt::CanonicalTypeId::int128));
  platform::StoreLittle16(frame.data() + 12,
      static_cast<std::uint16_t>(state));
  platform::StoreLittle16(frame.data() + 14, 0);
  platform::StoreLittle32(frame.data() + 16,
      static_cast<std::uint32_t>(payload.size()));
  platform::StoreLittle32(frame.data() + 20,
      OraclePhysicalChecksum(state, payload));
  std::copy(payload.begin(), payload.end(), frame.begin() + 24);
  return frame;
}

void Append(std::vector<platform::byte>* output,
            const std::vector<platform::byte>& bytes) {
  output->insert(output->end(), bytes.begin(), bytes.end());
}

void Persistence() {
  constexpr platform::Uuid descriptor_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x14}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x15}};
  const std::string codec_id = "datatype.int128.le.v1";

  std::vector<std::vector<platform::byte>> frames;
  for (const auto index : {0u, 4u, 5u, 6u, 8u}) {
    const auto payload = Payload(Bytes(kVectors[index].bytes));
    const auto production = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int128,
         dt::DatatypePhysicalValueState::value, payload});
    const auto oracle = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    Check(production.ok() && production.bytes == oracle,
          "production int128 physical frame matches independent oracle");
    frames.push_back(oracle);
  }
  const auto production_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::int128,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto oracle_null = OraclePhysicalFrame(
      dt::DatatypePhysicalValueState::sql_null, {});
  Check(production_null.ok() && production_null.bytes == oracle_null,
        "production int128 NULL frame matches independent oracle");
  frames.push_back(oracle_null);

  // This test-owned envelope records identity metadata and offsets around the
  // production physical frames. It is evidence scaffolding, not a product
  // disk-format declaration.
  constexpr std::size_t header_bytes = 136;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte,8> magic{{'S','B','I','1','2','8','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(descriptor_uuid.bytes.begin(), descriptor_uuid.bytes.end(),
            expected.begin() + 8);
  std::copy(type_uuid.bytes.begin(), type_uuid.bytes.end(),
            expected.begin() + 24);
  platform::StoreLittle32(expected.data() + 40, 1);
  platform::StoreLittle32(expected.data() + 44, 1);
  platform::StoreLittle32(expected.data() + 48, 1);
  platform::StoreLittle32(expected.data() + 52, 1);
  platform::StoreLittle32(expected.data() + 56,
                          static_cast<std::uint32_t>(codec_id.size()));
  platform::StoreLittle32(expected.data() + 60,
                          static_cast<std::uint32_t>(frames.size()));
  std::copy(codec_id.begin(), codec_id.end(), expected.begin() + 64);
  std::uint32_t offset = header_bytes;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    platform::StoreLittle32(expected.data() + 88 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 92 + index * 8,
                            static_cast<std::uint32_t>(frames[index].size()));
    offset += static_cast<std::uint32_t>(frames[index].size());
  }
  for (const auto& frame : frames) Append(&expected, frame);

#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-int128-" + std::to_string(pid) + ".codec");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create int128 persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact int128 bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen int128 persistence fixture independently");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected &&
      std::equal(actual.begin() + 8, actual.begin() + 24,
                 descriptor_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 24, actual.begin() + 40,
                 type_uuid.bytes.begin()) &&
      platform::LoadLittle32(actual.data() + 40) == 1 &&
      platform::LoadLittle32(actual.data() + 44) == 1 &&
      platform::LoadLittle32(actual.data() + 48) == 1 &&
      platform::LoadLittle32(actual.data() + 52) == 1 &&
      platform::LoadLittle32(actual.data() + 56) == codec_id.size() &&
      platform::LoadLittle32(actual.data() + 60) == frames.size() &&
      std::equal(actual.begin() + 64,
                 actual.begin() + 64 + codec_id.size(), codec_id.begin());
  for (std::size_t index = 0; index < frames.size() && decoded_all; ++index) {
    const auto frame_offset = platform::LoadLittle32(actual.data() + 88 + index * 8);
    const auto frame_size = platform::LoadLittle32(actual.data() + 92 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::int128 &&
        (index + 1 == frames.size()
             ? decoded.value.state == dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state == dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload ==
                       Payload(Bytes(kVectors[std::array<unsigned,5>{0,4,5,6,8}[index]].bytes)));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "independent reopen preserves exact int128 identities and frames");

  std::array<platform::byte,2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size(),
        "int128 persistence boundary refuses a short read");
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only int128 persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened int128 fixture");

  for (const auto& frame : frames) {
    auto corrupt = frame;
    corrupt[20] ^= 1;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(frame.data(), frame.size() - 1).ok(),
          "int128 physical decoder rejects corruption and truncation");
  }
}

}  // namespace

int main() {
  ExactIdentity();
  RepresentationAndCodecs();
  NumericAndCastAdapter();
  DescriptorAndNullState();
  Persistence();
  std::cout << "base int128 checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
