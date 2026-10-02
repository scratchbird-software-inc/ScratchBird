// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "datatype_binary.hpp"
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
    0xd3,0,0,0,0x07,0x1d,0x74,0x77,0xaf,0x72,0x6b,0x5f,0x70,0x72,0x65,0x66}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x16}};
constexpr platform::Uuid kCodecUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x17}};
constexpr std::string_view kCodecId = "datatype.network_prefix.network.v1";
constexpr std::string_view kRepresentation =
    "address16_prefix_length_u8_address_family_u8";

constexpr std::array<std::uint8_t, 18> kNil{};
constexpr std::array<std::uint8_t, 18> kAllOnes{{
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}};
constexpr std::array<std::uint8_t, 18> kArbitrary{{
    0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x5a,0xa5}};
// The final two positions are varied structurally. Core does not yet assign
// their valid ranges or authorize interpreting them as semantic values.
constexpr std::array<std::uint8_t, 18> kLastFieldsZero{{
    0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1,0,0}};
constexpr std::array<std::uint8_t, 18> kPrefixPositionVaried{{
    0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1,0x80,0}};
constexpr std::array<std::uint8_t, 18> kFamilyPositionVaried{{
    0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1,0,0x02}};
constexpr std::array<std::uint8_t, 18> kLastFieldsAllOnes{{
    0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1,0xff,0xff}};

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

scratchbird::engine::ExecutionTypeDescriptor NetworkPrefixDescriptor() {
  return DescriptorFor(dt::CanonicalTypeId::network_prefix);
}

dt::DatatypeOperationValue Present(std::string bytes = Bytes(kArbitrary)) {
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::network_prefix,
                                   std::move(bytes), false};
  value.descriptor = NetworkPrefixDescriptor();
  return value;
}

dt::DatatypeOperationValue TypedNull() {
  auto descriptor = NetworkPrefixDescriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::network_prefix, {}, true};
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
  return "SBSET2;element=network_prefix;descriptor=" +
      SetDescriptorFingerprint(descriptor) +
      ";ordered=0;nulls=0;duplicates=0;items=" + std::string(items);
}

std::string GenericValueFrame(std::string_view payload, bool is_null = false) {
  return "SBDV1;type=network_prefix;state=" +
      std::string(is_null ? "null" : "value") +
      ";payload=" + (is_null ? std::string{} : LowerHex(payload));
}

void ExactIdentityAndCohorts() {
  const auto descriptor = NetworkPrefixDescriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::network_prefix),
        "network prefix execution descriptor has the exact catalog identity");

  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV1, 1, 1, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV2, 2, 2, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV3, 3, 3, kDescriptorUuid, 1).ok,
        "network prefix is absent from immutable V1-V3 cohorts");

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
              admitted.row.canonical_value_bytes == 18 &&
              admitted.row.canonical_value_minimum_bytes == 18 &&
              admitted.row.canonical_value_maximum_bytes == 18 &&
              admitted.row.canonical_value_exact_bytes == 18 &&
              admitted.row.canonical_byte_order == "byte_sequence" &&
              admitted.row.canonical_representation == kRepresentation,
          "V4 introduction and V5 inheritance preserve exact network prefix tuple");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV4, 3, 4, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV4, 4, 3, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV4, 4, 4, kDescriptorUuid, 2).ok,
        "mixed cohort and descriptor generations refuse network prefix identity");

  dt::DatatypeStorageIdentityV1 storage;
  const auto layout =
      dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::network_prefix);
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
            layout.layout.inline_bytes == 18 &&
            layout.layout.requires_descriptor,
        "network prefix storage identity and layout retain descriptor-bound opaque binary18");
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
      static_cast<std::uint32_t>(dt::CanonicalTypeId::network_prefix));
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
  mix(static_cast<std::uint32_t>(dt::CanonicalTypeId::network_prefix));
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
      static_cast<std::uint32_t>(dt::CanonicalTypeId::network_prefix));
  platform::StoreLittle16(frame.data() + 12,
      static_cast<std::uint16_t>(state));
  platform::StoreLittle32(frame.data() + 16,
      static_cast<std::uint32_t>(payload.size()));
  platform::StoreLittle32(frame.data() + 20,
      OraclePhysicalChecksum(state, payload));
  std::copy(payload.begin(), payload.end(), frame.begin() + 24);
  return frame;
}

