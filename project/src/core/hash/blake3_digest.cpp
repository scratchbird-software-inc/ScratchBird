// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "blake3_digest.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace scratchbird::core::hash {
namespace {

using Word = std::uint32_t;
using Chain = std::array<Word, 8>;
using Block = std::array<Word, 16>;

constexpr Chain kIv = {
    0x6A09E667U,
    0xBB67AE85U,
    0x3C6EF372U,
    0xA54FF53AU,
    0x510E527FU,
    0x9B05688CU,
    0x1F83D9ABU,
    0x5BE0CD19U,
};

constexpr std::size_t kBlockBytes = 64;
constexpr std::size_t kChunkBytes = 1024;
constexpr Word kChunkStart = 1U;
constexpr Word kChunkEnd = 2U;
constexpr Word kParent = 4U;
constexpr Word kRoot = 8U;

constexpr std::array<std::uint8_t, 16> kMessagePermutation = {
    2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8,
};

struct Output final {
    Chain input_chaining_value{};
    Block block{};
    std::uint64_t counter = 0;
    Word block_length = 0;
    Word flags = 0;
};

void SecureZeroBytes(void* destination, std::size_t size) noexcept {
    auto* bytes = static_cast<volatile std::uint8_t*>(destination);
    while (size != 0) {
        *bytes = 0;
        ++bytes;
        --size;
    }
}

template <typename T>
void SecureZero(T& value) noexcept {
    SecureZeroBytes(&value, sizeof(value));
}

[[nodiscard]] constexpr Word RotateRight(Word value, unsigned int bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

void Mix(std::array<Word, 16>& state,
         std::size_t a,
         std::size_t b,
         std::size_t c,
         std::size_t d,
         Word message_x,
         Word message_y) noexcept {
    state[a] = state[a] + state[b] + message_x;
    state[d] = RotateRight(state[d] ^ state[a], 16U);
    state[c] += state[d];
    state[b] = RotateRight(state[b] ^ state[c], 12U);
    state[a] = state[a] + state[b] + message_y;
    state[d] = RotateRight(state[d] ^ state[a], 8U);
    state[c] += state[d];
    state[b] = RotateRight(state[b] ^ state[c], 7U);
}

void Round(std::array<Word, 16>& state, const Block& message) noexcept {
    Mix(state, 0, 4, 8, 12, message[0], message[1]);
    Mix(state, 1, 5, 9, 13, message[2], message[3]);
    Mix(state, 2, 6, 10, 14, message[4], message[5]);
    Mix(state, 3, 7, 11, 15, message[6], message[7]);
    Mix(state, 0, 5, 10, 15, message[8], message[9]);
    Mix(state, 1, 6, 11, 12, message[10], message[11]);
    Mix(state, 2, 7, 8, 13, message[12], message[13]);
    Mix(state, 3, 4, 9, 14, message[14], message[15]);
}

void Permute(Block& message) noexcept {
    Block original = message;
    for (std::size_t index = 0; index < message.size(); ++index) {
        message[index] = original[kMessagePermutation[index]];
    }
    SecureZero(original);
}

[[nodiscard]] Chain Compress(const Chain& chaining_value,
                             const Block& input_block,
                             std::uint64_t counter,
                             Word block_length,
                             Word flags) noexcept {
    std::array<Word, 16> state{};
    for (std::size_t index = 0; index < chaining_value.size(); ++index) {
        state[index] = chaining_value[index];
    }
    state[8] = kIv[0];
    state[9] = kIv[1];
    state[10] = kIv[2];
    state[11] = kIv[3];
    state[12] = static_cast<Word>(counter);
    state[13] = static_cast<Word>(counter >> 32U);
    state[14] = block_length;
    state[15] = flags;

    Block message = input_block;
    for (std::size_t round_index = 0; round_index < 7; ++round_index) {
        Round(state, message);
        if (round_index != 6) {
            Permute(message);
        }
    }

    Chain output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = state[index] ^ state[index + 8];
    }
    SecureZero(state);
    SecureZero(message);
    return output;
}

[[nodiscard]] Word Load32(const std::uint8_t* bytes) noexcept {
    return static_cast<Word>(bytes[0]) |
           (static_cast<Word>(bytes[1]) << 8U) |
           (static_cast<Word>(bytes[2]) << 16U) |
           (static_cast<Word>(bytes[3]) << 24U);
}

void Store32(Word value, std::uint8_t* output) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

[[nodiscard]] Block LoadBlock(
    const std::array<std::uint8_t, kBlockBytes>& bytes) noexcept {
    Block block{};
    for (std::size_t index = 0; index < block.size(); ++index) {
        block[index] = Load32(bytes.data() + (index * sizeof(Word)));
    }
    return block;
}

[[nodiscard]] Word ChunkStartFlag(
    const Blake3IncrementalStateV3& state) noexcept {
    return state.blocks_compressed == 0 ? kChunkStart : 0U;
}

[[nodiscard]] std::size_t CurrentChunkLength(
    const Blake3IncrementalStateV3& state) noexcept {
    return (static_cast<std::size_t>(state.blocks_compressed) * kBlockBytes) +
           state.block_length;
}

[[nodiscard]] Output CurrentChunkOutput(
    const Blake3IncrementalStateV3& state) noexcept {
    return Output{state.current_chunk_chaining_value,
                  LoadBlock(state.current_block),
                  state.current_chunk_counter,
                  state.block_length,
                  ChunkStartFlag(state) | kChunkEnd};
}

[[nodiscard]] Chain OutputChainingValue(const Output& output) noexcept {
    return Compress(output.input_chaining_value,
                    output.block,
                    output.counter,
                    output.block_length,
                    output.flags);
}

[[nodiscard]] Output ParentOutput(const Chain& left, const Chain& right) noexcept {
    Output output{};
    output.input_chaining_value = kIv;
    std::copy(left.begin(), left.end(), output.block.begin());
    std::copy(right.begin(), right.end(), output.block.begin() + left.size());
    output.counter = 0;
    output.block_length = static_cast<Word>(kBlockBytes);
    output.flags = kParent;
    return output;
}

[[nodiscard]] Chain ParentChainingValue(const Chain& left,
                                        const Chain& right) noexcept {
    Output output = ParentOutput(left, right);
    Chain result = OutputChainingValue(output);
    SecureZero(output);
    return result;
}

[[nodiscard]] Blake3Digest RootDigest(const Output& output) noexcept {
    Chain words = Compress(output.input_chaining_value,
                           output.block,
                           0,
                           output.block_length,
                           output.flags | kRoot);
    Blake3Digest digest{};
    for (std::size_t index = 0; index < words.size(); ++index) {
        Store32(words[index], digest.data() + (index * sizeof(Word)));
    }
    SecureZero(words);
    return digest;
}

[[nodiscard]] std::uint8_t PopulationCount(std::uint64_t value) noexcept {
    std::uint8_t count = 0;
    while (value != 0) {
        count = static_cast<std::uint8_t>(count + (value & 1U));
        value >>= 1U;
    }
    return count;
}

[[nodiscard]] bool IsAllZero(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint8_t combined = 0;
    for (std::size_t index = 0; index < size; ++index) {
        combined = static_cast<std::uint8_t>(combined | bytes[index]);
    }
    return combined == 0;
}

[[nodiscard]] bool RangesOverlap(const void* first,
                                 std::size_t first_size,
                                 const void* second,
                                 std::size_t second_size) noexcept {
    if (first_size == 0 || second_size == 0) {
        return false;
    }
    const std::uintptr_t first_start =
        reinterpret_cast<std::uintptr_t>(first);
    const std::uintptr_t second_start =
        reinterpret_cast<std::uintptr_t>(second);
    constexpr std::uintptr_t kMaximum =
        std::numeric_limits<std::uintptr_t>::max();
    if (first_start > kMaximum - (first_size - 1U) ||
        second_start > kMaximum - (second_size - 1U)) {
        // A valid C++ object range cannot wrap. Refuse conservatively before
        // reading either caller-provided range.
        return true;
    }
    const std::uintptr_t first_end = first_start + first_size - 1U;
    const std::uintptr_t second_end = second_start + second_size - 1U;
    return first_start <= second_end && second_start <= first_end;
}

[[nodiscard]] bool HasValidHeader(
    const Blake3IncrementalStateV3& state) noexcept {
    return state.magic == kBlake3IncrementalStateMagicV3 &&
           state.abi_version == kBlake3IncrementalStateAbiVersionV3;
}

[[nodiscard]] bool IsCleanTerminalState(
    const Blake3IncrementalStateV3& state,
    Blake3IncrementalLifecycleV3 lifecycle) noexcept {
    return HasValidHeader(state) &&
           state.lifecycle == lifecycle &&
           state.block_length == 0 && state.blocks_compressed == 0 &&
           state.cv_stack_length == 0 &&
           IsAllZero(state.reserved.data(), state.reserved.size()) &&
           state.total_input_bytes == 0 && state.current_chunk_counter == 0 &&
           IsAllZero(state.current_chunk_chaining_value.data(),
                     sizeof(state.current_chunk_chaining_value)) &&
           IsAllZero(state.current_block.data(), state.current_block.size()) &&
           IsAllZero(state.cv_stack.data(), sizeof(state.cv_stack));
}

[[nodiscard]] bool IsCleanFinalizedState(
    const Blake3IncrementalStateV3& state) noexcept {
    return IsCleanTerminalState(
        state, Blake3IncrementalLifecycleV3::kFinalized);
}

[[nodiscard]] bool IsCleanFailedState(
    const Blake3IncrementalStateV3& state) noexcept {
    return IsCleanTerminalState(state, Blake3IncrementalLifecycleV3::kFailed);
}

[[nodiscard]] bool IsUninitializedState(
    const Blake3IncrementalStateV3& state) noexcept {
    return IsAllZero(&state, sizeof(state));
}

[[nodiscard]] bool IsValidActiveState(
    const Blake3IncrementalStateV3& state) noexcept {
    if (!HasValidHeader(state) ||
        state.lifecycle != Blake3IncrementalLifecycleV3::kActive ||
        state.block_length > kBlockBytes || state.blocks_compressed > 15 ||
        state.cv_stack_length > kBlake3IncrementalCvStackCapacityV3 ||
        !IsAllZero(state.reserved.data(), state.reserved.size())) {
        return false;
    }

    constexpr std::uint64_t kMaximumChunkCounter =
        kBlake3IncrementalMaximumInputBytesV3 / kChunkBytes;
    if (state.current_chunk_counter > kMaximumChunkCounter) {
        return false;
    }

    const std::uint64_t current_chunk_bytes =
        (static_cast<std::uint64_t>(state.blocks_compressed) * kBlockBytes) +
        state.block_length;
    if (state.current_chunk_counter >
        (kBlake3IncrementalMaximumInputBytesV3 - current_chunk_bytes) /
            kChunkBytes) {
        return false;
    }
    const std::uint64_t expected_total =
        (state.current_chunk_counter * kChunkBytes) + current_chunk_bytes;
    if (expected_total != state.total_input_bytes ||
        state.cv_stack_length != PopulationCount(state.current_chunk_counter)) {
        return false;
    }

    // Successful updates retain the final block, including a complete final
    // block, so every nonempty message has 1..64 buffered bytes. Accepting the
    // alternate "one more compressed block, zero buffered bytes" form would
    // make exact block and chunk boundaries ambiguous at finalize. The empty
    // message is the sole active state with a zero-length current block.
    if (expected_total != 0 && state.block_length == 0) {
        return false;
    }

    // Before the first block in the current chunk is compressed, its chaining
    // value can only be the unkeyed BLAKE3 IV. This catches corruption of the
    // one chaining field whose exact value is structurally knowable. Later CVs
    // depend on caller bytes and cannot be revalidated without replaying input.
    if (state.blocks_compressed == 0 &&
        state.current_chunk_chaining_value != kIv) {
        return false;
    }

    const std::size_t block_tail = state.block_length;
    if (!IsAllZero(state.current_block.data() + block_tail,
                   state.current_block.size() - block_tail)) {
        return false;
    }

    for (std::size_t index = state.cv_stack_length;
         index < state.cv_stack.size();
         ++index) {
        if (!IsAllZero(state.cv_stack[index].data(),
                       sizeof(state.cv_stack[index]))) {
            return false;
        }
    }
    return true;
}

void TransitionToFailed(Blake3IncrementalStateV3& state) noexcept {
    SecureZero(state);
    state.magic = kBlake3IncrementalStateMagicV3;
    state.abi_version = kBlake3IncrementalStateAbiVersionV3;
    state.lifecycle = Blake3IncrementalLifecycleV3::kFailed;
}

[[nodiscard]] Blake3IncrementalStatusV3 StateLifecycleStatusAndFail(
    Blake3IncrementalStateV3& state) noexcept {
    if (IsCleanFailedState(state)) {
        return Blake3IncrementalStatusV3::kFailedState;
    }
    if (IsCleanFinalizedState(state)) {
        TransitionToFailed(state);
        return Blake3IncrementalStatusV3::kAlreadyFinalized;
    }
    if (IsUninitializedState(state)) {
        TransitionToFailed(state);
        return Blake3IncrementalStatusV3::kInitializationRequired;
    }
    TransitionToFailed(state);
    return Blake3IncrementalStatusV3::kInvalidState;
}

void ResetCurrentChunk(Blake3IncrementalStateV3& state,
                       std::uint64_t chunk_counter) noexcept {
    state.current_chunk_counter = chunk_counter;
    state.current_chunk_chaining_value = kIv;
    state.current_block.fill(0);
    state.block_length = 0;
    state.blocks_compressed = 0;
}

void CompressBufferedBlock(Blake3IncrementalStateV3& state) noexcept {
    Block block = LoadBlock(state.current_block);
    Chain next = Compress(state.current_chunk_chaining_value,
                          block,
                          state.current_chunk_counter,
                          static_cast<Word>(kBlockBytes),
                          ChunkStartFlag(state));
    state.current_chunk_chaining_value = next;
    state.current_block.fill(0);
    state.block_length = 0;
    state.blocks_compressed =
        static_cast<std::uint8_t>(state.blocks_compressed + 1U);
    SecureZero(block);
    SecureZero(next);
}

void UpdateCurrentChunk(Blake3IncrementalStateV3& state,
                        const std::uint8_t* input,
                        std::size_t input_size) noexcept {
    while (input_size != 0) {
        if (state.block_length == kBlockBytes) {
            CompressBufferedBlock(state);
        }
        const std::size_t available = kBlockBytes - state.block_length;
        const std::size_t take = std::min(available, input_size);
        std::memcpy(state.current_block.data() + state.block_length, input, take);
        state.block_length =
            static_cast<std::uint8_t>(state.block_length + take);
        input += take;
        input_size -= take;
    }
}

[[nodiscard]] bool AddCompletedChunk(
    Blake3IncrementalStateV3& state,
    Chain new_chaining_value,
    std::uint64_t total_completed_chunks) noexcept {
    while ((total_completed_chunks & 1U) == 0U) {
        if (state.cv_stack_length == 0) {
            SecureZero(new_chaining_value);
            return false;
        }
        state.cv_stack_length =
            static_cast<std::uint8_t>(state.cv_stack_length - 1U);
        Chain left = state.cv_stack[state.cv_stack_length];
        state.cv_stack[state.cv_stack_length].fill(0);
        Chain parent = ParentChainingValue(left, new_chaining_value);
        SecureZero(left);
        SecureZero(new_chaining_value);
        new_chaining_value = parent;
        SecureZero(parent);
        total_completed_chunks >>= 1U;
    }

    if (state.cv_stack_length >= state.cv_stack.size()) {
        SecureZero(new_chaining_value);
        return false;
    }
    state.cv_stack[state.cv_stack_length] = new_chaining_value;
    state.cv_stack_length =
        static_cast<std::uint8_t>(state.cv_stack_length + 1U);
    SecureZero(new_chaining_value);
    return true;
}

}  // namespace

