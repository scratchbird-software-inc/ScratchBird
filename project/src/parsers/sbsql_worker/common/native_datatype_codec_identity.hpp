// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_catalog_manifest.hpp"

namespace scratchbird::parser::sbsql {

// Compiled registry membership is not statement authority. Callers must also
// validate the owning receipt and column/result descriptor binding. A scalar
// descriptor occurrence need not equal its canonical datatype descriptor.
template <typename Descriptor>
bool MatchesNativeDatatypeCodecIdentity(
    const Descriptor& descriptor,
    const scratchbird::core::platform::Uuid& canonical_datatype_descriptor_uuid) {
  const scratchbird::core::datatypes::DatatypeTypeCodecIdentityRowV1* match = nullptr;
  for (const auto& row :
       scratchbird::core::datatypes::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    if (row.catalog_snapshot_uuid != descriptor.datatype_catalog_snapshot_uuid ||
        row.catalog_generation != descriptor.datatype_catalog_generation ||
        row.registry_generation != descriptor.datatype_registry_generation ||
        row.descriptor_uuid != canonical_datatype_descriptor_uuid) continue;
    if (match != nullptr) return false;
    match = &row;
  }
  return match != nullptr &&
         descriptor.descriptor_generation == match->descriptor_generation &&
         descriptor.type_uuid == match->type_uuid &&
         descriptor.type_generation == match->type_generation &&
         descriptor.codec_id == match->codec_id &&
         descriptor.codec_version == match->codec_version &&
         descriptor.codec_generation == match->codec_generation;
}

}  // namespace scratchbird::parser::sbsql
