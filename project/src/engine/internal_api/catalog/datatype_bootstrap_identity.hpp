// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/platform/runtime_platform.hpp"

namespace scratchbird::engine::internal_api {
// D711 bootstrap cohort published for newly created databases. These constants
// are for bootstrap publishers, not a fallback for statement consumers: those
// must use and validate their live receipt's exact datatype catalog cohort.
inline constexpr core::platform::Uuid kBootstrapDatatypeCatalogUuid{{
    0x01, 0xa1, 0x09, 0x5f, 0xf2, 0x05, 0x72, 0xd3,
    0xab, 0xea, 0x15, 0xc6, 0xd4, 0x1a, 0x4a, 0xd4}};
inline constexpr std::uint64_t kBootstrapDatatypeCatalogGeneration = 11;
inline constexpr std::uint64_t kBootstrapDatatypeRegistryGeneration = 11;
}  // namespace scratchbird::engine::internal_api
