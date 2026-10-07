// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "datatype_binary.hpp"
#include "datatype_binary_view.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_layout.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "datatype_storage_identity.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "disk_device.hpp"
#include "runtime_platform.hpp"
#include "../support/owned_temp_directory.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace binary_allocation_denial {
thread_local bool active = false;
thread_local std::size_t attempts = 0;
}

void* operator new(std::size_t size) {
  if (binary_allocation_denial::active) {
    ++binary_allocation_denial::attempts;
    throw std::bad_alloc();
  }
  if (void* result = std::malloc(size == 0 ? 1 : size)) return result;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return ::operator new(size, std::nothrow);
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}

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
    if (failures <= 100) std::cerr << "FAIL: " << reason << '\n';
  }
}

template <typename LeftUuid, typename RightUuid>
bool SameUuidBytes(const LeftUuid& left, const RightUuid& right) {
  return std::equal(std::begin(left.bytes), std::end(left.bytes),
                    std::begin(right.bytes), std::end(right.bytes));
}

template <typename Result>
bool RejectedAs(const Result& result, std::string_view code) {
  return !result.ok() && result.diagnostic.diagnostic_code == code;
}

dt::DatatypeBinaryAllocationFreeViewResult ValidateWithHeapDenied(
    const dt::DatatypeBinaryValueView& value,
    const dt::DatatypeBinaryDiagnosticContextV1& context) {
  binary_allocation_denial::attempts = 0;
  binary_allocation_denial::active = true;
  const auto result =
      dt::ValidateCanonicalBinaryValueViewNoAlloc(value, context);
  binary_allocation_denial::active = false;
  return result;
}

dt::DatatypeBinaryAllocationFreeViewResult DecodeWithHeapDenied(
    const platform::byte* bytes, std::size_t size,
    const dt::DatatypeBinaryDiagnosticContextV1& context) {
  binary_allocation_denial::attempts = 0;
  binary_allocation_denial::active = true;
  const auto result =
      dt::DecodeCanonicalBinaryValueViewNoAlloc(bytes, size, context);
  binary_allocation_denial::active = false;
  return result;
}

bool AllocationFreeFailurePublishesNothing(
    const dt::DatatypeBinaryAllocationFreeViewResult& result) {
  return result.value.type_id == dt::CanonicalTypeId::unknown &&
      !result.value.is_null && !result.value.payload_is_toast_reference &&
      result.value.payload_data == nullptr && result.value.payload_bytes == 0;
}

bool ExactDiagnosticParity(
    const dt::DatatypeBinaryAllocationFreeViewResult& allocation_free,
    const dt::DatatypeBinaryResult& owning) {
  if (allocation_free.status.code != owning.status.code ||
      allocation_free.status.severity != owning.status.severity ||
      allocation_free.status.subsystem != owning.status.subsystem ||
      allocation_free.diagnostic.diagnostic_code !=
          owning.diagnostic.diagnostic_code ||
      allocation_free.diagnostic.message_key != owning.diagnostic.message_key ||
      allocation_free.diagnostic.origin != owning.diagnostic.source_component) {
    return false;
  }

  const auto find_owning = [&](std::string_view key)
      -> const platform::DiagnosticArgument* {
    for (const auto& argument : owning.diagnostic.arguments) {
      if (argument.key == key) return &argument;
    }
    return nullptr;
  };
  for (std::size_t index = 0;
       index < allocation_free.diagnostic.argument_count; ++index) {
    const auto& argument = allocation_free.diagnostic.arguments[index];
    const auto* owning_argument = find_owning(argument.key);
    if (owning_argument == nullptr) return false;
    switch (argument.kind) {
      case dt::DatatypeBinaryDiagnosticArgumentKind::text: {
        const auto* value = owning_argument->text();
        if (value == nullptr || *value != argument.text) return false;
        break;
      }
      case dt::DatatypeBinaryDiagnosticArgumentKind::unsigned_integer: {
        const auto* value = owning_argument->text();
        if (value == nullptr || *value != std::to_string(argument.unsigned_integer))
          return false;
        break;
      }
      case dt::DatatypeBinaryDiagnosticArgumentKind::uuid: {
        const auto* value = owning_argument->uuid();
        if (value == nullptr || !SameUuidBytes(*value, argument.uuid))
          return false;
        break;
      }
      case dt::DatatypeBinaryDiagnosticArgumentKind::descriptor_reference: {
        const auto* value = owning_argument->uuid();
        const auto* generation =
            find_owning(std::string(argument.key) + "_generation");
        const auto* generation_text =
            generation == nullptr ? nullptr : generation->text();
        if (value == nullptr || !SameUuidBytes(*value, argument.uuid) ||
            generation_text == nullptr ||
            *generation_text != std::to_string(argument.generation)) {
          return false;
        }
        break;
      }
      case dt::DatatypeBinaryDiagnosticArgumentKind::none:
        return false;
    }
  }
  return true;
}

constexpr std::size_t kMaximumBytes = 16u * 1024u * 1024u;
constexpr platform::Uuid kDescriptorUuid{{
    0x2d,0x01,0,0,0x62,0x69,0x7e,0x61,0xb2,0x79,0,0,0,0,0,0}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x43}};
constexpr platform::Uuid kCodecUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x44}};
constexpr std::string_view kCodecId = "datatype.binary.octets.v1";
constexpr std::string_view kRepresentation = "exact_octets_without_text_conversion";
constexpr std::string_view kBinaryComponentBoundary = "SBDVAL01";

bool NoAllocDiagnosticHeader(
    const dt::DatatypeBinaryAllocationFreeViewResult& result,
    std::string_view code, std::string_view key, std::size_t argument_count) {
  return result.abi_version == 1 && result.diagnostic.abi_version == 1 &&
      result.diagnostic.status.code == result.status.code &&
      result.diagnostic.status.severity == result.status.severity &&
      result.diagnostic.status.subsystem == result.status.subsystem &&
      result.diagnostic.diagnostic_code == code &&
      result.diagnostic.message_key == key &&
      result.diagnostic.argument_count == argument_count &&
      result.diagnostic.origin == "core.datatypes.binary";
}

bool NoAllocTextArgument(const dt::DatatypeBinaryDiagnosticView& diagnostic,
                         std::size_t index, std::string_view key,
                         std::string_view value) {
  if (index >= diagnostic.argument_count) return false;
  const auto& argument = diagnostic.arguments[index];
  return argument.key == key &&
      argument.kind == dt::DatatypeBinaryDiagnosticArgumentKind::text &&
      argument.text == value;
}

bool NoAllocUnsignedArgument(
    const dt::DatatypeBinaryDiagnosticView& diagnostic, std::size_t index,
    std::string_view key, std::uint64_t value) {
  if (index >= diagnostic.argument_count) return false;
  const auto& argument = diagnostic.arguments[index];
  return argument.key == key &&
      argument.kind ==
          dt::DatatypeBinaryDiagnosticArgumentKind::unsigned_integer &&
      argument.unsigned_integer == value;
}

bool NoAllocDescriptorArgument(
    const dt::DatatypeBinaryDiagnosticView& diagnostic, std::size_t index) {
  if (index >= diagnostic.argument_count) return false;
  const auto& argument = diagnostic.arguments[index];
  return argument.key == "descriptor_ref" &&
      argument.kind ==
          dt::DatatypeBinaryDiagnosticArgumentKind::descriptor_reference &&
      SameUuidBytes(argument.uuid, kDescriptorUuid) && argument.generation == 1;
}

bool NoAllocFrameShape(
    const dt::DatatypeBinaryAllocationFreeViewResult& result,
    std::uint64_t offset, std::string_view reason) {
  return NoAllocDiagnosticHeader(result, "CTB.BINARY.FRAME_INVALID",
                                 "datatype.binary.frame_invalid", 3) &&
      NoAllocTextArgument(result.diagnostic, 0, "boundary",
                          kBinaryComponentBoundary) &&
      NoAllocUnsignedArgument(result.diagnostic, 1, "offset", offset) &&
      NoAllocTextArgument(result.diagnostic, 2, "reason", reason);
}

bool NoAllocIntegrityShape(
    const dt::DatatypeBinaryAllocationFreeViewResult& result,
    std::string_view reason) {
  return NoAllocDiagnosticHeader(result, "CTB.BINARY.INTEGRITY_FAILED",
                                 "datatype.binary.integrity_failed", 2) &&
      NoAllocTextArgument(result.diagnostic, 0, "boundary",
                          kBinaryComponentBoundary) &&
      NoAllocTextArgument(result.diagnostic, 1, "reason", reason);
}

bool NoAllocLengthShape(
    const dt::DatatypeBinaryAllocationFreeViewResult& result,
    std::uint64_t actual_bytes) {
  return NoAllocDiagnosticHeader(result, "CTB.BINARY.LENGTH_EXCEEDED",
                                 "datatype.binary.length_exceeded", 3) &&
      NoAllocUnsignedArgument(result.diagnostic, 0, "actual_bytes",
                              actual_bytes) &&
      NoAllocUnsignedArgument(result.diagnostic, 1, "maximum_bytes",
                              kMaximumBytes) &&
      NoAllocTextArgument(result.diagnostic, 2, "operation",
                          "binary_component_decode");
}

bool NoAllocNullStateShape(
    const dt::DatatypeBinaryAllocationFreeViewResult& result,
    std::uint64_t payload_bytes, std::string_view reason) {
  return NoAllocDiagnosticHeader(result, "DATATYPE.NULL_STATE.INVALID",
                                 "datatype.null_state_invalid", 4) &&
      NoAllocDescriptorArgument(result.diagnostic, 0) &&
      NoAllocTextArgument(result.diagnostic, 1, "boundary",
                          kBinaryComponentBoundary) &&
      NoAllocUnsignedArgument(result.diagnostic, 2, "payload_length",
                              payload_bytes) &&
      NoAllocTextArgument(result.diagnostic, 3, "reason", reason);
}

