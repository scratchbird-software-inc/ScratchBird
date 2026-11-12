// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_NAME_TREES
// The keys and payloads of the native name trees (catalog_name_tree_keys)
// against an independent oracle written here (its own escaping and field
// packing):
//   - every key and payload equals the oracle's bytes, including a key written
//     out byte by byte;
//   - order: sorting random name tuples field by field (scope, identifier
//     profile, language, own-lane key, record kind; object, kind, marker,
//     language, name class) gives the order of their encoded keys, with keys
//     that hold 0x00 and 0xFF bytes and prefixes of each other (a language is
//     a fixed 16-byte constant UUID: SB_CATALOG_LANGUAGE_CONSTANTS);
//   - prefix-freeness: one name's authoritative prefix starts exactly the keys
//     of that name (every class), never another name's;
//   - payloads round-trip; malformed ones (wrong size, truncated, overlong
//     LEB128, trailing bytes, a count past the end) are refused;
//   - the full-path lookup key is the escaped components, concatenated.
#include "catalog_language.hpp"
#include "catalog_name_tree_keys.hpp"

#include <algorithm>
#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace c = scratchbird::core::catalog;
using scratchbird::core::platform::byte;
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
  u.bytes[14] = static_cast<byte>(n >> 8);
  u.bytes[15] = static_cast<byte>(n);
  return u;
}

// ---- The oracle ---------------------------------------------------------------
void Esc(Bytes& out, const Bytes& in) {
  for (byte b : in) {
    out.push_back(b);
    if (b == 0) out.push_back(255);
  }
  out.push_back(0);
  out.push_back(0);
}
void U(Bytes& out, const Uuid& u) { out.insert(out.end(), u.bytes.begin(), u.bytes.end()); }
Bytes Text(const std::string& s) { return Bytes(s.begin(), s.end()); }

struct Name {
  Uuid scope, profile, object, entry;
  Uuid language;  // a language constant
  Bytes key;  // own lane
  bool exact = false;
  unsigned kind = 0;
  unsigned name_class = 1;
};
Bytes OracleAuthoritative(const Name& n, bool with_kind = true) {
  Bytes out;
  U(out, n.scope);
  U(out, n.profile);
  U(out, n.language);
  Esc(out, n.key);
  if (with_kind) {
    out.push_back(static_cast<byte>(n.kind >> 8));
    out.push_back(static_cast<byte>(n.kind));
  }
  return out;
}
Bytes OracleUuidEntry(const Name& n) {
  Bytes out;
  U(out, n.object);
  out.push_back(static_cast<byte>(n.kind >> 8));
  out.push_back(static_cast<byte>(n.kind));
  out.push_back(1);
  U(out, n.language);
  out.push_back(static_cast<byte>(n.name_class));
  return out;
}
// The production view of a name: the own-lane key goes to the lane the entry uses.
struct Entry {
  c::CatalogNameEntry owned;
  c::CatalogNameEntryView view() const { return c::BorrowCatalogNameEntry(owned); }
};
Entry Make(const Name& n) {
  Entry e;
  auto& o = e.owned;
  o.scope_uuid = {UuidKind::object, n.scope};
  o.identifier_profile_uuid = {UuidKind::object, n.profile};
  o.object_uuid = {UuidKind::object, n.object};
  o.name_entry_uuid = {UuidKind::object, n.entry};
  o.language_uuid = c::CatalogLanguageIdentity(n.language);
  o.requires_exact_match = n.exact;
  // The other lane holds something else, to show it is not used.
  o.normalized_lookup_key = n.exact ? Text("other-lane") : n.key;
  o.exact_lookup_key = n.exact ? n.key : Text("other-lane");
  o.name_class = static_cast<c::CatalogNameClass>(n.name_class);
  return e;
}

