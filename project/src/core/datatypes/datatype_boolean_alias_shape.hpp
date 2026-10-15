// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace scratchbird::core::datatypes {

// Immutable wire-shape exception from datatype.boolean.v1, not a catalog
// lookup or authority grant. Keep this independent of execution descriptors so
// external wire-codec consumers cannot acquire an engine dependency merely to
// validate the single specified descriptor/type alias.
inline constexpr bool IsExactCanonicalBooleanAliasShapeV1(
    const std::array<std::uint8_t,16>& descriptor_uuid,
    std::uint64_t descriptor_generation,
    const std::array<std::uint8_t,16>& type_uuid,
    std::uint64_t type_generation,std::string_view codec_id,
    std::uint16_t codec_version,std::uint64_t codec_generation,
    bool containing_slot_nullability_known) noexcept {
  constexpr std::array<std::uint8_t,16> identity{
      0x01,0,0,0,0x62,0x6f,0x7f,0x6c,0xa5,0x61,0x6e,0,0,0,0,0};
  return containing_slot_nullability_known&&descriptor_uuid==identity&&
      type_uuid==identity&&descriptor_generation==1&&type_generation==1&&
      codec_id=="datatype.boolean.u8.v1"&&codec_version==1&&codec_generation==1;
}

}  // namespace scratchbird::core::datatypes