bool NoAllocNullNotAdmittedShape(
    const dt::DatatypeBinaryAllocationFreeViewResult& result) {
  return NoAllocDiagnosticHeader(result, "DATATYPE.NULL_NOT_ADMITTED",
                                 "datatype.null_not_admitted", 3) &&
      NoAllocDescriptorArgument(result.diagnostic, 0) &&
      NoAllocTextArgument(result.diagnostic, 1, "boundary",
                          kBinaryComponentBoundary) &&
      NoAllocTextArgument(result.diagnostic, 2, "reason",
                          "nonnullable_catalog_metadata_cell");
}

struct PolicyReceiptRow {
  platform::Uuid uuid;
  std::uint64_t generation;
};

constexpr std::array<PolicyReceiptRow, 12> kPolicyReceipt{{
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x7d,0xa5,0x9d,0x9d,0x83,0x9b,0xc8,0x1b,0x09,0xca}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x74,0x66,0x9a,0xdb,0xe2,0x54,0x64,0xc6,0x5b,0x6a}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x70,0x73,0xa5,0xb7,0x32,0x60,0x6c,0xd0,0x91,0xe6}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x73,0x7b,0xa4,0x82,0xe6,0x96,0xfe,0x71,0x41,0xf8}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x77,0x89,0xab,0x99,0xa5,0x86,0x87,0x7b,0x87,0x88}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x73,0xe3,0xa0,0xd2,0xfd,0xdb,0x11,0x1e,0x47,0xc6}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x7e,0x96,0xae,0x18,0xf1,0x72,0x9a,0xd5,0x03,0x6d}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x71,0xb4,0xaa,0xfd,0xca,0xfe,0x1d,0x1d,0x4c,0xf2}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x72,0x60,0x98,0xd9,0x1d,0x7d,0xcd,0x6b,0x8e,0x39}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x78,0x46,0xa0,0x9d,0x21,0x9c,0x19,0xfb,0xa6,0xff}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x74,0x47,0xbb,0x2f,0xda,0x9c,0x9a,0xd5,0xa9,0x4a}}, 1},
    {{{0x01,0xa0,0xfe,0xa5,0x8a,0x12,0x7f,0x05,0xa8,0x38,0xee,0x9a,0xe6,0x29,0xe7,0x66}}, 1},
}};

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
  const auto result = dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  return result.ok() ? result.descriptor
                     : scratchbird::engine::ExecutionTypeDescriptor{};
}

scratchbird::engine::ExecutionTypeDescriptor BinaryDescriptor() {
  return DescriptorFor(dt::CanonicalTypeId::binary);
}

dt::DatatypeOperationValue Present(std::string bytes = std::string{"\0\xff\x10\x80\x7f", 5}) {
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::binary,
                                   std::move(bytes), false};
  value.descriptor = BinaryDescriptor();
  return value;
}

dt::DatatypeOperationValue TypedNull() {
  auto descriptor = BinaryDescriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::binary, {}, true};
  value.descriptor = descriptor;
  return value;
}

std::string FromHex(std::string_view hex) {
  const auto nibble = [](char value) -> unsigned {
    return value <= '9' ? static_cast<unsigned>(value - '0')
                        : static_cast<unsigned>(value - 'a' + 10);
  };
  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    bytes.push_back(static_cast<char>((nibble(hex[index]) << 4u) |
                                     nibble(hex[index + 1])));
  }
  return bytes;
}

std::uint64_t OracleBinaryChecksum(const std::vector<platform::byte>& payload) {
  std::uint64_t value = 1469598103934665603ull;
  for (const auto byte : payload) {
    value ^= byte;
    value *= 1099511628211ull;
  }
  return value;
}

// SBDVAL01 is the existing normative structural component envelope. It does
// not provide the statement receipt needed by generic binary operations.
std::vector<platform::byte> OracleBinaryFrame(
    const std::vector<platform::byte>& payload, bool is_null = false) {
  std::vector<platform::byte> frame(32 + payload.size(), 0);
  const std::array<platform::byte, 8> magic{{'S','B','D','V','A','L','0','1'}};
  std::copy(magic.begin(), magic.end(), frame.begin());
  platform::StoreLittle32(frame.data() + 8,
      static_cast<std::uint32_t>(dt::CanonicalTypeId::binary));
  platform::StoreLittle16(frame.data() + 12, is_null ? 1 : 0);
  platform::StoreLittle16(frame.data() + 14, 32);
  platform::StoreLittle32(frame.data() + 16,
      static_cast<std::uint32_t>(payload.size()));
  platform::StoreLittle64(frame.data() + 24, OracleBinaryChecksum(payload));
  std::copy(payload.begin(), payload.end(), frame.begin() + 32);
  return frame;
}

void ExactIdentityCohortsAndLayout() {
  const auto descriptor = BinaryDescriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::binary),
        "base.binary execution descriptor has the exact catalog identity");

  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV1, 1, 1, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV2, 2, 2, kDescriptorUuid, 1).ok,
        "base.binary is absent from immutable d701 and d702 cohorts");

  struct Cohort { platform::Uuid snapshot; std::uint64_t generation; };
  const std::array cohorts{
      Cohort{dt::kDatatypeCohortV3, 3}, Cohort{dt::kDatatypeCohortV4, 4},
      Cohort{dt::kDatatypeCohortV5, 5}};
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
              admitted.row.canonical_binary_type_code == 301 &&
              admitted.row.canonical_value_minimum_bytes == 0 &&
              admitted.row.canonical_value_maximum_bytes == kMaximumBytes &&
              admitted.row.canonical_value_exact_bytes == 0 &&
              admitted.row.canonical_value_variable_width &&
              admitted.row.canonical_value_exact_zero_is_width_marker &&
              admitted.row.canonical_byte_order == "byte_sequence" &&
              admitted.row.canonical_representation == kRepresentation &&
              admitted.row.empty_value_distinct_from_sql_null &&
              admitted.row.sql_null_requires_zero_payload &&
              admitted.row.variable_width_storage_without_truncation,
          "d703 introduction and d704/d705 historical inheritance preserve exact binary tuple");
  }

  const auto current = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV10, 10, 10, kDescriptorUuid, 1);
  Check(current.ok &&
            dt::IsExactCanonicalBinaryTypeCodecIdentityV3(current.row) &&
            SameUuidBytes(current.row.legacy_fields.type_uuid, kTypeUuid) &&
            SameUuidBytes(current.row.legacy_fields.codec_uuid, kCodecUuid) &&
            current.row.legacy_fields.codec_id == kCodecId &&
            current.row.legacy_fields.catalog_generation == 10 &&
            current.row.legacy_fields.registry_generation == 10 &&
            SameUuidBytes(current.row.descriptor_policy.uuid,
                          kPolicyReceipt[0].uuid) &&
            current.row.descriptor_policy.generation == 1 &&
            SameUuidBytes(current.row.canonicalization_policy.uuid,
                          kPolicyReceipt[1].uuid) &&
            current.row.canonicalization_policy.generation == 1 &&
            SameUuidBytes(current.row.ordering_policy.uuid,
                          kPolicyReceipt[2].uuid) &&
            current.row.ordering_policy.generation == 1 &&
            SameUuidBytes(current.row.hash_policy.uuid,
                          kPolicyReceipt[3].uuid) &&
            current.row.hash_policy.generation == 1 &&
            current.row.operation_policy.uuid.is_nil() &&
            current.row.operation_policy.generation == 0,
        "D710 V3 current authority preserves the exact binary tuple and policies");
  const auto historical = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV6, 6, 6, kDescriptorUuid, 1);
  Check(historical.ok &&
            !dt::IsExactCanonicalBinaryTypeCodecIdentityV3(historical.row) &&
            SameUuidBytes(historical.row.legacy_fields.type_uuid, kTypeUuid) &&
            SameUuidBytes(historical.row.legacy_fields.codec_uuid, kCodecUuid) &&
            historical.row.legacy_fields.codec_id == kCodecId &&
            historical.row.legacy_fields.catalog_generation == 6 &&
            historical.row.legacy_fields.registry_generation == 6 &&
            SameUuidBytes(historical.row.descriptor_policy.uuid,
                          kPolicyReceipt[0].uuid) &&
            SameUuidBytes(historical.row.canonicalization_policy.uuid,
                          kPolicyReceipt[1].uuid) &&
            SameUuidBytes(historical.row.ordering_policy.uuid,
                          kPolicyReceipt[2].uuid) &&
            SameUuidBytes(historical.row.hash_policy.uuid,
                          kPolicyReceipt[3].uuid),
        "d706 exact binary tuple remains available only as history");

  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 4, 5, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 4, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 2).ok,
        "mixed cohorts and descriptor generations refuse binary identity");

  dt::DatatypeStorageIdentityV1 storage;
  const auto layout = dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::binary);
  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV8, 8, 8, kDescriptorUuid, 1, &storage) &&
            SameUuidBytes(storage.type_uuid, kTypeUuid) &&
            storage.codec.has_value() &&
            SameUuidBytes(storage.codec->codec_uuid, kCodecUuid) &&
            storage.codec->codec_id == kCodecId && layout.ok() &&
            layout.layout.storage_class == dt::DatatypeStorageClass::inline_variable &&
            layout.layout.encoding == dt::DatatypeBinaryEncoding::opaque_bytes &&
            layout.layout.inline_bytes == 0 && layout.layout.alignment_bytes == 1 &&
            layout.layout.nullable && !layout.layout.requires_charset &&
            !layout.layout.requires_collation,
        "base.binary storage identity and layout preserve variable opaque octets");
}

