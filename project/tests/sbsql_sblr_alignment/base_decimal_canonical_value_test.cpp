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

scratchbird::engine::ExecutionTypeDescriptor DecimalDescriptor() {
  return DescriptorFor(dt::CanonicalTypeId::decimal);
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

std::string DecimalSetFrame(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    std::string_view items, bool allow_nulls = false,
    bool allow_duplicates = false) {
  return "SBSET2;element=decimal;descriptor=" +
      SetDescriptorFingerprint(descriptor) +
      ";ordered=0;nulls=" + (allow_nulls ? "1" : "0") +
      ";duplicates=" + (allow_duplicates ? "1" : "0") +
      ";items=" + std::string(items);
}

std::string DecimalValueFrame(std::string_view payload) {
  return "SBDV1;type=decimal;state=value;payload=" + LowerHex(payload);
}

std::string Bytes(const std::array<std::uint8_t, 24>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

constexpr std::array<std::uint8_t, 24> kZero{{
    0x00,0x01,0x01,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00}};
constexpr std::array<std::uint8_t, 24> kOne{{
    0x00,0x01,0x01,0x00, 0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00}};
constexpr std::array<std::uint8_t, 24> kNegativeOne{{
    0x80,0x01,0x01,0x00, 0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00}};
constexpr std::array<std::uint8_t, 24> kTwelvePointThirtyFour{{
    0x02,0x04,0x01,0x00, 0xd2,0x04,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00}};
constexpr std::array<std::uint8_t, 24> kNegativeTwelvePointThirtyFour{{
    0x82,0x04,0x01,0x00, 0xd2,0x04,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00}};
constexpr std::array<std::uint8_t, 24> kMaximum{{
    0x00,0x26,0x05,0x00, 0xff,0xc9,0x9a,0x3b,
    0xff,0xc9,0x9a,0x3b, 0xff,0xc9,0x9a,0x3b,
    0xff,0xc9,0x9a,0x3b, 0x63,0x00,0x00,0x00}};
constexpr std::array<std::uint8_t, 24> kScale38Quantum{{
    0x26,0x26,0x01,0x00, 0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00}};

constexpr platform::Uuid kDescriptorUuid{{
    0xa0,0,0,0,0x64,0x65,0x73,0x69,0xad,0x61,0x6c,0,0,0,0,0}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x13}};
constexpr std::string_view kCodecId = "datatype.decimal.base1e9.le.v1";

bool NilUuid(const platform::Uuid& uuid) {
  return std::all_of(std::begin(uuid.bytes), std::end(uuid.bytes),
                     [](std::uint8_t value) { return value == 0; });
}

dt::DatatypeOperationValue Present(
    const std::array<std::uint8_t, 24>& bytes = kTwelvePointThirtyFour) {
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::decimal, Bytes(bytes), false};
  value.descriptor = DecimalDescriptor();
  return value;
}

dt::DatatypeOperationValue TypedNull() {
  auto descriptor = DecimalDescriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::decimal, {}, true};
  value.descriptor = descriptor;
  return value;
}

