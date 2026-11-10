// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_name_path.hpp"

namespace scratchbird::core::catalog {
void CatalogNameIndex::Add(const CatalogNameVectorView& vector) {
  default_entry_[vector.object_uuid.value] = vector.default_name_entry_uuid.value;
}
void CatalogNameIndex::Add(const CatalogNameEntryView& entry) {
  entries_.insert_or_assign(entry.name_entry_uuid.value, entry);
  if (entry.default_for_language)
    by_language_.emplace(std::pair{entry.object_uuid.value, std::string(entry.language_tag)}, entry.name_entry_uuid.value);
}
void CatalogNameIndex::Add(const CatalogNamePayloadView& payload) {
  std::visit([&](const auto& value) { Add(value); }, payload);
}
CatalogNamePathName CatalogNameIndex::operator()(const Uuid& object, std::string_view language) const {
  CatalogNamePathName out;
  const auto [first, last] = by_language_.equal_range({object, std::string(language)});
  for (auto at = first; at != last; ++at) {
    const auto entry = entries_.find(at->second);
    if (entry == entries_.end()) continue;
    const auto& e = entry->second;
    if (e.object_uuid.value != object || !e.default_for_language || e.language_tag != language) continue;
    if (out.name && out.name->name_entry_uuid.value == e.name_entry_uuid.value) continue;  // added twice
    if (out.name) {
      out.ambiguous = true;
      return out;
    }
    out.name = e;
  }
  if (out.name) return out;
  const auto vector = default_entry_.find(object);
  if (vector == default_entry_.end()) return out;
  if (const auto entry = entries_.find(vector->second); entry != entries_.end()) out.name = entry->second;
  return out;
}

CatalogNamePath ResolveCatalogNamePath(const CatalogNameEntryView& entry, const CatalogNameIndex& names) {
  CatalogNamePath out;
  std::vector<CatalogNamePathComponent> reversed;
  out.error = WalkCatalogNamePath(entry, names, [&](const CatalogNameEntryView& component) {
    reversed.push_back({component.display_name, CatalogNameEntryOwnLaneKey(component)});
  });
  if (out.ok()) out.components.assign(reversed.rbegin(), reversed.rend());
  return out;
}

std::vector<byte> CatalogNamePathLookupKey(const CatalogNamePath& path) {
  std::vector<std::span<const byte>> keys;
  keys.reserve(path.components.size());
  for (const auto& component : path.components) keys.push_back(component.lookup_key);
  return CatalogNameFullPathLookupKey(keys);
}
}  // namespace scratchbird::core::catalog
