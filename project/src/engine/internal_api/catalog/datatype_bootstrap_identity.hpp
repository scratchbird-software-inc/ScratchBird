// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/platform/runtime_platform.hpp"

namespace scratchbird::engine::internal_api {
// Preliminary V11 bootstrap cohort published by the engine. These constants
// are for bootstrap publishers, not a fallback for statement consumers: those
// must use and validate their live receipt's exact datatype catalog cohort.
inline constexpr core::platform::Uuid kBootstrapDatatypeCatalogUuid{{
    0x01, 0x9d, 0, 0, 0, 0, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0xd7, 0x02}};
inline constexpr std::uint64_t kBootstrapDatatypeCatalogGeneration = 2;
inline constexpr std::uint64_t kBootstrapDatatypeRegistryGeneration = 2;
}  // namespace scratchbird::engine::internal_api