void ExactIdentityAndHardCodedVectors() {
  const auto descriptor = DecimalDescriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::decimal),
        "decimal has the exact UUID-bound catalog identity without treating default precision and scale as identity");

  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto row = manifest.ok()
      ? dt::LookupDatatypeCatalogRow(manifest.manifest,
                                     dt::CanonicalTypeId::decimal)
      : dt::DatatypeCatalogManifestResult{};
  auto descriptor_with = [&](std::uint32_t precision, std::uint32_t scale) {
    dt::CatalogExecutionTypeMetadata metadata;
    if (row.ok() && row.manifest.descriptor_rows.size() == 1) {
      metadata.descriptor_uuid =
          row.manifest.descriptor_rows.front().descriptor_uuid;
      metadata.descriptor_epoch =
          row.manifest.descriptor_rows.front().descriptor_epoch;
    }
    metadata.precision = precision;
    metadata.scale = scale;
    return dt::LookupExecutionTypeDescriptorFromCatalog(
        dt::CanonicalTypeId::decimal, metadata);
  };
  const auto p4s2 = descriptor_with(4, 2);
  Check(p4s2.ok() && p4s2.descriptor.precision == 4 &&
            p4s2.descriptor.scale == 2 &&
            descriptor_with(39, 0).ok() &&
            descriptor_with(76, 76).ok() &&
            !descriptor_with(77, 0).ok() &&
            !descriptor_with(38, 39).ok() &&
            !descriptor_with(4, 5).ok(),
        "descriptor metadata permits the two exact codecs and rejects p>76 or s>p without inferring an operation policy");

  for (const auto cohort : std::array{
           std::tuple{dt::kDatatypeCohortV1, 1ULL, 1ULL},
           std::tuple{dt::kDatatypeCohortV2, 2ULL, 2ULL},
           std::tuple{dt::kDatatypeCohortV3, 3ULL, 3ULL},
           std::tuple{dt::kDatatypeCohortV4, 4ULL, 4ULL},
           std::tuple{dt::kDatatypeCohortV5, 5ULL, 5ULL}}) {
    const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
        std::get<0>(cohort), std::get<1>(cohort), std::get<2>(cohort),
        kDescriptorUuid, 1);
    Check(admitted.ok && admitted.row.descriptor_uuid == kDescriptorUuid &&
              admitted.row.type_uuid == kTypeUuid &&
              admitted.row.descriptor_generation == 1 &&
              admitted.row.type_generation == 1 &&
              admitted.row.codec_id == kCodecId &&
              admitted.row.codec_version == 1 &&
              admitted.row.codec_generation == 1 &&
              admitted.row.canonical_value_bytes == 24 &&
              admitted.row.canonical_value_minimum_bytes == 24 &&
              admitted.row.canonical_value_maximum_bytes == 24 &&
              admitted.row.canonical_value_exact_bytes == 24 &&
              !admitted.row.null_supported && NilUuid(admitted.row.codec_uuid),
          "decimal exact identity is native in V1 and inherited unchanged through V5");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV1, 2, 1, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 4, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 2).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, kTypeUuid, 1).ok,
        "decimal crossed cohort generations and identities refuse");

  dt::DatatypeStorageIdentityV1 storage;
  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 1, &storage) &&
            storage.descriptor_uuid == kDescriptorUuid &&
            storage.type_uuid == kTypeUuid &&
            storage.type_id == dt::CanonicalTypeId::decimal &&
            storage.codec.has_value() &&
            storage.codec->codec_id == kCodecId,
        "decimal storage identity retains the exact V1 tuple in V5");

  struct Vector {
    const char* lexical;
    std::array<std::uint8_t, 24> bytes;
  };
  for (const auto& vector : std::array{
           Vector{"0", kZero},
           Vector{"1", kOne},
           Vector{"-1", kNegativeOne},
           Vector{"12.34", kTwelvePointThirtyFour},
           Vector{"-12.34", kNegativeTwelvePointThirtyFour},
           Vector{"99999999999999999999999999999999999999", kMaximum},
           Vector{"0.00000000000000000000000000000000000001",
                  kScale38Quantum}}) {
    const auto encoded = numeric::EncodeExactDecimalLittleEndian(vector.lexical);
    const auto decoded = numeric::DecodeExactDecimalLittleEndian(
        vector.bytes.data(), vector.bytes.size());
    Check(encoded.ok && encoded.canonical_bytes == vector.bytes &&
              decoded.ok && decoded.canonical_bytes == vector.bytes &&
              decoded.canonical_lexical == vector.lexical,
          std::string("independent hard-coded LE24 vector: ") + vector.lexical);
  }
}

