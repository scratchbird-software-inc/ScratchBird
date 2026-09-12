// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "engine/sblr/contextual_text_literal_v2_codec.hpp"
#include "engine/sblr/relational_descriptor_codec.hpp"
#include <limits>

namespace scratchbird::engine::internal_api {

// Structural/profile agreement, not a replacement for live catalog, receipt
// or comparison-resource authority. UUIDs remain binary throughout.
inline bool MatchesContextualTextDescriptorV2(
    const RelationalTypeDescriptor& descriptor,
    const sblr::ContextualTextLiteralProfileV2& profile,
    bool target) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (!sblr::ValidateRelationalTypeDescriptorV1(descriptor) ||
      !descriptor.datatype_identity_authoritative ||
      descriptor.descriptor_id != (target ? profile.target_descriptor_handle
                                         : profile.literal_descriptor_handle) ||
      (profile.target_character_limit > std::numeric_limits<std::uint32_t>::max() &&
       profile.target_character_limit != maximum)) return false;
  const bool width_matches = profile.target_character_limit == maximum
      ? !descriptor.width.has_value()
      : descriptor.width.has_value() && *descriptor.width == profile.target_character_limit;
  return width_matches &&
      (descriptor.nullability == RelationalNullability::kNonNull ||
       (target && descriptor.nullability == RelationalNullability::kNullable)) &&
      descriptor.descriptor_uuid.bytes == profile.descriptor_uuid &&
      descriptor.type_uuid.bytes == profile.type_uuid &&
      descriptor.collation_uuid.has_value() &&
      descriptor.collation_uuid->bytes == profile.collation_uuid &&
      !descriptor.timezone_profile_id.has_value() &&
      !descriptor.precision.has_value() && !descriptor.scale.has_value() &&
      descriptor.descriptor_generation == profile.descriptor_generation &&
      descriptor.type_generation == profile.type_generation &&
      descriptor.codec_id == sblr::kContextualTextCodecIdentifierV2 &&
      descriptor.codec_version == profile.codec_version &&
      descriptor.codec_generation == profile.codec_generation &&
      descriptor.statement_receipt_uuid.bytes == profile.statement_receipt_uuid &&
      descriptor.datatype_catalog_snapshot_uuid.bytes == profile.catalog_snapshot_uuid &&
      descriptor.datatype_catalog_generation == profile.catalog_generation &&
      descriptor.datatype_registry_generation == profile.datatype_registry_generation;
}

}  // namespace scratchbird::engine::internal_api