void Raw18AndLowerCodecs() {
  for (const auto& raw : {kNil, kAllOnes, kArbitrary, kLastFieldsZero,
                          kPrefixPositionVaried, kFamilyPositionVaried,
                          kLastFieldsAllOnes}) {
    const auto payload = Payload(Bytes(raw));
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::network_prefix, false, false, payload});
    const auto binary_oracle = OracleBinaryFrame(payload);
    const auto decoded_binary = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    Check(binary.ok() && binary.encoded == binary_oracle &&
              decoded_binary.ok() &&
              decoded_binary.value.type_id == dt::CanonicalTypeId::network_prefix &&
              !decoded_binary.value.is_null &&
              decoded_binary.value.payload == payload,
          "opaque network-prefix raw18 patterns survive exact binary framing");

    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::network_prefix,
         dt::DatatypePhysicalValueState::value, payload});
    const auto physical_oracle = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    const auto decoded_physical = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(physical.ok() && physical.bytes == physical_oracle &&
              decoded_physical.ok() &&
              decoded_physical.value.type_id == dt::CanonicalTypeId::network_prefix &&
              decoded_physical.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              decoded_physical.value.payload == payload,
          "opaque network-prefix raw18 patterns survive exact physical framing");
  }

  for (std::size_t size = 0; size <= 17; ++size) {
    std::vector<platform::byte> payload(size, 0x5a);
    const auto binary_frame = OracleBinaryFrame(payload);
    const auto physical_frame = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::network_prefix, false, false, payload}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::network_prefix,
                dt::DatatypePhysicalValueState::value, payload}).ok() &&
              !dt::DecodeDatatypeBinaryValue(binary_frame).ok() &&
              !dt::DecodeDatatypePhysicalValue(
                   physical_frame.data(), physical_frame.size()).ok(),
          "lower network prefix codecs reject every undersized value");
  }
  for (const auto size : {19u, 36u}) {
    std::vector<platform::byte> payload(size, 0x5a);
    const auto binary_frame = OracleBinaryFrame(payload);
    const auto physical_frame = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::network_prefix, false, false, payload}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::network_prefix,
                dt::DatatypePhysicalValueState::value, payload}).ok() &&
              !dt::DecodeDatatypeBinaryValue(binary_frame).ok() &&
              !dt::DecodeDatatypePhysicalValue(
                   physical_frame.data(), physical_frame.size()).ok(),
          "lower network prefix codecs reject oversized values");
  }
  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::network_prefix, true, false, {}});
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::network_prefix,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(binary_null.ok() && binary_null.encoded == OracleBinaryFrame({}, true) &&
            physical_null.ok() &&
            physical_null.bytes == OraclePhysicalFrame(
                dt::DatatypePhysicalValueState::sql_null, {}) &&
            !dt::EncodeDatatypeBinaryValue(
                 {dt::CanonicalTypeId::network_prefix, true, false, {0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
                 {dt::CanonicalTypeId::network_prefix,
                  dt::DatatypePhysicalValueState::sql_null, {0}}).ok(),
        "lower network prefix codecs preserve clean NULL and reject dirty NULL");
}

void NullAndDescriptorRules() {
  auto nullable = NetworkPrefixDescriptor();
  nullable.nullable_allowed = true;
  const auto typed_null = TypedNull();
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest typed;
    typed.value = typed_null;
    typed.target_type_id = dt::CanonicalTypeId::network_prefix;
    typed.target_descriptor = nullable;
    typed.context = context;
    typed.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto typed_result = dt::CastDatatypeValue(typed);

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::network_prefix;
    contextual.target_descriptor = nullable;
    contextual.context = context;
    contextual.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto contextual_result = dt::CastDatatypeValue(contextual);
    Check(typed_result.ok() && typed_result.category ==
              dt::DatatypeCastCategory::identity &&
              typed_result.value.type_id == dt::CanonicalTypeId::network_prefix &&
              typed_result.value.is_null &&
              typed_result.value.encoded_value.empty() &&
              contextual_result.ok() && contextual_result.category ==
              dt::DatatypeCastCategory::lossless_implicit &&
              contextual_result.value.type_id == dt::CanonicalTypeId::network_prefix &&
              contextual_result.value.is_null &&
              contextual_result.value.encoded_value.empty(),
          "contextual and exact typed network prefix NULL bind in every cast context");
  }

  auto alias_source = typed_null;
  alias_source.descriptor.stable_name = "network_prefix-display-alias";
  auto alias_target = nullable;
  alias_target.stable_name = "network_prefix-target-label";
  dt::DatatypeCastRequest alias;
  alias.value = alias_source;
  alias.target_type_id = dt::CanonicalTypeId::network_prefix;
  alias.target_descriptor = alias_target;
  const auto alias_result = dt::CastDatatypeValue(alias);
  Check(alias_result.ok() && alias_result.value.is_null &&
            alias_result.value.descriptor.stable_name ==
                alias_source.descriptor.stable_name,
        "network prefix typed-NULL identity ignores display aliases");

  auto mismatch = alias_target;
  ++mismatch.descriptor_epoch;
  alias.target_descriptor = mismatch;
  const auto mismatch_result = dt::CastDatatypeValue(alias);
  Check(RejectedAs(mismatch_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "target_descriptor_invalid") &&
            mismatch_result.value.type_id == dt::CanonicalTypeId::unknown &&
            mismatch_result.value.encoded_value.empty(),
        "network prefix typed NULL requires exact descriptor identity atomically");

  auto dirty = typed_null;
  dirty.encoded_value = "x";
  alias.value = dirty;
  alias.target_descriptor = nullable;
  const auto dirty_result = dt::CastDatatypeValue(alias);
  Check(RejectedAs(dirty_result, "DATATYPE.NULL_STATE.INVALID",
                   "null_or_descriptor_state_invalid") &&
            dirty_result.value.encoded_value.empty(),
        "dirty network prefix typed NULL fails before policy and publishes no value");

  auto nonnullable = nullable;
  nonnullable.nullable_allowed = false;
  alias.value = typed_null;
  alias.target_descriptor = nonnullable;
  const auto nonnullable_result = dt::CastDatatypeValue(alias);
  Check(RejectedAs(nonnullable_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "target_descriptor_invalid") &&
            nonnullable_result.value.encoded_value.empty(),
        "network prefix NULL rejects a non-exact nonnullable target before policy");
}

