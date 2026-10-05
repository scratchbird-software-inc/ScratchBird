// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"
#include "canonical_utf8.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "uuid.hpp"
#include "../hash/hash_digest_parts.hpp"

#include "scratchbird/engine/value.hpp"
#include "sbl_numeric.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <string_view>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::engine::ExecutionTypeDescriptor;
using scratchbird::engine::ExecutionTypeModifierFlag;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes};
}

Status ResourceErrorStatus() {
  return {StatusCode::memory_allocation_failed, Severity::error,
          Subsystem::datatypes};
}

bool EngineUuidIsNil(const scratchbird::engine::Uuid& uuid) noexcept {
  for (const auto byte : uuid.bytes) {
    if (byte != 0) return false;
  }
  return true;
}

bool EngineIdentityUuidValid(const scratchbird::engine::Uuid& uuid) noexcept {
  return !EngineUuidIsNil(uuid) && (uuid.bytes[6] & 0xf0u) == 0x70u &&
      (uuid.bytes[8] & 0xc0u) == 0x80u;
}

bool EngineUuidEquals(const scratchbird::engine::Uuid& left,
                      const scratchbird::engine::Uuid& right) noexcept {
  for (std::size_t index = 0; index < 16; ++index) {
    if (left.bytes[index] != right.bytes[index]) return false;
  }
  return true;
}

bool ExecutionDescriptorMatchesCurrentBuiltinCatalog(
    const ExecutionTypeDescriptor& descriptor,
    CanonicalTypeId type_id) {
  static const DatatypeCatalogManifestResult current =
      LoadCurrentCoreDatatypeCatalogManifest();
  if (!current.ok()) {
    return false;
  }
  for (const auto& row : current.manifest.descriptor_rows) {
    if (row.type_id != type_id) {
      continue;
    }
    scratchbird::engine::Uuid row_uuid{};
    std::copy(row.descriptor_uuid.value.bytes.begin(),
              row.descriptor_uuid.value.bytes.end(), row_uuid.bytes);
    return EngineUuidEquals(descriptor.descriptor_uuid, row_uuid) &&
        descriptor.descriptor_epoch == row.descriptor_epoch;
  }
  return false;
}

bool DescriptorHasFlag(const ExecutionTypeDescriptor& descriptor,
                       ExecutionTypeModifierFlag flag) noexcept {
  return (descriptor.modifier_flags &
          scratchbird::engine::ExecutionTypeModifierFlagBit(flag)) != 0;
}

bool OptionalDescriptorUuidValid(
    const scratchbird::engine::Uuid& uuid,
    const ExecutionTypeDescriptor& descriptor,
    ExecutionTypeModifierFlag flag) noexcept {
  const bool present = !EngineUuidIsNil(uuid);
  return present == DescriptorHasFlag(descriptor, flag) &&
      (!present || EngineIdentityUuidValid(uuid));
}

bool ExecutionDescriptorValidForType(
    const ExecutionTypeDescriptor& descriptor,
    CanonicalTypeId type_id) {
  // stable_name is presentation/alias metadata. UUID, generation, type shape,
  // modifiers, domains, and policy UUIDs determine runtime identity.
  const auto canonical = LookupDatatypeDescriptor(type_id);
  if (!canonical.ok() || type_id == CanonicalTypeId::null_type ||
      type_id == CanonicalTypeId::unknown ||
      !EngineIdentityUuidValid(descriptor.descriptor_uuid) ||
      descriptor.descriptor_epoch == 0 ||
      descriptor.canonical_type_id != static_cast<std::uint32_t>(type_id) ||
      descriptor.family != static_cast<scratchbird::engine::ExecutionTypeFamily>(
                               canonical.descriptor.family) ||
      descriptor.width_class !=
          static_cast<scratchbird::engine::ExecutionTypeWidthClass>(
              canonical.descriptor.width_class) ||
      descriptor.bit_width != canonical.descriptor.bit_width ||
      !descriptor.descriptor_authoritative || !descriptor.parser_independent) {
    return false;
  }
  if (EngineUuidIsNil(descriptor.domain_uuid) &&
      descriptor.domain_stack.empty() &&
      !ExecutionDescriptorMatchesCurrentBuiltinCatalog(descriptor, type_id)) {
    return false;
  }

  const bool precision_parameterized =
      type_id == CanonicalTypeId::decimal ||
      type_id == CanonicalTypeId::decimal_float;
  const bool length_allowed =
      descriptor.family == scratchbird::engine::ExecutionTypeFamily::character ||
      descriptor.family == scratchbird::engine::ExecutionTypeFamily::binary ||
      descriptor.family == scratchbird::engine::ExecutionTypeFamily::bit_string ||
      descriptor.family == scratchbird::engine::ExecutionTypeFamily::blob;
  const bool container_rank_allowed =
      descriptor.family == scratchbird::engine::ExecutionTypeFamily::structured ||
      descriptor.family == scratchbird::engine::ExecutionTypeFamily::document;
  if ((!precision_parameterized &&
       (descriptor.precision != canonical.descriptor.default_precision ||
        descriptor.scale != canonical.descriptor.default_scale)) ||
      (precision_parameterized &&
       (descriptor.precision == 0 || descriptor.scale > descriptor.precision ||
        (type_id == CanonicalTypeId::decimal &&
         (descriptor.precision > 38 || descriptor.scale > 38)))) ||
      (!length_allowed && descriptor.length != 0) ||
      (descriptor.family != scratchbird::engine::ExecutionTypeFamily::vector &&
       descriptor.vector_dimensions != 0) ||
      (!container_rank_allowed && descriptor.container_rank != 0) ||
      (!EngineUuidIsNil(descriptor.charset_uuid) &&
       descriptor.family != scratchbird::engine::ExecutionTypeFamily::character) ||
      (!EngineUuidIsNil(descriptor.collation_uuid) &&
       (descriptor.family != scratchbird::engine::ExecutionTypeFamily::character ||
        EngineUuidIsNil(descriptor.charset_uuid))) ||
      (!EngineUuidIsNil(descriptor.timezone_uuid) &&
       descriptor.family != scratchbird::engine::ExecutionTypeFamily::temporal)) {
    return false;
  }

  constexpr std::uint64_t kKnownModifierFlags =
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::precision) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::scale) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::length) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::charset_uuid) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::collation_uuid) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::timezone_uuid) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::domain_uuid) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::domain_stack) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::element_descriptor_uuid) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::vector_dimensions) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::container_rank) |
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          ExecutionTypeModifierFlag::security_policy_uuid);
  if ((descriptor.modifier_flags & ~kKnownModifierFlags) != 0 ||
      ((descriptor.precision != 0) !=
       DescriptorHasFlag(descriptor, ExecutionTypeModifierFlag::precision)) ||
      ((descriptor.scale != 0 || type_id == CanonicalTypeId::decimal ||
        type_id == CanonicalTypeId::decimal_float) !=
       DescriptorHasFlag(descriptor, ExecutionTypeModifierFlag::scale)) ||
      ((descriptor.length != 0) !=
       DescriptorHasFlag(descriptor, ExecutionTypeModifierFlag::length)) ||
      ((descriptor.vector_dimensions != 0) !=
       DescriptorHasFlag(descriptor,
                          ExecutionTypeModifierFlag::vector_dimensions)) ||
      ((descriptor.container_rank != 0) !=
       DescriptorHasFlag(descriptor,
                          ExecutionTypeModifierFlag::container_rank)) ||
      descriptor.domain_stack.empty() ==
          DescriptorHasFlag(descriptor, ExecutionTypeModifierFlag::domain_stack)) {
    return false;
  }
  if (EngineUuidIsNil(descriptor.domain_uuid) !=
          descriptor.domain_stack.empty() ||
      descriptor.domain_stack.size() >
          scratchbird::engine::kExecutionDomainStackMaxDepth) {
    return false;
  }
  for (std::size_t index = 0; index < descriptor.domain_stack.size(); ++index) {
    if (!EngineIdentityUuidValid(descriptor.domain_stack[index]) ||
        (index == 0 &&
         !EngineUuidEquals(descriptor.domain_stack[index],
                           descriptor.domain_uuid))) {
      return false;
    }
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      if (EngineUuidEquals(descriptor.domain_stack[index],
                           descriptor.domain_stack[earlier])) {
        return false;
      }
    }
  }
  return OptionalDescriptorUuidValid(
             descriptor.domain_uuid, descriptor,
             ExecutionTypeModifierFlag::domain_uuid) &&
      OptionalDescriptorUuidValid(
             descriptor.charset_uuid, descriptor,
             ExecutionTypeModifierFlag::charset_uuid) &&
      OptionalDescriptorUuidValid(
             descriptor.collation_uuid, descriptor,
             ExecutionTypeModifierFlag::collation_uuid) &&
      OptionalDescriptorUuidValid(
             descriptor.timezone_uuid, descriptor,
             ExecutionTypeModifierFlag::timezone_uuid) &&
      OptionalDescriptorUuidValid(
             descriptor.element_descriptor_uuid, descriptor,
             ExecutionTypeModifierFlag::element_descriptor_uuid) &&
      OptionalDescriptorUuidValid(
             descriptor.security_policy_uuid, descriptor,
             ExecutionTypeModifierFlag::security_policy_uuid);
}

bool ExecutionDescriptorPresent(
    const ExecutionTypeDescriptor& descriptor) noexcept {
  // A label is metadata rather than identity, but its presence still requires
  // validation so a label-only pseudo-descriptor cannot bypass UUID checks.
  return !EngineUuidIsNil(descriptor.descriptor_uuid) ||
      descriptor.descriptor_epoch != 0 || descriptor.canonical_type_id != 0 ||
      descriptor.family != scratchbird::engine::ExecutionTypeFamily::unknown ||
      descriptor.width_class !=
          scratchbird::engine::ExecutionTypeWidthClass::unknown ||
      !descriptor.stable_name.empty() || descriptor.bit_width != 0 ||
      descriptor.precision != 0 || descriptor.scale != 0 ||
      descriptor.length != 0 || descriptor.vector_dimensions != 0 ||
      descriptor.container_rank != 0 || descriptor.modifier_flags != 0 ||
      !EngineUuidIsNil(descriptor.domain_uuid) ||
      !descriptor.domain_stack.empty() ||
      !EngineUuidIsNil(descriptor.charset_uuid) ||
      !EngineUuidIsNil(descriptor.collation_uuid) ||
      !EngineUuidIsNil(descriptor.timezone_uuid) ||
      !EngineUuidIsNil(descriptor.element_descriptor_uuid) ||
      !EngineUuidIsNil(descriptor.security_policy_uuid) ||
      !descriptor.nullable_allowed || !descriptor.descriptor_authoritative ||
      !descriptor.parser_independent;
}

bool DomainBindingEquals(const ExecutionTypeDescriptor& left,
                         const ExecutionTypeDescriptor& right) noexcept {
  if (!EngineUuidEquals(left.domain_uuid, right.domain_uuid) ||
      left.domain_stack.size() != right.domain_stack.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.domain_stack.size(); ++index) {
    if (!EngineUuidEquals(left.domain_stack[index], right.domain_stack[index])) {
      return false;
    }
  }
  return true;
}

bool ExecutionDescriptorEqualsIgnoringNullability(
    const ExecutionTypeDescriptor& left,
    const ExecutionTypeDescriptor& right) noexcept {
  // Human and localized labels may differ for one UUID-bound descriptor.
  return EngineUuidEquals(left.descriptor_uuid, right.descriptor_uuid) &&
      left.descriptor_epoch == right.descriptor_epoch &&
      left.canonical_type_id == right.canonical_type_id &&
      left.family == right.family && left.width_class == right.width_class &&
      left.bit_width == right.bit_width && left.precision == right.precision &&
      left.scale == right.scale && left.length == right.length &&
      left.vector_dimensions == right.vector_dimensions &&
      left.container_rank == right.container_rank &&
      left.modifier_flags == right.modifier_flags &&
      DomainBindingEquals(left, right) &&
      EngineUuidEquals(left.charset_uuid, right.charset_uuid) &&
      EngineUuidEquals(left.collation_uuid, right.collation_uuid) &&
      EngineUuidEquals(left.timezone_uuid, right.timezone_uuid) &&
      EngineUuidEquals(left.element_descriptor_uuid,
                       right.element_descriptor_uuid) &&
      EngineUuidEquals(left.security_policy_uuid,
                       right.security_policy_uuid) &&
      left.descriptor_authoritative == right.descriptor_authoritative &&
      left.parser_independent == right.parser_independent;
}

bool ExecutionDescriptorEquals(const ExecutionTypeDescriptor& left,
                               const ExecutionTypeDescriptor& right) noexcept {
  return ExecutionDescriptorEqualsIgnoringNullability(left, right) &&
      left.nullable_allowed == right.nullable_allowed;
}

bool ExecutionDescriptorExactlyMatchesCurrentBuiltin(
    const ExecutionTypeDescriptor& descriptor,
    CanonicalTypeId type_id) {
  if (!ExecutionDescriptorValidForType(descriptor, type_id)) return false;
  static const DatatypeCatalogManifestResult current =
      LoadCurrentCoreDatatypeCatalogManifest();
  if (!current.ok()) return false;
  const auto row = LookupDatatypeCatalogRow(current.manifest, type_id);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return false;
  CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch =
      row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto expected =
      LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  return expected.ok() &&
      ExecutionDescriptorEquals(descriptor, expected.descriptor);
}

bool ExecutionDescriptorExactlyMatchesCurrentBuiltinIgnoringNullability(
    const ExecutionTypeDescriptor& descriptor,
    CanonicalTypeId type_id) {
  if (!ExecutionDescriptorValidForType(descriptor, type_id)) return false;
  static const DatatypeCatalogManifestResult current =
      LoadCurrentCoreDatatypeCatalogManifest();
  if (!current.ok()) return false;
  const auto row = LookupDatatypeCatalogRow(current.manifest, type_id);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return false;
  CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch =
      row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto expected =
      LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  return expected.ok() && ExecutionDescriptorEqualsIgnoringNullability(
                              descriptor, expected.descriptor);
}

bool DescriptorHasDomainBinding(
    const ExecutionTypeDescriptor& descriptor) noexcept {
  return !EngineUuidIsNil(descriptor.domain_uuid) ||
      !descriptor.domain_stack.empty();
}

bool IsUnresolvedRealSemantics(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::bfloat16 ||
      type_id == CanonicalTypeId::real16 ||
      type_id == CanonicalTypeId::real32 ||
      type_id == CanonicalTypeId::real64;
}

bool IsReal128(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::real128;
}

bool IsPolicyRestrictedReal(CanonicalTypeId type_id) noexcept {
  return IsUnresolvedRealSemantics(type_id) || IsReal128(type_id);
}

bool IsDecimal(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::decimal;
}

bool IsDecimalFloat(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::decimal_float;
}

bool IsUuid(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::uuid;
}

bool IsIpAddress(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::ip_address;
}

bool IsNetworkPrefix(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::network_prefix;
}

bool IsMacAddress(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::mac_address;
}

bool IsCharacter(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::character;
}

bool IsBinary(CanonicalTypeId type_id) noexcept {
  return type_id == CanonicalTypeId::binary;
}

inline constexpr std::size_t kCanonicalBinaryMaximumBytes =
    16u * 1024u * 1024u;

constexpr scratchbird::core::platform::Uuid kCurrentBinarySnapshotUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x08}};
constexpr scratchbird::core::platform::Uuid kBinaryDescriptorUuid{{
    0x2d, 0x01, 0x00, 0x00, 0x62, 0x69, 0x7e, 0x61,
    0xb2, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
constexpr scratchbird::core::platform::Uuid kBinaryTypeUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x43}};
constexpr scratchbird::core::platform::Uuid kBinaryCodecUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x44}};
constexpr scratchbird::core::platform::Uuid kBinaryDescriptorPolicyUuid{{
    0x01, 0xa0, 0xfe, 0xa5, 0x8a, 0x12, 0x7d, 0xa5,
    0x9d, 0x9d, 0x83, 0x9b, 0xc8, 0x1b, 0x09, 0xca}};
constexpr scratchbird::core::platform::Uuid kBinaryCanonicalizationPolicyUuid{{
    0x01, 0xa0, 0xfe, 0xa5, 0x8a, 0x12, 0x74, 0x66,
    0x9a, 0xdb, 0xe2, 0x54, 0x64, 0xc6, 0x5b, 0x6a}};
constexpr scratchbird::core::platform::Uuid kBinaryOrderingPolicyUuid{{
    0x01, 0xa0, 0xfe, 0xa5, 0x8a, 0x12, 0x70, 0x73,
    0xa5, 0xb7, 0x32, 0x60, 0x6c, 0xd0, 0x91, 0xe6}};
constexpr scratchbird::core::platform::Uuid kBinaryHashPolicyUuid{{
    0x01, 0xa0, 0xfe, 0xa5, 0x8a, 0x12, 0x73, 0x7b,
    0xa4, 0x82, 0xe6, 0x96, 0xfe, 0x71, 0x41, 0xf8}};

DatatypeTypeCodecIdentityLookupV3 CurrentBinaryIdentityRow() noexcept {
  auto result = LookupDatatypeTypeCodecIdentityV3(
      kCurrentBinarySnapshotUuid, 8, 8, kBinaryDescriptorUuid, 1);
  if (!result.ok) return result;

  const auto& row = result.row.legacy_fields;
  const auto policy_absent = [](const DatatypePolicyIdentityV3& policy) {
    return policy.uuid.is_nil() && policy.generation == 0;
  };
  if (row.catalog_snapshot_uuid != kCurrentBinarySnapshotUuid ||
      row.catalog_generation != 8 || row.registry_generation != 8 ||
      row.descriptor_uuid != kBinaryDescriptorUuid ||
      row.descriptor_generation != 1 || row.type_generation != 1 ||
      row.type_uuid != kBinaryTypeUuid || row.codec_uuid != kBinaryCodecUuid ||
      row.codec_version != 1 || row.codec_generation != 1 ||
      row.canonical_binary_type_code !=
          static_cast<u32>(CanonicalTypeId::binary) ||
      row.codec_id != "datatype.binary.octets.v1" ||
      row.canonical_value_bytes != 0 || !row.null_supported ||
      !row.canonical_value_variable_width ||
      !row.canonical_value_exact_zero_is_width_marker ||
      row.canonical_value_minimum_bytes != 0 ||
      row.canonical_value_maximum_bytes != kCanonicalBinaryMaximumBytes ||
      row.canonical_value_exact_bytes != 0 ||
      row.canonical_byte_order != "byte_sequence" ||
      row.canonical_representation !=
          "exact_octets_without_text_conversion" ||
      !row.canonical_charset.empty() || row.shortest_form_utf8_required ||
      row.implicit_normalization_allowed ||
      row.descriptor_bound_collation_required ||
      !row.empty_value_distinct_from_sql_null ||
      !row.sql_null_requires_zero_payload ||
      !row.variable_width_storage_without_truncation ||
      result.row.descriptor_policy.uuid != kBinaryDescriptorPolicyUuid ||
      result.row.descriptor_policy.generation != 1 ||
      result.row.canonicalization_policy.uuid !=
          kBinaryCanonicalizationPolicyUuid ||
      result.row.canonicalization_policy.generation != 1 ||
      result.row.ordering_policy.uuid != kBinaryOrderingPolicyUuid ||
      result.row.ordering_policy.generation != 1 ||
      result.row.hash_policy.uuid != kBinaryHashPolicyUuid ||
      result.row.hash_policy.generation != 1 ||
      !policy_absent(result.row.operation_policy)) {
    result.ok = false;
    result.diagnostic_id = "DATATYPE.DESCRIPTOR.INVALID";
  }
  return result;
}

bool BinaryDescriptorExactlyCurrent(
    const ExecutionTypeDescriptor& descriptor) {
  return CurrentBinaryIdentityRow().ok &&
      ExecutionDescriptorExactlyMatchesCurrentBuiltinIgnoringNullability(
          descriptor, CanonicalTypeId::binary);
}

bool CanonicalBinaryValueValid(const DatatypeOperationValue& value) {
  if (!IsBinary(value.type_id) ||
      !BinaryDescriptorExactlyCurrent(value.descriptor)) {
    return false;
  }
  if (value.is_null) {
    return value.encoded_value.empty() && value.descriptor.nullable_allowed;
  }
  return value.encoded_value.size() <= kCanonicalBinaryMaximumBytes;
}

void AppendLittleU32(std::string* bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
  }
}

void AppendLittleU64(std::string* bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
  }
}

void AppendUuidBytes(std::string* bytes,
                     const scratchbird::core::platform::Uuid& value) {
  bytes->append(reinterpret_cast<const char*>(value.bytes.data()),
                value.bytes.size());
}

bool BuildBinaryCohortPrefix(std::string* prefix, bool include_hash_policy) {
  const auto lookup = CurrentBinaryIdentityRow();
  if (!lookup.ok || prefix == nullptr) return false;
  const auto& row = lookup.row.legacy_fields;
  prefix->clear();
  prefix->reserve(include_hash_policy ? 190u : 168u);
  if (include_hash_policy) {
    prefix->append("ScratchBird.BaseBinary.Hash.V1");
  } else {
    prefix->append("SBBKEY01");
  }
  AppendUuidBytes(prefix, row.catalog_snapshot_uuid);
  AppendLittleU64(prefix, row.catalog_generation);
  AppendLittleU64(prefix, row.registry_generation);
  AppendUuidBytes(prefix, row.descriptor_uuid);
  AppendLittleU64(prefix, row.descriptor_generation);
  AppendUuidBytes(prefix, row.type_uuid);
  AppendLittleU64(prefix, row.type_generation);
  AppendUuidBytes(prefix, row.codec_uuid);
  AppendLittleU32(prefix, row.codec_version);
  AppendLittleU32(prefix, 0);
  AppendLittleU64(prefix, row.codec_generation);
  AppendUuidBytes(prefix, kBinaryCanonicalizationPolicyUuid);
  AppendLittleU64(prefix, 1);
  AppendUuidBytes(prefix, include_hash_policy ? kBinaryHashPolicyUuid
                                               : kBinaryOrderingPolicyUuid);
  AppendLittleU64(prefix, 1);
  return prefix->size() == (include_hash_policy ? 190u : 168u);
}

bool DecodeStrictBinaryHex(std::string_view text, std::string* bytes) {
  if (bytes == nullptr || text.size() < 2 || text[0] != '0' || text[1] != 'x') {
    return false;
  }
  text.remove_prefix(2);
  if ((text.size() % 2u) != 0 || text.size() / 2u > kCanonicalBinaryMaximumBytes) {
    return false;
  }
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  bytes->clear();
  bytes->reserve(text.size() / 2u);
  const auto nibble = [](char c) {
    return c <= '9' ? c - '0' : 10 + c - 'a';
  };
  for (std::size_t index = 0; index < text.size(); index += 2) {
    bytes->push_back(static_cast<char>(
        (nibble(text[index]) << 4) | nibble(text[index + 1])));
  }
  return true;
}

inline constexpr std::size_t kCanonicalCharacterMaximumBytes =
    16u * 1024u * 1024u;

bool CanonicalCharacterPayloadValid(const std::string& value,
                                    std::uint64_t* scalar_count = nullptr) {
  if (scalar_count != nullptr) *scalar_count = 0;
  if (value.size() > kCanonicalCharacterMaximumBytes) return false;
  return ValidateCanonicalUtf8(
      reinterpret_cast<const std::uint8_t*>(value.data()), value.size(),
      scalar_count);
}

const char* CanonicalCharacterFailureDetail(
    const DatatypeOperationValue& value) noexcept {
  return value.encoded_value.size() > kCanonicalCharacterMaximumBytes
      ? "character_length_exceeded"
      : "character_utf8_invalid";
}

bool DecimalFloatDescriptorValidForPresent(
    const ExecutionTypeDescriptor& descriptor) {
  return ExecutionDescriptorExactlyMatchesCurrentBuiltin(
      descriptor, CanonicalTypeId::decimal_float);
}

bool DecimalFloatCarrierStructurallyCanonical(std::string_view encoded) {
  return scratchbird::libraries::sbl_numeric::DecodeDecimal128LittleEndian(
             reinterpret_cast<const std::uint8_t*>(encoded.data()),
             encoded.size(), true)
      .value.has_value();
}

bool DecimalDescriptorValidForPresent(
    const ExecutionTypeDescriptor& descriptor) {
  return !DescriptorHasDomainBinding(descriptor) &&
      ExecutionDescriptorValidForType(descriptor, CanonicalTypeId::decimal);
}

bool DecimalCarrierStructurallyCanonical(std::string_view encoded) {
  return scratchbird::libraries::sbl_numeric::DecodeExactDecimalLittleEndian(
             reinterpret_cast<const std::uint8_t*>(encoded.data()),
             encoded.size())
      .ok;
}

std::size_t PolicyRestrictedRealCarrierByteWidth(
    CanonicalTypeId type_id) noexcept {
  if (type_id == CanonicalTypeId::real128) return 16u;
  if (type_id == CanonicalTypeId::real64) return 8u;
  return type_id == CanonicalTypeId::real32 ? 4u : 2u;
}

bool DecodeReal128Carrier(
    std::string_view encoded,
    scratchbird::libraries::sbl_numeric::Real128Bytes* bytes) noexcept {
  if (bytes == nullptr || encoded.size() != bytes->size()) return false;
  std::copy(encoded.begin(), encoded.end(), bytes->begin());
  return true;
}

