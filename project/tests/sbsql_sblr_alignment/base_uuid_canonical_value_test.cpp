// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"
#include "runtime_platform.hpp"
#include "../support/owned_temp_directory.hpp"

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
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x34}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x35}};
constexpr platform::Uuid kCodecUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x36}};
constexpr std::string_view kCodecId = "datatype.uuid.binary16.v1";
constexpr std::string_view kRepresentation =
    "sixteen_UUID_value_octets_without_text_conversion";

constexpr std::array<std::uint8_t, 16> kNil{};
constexpr std::array<std::uint8_t, 16> kAllOnes{{
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}};
constexpr std::array<std::uint8_t, 16> kArbitrary{{
    0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77}};

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

scratchbird::engine::ExecutionTypeDescriptor UuidDescriptor() {
  return DescriptorFor(dt::CanonicalTypeId::uuid);
}

dt::DatatypeOperationValue Present(std::string bytes = Bytes(kArbitrary)) {
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::uuid,
                                   std::move(bytes), false};
  value.descriptor = UuidDescriptor();
  return value;
}

dt::DatatypeOperationValue TypedNull() {
  auto descriptor = UuidDescriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::uuid, {}, true};
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
  return "SBSET2;element=uuid;descriptor=" +
      SetDescriptorFingerprint(descriptor) +
      ";ordered=0;nulls=0;duplicates=0;items=" + std::string(items);
}

std::string GenericValueFrame(std::string_view payload, bool is_null = false) {
  return "SBDV1;type=uuid;state=" +
      std::string(is_null ? "null" : "value") +
      ";payload=" + (is_null ? std::string{} : LowerHex(payload));
}

std::string PrivateUuidFrame(std::string_view payload, bool is_null = false) {
  std::string frame = "SBDVUUID";
  frame.push_back(is_null ? '\0' : '\1');
  frame.push_back(is_null ? '\0' : '\20');
  if (!is_null) frame.append(payload);
  return frame;
}

