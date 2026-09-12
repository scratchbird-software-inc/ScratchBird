// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
using TableTruncateUuid = std::array<std::uint8_t, 16>;
using TableTruncateSha = std::array<std::uint8_t, 32>;

struct SblrTableTruncateRequestV1 {
  TableTruncateUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t truncate_occurrence = 0;
};
// Bodies use the exact absolute wire fields in Core
// SBLR_TABLE_TRUNCATE_CANONICAL_FIELDS_V1, starting at byte16.
// They are not opaque authority tokens: validation checks each semantic field,
// but the engine must still resolve frozen proofs and execute actual effects.
struct SblrTableTruncateDescriptorV1 {
  std::array<std::uint8_t, 368> canonical_body{};
  TableTruncateSha evidence{};
  std::uint64_t availability_generation = 0;
};
struct SblrTableTruncateResultV1 {
  std::array<std::uint8_t, 136> canonical_body{};
  TableTruncateSha evidence{};
  std::uint64_t availability_generation = 0;
};
// Encoding failure returns no frame. Decoding failure leaves output unchanged,
// including when allocation or hashing fails. A valid frame is not commit proof.
std::vector<std::uint8_t> EncodeSblrTableTruncateRequestV1(const SblrTableTruncateRequestV1&);
bool DecodeSblrTableTruncateRequestV1(const std::uint8_t*, std::size_t,
                                     SblrTableTruncateRequestV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrTableTruncateDescriptorV1(
    const SblrTableTruncateDescriptorV1&, bool operand);
bool DecodeSblrTableTruncateDescriptorV1(const std::uint8_t*, std::size_t,
                                        SblrTableTruncateDescriptorV1*, std::string*, bool operand);
std::vector<std::uint8_t> EncodeSblrTableTruncateResultV1(const SblrTableTruncateResultV1&);
bool DecodeSblrTableTruncateResultV1(const std::uint8_t*, std::size_t,
                                    SblrTableTruncateResultV1*, std::string*);
}  // namespace scratchbird::engine::sblr