void MalformedCarrierAndLowerBinary() {
  for (const auto width : {0u, 1u, 16u, 23u, 25u, 32u}) {
    std::vector<std::uint8_t> malformed(width, 0x5a);
    const auto decoded = numeric::DecodeExactDecimalLittleEndian(
        malformed.data(), malformed.size());
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::decimal, false, false,
         std::vector<platform::byte>(malformed.begin(), malformed.end())});
    Check(!decoded.ok && !binary.ok() && binary.encoded.empty(),
          "wrong-width decimal carrier refuses without publishing bytes");
  }

  std::vector<std::array<std::uint8_t, 24>> malformed;
  auto reserved = kOne; reserved[3] = 1; malformed.push_back(reserved);
  auto scale = kOne; scale[0] = 39; malformed.push_back(scale);
  auto precision_zero = kOne; precision_zero[1] = 0; malformed.push_back(precision_zero);
  auto precision_large = kOne; precision_large[1] = 39; malformed.push_back(precision_large);
  auto groups_zero = kOne; groups_zero[2] = 0; malformed.push_back(groups_zero);
  auto groups_large = kOne; groups_large[2] = 6; malformed.push_back(groups_large);
  auto negative_zero = kZero; negative_zero[0] = 0x80; malformed.push_back(negative_zero);
  auto scaled_zero = kZero; scaled_zero[0] = 1; scaled_zero[1] = 1; malformed.push_back(scaled_zero);
  auto unused = kOne; unused[8] = 1; malformed.push_back(unused);
  auto nonminimal = kOne; nonminimal[2] = 2; malformed.push_back(nonminimal);
  auto group_range = kOne;
  const std::uint32_t billion = 1'000'000'000u;
  for (unsigned i = 0; i < 4; ++i)
    group_range[4 + i] = static_cast<std::uint8_t>(billion >> (8 * i));
  malformed.push_back(group_range);

  for (const auto& bytes : malformed) {
    const auto decoded = numeric::DecodeExactDecimalLittleEndian(
        bytes.data(), bytes.size());
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::decimal, false, false, Payload(Bytes(bytes))});
    Check(!decoded.ok && !binary.ok() && binary.encoded.empty(),
          "noncanonical LE24 decimal frame refuses atomically");
  }

  for (const auto& bytes : {kZero, kOne, kNegativeOne,
                            kTwelvePointThirtyFour, kMaximum,
                            kScale38Quantum}) {
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::decimal, false, false, Payload(Bytes(bytes))});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::decimal,
         dt::DatatypePhysicalValueState::value, Payload(Bytes(bytes))});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(binary.ok() && binary_back.ok() &&
              binary_back.value.type_id == dt::CanonicalTypeId::decimal &&
              binary_back.value.payload == Payload(Bytes(bytes)) &&
              physical.ok() && physical_back.ok() &&
              physical_back.value.type_id == dt::CanonicalTypeId::decimal &&
              physical_back.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              physical_back.value.payload == Payload(Bytes(bytes)),
          "lower binary and physical codecs preserve canonical decimal LE24");
  }

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::decimal, true, false, {}});
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::decimal,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(binary_null.ok() && physical_null.ok() &&
            !dt::EncodeDatatypeBinaryValue(
                 {dt::CanonicalTypeId::decimal, true, false, {0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
                 {dt::CanonicalTypeId::decimal,
                  dt::DatatypePhysicalValueState::sql_null, {0}}).ok(),
        "lower codecs preserve external clean NULL and reject dirty NULL");
}

void NullAndDescriptorPrecedence() {
  auto descriptor = DecimalDescriptor();
  descriptor.nullable_allowed = true;
  const auto typed_null = TypedNull();

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest typed;
    typed.value = typed_null;
    typed.target_type_id = dt::CanonicalTypeId::decimal;
    typed.target_descriptor = descriptor;
    typed.context = context;
    typed.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto typed_result = dt::CastDatatypeValue(typed);

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::decimal;
    contextual.target_descriptor = descriptor;
    contextual.context = context;
    contextual.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto contextual_result = dt::CastDatatypeValue(contextual);
    Check(typed_result.ok() && typed_result.value.is_null &&
              typed_result.value.type_id == dt::CanonicalTypeId::decimal &&
              typed_result.value.encoded_value.empty() &&
              contextual_result.ok() && contextual_result.value.is_null &&
              contextual_result.value.type_id == dt::CanonicalTypeId::decimal &&
              contextual_result.value.encoded_value.empty(),
          "same-descriptor typed NULL and contextual NULL bind in every context");
  }

  auto alias_source = typed_null;
  alias_source.descriptor.stable_name = "exact-number-alias";
  auto alias_target = descriptor;
  alias_target.stable_name = "numeric-display-label";
  dt::DatatypeCastRequest alias;
  alias.value = alias_source;
  alias.target_type_id = dt::CanonicalTypeId::decimal;
  alias.target_descriptor = alias_target;
  const auto alias_result = dt::CastDatatypeValue(alias);
  Check(alias_result.ok() && alias_result.value.is_null &&
            alias_result.value.descriptor.stable_name ==
                alias_source.descriptor.stable_name,
        "descriptor display aliases do not determine decimal typed-NULL identity");

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
  missing.descriptor.stable_name = "decimal";
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
        "mismatched missing label-only dirty and nonnullable decimal NULLs refuse");

  dt::DatatypeNumericOperationRequest numeric;
  numeric.type_id = dt::CanonicalTypeId::decimal;
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
        "decimal NULL result descriptor failures precede unresolved policy");

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
                     "decimal_comparison_policy_unresolved") &&
              RejectedAs(null_present, "SB_DATATYPE_COMPARISON_REJECTED",
                         "decimal_comparison_policy_unresolved") &&
              RejectedAs(null_null, "SB_DATATYPE_COMPARISON_REJECTED",
                         "decimal_comparison_policy_unresolved") &&
              present_null.comparison == 0 && null_present.comparison == 0 &&
              null_null.comparison == 0,
          "decimal comparison with NULL refuses under both NULL orderings");
  }

  auto mismatched_present = nullable_present;
  mismatched_present.descriptor = mismatched;
  const auto mismatch_left =
      dt::CompareDatatypeValues({mismatched_present, nullable_present});
  const auto mismatch_right =
      dt::CompareDatatypeValues({nullable_present, mismatched_present});
  dt::DatatypeNumericOperationRequest mismatched_numeric;
  mismatched_numeric.type_id = dt::CanonicalTypeId::decimal;
  mismatched_numeric.operation = dt::DatatypeNumericOperationKind::add;
  mismatched_numeric.left = nullable_present;
  mismatched_numeric.right = mismatched_present;
  mismatched_numeric.result_descriptor = nullable_present.descriptor;
  const auto mismatch_numeric_result =
      dt::ApplyNumericOperation(mismatched_numeric);
  Check(RejectedAs(mismatch_left, "DATATYPE.DESCRIPTOR.INVALID",
                   "decimal_operand_descriptor_invalid_or_mismatch") &&
            RejectedAs(mismatch_right, "DATATYPE.DESCRIPTOR.INVALID",
                       "decimal_operand_descriptor_invalid_or_mismatch") &&
            RejectedAs(mismatch_numeric_result,
                       "DATATYPE.DESCRIPTOR.INVALID",
                       "decimal_operand_descriptor_mismatch") &&
            mismatch_left.comparison == 0 && mismatch_right.comparison == 0 &&
            mismatch_numeric_result.value.encoded_value.empty(),
        "valid but unequal decimal descriptors fail before comparison and numeric policy");

}

