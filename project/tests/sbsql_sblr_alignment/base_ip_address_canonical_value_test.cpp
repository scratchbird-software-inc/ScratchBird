// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "datatype_binary.hpp"
#include "../support/network_ordering_checks.hpp"
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
    0xd2,0,0,0,0x69,0x70,0x7f,0x61,0xa4,0x64,0x72,0x65,0x73,0x73,0,0}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x14}};
constexpr platform::Uuid kCodecUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x15}};
constexpr std::string_view kCodecId = "datatype.ip_address.network.v1";
constexpr std::string_view kRepresentation =
    "IPv6_network_order_with_IPv4_mapped_prefix";

constexpr std::array<std::uint8_t, 16> kNil{};
constexpr std::array<std::uint8_t, 16> kAllOnes{{
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}};
constexpr std::array<std::uint8_t, 16> kArbitrary{{
    0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77}};
constexpr std::array<std::uint8_t, 16> kMappedZero{{
    0,0,0,0,0,0,0,0,0,0,0xff,0xff,0,0,0,0}};
constexpr std::array<std::uint8_t, 16> kMappedDocumentation{{
    0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xc0,0x00,0x02,0x01}};
constexpr std::array<std::uint8_t, 16> kMappedAllOnes{{
    0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff}};
constexpr std::array<std::uint8_t, 16> kAdjacentNonMapped{{
    0,0,0,0,0,0,0,0,0,0,0xff,0xfe,0xc0,0x00,0x02,0x01}};

template <std::size_t N>
std::string Bytes(const std::array<std::uint8_t, N>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

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

scratchbird::engine::ExecutionTypeDescriptor IpAddressDescriptor() {
  return DescriptorFor(dt::CanonicalTypeId::ip_address);
}

dt::DatatypeOperationValue Present(std::string bytes = Bytes(kArbitrary)) {
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::ip_address,
                                   std::move(bytes), false};
  value.descriptor = IpAddressDescriptor();
  return value;
}

dt::DatatypeOperationValue TypedNull() {
  auto descriptor = IpAddressDescriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::ip_address, {}, true};
  value.descriptor = descriptor;
  return value;
}

void AppendU16(std::string* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<char>((value >> 8u) & 0xffu));
  bytes->push_back(static_cast<char>(value & 0xffu));
}

void AppendU32(std::string* bytes, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
}

void AppendU64(std::string* bytes, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
}

void AppendUuid(std::string* bytes, const scratchbird::engine::Uuid& uuid) {
  bytes->append(reinterpret_cast<const char*>(uuid.bytes), sizeof(uuid.bytes));
}

void AppendString(std::string* bytes, std::string_view value) {
  AppendU32(bytes, static_cast<std::uint32_t>(value.size()));
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
  AppendUuid(&bytes, descriptor.descriptor_uuid);
  AppendU64(&bytes, descriptor.descriptor_epoch);
  AppendU32(&bytes, descriptor.canonical_type_id);
  AppendU16(&bytes, static_cast<std::uint16_t>(descriptor.family));
  AppendU16(&bytes, static_cast<std::uint16_t>(descriptor.width_class));
  AppendString(&bytes, descriptor.stable_name);
  AppendU32(&bytes, descriptor.bit_width);
  AppendU32(&bytes, descriptor.precision);
  AppendU32(&bytes, descriptor.scale);
  AppendU32(&bytes, descriptor.length);
  AppendU32(&bytes, descriptor.vector_dimensions);
  AppendU32(&bytes, descriptor.container_rank);
  AppendU64(&bytes, descriptor.modifier_flags);
  AppendUuid(&bytes, descriptor.domain_uuid);
  AppendU32(&bytes,
            static_cast<std::uint32_t>(descriptor.domain_stack.size()));
  for (const auto& domain : descriptor.domain_stack) AppendUuid(&bytes, domain);
  AppendUuid(&bytes, descriptor.charset_uuid);
  AppendUuid(&bytes, descriptor.collation_uuid);
  AppendUuid(&bytes, descriptor.timezone_uuid);
  AppendUuid(&bytes, descriptor.element_descriptor_uuid);
  AppendUuid(&bytes, descriptor.security_policy_uuid);
  bytes.push_back(descriptor.nullable_allowed ? '\x01' : '\x00');
  bytes.push_back(descriptor.descriptor_authoritative ? '\x01' : '\x00');
  bytes.push_back(descriptor.parser_independent ? '\x01' : '\x00');
  return LowerHex(bytes);
}

std::string SetFrame(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    std::string_view items = {}) {
  return "SBSET2;element=ip_address;descriptor=" +
      SetDescriptorFingerprint(descriptor) +
      ";ordered=0;nulls=0;duplicates=0;items=" + std::string(items);
}

std::string GenericValueFrame(std::string_view payload, bool is_null = false) {
  return "SBDV1;type=ip_address;state=" +
      std::string(is_null ? "null" : "value") +
      ";payload=" + (is_null ? std::string{} : LowerHex(payload));
}