void PresentSemanticsRefuse() {
  const auto present = Present();
  const auto original = present.encoded_value;
  auto binary_descriptor = DescriptorFor(dt::CanonicalTypeId::binary);
  auto character_descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  auto alias_present = present;
  alias_present.descriptor.stable_name = "network_prefix-display-alias";
  dt::DatatypeCastRequest alias_identity;
  alias_identity.value = alias_present;
  alias_identity.target_type_id = dt::CanonicalTypeId::network_prefix;
  alias_identity.target_descriptor = present.descriptor;
  const auto alias_cast = dt::CastDatatypeValue(alias_identity);
  const auto alias_serialized = dt::SerializeDatatypeValue({alias_present});
  Check(RejectedAs(alias_cast, "DATATYPE.CAST_FORBIDDEN",
                   "network_prefix_present_cast_policy_unresolved") &&
            RejectedAs(alias_serialized,
                       "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "network_prefix_serialization_policy_unresolved") &&
            alias_cast.value.encoded_value.empty() &&
            alias_serialized.serialized_value.empty(),
        "network prefix display aliases do not replace network prefix identity or bypass policy");
  for (const auto& raw : {kLastFieldsZero, kPrefixPositionVaried,
                          kFamilyPositionVaried, kLastFieldsAllOnes}) {
    const auto carrier = Present(Bytes(raw));
    dt::DatatypeCastRequest identity;
    identity.value = carrier;
    identity.target_type_id = dt::CanonicalTypeId::network_prefix;
    identity.target_descriptor = carrier.descriptor;
    const auto rejected = dt::CastDatatypeValue(identity);
    Check(RejectedAs(rejected, "DATATYPE.CAST_FORBIDDEN",
                     "network_prefix_present_cast_policy_unresolved") &&
              carrier.encoded_value == Bytes(raw) &&
              rejected.value.encoded_value.empty(),
          "operation boundary preserves opaque field-position raw18 bytes while refusing semantics");
  }
  for (const auto candidate : {dt::CanonicalTypeId::network_prefix,
                               dt::CanonicalTypeId::binary,
                               dt::CanonicalTypeId::character}) {
    Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::network_prefix, candidate) ==
              dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(candidate, dt::CanonicalTypeId::network_prefix) ==
              dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::network_prefix, candidate,
                                       true) ==
              dt::DatatypeCastCategory::forbidden,
          "all representative network prefix incident cast classifications refuse");
  }
  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    const auto candidate = descriptor.type_id;
    const auto incoming =
        dt::ClassifyDatatypeCast(candidate, dt::CanonicalTypeId::network_prefix);
    const auto incoming_compat = dt::ClassifyDatatypeCast(
        candidate, dt::CanonicalTypeId::network_prefix, true);
    const auto expected_incoming = candidate == dt::CanonicalTypeId::null_type
        ? dt::DatatypeCastCategory::lossless_implicit
        : dt::DatatypeCastCategory::forbidden;
    Check(incoming == expected_incoming &&
              incoming_compat == expected_incoming &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::network_prefix,
                                       candidate) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::network_prefix,
                                       candidate, true) ==
                  dt::DatatypeCastCategory::forbidden,
          "every registered cast classification incident to network prefix is fail-closed except contextual NULL binding");

    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      for (const bool compatibility : {false, true}) {
        dt::DatatypeCastRequest outgoing_runtime;
        outgoing_runtime.value = present;
        outgoing_runtime.target_type_id = candidate;
        outgoing_runtime.target_descriptor = DescriptorFor(candidate);
        outgoing_runtime.context = context;
        outgoing_runtime.explicit_cast =
            context == dt::DatatypeCastContext::explicit_cast;
        outgoing_runtime.reference_compatibility_profile = compatibility;
        const auto outgoing_runtime_result =
            dt::CastDatatypeValue(outgoing_runtime);
        Check(!outgoing_runtime_result.ok() &&
                  outgoing_runtime_result.value.type_id ==
                      dt::CanonicalTypeId::unknown &&
                  outgoing_runtime_result.value.encoded_value.empty(),
              "runtime network prefix PRESENT cast refuses every registered target/context/profile");

        if (candidate != dt::CanonicalTypeId::null_type) {
          auto source_descriptor = DescriptorFor(candidate);
          source_descriptor.nullable_allowed = true;
          dt::DatatypeCastRequest incoming_null_runtime;
          incoming_null_runtime.value = {candidate, {}, true};
          incoming_null_runtime.value.descriptor = source_descriptor;
          incoming_null_runtime.target_type_id =
              dt::CanonicalTypeId::network_prefix;
          incoming_null_runtime.target_descriptor = TypedNull().descriptor;
          incoming_null_runtime.context = context;
          incoming_null_runtime.explicit_cast =
              context == dt::DatatypeCastContext::explicit_cast;
          incoming_null_runtime.reference_compatibility_profile =
              compatibility;
          const auto incoming_null_runtime_result =
              dt::CastDatatypeValue(incoming_null_runtime);
          if (candidate == dt::CanonicalTypeId::network_prefix) {
            Check(incoming_null_runtime_result.ok() &&
                      incoming_null_runtime_result.value.type_id ==
                          dt::CanonicalTypeId::network_prefix &&
                      incoming_null_runtime_result.value.is_null &&
                      incoming_null_runtime_result.value.encoded_value.empty(),
                  "runtime network prefix typed-NULL identity remains exact in every context/profile");
          } else {
            Check(!incoming_null_runtime_result.ok() &&
                      incoming_null_runtime_result.value.type_id ==
                          dt::CanonicalTypeId::unknown &&
                      incoming_null_runtime_result.value.encoded_value.empty(),
                  "runtime cross-type typed NULL to network prefix refuses for every registered source/context/profile");
          }
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
      identity.target_type_id = dt::CanonicalTypeId::network_prefix;
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
      incoming.target_type_id = dt::CanonicalTypeId::network_prefix;
      incoming.target_descriptor = present.descriptor;
      const auto incoming_result = dt::CastDatatypeValue(incoming);
      Check(RejectedAs(identity_result, "DATATYPE.CAST_FORBIDDEN",
                       "network_prefix_present_cast_policy_unresolved") &&
                RejectedAs(outgoing_result, "DATATYPE.CAST_FORBIDDEN",
                           "network_prefix_present_cast_policy_unresolved") &&
                RejectedAs(incoming_result, "DATATYPE.CAST_FORBIDDEN",
                           "network_prefix_present_cast_policy_unresolved") &&
                identity_result.value.type_id == dt::CanonicalTypeId::unknown &&
                outgoing_result.value.type_id == dt::CanonicalTypeId::unknown &&
                incoming_result.value.type_id == dt::CanonicalTypeId::unknown &&
                identity_result.value.encoded_value.empty() &&
                outgoing_result.value.encoded_value.empty() &&
                incoming_result.value.encoded_value.empty() &&
                present.encoded_value == original,
            "network prefix PRESENT identity and binary casts refuse atomically");
    }
  }

  character_descriptor.nullable_allowed = true;
  dt::DatatypeCastRequest cross_null;
  cross_null.value = TypedNull();
  cross_null.target_type_id = dt::CanonicalTypeId::character;
  cross_null.target_descriptor = character_descriptor;
  const auto cross_null_result = dt::CastDatatypeValue(cross_null);
  Check(RejectedAs(cross_null_result, "DATATYPE.CAST_FORBIDDEN",
                   "network_prefix_cross_type_typed_null_cast_policy_unresolved") &&
            cross_null_result.value.encoded_value.empty(),
        "cross-type network prefix typed NULL cast refuses atomically");

  for (const auto operation : {
           dt::DatatypeNumericOperationKind::canonicalize,
           dt::DatatypeNumericOperationKind::add,
           dt::DatatypeNumericOperationKind::subtract,
           dt::DatatypeNumericOperationKind::multiply,
           dt::DatatypeNumericOperationKind::divide,
           dt::DatatypeNumericOperationKind::compare}) {
    dt::DatatypeNumericOperationRequest request;
    request.operation = operation;
    request.type_id = dt::CanonicalTypeId::network_prefix;
    request.left = present;
    request.right = present;
    request.result_descriptor = operation ==
            dt::DatatypeNumericOperationKind::compare
        ? DescriptorFor(dt::CanonicalTypeId::boolean)
        : present.descriptor;
    const auto result = dt::ApplyNumericOperation(request);
    Check(RejectedAs(result, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
                     "network_prefix_numeric_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() && result.comparison == 0,
          "every network prefix numeric operation refuses atomically");
  }

  dt::DatatypeNumericOperationRequest null_numeric;
  null_numeric.operation = dt::DatatypeNumericOperationKind::canonicalize;
  null_numeric.type_id = dt::CanonicalTypeId::network_prefix;
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
                   "network_prefix_numeric_policy_unresolved") &&
            RejectedAs(null_extract_result, "SB_DATATYPE_EXTRACT_REJECTED",
                       "network_prefix_extract_policy_unresolved") &&
            null_numeric_result.value.encoded_value.empty() &&
            null_numeric_result.comparison == 0 &&
            null_extract_result.value.encoded_value.empty(),
        "network prefix numeric and extract policy also refuse typed NULL atomically");

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
  Check(RejectedAs(compared, "SB_DATATYPE_COMPARISON_REJECTED",
                   "network_prefix_comparison_policy_unresolved") &&
            RejectedAs(null_compared, "SB_DATATYPE_COMPARISON_REJECTED",
                       "network_prefix_comparison_policy_unresolved") &&
            RejectedAs(key, "SB_DATATYPE_SORT_KEY_REJECTED",
                       "network_prefix_sort_key_policy_unresolved") &&
            RejectedAs(null_key, "SB_DATATYPE_SORT_KEY_REJECTED",
                       "network_prefix_sort_key_policy_unresolved") &&
            RejectedAs(hash, "SB_DATATYPE_HASH_REJECTED",
                       "network_prefix_hash_policy_unresolved") &&
            RejectedAs(null_hash, "SB_DATATYPE_HASH_REJECTED",
                       "network_prefix_hash_policy_unresolved") &&
            RejectedAs(display, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
                       "network_prefix_display_policy_unresolved") &&
            RejectedAs(null_display, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
                       "network_prefix_display_policy_unresolved") &&
            RejectedAs(serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "network_prefix_serialization_policy_unresolved") &&
            RejectedAs(null_serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "network_prefix_serialization_policy_unresolved") &&
            RejectedAs(extracted, "SB_DATATYPE_EXTRACT_REJECTED",
                       "network_prefix_extract_policy_unresolved") &&
            compared.comparison == 0 && null_compared.comparison == 0 &&
            key.sort_key.empty() && null_key.sort_key.empty() &&
            hash.stable_hash_hex.empty() && null_hash.stable_hash_hex.empty() &&
            display.display_value.empty() && null_display.display_value.empty() &&
            serialized.serialized_value.empty() &&
            null_serialized.serialized_value.empty() &&
            extracted.value.encoded_value.empty(),
        "all network prefix semantic and generic serialization surfaces refuse atomically");
}