std::string EncodeReal128Carrier(
    const scratchbird::libraries::sbl_numeric::Real128Bytes& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

CanonicalTypeId SelectUnresolvedRealType(
    CanonicalTypeId first,
    CanonicalTypeId second = CanonicalTypeId::unknown,
    CanonicalTypeId third = CanonicalTypeId::unknown) noexcept {
  // Preserve the established REAL16-before-BFLOAT16 diagnostic selection when
  // malformed requests mention more than one unresolved real family. REAL32
  // and REAL64 are appended without changing that existing ordering.
  for (const auto candidate : {first, second, third}) {
    if (candidate == CanonicalTypeId::real16) {
      return CanonicalTypeId::real16;
    }
  }
  for (const auto candidate : {first, second, third}) {
    if (candidate == CanonicalTypeId::bfloat16) {
      return CanonicalTypeId::bfloat16;
    }
  }
  for (const auto candidate : {first, second, third}) {
    if (candidate == CanonicalTypeId::real32) {
      return CanonicalTypeId::real32;
    }
  }
  for (const auto candidate : {first, second, third}) {
    if (candidate == CanonicalTypeId::real64) {
      return CanonicalTypeId::real64;
    }
  }
  return CanonicalTypeId::unknown;
}

const char* UnresolvedRealDetail(CanonicalTypeId type_id,
                                 const char* bfloat16_detail,
                                 const char* real16_detail,
                                 const char* real32_detail,
                                 const char* real64_detail) noexcept {
  if (type_id == CanonicalTypeId::real16) return real16_detail;
  if (type_id == CanonicalTypeId::real32) return real32_detail;
  if (type_id == CanonicalTypeId::real64) return real64_detail;
  return bfloat16_detail;
}

bool ValidCastContext(DatatypeCastContext context) noexcept {
  switch (context) {
    case DatatypeCastContext::implicit:
    case DatatypeCastContext::assignment:
    case DatatypeCastContext::explicit_cast:
      return true;
  }
  return false;
}

const char* GenericIntervalStructuralDiagnosticCode(
    const DatatypeOperationValue& value);

bool CanonicalOperationValueValid(const DatatypeOperationValue& value) {
  if (value.type_id == CanonicalTypeId::unknown ||
      value.type_id == CanonicalTypeId::null_type) {
    return false;
  }
  if (!LookupDatatypeDescriptor(value.type_id).ok()) {
    return false;
  }
  // The generic carrier has no current receipt or complete temporal profile.
  if (value.type_id == CanonicalTypeId::date ||
      value.type_id == CanonicalTypeId::time ||
      value.type_id == CanonicalTypeId::timestamp ||
      value.type_id == CanonicalTypeId::interval)
    return false;
  if (value.is_null) {
    return value.encoded_value.empty() &&
        ExecutionDescriptorValidForType(value.descriptor, value.type_id) &&
        value.descriptor.nullable_allowed;
  }
  if (ExecutionDescriptorPresent(value.descriptor) &&
      !ExecutionDescriptorValidForType(value.descriptor, value.type_id)) {
    return false;
  }
  // Core fixes exact structural carriers for BFLOAT16, REAL16, REAL32, and
  // REAL64 without completing their scalar semantic policies. Keep PRESENT
  // values out of DatatypeOperationValue until those policies exist; typed
  // NULL remains governed by the descriptor/null-state branch above.
  if (IsUnresolvedRealSemantics(value.type_id)) return false;
  if (IsDecimal(value.type_id)) {
    return DecimalDescriptorValidForPresent(value.descriptor) &&
        DecimalCarrierStructurallyCanonical(value.encoded_value);
  }
  if (IsDecimalFloat(value.type_id)) {
    return DecimalFloatDescriptorValidForPresent(value.descriptor) &&
        DecimalFloatCarrierStructurallyCanonical(value.encoded_value);
  }
  if (value.type_id == CanonicalTypeId::real128) {
    return ExecutionDescriptorExactlyMatchesCurrentBuiltin(
               value.descriptor, value.type_id) &&
        value.encoded_value.size() == 16;
  }
  if (IsUuid(value.type_id)) {
    return ExecutionDescriptorExactlyMatchesCurrentBuiltin(
               value.descriptor, value.type_id) &&
        value.encoded_value.size() == 16;
  }
  if (IsIpAddress(value.type_id)) {
    return ExecutionDescriptorExactlyMatchesCurrentBuiltin(
               value.descriptor, value.type_id) &&
        value.encoded_value.size() == 16;
  }
  if (IsNetworkPrefix(value.type_id)) {
    return ExecutionDescriptorExactlyMatchesCurrentBuiltin(
               value.descriptor, value.type_id) &&
        value.encoded_value.size() == 18;
  }
  if (IsMacAddress(value.type_id)) {
    return ExecutionDescriptorExactlyMatchesCurrentBuiltin(
               value.descriptor, value.type_id) &&
        value.encoded_value.size() == 8;
  }
  if (IsBinary(value.type_id)) {
    return CanonicalBinaryValueValid(value);
  }
  if (value.type_id == CanonicalTypeId::boolean) {
    return value.encoded_value.size() == 1 &&
        (static_cast<unsigned char>(value.encoded_value[0]) == 0u ||
         static_cast<unsigned char>(value.encoded_value[0]) == 1u);
  }
  if (value.type_id == CanonicalTypeId::int8 ||
      value.type_id == CanonicalTypeId::uint8) {
    // Every one-byte pattern is one canonical INT8 or UINT8 value.
    // Text is accepted only at cast and display boundaries.
    return value.encoded_value.size() == 1;
  }
  if (value.type_id == CanonicalTypeId::int16 ||
      value.type_id == CanonicalTypeId::uint16 ||
      value.type_id == CanonicalTypeId::int32 ||
      value.type_id == CanonicalTypeId::uint32 ||
      value.type_id == CanonicalTypeId::int64 ||
      value.type_id == CanonicalTypeId::uint64 ||
      value.type_id == CanonicalTypeId::int128 ||
      value.type_id == CanonicalTypeId::uint128) {
    // Every fixed-width pattern is one canonical little-endian integer value.
    // Signedness belongs to the descriptor.
    if (value.type_id == CanonicalTypeId::int64 ||
        value.type_id == CanonicalTypeId::uint64) {
      return value.encoded_value.size() == 8;
    }
    if (value.type_id == CanonicalTypeId::int128 ||
        value.type_id == CanonicalTypeId::uint128) {
      return ExecutionDescriptorPresent(value.descriptor) &&
          ExecutionDescriptorValidForType(value.descriptor, value.type_id) &&
          value.encoded_value.size() == 16;
    }
    return value.encoded_value.size() ==
        ((value.type_id == CanonicalTypeId::int32 ||
          value.type_id == CanonicalTypeId::uint32) ? 4 : 2);
  }
  return !IsCharacter(value.type_id) ||
      CanonicalCharacterPayloadValid(value.encoded_value);
}

const char* CanonicalOperationValueDiagnosticCode(
    const DatatypeOperationValue& value,
    const char* owning_operation_code = "DATATYPE.DESCRIPTOR.INVALID") {
  if (value.type_id == CanonicalTypeId::unknown ||
      value.type_id == CanonicalTypeId::null_type) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (value.type_id == CanonicalTypeId::interval) {
    if (const char* code = GenericIntervalStructuralDiagnosticCode(value))
      return code;
    return "CTI.INTERVAL.DESCRIPTOR_INVALID";
  }
  if (IsUuid(value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (IsIpAddress(value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (IsNetworkPrefix(value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (IsMacAddress(value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (IsBinary(value.type_id) &&
      !BinaryDescriptorExactlyCurrent(value.descriptor)) {
    return "CTB.BINARY.DESCRIPTOR_INVALID";
  }
  if (IsBinary(value.type_id) && !value.is_null &&
      value.encoded_value.size() > kCanonicalBinaryMaximumBytes) {
    return "CTB.BINARY.LENGTH_EXCEEDED";
  }
  if (IsCharacter(value.type_id) && !value.is_null &&
      value.encoded_value.size() > kCanonicalCharacterMaximumBytes) {
    return "CTB.TEXT.LENGTH_EXCEEDED";
  }
  if (value.is_null &&
      !ExecutionDescriptorValidForType(value.descriptor, value.type_id)) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (value.is_null && !value.encoded_value.empty()) {
    return "DATATYPE.NULL_STATE.INVALID";
  }
  if (value.is_null && !value.descriptor.nullable_allowed) {
    return "DATATYPE.NULL_NOT_ADMITTED";
  }
  if (value.type_id == CanonicalTypeId::date ||
      value.type_id == CanonicalTypeId::time ||
      value.type_id == CanonicalTypeId::timestamp) {
    return "CTI.TEMPORAL.DESCRIPTOR_INVALID";
  }
  if (IsUnresolvedRealSemantics(value.type_id) && !value.is_null &&
      (!ExecutionDescriptorPresent(value.descriptor) ||
       !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                        value.type_id))) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (value.type_id == CanonicalTypeId::real128 && !value.is_null &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (value.type_id == CanonicalTypeId::real128 && !value.is_null &&
      value.encoded_value.size() != 16) {
    return "NUMERIC.ENCODING.NONCANONICAL";
  }
  if (IsDecimal(value.type_id) && !value.is_null &&
      !DecimalDescriptorValidForPresent(value.descriptor)) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (IsDecimal(value.type_id) && !value.is_null &&
      !DecimalCarrierStructurallyCanonical(value.encoded_value)) {
    return "NUMERIC.ENCODING.NONCANONICAL";
  }
  if (IsDecimalFloat(value.type_id) && !value.is_null &&
      !DecimalFloatDescriptorValidForPresent(value.descriptor)) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  if (IsDecimalFloat(value.type_id) && !value.is_null &&
      !DecimalFloatCarrierStructurallyCanonical(value.encoded_value)) {
    return "NUMERIC.ENCODING.NONCANONICAL";
  }
  if (ExecutionDescriptorPresent(value.descriptor) &&
      !ExecutionDescriptorValidForType(value.descriptor, value.type_id)) {
    return "DATATYPE.DESCRIPTOR.INVALID";
  }
  return owning_operation_code;
}

const char* GenericBitStringStructuralDiagnosticCode(
    const DatatypeOperationValue& value) {
  if (value.type_id != CanonicalTypeId::bit_string) return nullptr;
  if (ExecutionDescriptorPresent(value.descriptor) &&
      !ExecutionDescriptorValidForType(value.descriptor, value.type_id))
    return "CTB.BIT.DESCRIPTOR_INVALID";
  if (value.is_null && !value.encoded_value.empty())
    return "DATATYPE.NULL_STATE.INVALID";
  if (value.is_null && ExecutionDescriptorPresent(value.descriptor) &&
      !value.descriptor.nullable_allowed)
    return "DATATYPE.NULL_NOT_ADMITTED";
  return nullptr;
}

const char* GenericDateStructuralDiagnosticCode(
    const DatatypeOperationValue& value) {
  if (value.type_id != CanonicalTypeId::date) return nullptr;
  if (ExecutionDescriptorPresent(value.descriptor) &&
      !ExecutionDescriptorValidForType(value.descriptor, value.type_id))
    return "CTI.TEMPORAL.DESCRIPTOR_INVALID";
  if (value.is_null && !value.encoded_value.empty())
    return "DATATYPE.NULL_STATE.INVALID";
  if (value.is_null && ExecutionDescriptorPresent(value.descriptor) &&
      !value.descriptor.nullable_allowed)
    return "DATATYPE.NULL_NOT_ADMITTED";
  return nullptr;
}

const char* GenericTimeStructuralDiagnosticCode(
    const DatatypeOperationValue& value) {
  if (value.type_id != CanonicalTypeId::time) return nullptr;
  if (ExecutionDescriptorPresent(value.descriptor) &&
      !ExecutionDescriptorValidForType(value.descriptor, value.type_id))
    return "CTI.TEMPORAL.DESCRIPTOR_INVALID";
  if (value.is_null && !value.encoded_value.empty())
    return "DATATYPE.NULL_STATE.INVALID";
  if (value.is_null && ExecutionDescriptorPresent(value.descriptor) &&
      !value.descriptor.nullable_allowed)
    return "DATATYPE.NULL_NOT_ADMITTED";
  return nullptr;
}

const char* GenericTimestampStructuralDiagnosticCode(
    const DatatypeOperationValue& value) {
  if (value.type_id != CanonicalTypeId::timestamp) return nullptr;
  if (ExecutionDescriptorPresent(value.descriptor) &&
      !ExecutionDescriptorValidForType(value.descriptor, value.type_id))
    return "CTI.TEMPORAL.DESCRIPTOR_INVALID";
  if (value.is_null && !value.encoded_value.empty())
    return "DATATYPE.NULL_STATE.INVALID";
  if (value.is_null && ExecutionDescriptorPresent(value.descriptor) &&
      !value.descriptor.nullable_allowed)
    return "DATATYPE.NULL_NOT_ADMITTED";
  return nullptr;
}

const char* GenericIntervalStructuralDiagnosticCode(
    const DatatypeOperationValue& value) {
  if (value.type_id != CanonicalTypeId::interval) return nullptr;
  if (ExecutionDescriptorPresent(value.descriptor) &&
      !ExecutionDescriptorValidForType(value.descriptor, value.type_id))
    return "CTI.INTERVAL.DESCRIPTOR_INVALID";
  if (value.is_null && !value.encoded_value.empty())
    return "DATATYPE.NULL_STATE.INVALID";
  if (value.is_null && ExecutionDescriptorPresent(value.descriptor) &&
      !value.descriptor.nullable_allowed)
    return "DATATYPE.NULL_NOT_ADMITTED";
  return nullptr;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

bool LegacySbdv1EncodedTypeIs(std::string_view encoded,
                              std::string_view wanted) noexcept {
  constexpr std::string_view kMagic = "SBDV1;";
  if (!encoded.starts_with(kMagic)) return false;
  encoded.remove_prefix(kMagic.size());
  while (!encoded.empty()) {
    const auto separator = encoded.find(';');
    const auto field = encoded.substr(0, separator);
    constexpr std::string_view kTypeKey = "type=";
    if (field.starts_with(kTypeKey)) {
      const auto value = field.substr(kTypeKey.size());
      if (value.size() == wanted.size()) {
        bool equal = true;
        for (std::size_t index = 0; index < value.size(); ++index) {
          const auto current = static_cast<unsigned char>(value[index]);
          if (static_cast<char>(std::tolower(current)) != wanted[index]) {
            equal = false;
            break;
          }
        }
        if (equal) return true;
      }
    }
    if (separator == std::string_view::npos) break;
    encoded.remove_prefix(separator + 1);
  }
  return false;
}

bool TextSeedReady(const DatatypeTextSeedAuthority& seed) {
  return seed.active && uuid::IsEngineIdentityUuid(seed.database_uuid) &&
      uuid::IsEngineIdentityUuid(seed.charset_uuid) &&
      uuid::IsEngineIdentityUuid(seed.collation_uuid) &&
      seed.resource_epoch != 0 && seed.collation_epoch != 0 &&
      seed.comparison_profile != resources::CollationProfile::unbound &&
      resources::ValidCollationProfile(seed.comparison_profile,
          seed.collation_case_insensitive, seed.collation_accent_insensitive) &&
      (resources::UsesUnicodeRoot(seed.comparison_profile)
          ? bool(seed.unicode_collation) : !seed.unicode_collation);
}

bool MakeTextComparisonKey(const DatatypeTextSeedAuthority& seed,
                           const std::string& value, std::string* key) {
  if (!TextSeedReady(seed)) return false;
  if (seed.comparison_profile == resources::CollationProfile::utf8_binary) {
    *key = value;
    return true;
  }
  const auto strength = static_cast<resources::UnicodeCollationStrength>(
      static_cast<std::uint64_t>(seed.comparison_profile) - 1);
  // Bound output by representable string storage. Execution owners retain
  // their operation memory admission; this primitive never silently truncates.
  return seed.unicode_collation->MakeSortKey(value, strength,
      {key->max_size(), key->max_size()}, key) == resources::UnicodeNormalizationStatus::ok;
}

std::string TextComparisonCohort(const DatatypeTextSeedAuthority& seed) {
  std::string bytes = "20:";
  for (const auto* id : {&seed.database_uuid, &seed.charset_uuid, &seed.collation_uuid})
    bytes.append(reinterpret_cast<const char*>(id->bytes.data()), id->bytes.size());
  for (const auto value : {seed.resource_epoch, seed.collation_epoch,
                           static_cast<std::uint64_t>(seed.comparison_profile)})
    for (unsigned n = 0; n < 8; ++n) bytes.push_back(static_cast<char>(value >> (8 * n)));
  return bytes;
}

std::string TextSeedDetail(const DatatypeTextSeedAuthority& seed) {
  return "seed_pack=" + seed.seed_pack_name +
         ";seed_version=" + seed.seed_pack_version +
         ";charset=" + seed.charset_name +
         ";collation=" + seed.collation_name;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

std::string HexEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

bool IsSignedInteger(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::int8 || type_id == CanonicalTypeId::int16 ||
         type_id == CanonicalTypeId::int32 || type_id == CanonicalTypeId::int64 ||
         type_id == CanonicalTypeId::int128;
}

bool IsUnsignedInteger(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::uint8 || type_id == CanonicalTypeId::uint16 ||
         type_id == CanonicalTypeId::uint32 || type_id == CanonicalTypeId::uint64 ||
         type_id == CanonicalTypeId::uint128;
}

bool IsInteger(CanonicalTypeId type_id) {
  return IsSignedInteger(type_id) || IsUnsignedInteger(type_id);
}

bool IsCanonical128Integer(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::int128 ||
         type_id == CanonicalTypeId::uint128;
}

bool IsReal(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::real16 || type_id == CanonicalTypeId::bfloat16 ||
         type_id == CanonicalTypeId::real32 || type_id == CanonicalTypeId::real64 ||
         type_id == CanonicalTypeId::real128;
}

bool IsNumeric(CanonicalTypeId type_id) {
  return IsInteger(type_id) || IsReal(type_id) || type_id == CanonicalTypeId::decimal ||
         type_id == CanonicalTypeId::decimal_float;
}

bool IsTemporal(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::date || type_id == CanonicalTypeId::time ||
         type_id == CanonicalTypeId::timestamp;
}

bool IsBinaryLike(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::binary || type_id == CanonicalTypeId::blob;
}

bool IsDocument(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::document || type_id == CanonicalTypeId::json_document ||
         type_id == CanonicalTypeId::binary_json_document || type_id == CanonicalTypeId::bson_document ||
         type_id == CanonicalTypeId::xml_document || type_id == CanonicalTypeId::hstore_document ||
         type_id == CanonicalTypeId::object_document || type_id == CanonicalTypeId::flattened_object_document;
}

bool IsOpaqueRenderOnly(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::opaque_extension;
}

bool DisplayAsMetadataSummary(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::blob:
    case CanonicalTypeId::binary_json_document:
    case CanonicalTypeId::bson_document:
    case CanonicalTypeId::raster:
    case CanonicalTypeId::columnar_segment:
    case CanonicalTypeId::aggregate_state:
    case CanonicalTypeId::hll_sketch:
    case CanonicalTypeId::bloom_filter:
    case CanonicalTypeId::quantile_sketch:
    case CanonicalTypeId::histogram_sketch:
    case CanonicalTypeId::ranking_summary:
    case CanonicalTypeId::vector_summary:
    case CanonicalTypeId::lob_locator:
    case CanonicalTypeId::external_file_locator:
    case CanonicalTypeId::remote_object_locator:
    case CanonicalTypeId::bridge_handle:
    case CanonicalTypeId::cursor_handle:
    case CanonicalTypeId::system_reference:
    case CanonicalTypeId::opaque_extension:
    case CanonicalTypeId::cursor:
    case CanonicalTypeId::result_set:
    case CanonicalTypeId::table_value:
      return true;
    default:
      return false;
  }
}

bool DisplayAsStructuredText(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::document:
    case CanonicalTypeId::json_document:
    case CanonicalTypeId::xml_document:
    case CanonicalTypeId::hstore_document:
    case CanonicalTypeId::object_document:
    case CanonicalTypeId::flattened_object_document:
    case CanonicalTypeId::set_value:
    case CanonicalTypeId::array:
    case CanonicalTypeId::list:
    case CanonicalTypeId::map:
    case CanonicalTypeId::row:
    case CanonicalTypeId::composite:
    case CanonicalTypeId::variant:
    case CanonicalTypeId::range:
    case CanonicalTypeId::multirange:
    case CanonicalTypeId::token_stream:
    case CanonicalTypeId::search_query:
    case CanonicalTypeId::search_rank_feature:
    case CanonicalTypeId::search_completion:
    case CanonicalTypeId::search_percolator:
    case CanonicalTypeId::geometry:
    case CanonicalTypeId::geography:
    case CanonicalTypeId::point:
    case CanonicalTypeId::shape:
    case CanonicalTypeId::vector:
    case CanonicalTypeId::dense_vector:
    case CanonicalTypeId::sparse_vector:
    case CanonicalTypeId::binary_vector:
    case CanonicalTypeId::quantized_vector:
    case CanonicalTypeId::graph_node:
    case CanonicalTypeId::graph_edge:
    case CanonicalTypeId::graph_path:
    case CanonicalTypeId::time_series_value:
      return true;
    default:
      return false;
  }
}

std::string DisplayMetadataSummary(CanonicalTypeId type_id, std::size_t payload_size) {
  return std::string("<") + CanonicalTypeName(type_id) + ":" +
         std::to_string(payload_size) + " bytes>";
}

std::uint32_t IntegerBits(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::int8:
    case CanonicalTypeId::uint8: return 8;
    case CanonicalTypeId::int16:
    case CanonicalTypeId::uint16: return 16;
    case CanonicalTypeId::int32:
    case CanonicalTypeId::uint32: return 32;
    case CanonicalTypeId::int64:
    case CanonicalTypeId::uint64: return 64;
    case CanonicalTypeId::int128:
    case CanonicalTypeId::uint128: return 128;
    default: return 0;
  }
}

std::string TrimLeadingZeros(std::string digits) {
  const bool negative = !digits.empty() && digits.front() == '-';
  const bool signed_prefix =
      !digits.empty() && (digits.front() == '-' || digits.front() == '+');
  std::size_t first = signed_prefix ? 1 : 0;
  while (first + 1 < digits.size() && digits[first] == '0') { ++first; }
  std::string normalized = digits.substr(first);
  if (normalized.empty()) { normalized = "0"; }
  if (negative && normalized != "0") { normalized.insert(normalized.begin(), '-'); }
  return normalized;
}

bool DecimalIntegerText(std::string_view value, bool allow_negative) {
  if (value.empty()) { return false; }
  std::size_t start = 0;
  if (value.front() == '-') {
    if (!allow_negative) { return false; }
    start = 1;
  } else if (value.front() == '+') {
    start = 1;
  }
  if (start == value.size()) { return false; }
  for (std::size_t i = start; i < value.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(value[i]))) { return false; }
  }
  return true;
}

int CompareUnsignedDecimal(std::string left, std::string right) {
  left = TrimLeadingZeros(std::move(left));
  right = TrimLeadingZeros(std::move(right));
  if (left.size() < right.size()) { return -1; }
  if (left.size() > right.size()) { return 1; }
  if (left < right) { return -1; }
  if (left > right) { return 1; }
  return 0;
}

std::string AbsoluteDecimal(std::string value) {
  if (!value.empty() && (value.front() == '-' || value.front() == '+')) { value.erase(value.begin()); }
  return TrimLeadingZeros(std::move(value));
}

bool IntegerFits(CanonicalTypeId target_type_id, const std::string& value) {
  if (!DecimalIntegerText(value, IsSignedInteger(target_type_id))) { return false; }
  const bool negative = !value.empty() && value.front() == '-';
  if (IsUnsignedInteger(target_type_id) && negative) { return false; }
  switch (target_type_id) {
    case CanonicalTypeId::int8:
    case CanonicalTypeId::int16:
    case CanonicalTypeId::int32:
    case CanonicalTypeId::int64: {
      try {
        std::size_t pos = 0;
        const long long parsed = std::stoll(value, &pos);
        if (pos != value.size()) { return false; }
        if (target_type_id == CanonicalTypeId::int8) {
          return parsed >= std::numeric_limits<std::int8_t>::min() && parsed <= std::numeric_limits<std::int8_t>::max();
        }
        if (target_type_id == CanonicalTypeId::int16) {
          return parsed >= std::numeric_limits<std::int16_t>::min() && parsed <= std::numeric_limits<std::int16_t>::max();
        }
        if (target_type_id == CanonicalTypeId::int32) {
          return parsed >= std::numeric_limits<std::int32_t>::min() && parsed <= std::numeric_limits<std::int32_t>::max();
        }
        return true;
      } catch (...) {
        return false;
      }
    }
    case CanonicalTypeId::uint8:
    case CanonicalTypeId::uint16:
    case CanonicalTypeId::uint32:
    case CanonicalTypeId::uint64: {
      try {
        std::size_t pos = 0;
        const unsigned long long parsed = std::stoull(value, &pos);
        if (pos != value.size()) { return false; }
        if (target_type_id == CanonicalTypeId::uint8) { return parsed <= std::numeric_limits<std::uint8_t>::max(); }
        if (target_type_id == CanonicalTypeId::uint16) { return parsed <= std::numeric_limits<std::uint16_t>::max(); }
        if (target_type_id == CanonicalTypeId::uint32) { return parsed <= std::numeric_limits<std::uint32_t>::max(); }
        return true;
      } catch (...) {
        return false;
      }
    }
    case CanonicalTypeId::int128:
    case CanonicalTypeId::uint128: {
      // Wide numeric range authority belongs to the portable numeric backend.
      // The lexical check above retains this operation API's admission rules.
      libraries::sbl_numeric::NumericRequest request;
      request.type = target_type_id == CanonicalTypeId::int128
          ? libraries::sbl_numeric::NumericType::int128
          : libraries::sbl_numeric::NumericType::uint128;
      request.left = {request.type, value, false};
      return libraries::sbl_numeric::ApplyNumericOperation(request).status ==
          libraries::sbl_numeric::NumericStatusCode::ok;
    }
    default:
      return false;
  }
}

std::string Int8DecimalText(std::string_view value) {
  if (value.size() != 1) return {};
  const unsigned raw = static_cast<unsigned char>(value[0]);
  const int decoded = raw < 0x80u ? static_cast<int>(raw)
                                  : static_cast<int>(raw) - 0x100;
  return std::to_string(decoded);
}

std::string Uint8DecimalText(std::string_view value) {
  if (value.size() != 1) return {};
  return std::to_string(static_cast<unsigned char>(value[0]));
}

bool EncodeInt8DecimalText(std::string_view value, std::string* encoded) {
  if (encoded == nullptr ||
      !IntegerFits(CanonicalTypeId::int8, std::string(value))) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const long parsed = std::stol(std::string(value), &consumed, 10);
    if (consumed != value.size()) return false;
    encoded->assign(1, static_cast<char>(static_cast<unsigned char>(parsed)));
    return true;
  } catch (...) {
    return false;
  }
}

bool EncodeUint8DecimalText(std::string_view value, std::string* encoded) {
  if (encoded == nullptr ||
      !IntegerFits(CanonicalTypeId::uint8, std::string(value))) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(std::string(value), &consumed, 10);
    if (consumed != value.size()) return false;
    encoded->assign(1, static_cast<char>(static_cast<unsigned char>(parsed)));
    return true;
  } catch (...) {
    return false;
  }
}

bool EncodeExactIntegralNumericTextAsByte(std::string_view value,
                                          bool signed_target,
                                          std::string* encoded);

bool EncodeIntegralNumericTextAsInt8(std::string_view value,
                                     std::string* encoded) {
  return EncodeExactIntegralNumericTextAsByte(value, true, encoded);
}

std::string IntegerOperationText(CanonicalTypeId type_id,
                                 std::string_view value) {
  if (type_id == CanonicalTypeId::int8) return Int8DecimalText(value);
  if (type_id == CanonicalTypeId::uint8) return Uint8DecimalText(value);
  if (type_id == CanonicalTypeId::int128) {
    std::string decoded;
    return DecodeCanonicalInt128Value(value, &decoded) ? decoded : std::string{};
  }
  if (type_id == CanonicalTypeId::uint128) {
    std::string decoded;
    return DecodeCanonicalUint128Value(value, &decoded) ? decoded : std::string{};
  }
  return std::string(value);
}

bool IntegerOperationValueValid(CanonicalTypeId type_id,
                                std::string_view value) {
  if (type_id == CanonicalTypeId::int8 ||
      type_id == CanonicalTypeId::uint8) {
    return value.size() == 1;
  }
  if (type_id == CanonicalTypeId::int16 ||
      type_id == CanonicalTypeId::uint16) return value.size() == 2;
  if (type_id == CanonicalTypeId::int32 ||
      type_id == CanonicalTypeId::uint32) return value.size() == 4;
  if (type_id == CanonicalTypeId::int64 ||
      type_id == CanonicalTypeId::uint64) return value.size() == 8;
  if (IsCanonical128Integer(type_id)) return value.size() == 16;
  return IntegerFits(type_id, std::string(value));
}

bool FloatingText(std::string_view value) {
  if (value.empty()) { return false; }
  try {
    std::size_t pos = 0;
    (void)std::stold(std::string(value), &pos);
    return pos == value.size();
  } catch (...) {
    return false;
  }
}

std::string TrimAsciiWhitespace(std::string value);

using U128 = unsigned __int128;
using I128 = __int128_t;

struct ParsedDecimal {
  bool negative = false;
  bool negative_zero = false;
  U128 coefficient = 0;
  std::uint32_t scale = 0;
};

enum class NumericSpecialKind {
  finite,
  quiet_nan,
  signaling_nan,
  positive_infinity,
  negative_infinity
};

struct ParsedSpecialNumeric {
  NumericSpecialKind kind = NumericSpecialKind::finite;
  bool negative_zero = false;
  std::string canonical;
};

U128 Pow10U128(std::uint32_t exponent) {
  U128 value = 1;
  for (std::uint32_t i = 0; i < exponent; ++i) { value *= 10; }
  return value;
}

std::string U128ToString(U128 value) {
  if (value == 0) { return "0"; }
  std::string out;
  while (value != 0) {
    const int digit = static_cast<int>(value % 10);
    out.push_back(static_cast<char>('0' + digit));
    value /= 10;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::uint32_t U128DigitCount(U128 value) {
  return static_cast<std::uint32_t>(U128ToString(value).size());
}

bool ParseU128DecimalDigits(const std::string& digits, U128* out) {
  U128 value = 0;
  for (char c : digits) {
    if (!std::isdigit(static_cast<unsigned char>(c))) { return false; }
    value = (value * 10) + static_cast<U128>(c - '0');
  }
  *out = value;
  return true;
}

bool ParseSignedDecimalExponent(const std::string& value, long long* out) {
  if (value.empty()) { return false; }
  std::size_t pos = 0;
  bool negative = false;
  if (value[pos] == '+' || value[pos] == '-') {
    negative = value[pos] == '-';
    ++pos;
  }
  if (pos == value.size()) { return false; }
  long long parsed = 0;
  for (; pos < value.size(); ++pos) {
    if (!std::isdigit(static_cast<unsigned char>(value[pos]))) { return false; }
    parsed = (parsed * 10) + (value[pos] - '0');
    if (parsed > 10000) { return false; }
  }
  *out = negative ? -parsed : parsed;
  return true;
}

bool ParseDecimalFiniteText(const std::string& input, ParsedDecimal* out) {
  std::string value = TrimAsciiWhitespace(input);
  if (value.empty()) { return false; }
  ParsedDecimal parsed;
  if (value.front() == '+' || value.front() == '-') {
    parsed.negative = value.front() == '-';
    value.erase(value.begin());
  }
  if (value.empty()) { return false; }
  const std::size_t exponent_pos = value.find_first_of("eE");
  std::string significand = exponent_pos == std::string::npos ? value : value.substr(0, exponent_pos);
  long long exponent = 0;
  if (exponent_pos != std::string::npos &&
      !ParseSignedDecimalExponent(value.substr(exponent_pos + 1), &exponent)) {
    return false;
  }
  const std::size_t decimal_pos = significand.find('.');
  if (decimal_pos != std::string::npos && significand.find('.', decimal_pos + 1) != std::string::npos) {
    return false;
  }
  std::string digits;
  std::uint32_t fractional_digits = 0;
  bool saw_digit = false;
  for (std::size_t i = 0; i < significand.size(); ++i) {
    const char c = significand[i];
    if (c == '.') { continue; }
    if (!std::isdigit(static_cast<unsigned char>(c))) { return false; }
    saw_digit = true;
    digits.push_back(c);
    if (decimal_pos != std::string::npos && i > decimal_pos) { ++fractional_digits; }
  }
  if (!saw_digit) { return false; }
  while (!digits.empty() && digits.front() == '0') { digits.erase(digits.begin()); }
  long long final_scale = static_cast<long long>(fractional_digits) - exponent;
  if (digits.empty()) {
    parsed.coefficient = 0;
    parsed.scale = 0;
    parsed.negative_zero = parsed.negative;
    *out = parsed;
    return true;
  }
  if (final_scale < 0) {
    const long long zeros = -final_scale;
    if (zeros > 38 || digits.size() + static_cast<std::size_t>(zeros) > 38) { return false; }
    digits.append(static_cast<std::size_t>(zeros), '0');
    final_scale = 0;
  }
  if (digits.size() > 38 || final_scale > 10000) { return false; }
  if (!ParseU128DecimalDigits(digits, &parsed.coefficient)) { return false; }
  parsed.scale = static_cast<std::uint32_t>(final_scale);
  parsed.negative_zero = parsed.negative && parsed.coefficient == 0;
  *out = parsed;
  return true;
}

bool EncodeExactIntegralNumericTextAsByte(std::string_view value,
                                          bool signed_target,
                                          std::string* encoded) {
  if (encoded == nullptr || value.empty() ||
      std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
      })) {
    return false;
  }
  ParsedDecimal parsed;
  if (!ParseDecimalFiniteText(std::string(value), &parsed) ||
      (!signed_target && parsed.negative)) {
    return false;
  }
  U128 magnitude = parsed.coefficient;
  if (parsed.scale != 0) {
    if (parsed.scale > 38) return false;
    const U128 divisor = Pow10U128(parsed.scale);
    if (magnitude % divisor != 0) return false;
    magnitude /= divisor;
  }
  if (signed_target) {
    const U128 limit = parsed.negative ? U128{128} : U128{127};
    if (magnitude > limit) return false;
    const int signed_value = parsed.negative
        ? -static_cast<int>(magnitude)
        : static_cast<int>(magnitude);
    encoded->assign(
        1, static_cast<char>(static_cast<unsigned char>(signed_value)));
    return true;
  }
  if (magnitude > U128{255}) return false;
  encoded->assign(
      1, static_cast<char>(static_cast<unsigned char>(magnitude)));
  return true;
}

bool RoundDecimalToScale(ParsedDecimal* value, std::uint32_t target_scale, DatatypeRoundingMode rounding) {
  if (value->coefficient == 0) {
    value->scale = target_scale;
    return true;
  }
  if (value->scale == target_scale) { return true; }
  if (value->scale < target_scale) {
    const std::uint32_t diff = target_scale - value->scale;
    if (diff > 38) { return false; }
    value->coefficient *= Pow10U128(diff);
    value->scale = target_scale;
    return true;
  }
  const std::uint32_t diff = value->scale - target_scale;
  if (diff > 38) { return false; }
  const U128 divisor = Pow10U128(diff);
  U128 quotient = value->coefficient / divisor;
  const U128 remainder = value->coefficient % divisor;
  bool increment = false;
  if (rounding != DatatypeRoundingMode::truncate && remainder != 0) {
    const U128 half = divisor / 2;
    if (remainder > half) {
      increment = true;
    } else if (remainder == half) {
      increment = rounding == DatatypeRoundingMode::half_up ||
                  (rounding == DatatypeRoundingMode::half_even && (quotient % 2) != 0);
    }
  }
  if (increment) { ++quotient; }
  value->coefficient = quotient;
  value->scale = target_scale;
  return true;
}

std::string RenderFixedDecimal(const ParsedDecimal& value) {
  std::string digits = U128ToString(value.coefficient);
  if (value.scale > 0) {
    if (digits.size() <= value.scale) {
      digits.insert(digits.begin(), static_cast<std::size_t>(value.scale - digits.size() + 1), '0');
    }
    digits.insert(digits.end() - static_cast<std::ptrdiff_t>(value.scale), '.');
  }
  if (value.negative && value.coefficient != 0) { digits.insert(digits.begin(), '-'); }
  return digits;
}

bool CanonicalizeExactDecimalText(const std::string& input,
                                  const DatatypeNumericContext& context,
                                  std::string* out) {
  if (context.precision == 0 || context.precision > 38 || context.scale > context.precision) { return false; }
  ParsedDecimal parsed;
  if (!ParseDecimalFiniteText(input, &parsed)) { return false; }
  if (!RoundDecimalToScale(&parsed, context.scale, context.rounding)) { return false; }
  if (U128DigitCount(parsed.coefficient) > context.precision) { return false; }
  *out = RenderFixedDecimal(parsed);
  return true;
}

ParsedSpecialNumeric ParseSpecialNumericText(const std::string& input) {
  const std::string value = LowerAscii(TrimAsciiWhitespace(input));
  ParsedSpecialNumeric parsed;
  if (value == "nan" || value == "+nan" || value == "-nan") {
    parsed.kind = NumericSpecialKind::quiet_nan;
    parsed.canonical = "NaN";
    return parsed;
  }
  if (value == "snan" || value == "+snan" || value == "-snan") {
    parsed.kind = NumericSpecialKind::signaling_nan;
    parsed.canonical = "sNaN";
    return parsed;
  }
  if (value == "inf" || value == "+inf" || value == "infinity" || value == "+infinity") {
    parsed.kind = NumericSpecialKind::positive_infinity;
    parsed.canonical = "Infinity";
    return parsed;
  }
  if (value == "-inf" || value == "-infinity") {
    parsed.kind = NumericSpecialKind::negative_infinity;
    parsed.canonical = "-Infinity";
    return parsed;
  }
  return parsed;
}

std::string RenderDecimalFloat(const ParsedDecimal& value) {
  if (value.coefficient == 0) { return value.negative_zero ? "-0E+0" : "0E+0"; }
  ParsedDecimal normalized = value;
  while (normalized.scale > 0 && (normalized.coefficient % 10) == 0) {
    normalized.coefficient /= 10;
    --normalized.scale;
  }
  std::string digits = U128ToString(normalized.coefficient);
  const long long exponent = static_cast<long long>(digits.size()) - 1 - static_cast<long long>(normalized.scale);
  std::string mantissa;
  if (normalized.negative) { mantissa.push_back('-'); }
  mantissa.push_back(digits.front());
  if (digits.size() > 1) {
    mantissa.push_back('.');
    mantissa.append(digits.substr(1));
  }
  mantissa.push_back('E');
  mantissa.push_back(exponent < 0 ? '-' : '+');
  mantissa.append(std::to_string(exponent < 0 ? -exponent : exponent));
  return mantissa;
}

bool CanonicalizeDecimalFloatText(const std::string& input,
                                  const DatatypeNumericContext& context,
                                  std::string* out) {
  const ParsedSpecialNumeric special = ParseSpecialNumericText(input);
  if (special.kind != NumericSpecialKind::finite) {
    if (!context.allow_special_values) { return false; }
    *out = special.canonical;
    return true;
  }
  DatatypeNumericContext finite_context = context;
  if (finite_context.precision == 0) { finite_context.precision = 34; }
  if (finite_context.precision > 38) { return false; }
  ParsedDecimal parsed;
  if (!ParseDecimalFiniteText(input, &parsed)) { return false; }
  while (parsed.scale > 0 && (parsed.coefficient % 10) == 0) {
    parsed.coefficient /= 10;
    --parsed.scale;
  }
  if (U128DigitCount(parsed.coefficient) > finite_context.precision) { return false; }
  *out = RenderDecimalFloat(parsed);
  return true;
}

bool CanonicalizeReal128Text(const std::string& input, std::string* out) {
  const ParsedSpecialNumeric special = ParseSpecialNumericText(input);
  if (special.kind != NumericSpecialKind::finite) {
    *out = special.canonical;
    return true;
  }
  std::string value = TrimAsciiWhitespace(input);
  if (value.empty()) { return false; }
  if (value.front() == '+') { value.erase(value.begin()); }
  errno = 0;
  char* end = nullptr;
  const long double parsed = std::strtold(value.c_str(), &end);
  if (end == value.c_str() || *end != '\0' || errno == ERANGE) { return false; }
  if (std::isnan(parsed)) {
    *out = "NaN";
    return true;
  }
  if (std::isinf(parsed)) {
    *out = std::signbit(parsed) ? "-Infinity" : "Infinity";
    return true;
  }
  if (parsed == 0.0L) {
    *out = std::signbit(parsed) || (!value.empty() && value.front() == '-') ? "-0" : "0";
    return true;
  }
  *out = value;
  return true;
}

DatatypeNumericOperationResult NumericFailure(
    std::string detail,
    std::string diagnostic_code = "SB_DATATYPE_NUMERIC_OPERATION_REJECTED") {
  DatatypeNumericOperationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      std::move(diagnostic_code),
                                                      "datatype.numeric_operation.rejected",
                                                      std::move(detail));
  return result;
}

DatatypeNumericFacts NumericFacts(
    const scratchbird::libraries::sbl_numeric::NumericResult& result) {
  DatatypeNumericFacts facts;
  facts.inexact = result.inexact;
  facts.underflow = result.underflow;
  facts.overflow = result.overflow;
  facts.invalid = result.invalid;
  facts.divide_by_zero = result.divide_by_zero;
  facts.subnormal = result.subnormal;
  facts.unordered = result.status ==
      scratchbird::libraries::sbl_numeric::NumericStatusCode::unordered;
  return facts;
}

I128 DecimalToSignedInteger(const ParsedDecimal& value) {
  const I128 magnitude = static_cast<I128>(value.coefficient);
  return value.negative && magnitude != 0 ? -magnitude : magnitude;
}

bool SignedIntegerToDecimal(I128 input, std::uint32_t scale, ParsedDecimal* out) {
  ParsedDecimal value;
  value.negative = input < 0;
  value.coefficient = input < 0 ? static_cast<U128>(-input) : static_cast<U128>(input);
  value.scale = scale;
  value.negative_zero = false;
  *out = value;
  return true;
}

bool AlignDecimalScales(ParsedDecimal* left, ParsedDecimal* right, std::uint32_t target_scale) {
  if (left->scale > target_scale || right->scale > target_scale) { return false; }
  if (left->scale < target_scale) {
    const std::uint32_t diff = target_scale - left->scale;
    if (diff > 38) { return false; }
    left->coefficient *= Pow10U128(diff);
    left->scale = target_scale;
  }
  if (right->scale < target_scale) {
    const std::uint32_t diff = target_scale - right->scale;
    if (diff > 38) { return false; }
    right->coefficient *= Pow10U128(diff);
    right->scale = target_scale;
  }
  return true;
}

struct ExactDecimalTextParts {
  bool negative = false;
  std::string integer_part = "0";
  std::string fractional_part;
};

bool ParseExactDecimalTextParts(const std::string& input,
                                ExactDecimalTextParts* out) {
  std::string value = TrimAsciiWhitespace(input);
  if (value.empty()) { return false; }
  ExactDecimalTextParts parts;
  if (value.front() == '+' || value.front() == '-') {
    parts.negative = value.front() == '-';
    value.erase(value.begin());
  }
  if (value.empty()) { return false; }
  const std::size_t exponent_pos = value.find_first_of("eE");
  std::string significand =
      exponent_pos == std::string::npos ? value : value.substr(0, exponent_pos);
  long long exponent = 0;
  if (exponent_pos != std::string::npos &&
      !ParseSignedDecimalExponent(value.substr(exponent_pos + 1), &exponent)) {
    return false;
  }
  const std::size_t decimal_pos = significand.find('.');
  if (decimal_pos != std::string::npos &&
      significand.find('.', decimal_pos + 1) != std::string::npos) {
    return false;
  }
  std::string digits;
  long long fractional_digits = 0;
  bool saw_digit = false;
  for (std::size_t i = 0; i < significand.size(); ++i) {
    const char c = significand[i];
    if (c == '.') { continue; }
    if (!std::isdigit(static_cast<unsigned char>(c))) { return false; }
    saw_digit = true;
    digits.push_back(c);
    if (decimal_pos != std::string::npos && i > decimal_pos) {
      ++fractional_digits;
    }
  }
  if (!saw_digit) { return false; }
  while (!digits.empty() && digits.front() == '0') {
    digits.erase(digits.begin());
  }
  if (digits.empty()) {
    parts.negative = false;
    *out = std::move(parts);
    return true;
  }
  long long final_scale = fractional_digits - exponent;
  if (final_scale < 0) {
    digits.append(static_cast<std::size_t>(-final_scale), '0');
    final_scale = 0;
  }
  const auto scale = static_cast<std::size_t>(final_scale);
  if (scale == 0) {
    parts.integer_part = std::move(digits);
  } else if (digits.size() <= scale) {
    parts.integer_part = "0";
    parts.fractional_part.assign(scale - digits.size(), '0');
    parts.fractional_part += digits;
  } else {
    const std::size_t integer_digits = digits.size() - scale;
    parts.integer_part = digits.substr(0, integer_digits);
    parts.fractional_part = digits.substr(integer_digits);
  }
  while (parts.integer_part.size() > 1 && parts.integer_part.front() == '0') {
    parts.integer_part.erase(parts.integer_part.begin());
  }
  while (!parts.fractional_part.empty() &&
         parts.fractional_part.back() == '0') {
    parts.fractional_part.pop_back();
  }
  *out = std::move(parts);
  return true;
}

bool CompareFiniteDecimalText(const std::string& left_text,
                              const std::string& right_text,
                              int* comparison) {
  ExactDecimalTextParts left;
  ExactDecimalTextParts right;
  if (!ParseExactDecimalTextParts(left_text, &left) ||
      !ParseExactDecimalTextParts(right_text, &right)) {
    return false;
  }
  if (left.negative != right.negative) {
    *comparison = left.negative ? -1 : 1;
    return true;
  }
  if (left.integer_part.size() < right.integer_part.size()) {
    *comparison = left.negative ? 1 : -1;
    return true;
  }
  if (left.integer_part.size() > right.integer_part.size()) {
    *comparison = left.negative ? -1 : 1;
    return true;
  }
  if (left.integer_part < right.integer_part) {
    *comparison = left.negative ? 1 : -1;
    return true;
  }
  if (left.integer_part > right.integer_part) {
    *comparison = left.negative ? -1 : 1;
    return true;
  }
  const std::size_t max_fraction =
      std::max(left.fractional_part.size(), right.fractional_part.size());
  for (std::size_t i = 0; i < max_fraction; ++i) {
    const char l = i < left.fractional_part.size() ? left.fractional_part[i] : '0';
    const char r = i < right.fractional_part.size() ? right.fractional_part[i] : '0';
    if (l < r) {
      *comparison = left.negative ? 1 : -1;
      return true;
    }
    if (l > r) {
      *comparison = left.negative ? -1 : 1;
      return true;
    }
  }
  *comparison = 0;
  return true;
}

bool ParseFixedDigits(const std::string& value, std::size_t pos, std::size_t count, int* out) {
  if (pos + count > value.size()) { return false; }
  int parsed = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = value[pos + i];
    if (!std::isdigit(static_cast<unsigned char>(c))) { return false; }
    parsed = parsed * 10 + (c - '0');
  }
  *out = parsed;
  return true;
}

bool LeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
  switch (month) {
    case 1: return 31;
    case 2: return LeapYear(year) ? 29 : 28;
    case 3: return 31;
    case 4: return 30;
    case 5: return 31;
    case 6: return 30;
    case 7: return 31;
    case 8: return 31;
    case 9: return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
    default: return 0;
  }
}

bool TimezoneSuffixText(std::string_view suffix) {
  if (suffix.empty()) { return true; }
  if (suffix == "Z" || suffix == "z") { return true; }
  if (suffix.size() != 6 || (suffix[0] != '+' && suffix[0] != '-') || suffix[3] != ':') { return false; }
  int hour = 0;
  int minute = 0;
  const std::string text(suffix);
  return ParseFixedDigits(text, 1, 2, &hour) && ParseFixedDigits(text, 4, 2, &minute) &&
         hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

std::size_t TimezoneSuffixStart(const std::string& value, std::size_t start) {
  for (std::size_t i = start; i < value.size(); ++i) {
    if (value[i] == 'Z' || value[i] == 'z' || value[i] == '+' || value[i] == '-') { return i; }
  }
  return value.size();
}

bool DateText(const std::string& value) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') { return false; }
  if (!ParseFixedDigits(value, 0, 4, &year) || !ParseFixedDigits(value, 5, 2, &month) ||
      !ParseFixedDigits(value, 8, 2, &day)) {
    return false;
  }
  return year >= 1 && month >= 1 && month <= 12 && day >= 1 && day <= DaysInMonth(year, month);
}

bool TimeText(const std::string& value) {
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (value.size() < 8 || value[2] != ':' || value[5] != ':') { return false; }
  if (!ParseFixedDigits(value, 0, 2, &hour) || !ParseFixedDigits(value, 3, 2, &minute) ||
      !ParseFixedDigits(value, 6, 2, &second)) {
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) { return false; }
  std::size_t pos = 8;
  if (pos < value.size() && value[pos] == '.') {
    ++pos;
    const std::size_t fraction_start = pos;
    while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) { ++pos; }
    const std::size_t fraction_digits = pos - fraction_start;
    if (fraction_digits == 0 || fraction_digits > 12) { return false; }
  }
  const std::size_t tz_start = TimezoneSuffixStart(value, pos);
  if (tz_start != pos && tz_start != value.size()) { return false; }
  return TimezoneSuffixText(std::string_view(value).substr(tz_start));
}

