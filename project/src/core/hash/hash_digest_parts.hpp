// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hash_digest.hpp"

namespace scratchbird::core::hash {

struct HashDigestSegment {
  const byte* data = nullptr;
  std::size_t size = 0;
};

enum class Sha256PartsError : unsigned char {
  none, segments_missing, segment_extent_invalid, provider_failure
};
struct Sha256PartsResult {
  Sha256PartsError error = Sha256PartsError::provider_failure;
  Digest256 digest{};
  bool ok() const { return error == Sha256PartsError::none; }
};
// Fixed result, including every error path. The existing EVP provider still
// owns its context allocations; absence of C++ allocation is not accounting
// proof for that provider. No partial digest is returned after a failure.
Sha256PartsResult ComputeSha256DigestPartsNative(const HashDigestSegment*, std::size_t);
inline const char* Sha256PartsErrorDetail(Sha256PartsError error) {
  switch(error) {
    case Sha256PartsError::segments_missing: return "segments_missing";
    case Sha256PartsError::segment_extent_invalid: return "segment_extent_invalid";
    default: return "";
  }
}

// Hash the exact concatenation without materializing it. Inputs are borrowed
// for this call; zero-length segments may have null pointers. A nonempty
// segment requires readable storage. Segment boundaries are not hashed.
HashDigestResult ComputeSha256DigestParts(const HashDigestSegment* segments,
                                        std::size_t segment_count);

}  // namespace scratchbird::core::hash
