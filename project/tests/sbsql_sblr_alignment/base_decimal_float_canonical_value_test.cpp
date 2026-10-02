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
#include "runtime_platform.hpp"
#include "sbl_numeric.hpp"

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
namespace numeric = scratchbird::libraries::sbl_numeric;
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
    if (failures <= 60) std::cerr << "FAIL: " << reason << '\n';
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
  const auto result =
      dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  return result.ok() ? result.descriptor
                     : scratchbird::engine::ExecutionTypeDescriptor{};
}

scratchbird::engine::ExecutionTypeDescriptor DecimalFloatDescriptor() {
  return DescriptorFor(dt::CanonicalTypeId::decimal_float);
}

std::vector<platform::byte> Payload(std::string_view bytes) {
  return {bytes.begin(), bytes.end()};
}

void AppendSetU16(std::string* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<char>((value >> 8u) & 0xffu));
  bytes->push_back(static_cast<char>(value & 0xffu));
}

void AppendSetU32(std::string* bytes, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
}

void AppendSetU64(std::string* bytes, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
}

void AppendSetUuid(std::string* bytes,
                   const scratchbird::engine::Uuid& uuid) {
  bytes->append(reinterpret_cast<const char*>(uuid.bytes), sizeof(uuid.bytes));
}

void AppendSetString(std::string* bytes, std::string_view value) {
  AppendSetU32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes->append(value);
}

std::string LowerHex(std::string_view bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    result.push_back(digits[byte >> 4u]);
    result.push_back(digits[byte & 0x0fu]);
  }
  return result;
}

std::string SetDescriptorFingerprint(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor) {
  std::string bytes = "SBTD1";
  AppendSetUuid(&bytes, descriptor.descriptor_uuid);
  AppendSetU64(&bytes, descriptor.descriptor_epoch);
  AppendSetU32(&bytes, descriptor.canonical_type_id);
  AppendSetU16(&bytes, static_cast<std::uint16_t>(descriptor.family));
  AppendSetU16(&bytes, static_cast<std::uint16_t>(descriptor.width_class));
  AppendSetString(&bytes, descriptor.stable_name);
  AppendSetU32(&bytes, descriptor.bit_width);
  AppendSetU32(&bytes, descriptor.precision);
  AppendSetU32(&bytes, descriptor.scale);
  AppendSetU32(&bytes, descriptor.length);
  AppendSetU32(&bytes, descriptor.vector_dimensions);
  AppendSetU32(&bytes, descriptor.container_rank);
  AppendSetU64(&bytes, descriptor.modifier_flags);
  AppendSetUuid(&bytes, descriptor.domain_uuid);
  AppendSetU32(&bytes,
               static_cast<std::uint32_t>(descriptor.domain_stack.size()));
  for (const auto& domain : descriptor.domain_stack) AppendSetUuid(&bytes, domain);
  AppendSetUuid(&bytes, descriptor.charset_uuid);
  AppendSetUuid(&bytes, descriptor.collation_uuid);
  AppendSetUuid(&bytes, descriptor.timezone_uuid);
  AppendSetUuid(&bytes, descriptor.element_descriptor_uuid);
  AppendSetUuid(&bytes, descriptor.security_policy_uuid);
  bytes.push_back(descriptor.nullable_allowed ? '\x01' : '\x00');
  bytes.push_back(descriptor.descriptor_authoritative ? '\x01' : '\x00');
  bytes.push_back(descriptor.parser_independent ? '\x01' : '\x00');
  return LowerHex(bytes);
}

std::string DecimalFloatSetFrame(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    std::string_view items, bool allow_nulls = false,
    bool allow_duplicates = false) {
  return "SBSET2;element=decimal_float;descriptor=" +
      SetDescriptorFingerprint(descriptor) +
      ";ordered=0;nulls=" + (allow_nulls ? "1" : "0") +
      ";duplicates=" + (allow_duplicates ? "1" : "0") +
      ";items=" + std::string(items);
}

std::string DecimalFloatValueFrame(std::string_view payload) {
  return "SBDV1;type=decimal_float;state=value;payload=" + LowerHex(payload);
}
template <std::size_t N>
std::string Bytes(const std::array<std::uint8_t, N>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Literal little-endian BID vectors. These do not use the production packer.
constexpr numeric::Decimal128Bytes kZero{{
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x30}};
constexpr numeric::Decimal128Bytes kNegativeZero{{
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x40,0xb0}};
constexpr numeric::Decimal128Bytes kOne{{
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x30}};
constexpr numeric::Decimal128Bytes kNegativeOne{{
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x40,0xb0}};
constexpr numeric::Decimal128Bytes kOneTenth{{
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x3e,0x30}};
constexpr numeric::Decimal128Bytes kOnePointZeroZero{{
    0x64,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x30}};
constexpr numeric::Decimal128Bytes kMinimum{{
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}};
constexpr numeric::Decimal128Bytes kMaximum{{
    0xff,0xff,0xff,0xff,0x63,0x8e,0x8d,0x37,
    0xc0,0x87,0xad,0xbe,0x09,0xed,0xff,0x5f}};
constexpr numeric::Decimal128Bytes kInfinity{{
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x78}};
constexpr numeric::Decimal128Bytes kQuietNan{{
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7c}};
constexpr numeric::Decimal128Bytes kSignalingNan123{{
    0x7b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7e}};
constexpr numeric::Decimal128Bytes kMalformedSteering{{
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60}};

constexpr numeric::Decimal128OrderKey kOneNumericOrderKey{{
    0x06,0x18,0x20,0x00,0x00,0x31,0x4d,0xc6,0x44,0x8d,0x93,
    0x38,0xc1,0x5b,0x0a,0x00,0x00,0x00,0x00,0x00,0x00}};

constexpr platform::Uuid kDescriptorUuid{{
    0xa1,0,0,0,0x10,0x65,0x73,0x69,0xad,0x61,0x6c,0x5f,0x66,0x6c,0x6f,0x61}};
constexpr platform::Uuid kSnapshotUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x05}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x24}};
constexpr platform::Uuid kCodecUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x25}};
constexpr platform::Uuid kNumericContextUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x26}};
constexpr platform::Uuid kSpecialPolicyUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x27}};
constexpr platform::Uuid kComparisonPolicyUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x28}};
constexpr std::string_view kCodecId = "datatype.decimal128.bid.le.v1";

dt::DatatypeOperationValue Present(
    const numeric::Decimal128Bytes& bytes = kOnePointZeroZero) {
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::decimal_float, Bytes(bytes), false};
  value.descriptor = DecimalFloatDescriptor();
  return value;
}

dt::DatatypeOperationValue TypedNull() {
  auto descriptor = DecimalFloatDescriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::decimal_float, {}, true};
  value.descriptor = descriptor;
  return value;
}

