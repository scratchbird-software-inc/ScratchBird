// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_NAME_LIST_TEST (Part B step S4, owner rulings 2026-11-20..24).
//
// ListNames and UuidToName (engine/internal_api/catalog/name_list_api.*) on PRODUCTION-CREATED native databases of one
// page size: the file is made by the real create path, extra names by the production catalog writers (the harness of
// native_name_resolve_test.cpp, which this file includes: create / forge / serve children, the offline ground-truth
// read, the served loop). Every served answer is compared with the OFFLINE read of the same file (an independent
// reader of the committed rows), never with itself. Driver:
//   size <bytes> [state...]   states: serving restricted failed names dead uncommitted many flow (default: all)
//   create|forge|serve ...    the harness children (the strace driver uses these directly)
//   listforge <path> <kind> <bootstrap> <service>   kinds: many, drop, add (production writers)
// State "flow" serves ONE file three times (names, then a committed drop, then a committed add) and requires the
// names digest to differ at every step: the cache-refresh signal a parser compares.

#define SB_NAME_LIST_TEST 1
#define main name_resolve_driver_main
#include "native_name_resolve_test.cpp"
#undef main

#include "engine/internal_api/catalog/name_list_api.hpp"

namespace {
constexpr std::string_view kBulkPrefix = "bulk_";
constexpr std::string_view kHiddenList = "list_hidden_name";
constexpr std::string_view kAddedLater = "Added_Later";
const auto kProcessStart = std::chrono::steady_clock::now();
constexpr int kBulkNames = 400;  // the names scan reads at most 4 MiB of leaf images (about 1500 of these rows in all): see the S4 report

// ---- helpers -----------------------------------------------------------------------------------------------------

eng::EngineListNamesRequest ListRequest(const p::Uuid& scope, const p::Uuid& language, std::uint32_t max,
                                        const std::string& token = {}) {
  eng::EngineListNamesRequest r;
  r.context.native_served_facts = env.facts;
  r.context.database_uuid = ToEngine(env.database);
  r.scope_uuid = ToEngine(scope);
  r.language_uuid = ToEngine(language);
  r.continuation = token;
  r.max_entries = max;
  return r;
}
eng::EngineUuidToNameRequest PathRequest(const p::Uuid& object, const p::Uuid& language) {
  eng::EngineUuidToNameRequest r;
  r.context.native_served_facts = env.facts;
  r.context.database_uuid = ToEngine(env.database);
  r.object_uuid = ToEngine(object);
  r.language_uuid = ToEngine(language);
  return r;
}
std::string CodeOf(const eng::EngineApiDiagnostic& d) { return d.code; }

struct Listing {
  std::vector<eng::EngineListedName> entries;
  std::size_t pages = 0;
  std::uint64_t digest = 0, count = 0;
  bool scope_is_named = false;
};
// Every page of one (scope, language) at one page size; each page is checked on the way.
Listing ListAll(const p::Uuid& scope, const p::Uuid& language, std::uint32_t page, const eng::EngineApiRequest* policy = nullptr) {
  Listing out;
  std::string token;
  for (;;) {
    auto request = ListRequest(scope, language, page, token);
    if (policy != nullptr) request.policy_profile = policy->policy_profile;
    const auto result = eng::EngineListNames(request);
    Check(result.ok, "a page: " + CodeOf(result.diagnostic));
    Check(result.entries.size() <= page, "a page never exceeds the requested size");
    Check(result.more == !result.next_continuation.empty(), "more iff a continuation is returned");
    if (out.pages == 0) {
      out.digest = result.names_digest;
      out.count = result.visible_entry_count;
      out.scope_is_named = result.scope_is_named;
    } else {
      Check(result.names_digest == out.digest, "the snapshot did not change between pages (digest)");
    }
    ++out.pages;
    out.entries.insert(out.entries.end(), result.entries.begin(), result.entries.end());
    if (!result.more) break;
    Check(!result.entries.empty(), "a page that has more is not empty");
    token = result.next_continuation;
  }
  return out;
}

// The OFFLINE expectation of ListNames: the file's own committed entries, the rule applied independently.
struct Expected {
  const NameFact* fact;
  bool fallback;
};
std::vector<Expected> ExpectedList(const Offline& offline, const p::Uuid& scope, const p::Uuid& language) {
  std::map<p::Uuid, std::vector<const NameFact*>> by_object;
  for (const auto& n : offline.names)
    if (n.scope == scope) by_object[n.object].push_back(&n);
  std::vector<Expected> out;
  for (const auto& [object, entries] : by_object) {
    (void)object;
    std::vector<const NameFact*> in_language;
    const NameFact* object_default = nullptr;
    for (const auto* e : entries) {
      if (e->language == language) in_language.push_back(e);
      if (e->default_for_object) object_default = e;
    }
    if (!in_language.empty()) {
      for (const auto* e : in_language) out.push_back({e, false});
    } else if (object_default != nullptr) {
      out.push_back({object_default, true});
    }
  }
  std::sort(out.begin(), out.end(), [](const Expected& a, const Expected& b) {
    if (a.fact->language != b.fact->language) return std::memcmp(a.fact->language.bytes.data(), b.fact->language.bytes.data(), 16) < 0;
    if (a.fact->text != b.fact->text) return a.fact->text < b.fact->text;
    return std::memcmp(a.fact->entry_uuid.bytes.data(), b.fact->entry_uuid.bytes.data(), 16) < 0;
  });
  return out;
}
bool Equal(const std::vector<eng::EngineListedName>& got, const std::vector<Expected>& want) {
  if (got.size() != want.size()) return false;
  for (std::size_t i = 0; i < got.size(); ++i) {
    const auto& g = got[i];
    const auto& w = *want[i].fact;
    if (!Same(g.name_entry_uuid, w.entry_uuid) || !Same(g.object_uuid, w.object) || !Same(g.language_uuid, w.language) ||
        g.name != w.text || g.object_kind != w.kind || g.name_class != static_cast<std::uint8_t>(w.name_class) ||
        g.default_for_language != w.default_for_language || g.default_for_object != w.default_for_object ||
        g.was_quoted != w.quoted || g.is_fallback != want[i].fallback || g.lifecycle_state != 2)
      return false;
  }
  return true;
}

// The OFFLINE expectation of UuidToName: root side first, each component the object's entry in the language (its
// default_for_language one), else its default_for_object one.
struct ExpectedComponent {
  const NameFact* fact;
  bool fallback;
};
const NameFact* PickEntry(const Offline& offline, const p::Uuid& object, const p::Uuid& language, bool* fallback) {
  const NameFact* object_default = nullptr;
  for (const auto& n : offline.names) {
    if (n.object != object) continue;
    if (n.language == language && n.default_for_language) {
      *fallback = false;
      return &n;
    }
    if (n.default_for_object) object_default = &n;
  }
  *fallback = true;
  return object_default;
}
std::vector<ExpectedComponent> ExpectedPath(const Offline& offline, const p::Uuid& object, const p::Uuid& language) {
  std::vector<ExpectedComponent> reversed;
  p::Uuid at = object;
  for (int guard = 0; guard < 80; ++guard) {
    bool fallback = false;
    const auto* e = PickEntry(offline, at, language, &fallback);
    Check(e != nullptr, "the offline path walk finds a name for every ancestor");
    reversed.push_back({e, e->language != language});
    if (e->scope == at) break;  // the database's own name
    at = e->scope;
  }
  return {reversed.rbegin(), reversed.rend()};
}

std::string DigestHex(std::uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(value));
  return buffer;
}

