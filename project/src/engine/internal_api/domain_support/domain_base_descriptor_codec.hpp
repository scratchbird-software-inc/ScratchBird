// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog/catalog_object_lifecycle_codec.hpp"

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

}  // namespace scratchbird::engine::internal_api