void ExactIdentityAndHardCodedVectors() {
  const auto descriptor = DecimalFloatDescriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::decimal_float) &&
            descriptor.precision == 34,
        "decimal_float has the exact UUID-bound catalog identity");

  const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
      dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 1);
  Check(dt::kDatatypeCohortV5 == kSnapshotUuid && admitted.ok &&
            admitted.row.descriptor_uuid == kDescriptorUuid &&
            admitted.row.type_uuid == kTypeUuid &&
            admitted.row.codec_uuid == kCodecUuid &&
            admitted.row.descriptor_generation == 1 &&
            admitted.row.type_generation == 1 &&
            admitted.row.codec_id == kCodecId &&
            admitted.row.codec_version == 1 &&
            admitted.row.codec_generation == 1 &&
            admitted.row.canonical_value_bytes == 16 &&
            admitted.row.canonical_value_minimum_bytes == 16 &&
            admitted.row.canonical_value_maximum_bytes == 16 &&
            admitted.row.canonical_value_exact_bytes == 16 &&
            admitted.row.canonical_byte_order == "little_endian" &&
            admitted.row.canonical_representation ==
                "IEEE754_decimal128_canonical_BID" &&
            admitted.row.sql_null_requires_zero_payload &&
            admitted.row.invalid_encoding_diagnostic_id ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            admitted.row.numeric_context_uuid == kNumericContextUuid &&
            admitted.row.numeric_context_generation == 1 &&
            admitted.row.special_value_policy_uuid == kSpecialPolicyUuid &&
            admitted.row.special_value_policy_generation == 1 &&
            admitted.row.comparison_policy_uuid == kComparisonPolicyUuid &&
            admitted.row.comparison_policy_generation == 1 &&
            admitted.row.comparison_profile ==
                "decimal128_numeric_total_nan_last_v1" &&
            admitted.row.allow_special_values,
        "decimal_float exact V5 tuple and policy UUIDs are immutable");

  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV1, 1, 1, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV2, 2, 2, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV3, 3, 3, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV4, 4, 4, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 4, 5, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 4, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, kTypeUuid, 1).ok,
        "the admitted descriptor/type/codec/policy row is V5-only; V4 storage-only bytes are not BID authority; crossed cohort generations and identities refuse");

  dt::DatatypeStorageIdentityV1 storage;
  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 1, &storage) &&
            storage.descriptor_uuid == kDescriptorUuid &&
            storage.type_uuid == kTypeUuid &&
            storage.type_id == dt::CanonicalTypeId::decimal_float &&
            storage.codec.has_value() &&
            storage.codec->codec_uuid == kCodecUuid &&
            storage.codec->codec_id == kCodecId,
        "decimal_float storage identity exposes the exact V5 tuple");

  struct Vector {
    const char* lexical;
    numeric::Decimal128Bytes bytes;
    bool special;
  };
  for (const auto& vector : std::array{
           Vector{"0", kZero, false},
           Vector{"-0", kNegativeZero, false},
           Vector{"1", kOne, false},
           Vector{"-1", kNegativeOne, false},
           Vector{"0.1", kOneTenth, false},
           Vector{"1.00", kOnePointZeroZero, false},
           Vector{"1E-6176", kMinimum, false},
           Vector{"9999999999999999999999999999999999E6111",
                  kMaximum, false},
           Vector{"Infinity", kInfinity, true},
           Vector{"NaN", kQuietNan, true},
           Vector{"sNaN123", kSignalingNan123, true}}) {
    const auto encoded =
        numeric::EncodeDecimal128LittleEndian(vector.lexical, vector.special);
    const auto decoded = numeric::DecodeDecimal128LittleEndian(
        vector.bytes.data(), vector.bytes.size(), vector.special);
    Check(encoded.numeric.status == numeric::NumericStatusCode::ok &&
              encoded.bytes == vector.bytes && encoded.value.has_value() &&
              decoded.numeric.status == numeric::NumericStatusCode::ok &&
              decoded.bytes == vector.bytes && decoded.value.has_value(),
          std::string("independent hard-coded LE16 BID vector: ") +
              vector.lexical);
  }

  const auto one_key = numeric::MakeDecimal128OrderKey(
      kOne.data(), kOne.size(),
      numeric::Decimal128OrderProfile::numeric_total_nan_last, true);
  Check(one_key.numeric.status == numeric::NumericStatusCode::ok &&
            one_key.key == kOneNumericOrderKey,
        "selected numeric-total profile produces the literal independent 21-byte key");

  const auto zero_key = numeric::MakeDecimal128OrderKey(
      kNegativeZero.data(), kNegativeZero.size(),
      numeric::Decimal128OrderProfile::numeric_total_nan_last, true);
  const numeric::Decimal128OrderKey kZeroNumericOrderKey{{
      0x05,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}};
  Check(zero_key.key == kZeroNumericOrderKey,
        "selected numeric-total profile coalesces signed zero to the literal key");

  for (const auto lexical :
       {"", "+", "-", ".", "e1", "1e", "1e+", "1e-", "1e1x",
        "1.2.3", " 1", "1 ", "1x", "1E6145", "1E-6177",
        "99999999999999999999999999999999999", "nan", "-Infinity"}) {
    const auto rejected = numeric::EncodeDecimal128LittleEndian(lexical);
    Check(rejected.numeric.status != numeric::NumericStatusCode::ok &&
              !rejected.bytes && !rejected.value,
          std::string("invalid or unadmitted lexical form refuses atomically: ") +
              lexical);
  }
  const auto embedded_nul =
      numeric::EncodeDecimal128LittleEndian(std::string("1\0", 2));
  Check(embedded_nul.numeric.status != numeric::NumericStatusCode::ok &&
            !embedded_nul.bytes && !embedded_nul.value,
        "embedded NUL lexical form refuses atomically");
}