// ---- the served checks ---------------------------------------------------------------------------------------------

int NameListServe(const std::string& state, const Offline& offline, srv::NativeDatabaseLoop& loop, const fs::path& directory,
                  const std::map<std::string, std::string>& before, const p::Uuid& database, const p::Uuid& bootstrap,
                  const p::Uuid& service) {
  const auto started_at = std::chrono::steady_clock::now();
  const auto en = cat::kCatalogLanguage_en, fr = cat::kCatalogLanguage_fr_CA, de = cat::kCatalogLanguage_de_DE;
  const auto security_scope = offline.security_schema;
  std::size_t calls = 0;
  const auto mark = [&](const char* what) {
    std::cout << "INFO t=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - kProcessStart).count() << " " << what << " calls=" << calls << std::endl;
  };
  mark("hook_start");
  const auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count(); };

  if (state == "failed") {
    // A failed database refuses both procedures with the SAME specific code the names source uses, distinct from
    // not-serving; nothing is read.
    const auto code = std::string(eng::NativeNamesSnapshotErrorCode(eng::NativeNamesSnapshotError::database_failed));
    const auto listed = eng::EngineListNames(ListRequest(security_scope, en, 10));
    Check(!listed.ok && CodeOf(listed.diagnostic) == code && listed.entries.empty(), "ListNames refuses a failed database: " + CodeOf(listed.diagnostic));
    const auto path = eng::EngineUuidToName(PathRequest(bootstrap, en));
    Check(!path.ok && CodeOf(path.diagnostic) == code && path.components.empty(), "UuidToName refuses a failed database");
    Check(code != eng::NativeNamesSnapshotErrorCode(eng::NativeNamesSnapshotError::database_not_serving), "failed and not-serving are distinct codes");
    calls = 2;
  } else {
    const auto read_started = std::chrono::steady_clock::now();
    const auto snapshot = Rows();
    Check(snapshot.ok(), "the names source reads");
    std::cout << "INFO snapshot_read_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read_started).count()
              << " rows=" << snapshot.rows.size() << std::endl;
    // 0. Input refusals: one specific code each, nothing read before them.
    {
      auto r = ListRequest(security_scope, en, 10);
      r.language_uuid = {};
      auto x = eng::EngineListNames(r);
      Check(!x.ok && CodeOf(x.diagnostic) == "CATALOG.NAMES_LIST_LANGUAGE_REQUIRED", "a nil language is refused");
      r = ListRequest(security_scope, en, 10);
      r.scope_uuid = {};
      x = eng::EngineListNames(r);
      Check(!x.ok && CodeOf(x.diagnostic) == "CATALOG.NAMES_LIST_SCOPE_REQUIRED", "a nil scope is refused");
      x = eng::EngineListNames(ListRequest(security_scope, en, 0));
      Check(!x.ok && CodeOf(x.diagnostic) == "CATALOG.NAMES_LIST_PAGE_SIZE_INVALID", "page size 0 is refused");
      x = eng::EngineListNames(ListRequest(security_scope, en, eng::kNameListMaximumPageEntries + 1));
      Check(!x.ok && CodeOf(x.diagnostic) == "CATALOG.NAMES_LIST_PAGE_SIZE_INVALID", "a page size above the maximum is refused, not clamped");
      x = eng::EngineListNames(ListRequest(security_scope, en, eng::kNameListMaximumPageEntries));
      Check(x.ok, "the maximum page size is accepted");
      x = eng::EngineListNames(ListRequest(security_scope, en, 10, "not a token"));
      Check(!x.ok && CodeOf(x.diagnostic) == "CATALOG.NAMES_LIST_CONTINUATION_INVALID", "a malformed continuation is refused");
      auto first = eng::EngineListNames(ListRequest(security_scope, en, 1));
      Check(first.ok && first.more && !first.next_continuation.empty(), "page size 1 over the principals' scope has more");
      x = eng::EngineListNames(ListRequest(security_scope, de, 1, first.next_continuation));
      Check(!x.ok && CodeOf(x.diagnostic) == "CATALOG.NAMES_LIST_CONTINUATION_MISMATCH", "a continuation of another language is refused");
      x = eng::EngineListNames(ListRequest(database, en, 1, first.next_continuation));
      Check(!x.ok && CodeOf(x.diagnostic) == "CATALOG.NAMES_LIST_CONTINUATION_MISMATCH", "a continuation of another scope is refused");
      auto pr = PathRequest(bootstrap, en);
      pr.object_uuid = {};
      auto y = eng::EngineUuidToName(pr);
      Check(!y.ok && CodeOf(y.diagnostic) == "CATALOG.NAMES_PATH_OBJECT_REQUIRED", "UuidToName: a nil object is refused");
      pr = PathRequest(bootstrap, en);
      pr.language_uuid = {};
      y = eng::EngineUuidToName(pr);
      Check(!y.ok && CodeOf(y.diagnostic) == "CATALOG.NAMES_PATH_LANGUAGE_REQUIRED", "UuidToName: a nil language is refused");
      y = eng::EngineUuidToName(PathRequest(NewId(), en));
      Check(!y.ok && CodeOf(y.diagnostic) == "CATALOG.NAMES_PATH_OBJECT_NOT_FOUND", "UuidToName: an unknown object is refused");
      eng::EngineListNamesRequest not_native = ListRequest(security_scope, en, 10);
      not_native.context.native_served_facts.reset();
      x = eng::EngineListNames(not_native);
      Check(!x.ok && CodeOf(x.diagnostic) == "CATALOG.NAMES_LIST_NOT_NATIVE", "a context with no native facts is refused");
      auto wrong_db = ListRequest(security_scope, en, 10);
      wrong_db.context.database_uuid = ToEngine(NewId());
      x = eng::EngineListNames(wrong_db);
      Check(!x.ok && CodeOf(x.diagnostic) == "CATALOG.NAMES_LIST_DATABASE_MISMATCH", "another database's UUID is refused");
      calls += 16;
    }
    mark("before_section_0");
    // 1. The unknown / empty scope: an unknown scope is an EMPTY result; a known object with no children is empty too,
    //    told apart by scope_is_named.
    {
      const auto unknown = eng::EngineListNames(ListRequest(NewId(), en, 10));
      Check(unknown.ok && unknown.entries.empty() && !unknown.more && !unknown.scope_is_named, "an unknown scope lists nothing and is not named");
      const auto childless = eng::EngineListNames(ListRequest(bootstrap, en, 10));
      Check(childless.ok && childless.entries.empty() && childless.scope_is_named, "a named object with no children lists nothing and is named");
      calls += 2;
    }
    mark("before_section_1");
    // 2. Every scope of the file, every language: the engine's page equals the offline expectation (one big page).
    std::map<p::Uuid, std::size_t> scope_sizes;
    for (const auto& n : offline.names) ++scope_sizes[n.scope];
    std::set<p::Uuid> scopes;
    for (const auto& [scope, size] : scope_sizes) {
      (void)size;
      scopes.insert(scope);
    }
    Check(scopes.size() >= 2 && scopes.count(security_scope) == 1 && scopes.count(database) == 1, "the file has the database scope and the security scope");
    // Every full listing is a snapshot read (about 60 ms): the database scope, the security scope and the six next
    // largest scopes are compared in three languages with the offline expectation (the file has hundreds of scopes).
    std::vector<p::Uuid> compared{database, security_scope};
    {
      std::vector<std::pair<std::size_t, p::Uuid>> by_size;
      for (const auto& [scope, size] : scope_sizes)
        if (scope != database && scope != security_scope) by_size.push_back({size, scope});
      std::sort(by_size.begin(), by_size.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
      for (std::size_t i = 0; i < by_size.size() && i < 6; ++i) compared.push_back(by_size[i].second);
    }
    std::size_t listed_entries = 0, fallback_entries = 0;
    for (const auto& scope : compared) {
      for (const auto& language : {en, fr, de}) {
        const auto want = ExpectedList(offline, scope, language);
        const auto got = ListAll(scope, language, eng::kNameListMaximumPageEntries);
        ++calls;
        Check(Equal(got.entries, want), "ListNames equals the offline expectation (scope, language)");
        Check(got.count == offline.names.size() && got.digest != 0, "the result reports the visible entry count and a digest");
        listed_entries += got.entries.size();
        for (const auto& e : got.entries) fallback_entries += e.is_fallback;
      }
    }
    Check(listed_entries > 0, "something was listed");
    mark("before_section_2");
    // 3. Paging: every page size over the principals' scope gives the same entries, no duplicate and no gap.
    {
      const auto want = ExpectedList(offline, security_scope, en);
      const std::size_t n = want.size();
      Check(n >= 10, "the principals' scope holds at least ten names");
      // Page size 1, 2, 3 and 7 on the smallest scope that has at least five names (each page is a snapshot read), the
      // big sizes (and n, n-1) on the principals' scope.
      {
        p::Uuid small;
        std::size_t small_size = 0;
        for (const auto& [scope, size] : scope_sizes)
          if (size >= 5 && (small_size == 0 || size < small_size)) {
            small = scope;
            small_size = size;
          }
        Check(small_size >= 5, "a small scope with at least five names exists");
        const auto small_want = ExpectedList(offline, small, en);
        for (const std::uint32_t size : {1u, 2u, 3u, 7u}) {
          const auto got = ListAll(small, en, size);
          calls += got.pages;
          Check(Equal(got.entries, small_want), "paged listing of a small scope equals the expectation at page size " + std::to_string(size));
          Check(got.pages == (small_want.size() + size - 1) / size, "the page count of a small scope at page size " + std::to_string(size));
        }
      }
      std::vector<std::uint32_t> sizes{50, 333, static_cast<std::uint32_t>(std::min<std::size_t>(n, 1024)), static_cast<std::uint32_t>(std::min<std::size_t>(n - 1, 1024))};
      if (state == "many") sizes = {100, 700, 1024};
      for (const auto size : sizes) {
        const auto got = ListAll(security_scope, en, size);
        calls += got.pages;
        Check(Equal(got.entries, want), "paged listing equals the unpaged expectation at page size " + std::to_string(size));
        Check(got.pages == (n + size - 1) / size, "the page count at page size " + std::to_string(size));
        std::set<p::Uuid> seen;
        for (const auto& e : got.entries) {
          p::Uuid id;
          std::copy(e.name_entry_uuid.bytes.begin(), e.name_entry_uuid.bytes.end(), id.bytes.begin());
          Check(seen.insert(id).second, "no entry twice across pages");
        }
        Check(seen.size() == n, "no entry lost across pages");
      }
    }
    mark("before_section_3");
    // 4. The requested language versus the default: the create's language is the default of every object; fr-CA and de-DE
    //    list only defaults (marked fallback) except where a French name exists.
    {
      const auto in_en = ListAll(security_scope, en, eng::kNameListMaximumPageEntries);
      const auto in_de = ListAll(security_scope, de, eng::kNameListMaximumPageEntries);
      calls += 2;
      Check(!in_en.entries.empty() && std::none_of(in_en.entries.begin(), in_en.entries.end(), [](const auto& e) { return e.is_fallback; }),
            "in the create's language no entry is a fallback");
      std::set<p::Uuid> scope_objects;
      for (const auto& n : offline.names)
        if (n.scope == security_scope) scope_objects.insert(n.object);
      Check(in_de.entries.size() == scope_objects.size() &&
                std::all_of(in_de.entries.begin(), in_de.entries.end(), [](const auto& e) { return e.is_fallback && e.default_for_object; }),
            "in a language no name has, each principal is listed once, by its default name, marked fallback");
      const auto* french = EntryOf(offline, bootstrap, std::string(kAliasFrench));
      if (french != nullptr) {
        const auto in_fr = ListAll(security_scope, fr, eng::kNameListMaximumPageEntries);
        ++calls;
        std::size_t bootstrap_in_fr = 0, others_fallback = 0;
        for (const auto& e : in_fr.entries) {
          if (Same(e.object_uuid, bootstrap)) {
            ++bootstrap_in_fr;
            Check(!e.is_fallback && e.name == kAliasFrench && e.default_for_language && e.name_class == 2, "the bootstrap principal is listed by its French name only, not as a fallback");
          } else {
            others_fallback += e.is_fallback;
          }
        }
        Check(bootstrap_in_fr == 1 && others_fallback + 1 == in_fr.entries.size(), "every other principal falls back to its default name in French");
        Check(std::none_of(in_en.entries.begin(), in_en.entries.end(), [](const auto& e) { return e.name == kAliasFrench; }), "the French name is not listed in English");
      }
      // Aliases: several names of one object in one language, all listed, each with its class.
      std::size_t bootstrap_en = 0;
      for (const auto& n : offline.names)
        if (n.object == bootstrap && n.language == en) ++bootstrap_en;
      std::size_t listed_bootstrap_en = 0;
      for (const auto& e : in_en.entries) listed_bootstrap_en += Same(e.object_uuid, bootstrap);
      Check(listed_bootstrap_en == bootstrap_en && bootstrap_en >= 1, "every English name of the bootstrap principal is listed");
      if (EntryOf(offline, bootstrap, std::string(kAliasKeep)) != nullptr) {
        Check(bootstrap_en >= 4, "forged: several names, one language, one object");
        for (const auto& e : in_en.entries) {
          if (e.name == kAliasKeep) Check(e.name_class == 2 && !e.default_for_object && !e.was_quoted, "an alias is listed as an alias, not the default");
          if (e.name == kQuotedMixed) Check(e.was_quoted && e.requires_exact_match && e.quote_style == 1, "the quoted datum is returned as stored");
        }
        Check(std::any_of(in_en.entries.begin(), in_en.entries.end(), [](const auto& e) { return e.name == kTwinBootstrap; }) &&
                  std::any_of(in_en.entries.begin(), in_en.entries.end(), [](const auto& e) { return e.name == kTwinService; }),
              "both quoted case twins are listed, as stored");
      }
      // Deleted and never-committed names are absent (the offline read, the ground truth, does not list them either).
      for (const auto& e : in_en.entries) {
        Check(e.name != kNeverCommitted, "a name no transaction committed is not listed");
        if (state == "dead") Check(e.name != kAliasDrop, "a dropped name is not listed");
      }
      if (state == "dead") Check(EntryOf(offline, bootstrap, std::string(kAliasDrop)) == nullptr, "the dropped alias is not in the offline read either");
      // The default flags: exactly one default_for_object per object in the unpaged English list.
      std::map<p::Uuid, int> defaults;
      for (const auto& e : in_en.entries) {
        p::Uuid id;
        std::copy(e.object_uuid.bytes.begin(), e.object_uuid.bytes.end(), id.bytes.begin());
        defaults[id] += e.default_for_object;
      }
      for (const auto& [object, count] : defaults) {
        (void)object;
        Check(count == 1, "one default name per object");
      }
    }
    // 5. State-specific entries of the "many" forge: the empty name, a hidden-class name, and the bulk aliases.
    if (state == "many") {
      const auto in_en = ListAll(security_scope, en, 1024);
      calls += in_en.pages;
      std::size_t bulk = 0, hidden = 0, empty = 0;
      for (const auto& e : in_en.entries) {
        bulk += std::string_view(e.name).starts_with(kBulkPrefix);
        if (e.name == kHiddenList) {
          ++hidden;
          Check(e.name_class == 5, "the hidden-class name is listed WITH its class (the parser decides)");
        }
        empty += e.name.empty();
      }
      Check(bulk == static_cast<std::size_t>(kBulkNames), "every bulk alias is listed");
      Check(hidden == 1, "the hidden-class name is listed");
      // S6: was an INFO count only (the production writer refused ""); now "" is listed exactly once, as the stored alias.
      Check(empty == 1, "S6: the empty name is listed once (written by the production writer, as stored)");
      for (const auto& e : in_en.entries)
        if (e.name.empty())
          Check(e.name_class == 2 && !e.default_for_object && !e.is_fallback && Same(e.object_uuid, bootstrap) && Same(e.language_uuid, en),
                "S6: the listed empty name is the bootstrap principal's English alias, not a fallback");
      std::cout << "INFO many_names listed=" << in_en.entries.size() << " pages_at_1024=" << in_en.pages << " empty_name_listed=" << empty
                << " snapshot_rows=" << in_en.count << std::endl;
      Check(in_en.pages == (in_en.entries.size() + 1023) / 1024, "a scope of thousands pages by the maximum page size");
    }
    mark("before_section_4");
    // 6. UuidToName: compared with the offline walk, for every distinct object of the file (the database object, the
    //    schemas, the principals), in three languages.
    {
      std::set<p::Uuid> objects;
      for (const auto& n : offline.names) objects.insert(n.object);
      std::size_t checked = 0, mixed = 0;
      // Every call is a snapshot read: the special objects in three languages, a spread of sixteen others in English.
      std::set<p::Uuid> special{bootstrap, service, database, security_scope};
      std::vector<std::pair<p::Uuid, bool>> sample;  // (object, all three languages)
      for (const auto& object : special) sample.push_back({object, true});
      {
        std::size_t index = 0;
        const std::size_t stride = std::max<std::size_t>(1, objects.size() / 16);
        for (const auto& object : objects)
          if ((index++ % stride) == 0 && special.count(object) == 0) sample.push_back({object, false});
      }
      for (const auto& [object, all_languages] : sample) {
        for (const auto& language : {en, fr, de}) {
          if (language != en && !all_languages) continue;
          const auto want = ExpectedPath(offline, object, language);
          const auto got = eng::EngineUuidToName(PathRequest(object, language));
          ++calls;
          ++checked;
          Check(got.ok, "UuidToName: " + CodeOf(got.diagnostic));
          Check(got.components.size() == want.size() && !want.empty(), "the path has the offline walk's components");
          for (std::size_t i = 0; i < want.size(); ++i) {
            const auto& g = got.components[i];
            Check(g.name == want[i].fact->text && Same(g.name_entry_uuid, want[i].fact->entry_uuid) && Same(g.object_uuid, want[i].fact->object) &&
                      g.is_fallback == want[i].fallback && g.is_database_root == (i == 0),
                  "a path component is the offline walk's (name, entry, fallback flag, root flag)");
          }
          Check(Same(got.components.front().object_uuid, database) && got.components.front().is_database_root, "the path starts at the database object");
          Check(Same(got.components.back().object_uuid, object), "the path ends at the object");
          Check(got.components.size() <= 65, "within the path cap");
          if (got.components.size() > 1 && std::any_of(got.components.begin(), got.components.end(), [](const auto& c) { return c.is_fallback; }) &&
              std::any_of(got.components.begin(), got.components.end(), [](const auto& c) { return !c.is_fallback; }))
            ++mixed;
        }
      }
      Check(checked >= 6, "paths were checked");
      // The bootstrap principal: [database root, sys, security schema, its own name] in English; in French the own name is
      // the French alias and the ancestors fall back (mixed); in German everything falls back.
      const auto path_en = eng::EngineUuidToName(PathRequest(bootstrap, en));
      {
        std::string shape;
        for (const auto& c : path_en.components) shape += "[" + c.name + " kind=" + std::to_string(c.object_kind) + " root=" + std::to_string(c.is_database_root) + "]";
        std::cout << "INFO bootstrap_path " << shape << " ok=" << path_en.ok << " " << CodeOf(path_en.diagnostic) << std::endl;
      }
      Check(path_en.ok && path_en.components.size() == 4, "the bootstrap principal's path is database / sys / security / principal");
      Check(path_en.components.back().name == kBootstrapName && !path_en.components.back().is_fallback, "its own name is the create's, in the requested language");
      Check(Same(path_en.components[2].object_uuid, security_scope) && path_en.components[1].object_kind != 0, "its parent component is the security schema, under sys");
      const auto path_de = eng::EngineUuidToName(PathRequest(bootstrap, de));
      Check(path_de.ok && std::all_of(path_de.components.begin(), path_de.components.end(), [](const auto& c) { return c.is_fallback; }),
            "in a language no name has, every component is its default name, marked fallback");
      Check(path_de.components.back().name == kBootstrapName, "and the default name is the create's");
      if (EntryOf(offline, bootstrap, std::string(kAliasFrench)) != nullptr) {
        const auto path_fr = eng::EngineUuidToName(PathRequest(bootstrap, fr));
        Check(path_fr.ok && path_fr.components.size() == 4 && path_fr.components.back().name == kAliasFrench && !path_fr.components.back().is_fallback &&
                  path_fr.components[0].is_fallback && path_fr.components[1].is_fallback && path_fr.components[2].is_fallback,
              "French: the own name is the French one, the ancestors fall back to their default names");
        Check(mixed >= 1, "a mixed fallback path was seen");
      }
      // The database object's own path: one component, the root, and its default name.
      const auto root = eng::EngineUuidToName(PathRequest(database, en));
      Check(root.ok && root.components.size() == 1 && root.components[0].is_database_root, "the database object's path is its own name");
      std::cout << "INFO database_root_default_name_bytes=" << root.components[0].name.size() << " name=" << root.components[0].name
                << " kind=" << root.components[0].object_kind << " scope_names=" << scopes.size() << std::endl;
    }
    mark("before_section_5");
    // 7. Metadata-visibility policy hides objects from both procedures (the same predicate as every public lookup).
    {
      auto hide_class = ListRequest(security_scope, en, 1024);
      hide_class.policy_profile.encoded_profiles.push_back("metadata_visibility:hide_class:user");
      const auto none = eng::EngineListNames(hide_class);
      std::size_t non_user = 0;
      for (const auto& w : ExpectedList(offline, security_scope, en)) non_user += w.fact->kind != kUserKind;
      Check(none.ok && none.entries.size() == non_user && std::none_of(none.entries.begin(), none.entries.end(), [](const auto& e) { return e.object_kind == kUserKind; }),
            "hide_class:user hides every user principal and nothing else");
      auto hide_all = ListRequest(security_scope, en, 1024);
      hide_all.policy_profile.encoded_profiles.push_back("metadata_visibility:hide_all");
      Check(eng::EngineListNames(hide_all).entries.empty(), "hide_all lists nothing");
      auto hide_one = ListRequest(security_scope, en, 1024);
      hide_one.policy_profile.encoded_profiles.push_back(std::string("metadata_visibility:hide_object:") + std::string(reinterpret_cast<const char*>(service.bytes.data()), 16));
      const auto some = eng::EngineListNames(hide_one);
      Check(some.ok && !some.entries.empty() && std::none_of(some.entries.begin(), some.entries.end(), [&](const auto& e) { return Same(e.object_uuid, service); }) &&
                std::any_of(some.entries.begin(), some.entries.end(), [&](const auto& e) { return Same(e.object_uuid, bootstrap); }),
            "hide_object hides that object only");
      auto hidden_path = PathRequest(bootstrap, en);
      hidden_path.policy_profile.encoded_profiles.push_back("metadata_visibility:hide_class:user");
      const auto refused = eng::EngineUuidToName(hidden_path);
      Check(!refused.ok && CodeOf(refused.diagnostic) == "CATALOG.NAMES_PATH_OBJECT_NOT_FOUND", "UuidToName of a hidden object is refused as not found");
      calls += 5;
    }
    mark("before_section_6");
    // 8. The loop paths: from loop work (the direct path) the result is the same; from three threads at once each gets
    //    its own correct answer.
    {
      const auto want = ListAll(security_scope, en, eng::kNameListMaximumPageEntries);
      eng::EngineListNamesResult in_loop;
      eng::EngineUuidToNameResult in_loop_path;
      const bool ran = loop.RunOnLoopThreadForTest([&] {
        in_loop = eng::EngineListNames(ListRequest(security_scope, en, eng::kNameListMaximumPageEntries));
        in_loop_path = eng::EngineUuidToName(PathRequest(bootstrap, en));
      });
      Check(ran && in_loop.ok && in_loop.entries.size() == std::min<std::size_t>(want.entries.size(), eng::kNameListMaximumPageEntries) && in_loop.names_digest == want.digest && in_loop_path.ok && in_loop_path.components.size() == 4,
            "ListNames and UuidToName work from loop work (the direct in-loop path) and agree with the cross-thread path");
      std::vector<std::string> failures(3);
      std::vector<std::thread> callers;
      for (int t = 0; t < 3; ++t)
        callers.emplace_back([&, t] {
          for (int i = 0; i < 3; ++i) {
            const auto a = eng::EngineListNames(ListRequest(security_scope, t == 0 ? en : t == 1 ? fr : de, eng::kNameListMaximumPageEntries));
            const auto b = eng::EngineUuidToName(PathRequest(bootstrap, en));
            if (!a.ok || a.names_digest != want.digest || !b.ok || b.components.size() != 4) failures[t] = "wrong answer";
          }
        });
      for (auto& c : callers) c.join();
      for (const auto& f : failures) Check(f.empty(), "a concurrent caller got a wrong answer: " + f);
      calls += 2 + 6 + 1;
    }
    // 8b. The S1 differential, with the parser module reading the engine's LISTED names (ListNames) instead of the registry
    //     projection: for every case of the S1 table the module's list-and-compare answer equals the engine's own answer to
    //     the typed spelling, and the engine asked the STORED spelling of the matched entries answers the same object.
    //     (The legs W and E of S1 run again in the same cases.) It runs only when the driver sets SB_NAME_LIST_DIFFERENTIAL (the
    //     states serving and names of `size`): about 30 s a state, so the strace driver, which runs the same serve child, does not.
    if (std::getenv("SB_NAME_LIST_DIFFERENTIAL") != nullptr) {
      const auto differential_started = std::chrono::steady_clock::now();
      nr::PageFetcher fetch = [&](const nr::Uuid16& scope, const nr::Uuid16& language, const std::string& continuation, std::uint32_t max) {
        p::Uuid scope_uuid, language_uuid;
        std::copy(scope.begin(), scope.end(), scope_uuid.bytes.begin());
        std::copy(language.begin(), language.end(), language_uuid.bytes.begin());
        const auto listed = eng::EngineListNames(ListRequest(scope_uuid, language_uuid, max, continuation));
        nr::ListedPage page;
        page.ok = listed.ok;
        page.refusal_code = listed.diagnostic.code;
        page.more = listed.more;
        page.next_continuation = listed.next_continuation;
        page.names_digest = listed.names_digest;
        for (const auto& e : listed.entries) {
          nr::ListedName name;
          std::copy(e.name_entry_uuid.bytes.begin(), e.name_entry_uuid.bytes.end(), name.entry.begin());
          std::copy(e.object_uuid.bytes.begin(), e.object_uuid.bytes.end(), name.object.begin());
          std::copy(e.language_uuid.bytes.begin(), e.language_uuid.bytes.end(), name.language.begin());
          name.name = e.name;
          name.object_kind = e.object_kind;
          name.name_class = e.name_class;
          name.default_for_language = e.default_for_language;
          name.default_for_object = e.default_for_object;
          name.is_fallback = e.is_fallback;
          name.was_quoted = e.was_quoted;
          page.entries.push_back(std::move(name));
        }
        return page;
      };
      nr::ListedNameSource listed_source(
          fetch,
          [](std::string_view tag) -> std::optional<nr::Uuid16> {
            const auto language = cat::CatalogLanguageForTag(tag);
            if (!language) return std::nullopt;
            nr::Uuid16 out{};
            std::copy(language->bytes.begin(), language->bytes.end(), out.begin());
            return out;
          },
          [](std::uint16_t kind) { return eng::NameRegistryObjectClassText(kind); });
      nr::NameListCache cache(listed_source);
      shared_name_cache = &cache;
      send_stored_spelling = true;
      const auto counts = DifferentialAgainstModule(offline, bootstrap, service, &listed_source);
      shared_name_cache = nullptr;
      send_stored_spelling = false;
      Check(!listed_source.failed(), "no list page was refused during the differential: " + listed_source.refusal_code());
      Check(counts.stored_sent >= 20, "the stored spelling was sent for at least twenty found cases");
      std::cout << "INFO list_differential cases=" << counts.cases << " engine_calls=" << counts.engine_calls << " found=" << counts.found
                << " not_found=" << counts.not_found << " refused_other=" << counts.invalid << " stored_sent=" << counts.stored_sent
                << " list_pages_fetched=" << listed_source.pages_fetched() << " cached_pages=" << cache.cached_pages() << " seconds="
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - differential_started).count() << std::endl;
      calls += counts.engine_calls + listed_source.pages_fetched();
    }
    mark("before_section_7");
    // 9. The change signal the parser compares, printed for the driver (state "flow" requires it to move between serves).
    {
      const auto first = eng::EngineListNames(ListRequest(security_scope, en, 1));
      std::cout << "LIST_DIGEST digest=" << DigestHex(first.names_digest) << " count=" << first.visible_entry_count
                << " max_generation=" << first.max_catalog_generation_id << " max_epoch=" << first.max_name_resolution_epoch
                << " offline_entries=" << offline.names.size() << std::endl;
      Check(first.visible_entry_count == offline.names.size(), "the visible entry count is the offline read's");
    }
    std::cout << "INFO listings fallback_entries=" << fallback_entries << " listed_entries=" << listed_entries << " scopes=" << scopes.size() << std::endl;
  }
  std::cout << "TIMING list_calls=" << calls << " seconds=" << since() << std::endl;
  // Reads write nothing: bytes unchanged, no side files; the source answers `closed` after the loop closes.
  Check(Snapshot(directory) == before, "the directory and every file's bytes are unchanged after every call");
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
  Check(loop.Close().ok, "the loop closes");
  if (state != "failed") {
    const auto closed = eng::EngineListNames(ListRequest(security_scope, en, 10));
    Check(!closed.ok && CodeOf(closed.diagnostic) == eng::NativeNamesSnapshotErrorCode(eng::NativeNamesSnapshotError::closed), "a listing after the loop closed is refused (closed)");
  }
  std::cout << "NAME_RESOLVE_OK checks=" << checks << " state=" << state << std::endl;
  return 0;
}

