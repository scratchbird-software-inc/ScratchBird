// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace scratchbird::core {

// Borrow a writable extent owned by the caller. The configured cryptographic
// backend owns OS entropy and fork/reseed handling; no PRNG/seed fallback.
// On backend failure the entire destination is cleared, including any prefix
// already filled. A null positive extent is invalid. Empty extent is a no-op.
// This primitive is not a security/provider/resource admission receipt.
inline bool FillCryptographicRandomBytes(unsigned char* destination,
                                         std::size_t size) noexcept {
  if(size!=0&&destination==nullptr)return false;
  std::size_t offset=0;
  while(offset<size) {
    const auto count=std::min(size-offset,static_cast<std::size_t>(std::numeric_limits<int>::max()));
    if(RAND_bytes(destination+offset,static_cast<int>(count))!=1) {
      OPENSSL_cleanse(destination,size);
      return false;
    }
    offset+=count;
  }
  return true;
}

}  // namespace scratchbird::core
