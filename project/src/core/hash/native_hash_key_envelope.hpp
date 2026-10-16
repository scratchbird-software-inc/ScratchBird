// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "runtime_platform.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace scratchbird::core::crypto {

// HASH-STANDALONE-KEY-CUSTODY-20261016. Binary encrypted representation and
// computation only. These caller-observable tuples are NEVER key-release,
// current-catalog-generation, protected-memory or storage/MGA authority.
struct NativeHashKeyBindingV1 {
    platform::Uuid node, index, key, hash_profile, seed_policy, wrapping_root, operation;
    std::uint64_t key_generation = 0, hash_profile_generation = 0;
    std::uint64_t seed_policy_generation = 0, wrapping_root_generation = 0;
    bool operator==(const NativeHashKeyBindingV1& other) const noexcept;
};
inline constexpr std::size_t kNativeHashKeyEnvelopeBytesV1 = 288;
using NativeHashKeyEnvelopeV1 = std::array<std::uint8_t, kNativeHashKeyEnvelopeBytesV1>;
struct NativeHashKeyInputV1 { const std::uint8_t* data = nullptr; std::size_t size = 0; };
struct NativeHashKeyOutputV1 { std::uint8_t* data = nullptr; std::size_t size = 0; };
struct NativeHashKeyCancellationV1 {
    bool (*requested)(void*) = nullptr;
    void* context = nullptr;
};
enum class NativeHashKeyEnvelopeErrorV1 : std::uint8_t {
    none, invalid_binding, invalid_extent, overlap, malformed, binding_mismatch,
    entropy_failure, provider_failure, authentication_failed, cancelled, memory_scope_unavailable
};

bool ValidNativeHashKeyBindingV1(const NativeHashKeyBindingV1&) noexcept;
// Structural validation against an expected tuple only; no key or plaintext
// is read/returned and a successful inspection is NOT authentication.
NativeHashKeyEnvelopeErrorV1 InspectNativeHashKeyEnvelopeV1(
    const NativeHashKeyBindingV1&, NativeHashKeyInputV1) noexcept;

// Wrapping root and routing key are exactly32 bytes. Output exactly288 bytes.
// Fresh salt/nonce come only from the actual Core cryptographic RNG, never a
// caller entropy override. Backend work requires an explicit live allocating
// crypto scope bound to binding.node; no process-default or foreign fallback.
// Inputs/tuple remain immutable and alive through all synchronous callbacks.
// Output is exclusive private staging, disjoint from tuple and all inputs.
// Bad extents/overlap leave output unchanged. After output admission every
// failure/exception/cancellation scrubs the whole output. Callback exceptions
// propagate only after erasure. Neither call creates a grant, key lease,
// source permission, persistent generation, publication or finality receipt.
NativeHashKeyEnvelopeErrorV1 SealNativeHashKeyEnvelopeV1(
    const NativeHashKeyBindingV1&, NativeHashKeyInputV1 wrapping_root,
    NativeHashKeyInputV1 routing_key, NativeHashKeyOutputV1 envelope,
    NativeHashKeyCancellationV1 = {});

// Root exactly32, immutable envelope exactly288, private plaintext output
// exactly32. The expected tuple comes from separately retained real admission.
// No plaintext is usable until full authentication AND final probe succeed.
// Derived keys and provider state are erased/released on every admitted exit;
// owner retains protected staging through its own publication/release fence.
NativeHashKeyEnvelopeErrorV1 OpenNativeHashKeyEnvelopeV1(
    const NativeHashKeyBindingV1&, NativeHashKeyInputV1 wrapping_root,
    NativeHashKeyInputV1 envelope, NativeHashKeyOutputV1 routing_key,
    NativeHashKeyCancellationV1 = {});

}  // namespace scratchbird::core::crypto