void ExactIdentityAndCohorts() {
  const auto descriptor = IpAddressDescriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::ip_address),
        "IP address execution descriptor has the exact catalog identity");

  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV1, 1, 1, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV2, 2, 2, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV3, 3, 3, kDescriptorUuid, 1).ok,
        "IP address is absent from immutable V1-V3 cohorts");

  struct Cohort { platform::Uuid snapshot; std::uint64_t generation; };
  const std::array cohorts{
      Cohort{kSnapshotV4, 4}, Cohort{kSnapshotV5, 5}};
  for (const auto& cohort : cohorts) {
    const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
        cohort.snapshot, cohort.generation, cohort.generation,
        kDescriptorUuid, 1);
    Check(admitted.ok &&
              SameUuidBytes(admitted.row.descriptor_uuid, kDescriptorUuid) &&
              SameUuidBytes(admitted.row.type_uuid, kTypeUuid) &&
              SameUuidBytes(admitted.row.codec_uuid, kCodecUuid) &&
              admitted.row.descriptor_generation == 1 &&
              admitted.row.type_generation == 1 &&
              admitted.row.codec_id == kCodecId &&
              admitted.row.codec_version == 1 &&
              admitted.row.codec_generation == 1 &&
              admitted.row.canonical_value_bytes == 16 &&
              admitted.row.canonical_value_minimum_bytes == 16 &&
              admitted.row.canonical_value_maximum_bytes == 16 &&
              admitted.row.canonical_value_exact_bytes == 16 &&
              admitted.row.canonical_byte_order == "byte_sequence" &&
              admitted.row.canonical_representation == kRepresentation,
          "V4 introduction and V5 inheritance preserve exact IP address tuple");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV4, 3, 4, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV4, 4, 3, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV4, 4, 4, kDescriptorUuid, 2).ok,
        "mixed cohort and descriptor generations refuse IP address identity");

  dt::DatatypeStorageIdentityV1 storage;
  const auto layout =
      dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::ip_address);
  Check(dt::LookupDatatypeStorageIdentityV1(
            kSnapshotV5, 5, 5, kDescriptorUuid, 1, &storage) &&
            SameUuidBytes(storage.type_uuid, kTypeUuid) &&
            storage.codec.has_value() &&
            SameUuidBytes(storage.codec->codec_uuid, kCodecUuid) &&
            storage.codec->codec_id == kCodecId && layout.ok() &&
            layout.layout.storage_class ==
                dt::DatatypeStorageClass::inline_fixed &&
            layout.layout.encoding ==
                dt::DatatypeBinaryEncoding::network_address_binary &&
            layout.layout.inline_bytes == 16 &&
            layout.layout.requires_descriptor,
        "IP address storage identity and layout retain descriptor-bound network-order binary16");
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
      static_cast<std::uint32_t>(dt::CanonicalTypeId::ip_address));
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
  mix(static_cast<std::uint32_t>(dt::CanonicalTypeId::ip_address));
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
      static_cast<std::uint32_t>(dt::CanonicalTypeId::ip_address));
  platform::StoreLittle16(frame.data() + 12,
      static_cast<std::uint16_t>(state));
  platform::StoreLittle32(frame.data() + 16,
      static_cast<std::uint32_t>(payload.size()));
  platform::StoreLittle32(frame.data() + 20,
      OraclePhysicalChecksum(state, payload));
  std::copy(payload.begin(), payload.end(), frame.begin() + 24);
  return frame;
}

