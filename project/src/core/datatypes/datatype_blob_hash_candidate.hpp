// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_blob_component.hpp"

namespace scratchbird::core::datatypes {

inline constexpr std::size_t kBlobHashCandidateBytesV3 = 304;
using BlobHashCandidateFrameV3 = std::array<byte, kBlobHashCandidateBytesV3>;

enum class BlobRawIndexPolicyDispositionV3 : u8 {
  hash_candidate_requires_recheck = 0,
  refused,
  profile_invalid,
  family_binding_invalid,
  input_invalid,
};
struct BlobRawIndexPolicyResultV3 {
  BlobRawIndexPolicyDispositionV3 disposition = BlobRawIndexPolicyDispositionV3::profile_invalid;
  std::string_view diagnostic_code{};
  platform::Uuid diagnostic_uuid{};
  u64 diagnostic_generation = 0;
  platform::Uuid descriptor_uuid{};
  u64 descriptor_generation = 0;
  u32 reason_enum = 0;
  u8 ordering_operation_enum = 0; // absent (zero) for nonordering refusals
  u64 logical_length_u64 = 0, budget_octets_u64 = 0, target_limit_u64 = 0;
};

// Actual binary registry identity and generation, never a caller family label.
// A hash result is conditional policy admission, NOT permission to read/write
// or a claim that an index exists. All fifteen other raw-key families refuse.
BlobRawIndexPolicyResultV3 SelectBlobRawIndexPolicyNoAllocV3(
    const BlobValidatedProfileHandleV3& profile,
    const platform::Uuid& index_family_uuid, u64 index_family_generation,
    u64 logical_length, u64 budget_octets, u64 target_limit) noexcept;

// Fixed-workspace representation machinery ONLY. A digest supplied by a caller,
// a well-formed frame or a matching projection is not content, MGA, lifetime,
// security or equality authority. The owning index must authenticate its source,
// bind the actual row version/generation and rebuild epoch, and perform a full
// protected content recheck for EVERY match (including unique-index checks).
// SQL NULL is not a candidate; containing operators own their null bitmap.
// Account the complete hash workspace and staged frame in the owning grant.
// No allocations, borrowed pointers, raw content or text UUIDs are retained.
// Every failure leaves the caller output unchanged. Private preconditions have
// no fabricated public diagnostic. Format failures use format3's exact vectors.
BlobComponentPassResultV3 EncodeBlobHashCandidateNoAllocV3(
    const BlobValidatedProfileHandleV3& profile, u64 logical_length,
    std::span<const byte> logical_content_digest,
    const platform::Uuid& recheck_row_version_uuid, u64 recheck_generation,
    u64 rebuild_epoch, BlobHashCandidateFrameV3& output) noexcept;

// Structural validation only: the projection bytes are NOT authenticated by
// this check. Neither the digest nor the row reference gains authority here.
// Exact binary identities are compared against the complete validated profile.
BlobComponentPassResultV3 ValidateBlobHashCandidateNoAllocV3(
    const BlobValidatedProfileHandleV3& profile,
    std::span<const byte> candidate) noexcept;

inline constexpr u64 kBlobHashCandidateEncodeWorkBytesV3 =
    sizeof(BlobHashCandidateFrameV3) + sizeof(hash::Blake3IncrementalStateV3) +
    sizeof(hash::Blake3Digest);

} // namespace scratchbird::core::datatypes