void ExactIdentityAndCohorts() {
  const auto descriptor = UuidDescriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::uuid),
        "UUID execution descriptor has the exact catalog identity");

  const auto absent = dt::LookupDatatypeTypeCodecIdentityV1(
      kSnapshotV1, 1, 1, kDescriptorUuid, 1);
  Check(!absent.ok, "UUID is absent from immutable V1/d701");

  struct Cohort { platform::Uuid snapshot; std::uint64_t generation; };
  const std::array cohorts{
      Cohort{kSnapshotV2, 2}, Cohort{kSnapshotV3, 3},
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
          "V2 introduction and V3-V5 inheritance preserve exact UUID tuple");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV2, 1, 2, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV2, 2, 1, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             kSnapshotV2, 2, 2, kDescriptorUuid, 2).ok,
        "mixed cohort and descriptor generations refuse UUID identity");
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
      static_cast<std::uint32_t>(dt::CanonicalTypeId::uuid));
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
  mix(static_cast<std::uint32_t>(dt::CanonicalTypeId::uuid));
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
      static_cast<std::uint32_t>(dt::CanonicalTypeId::uuid));
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
  for (const auto& raw : {kNil, kAllOnes, kArbitrary}) {
    const auto payload = Payload(Bytes(raw));
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::uuid, false, false, payload});
    const auto binary_oracle = OracleBinaryFrame(payload);
    const auto decoded_binary = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    Check(binary.ok() && binary.encoded == binary_oracle &&
              decoded_binary.ok() &&
              decoded_binary.value.type_id == dt::CanonicalTypeId::uuid &&
              !decoded_binary.value.is_null &&
              decoded_binary.value.payload == payload,
          "all UUID raw16 patterns, including nil, survive exact binary framing");

    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uuid,
         dt::DatatypePhysicalValueState::value, payload});
    const auto physical_oracle = OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, payload);
    const auto decoded_physical = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(physical.ok() && physical.bytes == physical_oracle &&
              decoded_physical.ok() &&
              decoded_physical.value.type_id == dt::CanonicalTypeId::uuid &&
              decoded_physical.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              decoded_physical.value.payload == payload,
          "all UUID raw16 patterns survive exact physical framing");
  }

  for (const auto size : {0u, 1u, 15u, 17u, 32u}) {
    std::vector<platform::byte> payload(size, 0x5a);
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::uuid, false, false, payload}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::uuid,
                dt::DatatypePhysicalValueState::value, payload}).ok(),
          "lower UUID codecs reject every sampled non-16-byte value");
  }
  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::uuid, true, false, {}});
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::uuid,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(binary_null.ok() && binary_null.encoded == OracleBinaryFrame({}, true) &&
            physical_null.ok() &&
            physical_null.bytes == OraclePhysicalFrame(
                dt::DatatypePhysicalValueState::sql_null, {}) &&
            !dt::EncodeDatatypeBinaryValue(
                 {dt::CanonicalTypeId::uuid, true, false, {0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
                 {dt::CanonicalTypeId::uuid,
                  dt::DatatypePhysicalValueState::sql_null, {0}}).ok(),
        "lower UUID codecs preserve clean NULL and reject dirty NULL");
}

void NullAndDescriptorRules() {
  auto nullable = UuidDescriptor();
  nullable.nullable_allowed = true;
  const auto typed_null = TypedNull();
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest typed;
    typed.value = typed_null;
    typed.target_type_id = dt::CanonicalTypeId::uuid;
    typed.target_descriptor = nullable;
    typed.context = context;
    typed.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto typed_result = dt::CastDatatypeValue(typed);

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::uuid;
    contextual.target_descriptor = nullable;
    contextual.context = context;
    contextual.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto contextual_result = dt::CastDatatypeValue(contextual);
    Check(typed_result.ok() && typed_result.category ==
              dt::DatatypeCastCategory::identity &&
              typed_result.value.type_id == dt::CanonicalTypeId::uuid &&
              typed_result.value.is_null &&
              typed_result.value.encoded_value.empty() &&
              contextual_result.ok() && contextual_result.category ==
              dt::DatatypeCastCategory::lossless_implicit &&
              contextual_result.value.type_id == dt::CanonicalTypeId::uuid &&
              contextual_result.value.is_null &&
              contextual_result.value.encoded_value.empty(),
          "contextual and exact typed UUID NULL bind in every cast context");
  }

  auto alias_source = typed_null;
  alias_source.descriptor.stable_name = "uuid-display-alias";
  auto alias_target = nullable;
  alias_target.stable_name = "uuid-target-label";
  dt::DatatypeCastRequest alias;
  alias.value = alias_source;
  alias.target_type_id = dt::CanonicalTypeId::uuid;
  alias.target_descriptor = alias_target;
  const auto alias_result = dt::CastDatatypeValue(alias);
  Check(alias_result.ok() && alias_result.value.is_null &&
            alias_result.value.descriptor.stable_name ==
                alias_source.descriptor.stable_name,
        "UUID typed-NULL identity ignores display aliases");

  auto mismatch = alias_target;
  ++mismatch.descriptor_epoch;
  alias.target_descriptor = mismatch;
  const auto mismatch_result = dt::CastDatatypeValue(alias);
  Check(RejectedAs(mismatch_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "target_descriptor_invalid") &&
            mismatch_result.value.type_id == dt::CanonicalTypeId::unknown &&
            mismatch_result.value.encoded_value.empty(),
        "UUID typed NULL requires exact descriptor identity atomically");

  auto dirty = typed_null;
  dirty.encoded_value = "x";
  alias.value = dirty;
  alias.target_descriptor = nullable;
  const auto dirty_result = dt::CastDatatypeValue(alias);
  Check(RejectedAs(dirty_result, "DATATYPE.NULL_STATE.INVALID",
                   "null_or_descriptor_state_invalid") &&
            dirty_result.value.encoded_value.empty(),
        "dirty UUID typed NULL fails before policy and publishes no value");

  auto nonnullable = nullable;
  nonnullable.nullable_allowed = false;
  alias.value = typed_null;
  alias.target_descriptor = nonnullable;
  const auto nonnullable_result = dt::CastDatatypeValue(alias);
  Check(RejectedAs(nonnullable_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "target_descriptor_invalid") &&
            nonnullable_result.value.encoded_value.empty(),
        "UUID NULL rejects a non-exact nonnullable target before policy");
}

