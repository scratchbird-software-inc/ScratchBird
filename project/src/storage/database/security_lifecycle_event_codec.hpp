// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../core/platform/runtime_platform.hpp"
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scratchbird::storage::database::security_event_codec {
using Uuid = scratchbird::core::platform::Uuid;
using Field = std::variant<std::string, Uuid, std::vector<Uuid>>;
inline constexpr std::string_view kMagic = "SBSECPL2";
inline constexpr std::size_t kMaximumBytes = 16 * 1024 * 1024;
// Positional schema retains fixed field ownership; identity slots never accept text.
inline std::string ExactSchema(std::string_view kind) {
  // All indices include the magic, kind, and creator transaction fields.
  std::size_t count = 0;
  std::vector<std::size_t> identities;
  if (kind == "PRINCIPAL") { count=10; identities={3}; }
  else if (kind == "ROLE") { count=9; identities={3,5}; }
  else if (kind == "GROUP") { count=9; identities={3}; }
  else if (kind == "MEMBERSHIP") { count=10; identities={3,4,5,7}; }
  else if (kind == "GRANT") { count=13; identities={3,4,6,9}; }
  else if (kind == "REVOKE") { count=8; identities={3,4,6}; }
  else if (kind == "AUDIT") { count=12; identities={3,5,6,10}; }
  else if (kind == "CACHE_INVALIDATE") { count=6; identities={4}; }
  else if (kind == "ROW_POLICY") { count=27; identities={3,4,8,12,16,18,21,24}; }
  else if (kind == "PRIVILEGE_TEMPLATE") { count=20; identities={3,5,6,13,14}; }
  else if (kind == "DEFINER_CACHE") { count=9; identities={4,5}; }
  else if (kind == "AUTH_CONTEXT_SUCCESSOR") count=5;
  else return {};
  std::string schema(count, 't');
  for (auto index : identities) schema[index]='u';
  if (kind == "PRIVILEGE_TEMPLATE") schema[8]='l';
  return schema;
}
inline void Put32(std::string& bytes, std::uint32_t value) {
  for (unsigned i=0; i<4; ++i) bytes.push_back(static_cast<char>(value >> (i*8)));
}
inline bool Get32(std::string_view bytes, std::size_t& cursor, std::uint32_t& value) {
  if (cursor > bytes.size() || bytes.size()-cursor < 4) return false;
  value=0;
  for (unsigned i=0; i<4; ++i) value |= std::uint32_t(static_cast<unsigned char>(bytes[cursor++])) << (i*8);
  return true;
}
struct Parts {
  std::vector<Field> fields;
  std::size_t size() const { return fields.size(); }
  const std::string& operator[](std::size_t index) const {
    static const std::string empty;
    if (index >= fields.size()) return empty;
    const auto* text=std::get_if<std::string>(&fields[index]);
    return text ? *text : empty;
  }
  Uuid identity(std::size_t index) const {
    if (index >= fields.size()) return {};
    const auto* value=std::get_if<Uuid>(&fields[index]);
    return value ? *value : Uuid{};
  }
  std::vector<Uuid> identities(std::size_t index) const {
    if (index >= fields.size()) return {};
    const auto* value=std::get_if<std::vector<Uuid>>(&fields[index]);
    return value ? *value : std::vector<Uuid>{};
  }
};
inline bool Valid(const Parts& parts) {
  if (parts.size()<3 || parts[0]!=kMagic) return false;
  const auto schema=ExactSchema(parts[1]);
  if (schema.size()!=parts.size()) return false;
  for (std::size_t i=0; i<schema.size(); ++i)
    if (parts.fields[i].index() != (schema[i]=='t' ? 0u : schema[i]=='u' ? 1u : 2u)) return false;
  return true;
}
inline std::string Encode(std::string kind, std::uint64_t creator, std::vector<Field> values) {
  Parts parts{{std::string(kMagic), std::move(kind), std::to_string(creator)}};
  for (auto& value : values) parts.fields.push_back(std::move(value));
  if (!Valid(parts)) return {};
  std::string bytes(kMagic);
  Put32(bytes, static_cast<std::uint32_t>(parts.size()));
  for (const auto& field : parts.fields) {
    bytes.push_back(static_cast<char>(field.index()));
    if (const auto* text=std::get_if<std::string>(&field)) {
      if (text->size()>kMaximumBytes) return {};
      Put32(bytes, static_cast<std::uint32_t>(text->size())); bytes+=*text;
    } else if (const auto* identity=std::get_if<Uuid>(&field)) {
      bytes.append(reinterpret_cast<const char*>(identity->bytes.data()),16);
    } else {
      const auto& identities=std::get<std::vector<Uuid>>(field);
      if (identities.size()>65536) return {};
      Put32(bytes, static_cast<std::uint32_t>(identities.size()));
      for (const auto& identity : identities) bytes.append(reinterpret_cast<const char*>(identity.bytes.data()),16);
    }
    if (bytes.size()>kMaximumBytes) return {};
  }
  return bytes;
}
inline Parts Decode(std::string_view bytes) {
  if (bytes.size()<12 || bytes.size()>kMaximumBytes || bytes.substr(0,8)!=kMagic) return {};
  std::size_t cursor=8; std::uint32_t count=0;
  if (!Get32(bytes,cursor,count) || count>64) return {};
  Parts parts;
  for (std::uint32_t i=0; i<count; ++i) {
    if (cursor==bytes.size()) return {};
    const auto tag=static_cast<unsigned char>(bytes[cursor++]);
    std::uint32_t size=0;
    if (tag==0) {
      if (!Get32(bytes,cursor,size) || size>bytes.size()-cursor) return {};
      parts.fields.emplace_back(std::string(bytes.substr(cursor,size)));cursor+=size;
    } else if (tag==1) {
      if (bytes.size()-cursor<16) return {};
      Uuid identity; std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()+cursor),16,identity.bytes.begin());cursor+=16;
      parts.fields.emplace_back(identity);
    } else if (tag==2) {
      if (!Get32(bytes,cursor,size) || size>65536 || size>(bytes.size()-cursor)/16) return {};
      std::vector<Uuid> identities(size);
      for (auto& identity : identities) { std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()+cursor),16,identity.bytes.begin());cursor+=16; }
      parts.fields.emplace_back(std::move(identities));
    } else return {};
  }
  return cursor==bytes.size() && Valid(parts) ? parts : Parts{};
}
inline std::string Frame(const std::vector<std::string>& events, std::size_t count = SIZE_MAX) {
  std::string bytes;
  for (std::size_t i=0; i<std::min(count,events.size()); ++i) {
    if (events[i].empty() || events[i].size()>kMaximumBytes) return {};
    Put32(bytes,static_cast<std::uint32_t>(events[i].size()));bytes+=events[i];
    if (bytes.size()>kMaximumBytes) return {};
  }
  return bytes;
}
inline bool Unframe(std::string_view bytes, std::vector<std::string>* output) {
  if (!output || bytes.size()>kMaximumBytes) return false;
  std::vector<std::string> events;
  std::size_t cursor=0;
  while (cursor<bytes.size()) {
    std::uint32_t size=0;
    if (!Get32(bytes,cursor,size) || !size || size>bytes.size()-cursor || !Decode(bytes.substr(cursor,size)).size()) return false;
    events.emplace_back(bytes.substr(cursor,size));cursor+=size;
  }
  *output=std::move(events);return true;
}
} // namespace scratchbird::storage::database::security_event_codec