// ---- Checks --------------------------------------------------------------------
void Golden() {
  Name n;
  n.scope = Id(1);
  n.profile = Id(2);
  n.language = c::kCatalogLanguage_en;
  n.key = {'a', 0, 'b'};
  n.kind = 0x0102;
  const auto key = c::CatalogNameAuthoritativeTreeKey(Make(n).view(), static_cast<c::CatalogRecordKind>(n.kind));
  Bytes want;
  U(want, Id(1));
  U(want, Id(2));
  U(want, c::kCatalogLanguage_en);  // fixed 16 bytes, no escape
  for (byte b : Bytes{'a', 0, 255, 'b', 0, 0, 1, 2}) want.push_back(b);
  Check(key == want, "a key written out byte by byte");
}
void Order(std::mt19937& rng) {
  std::vector<Uuid> languages;
  for (const auto& l : c::kCatalogLanguages) languages.push_back(l.uuid);
  const std::vector<Bytes> keys{{}, {0}, {0, 0}, {0, 255}, {1}, {255}, {255, 0}, {'a'}, {'a', 0}, {'a', 'b'}, {'b'}};
  std::vector<Name> names;
  for (int i = 0; i < 3000; ++i) {
    Name n;
    n.scope = Id(1 + rng() % 3);
    n.profile = Id(10 + rng() % 2);
    n.object = Id(100 + rng() % 4);
    n.entry = Id(200 + i);
    n.language = languages[rng() % languages.size()];
    n.key = keys[rng() % keys.size()];
    n.exact = rng() % 2;
    n.kind = 1 + rng() % 600;
    n.name_class = 1 + rng() % 5;
    names.push_back(n);
  }
  // Authoritative: field order scope, profile, language, key, kind.
  const auto tuple_a = [](const Name& n) {
    return std::tuple{n.scope.bytes, n.profile.bytes, n.language.bytes, n.key, n.kind};
  };
  const auto tuple_u = [](const Name& n) {
    return std::tuple{n.object.bytes, n.kind, n.language.bytes, n.name_class};
  };
  for (std::size_t i = 0; i + 1 < names.size(); ++i) {
    const auto& a = names[i];
    const auto& b = names[i + 1];
    const auto ka = c::CatalogNameAuthoritativeTreeKey(Make(a).view(), static_cast<c::CatalogRecordKind>(a.kind));
    const auto kb = c::CatalogNameAuthoritativeTreeKey(Make(b).view(), static_cast<c::CatalogRecordKind>(b.kind));
    Check(ka == OracleAuthoritative(a) && kb == OracleAuthoritative(b), "authoritative keys equal the oracle's");
    Check((ka < kb) == (tuple_a(a) < tuple_a(b)) && (ka == kb) == (tuple_a(a) == tuple_a(b)),
          "authoritative key order is the field order");
    const auto ua = c::CatalogNameUuidTreeEntryKey(Make(a).view(), static_cast<c::CatalogRecordKind>(a.kind));
    const auto ub = c::CatalogNameUuidTreeEntryKey(Make(b).view(), static_cast<c::CatalogRecordKind>(b.kind));
    Check(ua == OracleUuidEntry(a) && ub == OracleUuidEntry(b), "UUID -> name entry keys equal the oracle's");
    Check((ua < ub) == (tuple_u(a) < tuple_u(b)) && (ua == ub) == (tuple_u(a) == tuple_u(b)),
          "UUID -> name key order is the field order");
    // The vector key of an object sorts before every entry key of that object and kind.
    const auto vector = c::CatalogNameUuidTreeVectorKey(a.object, static_cast<c::CatalogRecordKind>(a.kind));
    Check(vector < ua && std::equal(vector.begin(), vector.end() - 1, ua.begin()), "the vector key sorts before its entries");
  }
  // Prefix-freeness: a name's prefix starts exactly the keys of that same name.
  for (int i = 0; i < 400; ++i) {
    const auto& a = names[rng() % names.size()];
    const auto& b = names[rng() % names.size()];
    const auto prefix = c::CatalogNameAuthoritativeTreeNamePrefix(Make(a).view());
    Check(prefix == OracleAuthoritative(a, false), "the name prefix equals the oracle's");
    const auto kb = c::CatalogNameAuthoritativeTreeKey(Make(b).view(), static_cast<c::CatalogRecordKind>(b.kind));
    const bool starts = kb.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), kb.begin());
    const bool same = a.scope == b.scope && a.profile == b.profile && a.language == b.language && a.key == b.key;
    Check(starts == same, "a name's prefix starts exactly that name's keys");
  }
}
void Payloads() {
  Name n;
  n.object = Id(7);
  n.entry = Id(8);
  const auto entry = Make(n);
  const auto a = c::CatalogNameAuthoritativeTreePayload(entry.view());
  Bytes want;
  U(want, Id(7));
  U(want, Id(8));
  Check(a == want, "authoritative payload: object then entry");
  const auto decoded = c::DecodeCatalogNameAuthoritativeTreePayload(a);
  Check(decoded && decoded->object_uuid == Id(7) && decoded->entry_uuid == Id(8), "authoritative payload round-trips");
  Check(!c::DecodeCatalogNameAuthoritativeTreePayload(std::span(a).first(31)) &&
            !c::DecodeCatalogNameAuthoritativeTreePayload(Bytes(33, 1)),
        "authoritative payload of the wrong size refused");

  c::CatalogNameVector v;
  v.name_vector_uuid = {UuidKind::object, Id(20)};
  v.default_name_entry_uuid = {UuidKind::object, Id(21)};
  v.name_collision_policy_uuid = {UuidKind::object, Id(22)};
  const auto vp = c::CatalogNameUuidTreeVectorPayload(c::BorrowCatalogNameVector(v));
  Bytes vw;
  U(vw, Id(20));
  U(vw, Id(21));
  U(vw, Id(22));
  Check(vp == vw, "vector payload: vector, default entry, collision policy");
  const auto vd = c::DecodeCatalogNameUuidTreeVectorPayload(vp);
  Check(vd && vd->vector_uuid == Id(20) && vd->default_entry_uuid == Id(21) && vd->collision_policy_uuid == Id(22),
        "vector payload round-trips");
  Check(!c::DecodeCatalogNameUuidTreeVectorPayload(std::span(vp).first(47)), "vector payload of the wrong size refused");

  const std::vector<std::string_view> path{"sales", std::string_view("q\0rt", 4), ""};
  const auto ep = c::CatalogNameUuidTreeEntryPayload(Id(30), path);
  Bytes ew;
  U(ew, Id(30));
  for (byte b : Bytes{3, 5, 's', 'a', 'l', 'e', 's', 4, 'q', 0, 'r', 't', 0}) ew.push_back(b);
  Check(ep == ew, "entry payload: entry, count, length-prefixed components");
  const auto ed = c::DecodeCatalogNameUuidTreeEntryPayload(ep);
  Check(ed && ed->entry_uuid == Id(30) && ed->display_components.size() == 3 && ed->display_components[0] == "sales" &&
            ed->display_components[1] == path[1] && ed->display_components[2].empty(),
        "entry payload round-trips, components borrowing the payload");
  const auto refused = [&](Bytes b, const std::string& what) {
    Check(!c::DecodeCatalogNameUuidTreeEntryPayload(b), "entry payload refused: " + what);
  };
  refused(Bytes(ep.begin(), ep.end() - 1), "truncated");
  {
    auto b = ep;
    b.push_back(0);
    refused(b, "a trailing byte");
  }
  {
    auto b = ep;
    b[16] = 0x83;
    b.insert(b.begin() + 17, 0x00);
    refused(b, "an overlong count");
  }
  {
    auto b = ep;
    b[16] = 9;
    refused(b, "a count past the end");
  }
  refused(Bytes(16, 1), "no count");
  {
    Bytes b;
    U(b, Id(31));
    b.push_back(0);
    const auto empty = c::DecodeCatalogNameUuidTreeEntryPayload(b);
    Check(empty && empty->display_components.empty(), "an entry payload with no components");
  }

  const std::vector<Bytes> components{{'s'}, {0}, {}};
  std::vector<std::span<const byte>> spans(components.begin(), components.end());
  Bytes fw;
  for (const auto& comp : components) Esc(fw, comp);
  Check(c::CatalogNameFullPathLookupKey(spans) == fw, "the full-path lookup key is the escaped components");
}
}  // namespace

int main() {
  try {
    std::mt19937 rng(20261109);
    Golden();
    Order(rng);
    Payloads();
    std::cout << "PASS native name tree keys checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native name tree keys: " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
