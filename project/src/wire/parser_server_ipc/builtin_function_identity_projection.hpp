// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../core/uuid/uuid.hpp"
#include <algorithm>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::wire {

// An engine-issued metadata projection, not permission to execute. Labels
// are parser lookup metadata; the only executable identity is the raw UUID.
struct BuiltinFunctionIdentity {
  std::string canonical_id;
  core::platform::Uuid function_uuid;
};

inline bool ValidBuiltinFunctionIdentities(
    std::span<const BuiltinFunctionIdentity> rows) {
  if (rows.empty() || rows.size() > 4096) return false;
  std::set<std::string> names;
  std::set<core::platform::Uuid> identities;
  for (const auto& row : rows) {
    if (row.canonical_id.empty() || row.canonical_id.size() > 256 ||
        !std::ranges::all_of(row.canonical_id, [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '.';
        }) || !core::uuid::IsEngineIdentityUuid(row.function_uuid) ||
        !names.insert(row.canonical_id).second ||
        !identities.insert(row.function_uuid).second) return false;
  }
  return true;
}

// Version 1: u16 version, u16 count; repeated u16 UTF-8 label length,
// label bytes, raw16 UUID. No textual UUID carrier and no trailing bytes.
inline bool EncodeBuiltinFunctionIdentities(
    std::span<const BuiltinFunctionIdentity> rows,
    std::vector<std::uint8_t>* output) {
  if (!output || !ValidBuiltinFunctionIdentities(rows)) return false;
  std::vector<std::uint8_t> encoded;
  const auto u16 = [&](std::uint16_t n) {
    encoded.push_back(static_cast<std::uint8_t>(n));
    encoded.push_back(static_cast<std::uint8_t>(n >> 8));
  };
  u16(1); u16(static_cast<std::uint16_t>(rows.size()));
  for (const auto& row : rows) {
    u16(static_cast<std::uint16_t>(row.canonical_id.size()));
    encoded.insert(encoded.end(), row.canonical_id.begin(), row.canonical_id.end());
    encoded.insert(encoded.end(), row.function_uuid.bytes.begin(), row.function_uuid.bytes.end());
  }
  *output = std::move(encoded);
  return true;
}

inline bool DecodeBuiltinFunctionIdentities(
    std::span<const std::uint8_t> bytes,
    std::vector<BuiltinFunctionIdentity>* output) {
  if (!output || bytes.size() < 4) return false;
  const auto u16 = [&](std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (std::uint16_t{bytes[at + 1]} << 8));
  };
  if (u16(0) != 1 || u16(2) == 0 || u16(2) > 4096) return false;
  std::vector<BuiltinFunctionIdentity> decoded;
  std::size_t at = 4;
  for (std::uint16_t i = 0; i < u16(2); ++i) {
    if (bytes.size() - at < 2) return false;
    const auto size = u16(at); at += 2;
    if (size == 0 || size > 256 || bytes.size() - at < size + 16u) return false;
    BuiltinFunctionIdentity row;
    row.canonical_id.assign(reinterpret_cast<const char*>(bytes.data() + at), size);
    at += size;
    std::copy_n(bytes.begin() + at, 16, row.function_uuid.bytes.begin()); at += 16;
    decoded.push_back(std::move(row));
  }
  if (at != bytes.size() || !ValidBuiltinFunctionIdentities(decoded)) return false;
  *output = std::move(decoded);
  return true;
}

}  // namespace scratchbird::wire