bool HexText(std::string_view value, bool allow_prefix, bool require_even_digits) {
  if (allow_prefix && value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
    value.remove_prefix(2);
  }
  if (value.empty()) { return false; }
  if (require_even_digits && (value.size() % 2) != 0) { return false; }
  for (char c : value) {
    if (HexValue(c) < 0) { return false; }
  }
  return true;
}

std::string HexDecodeStrict(std::string_view value, bool allow_prefix, bool* ok) {
  if (allow_prefix && value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
    value.remove_prefix(2);
  }
  if (!HexText(value, false, true)) {
    *ok = false;
    return {};
  }
  std::string out;
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    out.push_back(static_cast<char>((HexValue(value[i]) << 4) | HexValue(value[i + 1])));
  }
  *ok = true;
  return out;
}

std::string HexEncodeLower(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

class JsonTextValidator {
 public:
  explicit JsonTextValidator(std::string_view text) : text_(text) {}

  bool Valid() {
    SkipWhitespace();
    if (!ParseValue()) { return false; }
    SkipWhitespace();
    return pos_ == text_.size();
  }

 private:
  void SkipWhitespace() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
  }

  bool Consume(char expected) {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool ConsumeLiteral(std::string_view expected) {
    if (text_.substr(pos_, expected.size()) != expected) { return false; }
    pos_ += expected.size();
    return true;
  }

  bool ParseValue() {
    SkipWhitespace();
    if (pos_ >= text_.size()) { return false; }
    const char c = text_[pos_];
    if (c == '{') { return ParseObject(); }
    if (c == '[') { return ParseArray(); }
    if (c == '"') { return ParseString(); }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) { return ParseNumber(); }
    if (c == 't') { return ConsumeLiteral("true"); }
    if (c == 'f') { return ConsumeLiteral("false"); }
    if (c == 'n') { return ConsumeLiteral("null"); }
    return false;
  }

  bool ParseObject() {
    if (!Consume('{')) { return false; }
    SkipWhitespace();
    if (Consume('}')) { return true; }
    while (true) {
      SkipWhitespace();
      if (!ParseString()) { return false; }
      SkipWhitespace();
      if (!Consume(':')) { return false; }
      if (!ParseValue()) { return false; }
      SkipWhitespace();
      if (Consume('}')) { return true; }
      if (!Consume(',')) { return false; }
    }
  }

  bool ParseArray() {
    if (!Consume('[')) { return false; }
    SkipWhitespace();
    if (Consume(']')) { return true; }
    while (true) {
      if (!ParseValue()) { return false; }
      SkipWhitespace();
      if (Consume(']')) { return true; }
      if (!Consume(',')) { return false; }
    }
  }

  bool ParseString() {
    if (!Consume('"')) { return false; }
    while (pos_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      if (c == '"') { return true; }
      if (c < 0x20) { return false; }
      if (c == '\\') {
        if (pos_ >= text_.size()) { return false; }
        const char escaped = text_[pos_++];
        if (escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b' || escaped == 'f' ||
            escaped == 'n' || escaped == 'r' || escaped == 't') {
          continue;
        }
        if (escaped == 'u') {
          for (int i = 0; i < 4; ++i) {
            if (pos_ >= text_.size() || HexValue(text_[pos_++]) < 0) { return false; }
          }
          continue;
        }
        return false;
      }
    }
    return false;
  }

  bool ParseNumber() {
    if (Consume('-') && pos_ >= text_.size()) { return false; }
    if (Consume('0')) {
      if (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
    } else {
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
    }
    if (Consume('.')) {
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) { ++pos_; }
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
    }
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

bool JsonText(const std::string& value) {
  return JsonTextValidator(value).Valid();
}

std::string TrimAsciiWhitespace(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) { value.erase(value.begin()); }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) { value.pop_back(); }
  return value;
}

bool XmlNameText(std::string_view value) {
  if (value.empty()) { return false; }
  const unsigned char first = static_cast<unsigned char>(value.front());
  if (!std::isalpha(first) && first != '_' && first != ':') { return false; }
  for (char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!std::isalnum(u) && c != '_' && c != '-' && c != ':' && c != '.') { return false; }
  }
  return true;
}

bool XmlText(const std::string& input) {
  const std::string value = TrimAsciiWhitespace(input);
  if (value.size() < 7 || value.front() != '<' || value.back() != '>') { return false; }
  if (StartsWith(value, "<?xml")) {
    const auto declaration_end = value.find("?>");
    if (declaration_end == std::string::npos || declaration_end + 2 >= value.size()) { return false; }
    return XmlText(value.substr(declaration_end + 2));
  }
  if (value[1] == '/' || value[1] == '!' || value[1] == '?') { return false; }
  const auto open_end = value.find('>');
  if (open_end == std::string::npos || open_end < 2) { return false; }
  std::string open_name = value.substr(1, open_end - 1);
  const auto attr_pos = open_name.find_first_of(" \t\r\n/");
  const bool self_closing = !open_name.empty() && open_name.back() == '/';
  if (attr_pos != std::string::npos) { open_name = open_name.substr(0, attr_pos); }
  if (!XmlNameText(open_name)) { return false; }
  if (self_closing || (open_end > 0 && value[open_end - 1] == '/')) { return true; }
  const std::string close = "</" + open_name + ">";
  return value.size() >= close.size() && value.substr(value.size() - close.size()) == close;
}

DatatypeOperationValue BoolValue(bool value) {
  return {CanonicalTypeId::boolean,
          std::string(1, static_cast<char>(value ? 1 : 0)), false};
}

DatatypeOperationValue UInt64Value(std::uint64_t value) {
  std::string canonical_bytes;
  if (!EncodeCanonicalUint64Value(value, &canonical_bytes)) {
    return {};
  }
  return {CanonicalTypeId::uint64, std::move(canonical_bytes), false};
}

DatatypeCastResult CastFailure(
    std::string detail,
    DatatypeCastCategory category = DatatypeCastCategory::forbidden,
    std::string diagnostic_code = "DATATYPE.CAST_FORBIDDEN") {
  DatatypeCastResult result;
  result.status = ErrorStatus();
  result.category = category;
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      std::move(diagnostic_code),
                                                      "datatype.cast.rejected",
                                                      std::move(detail));
  return result;
}

DatatypeExtractResult ExtractFailure(
    std::string detail,
    std::string diagnostic_code = "SB_DATATYPE_EXTRACT_REJECTED") {
  DatatypeExtractResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      std::move(diagnostic_code),
                                                      "datatype.extract.rejected",
                                                      std::move(detail));
  return result;
}

DatatypeSetOperationResult SetFailure(
    std::string detail,
    std::string diagnostic_code = "SB_DATATYPE_SET_OPERATION_REJECTED") {
  DatatypeSetOperationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      std::move(diagnostic_code),
                                                      "datatype.set_operation.rejected",
                                                      std::move(detail));
  return result;
}

struct EncodedSetFrame {
  CanonicalTypeId element_type_id = CanonicalTypeId::unknown;
  bool descriptor_present = false;
  ExecutionTypeDescriptor element_descriptor;
  bool ordered = false;
  bool allow_null_elements = false;
  bool allow_duplicates = false;
  // Canonical item tokens keep element state disjoint from its payload:
  // N is SQL NULL and V<hex payload> is a present value.
  std::vector<std::string> items;
};

void AppendSetU16(std::string* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<char>((value >> 8) & 0xffu));
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

void AppendSetUuid(std::string* bytes, const scratchbird::engine::Uuid& uuid) {
  for (const auto byte : uuid.bytes) {
    bytes->push_back(static_cast<char>(byte));
  }
}

void AppendSetString(std::string* bytes, std::string_view value) {
  AppendSetU32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes->append(value);
}

std::string ExecutionDescriptorFingerprint(
    const ExecutionTypeDescriptor& descriptor) {
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
  return HexEncode(bytes);
}

bool IsCanonicalLowerHex(std::string_view value) {
  if ((value.size() % 2) != 0) { return false; }
  for (const char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool DecodeCanonicalLowerHex(std::string_view value, std::string* bytes) {
  if (bytes == nullptr || !IsCanonicalLowerHex(value)) { return false; }
  bytes->clear();
  bytes->reserve(value.size() / 2);
  for (std::size_t index = 0; index < value.size(); index += 2) {
    bytes->push_back(static_cast<char>(
        (HexValue(value[index]) << 4) | HexValue(value[index + 1])));
  }
  return true;
}

struct SetDescriptorReader {
  std::string_view bytes;
  std::size_t offset = 0;

  bool Take(std::size_t size, std::string_view* value) {
    if (value == nullptr || size > bytes.size() - offset) { return false; }
    *value = bytes.substr(offset, size);
    offset += size;
    return true;
  }

  bool U16(std::uint16_t* value) {
    std::string_view raw;
    if (value == nullptr || !Take(2, &raw)) { return false; }
    *value = (static_cast<std::uint16_t>(
                  static_cast<unsigned char>(raw[0])) << 8) |
        static_cast<unsigned char>(raw[1]);
    return true;
  }

  bool U32(std::uint32_t* value) {
    std::string_view raw;
    if (value == nullptr || !Take(4, &raw)) { return false; }
    *value = 0;
    for (const char byte : raw) {
      *value = (*value << 8) | static_cast<unsigned char>(byte);
    }
    return true;
  }

  bool U64(std::uint64_t* value) {
    std::string_view raw;
    if (value == nullptr || !Take(8, &raw)) { return false; }
    *value = 0;
    for (const char byte : raw) {
      *value = (*value << 8) | static_cast<unsigned char>(byte);
    }
    return true;
  }

  bool Uuid(scratchbird::engine::Uuid* value) {
    std::string_view raw;
    if (value == nullptr || !Take(16, &raw)) { return false; }
    for (std::size_t index = 0; index < 16; ++index) {
      value->bytes[index] = static_cast<unsigned char>(raw[index]);
    }
    return true;
  }

  bool String(std::string* value) {
    std::uint32_t size = 0;
    std::string_view raw;
    if (value == nullptr || !U32(&size) || !Take(size, &raw)) { return false; }
    value->assign(raw);
    return true;
  }

  bool Boolean(bool* value) {
    std::string_view raw;
    if (value == nullptr || !Take(1, &raw) ||
        (raw[0] != '\x00' && raw[0] != '\x01')) {
      return false;
    }
    *value = raw[0] == '\x01';
    return true;
  }
};

bool ParseExecutionDescriptorFingerprint(
    std::string_view fingerprint,
    ExecutionTypeDescriptor* descriptor) {
  std::string bytes;
  if (descriptor == nullptr ||
      !DecodeCanonicalLowerHex(fingerprint, &bytes)) {
    return false;
  }
  SetDescriptorReader reader{bytes};
  std::string_view magic;
  std::uint16_t family = 0;
  std::uint16_t width_class = 0;
  std::uint32_t domain_count = 0;
  if (!reader.Take(5, &magic) || magic != "SBTD1" ||
      !reader.Uuid(&descriptor->descriptor_uuid) ||
      !reader.U64(&descriptor->descriptor_epoch) ||
      !reader.U32(&descriptor->canonical_type_id) ||
      !reader.U16(&family) || !reader.U16(&width_class) ||
      !reader.String(&descriptor->stable_name) ||
      !reader.U32(&descriptor->bit_width) ||
      !reader.U32(&descriptor->precision) ||
      !reader.U32(&descriptor->scale) ||
      !reader.U32(&descriptor->length) ||
      !reader.U32(&descriptor->vector_dimensions) ||
      !reader.U32(&descriptor->container_rank) ||
      !reader.U64(&descriptor->modifier_flags) ||
      !reader.Uuid(&descriptor->domain_uuid) ||
      !reader.U32(&domain_count) ||
      domain_count > scratchbird::engine::kExecutionDomainStackMaxDepth) {
    return false;
  }
  descriptor->family = static_cast<scratchbird::engine::ExecutionTypeFamily>(family);
  descriptor->width_class =
      static_cast<scratchbird::engine::ExecutionTypeWidthClass>(width_class);
  descriptor->domain_stack.resize(domain_count);
  for (auto& domain : descriptor->domain_stack) {
    if (!reader.Uuid(&domain)) { return false; }
  }
  return reader.Uuid(&descriptor->charset_uuid) &&
      reader.Uuid(&descriptor->collation_uuid) &&
      reader.Uuid(&descriptor->timezone_uuid) &&
      reader.Uuid(&descriptor->element_descriptor_uuid) &&
      reader.Uuid(&descriptor->security_policy_uuid) &&
      reader.Boolean(&descriptor->nullable_allowed) &&
      reader.Boolean(&descriptor->descriptor_authoritative) &&
      reader.Boolean(&descriptor->parser_independent) &&
      reader.offset == reader.bytes.size();
}

bool ParseSetBooleanField(std::string_view field,
                          std::string_view name,
                          bool* value) {
  const std::string prefix = std::string(name) + "=";
  if (!StartsWith(field, prefix) || field.size() != prefix.size() + 1) {
    return false;
  }
  if (field.back() == '0') {
    *value = false;
    return true;
  }
  if (field.back() == '1') {
    *value = true;
    return true;
  }
  return false;
}

bool ParseEncodedSet(const std::string& encoded, EncodedSetFrame* frame) {
  if (frame == nullptr || !StartsWith(encoded, "SBSET2;")) { return false; }
  const auto marker = encoded.find(";items=");
  if (marker == std::string::npos) { return false; }
  const auto fields = Split(encoded.substr(7, marker - 7), ';');
  if (fields.size() != 5 || !StartsWith(fields[0], "element=") ||
      !StartsWith(fields[1], "descriptor=") ||
      !ParseSetBooleanField(fields[2], "ordered", &frame->ordered) ||
      !ParseSetBooleanField(fields[3], "nulls", &frame->allow_null_elements) ||
      !ParseSetBooleanField(fields[4], "duplicates", &frame->allow_duplicates)) {
    return false;
  }
  const std::string element_name = fields[0].substr(8);
  frame->element_type_id =
      CanonicalTypeIdFromStableName(element_name);
  if (frame->element_type_id == CanonicalTypeId::unknown ||
      frame->element_type_id == CanonicalTypeId::null_type ||
      element_name != CanonicalTypeName(frame->element_type_id)) {
    return false;
  }
  const std::string descriptor_fingerprint = fields[1].substr(11);
  frame->descriptor_present = descriptor_fingerprint != "none";
  if (frame->descriptor_present &&
      (!ParseExecutionDescriptorFingerprint(
           descriptor_fingerprint, &frame->element_descriptor) ||
       !ExecutionDescriptorValidForType(frame->element_descriptor,
                                        frame->element_type_id))) {
    return false;
  }
  if (frame->allow_null_elements &&
      (!frame->descriptor_present ||
       !frame->element_descriptor.nullable_allowed)) {
    return false;
  }

  const std::string item_list = encoded.substr(marker + 7);
  if (item_list.empty()) { return true; }
  if (item_list.front() == ',' || item_list.back() == ',') { return false; }
  std::set<std::string> unique_items;
  std::string previous_item;
  for (const auto& item : Split(item_list, ',')) {
    if (item == "N") {
      if (!frame->allow_null_elements || !frame->descriptor_present ||
          !frame->element_descriptor.nullable_allowed) {
        return false;
      }
    } else if (!item.empty() && item.front() == 'V') {
      const std::string_view hex(item.data() + 1, item.size() - 1);
      std::string decoded_item;
      if (!IsCanonicalLowerHex(hex) ||
          (IsPolicyRestrictedReal(frame->element_type_id) &&
           hex.size() !=
               PolicyRestrictedRealCarrierByteWidth(frame->element_type_id) *
                   2u) ||
          ((IsUuid(frame->element_type_id) ||
            IsIpAddress(frame->element_type_id)) &&
           hex.size() != 32u) ||
          (IsNetworkPrefix(frame->element_type_id) &&
           hex.size() != 36u) ||
          (IsMacAddress(frame->element_type_id) &&
           hex.size() != 16u) ||
          (IsDecimal(frame->element_type_id) &&
           (!DecodeCanonicalLowerHex(hex, &decoded_item) ||
            !DecimalCarrierStructurallyCanonical(decoded_item))) ||
          (IsDecimalFloat(frame->element_type_id) &&
           (!DecodeCanonicalLowerHex(hex, &decoded_item) ||
            !DecimalFloatCarrierStructurallyCanonical(decoded_item)))) {
        return false;
      }
    } else {
      return false;
    }
    if (!frame->allow_duplicates && !unique_items.insert(item).second) {
      return false;
    }
    if (!frame->ordered && !previous_item.empty() && item < previous_item) {
      return false;
    }
    previous_item = item;
    frame->items.push_back(item);
  }
  return true;
}

bool EncodedSetFrameMatchesDescriptor(
    const EncodedSetFrame& frame,
    const DatatypeSetDescriptor& descriptor) {
  if (frame.element_type_id != descriptor.element_type_id ||
      frame.ordered != descriptor.ordered ||
      frame.allow_null_elements != descriptor.allow_null_elements ||
      frame.allow_duplicates != descriptor.allow_duplicates) {
    return false;
  }
  const bool descriptor_present =
      ExecutionDescriptorPresent(descriptor.element_descriptor);
  if (frame.allow_null_elements || descriptor_present) {
    return descriptor_present == frame.descriptor_present &&
        descriptor_present &&
        ExecutionDescriptorValidForType(descriptor.element_descriptor,
                                        descriptor.element_type_id) &&
        ExecutionDescriptorEquals(frame.element_descriptor,
                                  descriptor.element_descriptor);
  }
  return !frame.descriptor_present;
}

std::string ExtractTemporalComponent(const std::string& value, const std::string& field) {
  const std::string date = value.size() >= 10 ? value.substr(0, 10) : value;
  if ((field == "year" || field == "yyyy") && DateText(date)) { return date.substr(0, 4); }
  if ((field == "month" || field == "mm") && DateText(date)) { return date.substr(5, 2); }
  if ((field == "day" || field == "dd") && DateText(date)) { return date.substr(8, 2); }
  const auto time_start = value.find('T') != std::string::npos ? value.find('T') + 1 :
                          (value.find(' ') != std::string::npos ? value.find(' ') + 1 : 0);
  const std::string time = time_start == 0 ? value : value.substr(time_start);
  if (field == "hour" && TimeText(time)) { return time.substr(0, 2); }
  if (field == "minute" && TimeText(time)) { return time.substr(3, 2); }
  if (field == "second" && TimeText(time)) { return time.substr(6, 2); }
  return {};
}

std::uint64_t HexToU64(std::string_view hex) {
  std::uint64_t value = 0;
  for (char c : hex) {
    const int digit = HexValue(c);
    if (digit < 0) { return 0; }
    value = (value << 4) | static_cast<std::uint64_t>(digit);
  }
  return value;
}

}  // namespace

bool EncodeCanonicalInt8Value(std::string_view decimal_text,
                              std::string* canonical_bytes) {
  return EncodeInt8DecimalText(decimal_text, canonical_bytes);
}

bool DecodeCanonicalInt8Value(std::string_view canonical_bytes,
                              std::string* decimal_text) {
  if (decimal_text == nullptr || canonical_bytes.size() != 1) return false;
  *decimal_text = Int8DecimalText(canonical_bytes);
  return true;
}

bool EncodeCanonicalUint8Value(std::string_view decimal_text,
                               std::string* canonical_bytes) {
  return EncodeUint8DecimalText(decimal_text, canonical_bytes);
}

bool DecodeCanonicalUint8Value(std::string_view canonical_bytes,
                               std::string* decimal_text) {
  if (decimal_text == nullptr || canonical_bytes.size() != 1) return false;
  *decimal_text = Uint8DecimalText(canonical_bytes);
  return true;
}

bool EncodeCanonicalInt16Value(std::int64_t value,
                               std::string* canonical_bytes) {
  if (canonical_bytes == nullptr ||
      value < std::numeric_limits<std::int16_t>::min() ||
      value > std::numeric_limits<std::int16_t>::max()) {
    return false;
  }
  const auto raw = static_cast<std::uint16_t>(value);
  canonical_bytes->resize(2);
  (*canonical_bytes)[0] = static_cast<char>(raw & 0xffu);
  (*canonical_bytes)[1] = static_cast<char>((raw >> 8u) & 0xffu);
  return true;
}

bool DecodeCanonicalInt16Value(std::string_view canonical_bytes,
                               std::int64_t* value) {
  if (value == nullptr || canonical_bytes.size() != 2) return false;
  const auto low = static_cast<unsigned char>(canonical_bytes[0]);
  const auto high = static_cast<unsigned char>(canonical_bytes[1]);
  const unsigned raw = low | (high << 8u);
  *value = raw < 0x8000u ? static_cast<std::int64_t>(raw)
                         : static_cast<std::int64_t>(raw) - 0x10000;
  return true;
}

bool EncodeCanonicalUint16Value(std::int64_t value,
                                std::string* canonical_bytes) {
  if (canonical_bytes == nullptr || value < 0 ||
      value > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  const auto raw = static_cast<std::uint16_t>(value);
  canonical_bytes->resize(2);
  (*canonical_bytes)[0] = static_cast<char>(raw & 0xffu);
  (*canonical_bytes)[1] = static_cast<char>((raw >> 8u) & 0xffu);
  return true;
}

bool DecodeCanonicalUint16Value(std::string_view canonical_bytes,
                                std::uint64_t* value) {
  if (value == nullptr || canonical_bytes.size() != 2) return false;
  const auto low = static_cast<unsigned char>(canonical_bytes[0]);
  const auto high = static_cast<unsigned char>(canonical_bytes[1]);
  *value = static_cast<std::uint64_t>(low | (high << 8u));
  return true;
}

bool EncodeBfloat16CarrierBitsV1(std::uint16_t raw_bits,
                                 std::string* carrier_bytes) {
  if (carrier_bytes == nullptr) return false;
  std::string staged(2, '\0');
  staged[0] = static_cast<char>(raw_bits & 0xffu);
  staged[1] = static_cast<char>((raw_bits >> 8u) & 0xffu);
  *carrier_bytes = std::move(staged);
  return true;
}

bool DecodeBfloat16CarrierBitsV1(std::string_view carrier_bytes,
                                 std::uint16_t* raw_bits) {
  if (raw_bits == nullptr || carrier_bytes.size() != 2) return false;
  const auto low = static_cast<std::uint16_t>(
      static_cast<unsigned char>(carrier_bytes[0]));
  const auto high = static_cast<std::uint16_t>(
      static_cast<unsigned char>(carrier_bytes[1]));
  const std::uint16_t staged =
      static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8u));
  *raw_bits = staged;
  return true;
}

bool EncodeReal16CarrierBitsV1(std::uint16_t raw_bits,
                               std::string* carrier_bytes) {
  if (carrier_bytes == nullptr) return false;
  std::string staged(2, '\0');
  staged[0] = static_cast<char>(raw_bits & 0xffu);
  staged[1] = static_cast<char>((raw_bits >> 8u) & 0xffu);
  *carrier_bytes = std::move(staged);
  return true;
}

bool DecodeReal16CarrierBitsV1(std::string_view carrier_bytes,
                               std::uint16_t* raw_bits) {
  if (raw_bits == nullptr || carrier_bytes.size() != 2) return false;
  const auto low = static_cast<std::uint16_t>(
      static_cast<unsigned char>(carrier_bytes[0]));
  const auto high = static_cast<std::uint16_t>(
      static_cast<unsigned char>(carrier_bytes[1]));
  const std::uint16_t staged =
      static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8u));
  *raw_bits = staged;
  return true;
}

bool EncodeReal32CarrierBitsV1(std::uint32_t raw_bits,
                               std::string* carrier_bytes) {
  if (carrier_bytes == nullptr) return false;
  std::string staged(4, '\0');
  staged[0] = static_cast<char>(raw_bits & 0xffu);
  staged[1] = static_cast<char>((raw_bits >> 8u) & 0xffu);
  staged[2] = static_cast<char>((raw_bits >> 16u) & 0xffu);
  staged[3] = static_cast<char>((raw_bits >> 24u) & 0xffu);
  *carrier_bytes = std::move(staged);
  return true;
}

bool DecodeReal32CarrierBitsV1(std::string_view carrier_bytes,
                               std::uint32_t* raw_bits) {
  if (raw_bits == nullptr || carrier_bytes.size() != 4) return false;
  std::uint32_t staged = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    staged |= static_cast<std::uint32_t>(
                  static_cast<unsigned char>(carrier_bytes[index]))
        << (8u * index);
  }
  *raw_bits = staged;
  return true;
}

bool EncodeReal64CarrierBitsV1(std::uint64_t raw_bits,
                               std::string* carrier_bytes) {
  if (carrier_bytes == nullptr) return false;
  std::string staged(8, '\0');
  for (std::size_t index = 0; index < 8; ++index) {
    staged[index] = static_cast<char>((raw_bits >> (8u * index)) & 0xffu);
  }
  *carrier_bytes = std::move(staged);
  return true;
}

bool DecodeReal64CarrierBitsV1(std::string_view carrier_bytes,
                               std::uint64_t* raw_bits) {
  if (raw_bits == nullptr || carrier_bytes.size() != 8) return false;
  std::uint64_t staged = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    staged |= static_cast<std::uint64_t>(
                  static_cast<unsigned char>(carrier_bytes[index]))
        << (8u * index);
  }
  *raw_bits = staged;
  return true;
}

