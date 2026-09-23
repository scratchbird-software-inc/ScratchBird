// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "binary_catalog_metadata.hpp"
#include <cctype>

namespace scratchbird::engine::internal_api {
using CatalogColumnMetadata = BinaryCatalogMetadata;
inline bool EncodeCatalogColumnMetadata(const CatalogColumnMetadata& fields,
                                        std::string* output) {
  return EncodeBinaryCatalogMetadata(fields, "column.v2", output);
}
inline bool DecodeCatalogColumnMetadata(std::string_view bytes,
                                        CatalogColumnMetadata* output) {
  return DecodeBinaryCatalogMetadata(bytes, "column.v2", output);
}
// Scalar spelling input may contain text attributes, but never UUID text.
// Persisted metadata always uses the framed, typed column schema.
inline bool AdmitCatalogColumnMetadata(std::string_view bytes,
                                       CatalogColumnMetadata* output) {
  if (!output) return false;
  if (bytes.starts_with("SBMETA"))
    return DecodeCatalogColumnMetadata(bytes, output);
  if (bytes.starts_with("SBDOMID2")) {
    if (bytes.size() != 24) return false;
    EngineUuid id;
    std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data() + 8),
                16, id.bytes.begin());
    if (!core::uuid::IsEngineIdentityUuid(id)) return false;
    CatalogColumnMetadata fields;
    fields.identities.emplace("domain_uuid", id);
    *output = std::move(fields);
    return true;
  }
  if (bytes.find('\0') != std::string_view::npos) return false;
  auto trim = [](std::string_view part) {
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.remove_prefix(1);
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) part.remove_suffix(1);
    return part;
  };
  CatalogColumnMetadata fields;
  while (!bytes.empty()) {
    const auto end = bytes.find(';');
    auto part = trim(bytes.substr(0, end));
    if (!part.empty()) {
      const auto equals = part.find('=');
      std::string key(trim(part.substr(0, equals)));
      for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const std::string value = equals == std::string_view::npos
          ? "true" : std::string(trim(part.substr(equals + 1)));
      if (key.empty() || key.ends_with("uuid") ||
          !fields.text.emplace(std::move(key), value).second) return false;
    }
    if (end == std::string_view::npos) break;
    bytes.remove_prefix(end + 1);
  }
  *output = std::move(fields);
  return true;
}
} // namespace scratchbird::engine::internal_api
