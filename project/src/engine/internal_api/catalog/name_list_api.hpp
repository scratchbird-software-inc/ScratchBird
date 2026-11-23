// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "api_diagnostics.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_NAME_LIST_API (Part B step S4, owner rulings 2026-11-20..24).
//
// The engine uses names ONLY in these procedures:
//   NameToUuid   path.name -> UUID      the EXISTING resolver (EngineResolveName); not repeated here.
//   UuidToName   UUID -> path.name      EngineUuidToName below.
//   ListNames    a scope's names        EngineListNames below: "the parser asks for the names within a schema for its
//                                       session language and the engine returns the name for the session language OR
//                                       the default name", so a parser can cache the scope and compare names with ITS
//                                       rules (case, quoting, search path), then send the engine UUIDs.
// Both procedures are MESSAGE VECTORS: UUIDs, flags and name bytes AS STORED; nothing here is rendered, folded,
// translated or interpreted. The quoted fields of an entry are the parser's opaque datum, returned as stored.
//
// Both read the committed name rows of a NATIVE served database through the existing NativeNamesSource (the loop's
// thread-safe cross-thread submit, or the direct path when called from loop work); a failed database refuses, a
// restricted one is read-only and allowed. SB_NAME_SNAPSHOT_OWN_TXN seam: the snapshot is the COMMITTED state, exactly
// as every names read today; the session's own uncommitted names are not in it (the transaction parameter arrives
// with the session's transaction; do not invent one here).

// The most entries one page may carry. A caller asks for 1..this many (0 or more is refused, never clamped).
inline constexpr std::uint32_t kNameListMaximumPageEntries = 1024;

// One name entry of a ListNames page.
struct EngineListedName {
  EngineUuid name_entry_uuid;
  EngineUuid object_uuid;
  EngineUuid language_uuid;            // the entry's own language (differs from the request's when is_fallback)
  std::string name;                    // the stored bytes, exactly as given
  std::uint16_t object_kind = 0;       // CatalogRecordKind (the u16 kind)
  std::uint8_t name_class = 1;         // CatalogNameClass: 1 primary, 2 alias, 3 compatibility, 4 generated, 5 hidden
  bool default_for_language = false;
  bool default_for_object = false;
  bool is_fallback = false;            // the object had no entry in the requested language: this is its default entry
  // The parser's opaque quoted datum, as stored (the engine never interprets it).
  bool was_quoted = false;
  std::uint8_t quote_style = 0;
  bool requires_exact_match = false;
  std::uint8_t lifecycle_state = 2;    // CatalogNameLifecycle::active: a snapshot holds live, active entries only
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t name_resolution_epoch = 0;
};

struct EngineListNamesRequest : EngineApiRequest {
  EngineUuid scope_uuid;       // the schema (or any object that is a name scope); required
  EngineUuid language_uuid;    // the requested language; required (nil refused)
  std::string continuation;    // empty: the first page; else the previous page's next_continuation, unchanged
  std::uint32_t max_entries = 256;  // 1..kNameListMaximumPageEntries
};

struct EngineListNamesResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;  // when !ok: one specific code per cause, structured fields only
  EngineUuid database_uuid;
  EngineUuid scope_uuid;
  EngineUuid language_uuid;
  // Deterministic order: (entry language, name bytes, entry UUID), as bytes.
  std::vector<EngineListedName> entries;
  bool more = false;                 // further pages follow
  std::string next_continuation;     // opaque (the last key plus the scope and language it is valid for); empty when !more
  // True when an object of that UUID has a name in the snapshot (distinguishes an empty scope from an unknown one;
  // an unknown scope is an EMPTY result, not an error).
  bool scope_is_named = false;
  // What the parser compares to know its cache is stale (valid for this call's snapshot):
  //  * names_digest: an order-independent 64-bit digest over EVERY visible name row (entry, object, scope, language,
  //    bytes, class, flags, quoted datum). Any committed name change moves it, including a drop (which writes only a
  //    delete marker the snapshot filters out, so no maximum over rows would move).
  //  * max_catalog_generation_id / max_name_resolution_epoch: the maxima over the rows (information; not a change signal).
  std::uint64_t names_digest = 0;
  std::uint64_t visible_entry_count = 0;
  std::uint64_t max_catalog_generation_id = 0;
  std::uint64_t max_name_resolution_epoch = 0;
};

// ListNames: for each object that has a name DIRECTLY in `scope` (an entry whose scope is it): every entry the object
// has in the requested language, or, when it has none in that language, its default (default_for_object) entry
// marked is_fallback. An object whose names in this scope are neither in the language nor include a default entry
// does not appear. Hidden-class entries are listed with their class (the parser decides). Objects the request's
// metadata-visibility policy hides are not listed.
EngineListNamesResult EngineListNames(const EngineListNamesRequest& request);

// One component of a path.
struct EngineNamePathComponent {
  EngineUuid object_uuid;
  EngineUuid name_entry_uuid;
  EngineUuid language_uuid;        // the language of the entry used
  std::string name;                // stored bytes
  std::uint16_t object_kind = 0;
  std::uint8_t name_class = 1;
  bool is_fallback = false;        // used the object's default name, not one in the requested language
  bool is_database_root = false;   // the database object (the root of the schema tree)
  bool was_quoted = false;         // the parser's opaque quoted datum, as stored
  std::uint8_t quote_style = 0;
  bool requires_exact_match = false;
  // Part B S3: the entry's own scope and the row's generation and epochs (the native EngineMapUuidToName answer needs
  // them: they are the row's, as stored).
  EngineUuid scope_uuid;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
};

struct EngineUuidToNameRequest : EngineApiRequest {
  EngineUuid object_uuid;     // required
  EngineUuid language_uuid;   // required (nil refused)
};

struct EngineUuidToNameResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineUuid database_uuid;
  EngineUuid object_uuid;
  EngineUuid language_uuid;
  // Root side first (the database object), the object itself last. Each component is the name of that object in the
  // requested language or, failing that, its default name (is_fallback). At most the core path cap (64 components
  // below the root, plus the root).
  std::vector<EngineNamePathComponent> components;
  std::uint64_t names_digest = 0;
  std::uint64_t visible_entry_count = 0;
};

// UuidToName: the full path from the database root to the object. Refuses: a nil object or language; an object with
// no (visible) name; an ancestor with no name, an ambiguous default, a cycle, or a chain deeper than the cap.
EngineUuidToNameResult EngineUuidToName(const EngineUuidToNameRequest& request);

// The class text of a native record kind, as the registry's own entries carry it (defined in name_registry.cpp).
std::string NameRegistryObjectClassText(std::uint16_t object_kind);

// The visibility predicate of every public name lookup (defined in name_registry.cpp).
bool NameRegistryObjectPubliclyVisible(const EngineApiRequest& request, const EngineUuid& object_uuid,
                                       std::uint16_t object_kind);

}  // namespace scratchbird::engine::internal_api
