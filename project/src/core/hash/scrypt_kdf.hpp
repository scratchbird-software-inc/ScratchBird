// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../common/scrypt_work_estimate.hpp"
#include <cstddef>
#include <cstdint>

namespace scratchbird::core::crypto {
struct ScryptInput { const std::uint8_t* data = nullptr; std::size_t size = 0; };
struct ScryptOutput { std::uint8_t* data = nullptr; std::size_t size = 0; };
struct ScryptCancellationProbe {
  bool (*requested)(void*) = nullptr;
  void* context = nullptr;
};
enum class ScryptExecutionCode { ok, invalid_parameters, invalid_buffers, cost_overflow, cancelled };

// Secret-data scratch on the native call stack, in addition to the exact
// B/V/X/Y extent and output. No recursive calls or hidden heap allocations.
// This is not the ABI stack-frame size; thread-stack and callback capacity
// remain separately admitted runtime resources.
std::size_t ScryptFixedScratchBytes() noexcept;

// Mechanism only: caller owns resource/security/provider admission, exclusive
// buffers and a synchronous cancellation source. No allocation or I/O. Invalid
// extents are untouched. Once accepted, workspace/scratch are erased on every
// exit; output is also erased on cancellation or a throwing probe. Success
// preserves only the complete raw key. Inputs may alias each other, but no
// mutable extent may overlap another extent. Callback exceptions propagate.
// The separately owned probe must obey the engine's callback resource policy.
ScryptExecutionCode DeriveScryptKey(ScryptInput password, ScryptInput salt,
    std::uint64_t n, std::uint32_t r, std::uint32_t p,
    ScryptOutput workspace, ScryptOutput output,
    ScryptCancellationProbe cancellation = {});
} // namespace scratchbird::core::crypto
