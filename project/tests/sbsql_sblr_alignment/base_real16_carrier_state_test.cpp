// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
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

scratchbird::engine::ExecutionTypeDescriptor Real16Descriptor() {
  return DescriptorFor(dt::CanonicalTypeId::real16);
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
  return "SBSET2;element=real16;descriptor=" +
      SetDescriptorFingerprint(descriptor) +
      ";ordered=0;nulls=" + (allow_nulls ? "1" : "0") +
      ";duplicates=" + (allow_duplicates ? "1" : "0") +
      ";items=" + std::string(items);
}

std::string Carrier(std::uint16_t raw_bits) {
  std::string encoded;
  if (!dt::EncodeReal16CarrierBitsV1(raw_bits, &encoded)) return {};
  return encoded;
}

dt::DatatypeOperationValue Present(std::uint16_t raw_bits) {
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::real16, Carrier(raw_bits), false};
  value.descriptor = Real16Descriptor();
  return value;
}

dt::DatatypeOperationValue TypedNull() {
  auto descriptor = Real16Descriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::real16, {}, true};
  value.descriptor = descriptor;
  return value;
}

constexpr platform::Uuid kDescriptorUuid{{
    0x8b,0,0,0,0x72,0x65,0x71,0x6c,0xb1,0x36,0,0,0,0,0,0}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x0e}};
constexpr platform::Uuid kCodecUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x0f}};
constexpr std::string_view kCodecId = "datatype.real16.ieee754.le.v1";

void ExactIdentity() {
  const auto descriptor = Real16Descriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::real16) &&
            descriptor.bit_width == 16,
        "real16 has the exact UUID-bound descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_snapshot_uuid == dt::kDatatypeCohortV5 &&
        row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::real16);
  });
  Check(found != rows.end() && found->descriptor_uuid == kDescriptorUuid &&
            found->type_uuid == kTypeUuid && found->codec_uuid == kCodecUuid &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == kCodecId && found->codec_version == 1 &&
            found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 2 &&
            found->canonical_value_maximum_bytes == 2 &&
            found->canonical_value_exact_bytes == 2 && found->null_supported &&
            found->canonical_byte_order == "little_endian" &&
            found->canonical_representation == "IEEE754_binary16",
        "real16 retains the exact V5 descriptor/type/codec tuple");

  for (const auto cohort : std::array{
           std::tuple{dt::kDatatypeCohortV4, 4ULL, 4ULL},
           std::tuple{dt::kDatatypeCohortV5, 5ULL, 5ULL}}) {
    const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
        std::get<0>(cohort), std::get<1>(cohort), std::get<2>(cohort),
        kDescriptorUuid, 1);
    Check(admitted.ok && admitted.row.type_uuid == kTypeUuid &&
              admitted.row.codec_uuid == kCodecUuid &&
              admitted.row.codec_id == kCodecId,
          "real16 exact identity is present in V4 and inherited by V5");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV1, 1, 1, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV2, 2, 2, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV3, 3, 3, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 4, 5, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, kTypeUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 2).ok,
        "real16 is absent from V1-V3 and crossed identities refuse");

  dt::DatatypeStorageIdentityV1 storage;
  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 1, &storage) &&
            storage.descriptor_uuid == kDescriptorUuid &&
            storage.type_uuid == kTypeUuid &&
            storage.type_id == dt::CanonicalTypeId::real16 &&
            storage.codec.has_value() &&
            storage.codec->codec_uuid == kCodecUuid &&
            storage.codec->codec_id == kCodecId,
        "real16 storage identity preserves the exact UUID tuple");
  const auto layout =
      dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::real16);
  Check(layout.ok() &&
            layout.layout.storage_class == dt::DatatypeStorageClass::inline_fixed &&
            layout.layout.encoding == dt::DatatypeBinaryEncoding::opaque_bytes &&
            layout.layout.inline_bytes == 2,
        "real16 layout is an opaque fixed two-byte structural carrier");
}