bool EncodeCanonicalInt32Value(std::int64_t value,
                               std::string* canonical_bytes) {
  if (canonical_bytes == nullptr ||
      value < std::numeric_limits<std::int32_t>::min() ||
      value > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  const auto raw = static_cast<std::uint32_t>(value);
  canonical_bytes->resize(4);
  (*canonical_bytes)[0] = static_cast<char>(raw & 0xffu);
  (*canonical_bytes)[1] = static_cast<char>((raw >> 8u) & 0xffu);
  (*canonical_bytes)[2] = static_cast<char>((raw >> 16u) & 0xffu);
  (*canonical_bytes)[3] = static_cast<char>((raw >> 24u) & 0xffu);
  return true;
}

bool DecodeCanonicalInt32Value(std::string_view canonical_bytes,
                               std::int64_t* value) {
  if (value == nullptr || canonical_bytes.size() != 4) return false;
  const auto raw =
      static_cast<std::uint32_t>(
          static_cast<unsigned char>(canonical_bytes[0])) |
      (static_cast<std::uint32_t>(
          static_cast<unsigned char>(canonical_bytes[1])) << 8u) |
      (static_cast<std::uint32_t>(
          static_cast<unsigned char>(canonical_bytes[2])) << 16u) |
      (static_cast<std::uint32_t>(
          static_cast<unsigned char>(canonical_bytes[3])) << 24u);
  *value = raw <= static_cast<std::uint32_t>(
                     std::numeric_limits<std::int32_t>::max())
      ? static_cast<std::int64_t>(raw)
      : static_cast<std::int64_t>(raw) - (std::int64_t{1} << 32u);
  return true;
}

bool EncodeCanonicalUint32Value(std::uint64_t value,
                                std::string* canonical_bytes) {
  if (canonical_bytes == nullptr ||
      value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const auto raw = static_cast<std::uint32_t>(value);
  canonical_bytes->resize(4);
  (*canonical_bytes)[0] = static_cast<char>(raw & 0xffu);
  (*canonical_bytes)[1] = static_cast<char>((raw >> 8u) & 0xffu);
  (*canonical_bytes)[2] = static_cast<char>((raw >> 16u) & 0xffu);
  (*canonical_bytes)[3] = static_cast<char>((raw >> 24u) & 0xffu);
  return true;
}

bool DecodeCanonicalUint32Value(std::string_view canonical_bytes,
                                std::uint64_t* value) {
  if (value == nullptr || canonical_bytes.size() != 4) return false;
  const auto raw =
      static_cast<std::uint32_t>(
          static_cast<unsigned char>(canonical_bytes[0])) |
      (static_cast<std::uint32_t>(
          static_cast<unsigned char>(canonical_bytes[1])) << 8u) |
      (static_cast<std::uint32_t>(
          static_cast<unsigned char>(canonical_bytes[2])) << 16u) |
      (static_cast<std::uint32_t>(
          static_cast<unsigned char>(canonical_bytes[3])) << 24u);
  *value = raw;
  return true;
}

bool EncodeCanonicalInt64Value(std::int64_t value,
                               std::string* canonical_bytes) {
  if (canonical_bytes == nullptr) return false;
  const auto raw = static_cast<std::uint64_t>(value);
  std::string encoded(8, '\0');
  for (unsigned byte = 0; byte < 8; ++byte) {
    encoded[byte] = static_cast<char>((raw >> (byte * 8u)) & 0xffu);
  }
  *canonical_bytes = std::move(encoded);
  return true;
}

bool DecodeCanonicalInt64Value(std::string_view canonical_bytes,
                               std::int64_t* value) {
  if (value == nullptr || canonical_bytes.size() != 8) return false;
  std::uint64_t raw = 0;
  for (unsigned byte = 0; byte < 8; ++byte) {
    raw |= static_cast<std::uint64_t>(
               static_cast<unsigned char>(canonical_bytes[byte]))
           << (byte * 8u);
  }
  std::int64_t decoded = 0;
  static_assert(sizeof(decoded) == sizeof(raw));
  std::memcpy(&decoded, &raw, sizeof(decoded));
  *value = decoded;
  return true;
}

bool EncodeCanonicalUint64Value(std::uint64_t value,
                                std::string* canonical_bytes) {
  if (canonical_bytes == nullptr) return false;
  std::string encoded(8, '\0');
  for (unsigned byte = 0; byte < 8; ++byte) {
    encoded[byte] = static_cast<char>((value >> (byte * 8u)) & 0xffu);
  }
  *canonical_bytes = std::move(encoded);
  return true;
}

bool DecodeCanonicalUint64Value(std::string_view canonical_bytes,
                                std::uint64_t* value) {
  if (value == nullptr || canonical_bytes.size() != 8) return false;
  std::uint64_t decoded = 0;
  for (unsigned byte = 0; byte < 8; ++byte) {
    decoded |= static_cast<std::uint64_t>(
                   static_cast<unsigned char>(canonical_bytes[byte]))
               << (byte * 8u);
  }
  *value = decoded;
  return true;
}

bool EncodeCanonicalInt128Value(std::string_view canonical_decimal,
                                std::string* canonical_bytes) {
  if (canonical_bytes == nullptr) return false;
  const auto encoded =
      libraries::sbl_numeric::EncodeInt128LittleEndian(canonical_decimal);
  if (encoded.status != libraries::sbl_numeric::NumericStatusCode::ok ||
      encoded.payload.size() != 16) {
    return false;
  }
  std::string staged;
  staged.assign(reinterpret_cast<const char*>(encoded.payload.data()),
                encoded.payload.size());
  *canonical_bytes = std::move(staged);
  return true;
}

bool DecodeCanonicalInt128Value(std::string_view canonical_bytes,
                                std::string* canonical_decimal) {
  if (canonical_decimal == nullptr || canonical_bytes.size() != 16) {
    return false;
  }
  const std::vector<std::uint8_t> payload(canonical_bytes.begin(),
                                          canonical_bytes.end());
  const auto decoded =
      libraries::sbl_numeric::DecodeInt128LittleEndian(payload);
  if (decoded.status != libraries::sbl_numeric::NumericStatusCode::ok ||
      decoded.value.type != libraries::sbl_numeric::NumericType::int128 ||
      decoded.value.is_null) {
    return false;
  }
  *canonical_decimal = decoded.value.encoded;
  return true;
}

bool EncodeCanonicalUint128Value(std::string_view canonical_decimal,
                                 std::string* canonical_bytes) {
  if (canonical_bytes == nullptr) return false;
  const auto encoded =
      libraries::sbl_numeric::EncodeUint128LittleEndian(canonical_decimal);
  if (encoded.status != libraries::sbl_numeric::NumericStatusCode::ok ||
      encoded.payload.size() != 16) {
    return false;
  }
  std::string staged;
  staged.assign(reinterpret_cast<const char*>(encoded.payload.data()),
                encoded.payload.size());
  *canonical_bytes = std::move(staged);
  return true;
}

bool DecodeCanonicalUint128Value(std::string_view canonical_bytes,
                                 std::string* canonical_decimal) {
  if (canonical_decimal == nullptr || canonical_bytes.size() != 16) {
    return false;
  }
  const std::vector<std::uint8_t> payload(canonical_bytes.begin(),
                                          canonical_bytes.end());
  const auto decoded =
      libraries::sbl_numeric::DecodeUint128LittleEndian(payload);
  if (decoded.status != libraries::sbl_numeric::NumericStatusCode::ok ||
      decoded.value.type != libraries::sbl_numeric::NumericType::uint128 ||
      decoded.value.is_null) {
    return false;
  }
  *canonical_decimal = decoded.value.encoded;
  return true;
}

bool EncodeCanonicalInt32BulkImportTextV1(
    std::string_view decimal_text,
    std::string* canonical_bytes) {
  if (canonical_bytes == nullptr || decimal_text.empty() ||
      decimal_text.front() == '+' || decimal_text.front() == ' ' ||
      decimal_text.back() == ' ' || decimal_text == "-0" ||
      (decimal_text.size() > 1 && decimal_text.front() == '0') ||
      (decimal_text.size() > 2 && decimal_text.front() == '-' &&
       decimal_text[1] == '0')) {
    return false;
  }

  std::int32_t parsed_value = 0;
  const auto parsed = std::from_chars(
      decimal_text.data(), decimal_text.data() + decimal_text.size(),
      parsed_value, 10);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != decimal_text.data() + decimal_text.size() ||
      std::to_string(parsed_value) != decimal_text) {
    return false;
  }

  std::string encoded;
  if (!EncodeCanonicalInt32Value(parsed_value, &encoded)) return false;
  *canonical_bytes = std::move(encoded);
  return true;
}

const char* DatatypeCastCategoryName(DatatypeCastCategory category) {
  switch (category) {
    case DatatypeCastCategory::identity: return "identity";
    case DatatypeCastCategory::lossless_implicit: return "lossless_implicit";
    case DatatypeCastCategory::lossless_explicit: return "lossless_explicit";
    case DatatypeCastCategory::lossy_explicit: return "lossy_explicit";
    case DatatypeCastCategory::reference_compatibility_explicit: return "reference_compatibility_explicit";
    case DatatypeCastCategory::domain_to_base: return "domain_to_base";
    case DatatypeCastCategory::base_to_domain: return "base_to_domain";
    case DatatypeCastCategory::forbidden: return "forbidden";
  }
  return "forbidden";
}

const char* DatatypeCastContextName(DatatypeCastContext context) {
  switch (context) {
    case DatatypeCastContext::implicit: return "implicit";
    case DatatypeCastContext::assignment: return "assignment";
    case DatatypeCastContext::explicit_cast: return "explicit";
  }
  return "unknown";
}

const char* DatatypeSetOperationKindName(DatatypeSetOperationKind operation) {
  switch (operation) {
    case DatatypeSetOperationKind::membership: return "membership";
    case DatatypeSetOperationKind::equals: return "equals";
    case DatatypeSetOperationKind::subset: return "subset";
    case DatatypeSetOperationKind::superset: return "superset";
    case DatatypeSetOperationKind::cardinality: return "cardinality";
  }
  return "membership";
}

const char* DatatypeNumericOperationKindName(DatatypeNumericOperationKind operation) {
  switch (operation) {
    case DatatypeNumericOperationKind::canonicalize: return "canonicalize";
    case DatatypeNumericOperationKind::add: return "add";
    case DatatypeNumericOperationKind::subtract: return "subtract";
    case DatatypeNumericOperationKind::multiply: return "multiply";
    case DatatypeNumericOperationKind::divide: return "divide";
    case DatatypeNumericOperationKind::compare: return "compare";
  }
  return "unknown";
}

const char* DatatypeRoundingModeName(DatatypeRoundingMode rounding) {
  switch (rounding) {
    case DatatypeRoundingMode::half_even: return "half_even";
    case DatatypeRoundingMode::half_up: return "half_up";
    case DatatypeRoundingMode::truncate: return "truncate";
  }
  return "unknown";
}

const char* DatatypeNullOrderingName(DatatypeNullOrdering null_ordering) {
  switch (null_ordering) {
    case DatatypeNullOrdering::nulls_first: return "nulls_first";
    case DatatypeNullOrdering::nulls_last: return "nulls_last";
  }
  return "unknown";
}

DatatypeCastCategory ClassifyDatatypeCast(CanonicalTypeId source_type_id,
                                          CanonicalTypeId target_type_id,
                                          bool reference_compatibility_profile) {
  // These types require authenticated V3 receipts and complete profiles. This
  // legacy enum-only classifier cannot admit even an apparent identity pair;
  // callers must use the profile-aware type API.
  if (source_type_id == CanonicalTypeId::interval ||
      target_type_id == CanonicalTypeId::interval ||
      source_type_id == CanonicalTypeId::bit_string ||
      target_type_id == CanonicalTypeId::bit_string ||
      source_type_id == CanonicalTypeId::date ||
      target_type_id == CanonicalTypeId::date ||
      source_type_id == CanonicalTypeId::time ||
      target_type_id == CanonicalTypeId::time ||
      source_type_id == CanonicalTypeId::timestamp ||
      target_type_id == CanonicalTypeId::timestamp) {
    return DatatypeCastCategory::forbidden;
  }
  if (target_type_id == CanonicalTypeId::null_type ||
      target_type_id == CanonicalTypeId::unknown) {
    return DatatypeCastCategory::forbidden;
  }
  if (!LookupDatatypeDescriptor(target_type_id).ok()) {
    return DatatypeCastCategory::forbidden;
  }
  if (IsBinary(source_type_id) || IsBinary(target_type_id)) {
    if (source_type_id == target_type_id) {
      return DatatypeCastCategory::identity;
    }
    if ((IsBinary(source_type_id) &&
         (IsCharacter(target_type_id) || IsUuid(target_type_id))) ||
        (IsBinary(target_type_id) &&
         (IsCharacter(source_type_id) || IsUuid(source_type_id)))) {
      return DatatypeCastCategory::lossless_explicit;
    }
    return DatatypeCastCategory::forbidden;
  }
  if (source_type_id == CanonicalTypeId::null_type) { return DatatypeCastCategory::lossless_implicit; }
  if (source_type_id == CanonicalTypeId::unknown) { return DatatypeCastCategory::forbidden; }
  if (!LookupDatatypeDescriptor(source_type_id).ok()) {
    return DatatypeCastCategory::forbidden;
  }
  // The structural UUID/IP-address raw 16-octet carriers, network-prefix raw
  // 18-octet carrier, and MAC-address raw 8-octet carrier are known, but no
  // PRESENT cast operation is admitted without the missing policy receipt.
  // Contextual NULL binding was handled above.
  if (IsUuid(source_type_id) || IsUuid(target_type_id) ||
      IsIpAddress(source_type_id) || IsIpAddress(target_type_id) ||
      IsNetworkPrefix(source_type_id) || IsNetworkPrefix(target_type_id) ||
      IsMacAddress(source_type_id) || IsMacAddress(target_type_id)) {
    return DatatypeCastCategory::forbidden;
  }
  // Contextual NULL binding was admitted above. No PRESENT cast pair touching
  // BFLOAT16, REAL16, REAL32, or REAL64, including identity, has semantic
  // authority.
  if (IsUnresolvedRealSemantics(source_type_id) ||
      IsUnresolvedRealSemantics(target_type_id)) {
    return DatatypeCastCategory::forbidden;
  }
  if (IsDecimal(source_type_id) || IsDecimal(target_type_id)) {
    return DatatypeCastCategory::forbidden;
  }
  if (IsDecimalFloat(source_type_id) || IsDecimalFloat(target_type_id)) {
    return DatatypeCastCategory::forbidden;
  }
  // REAL128 has an exact descriptor-bound LE16 identity operation, but Core
  // has not registered any cross-type PRESENT cast UUID involving it.
  if (IsReal128(source_type_id) || IsReal128(target_type_id)) {
    return source_type_id == target_type_id
        ? DatatypeCastCategory::identity
        : DatatypeCastCategory::forbidden;
  }
  // Core defines these types' value bytes but has not registered any complete
  // non-NULL cast pair, including identity. Contextual base.null binding is
  // admitted above; all other INT16/UINT16/INT32/UINT32/INT64/UINT64 routes
  // remain fail-closed.
  if (source_type_id == CanonicalTypeId::int16 ||
      target_type_id == CanonicalTypeId::int16 ||
      source_type_id == CanonicalTypeId::uint16 ||
      target_type_id == CanonicalTypeId::uint16 ||
      source_type_id == CanonicalTypeId::int32 ||
      target_type_id == CanonicalTypeId::int32 ||
      source_type_id == CanonicalTypeId::uint32 ||
      target_type_id == CanonicalTypeId::uint32 ||
      source_type_id == CanonicalTypeId::int64 ||
      target_type_id == CanonicalTypeId::int64 ||
      source_type_id == CanonicalTypeId::uint64 ||
      target_type_id == CanonicalTypeId::uint64) {
    return DatatypeCastCategory::forbidden;
  }
  if (source_type_id == target_type_id) { return DatatypeCastCategory::identity; }
  if (IsOpaqueRenderOnly(source_type_id) && IsCharacter(target_type_id)) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if (IsOpaqueRenderOnly(source_type_id) || IsOpaqueRenderOnly(target_type_id)) {
    return DatatypeCastCategory::forbidden;
  }
  if (IsInteger(source_type_id) && IsInteger(target_type_id)) {
    if ((source_type_id == CanonicalTypeId::int8 &&
         target_type_id == CanonicalTypeId::uint8) ||
        (source_type_id == CanonicalTypeId::uint8 &&
         target_type_id == CanonicalTypeId::int8)) {
      return DatatypeCastCategory::lossy_explicit;
    }
    if (source_type_id == CanonicalTypeId::uint8 &&
        IsSignedInteger(target_type_id) &&
        IntegerBits(target_type_id) > IntegerBits(source_type_id)) {
      return DatatypeCastCategory::lossless_implicit;
    }
    if (IsSignedInteger(source_type_id) == IsSignedInteger(target_type_id) &&
        IntegerBits(target_type_id) >= IntegerBits(source_type_id)) {
      return DatatypeCastCategory::lossless_implicit;
    }
    if (IntegerBits(target_type_id) >= IntegerBits(source_type_id)) { return DatatypeCastCategory::lossless_explicit; }
    return DatatypeCastCategory::lossy_explicit;
  }
  // Core has not admitted any non-integer UINT8 cast pair yet. Keep those
  // routes fail-closed while retaining the direct decimal-text value bridge.
  if (source_type_id == CanonicalTypeId::uint8 ||
      target_type_id == CanonicalTypeId::uint8) {
    return DatatypeCastCategory::forbidden;
  }
  if (IsNumeric(source_type_id) && IsNumeric(target_type_id)) { return DatatypeCastCategory::lossy_explicit; }
  if (IsInteger(source_type_id) && target_type_id == CanonicalTypeId::boolean) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if ((IsNumeric(source_type_id) || source_type_id == CanonicalTypeId::boolean ||
       IsTemporal(source_type_id) || IsDocument(source_type_id)) &&
      IsCharacter(target_type_id)) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if (IsCharacter(source_type_id) &&
      (IsNumeric(target_type_id) || target_type_id == CanonicalTypeId::boolean ||
       IsTemporal(target_type_id) || IsDocument(target_type_id))) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if ((IsBinaryLike(source_type_id) && IsCharacter(target_type_id)) ||
      (IsCharacter(source_type_id) && IsBinaryLike(target_type_id)) ||
      (IsBinaryLike(source_type_id) && IsBinaryLike(target_type_id))) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if (source_type_id == CanonicalTypeId::set_value || target_type_id == CanonicalTypeId::set_value) {
    return source_type_id == target_type_id ? DatatypeCastCategory::identity : DatatypeCastCategory::lossless_explicit;
  }
  if (reference_compatibility_profile) { return DatatypeCastCategory::reference_compatibility_explicit; }
  return DatatypeCastCategory::forbidden;
}

DatatypeCastResult CastDatatypeValue(const DatatypeCastRequest& request) {
  if (request.value.type_id == CanonicalTypeId::interval ||
      request.target_type_id == CanonicalTypeId::interval) {
    if (const char* code = GenericIntervalStructuralDiagnosticCode(request.value))
      return CastFailure("interval_generic_structural_refusal",
                         DatatypeCastCategory::forbidden, code);
    if (request.target_type_id == CanonicalTypeId::interval &&
        ExecutionDescriptorPresent(request.target_descriptor) &&
        !ExecutionDescriptorValidForType(request.target_descriptor,
                                         CanonicalTypeId::interval))
      return CastFailure("interval_target_descriptor_invalid",
                         DatatypeCastCategory::forbidden,
                         "CTI.INTERVAL.DESCRIPTOR_INVALID");
    return CastFailure("interval_d710_profile_required",
                       DatatypeCastCategory::forbidden,
                       "CTI.INTERVAL.DESCRIPTOR_INVALID");
  }
  if (request.value.type_id == CanonicalTypeId::timestamp ||
      request.target_type_id == CanonicalTypeId::timestamp) {
    if (const char* code =
            GenericTimestampStructuralDiagnosticCode(request.value))
      return CastFailure("timestamp_generic_structural_refusal",
                         DatatypeCastCategory::forbidden, code);
    if (request.target_type_id == CanonicalTypeId::timestamp &&
        ExecutionDescriptorPresent(request.target_descriptor) &&
        !ExecutionDescriptorValidForType(request.target_descriptor,
                                         CanonicalTypeId::timestamp))
      return CastFailure("timestamp_target_descriptor_invalid",
                         DatatypeCastCategory::forbidden,
                         "CTI.TEMPORAL.DESCRIPTOR_INVALID");
    return CastFailure("timestamp_d710_profile_required",
                       DatatypeCastCategory::forbidden,
                       "CTI.TEMPORAL.DESCRIPTOR_INVALID");
  }
  if (request.value.type_id == CanonicalTypeId::time ||
      request.target_type_id == CanonicalTypeId::time) {
    if (const char* code = GenericTimeStructuralDiagnosticCode(request.value))
      return CastFailure("time_generic_structural_refusal",
                         DatatypeCastCategory::forbidden, code);
    if (request.target_type_id == CanonicalTypeId::time &&
        ExecutionDescriptorPresent(request.target_descriptor) &&
        !ExecutionDescriptorValidForType(request.target_descriptor,
                                         CanonicalTypeId::time))
      return CastFailure("time_target_descriptor_invalid",
                         DatatypeCastCategory::forbidden,
                         "CTI.TEMPORAL.DESCRIPTOR_INVALID");
    return CastFailure("time_d710_profile_required",
                       DatatypeCastCategory::forbidden,
                       "CTI.TEMPORAL.DESCRIPTOR_INVALID");
  }
  if (request.value.type_id == CanonicalTypeId::date ||
      request.target_type_id == CanonicalTypeId::date) {
    if (const char* code = GenericDateStructuralDiagnosticCode(request.value))
      return CastFailure("date_generic_structural_refusal",
                         DatatypeCastCategory::forbidden, code);
    if (request.target_type_id == CanonicalTypeId::date &&
        ExecutionDescriptorPresent(request.target_descriptor) &&
        !ExecutionDescriptorValidForType(request.target_descriptor,
                                         CanonicalTypeId::date))
      return CastFailure("date_target_descriptor_invalid",
                         DatatypeCastCategory::forbidden,
                         "CTI.TEMPORAL.DESCRIPTOR_INVALID");
    return CastFailure("date_d710_profile_required",
                       DatatypeCastCategory::forbidden,
                       "CTI.TEMPORAL.DESCRIPTOR_INVALID");
  }
  if (request.value.type_id == CanonicalTypeId::bit_string ||
      request.target_type_id == CanonicalTypeId::bit_string) {
    if (const char* code =
            GenericBitStringStructuralDiagnosticCode(request.value))
      return CastFailure("bit_string_generic_structural_refusal",
                         DatatypeCastCategory::forbidden, code);
    if (request.target_type_id == CanonicalTypeId::bit_string &&
        ExecutionDescriptorPresent(request.target_descriptor) &&
        !ExecutionDescriptorValidForType(request.target_descriptor,
                                         CanonicalTypeId::bit_string))
      return CastFailure("bit_string_target_descriptor_invalid",
                         DatatypeCastCategory::forbidden,
                         "CTB.BIT.DESCRIPTOR_INVALID");
    return CastFailure("bit_string_v3_profile_required",
                       DatatypeCastCategory::forbidden,
                       "CTB.BIT.DESCRIPTOR_INVALID");
  }
  const bool source_is_contextual_null =
      request.value.type_id == CanonicalTypeId::null_type;
  if (source_is_contextual_null &&
      ExecutionDescriptorPresent(request.value.descriptor)) {
    return CastFailure("contextual_null_carries_descriptor",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.DESCRIPTOR.INVALID");
  }
  if ((IsBinary(request.value.type_id) && IsUuid(request.target_type_id) &&
       !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
           request.target_descriptor, CanonicalTypeId::uuid)) ||
      (IsUuid(request.value.type_id) && IsBinary(request.target_type_id) &&
       !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
           request.value.descriptor, CanonicalTypeId::uuid))) {
    return CastFailure("uuid_binary_cast_descriptor_invalid",
                       DatatypeCastCategory::forbidden,
                       "CINL.IDENTITY.DESCRIPTOR_INVALID");
  }
  const bool source_present_descriptor_invalid =
      !request.value.is_null &&
      ((IsDecimal(request.value.type_id) &&
        !DecimalDescriptorValidForPresent(request.value.descriptor)) ||
       (IsDecimalFloat(request.value.type_id) &&
        !DecimalFloatDescriptorValidForPresent(request.value.descriptor)) ||
       (IsUuid(request.value.type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.value.descriptor, request.value.type_id)) ||
       (IsIpAddress(request.value.type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.value.descriptor, request.value.type_id)) ||
       (IsNetworkPrefix(request.value.type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.value.descriptor, request.value.type_id)) ||
       (IsMacAddress(request.value.type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.value.descriptor, request.value.type_id)) ||
       (IsBinary(request.value.type_id) &&
        !BinaryDescriptorExactlyCurrent(request.value.descriptor)) ||
       ((IsUnresolvedRealSemantics(request.value.type_id) ||
         IsReal128(request.value.type_id)) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.value.descriptor, request.value.type_id)) ||
       (ExecutionDescriptorPresent(request.value.descriptor) &&
        !ExecutionDescriptorValidForType(request.value.descriptor,
                                         request.value.type_id)));
  if (!source_is_contextual_null &&
      (request.value.type_id == CanonicalTypeId::unknown ||
       !LookupDatatypeDescriptor(request.value.type_id).ok() ||
       (IsUuid(request.value.type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.value.descriptor, request.value.type_id)) ||
       (IsIpAddress(request.value.type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.value.descriptor, request.value.type_id)) ||
       (IsNetworkPrefix(request.value.type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.value.descriptor, request.value.type_id)) ||
       (IsMacAddress(request.value.type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.value.descriptor, request.value.type_id)) ||
       (IsBinary(request.value.type_id) &&
        !BinaryDescriptorExactlyCurrent(request.value.descriptor)) ||
       (request.value.is_null &&
        !ExecutionDescriptorValidForType(request.value.descriptor,
                                         request.value.type_id)) ||
       source_present_descriptor_invalid)) {
    return CastFailure("source_descriptor_invalid",
                       DatatypeCastCategory::forbidden,
                       IsBinary(request.value.type_id)
                           ? "CTB.BINARY.DESCRIPTOR_INVALID"
                           : "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (!source_is_contextual_null && IsReal128(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    return CastFailure("source_descriptor_invalid",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.DESCRIPTOR.INVALID");
  }
  const bool target_is_unresolved =
      request.target_type_id == CanonicalTypeId::null_type ||
      request.target_type_id == CanonicalTypeId::unknown;
  const bool result_is_null = source_is_contextual_null || request.value.is_null;
  if (target_is_unresolved &&
      (request.target_type_id == CanonicalTypeId::unknown ||
       ExecutionDescriptorPresent(request.target_descriptor))) {
    return CastFailure("unresolved_target_descriptor_invalid",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.DESCRIPTOR.INVALID");
  }
  const bool target_present_descriptor_invalid =
      !result_is_null &&
      ((IsDecimal(request.target_type_id) &&
        !DecimalDescriptorValidForPresent(request.target_descriptor)) ||
       (IsDecimalFloat(request.target_type_id) &&
        !DecimalFloatDescriptorValidForPresent(request.target_descriptor)) ||
       (IsUuid(request.target_type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.target_descriptor, request.target_type_id)) ||
       (IsIpAddress(request.target_type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.target_descriptor, request.target_type_id)) ||
       (IsNetworkPrefix(request.target_type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.target_descriptor, request.target_type_id)) ||
       (IsMacAddress(request.target_type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.target_descriptor, request.target_type_id)) ||
       (IsBinary(request.target_type_id) &&
        !BinaryDescriptorExactlyCurrent(request.target_descriptor)) ||
       ((IsUnresolvedRealSemantics(request.target_type_id) ||
         IsReal128(request.target_type_id)) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.target_descriptor, request.target_type_id)) ||
       (ExecutionDescriptorPresent(request.target_descriptor) &&
        !ExecutionDescriptorValidForType(request.target_descriptor,
                                         request.target_type_id)));
  if (!target_is_unresolved &&
      (!LookupDatatypeDescriptor(request.target_type_id).ok() ||
       (IsUuid(request.target_type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.target_descriptor, request.target_type_id)) ||
       (IsIpAddress(request.target_type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.target_descriptor, request.target_type_id)) ||
       (IsNetworkPrefix(request.target_type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.target_descriptor, request.target_type_id)) ||
       (IsMacAddress(request.target_type_id) &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.target_descriptor, request.target_type_id)) ||
       (IsBinary(request.target_type_id) &&
        !BinaryDescriptorExactlyCurrent(request.target_descriptor)) ||
       (result_is_null &&
        !ExecutionDescriptorValidForType(request.target_descriptor,
                                         request.target_type_id)) ||
       target_present_descriptor_invalid)) {
    return CastFailure("target_descriptor_invalid",
                       DatatypeCastCategory::forbidden,
                       IsBinary(request.target_type_id)
                           ? "CTB.BINARY.DESCRIPTOR_INVALID"
                           : "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (!target_is_unresolved && IsReal128(request.target_type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.target_descriptor, request.target_type_id)) {
    return CastFailure("target_descriptor_invalid",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (!result_is_null &&
      (IsCanonical128Integer(request.target_type_id) ||
       IsReal128(request.target_type_id) ||
       IsDecimal(request.target_type_id) ||
       IsDecimalFloat(request.target_type_id)) &&
      !ExecutionDescriptorPresent(request.target_descriptor)) {
    return CastFailure(
        request.target_type_id == CanonicalTypeId::int128
            ? "int128_target_descriptor_required"
            : request.target_type_id == CanonicalTypeId::uint128
                ? "uint128_target_descriptor_required"
                : request.target_type_id == CanonicalTypeId::real128
                    ? "real128_target_descriptor_required"
                    : request.target_type_id == CanonicalTypeId::decimal
                        ? "decimal_target_descriptor_required"
                        : "decimal_float_target_descriptor_required",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.DESCRIPTOR.INVALID");
  }
  const bool typed_null_identity =
      request.value.is_null &&
      (request.value.type_id == CanonicalTypeId::int16 ||
       request.value.type_id == CanonicalTypeId::uint16 ||
       request.value.type_id == CanonicalTypeId::int32 ||
       request.value.type_id == CanonicalTypeId::uint32 ||
       request.value.type_id == CanonicalTypeId::int64 ||
       request.value.type_id == CanonicalTypeId::uint64 ||
       request.value.type_id == CanonicalTypeId::int128 ||
       request.value.type_id == CanonicalTypeId::uint128 ||
       request.value.type_id == CanonicalTypeId::bfloat16 ||
       request.value.type_id == CanonicalTypeId::real16 ||
       request.value.type_id == CanonicalTypeId::real32 ||
       request.value.type_id == CanonicalTypeId::real64 ||
       request.value.type_id == CanonicalTypeId::real128 ||
       request.value.type_id == CanonicalTypeId::decimal ||
       request.value.type_id == CanonicalTypeId::decimal_float ||
       request.value.type_id == CanonicalTypeId::binary ||
       request.value.type_id == CanonicalTypeId::uuid ||
       request.value.type_id == CanonicalTypeId::ip_address ||
       request.value.type_id == CanonicalTypeId::network_prefix ||
       request.value.type_id == CanonicalTypeId::mac_address) &&
      request.value.type_id == request.target_type_id;
  const bool canonical_128_present_identity =
      !request.value.is_null &&
      (IsCanonical128Integer(request.value.type_id) ||
       IsReal128(request.value.type_id)) &&
      request.target_type_id == request.value.type_id;
  const bool typed_null_identity_descriptor_matches =
      request.value.type_id == CanonicalTypeId::binary
          ? ExecutionDescriptorEqualsIgnoringNullability(
                request.value.descriptor, request.target_descriptor)
          : ExecutionDescriptorEquals(request.value.descriptor,
                                      request.target_descriptor);
  if (typed_null_identity && !typed_null_identity_descriptor_matches) {
    return CastFailure("typed_null_identity_descriptor_mismatch",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (source_is_contextual_null &&
      (!request.value.is_null || !request.value.encoded_value.empty())) {
    return CastFailure("contextual_null_state_invalid",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.NULL_STATE.INVALID");
  }
  if (!result_is_null && IsDecimal(request.value.type_id) &&
      !DecimalCarrierStructurallyCanonical(request.value.encoded_value)) {
    return CastFailure("decimal_source_value_noncanonical",
                       DatatypeCastCategory::forbidden,
                       "NUMERIC.ENCODING.NONCANONICAL");
  }
  if (!result_is_null && IsDecimalFloat(request.value.type_id) &&
      !DecimalFloatCarrierStructurallyCanonical(
          request.value.encoded_value)) {
    return CastFailure("decimal_float_source_value_noncanonical",
                       DatatypeCastCategory::forbidden,
                       "NUMERIC.ENCODING.NONCANONICAL");
  }
  if (!result_is_null &&
      (IsUnresolvedRealSemantics(request.value.type_id) ||
       IsUnresolvedRealSemantics(request.target_type_id))) {
    const auto rejected_type = SelectUnresolvedRealType(
        request.value.type_id, request.target_type_id);
    return CastFailure(
        UnresolvedRealDetail(
            rejected_type,
            "bfloat16_present_cast_policy_unresolved",
            "real16_present_cast_policy_unresolved",
            "real32_present_cast_policy_unresolved",
            "real64_present_cast_policy_unresolved"));
  }
  if (!source_is_contextual_null &&
      !CanonicalOperationValueValid(request.value)) {
    return CastFailure(request.value.is_null
                           ? "null_or_descriptor_state_invalid"
                           : IsCharacter(request.value.type_id)
                               ? CanonicalCharacterFailureDetail(request.value)
                               : "source_value_invalid",
                       DatatypeCastCategory::forbidden,
                       CanonicalOperationValueDiagnosticCode(
                           request.value, "DATATYPE.CAST_FORBIDDEN"));
  }
  if (target_is_unresolved) {
    const bool unresolved_null =
        source_is_contextual_null;
    return CastFailure(
        request.target_type_id == CanonicalTypeId::null_type
            ? (unresolved_null ? "contextual_null_unresolved"
                               : "standalone_null_target_forbidden")
            : "unknown_target_forbidden",
        DatatypeCastCategory::forbidden,
        unresolved_null ? "DATATYPE.CONTEXT_REQUIRED"
                        : (request.target_type_id == CanonicalTypeId::null_type
                               ? "DATATYPE.CAST_FORBIDDEN"
                               : "DATATYPE.DESCRIPTOR.INVALID"));
  }
  if (result_is_null && !request.target_descriptor.nullable_allowed) {
    return CastFailure("target_descriptor_does_not_admit_null",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.NULL_NOT_ADMITTED");
  }
  if (!ValidCastContext(request.context)) {
    return CastFailure("cast_context_not_admitted",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.CAST_FORBIDDEN");
  }
  const auto validate_real128_cast_context = [&request](
      const DatatypeOperationValue* present_value,
      DatatypeCastCategory category) {
    namespace numeric = scratchbird::libraries::sbl_numeric;
    numeric::NumericContext context;
    context.precision = request.numeric_context.precision;
    context.scale = request.numeric_context.scale;
    context.allow_special_values =
        request.numeric_context.allow_special_values;
    switch (request.numeric_context.rounding) {
      case DatatypeRoundingMode::half_even:
        context.rounding = numeric::RoundingMode::half_even;
        break;
      case DatatypeRoundingMode::half_up:
        context.rounding = numeric::RoundingMode::half_up;
        break;
      case DatatypeRoundingMode::truncate:
        context.rounding = numeric::RoundingMode::truncate;
        break;
      default:
        context.rounding = static_cast<numeric::RoundingMode>(99);
        break;
    }

    numeric::Real128Bytes bytes{};
    if (present_value == nullptr) {
      // NULL suppresses value work, but not backend/context validation.
      bytes[14] = 0xffu;
      bytes[15] = 0x3fu;
    } else if (!DecodeReal128Carrier(present_value->encoded_value, &bytes)) {
      auto failed = CastFailure("real128_identity_payload_invalid", category,
                                "NUMERIC.ENCODING.NONCANONICAL");
      failed.numeric_facts.invalid = true;
      return failed;
    }

    numeric::Real128BinaryRequest binary_request;
    binary_request.operation = numeric::NumericOperation::canonicalize;
    binary_request.context = context;
    binary_request.left = bytes;
    const auto backend = numeric::ApplyReal128BinaryOperation(binary_request);
    if (backend.numeric.status != numeric::NumericStatusCode::ok ||
        (present_value != nullptr &&
         (!backend.bytes || *backend.bytes != bytes))) {
      auto failed = CastFailure(
          "real128_cast_context_rejected", category,
          backend.numeric.diagnostic_code.empty()
              ? "NUMERIC.REAL128.INVALID"
              : backend.numeric.diagnostic_code);
      failed.numeric_facts = NumericFacts(backend.numeric);
      if (backend.numeric.diagnostic_code.empty()) {
        failed.numeric_facts.invalid = true;
      }
      return failed;
    }

    DatatypeCastResult validated;
    validated.status = OkStatus();
    validated.category = category;
    validated.numeric_facts = NumericFacts(backend.numeric);
    validated.diagnostic = MakeDatatypeOperationDiagnostic(
        validated.status, "SB_DATATYPE_OK", "datatype.ok");
    return validated;
  };
  if (source_is_contextual_null) {
    DatatypeCastResult real128_context;
    if (request.target_type_id == CanonicalTypeId::real128) {
      real128_context = validate_real128_cast_context(
          nullptr, DatatypeCastCategory::lossless_implicit);
      if (!real128_context.ok()) return real128_context;
    }
    DatatypeCastResult result;
    result.status = OkStatus();
    result.category = DatatypeCastCategory::lossless_implicit;
    result.value = {request.target_type_id, {}, true};
    result.value.descriptor = request.target_descriptor;
    if (request.target_type_id == CanonicalTypeId::real128) {
      result.numeric_facts = real128_context.numeric_facts;
    }
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_OK", "datatype.ok");
    return result;
  }
  // Typed NULL identity is a state/descriptor validation, not a non-NULL
  // value-conversion policy. Preserve it even when a datatype's present-value
  // cast table remains unresolved.
  if (typed_null_identity) {
    DatatypeCastResult real128_context;
    if (request.value.type_id == CanonicalTypeId::real128) {
      real128_context = validate_real128_cast_context(
          nullptr, DatatypeCastCategory::identity);
      if (!real128_context.ok()) return real128_context;
    }
    DatatypeCastResult null_identity;
    null_identity.status = OkStatus();
    null_identity.category = DatatypeCastCategory::identity;
    null_identity.value = request.value;
    if (request.value.type_id == CanonicalTypeId::real128) {
      null_identity.numeric_facts = real128_context.numeric_facts;
    }
    null_identity.diagnostic = MakeDatatypeOperationDiagnostic(
        null_identity.status, "SB_DATATYPE_OK", "datatype.ok");
    return null_identity;
  }
  const bool binary_incident = IsBinary(request.value.type_id) ||
      IsBinary(request.target_type_id);
  if (binary_incident) {
    const auto category = ClassifyDatatypeCast(
        request.value.type_id, request.target_type_id,
        request.reference_compatibility_profile);
    if (category == DatatypeCastCategory::forbidden) {
      return CastFailure("base_binary_ordered_pair_forbidden");
    }
    // The registered context is authoritative for base.binary. The legacy
    // convenience flag cannot upgrade an implicit or assignment request.
    const bool explicit_request =
        request.context == DatatypeCastContext::explicit_cast;
    if (category == DatatypeCastCategory::lossless_explicit &&
        !explicit_request) {
      return CastFailure("explicit_cast_required", category);
    }
    try {
      if ((IsBinary(request.value.type_id) &&
           !BinaryDescriptorExactlyCurrent(request.value.descriptor)) ||
          (IsBinary(request.target_type_id) &&
           !BinaryDescriptorExactlyCurrent(request.target_descriptor))) {
        return CastFailure("binary_cast_descriptor_invalid", category,
                           "CTB.BINARY.DESCRIPTOR_INVALID");
      }
      if ((IsCharacter(request.value.type_id) &&
           !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
               request.value.descriptor, CanonicalTypeId::character)) ||
          (IsCharacter(request.target_type_id) &&
           !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
               request.target_descriptor, CanonicalTypeId::character)) ||
          (IsUuid(request.value.type_id) &&
           !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
               request.value.descriptor, CanonicalTypeId::uuid)) ||
          (IsUuid(request.target_type_id) &&
           !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
               request.target_descriptor, CanonicalTypeId::uuid))) {
        return CastFailure("binary_cast_descriptor_invalid", category,
                           "DATATYPE.DESCRIPTOR.INVALID");
      }
      if (request.value.type_id == request.target_type_id &&
          !ExecutionDescriptorEqualsIgnoringNullability(
              request.value.descriptor, request.target_descriptor)) {
        return CastFailure("binary_identity_descriptor_mismatch",
                           DatatypeCastCategory::forbidden,
                           "CTB.BINARY.DESCRIPTOR_INVALID");
      }
      DatatypeCastResult converted;
      converted.status = OkStatus();
      converted.category = category;
      converted.value.type_id = request.target_type_id;
      converted.value.is_null = request.value.is_null;
      converted.value.descriptor = request.target_descriptor;
      converted.diagnostic = MakeDatatypeOperationDiagnostic(
          converted.status, "SB_DATATYPE_OK", "datatype.ok");
      if (request.value.is_null) return converted;

      if (IsBinary(request.value.type_id) &&
          IsBinary(request.target_type_id)) {
        converted.value.encoded_value = request.value.encoded_value;
        return converted;
      }
      if (IsCharacter(request.value.type_id) &&
          IsBinary(request.target_type_id)) {
        if (!DecodeStrictBinaryHex(request.value.encoded_value,
                                   &converted.value.encoded_value)) {
          return CastFailure("strict_lowercase_0x_hex_required", category,
                             "CTB.BINARY.CAST_HEX_INVALID");
        }
        return converted;
      }
      if (IsBinary(request.value.type_id) &&
          IsCharacter(request.target_type_id)) {
        if (request.value.encoded_value.size() >
            (kCanonicalCharacterMaximumBytes - 2u) / 2u) {
          return CastFailure("binary_character_target_length_exceeded", category,
                             "CTB.BINARY.LENGTH_EXCEEDED");
        }
        converted.value.encoded_value =
            "0x" + HexEncodeLower(request.value.encoded_value);
        return converted;
      }
      if (IsUuid(request.value.type_id) &&
          IsBinary(request.target_type_id)) {
        converted.value.encoded_value = request.value.encoded_value;
        return converted;
      }
      if (IsBinary(request.value.type_id) &&
          IsUuid(request.target_type_id)) {
        if (request.value.encoded_value.size() != 16u) {
          return CastFailure("binary_uuid_requires_exactly_16_octets", category,
                             "DATATYPE.CAST_FORBIDDEN");
        }
        converted.value.encoded_value = request.value.encoded_value;
        return converted;
      }
      return CastFailure("base_binary_ordered_pair_forbidden");
    } catch (const std::bad_alloc&) {
      auto failure = CastFailure("binary_cast_allocation_failed", category,
                                 "CTB.BINARY.RESOURCE_EXHAUSTED");
      failure.status = ResourceErrorStatus();
      failure.diagnostic.status = failure.status;
      return failure;
    }
  }
  if (IsUuid(request.value.type_id) || IsUuid(request.target_type_id)) {
    return CastFailure(
        result_is_null
            ? "uuid_cross_type_typed_null_cast_policy_unresolved"
            : "uuid_present_cast_policy_unresolved");
  }
  if (IsIpAddress(request.value.type_id) ||
      IsIpAddress(request.target_type_id)) {
    return CastFailure(
        result_is_null
            ? "ip_address_cross_type_typed_null_cast_policy_unresolved"
            : "ip_address_present_cast_policy_unresolved");
  }
  if (IsNetworkPrefix(request.value.type_id) ||
      IsNetworkPrefix(request.target_type_id)) {
    return CastFailure(
        result_is_null
            ? "network_prefix_cross_type_typed_null_cast_policy_unresolved"
            : "network_prefix_present_cast_policy_unresolved");
  }
  if (IsMacAddress(request.value.type_id) ||
      IsMacAddress(request.target_type_id)) {
    return CastFailure(
        result_is_null
            ? "mac_address_cross_type_typed_null_cast_policy_unresolved"
            : "mac_address_present_cast_policy_unresolved");
  }
  if (IsDecimal(request.value.type_id) ||
      IsDecimal(request.target_type_id)) {
    return CastFailure(
        result_is_null
            ? "decimal_cross_type_typed_null_cast_policy_unresolved"
            : "decimal_present_cast_policy_unresolved");
  }
  if (IsDecimalFloat(request.value.type_id) ||
      IsDecimalFloat(request.target_type_id)) {
    return CastFailure(
        result_is_null
            ? "decimal_float_cross_type_typed_null_cast_policy_unresolved"
            : "decimal_float_present_cast_policy_unresolved");
  }
  if (canonical_128_present_identity) {
    if (!ExecutionDescriptorEquals(request.value.descriptor,
                                   request.target_descriptor)) {
      return CastFailure(
          request.value.type_id == CanonicalTypeId::int128
              ? "int128_identity_descriptor_mismatch"
              : request.value.type_id == CanonicalTypeId::uint128
                  ? "uint128_identity_descriptor_mismatch"
                  : "real128_identity_descriptor_mismatch",
                         DatatypeCastCategory::forbidden,
                         "DATATYPE.DESCRIPTOR.INVALID");
    }
    DatatypeCastResult real128_context;
    if (request.value.type_id == CanonicalTypeId::real128) {
      real128_context = validate_real128_cast_context(
          &request.value, DatatypeCastCategory::identity);
      if (!real128_context.ok()) return real128_context;
    }
    DatatypeCastResult identity;
    identity.status = OkStatus();
    identity.category = DatatypeCastCategory::identity;
    identity.value = request.value;
    if (request.value.type_id == CanonicalTypeId::real128) {
      identity.numeric_facts = real128_context.numeric_facts;
    }
    identity.diagnostic = MakeDatatypeOperationDiagnostic(
        identity.status, "SB_DATATYPE_OK", "datatype.ok");
    return identity;
  }
  if (IsReal128(request.value.type_id) ||
      IsReal128(request.target_type_id)) {
    return CastFailure(
        result_is_null
            ? "real128_cross_type_typed_null_cast_policy_unresolved"
            : "real128_present_cast_policy_unresolved");
  }
  DatatypeCastResult result;
  result.category = ClassifyDatatypeCast(request.value.type_id, request.target_type_id, request.reference_compatibility_profile);
  if (result.category == DatatypeCastCategory::forbidden) {
    return CastFailure(std::string(CanonicalTypeName(request.value.type_id)) + "->" + CanonicalTypeName(request.target_type_id));
  }
  const bool checked_assignment =
      ((request.target_type_id == CanonicalTypeId::int8 &&
        IsNumeric(request.value.type_id)) ||
       (request.target_type_id == CanonicalTypeId::uint8 &&
        IsInteger(request.value.type_id))) &&
      request.context == DatatypeCastContext::assignment &&
      (result.category == DatatypeCastCategory::lossless_explicit ||
       result.category == DatatypeCastCategory::lossy_explicit);
  const bool checked_request = request.explicit_cast ||
      request.context == DatatypeCastContext::explicit_cast ||
      checked_assignment;
  if (!checked_request && result.category != DatatypeCastCategory::identity &&
      result.category != DatatypeCastCategory::lossless_implicit) {
    return CastFailure("explicit_cast_required", result.category);
  }
  if (request.value.is_null &&
      request.value.type_id == request.target_type_id &&
      !ExecutionDescriptorEquals(request.value.descriptor,
                                 request.target_descriptor)) {
    return CastFailure("typed_null_identity_descriptor_mismatch",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.CAST_FORBIDDEN");
  }
  if (request.value.is_null &&
      request.value.type_id != request.target_type_id &&
      (DescriptorHasDomainBinding(request.value.descriptor) ||
       DescriptorHasDomainBinding(request.target_descriptor))) {
    return CastFailure("domain_binding_cast_policy_unavailable",
                       DatatypeCastCategory::forbidden,
                       "DATATYPE.CAST_FORBIDDEN");
  }
  if (request.value.is_null) {
    result.status = OkStatus();
    result.value.type_id = request.target_type_id;
    result.value.is_null = true;
    result.value.descriptor = request.target_descriptor;
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_OK", "datatype.ok");
    return result;
  }
  result.status = OkStatus();
  result.value.type_id = request.target_type_id;
  result.value.is_null = request.value.is_null;
  result.value.encoded_value = request.value.encoded_value;
  if (ExecutionDescriptorPresent(request.target_descriptor)) {
    result.value.descriptor = request.target_descriptor;
  }
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");

  const std::string value = IntegerOperationText(
      request.value.type_id, request.value.encoded_value);
  if (IsCanonical128Integer(request.value.type_id) &&
      request.target_type_id != request.value.type_id) {
    // Preserve previously admitted boundary-text conversions without leaking
    // the native LE16 carrier into another datatype's representation.
    result.value.encoded_value = value;
  }
  if (IsBinaryLike(request.value.type_id) && request.target_type_id == CanonicalTypeId::character) {
    result.value.encoded_value = HexEncodeLower(value);
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::character && IsBinaryLike(request.target_type_id)) {
    bool ok = false;
    result.value.encoded_value = HexDecodeStrict(value, true, &ok);
    if (!ok) { return CastFailure("binary_hex_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::character) {
    if (request.value.type_id == CanonicalTypeId::boolean) {
      result.value.encoded_value =
          static_cast<unsigned char>(value[0]) == 1u ? "TRUE" : "FALSE";
    } else if (request.value.type_id == CanonicalTypeId::int8 ||
               request.value.type_id == CanonicalTypeId::uint8 ||
               IsCanonical128Integer(request.value.type_id)) {
      result.value.encoded_value = value;
    }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::decimal) {
    DatatypeNumericContext context;
    context.precision = 38;
    context.scale = 0;
    if (!CanonicalizeExactDecimalText(value, context, &result.value.encoded_value)) {
      return CastFailure("decimal_invalid_or_out_of_range", result.category);
    }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::decimal_float) {
    DatatypeNumericContext context;
    context.precision = 34;
    context.allow_special_values = true;
    if (!CanonicalizeDecimalFloatText(value, context, &result.value.encoded_value)) {
      return CastFailure("decimal_float_invalid_or_out_of_range", result.category);
    }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::int128 ||
      request.target_type_id == CanonicalTypeId::uint128) {
    if (!IsInteger(request.value.type_id) && !IsCharacter(request.value.type_id)) {
      return CastFailure("numeric_source_required", result.category);
    }
    namespace numeric = scratchbird::libraries::sbl_numeric;
    const auto backend_type = request.target_type_id == CanonicalTypeId::int128
                                  ? numeric::NumericType::int128
                                  : numeric::NumericType::uint128;
    numeric::NumericRequest backend_request;
    backend_request.type = backend_type;
    backend_request.operation = numeric::NumericOperation::canonicalize;
    backend_request.left = {backend_type, value, false};
    const auto backend_result = numeric::ApplyNumericOperation(backend_request);
    if (backend_result.status != numeric::NumericStatusCode::ok) {
      return CastFailure(backend_result.diagnostic_code.empty()
                             ? "integer_out_of_range_or_invalid"
                             : backend_result.diagnostic_code,
                         result.category,
                         backend_result.status == numeric::NumericStatusCode::overflow
                             ? "NUMERIC.CAST.OUT_OF_RANGE"
                             : "DATATYPE.CAST_FORBIDDEN");
    }
    if (request.target_type_id == CanonicalTypeId::int128) {
      if (!EncodeCanonicalInt128Value(backend_result.value.encoded,
                                      &result.value.encoded_value)) {
        return CastFailure("NUMERIC.ENCODING.NONCANONICAL",
                           result.category);
      }
    } else {
      if (!EncodeCanonicalUint128Value(backend_result.value.encoded,
                                       &result.value.encoded_value)) {
        return CastFailure("NUMERIC.ENCODING.NONCANONICAL",
                           result.category);
      }
    }
    return result;
  }
  if (IsInteger(request.target_type_id)) {
    if (request.target_type_id == CanonicalTypeId::int8 &&
        IsNumeric(request.value.type_id) &&
        !IsInteger(request.value.type_id)) {
      if (!EncodeIntegralNumericTextAsInt8(
              value, &result.value.encoded_value)) {
        return CastFailure("integer_out_of_range_or_non_integral",
                           result.category);
      }
      return result;
    }
    if (IsInteger(request.value.type_id) || IsCharacter(request.value.type_id)) {
      if (!IntegerFits(request.target_type_id, value)) {
        return CastFailure("integer_out_of_range_or_invalid", result.category);
      }
      const std::string normalized = TrimLeadingZeros(value);
      if (request.target_type_id == CanonicalTypeId::int8 ||
          request.target_type_id == CanonicalTypeId::uint8) {
        const bool encoded = request.target_type_id == CanonicalTypeId::int8
            ? EncodeInt8DecimalText(normalized, &result.value.encoded_value)
            : EncodeUint8DecimalText(normalized, &result.value.encoded_value);
        if (!encoded) {
          return CastFailure("integer_out_of_range_or_invalid", result.category);
        }
      } else {
        result.value.encoded_value = normalized;
      }
      return result;
    }
    return CastFailure("numeric_source_required", result.category);
  }
  if (IsReal(request.target_type_id)) {
    if (!FloatingText(value)) { return CastFailure("real_or_decimal_invalid", result.category); }
    if (request.value.type_id == CanonicalTypeId::int8 ||
        request.value.type_id == CanonicalTypeId::uint8 ||
        IsCanonical128Integer(request.value.type_id)) {
      result.value.encoded_value = value;
    }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::boolean) {
    if (request.value.type_id == CanonicalTypeId::boolean) {
      return result;
    }
    const std::string lower = LowerAscii(value);
    if (lower == "true" || lower == "1") {
      result.value.encoded_value.assign(1, static_cast<char>(1));
      return result;
    }
    if (lower == "false" || lower == "0") {
      result.value.encoded_value.assign(1, static_cast<char>(0));
      return result;
    }
    return CastFailure("boolean_invalid", result.category);
  }
  if (request.target_type_id == CanonicalTypeId::time) {
    if (!TimeText(value)) { return CastFailure("time_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::set_value) {
    EncodedSetFrame frame;
    if (!ParseEncodedSet(value, &frame)) {
      return CastFailure("set_encoding_invalid", result.category);
    }
  }
  if (request.target_type_id == CanonicalTypeId::json_document ||
      request.target_type_id == CanonicalTypeId::binary_json_document ||
      request.target_type_id == CanonicalTypeId::bson_document ||
      request.target_type_id == CanonicalTypeId::object_document ||
      request.target_type_id == CanonicalTypeId::flattened_object_document ||
      request.target_type_id == CanonicalTypeId::document) {
    if (!JsonText(value)) { return CastFailure("json_document_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::xml_document) {
    if (!XmlText(value)) { return CastFailure("xml_document_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::hstore_document) {
    return CastFailure("hstore_document_requires_domain_or_reference_wire_profile", result.category);
  }
  return result;
}

DatatypeExtractResult ExtractDatatypeField(const DatatypeExtractRequest& request) {
  if (request.value.type_id == CanonicalTypeId::interval) {
    if (const char* code = GenericIntervalStructuralDiagnosticCode(request.value))
      return ExtractFailure("interval_generic_structural_refusal", code);
    return ExtractFailure("interval_d710_profile_required",
                          "CTI.INTERVAL.DESCRIPTOR_INVALID");
  }
  if (request.value.type_id == CanonicalTypeId::timestamp) {
    if (const char* code =
            GenericTimestampStructuralDiagnosticCode(request.value))
      return ExtractFailure("timestamp_generic_structural_refusal", code);
    return ExtractFailure("timestamp_d710_profile_required",
                          "CTI.TEMPORAL.DESCRIPTOR_INVALID");
  }
  if (request.value.type_id == CanonicalTypeId::time) {
    if (const char* code = GenericTimeStructuralDiagnosticCode(request.value))
      return ExtractFailure("time_generic_structural_refusal", code);
    return ExtractFailure("time_d710_profile_required",
                          "CTI.TEMPORAL.DESCRIPTOR_INVALID");
  }
  if (request.value.type_id == CanonicalTypeId::date) {
    if (const char* code = GenericDateStructuralDiagnosticCode(request.value))
      return ExtractFailure("date_generic_structural_refusal", code);
    return ExtractFailure("date_d710_profile_required",
                          "CTI.TEMPORAL.DESCRIPTOR_INVALID");
  }
  if (IsBinary(request.value.type_id) &&
      !BinaryDescriptorExactlyCurrent(request.value.descriptor)) {
    return ExtractFailure("binary_extract_descriptor_invalid",
                          "CTB.BINARY.DESCRIPTOR_INVALID");
  }
  if (IsUuid(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    return ExtractFailure("uuid_extract_descriptor_invalid",
                          "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsIpAddress(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    return ExtractFailure("ip_address_extract_descriptor_invalid",
                          "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsNetworkPrefix(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    return ExtractFailure("network_prefix_extract_descriptor_invalid",
                          "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsMacAddress(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    return ExtractFailure("mac_address_extract_descriptor_invalid",
                          "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsCharacter(request.value.type_id) &&
      (!ExecutionDescriptorPresent(request.value.descriptor) ||
       !ExecutionDescriptorValidForType(request.value.descriptor,
                                        request.value.type_id))) {
    return ExtractFailure("character_extract_descriptor_invalid",
                          "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsReal128(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    return ExtractFailure("real128_extract_descriptor_invalid",
                          "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (!CanonicalOperationValueValid(request.value)) {
    const bool bfloat16_value =
        request.value.type_id == CanonicalTypeId::bfloat16;
    const bool real16_value =
        request.value.type_id == CanonicalTypeId::real16;
    const bool real32_value =
        request.value.type_id == CanonicalTypeId::real32;
    const bool real64_value =
        request.value.type_id == CanonicalTypeId::real64;
    const bool decimal_value = IsDecimal(request.value.type_id);
    const bool decimal_float_value = IsDecimalFloat(request.value.type_id);
    const bool uuid_value = IsUuid(request.value.type_id);
    const bool ip_address_value = IsIpAddress(request.value.type_id);
    const bool network_prefix_value = IsNetworkPrefix(request.value.type_id);
    const bool mac_address_value = IsMacAddress(request.value.type_id);
    const bool character_value = IsCharacter(request.value.type_id);
    const bool descriptor_or_null_state_failure =
        request.value.type_id == CanonicalTypeId::unknown ||
        request.value.type_id == CanonicalTypeId::null_type ||
        request.value.is_null ||
        bfloat16_value || real16_value || real32_value || real64_value ||
        decimal_value || decimal_float_value || uuid_value || ip_address_value ||
        network_prefix_value || mac_address_value || character_value;
    const char* diagnostic_code = descriptor_or_null_state_failure
        ? CanonicalOperationValueDiagnosticCode(
              request.value, "SB_DATATYPE_EXTRACT_REJECTED")
        : "SB_DATATYPE_EXTRACT_REJECTED";
    return ExtractFailure(
        bfloat16_value
            ? (std::string_view(diagnostic_code) ==
                       "DATATYPE.DESCRIPTOR.INVALID"
                   ? "bfloat16_extract_descriptor_invalid"
                   : "bfloat16_extract_policy_unresolved")
            : real16_value
                ? (std::string_view(diagnostic_code) ==
                           "DATATYPE.DESCRIPTOR.INVALID"
                       ? "real16_extract_descriptor_invalid"
                       : "real16_extract_policy_unresolved")
            : real32_value
                ? (std::string_view(diagnostic_code) ==
                           "DATATYPE.DESCRIPTOR.INVALID"
                       ? "real32_extract_descriptor_invalid"
                       : "real32_extract_policy_unresolved")
            : real64_value
                ? (std::string_view(diagnostic_code) ==
                           "DATATYPE.DESCRIPTOR.INVALID"
                       ? "real64_extract_descriptor_invalid"
                       : "real64_extract_policy_unresolved")
            : decimal_value
                ? (std::string_view(diagnostic_code) ==
                           "DATATYPE.DESCRIPTOR.INVALID"
                       ? "decimal_extract_descriptor_invalid"
                       : std::string_view(diagnostic_code) ==
                                 "NUMERIC.ENCODING.NONCANONICAL"
                           ? "decimal_extract_value_noncanonical"
                           : "decimal_extract_policy_unresolved")
            : decimal_float_value
                ? (std::string_view(diagnostic_code) ==
                           "DATATYPE.DESCRIPTOR.INVALID"
                       ? "decimal_float_extract_descriptor_invalid"
                       : std::string_view(diagnostic_code) ==
                                 "NUMERIC.ENCODING.NONCANONICAL"
                           ? "decimal_float_extract_value_noncanonical"
                           : "decimal_float_extract_policy_unresolved")
            : uuid_value
                ? (std::string_view(diagnostic_code) ==
                           "DATATYPE.DESCRIPTOR.INVALID"
                       ? "uuid_extract_descriptor_invalid"
                       : "uuid_extract_value_invalid")
            : ip_address_value
                ? (std::string_view(diagnostic_code) ==
                           "DATATYPE.DESCRIPTOR.INVALID"
                       ? "ip_address_extract_descriptor_invalid"
                       : "ip_address_extract_value_invalid")
            : network_prefix_value
                ? (std::string_view(diagnostic_code) ==
                           "DATATYPE.DESCRIPTOR.INVALID"
                       ? "network_prefix_extract_descriptor_invalid"
                       : "network_prefix_extract_value_invalid")
            : mac_address_value
                ? (std::string_view(diagnostic_code) ==
                           "DATATYPE.DESCRIPTOR.INVALID"
                       ? "mac_address_extract_descriptor_invalid"
                       : "mac_address_extract_value_invalid")
            : character_value
                ? CanonicalCharacterFailureDetail(request.value)
            : request.value.is_null ? "null_or_descriptor_state_invalid"
                                    : "canonical_value_invalid",
        diagnostic_code);
  }
  if (IsUnresolvedRealSemantics(request.value.type_id)) {
    return ExtractFailure(UnresolvedRealDetail(
        request.value.type_id,
        "bfloat16_extract_policy_unresolved",
        "real16_extract_policy_unresolved",
        "real32_extract_policy_unresolved",
        "real64_extract_policy_unresolved"));
  }
  if (IsReal128(request.value.type_id)) {
    return ExtractFailure("real128_extract_policy_unresolved");
  }
  if (IsDecimal(request.value.type_id) && !request.value.is_null) {
    return ExtractFailure("decimal_extract_policy_unresolved");
  }
  if (IsDecimalFloat(request.value.type_id) && !request.value.is_null) {
    return ExtractFailure("decimal_float_extract_policy_unresolved");
  }
  if (IsUuid(request.value.type_id)) {
    return ExtractFailure("uuid_extract_policy_unresolved");
  }
  if (IsIpAddress(request.value.type_id)) {
    return ExtractFailure("ip_address_extract_policy_unresolved");
  }
  if (IsNetworkPrefix(request.value.type_id)) {
    return ExtractFailure("network_prefix_extract_policy_unresolved");
  }
  if (IsMacAddress(request.value.type_id)) {
    return ExtractFailure("mac_address_extract_policy_unresolved");
  }
  DatatypeExtractResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (IsOpaqueRenderOnly(request.value.type_id)) {
    return ExtractFailure("opaque_extract_rejected");
  }
  const std::string field = LowerAscii(request.field);
  if (IsTemporal(request.value.type_id)) {
    return ExtractFailure("temporal_int32_result_profile_unresolved");
  }
  if (request.value.is_null) {
    CanonicalTypeId result_type = CanonicalTypeId::unknown;
    if (request.value.type_id == CanonicalTypeId::character &&
               (field == "character_length" || field == "octet_length" || field == "length")) {
      result_type = CanonicalTypeId::uint64;
    } else if ((request.value.type_id == CanonicalTypeId::binary ||
                request.value.type_id == CanonicalTypeId::blob) &&
               (field == "octet_length" || field == "length")) {
      result_type = CanonicalTypeId::uint64;
    }
    if (result_type == CanonicalTypeId::unknown) {
      return ExtractFailure("unsupported_null_extract_field:" + field);
    }
    if ((IsBinary(request.value.type_id) &&
         !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
             request.result_descriptor, result_type)) ||
        (!IsBinary(request.value.type_id) &&
         !ExecutionDescriptorValidForType(request.result_descriptor,
                                          result_type))) {
      return ExtractFailure("null_extract_result_descriptor_invalid",
                            "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (!request.result_descriptor.nullable_allowed) {
      return ExtractFailure("null_extract_result_not_nullable",
                            "DATATYPE.NULL_NOT_ADMITTED");
    }
    result.value = {result_type, {}, true};
    result.value.descriptor = request.result_descriptor;
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::binary) {
    if (field == "octet_length" || field == "length") {
      if (!ExecutionDescriptorExactlyMatchesCurrentBuiltin(
              request.result_descriptor, CanonicalTypeId::uint64)) {
        return ExtractFailure("binary_length_result_descriptor_invalid",
                              "DATATYPE.DESCRIPTOR.INVALID");
      }
      result.value = UInt64Value(request.value.encoded_value.size());
      result.value.descriptor = request.result_descriptor;
      return result;
    }
    return ExtractFailure("unsupported_binary_field:" + field);
  }
  const std::string value = request.value.encoded_value;
  if (request.value.type_id == CanonicalTypeId::character) {
    if (field == "character_length" || field == "length") {
      std::uint64_t scalar_count = 0;
      if (!CanonicalCharacterPayloadValid(value, &scalar_count)) {
        return ExtractFailure(CanonicalCharacterFailureDetail(request.value),
                              CanonicalOperationValueDiagnosticCode(
                                  request.value,
                                  "SB_DATATYPE_EXTRACT_REJECTED"));
      }
      result.value = UInt64Value(scalar_count);
      return result;
    }
    if (field == "octet_length") {
      result.value = UInt64Value(value.size());
      return result;
    }
    return ExtractFailure("unsupported_text_field:" + field);
  }
  if (request.value.type_id == CanonicalTypeId::blob) {
    if (field == "octet_length" || field == "length") {
      result.value = UInt64Value(value.size());
      return result;
    }
    return ExtractFailure("unsupported_binary_field:" + field);
  }
  if (request.value.type_id == CanonicalTypeId::set_value && field == "cardinality") {
    EncodedSetFrame frame;
    if (!ParseEncodedSet(value, &frame)) { return ExtractFailure("set_encoding_invalid"); }
    result.value = UInt64Value(frame.items.size());
    return result;
  }
  return ExtractFailure("unsupported_extract_type:" + std::string(CanonicalTypeName(request.value.type_id)));
}

DatatypeSetOperationResult EncodeSetValue(const DatatypeSetDescriptor& descriptor,
                                          const std::vector<DatatypeOperationValue>& values) {
  if (descriptor.element_type_id == CanonicalTypeId::unknown ||
      descriptor.element_type_id == CanonicalTypeId::null_type) {
    return SetFailure("set_element_descriptor_required",
                      "DATATYPE.DESCRIPTOR.INVALID");
  }
  const bool element_descriptor_present =
      ExecutionDescriptorPresent(descriptor.element_descriptor);
  if ((IsPolicyRestrictedReal(descriptor.element_type_id) ||
       IsDecimal(descriptor.element_type_id) ||
       IsDecimalFloat(descriptor.element_type_id) ||
       IsUuid(descriptor.element_type_id) ||
       IsIpAddress(descriptor.element_type_id) ||
       IsNetworkPrefix(descriptor.element_type_id) ||
       IsMacAddress(descriptor.element_type_id) ||
       IsCharacter(descriptor.element_type_id) ||
       descriptor.allow_null_elements || element_descriptor_present) &&
      !(IsPolicyRestrictedReal(descriptor.element_type_id)
            ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                  descriptor.element_descriptor, descriptor.element_type_id)
            : IsDecimal(descriptor.element_type_id)
                ? DecimalDescriptorValidForPresent(
                      descriptor.element_descriptor)
            : IsDecimalFloat(descriptor.element_type_id)
                ? DecimalFloatDescriptorValidForPresent(
                      descriptor.element_descriptor)
            : IsUuid(descriptor.element_type_id)
                ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                      descriptor.element_descriptor,
                      descriptor.element_type_id)
            : IsIpAddress(descriptor.element_type_id)
                ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                      descriptor.element_descriptor,
                      descriptor.element_type_id)
            : IsNetworkPrefix(descriptor.element_type_id)
                ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                      descriptor.element_descriptor,
                      descriptor.element_type_id)
            : IsMacAddress(descriptor.element_type_id)
                ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                      descriptor.element_descriptor,
                      descriptor.element_type_id)
            : IsCharacter(descriptor.element_type_id)
                ? ExecutionDescriptorPresent(descriptor.element_descriptor) &&
                      ExecutionDescriptorValidForType(
                          descriptor.element_descriptor,
                          descriptor.element_type_id)
            : ExecutionDescriptorValidForType(descriptor.element_descriptor,
                                              descriptor.element_type_id))) {
    return SetFailure("set_element_descriptor_invalid",
                      "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (descriptor.allow_null_elements &&
      !descriptor.element_descriptor.nullable_allowed) {
    return SetFailure("set_element_descriptor_not_nullable",
                      "DATATYPE.NULL_NOT_ADMITTED");
  }
  if (IsUnresolvedRealSemantics(descriptor.element_type_id)) {
    return SetFailure(UnresolvedRealDetail(
        descriptor.element_type_id,
        "bfloat16_set_semantics_policy_unresolved",
        "real16_set_semantics_policy_unresolved",
        "real32_set_semantics_policy_unresolved",
        "real64_set_semantics_policy_unresolved"));
  }
  if (IsReal128(descriptor.element_type_id)) {
    return SetFailure("real128_set_semantics_policy_unresolved");
  }
  if (IsCharacter(descriptor.element_type_id)) {
    return SetFailure("character_set_collation_policy_unresolved");
  }
  std::vector<std::string> encoded_items;
  std::set<std::string> unique_items;
  for (const auto& value : values) {
    if (!CanonicalOperationValueValid(value)) {
      const char* failure_detail =
          value.type_id == CanonicalTypeId::bfloat16 && !value.is_null
              ? "bfloat16_set_element_policy_unresolved"
              : value.type_id == CanonicalTypeId::real16 && !value.is_null
                  ? "real16_set_element_policy_unresolved"
                  : value.type_id == CanonicalTypeId::real32 && !value.is_null
                      ? "real32_set_element_policy_unresolved"
                  : value.type_id == CanonicalTypeId::real64 && !value.is_null
                      ? "real64_set_element_policy_unresolved"
                  : IsDecimal(value.type_id)
                      ? "decimal_set_element_invalid"
                  : IsDecimalFloat(value.type_id)
                      ? "decimal_float_set_element_invalid"
                  : IsIpAddress(value.type_id)
                      ? "ip_address_set_element_invalid"
                  : IsNetworkPrefix(value.type_id)
                      ? "network_prefix_set_element_invalid"
                  : IsMacAddress(value.type_id)
                      ? "mac_address_set_element_invalid"
                      : "set_element_value_invalid";
      return SetFailure(failure_detail,
                        CanonicalOperationValueDiagnosticCode(
                            value, "SB_DATATYPE_SET_OPERATION_REJECTED"));
    }
    if (value.is_null && !descriptor.allow_null_elements) {
      return SetFailure("set_null_element_forbidden",
                        "DATATYPE.NULL_NOT_ADMITTED");
    }
    if (value.type_id != descriptor.element_type_id) { return SetFailure("set_element_type_mismatch"); }
    const bool value_descriptor_present =
        ExecutionDescriptorPresent(value.descriptor);
    if (element_descriptor_present != value_descriptor_present ||
        (element_descriptor_present &&
         !ExecutionDescriptorEquals(value.descriptor,
                                    descriptor.element_descriptor))) {
      return SetFailure("set_element_descriptor_mismatch",
                        "DATATYPE.DESCRIPTOR.INVALID");
    }
    const std::string encoded =
        value.is_null ? "N" : "V" + HexEncode(value.encoded_value);
    if (!descriptor.allow_duplicates) {
      if (!unique_items.insert(encoded).second) { continue; }
    }
    encoded_items.push_back(encoded);
  }
  if (IsDecimal(descriptor.element_type_id)) {
    return SetFailure("decimal_set_semantics_policy_unresolved");
  }
  if (IsDecimalFloat(descriptor.element_type_id)) {
    return SetFailure("decimal_float_set_semantics_policy_unresolved");
  }
  if (IsUuid(descriptor.element_type_id)) {
    return SetFailure("uuid_set_semantics_policy_unresolved");
  }
  if (IsIpAddress(descriptor.element_type_id)) {
    return SetFailure("ip_address_set_semantics_policy_unresolved");
  }
  if (IsNetworkPrefix(descriptor.element_type_id)) {
    return SetFailure("network_prefix_set_semantics_policy_unresolved");
  }
  if (IsMacAddress(descriptor.element_type_id)) {
    return SetFailure("mac_address_set_semantics_policy_unresolved");
  }
  if (!descriptor.ordered) { std::sort(encoded_items.begin(), encoded_items.end()); }
  std::ostringstream out;
  out << "SBSET2;element=" << CanonicalTypeName(descriptor.element_type_id)
      << ";descriptor="
      << (element_descriptor_present
              ? ExecutionDescriptorFingerprint(descriptor.element_descriptor)
              : "none")
      << ";ordered=" << (descriptor.ordered ? "1" : "0")
      << ";nulls=" << (descriptor.allow_null_elements ? "1" : "0")
      << ";duplicates=" << (descriptor.allow_duplicates ? "1" : "0")
      << ";items=";
  for (std::size_t i = 0; i < encoded_items.size(); ++i) {
    if (i != 0) { out << ","; }
    out << encoded_items[i];
  }
  DatatypeSetOperationResult result;
  result.status = OkStatus();
  result.encoded_set = out.str();
  result.value = {CanonicalTypeId::set_value, result.encoded_set, false};
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  return result;
}

DatatypeSetOperationResult ApplySetOperation(const DatatypeSetOperationRequest& request) {
  if (request.descriptor.element_type_id == CanonicalTypeId::unknown ||
      request.descriptor.element_type_id == CanonicalTypeId::null_type) {
    return SetFailure("set_element_descriptor_required",
                      "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsOpaqueRenderOnly(request.descriptor.element_type_id)) {
    return SetFailure("opaque_set_operation_rejected");
  }
  const bool decimal_set = IsDecimal(request.descriptor.element_type_id);
  const bool decimal_float_set =
      IsDecimalFloat(request.descriptor.element_type_id);
  const bool uuid_set = IsUuid(request.descriptor.element_type_id);
  const bool ip_address_set = IsIpAddress(request.descriptor.element_type_id);
  const bool network_prefix_set =
      IsNetworkPrefix(request.descriptor.element_type_id);
  const bool mac_address_set =
      IsMacAddress(request.descriptor.element_type_id);
  const bool character_set =
      IsCharacter(request.descriptor.element_type_id);
  if (character_set) {
    if (!ExecutionDescriptorPresent(
            request.descriptor.element_descriptor) ||
        !ExecutionDescriptorValidForType(
            request.descriptor.element_descriptor,
            request.descriptor.element_type_id)) {
      return SetFailure("set_element_descriptor_invalid",
                        "DATATYPE.DESCRIPTOR.INVALID");
    }
    return SetFailure("character_set_collation_policy_unresolved");
  }
  const bool policy_restricted_set =
      IsPolicyRestrictedReal(request.descriptor.element_type_id) ||
      decimal_set || decimal_float_set || uuid_set || ip_address_set ||
      network_prefix_set || mac_address_set;
  if (policy_restricted_set &&
      !(decimal_set
            ? DecimalDescriptorValidForPresent(
                  request.descriptor.element_descriptor)
            : decimal_float_set
                ? DecimalFloatDescriptorValidForPresent(
                      request.descriptor.element_descriptor)
            : uuid_set
                ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                      request.descriptor.element_descriptor,
                      request.descriptor.element_type_id)
            : ip_address_set
                ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                      request.descriptor.element_descriptor,
                      request.descriptor.element_type_id)
            : network_prefix_set
                ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                      request.descriptor.element_descriptor,
                      request.descriptor.element_type_id)
            : mac_address_set
                ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                      request.descriptor.element_descriptor,
                      request.descriptor.element_type_id)
            : ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                  request.descriptor.element_descriptor,
                  request.descriptor.element_type_id))) {
    return SetFailure("set_element_descriptor_invalid",
                      "DATATYPE.DESCRIPTOR.INVALID");
  }
  EncodedSetFrame left;
  if (!ParseEncodedSet(request.left_encoded_set, &left)) { return SetFailure("left_set_encoding_invalid"); }
  if (!EncodedSetFrameMatchesDescriptor(left, request.descriptor)) {
    return SetFailure("left_set_descriptor_mismatch",
                      "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (policy_restricted_set &&
      (request.operation == DatatypeSetOperationKind::equals ||
       request.operation == DatatypeSetOperationKind::subset ||
       request.operation == DatatypeSetOperationKind::superset)) {
    EncodedSetFrame right;
    if (!ParseEncodedSet(request.right_encoded_set, &right)) {
      return SetFailure("right_set_encoding_invalid");
    }
    if (!EncodedSetFrameMatchesDescriptor(right, request.descriptor)) {
      return SetFailure("right_set_descriptor_mismatch",
                        "DATATYPE.DESCRIPTOR.INVALID");
    }
  }
  if (policy_restricted_set &&
      request.operation == DatatypeSetOperationKind::membership) {
    if (request.right_value.type_id !=
        request.descriptor.element_type_id) {
      return SetFailure("set_membership_type_mismatch");
    }
    if (!(decimal_set
              ? DecimalDescriptorValidForPresent(
                    request.right_value.descriptor)
              : decimal_float_set
              ? DecimalFloatDescriptorValidForPresent(
                    request.right_value.descriptor)
              : IsReal128(request.descriptor.element_type_id)
              ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                    request.right_value.descriptor,
                    request.descriptor.element_type_id)
              : uuid_set
              ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                    request.right_value.descriptor,
                    request.descriptor.element_type_id)
              : ip_address_set
              ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                    request.right_value.descriptor,
                    request.descriptor.element_type_id)
              : network_prefix_set
              ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                    request.right_value.descriptor,
                    request.descriptor.element_type_id)
              : mac_address_set
              ? ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                    request.right_value.descriptor,
                    request.descriptor.element_type_id)
              : ExecutionDescriptorValidForType(
                    request.right_value.descriptor,
                    request.descriptor.element_type_id)) ||
        !ExecutionDescriptorEquals(request.descriptor.element_descriptor,
                                   request.right_value.descriptor)) {
      return SetFailure("set_membership_descriptor_mismatch",
                        "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.right_value.is_null &&
        (!request.right_value.encoded_value.empty() ||
         !request.right_value.descriptor.nullable_allowed)) {
      return SetFailure("set_membership_null_state_invalid",
                        request.right_value.encoded_value.empty()
                            ? "DATATYPE.NULL_NOT_ADMITTED"
                            : "DATATYPE.NULL_STATE.INVALID");
    }
    if (request.right_value.is_null &&
        !request.descriptor.allow_null_elements) {
      return SetFailure("set_membership_null_forbidden",
                        "DATATYPE.NULL_NOT_ADMITTED");
    }
    if (!request.right_value.is_null &&
        (decimal_set
             ? !DecimalCarrierStructurallyCanonical(
                   request.right_value.encoded_value)
             : decimal_float_set
             ? !DecimalFloatCarrierStructurallyCanonical(
                   request.right_value.encoded_value)
             : (uuid_set || ip_address_set)
             ? request.right_value.encoded_value.size() != 16u
             : network_prefix_set
             ? request.right_value.encoded_value.size() != 18u
             : mac_address_set
             ? request.right_value.encoded_value.size() != 8u
             : request.right_value.encoded_value.size() !=
                   PolicyRestrictedRealCarrierByteWidth(
                       request.descriptor.element_type_id))) {
      return SetFailure(
          "set_membership_value_invalid",
          (decimal_set || decimal_float_set)
              ? "NUMERIC.ENCODING.NONCANONICAL"
                      : "SB_DATATYPE_SET_OPERATION_REJECTED");
    }
  }
  if (policy_restricted_set) {
    return SetFailure(
        decimal_set
            ? "decimal_set_semantics_policy_unresolved"
            : decimal_float_set
            ? "decimal_float_set_semantics_policy_unresolved"
            : uuid_set
            ? "uuid_set_semantics_policy_unresolved"
            : ip_address_set
            ? "ip_address_set_semantics_policy_unresolved"
            : network_prefix_set
            ? "network_prefix_set_semantics_policy_unresolved"
            : mac_address_set
            ? "mac_address_set_semantics_policy_unresolved"
            : IsReal128(request.descriptor.element_type_id)
            ? "real128_set_semantics_policy_unresolved"
            : UnresolvedRealDetail(
                  request.descriptor.element_type_id,
                  "bfloat16_set_semantics_policy_unresolved",
                  "real16_set_semantics_policy_unresolved",
                  "real32_set_semantics_policy_unresolved",
                  "real64_set_semantics_policy_unresolved"));
  }
  DatatypeSetOperationResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.operation == DatatypeSetOperationKind::cardinality) {
    result.value = UInt64Value(left.items.size());
    return result;
  }
  if (request.operation == DatatypeSetOperationKind::membership) {
    if (!CanonicalOperationValueValid(request.right_value)) {
      return SetFailure(
          "set_membership_value_invalid",
          CanonicalOperationValueDiagnosticCode(
              request.right_value, "SB_DATATYPE_SET_OPERATION_REJECTED"));
    }
    if (request.right_value.type_id != request.descriptor.element_type_id) {
      return SetFailure("set_membership_type_mismatch");
    }
    if (request.right_value.is_null &&
        !request.descriptor.allow_null_elements) {
      return SetFailure("set_membership_null_forbidden",
                        "DATATYPE.NULL_NOT_ADMITTED");
    }
    const bool element_descriptor_present =
        ExecutionDescriptorPresent(request.descriptor.element_descriptor);
    const bool value_descriptor_present =
        ExecutionDescriptorPresent(request.right_value.descriptor);
    if (element_descriptor_present != value_descriptor_present ||
        (element_descriptor_present &&
         !ExecutionDescriptorEquals(request.descriptor.element_descriptor,
                                    request.right_value.descriptor))) {
      return SetFailure("set_membership_descriptor_mismatch",
                        "DATATYPE.DESCRIPTOR.INVALID");
    }
    const std::string item = request.right_value.is_null
        ? "N"
        : "V" + HexEncode(request.right_value.encoded_value);
    result.value = BoolValue(std::find(left.items.begin(), left.items.end(),
                                       item) != left.items.end());
    return result;
  }
  EncodedSetFrame right;
  if (!ParseEncodedSet(request.right_encoded_set, &right)) { return SetFailure("right_set_encoding_invalid"); }
  if (!EncodedSetFrameMatchesDescriptor(right, request.descriptor)) {
    return SetFailure("right_set_descriptor_mismatch",
                      "DATATYPE.DESCRIPTOR.INVALID");
  }
  std::sort(left.items.begin(), left.items.end());
  std::sort(right.items.begin(), right.items.end());
  if (request.operation == DatatypeSetOperationKind::equals) {
    result.value = BoolValue(left.items == right.items);
    return result;
  }
  if (request.operation == DatatypeSetOperationKind::subset) {
    result.value = BoolValue(std::includes(right.items.begin(), right.items.end(),
                                            left.items.begin(), left.items.end()));
    return result;
  }
  if (request.operation == DatatypeSetOperationKind::superset) {
    result.value = BoolValue(std::includes(left.items.begin(), left.items.end(),
                                            right.items.begin(), right.items.end()));
    return result;
  }
  return SetFailure("unknown_set_operation");
}

DatatypeNumericOperationResult ApplyNumericOperation(const DatatypeNumericOperationRequest& request) {
  namespace numeric = scratchbird::libraries::sbl_numeric;

  const auto invalid_request = [&](std::string detail,
                                   const char* diagnostic_code = nullptr) {
    auto failed = NumericFailure(std::move(detail),
        diagnostic_code != nullptr ? diagnostic_code :
        (request.type_id == CanonicalTypeId::real128 ? "NUMERIC.REAL128.INVALID" :
         "SB_DATATYPE_NUMERIC_OPERATION_REJECTED"));
    failed.numeric_facts.invalid = true;
    return failed;
  };
  DatatypeNumericOperationResult result;
  result.status = OkStatus();
  result.value = {request.type_id, {}, false};
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");

  const bool uuid_incident = IsUuid(request.type_id) ||
      IsUuid(request.left.type_id) ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       IsUuid(request.right.type_id));
  if (uuid_incident) {
    const auto uuid_descriptor_invalid = [](const DatatypeOperationValue& value) {
      return IsUuid(value.type_id) &&
          !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
              value.descriptor, CanonicalTypeId::uuid);
    };
    if (uuid_descriptor_invalid(request.left)) {
      return invalid_request("uuid_left_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        uuid_descriptor_invalid(request.right)) {
      return invalid_request("uuid_right_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if ((IsUuid(request.left.type_id) &&
         !CanonicalOperationValueValid(request.left)) ||
        (request.operation != DatatypeNumericOperationKind::canonicalize &&
         IsUuid(request.right.type_id) &&
         !CanonicalOperationValueValid(request.right))) {
      const auto& invalid = IsUuid(request.left.type_id) &&
                                    !CanonicalOperationValueValid(request.left)
          ? request.left
          : request.right;
      return invalid_request(
          "uuid_numeric_value_invalid",
          CanonicalOperationValueDiagnosticCode(
              invalid, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED"));
    }
    return invalid_request("uuid_numeric_policy_unresolved");
  }

  const bool ip_address_incident = IsIpAddress(request.type_id) ||
      IsIpAddress(request.left.type_id) ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       IsIpAddress(request.right.type_id));
  if (ip_address_incident) {
    const auto ip_address_descriptor_invalid = [](
        const DatatypeOperationValue& value) {
      return IsIpAddress(value.type_id) &&
          !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
              value.descriptor, CanonicalTypeId::ip_address);
    };
    if (ip_address_descriptor_invalid(request.left)) {
      return invalid_request("ip_address_left_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        ip_address_descriptor_invalid(request.right)) {
      return invalid_request("ip_address_right_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if ((IsIpAddress(request.left.type_id) &&
         !CanonicalOperationValueValid(request.left)) ||
        (request.operation != DatatypeNumericOperationKind::canonicalize &&
         IsIpAddress(request.right.type_id) &&
         !CanonicalOperationValueValid(request.right))) {
      const auto& invalid =
          IsIpAddress(request.left.type_id) &&
                  !CanonicalOperationValueValid(request.left)
              ? request.left
              : request.right;
      return invalid_request(
          "ip_address_numeric_value_invalid",
          CanonicalOperationValueDiagnosticCode(
              invalid, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED"));
    }
    return invalid_request("ip_address_numeric_policy_unresolved");
  }

  const bool network_prefix_incident = IsNetworkPrefix(request.type_id) ||
      IsNetworkPrefix(request.left.type_id) ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       IsNetworkPrefix(request.right.type_id));
  if (network_prefix_incident) {
    const auto network_prefix_descriptor_invalid = [](
        const DatatypeOperationValue& value) {
      return IsNetworkPrefix(value.type_id) &&
          !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
              value.descriptor, CanonicalTypeId::network_prefix);
    };
    if (network_prefix_descriptor_invalid(request.left)) {
      return invalid_request("network_prefix_left_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        network_prefix_descriptor_invalid(request.right)) {
      return invalid_request("network_prefix_right_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if ((IsNetworkPrefix(request.left.type_id) &&
         !CanonicalOperationValueValid(request.left)) ||
        (request.operation != DatatypeNumericOperationKind::canonicalize &&
         IsNetworkPrefix(request.right.type_id) &&
         !CanonicalOperationValueValid(request.right))) {
      const auto& invalid =
          IsNetworkPrefix(request.left.type_id) &&
                  !CanonicalOperationValueValid(request.left)
              ? request.left
              : request.right;
      return invalid_request(
          "network_prefix_numeric_value_invalid",
          CanonicalOperationValueDiagnosticCode(
              invalid, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED"));
    }
    return invalid_request("network_prefix_numeric_policy_unresolved");
  }

  const bool mac_address_incident = IsMacAddress(request.type_id) ||
      IsMacAddress(request.left.type_id) ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       IsMacAddress(request.right.type_id));
  if (mac_address_incident) {
    const auto mac_address_descriptor_invalid = [](
        const DatatypeOperationValue& value) {
      return IsMacAddress(value.type_id) &&
          !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
              value.descriptor, CanonicalTypeId::mac_address);
    };
    if (mac_address_descriptor_invalid(request.left)) {
      return invalid_request("mac_address_left_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        mac_address_descriptor_invalid(request.right)) {
      return invalid_request("mac_address_right_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if ((IsMacAddress(request.left.type_id) &&
         !CanonicalOperationValueValid(request.left)) ||
        (request.operation != DatatypeNumericOperationKind::canonicalize &&
         IsMacAddress(request.right.type_id) &&
         !CanonicalOperationValueValid(request.right))) {
      const auto& invalid =
          IsMacAddress(request.left.type_id) &&
                  !CanonicalOperationValueValid(request.left)
              ? request.left
              : request.right;
      return invalid_request(
          "mac_address_numeric_value_invalid",
          CanonicalOperationValueDiagnosticCode(
              invalid, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED"));
    }
    return invalid_request("mac_address_numeric_policy_unresolved");
  }

  if (IsReal128(request.type_id)) {
    if (request.left.type_id != CanonicalTypeId::real128 ||
        (request.operation != DatatypeNumericOperationKind::canonicalize &&
         request.right.type_id != CanonicalTypeId::real128)) {
      return invalid_request("numeric_argument_type_mismatch");
    }
    if (!ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.left.descriptor, CanonicalTypeId::real128)) {
      return invalid_request("real128_left_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.right.descriptor, CanonicalTypeId::real128)) {
      return invalid_request("real128_right_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        !ExecutionDescriptorEquals(request.left.descriptor,
                                   request.right.descriptor)) {
      return invalid_request("real128_operand_descriptor_mismatch",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
  }
  if (IsDecimal(request.type_id)) {
    if (!IsDecimal(request.left.type_id) ||
        (request.operation != DatatypeNumericOperationKind::canonicalize &&
         !IsDecimal(request.right.type_id))) {
      return invalid_request("numeric_argument_type_mismatch");
    }
    const auto valid_decimal_operand = [](const DatatypeOperationValue& value) {
      return value.is_null
          ? ExecutionDescriptorValidForType(value.descriptor,
                                            CanonicalTypeId::decimal)
          : DecimalDescriptorValidForPresent(value.descriptor);
    };
    if (!valid_decimal_operand(request.left)) {
      return invalid_request("decimal_left_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        !valid_decimal_operand(request.right)) {
      return invalid_request("decimal_right_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        !ExecutionDescriptorEquals(request.left.descriptor,
                                   request.right.descriptor)) {
      return invalid_request("decimal_operand_descriptor_mismatch",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (ExecutionDescriptorPresent(request.result_descriptor) &&
        !(request.operation == DatatypeNumericOperationKind::compare
              ? ExecutionDescriptorValidForType(
                    request.result_descriptor, CanonicalTypeId::boolean)
              : DecimalDescriptorValidForPresent(
                    request.result_descriptor))) {
      return invalid_request("decimal_result_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
  }
  if (IsDecimalFloat(request.type_id)) {
    if (!IsDecimalFloat(request.left.type_id) ||
        (request.operation != DatatypeNumericOperationKind::canonicalize &&
         !IsDecimalFloat(request.right.type_id))) {
      return invalid_request("numeric_argument_type_mismatch");
    }
    const auto valid_decimal_float_operand = [](
        const DatatypeOperationValue& value) {
      return value.is_null
          ? ExecutionDescriptorValidForType(
                value.descriptor, CanonicalTypeId::decimal_float)
          : DecimalFloatDescriptorValidForPresent(value.descriptor);
    };
    if (!valid_decimal_float_operand(request.left)) {
      return invalid_request("decimal_float_left_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        !valid_decimal_float_operand(request.right)) {
      return invalid_request("decimal_float_right_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        !ExecutionDescriptorEquals(request.left.descriptor,
                                   request.right.descriptor)) {
      return invalid_request("decimal_float_operand_descriptor_mismatch",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
    const bool decimal_float_null_result_possible = request.left.is_null ||
        (request.operation != DatatypeNumericOperationKind::canonicalize &&
         request.right.is_null);
    if (ExecutionDescriptorPresent(request.result_descriptor) &&
        !(request.operation == DatatypeNumericOperationKind::compare
              ? ExecutionDescriptorValidForType(
                    request.result_descriptor, CanonicalTypeId::boolean)
              : decimal_float_null_result_possible
                  ? ExecutionDescriptorValidForType(
                        request.result_descriptor,
                        CanonicalTypeId::decimal_float)
                  : DecimalFloatDescriptorValidForPresent(
                        request.result_descriptor))) {
      return invalid_request("decimal_float_result_descriptor_invalid",
                             "DATATYPE.DESCRIPTOR.INVALID");
    }
  }

  const auto unresolved_real_present_descriptor_invalid = [](
      const DatatypeOperationValue& value) {
    return IsUnresolvedRealSemantics(value.type_id) && !value.is_null &&
        !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            value.descriptor, value.type_id);
  };
  const bool left_unresolved_real_descriptor_invalid =
      unresolved_real_present_descriptor_invalid(request.left);
  const bool right_unresolved_real_descriptor_invalid =
      request.operation != DatatypeNumericOperationKind::canonicalize &&
      unresolved_real_present_descriptor_invalid(request.right);
  if (left_unresolved_real_descriptor_invalid ||
      right_unresolved_real_descriptor_invalid) {
    const CanonicalTypeId rejected_type = left_unresolved_real_descriptor_invalid
        ? request.left.type_id
        : request.right.type_id;
    return invalid_request(UnresolvedRealDetail(
                               rejected_type,
                               "bfloat16_operand_descriptor_invalid",
                               "real16_operand_descriptor_invalid",
                               "real32_operand_descriptor_invalid",
                               "real64_operand_descriptor_invalid"),
                           "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (request.operation != DatatypeNumericOperationKind::canonicalize &&
      IsUnresolvedRealSemantics(request.left.type_id) &&
      request.right.type_id == request.left.type_id &&
      !ExecutionDescriptorEquals(request.left.descriptor,
                                 request.right.descriptor)) {
    return invalid_request(UnresolvedRealDetail(
        request.left.type_id,
        "bfloat16_operand_descriptor_mismatch",
        "real16_operand_descriptor_mismatch",
        "real32_operand_descriptor_mismatch",
        "real64_operand_descriptor_mismatch"),
                           "DATATYPE.DESCRIPTOR.INVALID");
  }

  if (IsCanonical128Integer(request.type_id) &&
      (request.left.type_id != request.type_id ||
       (request.operation != DatatypeNumericOperationKind::canonicalize &&
        request.right.type_id != request.type_id))) {
    return invalid_request("numeric_argument_type_mismatch");
  }

  if (!CanonicalOperationValueValid(request.left) ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       !CanonicalOperationValueValid(request.right))) {
    const DatatypeOperationValue& invalid_value =
        !CanonicalOperationValueValid(request.left) ? request.left : request.right;
    const bool descriptor_or_null_state_failure =
        invalid_value.type_id == CanonicalTypeId::unknown ||
        invalid_value.type_id == CanonicalTypeId::null_type ||
        invalid_value.is_null || IsDecimal(invalid_value.type_id) ||
        IsDecimalFloat(invalid_value.type_id);
    const bool malformed_128_bit_payload =
        (IsCanonical128Integer(invalid_value.type_id) ||
         IsReal128(invalid_value.type_id)) &&
        !invalid_value.is_null &&
        ExecutionDescriptorValidForType(invalid_value.descriptor,
                                        invalid_value.type_id) &&
        invalid_value.encoded_value.size() != 16;
    const bool invalid_128_bit_descriptor =
        IsCanonical128Integer(invalid_value.type_id) &&
        !invalid_value.is_null &&
        !ExecutionDescriptorValidForType(invalid_value.descriptor,
                                         invalid_value.type_id);
    const bool unresolved_real_present =
        IsUnresolvedRealSemantics(invalid_value.type_id) &&
        !invalid_value.is_null;
    return invalid_request(
        unresolved_real_present
            ? UnresolvedRealDetail(
                  invalid_value.type_id,
                  "bfloat16_numeric_policy_unresolved",
                  "real16_numeric_policy_unresolved",
                  "real32_numeric_policy_unresolved",
                  "real64_numeric_policy_unresolved")
            : IsDecimal(invalid_value.type_id)
                ? "decimal_numeric_argument_invalid"
            : IsDecimalFloat(invalid_value.type_id)
                ? "decimal_float_numeric_argument_invalid"
                : "numeric_argument_value_invalid",
        malformed_128_bit_payload
            ? "NUMERIC.ENCODING.NONCANONICAL"
            : invalid_128_bit_descriptor
            ? "DATATYPE.DESCRIPTOR.INVALID"
            : unresolved_real_present
            ? CanonicalOperationValueDiagnosticCode(
                  invalid_value, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED")
            : descriptor_or_null_state_failure
            ? CanonicalOperationValueDiagnosticCode(
                  invalid_value, "SB_DATATYPE_NUMERIC_OPERATION_REJECTED")
            : nullptr);
  }
  if (IsUnresolvedRealSemantics(request.type_id) ||
      IsUnresolvedRealSemantics(request.left.type_id) ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       IsUnresolvedRealSemantics(request.right.type_id))) {
    const auto rejected_type = SelectUnresolvedRealType(
        request.type_id, request.left.type_id,
        request.operation != DatatypeNumericOperationKind::canonicalize
            ? request.right.type_id
            : CanonicalTypeId::unknown);
    return invalid_request(UnresolvedRealDetail(
        rejected_type,
        "bfloat16_numeric_policy_unresolved",
        "real16_numeric_policy_unresolved",
        "real32_numeric_policy_unresolved",
        "real64_numeric_policy_unresolved"));
  }
  if (IsCanonical128Integer(request.type_id) &&
      request.operation == DatatypeNumericOperationKind::compare &&
      (request.left.is_null || request.right.is_null)) {
    return invalid_request(
        request.type_id == CanonicalTypeId::int128
            ? "int128_null_comparison_policy_unresolved"
            : "uint128_null_comparison_policy_unresolved");
  }
  if (IsCanonical128Integer(request.type_id) &&
      request.operation != DatatypeNumericOperationKind::canonicalize &&
      !ExecutionDescriptorEquals(request.left.descriptor,
                                 request.right.descriptor)) {
    return invalid_request(
        request.type_id == CanonicalTypeId::int128
            ? "int128_operand_descriptor_mismatch"
            : "uint128_operand_descriptor_mismatch",
                           "DATATYPE.DESCRIPTOR.INVALID");
  }
  const CanonicalTypeId declared_result_type =
      request.operation == DatatypeNumericOperationKind::compare
          ? CanonicalTypeId::boolean
          : request.type_id;
  const bool null_result_possible = request.left.is_null ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       request.right.is_null);
  if (null_result_possible &&
      !ExecutionDescriptorValidForType(request.result_descriptor,
                                       declared_result_type)) {
    return invalid_request("numeric_result_descriptor_invalid",
                           "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (null_result_possible && !request.result_descriptor.nullable_allowed) {
    return invalid_request("numeric_result_descriptor_not_nullable",
                           "DATATYPE.NULL_NOT_ADMITTED");
  }
  if (IsDecimal(request.type_id) || IsDecimal(request.left.type_id) ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       IsDecimal(request.right.type_id))) {
    return invalid_request("decimal_numeric_policy_unresolved");
  }
  if (IsDecimalFloat(request.type_id) ||
      IsDecimalFloat(request.left.type_id) ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       IsDecimalFloat(request.right.type_id))) {
    return invalid_request("decimal_float_numeric_policy_unresolved");
  }
  if (IsCanonical128Integer(request.type_id) &&
      request.operation != DatatypeNumericOperationKind::compare &&
      ExecutionDescriptorPresent(request.result_descriptor) &&
      !ExecutionDescriptorValidForType(request.result_descriptor,
                                       request.type_id)) {
    return invalid_request(
        request.type_id == CanonicalTypeId::int128
            ? "int128_result_descriptor_invalid"
            : "uint128_result_descriptor_invalid",
                           "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsCanonical128Integer(request.type_id) &&
      request.operation != DatatypeNumericOperationKind::compare &&
      ExecutionDescriptorPresent(request.result_descriptor) &&
      !ExecutionDescriptorEquals(request.left.descriptor,
                                 request.result_descriptor)) {
    return invalid_request(
        request.type_id == CanonicalTypeId::int128
            ? "int128_result_descriptor_mismatch"
            : "uint128_result_descriptor_mismatch",
                           "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsReal128(request.type_id) &&
      request.operation != DatatypeNumericOperationKind::compare &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.result_descriptor, CanonicalTypeId::real128)) {
    return invalid_request("real128_result_descriptor_invalid",
                           "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsReal128(request.type_id) &&
      request.operation != DatatypeNumericOperationKind::compare &&
      !ExecutionDescriptorEquals(request.left.descriptor,
                                 request.result_descriptor)) {
    return invalid_request("real128_result_descriptor_mismatch",
                           "DATATYPE.DESCRIPTOR.INVALID");
  }
  if (IsReal128(request.type_id) &&
      request.operation == DatatypeNumericOperationKind::compare &&
      ExecutionDescriptorPresent(request.result_descriptor) &&
      !ExecutionDescriptorValidForType(request.result_descriptor,
                                       CanonicalTypeId::boolean)) {
    return invalid_request("numeric_compare_result_descriptor_invalid",
                           "DATATYPE.DESCRIPTOR.INVALID");
  }

  if (IsReal128(request.type_id)) {
    numeric::NumericOperation backend_operation;
    switch (request.operation) {
      case DatatypeNumericOperationKind::canonicalize:
        backend_operation = numeric::NumericOperation::canonicalize;
        break;
      case DatatypeNumericOperationKind::add:
        backend_operation = numeric::NumericOperation::add;
        break;
      case DatatypeNumericOperationKind::subtract:
        backend_operation = numeric::NumericOperation::subtract;
        break;
      case DatatypeNumericOperationKind::multiply:
        backend_operation = numeric::NumericOperation::multiply;
        break;
      case DatatypeNumericOperationKind::divide:
        backend_operation = numeric::NumericOperation::divide;
        break;
      case DatatypeNumericOperationKind::compare:
        backend_operation = numeric::NumericOperation::compare;
        break;
      default:
        return invalid_request("invalid_numeric_operation");
    }

    numeric::NumericContext backend_context;
    backend_context.precision = request.context.precision;
    backend_context.scale = request.context.scale;
    backend_context.allow_special_values = request.context.allow_special_values;
    switch (request.context.rounding) {
      case DatatypeRoundingMode::half_even:
        backend_context.rounding = numeric::RoundingMode::half_even;
        break;
      case DatatypeRoundingMode::half_up:
        backend_context.rounding = numeric::RoundingMode::half_up;
        break;
      case DatatypeRoundingMode::truncate:
        backend_context.rounding = numeric::RoundingMode::truncate;
        break;
      default:
        return invalid_request("invalid_rounding_mode");
    }

    numeric::Real128BinaryRequest binary_request;
    binary_request.operation = backend_operation;
    binary_request.context = backend_context;
    numeric::Real128Bytes left_bytes{};
    numeric::Real128Bytes right_bytes{};
    const bool null_result = request.left.is_null ||
        (request.operation != DatatypeNumericOperationKind::canonicalize &&
         request.right.is_null);
    if (null_result) {
      // The binary API carries values only. Use finite +1 probes to validate
      // backend availability, operation, and context before strict NULL
      // propagation without making a value conversion or publishing a probe.
      left_bytes[14] = 0xffu;
      left_bytes[15] = 0x3fu;
      right_bytes = left_bytes;
    } else {
      if (!DecodeReal128Carrier(request.left.encoded_value, &left_bytes)) {
        return invalid_request("real128_left_payload_invalid",
                               "NUMERIC.ENCODING.NONCANONICAL");
      }
      if (request.operation != DatatypeNumericOperationKind::canonicalize &&
          !DecodeReal128Carrier(request.right.encoded_value, &right_bytes)) {
        return invalid_request("real128_right_payload_invalid",
                               "NUMERIC.ENCODING.NONCANONICAL");
      }
    }
    binary_request.left = left_bytes;
    if (request.operation != DatatypeNumericOperationKind::canonicalize) {
      binary_request.right = right_bytes;
    }

    const auto backend_result =
        numeric::ApplyReal128BinaryOperation(binary_request);
    result.numeric_facts = NumericFacts(backend_result.numeric);
    if (backend_result.numeric.status != numeric::NumericStatusCode::ok) {
      auto failed = NumericFailure(
          backend_result.numeric.diagnostic_code.empty()
              ? "real128_binary_backend_failed"
              : backend_result.numeric.diagnostic_code,
          backend_result.numeric.diagnostic_code.empty()
              ? "SB_DATATYPE_NUMERIC_OPERATION_REJECTED"
              : backend_result.numeric.diagnostic_code);
      failed.numeric_facts = result.numeric_facts;
      return failed;
    }
    if (null_result) {
      result.value = {declared_result_type, {}, true};
      result.value.descriptor = request.result_descriptor;
      return result;
    }
    result.comparison = backend_result.numeric.comparison;
    if (request.operation == DatatypeNumericOperationKind::compare) {
      result.value = BoolValue(result.comparison == 0);
      if (ExecutionDescriptorPresent(request.result_descriptor)) {
        result.value.descriptor = request.result_descriptor;
      }
      return result;
    }
    if (!backend_result.bytes) {
      return invalid_request("real128_result_payload_missing",
                             "NUMERIC.ENCODING.NONCANONICAL");
    }
    result.value = {CanonicalTypeId::real128,
                    EncodeReal128Carrier(*backend_result.bytes), false};
    result.value.descriptor = request.result_descriptor;
    return result;
  }

  numeric::NumericRequest backend_request;
  backend_request.left.is_null = request.left.is_null;
  backend_request.right.is_null = request.right.is_null;
  if (IsCanonical128Integer(request.type_id)) {
    if (!request.left.is_null &&
        !(request.type_id == CanonicalTypeId::int128
              ? DecodeCanonicalInt128Value(request.left.encoded_value,
                                           &backend_request.left.encoded)
              : DecodeCanonicalUint128Value(request.left.encoded_value,
                                            &backend_request.left.encoded))) {
      return invalid_request(
          request.type_id == CanonicalTypeId::int128
              ? "int128_left_payload_invalid"
              : "uint128_left_payload_invalid",
                             "NUMERIC.ENCODING.NONCANONICAL");
    }
    if (request.operation != DatatypeNumericOperationKind::canonicalize &&
        !request.right.is_null &&
        !(request.type_id == CanonicalTypeId::int128
              ? DecodeCanonicalInt128Value(request.right.encoded_value,
                                           &backend_request.right.encoded)
              : DecodeCanonicalUint128Value(request.right.encoded_value,
                                            &backend_request.right.encoded))) {
      return invalid_request(
          request.type_id == CanonicalTypeId::int128
              ? "int128_right_payload_invalid"
              : "uint128_right_payload_invalid",
                             "NUMERIC.ENCODING.NONCANONICAL");
    }
  } else {
    backend_request.left.encoded = request.left.encoded_value;
    backend_request.right.encoded = request.right.encoded_value;
  }
  backend_request.context.precision = request.context.precision;
  backend_request.context.scale = request.context.scale;
  backend_request.context.allow_special_values = request.context.allow_special_values;
  switch (request.context.rounding) {
    case DatatypeRoundingMode::half_even: backend_request.context.rounding = numeric::RoundingMode::half_even; break;
    case DatatypeRoundingMode::half_up: backend_request.context.rounding = numeric::RoundingMode::half_up; break;
    case DatatypeRoundingMode::truncate: backend_request.context.rounding = numeric::RoundingMode::truncate; break;
    default: return invalid_request("invalid_rounding_mode");
  }
  switch (request.operation) {
    case DatatypeNumericOperationKind::canonicalize: backend_request.operation = numeric::NumericOperation::canonicalize; break;
    case DatatypeNumericOperationKind::add: backend_request.operation = numeric::NumericOperation::add; break;
    case DatatypeNumericOperationKind::subtract: backend_request.operation = numeric::NumericOperation::subtract; break;
    case DatatypeNumericOperationKind::multiply: backend_request.operation = numeric::NumericOperation::multiply; break;
    case DatatypeNumericOperationKind::divide: backend_request.operation = numeric::NumericOperation::divide; break;
    case DatatypeNumericOperationKind::compare: backend_request.operation = numeric::NumericOperation::compare; break;
    default: return invalid_request("invalid_numeric_operation");
  }
  switch (request.type_id) {
    case CanonicalTypeId::int128:
      backend_request.type = numeric::NumericType::int128;
      break;
    case CanonicalTypeId::uint128:
      backend_request.type = numeric::NumericType::uint128;
      break;
    case CanonicalTypeId::decimal:
      backend_request.type = numeric::NumericType::decimal;
      break;
    case CanonicalTypeId::decimal_float:
      backend_request.type = numeric::NumericType::decimal_float;
      break;
    case CanonicalTypeId::real128:
      backend_request.type = numeric::NumericType::real128;
      break;
    default:
      return invalid_request("unsupported_numeric_type");
  }
  if (request.left.type_id != request.type_id ||
      (request.operation != DatatypeNumericOperationKind::canonicalize &&
       request.right.type_id != request.type_id)) {
    return invalid_request("numeric_argument_type_mismatch");
  }
  backend_request.left.type = backend_request.type;
  backend_request.right.type = backend_request.type;

  const numeric::NumericResult backend_result = numeric::ApplyNumericOperation(backend_request);
  result.numeric_facts = NumericFacts(backend_result);
  if (backend_result.status == numeric::NumericStatusCode::null_result) {
    result.value = {declared_result_type, {}, true};
    result.value.descriptor = request.result_descriptor;
    return result;
  }
  if (backend_result.status != numeric::NumericStatusCode::ok) {
    const bool canonical_128_overflow =
        IsCanonical128Integer(request.type_id) &&
        backend_result.status == numeric::NumericStatusCode::overflow;
    auto failed = NumericFailure(
        backend_result.diagnostic_code.empty() ? "numeric_backend_failed" : backend_result.diagnostic_code,
        canonical_128_overflow
            ? (request.type_id == CanonicalTypeId::int128
                   ? "NUMERIC.INT128.OVERFLOW"
                   : "NUMERIC.UINT128.OVERFLOW")
            : request.type_id == CanonicalTypeId::real128 &&
                    !backend_result.diagnostic_code.empty()
                ? backend_result.diagnostic_code
                : "SB_DATATYPE_NUMERIC_OPERATION_REJECTED");
    failed.numeric_facts = result.numeric_facts;
    failed.numeric_facts.overflow =
        failed.numeric_facts.overflow || canonical_128_overflow;
    failed.numeric_facts.divide_by_zero =
        failed.numeric_facts.divide_by_zero ||
        (IsCanonical128Integer(request.type_id) &&
         backend_result.status == numeric::NumericStatusCode::divide_by_zero);
    return failed;
  }
  result.comparison = backend_result.comparison;
  if (request.operation == DatatypeNumericOperationKind::compare) {
    result.value = BoolValue(result.comparison == 0);
    if (ExecutionDescriptorPresent(request.result_descriptor)) {
      if (!ExecutionDescriptorValidForType(request.result_descriptor,
                                           CanonicalTypeId::boolean)) {
        return invalid_request("numeric_compare_result_descriptor_invalid",
                               "DATATYPE.DESCRIPTOR.INVALID");
      }
      result.value.descriptor = request.result_descriptor;
    }
    return result;
  }
  if (IsCanonical128Integer(request.type_id)) {
    const bool encoded = request.type_id == CanonicalTypeId::int128
        ? EncodeCanonicalInt128Value(backend_result.value.encoded,
                                     &result.value.encoded_value)
        : EncodeCanonicalUint128Value(backend_result.value.encoded,
                                      &result.value.encoded_value);
    if (!encoded) {
      return invalid_request(
          request.type_id == CanonicalTypeId::int128
              ? "int128_result_encoding_failed"
              : "uint128_result_encoding_failed",
                             "NUMERIC.ENCODING.NONCANONICAL");
    }
  } else {
    result.value.encoded_value = backend_result.value.encoded;
  }
  if (IsCanonical128Integer(request.type_id)) {
    result.value.descriptor =
        ExecutionDescriptorPresent(request.result_descriptor)
            ? request.result_descriptor
            : request.left.descriptor;
  }
  return result;
}

DatatypeComparisonResult CompareDatatypeValues(const DatatypeComparisonRequest& request) {
  DatatypeComparisonResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.left.type_id == CanonicalTypeId::interval ||
      request.right.type_id == CanonicalTypeId::interval) {
    const char* left_code =
        GenericIntervalStructuralDiagnosticCode(request.left);
    const char* right_code =
        GenericIntervalStructuralDiagnosticCode(request.right);
    const char* interval_code = nullptr;
    for (const char* candidate : {"CTI.INTERVAL.DESCRIPTOR_INVALID",
                                  "DATATYPE.NULL_STATE.INVALID",
                                  "DATATYPE.NULL_NOT_ADMITTED"}) {
      if ((left_code != nullptr && std::strcmp(left_code, candidate) == 0) ||
          (right_code != nullptr && std::strcmp(right_code, candidate) == 0)) {
        interval_code = candidate;
        break;
      }
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        interval_code != nullptr ? interval_code
                                 : "CTI.INTERVAL.DESCRIPTOR_INVALID",
        "datatype.comparison.rejected",
        interval_code != nullptr ? "interval_generic_structural_refusal"
                                 : "interval_d710_profile_required");
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::timestamp ||
      request.right.type_id == CanonicalTypeId::timestamp) {
    const char* timestamp_code =
        GenericTimestampStructuralDiagnosticCode(request.left);
    if (timestamp_code == nullptr)
      timestamp_code = GenericTimestampStructuralDiagnosticCode(request.right);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        timestamp_code != nullptr ? timestamp_code
                                  : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.comparison.rejected",
        timestamp_code != nullptr ? "timestamp_generic_structural_refusal"
                                  : "timestamp_d710_profile_required");
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::time ||
      request.right.type_id == CanonicalTypeId::time) {
    const char* time_code = GenericTimeStructuralDiagnosticCode(request.left);
    if (time_code == nullptr)
      time_code = GenericTimeStructuralDiagnosticCode(request.right);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        time_code != nullptr ? time_code : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.comparison.rejected",
        time_code != nullptr ? "time_generic_structural_refusal"
                             : "time_d710_profile_required");
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::date ||
      request.right.type_id == CanonicalTypeId::date) {
    const char* date_code = GenericDateStructuralDiagnosticCode(request.left);
    if (date_code == nullptr)
      date_code = GenericDateStructuralDiagnosticCode(request.right);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        date_code != nullptr ? date_code : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.comparison.rejected",
        date_code != nullptr ? "date_generic_structural_refusal"
                             : "date_d710_profile_required");
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::bit_string ||
      request.right.type_id == CanonicalTypeId::bit_string) {
    const char* bit_code =
        GenericBitStringStructuralDiagnosticCode(request.left);
    if (bit_code == nullptr)
      bit_code = GenericBitStringStructuralDiagnosticCode(request.right);
    if (bit_code != nullptr) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, bit_code, "datatype.comparison.rejected",
          "bit_string_generic_structural_refusal");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BIT.DESCRIPTOR_INVALID",
        "datatype.comparison.rejected", "bit_string_v3_profile_required");
    return result;
  }
  const bool binary_incident = IsBinary(request.left.type_id) ||
      IsBinary(request.right.type_id);
  if (binary_incident) {
    if (!IsBinary(request.left.type_id) || !IsBinary(request.right.type_id)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected", "binary_operand_type_mismatch");
      return result;
    }
    if (!BinaryDescriptorExactlyCurrent(request.left.descriptor) ||
        !BinaryDescriptorExactlyCurrent(request.right.descriptor) ||
        !ExecutionDescriptorEqualsIgnoringNullability(
            request.left.descriptor, request.right.descriptor)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "CTB.BINARY.DESCRIPTOR_INVALID",
          "datatype.comparison.rejected",
          "binary_operand_descriptor_invalid_or_mismatch");
      return result;
    }
    if (!CanonicalBinaryValueValid(request.left) ||
        !CanonicalBinaryValueValid(request.right)) {
      const auto& invalid = !CanonicalBinaryValueValid(request.left)
          ? request.left : request.right;
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          CanonicalOperationValueDiagnosticCode(
              invalid, "SB_DATATYPE_COMPARISON_REJECTED"),
          "datatype.comparison.rejected", "binary_value_invalid");
      return result;
    }
    if (request.null_ordering != DatatypeNullOrdering::nulls_first &&
        request.null_ordering != DatatypeNullOrdering::nulls_last) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected", "invalid_null_ordering");
      return result;
    }
    if (request.left.is_null || request.right.is_null) {
      if (request.left.is_null && request.right.is_null) return result;
      const int null_compare =
          request.null_ordering == DatatypeNullOrdering::nulls_first ? -1 : 1;
      result.comparison = request.left.is_null ? null_compare : -null_compare;
      return result;
    }
    const auto count = std::min(request.left.encoded_value.size(),
                                request.right.encoded_value.size());
    for (std::size_t index = 0; index < count; ++index) {
      const auto left = static_cast<unsigned char>(
          request.left.encoded_value[index]);
      const auto right = static_cast<unsigned char>(
          request.right.encoded_value[index]);
      if (left != right) {
        result.comparison = left < right ? -1 : 1;
        return result;
      }
    }
    result.comparison = request.left.encoded_value.size() <
            request.right.encoded_value.size()
        ? -1
        : request.left.encoded_value.size() >
                request.right.encoded_value.size()
            ? 1
            : 0;
    return result;
  }
  const bool uuid_incident = IsUuid(request.left.type_id) ||
      IsUuid(request.right.type_id);
  if (uuid_incident) {
    const auto uuid_descriptor_invalid = [](const DatatypeOperationValue& value) {
      return IsUuid(value.type_id) &&
          !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
              value.descriptor, CanonicalTypeId::uuid);
    };
    if (uuid_descriptor_invalid(request.left) ||
        uuid_descriptor_invalid(request.right) ||
        (IsUuid(request.left.type_id) && IsUuid(request.right.type_id) &&
         !ExecutionDescriptorEquals(request.left.descriptor,
                                    request.right.descriptor))) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.DESCRIPTOR.INVALID",
          "datatype.comparison.rejected",
          "uuid_operand_descriptor_invalid_or_mismatch");
      return result;
    }
    if ((IsUuid(request.left.type_id) &&
         !CanonicalOperationValueValid(request.left)) ||
        (IsUuid(request.right.type_id) &&
         !CanonicalOperationValueValid(request.right))) {
      const auto& invalid = IsUuid(request.left.type_id) &&
                                    !CanonicalOperationValueValid(request.left)
          ? request.left
          : request.right;
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          CanonicalOperationValueDiagnosticCode(
              invalid, "SB_DATATYPE_COMPARISON_REJECTED"),
          "datatype.comparison.rejected", "uuid_comparison_value_invalid");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_COMPARISON_REJECTED",
        "datatype.comparison.rejected", "uuid_comparison_policy_unresolved");
    return result;
  }
  const bool ip_address_incident = IsIpAddress(request.left.type_id) ||
      IsIpAddress(request.right.type_id);
  if (ip_address_incident) {
    const auto ip_address_descriptor_invalid = [](
        const DatatypeOperationValue& value) {
      return IsIpAddress(value.type_id) &&
          !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
              value.descriptor, CanonicalTypeId::ip_address);
    };
    if (ip_address_descriptor_invalid(request.left) ||
        ip_address_descriptor_invalid(request.right) ||
        (IsIpAddress(request.left.type_id) &&
         IsIpAddress(request.right.type_id) &&
         !ExecutionDescriptorEquals(request.left.descriptor,
                                    request.right.descriptor))) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.DESCRIPTOR.INVALID",
          "datatype.comparison.rejected",
          "ip_address_operand_descriptor_invalid_or_mismatch");
      return result;
    }
    if ((IsIpAddress(request.left.type_id) &&
         !CanonicalOperationValueValid(request.left)) ||
        (IsIpAddress(request.right.type_id) &&
         !CanonicalOperationValueValid(request.right))) {
      const auto& invalid =
          IsIpAddress(request.left.type_id) &&
                  !CanonicalOperationValueValid(request.left)
              ? request.left
              : request.right;
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          CanonicalOperationValueDiagnosticCode(
              invalid, "SB_DATATYPE_COMPARISON_REJECTED"),
          "datatype.comparison.rejected",
          "ip_address_comparison_value_invalid");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_COMPARISON_REJECTED",
        "datatype.comparison.rejected",
        "ip_address_comparison_policy_unresolved");
    return result;
  }
  const bool network_prefix_incident =
      IsNetworkPrefix(request.left.type_id) ||
      IsNetworkPrefix(request.right.type_id);
  if (network_prefix_incident) {
    const auto network_prefix_descriptor_invalid = [](
        const DatatypeOperationValue& value) {
      return IsNetworkPrefix(value.type_id) &&
          !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
              value.descriptor, CanonicalTypeId::network_prefix);
    };
    if (network_prefix_descriptor_invalid(request.left) ||
        network_prefix_descriptor_invalid(request.right) ||
        (IsNetworkPrefix(request.left.type_id) &&
         IsNetworkPrefix(request.right.type_id) &&
         !ExecutionDescriptorEquals(request.left.descriptor,
                                    request.right.descriptor))) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.DESCRIPTOR.INVALID",
          "datatype.comparison.rejected",
          "network_prefix_operand_descriptor_invalid_or_mismatch");
      return result;
    }
    if ((IsNetworkPrefix(request.left.type_id) &&
         !CanonicalOperationValueValid(request.left)) ||
        (IsNetworkPrefix(request.right.type_id) &&
         !CanonicalOperationValueValid(request.right))) {
      const auto& invalid =
          IsNetworkPrefix(request.left.type_id) &&
                  !CanonicalOperationValueValid(request.left)
              ? request.left
              : request.right;
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          CanonicalOperationValueDiagnosticCode(
              invalid, "SB_DATATYPE_COMPARISON_REJECTED"),
          "datatype.comparison.rejected",
          "network_prefix_comparison_value_invalid");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_COMPARISON_REJECTED",
        "datatype.comparison.rejected",
        "network_prefix_comparison_policy_unresolved");
    return result;
  }
  const bool mac_address_incident =
      IsMacAddress(request.left.type_id) ||
      IsMacAddress(request.right.type_id);
  if (mac_address_incident) {
    const auto mac_address_descriptor_invalid = [](
        const DatatypeOperationValue& value) {
      return IsMacAddress(value.type_id) &&
          !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
              value.descriptor, CanonicalTypeId::mac_address);
    };
    if (mac_address_descriptor_invalid(request.left) ||
        mac_address_descriptor_invalid(request.right) ||
        (IsMacAddress(request.left.type_id) &&
         IsMacAddress(request.right.type_id) &&
         !ExecutionDescriptorEquals(request.left.descriptor,
                                    request.right.descriptor))) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.DESCRIPTOR.INVALID",
          "datatype.comparison.rejected",
          "mac_address_operand_descriptor_invalid_or_mismatch");
      return result;
    }
    if ((IsMacAddress(request.left.type_id) &&
         !CanonicalOperationValueValid(request.left)) ||
        (IsMacAddress(request.right.type_id) &&
         !CanonicalOperationValueValid(request.right))) {
      const auto& invalid =
          IsMacAddress(request.left.type_id) &&
                  !CanonicalOperationValueValid(request.left)
              ? request.left
              : request.right;
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          CanonicalOperationValueDiagnosticCode(
              invalid, "SB_DATATYPE_COMPARISON_REJECTED"),
          "datatype.comparison.rejected",
          "mac_address_comparison_value_invalid");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_COMPARISON_REJECTED",
        "datatype.comparison.rejected",
        "mac_address_comparison_policy_unresolved");
    return result;
  }
  const bool decimal_incident =
      IsDecimal(request.left.type_id) || IsDecimal(request.right.type_id);
  if (decimal_incident) {
    const auto decimal_descriptor_invalid = [](
        const DatatypeOperationValue& value) {
      return IsDecimal(value.type_id) &&
          !(value.is_null
                ? ExecutionDescriptorValidForType(value.descriptor,
                                                  CanonicalTypeId::decimal)
                : DecimalDescriptorValidForPresent(value.descriptor));
    };
    if (decimal_descriptor_invalid(request.left) ||
        decimal_descriptor_invalid(request.right) ||
        (IsDecimal(request.left.type_id) &&
         IsDecimal(request.right.type_id) &&
         !ExecutionDescriptorEquals(request.left.descriptor,
                                    request.right.descriptor))) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.DESCRIPTOR.INVALID",
          "datatype.comparison.rejected",
          "decimal_operand_descriptor_invalid_or_mismatch");
      return result;
    }
  }
  const bool decimal_float_incident =
      IsDecimalFloat(request.left.type_id) ||
      IsDecimalFloat(request.right.type_id);
  if (decimal_float_incident) {
    const auto decimal_float_descriptor_invalid = [](
        const DatatypeOperationValue& value) {
      return IsDecimalFloat(value.type_id) &&
          !(value.is_null
                ? ExecutionDescriptorValidForType(
                      value.descriptor, CanonicalTypeId::decimal_float)
                : DecimalFloatDescriptorValidForPresent(value.descriptor));
    };
    if (decimal_float_descriptor_invalid(request.left) ||
        decimal_float_descriptor_invalid(request.right) ||
        (IsDecimalFloat(request.left.type_id) &&
         IsDecimalFloat(request.right.type_id) &&
         !ExecutionDescriptorEquals(request.left.descriptor,
                                    request.right.descriptor))) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.DESCRIPTOR.INVALID",
          "datatype.comparison.rejected",
          "decimal_float_operand_descriptor_invalid_or_mismatch");
      return result;
    }
  }
  const bool real128_incident =
      IsReal128(request.left.type_id) || IsReal128(request.right.type_id);
  if (real128_incident &&
      (!IsReal128(request.left.type_id) ||
       !IsReal128(request.right.type_id) ||
       !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
           request.left.descriptor, CanonicalTypeId::real128) ||
       !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
           request.right.descriptor, CanonicalTypeId::real128) ||
       !ExecutionDescriptorEquals(request.left.descriptor,
                                  request.right.descriptor))) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.comparison.rejected",
        "real128_operand_descriptor_invalid_or_mismatch");
    return result;
  }
  const auto unresolved_real_descriptor_invalid = [](
      const DatatypeOperationValue& value) {
    return IsUnresolvedRealSemantics(value.type_id) &&
        (!ExecutionDescriptorPresent(value.descriptor) ||
         !(value.is_null
               ? ExecutionDescriptorValidForType(value.descriptor,
                                                  value.type_id)
               : ExecutionDescriptorExactlyMatchesCurrentBuiltin(
                     value.descriptor, value.type_id)));
  };
  const bool left_unresolved_real_descriptor_invalid =
      unresolved_real_descriptor_invalid(request.left);
  const bool right_unresolved_real_descriptor_invalid =
      unresolved_real_descriptor_invalid(request.right);
  const bool unresolved_real_descriptor_mismatch =
      IsUnresolvedRealSemantics(request.left.type_id) &&
      request.right.type_id == request.left.type_id &&
       !ExecutionDescriptorEquals(request.left.descriptor,
                                  request.right.descriptor);
  if (left_unresolved_real_descriptor_invalid ||
      right_unresolved_real_descriptor_invalid ||
      unresolved_real_descriptor_mismatch) {
    const CanonicalTypeId rejected_type =
        left_unresolved_real_descriptor_invalid
            ? request.left.type_id
            : right_unresolved_real_descriptor_invalid
                ? request.right.type_id
                : request.left.type_id;
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.comparison.rejected",
        UnresolvedRealDetail(
            rejected_type,
            "bfloat16_operand_descriptor_invalid_or_mismatch",
            "real16_operand_descriptor_invalid_or_mismatch",
            "real32_operand_descriptor_invalid_or_mismatch",
            "real64_operand_descriptor_invalid_or_mismatch"));
    return result;
  }
  if (!CanonicalOperationValueValid(request.left) || !CanonicalOperationValueValid(request.right)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
        !CanonicalOperationValueValid(request.left)
            ? CanonicalOperationValueDiagnosticCode(
                  request.left, "SB_DATATYPE_COMPARISON_REJECTED")
            : CanonicalOperationValueDiagnosticCode(
                  request.right, "SB_DATATYPE_COMPARISON_REJECTED"),
        "datatype.comparison.rejected",
        IsUnresolvedRealSemantics(request.left.type_id) ||
                IsUnresolvedRealSemantics(request.right.type_id)
            ? UnresolvedRealDetail(
                  SelectUnresolvedRealType(request.left.type_id,
                                           request.right.type_id),
                  "bfloat16_comparison_policy_unresolved",
                  "real16_comparison_policy_unresolved",
                  "real32_comparison_policy_unresolved",
                  "real64_comparison_policy_unresolved")
            : "canonical_value_encoding_invalid");
    return result;
  }
  if (IsUnresolvedRealSemantics(request.left.type_id) ||
      IsUnresolvedRealSemantics(request.right.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_COMPARISON_REJECTED",
        "datatype.comparison.rejected",
        UnresolvedRealDetail(
            SelectUnresolvedRealType(request.left.type_id,
                                     request.right.type_id),
            "bfloat16_comparison_policy_unresolved",
            "real16_comparison_policy_unresolved",
            "real32_comparison_policy_unresolved",
            "real64_comparison_policy_unresolved"));
    return result;
  }
  if (decimal_incident) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_COMPARISON_REJECTED",
        "datatype.comparison.rejected",
        "decimal_comparison_policy_unresolved");
    return result;
  }
  if (decimal_float_incident) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_COMPARISON_REJECTED",
        "datatype.comparison.rejected",
        "decimal_float_comparison_policy_unresolved");
    return result;
  }
  const bool strict_binary_descriptor_type =
      request.right.type_id == request.left.type_id &&
      (request.left.type_id == CanonicalTypeId::int64 ||
       request.left.type_id == CanonicalTypeId::uint64 ||
       request.left.type_id == CanonicalTypeId::int128 ||
       request.left.type_id == CanonicalTypeId::uint128 ||
       request.left.type_id == CanonicalTypeId::real128);
  const bool strict_binary_descriptors_invalid =
      strict_binary_descriptor_type &&
      (!ExecutionDescriptorPresent(request.left.descriptor) ||
       !ExecutionDescriptorPresent(request.right.descriptor) ||
       !ExecutionDescriptorValidForType(request.left.descriptor,
                                        request.left.type_id) ||
       !ExecutionDescriptorValidForType(request.right.descriptor,
                                        request.right.type_id));
  if (((request.left.type_id == CanonicalTypeId::uint32 ||
        request.left.type_id == CanonicalTypeId::int64 ||
        request.left.type_id == CanonicalTypeId::uint64 ||
        request.left.type_id == CanonicalTypeId::int128 ||
        request.left.type_id == CanonicalTypeId::uint128 ||
        request.left.type_id == CanonicalTypeId::real128) &&
       request.right.type_id == request.left.type_id &&
       !ExecutionDescriptorEquals(request.left.descriptor,
                                  request.right.descriptor)) ||
      strict_binary_descriptors_invalid) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.comparison.rejected",
        request.left.type_id == CanonicalTypeId::uint32
            ? "uint32_descriptor_mismatch"
            : request.left.type_id == CanonicalTypeId::int64
                ? "int64_descriptor_mismatch"
                : request.left.type_id == CanonicalTypeId::uint64
                    ? "uint64_descriptor_mismatch"
                    : request.left.type_id == CanonicalTypeId::int128
                    ? "int128_descriptor_mismatch"
                        : request.left.type_id == CanonicalTypeId::uint128
                            ? "uint128_descriptor_mismatch"
                            : "real128_descriptor_mismatch");
    return result;
  }
  if (request.null_ordering != DatatypeNullOrdering::nulls_first &&
      request.null_ordering != DatatypeNullOrdering::nulls_last) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
        "SB_DATATYPE_COMPARISON_REJECTED", "datatype.comparison.rejected", "invalid_null_ordering");
    return result;
  }
  if (request.left.type_id != request.right.type_id) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_COMPARISON_REJECTED",
                                                        "datatype.comparison.rejected",
                                                        "type_mismatch");
    return result;
  }
  if ((!request.left.is_null && IsInteger(request.left.type_id) &&
       !IntegerOperationValueValid(request.left.type_id,
                                   request.left.encoded_value)) ||
      (!request.right.is_null && IsInteger(request.right.type_id) &&
       !IntegerOperationValueValid(request.right.type_id,
                                   request.right.encoded_value))) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
        "SB_DATATYPE_COMPARISON_REJECTED", "datatype.comparison.rejected",
        "integer_comparison_value_invalid");
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::uint16 &&
      (request.left.is_null || request.right.is_null)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_COMPARISON_REJECTED",
        "datatype.comparison.rejected", "uint16_null_comparison_policy_unresolved");
    return result;
  }
  if ((request.left.type_id == CanonicalTypeId::int32 ||
       request.left.type_id == CanonicalTypeId::uint32 ||
       request.left.type_id == CanonicalTypeId::int64 ||
       request.left.type_id == CanonicalTypeId::uint64 ||
       request.left.type_id == CanonicalTypeId::int128 ||
       request.left.type_id == CanonicalTypeId::uint128) &&
      (request.left.is_null || request.right.is_null)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_COMPARISON_REJECTED",
        "datatype.comparison.rejected",
        request.left.type_id == CanonicalTypeId::int32
            ? "int32_null_comparison_policy_unresolved"
            : request.left.type_id == CanonicalTypeId::uint32
                ? "uint32_null_comparison_policy_unresolved"
                : request.left.type_id == CanonicalTypeId::int64
                    ? "int64_null_comparison_policy_unresolved"
                    : request.left.type_id == CanonicalTypeId::uint64
                        ? "uint64_null_comparison_policy_unresolved"
                        : request.left.type_id == CanonicalTypeId::int128
                            ? "int128_null_comparison_policy_unresolved"
                            : "uint128_null_comparison_policy_unresolved");
    return result;
  }
  if (request.left.is_null || request.right.is_null) {
    if (request.left.is_null && request.right.is_null) {
      result.comparison = 0;
    } else {
      const int null_compare = request.null_ordering == DatatypeNullOrdering::nulls_first ? -1 : 1;
      result.comparison = request.left.is_null ? null_compare : -null_compare;
    }
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::real128) {
    DatatypeNumericOperationRequest numeric;
    numeric.operation = DatatypeNumericOperationKind::compare;
    numeric.type_id = request.left.type_id;
    numeric.left = request.left;
    numeric.right = request.right;
    numeric.context = request.numeric_context;
    const auto compared = ApplyNumericOperation(numeric);
    result.status = compared.status;
    result.comparison = compared.comparison;
    result.diagnostic = compared.diagnostic;
    result.numeric_facts = compared.numeric_facts;
    return result;
  }
  if (IsCanonical128Integer(request.left.type_id)) {
    std::string left_decimal;
    std::string right_decimal;
    const bool decoded_left = request.left.type_id == CanonicalTypeId::int128
        ? DecodeCanonicalInt128Value(request.left.encoded_value, &left_decimal)
        : DecodeCanonicalUint128Value(request.left.encoded_value, &left_decimal);
    const bool decoded_right = request.right.type_id == CanonicalTypeId::int128
        ? DecodeCanonicalInt128Value(request.right.encoded_value, &right_decimal)
        : DecodeCanonicalUint128Value(request.right.encoded_value, &right_decimal);
    if (!decoded_left || !decoded_right) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected",
          request.left.type_id == CanonicalTypeId::int128
              ? "int128_payload_invalid"
              : "uint128_payload_invalid");
      return result;
    }
    libraries::sbl_numeric::NumericRequest numeric;
    numeric.operation = libraries::sbl_numeric::NumericOperation::compare;
    numeric.type = request.left.type_id == CanonicalTypeId::int128
        ? libraries::sbl_numeric::NumericType::int128
        : libraries::sbl_numeric::NumericType::uint128;
    numeric.left = {numeric.type, std::move(left_decimal), false};
    numeric.right = {numeric.type, std::move(right_decimal), false};
    const auto compared =
        libraries::sbl_numeric::ApplyNumericOperation(numeric);
    if (compared.status !=
        libraries::sbl_numeric::NumericStatusCode::ok) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected",
          compared.diagnostic_code.empty()
              ? (request.left.type_id == CanonicalTypeId::int128
                     ? "int128_comparison_failed"
                     : "uint128_comparison_failed")
              : compared.diagnostic_code);
      return result;
    }
    result.comparison = compared.comparison;
    return result;
  }
  if (IsOpaqueRenderOnly(request.left.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_COMPARISON_REJECTED",
                                                        "datatype.comparison.rejected",
                                                        "opaque_comparison_rejected");
    return result;
  }
  std::string left = request.left.encoded_value;
  std::string right = request.right.encoded_value;
  if (request.left.type_id == CanonicalTypeId::int16) {
    std::int64_t left_value = 0;
    std::int64_t right_value = 0;
    if (!DecodeCanonicalInt16Value(left, &left_value) ||
        !DecodeCanonicalInt16Value(right, &right_value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected", "int16_payload_invalid");
      return result;
    }
    result.comparison = left_value < right_value ? -1 :
        (left_value > right_value ? 1 : 0);
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::uint16) {
    std::uint64_t left_value = 0;
    std::uint64_t right_value = 0;
    if (!DecodeCanonicalUint16Value(left, &left_value) ||
        !DecodeCanonicalUint16Value(right, &right_value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected", "uint16_payload_invalid");
      return result;
    }
    result.comparison = left_value < right_value ? -1 :
        (left_value > right_value ? 1 : 0);
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::int32) {
    std::int64_t left_value = 0;
    std::int64_t right_value = 0;
    if (!DecodeCanonicalInt32Value(left, &left_value) ||
        !DecodeCanonicalInt32Value(right, &right_value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected", "int32_payload_invalid");
      return result;
    }
    result.comparison = left_value < right_value ? -1 :
        (left_value > right_value ? 1 : 0);
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::uint32) {
    std::uint64_t left_value = 0;
    std::uint64_t right_value = 0;
    if (!DecodeCanonicalUint32Value(left, &left_value) ||
        !DecodeCanonicalUint32Value(right, &right_value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected", "uint32_payload_invalid");
      return result;
    }
    result.comparison = left_value < right_value ? -1 :
        (left_value > right_value ? 1 : 0);
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::int64) {
    std::int64_t left_value = 0;
    std::int64_t right_value = 0;
    if (!DecodeCanonicalInt64Value(left, &left_value) ||
        !DecodeCanonicalInt64Value(right, &right_value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected", "int64_payload_invalid");
      return result;
    }
    result.comparison = left_value < right_value ? -1 :
        (left_value > right_value ? 1 : 0);
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::uint64) {
    std::uint64_t left_value = 0;
    std::uint64_t right_value = 0;
    if (!DecodeCanonicalUint64Value(left, &left_value) ||
        !DecodeCanonicalUint64Value(right, &right_value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected", "uint64_payload_invalid");
      return result;
    }
    result.comparison = left_value < right_value ? -1 :
        (left_value > right_value ? 1 : 0);
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::int8 ||
      request.left.type_id == CanonicalTypeId::uint8) {
    left = IntegerOperationText(request.left.type_id, left);
    right = IntegerOperationText(request.right.type_id, right);
  }
  if (request.left.type_id == CanonicalTypeId::character) {
    if (!TextSeedReady(request.text_seed)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected",
          "collation_resource_missing");
      return result;
    }
    if (request.case_insensitive_character_compare &&
        !request.text_seed.collation_case_insensitive) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected",
          "collation_mode_mismatch:" + TextSeedDetail(request.text_seed));
      return result;
    }
    if (!MakeTextComparisonKey(request.text_seed, left, &left) ||
        !MakeTextComparisonKey(request.text_seed, right, &right)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
          "SB_DATATYPE_COMPARISON_REJECTED", "datatype.comparison.rejected", "collation_key_failed");
      return result;
    }
  }
  if (IsInteger(request.left.type_id)) {
    left = TrimLeadingZeros(std::move(left));
    right = TrimLeadingZeros(std::move(right));
    const bool left_negative = !left.empty() && left.front() == '-';
    const bool right_negative = !right.empty() && right.front() == '-';
    if (left_negative != right_negative) {
      result.comparison = left_negative ? -1 : 1;
      return result;
    }
    result.comparison = CompareUnsignedDecimal(AbsoluteDecimal(left), AbsoluteDecimal(right));
    if (left_negative) {
      result.comparison = -result.comparison;
    }
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::decimal ||
      request.left.type_id == CanonicalTypeId::decimal_float) {
    if (!CompareFiniteDecimalText(left, right, &result.comparison)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected",
          "numeric_comparison_invalid");
    }
    return result;
  }
  if (IsReal(request.left.type_id)) {
    const long double left_value = std::strtold(left.c_str(), nullptr);
    const long double right_value = std::strtold(right.c_str(), nullptr);
    result.comparison = left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
    return result;
  }
  result.comparison = left < right ? -1 : (left > right ? 1 : 0);
  return result;
}