void DescriptorCarrierAndFramePrecedence() {
  const auto present = Present();
  const auto character_descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  auto descriptorless = present;
  descriptorless.descriptor = {};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "network_prefix";
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
    cast.target_type_id = dt::CanonicalTypeId::network_prefix;
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
          "non-exact network prefix PRESENT descriptor precedes policy on every surface");
  }

  auto malformed = present;
  malformed.encoded_value.pop_back();
  dt::DatatypeCastRequest malformed_cast;
  malformed_cast.value = malformed;
  malformed_cast.target_type_id = dt::CanonicalTypeId::network_prefix;
  malformed_cast.target_descriptor = present.descriptor;
  const auto cast = dt::CastDatatypeValue(malformed_cast);
  dt::DatatypeNumericOperationRequest malformed_numeric;
  malformed_numeric.operation =
      dt::DatatypeNumericOperationKind::canonicalize;
  malformed_numeric.type_id = dt::CanonicalTypeId::network_prefix;
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
                       "network_prefix_numeric_value_invalid") &&
            !compare.ok() && !key.ok() && !hash.ok() && !display.ok() &&
            !serialized.ok() && !extracted.ok() && compare.comparison == 0 &&
            cast.value.encoded_value.empty() &&
            numeric.value.encoded_value.empty() && numeric.comparison == 0 &&
            key.sort_key.empty() && hash.stable_hash_hex.empty() &&
            display.display_value.empty() &&
            serialized.serialized_value.empty() &&
            extracted.value.encoded_value.empty() &&
            DiagnosticDetail(compare.diagnostic) ==
                "network_prefix_comparison_value_invalid" &&
            DiagnosticDetail(key.diagnostic) ==
                "network_prefix_sort_key_value_invalid" &&
            DiagnosticDetail(hash.diagnostic) ==
                "network_prefix_hash_value_invalid" &&
            DiagnosticDetail(display.diagnostic) ==
                "network_prefix_display_value_invalid" &&
            DiagnosticDetail(serialized.diagnostic) ==
                "network_prefix_serialization_value_invalid" &&
            DiagnosticDetail(extracted.diagnostic) ==
                "network_prefix_extract_value_invalid",
        "malformed network prefix width precedes semantic policy and publishes nothing");

  for (const bool is_null : {false, true}) {
    dt::DatatypeDeserializationRequest restore;
    restore.expected_type_id = dt::CanonicalTypeId::network_prefix;
    restore.expected_descriptor = is_null ? TypedNull().descriptor
                                          : present.descriptor;
    restore.serialized_value = GenericValueFrame(Bytes(kArbitrary), is_null);
    const auto result = dt::DeserializeDatatypeValue(restore);
    Check(RejectedAs(result, "SB_DATATYPE_DESERIALIZATION_REJECTED",
                     "network_prefix_deserialization_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty(),
          "generic network prefix frames cannot admit PRESENT or typed NULL");
  }

  dt::DatatypeDeserializationRequest malformed_restore;
  malformed_restore.expected_type_id = dt::CanonicalTypeId::network_prefix;
  malformed_restore.expected_descriptor = present.descriptor;
  malformed_restore.serialized_value = GenericValueFrame("short");
  const auto malformed_result =
      dt::DeserializeDatatypeValue(malformed_restore);
  Check(!malformed_result.ok() &&
            DiagnosticDetail(malformed_result.diagnostic) ==
                "network_prefix_deserialization_value_invalid" &&
            malformed_result.value.encoded_value.empty(),
        "malformed network prefix frame payload precedes unresolved policy atomically");

  auto missing_descriptor_restore = malformed_restore;
  missing_descriptor_restore.expected_descriptor = {};
  const auto missing_descriptor_result =
      dt::DeserializeDatatypeValue(missing_descriptor_restore);
  Check(RejectedAs(missing_descriptor_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "expected_descriptor_invalid") &&
            missing_descriptor_result.value.encoded_value.empty(),
        "missing network prefix expected descriptor precedes malformed frame payload");

  dt::DatatypeDeserializationRequest bad_state;
  bad_state.expected_type_id = dt::CanonicalTypeId::network_prefix;
  bad_state.expected_descriptor = present.descriptor;
  bad_state.serialized_value =
      "SBDV1;type=network_prefix;state=other;payload=" +
      LowerHex(Bytes(kArbitrary));
  const auto bad_state_result = dt::DeserializeDatatypeValue(bad_state);
  Check(RejectedAs(bad_state_result, "DTYPE.VALUE.STATE_UNHANDLED",
                   "value_state_invalid") &&
            bad_state_result.value.encoded_value.empty(),
        "network prefix state validation precedes unresolved policy");

  dt::DatatypeDeserializationRequest dirty_null;
  dirty_null.expected_type_id = dt::CanonicalTypeId::network_prefix;
  dirty_null.expected_descriptor = TypedNull().descriptor;
  dirty_null.serialized_value =
      "SBDV1;type=network_prefix;state=null;payload=00";
  const auto dirty_null_result = dt::DeserializeDatatypeValue(dirty_null);
  Check(RejectedAs(dirty_null_result, "DATATYPE.NULL_STATE.INVALID",
                   "null_payload_present") &&
            dirty_null_result.value.encoded_value.empty(),
        "network prefix dirty NULL fails before unresolved policy");
}