void ExactOctetsAndLowerCodecs() {
  std::string all_octets;
  for (unsigned value = 0; value <= 255; ++value)
    all_octets.push_back(static_cast<char>(value));
  const std::array carriers{
      std::string{}, std::string{"\0", 1},
      std::string{"\0\xff\x10\x80\x7f", 5}, all_octets};
  for (const auto& carrier : carriers) {
    const auto payload = Payload(carrier);
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::binary, false, false, payload});
    const auto decoded_binary = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    Check(binary.ok() && binary.encoded == OracleBinaryFrame(payload) &&
              decoded_binary.ok() &&
              decoded_binary.value.type_id == dt::CanonicalTypeId::binary &&
              !decoded_binary.value.is_null &&
              !decoded_binary.value.payload_is_toast_reference &&
              decoded_binary.value.payload == payload,
          "SBDVAL01 preserves exact binary octets including empty PRESENT");

    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::binary,
         dt::DatatypePhysicalValueState::value, payload});
    const auto decoded_physical = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(physical.ok() && decoded_physical.ok() &&
              decoded_physical.value.type_id == dt::CanonicalTypeId::binary &&
              decoded_physical.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              decoded_physical.value.payload == payload,
          "physical component roundtrip preserves state and exact binary octets");
  }

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::binary, true, false, {}});
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::binary,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(binary_null.ok() && binary_null.encoded == OracleBinaryFrame({}, true) &&
            physical_null.ok() &&
            binary_null.encoded != OracleBinaryFrame({}, false) &&
            !dt::EncodeDatatypeBinaryValue(
                {dt::CanonicalTypeId::binary, true, false, {0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
                {dt::CanonicalTypeId::binary,
                 dt::DatatypePhysicalValueState::sql_null, {0}}).ok(),
        "empty PRESENT is distinct from clean typed NULL and dirty NULL refuses");

  for (const auto state : {dt::DatatypePhysicalValueState::overflow_root,
                           dt::DatatypePhysicalValueState::overflow_chunk,
                           dt::DatatypePhysicalValueState::locator_handle,
                           dt::DatatypePhysicalValueState::opaque_handle,
                           dt::DatatypePhysicalValueState::protected_chunk_root}) {
    const auto rejected = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::binary, state, {1, 2, 3}});
    Check(RejectedAs(rejected, "CTB.BINARY.FRAME_INVALID") &&
              rejected.bytes.empty(),
          "base.binary component refuses locator/overflow value states");
  }
  const auto toast = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::binary, false, true, {1, 2, 3}});
  Check(RejectedAs(toast, "CTB.BINARY.FRAME_INVALID") && toast.encoded.empty(),
        "base.binary component never substitutes a TOAST reference for VALUE octets");

  const auto null_with_unreadable_payload = dt::ValidateDatatypeBinaryValueView(
      {dt::CanonicalTypeId::binary, true, false, nullptr, 1});
  const auto null_with_toast_state = dt::ValidateDatatypeBinaryValueView(
      {dt::CanonicalTypeId::binary, true, true, nullptr, 0});
  Check(RejectedAs(null_with_unreadable_payload,
                   "DATATYPE.NULL_STATE.INVALID") &&
            RejectedAs(null_with_toast_state,
                       "DATATYPE.NULL_STATE.INVALID") &&
            null_with_unreadable_payload.bytes_written == 0 &&
            null_with_toast_state.bytes_written == 0,
        "binary SQL_NULL state rejects payload and TOAST before pointer validation");

  const platform::byte inaccessible_if_touched = 0x5a;
  const auto oversized_view = dt::ValidateDatatypeBinaryValueView(
      {dt::CanonicalTypeId::binary, false, false, &inaccessible_if_touched,
       kMaximumBytes + 1});
  Check(RejectedAs(oversized_view, "CTB.BINARY.LENGTH_EXCEEDED") &&
            oversized_view.bytes_written == 0,
        "borrowed 16MiB+1 view refuses by declared length before payload access");

  auto unknown_flags = OracleBinaryFrame({0});
  platform::StoreLittle16(unknown_flags.data() + 12, 0x8000u);
  const auto unknown_flags_result =
      dt::DecodeDatatypeBinaryValue(unknown_flags);
  Check(!unknown_flags_result.ok() &&
            unknown_flags_result.value.payload.empty(),
        "SBDVAL01 unknown flags refuse without payload publication");

  std::vector<platform::byte> maximum(kMaximumBytes, 0xff);
  const auto max_binary = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::binary, false, false, maximum});
  const auto max_physical = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::binary,
       dt::DatatypePhysicalValueState::value, maximum});
  Check(max_binary.ok() && max_binary.value.payload == maximum &&
            max_binary.encoded.size() == maximum.size() + 32 &&
            max_physical.ok() && max_physical.value.payload == maximum,
        "exact 16MiB all-ff maximum remains a base.binary VALUE");
  maximum.push_back(0xff);
  const auto too_large_binary = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::binary, false, false, maximum});
  const auto too_large_physical = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::binary,
       dt::DatatypePhysicalValueState::value, maximum});
  Check(RejectedAs(too_large_binary, "CTB.BINARY.LENGTH_EXCEEDED") &&
            too_large_binary.encoded.empty() &&
            too_large_binary.value.payload.empty() &&
            RejectedAs(too_large_physical, "CTB.BINARY.LENGTH_EXCEEDED") &&
            too_large_physical.bytes.empty() &&
            too_large_physical.value.payload.empty(),
        "16MiB+1 refuses atomically without result publication");
}

