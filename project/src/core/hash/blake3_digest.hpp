// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace scratchbird::core::hash {

using Blake3Digest = std::array<std::uint8_t, 32>;

// Unkeyed BLAKE3-256 over the complete borrowed byte sequence. No allocation,
// external provider, text encoding, mutable global state, or input mutation.
// The caller owns input lifetime, resource admission and execution cancellation.
Blake3Digest ComputeBlake3Digest(const std::vector<std::uint8_t>& input) noexcept;

}  // namespace scratchbird::core::hash
