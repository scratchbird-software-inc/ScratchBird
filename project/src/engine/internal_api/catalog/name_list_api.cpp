// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/name_list_api.hpp"

#include "catalog_name_path.hpp"
#include "native_legacy_registry_refusal.hpp"
#include "native_served_database_facts.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace scratchbird::engine::internal_api {
namespace {
namespace c = scratchbird::core::catalog;
using Row = NativeNameRowValue;

EngineApiDiagnostic Refusal(const char* code, const char* key, const EngineRequestContext& context,
                            const char* cause = nullptr) {
  auto diagnostic = MakeEngineApiDiagnostic(code, key, {}, true);
  if (cause != nullptr) diagnostic.fields.push_back({"cause", cause});
  diagnostic.identity_fields.push_back({"database_uuid", context.database_uuid});
  return diagnostic;
}

// One read of the committed rows, or the specific refusal. Admission is the names source's own (serving and
// restricted read; failed and not-serving refuse), checked here first so a marker with no loop refuses the same way.
struct Read {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  NativeNamesSnapshot snapshot;
};
Read ReadRows(const EngineRequestContext& context, const NativeNamesReadFilter& filter = {}) {
  Read out;
  if (!NativeServedContext(context)) {
    out.diagnostic = Refusal("CATALOG.NAMES_LIST_NOT_NATIVE", "catalog.names_list.not_native", context);
    return out;
  }
  const auto& facts = *context.native_served_facts;
  if (!context.database_uuid.is_nil() && context.database_uuid != facts.database_uuid) {
    out.diagnostic = Refusal("CATALOG.NAMES_LIST_DATABASE_MISMATCH", "catalog.names_list.database_mismatch", context);
    return out;
  }
  if (facts.CurrentServingState() == NativeServedServingState::failed) {
    out.diagnostic = Refusal(NativeNamesSnapshotErrorCode(NativeNamesSnapshotError::database_failed),
                             "engine.native.names_snapshot_refused", context,
                             NativeNamesSnapshotErrorName(NativeNamesSnapshotError::database_failed));
    return out;
  }
  if (!facts.names_source) {
    out.diagnostic = Refusal(NativeNamesSnapshotErrorCode(NativeNamesSnapshotError::names_source_failure),
                             "engine.native.names_snapshot_refused", context,
                             NativeNamesSnapshotErrorName(NativeNamesSnapshotError::names_source_failure));
    return out;
  }
  out.snapshot = facts.names_source->Read(filter);
  if (!out.snapshot.ok()) {
    out.diagnostic = Refusal(NativeNamesSnapshotErrorCode(out.snapshot.error), "engine.native.names_snapshot_refused",
                             context, NativeNamesSnapshotErrorName(out.snapshot.error));
    out.snapshot.rows.clear();
    return out;
  }
  out.ok = true;
  return out;
}

// (The row digest and the snapshot digest moved to native_served_database_facts.hpp, NativeNameRowDigest: the source computes
// them over every committed name row while it streams, whatever scope the caller asked for.)

int CompareUuid(const core::platform::Uuid& a, const core::platform::Uuid& b) {
  return std::memcmp(a.bytes.data(), b.bytes.data(), 16);
}

// The sort key of a listed entry: (entry language, name bytes, entry UUID), as bytes.
bool KeyLess(const Row& a, const Row& b) {
  if (const int l = CompareUuid(a.language_uuid, b.language_uuid)) return l < 0;
  if (a.raw_name_text != b.raw_name_text) return a.raw_name_text < b.raw_name_text;
  return CompareUuid(a.name_entry_uuid, b.name_entry_uuid) < 0;
}

// Continuation token: "SBN1" | scope(16) | language(16) | last language(16) | last entry(16) | last name bytes.
constexpr char kTokenMagic[4] = {'S', 'B', 'N', '1'};
constexpr std::size_t kTokenFixed = 4 + 16 * 4;
std::string EncodeToken(const core::platform::Uuid& scope, const core::platform::Uuid& language, const Row& last) {
  std::string token(kTokenMagic, 4);
  for (const auto* id : {&scope, &language, &last.language_uuid, &last.name_entry_uuid})
    token.append(reinterpret_cast<const char*>(id->bytes.data()), 16);
  token += last.raw_name_text;
  return token;
}

using UuidKey = decltype(core::platform::Uuid{}.bytes);

EngineListedName Listed(const Row& r, bool fallback) {
  EngineListedName e;
  e.name_entry_uuid = r.name_entry_uuid;
  e.object_uuid = r.object_uuid;
  e.language_uuid = r.language_uuid;
  e.name = r.raw_name_text;
  e.object_kind = r.object_kind;
  e.name_class = r.name_class;
  e.default_for_language = r.default_for_language;
  e.default_for_object = r.default_for_object;
  e.is_fallback = fallback;
  e.was_quoted = r.was_quoted;
  e.quote_style = r.quote_style;
  e.requires_exact_match = r.requires_exact_match;
  e.catalog_generation_id = r.catalog_generation_id;
  e.name_resolution_epoch = r.name_resolution_epoch;
  return e;
}
}  // namespace

