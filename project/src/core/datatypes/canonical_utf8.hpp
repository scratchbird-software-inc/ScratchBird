// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace scratchbird::core::datatypes {
// Exact shortest-form Unicode scalar sequence. Embedded NUL and empty values
// are legal; callers impose label/field restrictions separately. No collation,
// normalization, locale, charset inference or policy default is performed.
inline bool ValidateCanonicalUtf8(const std::uint8_t* bytes, std::size_t size,
                                  std::uint64_t* scalar_count = nullptr) {
  if (scalar_count != nullptr) *scalar_count = 0;
  if (bytes == nullptr && size != 0) return false;
  std::uint64_t count = 0;
  std::size_t offset = 0;
  while (offset < size) {
    const unsigned char first = bytes[offset];
    if (first <= 0x7f) {
      ++offset;
      ++count;
      continue;
    }
    std::size_t length = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
    } else {
      return false;
    }
    if (length > size - offset) {
      return false;
    }
    for (std::size_t index = 1; index < length; ++index) {
      if ((bytes[offset + index] & 0xc0) != 0x80) {
        return false;
      }
    }
    if ((first == 0xe0 && bytes[offset + 1] < 0xa0) ||
        (first == 0xed && bytes[offset + 1] > 0x9f) ||
        (first == 0xf0 && bytes[offset + 1] < 0x90) ||
        (first == 0xf4 && bytes[offset + 1] > 0x8f)) {
      return false;
    }
    offset += length;
    ++count;
  }
  if (scalar_count != nullptr) *scalar_count = count;
  return true;
}
}  // namespace scratchbird::core::datatypes