void AllocationFreeBorrowedViews() {
  const std::array<platform::byte, 5> payload{{0x00, 0xff, 0x10, 0x80, 0x7f}};
  const dt::DatatypeBinaryDiagnosticContextV1 diagnostic_context{
      kDescriptorUuid, 1};

  const auto empty = ValidateWithHeapDenied(
      {dt::CanonicalTypeId::binary, false, false, nullptr, 0},
      diagnostic_context);
  const auto empty_attempts = binary_allocation_denial::attempts;
  Check(empty.ok() && empty_attempts == 0 && empty.abi_version == 1 &&
            empty.diagnostic.abi_version == 1 &&
            empty.value.type_id == dt::CanonicalTypeId::binary &&
            !empty.value.is_null && empty.value.payload_data == nullptr &&
            empty.value.payload_bytes == 0,
        "allocation-free validator admits empty PRESENT without heap use");

  const auto present = ValidateWithHeapDenied(
      {dt::CanonicalTypeId::binary, false, false,
       payload.data(), payload.size()}, diagnostic_context);
  const auto present_attempts = binary_allocation_denial::attempts;
  Check(present.ok() && present_attempts == 0 &&
            present.value.payload_data == payload.data() &&
            present.value.payload_bytes == payload.size() &&
            std::equal(payload.begin(), payload.end(),
                       present.value.payload_data),
        "allocation-free validator returns the exact borrowed payload pointer");

  const platform::byte maximum_sentinel = 0xa5;
  const auto maximum = ValidateWithHeapDenied(
      {dt::CanonicalTypeId::binary, false, false, &maximum_sentinel,
       kMaximumBytes}, diagnostic_context);
  const auto maximum_attempts = binary_allocation_denial::attempts;
  Check(maximum.ok() && maximum_attempts == 0 &&
            maximum.value.payload_data == &maximum_sentinel &&
            maximum.value.payload_bytes == kMaximumBytes,
        "allocation-free validator admits the maximum declared borrowed span without reading it");

  const auto typed_null = ValidateWithHeapDenied(
      {dt::CanonicalTypeId::binary, true, false, nullptr, 0},
      diagnostic_context);
  const auto typed_null_attempts = binary_allocation_denial::attempts;
  Check(!typed_null.ok() && typed_null_attempts == 0 &&
            NoAllocNullNotAdmittedShape(typed_null) &&
            AllocationFreeFailurePublishesNothing(typed_null),
        "allocation-free direct binary validator refuses typed SQL_NULL with exact descriptor context");

  const auto empty_frame = OracleBinaryFrame({});
  const auto payload_frame = OracleBinaryFrame(
      {payload.begin(), payload.end()});
  const auto null_frame = OracleBinaryFrame({}, true);
  const auto decoded_empty = DecodeWithHeapDenied(
      empty_frame.data(), empty_frame.size(), diagnostic_context);
  const auto decoded_empty_attempts = binary_allocation_denial::attempts;
  const auto decoded_payload = DecodeWithHeapDenied(
      payload_frame.data(), payload_frame.size(), diagnostic_context);
  const auto decoded_payload_attempts = binary_allocation_denial::attempts;
  const auto decoded_null = DecodeWithHeapDenied(
      null_frame.data(), null_frame.size(), diagnostic_context);
  const auto decoded_null_attempts = binary_allocation_denial::attempts;
  Check(decoded_empty.ok() && decoded_empty_attempts == 0 &&
            decoded_empty.value.payload_data == empty_frame.data() + 32 &&
            decoded_empty.value.payload_bytes == 0 &&
            decoded_payload.ok() && decoded_payload_attempts == 0 &&
            decoded_payload.value.payload_data == payload_frame.data() + 32 &&
            decoded_payload.value.payload_bytes == payload.size() &&
            !decoded_null.ok() && decoded_null_attempts == 0 &&
            NoAllocNullNotAdmittedShape(decoded_null) &&
            AllocationFreeFailurePublishesNothing(decoded_null),
        "allocation-free decoder borrows PRESENT bytes and refuses typed SQL_NULL in the direct profile");
  const auto owning_decoded_null = dt::DecodeDatatypeBinaryValue(null_frame);
  Check(owning_decoded_null.ok() && owning_decoded_null.value.is_null &&
            owning_decoded_null.value.payload.empty(),
        "owning general binary decoder retains clean typed SQL_NULL admission");

  std::vector<platform::byte> maximum_payload(kMaximumBytes, 0x5a);
  const auto maximum_frame = OracleBinaryFrame(maximum_payload);
  const auto decoded_maximum = DecodeWithHeapDenied(
      maximum_frame.data(), maximum_frame.size(), diagnostic_context);
  const auto decoded_maximum_attempts = binary_allocation_denial::attempts;
  Check(decoded_maximum.ok() && decoded_maximum_attempts == 0 &&
            decoded_maximum.value.payload_data == maximum_frame.data() + 32 &&
            decoded_maximum.value.payload_bytes == kMaximumBytes &&
            decoded_maximum.value.payload_data[0] == 0x5a &&
            decoded_maximum.value.payload_data[kMaximumBytes - 1] == 0x5a,
        "allocation-free decoder validates and borrows the exact 16MiB boundary");

  std::array<platform::byte, 32> short_source{};
  for (std::size_t short_bytes = 0; short_bytes < short_source.size();
       ++short_bytes) {
    const auto short_result = DecodeWithHeapDenied(
        short_source.data(), short_bytes, diagnostic_context);
    const auto short_attempts = binary_allocation_denial::attempts;
    Check(!short_result.ok() && short_attempts == 0 &&
              NoAllocFrameShape(short_result, short_bytes,
                                "source_null_or_truncated_header") &&
              AllocationFreeFailurePublishesNothing(short_result),
          "allocation-free decoder refuses every non-null 0..31-byte frame before access beyond its extent");
  }
  for (const std::size_t null_extent : {std::size_t{0}, std::size_t{32}}) {
    const auto null_source = DecodeWithHeapDenied(
        nullptr, null_extent, diagnostic_context);
    const auto null_attempts = binary_allocation_denial::attempts;
    Check(!null_source.ok() && null_attempts == 0 &&
              NoAllocFrameShape(null_source, null_extent,
                                "source_null_or_truncated_header") &&
              AllocationFreeFailurePublishesNothing(null_source),
          "allocation-free decoder refuses a null source before dereference");
  }
  const platform::byte extent_sentinel = 0x53;
  const auto unrepresentable_extent = DecodeWithHeapDenied(
      &extent_sentinel, std::numeric_limits<std::size_t>::max(),
      diagnostic_context);
  const auto unrepresentable_attempts = binary_allocation_denial::attempts;
  Check(!unrepresentable_extent.ok() && unrepresentable_attempts == 0 &&
            NoAllocFrameShape(unrepresentable_extent, 0,
                              "source_extent_unrepresentable") &&
            AllocationFreeFailurePublishesNothing(unrepresentable_extent),
        "allocation-free decoder rejects SIZE_MAX extent before dereferencing its sentinel");

  const auto check_frame_precedence = [&](const std::vector<platform::byte>& frame,
                                          std::uint64_t offset,
                                          std::string_view reason,
                                          const std::string& message) {
    const auto result = DecodeWithHeapDenied(
        frame.data(), frame.size(), diagnostic_context);
    const auto attempts = binary_allocation_denial::attempts;
    Check(!result.ok() && attempts == 0 &&
              NoAllocFrameShape(result, offset, reason) &&
              AllocationFreeFailurePublishesNothing(result),
          message);
  };

  auto magic_precedence = payload_frame;
  magic_precedence[0] ^= 1u;
  platform::StoreLittle16(magic_precedence.data() + 14, 31);
  platform::StoreLittle32(
      magic_precedence.data() + 8,
      static_cast<std::uint32_t>(dt::CanonicalTypeId::character));
  platform::StoreLittle16(magic_precedence.data() + 12, 0x8003u);
  magic_precedence[24] ^= 1u;
  check_frame_precedence(
      magic_precedence, 0, "bad_magic",
      "bad magic precedes simultaneous header/type/flags/state/checksum defects");

  auto header_precedence = payload_frame;
  platform::StoreLittle16(header_precedence.data() + 14, 31);
  platform::StoreLittle32(
      header_precedence.data() + 8,
      static_cast<std::uint32_t>(dt::CanonicalTypeId::character));
  platform::StoreLittle16(header_precedence.data() + 12, 0x8003u);
  header_precedence[24] ^= 1u;
  check_frame_precedence(
      header_precedence, 14, "bad_header_size",
      "bad header precedes simultaneous type/flags/state/checksum defects");

  auto extent_precedence = payload_frame;
  platform::StoreLittle32(
      extent_precedence.data() + 16,
      platform::LoadLittle32(extent_precedence.data() + 16) + 1);
  platform::StoreLittle32(
      extent_precedence.data() + 8,
      static_cast<std::uint32_t>(dt::CanonicalTypeId::character));
  platform::StoreLittle16(extent_precedence.data() + 12, 0x8003u);
  extent_precedence[24] ^= 1u;
  check_frame_precedence(
      extent_precedence, 16, "payload_extent_mismatch_or_trailing",
      "declared extent precedes simultaneous type/flags/state/checksum defects");

  auto flags_precedence = payload_frame;
  platform::StoreLittle16(flags_precedence.data() + 12, 0x8001u);
  flags_precedence[24] ^= 1u;
  check_frame_precedence(
      flags_precedence, 13, "unknown_flags_or_reserved_nonzero",
      "unknown flags precede simultaneous dirty-NULL and checksum defects");

  auto reserved_precedence = payload_frame;
  platform::StoreLittle16(reserved_precedence.data() + 12, 1);
  reserved_precedence[20] = 1;
  reserved_precedence[24] ^= 1u;
  check_frame_precedence(
      reserved_precedence, 20, "unknown_flags_or_reserved_nonzero",
      "reserved bytes precede simultaneous dirty-NULL and checksum defects");

  auto toast_checksum_precedence = payload_frame;
  platform::StoreLittle16(toast_checksum_precedence.data() + 12, 2);
  toast_checksum_precedence[24] ^= 1u;
  check_frame_precedence(
      toast_checksum_precedence, 12, "binary_reference_state_not_admitted",
      "TOAST/reference state precedes a simultaneous checksum defect");

  maximum_payload.push_back(0x5a);
  auto overlength_bad_checksum = OracleBinaryFrame(maximum_payload);
  overlength_bad_checksum[24] ^= 1u;
  const auto overlength_decode = DecodeWithHeapDenied(
      overlength_bad_checksum.data(), overlength_bad_checksum.size(),
      diagnostic_context);
  const auto overlength_decode_attempts = binary_allocation_denial::attempts;
  Check(!overlength_decode.ok() && overlength_decode_attempts == 0 &&
            NoAllocLengthShape(overlength_decode, kMaximumBytes + 1) &&
            AllocationFreeFailurePublishesNothing(overlength_decode),
        "complete 16MiB+1 frame refuses for length before a bad checksum");

  struct MalformedFrame {
    const char* name;
    const char* diagnostic_code;
    std::uint64_t diagnostic_offset;
    const char* diagnostic_reason;
    std::vector<platform::byte> bytes;
    bool comparable_to_owning;
  };
  std::vector<MalformedFrame> malformed;
  auto add_mutation = [&](const char* name, const char* diagnostic_code,
                          std::uint64_t diagnostic_offset,
                          const char* diagnostic_reason, auto mutation,
                          bool comparable = true) {
    auto bytes = payload_frame;
    mutation(&bytes);
    malformed.push_back({name, diagnostic_code, diagnostic_offset,
                         diagnostic_reason, std::move(bytes), comparable});
  };
  add_mutation("magic", "CTB.BINARY.FRAME_INVALID", 0, "bad_magic",
               [](auto* bytes) { (*bytes)[0] ^= 1u; });
  add_mutation("header", "CTB.BINARY.FRAME_INVALID", 14, "bad_header_size",
               [](auto* bytes) {
                 platform::StoreLittle16(bytes->data() + 14, 31);
               });
  add_mutation("flags", "CTB.BINARY.FRAME_INVALID", 13,
               "unknown_flags_or_reserved_nonzero", [](auto* bytes) {
                 platform::StoreLittle16(bytes->data() + 12, 0x8000u);
               });
  add_mutation("reserved", "CTB.BINARY.FRAME_INVALID", 20,
               "unknown_flags_or_reserved_nonzero",
               [](auto* bytes) { (*bytes)[20] = 1; });
  add_mutation("type", "CTB.BINARY.FRAME_INVALID", 8,
               "expected_binary_type_code_301", [](auto* bytes) {
                 platform::StoreLittle32(
                     bytes->data() + 8,
                     static_cast<std::uint32_t>(
                         dt::CanonicalTypeId::character));
               }, false);
  add_mutation("state", "DATATYPE.NULL_STATE.INVALID", 0,
               "null_payload_or_reference_state", [](auto* bytes) {
                 platform::StoreLittle16(bytes->data() + 12, 1);
               });
  add_mutation("length", "CTB.BINARY.FRAME_INVALID", 16,
               "payload_extent_mismatch_or_trailing", [](auto* bytes) {
                 platform::StoreLittle32(
                     bytes->data() + 16,
                     platform::LoadLittle32(bytes->data() + 16) + 1);
               });
  add_mutation("checksum", "CTB.BINARY.INTEGRITY_FAILED", 0,
               "payload_checksum_mismatch",
               [](auto* bytes) { (*bytes)[24] ^= 1u; });

  for (const auto& fixture : malformed) {
    const auto owning = fixture.comparable_to_owning
        ? dt::DecodeDatatypeBinaryValue(fixture.bytes)
        : dt::DatatypeBinaryResult{};
    const auto noalloc = DecodeWithHeapDenied(
        fixture.bytes.data(), fixture.bytes.size(), diagnostic_context);
    const auto allocation_attempts = binary_allocation_denial::attempts;
    const bool diagnostic_correct = fixture.comparable_to_owning
        ? !owning.ok() && ExactDiagnosticParity(noalloc, owning)
        : true;
    const bool diagnostic_shape =
        std::string_view(fixture.diagnostic_code) ==
                "CTB.BINARY.FRAME_INVALID"
            ? NoAllocFrameShape(noalloc, fixture.diagnostic_offset,
                                fixture.diagnostic_reason)
            : std::string_view(fixture.diagnostic_code) ==
                      "CTB.BINARY.INTEGRITY_FAILED"
                ? NoAllocIntegrityShape(noalloc, fixture.diagnostic_reason)
                : NoAllocNullStateShape(noalloc, payload.size(),
                                        fixture.diagnostic_reason);
    Check(!noalloc.ok() && allocation_attempts == 0 && diagnostic_correct &&
              diagnostic_shape &&
              RejectedAs(noalloc, fixture.diagnostic_code) &&
              AllocationFreeFailurePublishesNothing(noalloc),
          std::string("allocation-free decoder rejects malformed ") +
              fixture.name + " with no heap use or partial value");
  }

  const auto null_payload_noalloc = ValidateWithHeapDenied(
      {dt::CanonicalTypeId::binary, true, false, nullptr, 1},
      diagnostic_context);
  const auto null_payload_attempts = binary_allocation_denial::attempts;
  const auto null_payload_owning = dt::ValidateDatatypeBinaryValue(
      {dt::CanonicalTypeId::binary, true, false, {0}});
  const auto toast_noalloc = ValidateWithHeapDenied(
      {dt::CanonicalTypeId::binary, false, true,
       payload.data(), payload.size()}, diagnostic_context);
  const auto toast_attempts = binary_allocation_denial::attempts;
  const auto toast_owning = dt::ValidateDatatypeBinaryValue(
      {dt::CanonicalTypeId::binary, false, true,
       {payload.begin(), payload.end()}});
  Check(null_payload_attempts == 0 && toast_attempts == 0 &&
            ExactDiagnosticParity(null_payload_noalloc,
                                  null_payload_owning) &&
            ExactDiagnosticParity(toast_noalloc, toast_owning) &&
            NoAllocNullStateShape(null_payload_noalloc, 1,
                                  "null_payload_or_reference_state") &&
            NoAllocFrameShape(toast_noalloc, 12,
                              "binary_reference_state_not_admitted") &&
            AllocationFreeFailurePublishesNothing(null_payload_noalloc) &&
            AllocationFreeFailurePublishesNothing(toast_noalloc),
        "allocation-free validation has owning diagnostic parity and no partial value");

  const auto too_large_noalloc = ValidateWithHeapDenied(
      {dt::CanonicalTypeId::binary, false, false, &maximum_sentinel,
       kMaximumBytes + 1}, diagnostic_context);
  const auto too_large_attempts = binary_allocation_denial::attempts;
  Check(!too_large_noalloc.ok() && too_large_attempts == 0 &&
            NoAllocLengthShape(too_large_noalloc, kMaximumBytes + 1) &&
            AllocationFreeFailurePublishesNothing(too_large_noalloc),
        "allocation-free validator refuses 16MiB+1 with exact typed length diagnostic");
}

