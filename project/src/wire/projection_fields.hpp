// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../core/uuid/uuid.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::wire::projection_fields {
// Bounded length-framed projection fields. Identity positions contain exactly
// 16 bytes, including nil for optional references. Never delimiter/hex encode
// identity bytes; fields may contain every octet including NUL and '|'.
inline constexpr std::string_view kMagic = "SBPF0001";
inline constexpr std::size_t kMaximumBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaximumFields = 256;
inline std::string Identity(const core::platform::Uuid& id) {
  return {reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size()};
}
inline bool Read(std::string_view field, core::platform::Uuid* out) {
  if (!out || field.size() != 16) return false;
  core::platform::Uuid candidate;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(field.data()), 16,
              candidate.bytes.begin());
  if (!candidate.is_nil() && !core::uuid::IsEngineIdentityUuid(candidate)) return false;
  *out = candidate;
  return true;
}
inline bool Read(std::string_view field, std::string* out) {
  if (!out) return false;
  out->assign(field);
  return true;
}
inline std::string Encode(const std::vector<std::string>& fields) {
  if (fields.size() > kMaximumFields) return {};
  std::size_t size = 12;
  for (const auto& field : fields) {
    if (field.size() > kMaximumBytes - 4 || size > kMaximumBytes - 4 - field.size()) return {};
    size += 4 + field.size();
  }
  std::string out(kMagic);
  out.reserve(size);
  const auto put = [&](std::uint32_t n) {
    for (unsigned i=0; i<4; ++i) out.push_back(static_cast<char>(n >> (8*i)));
  };
  put(static_cast<std::uint32_t>(fields.size()));
  for (const auto& field : fields) {
    put(static_cast<std::uint32_t>(field.size())); out.append(field);
  }
  return out;
}
inline bool Decode(std::string_view bytes, std::vector<std::string_view>* out) {
  if (!out || bytes.size() < 12 || bytes.size() > kMaximumBytes || !bytes.starts_with(kMagic)) return false;
  std::size_t cursor=8;
  const auto get = [&](std::uint32_t* n) {
    if (bytes.size()-cursor < 4) return false;
    *n=0;
    for(unsigned i=0;i<4;++i) *n |= std::uint32_t(static_cast<unsigned char>(bytes[cursor++])) << (8*i);
    return true;
  };
  std::uint32_t count=0;
  if (!get(&count) || count>kMaximumFields || count>(bytes.size()-cursor)/4) return false;
  // Validate all lengths before allocating views.
  const auto begin=cursor;
  for(std::uint32_t i=0;i<count;++i) {
    std::uint32_t size=0;
    if(!get(&size)||size>bytes.size()-cursor)return false;
    cursor+=size;
  }
  if(cursor!=bytes.size())return false;
  cursor=begin;
  std::vector<std::string_view> fields;
  fields.reserve(count);
  for(std::uint32_t i=0;i<count;++i) {
    std::uint32_t size=0;get(&size);
    fields.push_back(bytes.substr(cursor,size));cursor+=size;
  }
  out->swap(fields);return true;
}
inline std::vector<std::string_view> Decode(std::string_view bytes) {
  std::vector<std::string_view> fields;
  Decode(bytes,&fields);
  return fields;
}
} // namespace scratchbird::wire::projection_fields