std::string OrderedIntegerKey(CanonicalTypeId type, const std::string& value) {
  const bool negative = !value.empty() && value.front() == '-';
  // The caller has checked the exact datatype range. Work on bytes so the
  // full 128-bit cohort does not depend on compiler-native integer width.
  std::string bytes(IntegerBits(type) / 8, '\0');
  for (const char digit : AbsoluteDecimal(value)) {
    unsigned carry = static_cast<unsigned>(digit - '0');
    for (std::size_t i = bytes.size(); i != 0; --i) {
      const unsigned next = static_cast<unsigned char>(bytes[i - 1]) * 10u + carry;
      bytes[i - 1] = static_cast<char>(next & 255u);
      carry = next >> 8u;
    }
  }
  if (negative) {
    unsigned carry = 1;
    for (std::size_t i = bytes.size(); i != 0; --i) {
      const unsigned next = (static_cast<unsigned char>(bytes[i - 1]) ^ 255u) + carry;
      bytes[i - 1] = static_cast<char>(next & 255u);
      carry = next >> 8u;
    }
  }
  if (IsSignedInteger(type))
    bytes.front() = static_cast<char>(static_cast<unsigned char>(bytes.front()) ^ 128u);
  return bytes;
}

std::string FixedWidthUnsigned(std::size_t value, std::size_t width) {
  std::string out = std::to_string(value);
  if (out.size() < width) {
    out.insert(out.begin(), width - out.size(), '0');
  }
  return out;
}

