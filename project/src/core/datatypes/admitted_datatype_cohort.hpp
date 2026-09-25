// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../platform/runtime_platform.hpp"
namespace scratchbird::core::datatypes {
// DATATYPE-TYPE-CODEC-IDENTITY-SUCCESSOR-V2: codec admission only. A caller
// must additionally match its engine-owned live statement receipt.
inline constexpr platform::Uuid kDatatypeCohortV1{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x01}};
inline constexpr platform::Uuid kDatatypeCohortV2{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x02}};
inline bool IsAdmittedDatatypeCohort(const platform::Uuid& snapshot,
    platform::u64 catalog_generation, platform::u64 registry_generation) {
  return (snapshot == kDatatypeCohortV1 && catalog_generation == 1 && registry_generation == 1) ||
         (snapshot == kDatatypeCohortV2 && catalog_generation == 2 && registry_generation == 2);
}
inline bool IsAdmittedDatatypeCohort(const std::array<platform::byte, 16>& snapshot,
    platform::u64 catalog_generation, platform::u64 registry_generation) {
  return IsAdmittedDatatypeCohort(platform::Uuid{snapshot}, catalog_generation, registry_generation);
}
}  // namespace scratchbird::core::datatypes