void MalformedCarrierAndLowerBinary() {
  for (const auto width : {0u, 1u, 15u, 17u, 24u, 32u}) {
    std::vector<std::uint8_t> malformed(width, 0x5a);
    const auto decoded = numeric::DecodeDecimal128LittleEndian(
        malformed.data(), malformed.size(), true);
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::decimal_float, false, false,
         std::vector<platform::byte>(malformed.begin(), malformed.end())});
    Check(decoded.numeric.status != numeric::NumericStatusCode::ok &&
              decoded.numeric.diagnostic_code ==
                  "numeric.decimal128.width_invalid" &&
              !decoded.bytes && !decoded.value &&
              !binary.ok() && binary.encoded.empty(),
          "wrong-width decimal_float carrier refuses without publishing bytes");
  }

  struct MalformedVector {
    numeric::Decimal128Bytes bytes;
    const char* diagnostic;
  };
  std::vector<MalformedVector> malformed{{
      kMalformedSteering, "numeric.decimal128.finite_noncanonical"}};
  auto infinity_payload = kInfinity;
  infinity_payload[0] = 1;
  malformed.push_back(
      {infinity_payload, "numeric.decimal128.infinity_noncanonical"});
  auto reserved_nan = kQuietNan;
  reserved_nan[13] = 0x40;
  malformed.push_back(
      {reserved_nan, "numeric.decimal128.nan_noncanonical"});
  auto excessive_nan_payload = kQuietNan;
  std::fill(excessive_nan_payload.begin(), excessive_nan_payload.begin() + 14,
            0xff);
  excessive_nan_payload[13] = 0x3f;
  malformed.push_back(
      {excessive_nan_payload, "numeric.decimal128.nan_noncanonical"});
  numeric::Decimal128Bytes excessive_coefficient = kOne;
  std::fill(excessive_coefficient.begin(),
            excessive_coefficient.begin() + 14, 0xff);
  excessive_coefficient[14] = 0x41;
  malformed.push_back(
      {excessive_coefficient,
       "numeric.decimal128.coefficient_out_of_range"});

  for (const auto& vector : malformed) {
    const auto decoded = numeric::DecodeDecimal128LittleEndian(
        vector.bytes.data(), vector.bytes.size(), true);
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::decimal_float, false, false,
         Payload(Bytes(vector.bytes))});
    Check(decoded.numeric.status != numeric::NumericStatusCode::ok &&
              decoded.numeric.diagnostic_code == vector.diagnostic &&
              !decoded.bytes && !decoded.value &&
              !binary.ok() && binary.encoded.empty(),
          "noncanonical LE16 BID frame refuses atomically");
  }

  for (const auto& bytes : {kZero, kNegativeZero, kOne, kOnePointZeroZero,
                            kMinimum, kMaximum, kInfinity, kQuietNan,
                            kSignalingNan123}) {
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::decimal_float, false, false,
         Payload(Bytes(bytes))});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::decimal_float,
         dt::DatatypePhysicalValueState::value, Payload(Bytes(bytes))});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(binary.ok() && binary_back.ok() &&
              binary_back.value.type_id ==
                  dt::CanonicalTypeId::decimal_float &&
              binary_back.value.payload == Payload(Bytes(bytes)) &&
              physical.ok() && physical_back.ok() &&
              physical_back.value.type_id ==
                  dt::CanonicalTypeId::decimal_float &&
              physical_back.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              physical_back.value.payload == Payload(Bytes(bytes)),
          "lower binary and physical codecs preserve exact decimal128 BID bytes");
  }

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::decimal_float, true, false, {}});
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::decimal_float,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(binary_null.ok() && physical_null.ok() &&
            !dt::EncodeDatatypeBinaryValue(
                 {dt::CanonicalTypeId::decimal_float, true, false, {0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
                 {dt::CanonicalTypeId::decimal_float,
                  dt::DatatypePhysicalValueState::sql_null, {0}}).ok(),
        "lower codecs preserve external clean NULL and reject dirty NULL");
}

void NullAndDescriptorPrecedence() {
  auto descriptor = DecimalFloatDescriptor();
  descriptor.nullable_allowed = true;
  const auto typed_null = TypedNull();

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest typed;
    typed.value = typed_null;
    typed.target_type_id = dt::CanonicalTypeId::decimal_float;
    typed.target_descriptor = descriptor;
    typed.context = context;
    typed.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto typed_result = dt::CastDatatypeValue(typed);

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::decimal_float;
    contextual.target_descriptor = descriptor;
    contextual.context = context;
    contextual.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto contextual_result = dt::CastDatatypeValue(contextual);
    Check(typed_result.ok() && typed_result.value.is_null &&
              typed_result.value.type_id == dt::CanonicalTypeId::decimal_float &&
              typed_result.value.encoded_value.empty() &&
              contextual_result.ok() && contextual_result.value.is_null &&
              contextual_result.value.type_id == dt::CanonicalTypeId::decimal_float &&
              contextual_result.value.encoded_value.empty(),
          "same-descriptor typed NULL and contextual NULL bind in every context");
  }

  auto alias_source = typed_null;
  alias_source.descriptor.stable_name = "exact-number-alias";
  auto alias_target = descriptor;
  alias_target.stable_name = "numeric-display-label";
  dt::DatatypeCastRequest alias;
  alias.value = alias_source;
  alias.target_type_id = dt::CanonicalTypeId::decimal_float;
  alias.target_descriptor = alias_target;
  const auto alias_result = dt::CastDatatypeValue(alias);
  Check(alias_result.ok() && alias_result.value.is_null &&
            alias_result.value.descriptor.stable_name ==
                alias_source.descriptor.stable_name,
        "descriptor display aliases do not determine decimal_float typed-NULL identity");

  auto mismatched = descriptor;
  mismatched.security_policy_uuid = FixtureV7Uuid(0xc0);
  mismatched.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  alias.value = typed_null;
  alias.target_descriptor = mismatched;
  const auto mismatch = dt::CastDatatypeValue(alias);

  auto missing = typed_null;
  missing.descriptor = {};
  alias.value = missing;
  alias.target_descriptor = descriptor;
  const auto missing_result = dt::CastDatatypeValue(alias);
  missing.descriptor.stable_name = "decimal_float";
  alias.value = missing;
  const auto label_result = dt::CastDatatypeValue(alias);
  alias.value = typed_null;
  alias.value.encoded_value = Bytes(kZero);
  const auto dirty = dt::CastDatatypeValue(alias);
  alias.value = typed_null;
  alias.target_descriptor = descriptor;
  alias.target_descriptor.nullable_allowed = false;
  const auto nonnullable = dt::CastDatatypeValue(alias);
  alias.value = {dt::CanonicalTypeId::null_type, {}, true};
  const auto contextual_nonnullable = dt::CastDatatypeValue(alias);
  Check(!mismatch.ok() && !missing_result.ok() && !label_result.ok() &&
            !dirty.ok() && !nonnullable.ok() &&
            !contextual_nonnullable.ok() &&
            RejectedAs(mismatch, "DATATYPE.DESCRIPTOR.INVALID",
                       "typed_null_identity_descriptor_mismatch") &&
            RejectedAs(missing_result, "DATATYPE.DESCRIPTOR.INVALID",
                       "source_descriptor_invalid") &&
            RejectedAs(label_result, "DATATYPE.DESCRIPTOR.INVALID",
                       "source_descriptor_invalid") &&
            RejectedAs(dirty, "DATATYPE.NULL_STATE.INVALID",
                       "null_or_descriptor_state_invalid") &&
            RejectedAs(nonnullable, "DATATYPE.DESCRIPTOR.INVALID",
                       "typed_null_identity_descriptor_mismatch") &&
            RejectedAs(contextual_nonnullable,
                       "DATATYPE.NULL_NOT_ADMITTED",
                       "target_descriptor_does_not_admit_null"),
        "mismatched missing label-only dirty and nonnullable decimal_float NULLs refuse");

  dt::DatatypeNumericOperationRequest numeric;
  numeric.type_id = dt::CanonicalTypeId::decimal_float;
  numeric.operation = dt::DatatypeNumericOperationKind::canonicalize;
  numeric.left = typed_null;
  const auto missing_numeric_result = dt::ApplyNumericOperation(numeric);
  numeric.result_descriptor = descriptor;
  numeric.result_descriptor.nullable_allowed = false;
  const auto nonnullable_numeric_result = dt::ApplyNumericOperation(numeric);
  Check(!missing_numeric_result.ok() && !nonnullable_numeric_result.ok() &&
            missing_numeric_result.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            DiagnosticDetail(missing_numeric_result.diagnostic) ==
                "numeric_result_descriptor_invalid" &&
            nonnullable_numeric_result.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_NOT_ADMITTED" &&
            DiagnosticDetail(nonnullable_numeric_result.diagnostic) ==
                "numeric_result_descriptor_not_nullable" &&
            missing_numeric_result.value.encoded_value.empty() &&
            nonnullable_numeric_result.value.encoded_value.empty(),
        "decimal_float NULL result descriptor failures precede unresolved policy");

  auto nullable_present = Present();
  nullable_present.descriptor = typed_null.descriptor;
  for (const auto ordering : {dt::DatatypeNullOrdering::nulls_first,
                              dt::DatatypeNullOrdering::nulls_last}) {
    const auto present_null = dt::CompareDatatypeValues(
        {nullable_present, typed_null, ordering});
    const auto null_present = dt::CompareDatatypeValues(
        {typed_null, nullable_present, ordering});
    const auto null_null = dt::CompareDatatypeValues(
        {typed_null, typed_null, ordering});
    Check(RejectedAs(present_null, "SB_DATATYPE_COMPARISON_REJECTED",
                     "decimal_float_comparison_policy_unresolved") &&
              RejectedAs(null_present, "SB_DATATYPE_COMPARISON_REJECTED",
                         "decimal_float_comparison_policy_unresolved") &&
              RejectedAs(null_null, "SB_DATATYPE_COMPARISON_REJECTED",
                         "decimal_float_comparison_policy_unresolved") &&
              present_null.comparison == 0 && null_present.comparison == 0 &&
              null_null.comparison == 0,
          "decimal_float comparison with NULL refuses under both NULL orderings");
  }

  auto mismatched_present = nullable_present;
  mismatched_present.descriptor = mismatched;
  const auto mismatch_left =
      dt::CompareDatatypeValues({mismatched_present, nullable_present});
  const auto mismatch_right =
      dt::CompareDatatypeValues({nullable_present, mismatched_present});
  dt::DatatypeNumericOperationRequest mismatched_numeric;
  mismatched_numeric.type_id = dt::CanonicalTypeId::decimal_float;
  mismatched_numeric.operation = dt::DatatypeNumericOperationKind::add;
  mismatched_numeric.left = nullable_present;
  mismatched_numeric.right = mismatched_present;
  mismatched_numeric.result_descriptor = nullable_present.descriptor;
  const auto mismatch_numeric_result =
      dt::ApplyNumericOperation(mismatched_numeric);
  Check(RejectedAs(mismatch_left, "DATATYPE.DESCRIPTOR.INVALID",
                   "decimal_float_operand_descriptor_invalid_or_mismatch") &&
            RejectedAs(mismatch_right, "DATATYPE.DESCRIPTOR.INVALID",
                       "decimal_float_operand_descriptor_invalid_or_mismatch") &&
            RejectedAs(mismatch_numeric_result,
                       "DATATYPE.DESCRIPTOR.INVALID",
                       "decimal_float_right_descriptor_invalid") &&
            mismatch_left.comparison == 0 && mismatch_right.comparison == 0 &&
            mismatch_numeric_result.value.encoded_value.empty(),
        "valid but unequal decimal_float descriptors fail before comparison and numeric policy");

}

