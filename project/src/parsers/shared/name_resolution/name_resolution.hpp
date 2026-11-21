// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_PARSER_NAME_RESOLUTION (Part B step S1, owner rulings
// 2026-11-20..22). The parser-side name resolver.
//
// WHY HERE. A name is the parser's business: the engine stores a name as
// given and indexes (scope UUID, language, name); how two presented names are
// compared, which scope and language are tried and in what order, and what
// "the same name" means are the DIALECT's rules, and different parsers have
// different rules. This module lives under parsers/shared (outside the engine
// namespace, depending on the C++ standard library only) so that every parser
// can use it and none of the rules sits in the engine. A dialect supplies its
// rule through NameFoldRule; only the SBsql rule is implemented (the
// emulation parsers are skeletons until the design stabilises).
//
// WHAT IT REPRODUCES. S1 changes no engine code. Each rule below mirrors what
// the engine's resolver does TODAY (the source line is named at the rule), so
// that sending the module's spelling over the CURRENT engine call gives the
// answer the raw typed spelling gives. S2 then removes the engine's copy of
// the rules; nothing in this module changes for that.
//
//  * The comparison key of one presented component: the spelling when quoted,
//    the spelling folded to UPPER case (ASCII letters only) when unquoted
//    (owner Q177, Q180-Q183; engine mirror name_registry.cpp NameRegistryLookupKey
//    + ProfileFold, the "sbsql_v3" label).
//  * A stored name's own lane: its exact spelling when it was created quoted,
//    its fold when it was created unquoted (name_registry.cpp
//    NameRegistryEntryOwnLaneKey). A presented name finds a stored name when
//    its key equals that lane (NameRegistryEntryMatchesLookupKey): so
//    unquoted hello finds Hello, HELLO and a quoted "HELLO" but not a
//    quoted-only "Hello"; quoted "HELLO" finds an unquoted-created Hello.
//  * Twins (NameRegistryLookupKeysCollide): two names collide when their own
//    lanes are equal, so case-only twins coexist only when both are quoted.
//  * Language: session language, then the database default, then "und"
//    (name_registry.cpp LanguageCandidates); the first language with any
//    match wins.
//  * Scope: target schema, current schema, the search path in order, the
//    default root, then the nil scope (name_registry.cpp ScopeCandidates). A
//    qualified path walks schema by schema (ResolveQualifiedNameRegistryScopes).
//  * More than one distinct object matching is an ambiguity, never a pick.
//
// NOT IN THE ENGINE TODAY, SO NOT HERE: the owner's "quoted names are
// evaluated first and block ambiguous unquoted resolution". The engine has no
// quoted-first stage (it reports an ambiguity), so this module reports the
// same ambiguity. See the S1 report.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::parsers::name_resolution {

// A binary UUID (16 bytes, all zero = nil). The module needs no UUID library.
using Uuid16 = std::array<std::uint8_t, 16>;
inline bool IsNil(const Uuid16& id) {
  for (const auto byte : id)
    if (byte != 0) return false;
  return true;
}

// ---- the dialect rule -----------------------------------------------------------------------------------------------

// One dialect's rule for the comparison key of a presented name component.
// SBsql: SbsqlNameFoldRule(). Another parser supplies its own (e.g. a
// PostgreSQL-style lower fold) without touching anything else here.
class NameFoldRule {
 public:
  virtual ~NameFoldRule() = default;
  virtual std::string_view Label() const noexcept = 0;
  // The key of ONE component: how the dialect compares it.
  virtual std::string Key(std::string_view spelling, bool quoted) const = 0;
};

// The SBsql comparison key (owner Q177, Q180-Q183, Q185 = 1): the spelling
// when quoted, the spelling folded to upper case when unquoted. Only the ASCII
// letters fold (Q182) until a case-folding table is a declared seed resource.
// This is THE one copy: NativeIdentifierComponentKey (the SBsql AST) calls it.
inline std::string SbsqlIdentifierKey(std::string_view spelling, bool quoted) {
  std::string key(spelling);
  if (quoted) return key;
  for (auto& ch : key)
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  return key;
}

