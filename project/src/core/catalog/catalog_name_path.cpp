// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_name_path.hpp"

namespace scratchbird::core::catalog {
// SB_NATIVE_NAME_DEFAULTS_ON_ENTRIES (owner Q228 revisited): a vector holds
// no default; the object default is its entry marked default_for_object.
// (Was: default_entry_ from the vector's default_name_entry_uuid.)
void CatalogNameIndex::Add(const CatalogNameVectorView&) {}
void CatalogNameIndex::Add(const CatalogNameEntryView& entry) {
  entries_.insert_or_assign(entry.name_entry_uuid.value, entry);
  if (entry.default_for_language)
    by_language_.emplace(std::pair{entry.object_uuid.value, entry.language_uuid.value}, entry.name_entry_uuid.value);
  if (entry.default_for_object) default_entry_.emplace(entry.object_uuid.value, entry.name_entry_uuid.value);
}
void CatalogNameIndex::Add(const CatalogNamePayloadView& payload) {
  std::visit([&](const auto& value) { Add(value); }, payload);
}
CatalogNamePathName CatalogNameIndex::operator()(const Uuid& object, const Uuid& language) const {
  CatalogNamePathName out;
  const auto [first, last] = by_language_.equal_range({object, language});
  for (auto at = first; at != last; ++at) {
    const auto entry = entries_.find(at->second);
    if (entry == entries_.end()) continue;
    const auto& e = entry->second;
    if (e.object_uuid.value != object || !e.default_for_language || e.language_uuid.value != language) continue;
    if (out.name && out.name->name_entry_uuid.value == e.name_entry_uuid.value) continue;  // added twice
    if (out.name) {
      out.ambiguous = true;
      return out;
    }
    out.name = e;
  }
  if (out.name) return out;
  // The object default: its entry marked default_for_object (re-checked, so a
  // replaced entry drops out; two are ambiguous).
  const auto [dfirst, dlast] = default_entry_.equal_range(object);
  for (auto at = dfirst; at != dlast; ++at) {
    const auto entry = entries_.find(at->second);
    if (entry == entries_.end() || entry->second.object_uuid.value != object || !entry->second.default_for_object) continue;
    if (out.name && out.name->name_entry_uuid.value == entry->second.name_entry_uuid.value) continue;
    if (out.name) {
      out.ambiguous = true;
      return out;
    }
    out.name = entry->second;
  }
  return out;
}

CatalogNamePath ResolveCatalogNamePath(const CatalogNameEntryView& entry, const CatalogNameIndex& names) {
  CatalogNamePath out;
  std::vector<CatalogNamePathComponent> reversed;
  out.error = WalkCatalogNamePath(entry, names, [&](const CatalogNameEntryView& component) {
    reversed.push_back({component});
  });
  if (out.ok()) out.components.assign(reversed.rbegin(), reversed.rend());
  return out;
}

}  // namespace scratchbird::core::catalog
