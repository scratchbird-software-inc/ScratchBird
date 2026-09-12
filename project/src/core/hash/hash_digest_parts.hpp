// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hash_digest.hpp"

namespace scratchbird::core::hash {

struct HashDigestSegment {
  const byte* data = nullptr;
  std::size_t size = 0;
};

// Hash the exact concatenation without materializing it. Inputs are borrowed
// for this call; zero-length segments may have null pointers. A nonempty
// segment requires readable storage. Segment boundaries are not hashed.
HashDigestResult ComputeSha256DigestParts(const HashDigestSegment* segments,
                                        std::size_t segment_count);

}  // namespace scratchbird::core::hash