void PresentSemanticSurfacesRefuse() {
  const auto present = Present();
  const auto original = present.encoded_value;
  auto character_descriptor = DescriptorFor(dt::CanonicalTypeId::character);

  for (const auto& candidate : dt::BuiltinDatatypeDescriptors()) {
    Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::decimal,
                                   candidate.type_id) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::decimal) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::decimal,
                                       candidate.type_id, true) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::decimal, true) ==
                  dt::DatatypeCastCategory::forbidden,
          "every registered PRESENT cast classifier pair incident to decimal refuses");
  }

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    for (const bool compatibility : {false, true}) {
      dt::DatatypeCastRequest identity;
      identity.value = present;
      identity.target_type_id = dt::CanonicalTypeId::decimal;
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
      incoming.target_type_id = dt::CanonicalTypeId::decimal;
      incoming.target_descriptor = present.descriptor;
      incoming.context = context;
      incoming.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
      incoming.reference_compatibility_profile = compatibility;
      const auto incoming_result = dt::CastDatatypeValue(incoming);
      Check(RejectedAs(identity_result, "DATATYPE.CAST_FORBIDDEN",
                       "decimal_present_cast_policy_unresolved") &&
                RejectedAs(outgoing_result, "DATATYPE.CAST_FORBIDDEN",
                           "decimal_present_cast_policy_unresolved") &&
                RejectedAs(incoming_result, "DATATYPE.CAST_FORBIDDEN",
                           "decimal_present_cast_policy_unresolved") &&
                identity_result.value.type_id == dt::CanonicalTypeId::unknown &&
                identity_result.value.encoded_value.empty() &&
                outgoing_result.value.type_id == dt::CanonicalTypeId::unknown &&
                outgoing_result.value.encoded_value.empty() &&
                incoming_result.value.type_id == dt::CanonicalTypeId::unknown &&
                incoming_result.value.encoded_value.empty() &&
                present.encoded_value == original,
            "representative PRESENT decimal casts reach exact policy refusal atomically");

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
      null_incoming.target_type_id = dt::CanonicalTypeId::decimal;
      null_incoming.target_descriptor = TypedNull().descriptor;
      null_incoming.context = context;
      null_incoming.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      null_incoming.reference_compatibility_profile = compatibility;
      const auto null_incoming_result =
          dt::CastDatatypeValue(null_incoming);
      Check(RejectedAs(null_outgoing_result, "DATATYPE.CAST_FORBIDDEN",
                       "decimal_cross_type_typed_null_cast_policy_unresolved") &&
                RejectedAs(null_incoming_result, "DATATYPE.CAST_FORBIDDEN",
                           "decimal_cross_type_typed_null_cast_policy_unresolved") &&
                null_outgoing_result.value.type_id ==
                    dt::CanonicalTypeId::unknown &&
                null_outgoing_result.value.encoded_value.empty() &&
                null_incoming_result.value.type_id ==
                    dt::CanonicalTypeId::unknown &&
                null_incoming_result.value.encoded_value.empty(),
            "typed NULL decimal cross casts refuse in every context and compatibility profile");
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
    request.type_id = dt::CanonicalTypeId::decimal;
    request.left = present;
    request.right = present;
    request.result_descriptor = operation ==
            dt::DatatypeNumericOperationKind::compare
        ? DescriptorFor(dt::CanonicalTypeId::boolean)
        : present.descriptor;
    const auto result = dt::ApplyNumericOperation(request);
    Check(RejectedAs(result, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
                     "decimal_numeric_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() && result.comparison == 0 &&
              result.numeric_facts.invalid,
          "every PRESENT decimal numeric surface refuses atomically");
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
                "decimal_comparison_policy_unresolved",
        "alias-only name differences pass descriptor validation and reach decimal comparison refusal");

  auto descriptorless = present;
  descriptorless.descriptor = {};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "decimal";
  auto wrong_descriptor = present;
  wrong_descriptor.descriptor = character_descriptor;
  for (const auto& invalid : {descriptorless, label_only, wrong_descriptor}) {
    dt::DatatypeCastRequest cast;
    cast.value = invalid;
    cast.target_type_id = dt::CanonicalTypeId::decimal;
    cast.target_descriptor = present.descriptor;
    const auto cast_result = dt::CastDatatypeValue(cast);
    dt::DatatypeNumericOperationRequest numeric_request;
    numeric_request.operation = dt::DatatypeNumericOperationKind::canonicalize;
    numeric_request.type_id = dt::CanonicalTypeId::decimal;
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
                  "decimal_left_descriptor_invalid" &&
              compare_left.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(compare_left.diagnostic) ==
                  "decimal_operand_descriptor_invalid_or_mismatch" &&
              compare_right.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(compare_right.diagnostic) ==
                  "decimal_operand_descriptor_invalid_or_mismatch" &&
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
                  "decimal_extract_descriptor_invalid" &&
              key.sort_key.empty() && hash.stable_hash_hex.empty() &&
              display.display_value.empty() &&
              serialized.serialized_value.empty() &&
              extracted.value.encoded_value.empty(),
          "invalid decimal PRESENT descriptors precede every semantic policy");
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
                "decimal_comparison_policy_unresolved" &&
            key.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SORT_KEY_REJECTED" &&
            DiagnosticDetail(key.diagnostic) ==
                "decimal_sort_key_policy_unresolved" &&
            hash.diagnostic.diagnostic_code ==
                "SB_DATATYPE_HASH_REJECTED" &&
            DiagnosticDetail(hash.diagnostic) ==
                "decimal_hash_policy_unresolved" &&
            display.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DISPLAY_RENDER_REJECTED" &&
            DiagnosticDetail(display.diagnostic) ==
                "decimal_display_policy_unresolved" &&
            serialized.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SERIALIZATION_REJECTED" &&
            DiagnosticDetail(serialized.diagnostic) ==
                "decimal_serialization_policy_unresolved" &&
            extracted.diagnostic.diagnostic_code ==
                "SB_DATATYPE_EXTRACT_REJECTED" &&
            DiagnosticDetail(extracted.diagnostic) ==
                "decimal_extract_policy_unresolved",
        "compare key hash display serialization and extract refuse PRESENT decimal");

  auto malformed_header_bytes = kOne;
  malformed_header_bytes[3] = 1;
  auto malformed_header = Present(malformed_header_bytes);
  auto malformed_width = present;
  malformed_width.encoded_value.pop_back();
  for (const auto& malformed : {malformed_header, malformed_width}) {
    dt::DatatypeCastRequest cast;
    cast.value = malformed;
    cast.target_type_id = dt::CanonicalTypeId::decimal;
    cast.target_descriptor = present.descriptor;
    const auto cast_result = dt::CastDatatypeValue(cast);

    dt::DatatypeNumericOperationRequest numeric_left;
    numeric_left.operation = dt::DatatypeNumericOperationKind::canonicalize;
    numeric_left.type_id = dt::CanonicalTypeId::decimal;
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
    malformed_restore.expected_type_id = dt::CanonicalTypeId::decimal;
    malformed_restore.expected_descriptor = present.descriptor;
    malformed_restore.serialized_value =
        DecimalValueFrame(malformed.encoded_value);
    const auto malformed_restored =
        dt::DeserializeDatatypeValue(malformed_restore);

    Check(RejectedAs(cast_result, "NUMERIC.ENCODING.NONCANONICAL",
                     "decimal_source_value_noncanonical") &&
              RejectedAs(numeric_left_result,
                         "NUMERIC.ENCODING.NONCANONICAL",
                         "decimal_numeric_argument_invalid") &&
              RejectedAs(numeric_right_result,
                         "NUMERIC.ENCODING.NONCANONICAL",
                         "decimal_numeric_argument_invalid") &&
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
                         "decimal_extract_value_noncanonical") &&
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
          "malformed decimal carriers fail before every semantic policy and publish no output");
  }

  dt::DatatypeDeserializationRequest restore;
  restore.expected_type_id = dt::CanonicalTypeId::decimal;
  restore.expected_descriptor = present.descriptor;
  restore.serialized_value =
      "SBDV1;type=decimal;state=value;payload=";
  static constexpr char hex[] = "0123456789abcdef";
  for (const auto byte : kTwelvePointThirtyFour) {
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
                "decimal_deserialization_policy_unresolved",
        "generic framing cannot re-admit a PRESENT decimal value");

  dt::DatatypeSetDescriptor set_descriptor;
  set_descriptor.element_type_id = dt::CanonicalTypeId::decimal;
  set_descriptor.element_descriptor = present.descriptor;
  const auto encoded_set = dt::EncodeSetValue(set_descriptor, {present});
  const auto empty_set = dt::EncodeSetValue(set_descriptor, {});
  Check(!encoded_set.ok() && encoded_set.encoded_set.empty() &&
            !empty_set.ok() && empty_set.encoded_set.empty() &&
            encoded_set.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SET_OPERATION_REJECTED" &&
            empty_set.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SET_OPERATION_REJECTED" &&
            DiagnosticDetail(encoded_set.diagnostic) ==
                "decimal_set_semantics_policy_unresolved" &&
            DiagnosticDetail(empty_set.diagnostic) ==
                "decimal_set_semantics_policy_unresolved",
        "PRESENT and empty decimal set construction refuse");

  const auto valid_frame = DecimalSetFrame(
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
                  "decimal_set_semantics_policy_unresolved",
          "every valid decimal set semantic surface reaches policy refusal");
  }

  auto missing_set_descriptor = set_descriptor;
  missing_set_descriptor.element_descriptor = {};
  auto label_set_descriptor = missing_set_descriptor;
  label_set_descriptor.element_descriptor.stable_name = "decimal";
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
          "invalid decimal set descriptors fail before frame and set policy");
  }

  const std::array<std::string, 2> malformed_left_frames{
      "not-a-set-frame",
      DecimalSetFrame(present.descriptor, "V00")};
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
            "malformed decimal left set frame fails before set policy");
    }
  }

  auto mismatched_right_descriptor = present.descriptor;
  mismatched_right_descriptor.precision = 4;
  mismatched_right_descriptor.scale = 2;
  const auto mismatched_right_frame = DecimalSetFrame(
      mismatched_right_descriptor, "V" + LowerHex(present.encoded_value));
  for (const auto operation : {dt::DatatypeSetOperationKind::equals,
                               dt::DatatypeSetOperationKind::subset,
                               dt::DatatypeSetOperationKind::superset}) {
    dt::DatatypeSetOperationRequest malformed_right;
    malformed_right.operation = operation;
    malformed_right.descriptor = set_descriptor;
    malformed_right.left_encoded_set = valid_frame;
    malformed_right.right_encoded_set = "not-a-set-frame";
    const auto malformed_right_result =
        dt::ApplySetOperation(malformed_right);
    malformed_right.right_encoded_set = mismatched_right_frame;
    const auto mismatched_right_result =
        dt::ApplySetOperation(malformed_right);
    Check(RejectedAs(malformed_right_result,
                     "SB_DATATYPE_SET_OPERATION_REJECTED",
                     "right_set_encoding_invalid") &&
              malformed_right_result.value.encoded_value.empty(),
          "malformed right set frame fails before decimal set policy");
    Check(RejectedAs(mismatched_right_result,
                     "DATATYPE.DESCRIPTOR.INVALID",
                     "right_set_descriptor_mismatch") &&
              mismatched_right_result.value.encoded_value.empty(),
          "descriptor-mismatched right set frame fails before decimal set policy");
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
    request.left_encoded_set = DecimalSetFrame(
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
  label_membership_descriptor.descriptor.stable_name = "decimal";
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
        "decimal set membership type descriptor NULL and carrier precedence is exact");

  auto nullable_set_descriptor = set_descriptor;
  nullable_set_descriptor.element_descriptor = TypedNull().descriptor;
  nullable_set_descriptor.allow_null_elements = true;
  const auto null_only =
      dt::EncodeSetValue(nullable_set_descriptor, {TypedNull()});
  nullable_set_descriptor.allow_duplicates = true;
  const auto duplicate_nulls = dt::EncodeSetValue(
      nullable_set_descriptor, {TypedNull(), TypedNull()});
  Check(RejectedAs(null_only, "SB_DATATYPE_SET_OPERATION_REJECTED",
                   "decimal_set_semantics_policy_unresolved") &&
            RejectedAs(duplicate_nulls,
                       "SB_DATATYPE_SET_OPERATION_REJECTED",
                       "decimal_set_semantics_policy_unresolved") &&
            null_only.encoded_set.empty() && duplicate_nulls.encoded_set.empty(),
        "typed-NULL-only and duplicate decimal sets refuse before grouping or publication");
}

