// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace scratchbird::core::hash {

using Blake3Digest = std::array<std::uint8_t, 32>;

// Existing source-compatible one-shot API. The implementation is routed through
// the bounded, allocation-free V3 incremental API below.
[[nodiscard]] Blake3Digest ComputeBlake3Digest(
    const std::vector<std::uint8_t>& input) noexcept;

enum class Blake3IncrementalStatusV3 : std::uint8_t {
    kOk = 0,
    kNullState,
    kNullInput,
    kNullDigest,
    kStateOverlap,
    kInitializationRequired,
    kFailedState,
    kInvalidState,
    kLengthOverflow,
    kAlreadyFinalized,
    kInternalCapacityExceeded,
};

enum class Blake3IncrementalLifecycleV3 : std::uint8_t {
    kInvalid = 0,
    kActive = 1,
    kFinalized = 2,
    kFailed = 3,
};

inline constexpr std::uint64_t kBlake3IncrementalMaximumInputBytesV3 =
    std::numeric_limits<std::uint64_t>::max();
inline constexpr std::size_t kBlake3IncrementalCvStackCapacityV3 = 54;
inline constexpr std::uint32_t kBlake3IncrementalStateMagicV3 = 0x334b4c42U;
inline constexpr std::uint16_t kBlake3IncrementalStateAbiVersionV3 = 3;

// A deliberately concrete, fixed-size state. It is trivially copyable so that
// callers may place it in fixed arenas, but copied active states represent
// independent hash branches and must each be finalized or scrubbed separately.
// Callers must treat all fields as read-only implementation state.
struct alignas(8) Blake3IncrementalStateV3 final {
    std::uint32_t magic = 0;
    std::uint16_t abi_version = 0;
    Blake3IncrementalLifecycleV3 lifecycle =
        Blake3IncrementalLifecycleV3::kInvalid;
    std::uint8_t block_length = 0;
    std::uint8_t blocks_compressed = 0;
    std::uint8_t cv_stack_length = 0;
    std::array<std::uint8_t, 6> reserved{};

    std::uint64_t total_input_bytes = 0;
    std::uint64_t current_chunk_counter = 0;
    std::array<std::uint32_t, 8> current_chunk_chaining_value{};
    std::array<std::uint8_t, 64> current_block{};
    std::array<std::array<std::uint32_t, 8>,
               kBlake3IncrementalCvStackCapacityV3>
        cv_stack{};
};

static_assert(std::is_standard_layout_v<Blake3IncrementalStateV3>);
static_assert(std::is_trivially_copyable_v<Blake3IncrementalStateV3>);
static_assert(sizeof(Blake3IncrementalStatusV3) == 1);
static_assert(sizeof(Blake3IncrementalLifecycleV3) == 1);
static_assert(sizeof(Blake3IncrementalStateV3) == 1856);
static_assert(alignof(Blake3IncrementalStateV3) == 8);
static_assert(offsetof(Blake3IncrementalStateV3, magic) == 0);
static_assert(offsetof(Blake3IncrementalStateV3, abi_version) == 4);
static_assert(offsetof(Blake3IncrementalStateV3, lifecycle) == 6);
static_assert(offsetof(Blake3IncrementalStateV3, block_length) == 7);
static_assert(offsetof(Blake3IncrementalStateV3, blocks_compressed) == 8);
static_assert(offsetof(Blake3IncrementalStateV3, cv_stack_length) == 9);
static_assert(offsetof(Blake3IncrementalStateV3, reserved) == 10);
static_assert(offsetof(Blake3IncrementalStateV3, total_input_bytes) == 16);
static_assert(offsetof(Blake3IncrementalStateV3, current_chunk_counter) == 24);
static_assert(offsetof(Blake3IncrementalStateV3,
                       current_chunk_chaining_value) == 32);
static_assert(offsetof(Blake3IncrementalStateV3, current_block) == 64);
static_assert(offsetof(Blake3IncrementalStateV3, cv_stack) == 128);