void PresentCastsAndProtectedOperations() {
  const auto present = Present();
  const auto original = present.encoded_value;
  auto binary_descriptor = DescriptorFor(dt::CanonicalTypeId::binary);
  auto character_descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  auto alias_present = present;
  alias_present.descriptor.stable_name = "uuid-display-alias";
  dt::DatatypeCastRequest alias_identity;
  alias_identity.value = alias_present;
  alias_identity.target_type_id = dt::CanonicalTypeId::uuid;
  alias_identity.target_descriptor = present.descriptor;
  const auto alias_cast = dt::CastDatatypeValue(alias_identity);
  const auto alias_serialized = dt::SerializeDatatypeValue({alias_present});
  Check(alias_cast.ok() && alias_cast.category==dt::DatatypeCastCategory::identity &&
            alias_cast.value.encoded_value==original && !alias_cast.value.is_null &&
            RejectedAs(alias_serialized,
                       "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "uuid_serialization_policy_unresolved") &&
            alias_serialized.serialized_value.empty(),
        "UUID display aliases do not replace UUID identity or bypass policy");
  for (const auto candidate : {dt::CanonicalTypeId::uuid,
                               dt::CanonicalTypeId::binary,
                               dt::CanonicalTypeId::character}) {
    const auto expected = candidate == dt::CanonicalTypeId::uuid
        ? dt::DatatypeCastCategory::identity : candidate == dt::CanonicalTypeId::binary
        ? dt::DatatypeCastCategory::lossless_explicit
        : dt::DatatypeCastCategory::forbidden;
    Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uuid, candidate) ==
              expected &&
              dt::ClassifyDatatypeCast(candidate, dt::CanonicalTypeId::uuid) ==
              expected &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uuid, candidate,
                                       true) ==
              expected &&
              dt::ClassifyDatatypeCast(candidate, dt::CanonicalTypeId::uuid,
                                       true) == expected,
          "UUID identity is classified separately from explicit binary conversion and unresolved text policy");
  }
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    for (const bool compatibility : {false, true}) {
      dt::DatatypeCastRequest identity;
      identity.value = present;
      identity.target_type_id = dt::CanonicalTypeId::uuid;
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
      incoming.target_type_id = dt::CanonicalTypeId::uuid;
      incoming.target_descriptor = present.descriptor;
      const auto incoming_result = dt::CastDatatypeValue(incoming);
      const bool identity_preserved = identity_result.ok() &&
          identity_result.category==dt::DatatypeCastCategory::identity &&
          identity_result.value.type_id==dt::CanonicalTypeId::uuid &&
          !identity_result.value.is_null && identity_result.value.encoded_value==original &&
          SameUuidBytes(identity_result.value.descriptor.descriptor_uuid,present.descriptor.descriptor_uuid) &&
          identity_result.value.descriptor.descriptor_epoch==present.descriptor.descriptor_epoch;
      if (context == dt::DatatypeCastContext::explicit_cast) {
        Check(identity_preserved && outgoing_result.ok() && incoming_result.ok() &&
                  outgoing_result.category ==
                      dt::DatatypeCastCategory::lossless_explicit &&
                  incoming_result.category ==
                      dt::DatatypeCastCategory::lossless_explicit &&
                  outgoing_result.value.type_id ==
                      dt::CanonicalTypeId::binary &&
                  incoming_result.value.type_id ==
                      dt::CanonicalTypeId::uuid &&
                  !outgoing_result.value.is_null &&
                  !incoming_result.value.is_null &&
                  outgoing_result.value.encoded_value == original &&
                  incoming_result.value.encoded_value == original &&
                  present.encoded_value == original,
              "explicit UUID/binary and UUID identity casts preserve all16 bytes without granting other operations");
      } else {
        Check(identity_preserved &&
                  RejectedAs(outgoing_result, "DATATYPE.CAST_FORBIDDEN",
                             "explicit_cast_required") &&
                  RejectedAs(incoming_result, "DATATYPE.CAST_FORBIDDEN",
                             "explicit_cast_required") &&
                  outgoing_result.value.type_id ==
                      dt::CanonicalTypeId::unknown &&
                  incoming_result.value.type_id ==
                      dt::CanonicalTypeId::unknown &&
                  outgoing_result.value.encoded_value.empty() &&
                  incoming_result.value.encoded_value.empty() &&
                  present.encoded_value == original,
              "implicit and assignment UUID/binary raw16 casts refuse atomically");
      }
    }
  }

  dt::DatatypeCastRequest truncated_binary;
  truncated_binary.value = {dt::CanonicalTypeId::binary,
                            original.substr(0, original.size() - 1), false};
  truncated_binary.value.descriptor = binary_descriptor;
  truncated_binary.target_type_id = dt::CanonicalTypeId::uuid;
  truncated_binary.target_descriptor = present.descriptor;
  truncated_binary.context = dt::DatatypeCastContext::explicit_cast;
  truncated_binary.explicit_cast = true;
  const auto truncated_result = dt::CastDatatypeValue(truncated_binary);
  Check(RejectedAs(truncated_result, "DATATYPE.CAST_FORBIDDEN",
                   "binary_uuid_requires_exactly_16_octets") &&
            truncated_result.value.type_id == dt::CanonicalTypeId::unknown &&
            truncated_result.value.encoded_value.empty(),
        "explicit binary-to-UUID cast refuses a truncated raw15 value atomically");

  character_descriptor.nullable_allowed = true;
  dt::DatatypeCastRequest cross_null;
  cross_null.value = TypedNull();
  cross_null.target_type_id = dt::CanonicalTypeId::character;
  cross_null.target_descriptor = character_descriptor;
  const auto cross_null_result = dt::CastDatatypeValue(cross_null);
  Check(RejectedAs(cross_null_result, "DATATYPE.CAST_FORBIDDEN",
                   "uuid_cross_type_typed_null_cast_policy_unresolved") &&
            cross_null_result.value.encoded_value.empty(),
        "cross-type UUID typed NULL cast refuses atomically");

  for (const auto operation : {
           dt::DatatypeNumericOperationKind::canonicalize,
           dt::DatatypeNumericOperationKind::add,
           dt::DatatypeNumericOperationKind::subtract,
           dt::DatatypeNumericOperationKind::multiply,
           dt::DatatypeNumericOperationKind::divide,
           dt::DatatypeNumericOperationKind::compare}) {
    dt::DatatypeNumericOperationRequest request;
    request.operation = operation;
    request.type_id = dt::CanonicalTypeId::uuid;
    request.left = present;
    request.right = present;
    request.result_descriptor = operation ==
            dt::DatatypeNumericOperationKind::compare
        ? DescriptorFor(dt::CanonicalTypeId::boolean)
        : present.descriptor;
    const auto result = dt::ApplyNumericOperation(request);
    Check(RejectedAs(result, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
                     "uuid_numeric_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() && result.comparison == 0,
          "every UUID numeric operation refuses atomically");
  }

  dt::DatatypeNumericOperationRequest null_numeric;
  null_numeric.operation = dt::DatatypeNumericOperationKind::canonicalize;
  null_numeric.type_id = dt::CanonicalTypeId::uuid;
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
                   "uuid_numeric_policy_unresolved") &&
            RejectedAs(null_extract_result, "SB_DATATYPE_EXTRACT_REJECTED",
                       "uuid_extract_policy_unresolved") &&
            null_numeric_result.value.encoded_value.empty() &&
            null_numeric_result.comparison == 0 &&
            null_extract_result.value.encoded_value.empty(),
        "UUID numeric and extract policy also refuse typed NULL atomically");

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
                   "uuid_ordering_profile_invalid") &&
            RejectedAs(null_compared, "SB_DATATYPE_COMPARISON_REJECTED",
                       "uuid_ordering_profile_invalid") &&
            RejectedAs(key, "SB_DATATYPE_SORT_KEY_REJECTED",
                       "uuid_ordering_profile_invalid") &&
            RejectedAs(null_key, "SB_DATATYPE_SORT_KEY_REJECTED",
                       "uuid_ordering_profile_invalid") &&
            RejectedAs(hash, "SB_DATATYPE_HASH_REJECTED",
                       "uuid_hash_policy_unresolved") &&
            RejectedAs(null_hash, "SB_DATATYPE_HASH_REJECTED",
                       "uuid_hash_policy_unresolved") &&
            RejectedAs(display, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
                       "uuid_display_policy_unresolved") &&
            RejectedAs(null_display, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
                       "uuid_display_policy_unresolved") &&
            RejectedAs(serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "uuid_serialization_policy_unresolved") &&
            RejectedAs(null_serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                       "uuid_serialization_policy_unresolved") &&
            RejectedAs(extracted, "SB_DATATYPE_EXTRACT_REJECTED",
                       "uuid_extract_policy_unresolved") &&
            compared.comparison == 0 && null_compared.comparison == 0 &&
            key.sort_key.empty() && null_key.sort_key.empty() &&
            hash.stable_hash_hex.empty() && null_hash.stable_hash_hex.empty() &&
            display.display_value.empty() && null_display.display_value.empty() &&
            serialized.serialized_value.empty() &&
            null_serialized.serialized_value.empty() &&
            extracted.value.encoded_value.empty(),
        "all UUID semantic and generic serialization surfaces refuse atomically");
}

