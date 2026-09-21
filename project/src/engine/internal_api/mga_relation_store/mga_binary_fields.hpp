// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

namespace scratchbird::engine::internal_api {
namespace mga_binary_fields_detail {
template<class UInt>
bool ReadUnsigned(std::span<const std::uint8_t> bytes,
                  std::size_t* offset, UInt* out) {
  static_assert(std::is_unsigned_v<UInt>);
  if (offset == nullptr || out == nullptr || *offset > bytes.size() ||
      bytes.size() - *offset < sizeof(UInt)) return false;
  UInt candidate = 0;
  for (std::size_t n = 0; n < sizeof(UInt); ++n) {
    candidate |= static_cast<UInt>(static_cast<UInt>(bytes[*offset + n]) << (n * 8));
  }
  *out = candidate;
  *offset += sizeof(UInt);
  return true;
}
}  // namespace mga_binary_fields_detail

// Fixed little-endian fields; malformed input never changes cursor/output.
inline bool ReadBinaryU8(std::span<const std::uint8_t> bytes, std::size_t* offset, std::uint8_t* out) {
  return mga_binary_fields_detail::ReadUnsigned(bytes, offset, out);
}
inline bool ReadBinaryU16(std::span<const std::uint8_t> bytes, std::size_t* offset, std::uint16_t* out) {
  return mga_binary_fields_detail::ReadUnsigned(bytes, offset, out);
}
inline bool ReadBinaryU32(std::span<const std::uint8_t> bytes, std::size_t* offset, std::uint32_t* out) {
  return mga_binary_fields_detail::ReadUnsigned(bytes, offset, out);
}
inline bool ReadBinaryU64(std::span<const std::uint8_t> bytes, std::size_t* offset, std::uint64_t* out) {
  return mga_binary_fields_detail::ReadUnsigned(bytes, offset, out);
}

// Length-framed bytes, not identity or datatype authority. Allocation failure
// propagates without publishing a partial string or consuming the length word.
inline bool ReadBinaryString(std::span<const std::uint8_t> bytes,
                              std::size_t* offset, std::string* out) {
  if (offset == nullptr || out == nullptr) return false;
  auto next = *offset;
  std::uint32_t length = 0;
  if (!ReadBinaryU32(bytes, &next, &length) ||
      length > bytes.size() - next) return false;
  std::string candidate(reinterpret_cast<const char*>(bytes.data() + next), length);
  out->swap(candidate);
  *offset = next + length;
  return true;
}

}  // namespace scratchbird::engine::internal_api