void SetSurfacesRefuse() {
  const auto present = Present();
  dt::DatatypeSetDescriptor descriptor;
  descriptor.element_type_id = dt::CanonicalTypeId::network_prefix;
  descriptor.element_descriptor = present.descriptor;
  const auto encoded = dt::EncodeSetValue(descriptor, {present});
  const auto empty = dt::EncodeSetValue(descriptor, {});
  Check(RejectedAs(encoded, "SB_DATATYPE_SET_OPERATION_REJECTED",
                   "network_prefix_set_semantics_policy_unresolved") &&
            RejectedAs(empty, "SB_DATATYPE_SET_OPERATION_REJECTED",
                       "network_prefix_set_semantics_policy_unresolved") &&
            encoded.encoded_set.empty() && empty.encoded_set.empty(),
        "network prefix set construction refuses nonempty and empty sets atomically");

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
                     "network_prefix_set_semantics_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() && result.encoded_set.empty(),
          "every network prefix set operation refuses atomically");
  }
}

void FileDevicePersistence() {
  const std::array carriers{kNil, kAllOnes, kArbitrary, kLastFieldsZero,
                            kPrefixPositionVaried, kFamilyPositionVaried,
                            kLastFieldsAllOnes};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto& carrier : carriers) {
    frames.push_back(OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, Payload(Bytes(carrier))));
  }
  frames.push_back(OraclePhysicalFrame(
      dt::DatatypePhysicalValueState::sql_null, {}));

  constexpr std::size_t header_bytes = 128;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte, 8> magic{{'S','B','N','P','F','X','0','1'}};
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
      ("sb-base-network_prefix-" + std::to_string(pid) + ".carrier");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create test-owned network prefix persistence envelope");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write sync and close exact test-owned network prefix envelope");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen test-owned network prefix envelope read-only");
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
        decoded.value.type_id == dt::CanonicalTypeId::network_prefix &&
        (index + 1 == frames.size()
             ? decoded.value.state ==
                       dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state ==
                       dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Bytes(carriers[index])));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "FileDevice close/reopen preserves exact network prefix identity, bytes and NULL");

  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size() &&
            !rejected_write.ok() && rejected_write.bytes_transferred == 0 &&
            reader.Close().ok(),
        "network prefix fixture detects short reads and read-only writes");

  disk::FileDevice corrupt_writer;
  Check(corrupt_writer.Open(path.string(),
                            disk::FileOpenMode::open_existing).ok(),
        "reopen network prefix envelope to persist corruption");
  const platform::byte corrupt_checksum =
      static_cast<platform::byte>(expected[header_bytes + 20] ^ 1u);
  const auto corrupt_write = corrupt_writer.WriteAt(
      header_bytes + 20, &corrupt_checksum, 1);
  Check(corrupt_write.ok() && corrupt_write.bytes_transferred == 1 &&
            corrupt_writer.Sync().ok() && corrupt_writer.Close().ok(),
        "persist and close network prefix checksum corruption");

  disk::FileDevice corrupt_reader;
  Check(corrupt_reader.Open(path.string(),
                            disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen corrupt network prefix envelope");
  std::vector<platform::byte> corrupt_actual(expected.size());
  const auto corrupt_read = corrupt_reader.ReadAt(
      0, corrupt_actual.data(), corrupt_actual.size());
  const auto corrupt_decoded = corrupt_read.ok()
      ? dt::DecodeDatatypePhysicalValue(
            corrupt_actual.data() + header_bytes, frames.front().size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(corrupt_read.ok() && !corrupt_decoded.ok() &&
            corrupt_reader.Close().ok(),
        "FileDevice reopen exposes network prefix physical corruption");

  disk::FileDevice truncator;
  Check(truncator.Open(path.string(),
                       disk::FileOpenMode::create_or_truncate).ok(),
        "open network prefix envelope through truncate mode");
  const auto truncated_write = truncator.WriteAt(
      0, expected.data(), expected.size() - 1);
  Check(truncated_write.ok() &&
            truncated_write.bytes_transferred == expected.size() - 1 &&
            truncator.Sync().ok() && truncator.Close().ok(),
        "persist and close one-byte-truncated network prefix envelope");
  disk::FileDevice truncated_reader;
  Check(truncated_reader.Open(path.string(),
                              disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen truncated network prefix envelope");
  std::vector<platform::byte> truncated_actual(expected.size());
  const auto truncated_read = truncated_reader.ReadAt(
      0, truncated_actual.data(), truncated_actual.size());
  Check(!truncated_read.ok() &&
            truncated_read.bytes_transferred < truncated_actual.size() &&
            truncated_reader.Close().ok(),
        "FileDevice refuses a full read of truncated network prefix envelope");

  for (const auto& frame : frames) {
    auto corrupt = frame;
    corrupt[20] ^= 1u;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(
                   frame.data(), frame.size() - 1).ok(),
          "network prefix physical decoder rejects corruption and truncation");
  }
}

}  // namespace

int main() {
  ExactIdentityAndCohorts();
  Raw18AndLowerCodecs();
  NullAndDescriptorRules();
  PresentSemanticsRefuse();
  DescriptorCarrierAndFramePrecedence();
  SetSurfacesRefuse();
  FileDevicePersistence();
  std::cout << "base network_prefix canonical value checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