// Initialize is legal only for all-zero (uninitialized) storage. Calling it on
// active or finalized storage transitions the state to failed. Calling it on a
// failed state reports kFailedState without mutation. Recovery is scrub, then
// initialize. A null state is reported and not dereferenced.
[[nodiscard]] Blake3IncrementalStatusV3 InitializeBlake3IncrementalV3(
    Blake3IncrementalStateV3* state) noexcept;

// A zero-length update is a successful no-op and accepts input == nullptr.
// A nonzero update requires non-null input and must not overlap any state byte.
// An overlap is a pre-admission pointer-contract refusal and changes neither
// range. An address extent that would wrap uintptr_t is conservatively refused
// as overlap before any read. Every other invalid admitted call scrubs and
// transitions state to failed. Length overflow is checked before any input byte
// is read.
[[nodiscard]] Blake3IncrementalStatusV3 UpdateBlake3IncrementalV3(
    Blake3IncrementalStateV3* state,
    const std::uint8_t* input,
    std::size_t input_size) noexcept;

// On success, writes exactly 32 digest bytes, securely clears all buffered and
// tree state, and leaves only a finalized lifecycle marker. digest_out must not
// overlap any state byte; an address extent that would wrap uintptr_t is also
// refused as overlap. An overlap changes neither range. Every other failure
// leaves a disjoint digest_out unchanged, scrubs the state, and marks it failed.
// Finalize may succeed exactly once per initialization.
[[nodiscard]] Blake3IncrementalStatusV3 FinalizeBlake3IncrementalV3(
    Blake3IncrementalStateV3* state,
    Blake3Digest* digest_out) noexcept;

// Securely clears the complete object to the uninitialized state. Scrub is the
// only legal call after failure. Scrubbing nullptr is an intentional no-op.
void ScrubBlake3IncrementalV3(Blake3IncrementalStateV3* state) noexcept;

using Blake3KeyV1 = std::array<std::uint8_t, 32>;
using Blake3Digest128V1 = std::array<std::uint8_t, 16>;

// Bounded keyed-mode computation only; this class does not generate keys or
// establish custody, authorization, storage admission or equality. The caller
// supplies a valid disjoint 32-byte key and charges this entire fixed object to
// its actual memory owner. The original key remains the caller's responsibility.
// No allocation, implicit copies or moves. Secret-derived state is erased on
// finalization, admitted failure and destruction (including unfinished work).
// Overlap refusals change neither range, as in V3. Finalize writes exactly the
// first 16 BLAKE3 output bytes, once, and leaves disjoint output unchanged on
// error. Unkeyed V3's ABI, lifecycle and outputs remain unchanged.
class alignas(8) Blake3KeyedHasherV1 final {
public:
    explicit Blake3KeyedHasherV1(const Blake3KeyV1& key) noexcept;
    ~Blake3KeyedHasherV1() noexcept;
    Blake3KeyedHasherV1(const Blake3KeyedHasherV1&) = delete;
    Blake3KeyedHasherV1& operator=(const Blake3KeyedHasherV1&) = delete;
    Blake3KeyedHasherV1(Blake3KeyedHasherV1&&) = delete;
    Blake3KeyedHasherV1& operator=(Blake3KeyedHasherV1&&) = delete;

    [[nodiscard]] Blake3IncrementalStatusV3 Update(
        const std::uint8_t* input, std::size_t input_size) noexcept;
    [[nodiscard]] Blake3IncrementalStatusV3 Finalize(
        Blake3Digest128V1* digest_out) noexcept;

private:
    Blake3IncrementalStateV3 state_{};
    std::array<std::uint32_t, 8> key_words_{};
};

static_assert(sizeof(Blake3KeyedHasherV1) == 1888);
static_assert(alignof(Blake3KeyedHasherV1) == 8);
static_assert(!std::is_copy_constructible_v<Blake3KeyedHasherV1>);
static_assert(!std::is_move_constructible_v<Blake3KeyedHasherV1>);

}  // namespace scratchbird::core::hash