std::uint32_t OraclePhysicalChecksum(
    dt::DatatypePhysicalValueState state,
    const std::vector<platform::byte>& payload) {
  std::uint32_t value = 2166136261u;
  const auto mix = [&value](std::uint32_t next) {
    value ^= next;
    value *= 16777619u;
  };
  mix(static_cast<std::uint32_t>(dt::CanonicalTypeId::decimal));
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
      static_cast<std::uint32_t>(dt::CanonicalTypeId::decimal));
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
  const std::array carriers{kZero, kTwelvePointThirtyFour, kMaximum};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto& carrier : carriers) {
    const auto payload = Payload(Bytes(carrier));
    const auto production = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::decimal,
         dt::DatatypePhysicalValueState::value, payload});
    const auto oracle = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    Check(production.ok() && production.bytes == oracle,
          "decimal physical frame matches an independent structural oracle");
    frames.push_back(oracle);
  }
  const auto production_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::decimal,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto oracle_null = OraclePhysicalFrame(
      dt::DatatypePhysicalValueState::sql_null, {});
  Check(production_null.ok() && production_null.bytes == oracle_null,
        "decimal NULL physical frame matches the independent oracle");
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
      ("sb-base-decimal-" + std::to_string(pid) + ".carrier");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create decimal carrier persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write sync and close exact decimal carrier bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen decimal carrier fixture independently");
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
        decoded.value.type_id == dt::CanonicalTypeId::decimal &&
        (index + 1 == frames.size()
             ? decoded.value.state ==
                       dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state ==
                       dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Bytes(carriers[index])));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "close and reopen preserves decimal identity and exact LE24 frames");

  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size(),
        "decimal component boundary refuses a short read");
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only decimal persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened decimal fixture");

  disk::FileDevice corrupt_writer;
  Check(corrupt_writer.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
        "reopen decimal fixture for persisted corruption");
  const platform::byte corrupt_checksum =
      static_cast<platform::byte>(expected[header_bytes + 20] ^ 1u);
  const auto corrupt_write = corrupt_writer.WriteAt(
      header_bytes + 20, &corrupt_checksum, sizeof(corrupt_checksum));
  Check(corrupt_write.ok() && corrupt_write.bytes_transferred == 1 &&
            corrupt_writer.Sync().ok() && corrupt_writer.Close().ok(),
        "persist and close a corrupt decimal physical frame");

  disk::FileDevice corrupt_reader;
  Check(corrupt_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen corrupt decimal fixture independently");
  std::vector<platform::byte> corrupt_actual(expected.size());
  const auto corrupt_read = corrupt_reader.ReadAt(
      0, corrupt_actual.data(), corrupt_actual.size());
  const auto corrupt_decoded = corrupt_read.ok()
      ? dt::DecodeDatatypePhysicalValue(
            corrupt_actual.data() + header_bytes, frames.front().size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(corrupt_read.ok() && !corrupt_decoded.ok() &&
            corrupt_reader.Close().ok(),
        "FileDevice reopen exposes decimal physical corruption");

  disk::FileDevice truncator;
  Check(truncator.Open(path.string(),
                       disk::FileOpenMode::create_or_truncate).ok(),
        "open decimal fixture through truncate mode");
  const auto truncated_write = truncator.WriteAt(
      0, expected.data(), expected.size() - 1);
  Check(truncated_write.ok() &&
            truncated_write.bytes_transferred == expected.size() - 1 &&
            truncator.Sync().ok() && truncator.Close().ok(),
        "persist and close one-byte-truncated decimal fixture");

  disk::FileDevice truncated_reader;
  Check(truncated_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen truncated decimal fixture independently");
  std::vector<platform::byte> truncated_actual(expected.size());
  const auto truncated_read = truncated_reader.ReadAt(
      0, truncated_actual.data(), truncated_actual.size());
  Check(!truncated_read.ok() &&
            truncated_read.bytes_transferred < truncated_actual.size() &&
            truncated_reader.Close().ok(),
        "FileDevice refuses full read from truncated decimal fixture");

  for (const auto& frame : frames) {
    auto corrupt = frame;
    corrupt[20] ^= 1u;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(
                   frame.data(), frame.size() - 1).ok(),
          "decimal physical decoder rejects corruption and truncation");
  }
}

}  // namespace

int main() {
  ExactIdentityAndHardCodedVectors();
  MalformedCarrierAndLowerBinary();
  NullAndDescriptorPrecedence();
  PresentSemanticSurfacesRefuse();
  Persistence();
  std::cout << "base decimal canonical value checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
