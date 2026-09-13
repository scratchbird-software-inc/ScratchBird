// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "blake3_digest.hpp"

#include <algorithm>
#include <limits>

namespace scratchbird::core::hash {
namespace {

// Independently implemented from the BLAKE3 algorithm specification, section 2:
// https://github.com/BLAKE3-team/BLAKE3-specs/blob/master/blake3.pdf
using Word = std::uint32_t;
using Chain = std::array<Word, 8>;
using Block = std::array<Word, 16>;
constexpr Chain kInitial{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
constexpr std::array<unsigned, 16> kPermutation{2, 6, 3, 10, 7, 0, 4, 13,
                                               1, 11, 12, 5, 9, 14, 15, 8};
constexpr Word kStart = 1, kEnd = 2, kParent = 4, kRoot = 8;

Word Rotate(Word value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32 - bits));
}

void Mix(Word& a, Word& b, Word& c, Word& d, Word first, Word second) noexcept {
  a += b + first;
  d = Rotate(d ^ a, 16);
  c += d;
  b = Rotate(b ^ c, 12);
  a += b + second;
  d = Rotate(d ^ a, 8);
  c += d;
  b = Rotate(b ^ c, 7);
}

Chain Compress(const Chain& input, Block message, std::uint64_t counter,
               Word length, Word flags) noexcept {
  Block state{};
  std::copy(input.begin(), input.end(), state.begin());
  std::copy_n(kInitial.begin(), 4, state.begin() + 8);
  state[12] = static_cast<Word>(counter);
  state[13] = static_cast<Word>(counter >> 32);
  state[14] = length;
  state[15] = flags;
  for (unsigned round = 0; round < 7; ++round) {
    for (unsigned column = 0; column < 4; ++column)
      Mix(state[column], state[column + 4], state[column + 8], state[column + 12],
          message[2 * column], message[2 * column + 1]);
    for (unsigned diagonal = 0; diagonal < 4; ++diagonal)
      Mix(state[diagonal], state[4 + (diagonal + 1) % 4],
          state[8 + (diagonal + 2) % 4], state[12 + (diagonal + 3) % 4],
          message[8 + 2 * diagonal], message[9 + 2 * diagonal]);
    if (round != 6) {
      Block permuted{};
      for (unsigned i = 0; i < 16; ++i) permuted[i] = message[kPermutation[i]];
      message = permuted;
    }
  }
  Chain result{};
  for (unsigned i = 0; i < 8; ++i) result[i] = state[i] ^ state[i + 8];
  return result;
}

Chain Chunk(const std::uint8_t* bytes, std::size_t size, std::uint64_t index,
            bool root) noexcept {
  Chain chain = kInitial;
  std::size_t position = 0;
  do {
    const auto count = std::min<std::size_t>(64, size - position);
    Block block{};
    for (std::size_t i = 0; i < count; ++i)
      block[i / 4] |= static_cast<Word>(bytes[position + i]) << (8 * (i % 4));
    const bool last = position + count == size;
    const Word flags = (position == 0 ? kStart : 0) | (last ? kEnd : 0) |
                       (last && root ? kRoot : 0);
    chain = Compress(chain, block, index, static_cast<Word>(count), flags);
    position += count;
  } while (position < size);
  return chain;
}

Chain Tree(const std::uint8_t* bytes, std::size_t size, std::uint64_t index,
           bool root) noexcept {
  if (size <= 1024) return Chunk(bytes, size, index, root);
  // Largest full power-of-two subtree strictly shorter than this input.
  // Division avoids overflow at the maximum addressable input length. The
  // recursion has at most 55 frames for a 64-bit byte extent, not one per chunk.
  std::size_t left_size = 1024;
  while (left_size <= (size - 1) / 2) left_size *= 2;
  const auto left = Tree(bytes, left_size, index, false);
  const auto right = Tree(bytes + left_size, size - left_size,
                          index + left_size / 1024, false);
  Block children{};
  std::copy(left.begin(), left.end(), children.begin());
  std::copy(right.begin(), right.end(), children.begin() + 8);
  return Compress(kInitial, children, 0, 64, kParent | (root ? kRoot : 0));
}

}  // namespace

Blake3Digest ComputeBlake3Digest(const std::vector<std::uint8_t>& input) noexcept {
  static_assert(std::numeric_limits<std::size_t>::digits <= 64);
  const auto words = Tree(input.data(), input.size(), 0, true);
  Blake3Digest digest{};
  for (std::size_t i = 0; i < digest.size(); ++i)
    digest[i] = static_cast<std::uint8_t>(words[i / 4] >> (8 * (i % 4)));
  return digest;
}

}  // namespace scratchbird::core::hash