void PresentSemanticSurfacesRefuse() {
  const auto present = Present();
  const auto original = present.encoded_value;
  auto character_descriptor = DescriptorFor(dt::CanonicalTypeId::character);

  for (const auto& candidate : dt::BuiltinDatatypeDescriptors()) {
    Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::decimal_float,
                                   candidate.type_id) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::decimal_float) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::decimal_float,
                                       candidate.type_id, true) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::decimal_float, true) ==
                  dt::DatatypeCastCategory::forbidden,
          "every registered PRESENT cast classifier pair incident to decimal_float refuses");

    const std::string expected_detail =
        candidate.type_id == dt::CanonicalTypeId::bfloat16
            ? "bfloat16_present_cast_policy_unresolved"
        : candidate.type_id == dt::CanonicalTypeId::real16
            ? "real16_present_cast_policy_unresolved"
        : candidate.type_id == dt::CanonicalTypeId::real32
            ? "real32_present_cast_policy_unresolved"
        : candidate.type_id == dt::CanonicalTypeId::real64
            ? "real64_present_cast_policy_unresolved"
        : candidate.type_id == dt::CanonicalTypeId::decimal
            ? "decimal_present_cast_policy_unresolved"
            : "decimal_float_present_cast_policy_unresolved";
    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      for (const bool compatibility : {false, true}) {
        dt::DatatypeCastRequest outgoing;
        outgoing.value = present;
        outgoing.target_type_id = candidate.type_id;
        outgoing.target_descriptor = DescriptorFor(candidate.type_id);
        outgoing.context = context;
        outgoing.explicit_cast =
            context == dt::DatatypeCastContext::explicit_cast;
        outgoing.reference_compatibility_profile = compatibility;
        const auto result = dt::CastDatatypeValue(outgoing);
        Check(RejectedAs(result, "DATATYPE.CAST_FORBIDDEN",
                         expected_detail) &&
                  result.value.type_id == dt::CanonicalTypeId::unknown &&
                  result.value.encoded_value.empty() &&
                  present.encoded_value == original,
              "every registered decimal_float PRESENT outgoing cast refuses atomically in every context and compatibility profile");

        if (candidate.type_id != dt::CanonicalTypeId::decimal_float) {
          const std::string null_detail =
              candidate.type_id == dt::CanonicalTypeId::decimal
                  ? "decimal_cross_type_typed_null_cast_policy_unresolved"
                  : "decimal_float_cross_type_typed_null_cast_policy_unresolved";
          auto candidate_null = dt::DatatypeOperationValue{
              candidate.type_id, {}, true};
          candidate_null.descriptor = DescriptorFor(candidate.type_id);

          dt::DatatypeCastRequest null_outgoing;
          null_outgoing.value = TypedNull();
          null_outgoing.target_type_id = candidate.type_id;
          null_outgoing.target_descriptor = candidate_null.descriptor;
          null_outgoing.context = context;
          null_outgoing.explicit_cast =
              context == dt::DatatypeCastContext::explicit_cast;
          null_outgoing.reference_compatibility_profile = compatibility;

          dt::DatatypeCastRequest null_incoming;
          null_incoming.value = candidate_null;
          null_incoming.target_type_id = dt::CanonicalTypeId::decimal_float;
          null_incoming.target_descriptor = TypedNull().descriptor;
          null_incoming.context = context;
          null_incoming.explicit_cast =
              context == dt::DatatypeCastContext::explicit_cast;
          null_incoming.reference_compatibility_profile = compatibility;

          const auto outgoing_null_result =
              dt::CastDatatypeValue(null_outgoing);
          const auto incoming_null_result =
              dt::CastDatatypeValue(null_incoming);
          Check(RejectedAs(outgoing_null_result, "DATATYPE.CAST_FORBIDDEN",
                           null_detail) &&
                    RejectedAs(incoming_null_result,
                               "DATATYPE.CAST_FORBIDDEN", null_detail) &&
                    outgoing_null_result.value.encoded_value.empty() &&
                    incoming_null_result.value.encoded_value.empty(),
                "every registered decimal_float cross-type typed NULL cast refuses in both directions");
        }
      }
    }
  }

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    for (const bool compatibility : {false, true}) {
      dt::DatatypeCastRequest identity;
      identity.value = present;
      identity.target_type_id = dt::CanonicalTypeId::decimal_float;
      identity.target_descriptor = present.descriptor;
      identity.context = context;
      identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
      identity.reference_compatibility_profile = compatibility;
      const auto identity_result = dt::CastDatatypeValue(identity);

      dt::DatatypeCastRequest outgoing;
      outgoing.value = present;
      outgoing.target_type_id = dt::CanonicalTypeId::character;
      outgoing.target_descriptor = character_descriptor;
      outgoing.context = context;
      outgoing.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
      outgoing.reference_compatibility_profile = compatibility;
      const auto outgoing_result = dt::CastDatatypeValue(outgoing);

      dt::DatatypeCastRequest incoming;
      incoming.value = {dt::CanonicalTypeId::character, "12.34", false};
      incoming.value.descriptor = character_descriptor;
      incoming.target_type_id = dt::CanonicalTypeId::decimal_float;
      incoming.target_descriptor = present.descriptor;
      incoming.context = context;
      incoming.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
      incoming.reference_compatibility_profile = compatibility;
      const auto incoming_result = dt::CastDatatypeValue(incoming);
      Check(RejectedAs(identity_result, "DATATYPE.CAST_FORBIDDEN",
                       "decimal_float_present_cast_policy_unresolved") &&
                RejectedAs(outgoing_result, "DATATYPE.CAST_FORBIDDEN",
                           "decimal_float_present_cast_policy_unresolved") &&
                RejectedAs(incoming_result, "DATATYPE.CAST_FORBIDDEN",
                           "decimal_float_present_cast_policy_unresolved") &&
                identity_result.value.type_id == dt::CanonicalTypeId::unknown &&
                identity_result.value.encoded_value.empty() &&
                outgoing_result.value.type_id == dt::CanonicalTypeId::unknown &&
                outgoing_result.value.encoded_value.empty() &&
                incoming_result.value.type_id == dt::CanonicalTypeId::unknown &&
                incoming_result.value.encoded_value.empty() &&
                present.encoded_value == original,
            "representative PRESENT decimal_float casts reach exact policy refusal atomically");

      character_descriptor.nullable_allowed = true;
      dt::DatatypeCastRequest null_outgoing;
      null_outgoing.value = TypedNull();
      null_outgoing.target_type_id = dt::CanonicalTypeId::character;
      null_outgoing.target_descriptor = character_descriptor;
      null_outgoing.context = context;
      null_outgoing.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      null_outgoing.reference_compatibility_profile = compatibility;
      const auto null_outgoing_result =
          dt::CastDatatypeValue(null_outgoing);

      dt::DatatypeCastRequest null_incoming;
      null_incoming.value = {dt::CanonicalTypeId::character, {}, true};
      null_incoming.value.descriptor = character_descriptor;
      null_incoming.target_type_id = dt::CanonicalTypeId::decimal_float;
      null_incoming.target_descriptor = TypedNull().descriptor;
      null_incoming.context = context;
      null_incoming.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      null_incoming.reference_compatibility_profile = compatibility;
      const auto null_incoming_result =
          dt::CastDatatypeValue(null_incoming);
      Check(RejectedAs(null_outgoing_result, "DATATYPE.CAST_FORBIDDEN",
                       "decimal_float_cross_type_typed_null_cast_policy_unresolved") &&
                RejectedAs(null_incoming_result, "DATATYPE.CAST_FORBIDDEN",
                           "decimal_float_cross_type_typed_null_cast_policy_unresolved") &&
                null_outgoing_result.value.type_id ==
                    dt::CanonicalTypeId::unknown &&
                null_outgoing_result.value.encoded_value.empty() &&
                null_incoming_result.value.type_id ==
                    dt::CanonicalTypeId::unknown &&
                null_incoming_result.value.encoded_value.empty(),
            "typed NULL decimal_float cross casts refuse in every context and compatibility profile");
    }
  }

  for (const auto operation : {
           dt::DatatypeNumericOperationKind::canonicalize,
           dt::DatatypeNumericOperationKind::add,
           dt::DatatypeNumericOperationKind::subtract,
           dt::DatatypeNumericOperationKind::multiply,
           dt::DatatypeNumericOperationKind::divide,
           dt::DatatypeNumericOperationKind::compare}) {
    dt::DatatypeNumericOperationRequest request;
    request.operation = operation;
    request.type_id = dt::CanonicalTypeId::decimal_float;
    request.left = present;
    request.right = present;
    request.result_descriptor = operation ==
            dt::DatatypeNumericOperationKind::compare
        ? DescriptorFor(dt::CanonicalTypeId::boolean)
        : present.descriptor;
    const auto result = dt::ApplyNumericOperation(request);
    Check(RejectedAs(result, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
                     "decimal_float_numeric_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() && result.comparison == 0 &&
              result.numeric_facts.invalid,
          "every PRESENT decimal_float numeric surface refuses atomically");
  }

  auto alias_left = present;
  auto alias_right = present;
  alias_left.descriptor.stable_name = "numeric-left-alias";
  alias_right.descriptor.stable_name = "numeric-right-alias";
  const auto alias_compare =
      dt::CompareDatatypeValues({alias_left, alias_right});
  Check(!alias_compare.ok() && alias_compare.comparison == 0 &&
            alias_compare.diagnostic.diagnostic_code ==
                "SB_DATATYPE_COMPARISON_REJECTED" &&
            DiagnosticDetail(alias_compare.diagnostic) ==
                "decimal_float_comparison_policy_unresolved",
        "alias-only name differences pass descriptor validation and reach decimal_float comparison refusal");

  auto descriptorless = present;
  descriptorless.descriptor = {};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "decimal_float";
  auto wrong_descriptor = present;
  wrong_descriptor.descriptor = character_descriptor;
  auto wrong_generation = present;
  ++wrong_generation.descriptor.descriptor_epoch;
  for (const auto& invalid :
       {descriptorless, label_only, wrong_descriptor, wrong_generation}) {
    dt::DatatypeCastRequest cast;
    cast.value = invalid;
    cast.target_type_id = dt::CanonicalTypeId::decimal_float;
    cast.target_descriptor = present.descriptor;
    const auto cast_result = dt::CastDatatypeValue(cast);
    dt::DatatypeNumericOperationRequest numeric_request;
    numeric_request.operation = dt::DatatypeNumericOperationKind::canonicalize;
    numeric_request.type_id = dt::CanonicalTypeId::decimal_float;
    numeric_request.left = invalid;
    numeric_request.result_descriptor = present.descriptor;
    const auto numeric_result = dt::ApplyNumericOperation(numeric_request);
    const auto compare_left = dt::CompareDatatypeValues({invalid, present});
    const auto compare_right = dt::CompareDatatypeValues({present, invalid});
    const auto key = dt::MakeDatatypeSortKey({invalid});
    const auto hash = dt::HashDatatypeValue({invalid});
    const auto display = dt::RenderDatatypeValueForDisplay({invalid});
    const auto serialized = dt::SerializeDatatypeValue({invalid});
    dt::DatatypeExtractRequest extract;
    extract.value = invalid;
    extract.field = "unsupported";
    const auto extracted = dt::ExtractDatatypeField(extract);
    Check(!cast_result.ok() && !numeric_result.ok() && !compare_left.ok() &&
              !compare_right.ok() && !key.ok() && !hash.ok() && !display.ok() &&
              !serialized.ok() && !extracted.ok() &&
              cast_result.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(cast_result.diagnostic) ==
                  "source_descriptor_invalid" &&
              numeric_result.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(numeric_result.diagnostic) ==
                  "decimal_float_left_descriptor_invalid" &&
              compare_left.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(compare_left.diagnostic) ==
                  "decimal_float_operand_descriptor_invalid_or_mismatch" &&
              compare_right.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(compare_right.diagnostic) ==
                  "decimal_float_operand_descriptor_invalid_or_mismatch" &&
              key.diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(key.diagnostic) ==
                  "canonical_value_encoding_invalid" &&
              hash.diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(hash.diagnostic) ==
                  "canonical_value_encoding_invalid" &&
              display.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(display.diagnostic) ==
                  "canonical_value_encoding_invalid" &&
              serialized.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(serialized.diagnostic) ==
                  "canonical_value_encoding_invalid" &&
              extracted.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(extracted.diagnostic) ==
                  "decimal_float_extract_descriptor_invalid" &&
              key.sort_key.empty() && hash.stable_hash_hex.empty() &&
              display.display_value.empty() &&
              serialized.serialized_value.empty() &&
              extracted.value.encoded_value.empty(),
          "invalid decimal_float PRESENT descriptors precede every semantic policy");
  }

  for (const auto& invalid_target :
       std::array{scratchbird::engine::ExecutionTypeDescriptor{},
                  [] {
                    scratchbird::engine::ExecutionTypeDescriptor descriptor;
                    descriptor.stable_name = "decimal_float";
                    return descriptor;
                  }(),
                  character_descriptor}) {
    dt::DatatypeCastRequest cast;
    cast.value = present;
    cast.target_type_id = dt::CanonicalTypeId::decimal_float;
    cast.target_descriptor = invalid_target;
    const auto result = dt::CastDatatypeValue(cast);
    Check(RejectedAs(result, "DATATYPE.DESCRIPTOR.INVALID",
                     "target_descriptor_invalid") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty(),
          "invalid decimal_float target descriptor precedes PRESENT cast policy");
  }

  const auto compared = dt::CompareDatatypeValues({present, present});
  const auto key = dt::MakeDatatypeSortKey({present});
  const auto hash = dt::HashDatatypeValue({present});
  const auto display = dt::RenderDatatypeValueForDisplay({present});
  const auto serialized = dt::SerializeDatatypeValue({present});
  dt::DatatypeExtractRequest extract;
  extract.value = present;
  extract.field = "unsupported";
  const auto extracted = dt::ExtractDatatypeField(extract);
  Check(!compared.ok() && compared.comparison == 0 && !key.ok() &&
            key.sort_key.empty() && !hash.ok() &&
            hash.stable_hash_hex.empty() && !display.ok() &&
            display.display_value.empty() && !serialized.ok() &&
            serialized.serialized_value.empty() && !extracted.ok() &&
            extracted.value.encoded_value.empty() &&
            compared.diagnostic.diagnostic_code ==
                "SB_DATATYPE_COMPARISON_REJECTED" &&
            DiagnosticDetail(compared.diagnostic) ==
                "decimal_float_comparison_policy_unresolved" &&
            key.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SORT_KEY_REJECTED" &&
            DiagnosticDetail(key.diagnostic) ==
                "decimal_float_sort_key_policy_unresolved" &&
            hash.diagnostic.diagnostic_code ==
                "SB_DATATYPE_HASH_REJECTED" &&
            DiagnosticDetail(hash.diagnostic) ==
                "decimal_float_hash_policy_unresolved" &&
            display.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DISPLAY_RENDER_REJECTED" &&
            DiagnosticDetail(display.diagnostic) ==
                "decimal_float_display_policy_unresolved" &&
            serialized.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SERIALIZATION_REJECTED" &&
            DiagnosticDetail(serialized.diagnostic) ==
                "decimal_float_serialization_policy_unresolved" &&
            extracted.diagnostic.diagnostic_code ==
                "SB_DATATYPE_EXTRACT_REJECTED" &&
            DiagnosticDetail(extracted.diagnostic) ==
                "decimal_float_extract_policy_unresolved",
        "compare key hash display serialization and extract refuse PRESENT decimal_float");

  auto malformed_header = Present(kMalformedSteering);
  auto malformed_width = present;
  malformed_width.encoded_value.pop_back();
  for (const auto& malformed : {malformed_header, malformed_width}) {
    dt::DatatypeCastRequest cast;
    cast.value = malformed;
    cast.target_type_id = dt::CanonicalTypeId::decimal_float;
    cast.target_descriptor = present.descriptor;
    const auto cast_result = dt::CastDatatypeValue(cast);

    dt::DatatypeNumericOperationRequest numeric_left;
    numeric_left.operation = dt::DatatypeNumericOperationKind::canonicalize;
    numeric_left.type_id = dt::CanonicalTypeId::decimal_float;
    numeric_left.left = malformed;
    numeric_left.result_descriptor = present.descriptor;
    const auto numeric_left_result =
        dt::ApplyNumericOperation(numeric_left);
    auto numeric_right = numeric_left;
    numeric_right.operation = dt::DatatypeNumericOperationKind::add;
    numeric_right.left = present;
    numeric_right.right = malformed;
    const auto numeric_right_result =
        dt::ApplyNumericOperation(numeric_right);

    const auto compare_left =
        dt::CompareDatatypeValues({malformed, present});
    const auto compare_right =
        dt::CompareDatatypeValues({present, malformed});
    const auto malformed_key = dt::MakeDatatypeSortKey({malformed});
    const auto malformed_hash = dt::HashDatatypeValue({malformed});
    const auto malformed_display =
        dt::RenderDatatypeValueForDisplay({malformed});
    const auto malformed_serialized =
        dt::SerializeDatatypeValue({malformed});
    dt::DatatypeExtractRequest malformed_extract;
    malformed_extract.value = malformed;
    malformed_extract.field = "unsupported";
    const auto malformed_extracted =
        dt::ExtractDatatypeField(malformed_extract);

    dt::DatatypeDeserializationRequest malformed_restore;
    malformed_restore.expected_type_id = dt::CanonicalTypeId::decimal_float;
    malformed_restore.expected_descriptor = present.descriptor;
    malformed_restore.serialized_value =
        DecimalFloatValueFrame(malformed.encoded_value);
    const auto malformed_restored =
        dt::DeserializeDatatypeValue(malformed_restore);

    Check(RejectedAs(cast_result, "NUMERIC.ENCODING.NONCANONICAL",
                     "decimal_float_source_value_noncanonical") &&
              RejectedAs(numeric_left_result,
                         "NUMERIC.ENCODING.NONCANONICAL",
                         "decimal_float_numeric_argument_invalid") &&
              RejectedAs(numeric_right_result,
                         "NUMERIC.ENCODING.NONCANONICAL",
                         "decimal_float_numeric_argument_invalid") &&
              RejectedAs(compare_left, "NUMERIC.ENCODING.NONCANONICAL",
                         "canonical_value_encoding_invalid") &&
              RejectedAs(compare_right, "NUMERIC.ENCODING.NONCANONICAL",
                         "canonical_value_encoding_invalid") &&
              RejectedAs(malformed_key, "NUMERIC.ENCODING.NONCANONICAL",
                         "canonical_value_encoding_invalid") &&
              RejectedAs(malformed_hash, "NUMERIC.ENCODING.NONCANONICAL",
                         "canonical_value_encoding_invalid") &&
              RejectedAs(malformed_display,
                         "NUMERIC.ENCODING.NONCANONICAL",
                         "canonical_value_encoding_invalid") &&
              RejectedAs(malformed_serialized,
                         "NUMERIC.ENCODING.NONCANONICAL",
                         "canonical_value_encoding_invalid") &&
              RejectedAs(malformed_extracted,
                         "NUMERIC.ENCODING.NONCANONICAL",
                         "decimal_float_extract_value_noncanonical") &&
              RejectedAs(malformed_restored,
                         "NUMERIC.ENCODING.NONCANONICAL",
                         "canonical_value_encoding_invalid") &&
              cast_result.value.encoded_value.empty() &&
              numeric_left_result.value.encoded_value.empty() &&
              numeric_right_result.value.encoded_value.empty() &&
              compare_left.comparison == 0 && compare_right.comparison == 0 &&
              malformed_key.sort_key.empty() &&
              malformed_hash.stable_hash_hex.empty() &&
              malformed_display.display_value.empty() &&
              malformed_serialized.serialized_value.empty() &&
              malformed_extracted.value.encoded_value.empty() &&
              malformed_restored.value.encoded_value.empty(),
          "malformed decimal_float carriers fail before every semantic policy and publish no output");
  }

  dt::DatatypeDeserializationRequest restore;
  restore.expected_type_id = dt::CanonicalTypeId::decimal_float;
  restore.expected_descriptor = present.descriptor;
  restore.serialized_value =
      "SBDV1;type=decimal_float;state=value;payload=";
  static constexpr char hex[] = "0123456789abcdef";
  for (const auto byte : kOnePointZeroZero) {
    restore.serialized_value.push_back(hex[byte >> 4]);
    restore.serialized_value.push_back(hex[byte & 15]);
  }
  const auto restored = dt::DeserializeDatatypeValue(restore);
  Check(!restored.ok() &&
            restored.value.type_id == dt::CanonicalTypeId::unknown &&
            restored.value.encoded_value.empty() &&
            restored.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
            DiagnosticDetail(restored.diagnostic) ==
                "decimal_float_deserialization_policy_unresolved",
        "generic framing cannot re-admit a PRESENT decimal_float value");

  for (const auto& invalid_descriptor :
       std::array{scratchbird::engine::ExecutionTypeDescriptor{},
                  [] {
                    scratchbird::engine::ExecutionTypeDescriptor descriptor;
                    descriptor.stable_name = "decimal_float";
                    return descriptor;
                  }(),
                  character_descriptor}) {
    auto invalid_restore = restore;
    invalid_restore.expected_descriptor = invalid_descriptor;
    const auto invalid_result =
        dt::DeserializeDatatypeValue(invalid_restore);
    Check(RejectedAs(invalid_result, "DATATYPE.DESCRIPTOR.INVALID",
                     "expected_descriptor_invalid") &&
              invalid_result.value.type_id == dt::CanonicalTypeId::unknown &&
              invalid_result.value.encoded_value.empty(),
          "invalid decimal_float expected descriptor precedes deserialization policy");
  }

  dt::DatatypeSetDescriptor set_descriptor;
  set_descriptor.element_type_id = dt::CanonicalTypeId::decimal_float;
  set_descriptor.element_descriptor = present.descriptor;
  const auto encoded_set = dt::EncodeSetValue(set_descriptor, {present});
  const auto empty_set = dt::EncodeSetValue(set_descriptor, {});
  auto malformed_set_element = present;
  malformed_set_element.encoded_value.pop_back();
  const auto malformed_set =
      dt::EncodeSetValue(set_descriptor, {malformed_set_element});
  Check(!encoded_set.ok() && encoded_set.encoded_set.empty() &&
            !empty_set.ok() && empty_set.encoded_set.empty() &&
            encoded_set.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SET_OPERATION_REJECTED" &&
            empty_set.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SET_OPERATION_REJECTED" &&
            DiagnosticDetail(encoded_set.diagnostic) ==
                "decimal_float_set_semantics_policy_unresolved" &&
            DiagnosticDetail(empty_set.diagnostic) ==
                "decimal_float_set_semantics_policy_unresolved" &&
            RejectedAs(malformed_set, "NUMERIC.ENCODING.NONCANONICAL",
                       "decimal_float_set_element_invalid") &&
            malformed_set.encoded_set.empty(),
        "PRESENT and empty decimal_float set construction refuse");

  const auto valid_frame = DecimalFloatSetFrame(
      present.descriptor, "V" + LowerHex(present.encoded_value));

  for (const auto operation : {dt::DatatypeSetOperationKind::membership,
                               dt::DatatypeSetOperationKind::equals,
                               dt::DatatypeSetOperationKind::subset,
                               dt::DatatypeSetOperationKind::superset,
                               dt::DatatypeSetOperationKind::cardinality}) {
    dt::DatatypeSetOperationRequest request;
    request.operation = operation;
    request.descriptor = set_descriptor;
    request.left_encoded_set = valid_frame;
    request.right_encoded_set = valid_frame;
    request.right_value = present;
    const auto result = dt::ApplySetOperation(request);
    Check(!result.ok() && result.encoded_set.empty() &&
              result.value.encoded_value.empty() &&
              result.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SET_OPERATION_REJECTED" &&
              DiagnosticDetail(result.diagnostic) ==
                  "decimal_float_set_semantics_policy_unresolved",
          "every valid decimal_float set semantic surface reaches policy refusal");
  }

  auto missing_set_descriptor = set_descriptor;
  missing_set_descriptor.element_descriptor = {};
  auto label_set_descriptor = missing_set_descriptor;
  label_set_descriptor.element_descriptor.stable_name = "decimal_float";
  auto wrong_set_descriptor = set_descriptor;
  wrong_set_descriptor.element_descriptor = character_descriptor;
  for (const auto& invalid_descriptor :
       {missing_set_descriptor, label_set_descriptor, wrong_set_descriptor}) {
    const auto encoded = dt::EncodeSetValue(invalid_descriptor, {present});
    dt::DatatypeSetOperationRequest request;
    request.operation = dt::DatatypeSetOperationKind::membership;
    request.descriptor = invalid_descriptor;
    request.left_encoded_set = valid_frame;
    request.right_value = present;
    const auto applied = dt::ApplySetOperation(request);
    Check(RejectedAs(encoded, "DATATYPE.DESCRIPTOR.INVALID",
                     "set_element_descriptor_invalid") &&
              RejectedAs(applied, "DATATYPE.DESCRIPTOR.INVALID",
                         "set_element_descriptor_invalid") &&
              encoded.encoded_set.empty() && applied.encoded_set.empty() &&
              applied.value.encoded_value.empty(),
          "invalid decimal_float set descriptors fail before frame and set policy");
  }

  const std::array<std::string, 2> malformed_left_frames{
      "not-a-set-frame",
      DecimalFloatSetFrame(present.descriptor, "V00")};
  for (const auto& malformed_frame : malformed_left_frames) {
    for (const auto operation : {dt::DatatypeSetOperationKind::membership,
                                 dt::DatatypeSetOperationKind::equals,
                                 dt::DatatypeSetOperationKind::subset,
                                 dt::DatatypeSetOperationKind::superset,
                                 dt::DatatypeSetOperationKind::cardinality}) {
      dt::DatatypeSetOperationRequest request;
      request.operation = operation;
      request.descriptor = set_descriptor;
      request.left_encoded_set = malformed_frame;
      request.right_encoded_set = valid_frame;
      request.right_value = present;
      const auto result = dt::ApplySetOperation(request);
      Check(RejectedAs(result, "SB_DATATYPE_SET_OPERATION_REJECTED",
                       "left_set_encoding_invalid") &&
                result.encoded_set.empty() &&
                result.value.encoded_value.empty(),
            "malformed decimal_float left set frame fails before set policy");
    }
  }

  auto mismatched_right_descriptor = present.descriptor;
  mismatched_right_descriptor.security_policy_uuid = FixtureV7Uuid(0xd0);
  mismatched_right_descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  const auto mismatched_right_frame = DecimalFloatSetFrame(
      mismatched_right_descriptor, "V" + LowerHex(present.encoded_value));
  const std::array<std::string, 2> malformed_right_frames{
      "not-a-set-frame",
      DecimalFloatSetFrame(present.descriptor, "V00")};
  for (const auto operation : {dt::DatatypeSetOperationKind::equals,
                               dt::DatatypeSetOperationKind::subset,
                               dt::DatatypeSetOperationKind::superset}) {
    for (const auto& malformed_frame : malformed_right_frames) {
      dt::DatatypeSetOperationRequest malformed_right;
      malformed_right.operation = operation;
      malformed_right.descriptor = set_descriptor;
      malformed_right.left_encoded_set = valid_frame;
      malformed_right.right_encoded_set = malformed_frame;
      const auto malformed_right_result =
          dt::ApplySetOperation(malformed_right);
      Check(RejectedAs(malformed_right_result,
                       "SB_DATATYPE_SET_OPERATION_REJECTED",
                       "right_set_encoding_invalid") &&
                malformed_right_result.value.encoded_value.empty(),
            "malformed right set frame fails before decimal_float set policy");
    }
    dt::DatatypeSetOperationRequest mismatched_right;
    mismatched_right.operation = operation;
    mismatched_right.descriptor = set_descriptor;
    mismatched_right.left_encoded_set = valid_frame;
    mismatched_right.right_encoded_set = mismatched_right_frame;
    const auto mismatched_right_result =
        dt::ApplySetOperation(mismatched_right);
    Check(RejectedAs(mismatched_right_result,
                     "DATATYPE.DESCRIPTOR.INVALID",
                     "right_set_descriptor_mismatch") &&
              mismatched_right_result.value.encoded_value.empty(),
          "descriptor-mismatched right set frame fails before decimal_float set policy");
  }

  const auto membership_result = [&](dt::DatatypeOperationValue value,
                                     bool use_nullable_descriptor = false,
                                     bool allow_nulls = false) {
    dt::DatatypeSetOperationRequest request;
    request.operation = dt::DatatypeSetOperationKind::membership;
    request.descriptor = set_descriptor;
    if (use_nullable_descriptor) {
      request.descriptor.element_descriptor = TypedNull().descriptor;
    }
    request.descriptor.allow_null_elements = allow_nulls;
    request.left_encoded_set = DecimalFloatSetFrame(
        request.descriptor.element_descriptor, "", allow_nulls);
    request.right_value = value;
    return dt::ApplySetOperation(request);
  };
  auto wrong_type = present;
  wrong_type.type_id = dt::CanonicalTypeId::character;
  wrong_type.descriptor = character_descriptor;
  wrong_type.encoded_value = "12.34";
  auto missing_membership_descriptor = present;
  missing_membership_descriptor.descriptor = {};
  auto label_membership_descriptor = missing_membership_descriptor;
  label_membership_descriptor.descriptor.stable_name = "decimal_float";
  auto wrong_membership_descriptor = present;
  wrong_membership_descriptor.descriptor = character_descriptor;
  auto dirty_null = TypedNull();
  dirty_null.encoded_value = Bytes(kZero);
  auto malformed_membership = present;
  malformed_membership.encoded_value.pop_back();
  const auto wrong_type_result = membership_result(wrong_type);
  const auto missing_descriptor_result =
      membership_result(missing_membership_descriptor);
  const auto label_descriptor_result =
      membership_result(label_membership_descriptor);
  const auto wrong_descriptor_result =
      membership_result(wrong_membership_descriptor);
  const auto dirty_null_result = membership_result(dirty_null, true, true);
  const auto null_disallowed_result = membership_result(TypedNull(), true);
  const auto malformed_membership_result =
      membership_result(malformed_membership);
  Check(RejectedAs(wrong_type_result,
                   "SB_DATATYPE_SET_OPERATION_REJECTED",
                   "set_membership_type_mismatch") &&
            RejectedAs(missing_descriptor_result,
                       "DATATYPE.DESCRIPTOR.INVALID",
                       "set_membership_descriptor_mismatch") &&
            RejectedAs(label_descriptor_result,
                       "DATATYPE.DESCRIPTOR.INVALID",
                       "set_membership_descriptor_mismatch") &&
            RejectedAs(wrong_descriptor_result,
                       "DATATYPE.DESCRIPTOR.INVALID",
                       "set_membership_descriptor_mismatch") &&
            RejectedAs(dirty_null_result, "DATATYPE.NULL_STATE.INVALID",
                       "set_membership_null_state_invalid") &&
            RejectedAs(null_disallowed_result,
                       "DATATYPE.NULL_NOT_ADMITTED",
                       "set_membership_null_forbidden") &&
            RejectedAs(malformed_membership_result,
                       "NUMERIC.ENCODING.NONCANONICAL",
                       "set_membership_value_invalid"),
        "decimal_float set membership type descriptor NULL and carrier precedence is exact");

  auto nullable_set_descriptor = set_descriptor;
  nullable_set_descriptor.element_descriptor = TypedNull().descriptor;
  nullable_set_descriptor.allow_null_elements = true;
  const auto null_only =
      dt::EncodeSetValue(nullable_set_descriptor, {TypedNull()});
  nullable_set_descriptor.allow_duplicates = true;
  const auto duplicate_nulls = dt::EncodeSetValue(
      nullable_set_descriptor, {TypedNull(), TypedNull()});
  Check(RejectedAs(null_only, "SB_DATATYPE_SET_OPERATION_REJECTED",
                   "decimal_float_set_semantics_policy_unresolved") &&
            RejectedAs(duplicate_nulls,
                       "SB_DATATYPE_SET_OPERATION_REJECTED",
                       "decimal_float_set_semantics_policy_unresolved") &&
            null_only.encoded_set.empty() && duplicate_nulls.encoded_set.empty(),
        "typed-NULL-only and duplicate decimal_float sets refuse before grouping or publication");
}