void InvertDecimalDigits(std::string* value) {
  for (char& digit : *value) {
    digit = static_cast<char>('9' - (digit - '0'));
  }
}

struct DecimalSortParts {
  bool negative = false;
  std::string integer_part;
  std::string fractional_part;
};

bool DecimalSortPartsFromText(const std::string& value,
                              DecimalSortParts* parts) {
  ParsedDecimal parsed;
  if (!ParseDecimalFiniteText(value, &parsed)) {
    return false;
  }
  while (parsed.scale > 0 && (parsed.coefficient % 10) == 0) {
    parsed.coefficient /= 10;
    --parsed.scale;
  }

  parts->negative = parsed.negative && parsed.coefficient != 0;
  parts->integer_part = "0";
  parts->fractional_part.clear();
  if (parsed.coefficient == 0) {
    return true;
  }

  std::string digits = U128ToString(parsed.coefficient);
  if (parsed.scale == 0) {
    parts->integer_part = std::move(digits);
    return true;
  }
  const auto scale = static_cast<std::size_t>(parsed.scale);
  if (digits.size() <= scale) {
    parts->integer_part = "0";
    parts->fractional_part.assign(scale - digits.size(), '0');
    parts->fractional_part += digits;
  } else {
    const std::size_t integer_digits = digits.size() - scale;
    parts->integer_part = digits.substr(0, integer_digits);
    parts->fractional_part = digits.substr(integer_digits);
  }
  while (parts->integer_part.size() > 1 && parts->integer_part.front() == '0') {
    parts->integer_part.erase(parts->integer_part.begin());
  }
  while (!parts->fractional_part.empty() &&
         parts->fractional_part.back() == '0') {
    parts->fractional_part.pop_back();
  }
  return true;
}

