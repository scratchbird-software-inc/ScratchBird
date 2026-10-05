// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "blake3_digest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* result = std::malloc(size == 0 ? 1 : size)) {
        return result;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {

namespace h = scratchbird::core::hash;

std::uint64_t g_checks = 0;
std::uint64_t g_failures = 0;

void Check(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        if (g_failures <= 30) {
            std::cerr << "FAIL: " << message << '\n';
        }
    }
}

[[nodiscard]] std::uint8_t Nibble(char value) {
    return static_cast<std::uint8_t>(value <= '9' ? value - '0'
                                                  : value - 'a' + 10);
}

[[nodiscard]] h::Blake3Digest DecodeDigest(const char* hex) {
    h::Blake3Digest digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        digest[index] = static_cast<std::uint8_t>(
            (Nibble(hex[index * 2]) << 4U) | Nibble(hex[index * 2 + 1]));
    }
    return digest;
}

[[nodiscard]] h::Blake3Digest IncrementalFixedStep(
    const std::vector<std::uint8_t>& input,
    std::size_t step,
    bool* statuses_ok = nullptr) {
    h::Blake3IncrementalStateV3 state{};
    bool ok = h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kOk;
    ok = ok && h::UpdateBlake3IncrementalV3(&state, nullptr, 0) ==
                   h::Blake3IncrementalStatusV3::kOk;
    std::size_t offset = 0;
    while (offset < input.size()) {
        const std::size_t take = std::min(step, input.size() - offset);
        ok = ok && h::UpdateBlake3IncrementalV3(
                       &state, input.data() + offset, take) ==
                       h::Blake3IncrementalStatusV3::kOk;
        ok = ok && h::UpdateBlake3IncrementalV3(&state, nullptr, 0) ==
                       h::Blake3IncrementalStatusV3::kOk;
        offset += take;
    }
    h::Blake3Digest digest{};
    ok = ok && h::FinalizeBlake3IncrementalV3(&state, &digest) ==
                   h::Blake3IncrementalStatusV3::kOk;
    if (statuses_ok != nullptr) {
        *statuses_ok = ok;
    }
    return digest;
}

[[nodiscard]] h::Blake3Digest IncrementalSplit(
    const std::vector<std::uint8_t>& input,
    std::size_t split,
    bool* statuses_ok = nullptr) {
    h::Blake3IncrementalStateV3 state{};
    bool ok = h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kOk;
    ok = ok && h::UpdateBlake3IncrementalV3(
                   &state, input.data(), split) ==
                   h::Blake3IncrementalStatusV3::kOk;
    ok = ok && h::UpdateBlake3IncrementalV3(&state, nullptr, 0) ==
                   h::Blake3IncrementalStatusV3::kOk;
    ok = ok && h::UpdateBlake3IncrementalV3(
                   &state, input.data() + split, input.size() - split) ==
                   h::Blake3IncrementalStatusV3::kOk;
    h::Blake3Digest digest{};
    ok = ok && h::FinalizeBlake3IncrementalV3(&state, &digest) ==
                   h::Blake3IncrementalStatusV3::kOk;
    if (statuses_ok != nullptr) {
        *statuses_ok = ok;
    }
    return digest;
}