std::uint32_t OraclePhysicalChecksum(
    dt::DatatypePhysicalValueState state,
    const std::vector<platform::byte>& payload) {
  std::uint32_t value = 2166136261u;
  const auto mix = [&value](std::uint32_t next) {
    value ^= next;
    value *= 16777619u;
  };
  mix(static_cast<std::uint32_t>(dt::CanonicalTypeId::decimal_float));
  mix(static_cast<std::uint32_t>(state));
  for (const auto byte : payload) mix(byte);
  return value;
}

std::vector<platform::byte> OraclePhysicalFrame(
    dt::DatatypePhysicalValueState state,
    const std::vector<platform::byte>& payload) {
  std::vector<platform::byte> frame(24 + payload.size(), 0);
  const std::array<platform::byte, 8> magic{
      {'S','B','D','P','V','0','0','1'}};
  std::copy(magic.begin(), magic.end(), frame.begin());
  platform::StoreLittle32(frame.data() + 8,
      static_cast<std::uint32_t>(dt::CanonicalTypeId::decimal_float));
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

void Persistence() {
  const std::array carriers{kZero, kOnePointZeroZero, kMaximum};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto& carrier : carriers) {
    const auto payload = Payload(Bytes(carrier));
    const auto production = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::decimal_float,
         dt::DatatypePhysicalValueState::value, payload});
    const auto oracle = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    Check(production.ok() && production.bytes == oracle,
          "decimal_float physical frame matches an independent structural oracle");
    frames.push_back(oracle);
  }
  const auto production_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::decimal_float,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto oracle_null = OraclePhysicalFrame(
      dt::DatatypePhysicalValueState::sql_null, {});
  Check(production_null.ok() && production_null.bytes == oracle_null,
        "decimal_float NULL physical frame matches the independent oracle");
  frames.push_back(oracle_null);

  constexpr std::size_t header_bytes = 144;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte, 8> magic{
      {'S','B','D','E','C','0','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(kDescriptorUuid.bytes.begin(), kDescriptorUuid.bytes.end(),
            expected.begin() + 8);
  std::copy(kTypeUuid.bytes.begin(), kTypeUuid.bytes.end(),
            expected.begin() + 24);
  platform::StoreLittle32(expected.data() + 40, 1);
  platform::StoreLittle32(expected.data() + 44, 1);
  platform::StoreLittle32(expected.data() + 48, 1);
  platform::StoreLittle32(expected.data() + 52,
                          static_cast<std::uint32_t>(kCodecId.size()));
  platform::StoreLittle32(expected.data() + 56,
                          static_cast<std::uint32_t>(frames.size()));
  std::copy(kCodecId.begin(), kCodecId.end(), expected.begin() + 60);
  std::uint32_t offset = header_bytes;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    platform::StoreLittle32(expected.data() + 108 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 112 + index * 8,
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
      ("sb-base-decimal-float-" + std::to_string(pid) + ".carrier");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create decimal_float carrier persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write sync and close exact decimal_float carrier bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen decimal_float carrier fixture independently");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected &&
      std::equal(actual.begin() + 8, actual.begin() + 24,
                 kDescriptorUuid.bytes.begin()) &&
      std::equal(actual.begin() + 24, actual.begin() + 40,
                 kTypeUuid.bytes.begin());
  for (std::size_t index = 0; index < frames.size() && decoded_all; ++index) {
    const auto frame_offset =
        platform::LoadLittle32(actual.data() + 108 + index * 8);
    const auto frame_size =
        platform::LoadLittle32(actual.data() + 112 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::decimal_float &&
        (index + 1 == frames.size()
             ? decoded.value.state ==
                       dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state ==
                       dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Bytes(carriers[index])));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "close and reopen preserves decimal_float identity and exact LE16 frames");

  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size(),
        "decimal_float component boundary refuses a short read");
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only decimal_float persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened decimal_float fixture");

  disk::FileDevice corrupt_writer;
  Check(corrupt_writer.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
        "reopen decimal_float fixture for persisted corruption");
  const platform::byte corrupt_checksum =
      static_cast<platform::byte>(expected[header_bytes + 20] ^ 1u);
  const auto corrupt_write = corrupt_writer.WriteAt(
      header_bytes + 20, &corrupt_checksum, sizeof(corrupt_checksum));
  Check(corrupt_write.ok() && corrupt_write.bytes_transferred == 1 &&
            corrupt_writer.Sync().ok() && corrupt_writer.Close().ok(),
        "persist and close a corrupt decimal_float physical frame");

  disk::FileDevice corrupt_reader;
  Check(corrupt_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen corrupt decimal_float fixture independently");
  std::vector<platform::byte> corrupt_actual(expected.size());
  const auto corrupt_read = corrupt_reader.ReadAt(
      0, corrupt_actual.data(), corrupt_actual.size());
  const auto corrupt_decoded = corrupt_read.ok()
      ? dt::DecodeDatatypePhysicalValue(
            corrupt_actual.data() + header_bytes, frames.front().size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(corrupt_read.ok() && !corrupt_decoded.ok() &&
            corrupt_reader.Close().ok(),
        "FileDevice reopen exposes decimal_float physical corruption");

  disk::FileDevice truncator;
  Check(truncator.Open(path.string(),
                       disk::FileOpenMode::create_or_truncate).ok(),
        "open decimal_float fixture through truncate mode");
  const auto truncated_write = truncator.WriteAt(
      0, expected.data(), expected.size() - 1);
  Check(truncated_write.ok() &&
            truncated_write.bytes_transferred == expected.size() - 1 &&
            truncator.Sync().ok() && truncator.Close().ok(),
        "persist and close one-byte-truncated decimal_float fixture");

  disk::FileDevice truncated_reader;
  Check(truncated_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen truncated decimal_float fixture independently");
  std::vector<platform::byte> truncated_actual(expected.size());
  const auto truncated_read = truncated_reader.ReadAt(
      0, truncated_actual.data(), truncated_actual.size());
  Check(!truncated_read.ok() &&
            truncated_read.bytes_transferred < truncated_actual.size() &&
            truncated_reader.Close().ok(),
        "FileDevice refuses full read from truncated decimal_float fixture");

  for (const auto& frame : frames) {
    auto corrupt = frame;
    corrupt[20] ^= 1u;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(
                   frame.data(), frame.size() - 1).ok(),
          "decimal_float physical decoder rejects corruption and truncation");
  }
}

}  // namespace

int main() {
  ExactIdentityAndHardCodedVectors();
  MalformedCarrierAndLowerBinary();
  NullAndDescriptorPrecedence();
  PresentSemanticSurfacesRefuse();
  Persistence();
  std::cout << "base decimal_float canonical value checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
