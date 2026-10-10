// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog/catalog_object_lifecycle_codec.hpp"
#include "datatype_type_codec_identity_v3.hpp"

namespace scratchbird::engine::internal_api {

// A structural carrier, not catalog admission. The owning operation must
// revalidate this exact datatype UUID/generation and its modifiers in the
// current catalog cohort. Names cannot supply a missing identity.
inline bool DomainBaseDescriptorStructureValidV1(const EngineDescriptor& value) {
  for (const auto& identity : {value.descriptor_uuid, value.type_uuid,
                               value.datatype_descriptor_uuid})
    if (!core::uuid::IsEngineIdentityUuid(identity)) return false;
  for (const auto& identity : {value.charset_uuid, value.collation_uuid})
    if (!identity.is_nil() && !core::uuid::IsEngineIdentityUuid(identity)) return false;
  return value.datatype_descriptor_generation != 0 &&
      value.descriptor_kind == "scalar" && !value.canonical_type_name.empty();
}

template<class T>
inline auto DomainBaseDescriptorFieldsV1(T& value) {
  return std::tie(value.descriptor_uuid, value.type_uuid,
                  value.datatype_descriptor_uuid, value.datatype_descriptor_generation,
                  value.charset_uuid, value.collation_uuid, value.descriptor_kind,
                  value.canonical_type_name, value.encoded_descriptor);
}

inline bool EncodeDomainBaseDescriptorV1(const EngineDescriptor& value,
                                         std::string* output) {
  if (!output || !DomainBaseDescriptorStructureValidV1(value)) return false;
  std::string encoded("SBDTDS01");
  if (!std::apply([&](const auto&... field) {
        return (catalog_record_codec::Put(encoded, field) && ...);
      }, DomainBaseDescriptorFieldsV1(value)) ||
      encoded.size() > kApiBehaviorRecordMaximumBytes) return false;
  output->swap(encoded);
  return true;
}

inline bool DecodeDomainBaseDescriptorV1(std::string_view encoded,
                                         EngineDescriptor* output) {
  if (!output || encoded.size() < 8 || encoded.size() > kApiBehaviorRecordMaximumBytes ||
      encoded.substr(0, 8) != "SBDTDS01") return false;
  const std::span<const std::uint8_t> bytes(
      reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
  std::size_t cursor = 8;
  EngineDescriptor staged;
  if (!std::apply([&](auto&... field) {
        return (catalog_record_codec::Get(bytes, cursor, field) && ...);
      }, DomainBaseDescriptorFieldsV1(staged)) || cursor != bytes.size() ||
      !DomainBaseDescriptorStructureValidV1(staged)) return false;
  *output = std::move(staged);
  return true;
}

// Explicit inheritance binding to one immutable registry cohort, including
// codec and every policy tuple. This is still a structural carrier; execution
// must authenticate it against that cohort and the live domain chain.
struct DomainInheritedBaseBindingV1 {
  EngineDescriptor base;
  EngineUuid catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::uint64_t registry_generation = 0;
  EngineUuid codec_uuid;
  std::uint32_t codec_version = 0;
  std::uint64_t codec_generation = 0;
  std::array<EngineUuid, 9> policy_uuids{};
  std::array<std::uint64_t, 9> policy_generations{};
  std::string native_profile_fingerprint;
  bool operator==(const DomainInheritedBaseBindingV1&) const = default;
};

inline DomainInheritedBaseBindingV1 CaptureDomainInheritedBaseBindingV1(
    const EngineDescriptor& base,
    const core::datatypes::DatatypeTypeCodecIdentityRowV3& row) {
  DomainInheritedBaseBindingV1 result;
  result.base = base;
  const auto& legacy = row.legacy_fields;
  result.catalog_snapshot_uuid = legacy.catalog_snapshot_uuid;
  result.catalog_generation = legacy.catalog_generation;
  result.registry_generation = legacy.registry_generation;
  result.codec_uuid = legacy.codec_uuid;
  result.codec_version = legacy.codec_version;
  result.codec_generation = legacy.codec_generation;
  result.policy_uuids = {row.descriptor_policy.uuid, row.canonicalization_policy.uuid,
      row.ordering_policy.uuid, row.hash_policy.uuid, row.operation_policy.uuid,
      legacy.numeric_context_uuid, legacy.special_value_policy_uuid,
      legacy.comparison_policy_uuid, row.native_fields.policy_profile_uuid};
  result.policy_generations = {row.descriptor_policy.generation, row.canonicalization_policy.generation,
      row.ordering_policy.generation, row.hash_policy.generation, row.operation_policy.generation,
      legacy.numeric_context_generation, legacy.special_value_policy_generation,
      legacy.comparison_policy_generation, row.native_fields.policy_profile_generation};
  result.native_profile_fingerprint.assign(
      reinterpret_cast<const char*>(row.native_fields.profile_fingerprint_sha256.data()), 32);
  return result;
}

inline bool DomainInheritedBaseBindingStructureValidV1(const DomainInheritedBaseBindingV1& value) {
  if (!DomainBaseDescriptorStructureValidV1(value.base) ||
      !core::uuid::IsEngineIdentityUuid(value.catalog_snapshot_uuid) ||
      (!value.codec_uuid.is_nil() && !core::uuid::IsEngineIdentityUuid(value.codec_uuid)) ||
      !value.catalog_generation || !value.registry_generation ||
      !value.codec_version || value.codec_version > 65535 || !value.codec_generation ||
      value.native_profile_fingerprint.size() != 32) return false;
  for (std::size_t i = 0; i < value.policy_uuids.size(); ++i) {
    if (value.policy_uuids[i].is_nil() != (value.policy_generations[i] == 0) ||
        (!value.policy_uuids[i].is_nil() && !core::uuid::IsEngineIdentityUuid(value.policy_uuids[i]))) return false;
  }
  if (value.policy_uuids.back().is_nil() &&
      value.native_profile_fingerprint.find_first_not_of('\0') != std::string::npos) return false;
  return true;
}

template<class T>
inline auto DomainInheritedBaseBindingFieldsV1(T& value) {
  return std::tie(value.catalog_snapshot_uuid, value.catalog_generation, value.registry_generation,
                  value.codec_uuid, value.codec_version, value.codec_generation);
}

inline bool EncodeDomainInheritedBaseBindingV1(const DomainInheritedBaseBindingV1& value,
                                               std::string* output) {
  if (!output || !DomainInheritedBaseBindingStructureValidV1(value)) return false;
  std::string encoded("SBDPFB01"), base;
  if (!EncodeDomainBaseDescriptorV1(value.base, &base) ||
      !std::apply([&](const auto&... field) {
        return (catalog_record_codec::Put(encoded, field) && ...);
      }, DomainInheritedBaseBindingFieldsV1(value))) return false;
  for (std::size_t i = 0; i < value.policy_uuids.size(); ++i)
    if (!catalog_record_codec::Put(encoded, value.policy_uuids[i]) ||
        !catalog_record_codec::Put(encoded, value.policy_generations[i])) return false;
  encoded.append(value.native_profile_fingerprint);
  if (!catalog_record_codec::Put(encoded, base) || encoded.size() > kApiBehaviorRecordMaximumBytes) return false;
  output->swap(encoded);
  return true;
}

inline bool DecodeDomainInheritedBaseBindingV1(std::string_view encoded,
                                               DomainInheritedBaseBindingV1* output) {
  if (!output || encoded.size() > kApiBehaviorRecordMaximumBytes || !encoded.starts_with("SBDPFB01")) return false;
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
  std::size_t cursor = 8;
  DomainInheritedBaseBindingV1 staged;
  if (!std::apply([&](auto&... field) {
        return (catalog_record_codec::Get(bytes, cursor, field) && ...);
      }, DomainInheritedBaseBindingFieldsV1(staged))) return false;
  for (std::size_t i = 0; i < staged.policy_uuids.size(); ++i)
    if (!catalog_record_codec::Get(bytes, cursor, staged.policy_uuids[i]) ||
        !catalog_record_codec::Get(bytes, cursor, staged.policy_generations[i])) return false;
  if (cursor > bytes.size() || bytes.size() - cursor < 32) return false;
  staged.native_profile_fingerprint.assign(encoded.substr(cursor, 32));
  cursor += 32;
  std::string base;
  if (!catalog_record_codec::Get(bytes, cursor, base) || cursor != bytes.size() ||
      !DecodeDomainBaseDescriptorV1(base, &staged.base) || !DomainInheritedBaseBindingStructureValidV1(staged)) return false;
  *output = std::move(staged);
  return true;
}

}  // namespace scratchbird::engine::internal_api