void Raw16AndLowerCodecs() {
  for (const auto& raw : {kNil, kAllOnes, kArbitrary, kMappedZero,
                          kMappedDocumentation, kMappedAllOnes,
                          kAdjacentNonMapped}) {
    const auto payload = Payload(Bytes(raw));
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::ip_address, false, false, payload});
    const auto binary_oracle = OracleBinaryFrame(payload);
    const auto decoded_binary = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    Check(binary.ok() && binary.encoded == binary_oracle &&
              decoded_binary.ok() &&
              decoded_binary.value.type_id == dt::CanonicalTypeId::ip_address &&
              !decoded_binary.value.is_null &&
              decoded_binary.value.payload == payload,
          "IPv6 and mapped/adjacent IP raw16 patterns survive exact binary framing");

    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::ip_address,
         dt::DatatypePhysicalValueState::value, payload});
    const auto physical_oracle = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    const auto decoded_physical = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(physical.ok() && physical.bytes == physical_oracle &&
              decoded_physical.ok() &&
              decoded_physical.value.type_id == dt::CanonicalTypeId::ip_address &&
              decoded_physical.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              decoded_physical.value.payload == payload,
          "IPv6 and mapped/adjacent IP raw16 patterns survive exact physical framing");
  }

  for (std::size_t size = 0; size <= 15; ++size) {
    std::vector<platform::byte> payload(size, 0x5a);
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::ip_address, false, false, payload}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::ip_address,
                dt::DatatypePhysicalValueState::value, payload}).ok(),
          "lower IP address codecs reject every sampled non-16-byte value");
  }
  for (const auto size : {17u, 32u}) {
    std::vector<platform::byte> payload(size, 0x5a);
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::ip_address, false, false, payload}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::ip_address,
                dt::DatatypePhysicalValueState::value, payload}).ok(),
          "lower IP address codecs reject oversized values");
  }
  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::ip_address, true, false, {}});
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::ip_address,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(binary_null.ok() && binary_null.encoded == OracleBinaryFrame({}, true) &&
            physical_null.ok() &&
            physical_null.bytes == OraclePhysicalFrame(
                dt::DatatypePhysicalValueState::sql_null, {}) &&
            !dt::EncodeDatatypeBinaryValue(
                 {dt::CanonicalTypeId::ip_address, true, false, {0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
                 {dt::CanonicalTypeId::ip_address,
                  dt::DatatypePhysicalValueState::sql_null, {0}}).ok(),
        "lower IP address codecs preserve clean NULL and reject dirty NULL");
}

void NullAndDescriptorRules() {
  auto nullable = IpAddressDescriptor();
  nullable.nullable_allowed = true;
  const auto typed_null = TypedNull();
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest typed;
    typed.value = typed_null;
    typed.target_type_id = dt::CanonicalTypeId::ip_address;
    typed.target_descriptor = nullable;
    typed.context = context;
    typed.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto typed_result = dt::CastDatatypeValue(typed);

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::ip_address;
    contextual.target_descriptor = nullable;
    contextual.context = context;
    contextual.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto contextual_result = dt::CastDatatypeValue(contextual);
    Check(typed_result.ok() && typed_result.category ==
              dt::DatatypeCastCategory::identity &&
              typed_result.value.type_id == dt::CanonicalTypeId::ip_address &&
              typed_result.value.is_null &&
              typed_result.value.encoded_value.empty() &&
              contextual_result.ok() && contextual_result.category ==
              dt::DatatypeCastCategory::lossless_implicit &&
              contextual_result.value.type_id == dt::CanonicalTypeId::ip_address &&
              contextual_result.value.is_null &&
              contextual_result.value.encoded_value.empty(),
          "contextual and exact typed IP address NULL bind in every cast context");
  }

  auto alias_source = typed_null;
  alias_source.descriptor.stable_name = "ip_address-display-alias";
  auto alias_target = nullable;
  alias_target.stable_name = "ip_address-target-label";
  dt::DatatypeCastRequest alias;
  alias.value = alias_source;
  alias.target_type_id = dt::CanonicalTypeId::ip_address;
  alias.target_descriptor = alias_target;
  const auto alias_result = dt::CastDatatypeValue(alias);
  Check(alias_result.ok() && alias_result.value.is_null &&
            alias_result.value.descriptor.stable_name ==
                alias_source.descriptor.stable_name,
        "IP address typed-NULL identity ignores display aliases");

  auto mismatch = alias_target;
  ++mismatch.descriptor_epoch;
  alias.target_descriptor = mismatch;
  const auto mismatch_result = dt::CastDatatypeValue(alias);
  Check(RejectedAs(mismatch_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "target_descriptor_invalid") &&
            mismatch_result.value.type_id == dt::CanonicalTypeId::unknown &&
            mismatch_result.value.encoded_value.empty(),
        "IP address typed NULL requires exact descriptor identity atomically");

  auto dirty = typed_null;
  dirty.encoded_value = "x";
  alias.value = dirty;
  alias.target_descriptor = nullable;
  const auto dirty_result = dt::CastDatatypeValue(alias);
  Check(RejectedAs(dirty_result, "DATATYPE.NULL_STATE.INVALID",
                   "null_or_descriptor_state_invalid") &&
            dirty_result.value.encoded_value.empty(),
        "dirty IP address typed NULL fails before policy and publishes no value");

  auto nonnullable = nullable;
  nonnullable.nullable_allowed = false;
  alias.value = typed_null;
  alias.target_descriptor = nonnullable;
  const auto nonnullable_result = dt::CastDatatypeValue(alias);
  Check(RejectedAs(nonnullable_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "target_descriptor_invalid") &&
            nonnullable_result.value.encoded_value.empty(),
        "IP address NULL rejects a non-exact nonnullable target before policy");
}

