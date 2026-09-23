// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_object_lifecycle_codec.hpp"
#include <map>

namespace scratchbird::engine::internal_api {
// UUID references have a separate typed section. Opaque expression/name/hash
// data remains length framed; no identity is inferred from a string value.
struct BinaryCatalogMetadata {
  std::map<std::string, std::string> text;
  std::map<std::string, EngineUuid> identities;
};
inline bool EncodeBinaryCatalogMetadata(const BinaryCatalogMetadata& fields, std::string_view schema, std::string* output) {
  if (!output || schema.empty() || schema.size() > 256 || fields.text.size() + fields.identities.size() > 65536) return false;
  std::string bytes = "SBMETA02";
  if (!AppendBinaryString(&bytes, schema)) return false;
  AppendBinaryU32(&bytes, static_cast<std::uint32_t>(fields.text.size()));
  AppendBinaryU32(&bytes, static_cast<std::uint32_t>(fields.identities.size()));
  for (const auto& [key, value] : fields.text) {
    if (key.empty() || key.ends_with("uuid") || fields.identities.contains(key) ||
        !catalog_record_codec::Put(bytes, key) || !catalog_record_codec::Put(bytes, value)) return false;
  }
  for (const auto& [key, value] : fields.identities) {
    if (key.empty() || !catalog_record_codec::Put(bytes, key) || !catalog_record_codec::Put(bytes, value)) return false;
  }
  if (bytes.size() > kApiBehaviorRecordMaximumBytes) return false;
  output->swap(bytes); return true;
}
inline bool DecodeBinaryCatalogMetadata(std::string_view bytes, std::string_view schema, BinaryCatalogMetadata* output) {
  if (!output || bytes.size() < 20 || bytes.size() > kApiBehaviorRecordMaximumBytes || bytes.substr(0,8) != "SBMETA02") return false;
  const std::span<const std::uint8_t> input(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  std::size_t cursor = 8;
  std::string stored_schema;
  if (!ReadBinaryString(input, &cursor, &stored_schema) || stored_schema != schema) return false;
  std::uint32_t texts = 0, identities = 0;
  if (!ReadBinaryU32(input, &cursor, &texts) || !ReadBinaryU32(input, &cursor, &identities) ||
      texts > 65536 || identities > 65536 - texts) return false;
  BinaryCatalogMetadata fields;
  for (std::uint32_t n=0; n<texts; ++n) {
    std::string key, value;
    if (!catalog_record_codec::Get(input, cursor, key) || !catalog_record_codec::Get(input, cursor, value) ||
        key.empty() || key.ends_with("uuid") || !fields.text.emplace(key, std::move(value)).second) return false;
  }
  for (std::uint32_t n=0; n<identities; ++n) {
    std::string key; EngineUuid value;
    if (!catalog_record_codec::Get(input, cursor, key) || !catalog_record_codec::Get(input, cursor, value) ||
        key.empty() || fields.text.contains(key) || !fields.identities.emplace(key, value).second) return false;
  }
  if (cursor != input.size()) return false;
  *output = std::move(fields); return true;
}
inline EngineUuid BinaryCatalogUuid(const BinaryCatalogMetadata& fields, const std::string& key,
                                         EngineUuid fallback = {}) {
  const auto it = fields.identities.find(key);
  return it == fields.identities.end() || it->second.is_nil() ? fallback : it->second;
}
}  // namespace scratchbird::engine::internal_api