void CarrierAndLowerCodecs() {
  // Exhaustion here establishes only an invertible uint16/LE2 carrier bridge.
  // It deliberately makes no claim that any bit pattern is a semantic value.
  bool all_carriers_round_trip = true;
  for (std::uint32_t bits = 0; bits <= 0xffffu; ++bits) {
    std::string encoded = "sentinel";
    std::uint16_t decoded = 0xa55au;
    const bool encoded_ok = dt::EncodeReal16CarrierBitsV1(
        static_cast<std::uint16_t>(bits), &encoded);
    const bool decoded_ok = encoded_ok &&
        dt::DecodeReal16CarrierBitsV1(encoded, &decoded);
    if (!encoded_ok || !decoded_ok || encoded.size() != 2 ||
        static_cast<unsigned char>(encoded[0]) != (bits & 0xffu) ||
        static_cast<unsigned char>(encoded[1]) != ((bits >> 8u) & 0xffu) ||
        decoded != bits) {
      all_carriers_round_trip = false;
      break;
    }
  }
  Check(all_carriers_round_trip,
        "all opaque real16 uint16/LE2 carrier patterns round trip");

  Check(!dt::EncodeReal16CarrierBitsV1(0, nullptr),
        "real16 carrier encoder rejects a null output");
  for (const auto width : {0u, 1u, 3u, 4u, 16u}) {
    const std::string malformed(width, static_cast<char>(0x5a));
    std::uint16_t decoded = 0xa55au;
    const auto before = decoded;
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::real16, false, false, Payload(malformed)});
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::real16,
         dt::DatatypePhysicalValueState::value, Payload(malformed)});
    Check(!dt::DecodeReal16CarrierBitsV1(malformed, &decoded) &&
              decoded == before && !binary.ok() && binary.encoded.empty() &&
              !physical.ok() && physical.bytes.empty(),
          "malformed real16 carrier widths refuse atomically");
  }
  const auto valid = Carrier(0xa55au);
  Check(!dt::DecodeReal16CarrierBitsV1(valid, nullptr),
        "real16 carrier decoder rejects a null output");

  for (const auto bits : {std::uint16_t{0x0000}, std::uint16_t{0x0001},
                          std::uint16_t{0x00ff}, std::uint16_t{0x8000},
                          std::uint16_t{0xffff}}) {
    const auto bytes = Carrier(bits);
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::real16, false, false, Payload(bytes)});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::real16,
         dt::DatatypePhysicalValueState::value, Payload(bytes)});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(binary.ok() && binary_back.ok() &&
              binary_back.value.type_id == dt::CanonicalTypeId::real16 &&
              !binary_back.value.is_null &&
              binary_back.value.payload == Payload(bytes) && physical.ok() &&
              physical_back.ok() &&
              physical_back.value.type_id == dt::CanonicalTypeId::real16 &&
              physical_back.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              physical_back.value.payload == Payload(bytes),
          "binary and physical codecs preserve opaque real16 LE2 bytes");
  }

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::real16, true, false, {}});
  const auto binary_null_back = binary_null.ok()
      ? dt::DecodeDatatypeBinaryValue(binary_null.encoded)
      : dt::DatatypeBinaryResult{};
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::real16,
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
        "lower codecs preserve real16 SQL NULL as state with zero bytes");
  Check(!dt::EncodeDatatypeBinaryValue(
             {dt::CanonicalTypeId::real16, true, false, {0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
             {dt::CanonicalTypeId::real16,
              dt::DatatypePhysicalValueState::sql_null, {0}}).ok(),
        "lower codecs refuse payload bytes in real16 NULL state");
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
        "descriptor envelope preserves exact real16 identity fields");
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
          "corrupt or truncated real16 descriptor envelopes refuse atomically");
  }
}

void NullStateAndDescriptors() {
  auto descriptor = Real16Descriptor();
  descriptor.nullable_allowed = true;
  auto typed_null = TypedNull();

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = typed_null;
    identity.target_type_id = dt::CanonicalTypeId::real16;
    identity.target_descriptor = descriptor;
    identity.context = context;
    identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto bound = dt::CastDatatypeValue(identity);
    Check(bound.ok() &&
              bound.category == dt::DatatypeCastCategory::identity &&
              bound.value.type_id == dt::CanonicalTypeId::real16 &&
              bound.value.is_null && bound.value.encoded_value.empty() &&
              SameUuidBytes(bound.value.descriptor.descriptor_uuid,
                            descriptor.descriptor_uuid),
          "same-descriptor typed real16 NULL identity is admitted");

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::real16;
    contextual.target_descriptor = descriptor;
    contextual.context = context;
    contextual.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    const auto contextual_result = dt::CastDatatypeValue(contextual);
    Check(contextual_result.ok() && contextual_result.value.is_null &&
              contextual_result.value.type_id ==
                  dt::CanonicalTypeId::real16 &&
              contextual_result.value.encoded_value.empty() &&
              SameUuidBytes(contextual_result.value.descriptor.descriptor_uuid,
                            descriptor.descriptor_uuid),
          "contextual NULL binds the exact real16 descriptor");
  }

  auto alias_source = typed_null;
  alias_source.descriptor.stable_name = "opaque-half-carrier";
  auto alias_target = descriptor;
  alias_target.stable_name = "transport-bits-16";
  dt::DatatypeCastRequest alias_identity;
  alias_identity.value = alias_source;
  alias_identity.target_type_id = dt::CanonicalTypeId::real16;
  alias_identity.target_descriptor = alias_target;
  const auto alias_result = dt::CastDatatypeValue(alias_identity);
  Check(alias_result.ok() && alias_result.value.is_null &&
            alias_result.value.descriptor.stable_name ==
                alias_source.descriptor.stable_name,
        "real16 typed NULL identity ignores descriptor display aliases");

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
        "typed real16 NULL identity refuses descriptor substitution");

  auto descriptorless = typed_null;
  descriptorless.descriptor = {};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "real16";
  alias_identity.target_descriptor = descriptor;
  alias_identity.value = descriptorless;
  const auto missing = dt::CastDatatypeValue(alias_identity);
  alias_identity.value = label_only;
  const auto pseudo = dt::CastDatatypeValue(alias_identity);
  Check(!missing.ok() && missing.value.encoded_value.empty() &&
            !pseudo.ok() && pseudo.value.encoded_value.empty(),
        "missing and label-only real16 NULL descriptors refuse");

  alias_identity.value = typed_null;
  alias_identity.value.encoded_value = Carrier(0);
  const auto dirty_null = dt::CastDatatypeValue(alias_identity);
  alias_identity.value = typed_null;
  alias_identity.target_descriptor = descriptor;
  alias_identity.target_descriptor.nullable_allowed = false;
  const auto nonnullable = dt::CastDatatypeValue(alias_identity);
  Check(!dirty_null.ok() && dirty_null.value.encoded_value.empty() &&
            !nonnullable.ok() && nonnullable.value.encoded_value.empty(),
        "dirty or non-nullable real16 NULL state refuses atomically");

  const auto serialized = dt::SerializeDatatypeValue({typed_null});
  dt::DatatypeDeserializationRequest restore;
  restore.expected_type_id = dt::CanonicalTypeId::real16;
  restore.expected_descriptor = descriptor;
  restore.serialized_value = serialized.serialized_value;
  const auto restored = serialized.ok()
      ? dt::DeserializeDatatypeValue(restore)
      : dt::DatatypeDeserializationResult{};
  Check(serialized.ok() && !serialized.serialized_value.empty() &&
            SameUuidBytes(serialized.descriptor.descriptor_uuid,
                          descriptor.descriptor_uuid) && restored.ok() &&
            restored.value.type_id == dt::CanonicalTypeId::real16 &&
            restored.value.is_null && restored.value.encoded_value.empty() &&
            SameUuidBytes(restored.value.descriptor.descriptor_uuid,
                          descriptor.descriptor_uuid),
        "generic framing round trips real16 typed NULL state and descriptor");
}