const NameFoldRule& SbsqlNameFoldRule() noexcept;

// ---- presented names ---------------------------------------------------------------------------------------------------

struct NameComponent {
  std::string spelling;
  bool quoted = false;  // quoted in the source, or the whole name was marked quoted
};
inline bool operator==(const NameComponent& a, const NameComponent& b) {
  return a.spelling == b.spelling && a.quoted == b.quoted;
}

// Splits presented dotted text into components. Mirrors, rule for rule, the
// splitter the parser server runs on the same text today (server/ipc_server.cpp
// PsNameSplitPresentedName): a double quote opens/closes a quoted run, "" inside
// a run is one quote, a dot outside a run separates components, a component is
// quoted when any of it was in quotes or the request marked the whole name
// quoted. std::nullopt where that splitter refuses: an empty component (so the
// empty name "", "a..b", ".a" and "a." are refused HERE exactly as there) or an
// unterminated quote. The caller sends the text unchanged in that case and the
// server decides, as today; this module invents no refusal of its own.
std::optional<std::vector<NameComponent>> SplitPresentedName(std::string_view presented_name,
                                                             bool request_quoted);

// Renders components as dotted text: an unquoted component bare, a quoted one
// in double quotes with each inner quote doubled (parsers/sbsql_worker/wire
// EncodeQualifiedPresentedName's form).
std::string RenderPresentedName(const std::vector<NameComponent>& components);

// ---- what a parser sends to the engine ------------------------------------------------------------------------------

// The presented name a parser sends over the CURRENT wire (schemas 7001-7008,
// unchanged) or to the embedded server frame. Every unquoted component is
// already folded by the rule; every quoted component is exact. The engine
// still folds unquoted text itself (idempotent on a folded SBsql name), S2
// removes that. `quoted` is passed through untouched.
struct PreparedWireName {
  std::string presented_name;  // what is sent
  bool quoted = false;         // what is sent
  std::string original;        // what the caller typed
  bool rewritten = false;      // presented_name differs from original

  // The server echoes the presented text back as the canonical name of a
  // resolved name. The caller's cache and diagnostics always held the TYPED
  // text there, so a reply that echoes the text this module sent is mapped
  // back to the typed text; any other reply (an engine-owned name) is kept.
  std::string RestoreEcho(std::string returned) const {
    if (rewritten && returned == presented_name) return original;
    return returned;
  }
};

// Folds each component of `presented_name` by `rule`. Unchanged (rewritten =
// false) when the text does not split (see SplitPresentedName) and for the two
// engine RESOURCE classes, charset and collation: those names are resolved by
// the engine's resource seed catalogue (name_resolution_api.cpp
// ResolveEngineResourceName, which reads the raw text and never the registry
// lanes), are not SQL identifiers of a schema, and are not folded by any
// registry rule.
PreparedWireName PrepareWireName(const NameFoldRule& rule, std::string_view presented_name, bool quoted,
                                 std::string_view object_class);
PreparedWireName PrepareSbsqlWireName(std::string_view presented_name, bool quoted, std::string_view object_class);

// ---- the stored names a resolution reads ------------------------------------------------------------------------------

// One name row as a source supplies it TODAY: the engine row still carries the
// quoted flag (the own lane depends on it). Whether the rows keep it is a
// Part B question (see the S1 report); a source that supplies a stored KEY
// instead sets was_quoted = true and raw_text = the key.
struct NameEntry {
  Uuid16 scope{};
  Uuid16 object{};
  std::string language;      // the language tag (today's representation)
  std::string object_class;  // today's class text
  std::string raw_text;      // the name as stored
  bool was_quoted = false;   // created quoted
};

