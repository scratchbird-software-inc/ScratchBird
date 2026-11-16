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
//   - payloads round-trip; malformed ones (wrong size, truncated, trailing
//     bytes) are refused; SB_NATIVE_NAME_ROW_V4: an entry payload is the entry
//     UUID only, and a key's own lane is derived (its fold is in the key).
//     (Was: LEB128 display components, and the full-path lookup key.)
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

struct Name {
  Uuid scope, profile, object, entry;
  Uuid language;  // a language constant
  Bytes key;  // own lane
  bool exact = false;
  unsigned kind = 0;
  unsigned name_class = 1;
  // SB_NATIVE_NAME_DEFAULT_INDEX (owner Q228 revisited, Q259): 0 object
  // default, 1 language default only, 2 neither.
  unsigned marker = 2;
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
  out.push_back(static_cast<byte>(n.marker));  // SB_NATIVE_NAME_DEFAULT_INDEX (was: none)
  out.push_back(static_cast<byte>(n.name_class));
  return out;
}
// The production view of a name. SB_NATIVE_NAME_ROW_V4 (owner Q147 = 1): the
// row holds the raw text; the own-lane key is the raw text under the lane's
// fold (none here, so the key is the oracle's bytes). (Was: the key stored in
// the lane the entry used, the other lane holding something else.)
struct Entry {
  c::CatalogNameEntry owned;
  c::CatalogNameEntryView view() const { return c::BorrowCatalogNameEntry(owned); }
  c::CatalogNameDerivedText own() const { return {owned.raw_name_text, c::IdentifierFoldMode::none}; }
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
  o.raw_name_text.assign(n.key.begin(), n.key.end());
  o.name_class = static_cast<c::CatalogNameClass>(n.name_class);
  o.default_for_object = n.marker == 0;
  o.default_for_language = n.marker <= 1;
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
  const auto key = c::CatalogNameAuthoritativeTreeKey(Make(n).view(), Make(n).own(), static_cast<c::CatalogRecordKind>(n.kind));
  Bytes want;
  U(want, Id(1));
  U(want, Id(2));
  U(want, c::kCatalogLanguage_en);  // fixed 16 bytes, no escape
  for (byte b : Bytes{'a', 0, 255, 'b', 0, 0, 1, 2}) want.push_back(b);
  Check(key == want, "a key written out byte by byte");
  // SB_NATIVE_NAME_ROW_V4: the key carries the derived lane, folded as the
  // lane says; the matcher the startup verifier uses agrees with the encoder
  // and refuses a key whose own-lane part is the unfolded text (risk R4).
  n.key = {'a', 'B', 'c'};
  const auto e = Make(n);
  const c::CatalogNameDerivedText upper{e.owned.raw_name_text, c::IdentifierFoldMode::upper_ascii};
  const auto folded = c::CatalogNameAuthoritativeTreeKey(e.view(), upper, static_cast<c::CatalogRecordKind>(n.kind));
  Bytes fw;
  U(fw, Id(1));
  U(fw, Id(2));
  U(fw, c::kCatalogLanguage_en);
  for (byte b : Bytes{'A', 'B', 'C', 0, 0, 1, 2}) fw.push_back(b);
  Check(folded == fw, "the own lane's fold is in the key");
  Check(c::CatalogNameAuthoritativeTreeKeyEquals(e.view(), upper, static_cast<c::CatalogRecordKind>(n.kind), folded),
        "the startup matcher accepts the key the encoder wrote");
  Check(!c::CatalogNameAuthoritativeTreeKeyEquals(e.view(), e.own(), static_cast<c::CatalogRecordKind>(n.kind), folded),
        "the startup matcher refuses a stored key that disagrees with the derived lane");
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
    n.marker = rng() % 3;
    names.push_back(n);
  }
  // Authoritative: field order scope, profile, language, key, kind.
  const auto tuple_a = [](const Name& n) {
    return std::tuple{n.scope.bytes, n.profile.bytes, n.language.bytes, n.key, n.kind};
  };
  const auto tuple_u = [](const Name& n) {
    return std::tuple{n.object.bytes, n.kind, n.language.bytes, n.marker, n.name_class};
  };
  for (std::size_t i = 0; i + 1 < names.size(); ++i) {
    const auto& a = names[i];
    const auto& b = names[i + 1];
    const auto ka = c::CatalogNameAuthoritativeTreeKey(Make(a).view(), Make(a).own(), static_cast<c::CatalogRecordKind>(a.kind));
    const auto kb = c::CatalogNameAuthoritativeTreeKey(Make(b).view(), Make(b).own(), static_cast<c::CatalogRecordKind>(b.kind));
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
    const auto prefix = c::CatalogNameAuthoritativeTreeNamePrefix(Make(a).view(), Make(a).own());
    Check(prefix == OracleAuthoritative(a, false), "the name prefix equals the oracle's");
    const auto kb = c::CatalogNameAuthoritativeTreeKey(Make(b).view(), Make(b).own(), static_cast<c::CatalogRecordKind>(b.kind));
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
  // SB_NATIVE_NAME_DEFAULTS_ON_ENTRIES (owner Q228 revisited): no vector
  // default; the payload is vector | collision policy. (Was: vector, default
  // entry Id(21), collision policy; 48 bytes.)
  v.name_collision_policy_uuid = {UuidKind::object, Id(22)};
  const auto vp = c::CatalogNameUuidTreeVectorPayload(c::BorrowCatalogNameVector(v));
  Bytes vw;
  U(vw, Id(20));
  U(vw, Id(22));
  Check(vp == vw, "vector payload: vector, collision policy");
  const auto vd = c::DecodeCatalogNameUuidTreeVectorPayload(vp);
  Check(vd && vd->vector_uuid == Id(20) && vd->collision_policy_uuid == Id(22), "vector payload round-trips");
  Check(!c::DecodeCatalogNameUuidTreeVectorPayload(std::span(vp).first(31)), "vector payload of the wrong size refused");

  // SB_NATIVE_NAME_ROW_V4 (owner Q158 = 1): the UUID-to-name entry payload is
  // the entry UUID only; rendering reads the row. (Was: entry, LEB128 count,
  // length-prefixed display components; malformed counts and lengths were
  // refused; and the full-path lookup key was the escaped components,
  // concatenated.)
  // SB_NATIVE_NAME_DEFAULT_INDEX (owner Q228 revisited, Q259: "an index that
  // holds data as well"): the entry payload is the entry UUID then its raw
  // name, so the index covers rendering. (Was, SB_NATIVE_NAME_ROW_V4: the
  // entry UUID only, a trailing byte refused.)
  Name pn;
  pn.entry = Id(30);
  pn.key = {'n', 0, 'm'};
  const auto ep = c::CatalogNameUuidTreeEntryPayload(Make(pn).view());
  Bytes ew;
  U(ew, Id(30));
  for (byte b : pn.key) ew.push_back(b);
  Check(ep == ew, "entry payload: the entry UUID and the name");
  const auto ed = c::DecodeCatalogNameUuidTreeEntryPayload(ep);
  Check(ed && *ed == Id(30), "entry payload round-trips");
  Check(!c::DecodeCatalogNameUuidTreeEntryPayload(Bytes(ep.begin(), ep.begin() + 15)), "entry payload refused: truncated");
  Check(c::CatalogNameUuidTreeEntryPayloadEquals(Make(pn).view(), ep), "the startup matcher accepts the entry's payload");
  {
    auto b = ep;
    b.push_back(0);
    Check(!c::CatalogNameUuidTreeEntryPayloadEquals(Make(pn).view(), b), "the startup matcher refuses a payload with another name");
  }
  pn.entry = Id(31);
  Check(!c::CatalogNameUuidTreeEntryPayloadEquals(Make(pn).view(), ep), "the startup matcher refuses another entry's payload");
  // The default prefix: object | kind | 0x01 | language holds markers 0 and
  // 1 (the (object, language)'s defaults) before 2.
  Name dn;
  dn.object = Id(40);
  dn.language = c::kCatalogLanguage_en;
  dn.kind = 5;
  const auto prefix = c::CatalogNameUuidTreeLanguagePrefix(dn.object, static_cast<c::CatalogRecordKind>(dn.kind), dn.language);
  for (unsigned marker = 0; marker < 3; ++marker) {
    dn.marker = marker;
    const auto key = c::CatalogNameUuidTreeEntryKey(Make(dn).view(), static_cast<c::CatalogRecordKind>(dn.kind));
    Check(std::equal(prefix.begin(), prefix.end(), key.begin()) && key[prefix.size()] == marker,
          "the default marker follows the language prefix");
  }
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