std::string OrderedFiniteDecimalKey(const std::string& value) {
  DecimalSortParts parts;
  if (!DecimalSortPartsFromText(value, &parts)) {
    return {};
  }
  if (parts.integer_part == "0" && parts.fractional_part.empty()) {
    return "1:zero";
  }

  static constexpr std::size_t kIntegerLengthWidth = 6;
  static constexpr std::size_t kMaxEncodedIntegerLength = 999999;
  static constexpr std::size_t kNegativeFractionWidth = 10000;
  const std::size_t integer_length =
      std::min(parts.integer_part.size(), kMaxEncodedIntegerLength);
  if (!parts.negative) {
    return "2:" + FixedWidthUnsigned(integer_length, kIntegerLengthWidth) +
           ":" + parts.integer_part + ":" + parts.fractional_part;
  }

  std::string inverted_integer = parts.integer_part;
  InvertDecimalDigits(&inverted_integer);
  std::string inverted_fraction = parts.fractional_part;
  if (inverted_fraction.size() < kNegativeFractionWidth) {
    inverted_fraction.append(kNegativeFractionWidth - inverted_fraction.size(),
                             '0');
  }
  if (inverted_fraction.size() > kNegativeFractionWidth) {
    inverted_fraction.resize(kNegativeFractionWidth);
  }
  InvertDecimalDigits(&inverted_fraction);
  return "0:" + FixedWidthUnsigned(kMaxEncodedIntegerLength - integer_length,
                                   kIntegerLengthWidth) +
         ":" + inverted_integer + ":" + inverted_fraction;
}

bool CanonicalHashPayload(const DatatypeOperationValue& value,
                          std::string* payload,
                          std::string* failure_detail) {
  if (IsUuid(value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    *failure_detail = "uuid_hash_descriptor_invalid";
    return false;
  }
  if (IsIpAddress(value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    *failure_detail = "ip_address_hash_descriptor_invalid";
    return false;
  }
  if (IsNetworkPrefix(value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    *failure_detail = "network_prefix_hash_descriptor_invalid";
    return false;
  }
  if (IsMacAddress(value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    *failure_detail = "mac_address_hash_descriptor_invalid";
    return false;
  }
  if (IsReal128(value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(value.descriptor,
                                                       value.type_id)) {
    *failure_detail = "real128_hash_descriptor_invalid";
    return false;
  }
  if (!CanonicalOperationValueValid(value)) {
    *failure_detail = IsUuid(value.type_id)
                          ? "uuid_hash_value_invalid"
                          : IsIpAddress(value.type_id)
                          ? "ip_address_hash_value_invalid"
                          : IsNetworkPrefix(value.type_id)
                          ? "network_prefix_hash_value_invalid"
                          : IsMacAddress(value.type_id)
                          ? "mac_address_hash_value_invalid"
                          : IsUnresolvedRealSemantics(value.type_id) &&
                              !value.is_null
                          ? UnresolvedRealDetail(
                                value.type_id,
                                "bfloat16_hash_policy_unresolved",
                                "real16_hash_policy_unresolved",
                                "real32_hash_policy_unresolved",
                                "real64_hash_policy_unresolved")
                          : "canonical_value_encoding_invalid";
    return false;
  }
  if (IsUnresolvedRealSemantics(value.type_id)) {
    *failure_detail = UnresolvedRealDetail(
        value.type_id,
        "bfloat16_hash_policy_unresolved",
        "real16_hash_policy_unresolved",
        "real32_hash_policy_unresolved",
        "real64_hash_policy_unresolved");
    return false;
  }
  if (IsReal128(value.type_id)) {
    *failure_detail = "real128_hash_policy_unresolved";
    return false;
  }
  if (IsDecimal(value.type_id)) {
    *failure_detail = "decimal_hash_policy_unresolved";
    return false;
  }
  if (IsDecimalFloat(value.type_id)) {
    *failure_detail = "decimal_float_hash_policy_unresolved";
    return false;
  }
  if (IsUuid(value.type_id)) {
    *failure_detail = "uuid_hash_policy_unresolved";
    return false;
  }
  if (IsIpAddress(value.type_id)) {
    *failure_detail = "ip_address_hash_policy_unresolved";
    return false;
  }
  if (IsNetworkPrefix(value.type_id)) {
    *failure_detail = "network_prefix_hash_policy_unresolved";
    return false;
  }
  if (IsMacAddress(value.type_id)) {
    *failure_detail = "mac_address_hash_policy_unresolved";
    return false;
  }
  if (value.type_id == CanonicalTypeId::int16 ||
      value.type_id == CanonicalTypeId::uint16 ||
      value.type_id == CanonicalTypeId::int32 ||
      value.type_id == CanonicalTypeId::uint32 ||
      value.type_id == CanonicalTypeId::int64 ||
      value.type_id == CanonicalTypeId::uint64 ||
      value.type_id == CanonicalTypeId::int128 ||
      value.type_id == CanonicalTypeId::uint128) {
    *failure_detail = value.type_id == CanonicalTypeId::int16
        ? "int16_hash_profile_unresolved"
        : value.type_id == CanonicalTypeId::uint16
            ? "uint16_hash_profile_unresolved"
            : value.type_id == CanonicalTypeId::int32
                ? "int32_hash_profile_unresolved"
                : value.type_id == CanonicalTypeId::uint32
                    ? "uint32_hash_profile_unresolved"
                    : value.type_id == CanonicalTypeId::int64
                        ? "int64_hash_profile_unresolved"
                        : value.type_id == CanonicalTypeId::uint64
                            ? "uint64_hash_profile_unresolved"
                            : value.type_id == CanonicalTypeId::int128
                                ? "int128_hash_profile_unresolved"
                                : "uint128_hash_profile_unresolved";
    return false;
  }
  if (value.is_null) {
    payload->clear();
    return true;
  }
  if (IsInteger(value.type_id)) {
    if (!IntegerOperationValueValid(value.type_id, value.encoded_value)) {
      *failure_detail = "integer_hash_value_invalid";
      return false;
    }
    *payload = (value.type_id == CanonicalTypeId::int8 ||
                value.type_id == CanonicalTypeId::uint8)
        ? value.encoded_value
        : TrimLeadingZeros(value.encoded_value);
    return true;
  }
  if (value.type_id == CanonicalTypeId::decimal) {
    ParsedDecimal parsed;
    if (!ParseDecimalFiniteText(value.encoded_value, &parsed)) {
      *failure_detail = "decimal_hash_value_invalid";
      return false;
    }
    while (parsed.scale > 0 && (parsed.coefficient % 10) == 0) {
      parsed.coefficient /= 10;
      --parsed.scale;
    }
    *payload = RenderFixedDecimal(parsed);
    return true;
  }
  if (value.type_id == CanonicalTypeId::decimal_float) {
    DatatypeNumericContext context;
    context.precision = 38;
    context.allow_special_values = true;
    if (!CanonicalizeDecimalFloatText(value.encoded_value, context, payload)) {
      *failure_detail = "decimal_float_hash_value_invalid";
      return false;
    }
    return true;
  }
  if (IsReal(value.type_id)) {
    if (!CanonicalizeReal128Text(value.encoded_value, payload)) {
      *failure_detail = "real_hash_value_invalid";
      return false;
    }
    return true;
  }
  *payload = value.encoded_value;
  return true;
}

DatatypeSortKeyResult MakeDatatypeSortKey(const DatatypeSortKeyRequest& request) {
  DatatypeSortKeyResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.value.type_id == CanonicalTypeId::interval) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTI.TEMPORAL.ORDERING_REFUSED",
        "datatype.sort_key.rejected", "interval_has_no_natural_order");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::timestamp) {
    const char* timestamp_code =
        GenericTimestampStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        timestamp_code != nullptr ? timestamp_code
                                  : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.sort_key.rejected",
        timestamp_code != nullptr ? "timestamp_generic_structural_refusal"
                                  : "timestamp_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::time) {
    const char* time_code = GenericTimeStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        time_code != nullptr ? time_code : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.sort_key.rejected",
        time_code != nullptr ? "time_generic_structural_refusal"
                             : "time_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::date) {
    const char* date_code = GenericDateStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        date_code != nullptr ? date_code : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.sort_key.rejected",
        date_code != nullptr ? "date_generic_structural_refusal"
                             : "date_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::bit_string) {
    if (const char* code =
            GenericBitStringStructuralDiagnosticCode(request.value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, code, "datatype.sort_key.rejected",
          "bit_string_generic_structural_refusal");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BIT.DESCRIPTOR_INVALID",
        "datatype.sort_key.rejected", "bit_string_v3_profile_required");
    return result;
  }
  if (IsBinary(request.value.type_id)) {
    if (!BinaryDescriptorExactlyCurrent(request.value.descriptor)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "CTB.BINARY.DESCRIPTOR_INVALID",
          "datatype.sort_key.rejected", "binary_sort_key_descriptor_invalid");
      return result;
    }
    if (!CanonicalBinaryValueValid(request.value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          CanonicalOperationValueDiagnosticCode(
              request.value, "SB_DATATYPE_SORT_KEY_REJECTED"),
          "datatype.sort_key.rejected", "binary_sort_key_value_invalid");
      return result;
    }
    if (request.null_ordering != DatatypeNullOrdering::nulls_first &&
        request.null_ordering != DatatypeNullOrdering::nulls_last) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "CTB.BINARY.ORDERING_REFUSED",
          "datatype.sort_key.rejected", "invalid_null_ordering");
      return result;
    }
    try {
      std::string prefix;
      if (!BuildBinaryCohortPrefix(&prefix, false)) {
        result.status = ErrorStatus();
        result.diagnostic = MakeDatatypeOperationDiagnostic(
            result.status, "CTB.BINARY.DESCRIPTOR_INVALID",
            "datatype.sort_key.rejected", "binary_identity_row_missing");
        return result;
      }
      result.sort_key = std::move(prefix);
      if (request.value.is_null) {
        result.sort_key.push_back(
            request.null_ordering == DatatypeNullOrdering::nulls_first
                ? '\0' : '\2');
        return result;
      }
      const std::size_t maximum = 168u + 1u +
          2u * request.value.encoded_value.size() + 2u;
      result.sort_key.reserve(maximum);
      result.sort_key.push_back('\1');
      for (const unsigned char octet : request.value.encoded_value) {
        if (octet == 0u) {
          result.sort_key.push_back('\0');
          result.sort_key.push_back(static_cast<char>(0xffu));
        } else {
          result.sort_key.push_back(static_cast<char>(octet));
        }
      }
      result.sort_key.push_back('\0');
      result.sort_key.push_back('\0');
      return result;
    } catch (const std::bad_alloc&) {
      result.sort_key.clear();
      result.status = ResourceErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "CTB.BINARY.RESOURCE_EXHAUSTED",
          "datatype.sort_key.rejected", "binary_sort_key_allocation_failed");
      return result;
    }
  }
  if (IsUuid(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.sort_key.rejected", "uuid_sort_key_descriptor_invalid");
    return result;
  }
  if (IsIpAddress(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.sort_key.rejected",
        "ip_address_sort_key_descriptor_invalid");
    return result;
  }
  if (IsNetworkPrefix(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.sort_key.rejected",
        "network_prefix_sort_key_descriptor_invalid");
    return result;
  }
  if (IsMacAddress(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.sort_key.rejected",
        "mac_address_sort_key_descriptor_invalid");
    return result;
  }
  if (IsReal128(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.sort_key.rejected", "real128_sort_key_descriptor_invalid");
    return result;
  }
  if (!CanonicalOperationValueValid(request.value)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
        CanonicalOperationValueDiagnosticCode(
            request.value, "SB_DATATYPE_SORT_KEY_REJECTED"),
        "datatype.sort_key.rejected",
        IsUuid(request.value.type_id)
            ? "uuid_sort_key_value_invalid"
            : IsIpAddress(request.value.type_id)
            ? "ip_address_sort_key_value_invalid"
            : IsNetworkPrefix(request.value.type_id)
            ? "network_prefix_sort_key_value_invalid"
            : IsMacAddress(request.value.type_id)
            ? "mac_address_sort_key_value_invalid"
            : IsUnresolvedRealSemantics(request.value.type_id) &&
                !request.value.is_null
            ? UnresolvedRealDetail(
                  request.value.type_id,
                  "bfloat16_sort_key_policy_unresolved",
                  "real16_sort_key_policy_unresolved",
                  "real32_sort_key_policy_unresolved",
                  "real64_sort_key_policy_unresolved")
            : "canonical_value_encoding_invalid");
    return result;
  }
  if (IsUnresolvedRealSemantics(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
        "datatype.sort_key.rejected",
        UnresolvedRealDetail(
            request.value.type_id,
            "bfloat16_sort_key_policy_unresolved",
            "real16_sort_key_policy_unresolved",
            "real32_sort_key_policy_unresolved",
            "real64_sort_key_policy_unresolved"));
    return result;
  }
  if (IsReal128(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
        "datatype.sort_key.rejected", "real128_sort_key_policy_unresolved");
    return result;
  }
  if (IsDecimal(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
        "datatype.sort_key.rejected",
        "decimal_sort_key_policy_unresolved");
    return result;
  }
  if (IsDecimalFloat(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
        "datatype.sort_key.rejected",
        "decimal_float_sort_key_policy_unresolved");
    return result;
  }
  if (IsUuid(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
        "datatype.sort_key.rejected", "uuid_sort_key_policy_unresolved");
    return result;
  }
  if (IsIpAddress(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
        "datatype.sort_key.rejected",
        "ip_address_sort_key_policy_unresolved");
    return result;
  }
  if (IsNetworkPrefix(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
        "datatype.sort_key.rejected",
        "network_prefix_sort_key_policy_unresolved");
    return result;
  }
  if (IsMacAddress(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
        "datatype.sort_key.rejected",
        "mac_address_sort_key_policy_unresolved");
    return result;
  }
  if (request.null_ordering != DatatypeNullOrdering::nulls_first &&
      request.null_ordering != DatatypeNullOrdering::nulls_last) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
        "SB_DATATYPE_SORT_KEY_REJECTED", "datatype.sort_key.rejected", "invalid_null_ordering");
    return result;
  }
  if (IsInteger(request.value.type_id)) {
    if (!request.value.is_null &&
        !IntegerOperationValueValid(request.value.type_id,
                                    request.value.encoded_value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
          "SB_DATATYPE_SORT_KEY_REJECTED", "datatype.sort_key.rejected", "integer_sort_key_out_of_range");
      return result;
    }
    result.sort_key.assign(1, request.value.is_null
        ? (request.null_ordering == DatatypeNullOrdering::nulls_first ? '\0' : '\2') : '\1');
    if (!request.value.is_null) {
      if (request.value.type_id == CanonicalTypeId::int16) {
        result.sort_key.push_back(static_cast<char>(
            static_cast<unsigned char>(request.value.encoded_value[1]) ^ 0x80u));
        result.sort_key.push_back(request.value.encoded_value[0]);
      } else if (request.value.type_id == CanonicalTypeId::uint16) {
        result.sort_key.push_back(request.value.encoded_value[1]);
        result.sort_key.push_back(request.value.encoded_value[0]);
      } else if (request.value.type_id == CanonicalTypeId::int32) {
        result.sort_key.push_back(static_cast<char>(
            static_cast<unsigned char>(request.value.encoded_value[3]) ^ 0x80u));
        result.sort_key.push_back(request.value.encoded_value[2]);
        result.sort_key.push_back(request.value.encoded_value[1]);
        result.sort_key.push_back(request.value.encoded_value[0]);
      } else if (request.value.type_id == CanonicalTypeId::uint32) {
        result.sort_key.push_back(request.value.encoded_value[3]);
        result.sort_key.push_back(request.value.encoded_value[2]);
        result.sort_key.push_back(request.value.encoded_value[1]);
        result.sort_key.push_back(request.value.encoded_value[0]);
      } else if (request.value.type_id == CanonicalTypeId::int64) {
        result.sort_key.push_back(static_cast<char>(
            static_cast<unsigned char>(request.value.encoded_value[7]) ^ 0x80u));
        for (int byte = 6; byte >= 0; --byte) {
          result.sort_key.push_back(request.value.encoded_value[byte]);
        }
      } else if (request.value.type_id == CanonicalTypeId::uint64) {
        for (int byte = 7; byte >= 0; --byte) {
          result.sort_key.push_back(request.value.encoded_value[byte]);
        }
      } else if (request.value.type_id == CanonicalTypeId::int128) {
        // Core requires comparison-key behavior for mandatory 128-bit values
        // to cross the sbl_numeric boundary. That backend does not yet expose
        // an INT128 binary-key API, so the adapter cannot independently
        // implement the otherwise specified sign-transformed BE16 key.
        result.status = ErrorStatus();
        result.sort_key.clear();
        result.diagnostic = MakeDatatypeOperationDiagnostic(
            result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
            "datatype.sort_key.rejected",
            "int128_sort_key_backend_api_unavailable");
        return result;
      } else if (request.value.type_id == CanonicalTypeId::uint128) {
        const std::vector<std::uint8_t> payload(
            request.value.encoded_value.begin(),
            request.value.encoded_value.end());
        const auto key =
            libraries::sbl_numeric::MakeUint128OrderKeyLittleEndian(payload);
        if (key.status != libraries::sbl_numeric::NumericStatusCode::ok ||
            key.payload.size() != 16) {
          result.status = ErrorStatus();
          result.sort_key.clear();
          result.diagnostic = MakeDatatypeOperationDiagnostic(
              result.status, "SB_DATATYPE_SORT_KEY_REJECTED",
              "datatype.sort_key.rejected",
              key.diagnostic_code.empty()
                  ? "uint128_sort_key_backend_failed"
                  : key.diagnostic_code);
          return result;
        }
        result.sort_key.append(
            reinterpret_cast<const char*>(key.payload.data()),
            key.payload.size());
      } else {
        const std::string value = IntegerOperationText(
            request.value.type_id, request.value.encoded_value);
        result.sort_key.append(OrderedIntegerKey(request.value.type_id, value));
      }
    }
    return result;
  }
  if (request.value.is_null) {
    result.sort_key =
        request.null_ordering == DatatypeNullOrdering::nulls_first ? "00:null" : "ff:null";
    return result;
  }
  if (IsOpaqueRenderOnly(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_SORT_KEY_REJECTED",
                                                        "datatype.sort_key.rejected",
                                                        "opaque_sort_key_rejected");
    return result;
  }
  std::string value = request.value.encoded_value;
  if (request.value.type_id == CanonicalTypeId::character) {
    {
      if (!TextSeedReady(request.text_seed)) {
        result.status = ErrorStatus();
        result.diagnostic = MakeDatatypeOperationDiagnostic(
            result.status,
            "SB_DATATYPE_SORT_KEY_REJECTED",
            "datatype.sort_key.rejected",
            "collation_resource_missing");
        return result;
      }
      if (request.case_insensitive_character_compare &&
          !request.text_seed.collation_case_insensitive) {
        result.status = ErrorStatus();
        result.diagnostic = MakeDatatypeOperationDiagnostic(
            result.status,
            "SB_DATATYPE_SORT_KEY_REJECTED",
            "datatype.sort_key.rejected",
            "collation_mode_mismatch:" + TextSeedDetail(request.text_seed));
        return result;
      }
      if (!MakeTextComparisonKey(request.text_seed, value, &value)) {
        result.status = ErrorStatus();
        result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
            "SB_DATATYPE_SORT_KEY_REJECTED", "datatype.sort_key.rejected", "collation_key_failed");
        return result;
      }
    }
    result.sort_key = TextComparisonCohort(request.text_seed) + value;
    return result;
  }
  if (IsNumeric(request.value.type_id)) {
    const ParsedSpecialNumeric special = ParseSpecialNumericText(value);
    if (special.kind == NumericSpecialKind::negative_infinity) {
      result.sort_key = "11:00:-inf";
      return result;
    }
    if (special.kind == NumericSpecialKind::positive_infinity) {
      result.sort_key = "11:fe:+inf";
      return result;
    }
    if (special.kind == NumericSpecialKind::quiet_nan ||
        special.kind == NumericSpecialKind::signaling_nan) {
      result.sort_key = "11:ff:nan";
      return result;
    }
    const std::string ordered = OrderedFiniteDecimalKey(value);
    if (ordered.empty()) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          "SB_DATATYPE_SORT_KEY_REJECTED",
          "datatype.sort_key.rejected",
          "numeric_sort_key_invalid");
      return result;
    }
    result.sort_key = "11:" + ordered;
    return result;
  }
  if (IsTemporal(request.value.type_id)) {
    result.sort_key = "30:" + value;
    return result;
  }
  result.sort_key = "40:" + HexEncodeLower(value);
  return result;
}

