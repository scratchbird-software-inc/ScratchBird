// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "../support/generic_cast_boundary_expectations.hpp"
#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
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
      const auto* value = argument.text();
      return value == nullptr ? std::string{} : *value;
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
  const auto result = dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  return result.ok() ? result.descriptor
                     : scratchbird::engine::ExecutionTypeDescriptor{};
}

scratchbird::engine::ExecutionTypeDescriptor Real64Descriptor() {
  return DescriptorFor(dt::CanonicalTypeId::real64);
}

std::vector<platform::byte> Payload(std::string_view bytes) {
  return {bytes.begin(), bytes.end()};
}

std::vector<platform::byte> UuidPayload(const platform::Uuid& uuid) {
  return {uuid.bytes.begin(), uuid.bytes.end()};
}

void AppendSetU16(std::string* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<char>((value >> 8u) & 0xffu));
  bytes->push_back(static_cast<char>(value & 0xffu));
}

void AppendSetU32(std::string* bytes, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
  }
}

void AppendSetU64(std::string* bytes, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
  }
}

void AppendSetUuid(std::string* bytes,
                   const scratchbird::engine::Uuid& uuid) {
  bytes->append(reinterpret_cast<const char*>(uuid.bytes),
                sizeof(uuid.bytes));
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
  for (const auto& domain : descriptor.domain_stack) {
    AppendSetUuid(&bytes, domain);
  }
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

std::string RealSetFrame(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    std::string_view items,
    bool allow_nulls = false,
    bool allow_duplicates = false) {
  return "SBSET2;element=real64;descriptor=" +
      SetDescriptorFingerprint(descriptor) +
      ";ordered=0;nulls=" + (allow_nulls ? "1" : "0") +
      ";duplicates=" + (allow_duplicates ? "1" : "0") +
      ";items=" + std::string(items);
}

std::string Carrier(std::uint64_t raw_bits) {
  std::string encoded;
  if (!dt::EncodeReal64CarrierBitsV1(raw_bits, &encoded)) return {};
  return encoded;
}

dt::DatatypeOperationValue Present(std::uint64_t raw_bits) {
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::real64, Carrier(raw_bits), false};
  value.descriptor = Real64Descriptor();
  return value;
}

dt::DatatypeOperationValue TypedNull() {
  auto descriptor = Real64Descriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::real64, {}, true};
  value.descriptor = descriptor;
  return value;
}

constexpr platform::Uuid kDescriptorUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x31}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x32}};
constexpr platform::Uuid kCodecUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x33}};
constexpr std::string_view kCodecId = "datatype.real64.ieee754.le.v1";

void ExactIdentity() {
  const auto descriptor = Real64Descriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::real64) &&
            descriptor.bit_width == 64,
        "real64 has the exact UUID-bound descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_snapshot_uuid == dt::kDatatypeCohortV5 &&
        row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::real64);
  });
  Check(found != rows.end() && found->descriptor_uuid == kDescriptorUuid &&
            found->type_uuid == kTypeUuid && found->codec_uuid == kCodecUuid &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == kCodecId && found->codec_version == 1 &&
            found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 8 &&
            found->canonical_value_maximum_bytes == 8 &&
            found->canonical_value_exact_bytes == 8 && found->null_supported &&
            found->canonical_byte_order == "little_endian" &&
            found->canonical_representation == "IEEE754_binary64",
        "real64 retains the exact V5 descriptor/type/codec tuple");

  for (const auto cohort : std::array{
           std::tuple{dt::kDatatypeCohortV2, 2ULL, 2ULL},
           std::tuple{dt::kDatatypeCohortV3, 3ULL, 3ULL},
           std::tuple{dt::kDatatypeCohortV4, 4ULL, 4ULL},
           std::tuple{dt::kDatatypeCohortV5, 5ULL, 5ULL}}) {
    const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
        std::get<0>(cohort), std::get<1>(cohort), std::get<2>(cohort),
        kDescriptorUuid, 1);
    Check(admitted.ok && admitted.row.type_uuid == kTypeUuid &&
              admitted.row.codec_uuid == kCodecUuid &&
              admitted.row.codec_id == kCodecId,
          "real64 exact identity is present in V2 and inherited through V5");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV1, 1, 1, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 4, 5, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, kTypeUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 2).ok,
        "real64 is absent from V1 and crossed identities refuse");

  dt::DatatypeStorageIdentityV1 storage;
  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 1, &storage) &&
            storage.descriptor_uuid == kDescriptorUuid &&
            storage.type_uuid == kTypeUuid &&
            storage.type_id == dt::CanonicalTypeId::real64 &&
            storage.codec.has_value() &&
            storage.codec->codec_uuid == kCodecUuid &&
            storage.codec->codec_id == kCodecId,
        "real64 storage identity preserves the exact UUID tuple");
  const auto layout =
      dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::real64);
  Check(layout.ok() &&
            layout.layout.storage_class == dt::DatatypeStorageClass::inline_fixed &&
            layout.layout.encoding ==
                dt::DatatypeBinaryEncoding::ieee754_binary64_little_endian &&
            layout.layout.inline_bytes == 8,
        "real64 layout is an opaque fixed eight-byte structural carrier");
}