void PresentSemanticSurfacesRefuse() {
  const auto present = Present(0xa55au);
  const auto original_payload = present.encoded_value;

  for (const auto& candidate : dt::BuiltinDatatypeDescriptors()) {
    Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::real16,
                                   candidate.type_id) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::real16) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::real16,
                                       candidate.type_id, true) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate.type_id,
                                       dt::CanonicalTypeId::real16, true) ==
                  dt::DatatypeCastCategory::forbidden,
          "every registered cast classifier pair incident to real16 refuses");
  }

  const auto character_descriptor =
      DescriptorFor(dt::CanonicalTypeId::character);
  for (const auto& candidate : dt::BuiltinDatatypeDescriptors()) {
    const auto candidate_descriptor = DescriptorFor(candidate.type_id);
    dt::DatatypeOperationValue candidate_value{
        candidate.type_id, "test-only-present-placeholder", false};
    candidate_value.descriptor = candidate_descriptor;
    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      for (const bool compatibility_profile : {false, true}) {
        dt::DatatypeCastRequest outgoing;
        outgoing.value = present;
        outgoing.target_type_id = candidate.type_id;
        outgoing.target_descriptor = candidate_descriptor;
        outgoing.context = context;
        outgoing.explicit_cast =
            context == dt::DatatypeCastContext::explicit_cast;
        outgoing.reference_compatibility_profile = compatibility_profile;
        const auto outgoing_result = dt::CastDatatypeValue(outgoing);

        dt::DatatypeCastRequest incoming;
        incoming.value = candidate_value;
        incoming.target_type_id = dt::CanonicalTypeId::real16;
        incoming.target_descriptor = present.descriptor;
        incoming.context = context;
        incoming.explicit_cast =
            context == dt::DatatypeCastContext::explicit_cast;
        incoming.reference_compatibility_profile = compatibility_profile;
        const auto incoming_result = dt::CastDatatypeValue(incoming);

        Check(!outgoing_result.ok() &&
                  outgoing_result.category ==
                      dt::DatatypeCastCategory::forbidden &&
                  outgoing_result.value.type_id ==
                      dt::CanonicalTypeId::unknown &&
                  outgoing_result.value.encoded_value.empty() &&
                  outgoing_result.diagnostic.diagnostic_code ==
                      "DATATYPE.CAST_FORBIDDEN" &&
                  DiagnosticDetail(outgoing_result.diagnostic) ==
                      "real16_present_cast_policy_unresolved" &&
                  !incoming_result.ok() &&
                  incoming_result.category ==
                      dt::DatatypeCastCategory::forbidden &&
                  incoming_result.value.type_id ==
                      dt::CanonicalTypeId::unknown &&
                  incoming_result.value.encoded_value.empty() &&
                  incoming_result.diagnostic.diagnostic_code ==
                      "DATATYPE.CAST_FORBIDDEN" &&
                  DiagnosticDetail(incoming_result.diagnostic) ==
                      "real16_present_cast_policy_unresolved" &&
                  present.encoded_value == original_payload &&
                  candidate_value.encoded_value ==
                      "test-only-present-placeholder",
              "runtime PRESENT casts incident to real16 refuse at policy");
      }
    }
  }

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    for (const bool compatibility_profile : {false, true}) {
      dt::DatatypeCastRequest identity;
      identity.value = present;
      identity.target_type_id = dt::CanonicalTypeId::real16;
      identity.target_descriptor = present.descriptor;
      identity.context = context;
      identity.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      identity.reference_compatibility_profile = compatibility_profile;
      const auto identity_result = dt::CastDatatypeValue(identity);

      dt::DatatypeCastRequest outgoing;
      outgoing.value = present;
      outgoing.target_type_id = dt::CanonicalTypeId::character;
      outgoing.target_descriptor = character_descriptor;
      outgoing.context = context;
      outgoing.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      outgoing.reference_compatibility_profile = compatibility_profile;
      const auto outgoing_result = dt::CastDatatypeValue(outgoing);

      dt::DatatypeCastRequest incoming;
      incoming.value = {dt::CanonicalTypeId::character, "1", false};
      incoming.value.descriptor = character_descriptor;
      incoming.target_type_id = dt::CanonicalTypeId::real16;
      incoming.target_descriptor = present.descriptor;
      incoming.context = context;
      incoming.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      incoming.reference_compatibility_profile = compatibility_profile;
      const auto incoming_result = dt::CastDatatypeValue(incoming);
      Check(!identity_result.ok() &&
                identity_result.value.type_id == dt::CanonicalTypeId::unknown &&
                identity_result.value.encoded_value.empty() &&
                !identity_result.value.is_null &&
                identity.value.encoded_value == original_payload &&
                !outgoing_result.ok() &&
                outgoing_result.value.encoded_value.empty() &&
                !incoming_result.ok() &&
                incoming_result.value.encoded_value.empty(),
            "representative PRESENT casts refuse in every context/profile");

      dt::DatatypeCastRequest null_outgoing;
      null_outgoing.value = TypedNull();
      null_outgoing.target_type_id = dt::CanonicalTypeId::character;
      null_outgoing.target_descriptor = character_descriptor;
      null_outgoing.context = context;
      null_outgoing.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      null_outgoing.reference_compatibility_profile = compatibility_profile;
      const auto null_outgoing_result =
          dt::CastDatatypeValue(null_outgoing);

      dt::DatatypeCastRequest null_incoming;
      null_incoming.value = {dt::CanonicalTypeId::character, {}, true};
      null_incoming.value.descriptor = character_descriptor;
      null_incoming.target_type_id = dt::CanonicalTypeId::real16;
      null_incoming.target_descriptor = present.descriptor;
      null_incoming.context = context;
      null_incoming.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      null_incoming.reference_compatibility_profile = compatibility_profile;
      const auto null_incoming_result =
          dt::CastDatatypeValue(null_incoming);
      Check(!null_outgoing_result.ok() &&
                null_outgoing_result.value.type_id ==
                    dt::CanonicalTypeId::unknown &&
                null_outgoing_result.value.encoded_value.empty() &&
                !null_incoming_result.ok() &&
                null_incoming_result.value.type_id ==
                    dt::CanonicalTypeId::unknown &&
                null_incoming_result.value.encoded_value.empty(),
            "typed NULL real16 cross casts refuse in every context/profile");
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
    request.type_id = dt::CanonicalTypeId::real16;
    request.left = present;
    request.right = present;
    request.result_descriptor = operation ==
            dt::DatatypeNumericOperationKind::compare
        ? DescriptorFor(dt::CanonicalTypeId::boolean)
        : present.descriptor;
    const auto result = dt::ApplyNumericOperation(request);
    Check(!result.ok() && result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() && !result.value.is_null &&
              result.comparison == 0 && result.numeric_facts.invalid &&
              result.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_NUMERIC_OPERATION_REJECTED" &&
              DiagnosticDetail(result.diagnostic) ==
                  "real16_numeric_policy_unresolved" &&
              request.left.encoded_value == original_payload &&
              request.right.encoded_value == original_payload,
          "every PRESENT real16 numeric operation refuses atomically");
  }

  auto alias_left = present;
  auto alias_right = present;
  alias_left.descriptor.stable_name = "opaque-half-carrier";
  alias_right.descriptor.stable_name = "transport-bits-16";
  auto compared = dt::CompareDatatypeValues({alias_left, alias_right});
  Check(!compared.ok() && compared.comparison == 0 &&
            compared.diagnostic.diagnostic_code ==
                "SB_DATATYPE_COMPARISON_REJECTED" &&
            DiagnosticDetail(compared.diagnostic) ==
                "real16_comparison_policy_unresolved",
        "real16 aliases do not enable PRESENT comparison semantics");

  auto mismatched = present;
  mismatched.descriptor.security_policy_uuid = FixtureV7Uuid(0xd0);
  mismatched.descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  const auto mismatch_left =
      dt::CompareDatatypeValues({mismatched, present});
  const auto mismatch_right =
      dt::CompareDatatypeValues({present, mismatched});
  const auto null_value = TypedNull();
  const auto present_null =
      dt::CompareDatatypeValues({present, null_value});
  const auto null_present =
      dt::CompareDatatypeValues({null_value, present});
  const auto null_null =
      dt::CompareDatatypeValues({null_value, null_value});
  Check(!mismatch_left.ok() && mismatch_left.comparison == 0 &&
            mismatch_left.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            !mismatch_right.ok() && mismatch_right.comparison == 0 &&
            mismatch_right.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            !present_null.ok() && present_null.comparison == 0 &&
            present_null.diagnostic.diagnostic_code ==
                "SB_DATATYPE_COMPARISON_REJECTED" &&
            DiagnosticDetail(present_null.diagnostic) ==
                "real16_comparison_policy_unresolved" &&
            !null_present.ok() && null_present.comparison == 0 &&
            null_present.diagnostic.diagnostic_code ==
                "SB_DATATYPE_COMPARISON_REJECTED" &&
            DiagnosticDetail(null_present.diagnostic) ==
                "real16_comparison_policy_unresolved" &&
            !null_null.ok() && null_null.comparison == 0 &&
            null_null.diagnostic.diagnostic_code ==
                "SB_DATATYPE_COMPARISON_REJECTED" &&
            DiagnosticDetail(null_null.diagnostic) ==
                "real16_comparison_policy_unresolved",
        "real16 comparison refuses descriptor mismatch and NULL cases");

  auto descriptorless_present = present;
  descriptorless_present.descriptor = {};
  auto label_only_present = descriptorless_present;
  label_only_present.descriptor.stable_name = "real16";
  dt::DatatypeOperationValue wrong_descriptor_present{
      dt::CanonicalTypeId::real16, present.encoded_value, false,
      character_descriptor};
  for (const auto& invalid : {descriptorless_present, label_only_present,
                              mismatched, wrong_descriptor_present}) {
    dt::DatatypeCastRequest cast;
    cast.value = invalid;
    cast.target_type_id = dt::CanonicalTypeId::real16;
    cast.target_descriptor = present.descriptor;
    const auto cast_result = dt::CastDatatypeValue(cast);

    dt::DatatypeNumericOperationRequest numeric;
    numeric.operation = dt::DatatypeNumericOperationKind::canonicalize;
    numeric.type_id = dt::CanonicalTypeId::real16;
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
          "invalid PRESENT descriptors fail before real16 policy");
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
        incoming.target_type_id = dt::CanonicalTypeId::real16;
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
              "invalid real16 target descriptor precedes cast policy");
      }
    }
  }

  dt::DatatypeExtractRequest exact_extract;
  exact_extract.value = present;
  exact_extract.field = "unsupported";
  const auto exact_extract_result = dt::ExtractDatatypeField(exact_extract);
  Check(!exact_extract_result.ok() &&
            exact_extract_result.value.type_id == dt::CanonicalTypeId::unknown &&
            exact_extract_result.value.encoded_value.empty() &&
            exact_extract_result.diagnostic.diagnostic_code ==
                "SB_DATATYPE_EXTRACT_REJECTED" &&
            DiagnosticDetail(exact_extract_result.diagnostic) ==
                "real16_extract_policy_unresolved",
        "exact-descriptor real16 extraction reaches supported refusal");
  for (const auto& invalid :
       {descriptorless_present, label_only_present,
        dt::DatatypeOperationValue{dt::CanonicalTypeId::real16,
                                   present.encoded_value, false,
                                   character_descriptor}}) {
    auto request = exact_extract;
    request.value = invalid;
    const auto result = dt::ExtractDatatypeField(request);
    const bool valid_failure = !result.ok() &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() &&
              result.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(result.diagnostic) ==
                  "real16_extract_descriptor_invalid";
    Check(valid_failure,
          "real16 extraction validates PRESENT descriptor before policy: " +
              result.diagnostic.diagnostic_code + "/" +
              DiagnosticDetail(result.diagnostic));
  }

  const auto key = dt::MakeDatatypeSortKey({present});
  const auto null_key = dt::MakeDatatypeSortKey({null_value});
  const auto hash = dt::HashDatatypeValue({present});
  const auto null_hash = dt::HashDatatypeValue({null_value});
  const auto display = dt::RenderDatatypeValueForDisplay({present});
  const auto null_display = dt::RenderDatatypeValueForDisplay({null_value});
  const auto serialized = dt::SerializeDatatypeValue({present});
  Check(!key.ok() && key.sort_key.empty() &&
            key.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SORT_KEY_REJECTED" &&
            DiagnosticDetail(key.diagnostic) ==
                "real16_sort_key_policy_unresolved" &&
            !null_key.ok() && null_key.sort_key.empty() &&
            null_key.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SORT_KEY_REJECTED" &&
            DiagnosticDetail(null_key.diagnostic) ==
                "real16_sort_key_policy_unresolved" &&
            !hash.ok() && hash.stable_hash_hex.empty() &&
            hash.diagnostic.diagnostic_code ==
                "SB_DATATYPE_HASH_REJECTED" &&
            DiagnosticDetail(hash.diagnostic) ==
                "real16_hash_policy_unresolved" &&
            !null_hash.ok() && null_hash.stable_hash_hex.empty() &&
            null_hash.diagnostic.diagnostic_code ==
                "SB_DATATYPE_HASH_REJECTED" &&
            DiagnosticDetail(null_hash.diagnostic) ==
                "real16_hash_policy_unresolved" &&
            !display.ok() && display.canonical_type_name.empty() &&
            display.display_value.empty() &&
            display.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DISPLAY_RENDER_REJECTED" &&
            DiagnosticDetail(display.diagnostic) ==
                "real16_display_policy_unresolved" &&
            !null_display.ok() &&
            null_display.canonical_type_name.empty() &&
            null_display.display_value.empty() &&
            null_display.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DISPLAY_RENDER_REJECTED" &&
            DiagnosticDetail(null_display.diagnostic) ==
                "real16_display_policy_unresolved" &&
            !serialized.ok() && serialized.serialized_value.empty() &&
            serialized.descriptor.canonical_type_id == 0 &&
            serialized.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SERIALIZATION_REJECTED" &&
            DiagnosticDetail(serialized.diagnostic) ==
                "real16_present_value_policy_unresolved",
        "real16 key, hash, display, and PRESENT serialization fail closed");

  dt::DatatypeDeserializationRequest restore;
  restore.expected_type_id = dt::CanonicalTypeId::real16;
  restore.expected_descriptor = present.descriptor;
  restore.serialized_value = "SBDV1;type=real16;state=value;payload=a55a";
  const auto restored = dt::DeserializeDatatypeValue(restore);
  Check(!restored.ok() &&
            restored.value.type_id == dt::CanonicalTypeId::unknown &&
            restored.value.encoded_value.empty() && !restored.value.is_null &&
            restored.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
            DiagnosticDetail(restored.diagnostic) ==
                "real16_present_value_policy_unresolved",
        "generic framing cannot re-admit a PRESENT real16 operation value");

  for (const auto& invalid_expected_descriptor :
       {scratchbird::engine::ExecutionTypeDescriptor{},
        label_only_present.descriptor, character_descriptor,
        mismatched.descriptor}) {
    auto invalid_restore = restore;
    invalid_restore.expected_descriptor = invalid_expected_descriptor;
    const auto invalid_result =
        dt::DeserializeDatatypeValue(invalid_restore);
    Check(!invalid_result.ok() &&
              invalid_result.value.type_id == dt::CanonicalTypeId::unknown &&
              invalid_result.value.encoded_value.empty() &&
              !invalid_result.value.is_null &&
              invalid_result.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID",
          "invalid real16 deserialize descriptor precedes semantic policy");
  }

  auto malformed_hex_restore = restore;
  malformed_hex_restore.serialized_value =
      "SBDV1;type=real16;state=value;payload=zz";
  const auto malformed_hex =
      dt::DeserializeDatatypeValue(malformed_hex_restore);
  auto invalid_descriptor_malformed_hex = malformed_hex_restore;
  invalid_descriptor_malformed_hex.expected_descriptor = character_descriptor;
  const auto invalid_descriptor_first =
      dt::DeserializeDatatypeValue(invalid_descriptor_malformed_hex);
  for (const auto& non_exact_descriptor :
       {scratchbird::engine::ExecutionTypeDescriptor{},
        label_only_present.descriptor, mismatched.descriptor}) {
    auto non_exact_malformed_hex = malformed_hex_restore;
    non_exact_malformed_hex.expected_descriptor = non_exact_descriptor;
    const auto descriptor_precedence =
        dt::DeserializeDatatypeValue(non_exact_malformed_hex);
    Check(!descriptor_precedence.ok() &&
              descriptor_precedence.value.type_id ==
                  dt::CanonicalTypeId::unknown &&
              descriptor_precedence.value.encoded_value.empty() &&
              !descriptor_precedence.value.is_null &&
              descriptor_precedence.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(descriptor_precedence.diagnostic) ==
                  "expected_descriptor_invalid",
          "non-exact real16 descriptor precedes malformed payload hex");
  }
  Check(!malformed_hex.ok() &&
            malformed_hex.value.type_id == dt::CanonicalTypeId::unknown &&
            malformed_hex.value.encoded_value.empty() &&
            malformed_hex.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
            DiagnosticDetail(malformed_hex.diagnostic) ==
                "payload_hex_invalid" &&
            !invalid_descriptor_first.ok() &&
            invalid_descriptor_first.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            invalid_descriptor_first.value.encoded_value.empty() &&
            invalid_descriptor_first.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            DiagnosticDetail(invalid_descriptor_first.diagnostic) ==
                "expected_descriptor_invalid",
        "real16 deserialization preserves descriptor and frame precedence");

  for (const auto payload : {std::string_view{"00"},
                             std::string_view{"000000"}}) {
    auto wrong_width_restore = restore;
    wrong_width_restore.serialized_value =
        "SBDV1;type=real16;state=value;payload=" + std::string(payload);
    const auto wrong_width =
        dt::DeserializeDatatypeValue(wrong_width_restore);
    Check(!wrong_width.ok() &&
              wrong_width.value.type_id == dt::CanonicalTypeId::unknown &&
              wrong_width.value.encoded_value.empty() &&
              wrong_width.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
              DiagnosticDetail(wrong_width.diagnostic) ==
                  "real16_present_value_policy_unresolved",
          "wrong-width real16 generic frames cannot gain semantic admission");
  }

  dt::DatatypeSetDescriptor set_descriptor;
  set_descriptor.element_type_id = dt::CanonicalTypeId::real16;
  set_descriptor.element_descriptor = present.descriptor;
  const auto encoded_set = dt::EncodeSetValue(set_descriptor, {present});
  const auto empty_set = dt::EncodeSetValue(set_descriptor, {});
  Check(!encoded_set.ok() && encoded_set.encoded_set.empty() &&
            encoded_set.value.type_id == dt::CanonicalTypeId::unknown &&
            encoded_set.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SET_OPERATION_REJECTED" &&
            DiagnosticDetail(encoded_set.diagnostic) ==
                "real16_set_semantics_policy_unresolved" &&
            !empty_set.ok() && empty_set.encoded_set.empty() &&
            empty_set.value.type_id == dt::CanonicalTypeId::unknown &&
            empty_set.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SET_OPERATION_REJECTED" &&
            DiagnosticDetail(empty_set.diagnostic) ==
                "real16_set_semantics_policy_unresolved",
        "PRESENT and empty real16 set construction refuse at policy");

  auto nullable_set_descriptor = set_descriptor;
  nullable_set_descriptor.allow_null_elements = true;
  const auto typed_null_set = dt::EncodeSetValue(
      nullable_set_descriptor, {null_value});
  const auto duplicate_null_set = dt::EncodeSetValue(
      nullable_set_descriptor, {null_value, null_value});
  Check(!typed_null_set.ok() && typed_null_set.encoded_set.empty() &&
            typed_null_set.value.type_id == dt::CanonicalTypeId::unknown &&
            DiagnosticDetail(typed_null_set.diagnostic) ==
                "real16_set_semantics_policy_unresolved" &&
            !duplicate_null_set.ok() &&
            duplicate_null_set.encoded_set.empty() &&
            duplicate_null_set.value.type_id == dt::CanonicalTypeId::unknown &&
            DiagnosticDetail(duplicate_null_set.diagnostic) ==
                "real16_set_semantics_policy_unresolved",
        "typed-NULL-only real16 sets refuse before deduplication or grouping");

  const std::string valid_frame = RealSetFrame(
      present.descriptor, "V" + LowerHex(present.encoded_value));

  for (const auto& invalid_descriptor :
       {scratchbird::engine::ExecutionTypeDescriptor{},
        label_only_present.descriptor, character_descriptor}) {
    auto invalid_set_descriptor = set_descriptor;
    invalid_set_descriptor.element_descriptor = invalid_descriptor;
    const auto encoded = dt::EncodeSetValue(invalid_set_descriptor, {});
    dt::DatatypeSetOperationRequest request;
    request.operation = dt::DatatypeSetOperationKind::membership;
    request.descriptor = invalid_set_descriptor;
    request.left_encoded_set = "malformed-must-not-win";
    request.right_value = present;
    const auto applied = dt::ApplySetOperation(request);
    const bool valid_failure =
        !encoded.ok() && encoded.encoded_set.empty() &&
              encoded.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(encoded.diagnostic) ==
                  "set_element_descriptor_invalid" &&
              !applied.ok() && applied.encoded_set.empty() &&
              applied.value.type_id == dt::CanonicalTypeId::unknown &&
              applied.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(applied.diagnostic) ==
                  "set_element_descriptor_invalid";
    Check(valid_failure,
          "real16 set descriptor failure precedes frame or semantic policy: " +
              encoded.diagnostic.diagnostic_code + "/" +
              DiagnosticDetail(encoded.diagnostic) + " versus " +
              applied.diagnostic.diagnostic_code + "/" +
              DiagnosticDetail(applied.diagnostic));
  }

  for (const auto operation : {dt::DatatypeSetOperationKind::membership,
                               dt::DatatypeSetOperationKind::equals,
                               dt::DatatypeSetOperationKind::subset,
                               dt::DatatypeSetOperationKind::superset,
                               dt::DatatypeSetOperationKind::cardinality}) {
    dt::DatatypeSetOperationRequest request;
    request.operation = operation;
    request.descriptor = set_descriptor;
    request.left_encoded_set = valid_frame;
    request.right_value = present;
    request.right_encoded_set = valid_frame;
    const auto result = dt::ApplySetOperation(request);
    Check(!result.ok() && result.encoded_set.empty() &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() &&
              DiagnosticDetail(result.diagnostic) ==
                  "real16_set_semantics_policy_unresolved",
          "a valid real16 set frame reaches only unresolved-policy refusal");
  }

  for (const auto operation : {dt::DatatypeSetOperationKind::equals,
                               dt::DatatypeSetOperationKind::subset,
                               dt::DatatypeSetOperationKind::superset}) {
    dt::DatatypeSetOperationRequest malformed_right;
    malformed_right.operation = operation;
    malformed_right.descriptor = set_descriptor;
    malformed_right.left_encoded_set = valid_frame;
    malformed_right.right_encoded_set = "malformed-right-frame";
    const auto malformed = dt::ApplySetOperation(malformed_right);

    auto mismatched_right = malformed_right;
    mismatched_right.right_encoded_set = RealSetFrame(
        present.descriptor, "V" + LowerHex(present.encoded_value),
        false, true);
    const auto descriptor_mismatch =
        dt::ApplySetOperation(mismatched_right);
    Check(!malformed.ok() && malformed.encoded_set.empty() &&
              malformed.value.type_id == dt::CanonicalTypeId::unknown &&
              malformed.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SET_OPERATION_REJECTED" &&
              DiagnosticDetail(malformed.diagnostic) ==
                  "right_set_encoding_invalid" &&
              !descriptor_mismatch.ok() &&
              descriptor_mismatch.encoded_set.empty() &&
              descriptor_mismatch.value.type_id ==
                  dt::CanonicalTypeId::unknown &&
              descriptor_mismatch.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              DiagnosticDetail(descriptor_mismatch.diagnostic) ==
                  "right_set_descriptor_mismatch",
          "real16 binary set operations validate the right frame first");
  }

  const auto membership = [&](const dt::DatatypeOperationValue& value,
                              const dt::DatatypeSetDescriptor& descriptor =
                                  dt::DatatypeSetDescriptor{}) {
    dt::DatatypeSetOperationRequest request;
    request.operation = dt::DatatypeSetOperationKind::membership;
    request.descriptor =
        descriptor.element_type_id == dt::CanonicalTypeId::unknown
            ? set_descriptor
            : descriptor;
    request.left_encoded_set = valid_frame;
    request.right_value = value;
    return dt::ApplySetOperation(request);
  };

  dt::DatatypeSetOperationRequest nullable_null_membership;
  nullable_null_membership.operation =
      dt::DatatypeSetOperationKind::membership;
  nullable_null_membership.descriptor = nullable_set_descriptor;
  nullable_null_membership.left_encoded_set =
      RealSetFrame(present.descriptor, "N", true);
  nullable_null_membership.right_value = null_value;
  const auto nullable_null_membership_result =
      dt::ApplySetOperation(nullable_null_membership);
  Check(!nullable_null_membership_result.ok() &&
            nullable_null_membership_result.encoded_set.empty() &&
            nullable_null_membership_result.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            nullable_null_membership_result.value.encoded_value.empty() &&
            nullable_null_membership_result.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SET_OPERATION_REJECTED" &&
            DiagnosticDetail(nullable_null_membership_result.diagnostic) ==
                "real16_set_semantics_policy_unresolved",
        "nullable clean real16 NULL membership reaches policy refusal");

  dt::DatatypeOperationValue wrong_type{
      dt::CanonicalTypeId::character, "x", false};
  wrong_type.descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  const auto wrong_type_result = membership(wrong_type);
  auto missing_membership_descriptor = present;
  missing_membership_descriptor.descriptor = {};
  const auto missing_descriptor_result =
      membership(missing_membership_descriptor);
  const auto wrong_descriptor_result = membership(mismatched);
  auto dirty_null = null_value;
  dirty_null.encoded_value = Carrier(0);
  const auto dirty_null_result = membership(dirty_null);
  const auto null_disallowed_result = membership(null_value);
  Check(!wrong_type_result.ok() &&
            wrong_type_result.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SET_OPERATION_REJECTED" &&
            DiagnosticDetail(wrong_type_result.diagnostic) ==
                "set_membership_type_mismatch" &&
            !missing_descriptor_result.ok() &&
            missing_descriptor_result.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            DiagnosticDetail(missing_descriptor_result.diagnostic) ==
                "set_membership_descriptor_mismatch" &&
            !wrong_descriptor_result.ok() &&
            wrong_descriptor_result.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            DiagnosticDetail(wrong_descriptor_result.diagnostic) ==
                "set_membership_descriptor_mismatch" &&
            !dirty_null_result.ok() &&
            dirty_null_result.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID" &&
            DiagnosticDetail(dirty_null_result.diagnostic) ==
                "set_membership_null_state_invalid" &&
            !null_disallowed_result.ok() &&
            null_disallowed_result.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_NOT_ADMITTED" &&
            DiagnosticDetail(null_disallowed_result.diagnostic) ==
                "set_membership_null_forbidden",
        "real16 membership validates type, descriptor, and NULL state first");

  for (const auto width : {1u, 3u}) {
    auto malformed_present = present;
    malformed_present.encoded_value.assign(width, '\0');
    const auto result = membership(malformed_present);
    Check(!result.ok() && result.encoded_set.empty() &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SET_OPERATION_REJECTED" &&
              DiagnosticDetail(result.diagnostic) ==
                  "set_membership_value_invalid",
          "real16 membership rejects wrong-width PRESENT carrier before policy");
  }

  for (const auto malformed_item : {"V00", "V000000"}) {
    const std::string malformed_frame =
        RealSetFrame(present.descriptor, malformed_item);
    for (const auto operation : {dt::DatatypeSetOperationKind::membership,
                                 dt::DatatypeSetOperationKind::equals,
                                 dt::DatatypeSetOperationKind::subset,
                                 dt::DatatypeSetOperationKind::superset,
                                 dt::DatatypeSetOperationKind::cardinality}) {
      dt::DatatypeSetOperationRequest request;
      request.operation = operation;
      request.descriptor = set_descriptor;
      request.left_encoded_set = malformed_frame;
      request.right_value = present;
      request.right_encoded_set = valid_frame;
      const auto result = dt::ApplySetOperation(request);
      Check(!result.ok() && result.encoded_set.empty() &&
                result.value.type_id == dt::CanonicalTypeId::unknown &&
                result.value.encoded_value.empty() &&
                result.diagnostic.diagnostic_code ==
                    "SB_DATATYPE_SET_OPERATION_REJECTED" &&
                DiagnosticDetail(result.diagnostic) ==
                    "left_set_encoding_invalid",
            "wrong-width real16 set items refuse before semantic policy");
    }
  }
}

