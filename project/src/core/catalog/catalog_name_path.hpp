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
// full path as part of its row"; owner Q157 = 1, 2026-11-14: "store path and
// name separately" = the parent reference already in the row). The one rule for
// a name entry's path, used by the create, the catalog writer, startup
// verification and the tests.
//
// The path runs from below the database root to the entry. Each ancestor is
// the scope object of the component after it; its component is its name in the
// entry's language (its entry marked default_for_language), else its default
// entry (the vector's). The chain ends at an entry whose scope is its own object:
// the database's names, which are a path of one component.
// SB_NATIVE_NAME_ROW_V4 (Q157 = 1, plan 35 S2): the path is derived at read by
// walking the scope references; nothing of it is stored, so a rename, drop or
// default change writes only its own row. (Was: every component contributed
// its display name and own-lane key; the entry row stored the escaped own-lane
// keys, root side first, as full_path_lookup_key, with path_component_count,
// and a change regenerated every descendant's copy, SB_NATIVE_NAME_CASCADE.)
// Refusals: an ancestor with no name; an ancestor with two entries marked
// default_for_language in the entry's language (the spec does not say there is
// at most one; the path rule needs it, so two are refused, not picked between:
// owner Q43 = 1, 2026-11-09);
// a scope chain that returns to an object already on it; a chain deeper than
// kCatalogNamePathMaximumDepth.
inline constexpr std::size_t kCatalogNamePathMaximumDepth = 64;

// mismatch: a stored path that was not the walk's; no longer returned since
// SB_NATIVE_NAME_ROW_V4 (kept so the values keep their numbers).
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

// Whether an entry's path walks: every scope named (unambiguously), no cycle,
// at most kCatalogNamePathMaximumDepth components. Allocation-free when the
// lookup is. (Was: also that the stored full path and count were the walk's,
// else mismatch; SB_NATIVE_NAME_ROW_V4 stores none.)
template <class PathName>
CatalogNamePathError CheckCatalogNameEntryPath(const CatalogNameEntryView& entry, const PathName& path_name) {
  return WalkCatalogNamePath(entry, path_name, [](const CatalogNameEntryView&) {});
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

// A component: the entry the walk chose (its raw text is its display under
// preserve_user_text; DeriveCatalogNameEntryKeys gives the display and keys
// under the entry's profile). (Was: its stored display name and own-lane key.)
struct CatalogNamePathComponent {
  CatalogNameEntryView entry;
};
struct CatalogNamePath {
  CatalogNamePathError error = CatalogNamePathError::none;
  std::vector<CatalogNamePathComponent> components;  // root side first, the entry last
  bool ok() const noexcept { return error == CatalogNamePathError::none; }
};
CatalogNamePath ResolveCatalogNamePath(const CatalogNameEntryView& entry, const CatalogNameIndex& names);
}  // namespace scratchbird::core::catalog