// ---- forge: production writers --------------------------------------------------------------------------------------
// kinds: many (a hidden-class name and kBulkNames aliases on the bootstrap principal, in one transaction; the empty
// name "" is refused by the production writer today, invalid_name: Part B S6 admits it, then this forge adds it), drop (the alias Drop_Alias of the "names" forge, a committed drop), add (one new alias).
// PART B S6 (2026-11-23): the writer admits "" now, so the "many" forge adds the empty English alias of the bootstrap principal
// (old: the comment above said it was refused; new: it is written and the "many" serve asserts it is listed once).
int ListForgeMode(const fs::path& path, const std::string& kind, const p::Uuid& bootstrap, const p::Uuid& service) {
  (void)service;
  ProcessMemory();
  const auto before = ReadOffline(path);
  const auto* base_boot = EntryOf(before, bootstrap, std::string(kBootstrapName));
  Check(base_boot != nullptr, "the create wrote the bootstrap entry");
  if (kind == "drop") {
    const auto* target = EntryOf(before, bootstrap, std::string(kAliasDrop));
    Check(target != nullptr, "the alias to drop is visible and committed");
    auto w = BeginWriting(path, before);
    Touch(*w, before.security.at(bootstrap));
    DropEntry(*w, *target);
    Commit(*w);
    Check(w->device.Close().ok(), "close");
  } else {
    auto w = BeginWriting(path, before);
    Touch(*w, before.security.at(bootstrap));
    if (kind == "add") {
      std::vector<db::CatalogInsertRow> rows;
      rows.push_back(EntryRow(*w, *base_boot, {std::string(kAddedLater), false, std::nullopt, cat::CatalogNameClass::alias, false}));
      Insert(*w, std::move(rows));
    } else if (kind == "many") {
      std::vector<db::CatalogInsertRow> rows;
      rows.push_back(EntryRow(*w, *base_boot, {std::string(kHiddenList), false, std::nullopt, cat::CatalogNameClass::hidden, false}));
      rows.push_back(EntryRow(*w, *base_boot, {std::string(kEmptyName), false, std::nullopt, cat::CatalogNameClass::alias, false}));  // S6
      Insert(*w, std::move(rows));
      for (int i = 0; i < kBulkNames; i += 10) {
        rows.clear();
        for (int j = i; j < std::min(i + 10, kBulkNames); ++j) {
          char name[32];
          std::snprintf(name, sizeof name, "bulk_%05d", j);
          rows.push_back(EntryRow(*w, *base_boot, {name, false, std::nullopt, cat::CatalogNameClass::alias, false}));
        }
        Insert(*w, std::move(rows));
      }
    } else {
      return 2;
    }
    Commit(*w);
    Check(w->device.Close().ok(), "close");
  }
  std::cout << "FORGED kind=" << kind << std::endl;
  return 0;
}

