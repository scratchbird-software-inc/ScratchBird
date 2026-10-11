// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_catalog_manifest.hpp"
#include "datatype_decimal_codec_profile.hpp"
#include <optional>

namespace scratchbird::parser::sbsql {

// Check supplied occurrence evidence, never synthesize a profile from the base
// codec UUID (which is a different identity). The engine still admits the live
// statement/occurrence; this check only prevents lossy or contradictory lowering.
template <typename Descriptor>
bool MatchesNativeDecimalOccurrenceProfile(
    const Descriptor& descriptor, std::optional<std::uint32_t> precision,
    std::optional<std::uint32_t> scale) {
  namespace dt = scratchbird::core::datatypes;
  if (!descriptor.decimal_codec_profile) return true;
  const auto& profile = *descriptor.decimal_codec_profile;
  if (!precision || !scale ||
      !dt::ExactDecimalCodecProfileMatches(profile.uuid, profile.generation,
                                          *precision, *scale)) return false;
  const dt::DatatypeTypeCodecIdentityRowV1* match = nullptr;
  for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = entry.legacy_fields;
    if (row.catalog_snapshot_uuid != descriptor.datatype_catalog_snapshot_uuid ||
        row.catalog_generation != descriptor.datatype_catalog_generation ||
        row.registry_generation != descriptor.datatype_registry_generation ||
        row.type_uuid != descriptor.type_uuid) continue;
    if (match) return false;
    match = &row;
  }
  return match && match->canonical_binary_type_code ==
      static_cast<std::uint32_t>(dt::CanonicalTypeId::decimal) &&
      descriptor.descriptor_generation == match->descriptor_generation &&
      descriptor.type_generation == match->type_generation &&
      descriptor.codec_id == match->codec_id &&
      descriptor.codec_version == match->codec_version &&
      descriptor.codec_generation == match->codec_generation;
}

// Compiled registry membership is not statement authority. Callers must also
// validate the owning receipt and column/result descriptor binding. A scalar
// descriptor occurrence need not equal its canonical datatype descriptor.
template <typename Descriptor>
bool MatchesNativeDatatypeCodecIdentity(
    const Descriptor& descriptor,
    const scratchbird::core::platform::Uuid& canonical_datatype_descriptor_uuid) {
  const scratchbird::core::datatypes::DatatypeTypeCodecIdentityRowV1* match = nullptr;
  for (const auto& entry :
       scratchbird::core::datatypes::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = entry.legacy_fields;
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