// The own lane of a stored name under a rule (engine mirror NameRegistryEntryOwnLaneKey).
std::string OwnLane(const NameFoldRule& rule, const NameEntry& entry);
// Two names collide when their own lanes are equal (engine mirror NameRegistryLookupKeysCollide).
bool NamesCollide(const NameFoldRule& rule, const NameEntry& a, const NameEntry& b);

// Where the stored names come from. S1: a source that reads what the engine
// exposes today. S4: the engine's ListNames. The module never reads a file.
class NameSource {
 public:
  virtual ~NameSource() = default;
  // The names of one (scope, language) page, as of the caller's snapshot.
  virtual std::vector<NameEntry> List(const Uuid16& scope, std::string_view language) = 0;
};

// The parser's list cache: one page per (scope, language), refreshed after a
// commit or rollback (the snapshot the parser reads moves with its
// transaction), and when the catalog generation a reply carried changes.
class NameListCache {
 public:
  explicit NameListCache(NameSource& source) : source_(&source) {}
  const std::vector<NameEntry>& Page(const Uuid16& scope, const std::string& language);
  // Refresh hooks: the parser calls one after its own commit or rollback.
  void OnTransactionEnd() { pages_.clear(); }
  // Drops the pages when the generation differs from the last one seen.
  void ObserveGeneration(std::uint64_t generation);
  std::size_t source_reads() const noexcept { return source_reads_; }
  std::size_t cached_pages() const noexcept { return pages_.size(); }

 private:
  NameSource* source_;
  std::map<std::pair<Uuid16, std::string>, std::vector<NameEntry>> pages_;
  std::optional<std::uint64_t> generation_;
  std::size_t source_reads_ = 0;
};

// ---- the resolution -----------------------------------------------------------------------------------------------------

// What the session knows: its scopes (as UUIDs) and its languages (as tags,
// today's representation; the engine's wire still carries tags).
struct ResolutionContext {
  Uuid16 target_schema{};
  Uuid16 current_schema{};
  std::vector<Uuid16> search_path;  // in search order
  Uuid16 default_root{};
  std::string session_language;     // empty: the default language
  std::string default_language;     // empty: "en" (name_registry.cpp NameRegistryDefaultLanguage)
};

// The language candidates in order: session, database default, "und".
std::vector<std::string> LanguageCandidates(const ResolutionContext& context);
// The scope candidates in order, de-duplicated: target, current, search path, default root, nil.
std::vector<Uuid16> ScopeCandidates(const ResolutionContext& context);

enum class ResolveStatus { found, not_found, ambiguous, invalid_request };

struct ResolveOutcome {
  ResolveStatus status = ResolveStatus::not_found;
  NameEntry entry;                 // the matched name (found)
  std::vector<Uuid16> objects;     // every distinct matching object (found: one; ambiguous: several)
  // The key sent for each step of the path (folded when unquoted, exact when quoted).
  std::vector<std::string> keys;
};

struct ResolveRequest {
  std::vector<NameComponent> path;  // the qualifier steps, then the leaf last
  // Which stored classes the leaf may be; empty: any. The engine's
  // ObjectClassMatchesRequest maps request classes onto entry classes; the
  // caller owns that mapping.
  std::function<bool(const std::string&)> accepts_class;
};

// path -> object. The engine's order, rule for rule: qualifier steps walk the
// schema entries (ResolveQualifiedNameRegistryScopes), then the leaf is looked
// for per language, per scope (ResolveNameRegistryPrivate).
//  * an empty leaf is invalid_request (the engine: name_required);
//  * an empty qualifier step finds nothing (the engine: the scope walk gives {});
//  * several distinct objects in the first language that matches: ambiguous.
ResolveOutcome Resolve(const NameFoldRule& rule, NameListCache& names, const ResolutionContext& context,
                       const ResolveRequest& request);

}  // namespace scratchbird::parsers::name_resolution