std::uint32_t OraclePhysicalChecksum(
    dt::DatatypePhysicalValueState state,
    const std::vector<platform::byte>& payload) {
  std::uint32_t value = 2166136261u;
  const auto mix = [&value](std::uint32_t next) {
    value ^= next;
    value *= 16777619u;
  };
  mix(static_cast<std::uint32_t>(dt::CanonicalTypeId::real16));
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
      static_cast<std::uint32_t>(dt::CanonicalTypeId::real16));
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
  const std::array<std::uint16_t, 3> patterns{{0x0000, 0x5aa5, 0xffff}};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto bits : patterns) {
    const auto payload = Payload(Carrier(bits));
    const auto production = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::real16,
         dt::DatatypePhysicalValueState::value, payload});
    const auto oracle = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    Check(production.ok() && production.bytes == oracle,
          "real16 physical frame matches an independent structural oracle");
    frames.push_back(oracle);
  }
  const auto production_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::real16,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto oracle_null = OraclePhysicalFrame(
      dt::DatatypePhysicalValueState::sql_null, {});
  Check(production_null.ok() && production_null.bytes == oracle_null,
        "real16 NULL physical frame matches the independent oracle");
  frames.push_back(oracle_null);

  // This test-owned envelope records exact identity and frame offsets. It is
  // evidence scaffolding, not a product page, wire, or semantic format.
  constexpr std::size_t header_bytes = 152;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte, 8> magic{
      {'S','B','R','E','1','6','0','1'}};
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
      ("sb-base-real16-" + std::to_string(pid) + ".carrier");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create real16 carrier persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact real16 carrier bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen real16 carrier persistence fixture independently");
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
        decoded.value.type_id == dt::CanonicalTypeId::real16 &&
        (index + 1 == frames.size()
             ? decoded.value.state ==
                       dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state ==
                       dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Carrier(patterns[index])));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "reopen preserves exact real16 identity and opaque carrier frames");

  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size(),
        "real16 persistence boundary refuses a short read");
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only real16 persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened real16 fixture");

  disk::FileDevice corrupt_writer;
  Check(corrupt_writer.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
        "reopen real16 fixture for a persisted corruption check");
  const platform::byte corrupt_checksum =
      static_cast<platform::byte>(expected[header_bytes + 20] ^ 1u);
  const auto corrupt_write = corrupt_writer.WriteAt(
      header_bytes + 20, &corrupt_checksum, sizeof(corrupt_checksum));
  Check(corrupt_write.ok() &&
            corrupt_write.bytes_transferred == sizeof(corrupt_checksum) &&
            corrupt_writer.Sync().ok() && corrupt_writer.Close().ok(),
        "persist and close a corrupt real16 physical frame");

  disk::FileDevice corrupt_reader;
  Check(corrupt_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen the corrupt real16 fixture independently");
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
        "FileDevice reopen exposes corruption to the real16 physical decoder");

  disk::FileDevice truncator;
  Check(truncator.Open(
            path.string(), disk::FileOpenMode::create_or_truncate).ok(),
        "open the real16 fixture through the FileDevice truncate mode");
  const auto truncated_write = truncator.WriteAt(
      0, expected.data(), expected.size() - 1);
  Check(truncated_write.ok() &&
            truncated_write.bytes_transferred == expected.size() - 1 &&
            truncator.Sync().ok() && truncator.Close().ok(),
        "persist and close a one-byte-truncated real16 fixture");

  disk::FileDevice truncated_reader;
  Check(truncated_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen the truncated real16 fixture independently");
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
          "real16 physical decoder rejects corruption and truncation");
  }
}

}  // namespace

int main() {
  ExactIdentity();
  CarrierAndLowerCodecs();
  DescriptorEnvelope();
  NullStateAndDescriptors();
  PresentSemanticSurfacesRefuse();
  Persistence();
  std::cout << "base real16 carrier/state checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
