// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../platform/runtime_platform.hpp"
namespace scratchbird::core::datatypes {
// Immutable datatype codec cohorts V1-V11: codec admission only. A caller
// must additionally match its engine-owned live statement receipt.
inline constexpr platform::Uuid kDatatypeCohortV1{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x01}};
inline constexpr platform::Uuid kDatatypeCohortV2{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x02}};
inline constexpr platform::Uuid kDatatypeCohortV3{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x03}};
inline constexpr platform::Uuid kDatatypeCohortV4{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x04}};
inline constexpr platform::Uuid kDatatypeCohortV5{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x05}};
inline constexpr platform::Uuid kDatatypeCohortV6{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x06}};
inline constexpr platform::Uuid kDatatypeCohortV7{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x07}};
inline constexpr platform::Uuid kDatatypeCohortV8{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x08}};
inline constexpr platform::Uuid kDatatypeCohortV9{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x09}};
inline constexpr platform::Uuid kDatatypeCohortV10{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10}};
inline constexpr platform::Uuid kDatatypeCohortV11{{
    0x01,0xa1,0x09,0x5f,0xf2,0x05,0x72,0xd3,
    0xab,0xea,0x15,0xc6,0xd4,0x1a,0x4a,0xd4}};
inline bool IsAdmittedDatatypeCohort(const platform::Uuid& snapshot,
    platform::u64 catalog_generation, platform::u64 registry_generation) {
  return (snapshot == kDatatypeCohortV1 && catalog_generation == 1 && registry_generation == 1) ||
         (snapshot == kDatatypeCohortV2 && catalog_generation == 2 && registry_generation == 2) ||
         (snapshot == kDatatypeCohortV3 && catalog_generation == 3 && registry_generation == 3) ||
         (snapshot == kDatatypeCohortV4 && catalog_generation == 4 && registry_generation == 4) ||
         (snapshot == kDatatypeCohortV5 && catalog_generation == 5 && registry_generation == 5) ||
         (snapshot == kDatatypeCohortV6 && catalog_generation == 6 && registry_generation == 6) ||
         (snapshot == kDatatypeCohortV7 && catalog_generation == 7 && registry_generation == 7) ||
         (snapshot == kDatatypeCohortV8 && catalog_generation == 8 && registry_generation == 8) ||
         (snapshot == kDatatypeCohortV9 && catalog_generation == 9 && registry_generation == 9) ||
         (snapshot == kDatatypeCohortV10 && catalog_generation == 10 && registry_generation == 10) ||
         (snapshot == kDatatypeCohortV11 && catalog_generation == 11 && registry_generation == 11);
}
inline bool IsAdmittedDatatypeCohort(const std::array<platform::byte, 16>& snapshot,
    platform::u64 catalog_generation, platform::u64 registry_generation) {
  return IsAdmittedDatatypeCohort(platform::Uuid{snapshot}, catalog_generation, registry_generation);
}
}  // namespace scratchbird::core::datatypes
