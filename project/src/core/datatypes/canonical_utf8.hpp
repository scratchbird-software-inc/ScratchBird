// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace scratchbird::core::datatypes {
// Exact shortest-form scalar decoding. No allocation, charset inference,
// normalization or collation. Failure leaves both outputs unchanged.
inline bool DecodeCanonicalUtf8Scalar(const std::uint8_t* bytes, std::size_t size,
                                     std::size_t* offset, std::uint32_t* scalar) noexcept {
  if (!bytes || !offset || !scalar || *offset >= size) return false;
  const std::size_t start = *offset;
  const auto first = bytes[start];
  std::size_t length = 0;
  std::uint32_t decoded = 0;
  if (first <= 0x7f) { length = 1; decoded = first; }
  else if (first >= 0xc2 && first <= 0xdf) { length = 2; decoded = first & 0x1f; }
  else if (first >= 0xe0 && first <= 0xef) { length = 3; decoded = first & 0x0f; }
  else if (first >= 0xf0 && first <= 0xf4) { length = 4; decoded = first & 7; }
  else return false;
  if (length > size - start) return false;
  for (std::size_t i = 1; i < length; ++i) {
    const auto next = bytes[start + i];
    if ((next & 0xc0) != 0x80) return false;
    decoded = (decoded << 6) | (next & 0x3f);
  }
  if ((length == 2 && decoded < 0x80) ||
      (length == 3 && decoded < 0x800) ||
      (length == 4 && decoded < 0x10000) ||
      (decoded >= 0xd800 && decoded <= 0xdfff) || decoded > 0x10ffff) return false;
  *offset = start + length;
  *scalar = decoded;
  return true;
}

// Embedded NUL and empty values are legal; callers impose label restrictions.
// On validation failure the optional scalar count is zero, never a prefix count.
inline bool ValidateCanonicalUtf8(const std::uint8_t* bytes, std::size_t size,
                                  std::uint64_t* scalar_count = nullptr) noexcept {
  if (scalar_count) *scalar_count = 0;
  if (!bytes && size != 0) return false;
  std::uint64_t count = 0;
  std::size_t offset = 0;
  while (offset < size) {
    std::uint32_t scalar = 0;
    if (!DecodeCanonicalUtf8Scalar(bytes, size, &offset, &scalar)) return false;
    ++count;
  }
  if (scalar_count) *scalar_count = count;
  return true;
}
}  // namespace scratchbird::core::datatypes