void DescriptorCarrierAndFramePrecedence() {
  const auto present = Present();
  const auto character_descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  auto descriptorless = present;
  descriptorless.descriptor = {};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "uuid";
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
    cast.target_type_id = dt::CanonicalTypeId::uuid;
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
          "non-exact UUID PRESENT descriptor precedes policy on every surface");
  }

  auto malformed = present;
  malformed.encoded_value.pop_back();
  dt::DatatypeCastRequest malformed_cast;
  malformed_cast.value = malformed;
  malformed_cast.target_type_id = dt::CanonicalTypeId::uuid;
  malformed_cast.target_descriptor = present.descriptor;
  const auto cast = dt::CastDatatypeValue(malformed_cast);
  dt::DatatypeNumericOperationRequest malformed_numeric;
  malformed_numeric.operation =
      dt::DatatypeNumericOperationKind::canonicalize;
  malformed_numeric.type_id = dt::CanonicalTypeId::uuid;
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
                       "uuid_numeric_value_invalid") &&
            !compare.ok() && !key.ok() && !hash.ok() && !display.ok() &&
            !serialized.ok() && !extracted.ok() && compare.comparison == 0 &&
            cast.value.encoded_value.empty() &&
            numeric.value.encoded_value.empty() && numeric.comparison == 0 &&
            key.sort_key.empty() && hash.stable_hash_hex.empty() &&
            display.display_value.empty() &&
            serialized.serialized_value.empty() &&
            extracted.value.encoded_value.empty() &&
            DiagnosticDetail(compare.diagnostic) ==
                "uuid_comparison_value_invalid" &&
            DiagnosticDetail(key.diagnostic) ==
                "uuid_sort_key_value_invalid" &&
            DiagnosticDetail(hash.diagnostic) ==
                "uuid_hash_value_invalid" &&
            DiagnosticDetail(display.diagnostic) ==
                "uuid_display_value_invalid" &&
            DiagnosticDetail(serialized.diagnostic) ==
                "uuid_serialization_value_invalid" &&
            DiagnosticDetail(extracted.diagnostic) ==
                "uuid_extract_value_invalid",
        "malformed UUID width precedes semantic policy and publishes nothing");

  for (const bool is_null : {false, true}) {
    for (const auto& frame : {GenericValueFrame(Bytes(kArbitrary), is_null),
                              PrivateUuidFrame(Bytes(kArbitrary), is_null)}) {
      dt::DatatypeDeserializationRequest restore;
      restore.expected_type_id = dt::CanonicalTypeId::uuid;
      restore.expected_descriptor = is_null ? TypedNull().descriptor
                                            : present.descriptor;
      restore.serialized_value = frame;
      const auto result = dt::DeserializeDatatypeValue(restore);
      Check(RejectedAs(result, "SB_DATATYPE_DESERIALIZATION_REJECTED",
                       "uuid_deserialization_policy_unresolved") &&
                result.value.type_id == dt::CanonicalTypeId::unknown &&
                result.value.encoded_value.empty(),
            "generic and private UUID frames cannot admit PRESENT or typed NULL");
    }
  }

  dt::DatatypeDeserializationRequest malformed_restore;
  malformed_restore.expected_type_id = dt::CanonicalTypeId::uuid;
  malformed_restore.expected_descriptor = present.descriptor;
  malformed_restore.serialized_value = GenericValueFrame("short");
  const auto malformed_result =
      dt::DeserializeDatatypeValue(malformed_restore);
  Check(!malformed_result.ok() &&
            DiagnosticDetail(malformed_result.diagnostic) ==
                "canonical_value_encoding_invalid" &&
            malformed_result.value.encoded_value.empty(),
        "malformed UUID frame payload precedes unresolved policy atomically");

  auto missing_descriptor_restore = malformed_restore;
  missing_descriptor_restore.expected_descriptor = {};
  const auto missing_descriptor_result =
      dt::DeserializeDatatypeValue(missing_descriptor_restore);
  Check(RejectedAs(missing_descriptor_result, "DATATYPE.DESCRIPTOR.INVALID",
                   "expected_descriptor_invalid") &&
            missing_descriptor_result.value.encoded_value.empty(),
        "missing UUID expected descriptor precedes malformed frame payload");

  dt::DatatypeDeserializationRequest short_private;
  short_private.expected_type_id = dt::CanonicalTypeId::uuid;
  short_private.expected_descriptor = {};
  short_private.serialized_value = "SBDVUUID";
  const auto short_private_result =
      dt::DeserializeDatatypeValue(short_private);
  Check(RejectedAs(short_private_result,
                   "SB_DATATYPE_DESERIALIZATION_REJECTED",
                   "uuid_binary_frame_invalid") &&
            short_private_result.value.encoded_value.empty(),
        "malformed private UUID frame precedes missing descriptor");

  auto valid_private_missing_descriptor = short_private;
  valid_private_missing_descriptor.serialized_value =
      PrivateUuidFrame(Bytes(kArbitrary));
  const auto private_missing_descriptor_result =
      dt::DeserializeDatatypeValue(valid_private_missing_descriptor);
  Check(RejectedAs(private_missing_descriptor_result,
                   "DATATYPE.DESCRIPTOR.INVALID",
                   "expected_descriptor_invalid") &&
            private_missing_descriptor_result.value.encoded_value.empty(),
        "valid private UUID frame then requires the exact descriptor");

  auto bad_state_frame = PrivateUuidFrame(Bytes(kArbitrary));
  bad_state_frame[8] = '\2';
  dt::DatatypeDeserializationRequest bad_state;
  bad_state.expected_type_id = dt::CanonicalTypeId::uuid;
  bad_state.expected_descriptor = present.descriptor;
  bad_state.serialized_value = bad_state_frame;
  const auto bad_state_result = dt::DeserializeDatatypeValue(bad_state);
  Check(RejectedAs(bad_state_result, "DTYPE.VALUE.STATE_UNHANDLED",
                   "value_state_invalid") &&
            bad_state_result.value.encoded_value.empty(),
        "private UUID state validation precedes unresolved policy");

  std::string dirty_null_frame = "SBDVUUID";
  dirty_null_frame.push_back('\0');
  dirty_null_frame.push_back('\1');
  dirty_null_frame.push_back('x');
  dt::DatatypeDeserializationRequest dirty_null;
  dirty_null.expected_type_id = dt::CanonicalTypeId::uuid;
  dirty_null.expected_descriptor = TypedNull().descriptor;
  dirty_null.serialized_value = dirty_null_frame;
  const auto dirty_null_result = dt::DeserializeDatatypeValue(dirty_null);
  Check(RejectedAs(dirty_null_result, "DATATYPE.NULL_STATE.INVALID",
                   "null_payload_present") &&
            dirty_null_result.value.encoded_value.empty(),
        "private UUID dirty NULL fails before unresolved policy");
}

