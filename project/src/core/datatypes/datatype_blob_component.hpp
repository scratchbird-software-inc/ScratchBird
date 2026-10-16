// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_blob.hpp"
#include "blake3_digest.hpp"

namespace scratchbird::core::datatypes {

inline constexpr std::size_t kBlobComponentHeaderBytesV3 = 320;
using BlobComponentHeaderV3 = std::array<byte, kBlobComponentHeaderBytesV3>;

enum class BlobComponentPassFailureV3 : u8 {
  none = 0,
  profile_invalid,
  invalid_encode_state,
  invalid_phase,
  input_overlap,
  input_extent_invalid,
  content_extent_mismatch,
  hash_state_failed,
  canonical_encoding_invalid,
  integrity_failed,
};

// Only actual encoded-field/digest failures populate the public vector.
// Call-site, lifecycle, profile-admission and chunk-schedule mistakes are
// distinct internal failures; they cannot invent a malformed persisted frame.
struct BlobComponentPassResultV3 {
  BlobComponentPassFailureV3 failure = BlobComponentPassFailureV3::none;
  std::string_view diagnostic_code{};
  u32 reason_enum = 0;
  u8 format_enum = 0;
  bool failure_offset_present = false;
  u64 failure_offset = 0;
  platform::Uuid diagnostic_uuid{};
  u64 diagnostic_generation = 0;
  bool ok() const noexcept { return failure == BlobComponentPassFailureV3::none; }
};

// Pure VALUE logical-content digest, NOT equality, lifetime or index authority.
// A single hash state avoids constructing/hashing a component frame merely to
// find its logical digest. The owning consumer accounts this complete workspace
// and source windows, authenticates source reads and gates output publication.
class BlobLogicalDigestPassV3 final {
 public:
  BlobLogicalDigestPassV3() noexcept = default;
  ~BlobLogicalDigestPassV3() noexcept { Scrub(); }
  BlobLogicalDigestPassV3(const BlobLogicalDigestPassV3&) = delete;
  BlobLogicalDigestPassV3& operator=(const BlobLogicalDigestPassV3&) = delete;
  BlobLogicalDigestPassV3(BlobLogicalDigestPassV3&&) = delete;
  BlobLogicalDigestPassV3& operator=(BlobLogicalDigestPassV3&&) = delete;
  BlobComponentPassResultV3 Begin(const BlobValidatedProfileHandleV3&, u64 length) noexcept;
  BlobComponentPassResultV3 Update(std::span<const byte>) noexcept;
  BlobComponentPassResultV3 Finish(hash::Blake3Digest&) noexcept;
  void Scrub() noexcept;
 private:
  enum class Phase : u8 { empty, active, finished, failed };
  BlobComponentPassResultV3 Fail(BlobComponentPassFailureV3) noexcept;
  bool Integer(u64) noexcept;
  hash::Blake3IncrementalStateV3 hash_{};
  hash::Blake3Digest staged_{};
  std::array<byte,8> integer_{};
  u64 expected_=0, consumed_=0;
  Phase phase_=Phase::empty;
};

// Initial decoder gate: validate the already admitted320-byte header without
// constructing/reserving either hash state or any additional header buffer.
// This lets malformed fields win before later hash/content/sink admission.
// Success proves byte shape/identity ONLY, not integrity, lifetime or authority.
BlobComponentPassResultV3 ValidateBlobComponentHeaderNoAllocV3(
    const BlobValidatedProfileHandleV3& profile, u64 declared_extent,
    std::span<const byte> header, bool null_allowed = true) noexcept;

// Pure fixed-workspace framing/digest machinery, NOT a retained BLOB value,
// token issuer, reader adapter, sink, storage locator or execution authority.
// The owning codec must admit/account sizeof(this), input/header staging and
// the source/sink work before use. Input is consumed synchronously and never
// retained. Decode initialization alone proves only header shape/identity.
//
// Encode: one complete digest pass creates the exact320-byte header. The
// owning two-pass encoder must still revalidate, reread and atomically write
// header+content using the SAME retained source snapshot. This helper cannot
// replace the second pass or qualify a materialized storage shortcut.
// Decode: initialize from an already admitted exact header, stream the exact
// content, then FinishDecode checks BOTH digests in registered precedence.
class BlobComponentDigestPassV3 final {
 public:
  BlobComponentDigestPassV3() noexcept = default;
  ~BlobComponentDigestPassV3() noexcept { Scrub(); }
  BlobComponentDigestPassV3(const BlobComponentDigestPassV3&) = delete;
  BlobComponentDigestPassV3& operator=(const BlobComponentDigestPassV3&) = delete;
  BlobComponentDigestPassV3(BlobComponentDigestPassV3&&) = delete;
  BlobComponentDigestPassV3& operator=(BlobComponentDigestPassV3&&) = delete;

  BlobComponentPassResultV3 BeginEncode(
      const BlobValidatedProfileHandleV3& profile, BlobValueStateV3 state,
      u64 logical_length) noexcept;
  BlobComponentPassResultV3 BeginDecode(
      const BlobValidatedProfileHandleV3& profile, u64 declared_extent,
      std::span<const byte> header, bool null_allowed = true) noexcept;
  BlobComponentPassResultV3 UpdateContent(std::span<const byte> bytes) noexcept;
  // Every failure preserves caller output. Neither method publishes a value
  // or survives owner cleanup as a grant. Finalize succeeds exactly once.
  BlobComponentPassResultV3 FinishEncode(BlobComponentHeaderV3& output) noexcept;
  BlobComponentPassResultV3 FinishDecode() noexcept;
  void Scrub() noexcept;

 private:
  enum class Phase : u8 { empty = 0, encoding, decoding, finished, failed };
  BlobComponentPassResultV3 StartDigests() noexcept;
  BlobComponentPassResultV3 FinishDigests() noexcept;
  BlobComponentPassResultV3 Fail(BlobComponentPassFailureV3) noexcept;
  void ClearWork() noexcept;
  Phase phase_ = Phase::empty;
  BlobValueStateV3 state_ = BlobValueStateV3::value;
  u64 expected_ = 0;
  u64 consumed_ = 0;
  u64 hash_policy_generation_ = 0;
  BlobComponentHeaderV3 header_{};
  std::array<byte,64> expected_digests_{};
  hash::Blake3IncrementalStateV3 content_hash_{};
  hash::Blake3IncrementalStateV3 frame_hash_{};
};

// Two final digest staging objects are additional to the resident state. The
// caller's source window/header output and sink staging remain separate work.
inline constexpr u64 kBlobComponentDigestPassFixedWorkBytesV3 =
    sizeof(BlobComponentDigestPassV3) + 2*sizeof(hash::Blake3Digest);

} // namespace scratchbird::core::datatypes