EngineListNamesResult EngineListNames(const EngineListNamesRequest& request) {
  EngineListNamesResult result;
  const auto& context = request.context;
  result.scope_uuid = request.scope_uuid;
  result.language_uuid = request.language_uuid;
  if (request.scope_uuid.is_nil()) {
    result.diagnostic = Refusal("CATALOG.NAMES_LIST_SCOPE_REQUIRED", "catalog.names_list.scope_required", context);
    return result;
  }
  if (request.language_uuid.is_nil()) {
    result.diagnostic = Refusal("CATALOG.NAMES_LIST_LANGUAGE_REQUIRED", "catalog.names_list.language_required", context);
    return result;
  }
  if (request.max_entries == 0 || request.max_entries > kNameListMaximumPageEntries) {
    result.diagnostic = Refusal("CATALOG.NAMES_LIST_PAGE_SIZE_INVALID", "catalog.names_list.page_size_invalid", context);
    result.diagnostic.fields.push_back({"maximum_page_entries", std::to_string(kNameListMaximumPageEntries)});
    return result;
  }
  const Row* after = nullptr;
  Row after_key;
  if (!request.continuation.empty()) {
    const auto& t = request.continuation;
    if (t.size() < kTokenFixed || std::memcmp(t.data(), kTokenMagic, 4) != 0) {
      result.diagnostic = Refusal("CATALOG.NAMES_LIST_CONTINUATION_INVALID", "catalog.names_list.continuation_invalid", context);
      return result;
    }
    core::platform::Uuid scope, language;
    std::memcpy(scope.bytes.data(), t.data() + 4, 16);
    std::memcpy(language.bytes.data(), t.data() + 20, 16);
    if (scope != request.scope_uuid || language != request.language_uuid) {
      result.diagnostic = Refusal("CATALOG.NAMES_LIST_CONTINUATION_MISMATCH", "catalog.names_list.continuation_mismatch", context);
      return result;
    }
    std::memcpy(after_key.language_uuid.bytes.data(), t.data() + 36, 16);
    std::memcpy(after_key.name_entry_uuid.bytes.data(), t.data() + 52, 16);
    after_key.raw_name_text.assign(t.data() + kTokenFixed, t.size() - kTokenFixed);
    after = &after_key;
  }
  // SB_NAMES_SCOPE_ARGUMENT: only this scope's rows (and the scope object's own names, for scope_is_named) are retained;
  // the digest, the entry count and the generation maxima are the source's, over every committed name row.
  NativeNamesReadFilter filter;
  filter.all_scopes = false;
  filter.scopes.push_back(request.scope_uuid);
  filter.objects.push_back(request.scope_uuid);
  auto read = ReadRows(context, filter);
  if (!read.ok) {
    result.diagnostic = std::move(read.diagnostic);
    return result;
  }
  const auto& rows = read.snapshot.rows;
  result.database_uuid = read.snapshot.database_uuid;
  result.names_digest = read.snapshot.names_digest;
  result.visible_entry_count = read.snapshot.visible_entry_count;
  result.max_catalog_generation_id = read.snapshot.max_catalog_generation_id;
  result.max_name_resolution_epoch = read.snapshot.max_name_resolution_epoch;
  for (const auto& row : rows)
    if (row.object_uuid == request.scope_uuid) result.scope_is_named = true;

  // The objects with a name directly in the scope: their entries in the language, else their default entry.
  struct Candidate {
    const Row* row;
    bool fallback;
  };
  struct Group {
    std::vector<const Row*> in_language;
    const Row* object_default = nullptr;
  };
  std::map<UuidKey, Group> groups;
  for (const auto& row : rows) {
    if (row.scope_uuid != request.scope_uuid) continue;
    auto& group = groups[row.object_uuid.bytes];
    if (row.language_uuid == request.language_uuid) group.in_language.push_back(&row);
    if (row.default_for_object) group.object_default = &row;
  }
  std::vector<Candidate> candidates;
  for (const auto& [object, group] : groups) {
    (void)object;
    const Row* any = !group.in_language.empty() ? group.in_language.front() : group.object_default;
    if (any == nullptr) continue;
    if (!NameRegistryObjectPubliclyVisible(request, any->object_uuid, any->object_kind)) continue;
    if (!group.in_language.empty()) {
      for (const auto* row : group.in_language) candidates.push_back({row, false});
    } else {
      candidates.push_back({group.object_default, true});
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return KeyLess(*a.row, *b.row); });
  auto from = candidates.begin();
  if (after != nullptr)
    from = std::upper_bound(candidates.begin(), candidates.end(), *after,
                            [](const Row& key, const Candidate& c) { return KeyLess(key, *c.row); });
  const auto remaining = static_cast<std::size_t>(candidates.end() - from);
  const auto take = std::min<std::size_t>(remaining, request.max_entries);
  result.entries.reserve(take);
  for (std::size_t i = 0; i < take; ++i) result.entries.push_back(Listed(*from[i].row, from[i].fallback));
  result.more = take < remaining;
  if (result.more) result.next_continuation = EncodeToken(request.scope_uuid, request.language_uuid, *from[take - 1].row);
  result.ok = true;
  return result;
}