Blake3IncrementalStatusV3 InitializeBlake3IncrementalV3(
    Blake3IncrementalStateV3* state) noexcept {
    if (state == nullptr) {
        return Blake3IncrementalStatusV3::kNullState;
    }
    if (IsCleanFailedState(*state)) {
        return Blake3IncrementalStatusV3::kFailedState;
    }
    if (!IsUninitializedState(*state)) {
        if (IsCleanFinalizedState(*state)) {
            TransitionToFailed(*state);
            return Blake3IncrementalStatusV3::kAlreadyFinalized;
        }
        TransitionToFailed(*state);
        return Blake3IncrementalStatusV3::kInvalidState;
    }
    state->magic = kBlake3IncrementalStateMagicV3;
    state->abi_version = kBlake3IncrementalStateAbiVersionV3;
    state->lifecycle = Blake3IncrementalLifecycleV3::kActive;
    state->current_chunk_chaining_value = kIv;
    return Blake3IncrementalStatusV3::kOk;
}

Blake3IncrementalStatusV3 UpdateBlake3IncrementalV3(
    Blake3IncrementalStateV3* state,
    const std::uint8_t* input,
    std::size_t input_size) noexcept {
    if (state == nullptr) {
        return Blake3IncrementalStatusV3::kNullState;
    }
    if (IsCleanFailedState(*state)) {
        return Blake3IncrementalStatusV3::kFailedState;
    }
    if (input_size != 0 && input == nullptr) {
        TransitionToFailed(*state);
        return Blake3IncrementalStatusV3::kNullInput;
    }
    if (input_size != 0 &&
        RangesOverlap(input, input_size, state, sizeof(*state))) {
        return Blake3IncrementalStatusV3::kStateOverlap;
    }
    if (!IsValidActiveState(*state)) {
        return StateLifecycleStatusAndFail(*state);
    }

    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    const std::uint64_t input_size_u64 =
        static_cast<std::uint64_t>(input_size);
    if (input_size_u64 >
        (kBlake3IncrementalMaximumInputBytesV3 - state->total_input_bytes)) {
        TransitionToFailed(*state);
        return Blake3IncrementalStatusV3::kLengthOverflow;
    }
    if (input_size == 0) {
        return Blake3IncrementalStatusV3::kOk;
    }

    state->total_input_bytes += input_size_u64;
    while (input_size != 0) {
        if (CurrentChunkLength(*state) == kChunkBytes) {
            Output completed_output = CurrentChunkOutput(*state);
            Chain completed_cv = OutputChainingValue(completed_output);
            const std::uint64_t completed_chunks =
                state->current_chunk_counter + 1U;
            const bool added =
                AddCompletedChunk(*state, completed_cv, completed_chunks);
            SecureZero(completed_output);
            SecureZero(completed_cv);
            if (!added) {
                TransitionToFailed(*state);
                return Blake3IncrementalStatusV3::kInternalCapacityExceeded;
            }
            ResetCurrentChunk(*state, completed_chunks);
        }

        const std::size_t chunk_remaining =
            kChunkBytes - CurrentChunkLength(*state);
        const std::size_t take = std::min(chunk_remaining, input_size);
        UpdateCurrentChunk(*state, input, take);
        input += take;
        input_size -= take;
    }
    return Blake3IncrementalStatusV3::kOk;
}

