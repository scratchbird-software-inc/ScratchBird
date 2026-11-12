// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_name_envelope.hpp"
#include "catalog_name_record_codec.hpp"
#include "catalog_name_tree_keys.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::core::catalog {
// SEARCH_KEY: SB_NATIVE_NAME_PATH (owner, 2026-11-09: the name "should hold the
// full path as part of its row"). The one rule for a name entry's path, used by
// the create, the catalog writer, startup verification and the tests.
//
// The path runs from below the database root to the entry. Each ancestor is
// the scope object of the component after it; its component is its name in the
// entry's language (its entry marked default_for_language), else its default
// entry (the vector's). The chain ends at an entry whose scope is its own object:
// the database's names, which are a path of one component. Every component
// contributes its display name and its own-lane lookup key (the exact key when
// it requires an exact match, else the normalized key). The entry row's
// full_path_lookup_key is the escaped own-lane keys, root side first,
// concatenated (CatalogNameFullPathLookupKey), and path_component_count counts
// them.
// Refusals: an ancestor with no name; an ancestor with two entries marked
// default_for_language in the entry's language (the spec does not say there is
// at most one; the path rule needs it, so two are refused, not picked between:
// owner Q43 = 1, 2026-11-09);
// a scope chain that returns to an object already on it; a chain deeper than
// kCatalogNamePathMaximumDepth.
inline constexpr std::size_t kCatalogNamePathMaximumDepth = 64;

enum class CatalogNamePathError : u8 { none, unnamed_scope, ambiguous_scope, cycle, too_deep, mismatch };

// What a caller's lookup gives for (object, language): the name a path through
// that object uses, or none, or that it is ambiguous.
struct CatalogNamePathName {
  std::optional<CatalogNameEntryView> name;
  bool ambiguous = false;
};

// Calls visit(component) for the entry, then each ancestor toward the root.
// Allocation-free; the lookup decides what it costs.
template <class PathName, class Visit>
CatalogNamePathError WalkCatalogNamePath(const CatalogNameEntryView& entry, const PathName& path_name, Visit&& visit) {
  std::array<Uuid, kCatalogNamePathMaximumDepth> seen{};
  std::size_t depth = 0;
  seen[depth++] = entry.object_uuid.value;
  visit(entry);
  auto scope = entry.scope_uuid.value;
  if (scope == entry.object_uuid.value) return CatalogNamePathError::none;  // the database's own names
  for (;;) {
    const CatalogNamePathName found = path_name(scope, entry.language_uuid.value);
    if (found.ambiguous) return CatalogNamePathError::ambiguous_scope;
    if (!found.name || found.name->object_uuid.value != scope) return CatalogNamePathError::unnamed_scope;
    if (found.name->scope_uuid.value == scope) return CatalogNamePathError::none;  // the database: below its root
    if (std::find(seen.begin(), seen.begin() + static_cast<std::ptrdiff_t>(depth), scope) !=
        seen.begin() + static_cast<std::ptrdiff_t>(depth))
      return CatalogNamePathError::cycle;
    if (depth >= kCatalogNamePathMaximumDepth) return CatalogNamePathError::too_deep;
    seen[depth++] = scope;
    visit(*found.name);
    scope = found.name->scope_uuid.value;
  }
}

namespace detail {
inline std::size_t CatalogNameEscapedSize(std::span<const byte> raw) noexcept {
  return raw.size() + 2 + static_cast<std::size_t>(std::count(raw.begin(), raw.end(), byte{0}));
}
inline bool CatalogNameEscapedEquals(std::span<const byte> raw, std::span<const byte> escaped) noexcept {
  std::size_t at = 0;
  for (const byte b : raw) {
    if (at >= escaped.size() || escaped[at++] != b) return false;
    if (!b && (at >= escaped.size() || escaped[at++] != 0xff)) return false;
  }
  return at + 2 == escaped.size() && escaped[at] == 0 && escaped[at + 1] == 0;
}
}  // namespace detail

// Whether an entry's stored full path (key and count) is the one the rule
// resolves. Allocation-free: sizes first, then each component compared in
// place from the end of the stored key.
template <class PathName>
CatalogNamePathError CheckCatalogNameEntryPath(const CatalogNameEntryView& entry, const PathName& path_name) {
  std::size_t total = 0, count = 0;
  const auto sized = WalkCatalogNamePath(entry, path_name, [&](const CatalogNameEntryView& component) {
    total += detail::CatalogNameEscapedSize(CatalogNameEntryOwnLaneKey(component));
    ++count;
  });
  if (sized != CatalogNamePathError::none) return sized;
  if (count != entry.path_component_count || total != entry.full_path_lookup_key.size())
    return CatalogNamePathError::mismatch;
  std::size_t end = total;
  bool same = true;
  (void)WalkCatalogNamePath(entry, path_name, [&](const CatalogNameEntryView& component) {
    const auto key = CatalogNameEntryOwnLaneKey(component);
    const auto size = detail::CatalogNameEscapedSize(key);
    end -= size;
    same = same && detail::CatalogNameEscapedEquals(key, entry.full_path_lookup_key.subspan(end, size));
  });
  return same ? CatalogNamePathError::none : CatalogNamePathError::mismatch;
}

// The names a caller can see (its live vectors and entries), indexed. Views
// borrow the caller's payloads, which outlive the index. A later Add of the
// same vector or entry UUID replaces the earlier one.
class CatalogNameIndex {
 public:
  void Add(const CatalogNameVectorView& vector);
  void Add(const CatalogNameEntryView& entry);
  void Add(const CatalogNamePayloadView& payload);
  // The lookup the path rule uses.
  CatalogNamePathName operator()(const Uuid& object, const Uuid& language) const;

 private:
  std::map<Uuid, Uuid> default_entry_;  // object -> its vector's default entry
  std::map<Uuid, CatalogNameEntryView> entries_;  // entry UUID -> entry
  // (object, language) -> entry UUIDs ever added as default_for_language there;
  // a lookup re-checks each against entries_, so a replaced entry drops out.
  std::multimap<std::pair<Uuid, Uuid>, Uuid> by_language_;
};

struct CatalogNamePathComponent {
  std::string_view display_name;
  std::span<const byte> lookup_key;  // own lane
};
struct CatalogNamePath {
  CatalogNamePathError error = CatalogNamePathError::none;
  std::vector<CatalogNamePathComponent> components;  // root side first, the entry last
  bool ok() const noexcept { return error == CatalogNamePathError::none; }
};
CatalogNamePath ResolveCatalogNamePath(const CatalogNameEntryView& entry, const CatalogNameIndex& names);
// The full-path lookup key of a resolved path.
std::vector<byte> CatalogNamePathLookupKey(const CatalogNamePath& path);
}  // namespace scratchbird::core::catalog