void PresentSemanticsRefuse() {
  const auto present = Present();
  const auto original = present.encoded_value;
  auto binary_descriptor = DescriptorFor(dt::CanonicalTypeId::binary);
  auto character_descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  auto alias_present = present;
  alias_present.descriptor.stable_name = "ip_address-display-alias";
  dt::DatatypeCastRequest alias_identity;
  alias_identity.value = alias_present;
  alias_identity.target_type_id = dt::CanonicalTypeId::ip_address;
  alias_identity.target_descriptor = present.descriptor;
  const auto alias_cast = dt::CastDatatypeValue(alias_identity);
  const auto alias_serialized = dt::SerializeDatatypeValue({alias_present});
  Check(RejectedAs(alias_cast, "DATATYPE.CAST_FORBIDDEN",
                   "ip_address_present_cast_policy_unresolved") &&
            RejectedAs(alias_serialized,
                       "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "ip_address_serialization_policy_unresolved") &&
            alias_cast.value.encoded_value.empty() &&
            alias_serialized.serialized_value.empty(),
        "IP address display aliases do not replace IP address identity or bypass policy");
  for (const auto& raw : {kMappedZero, kMappedDocumentation,
                          kMappedAllOnes, kAdjacentNonMapped}) {
    const auto mapped = Present(Bytes(raw));
    dt::DatatypeCastRequest identity;
    identity.value = mapped;
    identity.target_type_id = dt::CanonicalTypeId::ip_address;
    identity.target_descriptor = mapped.descriptor;
    const auto rejected = dt::CastDatatypeValue(identity);
    Check(RejectedAs(rejected, "DATATYPE.CAST_FORBIDDEN",
                     "ip_address_present_cast_policy_unresolved") &&
              mapped.encoded_value == Bytes(raw) &&
              rejected.value.encoded_value.empty(),
          "operation boundary preserves mapped and adjacent raw16 bytes while refusing semantics");
  }
  for (const auto candidate : {dt::CanonicalTypeId::ip_address,
                               dt::CanonicalTypeId::binary,
                               dt::CanonicalTypeId::character}) {
    Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::ip_address, candidate) ==
              dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate, dt::CanonicalTypeId::ip_address) ==
              dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::ip_address, candidate,
                                       true) ==
              dt::DatatypeCastCategory::forbidden,
          "all representative IP address incident cast classifications refuse");
  }
  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    const auto candidate = descriptor.type_id;
    const auto incoming =
        dt::ClassifyDatatypeCast(candidate, dt::CanonicalTypeId::ip_address);
    const auto incoming_compat = dt::ClassifyDatatypeCast(
        candidate, dt::CanonicalTypeId::ip_address, true);
    const auto expected_incoming = candidate == dt::CanonicalTypeId::null_type
        ? dt::DatatypeCastCategory::lossless_implicit
        : dt::DatatypeCastCategory::forbidden;
    Check(incoming == expected_incoming &&
              incoming_compat == expected_incoming &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::ip_address,
                                       candidate) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::ip_address,
                                       candidate, true) ==
                  dt::DatatypeCastCategory::forbidden,
          "every registered cast classification incident to IP address is fail-closed except contextual NULL binding");
  }
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    for (const bool compatibility : {false, true}) {
      dt::DatatypeCastRequest identity;
      identity.value = present;
      identity.target_type_id = dt::CanonicalTypeId::ip_address;
      identity.target_descriptor = present.descriptor;
      identity.context = context;
      identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
      identity.reference_compatibility_profile = compatibility;
      const auto identity_result = dt::CastDatatypeValue(identity);

      dt::DatatypeCastRequest outgoing = identity;
      outgoing.target_type_id = dt::CanonicalTypeId::binary;
      outgoing.target_descriptor = binary_descriptor;
      const auto outgoing_result = dt::CastDatatypeValue(outgoing);

      dt::DatatypeCastRequest incoming = identity;
      incoming.value = {dt::CanonicalTypeId::binary, original, false};
      incoming.value.descriptor = binary_descriptor;
      incoming.target_type_id = dt::CanonicalTypeId::ip_address;
      incoming.target_descriptor = present.descriptor;
      const auto incoming_result = dt::CastDatatypeValue(incoming);
      Check(RejectedAs(identity_result, "DATATYPE.CAST_FORBIDDEN",
                       "ip_address_present_cast_policy_unresolved") &&
                RejectedAs(outgoing_result, "DATATYPE.CAST_FORBIDDEN",
                           "base_binary_ordered_pair_forbidden") &&
                RejectedAs(incoming_result, "DATATYPE.CAST_FORBIDDEN",
                           "base_binary_ordered_pair_forbidden") &&
                identity_result.value.type_id == dt::CanonicalTypeId::unknown &&
                outgoing_result.value.type_id == dt::CanonicalTypeId::unknown &&
                incoming_result.value.type_id == dt::CanonicalTypeId::unknown &&
                identity_result.value.encoded_value.empty() &&
                outgoing_result.value.encoded_value.empty() &&
                incoming_result.value.encoded_value.empty() &&
                present.encoded_value == original,
            "IP address PRESENT identity and binary casts refuse atomically");
    }
  }

  character_descriptor.nullable_allowed = true;
  dt::DatatypeCastRequest cross_null;
  cross_null.value = TypedNull();
  cross_null.target_type_id = dt::CanonicalTypeId::character;
  cross_null.target_descriptor = character_descriptor;
  const auto cross_null_result = dt::CastDatatypeValue(cross_null);
  Check(RejectedAs(cross_null_result, "DATATYPE.CAST_FORBIDDEN",
                   "ip_address_cross_type_typed_null_cast_policy_unresolved") &&
            cross_null_result.value.encoded_value.empty(),
        "cross-type IP address typed NULL cast refuses atomically");

  for (const auto operation : {
           dt::DatatypeNumericOperationKind::canonicalize,
           dt::DatatypeNumericOperationKind::add,
           dt::DatatypeNumericOperationKind::subtract,
           dt::DatatypeNumericOperationKind::multiply,
           dt::DatatypeNumericOperationKind::divide,
           dt::DatatypeNumericOperationKind::compare}) {
    dt::DatatypeNumericOperationRequest request;
    request.operation = operation;
    request.type_id = dt::CanonicalTypeId::ip_address;
    request.left = present;
    request.right = present;
    request.result_descriptor = operation ==
            dt::DatatypeNumericOperationKind::compare
        ? DescriptorFor(dt::CanonicalTypeId::boolean)
        : present.descriptor;
    const auto result = dt::ApplyNumericOperation(request);
    Check(RejectedAs(result, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
                     "ip_address_numeric_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() && result.comparison == 0,
          "every IP address numeric operation refuses atomically");
  }

  dt::DatatypeNumericOperationRequest null_numeric;
  null_numeric.operation = dt::DatatypeNumericOperationKind::canonicalize;
  null_numeric.type_id = dt::CanonicalTypeId::ip_address;
  null_numeric.left = TypedNull();
  null_numeric.result_descriptor = TypedNull().descriptor;
  const auto null_numeric_result = dt::ApplyNumericOperation(null_numeric);
  dt::DatatypeExtractRequest null_extract;
  null_extract.value = TypedNull();
  null_extract.field = "version";
  null_extract.result_descriptor = DescriptorFor(dt::CanonicalTypeId::uint8);
  const auto null_extract_result = dt::ExtractDatatypeField(null_extract);
  Check(RejectedAs(null_numeric_result,
                   "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
                   "ip_address_numeric_policy_unresolved") &&
            RejectedAs(null_extract_result, "SB_DATATYPE_EXTRACT_REJECTED",
                       "ip_address_extract_policy_unresolved") &&
            null_numeric_result.value.encoded_value.empty() &&
            null_numeric_result.comparison == 0 &&
            null_extract_result.value.encoded_value.empty(),
        "IP address numeric and extract policy also refuse typed NULL atomically");

  const auto compared = dt::CompareDatatypeValues({present, present});
  const auto null_compared =
      dt::CompareDatatypeValues({TypedNull(), TypedNull()});
  const auto key = dt::MakeDatatypeSortKey({present});
  const auto null_key = dt::MakeDatatypeSortKey({TypedNull()});
  const auto hash = dt::HashDatatypeValue({present});
  const auto null_hash = dt::HashDatatypeValue({TypedNull()});
  const auto display = dt::RenderDatatypeValueForDisplay({present});
  const auto null_display = dt::RenderDatatypeValueForDisplay({TypedNull()});
  const auto serialized = dt::SerializeDatatypeValue({present});
  const auto null_serialized = dt::SerializeDatatypeValue({TypedNull()});
  dt::DatatypeExtractRequest extract;
  extract.value = present;
  extract.field = "version";
  extract.result_descriptor = DescriptorFor(dt::CanonicalTypeId::uint8);
  const auto extracted = dt::ExtractDatatypeField(extract);
  Check(compared.ok() && key.ok() && hash.ok() &&
            null_compared.ok() && null_key.ok() && null_hash.ok() &&
            RejectedAs(display, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
                       "ip_address_display_policy_unresolved") &&
            RejectedAs(null_display, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
                       "ip_address_display_policy_unresolved") &&
            RejectedAs(serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "ip_address_serialization_policy_unresolved") &&
            RejectedAs(null_serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "ip_address_serialization_policy_unresolved") &&
            RejectedAs(extracted, "SB_DATATYPE_EXTRACT_REJECTED",
                       "ip_address_extract_policy_unresolved") &&
            compared.comparison == 0 && null_compared.comparison == 0 &&
            key.sort_key.size() == 17 && null_key.sort_key == std::string(1, '\0') &&
            hash.stable_hash_hex.size() == 16 && null_hash.stable_hash_hex.size() == 16 &&
            display.display_value.empty() && null_display.display_value.empty() &&
            serialized.serialized_value.empty() &&
            null_serialized.serialized_value.empty() &&
            extracted.value.encoded_value.empty(),
        "IP address ordering contract does not grant unrelated generic operations");
}