void SetSurfacesRefuse() {
  const auto present = Present();
  dt::DatatypeSetDescriptor descriptor;
  descriptor.element_type_id = dt::CanonicalTypeId::uuid;
  descriptor.element_descriptor = present.descriptor;
  const auto encoded = dt::EncodeSetValue(descriptor, {present});
  const auto empty = dt::EncodeSetValue(descriptor, {});
  Check(RejectedAs(encoded, "SB_DATATYPE_SET_OPERATION_REJECTED",
                   "uuid_set_semantics_policy_unresolved") &&
            RejectedAs(empty, "SB_DATATYPE_SET_OPERATION_REJECTED",
                       "uuid_set_semantics_policy_unresolved") &&
            encoded.encoded_set.empty() && empty.encoded_set.empty(),
        "UUID set construction refuses nonempty and empty sets atomically");

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
                     "uuid_set_semantics_policy_unresolved") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty() && result.encoded_set.empty(),
          "every UUID set operation refuses atomically");
  }
}

void FileDevicePersistence() {
  const std::array carriers{kNil, kAllOnes, kArbitrary};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto& carrier : carriers) {
    frames.push_back(OraclePhysicalFrame(
        dt::DatatypePhysicalValueState::value, Payload(Bytes(carrier))));
  }
  frames.push_back(OraclePhysicalFrame(
      dt::DatatypePhysicalValueState::sql_null, {}));

  constexpr std::size_t header_bytes = 128;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte, 8> magic{{'S','B','U','U','I','D','0','1'}};
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

  scratchbird::tests::OwnedTempDirectory fixture;
  const auto root=fixture.path();
  const fs::path path=root/"uuid.carrier";
  {
  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create test-owned UUID persistence envelope");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write sync and close exact test-owned UUID envelope");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen test-owned UUID envelope read-only");
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
        decoded.value.type_id == dt::CanonicalTypeId::uuid &&
        (index + 1 == frames.size()
             ? decoded.value.state ==
                       dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state ==
                       dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Bytes(carriers[index])));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "FileDevice close/reopen preserves exact UUID identity, bytes and NULL");

  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size() &&
            !rejected_write.ok() && rejected_write.bytes_transferred == 0 &&
            reader.Close().ok(),
        "UUID fixture detects short reads and read-only writes");

  disk::FileDevice corrupt_writer;
  Check(corrupt_writer.Open(path.string(),
                            disk::FileOpenMode::open_existing).ok(),
        "reopen UUID envelope to persist corruption");
  const platform::byte corrupt_checksum =
      static_cast<platform::byte>(expected[header_bytes + 20] ^ 1u);
  const auto corrupt_write = corrupt_writer.WriteAt(
      header_bytes + 20, &corrupt_checksum, 1);
  Check(corrupt_write.ok() && corrupt_write.bytes_transferred == 1 &&
            corrupt_writer.Sync().ok() && corrupt_writer.Close().ok(),
        "persist and close UUID checksum corruption");

  disk::FileDevice corrupt_reader;
  Check(corrupt_reader.Open(path.string(),
                            disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen corrupt UUID envelope");
  std::vector<platform::byte> corrupt_actual(expected.size());
  const auto corrupt_read = corrupt_reader.ReadAt(
      0, corrupt_actual.data(), corrupt_actual.size());
  const auto corrupt_decoded = corrupt_read.ok()
      ? dt::DecodeDatatypePhysicalValue(
            corrupt_actual.data() + header_bytes, frames.front().size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(corrupt_read.ok() && !corrupt_decoded.ok() &&
            corrupt_reader.Close().ok(),
        "FileDevice reopen exposes UUID physical corruption");

  disk::FileDevice truncator;
  Check(truncator.Open(path.string(),
                       disk::FileOpenMode::create_or_truncate).ok(),
        "open UUID envelope through truncate mode");
  const auto truncated_write = truncator.WriteAt(
      0, expected.data(), expected.size() - 1);
  Check(truncated_write.ok() &&
            truncated_write.bytes_transferred == expected.size() - 1 &&
            truncator.Sync().ok() && truncator.Close().ok(),
        "persist and close one-byte-truncated UUID envelope");
  disk::FileDevice truncated_reader;
  Check(truncated_reader.Open(path.string(),
                              disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen truncated UUID envelope");
  std::vector<platform::byte> truncated_actual(expected.size());
  const auto truncated_read = truncated_reader.ReadAt(
      0, truncated_actual.data(), truncated_actual.size());
  Check(!truncated_read.ok() &&
            truncated_read.bytes_transferred < truncated_actual.size() &&
            truncated_reader.Close().ok(),
        "FileDevice refuses a full read of truncated UUID envelope");

  for (const auto& frame : frames) {
    auto corrupt = frame;
    corrupt[20] ^= 1u;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(
                   frame.data(), frame.size() - 1).ok(),
          "UUID physical decoder rejects corruption and truncation");
  }
  }
  fixture.Cleanup();
  Check(!fs::exists(root),"UUID fixture and owner sidecars cleaned after all devices close");
}

}  // namespace

int main() {
  ExactIdentityAndCohorts();
  Raw16AndLowerCodecs();
  NullAndDescriptorRules();
  PresentCastsAndProtectedOperations();
  DescriptorCarrierAndFramePrecedence();
  SetSurfacesRefuse();
  FileDevicePersistence();
  std::cout << "base uuid canonical value checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