// ---- driver ---------------------------------------------------------------------------------------------------------

std::string ServeOnce(const std::string& self, const fs::path& path, const std::string& state, const std::string& created, const std::string& tag,
                      std::chrono::steady_clock::time_point* started = nullptr) {
  int status = 0;
  const auto start = std::chrono::steady_clock::now();
  const std::string differential = (state == "serving" || state == "names") ? "SB_NAME_LIST_DIFFERENTIAL=1 " : "";
  const auto served = Capture(differential + "'" + self + "' serve '" + path.string() + "' " + state + " " + Field(created, "database") + " " +
                                  Field(created, "bootstrap") + " " + Field(created, "service"),
                              &status);
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  Check(status == 0 && served.find("NAME_RESOLVE_OK") != std::string::npos, tag + state + ": serve child: " + served.substr(0, 3000));
  Check(served.find(std::string(kBootstrapPassword)) == std::string::npos, tag + state + ": no test password in any child output");
  for (std::size_t at = 0; (at = served.find("INFO ", at)) != std::string::npos; at += 5)
    std::printf("%s%s: %s\n", tag.c_str(), state.c_str(), served.substr(at, served.find('\n', at) - at).c_str());
  const auto timing = served.find("TIMING");
  std::printf("%s%s: %.1f s; %s", tag.c_str(), state.c_str(), seconds, timing == std::string::npos ? "\n" : served.substr(timing).c_str());
  fflush(stdout);
  if (started != nullptr) *started = start;
  return served;
}