void CarrierAndLowerCodecs() {
  // Representative and generated cases establish an invertible uint64/LE8
  // carrier bridge. They deliberately assign no semantic meaning to a pattern.
  std::vector<std::uint64_t> patterns{
      0x0000000000000000ULL, 0x0000000000000001ULL,
      0x00000000000000ffULL, 0x0000000000000100ULL,
      0x7fffffffffffffffULL, 0x8000000000000000ULL,
      0xa55a3cc39669f00fULL, 0xffffffffffffffffULL};
  std::uint64_t generated = 0x6d2b79f5a4c31e27ULL;
  for (std::size_t index = 0; index < 4096; ++index) {
    generated = generated * 6364136223846793005ULL +
        1442695040888963407ULL;
    patterns.push_back(generated);
  }
  bool all_carriers_round_trip = true;
  for (const std::uint64_t bits : patterns) {
    std::string encoded = "sentinel";
    std::uint64_t decoded = 0xa55a3cc39669f00fULL;
    const bool encoded_ok = dt::EncodeReal64CarrierBitsV1(bits, &encoded);
    const bool decoded_ok = encoded_ok &&
        dt::DecodeReal64CarrierBitsV1(encoded, &decoded);
    if (!encoded_ok || !decoded_ok || encoded.size() != 8 ||
        decoded != bits) {
      all_carriers_round_trip = false;
      break;
    }
    for (std::size_t byte = 0; byte < 8; ++byte) {
      if (static_cast<unsigned char>(encoded[byte]) !=
          ((bits >> (byte * 8u)) & 0xffu)) {
        all_carriers_round_trip = false;
        break;
      }
    }
    if (!all_carriers_round_trip) break;
  }
  Check(all_carriers_round_trip,
        "representative and generated opaque real64 uint64/LE8 carriers round trip");

  Check(!dt::EncodeReal64CarrierBitsV1(0, nullptr),
        "real64 carrier encoder rejects a null output");
  for (const auto width : {0u, 1u, 2u, 4u, 7u, 9u, 16u}) {
    const std::string malformed(width, static_cast<char>(0x5a));
    std::uint64_t decoded = 0xa55a3cc39669f00fULL;
    const auto before = decoded;
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::real64, false, false, Payload(malformed)});
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::real64,
         dt::DatatypePhysicalValueState::value, Payload(malformed)});
    Check(!dt::DecodeReal64CarrierBitsV1(malformed, &decoded) &&
              decoded == before && !binary.ok() && binary.encoded.empty() &&
              !physical.ok() && physical.bytes.empty(),
          "malformed real64 carrier widths refuse atomically");
  }
  const auto valid = Carrier(0xa55a3cc39669f00fULL);
  Check(!dt::DecodeReal64CarrierBitsV1(valid, nullptr),
        "real64 carrier decoder rejects a null output");

  for (const auto bits : {std::uint64_t{0x0000000000000000ULL},
                          std::uint64_t{0x0000000000000001ULL},
                          std::uint64_t{0x00000000000000ffULL},
                          std::uint64_t{0x8000000000000000ULL},
                          std::uint64_t{0xffffffffffffffffULL}}) {
    const auto bytes = Carrier(bits);
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::real64, false, false, Payload(bytes)});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::real64,
         dt::DatatypePhysicalValueState::value, Payload(bytes)});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(binary.ok() && binary_back.ok() &&
              binary_back.value.type_id == dt::CanonicalTypeId::real64 &&
              !binary_back.value.is_null &&
              binary_back.value.payload == Payload(bytes) && physical.ok() &&
              physical_back.ok() &&
              physical_back.value.type_id == dt::CanonicalTypeId::real64 &&
              physical_back.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              physical_back.value.payload == Payload(bytes),
          "binary and physical codecs preserve opaque real64 LE8 bytes");
  }

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::real64, true, false, {}});
  const auto binary_null_back = binary_null.ok()
      ? dt::DecodeDatatypeBinaryValue(binary_null.encoded)
      : dt::DatatypeBinaryResult{};
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::real64,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto physical_null_back = physical_null.ok()
      ? dt::DecodeDatatypePhysicalValue(physical_null.bytes.data(),
                                        physical_null.bytes.size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(binary_null.ok() && binary_null_back.ok() &&
            binary_null_back.value.is_null &&
            binary_null_back.value.payload.empty() && physical_null.ok() &&
            physical_null_back.ok() &&
            physical_null_back.value.state ==
                dt::DatatypePhysicalValueState::sql_null &&
            physical_null_back.value.payload.empty(),
        "lower codecs preserve real64 SQL NULL as state with zero bytes");
  Check(!dt::EncodeDatatypeBinaryValue(
             {dt::CanonicalTypeId::real64, true, false, {0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
             {dt::CanonicalTypeId::real64,
              dt::DatatypePhysicalValueState::sql_null, {0}}).ok(),
        "lower codecs refuse payload bytes in real64 NULL state");
}

void DescriptorEnvelope() {
  dt::DatatypeDescriptorEnvelope envelope;
  envelope.kind = dt::DatatypeDescriptorEnvelopeKind::datatype_transport;
  envelope.integrity_profile =
      dt::DatatypeDescriptorIntegrityProfile::strong;
  envelope.records = {
      {"descriptor_uuid", UuidPayload(kDescriptorUuid)},
      {"type_uuid", UuidPayload(kTypeUuid)},
      {"codec_uuid", UuidPayload(kCodecUuid)},
      {"codec_id", Payload(kCodecId)},
  };
  const auto encoded = dt::EncodeDatatypeDescriptorEnvelope(envelope);
  const auto decoded = encoded.ok()
      ? dt::DecodeDatatypeDescriptorEnvelope(encoded.encoded)
      : dt::DatatypeDescriptorEnvelopeResult{};
  Check(encoded.ok() && decoded.ok() &&
            decoded.envelope.kind == envelope.kind &&
            decoded.envelope.records.size() == envelope.records.size() &&
            decoded.envelope.records[0].payload == UuidPayload(kDescriptorUuid) &&
            decoded.envelope.records[1].payload == UuidPayload(kTypeUuid) &&
            decoded.envelope.records[2].payload == UuidPayload(kCodecUuid) &&
            decoded.envelope.records[3].payload == Payload(kCodecId),
        "descriptor envelope preserves exact real64 identity fields");
  if (encoded.ok() && !encoded.encoded.empty()) {
    auto corrupt = encoded.encoded;
    corrupt.back() ^= 1u;
    const auto corrupt_result = dt::DecodeDatatypeDescriptorEnvelope(corrupt);
    auto truncated = encoded.encoded;
    truncated.pop_back();
    const auto truncated_result =
        dt::DecodeDatatypeDescriptorEnvelope(truncated);
    Check(!corrupt_result.ok() && corrupt_result.encoded.empty() &&
              corrupt_result.envelope.records.empty() &&
              !truncated_result.ok() && truncated_result.encoded.empty() &&
              truncated_result.envelope.records.empty(),
          "corrupt or truncated real64 descriptor envelopes refuse atomically");
  }
}

void NullStateAndDescriptors() {
  auto descriptor = Real64Descriptor();
  descriptor.nullable_allowed = true;
  auto typed_null = TypedNull();

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = typed_null;
    identity.target_type_id = dt::CanonicalTypeId::real64;
    identity.target_descriptor = descriptor;
    identity.context = context;
    identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto bound = dt::CastDatatypeValue(identity);
    Check(bound.ok() &&
              bound.category == dt::DatatypeCastCategory::identity &&
              bound.value.type_id == dt::CanonicalTypeId::real64 &&
              bound.value.is_null && bound.value.encoded_value.empty() &&
              SameUuidBytes(bound.value.descriptor.descriptor_uuid,
                            descriptor.descriptor_uuid),
          "same-descriptor typed real64 NULL identity is admitted");

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::real64;
    contextual.target_descriptor = descriptor;
    contextual.context = context;
    contextual.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    const auto contextual_result = dt::CastDatatypeValue(contextual);
    Check(contextual_result.ok() && contextual_result.value.is_null &&
              contextual_result.value.type_id ==
                  dt::CanonicalTypeId::real64 &&
              contextual_result.value.encoded_value.empty() &&
              SameUuidBytes(contextual_result.value.descriptor.descriptor_uuid,
                            descriptor.descriptor_uuid),
          "contextual NULL binds the exact real64 descriptor");
  }

  auto alias_source = typed_null;
  alias_source.descriptor.stable_name = "opaque-carrier";
  auto alias_target = descriptor;
  alias_target.stable_name = "transport-bits-32";
  dt::DatatypeCastRequest alias_identity;
  alias_identity.value = alias_source;
  alias_identity.target_type_id = dt::CanonicalTypeId::real64;
  alias_identity.target_descriptor = alias_target;
  const auto alias_result = dt::CastDatatypeValue(alias_identity);
  Check(alias_result.ok() && alias_result.value.is_null &&
            alias_result.value.descriptor.stable_name ==
                alias_source.descriptor.stable_name,
        "real64 typed NULL identity ignores descriptor display aliases");

  auto different_descriptor = descriptor;
  different_descriptor.security_policy_uuid = FixtureV7Uuid(0xc0);
  different_descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  alias_identity.value = typed_null;
  alias_identity.target_descriptor = different_descriptor;
  const auto mismatch = dt::CastDatatypeValue(alias_identity);
  Check(!mismatch.ok() && mismatch.value.type_id == dt::CanonicalTypeId::unknown &&
            mismatch.value.encoded_value.empty() && !mismatch.value.is_null,
        "typed real64 NULL identity refuses descriptor substitution");

  auto descriptorless = typed_null;
  descriptorless.descriptor = {};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "real64";
  alias_identity.target_descriptor = descriptor;
  alias_identity.value = descriptorless;
  const auto missing = dt::CastDatatypeValue(alias_identity);
  alias_identity.value = label_only;
  const auto pseudo = dt::CastDatatypeValue(alias_identity);
  Check(!missing.ok() && missing.value.encoded_value.empty() &&
            !pseudo.ok() && pseudo.value.encoded_value.empty(),
        "missing and label-only real64 NULL descriptors refuse");

  alias_identity.value = typed_null;
  alias_identity.value.encoded_value = Carrier(0);
  const auto dirty_null = dt::CastDatatypeValue(alias_identity);
  alias_identity.value = typed_null;
  alias_identity.target_descriptor = descriptor;
  alias_identity.target_descriptor.nullable_allowed = false;
  const auto nonnullable = dt::CastDatatypeValue(alias_identity);
  Check(!dirty_null.ok() && dirty_null.value.encoded_value.empty() &&
            !nonnullable.ok() && nonnullable.value.encoded_value.empty(),
        "dirty or non-nullable real64 NULL state refuses atomically");

  const auto serialized = dt::SerializeDatatypeValue({typed_null});
  dt::DatatypeDeserializationRequest restore;
  restore.expected_type_id = dt::CanonicalTypeId::real64;
  restore.expected_descriptor = descriptor;
  restore.serialized_value = serialized.serialized_value;
  const auto restored = serialized.ok()
      ? dt::DeserializeDatatypeValue(restore)
      : dt::DatatypeDeserializationResult{};
  Check(serialized.ok() && !serialized.serialized_value.empty() &&
            SameUuidBytes(serialized.descriptor.descriptor_uuid,
                          descriptor.descriptor_uuid) && restored.ok() &&
            restored.value.type_id == dt::CanonicalTypeId::real64 &&
            restored.value.is_null && restored.value.encoded_value.empty() &&
            SameUuidBytes(restored.value.descriptor.descriptor_uuid,
                          descriptor.descriptor_uuid),
        "generic framing round trips real64 typed NULL state and descriptor");
}

void PresentSemanticSurfacesWork() {
  using T=dt::CanonicalTypeId;
  const auto present=Present(0x3ff8000000000000ULL); // 1.5
  const auto one=Present(0x3ff0000000000000ULL);
  const auto zero=Present(0), negative_zero=Present(0x8000000000000000ULL);
  const auto character_descriptor=DescriptorFor(T::character);
  const auto cast=[&](dt::DatatypeOperationValue value,T target,
                      dt::DatatypeCastContext context=dt::DatatypeCastContext::explicit_cast) {
    dt::DatatypeCastRequest request;
    request.value=std::move(value); request.target_type_id=target;
    request.target_descriptor=DescriptorFor(target); request.context=context;
    return dt::CastDatatypeValue(request);
  };
  for(const auto& candidate:dt::BuiltinDatatypeDescriptors()) {
    const auto type=candidate.type_id;
    const bool integer=type==T::int8 || type==T::uint8 || type==T::int16 || type==T::uint16 ||
        type==T::int32 || type==T::uint32 || type==T::int64 || type==T::uint64 ||
        type==T::int128 || type==T::uint128;
    const bool peer=integer || type==T::character || type==T::real64 || type==T::real128;
    for(bool compatibility:{false,true}) {
      Check((dt::ClassifyDatatypeCast(T::real64,type,compatibility)!=dt::DatatypeCastCategory::forbidden)==peer,
            "REAL64 outgoing classifier matrix");
      Check((dt::ClassifyDatatypeCast(type,T::real64,compatibility)!=dt::DatatypeCastCategory::forbidden)==
                (peer || type==T::null_type),"REAL64 incoming classifier matrix");
    }
    if(integer) {
      const auto exact=cast(one,type);
      Check(exact.ok() && !exact.value.encoded_value.empty() && exact.value.encoded_value[0]==1,
            "integral REAL64 casts to every native integer width");
      if(exact.ok()) {
        const auto roundtrip=cast(exact.value,T::real64);
        Check(roundtrip.ok() && roundtrip.value.encoded_value==one.encoded_value,"integer native cast roundtrip");
      }
      const auto fractional=cast(present,type);
      Check(!fractional.ok() && fractional.value.encoded_value.empty() && fractional.numeric_facts.invalid &&
                fractional.numeric_facts.inexact,"fractional conversion refuses with loss facts");
      const auto implicit=cast(one,type,dt::DatatypeCastContext::implicit);
      Check(!implicit.ok(),"REAL64 integer narrowing is not implicit");
      Check(cast(one,type,dt::DatatypeCastContext::assignment).ok(),"exact integer assignment is admitted");
    } else if(!peer && type!=T::null_type) {
      Check(!cast(one,type).ok(),"unrelated type cannot reinterpret REAL64 bytes");
    }
  }
  for(auto context:{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment,
                    dt::DatatypeCastContext::explicit_cast}) {
    auto identity=cast(present,T::real64,context);
    Check(identity.ok() && identity.value.encoded_value==present.encoded_value,"exact identity preserves payload");
  }
  auto text=cast(present,T::character);
  Check(dt::ClassifyDatatypeCast(T::real64,T::character,false)==dt::DatatypeCastCategory::lossy_explicit &&
        dt::ClassifyDatatypeCast(T::character,T::real64,false)==dt::DatatypeCastCategory::lossy_explicit,
        "text conversion classification admits possible rounding and NaN payload loss");
  Check(text.ok() && text.value.encoded_value=="1.5","explicit text rendering");
  auto parsed=cast(text.value,T::real64);
  Check(parsed.ok() && parsed.value.encoded_value==present.encoded_value,"explicit text roundtrip");
  for(const auto* malformed:{"","1x","1 2","NaN","Infinity","1e+","0x"}) {
    dt::DatatypeOperationValue input{T::character,malformed,false,character_descriptor};
    auto failure=cast(input,T::real64);
    Check(!failure.ok() && failure.value.encoded_value.empty() &&
              failure.diagnostic.diagnostic_code=="NUMERIC.REAL64.INVALID","invalid text cast has exact diagnostic");
  }
  auto wide=cast(present,T::real128);
  auto narrow=cast(wide.value,T::real64);
  Check(wide.ok() && wide.value.encoded_value.size()==16 && narrow.ok() &&
            narrow.value.encoded_value==present.encoded_value,"native binary widening and narrowing");
  auto max=Present(0x7fefffffffffffffULL);
  Check(!cast(max,T::int128).ok() && !cast(max,T::uint128).ok(),"finite integer overflow rejected");

  for(const auto [operation,expected]:{
      std::pair{dt::DatatypeNumericOperationKind::canonicalize,0x3ff8000000000000ULL},
      std::pair{dt::DatatypeNumericOperationKind::add,0x4004000000000000ULL},
      std::pair{dt::DatatypeNumericOperationKind::subtract,0x3fe0000000000000ULL},
      std::pair{dt::DatatypeNumericOperationKind::multiply,0x3ff8000000000000ULL},
      std::pair{dt::DatatypeNumericOperationKind::divide,0x3ff8000000000000ULL}}) {
    dt::DatatypeNumericOperationRequest request;
    request.operation=operation;request.type_id=T::real64;
    request.left=present;request.right=one;request.result_descriptor=present.descriptor;
    const auto result=dt::ApplyNumericOperation(request);
    Check(result.ok() && result.value.encoded_value==Carrier(expected) &&
              !result.numeric_facts.inexact,"native arithmetic exact output");
    auto null=TypedNull();request.left=null;request.right=null;request.result_descriptor=null.descriptor;
    const auto null_result=dt::ApplyNumericOperation(request);
    Check(null_result.ok() && null_result.value.is_null && null_result.value.encoded_value.empty(),
          "typed NULL arithmetic does not manufacture a payload");
    request.context.rounding=static_cast<dt::DatatypeRoundingMode>(99);
    Check(!dt::ApplyNumericOperation(request).ok(),"NULL cannot bypass invalid numeric context");
  }
  Check(dt::CompareDatatypeValues({present,one}).comparison>0,"native ordered comparison");
  auto nonnullable=one;nonnullable.descriptor.nullable_allowed=false;
  Check(dt::CompareDatatypeValues({nonnullable,one}).ok() &&
            dt::CompareDatatypeValues({nonnullable,one}).comparison==0,
        "PRESENT numeric comparison accepts different NULL admission flags");
  dt::DatatypeNumericOperationRequest mixed_nullability;
  mixed_nullability.type_id=T::real64;mixed_nullability.operation=dt::DatatypeNumericOperationKind::add;
  mixed_nullability.left=nonnullable;mixed_nullability.right=one;
  mixed_nullability.result_descriptor=nonnullable.descriptor;
  auto mixed_result=dt::ApplyNumericOperation(mixed_nullability);
  Check(mixed_result.ok() && mixed_result.value.encoded_value==Carrier(0x4000000000000000ULL),
        "PRESENT arithmetic does not treat NULL admission as numeric identity");
  mixed_nullability.right=TypedNull();
  Check(!dt::ApplyNumericOperation(mixed_nullability).ok(),"nonnullable result still rejects actual NULL");
  Check(dt::CompareDatatypeValues({zero,negative_zero}).ok() &&
            dt::CompareDatatypeValues({zero,negative_zero}).comparison==0,"signed zero numeric equality");
  auto alias_left=one,alias_right=one;
  alias_left.descriptor.stable_name="display alias";alias_right.descriptor.stable_name="another alias";
  Check(dt::CompareDatatypeValues({alias_left,alias_right}).ok(),"display aliases are not identity");
  auto mismatched=present;
  mismatched.descriptor.security_policy_uuid=FixtureV7Uuid(0xd0);
  mismatched.descriptor.modifier_flags |= scratchbird::engine::ExecutionTypeModifierFlagBit(
      scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  auto descriptorless_present = present;
  descriptorless_present.descriptor = {};
  auto label_only_present = descriptorless_present;
  label_only_present.descriptor.stable_name = "real64";
  dt::DatatypeOperationValue wrong_descriptor_present{
      dt::CanonicalTypeId::real64, present.encoded_value, false,
      character_descriptor};
  for (const auto& invalid : {descriptorless_present, label_only_present,
                              mismatched, wrong_descriptor_present}) {
    dt::DatatypeCastRequest cast;
    cast.value = invalid;
    cast.target_type_id = dt::CanonicalTypeId::real64;
    cast.target_descriptor = present.descriptor;
    const auto cast_result = dt::CastDatatypeValue(cast);

    dt::DatatypeNumericOperationRequest numeric;
    numeric.operation = dt::DatatypeNumericOperationKind::canonicalize;
    numeric.type_id = dt::CanonicalTypeId::real64;
    numeric.left = invalid;
    numeric.result_descriptor = present.descriptor;
    const auto numeric_result = dt::ApplyNumericOperation(numeric);

    const auto compare_left =
        dt::CompareDatatypeValues({invalid, present});
    const auto compare_right =
        dt::CompareDatatypeValues({present, invalid});
    const auto invalid_key = dt::MakeDatatypeSortKey({invalid});
    const auto invalid_hash = dt::HashDatatypeValue({invalid});
    const auto invalid_display =
        dt::RenderDatatypeValueForDisplay({invalid});
    const auto invalid_serialized = dt::SerializeDatatypeValue({invalid});
    Check(!cast_result.ok() &&
              cast_result.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              cast_result.value.encoded_value.empty() &&
              !numeric_result.ok() &&
              numeric_result.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              numeric_result.value.encoded_value.empty() &&
              !compare_left.ok() && compare_left.comparison == 0 &&
              compare_left.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              !compare_right.ok() && compare_right.comparison == 0 &&
              compare_right.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              !invalid_key.ok() && invalid_key.sort_key.empty() &&
              invalid_key.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              !invalid_hash.ok() && invalid_hash.stable_hash_hex.empty() &&
              invalid_hash.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              !invalid_display.ok() && invalid_display.display_value.empty() &&
              invalid_display.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              !invalid_serialized.ok() &&
              invalid_serialized.serialized_value.empty() &&
              invalid_serialized.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID",
          "invalid PRESENT descriptors fail before real64 policy");
  }

  for (const auto& invalid_target_descriptor :
       {scratchbird::engine::ExecutionTypeDescriptor{},
        label_only_present.descriptor, character_descriptor,
        mismatched.descriptor}) {
    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      for (const bool compatibility_profile : {false, true}) {
        dt::DatatypeCastRequest incoming;
        incoming.value = {dt::CanonicalTypeId::character, "1", false};
        incoming.value.descriptor = character_descriptor;
        incoming.target_type_id = dt::CanonicalTypeId::real64;
        incoming.target_descriptor = invalid_target_descriptor;
        incoming.context = context;
        incoming.explicit_cast =
            context == dt::DatatypeCastContext::explicit_cast;
        incoming.reference_compatibility_profile = compatibility_profile;
        const auto result = dt::CastDatatypeValue(incoming);
        Check(!result.ok() &&
                  result.category == dt::DatatypeCastCategory::forbidden &&
                  result.value.type_id == dt::CanonicalTypeId::unknown &&
                  result.value.encoded_value.empty() &&
                  result.diagnostic.diagnostic_code ==
                      "DATATYPE.DESCRIPTOR.INVALID" &&
                  DiagnosticDetail(result.diagnostic) ==
                      "target_descriptor_invalid",
              "invalid real64 target descriptor precedes cast policy");
      }
    }
  }


  Check(dt::MakeDatatypeSortKey({zero}).sort_key==dt::MakeDatatypeSortKey({negative_zero}).sort_key,
        "numeric sort coalesces signed zero");
  Check(dt::HashDatatypeValue({zero}).stable_hash_hex==dt::HashDatatypeValue({negative_zero}).stable_hash_hex,
        "numeric hash coalesces signed zero");
  auto previous=dt::MakeDatatypeSortKey({Present(0xffefffffffffffffULL)});
  for(auto bits:{0xbff0000000000000ULL,0x8000000000000001ULL,0ULL,1ULL,
                 0x3ff0000000000000ULL,0x7fefffffffffffffULL}) {
    auto next=dt::MakeDatatypeSortKey({Present(bits)});
    Check(previous.ok() && next.ok() && previous.sort_key<next.sort_key,"native sort monotonicity");
    previous=std::move(next);
  }
  for(auto bits:{0x7ff0000000000000ULL,0x7ff8000000000001ULL,0xfff0000000000001ULL}) {
    auto special=Present(bits);
    Check(!dt::HashDatatypeValue({special}).ok() && !dt::MakeDatatypeSortKey({special}).ok(),
          "default finite hash and order refuse specials");
    auto identity=cast(special,T::real64);
    Check(!identity.ok(),"default numeric identity requires special admission");
  }
  Check(dt::RenderDatatypeValueForDisplay({present}).display_value=="1.5","display uses explicit decimal boundary");
  Check(dt::RenderDatatypeValueForDisplay({negative_zero}).display_value=="-0","display preserves signed zero");
  Check(dt::RenderDatatypeValueForDisplay({TypedNull()}).display_value=="NULL","display NULL");
  const auto serialized=dt::SerializeDatatypeValue({present});
  dt::DatatypeDeserializationRequest restore;
  restore.expected_type_id=T::real64;restore.expected_descriptor=present.descriptor;
  restore.serialized_value=serialized.serialized_value;
  auto restored=dt::DeserializeDatatypeValue(restore);
  Check(serialized.ok() && restored.ok() && restored.value.encoded_value==present.encoded_value,
        "generic explicit frame roundtrip preserves native payload");
  for(const auto& invalid_descriptor:{scratchbird::engine::ExecutionTypeDescriptor{},
                                     label_only_present.descriptor,character_descriptor,mismatched.descriptor}) {
    auto invalid=restore;invalid.expected_descriptor=invalid_descriptor;
    for(const auto* payload:{"zz","000000000000f83f"}) {
      invalid.serialized_value="SBDV1;type=real64;state=value;payload="+std::string(payload);
      const auto failure=dt::DeserializeDatatypeValue(invalid);
      Check(!failure.ok() && failure.value.encoded_value.empty() &&
                failure.diagnostic.diagnostic_code=="DATATYPE.DESCRIPTOR.INVALID",
            "descriptor rejection precedes malformed frame payload");
    }
  }
  for(const auto* payload:{"zz","","00","00000000","00000000000000","000000000000000000"}) {
    auto invalid=restore;invalid.serialized_value="SBDV1;type=real64;state=value;payload="+std::string(payload);
    auto failure=dt::DeserializeDatatypeValue(invalid);
    Check(!failure.ok() && failure.value.encoded_value.empty(),"malformed width and hex refuse without value");
  }
  dt::DatatypeExtractRequest extract;extract.value=present;extract.field="unsupported";
  Check(!dt::ExtractDatatypeField(extract).ok(),"REAL64 has no arbitrary extract fields");

  dt::DatatypeSetDescriptor descriptor;descriptor.element_type_id=T::real64;descriptor.element_descriptor=present.descriptor;
  const auto set=dt::EncodeSetValue(descriptor,{zero,negative_zero,one,present});
  Check(set.ok() && dt::EncodeSetValue(descriptor,{}).ok(),"real64 set values and empty set admitted");
  for(auto operation:{dt::DatatypeSetOperationKind::equals,dt::DatatypeSetOperationKind::subset,
                      dt::DatatypeSetOperationKind::superset,dt::DatatypeSetOperationKind::membership}) {
    dt::DatatypeSetOperationRequest request;
    request.operation=operation;request.descriptor=descriptor;
    request.left_encoded_set=set.encoded_set;request.right_encoded_set=set.encoded_set;request.right_value=negative_zero;
    auto result=dt::ApplySetOperation(request);
    Check(result.ok() && result.value.encoded_value==std::string(1,'\1'),"native set semantics and zero membership");
    for(const auto* malformed_item:{"V00","V00000000","V00000000000000","V000000000000000000","Vzz",
                                    "V010000000000f87f"}) {
      request.left_encoded_set=RealSetFrame(present.descriptor,malformed_item);
      result=dt::ApplySetOperation(request);
      Check(!result.ok() && result.value.encoded_value.empty(),"malformed/special set item rejected before effects");
    }
    request.left_encoded_set=set.encoded_set;
    if(operation!=dt::DatatypeSetOperationKind::membership) {
      request.right_encoded_set="malformed";
      Check(!dt::ApplySetOperation(request).ok(),"malformed right set rejected");
    }
  }
  for(unsigned width:{0u,1u,2u,4u,7u,9u,16u}) {
    auto malformed=present;malformed.encoded_value.assign(width,'\0');
    Check(!cast(malformed,T::real64).ok() && !dt::CompareDatatypeValues({malformed,one}).ok() &&
              !dt::MakeDatatypeSortKey({malformed}).ok() && !dt::HashDatatypeValue({malformed}).ok() &&
              !dt::SerializeDatatypeValue({malformed}).ok() &&
              !dt::RenderDatatypeValueForDisplay({malformed}).ok(),"all scalar surfaces reject wrong widths");
    Check(!dt::EncodeSetValue(descriptor,{malformed}).ok(),"set encoding rejects wrong native widths");
    dt::DatatypeNumericOperationRequest numeric;
    numeric.type_id=T::real64;numeric.left=malformed;numeric.result_descriptor=present.descriptor;
    const auto bad_numeric=dt::ApplyNumericOperation(numeric);
    Check(!bad_numeric.ok() && bad_numeric.diagnostic.diagnostic_code=="NUMERIC.ENCODING.NONCANONICAL" &&
              bad_numeric.numeric_facts.invalid && bad_numeric.value.encoded_value.empty(),
          "malformed numeric carrier has exact encoding diagnostic and no substitute value");
  }
  auto nullable=descriptor;nullable.allow_null_elements=true;nullable.element_descriptor=TypedNull().descriptor;
  Check(dt::EncodeSetValue(nullable,{TypedNull()}).ok(),"nullable set preserves external NULL");
  Check(!dt::EncodeSetValue(descriptor,{TypedNull()}).ok(),"nonnullable set refuses NULL");
  Check(!dt::EncodeSetValue(descriptor,{Present(0x7ff8000000000001ULL)}).ok(),"set rejects unadmitted NaN");
}

std::uint32_t OraclePhysicalChecksum(
    dt::DatatypePhysicalValueState state,
    const std::vector<platform::byte>& payload) {
  std::uint32_t value = 2166136261u;
  const auto mix = [&value](std::uint32_t next) {
    value ^= next;
    value *= 16777619u;
  };
  mix(static_cast<std::uint32_t>(dt::CanonicalTypeId::real64));
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
      static_cast<std::uint32_t>(dt::CanonicalTypeId::real64));
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
  const std::array<std::uint64_t, 3> patterns{{0x0000000000000000ULL, 0x5aa53cc39669f00fULL, 0xffffffffffffffffULL}};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto bits : patterns) {
    const auto payload = Payload(Carrier(bits));
    const auto production = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::real64,
         dt::DatatypePhysicalValueState::value, payload});
    const auto oracle = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    Check(production.ok() && production.bytes == oracle,
          "real64 physical frame matches an independent structural oracle");
    frames.push_back(oracle);
  }
  const auto production_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::real64,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto oracle_null = OraclePhysicalFrame(
      dt::DatatypePhysicalValueState::sql_null, {});
  Check(production_null.ok() && production_null.bytes == oracle_null,
        "real64 NULL physical frame matches the independent oracle");
  frames.push_back(oracle_null);

  // This test-owned envelope records exact identity and frame offsets. It is
  // evidence scaffolding, not a product page, wire, or semantic format.
  constexpr std::size_t header_bytes = 152;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte, 8> magic{
      {'S','B','R','E','6','4','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(kDescriptorUuid.bytes.begin(), kDescriptorUuid.bytes.end(),
            expected.begin() + 8);
  std::copy(kTypeUuid.bytes.begin(), kTypeUuid.bytes.end(),
            expected.begin() + 24);
  std::copy(kCodecUuid.bytes.begin(), kCodecUuid.bytes.end(),
            expected.begin() + 40);
  platform::StoreLittle32(expected.data() + 56, 1);
  platform::StoreLittle32(expected.data() + 60, 1);
  platform::StoreLittle32(expected.data() + 64, 1);
  platform::StoreLittle32(expected.data() + 68, 1);
  platform::StoreLittle32(expected.data() + 72,
                          static_cast<std::uint32_t>(kCodecId.size()));
  platform::StoreLittle32(expected.data() + 76,
                          static_cast<std::uint32_t>(frames.size()));
  std::copy(kCodecId.begin(), kCodecId.end(), expected.begin() + 80);
  std::uint32_t offset = header_bytes;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    platform::StoreLittle32(expected.data() + 120 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 124 + index * 8,
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
      ("sb-base-real64-" + std::to_string(pid) + ".carrier");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create real64 carrier persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact real64 carrier bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen real64 carrier persistence fixture independently");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected &&
      std::equal(actual.begin() + 8, actual.begin() + 24,
                 kDescriptorUuid.bytes.begin()) &&
      std::equal(actual.begin() + 24, actual.begin() + 40,
                 kTypeUuid.bytes.begin()) &&
      std::equal(actual.begin() + 40, actual.begin() + 56,
                 kCodecUuid.bytes.begin()) &&
      platform::LoadLittle32(actual.data() + 56) == 1 &&
      platform::LoadLittle32(actual.data() + 60) == 1 &&
      platform::LoadLittle32(actual.data() + 64) == 1 &&
      platform::LoadLittle32(actual.data() + 68) == 1 &&
      platform::LoadLittle32(actual.data() + 72) == kCodecId.size() &&
      platform::LoadLittle32(actual.data() + 76) == frames.size() &&
      std::equal(actual.begin() + 80,
                 actual.begin() + 80 + kCodecId.size(), kCodecId.begin());
  for (std::size_t index = 0; index < frames.size() && decoded_all; ++index) {
    const auto frame_offset =
        platform::LoadLittle32(actual.data() + 120 + index * 8);
    const auto frame_size =
        platform::LoadLittle32(actual.data() + 124 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::real64 &&
        (index + 1 == frames.size()
             ? decoded.value.state ==
                       dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state ==
                       dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Carrier(patterns[index])));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "reopen preserves exact real64 identity and opaque carrier frames");

  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size(),
        "real64 persistence boundary refuses a short read");
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only real64 persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened real64 fixture");

  disk::FileDevice corrupt_writer;
  Check(corrupt_writer.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
        "reopen real64 fixture for a persisted corruption check");
  const platform::byte corrupt_checksum =
      static_cast<platform::byte>(expected[header_bytes + 20] ^ 1u);
  const auto corrupt_write = corrupt_writer.WriteAt(
      header_bytes + 20, &corrupt_checksum, sizeof(corrupt_checksum));
  Check(corrupt_write.ok() &&
            corrupt_write.bytes_transferred == sizeof(corrupt_checksum) &&
            corrupt_writer.Sync().ok() && corrupt_writer.Close().ok(),
        "persist and close a corrupt real64 physical frame");

  disk::FileDevice corrupt_reader;
  Check(corrupt_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen the corrupt real64 fixture independently");
  std::vector<platform::byte> corrupt_actual(expected.size());
  const auto corrupt_read = corrupt_reader.ReadAt(
      0, corrupt_actual.data(), corrupt_actual.size());
  const auto corrupt_decoded = corrupt_read.ok()
      ? dt::DecodeDatatypePhysicalValue(
            corrupt_actual.data() + header_bytes, frames.front().size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(corrupt_read.ok() &&
            corrupt_read.bytes_transferred == corrupt_actual.size() &&
            !corrupt_decoded.ok() && corrupt_reader.Close().ok(),
        "FileDevice reopen exposes corruption to the real64 physical decoder");

  disk::FileDevice truncator;
  Check(truncator.Open(
            path.string(), disk::FileOpenMode::create_or_truncate).ok(),
        "open the real64 fixture through the FileDevice truncate mode");
  const auto truncated_write = truncator.WriteAt(
      0, expected.data(), expected.size() - 1);
  Check(truncated_write.ok() &&
            truncated_write.bytes_transferred == expected.size() - 1 &&
            truncator.Sync().ok() && truncator.Close().ok(),
        "persist and close a one-byte-truncated real64 fixture");

  disk::FileDevice truncated_reader;
  Check(truncated_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen the truncated real64 fixture independently");
  std::vector<platform::byte> truncated_actual(expected.size());
  const auto truncated_read = truncated_reader.ReadAt(
      0, truncated_actual.data(), truncated_actual.size());
  Check(!truncated_read.ok() &&
            truncated_read.bytes_transferred < truncated_actual.size() &&
            truncated_reader.Close().ok(),
        "FileDevice refuses a full-length read from the truncated fixture");

  for (const auto& frame : frames) {
    auto corrupt = frame;
    corrupt[20] ^= 1u;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(
                   frame.data(), frame.size() - 1).ok(),
          "real64 physical decoder rejects corruption and truncation");
  }
}

}  // namespace

int main() {
  ExactIdentity();
  CarrierAndLowerCodecs();
  DescriptorEnvelope();
  NullStateAndDescriptors();
  PresentSemanticSurfacesWork();
  Persistence();
  std::cout << "base real64 carrier/state checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