void DescriptorCarrierAndFramePrecedence() {
  const auto present = Present();
  const auto character_descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  auto descriptorless = present;
  descriptorless.descriptor = {};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "ip_address";
  auto wrong = present;
  wrong.descriptor = character_descriptor;
  auto stale = present;
  ++stale.descriptor.descriptor_epoch;
  auto domain_bound = present;
  domain_bound.descriptor.domain_uuid.bytes[15] = 1;
  auto security_bound = present;
  security_bound.descriptor.security_policy_uuid.bytes[15] = 1;
  for (const auto& invalid : {descriptorless, label_only, wrong, stale,
                              domain_bound, security_bound}) {
    dt::DatatypeCastRequest cast;
    cast.value = invalid;
    cast.target_type_id = dt::CanonicalTypeId::ip_address;
    cast.target_descriptor = present.descriptor;
    const auto cast_result = dt::CastDatatypeValue(cast);
    const auto compare = dt::CompareDatatypeValues({invalid, present});
    const auto key = dt::MakeDatatypeSortKey({invalid});
    const auto hash = dt::HashDatatypeValue({invalid});
    const auto display = dt::RenderDatatypeValueForDisplay({invalid});
    const auto serialized = dt::SerializeDatatypeValue({invalid});
    dt::DatatypeExtractRequest extract;
    extract.value = invalid;
    extract.field = "version";
    extract.result_descriptor = DescriptorFor(dt::CanonicalTypeId::uint8);
    const auto extracted = dt::ExtractDatatypeField(extract);
    Check(cast_result.diagnostic.diagnostic_code ==
              "DATATYPE.DESCRIPTOR.INVALID" &&
              compare.diagnostic.diagnostic_code ==
              "DATATYPE.DESCRIPTOR.INVALID" &&
              key.diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID" &&
              hash.diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID" &&
              display.diagnostic.diagnostic_code ==
              "DATATYPE.DESCRIPTOR.INVALID" &&
              serialized.diagnostic.diagnostic_code ==
              "DATATYPE.DESCRIPTOR.INVALID" &&
              extracted.diagnostic.diagnostic_code ==
              "DATATYPE.DESCRIPTOR.INVALID" &&
              cast_result.value.encoded_value.empty() &&
              compare.comparison == 0 && key.sort_key.empty() &&
              hash.stable_hash_hex.empty() && display.display_value.empty() &&
              serialized.serialized_value.empty() &&
              extracted.value.encoded_value.empty(),
          "non-exact IP address PRESENT descriptor precedes policy on every surface");
  }

  auto malformed = present;
  malformed.encoded_value.pop_back();
  dt::DatatypeCastRequest malformed_cast;
  malformed_cast.value = malformed;
  malformed_cast.target_type_id = dt::CanonicalTypeId::ip_address;
  malformed_cast.target_descriptor = present.descriptor;
  const auto cast = dt::CastDatatypeValue(malformed_cast);
  dt::DatatypeNumericOperationRequest malformed_numeric;
  malformed_numeric.operation =
      dt::DatatypeNumericOperationKind::canonicalize;
  malformed_numeric.type_id = dt::CanonicalTypeId::ip_address;
  malformed_numeric.left = malformed;
  malformed_numeric.result_descriptor = present.descriptor;
  const auto numeric = dt::ApplyNumericOperation(malformed_numeric);
  const auto compare = dt::CompareDatatypeValues({malformed, present});
  const auto key = dt::MakeDatatypeSortKey({malformed});
  const auto hash = dt::HashDatatypeValue({malformed});
  const auto display = dt::RenderDatatypeValueForDisplay({malformed});
  const auto serialized = dt::SerializeDatatypeValue({malformed});
  dt::DatatypeExtractRequest extract;
  extract.value = malformed;
  extract.field = "version";
  extract.result_descriptor = DescriptorFor(dt::CanonicalTypeId::uint8);
  const auto extracted = dt::ExtractDatatypeField(extract);
  Check(RejectedAs(cast, "DATATYPE.CAST_FORBIDDEN",
                   "source_value_invalid") &&
            RejectedAs(numeric, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
                       "ip_address_numeric_value_invalid") &&
            !compare.ok() && !key.ok() && !hash.ok() && !display.ok() &&
            !serialized.ok() && !extracted.ok() && compare.comparison == 0 &&
            cast.value.encoded_value.empty() &&
            numeric.value.encoded_value.empty() && numeric.comparison == 0 &&
            key.sort_key.empty() && hash.stable_hash_hex.empty() &&
            display.display_value.empty() &&
            serialized.serialized_value.empty() &&
            extracted.value.encoded_value.empty() &&
            DiagnosticDetail(compare.diagnostic) ==
                "network_native_carrier_width_invalid" &&
            DiagnosticDetail(key.diagnostic) ==
                "network_native_carrier_width_invalid" &&
            DiagnosticDetail(hash.diagnostic) ==
                "network_native_carrier_width_invalid" &&
            DiagnosticDetail(display.diagnostic) ==
                "ip_address_display_value_invalid" &&
            DiagnosticDetail(serialized.diagnostic) ==
                "ip_address_serialization_value_invalid" &&
            DiagnosticDetail(extracted.diagnostic) ==
                "ip_address_extract_value_invalid",
        "malformed IP address width precedes semantic policy and publishes nothing");

  for (const bool is_null : {false, true}) {
    dt::DatatypeDeserializationRequest restore;
    restore.expected_type_id = dt::CanonicalTypeId::ip_address;
    restore.expected_descriptor = is_null ? TypedNull().descriptor
                                          : present.descriptor;
    restore.serialized_value = GenericValueFrame(Bytes(kArbitrary), is_null);
    const auto result = dt::DeserializeDatatypeValue(restore);
    Check(RejectedAs(result, "SB_DATATYPE_DESERIALIZATION_REJECTED",
                     "ip_address_deserialization_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty(),
          "generic IP address frames cannot admit PRESENT or typed NULL");
  }

  dt::DatatypeDeserializationRequest malformed_restore;
  malformed_restore.expected_type_id = dt::CanonicalTypeId::ip_address;
  malformed_restore.expected_descriptor = present.descriptor;
  malformed_restore.serialized_value = GenericValueFrame("short");
  const auto malformed_result =
      dt::DeserializeDatatypeValue(malformed_restore);
  Check(!malformed_result.ok() &&
            DiagnosticDetail(malformed_result.diagnostic) ==
                "ip_address_deserialization_value_invalid" &&
            malformed_result.value.encoded_value.empty(),
        "malformed IP address frame payload precedes unresolved policy atomically");

  auto missing_descriptor_restore = malformed_restore;
  missing_descriptor_restore.expected_descriptor = {};
  const auto missing_descriptor_result =
      dt::DeserializeDatatypeValue(missing_descriptor_restore);
  Check(RejectedAs(missing_descriptor_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "expected_descriptor_invalid") &&
            missing_descriptor_result.value.encoded_value.empty(),
        "missing IP address expected descriptor precedes malformed frame payload");

  dt::DatatypeDeserializationRequest bad_state;
  bad_state.expected_type_id = dt::CanonicalTypeId::ip_address;
  bad_state.expected_descriptor = present.descriptor;
  bad_state.serialized_value =
      "SBDV1;type=ip_address;state=other;payload=" +
      LowerHex(Bytes(kArbitrary));
  const auto bad_state_result = dt::DeserializeDatatypeValue(bad_state);
  Check(RejectedAs(bad_state_result, "DTYPE.VALUE.STATE_UNHANDLED",
                   "value_state_invalid") &&
            bad_state_result.value.encoded_value.empty(),
        "IP address state validation precedes unresolved policy");

  dt::DatatypeDeserializationRequest dirty_null;
  dirty_null.expected_type_id = dt::CanonicalTypeId::ip_address;
  dirty_null.expected_descriptor = TypedNull().descriptor;
  dirty_null.serialized_value =
      "SBDV1;type=ip_address;state=null;payload=00";
  const auto dirty_null_result = dt::DeserializeDatatypeValue(dirty_null);
  Check(RejectedAs(dirty_null_result, "DATATYPE.NULL_STATE.INVALID",
                   "null_payload_present") &&
            dirty_null_result.value.encoded_value.empty(),
        "IP address dirty NULL fails before unresolved policy");
}

void SetSurfacesRefuse() {
  const auto present = Present();
  dt::DatatypeSetDescriptor descriptor;
  descriptor.element_type_id = dt::CanonicalTypeId::ip_address;
  descriptor.element_descriptor = present.descriptor;
  const auto encoded = dt::EncodeSetValue(descriptor, {present});
  const auto empty = dt::EncodeSetValue(descriptor, {});
  Check(RejectedAs(encoded, "SB_DATATYPE_SET_OPERATION_REJECTED",
                   "ip_address_set_semantics_policy_unresolved") &&
            RejectedAs(empty, "SB_DATATYPE_SET_OPERATION_REJECTED",
                       "ip_address_set_semantics_policy_unresolved") &&
            encoded.encoded_set.empty() && empty.encoded_set.empty(),
        "IP address set construction refuses nonempty and empty sets atomically");

  const auto frame = SetFrame(
      present.descriptor, "V" + LowerHex(Bytes(kArbitrary)));
  for (const auto operation : {dt::DatatypeSetOperationKind::membership,
                               dt::DatatypeSetOperationKind::equals,
                               dt::DatatypeSetOperationKind::subset,
                               dt::DatatypeSetOperationKind::superset,
                               dt::DatatypeSetOperationKind::cardinality}) {
    dt::DatatypeSetOperationRequest request;
    request.operation = operation;
    request.descriptor = descriptor;
    request.left_encoded_set = frame;
    request.right_value = present;
    request.right_encoded_set = frame;
    const auto result = dt::ApplySetOperation(request);
    Check(RejectedAs(result, "SB_DATATYPE_SET_OPERATION_REJECTED",
                     "ip_address_set_semantics_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() && result.encoded_set.empty(),
          "every IP address set operation refuses atomically");
  }
}

void FileDevicePersistence() {
  const std::array carriers{kNil, kAllOnes, kArbitrary, kMappedZero,
                            kMappedDocumentation, kMappedAllOnes,
                            kAdjacentNonMapped};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto& carrier : carriers) {
    frames.push_back(OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, Payload(Bytes(carrier))));
  }
  frames.push_back(OraclePhysicalFrame(
      dt::DatatypePhysicalValueState::sql_null, {}));

  constexpr std::size_t header_bytes = 128;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte, 8> magic{{'S','B','I','P','A','D','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(kDescriptorUuid.bytes.begin(), kDescriptorUuid.bytes.end(),
            expected.begin() + 8);
  std::copy(kTypeUuid.bytes.begin(), kTypeUuid.bytes.end(),
            expected.begin() + 24);
  std::copy(kCodecUuid.bytes.begin(), kCodecUuid.bytes.end(),
            expected.begin() + 40);
  platform::StoreLittle32(expected.data() + 56, 1);
  platform::StoreLittle32(expected.data() + 60,
                          static_cast<std::uint32_t>(frames.size()));
  std::uint32_t offset = header_bytes;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    platform::StoreLittle32(expected.data() + 64 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 68 + index * 8,
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
      ("sb-base-ip_address-" + std::to_string(pid) + ".carrier");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create test-owned IP address persistence envelope");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write sync and close exact test-owned IP address envelope");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen test-owned IP address envelope read-only");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected;
  for (std::size_t index = 0; index < frames.size() && decoded_all; ++index) {
    const auto frame_offset =
        platform::LoadLittle32(actual.data() + 64 + index * 8);
    const auto frame_size =
        platform::LoadLittle32(actual.data() + 68 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::ip_address &&
        (index + 1 == frames.size()
             ? decoded.value.state ==
                       dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state ==
                       dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Bytes(carriers[index])));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "FileDevice close/reopen preserves exact IP address identity, bytes and NULL");

  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size() &&
            !rejected_write.ok() && rejected_write.bytes_transferred == 0 &&
            reader.Close().ok(),
        "IP address fixture detects short reads and read-only writes");

  disk::FileDevice corrupt_writer;
  Check(corrupt_writer.Open(path.string(),
                            disk::FileOpenMode::open_existing).ok(),
        "reopen IP address envelope to persist corruption");
  const platform::byte corrupt_checksum =
      static_cast<platform::byte>(expected[header_bytes + 20] ^ 1u);
  const auto corrupt_write = corrupt_writer.WriteAt(
      header_bytes + 20, &corrupt_checksum, 1);
  Check(corrupt_write.ok() && corrupt_write.bytes_transferred == 1 &&
            corrupt_writer.Sync().ok() && corrupt_writer.Close().ok(),
        "persist and close IP address checksum corruption");

  disk::FileDevice corrupt_reader;
  Check(corrupt_reader.Open(path.string(),
                            disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen corrupt IP address envelope");
  std::vector<platform::byte> corrupt_actual(expected.size());
  const auto corrupt_read = corrupt_reader.ReadAt(
      0, corrupt_actual.data(), corrupt_actual.size());
  const auto corrupt_decoded = corrupt_read.ok()
      ? dt::DecodeDatatypePhysicalValue(
            corrupt_actual.data() + header_bytes, frames.front().size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(corrupt_read.ok() && !corrupt_decoded.ok() &&
            corrupt_reader.Close().ok(),
        "FileDevice reopen exposes IP address physical corruption");

  disk::FileDevice truncator;
  Check(truncator.Open(path.string(),
                       disk::FileOpenMode::create_or_truncate).ok(),
        "open IP address envelope through truncate mode");
  const auto truncated_write = truncator.WriteAt(
      0, expected.data(), expected.size() - 1);
  Check(truncated_write.ok() &&
            truncated_write.bytes_transferred == expected.size() - 1 &&
            truncator.Sync().ok() && truncator.Close().ok(),
        "persist and close one-byte-truncated IP address envelope");
  disk::FileDevice truncated_reader;
  Check(truncated_reader.Open(path.string(),
                              disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen truncated IP address envelope");
  std::vector<platform::byte> truncated_actual(expected.size());
  const auto truncated_read = truncated_reader.ReadAt(
      0, truncated_actual.data(), truncated_actual.size());
  Check(!truncated_read.ok() &&
            truncated_read.bytes_transferred < truncated_actual.size() &&
            truncated_reader.Close().ok(),
        "FileDevice refuses a full read of truncated IP address envelope");

  for (const auto& frame : frames) {
    auto corrupt = frame;
    corrupt[20] ^= 1u;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(
                   frame.data(), frame.size() - 1).ok(),
          "IP address physical decoder rejects corruption and truncation");
  }
}

}  // namespace

int main() {
  scratchbird::tests::CheckNetworkOrdering(dt::CanonicalTypeId::ip_address,
      DescriptorFor(dt::CanonicalTypeId::ip_address), Check);
  ExactIdentityAndCohorts();
  Raw16AndLowerCodecs();
  NullAndDescriptorRules();
  PresentSemanticsRefuse();
  DescriptorCarrierAndFramePrecedence();
  SetSurfacesRefuse();
  FileDevicePersistence();
  std::cout << "base ip_address canonical value checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