void LengthNullDescriptorAndSerializationRules() {
  auto uint64_descriptor = DescriptorFor(dt::CanonicalTypeId::uint64);
  for (const auto& [bytes, expected] :
       std::array<std::pair<std::string, std::uint64_t>, 2>{{
           {std::string{}, 0}, {std::string{"\0\xff\x10", 3}, 3}}}) {
    for (const std::string_view field : {"length", "octet_length"}) {
      dt::DatatypeExtractRequest request;
      request.value = Present(bytes);
      request.field = std::string(field);
      request.result_descriptor = uint64_descriptor;
      const auto result = dt::ExtractDatatypeField(request);
      std::uint64_t decoded = 0;
      Check(result.ok() && !result.value.is_null &&
                result.value.type_id == dt::CanonicalTypeId::uint64 &&
                dt::DecodeCanonicalUint64Value(result.value.encoded_value,
                                               &decoded) &&
                decoded == expected,
            "binary intrinsic length is exact octet length");
    }
  }

  uint64_descriptor.nullable_allowed = true;
  dt::DatatypeExtractRequest null_length;
  null_length.value = TypedNull();
  null_length.field = "octet_length";
  null_length.result_descriptor = uint64_descriptor;
  const auto null_result = dt::ExtractDatatypeField(null_length);
  Check(null_result.ok() && null_result.value.is_null &&
            null_result.value.type_id == dt::CanonicalTypeId::uint64 &&
            null_result.value.encoded_value.empty(),
        "binary length propagates typed NULL without manufacturing zero");

  const auto present = Present("abc");
  auto descriptorless = present;
  descriptorless.descriptor = {};
  auto label_only = descriptorless;
  label_only.descriptor.stable_name = "binary";
  auto wrong = present;
  wrong.descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  auto stale = present;
  ++stale.descriptor.descriptor_epoch;
  for (const auto& invalid : {descriptorless, label_only, wrong, stale}) {
    const auto serialized = dt::SerializeDatatypeValue({invalid});
    Check(RejectedAs(serialized, "CTB.BINARY.DESCRIPTOR_INVALID") &&
              serialized.serialized_value.empty(),
          "generic binary serialization rejects missing, label-only and wrong descriptors");
  }

  for (const auto& value : {present, TypedNull()}) {
    const auto serialized = dt::SerializeDatatypeValue({value});
    Check(RejectedAs(serialized,
                     "CTB.BINARY.SERIALIZATION_PROFILE_MISSING") &&
              serialized.serialized_value.empty(),
          "generic SBDV1 binary serialization refuses without live receipt profile");
  }
  for (const auto frame : {
           std::string{"SBDV1;type=binary;state=value;payload=616263"},
           std::string{"SBDV1;type=binary;state=null;payload="}}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::binary;
    request.expected_descriptor = frame.find("state=null") == std::string::npos
        ? BinaryDescriptor() : TypedNull().descriptor;
    request.serialized_value = frame;
    const auto result = dt::DeserializeDatatypeValue(request);
    Check(RejectedAs(result,
                     "CTB.BINARY.SERIALIZATION_PROFILE_MISSING") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty(),
          "generic SBDV1 binary deserialization refuses without live receipt profile");
  }
  dt::DatatypeDeserializationRequest missing;
  missing.expected_type_id = dt::CanonicalTypeId::binary;
  missing.serialized_value = "SBDV1;type=binary;state=value;payload=00";
  const auto missing_result = dt::DeserializeDatatypeValue(missing);
  Check(RejectedAs(missing_result, "CTB.BINARY.DESCRIPTOR_INVALID") &&
            missing_result.value.encoded_value.empty(),
        "generic binary deserialization rejects missing descriptor before policy");

  const std::array malformed_frames{
      std::string{"NOTSBDV1;type=binary;state=value;payload=00"},
      std::string{"SBDV1;type=binary;state=value"},
      std::string{"SBDV1;type=binary;state=value;payload=00;payload=00"},
      std::string{"SBDV1;type=binary;state=value;payload=00;unknown=x"},
      std::string{"SBDV1;type=binary;state=value;payload=00;"},
      std::string{"SBDV1;type=binary;state=missing;payload=00"},
      std::string{"SBDV1;type=binary;state=value;payload=0g"}};
  for (const auto& frame : malformed_frames) {
    dt::DatatypeDeserializationRequest malformed;
    malformed.expected_type_id = dt::CanonicalTypeId::binary;
    malformed.expected_descriptor = BinaryDescriptor();
    malformed.serialized_value = frame;
    const auto malformed_result = dt::DeserializeDatatypeValue(malformed);
    Check(RejectedAs(malformed_result, "CTB.BINARY.FRAME_INVALID") &&
              malformed_result.value.type_id == dt::CanonicalTypeId::unknown &&
              !malformed_result.value.is_null &&
              malformed_result.value.encoded_value.empty(),
          "malformed generic binary SBDV1 frame refuses without value publication");
  }

  dt::DatatypeDeserializationRequest dirty_null_decode;
  dirty_null_decode.expected_type_id = dt::CanonicalTypeId::binary;
  dirty_null_decode.expected_descriptor = TypedNull().descriptor;
  dirty_null_decode.serialized_value =
      "SBDV1;type=binary;state=null;payload=00";
  const auto dirty_null_decode_result =
      dt::DeserializeDatatypeValue(dirty_null_decode);
  Check(RejectedAs(dirty_null_decode_result, "DATATYPE.NULL_STATE.INVALID") &&
            dirty_null_decode_result.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            dirty_null_decode_result.value.encoded_value.empty(),
        "generic binary dirty NULL refuses before missing receipt policy");

  dt::DatatypeDeserializationRequest nonnullable_null;
  nonnullable_null.expected_type_id = dt::CanonicalTypeId::binary;
  nonnullable_null.expected_descriptor = BinaryDescriptor();
  nonnullable_null.expected_descriptor.nullable_allowed = false;
  nonnullable_null.serialized_value =
      "SBDV1;type=binary;state=null;payload=";
  const auto nonnullable_null_result =
      dt::DeserializeDatatypeValue(nonnullable_null);
  Check(RejectedAs(nonnullable_null_result, "DATATYPE.NULL_NOT_ADMITTED") &&
            nonnullable_null_result.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            nonnullable_null_result.value.encoded_value.empty(),
        "generic binary nonnullable NULL refuses before missing receipt policy");

  dt::DatatypeDeserializationRequest overlength;
  overlength.expected_type_id = dt::CanonicalTypeId::binary;
  overlength.expected_descriptor = BinaryDescriptor();
  overlength.serialized_value =
      "SBDV1;type=binary;state=value;payload=" +
      std::string(2u * (kMaximumBytes + 1u), '0');
  const auto overlength_result = dt::DeserializeDatatypeValue(overlength);
  Check(RejectedAs(overlength_result, "CTB.BINARY.LENGTH_EXCEEDED") &&
            overlength_result.value.type_id == dt::CanonicalTypeId::unknown &&
            overlength_result.value.encoded_value.empty(),
        "generic binary declared overlength refuses before decode and missing receipt policy");

  auto dirty_null = TypedNull();
  dirty_null.encoded_value = "x";
  const auto dirty_serialized = dt::SerializeDatatypeValue({dirty_null});
  Check(RejectedAs(dirty_serialized, "DATATYPE.NULL_STATE.INVALID") &&
            dirty_serialized.serialized_value.empty(),
        "dirty typed binary NULL refuses before serialization policy");
}