void OfficialKnownAnswersAndSegmentation() {
    struct Known final {
        std::size_t size;
        const char* hex;
    };
    // BLAKE3 upstream test_vectors.json. Message byte i is i modulo 251;
    // these are the unkeyed 256-bit results already used by ScratchBird.
    constexpr Known cases[] = {
        {0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
        {1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
        {63, "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b"},
        {64, "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98"},
        {65, "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee"},
        {127, "d81293fda863f008c09e92fc382a81f5a0b4a1251cba1634016a0f86a6bd640d"},
        {128, "f17e570564b26578c33bb7f44643f539624b05df1a76c81f30acd548c44b45ef"},
        {129, "683aaae9f3c5ba37eaaf072aed0f9e30bac0865137bae68b1fde4ca2aebdcb12"},
        {1023, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
        {1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
        {1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
        {2048, "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
        {2049, "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030"},
        {3072, "b98cb0ff3623be03326b373de6b9095218513e64f1ee2edd2525c7ad1e5cffd2"},
        {3073, "7124b49501012f81cc7f11ca069ec9226cecb8a2c850cfe644e327d22d3e1cd3"},
        {4096, "015094013f57a5277b59d8475c0501042c0b642e531b0a1c8f58d2163229e969"},
        {4097, "9b4052b38f1c5fc8b1f9ff7ac7b27cd242487b3d890d15c96a1c25b8aa0fb995"},
        {5120, "9cadc15fed8b5d854562b26a9536d9707cadeda9b143978f319ab34230535833"},
        {5121, "628bd2cb2004694adaab7bbd778a25df25c47b9d4155a55f8fbd79f2fe154cff"},
        {6144, "3e2e5b74e048f3add6d21faab3f83aa44d3b2278afb83b80b3c35164ebeca205"},
        {6145, "f1323a8631446cc50536a9f705ee5cb619424d46887f3c376c695b70e0f0507f"},
        {7168, "61da957ec2499a95d6b8023e2b0e604ec7f6b50e80a9678b89d2628e99ada77a"},
        {7169, "a003fc7a51754a9b3c7fae0367ab3d782dccf28855a03d435f8cfe74605e7817"},
        {8192, "aae792484c8efe4f19e2ca7d371d8c467ffb10748d8a5a1ae579948f718a2a63"},
        {8193, "bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b"},
        {16384, "f875d6646de28985646f34ee13be9a576fd515f76b5b0a26bb324735041ddde4"},
        {31744, "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47"},
        {102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"},
    };
    constexpr std::size_t steps[] = {
        1, 2, 3, 7, 31, 63, 64, 65, 127, 128, 129,
        511, 1023, 1024, 1025, 4096,
    };

    for (const Known& known : cases) {
        std::vector<std::uint8_t> input(known.size);
        for (std::size_t index = 0; index < input.size(); ++index) {
            input[index] = static_cast<std::uint8_t>(index % 251U);
        }
        const std::vector<std::uint8_t> saved = input;
        const h::Blake3Digest expected = DecodeDigest(known.hex);

        const std::uint64_t allocations_before =
            g_allocations.load(std::memory_order_relaxed);
        const h::Blake3Digest one_shot = h::ComputeBlake3Digest(input);
        const std::uint64_t allocations_after =
            g_allocations.load(std::memory_order_relaxed);
        Check(one_shot == expected, "one-shot digest differs from official vector");
        Check(allocations_before == allocations_after,
              "one-shot digest allocated memory");
        Check(input == saved, "one-shot digest changed caller input");

        for (std::size_t step : steps) {
            bool statuses_ok = false;
            const std::uint64_t incremental_allocations_before =
                g_allocations.load(std::memory_order_relaxed);
            const h::Blake3Digest actual =
                IncrementalFixedStep(input, step, &statuses_ok);
            const std::uint64_t incremental_allocations_after =
                g_allocations.load(std::memory_order_relaxed);
            Check(statuses_ok, "incremental fixed-step call returned failure");
            Check(actual == expected,
                  "incremental fixed-step digest differs from official vector");
            Check(incremental_allocations_before ==
                      incremental_allocations_after,
                  "incremental fixed-step digest allocated memory");
        }

        if (known.size <= 4097) {
            for (std::size_t split = 0; split <= known.size; ++split) {
                bool statuses_ok = false;
                const h::Blake3Digest actual =
                    IncrementalSplit(input, split, &statuses_ok);
                Check(statuses_ok,
                      "two-segment exhaustive split returned failure");
                Check(actual == expected,
                      "two-segment exhaustive split changed digest");
            }
        }
    }
}

void LifecycleAndScrub() {
    h::Blake3IncrementalStateV3 state{};
    h::Blake3Digest output{};
    output.fill(0xa5);
    const h::Blake3Digest sentinel = output;

    Check(h::InitializeBlake3IncrementalV3(nullptr) ==
              h::Blake3IncrementalStatusV3::kNullState,
          "initialize accepted null state");
    Check(h::UpdateBlake3IncrementalV3(nullptr, nullptr, 0) ==
              h::Blake3IncrementalStatusV3::kNullState,
          "update accepted null state");
    Check(h::FinalizeBlake3IncrementalV3(nullptr, &output) ==
              h::Blake3IncrementalStatusV3::kNullState &&
              output == sentinel,
          "finalize null-state changed output");

    Check(h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "initialize failed");
    Check(h::UpdateBlake3IncrementalV3(&state, nullptr, 0) ==
              h::Blake3IncrementalStatusV3::kOk,
          "zero update with null input failed");
    Check(h::FinalizeBlake3IncrementalV3(&state, &output) ==
              h::Blake3IncrementalStatusV3::kOk &&
              output == DecodeDigest(
                            "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"),
          "empty incremental digest is wrong");

    h::Blake3IncrementalStateV3 expected_finalized{};
    expected_finalized.magic = h::kBlake3IncrementalStateMagicV3;
    expected_finalized.abi_version =
        h::kBlake3IncrementalStateAbiVersionV3;
    expected_finalized.lifecycle =
        h::Blake3IncrementalLifecycleV3::kFinalized;
    Check(std::memcmp(&state, &expected_finalized, sizeof(state)) == 0,
          "successful finalize retained buffered/tree material");

    h::Blake3Digest second_output{};
    second_output.fill(0x3c);
    const h::Blake3Digest second_sentinel = second_output;
    const std::uint8_t byte = 7;
    Check(h::UpdateBlake3IncrementalV3(&state, &byte, 1) ==
              h::Blake3IncrementalStatusV3::kAlreadyFinalized,
          "update after finalize was not refused");
    Check(h::FinalizeBlake3IncrementalV3(&state, &second_output) ==
              h::Blake3IncrementalStatusV3::kFailedState &&
              second_output == second_sentinel,
          "call after terminal-call failure published or changed a digest");
    Check(state.lifecycle == h::Blake3IncrementalLifecycleV3::kFailed,
          "invalid post-finalize call did not poison the state");
    Check(h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kFailedState,
          "initialize illegally recovered a failed state without scrub");

    h::ScrubBlake3IncrementalV3(&state);
    const std::array<std::uint8_t, sizeof(state)> zero{};
    Check(std::memcmp(&state, zero.data(), sizeof(state)) == 0,
          "explicit scrub did not clear the complete state");
    Check(h::UpdateBlake3IncrementalV3(&state, nullptr, 0) ==
              h::Blake3IncrementalStatusV3::kInitializationRequired,
          "scrubbed state remained usable without initialize");
    h::ScrubBlake3IncrementalV3(nullptr);
    h::ScrubBlake3IncrementalV3(&state);
    Check(h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "scrubbed state could not be initialized again");

    // Invalid admitted arguments fail closed and scrub active material.
    Check(h::UpdateBlake3IncrementalV3(&state, &byte, 1) ==
              h::Blake3IncrementalStatusV3::kOk,
          "invalid-argument setup update failed");
    Check(h::UpdateBlake3IncrementalV3(&state, nullptr, 1) ==
              h::Blake3IncrementalStatusV3::kNullInput &&
              state.lifecycle == h::Blake3IncrementalLifecycleV3::kFailed,
          "null input did not fail and poison active state");
    Check(h::UpdateBlake3IncrementalV3(&state, &byte, 1) ==
              h::Blake3IncrementalStatusV3::kFailedState,
          "failed state accepted update");
    h::ScrubBlake3IncrementalV3(&state);
    Check(h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "null-input failure did not recover through scrub/init");
    Check(h::FinalizeBlake3IncrementalV3(&state, nullptr) ==
              h::Blake3IncrementalStatusV3::kNullDigest &&
              state.lifecycle == h::Blake3IncrementalLifecycleV3::kFailed,
          "null digest did not fail and poison active state");
}

void InvalidStateAndOverflow() {
    h::Blake3IncrementalStateV3 clean{};
    Check(h::InitializeBlake3IncrementalV3(&clean) ==
              h::Blake3IncrementalStatusV3::kOk,
          "invalid-state test setup failed");

    auto IsRefused = [](h::Blake3IncrementalStateV3 state) {
        const auto status =
            h::UpdateBlake3IncrementalV3(&state, nullptr, 0);
        return status == h::Blake3IncrementalStatusV3::kInvalidState &&
               state.lifecycle == h::Blake3IncrementalLifecycleV3::kFailed &&
               state.total_input_bytes == 0 &&
               state.current_chunk_counter == 0;
    };

    auto changed = clean;
    changed.magic ^= 1U;
    Check(IsRefused(changed), "bad magic was accepted");
    changed = clean;
    ++changed.abi_version;
    Check(IsRefused(changed), "bad ABI version was accepted");
    changed = clean;
    changed.lifecycle = h::Blake3IncrementalLifecycleV3::kInvalid;
    Check(IsRefused(changed), "invalid lifecycle was accepted");
    changed = clean;
    changed.reserved[0] = 1;
    Check(IsRefused(changed), "nonzero reserved byte was accepted");
    changed = clean;
    changed.block_length = 65;
    Check(IsRefused(changed), "oversize block length was accepted");
    changed = clean;
    changed.blocks_compressed = 16;
    Check(IsRefused(changed), "oversize compressed-block count was accepted");
    changed = clean;
    changed.cv_stack_length = 55;
    Check(IsRefused(changed), "oversize CV stack count was accepted");
    changed = clean;
    changed.total_input_bytes = 1;
    Check(IsRefused(changed), "inconsistent total length was accepted");
    changed = clean;
    changed.current_chunk_counter = 1;
    changed.total_input_bytes = 1025;
    changed.block_length = 1;
    Check(IsRefused(changed), "inconsistent stack population was accepted");
    changed = clean;
    changed.blocks_compressed = 1;
    changed.total_input_bytes = 64;
    Check(IsRefused(changed),
          "alternate exact-block representation with no buffered block was accepted");
    changed = clean;
    changed.blocks_compressed = 15;
    changed.total_input_bytes = 960;
    Check(IsRefused(changed),
          "fifteen compressed blocks with no retained final block were accepted");
    changed = clean;
    changed.current_chunk_counter = 1;
    changed.cv_stack_length = 1;
    changed.total_input_bytes = 1024;
    Check(IsRefused(changed),
          "completed-chunk stack plus empty current chunk was accepted");
    changed = clean;
    changed.current_chunk_chaining_value[0] ^= 1U;
    Check(IsRefused(changed),
          "non-IV empty current chunk chaining value was accepted");
    changed = clean;
    changed.block_length = 1;
    changed.total_input_bytes = 1;
    changed.current_block[0] = 0x5a;
    changed.current_chunk_chaining_value[7] ^= 1U;
    Check(IsRefused(changed),
          "non-IV uncompressed partial chunk chaining value was accepted");
    changed = clean;
    changed.current_block[0] = 1;
    Check(IsRefused(changed), "dirty unused block tail was accepted");
    changed = clean;
    changed.cv_stack[0][0] = 1;
    Check(IsRefused(changed), "dirty unused CV slot was accepted");

    h::Blake3IncrementalStateV3 maximum{};
    Check(h::InitializeBlake3IncrementalV3(&maximum) ==
              h::Blake3IncrementalStatusV3::kOk,
          "maximum-state setup initialization failed");
    maximum.total_input_bytes =
        h::kBlake3IncrementalMaximumInputBytesV3;
    maximum.current_chunk_counter =
        h::kBlake3IncrementalMaximumInputBytesV3 / 1024U;
    maximum.blocks_compressed = 15;
    maximum.block_length = 63;
    maximum.cv_stack_length = 53;
    Check(h::UpdateBlake3IncrementalV3(&maximum, nullptr, 0) ==
              h::Blake3IncrementalStatusV3::kOk,
          "structurally valid INT64_MAX state was rejected");
    const std::uint8_t byte = 0;
    Check(h::UpdateBlake3IncrementalV3(&maximum, &byte, 1) ==
              h::Blake3IncrementalStatusV3::kLengthOverflow,
          "INT64_MAX plus one byte did not report length overflow");
    Check(maximum.lifecycle == h::Blake3IncrementalLifecycleV3::kFailed &&
              maximum.total_input_bytes == 0 &&
              maximum.current_chunk_counter == 0,
          "length-overflow refusal did not scrub and poison state");
}

void PointerAliasingAndTransitions() {
    h::Blake3IncrementalStateV3 state{};
    Check(h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "alias test initialization failed");

    const h::Blake3IncrementalStateV3 before_input_alias = state;
    const auto* aliased_input = reinterpret_cast<const std::uint8_t*>(&state) + 1;
    Check(h::UpdateBlake3IncrementalV3(&state, aliased_input, 1) ==
                  h::Blake3IncrementalStatusV3::kStateOverlap &&
              std::memcmp(&state, &before_input_alias, sizeof(state)) == 0,
          "input/state overlap was read or changed state");

    const h::Blake3IncrementalStateV3 before_output_alias = state;
    auto* aliased_digest = reinterpret_cast<h::Blake3Digest*>(
        state.current_block.data());
    Check(h::FinalizeBlake3IncrementalV3(&state, aliased_digest) ==
                  h::Blake3IncrementalStatusV3::kStateOverlap &&
              std::memcmp(&state, &before_output_alias, sizeof(state)) == 0,
          "digest/state overlap changed aliased bytes or lifecycle");

    h::Blake3Digest digest{};
    Check(h::FinalizeBlake3IncrementalV3(&state, &digest) ==
              h::Blake3IncrementalStatusV3::kOk,
          "overlap preflight incorrectly consumed active state");

    Check(h::InitializeBlake3IncrementalV3(&state) ==
                  h::Blake3IncrementalStatusV3::kAlreadyFinalized &&
              state.lifecycle == h::Blake3IncrementalLifecycleV3::kFailed,
          "initialize on finalized state did not fail closed");
    Check(h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kFailedState,
          "failed state accepted initialize");
    h::ScrubBlake3IncrementalV3(&state);
    Check(h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "scrub plus initialize did not recover terminal failure");
    Check(h::InitializeBlake3IncrementalV3(&state) ==
                  h::Blake3IncrementalStatusV3::kInvalidState &&
              state.lifecycle == h::Blake3IncrementalLifecycleV3::kFailed,
          "initialize on active state did not fail closed");

    h::ScrubBlake3IncrementalV3(&state);
    h::Blake3Digest unchanged{};
    unchanged.fill(0x6d);
    const h::Blake3Digest sentinel = unchanged;
    Check(h::FinalizeBlake3IncrementalV3(&state, &unchanged) ==
                  h::Blake3IncrementalStatusV3::kInitializationRequired &&
              unchanged == sentinel &&
              state.lifecycle == h::Blake3IncrementalLifecycleV3::kFailed,
          "finalize-before-initialize changed destination or did not fail state");
}

void RangeBoundaryAndWrapClassification() {
    constexpr std::size_t kMargin = 64;
    alignas(h::Blake3IncrementalStateV3)
        std::array<std::uint8_t,
                   sizeof(h::Blake3IncrementalStateV3) + (2U * kMargin)>
            storage{};

    const auto reconstruct = [&storage]() {
        storage.fill(0);
        return ::new (static_cast<void*>(storage.data() + kMargin))
            h::Blake3IncrementalStateV3{};
    };
    auto* const enclosing_bytes = storage.data();
    auto* const state_start = enclosing_bytes + kMargin;
    auto* const state_end = state_start + sizeof(h::Blake3IncrementalStateV3);
    auto* state = reconstruct();
    Check(h::InitializeBlake3IncrementalV3(state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "range-boundary left-adjacency setup failed");
    *(state_start - 1U) = 0x31;
    Check(h::UpdateBlake3IncrementalV3(state, state_start - 1U, 1) ==
              h::Blake3IncrementalStatusV3::kOk,
          "input immediately left-adjacent to state was treated as overlap");
    h::Blake3Digest actual{};
    Check(h::FinalizeBlake3IncrementalV3(state, &actual) ==
              h::Blake3IncrementalStatusV3::kOk,
          "left-adjacent input state did not finalize");
    Check(actual == h::ComputeBlake3Digest(std::vector<std::uint8_t>{0x31}),
          "left-adjacent input was not read exactly");

    state = reconstruct();
    Check(h::InitializeBlake3IncrementalV3(state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "range-boundary right-adjacency setup failed");
    *state_end = 0x7b;
    Check(h::UpdateBlake3IncrementalV3(state, state_end, 1) ==
                  h::Blake3IncrementalStatusV3::kOk,
          "input immediately right-adjacent to state was treated as overlap");
    Check(h::FinalizeBlake3IncrementalV3(state, &actual) ==
                  h::Blake3IncrementalStatusV3::kOk &&
              actual ==
                  h::ComputeBlake3Digest(std::vector<std::uint8_t>{0x7b}),
          "right-adjacent input was not read exactly");

    state = reconstruct();
    Check(h::InitializeBlake3IncrementalV3(state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "partial-overlap setup failed");
    const h::Blake3IncrementalStateV3 clean = *state;
    Check(h::UpdateBlake3IncrementalV3(state, state_start - 1U, 2) ==
                  h::Blake3IncrementalStatusV3::kStateOverlap &&
              std::memcmp(state, &clean, sizeof(*state)) == 0,
          "input overlapping the state start by one byte was not refused atomically");
    Check(h::UpdateBlake3IncrementalV3(state, state_end - 1U, 2) ==
                  h::Blake3IncrementalStatusV3::kStateOverlap &&
              std::memcmp(state, &clean, sizeof(*state)) == 0,
          "input overlapping the state end by one byte was not refused atomically");

    const auto wrapped_input = reinterpret_cast<const std::uint8_t*>(
        std::numeric_limits<std::uintptr_t>::max() - 3U);
    Check(h::UpdateBlake3IncrementalV3(state, wrapped_input, 8) ==
                  h::Blake3IncrementalStatusV3::kStateOverlap &&
              std::memcmp(state, &clean, sizeof(*state)) == 0,
          "uintptr-wrapping input extent was not conservatively refused as overlap");

    auto* left_adjacent_digest = ::new (static_cast<void*>(
        state_start - sizeof(h::Blake3Digest))) h::Blake3Digest{};
    Check(h::FinalizeBlake3IncrementalV3(state, left_adjacent_digest) ==
              h::Blake3IncrementalStatusV3::kOk,
          "digest immediately left-adjacent to state was treated as overlap");
    Check(*left_adjacent_digest == h::ComputeBlake3Digest({}),
          "left-adjacent digest destination received wrong bytes");

    state = reconstruct();
    Check(h::InitializeBlake3IncrementalV3(state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "right-adjacent digest setup failed");
    auto* right_adjacent_digest = ::new (static_cast<void*>(
        state_end)) h::Blake3Digest{};
    Check(h::FinalizeBlake3IncrementalV3(state, right_adjacent_digest) ==
                  h::Blake3IncrementalStatusV3::kOk &&
              *right_adjacent_digest == h::ComputeBlake3Digest({}),
          "digest immediately right-adjacent to state failed");

    state = reconstruct();
    Check(h::InitializeBlake3IncrementalV3(state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "partial digest-overlap setup failed");
    const h::Blake3IncrementalStateV3 before_digest_overlap = *state;
    auto* overlaps_start = reinterpret_cast<h::Blake3Digest*>(
        state_start - (sizeof(h::Blake3Digest) - 1U));
    Check(h::FinalizeBlake3IncrementalV3(state, overlaps_start) ==
                  h::Blake3IncrementalStatusV3::kStateOverlap &&
              std::memcmp(state, &before_digest_overlap, sizeof(*state)) == 0,
          "digest overlapping state start by one byte changed state");
    auto* overlaps_end = reinterpret_cast<h::Blake3Digest*>(
        state_end - 1U);
    Check(h::FinalizeBlake3IncrementalV3(state, overlaps_end) ==
                  h::Blake3IncrementalStatusV3::kStateOverlap &&
              std::memcmp(state, &before_digest_overlap, sizeof(*state)) == 0,
          "digest overlapping state end by one byte changed state");

    auto* wrapped_digest = reinterpret_cast<h::Blake3Digest*>(
        std::numeric_limits<std::uintptr_t>::max() - 15U);
    Check(h::FinalizeBlake3IncrementalV3(state, wrapped_digest) ==
                  h::Blake3IncrementalStatusV3::kStateOverlap &&
              std::memcmp(state, &before_digest_overlap, sizeof(*state)) == 0,
          "uintptr-wrapping digest extent was not conservatively refused as overlap");
    h::ScrubBlake3IncrementalV3(state);
}

void BranchingAndIrregularSegments() {
    std::vector<std::uint8_t> prefix(1500);
    std::vector<std::uint8_t> left_suffix(7000);
    std::vector<std::uint8_t> right_suffix(7001);
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        prefix[index] = static_cast<std::uint8_t>((index * 17U) & 0xffU);
    }
    for (std::size_t index = 0; index < left_suffix.size(); ++index) {
        left_suffix[index] = static_cast<std::uint8_t>((index * 29U) & 0xffU);
    }
    right_suffix = left_suffix;
    right_suffix.push_back(0x5a);

    h::Blake3IncrementalStateV3 state{};
    Check(h::InitializeBlake3IncrementalV3(&state) ==
                  h::Blake3IncrementalStatusV3::kOk &&
              h::UpdateBlake3IncrementalV3(
                  &state, prefix.data(), prefix.size()) ==
                  h::Blake3IncrementalStatusV3::kOk,
          "branch prefix setup failed");
    h::Blake3IncrementalStateV3 branch = state;
    Check(h::UpdateBlake3IncrementalV3(
              &state, left_suffix.data(), left_suffix.size()) ==
              h::Blake3IncrementalStatusV3::kOk,
          "left branch update failed");
    Check(h::UpdateBlake3IncrementalV3(
              &branch, right_suffix.data(), right_suffix.size()) ==
              h::Blake3IncrementalStatusV3::kOk,
          "right branch update failed");
    h::Blake3Digest left{};
    h::Blake3Digest right{};
    Check(h::FinalizeBlake3IncrementalV3(&state, &left) ==
                  h::Blake3IncrementalStatusV3::kOk &&
              h::FinalizeBlake3IncrementalV3(&branch, &right) ==
                  h::Blake3IncrementalStatusV3::kOk,
          "branch finalize failed");

    std::vector<std::uint8_t> left_all = prefix;
    left_all.insert(left_all.end(), left_suffix.begin(), left_suffix.end());
    std::vector<std::uint8_t> right_all = prefix;
    right_all.insert(right_all.end(), right_suffix.begin(), right_suffix.end());
    Check(left == h::ComputeBlake3Digest(left_all),
          "copied-state left branch differs from one-shot");
    Check(right == h::ComputeBlake3Digest(right_all),
          "copied-state right branch differs from one-shot");
    Check(left != right, "divergent copied states produced equal digests");

    std::vector<std::uint8_t> irregular(100000);
    for (std::size_t index = 0; index < irregular.size(); ++index) {
        irregular[index] = static_cast<std::uint8_t>((index * 131U + 7U) & 0xffU);
    }
    const h::Blake3Digest expected = h::ComputeBlake3Digest(irregular);
    h::ScrubBlake3IncrementalV3(&state);
    Check(h::InitializeBlake3IncrementalV3(&state) ==
              h::Blake3IncrementalStatusV3::kOk,
          "irregular setup failed");
    std::uint32_t generator = 0x12345678U;
    std::size_t offset = 0;
    while (offset < irregular.size()) {
        generator = (generator * 1664525U) + 1013904223U;
        const std::size_t proposed = (generator % 3000U) + 1U;
        const std::size_t take =
            std::min(proposed, irregular.size() - offset);
        Check(h::UpdateBlake3IncrementalV3(
                  &state, irregular.data() + offset, take) ==
                  h::Blake3IncrementalStatusV3::kOk,
              "irregular segmented update failed");
        offset += take;
    }
    h::Blake3Digest actual{};
    Check(h::FinalizeBlake3IncrementalV3(&state, &actual) ==
                  h::Blake3IncrementalStatusV3::kOk &&
              actual == expected,
          "irregular segmentation differs from one-shot");
}

}  // namespace

int main() {
    static_assert(sizeof(h::Blake3IncrementalStateV3) == 1856);
    static_assert(h::kBlake3IncrementalCvStackCapacityV3 == 54);
    static_assert((h::kBlake3IncrementalMaximumInputBytesV3 / 1024U) ==
                  9007199254740991ULL);

    OfficialKnownAnswersAndSegmentation();
    LifecycleAndScrub();
    InvalidStateAndOverflow();
    PointerAliasingAndTransitions();
    RangeBoundaryAndWrapClassification();
    BranchingAndIrregularSegments();

    std::cout << "checks=" << g_checks << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