DatatypeHashResult HashDatatypeValue(const DatatypeHashRequest& request) {
  DatatypeHashResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.value.type_id == CanonicalTypeId::interval) {
    const char* interval_code =
        GenericIntervalStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        interval_code != nullptr ? interval_code
                                 : "CTI.INTERVAL.DESCRIPTOR_INVALID",
        "datatype.hash.rejected",
        interval_code != nullptr ? "interval_generic_structural_refusal"
                                 : "interval_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::timestamp) {
    const char* timestamp_code =
        GenericTimestampStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        timestamp_code != nullptr ? timestamp_code
                                  : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.hash.rejected",
        timestamp_code != nullptr ? "timestamp_generic_structural_refusal"
                                  : "timestamp_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::time) {
    const char* time_code = GenericTimeStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        time_code != nullptr ? time_code : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.hash.rejected",
        time_code != nullptr ? "time_generic_structural_refusal"
                             : "time_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::date) {
    const char* date_code = GenericDateStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        date_code != nullptr ? date_code : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.hash.rejected",
        date_code != nullptr ? "date_generic_structural_refusal"
                             : "date_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::bit_string) {
    if (const char* code =
            GenericBitStringStructuralDiagnosticCode(request.value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, code, "datatype.hash.rejected",
          "bit_string_generic_structural_refusal");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BIT.DESCRIPTOR_INVALID",
        "datatype.hash.rejected", "bit_string_v3_profile_required");
    return result;
  }
  if (IsBinary(request.value.type_id)) {
    if (!BinaryDescriptorExactlyCurrent(request.value.descriptor)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "CTB.BINARY.DESCRIPTOR_INVALID",
          "datatype.hash.rejected", "binary_hash_descriptor_invalid");
      return result;
    }
    if (!CanonicalBinaryValueValid(request.value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          CanonicalOperationValueDiagnosticCode(
              request.value, "SB_DATATYPE_HASH_REJECTED"),
          "datatype.hash.rejected", "binary_hash_value_invalid");
      return result;
    }
    try {
      std::string header;
      if (!BuildBinaryCohortPrefix(&header, true)) {
        result.status = ErrorStatus();
        result.diagnostic = MakeDatatypeOperationDiagnostic(
            result.status, "CTB.BINARY.DESCRIPTOR_INVALID",
            "datatype.hash.rejected", "binary_identity_row_missing");
        return result;
      }
      header.push_back(request.value.is_null ? '\1' : '\0');
      AppendLittleU64(&header, request.value.is_null
          ? 0u : request.value.encoded_value.size());
      const hash::HashDigestSegment segments[] = {
          {reinterpret_cast<const platform::byte*>(header.data()), header.size()},
          {request.value.is_null || request.value.encoded_value.empty()
               ? nullptr
               : reinterpret_cast<const platform::byte*>(
                     request.value.encoded_value.data()),
           request.value.is_null ? 0u : request.value.encoded_value.size()},
      };
      const auto digest = hash::ComputeSha256DigestParts(segments, 2);
      if (!digest.ok() || digest.digest_bytes != hash::kSha256DigestBytes) {
        result.status = ResourceErrorStatus();
        result.diagnostic = MakeDatatypeOperationDiagnostic(
            result.status, "CTB.BINARY.RESOURCE_EXHAUSTED",
            "datatype.hash.rejected", "binary_sha256_failed");
        return result;
      }
      result.stable_hash_hex = hash::HexLower(digest.digest);
      return result;
    } catch (const std::bad_alloc&) {
      result.stable_hash_hex.clear();
      result.status = ResourceErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "CTB.BINARY.RESOURCE_EXHAUSTED",
          "datatype.hash.rejected", "binary_hash_allocation_failed");
      return result;
    }
  }
  std::uint64_t hash = 1469598103934665603ull;
  auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(static_cast<std::uint64_t>(request.value.type_id));
  mix(request.value.is_null ? 1u : 0u);
  std::string payload;
  std::string failure_detail;
  if (!CanonicalHashPayload(request.value, &payload, &failure_detail)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        CanonicalOperationValueDiagnosticCode(
            request.value, "SB_DATATYPE_HASH_REJECTED"),
        "datatype.hash.rejected",
        failure_detail);
    return result;
  }
  if (!request.value.is_null && request.value.type_id == CanonicalTypeId::character) {
    if (!MakeTextComparisonKey(request.text_seed, payload, &payload)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
          "SB_DATATYPE_HASH_REJECTED", "datatype.hash.rejected", "collation_key_failed");
      return result;
    }
    payload = TextComparisonCohort(request.text_seed) + payload;
  }
  for (unsigned char c : payload) {
    mix(c);
  }
  static constexpr char kHex[] = "0123456789abcdef";
  result.stable_hash_hex.resize(16);
  for (int index = 15; index >= 0; --index) {
    result.stable_hash_hex[index] = kHex[hash & 0x0fu];
    hash >>= 4;
  }
  return result;
}

DatatypeSerializationResult SerializeDatatypeValue(
    const DatatypeSerializationRequest& request) {
  DatatypeSerializationResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.value.type_id == CanonicalTypeId::interval) {
    const char* interval_code =
        GenericIntervalStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        interval_code != nullptr
            ? interval_code
            : "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
        "datatype.serialization.rejected",
        interval_code != nullptr ? "interval_generic_structural_refusal"
                                 : "interval_composed_adapter_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::timestamp) {
    const char* timestamp_code =
        GenericTimestampStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        timestamp_code != nullptr
            ? timestamp_code
            : "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
        "datatype.serialization.rejected",
        timestamp_code != nullptr ? "timestamp_generic_structural_refusal"
                                  : "timestamp_composed_adapter_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::time) {
    const char* time_code = GenericTimeStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        time_code != nullptr
            ? time_code
            : "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
        "datatype.serialization.rejected",
        time_code != nullptr ? "time_generic_structural_refusal"
                             : "time_composed_adapter_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::date) {
    const char* date_code = GenericDateStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        date_code != nullptr
            ? date_code
            : "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
        "datatype.serialization.rejected",
        date_code != nullptr ? "date_generic_structural_refusal"
                             : "date_composed_adapter_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::bit_string) {
    if (const char* code =
            GenericBitStringStructuralDiagnosticCode(request.value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, code, "datatype.serialization.rejected",
          "bit_string_generic_structural_refusal");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
        "datatype.serialization.rejected", "bit_string_composed_adapter_required");
    return result;
  }
  if (IsBinary(request.value.type_id) &&
      !BinaryDescriptorExactlyCurrent(request.value.descriptor)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BINARY.DESCRIPTOR_INVALID",
        "datatype.serialization.rejected",
        "binary_serialization_descriptor_invalid");
    return result;
  }
  if (IsUuid(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.serialization.rejected",
        "uuid_serialization_descriptor_invalid");
    return result;
  }
  if (IsIpAddress(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.serialization.rejected",
        "ip_address_serialization_descriptor_invalid");
    return result;
  }
  if (IsNetworkPrefix(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.serialization.rejected",
        "network_prefix_serialization_descriptor_invalid");
    return result;
  }
  if (IsMacAddress(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.serialization.rejected",
        "mac_address_serialization_descriptor_invalid");
    return result;
  }
  if (IsCharacter(request.value.type_id) &&
      (!ExecutionDescriptorPresent(request.value.descriptor) ||
       !ExecutionDescriptorValidForType(request.value.descriptor,
                                        request.value.type_id))) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.serialization.rejected",
        "character_serialization_descriptor_invalid");
    return result;
  }
  if (IsReal128(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.serialization.rejected",
        "real128_serialization_descriptor_invalid");
    return result;
  }
  if (!CanonicalOperationValueValid(request.value)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
        CanonicalOperationValueDiagnosticCode(
            request.value, "SB_DATATYPE_SERIALIZATION_REJECTED"),
        "datatype.serialization.rejected",
        IsUuid(request.value.type_id)
            ? "uuid_serialization_value_invalid"
            : IsIpAddress(request.value.type_id)
            ? "ip_address_serialization_value_invalid"
            : IsNetworkPrefix(request.value.type_id)
            ? "network_prefix_serialization_value_invalid"
            : IsMacAddress(request.value.type_id)
            ? "mac_address_serialization_value_invalid"
            : IsCharacter(request.value.type_id)
            ? CanonicalCharacterFailureDetail(request.value)
            : IsUnresolvedRealSemantics(request.value.type_id) &&
                !request.value.is_null
            ? UnresolvedRealDetail(
                  request.value.type_id,
                  "bfloat16_present_value_policy_unresolved",
                  "real16_present_value_policy_unresolved",
                  "real32_present_value_policy_unresolved",
                  "real64_present_value_policy_unresolved")
            : "canonical_value_encoding_invalid");
    return result;
  }
  if (IsBinary(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BINARY.SERIALIZATION_PROFILE_MISSING",
        "datatype.serialization.rejected",
        "generic_sbdv1_lacks_binary_receipt");
    return result;
  }
  if (IsDecimal(request.value.type_id) && !request.value.is_null) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SERIALIZATION_REJECTED",
        "datatype.serialization.rejected",
        "decimal_serialization_policy_unresolved");
    return result;
  }
  if (IsDecimalFloat(request.value.type_id) && !request.value.is_null) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SERIALIZATION_REJECTED",
        "datatype.serialization.rejected",
        "decimal_float_serialization_policy_unresolved");
    return result;
  }
  if (IsUuid(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SERIALIZATION_REJECTED",
        "datatype.serialization.rejected",
        "uuid_serialization_policy_unresolved");
    return result;
  }
  if (IsIpAddress(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SERIALIZATION_REJECTED",
        "datatype.serialization.rejected",
        "ip_address_serialization_policy_unresolved");
    return result;
  }
  if (IsNetworkPrefix(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SERIALIZATION_REJECTED",
        "datatype.serialization.rejected",
        "network_prefix_serialization_policy_unresolved");
    return result;
  }
  if (IsMacAddress(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SERIALIZATION_REJECTED",
        "datatype.serialization.rejected",
        "mac_address_serialization_policy_unresolved");
    return result;
  }
  if (IsCharacter(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_SERIALIZATION_REJECTED",
        "datatype.serialization.rejected",
        "character_serialization_policy_unresolved");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::unknown) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_SERIALIZATION_REJECTED",
                                                        "datatype.serialization.rejected",
                                                        "unknown_type");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::null_type) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.serialization.rejected",
        "standalone_null_type");
    return result;
  }
  result.descriptor = request.value.descriptor;
  result.serialized_value = std::string("SBDV1;type=") +
                            CanonicalTypeName(request.value.type_id) +
                            ";state=" + (request.value.is_null ? "null" : "value") +
                            ";payload=" + HexEncodeLower(request.value.encoded_value);
  return result;
}

static DatatypeDeserializationResult DeserializeDatatypeValueUnchecked(
    const DatatypeDeserializationRequest& request) {
  DatatypeDeserializationResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.expected_type_id == CanonicalTypeId::interval ||
      LegacySbdv1EncodedTypeIs(request.serialized_value, "interval")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
        "datatype.deserialization.rejected",
        "interval_composed_adapter_required");
    return result;
  }
  if (request.expected_type_id == CanonicalTypeId::bit_string ||
      StartsWith(request.serialized_value, "SBDV1;type=bit_string;")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
        "datatype.deserialization.rejected", "bit_string_composed_adapter_required");
    return result;
  }
  if (request.expected_type_id == CanonicalTypeId::date ||
      StartsWith(request.serialized_value, "SBDV1;type=date;")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
        "datatype.deserialization.rejected", "date_composed_adapter_required");
    return result;
  }
  if (request.expected_type_id == CanonicalTypeId::time ||
      StartsWith(request.serialized_value, "SBDV1;type=time;")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
        "datatype.deserialization.rejected", "time_composed_adapter_required");
    return result;
  }
  if (request.expected_type_id == CanonicalTypeId::timestamp ||
      StartsWith(request.serialized_value, "SBDV1;type=timestamp;")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
        "datatype.deserialization.rejected",
        "timestamp_composed_adapter_required");
    return result;
  }
  if (request.expected_type_id == CanonicalTypeId::binary &&
      !BinaryDescriptorExactlyCurrent(request.expected_descriptor)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BINARY.DESCRIPTOR_INVALID",
        "datatype.deserialization.rejected",
        "expected_binary_descriptor_invalid");
    return result;
  }
  const bool binary_expected =
      request.expected_type_id == CanonicalTypeId::binary ||
      (request.expected_type_id == CanonicalTypeId::unknown &&
       BinaryDescriptorExactlyCurrent(request.expected_descriptor));
  if (binary_expected && StartsWith(request.serialized_value, "SBDVUUID")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BINARY.FRAME_INVALID",
        "datatype.deserialization.rejected",
        "binary_serialization_magic_invalid");
    return result;
  }
  if (StartsWith(request.serialized_value, "SBDVUUID")) {
    const auto& bytes = request.serialized_value;
    if (bytes.size() < 10) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
          "SB_DATATYPE_DESERIALIZATION_REJECTED", "datatype.deserialization.rejected",
          "uuid_binary_frame_invalid");
      return result;
    }
    const auto declared_payload_size =
        static_cast<std::size_t>(static_cast<unsigned char>(bytes[9]));
    if (bytes.size() != 10 + declared_payload_size) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
          "datatype.deserialization.rejected", "uuid_binary_frame_invalid");
      return result;
    }
    if (request.expected_type_id != CanonicalTypeId::unknown &&
        request.expected_type_id != CanonicalTypeId::uuid) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.DESCRIPTOR.INVALID",
          "datatype.deserialization.rejected", "type_mismatch");
      return result;
    }
    const bool decoded_is_null = bytes[8] == '\0';
    if (!ExecutionDescriptorExactlyMatchesCurrentBuiltin(
            request.expected_descriptor, CanonicalTypeId::uuid)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.DESCRIPTOR.INVALID",
          "datatype.deserialization.rejected", "expected_descriptor_invalid");
      return result;
    }
    if (bytes[8] != '\0' && bytes[8] != '\1') {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DTYPE.VALUE.STATE_UNHANDLED",
          "datatype.deserialization.rejected", "value_state_invalid");
      return result;
    }
    if (decoded_is_null && (bytes[9] != '\0' || bytes.size() != 10)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.NULL_STATE.INVALID",
          "datatype.deserialization.rejected", "null_payload_present");
      return result;
    }
    if (bytes[8] == '\1' && (bytes[9] != '\20' || bytes.size() != 26)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
          "datatype.deserialization.rejected", "uuid_binary_frame_invalid");
      return result;
    }
    if (decoded_is_null && !request.expected_descriptor.nullable_allowed) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.NULL_NOT_ADMITTED",
          "datatype.deserialization.rejected", "expected_descriptor_not_nullable");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected",
        "uuid_deserialization_policy_unresolved");
    return result;
  }
  if (!StartsWith(request.serialized_value, "SBDV1;")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        binary_expected
                                                            ? "CTB.BINARY.FRAME_INVALID"
                                                            : "SB_DATATYPE_DESERIALIZATION_REJECTED",
                                                        "datatype.deserialization.rejected",
                                                        "bad_magic");
    return result;
  }
  std::map<std::string, std::string> fields;
  bool malformed_fields = false;
  const auto parts = Split(request.serialized_value.substr(6), ';');
  if (parts.size() != 3 ||
      (!request.serialized_value.empty() &&
       request.serialized_value.back() == ';')) {
    malformed_fields = true;
  }
  for (const auto& part : parts) {
    const auto equals = part.find('=');
    if (equals == std::string::npos || equals == 0) {
      malformed_fields = true;
      continue;
    }
    const auto key = part.substr(0, equals);
    if ((key != "type" && key != "state" && key != "payload") ||
        !fields.emplace(key, part.substr(equals + 1)).second) {
      malformed_fields = true;
    }
  }
  if (malformed_fields || fields.size() != 3 || !fields.contains("type") ||
      !fields.contains("state") || !fields.contains("payload")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        binary_expected ? "CTB.BINARY.FRAME_INVALID"
                        : "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected", "field_frame_invalid");
    return result;
  }
  const std::string stable_type_name = LowerAscii(fields.at("type"));
  if (stable_type_name == "null" || stable_type_name == "null_type") {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "standalone_null_type");
    return result;
  }
  const CanonicalTypeId type_id =
      CanonicalTypeIdFromStableName(stable_type_name);
  if (type_id == CanonicalTypeId::unknown ||
      (request.expected_type_id != CanonicalTypeId::unknown &&
       request.expected_type_id != type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "type_mismatch");
    return result;
  }
  if (IsBinary(type_id) &&
      !BinaryDescriptorExactlyCurrent(request.expected_descriptor)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BINARY.DESCRIPTOR_INVALID",
        "datatype.deserialization.rejected",
        "expected_binary_descriptor_invalid");
    return result;
  }
  bool ok = false;
  DatatypeOperationValue staged;
  staged.type_id = type_id;
  const auto& state = fields.at("state");
  if (IsUuid(type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.expected_descriptor, type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (IsIpAddress(type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.expected_descriptor, type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (IsNetworkPrefix(type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.expected_descriptor, type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (IsMacAddress(type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.expected_descriptor, type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (IsCharacter(type_id) &&
      (!ExecutionDescriptorPresent(request.expected_descriptor) ||
       !ExecutionDescriptorValidForType(request.expected_descriptor,
                                        type_id))) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (ExecutionDescriptorPresent(request.expected_descriptor) &&
      !ExecutionDescriptorValidForType(request.expected_descriptor, type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (type_id == CanonicalTypeId::real128 &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.expected_descriptor, type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (state != "null" && state != "value") {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        binary_expected ? "CTB.BINARY.FRAME_INVALID"
                        : "DTYPE.VALUE.STATE_UNHANDLED",
        "datatype.deserialization.rejected", "value_state_invalid");
    return result;
  }
  staged.is_null = state == "null";
  if (!staged.is_null && IsDecimal(type_id) &&
      !DecimalDescriptorValidForPresent(request.expected_descriptor)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (!staged.is_null && IsDecimalFloat(type_id) &&
      !DecimalFloatDescriptorValidForPresent(
          request.expected_descriptor)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (!staged.is_null && IsUnresolvedRealSemantics(type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.expected_descriptor, type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  if (staged.is_null &&
      !ExecutionDescriptorValidForType(request.expected_descriptor, type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.deserialization.rejected", "expected_descriptor_invalid");
    return result;
  }
  staged.descriptor = request.expected_descriptor;
  if (staged.is_null) {
    const auto& payload = fields.at("payload");
    if (!payload.empty()) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "DATATYPE.NULL_STATE.INVALID",
          "datatype.deserialization.rejected", "null_payload_present");
      return result;
    }
    ok = true;
  }
  if (!staged.is_null) {
    // Empty TEXT/BINARY is a value, not NULL or a missing payload field.
    if ((type_id == CanonicalTypeId::character || IsBinary(type_id)) &&
        fields.contains("payload") &&
        fields.at("payload").empty()) {
      ok = true;
    } else {
      if (IsBinary(type_id) &&
          fields["payload"].size() > 2u * kCanonicalBinaryMaximumBytes) {
        result.status = ErrorStatus();
        result.diagnostic = MakeDatatypeOperationDiagnostic(
            result.status, "CTB.BINARY.LENGTH_EXCEEDED",
            "datatype.deserialization.rejected",
            "binary_payload_length_exceeded");
        return result;
      }
      staged.encoded_value = HexDecodeStrict(fields["payload"], false, &ok);
    }
  }
  if (!staged.is_null && !ok) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        binary_expected
                                                            ? "CTB.BINARY.FRAME_INVALID"
                                                            : "SB_DATATYPE_DESERIALIZATION_REJECTED",
                                                        "datatype.deserialization.rejected",
                                                        "payload_hex_invalid");
    return result;
  }
  if (!staged.is_null && type_id == CanonicalTypeId::real128) {
    if (staged.encoded_value.size() != 16) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "NUMERIC.ENCODING.NONCANONICAL",
          "datatype.deserialization.rejected",
          "real128_payload_width_invalid");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected",
        "real128_deserialization_policy_unresolved");
    return result;
  }
  if (staged.is_null && !request.expected_descriptor.nullable_allowed) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.NULL_NOT_ADMITTED",
        "datatype.deserialization.rejected", "expected_descriptor_not_nullable");
    return result;
  }
  if (!CanonicalOperationValueValid(staged)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
        CanonicalOperationValueDiagnosticCode(
            staged, "SB_DATATYPE_DESERIALIZATION_REJECTED"),
        "datatype.deserialization.rejected",
        IsIpAddress(staged.type_id)
            ? "ip_address_deserialization_value_invalid"
            : IsNetworkPrefix(staged.type_id)
            ? "network_prefix_deserialization_value_invalid"
            : IsMacAddress(staged.type_id)
            ? "mac_address_deserialization_value_invalid"
            : IsCharacter(staged.type_id)
            ? CanonicalCharacterFailureDetail(staged)
            : IsUnresolvedRealSemantics(staged.type_id) && !staged.is_null
            ? UnresolvedRealDetail(
                  staged.type_id,
                  "bfloat16_present_value_policy_unresolved",
                  "real16_present_value_policy_unresolved",
                  "real32_present_value_policy_unresolved",
                  "real64_present_value_policy_unresolved")
            : "canonical_value_encoding_invalid");
    return result;
  }
  if (IsBinary(staged.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BINARY.SERIALIZATION_PROFILE_MISSING",
        "datatype.deserialization.rejected",
        "generic_sbdv1_lacks_binary_receipt");
    return result;
  }
  if (IsDecimal(staged.type_id) && !staged.is_null) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected",
        "decimal_deserialization_policy_unresolved");
    return result;
  }
  if (IsDecimalFloat(staged.type_id) && !staged.is_null) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected",
        "decimal_float_deserialization_policy_unresolved");
    return result;
  }
  if (IsUuid(staged.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected",
        "uuid_deserialization_policy_unresolved");
    return result;
  }
  if (IsIpAddress(staged.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected",
        "ip_address_deserialization_policy_unresolved");
    return result;
  }
  if (IsNetworkPrefix(staged.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected",
        "network_prefix_deserialization_policy_unresolved");
    return result;
  }
  if (IsMacAddress(staged.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected",
        "mac_address_deserialization_policy_unresolved");
    return result;
  }
  if (IsCharacter(staged.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DESERIALIZATION_REJECTED",
        "datatype.deserialization.rejected",
        "character_deserialization_policy_unresolved");
    return result;
  }
  result.value = std::move(staged);
  return result;
}

DatatypeDeserializationResult DeserializeDatatypeValue(
    const DatatypeDeserializationRequest& request) {
  try {
    return DeserializeDatatypeValueUnchecked(request);
  } catch (const std::bad_alloc&) {
    if (request.expected_type_id != CanonicalTypeId::binary &&
        !BinaryDescriptorExactlyCurrent(request.expected_descriptor)) {
      throw;
    }
    DatatypeDeserializationResult result;
    result.status = ResourceErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BINARY.RESOURCE_EXHAUSTED",
        "datatype.deserialization.rejected",
        "binary_deserialization_allocation_failed");
    return result;
  }
}

DatatypeDisplayRenderResult RenderDatatypeValueForDisplay(
    const DatatypeDisplayRenderRequest& request) {
  DatatypeDisplayRenderResult result;
  result.status = OkStatus();
  result.diagnostic =
      MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  result.explicit_display_boundary = true;
  if (request.value.type_id == CanonicalTypeId::interval) {
    const char* interval_code =
        GenericIntervalStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        interval_code != nullptr ? interval_code
                                 : "CTI.INTERVAL.DESCRIPTOR_INVALID",
        "datatype.display_render.rejected",
        interval_code != nullptr ? "interval_generic_structural_refusal"
                                 : "interval_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::timestamp) {
    const char* timestamp_code =
        GenericTimestampStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        timestamp_code != nullptr ? timestamp_code
                                  : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.display_render.rejected",
        timestamp_code != nullptr ? "timestamp_generic_structural_refusal"
                                  : "timestamp_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::time) {
    const char* time_code = GenericTimeStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        time_code != nullptr ? time_code : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.display_render.rejected",
        time_code != nullptr ? "time_generic_structural_refusal"
                             : "time_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::date) {
    const char* date_code = GenericDateStructuralDiagnosticCode(request.value);
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        date_code != nullptr ? date_code : "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "datatype.display_render.rejected",
        date_code != nullptr ? "date_generic_structural_refusal"
                             : "date_d710_profile_required");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::bit_string) {
    if (const char* code =
            GenericBitStringStructuralDiagnosticCode(request.value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, code, "datatype.display_render.rejected",
          "bit_string_generic_structural_refusal");
      return result;
    }
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BIT.DESCRIPTOR_INVALID",
        "datatype.display_render.rejected", "bit_string_v3_profile_required");
    return result;
  }
  if (IsBinary(request.value.type_id) &&
      !BinaryDescriptorExactlyCurrent(request.value.descriptor)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "CTB.BINARY.DESCRIPTOR_INVALID",
        "datatype.display_render.rejected",
        "binary_display_descriptor_invalid");
    return result;
  }
  if (IsUuid(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.display_render.rejected",
        "uuid_display_descriptor_invalid");
    return result;
  }
  if (IsIpAddress(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.display_render.rejected",
        "ip_address_display_descriptor_invalid");
    return result;
  }
  if (IsNetworkPrefix(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.display_render.rejected",
        "network_prefix_display_descriptor_invalid");
    return result;
  }
  if (IsMacAddress(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.display_render.rejected",
        "mac_address_display_descriptor_invalid");
    return result;
  }
  if (IsReal128(request.value.type_id) &&
      !ExecutionDescriptorExactlyMatchesCurrentBuiltin(
          request.value.descriptor, request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "DATATYPE.DESCRIPTOR.INVALID",
        "datatype.display_render.rejected",
        "real128_display_descriptor_invalid");
    return result;
  }
  if (!CanonicalOperationValueValid(request.value)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        CanonicalOperationValueDiagnosticCode(
            request.value, "SB_DATATYPE_DISPLAY_RENDER_REJECTED"),
        "datatype.display_render.rejected",
        IsUuid(request.value.type_id)
            ? "uuid_display_value_invalid"
            : IsIpAddress(request.value.type_id)
            ? "ip_address_display_value_invalid"
            : IsNetworkPrefix(request.value.type_id)
            ? "network_prefix_display_value_invalid"
            : IsMacAddress(request.value.type_id)
            ? "mac_address_display_value_invalid"
            : request.value.type_id == CanonicalTypeId::null_type
            ? "standalone_null_type"
            : IsUnresolvedRealSemantics(request.value.type_id) &&
                      !request.value.is_null
                ? UnresolvedRealDetail(
                      request.value.type_id,
                      "bfloat16_display_policy_unresolved",
                      "real16_display_policy_unresolved",
                      "real32_display_policy_unresolved",
                      "real64_display_policy_unresolved")
                : "canonical_value_encoding_invalid");
    return result;
  }
  if (IsUnresolvedRealSemantics(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
        "datatype.display_render.rejected",
        UnresolvedRealDetail(
            request.value.type_id,
            "bfloat16_display_policy_unresolved",
            "real16_display_policy_unresolved",
            "real32_display_policy_unresolved",
            "real64_display_policy_unresolved"));
    return result;
  }
  if (IsReal128(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
        "datatype.display_render.rejected",
        "real128_display_policy_unresolved");
    return result;
  }
  if (IsDecimal(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
        "datatype.display_render.rejected",
        "decimal_display_policy_unresolved");
    return result;
  }
  if (IsDecimalFloat(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
        "datatype.display_render.rejected",
        "decimal_float_display_policy_unresolved");
    return result;
  }
  if (IsUuid(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
        "datatype.display_render.rejected",
        "uuid_display_policy_unresolved");
    return result;
  }
  if (IsIpAddress(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
        "datatype.display_render.rejected",
        "ip_address_display_policy_unresolved");
    return result;
  }
  if (IsNetworkPrefix(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
        "datatype.display_render.rejected",
        "network_prefix_display_policy_unresolved");
    return result;
  }
  if (IsMacAddress(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
        "datatype.display_render.rejected",
        "mac_address_display_policy_unresolved");
    return result;
  }
  result.canonical_type_name = CanonicalTypeName(request.value.type_id);
  if (request.value.is_null) {
    result.display_value = "NULL";
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::int128) {
    if (!DecodeCanonicalInt128Value(request.value.encoded_value,
                                    &result.display_value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "NUMERIC.ENCODING.NONCANONICAL",
          "datatype.display_render.rejected", "int128_payload_invalid");
    }
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::uint128) {
    if (!DecodeCanonicalUint128Value(request.value.encoded_value,
                                     &result.display_value)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "NUMERIC.ENCODING.NONCANONICAL",
          "datatype.display_render.rejected", "uint128_payload_invalid");
    }
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::int16 ||
      request.value.type_id == CanonicalTypeId::uint16 ||
      request.value.type_id == CanonicalTypeId::int32 ||
      request.value.type_id == CanonicalTypeId::uint32 ||
      request.value.type_id == CanonicalTypeId::int64 ||
      request.value.type_id == CanonicalTypeId::uint64) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status, "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
        "datatype.display_render.rejected",
        request.value.type_id == CanonicalTypeId::int16
            ? "int16_render_profile_unresolved"
            : request.value.type_id == CanonicalTypeId::uint16
                ? "uint16_render_profile_unresolved"
                : request.value.type_id == CanonicalTypeId::int32
                    ? "int32_render_profile_unresolved"
                    : request.value.type_id == CanonicalTypeId::uint32
                        ? "uint32_render_profile_unresolved"
                        : request.value.type_id == CanonicalTypeId::int64
                            ? "int64_render_profile_unresolved"
                            : "uint64_render_profile_unresolved");
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::binary) {
    try {
      const auto encoded = HexEncodeLower(request.value.encoded_value);
      result.display_value = request.export_literal
          ? "X'" + encoded + "'"
          : "0x" + encoded;
    } catch (const std::bad_alloc&) {
      result.display_value.clear();
      result.status = ResourceErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status, "CTB.BINARY.RESOURCE_EXHAUSTED",
          "datatype.display_render.rejected",
          "binary_render_allocation_failed");
    }
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::bit_string) {
    result.display_value = "0x" + HexEncodeLower(request.value.encoded_value);
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::boolean) {
    result.display_value =
        static_cast<unsigned char>(request.value.encoded_value[0]) == 1u
            ? "TRUE"
            : "FALSE";
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::int8 ||
      request.value.type_id == CanonicalTypeId::uint8) {
    result.display_value = IntegerOperationText(
        request.value.type_id, request.value.encoded_value);
    return result;
  }
  if (DisplayAsMetadataSummary(request.value.type_id) &&
      (request.redact_opaque_payload || request.value.type_id != CanonicalTypeId::opaque_extension)) {
    result.payload_redacted = true;
    result.display_value =
        DisplayMetadataSummary(request.value.type_id, request.value.encoded_value.size());
    return result;
  }
  if (request.export_literal && request.value.type_id == CanonicalTypeId::character) {
    std::string quoted = "'";
    for (char ch : request.value.encoded_value) {
      if (ch == '\'') {
        quoted += "''";
      } else {
        quoted.push_back(ch);
      }
    }
    quoted.push_back('\'');
    result.display_value = std::move(quoted);
    return result;
  }
  if (DisplayAsStructuredText(request.value.type_id)) {
    result.display_value = request.value.encoded_value;
    return result;
  }
  result.display_value = request.value.encoded_value;
  return result;
}

CanonicalTypeId CanonicalTypeIdFromStableName(const std::string& stable_name) {
  const std::string lower = LowerAscii(stable_name);
  if (lower == "string" || lower == "varchar" || lower == "char" || lower == "text") { return CanonicalTypeId::character; }
  if (lower == "integer" || lower == "int") {
    return CanonicalTypeId::int32;
  }
  if (lower == "bigint") { return CanonicalTypeId::int64; }
  if (lower == "smallint") { return CanonicalTypeId::int16; }
  if (lower == "double" || lower == "double_precision" ||
      lower == "double precision") {
    return CanonicalTypeId::real64;
  }
  if (lower == "float") { return CanonicalTypeId::real32; }
  if (lower == "numeric") { return CanonicalTypeId::decimal; }
  if (lower == "set") { return CanonicalTypeId::set_value; }
  // SQL/XML exposes the public descriptor spelling `xml`; the storage/runtime
  // authority remains the canonical xml_document type identity.
  if (lower == "json") { return CanonicalTypeId::json_document; }
  if (lower == "list<text nullable>") { return CanonicalTypeId::list; }
  if (lower == "xml") { return CanonicalTypeId::xml_document; }
  // CDSSV-VECTOR: normalized builtin descriptor profile names retain their
  // exact public spelling while binding to an existing canonical vector type
  // identity.  They are not new storage type codes or donor aliases.
  if (lower == "bit_vector") { return CanonicalTypeId::binary_vector; }
  if (lower == "int8_vector" || lower == "float16_vector") {
    return CanonicalTypeId::quantized_vector;
  }
  // Zoned temporal families require distinct qualified identities. Until those
  // identities are admitted, a stable-name lookup must fail closed instead of
  // collapsing a zoned value into a timezone-free local-civil base type.
  if (lower == "time_tz" || lower == "timetz" ||
      lower == "time with time zone") {
    return CanonicalTypeId::unknown;
  }
  if (lower == "timestamp_tz" || lower == "timestamptz" ||
      lower == "timestamp with time zone") {
    return CanonicalTypeId::unknown;
  }
  for (const auto& descriptor : BuiltinDatatypeDescriptors()) {
    if (LowerAscii(descriptor.stable_name) == lower || LowerAscii(CanonicalTypeName(descriptor.type_id)) == lower) {
      return descriptor.type_id;
    }
  }
  return CanonicalTypeId::unknown;
}

bool IsUuidText(const std::string& value) {
  if (value.size() != 36) { return false; }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') { return false; }
    } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

DiagnosticRecord MakeDatatypeOperationDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) { arguments.push_back({"detail", std::move(detail)}); }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.operations");
}

}  // namespace scratchbird::core::datatypes