void EqualityOrderingHashAndSort() {
  struct OrderedPair { std::string left; std::string right; int sign; };
  const std::array pairs{
      OrderedPair{"", std::string{"\0", 1}, -1},
      OrderedPair{std::string{"\0", 1}, std::string{"\0\0", 2}, -1},
      OrderedPair{std::string{"\x7f", 1}, std::string{"\x80", 1}, -1},
      OrderedPair{std::string{"\xff", 1}, std::string{"\0", 1}, 1},
      OrderedPair{std::string{"\0\xff", 2}, std::string{"\0\xff", 2}, 0}};
  for (const auto& pair : pairs) {
    const auto result = dt::CompareDatatypeValues(
        {Present(pair.left), Present(pair.right)});
    Check(result.ok() && result.comparison == pair.sign,
          "binary compare uses unsigned lexicographic shorter-prefix-first order");
  }

  auto nullable_present = Present("same-cohort");
  auto nonnullable_present = nullable_present;
  nonnullable_present.descriptor.nullable_allowed = false;
  const auto nullable_cross_compare = dt::CompareDatatypeValues(
      {nullable_present, nonnullable_present});
  Check(nullable_cross_compare.ok() &&
            nullable_cross_compare.comparison == 0,
        "binary PRESENT comparison treats nullability as a slot constraint, not cohort identity");

  const auto typed_null = TypedNull();
  const auto empty = Present("");
  const auto first = dt::CompareDatatypeValues(
      {typed_null, empty, dt::DatatypeNullOrdering::nulls_first});
  const auto last = dt::CompareDatatypeValues(
      {typed_null, empty, dt::DatatypeNullOrdering::nulls_last});
  Check(first.ok() && first.comparison < 0 && last.ok() && last.comparison > 0,
        "binary comparison orders typed NULL without interpreting a payload");

  auto mismatched = Present("x");
  ++mismatched.descriptor.descriptor_epoch;
  const auto refused = dt::CompareDatatypeValues({Present("x"), mismatched});
  Check(RejectedAs(refused, "CTB.BINARY.DESCRIPTOR_INVALID") &&
            refused.comparison == 0,
        "binary comparison refuses crossed descriptors without result publication");

  struct HashVector { std::string bytes; const char* expected; };
  const std::array hashes{
      HashVector{"", "93a3d65865711ffac91b9cd4e5cc9054b24a73d8967a2ed6f067f1704df40d24"},
      HashVector{std::string{"\0", 1}, "7868bd65d5e0aea4d6a1b4a87fd32f40ea85611177685ffa4f4f62bda0a45ec5"},
      HashVector{std::string{"\1", 1}, "6ee6bda41d4dd765b493f4513ed9e9e7138088e529b9833cd11c809ddb379a90"},
      HashVector{std::string{"\xff", 1}, "f38b6359c5cf3b67305372a2f4fc318b07af0f87a2a924dbae064cb78742467b"},
      HashVector{"abc", "40549655547cdbc3832ff662b16e00beac3a18e78326335d4245aab34994e1e2"}};
  for (const auto& vector : hashes) {
    const auto result = dt::HashDatatypeValue({Present(vector.bytes)});
    Check(result.ok() && result.stable_hash_hex == vector.expected,
          "binary hash matches independent cohort-qualified SHA-256 vector");
  }
  const auto null_hash = dt::HashDatatypeValue({typed_null});
  Check(null_hash.ok() && null_hash.stable_hash_hex ==
            "b791224f144ffb2cf4bde65aade13606c79c4f291773992cced6308771dede61",
        "typed binary NULL hash matches independent state-qualified vector");

  const std::string prefix = FromHex(
      "5342424b45593031019d000000007000800000000000d7080800000000000000"
      "08000000000000002d01000062697e61b2790000000000000100000000000000"
      "019d000000007000800000000000d7430100000000000000019d000000007000"
      "800000000000d7440100000000000000010000000000000001a0fea58a127466"
      "9adbe25464c65b6a010000000000000001a0fea58a127073a5b732606cd091e6"
      "0100000000000000");
  const std::array sort_vectors{
      std::pair{std::string{}, FromHex("010000")},
      std::pair{std::string{"\0", 1}, FromHex("0100ff0000")},
      std::pair{std::string{"\0\0", 2}, FromHex("0100ff00ff0000")},
      std::pair{std::string{"\1", 1}, FromHex("01010000")},
      std::pair{std::string{"\xff", 1}, FromHex("01ff0000")}};
  Check(prefix.size() == 168, "independent D708 binary sort cohort prefix is 168 bytes");
  for (const auto& vector : sort_vectors) {
    const auto result = dt::MakeDatatypeSortKey({Present(vector.first)});
    Check(result.ok() && result.sort_key == prefix + vector.second,
          "binary sort key matches independent cohort prefix and escaped suffix");
  }
  const auto null_first = dt::MakeDatatypeSortKey(
      {typed_null, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey(
      {typed_null, dt::DatatypeNullOrdering::nulls_last});
  Check(null_first.ok() && null_first.sort_key == prefix + FromHex("00") &&
            null_last.ok() && null_last.sort_key == prefix + FromHex("02"),
        "typed binary NULL sort keys are cohort-qualified state markers");

  auto dirty_null = typed_null;
  dirty_null.encoded_value = "payload-must-not-be-read";
  const auto dirty_compare = dt::CompareDatatypeValues({dirty_null, typed_null});
  const auto dirty_hash = dt::HashDatatypeValue({dirty_null});
  const auto dirty_key = dt::MakeDatatypeSortKey({dirty_null});
  Check(RejectedAs(dirty_compare, "DATATYPE.NULL_STATE.INVALID") &&
            RejectedAs(dirty_hash, "DATATYPE.NULL_STATE.INVALID") &&
            RejectedAs(dirty_key, "DATATYPE.NULL_STATE.INVALID") &&
            dirty_compare.comparison == 0 &&
            dirty_hash.stable_hash_hex.empty() && dirty_key.sort_key.empty(),
        "dirty typed binary NULL refuses compare/hash/sort without payload interpretation");

  const auto maximum_key = dt::MakeDatatypeSortKey(
      {Present(std::string(kMaximumBytes, '\0'))});
  Check(maximum_key.ok() && maximum_key.sort_key.size() == 33'554'603u,
        "16MiB zero value has the independently calculated maximum sort-key size");
}

dt::DatatypeCastResult ExplicitCast(const dt::DatatypeOperationValue& value,
                                    dt::CanonicalTypeId target) {
  dt::DatatypeCastRequest request;
  request.value = value;
  request.target_type_id = target;
  request.context = dt::DatatypeCastContext::explicit_cast;
  request.explicit_cast = true;
  request.target_descriptor = DescriptorFor(target);
  return dt::CastDatatypeValue(request);
}

void RenderAndClosedCastFamilies() {
  const auto empty = dt::RenderDatatypeValueForDisplay({Present("")});
  const auto octets = dt::RenderDatatypeValueForDisplay(
      {Present(std::string{"\0\x41\x66", 3})});
  const auto null_display = dt::RenderDatatypeValueForDisplay({TypedNull()});
  dt::DatatypeDisplayRenderRequest literal_request;
  literal_request.value = Present(std::string{"\0\xaf", 2});
  literal_request.export_literal = true;
  const auto literal = dt::RenderDatatypeValueForDisplay(literal_request);
  Check(empty.ok() && empty.display_value == "0x" && octets.ok() &&
            octets.display_value == "0x004166" && null_display.ok() &&
            null_display.display_value == "NULL" && literal.ok() &&
            literal.display_value == "X'00af'",
        "binary canonical display is lowercase 0x followed by exact hex octets");

  const auto character_descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  const auto make_character = [&](std::string text) {
    dt::DatatypeOperationValue value{dt::CanonicalTypeId::character,
                                     std::move(text), false};
    value.descriptor = character_descriptor;
    return value;
  };
  for (const auto& [text, expected] :
       std::array<std::pair<std::string, std::string>, 3>{{
           {"0x", ""}, {"0x00ff", std::string{"\0\xff", 2}},
           {"0x616263", "abc"}}}) {
    const auto result = ExplicitCast(make_character(text), dt::CanonicalTypeId::binary);
    Check(result.ok() && result.category == dt::DatatypeCastCategory::lossless_explicit &&
              result.value.encoded_value == expected && !result.value.is_null,
          "explicit character strict-lowercase-0x cast yields exact binary octets");
  }
  for (const auto malformed : {"", "00", "0X00", "0x0", "0x0A",
                               "0xgg", "0x00 ", "0x00_01"}) {
    const auto result = ExplicitCast(make_character(malformed), dt::CanonicalTypeId::binary);
    Check(RejectedAs(result, "CTB.BINARY.CAST_HEX_INVALID") &&
              result.value.encoded_value.empty(),
          "character to binary rejects every non-strict hex form atomically");
  }
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment}) {
    dt::DatatypeCastRequest request;
    request.value = make_character("0x00");
    request.target_type_id = dt::CanonicalTypeId::binary;
    request.target_descriptor = BinaryDescriptor();
    request.context = context;
    const auto result = dt::CastDatatypeValue(request);
    Check(RejectedAs(result, "DATATYPE.CAST_FORBIDDEN") &&
              result.value.encoded_value.empty(),
          "character-to-binary requires explicit cast context");
  }

  const auto binary_to_character = ExplicitCast(
      Present(std::string{"\0\xff\x10", 3}), dt::CanonicalTypeId::character);
  Check(binary_to_character.ok() &&
            binary_to_character.value.encoded_value == "0x00ff10",
        "explicit binary to character cast emits canonical lowercase 0x text");

  auto invalid_binary_source = Present("abc");
  ++invalid_binary_source.descriptor.descriptor_epoch;
  dt::DatatypeCastRequest invalid_source_request;
  invalid_source_request.value = invalid_binary_source;
  invalid_source_request.target_type_id = dt::CanonicalTypeId::character;
  invalid_source_request.target_descriptor = character_descriptor;
  invalid_source_request.context = dt::DatatypeCastContext::explicit_cast;
  invalid_source_request.explicit_cast = true;
  const auto invalid_binary_source_result =
      dt::CastDatatypeValue(invalid_source_request);

  dt::DatatypeCastRequest invalid_target_request;
  invalid_target_request.value = make_character("0x00");
  invalid_target_request.target_type_id = dt::CanonicalTypeId::binary;
  invalid_target_request.target_descriptor = BinaryDescriptor();
  ++invalid_target_request.target_descriptor.descriptor_epoch;
  invalid_target_request.context = dt::DatatypeCastContext::explicit_cast;
  invalid_target_request.explicit_cast = true;
  const auto invalid_binary_target_result =
      dt::CastDatatypeValue(invalid_target_request);
  Check(RejectedAs(invalid_binary_source_result,
                   "CTB.BINARY.DESCRIPTOR_INVALID") &&
            RejectedAs(invalid_binary_target_result,
                       "CTB.BINARY.DESCRIPTOR_INVALID") &&
            invalid_binary_source_result.value.encoded_value.empty() &&
            invalid_binary_target_result.value.encoded_value.empty(),
        "binary source and target descriptor failures retain binary diagnostic ownership");

  std::string raw_uuid;
  for (unsigned value = 0; value < 16; ++value)
    raw_uuid.push_back(static_cast<char>(value));
  dt::DatatypeOperationValue uuid{dt::CanonicalTypeId::uuid, raw_uuid, false};
  uuid.descriptor = DescriptorFor(dt::CanonicalTypeId::uuid);
  const auto uuid_to_binary = ExplicitCast(uuid, dt::CanonicalTypeId::binary);
  const auto binary_to_uuid = ExplicitCast(Present(raw_uuid), dt::CanonicalTypeId::uuid);
  Check(uuid_to_binary.ok() && uuid_to_binary.value.encoded_value == raw_uuid &&
            binary_to_uuid.ok() && binary_to_uuid.value.encoded_value == raw_uuid,
        "explicit UUID raw16 casts preserve all sixteen octets");

  const auto expect_context_refusal = [](
      const dt::DatatypeOperationValue& value, dt::CanonicalTypeId target,
      dt::DatatypeCastContext context) {
    dt::DatatypeCastRequest request;
    request.value = value;
    request.target_type_id = target;
    request.target_descriptor = DescriptorFor(target);
    request.context = context;
    // This compatibility bit must not upgrade an implicit or assignment
    // request into the explicit context authorized by the closed cast table.
    request.explicit_cast = true;
    const auto result = dt::CastDatatypeValue(request);
    Check(RejectedAs(result, "DATATYPE.CAST_FORBIDDEN") &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty(),
          "legacy explicit_cast flag cannot upgrade binary cross-cast context");
  };
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment}) {
    expect_context_refusal(make_character("0x00"),
                           dt::CanonicalTypeId::binary, context);
    expect_context_refusal(Present(std::string{"\0", 1}),
                           dt::CanonicalTypeId::character, context);
    expect_context_refusal(uuid, dt::CanonicalTypeId::binary, context);
    expect_context_refusal(Present(raw_uuid), dt::CanonicalTypeId::uuid,
                           context);
  }

  auto invalid_uuid_source = uuid;
  ++invalid_uuid_source.descriptor.descriptor_epoch;
  const auto invalid_uuid_source_result =
      ExplicitCast(invalid_uuid_source, dt::CanonicalTypeId::binary);
  dt::DatatypeCastRequest invalid_uuid_target_request;
  invalid_uuid_target_request.value = Present(raw_uuid);
  invalid_uuid_target_request.target_type_id = dt::CanonicalTypeId::uuid;
  invalid_uuid_target_request.target_descriptor =
      DescriptorFor(dt::CanonicalTypeId::uuid);
  ++invalid_uuid_target_request.target_descriptor.descriptor_epoch;
  invalid_uuid_target_request.context = dt::DatatypeCastContext::explicit_cast;
  invalid_uuid_target_request.explicit_cast = true;
  const auto invalid_uuid_target_result =
      dt::CastDatatypeValue(invalid_uuid_target_request);
  Check(RejectedAs(invalid_uuid_source_result,
                   "CINL.IDENTITY.DESCRIPTOR_INVALID") &&
            RejectedAs(invalid_uuid_target_result,
                       "CINL.IDENTITY.DESCRIPTOR_INVALID") &&
            invalid_uuid_source_result.value.encoded_value.empty() &&
            invalid_uuid_target_result.value.encoded_value.empty(),
        "UUID source and target descriptor failures retain CINL identity ownership");
  for (const auto size : {15u, 17u}) {
    const auto result = ExplicitCast(Present(std::string(size, 'x')),
                                     dt::CanonicalTypeId::uuid);
    Check(RejectedAs(result, "DATATYPE.CAST_FORBIDDEN") &&
              result.value.encoded_value.empty(),
          "binary to UUID refuses non-raw16 payloads atomically");
  }

  for (const auto target : {dt::CanonicalTypeId::blob,
                            dt::CanonicalTypeId::real128,
                            dt::CanonicalTypeId::int32}) {
    const auto result = ExplicitCast(Present("abc"), target);
    // BLOB now owns a V3 profile API; the generic V1/V2 entrypoint must retain
    // that exact route diagnostic, not relabel it as generic cast admission.
    Check(RejectedAs(result, target==dt::CanonicalTypeId::blob
                                ? "BLOB.V1_V2_REFUSED" : "DATATYPE.CAST_FORBIDDEN") &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::binary,target)==dt::DatatypeCastCategory::forbidden &&
              result.value.encoded_value.empty(),
          "closed base.binary cast registry forbids every other target family");
  }

  dt::DatatypeOperationValue real128{dt::CanonicalTypeId::real128,
                                     std::string(16, '\0'), false};
  real128.descriptor = DescriptorFor(dt::CanonicalTypeId::real128);
  const auto real128_to_binary = ExplicitCast(real128, dt::CanonicalTypeId::binary);
  Check(RejectedAs(real128_to_binary, "DATATYPE.CAST_FORBIDDEN") &&
            real128_to_binary.value.encoded_value.empty(),
        "real128 to binary remains forbidden despite the old scalar wording");

  for (const auto& value : {Present("identity"), TypedNull()}) {
    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      dt::DatatypeCastRequest request;
      request.value = value;
      request.target_type_id = dt::CanonicalTypeId::binary;
      request.target_descriptor = BinaryDescriptor();
      request.context = context;
      request.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
      const auto identity = dt::CastDatatypeValue(request);
      Check(identity.ok() &&
                identity.category == dt::DatatypeCastCategory::identity &&
                identity.value.type_id == dt::CanonicalTypeId::binary &&
                identity.value.is_null == value.is_null &&
                identity.value.encoded_value == value.encoded_value &&
                SameUuidBytes(identity.value.descriptor.descriptor_uuid,
                              kDescriptorUuid),
            "binary identity cast preserves state, bytes and descriptor in every admitted context");
    }
  }

  auto nullable_identity_source = Present("nullable-slot-identity");
  auto nonnullable_identity_source = nullable_identity_source;
  nonnullable_identity_source.descriptor.nullable_allowed = false;
  for (const auto& [source, target_nullable] :
       std::array<std::pair<dt::DatatypeOperationValue, bool>, 2>{{
           {nullable_identity_source, false},
           {nonnullable_identity_source, true}}}) {
    dt::DatatypeCastRequest request;
    request.value = source;
    request.target_type_id = dt::CanonicalTypeId::binary;
    request.target_descriptor = BinaryDescriptor();
    request.target_descriptor.nullable_allowed = target_nullable;
    request.context = dt::DatatypeCastContext::implicit;
    const auto identity = dt::CastDatatypeValue(request);
    Check(identity.ok() &&
              identity.category == dt::DatatypeCastCategory::identity &&
              !identity.value.is_null &&
              identity.value.encoded_value == source.encoded_value,
          "binary PRESENT identity admits nullable/nonnullable slot descriptors in one cohort");
  }

  dt::DatatypeCastRequest nonnullable_null_identity;
  nonnullable_null_identity.value = TypedNull();
  nonnullable_null_identity.target_type_id = dt::CanonicalTypeId::binary;
  nonnullable_null_identity.target_descriptor = BinaryDescriptor();
  nonnullable_null_identity.target_descriptor.nullable_allowed = false;
  nonnullable_null_identity.context = dt::DatatypeCastContext::implicit;
  const auto nonnullable_null_identity_result =
      dt::CastDatatypeValue(nonnullable_null_identity);
  Check(RejectedAs(nonnullable_null_identity_result,
                   "DATATYPE.NULL_NOT_ADMITTED") &&
            nonnullable_null_identity_result.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            !nonnullable_null_identity_result.value.is_null &&
            nonnullable_null_identity_result.value.encoded_value.empty(),
        "binary typed NULL identity reaches target nullability admission and publishes no value");
}

