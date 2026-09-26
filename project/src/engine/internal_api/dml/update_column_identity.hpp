// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../api_types.hpp"
#include "../../../core/datatypes/datatype_catalog_manifest.hpp"
#include "../../../core/uuid/uuid.hpp"
#include <utility>

namespace scratchbird::engine::internal_api {

struct DmlUpdateBoundColumnV1 {
  EngineUuid column_uuid;
  std::uint64_t column_generation = 0;
  std::uint32_t ordinal = 0;
  std::string canonical_name_key;
  EngineUuid datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation = 0;
  EngineUuid type_uuid;
  std::uint64_t type_generation = 0;
  std::string codec_id;
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;
};

// Retain a column's exact binary binding after the owner has looked up the
// selected catalog row. This is not catalog selection, authorization or
// issuance. encoded_descriptor and type spellings are never identity sources.
// Stage all allocating copies before replacing the caller's output.
inline bool BindDmlUpdateColumnIdentityV1(
    const EngineUuid& column_uuid, std::uint64_t column_generation,
    std::uint32_t ordinal, const std::string& canonical_name_key,
    const EngineDescriptor& descriptor,
    const core::datatypes::DatatypeTypeCodecIdentityRowV1& selected,
    DmlUpdateBoundColumnV1* output) {
  const auto valid = core::uuid::IsEngineIdentityUuid;
  if (output == nullptr || !valid(column_uuid) || column_generation == 0 ||
      !valid(descriptor.descriptor_uuid) ||
      !valid(descriptor.datatype_descriptor_uuid) || !valid(descriptor.type_uuid) ||
      descriptor.datatype_descriptor_generation == 0 ||
      !valid(selected.catalog_snapshot_uuid) || selected.catalog_generation == 0 ||
      selected.registry_generation == 0 ||
      descriptor.datatype_descriptor_uuid != selected.descriptor_uuid ||
      descriptor.datatype_descriptor_generation != selected.descriptor_generation ||
      descriptor.type_uuid != selected.type_uuid || selected.type_generation == 0 ||
      selected.codec_id.empty() || selected.codec_version == 0 ||
      selected.codec_generation == 0) return false;
  DmlUpdateBoundColumnV1 staged;
  staged.column_uuid = column_uuid;
  staged.column_generation = column_generation;
  staged.ordinal = ordinal;
  staged.canonical_name_key = canonical_name_key;
  staged.datatype_descriptor_uuid = selected.descriptor_uuid;
  staged.datatype_descriptor_generation = selected.descriptor_generation;
  staged.type_uuid = selected.type_uuid;
  staged.type_generation = selected.type_generation;
  staged.codec_id = selected.codec_id;
  staged.codec_version = selected.codec_version;
  staged.codec_generation = selected.codec_generation;
  *output = std::move(staged);
  return true;
}

} // namespace scratchbird::engine::internal_api