EngineUuidToNameResult EngineUuidToName(const EngineUuidToNameRequest& request) {
  EngineUuidToNameResult result;
  const auto& context = request.context;
  result.object_uuid = request.object_uuid;
  result.language_uuid = request.language_uuid;
  if (request.object_uuid.is_nil()) {
    result.diagnostic = Refusal("CATALOG.NAMES_PATH_OBJECT_REQUIRED", "catalog.names_path.object_required", context);
    return result;
  }
  if (request.language_uuid.is_nil()) {
    result.diagnostic = Refusal("CATALOG.NAMES_PATH_LANGUAGE_REQUIRED", "catalog.names_path.language_required", context);
    return result;
  }
  auto read = ReadRows(context);
  if (!read.ok) {
    result.diagnostic = std::move(read.diagnostic);
    return result;
  }
  // All scopes: the path walk climbs through the ancestors' scopes, which are not known before the walk.
  const auto& rows = read.snapshot.rows;
  result.database_uuid = read.snapshot.database_uuid;
  result.names_digest = read.snapshot.names_digest;
  result.visible_entry_count = read.snapshot.visible_entry_count;

  // The existing path rule (core/catalog WalkCatalogNamePath) over the committed rows: its lookup is the existing
  // CatalogNameIndex (the entry marked default_for_language in the language, else the entry marked default_for_object).
  // The walk asks each ancestor in the ENTRY's language; this lookup ignores that argument and asks in the REQUESTED
  // language, which is the owner's "the name for the session language OR the default name" for every component.
  c::CatalogNameIndex index;
  std::map<UuidKey, const Row*> by_entry;
  for (const auto& row : rows) {
    c::CatalogNameEntryView view;
    view.name_entry_uuid = {core::platform::UuidKind::object, row.name_entry_uuid};
    view.object_uuid = {core::platform::UuidKind::object, row.object_uuid};
    view.scope_uuid = {core::platform::UuidKind::object, row.scope_uuid};
    view.language_uuid = c::CatalogLanguageIdentity(row.language_uuid);
    view.object_kind = static_cast<c::CatalogRecordKind>(row.object_kind);
    view.name_class = static_cast<c::CatalogNameClass>(row.name_class);
    view.raw_name_text = row.raw_name_text;
    view.default_for_language = row.default_for_language;
    view.default_for_object = row.default_for_object;
    index.Add(view);
    by_entry[row.name_entry_uuid.bytes] = &row;
  }
  const auto language = request.language_uuid;
  const auto in_language = [&](const core::platform::Uuid& object, const core::platform::Uuid&) {
    return index(object, language);
  };
  const auto own = index(request.object_uuid, language);
  if (own.ambiguous) {
    result.diagnostic = Refusal("CATALOG.NAMES_PATH_NAME_AMBIGUOUS", "catalog.names_path.name_ambiguous", context);
    return result;
  }
  if (!own.name || !NameRegistryObjectPubliclyVisible(request, request.object_uuid,
                                                      static_cast<std::uint16_t>(own.name->object_kind))) {
    result.diagnostic = Refusal("CATALOG.NAMES_PATH_OBJECT_NOT_FOUND", "catalog.names_path.object_not_found", context);
    return result;
  }
  std::vector<c::CatalogNameEntryView> reversed;
  const auto walked = c::WalkCatalogNamePath(*own.name, in_language,
                                             [&](const c::CatalogNameEntryView& e) { reversed.push_back(e); });
  switch (walked) {
    case c::CatalogNamePathError::none: break;
    case c::CatalogNamePathError::unnamed_scope:
      result.diagnostic = Refusal("CATALOG.NAMES_PATH_ANCESTOR_UNNAMED", "catalog.names_path.ancestor_unnamed", context);
      return result;
    case c::CatalogNamePathError::ambiguous_scope:
      result.diagnostic = Refusal("CATALOG.NAMES_PATH_ANCESTOR_AMBIGUOUS", "catalog.names_path.ancestor_ambiguous", context);
      return result;
    case c::CatalogNamePathError::cycle:
      result.diagnostic = Refusal("CATALOG.NAMES_PATH_CYCLE", "catalog.names_path.cycle", context);
      return result;
    case c::CatalogNamePathError::too_deep:
      result.diagnostic = Refusal("CATALOG.NAMES_PATH_TOO_DEEP", "catalog.names_path.too_deep", context);
      return result;
  }
  // The walk stops below the database root (it does not visit it); the root is the last component's scope. A path of
  // one whose scope is its own object IS the root's own name.
  std::vector<c::CatalogNameEntryView> chain(reversed.rbegin(), reversed.rend());
  const bool own_is_root = own.name->scope_uuid.value == own.name->object_uuid.value;
  if (!own_is_root) {
    const auto root = index(chain.front().scope_uuid.value, language);
    if (!root.name || root.ambiguous) {
      result.diagnostic = Refusal("CATALOG.NAMES_PATH_ANCESTOR_UNNAMED", "catalog.names_path.ancestor_unnamed", context);
      return result;
    }
    chain.insert(chain.begin(), *root.name);
  }
  for (std::size_t i = 0; i < chain.size(); ++i) {
    const auto& e = chain[i];
    const Row& row = *by_entry.at(e.name_entry_uuid.value.bytes);
    if (!NameRegistryObjectPubliclyVisible(request, row.object_uuid, row.object_kind)) {
      result.diagnostic = Refusal("CATALOG.NAMES_PATH_OBJECT_NOT_FOUND", "catalog.names_path.object_not_found", context);
      result.components.clear();
      return result;
    }
    EngineNamePathComponent component;
    component.object_uuid = row.object_uuid;
    component.name_entry_uuid = row.name_entry_uuid;
    component.language_uuid = row.language_uuid;
    component.name = row.raw_name_text;
    component.object_kind = row.object_kind;
    component.name_class = row.name_class;
    component.is_fallback = row.language_uuid != language;
    component.is_database_root = i == 0;
    component.was_quoted = row.was_quoted;
    component.quote_style = row.quote_style;
    component.requires_exact_match = row.requires_exact_match;
    result.components.push_back(std::move(component));
  }
  result.ok = true;
  return result;
}

}  // namespace scratchbird::engine::internal_api
