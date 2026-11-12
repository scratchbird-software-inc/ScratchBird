// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_NAME_PATH
// The full-path rule (catalog_name_path) against an independent oracle written
// here: its own walk over object parents, its own choice of each ancestor's
// name, its own escaping.
//   - random name forests (one database, objects nested up to eight deep,
//     names in two languages with language defaults, aliases, entries in a
//     language no ancestor has, keys holding 0x00 and 0xFF, exact and
//     normalized lanes): every entry's resolved components and lookup key are
//     the oracle's, and its stored path checks;
//   - a stored path with a changed byte, one byte short, a component too many
//     or too few, two components swapped, or a wrong count does not check;
//   - refusals: an ancestor with no name, an ancestor with two language
//     defaults (the entry's own object having two is not a refusal), a scope
//     cycle, 65 components (64 resolve);
//   - the index: an entry added twice is one name; a re-added entry that moved
//     language is no longer found under the old one;
//   - the in-place check allocates nothing when its lookup allocates nothing.
#include "catalog_language.hpp"
#include "catalog_name_path.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <new>
#include <random>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic<long> allocations{0};
}
void* operator new(std::size_t n) {
  ++allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace c = scratchbird::core::catalog;
// The fixture's language labels as language constants
// (SB_CATALOG_LANGUAGE_CONSTANTS): "en", and two other languages.
scratchbird::core::platform::Uuid Lang(std::string_view label) {
  if (label == "en") return c::kCatalogLanguage_en;
  if (label == "fr") return c::kCatalogLanguage_fr_CA;
  if (label == "de") return c::kCatalogLanguage_de_DE;
  throw std::logic_error("unknown fixture language label");
}
using scratchbird::core::platform::byte;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::UuidKind;
using Bytes = std::vector<byte>;

namespace {
unsigned long checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(what + " (line " + std::to_string(at.line()) + ")");
}
Uuid Id(unsigned n) {
  Uuid u{};
  u.bytes[0] = 1;
  u.bytes[6] = 0x70;
  u.bytes[8] = 0x80;
  u.bytes[12] = static_cast<byte>(n >> 24);
  u.bytes[13] = static_cast<byte>(n >> 16);
  u.bytes[14] = static_cast<byte>(n >> 8);
  u.bytes[15] = static_cast<byte>(n);
  return u;
}
TypedUuid T(unsigned n) { return {UuidKind::object, Id(n)}; }

// ---- The oracle -----------------------------------------------------------------
void Esc(Bytes& out, const Bytes& in) {
  for (byte b : in) {
    out.push_back(b);
    if (b == 0) out.push_back(255);
  }
  out.push_back(0);
  out.push_back(0);
}
const Bytes& Own(const c::CatalogNameEntry& e) { return e.requires_exact_match ? e.exact_lookup_key : e.normalized_lookup_key; }

// A forest of named objects. Object 1 is the database; its names scope to it.
struct Forest {
  unsigned next = 1000000;
  std::map<unsigned, unsigned> parent;               // object -> scope object (1 -> 1)
  std::map<unsigned, unsigned> default_entry;        // object -> its default entry
  std::map<unsigned, c::CatalogNameEntry> entries;   // entry -> entry
  std::map<unsigned, c::CatalogNameVector> vectors;  // object -> vector

  c::CatalogNameEntry& Name(unsigned object, std::string language, Bytes key, bool exact, bool language_default,
                            bool object_default, std::string display) {
    const unsigned id = ++next;
    c::CatalogNameEntry e;
    e.name_entry_uuid = T(id);
    e.name_vector_uuid = T(object + 500000);
    e.object_uuid = T(object);
    e.object_kind = c::CatalogRecordKind::table_descriptor;
    e.scope_uuid = T(parent.at(object));
    e.language_uuid = c::CatalogLanguageIdentity(Lang(language));
    e.display_name = std::move(display);
    e.requires_exact_match = exact;
    (exact ? e.exact_lookup_key : e.normalized_lookup_key) = std::move(key);
    (exact ? e.normalized_lookup_key : e.exact_lookup_key) = {byte{'z'}};
    e.default_for_language = language_default;
    e.default_for_object = object_default;
    if (object_default) {
      default_entry[object] = id;
      c::CatalogNameVector v;
      v.name_vector_uuid = T(object + 500000);
      v.object_uuid = T(object);
      v.default_name_entry_uuid = T(id);
      v.default_language_uuid = e.language_uuid;
      vectors[object] = v;
    }
    return entries[id] = std::move(e);
  }
  // The name a path through `object` uses in `language`.
  const c::CatalogNameEntry* PathName(unsigned object, const Uuid& language) const {
    for (const auto& [id, e] : entries)
      if (e.object_uuid.value == Id(object) && e.default_for_language && e.language_uuid.value == language) return &e;
    const auto d = default_entry.find(object);
    return d == default_entry.end() ? nullptr : &entries.at(d->second);
  }
  struct Path {
    Bytes key;
    std::vector<std::string> display;
    std::vector<Bytes> keys;
  };
  Path OraclePath(const c::CatalogNameEntry& e) const {
    std::vector<const c::CatalogNameEntry*> chain{&e};
    unsigned object = 0;
    for (const auto& [o, p] : parent)
      if (Id(o) == e.object_uuid.value) object = o;
    if (object != 1)
      for (unsigned x = parent.at(object); x != 1; x = parent.at(x)) chain.push_back(PathName(x, e.language_uuid.value));
    std::reverse(chain.begin(), chain.end());
    Path out;
    for (const auto* n : chain) {
      Esc(out.key, Own(*n));
      out.display.push_back(n->display_name);
      out.keys.push_back(Own(*n));
    }
    return out;
  }
};

// Views borrow their payloads: keep every payload here for the index's life.
struct Borrowed {
  std::deque<c::CatalogNamePayload> payloads;
  c::CatalogNameIndex index;
  explicit Borrowed(const Forest& f) {
    for (const auto& [o, v] : f.vectors) index.Add(c::BorrowCatalogNamePayload(payloads.emplace_back(v)));
    for (const auto& [id, e] : f.entries) index.Add(c::BorrowCatalogNamePayload(payloads.emplace_back(e)));
  }
};
// Borrows `e` itself.
c::CatalogNameEntryView View(const c::CatalogNameEntry& e) { return c::BorrowCatalogNameEntry(e); }
// A copy of `e` that carries `path`, and the payload its view borrows.
struct Stored {
  c::CatalogNamePayload payload;
  c::CatalogNameEntryView view;
  Stored(c::CatalogNameEntry e, const Bytes& key, std::size_t count) : payload(([&] {
      e.full_path_lookup_key = key;
      e.path_component_count = count;
      return c::CatalogNamePayload{std::move(e)};
    })()), view(std::get<c::CatalogNameEntryView>(c::BorrowCatalogNamePayload(payload))) {}
};

Bytes RandomKey(std::mt19937_64& rng) {
  static const byte alphabet[] = {0, 0, 255, 255, 'a', 'b', 'c', 1};
  Bytes out(rng() % 6);
  for (auto& b : out) b = alphabet[rng() % sizeof alphabet];
  return out;
}

void RandomForests() {
  std::mt19937_64 rng(20261109);
  std::size_t entries_checked = 0, deepest = 0;
  for (unsigned forest = 0; forest < 60; ++forest) {
    Forest f;
    f.parent[1] = 1;
    f.Name(1, "en", RandomKey(rng), rng() % 2, true, true, "db");
    if (rng() % 2) f.Name(1, "fr", RandomKey(rng), rng() % 2, true, false, "bd");
    std::map<unsigned, unsigned> depth{{1, 0}};
    const unsigned objects = 2 + static_cast<unsigned>(rng() % 40);
    for (unsigned o = 2; o < 2 + objects; ++o) {
      unsigned p;
      do p = 1 + static_cast<unsigned>(rng() % (o - 1));
      while (depth[p] >= 8);
      f.parent[o] = p;
      depth[o] = depth[p] + 1;
      f.Name(o, "en", RandomKey(rng), rng() % 2, true, true, "en" + std::to_string(o));
      if (rng() % 10 < 4) f.Name(o, "fr", RandomKey(rng), rng() % 2, true, false, "fr" + std::to_string(o));
      if (rng() % 10 < 3) f.Name(o, "en", RandomKey(rng), rng() % 2, false, false, "alias" + std::to_string(o));
      if (rng() % 10 < 2) f.Name(o, "de", RandomKey(rng), rng() % 2, false, false, "de" + std::to_string(o));
    }
    Borrowed names(f);
    for (const auto& [id, e] : f.entries) {
      const auto want = f.OraclePath(e);
      const auto view = View(e);
      const auto got = c::ResolveCatalogNamePath(view, names.index);
      Check(got.ok(), "every name in a well-formed forest resolves");
      Check(got.components.size() == want.display.size(), "the component count is the oracle's");
      for (std::size_t i = 0; i < want.display.size(); ++i) {
        Check(got.components[i].display_name == want.display[i], "each display component is the oracle's");
        Check(std::equal(got.components[i].lookup_key.begin(), got.components[i].lookup_key.end(),
                         want.keys[i].begin(), want.keys[i].end()),
              "each component key is the oracle's own-lane key");
      }
      Check(c::CatalogNamePathLookupKey(got) == want.key, "the full-path lookup key is the oracle's");
      deepest = std::max(deepest, want.display.size());
      const Stored good(e, want.key, want.display.size());
      Check(c::CheckCatalogNameEntryPath(good.view, names.index) == c::CatalogNamePathError::none,
            "the oracle's stored path checks");
      const auto refused = [&](const Bytes& key, std::size_t count, const std::string& what) {
        const Stored bad(e, key, count);
        Check(c::CheckCatalogNameEntryPath(bad.view, names.index) == c::CatalogNamePathError::mismatch, what);
      };
      const auto n = want.display.size();
      refused(want.key, n + 1, "a count one too many does not check");
      refused(want.key, n - 1, "a count one too few does not check");
      refused({}, 0, "no stored path does not check");
      for (std::size_t at : {std::size_t{0}, want.key.size() / 2, want.key.size() - 1}) {
        auto key = want.key;
        key[at] ^= byte{0x5a};
        refused(key, n, "a stored path with a changed byte does not check");
      }
      refused(Bytes(want.key.begin(), want.key.end() - 1), n, "a stored path one byte short does not check");
      auto longer = want.key;
      Esc(longer, {byte{'q'}});
      refused(longer, n + 1, "a stored path with a component too many does not check");
      if (n >= 2 && want.keys[0] != want.keys[1]) {
        Bytes swapped;
        Esc(swapped, want.keys[1]);
        Esc(swapped, want.keys[0]);
        for (std::size_t i = 2; i < n; ++i) Esc(swapped, want.keys[i]);
        refused(swapped, n, "a stored path with two components swapped does not check");
      }
      if (n >= 2) {
        Bytes short_path;
        for (std::size_t i = 1; i < n; ++i) Esc(short_path, want.keys[i]);
        refused(short_path, n, "a stored path missing its root-side component does not check");
      }
      ++entries_checked;
    }
  }
  Check(deepest == 8, "the forests reach eight levels below the database (eight components)");
  std::cout << "random forests: " << entries_checked << " entries\n";
}

void Refusals() {
  // a -> b -> c below the database; d's scope is unnamed.
  Forest f;
  f.parent = {{1, 1}, {2, 1}, {3, 2}, {4, 3}, {5, 9}, {9, 1}};
  f.Name(1, "en", {byte{'d'}}, false, true, true, "db");
  f.Name(2, "en", {byte{'a'}}, false, true, true, "a");
  f.Name(3, "en", {byte{'b'}}, false, true, true, "b");
  auto& leaf = f.Name(4, "en", {byte{'c'}}, false, true, true, "c");
  auto& orphan = f.Name(5, "en", {byte{'e'}}, false, true, true, "e");  // object 9 has no names
  {
    Borrowed names(f);
    Check(c::ResolveCatalogNamePath(View(leaf), names.index).ok(), "a named chain resolves");
    Check(c::ResolveCatalogNamePath(View(orphan), names.index).error == c::CatalogNamePathError::unnamed_scope,
          "an ancestor with no name is refused");
    Check(c::CheckCatalogNameEntryPath(View(orphan), names.index) == c::CatalogNamePathError::unnamed_scope,
          "the in-place check gives the same refusal");
  }
  // Two language defaults on an ancestor: refused; on the entry's own object: not.
  auto& second = f.Name(3, "en", {byte{'B'}}, false, true, false, "B");
  {
    Borrowed names(f);
    Check(c::ResolveCatalogNamePath(View(leaf), names.index).error == c::CatalogNamePathError::ambiguous_scope,
          "an ancestor with two language defaults in the entry's language is refused");
    Check(c::CheckCatalogNameEntryPath(View(leaf), names.index) == c::CatalogNamePathError::ambiguous_scope,
          "the in-place check gives the same refusal");
    Check(c::ResolveCatalogNamePath(View(second), names.index).ok(),
          "two language defaults on the entry's own object are not a path refusal");
    auto fr = leaf;
    fr.language_uuid = c::CatalogLanguageIdentity(Lang("fr"));
    fr.default_for_language = false;
    Check(c::ResolveCatalogNamePath(View(fr), names.index).ok(),
          "in another language the ancestor's default entry is its one name");
  }
  // A scope cycle: 6 scopes to 7, 7 to 6.
  Forest g;
  g.parent = {{1, 1}, {6, 7}, {7, 6}};
  g.Name(1, "en", {byte{'d'}}, false, true, true, "db");
  auto& looped = g.Name(6, "en", {byte{'x'}}, false, true, true, "x");
  g.Name(7, "en", {byte{'y'}}, false, true, true, "y");
  {
    Borrowed names(g);
    Check(c::ResolveCatalogNamePath(View(looped), names.index).error == c::CatalogNamePathError::cycle,
          "a scope cycle is refused");
    Check(c::CheckCatalogNameEntryPath(View(looped), names.index) == c::CatalogNamePathError::cycle,
          "the in-place check gives the same refusal");
  }
  // Depth: 64 components resolve, 65 do not.
  Forest h;
  h.parent[1] = 1;
  h.Name(1, "en", {byte{'d'}}, false, true, true, "db");
  std::vector<c::CatalogNameEntry*> chain;
  for (unsigned o = 2; o <= 66; ++o) {
    h.parent[o] = o == 2 ? 1 : o - 1;
    chain.push_back(&h.Name(o, "en", {static_cast<byte>(o)}, false, true, true, std::to_string(o)));
  }
  {
    Borrowed names(h);
    const auto at64 = c::ResolveCatalogNamePath(View(*chain[63]), names.index);
    Check(at64.ok() && at64.components.size() == 64, "64 components resolve");
    Check(c::ResolveCatalogNamePath(View(*chain[64]), names.index).error == c::CatalogNamePathError::too_deep,
          "65 components are refused");
    Check(c::CheckCatalogNameEntryPath(View(*chain[64]), names.index) == c::CatalogNamePathError::too_deep,
          "the in-place check gives the same refusal");
  }
  // The database's own name is a path of one component.
  {
    Borrowed names(f);
    const auto& db = f.entries.begin()->second;
    const auto path = c::ResolveCatalogNamePath(View(db), names.index);
    Check(path.ok() && path.components.size() == 1 && path.components[0].display_name == "db",
          "the database's name is a path of one component");
  }
}

void IndexReplacement() {
  Forest f;
  f.parent = {{1, 1}, {2, 1}, {3, 2}};
  f.Name(1, "en", {byte{'d'}}, false, true, true, "db");
  f.Name(2, "en", {byte{'a'}}, false, true, true, "a");
  auto& fr_name = f.Name(2, "fr", {byte{'f'}}, false, true, false, "f");
  auto& leaf = f.Name(3, "fr", {byte{'c'}}, false, true, true, "c");
  Borrowed names(f);
  const auto fr_payload = c::CatalogNamePayload{fr_name};
  names.index.Add(c::BorrowCatalogNamePayload(fr_payload));  // the same entry again
  auto path = c::ResolveCatalogNamePath(View(leaf), names.index);
  Check(path.ok() && path.components[0].display_name == "f", "an entry added twice is one name");
  auto moved = fr_name;
  moved.language_uuid = c::CatalogLanguageIdentity(Lang("de"));
  const auto moved_payload = c::CatalogNamePayload{moved};
  names.index.Add(c::BorrowCatalogNamePayload(moved_payload));
  path = c::ResolveCatalogNamePath(View(leaf), names.index);
  Check(path.ok() && path.components[0].display_name == "a",
        "a re-added entry that moved language is no longer found under the old one");
}

void NoAllocation() {
  Forest f;
  f.parent = {{1, 1}, {2, 1}, {3, 2}, {4, 3}};
  const auto& db = f.Name(1, "en", {byte{'d'}}, false, true, true, "db");
  const auto& a = f.Name(2, "en", {byte{'a'}, byte{0}}, false, true, true, "a");
  const auto& b = f.Name(3, "en", {byte{'b'}}, true, true, true, "b");
  const auto& c4 = f.Name(4, "en", {byte{0}, byte{'c'}}, false, true, true, "c");
  const auto want = f.OraclePath(c4);
  const Stored stored(c4, want.key, want.display.size());
  const auto vdb = View(db), va = View(a), vb = View(b);
  const auto lookup = [&](const Uuid& object, const Uuid&) {
    c::CatalogNamePathName out;
    if (object == vdb.object_uuid.value) out.name = vdb;
    if (object == va.object_uuid.value) out.name = va;
    if (object == vb.object_uuid.value) out.name = vb;
    return out;
  };
  const long before = allocations.load();
  const auto checked = c::CheckCatalogNameEntryPath(stored.view, lookup);
  const long during = allocations.load() - before;
  Check(checked == c::CatalogNamePathError::none, "the in-place check accepts the oracle's path");
  Check(during == 0, "the in-place check allocates nothing when its lookup allocates nothing");
}
}  // namespace

int main() {
  try {
    RandomForests();
    Refusals();
    IndexReplacement();
    NoAllocation();
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
  std::cout << "native_name_path: " << checks << " checks passed\n";
  return 0;
}
