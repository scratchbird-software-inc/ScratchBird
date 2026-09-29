// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/core/platform/runtime_platform.hpp"
#include <cstdint>
#include <string>

namespace scratchbird::tests {
// Independent IPL/DPE byte oracle: no production sort-key or index encoder.
// A non-NULL ascending int64 component, NULLS FIRST, without collation.
inline std::string ExpectedInt64OrderedIndexKey(
    const core::platform::Uuid& datatype_uuid, std::uint64_t generation,
    std::int64_t value) {
  std::string bytes = "SBKOBIN:SBKO";
  bytes += std::string("\x7f\x00\x00\x00\x01\x04", 6);
  bytes.append(reinterpret_cast<const char*>(datatype_uuid.bytes.data()), 16);
  for (int shift = 56; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<char>((generation >> shift) & 0xff));
  bytes.push_back('\0');  // No collation identity.
  bytes.push_back('\1');  // Datatype value-state: present.
  const auto ordered = static_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << 63);
  for (int shift = 56; shift >= 0; shift -= 8) {
    const auto byte = static_cast<unsigned char>((ordered >> shift) & 0xff);
    bytes.push_back(static_cast<char>(byte));
    if (byte == 0) bytes.push_back(static_cast<char>(0xff));
  }
  bytes.append(2, '\0');
  return bytes;
}
}  // namespace scratchbird::tests