void FileDevicePersistence() {
  const std::vector<std::string> carriers{
      "", std::string{"\0\xff\x10\x80\x7f", 5}, "abc"};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto& carrier : carriers) {
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::binary,
         dt::DatatypePhysicalValueState::value, Payload(carrier)});
    if (encoded.ok()) frames.push_back(encoded.bytes);
  }
  const auto null_encoded = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::binary,
       dt::DatatypePhysicalValueState::sql_null, {}});
  if (null_encoded.ok()) frames.push_back(null_encoded.bytes);
  Check(frames.size() == carriers.size() + 1,
        "physical component produced every binary persistence frame");
  if (frames.size() != carriers.size() + 1) return;

  // This is a deliberately test-owned container, not a production wire or
  // storage envelope. Its fixed header records the complete Core D708 receipt
  // followed by offsets to production physical-component frames.
  constexpr std::size_t header_bytes = 512;
  constexpr std::size_t codec_id_offset = 128;
  constexpr std::size_t codec_id_capacity = 32;
  constexpr std::size_t policies_offset = 160;
  constexpr std::size_t policy_row_bytes = 24;
  constexpr std::size_t frame_count_offset = 448;
  constexpr std::size_t frame_table_offset = 456;
  static_assert(kCodecId.size() <= codec_id_capacity);
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte, 8> magic{{'T','E','S','T','B','I','N','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(dt::kDatatypeCohortV8.bytes.begin(),
            dt::kDatatypeCohortV8.bytes.end(), expected.begin() + 8);
  platform::StoreLittle64(expected.data() + 24, 8);
  platform::StoreLittle64(expected.data() + 32, 8);
  std::copy(kDescriptorUuid.bytes.begin(), kDescriptorUuid.bytes.end(),
            expected.begin() + 40);
  platform::StoreLittle64(expected.data() + 56, 1);
  std::copy(kTypeUuid.bytes.begin(), kTypeUuid.bytes.end(), expected.begin() + 64);
  platform::StoreLittle64(expected.data() + 80, 1);
  std::copy(kCodecUuid.bytes.begin(), kCodecUuid.bytes.end(), expected.begin() + 88);
  platform::StoreLittle64(expected.data() + 104, 1);
  platform::StoreLittle32(expected.data() + 112, 1);
  platform::StoreLittle32(expected.data() + 116, 301);
  platform::StoreLittle32(expected.data() + 120,
                          static_cast<std::uint32_t>(kCodecId.size()));
  platform::StoreLittle32(expected.data() + 124,
                          static_cast<std::uint32_t>(kPolicyReceipt.size()));
  std::copy(kCodecId.begin(), kCodecId.end(),
            expected.begin() + codec_id_offset);
  for (std::size_t index = 0; index < kPolicyReceipt.size(); ++index) {
    const auto offset = policies_offset + index * policy_row_bytes;
    std::copy(kPolicyReceipt[index].uuid.bytes.begin(),
              kPolicyReceipt[index].uuid.bytes.end(), expected.begin() + offset);
    platform::StoreLittle64(expected.data() + offset + 16,
                            kPolicyReceipt[index].generation);
  }
  platform::StoreLittle32(expected.data() + frame_count_offset,
                          static_cast<std::uint32_t>(frames.size()));
  std::uint32_t offset = header_bytes;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    platform::StoreLittle32(expected.data() + frame_table_offset + index * 8,
                            offset);
    platform::StoreLittle32(expected.data() + frame_table_offset + 4 + index * 8,
                            static_cast<std::uint32_t>(frames[index].size()));
    offset += static_cast<std::uint32_t>(frames[index].size());
  }
  for (const auto& frame : frames)
    expected.insert(expected.end(), frame.begin(), frame.end());

  scratchbird::tests::OwnedTempDirectory fixture;
  const auto root=fixture.path();
  const fs::path path=root/"binary.test-container";
  {
  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create test-owned binary persistence envelope");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write sync and close test-owned binary persistence envelope");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen test-owned binary persistence envelope read-only");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  const auto uuid_at = [&actual](std::size_t offset,
                                 const platform::Uuid& expected_uuid) {
    return std::equal(expected_uuid.bytes.begin(), expected_uuid.bytes.end(),
                      actual.begin() + offset);
  };
  bool receipt_exact = read.ok() && actual.size() >= header_bytes &&
      std::equal(magic.begin(), magic.end(), actual.begin()) &&
      uuid_at(8, dt::kDatatypeCohortV8) &&
      platform::LoadLittle64(actual.data() + 24) == 8 &&
      platform::LoadLittle64(actual.data() + 32) == 8 &&
      uuid_at(40, kDescriptorUuid) &&
      platform::LoadLittle64(actual.data() + 56) == 1 &&
      uuid_at(64, kTypeUuid) &&
      platform::LoadLittle64(actual.data() + 80) == 1 &&
      uuid_at(88, kCodecUuid) &&
      platform::LoadLittle64(actual.data() + 104) == 1 &&
      platform::LoadLittle32(actual.data() + 112) == 1 &&
      platform::LoadLittle32(actual.data() + 116) == 301 &&
      platform::LoadLittle32(actual.data() + 120) == kCodecId.size() &&
      platform::LoadLittle32(actual.data() + 124) == kPolicyReceipt.size() &&
      std::string_view(reinterpret_cast<const char*>(actual.data() + codec_id_offset),
                       kCodecId.size()) == kCodecId &&
      platform::LoadLittle32(actual.data() + frame_count_offset) == frames.size();
  for (std::size_t index = 0; index < kPolicyReceipt.size() && receipt_exact;
       ++index) {
    const auto policy_offset = policies_offset + index * policy_row_bytes;
    receipt_exact = uuid_at(policy_offset, kPolicyReceipt[index].uuid) &&
        platform::LoadLittle64(actual.data() + policy_offset + 16) ==
            kPolicyReceipt[index].generation;
  }
  Check(receipt_exact,
        "FileDevice reopen recovers the complete exact D708 binary receipt");

  platform::Uuid recovered_snapshot{};
  platform::Uuid recovered_descriptor{};
  std::copy_n(actual.begin() + 8, 16, recovered_snapshot.bytes.begin());
  std::copy_n(actual.begin() + 40, 16, recovered_descriptor.bytes.begin());
  const auto admitted = dt::LookupDatatypeTypeCodecIdentityV3(
      recovered_snapshot, platform::LoadLittle64(actual.data() + 24),
      platform::LoadLittle64(actual.data() + 32), recovered_descriptor,
      platform::LoadLittle64(actual.data() + 56));
  Check(receipt_exact && admitted.ok &&
            SameUuidBytes(admitted.row.legacy_fields.type_uuid, kTypeUuid) &&
            admitted.row.legacy_fields.type_generation ==
                platform::LoadLittle64(actual.data() + 80) &&
            SameUuidBytes(admitted.row.legacy_fields.codec_uuid, kCodecUuid) &&
            admitted.row.legacy_fields.codec_generation ==
                platform::LoadLittle64(actual.data() + 104) &&
            admitted.row.legacy_fields.codec_version ==
                platform::LoadLittle32(actual.data() + 112) &&
            admitted.row.legacy_fields.codec_id == kCodecId,
        "recovered test-owned receipt revalidates against exact compiled D708 V3 row");

  bool decoded_all = receipt_exact && actual == expected;
  for (std::size_t index = 0; index < frames.size() && decoded_all; ++index) {
    const auto frame_offset = platform::LoadLittle32(
        actual.data() + frame_table_offset + index * 8);
    const auto frame_size = platform::LoadLittle32(
        actual.data() + frame_table_offset + 4 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::binary &&
        (index == carriers.size()
             ? decoded.value.state == dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state == dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(carriers[index]));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "FileDevice close/reopen preserves exact binary state and octets");
  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(actual.size() - 1,
                                        short_buffer.data(), short_buffer.size());
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size() &&
            !rejected_write.ok() && rejected_write.bytes_transferred == 0 &&
            reader.Close().ok(),
        "binary fixture detects short reads and read-only writes");

  auto corrupt = frames[1];
  corrupt.back() ^= 1u;
  Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
            !dt::DecodeDatatypePhysicalValue(
                frames[1].data(), frames[1].size() - 1).ok(),
        "binary physical decoder rejects corruption and truncation");
  }
  fixture.Cleanup();
  Check(!fs::exists(root),"binary fixture and owner sidecars cleaned after all devices close");
}

}  // namespace

int main() {
  ExactIdentityCohortsAndLayout();
  ExactOctetsAndLowerCodecs();
  AllocationFreeBorrowedViews();
  LengthNullDescriptorAndSerializationRules();
  EqualityOrderingHashAndSort();
  RenderAndClosedCastFamilies();
  FileDevicePersistence();
  std::cout << "base.binary canonical value checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
