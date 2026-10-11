// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_catalog_manifest.hpp"
#include <cstdint>

namespace scratchbird::core::datatypes {
// Profile labels are not semantic authority, but their owned buffers still
// belong to the input. Include reserved storage and the string terminator.
inline bool DatatypeIdentityStringStorageOverlaps(
    const void* output, std::size_t bytes,
    const DatatypeTypeCodecIdentityRowV1& identity) noexcept {
  if (!output || bytes == 0) return false;
  for (const auto* value : {&identity.codec_id, &identity.canonical_name,
       &identity.canonical_byte_order, &identity.canonical_representation,
       &identity.canonical_charset, &identity.invalid_encoding_diagnostic_id,
       &identity.comparison_profile}) {
    const auto left = reinterpret_cast<std::uintptr_t>(output);
    const auto right = reinterpret_cast<std::uintptr_t>(value->data());
    if (left <= right ? right-left < bytes : left-right <= value->capacity()) return true;
  }
  return false;
}
}  // namespace scratchbird::core::datatypes