void ListOneSize(const std::string& self, const fs::path& root, std::uint32_t page_size, const std::vector<std::string>& states) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  for (const auto& state : states) {
    const auto dir = root / (state + std::to_string(page_size));
    fs::create_directory(dir);
    const auto path = dir / "names.sdb";
    int status = 0;
    const auto created = Capture("'" + self + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
    Check(status == 0 && created.starts_with("CREATED "), tag + state + ": create: " + created);
    const auto forge = [&](const std::string& kind, bool own) {
      const auto out = Capture("'" + self + "' " + (own ? "listforge" : "forge") + " '" + path.string() + "' " + kind + " " + Field(created, "bootstrap") + " " + Field(created, "service"), &status);
      Check(status == 0 && out.find("FORGED kind=" + kind) != std::string::npos, tag + state + ": the engine's writers wrote (" + kind + "): " + out.substr(0, 1500));
    };
    if (state == "restricted") {
      Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' committing", &status).find("FIXTURE ok") != std::string::npos && status == 0,
            tag + "the native_serving writer left a committing slot");
    } else if (state == "failed") {
      Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' active", &status).find("FIXTURE ok") != std::string::npos && status == 0,
            tag + "the native_serving writer left an active slot (the Restart will write; the seam fails it)");
    } else if (state == "names" || state == "dead" || state == "uncommitted") {
      forge(state, false);
    } else if (state == "many") {
      forge("many", true);
    } else if (state == "flow") {
      forge("names", false);
    }
    if (state != "flow") {
      ServeOnce(self, path, state, created, tag);
    } else {
      // One file, three committed states, three serves: the digest must differ at every step and the counts follow.
      const auto first = ServeOnce(self, path, state, created, tag);
      forge("drop", true);
      const auto second = ServeOnce(self, path, state, created, tag);
      forge("add", true);
      const auto third = ServeOnce(self, path, state, created, tag);
      const auto d1 = Field(first, "digest"), d2 = Field(second, "digest"), d3 = Field(third, "digest");
      const auto n1 = std::stoull(Field(first, "count")), n2 = std::stoull(Field(second, "count")), n3 = std::stoull(Field(third, "count"));
      Check(!d1.empty() && !d2.empty() && !d3.empty(), tag + "flow: every serve reported its digest");
      Check(d1 != d2 && d2 != d3 && d1 != d3, tag + "flow: the names digest differs after a committed drop and after a committed add (" + d1 + " " + d2 + " " + d3 + ")");
      Check(n2 + 1 == n1 && n3 == n2 + 1, tag + "flow: the visible entry count follows the drop and the add");
      // The generation maxima are information only: a drop writes a delete marker the snapshot filters out, so no
      // maximum over the rows is a change signal; report what they did.
      std::printf("%sflow: digests %s %s %s; max_generation %s %s %s\n", tag.c_str(), d1.c_str(), d2.c_str(), d3.c_str(),
                  Field(first, "max_generation").c_str(), Field(second, "max_generation").c_str(), Field(third, "max_generation").c_str());
    }
    // The database directory is removed as soon as its state finishes (the /tmp footprint).
    std::error_code ignored;
    fs::remove_all(dir, ignored);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 6 && std::string(argv[1]) == "listforge") return ListForgeMode(argv[2], argv[3], FromHex(argv[4]), FromHex(argv[5]));
  if (argc >= 3 && std::string(argv[1]) == "size") {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    std::vector<std::string> states{"serving", "restricted", "failed", "names", "dead", "uncommitted", "many", "flow"};
    if (argc > 3) states.assign(argv + 3, argv + argc);
    ListOneSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])), states);
    artifacts.Cleanup();
    std::printf("native_name_list: %d driver checks passed\n", checks);
    return 0;
  }
  return name_resolve_driver_main(argc, argv);
}