Blake3IncrementalStatusV3 FinalizeBlake3IncrementalV3(
    Blake3IncrementalStateV3* state,
    Blake3Digest* digest_out) noexcept {
    if (state == nullptr) {
        return Blake3IncrementalStatusV3::kNullState;
    }
    if (IsCleanFailedState(*state)) {
        return Blake3IncrementalStatusV3::kFailedState;
    }
    if (digest_out == nullptr) {
        TransitionToFailed(*state);
        return Blake3IncrementalStatusV3::kNullDigest;
    }
    if (RangesOverlap(digest_out, sizeof(*digest_out), state, sizeof(*state))) {
        return Blake3IncrementalStatusV3::kStateOverlap;
    }
    if (!IsValidActiveState(*state)) {
        return StateLifecycleStatusAndFail(*state);
    }

    Output output = CurrentChunkOutput(*state);
    for (std::size_t index = state->cv_stack_length; index != 0; --index) {
        Chain right = OutputChainingValue(output);
        Output parent = ParentOutput(state->cv_stack[index - 1U], right);
        SecureZero(right);
        SecureZero(output);
        output = parent;
        SecureZero(parent);
    }

    Blake3Digest digest = RootDigest(output);
    SecureZero(output);

    SecureZero(*state);
    state->magic = kBlake3IncrementalStateMagicV3;
    state->abi_version = kBlake3IncrementalStateAbiVersionV3;
    state->lifecycle = Blake3IncrementalLifecycleV3::kFinalized;

    *digest_out = digest;
    SecureZero(digest);
    return Blake3IncrementalStatusV3::kOk;
}

void ScrubBlake3IncrementalV3(Blake3IncrementalStateV3* state) noexcept {
    if (state != nullptr) {
        SecureZero(*state);
    }
}

Blake3Digest ComputeBlake3Digest(
    const std::vector<std::uint8_t>& input) noexcept {
    Blake3Digest digest{};
    Blake3IncrementalStateV3 state{};
    if (InitializeBlake3IncrementalV3(&state) !=
            Blake3IncrementalStatusV3::kOk ||
        UpdateBlake3IncrementalV3(&state, input.data(), input.size()) !=
            Blake3IncrementalStatusV3::kOk ||
        FinalizeBlake3IncrementalV3(&state, &digest) !=
            Blake3IncrementalStatusV3::kOk) {
        ScrubBlake3IncrementalV3(&state);
        digest.fill(0);
    }
    return digest;
}

}  // namespace scratchbird::core::hash
