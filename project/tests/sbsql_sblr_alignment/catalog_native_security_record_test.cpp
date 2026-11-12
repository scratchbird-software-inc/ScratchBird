// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SEARCH_KEY: SB_CATALOG_NATIVE_SECURITY_RECORDS (owner Q61 = 1, Q62 = 1, Q65 = 1).
// Typed native principal/role/group/grant value blocks: an independent
// hand-built SBCV byte oracle per record (no production schema or encoder),
// round trips, every-field refusals, grant class rules, credential
// all-or-none and range, and every listed right. Codec only; not wired.
#include "catalog_native_security_record.hpp"
#include "bootstrap_policy_fixture.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <set>
#include <source_location>
#include <stdexcept>
thread_local long allocation_budget = -1;
thread_local unsigned allocation_count = 0;
void* operator new(std::size_t size) {
  ++allocation_count;
  if (allocation_budget >= 0 && allocation_budget-- == 0) throw std::bad_alloc();
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace f = scratchbird::tests::bootstrap_policy;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* why, std::source_location loc = std::source_location::current()) {
  ++checks;
  if (!ok) {
    allocation_budget = -1;
    throw std::runtime_error(std::string(why) + " line=" + std::to_string(loc.line()));
  }
}
template <class Call>
auto NoHeap(Call&& call) {
  allocation_budget = 0;
  allocation_count = 0;
  auto result = call();
  const auto calls = allocation_count;
  allocation_budget = -1;
  Check(!calls, "bounded native security codecs never allocate");
  return result;
}

// ---- independent oracle: hand-built SBCV with wire overrides ----
// omit: tags left off the wire; number: tag -> raw u64 replacing the value;
// opaque/boolean: tag -> raw bytes replacing the value; append: extra
// trailing integer fields.
struct Wire {
  std::set<unsigned> omit;
  std::map<unsigned, p::u64> number;
  std::map<unsigned, std::string> opaque;
  std::map<unsigned, std::string> boolean;
  std::map<unsigned, p::u64> append;
} wire;
struct G {
  f::Golden g;
  explicit G(unsigned schema) : g(schema) {}
  void Id(unsigned tag, const p::Uuid& id) {
    if (!wire.omit.count(tag)) g.Identity(tag, id);
  }
  void OptId(unsigned tag, const std::optional<p::Uuid>& id) {
    if (id) Id(tag, *id);
  }
  void Int(unsigned tag, p::u64 v) {
    if (wire.omit.count(tag)) return;
    if (auto it = wire.number.find(tag); it != wire.number.end()) v = it->second;
    g.Integer(tag, v);
  }
  template <class En>
  void OptInt(unsigned tag, const std::optional<En>& v) {
    if (v) Int(tag, static_cast<p::u64>(*v));
  }
  void Bool(unsigned tag, bool v) {
    if (wire.omit.count(tag)) return;
    if (auto it = wire.boolean.find(tag); it != wire.boolean.end()) return g.Field(tag, 2, it->second);
    g.Boolean(tag, v);
  }
  template <std::size_t N>
  void Opaque(unsigned tag, const std::array<p::byte, N>& b) {
    if (wire.omit.count(tag)) return;
    if (auto it = wire.opaque.find(tag); it != wire.opaque.end()) return g.Field(tag, 4, it->second);
    g.Field(tag, 4, {reinterpret_cast<const char*>(b.data()), N});
  }
  template <class R>
  void Common(const R& r, const p::Uuid& primary) {
    Id(1, primary);
    Id(2, r.origin_transaction_uuid.value);
    Int(3, r.origin_local_transaction_id);
    Int(4, r.creator_tx);
    Int(5, r.policy_generation);
    OptId(6, r.policy_pack_uuid);
  }
  std::string Finish() {
    for (const auto& [tag, v] : wire.append) g.Integer(tag, v);
    return g.Finish();
  }
};
std::string Golden(const c::NativeSecurityPrincipal& r) {
  G g(65635);
  g.Common(r, r.principal_uuid);
  g.Int(7, p::u64(r.principal_kind));
  g.OptInt(8, r.authority_class);
  g.Bool(9, r.active);
  g.Bool(10, r.create_time_only);
  g.Bool(11, r.bootstrap_principal);
  g.Int(12, r.security_generation);
  g.Int(13, r.security_context_authority_version);
  g.Int(14, r.security_context_generation);
  if (r.credential) {
    g.Int(15, p::u64(r.credential->scheme));
    g.Int(16, r.credential->iterations);
    g.Opaque(17, r.credential->salt);
    g.Opaque(18, r.credential->verifier);
  }
  return g.Finish();
}
std::string Golden(const c::NativeSecurityRole& r) {
  G g(65636);
  g.Common(r, r.role_uuid);
  g.OptInt(7, r.authority_class);
  g.Bool(8, r.engine_owned);
  g.Bool(9, r.immutable);
  g.Bool(10, r.active);
  g.Bool(11, r.loaded_at_database_create);
  g.Bool(12, r.create_time_only);
  g.Int(13, r.security_context_authority_version);
  g.Int(14, r.security_context_generation);
  return g.Finish();
}
std::string Golden(const c::NativeSecurityGroup& r) {
  G g(65637);
  g.Common(r, r.group_uuid);
  g.Bool(7, r.engine_owned);
  g.Bool(8, r.loaded_at_database_create);
  return g.Finish();
}
std::string Golden(const c::NativeSecurityGrant& r) {
  G g(65638);
  g.Common(r, r.grant_uuid);
  g.Int(7, p::u64(r.grant_class));
  g.OptId(8, r.subject_uuid);
  g.OptId(9, r.member_uuid);
  g.OptId(10, r.parent_uuid);
  g.OptId(11, r.target_uuid);
  g.OptId(12, r.membership_uuid);
  g.OptId(13, r.principal_uuid);
  g.OptId(14, r.role_uuid);
  g.OptInt(15, r.subject_kind);
  g.OptInt(16, r.member_kind);
  g.OptInt(17, r.parent_kind);
  g.OptInt(18, r.effect);
  g.OptInt(19, r.right);
  g.Int(20, r.security_generation);
  return g.Finish();
}
// The oracle with one-off wire overrides, reset afterwards.
template <class R>
std::string GoldenWith(const R& r, std::function<void(Wire&)> set) {
  wire = {};
  set(wire);
  auto out = Golden(r);
  wire = {};
  return out;
}

// ---- codec table ----
template <class R>
struct Codec {
  std::optional<std::size_t> (*bytes)(const R&);
  c::CatalogValueEncodeIntoResult (*into)(const R&, std::span<p::byte>);
  c::CatalogValueEncodeResult (*owning)(const R&);
  c::NativeSecurityDecodeResult<R> (*decode)(std::string_view);
  std::size_t max;
};
const Codec<c::NativeSecurityPrincipal> principal_codec{
    c::NativeSecurityPrincipalEncodedBytes, c::EncodeNativeSecurityPrincipalInto, c::EncodeNativeSecurityPrincipal,
    c::DecodeNativeSecurityPrincipal, c::kNativeSecurityPrincipalMaxBytes};
const Codec<c::NativeSecurityRole> role_codec{c::NativeSecurityRoleEncodedBytes, c::EncodeNativeSecurityRoleInto,
                                              c::EncodeNativeSecurityRole, c::DecodeNativeSecurityRole,
                                              c::kNativeSecurityRoleMaxBytes};
const Codec<c::NativeSecurityGroup> group_codec{c::NativeSecurityGroupEncodedBytes, c::EncodeNativeSecurityGroupInto,
                                                c::EncodeNativeSecurityGroup, c::DecodeNativeSecurityGroup,
                                                c::kNativeSecurityGroupMaxBytes};
const Codec<c::NativeSecurityGrant> grant_codec{c::NativeSecurityGrantEncodedBytes, c::EncodeNativeSecurityGrantInto,
                                                c::EncodeNativeSecurityGrant, c::DecodeNativeSecurityGrant,
                                                c::kNativeSecurityGrantMaxBytes};
template <class R> const Codec<R>& CodecOf();
template <> const Codec<c::NativeSecurityPrincipal>& CodecOf() { return principal_codec; }
template <> const Codec<c::NativeSecurityRole>& CodecOf() { return role_codec; }
template <> const Codec<c::NativeSecurityGroup>& CodecOf() { return group_codec; }
template <> const Codec<c::NativeSecurityGrant>& CodecOf() { return grant_codec; }

std::string_view View(std::span<const p::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }
template <class R>
void Roundtrip(const R& value) {
  const auto& k = CodecOf<R>();
  const auto expected = Golden(value);
  std::array<p::byte, 512> output;
  output.fill(0xa7);
  const auto size = NoHeap([&] { return k.bytes(value); });
  Check(size && *size == expected.size() && *size <= k.max, "independent complete byte count within maximum");
  const auto encoded = NoHeap([&] { return k.into(value, std::span(output).subspan(1, *size + 3)); });
  Check(encoded.ok() && View(encoded.bytes) == expected, "exact independent SBCV bytes");
  Check(output.front() == 0xa7 && std::all_of(output.begin() + expected.size() + 1, output.end(),
                                              [](auto b) { return b == 0xa7; }),
        "unaligned destination preserves guards");
  const auto owned = k.owning(value);
  Check(owned.ok() && View(owned.bytes) == expected, "owning encode equals the oracle");
  const auto decoded = NoHeap([&] { return k.decode(expected); });
  Check(decoded.ok() && Golden(*decoded.record) == expected, "every field retained through decode");
  for (std::size_t n = 0; n < expected.size(); ++n) {
    Check(!NoHeap([&] { return k.decode(std::string_view(expected).substr(0, n)); }).ok(), "truncated wire refused");
  }
  Check(!k.decode(expected + 'x').ok(), "trailing byte refused");
  std::array<p::byte, 512> shorter;
  shorter.fill(0xab);
  const auto before = shorter;
  const auto short_write = NoHeap([&] { return k.into(value, std::span(shorter).first(expected.size() - 1)); });
  Check(!short_write.ok() && short_write.bytes.empty() && shorter == before, "short output never writes a prefix");
}
template <class R>
void Refuse(const R& value, bool wire_too = true) {
  const auto& k = CodecOf<R>();
  std::array<p::byte, 512> output;
  output.fill(0xa7);
  const auto before = output;
  const auto encoded = NoHeap([&] { return k.into(value, output); });
  Check(!encoded.ok() && encoded.bytes.empty() && output == before, "invalid record leaves output unchanged");
  Check(!NoHeap([&] { return k.bytes(value); }), "invalid record has no encoded size");
  Check(!k.owning(value).ok(), "owning encode refuses invalid record");
  if (wire_too) {
    const auto raw = Golden(value);
    const auto decoded = NoHeap([&] { return k.decode(raw); });
    Check(!decoded.ok() && !decoded.record, "invalid record bytes refused on decode");
  }
}
template <class R>
void RefuseWire(const std::string& raw, const char* why) {
  const auto decoded = NoHeap([&] { return CodecOf<R>().decode(raw); });
  Check(!decoded.ok() && !decoded.record, why);
}
// Each required field left off the wire is refused; each optional one is not
// refused for being absent (if the record stays valid).
template <class R>
void RequiredOnWire(const R& full, std::initializer_list<unsigned> required) {
  for (unsigned tag : required)
    RefuseWire<R>(GoldenWith(full, [&](Wire& w) { w.omit.insert(tag); }), "missing required field refused");
  RefuseWire<R>(GoldenWith(full, [&](Wire& w) { w.append[99] = 1; }), "unknown field refused");
}
template <class R>
void CommonRefusals(R base, p::Uuid R::*primary) {
  auto b = base; b.*primary = {}; Refuse(b);
  b = base; (b.*primary).bytes[6] = 0x40; Refuse(b);  // version 4, not an engine identity
  b = base; b.origin_transaction_uuid.value = {}; Refuse(b);
  b = base; b.origin_transaction_uuid.value = b.*primary; Refuse(b);
  b = base; b.origin_transaction_uuid.kind = p::UuidKind::object; Refuse(b, false);
  b = base; b.origin_local_transaction_id = 0; Refuse(b);
  b = base; b.policy_pack_uuid = p::Uuid{}; Refuse(b);
  b = base; b.policy_pack_uuid = b.*primary; Refuse(b);
  b = base; b.policy_pack_uuid = b.origin_transaction_uuid.value; Refuse(b);
  b = base; b.creator_tx = 0; b.policy_generation = 0; Roundtrip(b);  // creator_tx/policy_generation may be 0
  b = base; b.creator_tx = ~p::u64{0}; b.policy_generation = ~p::u64{0}; Roundtrip(b);
  b = base; b.policy_pack_uuid = f::Id(90); Roundtrip(b);
  // Framing: wrong schema id, version and unknown field.
  const auto raw = Golden(base);
  for (const unsigned offset : {0u, 4u, 6u, 8u, 12u, 16u, 20u, 22u, 24u, 26u}) {
    auto invalid = raw;
    invalid[offset] ^= 0x7f;
    RefuseWire<R>(invalid, "invalid framing/schema/tag/type refused");
  }
}
p::TypedUuid Tx(unsigned n) { return {p::UuidKind::transaction, f::Id(n)}; }

// ---- principal ----
c::NativeSecurityPrincipal Principal() {
  c::NativeSecurityPrincipal r;
  r.principal_uuid = f::Id(101);
  r.origin_transaction_uuid = Tx(5);
  r.origin_local_transaction_id = 1;
  r.creator_tx = 1;
  r.policy_generation = 3;
  r.principal_kind = c::CatalogSecurityPrincipalKind::user;
  r.active = true;
  r.security_generation = 7;
  r.security_context_authority_version = 2;
  r.security_context_generation = 4;
  return r;
}
c::NativeSecurityCredential Credential() {
  c::NativeSecurityCredential k;
  k.scheme = c::CatalogSecurityCredentialScheme::pbkdf2_sha256_v1;
  k.iterations = 600000;
  for (unsigned i = 0; i < 16; ++i) k.salt[i] = p::byte(i + 1);
  for (unsigned i = 0; i < 32; ++i) k.verifier[i] = p::byte(0x80 + i);
  return k;
}
void Principals() {
  const auto base = Principal();
  Roundtrip(base);
  Check(Golden(base).size() == 24 + 2 * 24 + 7 * 16 + 3 * 9, "principal minimum byte count");
  auto b = base;
  b.principal_kind = c::CatalogSecurityPrincipalKind::service;
  Roundtrip(b);
  for (unsigned a = 1; a <= 6; ++a) {
    b = base;
    b.authority_class = c::CatalogSecurityAuthorityClass(a);
    Roundtrip(b);
  }
  b = base;
  b.active = false; b.create_time_only = true; b.bootstrap_principal = true;
  Roundtrip(b);
  b = base;
  b.security_generation = b.security_context_authority_version = b.security_context_generation = 0;
  Roundtrip(b);
  auto full = base;
  full.policy_pack_uuid = f::Id(90);
  full.authority_class = c::CatalogSecurityAuthorityClass::request_action;
  full.credential = Credential();
  Roundtrip(full);
  Check(Golden(full).size() == c::kNativeSecurityPrincipalMaxBytes, "every optional field present = registered maximum");
  for (const p::u64 it : {600000ull, 600001ull, 9999999ull, 10000000ull}) {
    b = full; b.credential->iterations = it; Roundtrip(b);
  }
  for (const p::u64 it : {0ull, 1ull, 599999ull, 10000001ull, ~0ull}) {
    b = full; b.credential->iterations = it; Refuse(b);
  }
  b = full; b.credential->scheme = c::CatalogSecurityCredentialScheme::none; Refuse(b);
  b = full; b.credential->scheme = c::CatalogSecurityCredentialScheme(2); Refuse(b);
  b = full; b.credential->salt.fill(0); Refuse(b);
  b = full; b.credential->verifier.fill(0); Refuse(b);
  b = full; b.credential->salt.fill(0); b.credential->salt[15] = 1; Roundtrip(b);
  b = full; b.credential->verifier.fill(0); b.credential->verifier[0] = 1; Roundtrip(b);
  CommonRefusals(base, &c::NativeSecurityPrincipal::principal_uuid);
  b = base; b.principal_kind = c::CatalogSecurityPrincipalKind::none; Refuse(b);
  b = base; b.principal_kind = c::CatalogSecurityPrincipalKind(3); Refuse(b);
  b = base; b.authority_class = c::CatalogSecurityAuthorityClass::none; Refuse(b);
  b = base; b.authority_class = c::CatalogSecurityAuthorityClass(7); Refuse(b);
  using P = c::NativeSecurityPrincipal;
  // u16 enum carried as u64: a wider wire value is refused, never truncated.
  RefuseWire<P>(GoldenWith(base, [](Wire& w) { w.number[7] = 0x10001; }), "principal_kind wider than u16 refused");
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.number[8] = 0x10001; }), "authority_class wider than u16 refused");
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.number[15] = 0x10001; }), "credential scheme wider than u16 refused");
  // Credential all-or-none on the wire.
  for (unsigned tag = 15; tag <= 18; ++tag)
    RefuseWire<P>(GoldenWith(full, [&](Wire& w) { w.omit.insert(tag); }), "partial credential (one field missing) refused");
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.omit = {16, 17, 18}; }), "credential scheme alone refused");
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.omit = {15, 16, 18}; }), "credential salt alone refused");
  // Exact opaque lengths.
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.opaque[17] = std::string(15, '\x01'); }), "15-byte salt refused");
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.opaque[17] = std::string(17, '\x01'); }), "17-byte salt refused");
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.opaque[17] = ""; }), "empty salt refused");
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.opaque[18] = std::string(31, '\x01'); }), "31-byte verifier refused");
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.opaque[18] = std::string(33, '\x01'); }), "33-byte verifier refused");
  RefuseWire<P>(GoldenWith(full, [](Wire& w) { w.boolean[9] = std::string(1, '\x02'); }), "boolean other than 0/1 refused");
  RequiredOnWire(full, {1, 2, 3, 4, 5, 7, 9, 10, 11, 12, 13, 14});
  // Optional fields absent on the wire decode as absent.
  const auto bare = c::DecodeNativeSecurityPrincipal(GoldenWith(full, [](Wire& w) { w.omit = {6, 8, 15, 16, 17, 18}; }));
  Check(bare.ok() && !bare.record->policy_pack_uuid && !bare.record->authority_class && !bare.record->credential,
        "optional principal fields absent");
  // A principal block is not a role/group/grant block.
  RefuseWire<c::NativeSecurityRole>(Golden(base), "principal schema id refused by the role decoder");
  RefuseWire<c::NativeSecurityGrant>(Golden(base), "principal schema id refused by the grant decoder");
  auto overlap = full;
  Check(NoHeap([&] {
          return c::EncodeNativeSecurityPrincipalInto(overlap, {reinterpret_cast<p::byte*>(&overlap), sizeof(overlap)});
        }).error == c::CatalogValueError::invalid_backing,
        "encoding cannot overwrite its input object");
}

// ---- role ----
c::NativeSecurityRole Role() {
  c::NativeSecurityRole r;
  r.role_uuid = f::Id(201);
  r.origin_transaction_uuid = Tx(5);
  r.origin_local_transaction_id = 1;
  r.creator_tx = 1;
  r.policy_generation = 1;
  r.engine_owned = r.immutable = r.active = r.loaded_at_database_create = true;
  return r;
}
void Roles() {
  const auto base = Role();
  Roundtrip(base);  // both counters 0: required, value may be 0
  auto b = base;
  b.engine_owned = b.immutable = b.active = b.loaded_at_database_create = false;
  b.create_time_only = true;
  b.security_context_authority_version = 9; b.security_context_generation = ~p::u64{0};
  Roundtrip(b);
  for (unsigned a = 1; a <= 6; ++a) {
    b = base; b.authority_class = c::CatalogSecurityAuthorityClass(a); Roundtrip(b);
  }
  auto full = base;
  full.policy_pack_uuid = f::Id(90);
  full.authority_class = c::CatalogSecurityAuthorityClass::l5_database;
  Roundtrip(full);
  Check(Golden(full).size() == c::kNativeSecurityRoleMaxBytes, "role maximum byte count");
  CommonRefusals(base, &c::NativeSecurityRole::role_uuid);
  b = base; b.authority_class = c::CatalogSecurityAuthorityClass::none; Refuse(b);
  b = base; b.authority_class = c::CatalogSecurityAuthorityClass(7); Refuse(b);
  using R = c::NativeSecurityRole;
  RefuseWire<R>(GoldenWith(full, [](Wire& w) { w.number[7] = 0x10002; }), "role authority_class wider than u16 refused");
  RequiredOnWire(full, {1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14});
  RefuseWire<c::NativeSecurityGroup>(Golden(base), "role schema id refused by the group decoder");
}

// ---- group ----
c::NativeSecurityGroup Group() {
  c::NativeSecurityGroup r;
  r.group_uuid = f::Id(251);
  r.origin_transaction_uuid = Tx(5);
  r.origin_local_transaction_id = 1;
  r.creator_tx = 1;
  r.policy_generation = 1;
  return r;
}
void Groups() {
  const auto base = Group();
  Roundtrip(base);
  auto b = base; b.engine_owned = true; b.loaded_at_database_create = true; Roundtrip(b);
  auto full = b; full.policy_pack_uuid = f::Id(90); Roundtrip(full);
  Check(Golden(full).size() == c::kNativeSecurityGroupMaxBytes, "group maximum byte count");
  CommonRefusals(base, &c::NativeSecurityGroup::group_uuid);
  RequiredOnWire(full, {1, 2, 3, 4, 5, 7, 8});
  RefuseWire<c::NativeSecurityPrincipal>(Golden(base), "group schema id refused by the principal decoder");
}

// ---- grant ----
c::NativeSecurityGrant GrantBase() {
  c::NativeSecurityGrant r;
  r.grant_uuid = f::Id(301);
  r.origin_transaction_uuid = Tx(5);
  r.origin_local_transaction_id = 1;
  r.creator_tx = 1;
  r.policy_generation = 1;
  r.security_generation = 1;
  return r;
}
c::NativeSecurityGrant Privilege() {
  auto r = GrantBase();
  r.grant_class = c::CatalogSecurityGrantClass::privilege;
  r.subject_uuid = f::Id(310);
  r.subject_kind = c::CatalogSecuritySubjectKind::role;
  r.right = c::CatalogSecurityRight::SELECT;
  r.effect = c::CatalogSecurityEffect::allow;
  r.target_uuid = f::Id(311);
  return r;
}
c::NativeSecurityGrant Membership(c::CatalogSecurityGrantClass k) {
  auto r = GrantBase();
  r.grant_class = k;
  r.member_uuid = f::Id(320);
  r.parent_uuid = f::Id(321);
  r.member_kind = k == c::CatalogSecurityGrantClass::group_membership ? c::CatalogSecuritySubjectKind::group
                                                                       : c::CatalogSecuritySubjectKind::principal;
  r.parent_kind = c::CatalogSecuritySubjectKind::role;
  return r;
}
c::NativeSecurityGrant Binding() {
  auto r = GrantBase();
  r.grant_class = c::CatalogSecurityGrantClass::agent_principal_binding;
  r.subject_uuid = f::Id(330);
  r.subject_kind = c::CatalogSecuritySubjectKind::principal;
  r.target_uuid = f::Id(331);
  return r;
}
void Grants() {
  using K = c::CatalogSecurityGrantClass;
  using S = c::CatalogSecuritySubjectKind;
  using Eff = c::CatalogSecurityEffect;
  const auto priv = Privilege(), role_m = Membership(K::role_membership), group_m = Membership(K::group_membership),
             bind = Binding();
  for (const auto& g : {priv, role_m, group_m, bind}) Roundtrip(g);
  auto b = priv; b.target_uuid.reset(); Roundtrip(b);
  b = priv; b.effect = Eff::deny; Roundtrip(b);
  for (const auto s : {S::principal, S::role, S::group}) { b = priv; b.subject_kind = s; Roundtrip(b); }
  // Coordinator correction 2026-11-12 (3a-2): for both membership classes
  // every (member principal|role|group, parent role|group) pair is accepted
  // against the independent oracle; the class name is a legacy label, the
  // kinds decide the edge. A principal parent is refused (below).
  for (const auto& m : {role_m, group_m})
    for (const auto mk : {S::principal, S::role, S::group})
      for (const auto pk : {S::role, S::group}) { b = m; b.member_kind = mk; b.parent_kind = pk; Roundtrip(b); }
  b = role_m; b.effect = Eff::allow; b.membership_uuid = f::Id(322); Roundtrip(b);
  b = group_m; b.effect = Eff::allow; Roundtrip(b);
  b = bind; b.principal_uuid = f::Id(332); b.role_uuid = f::Id(333); b.membership_uuid = f::Id(334); Roundtrip(b);
  // Every listed right is accepted in a privilege grant.
  unsigned rights = 0;
  for (const auto& r : c::kCatalogSecurityRights) { b = priv; b.right = r.right; Roundtrip(b); ++rights; }
  Check(rights == 122, "all 122 listed rights exercised");
  // Maximum: every optional field present (not a valid class shape; bounded by size only).
  {
    f::Golden g(65638);
    for (unsigned t = 1; t <= 20; ++t) {
      if (t == 2) g.Identity(t, f::Id(5));
      else if (t == 1 || t == 6 || (t >= 8 && t <= 14)) g.Identity(t, f::Id(400 + t));
      else g.Integer(t, 1);
    }
    Check(g.Finish().size() == c::kNativeSecurityGrantMaxBytes, "grant maximum byte count");
  }
  CommonRefusals(priv, &c::NativeSecurityGrant::grant_uuid);
  // Enumerations.
  b = priv; b.grant_class = K::none; Refuse(b);
  b = priv; b.grant_class = K(5); Refuse(b);
  b = priv; b.subject_kind = S::none; Refuse(b);
  b = priv; b.subject_kind = S(4); Refuse(b);
  b = priv; b.effect = Eff::none; Refuse(b);
  b = priv; b.effect = Eff(3); Refuse(b);
  b = priv; b.right = c::CatalogSecurityRight::none; Refuse(b);
  b = priv; b.right = c::CatalogSecurityRight(123); Refuse(b);  // unknown right
  b = priv; b.right = c::CatalogSecurityRight(999); Refuse(b);
  b = priv; b.right = c::CatalogSecurityRight(0xffff); Refuse(b);
  b = role_m; b.member_kind = S::none; Refuse(b);
  b = role_m; b.member_kind = S(4); Refuse(b);
  b = role_m; b.parent_kind = S::none; Refuse(b);
  b = role_m; b.parent_kind = S(4); Refuse(b);
  using Gt = c::NativeSecurityGrant;
  RefuseWire<Gt>(GoldenWith(priv, [](Wire& w) { w.number[7] = 0x10001; }), "grant_class wider than u16 refused");
  RefuseWire<Gt>(GoldenWith(priv, [](Wire& w) { w.number[15] = 0x10002; }), "subject_kind wider than u16 refused");
  RefuseWire<Gt>(GoldenWith(role_m, [](Wire& w) { w.number[16] = 0x10001; }), "member_kind wider than u16 refused");
  RefuseWire<Gt>(GoldenWith(role_m, [](Wire& w) { w.number[17] = 0x10002; }), "parent_kind wider than u16 refused");
  RefuseWire<Gt>(GoldenWith(priv, [](Wire& w) { w.number[18] = 0x10001; }), "effect wider than u16 refused");
  RefuseWire<Gt>(GoldenWith(priv, [](Wire& w) { w.number[19] = 0x10006; }), "right wider than u16 refused (not SELECT)");
  RequiredOnWire(priv, {1, 2, 3, 4, 5, 7, 20});
  // Privilege rules.
  b = priv; b.subject_uuid.reset(); Refuse(b);
  b = priv; b.subject_kind.reset(); Refuse(b);
  b = priv; b.right.reset(); Refuse(b);
  b = priv; b.effect.reset(); Refuse(b);
  b = priv; b.member_uuid = f::Id(320); Refuse(b);
  b = priv; b.parent_uuid = f::Id(321); Refuse(b);
  b = priv; b.member_kind = S::principal; Refuse(b);
  b = priv; b.parent_kind = S::role; Refuse(b);
  RefuseWire<Gt>(GoldenWith(priv, [](Wire& w) { w.omit.insert(19); }), "privilege without right refused on wire");
  // Membership rules (both classes).
  for (const auto& m : {role_m, group_m}) {
    b = m; b.member_uuid.reset(); Refuse(b);
    b = m; b.parent_uuid.reset(); Refuse(b);
    b = m; b.member_kind.reset(); Refuse(b);
    b = m; b.parent_kind.reset(); Refuse(b);
    // (3a-2 correction: parent_kind = group is now accepted, above.)
    for (const auto mk : {S::principal, S::role, S::group}) { b = m; b.member_kind = mk; b.parent_kind = S::principal; Refuse(b); }
    b = m; b.effect = Eff::deny; Refuse(b);
    b = m; b.right = c::CatalogSecurityRight::SELECT; Refuse(b);
    b = m; b.subject_uuid = f::Id(310); Refuse(b);
    b = m; b.subject_kind = S::role; Refuse(b);
    b = m; b.parent_uuid = b.member_uuid; Refuse(b);
  }
  // (3a-2 correction: a group_membership member may be a principal or role,
  // accepted above; it was refused under the 3a-1 rule.)
  // Agent principal binding rules.
  b = bind; b.subject_uuid.reset(); Refuse(b);
  b = bind; b.subject_kind.reset(); Refuse(b);
  b = bind; b.subject_kind = S::role; Refuse(b);
  b = bind; b.subject_kind = S::group; Refuse(b);
  b = bind; b.target_uuid.reset(); Refuse(b);
  b = bind; b.right = c::CatalogSecurityRight::CONNECT; Refuse(b);
  b = bind; b.effect = Eff::allow; Refuse(b);
  b = bind; b.member_uuid = f::Id(320); Refuse(b);
  b = bind; b.parent_kind = S::role; Refuse(b);
  // Optional identities: nil, non-v7, equal to the grant or origin transaction.
  for (auto field : {&Gt::subject_uuid, &Gt::target_uuid, &Gt::membership_uuid, &Gt::principal_uuid, &Gt::role_uuid}) {
    b = bind; b.*field = p::Uuid{}; Refuse(b);
    b = bind; b.*field = b.grant_uuid; Refuse(b);
    b = bind; b.*field = b.origin_transaction_uuid.value; Refuse(b);
    b = bind; b.*field = f::Id(340); (*(b.*field)).bytes[6] = 0x40; Refuse(b);
  }
  b = role_m; b.member_uuid = b.grant_uuid; Refuse(b);
  b = role_m; b.parent_uuid = p::Uuid{}; Refuse(b);
}

// ---- metadata binding ----
template <class R>
c::CatalogMetadataVersion Meta(const R& r, const p::Uuid& primary, c::CatalogRecordKind kind) {
  auto m = f::Metadata(0, r.origin_transaction_uuid, f::Id(60), Golden(r));
  m.record.header.kind = kind;
  m.record.header.object_uuid = {p::UuidKind::object, primary};
  using O = c::CatalogObjectSubtype;
  using Kd = c::CatalogRecordKind;
  m.object_subtype = kind == Kd::user_account    ? O::user_account
                     : kind == Kd::role_account  ? O::role_account
                     : kind == Kd::group_account ? O::group_account
                                                 : O::grant_record;
  m.creator_local_transaction_id = r.origin_local_transaction_id;
  m.definition_version = 1;
  return m;
}
template <class R>
void Binds(const R& r, const p::Uuid& primary, c::CatalogRecordKind kind,
           bool (*matches)(const c::CatalogMetadataVersionView&)) {
  const auto m = Meta(r, primary, kind);
  auto ok = [&](const c::CatalogMetadataVersion& v) { return matches(c::BorrowCatalogMetadataVersion(v)); };
  Check(ok(m), "exact metadata binding accepted");
  for (unsigned n = 0; n < 11; ++n) {
    auto bad = m;
    switch (n) {
      case 0: bad.record.header.kind = kind == c::CatalogRecordKind::grant_record ? c::CatalogRecordKind::user_account
                                                                                   : c::CatalogRecordKind::grant_record; break;
      case 1: bad.record.header.object_uuid.value = f::Id(999); break;
      case 2: bad.record.header.object_uuid.kind = p::UuidKind::row; break;
      case 3: bad.object_subtype = c::CatalogObjectSubtype::none; break;
      case 4: bad.object_subtype = c::CatalogObjectSubtype::bootstrap_policy_pack; break;
      case 5: bad.authority_scope = c::CatalogAuthorityScope::cluster; break;
      case 6: bad.creator_transaction_uuid = Tx(8); break;
      case 7: bad.creator_local_transaction_id = r.origin_local_transaction_id + 1; break;
      case 8: bad.definition_version = 0; break;
      case 9: bad.creator_transaction_uuid.kind = p::UuidKind::object; break;
      case 10: bad.record.payload.resize(bad.record.payload.size() - 1); break;
    }
    Check(!ok(bad), "metadata binding refuses a mismatched header/subtype/scope/creator/payload");
  }
  auto later = m;
  later.definition_version = 2;
  later.creator_transaction_uuid = Tx(8);
  later.creator_local_transaction_id = 9;
  Check(ok(later), "a later version may be created by another transaction");
}
void Metadata() {
  const auto pr = Principal();
  Binds(pr, pr.principal_uuid, c::CatalogRecordKind::user_account, c::NativeSecurityPrincipalMatchesMetadata);
  const auto ro = Role();
  Binds(ro, ro.role_uuid, c::CatalogRecordKind::role_account, c::NativeSecurityRoleMatchesMetadata);
  const auto gr = Group();
  Binds(gr, gr.group_uuid, c::CatalogRecordKind::group_account, c::NativeSecurityGroupMatchesMetadata);
  const auto gt = Privilege();
  Binds(gt, gt.grant_uuid, c::CatalogRecordKind::grant_record, c::NativeSecurityGrantMatchesMetadata);
  // A record of one kind is not accepted by another kind's binding.
  auto cross = Meta(ro, ro.role_uuid, c::CatalogRecordKind::role_account);
  Check(!c::NativeSecurityPrincipalMatchesMetadata(c::BorrowCatalogMetadataVersion(cross)),
        "role payload refused by the principal binding");
}
}  // namespace
int main() {
  try {
    Principals();
    Roles();
    Groups();
    Grants();
    Metadata();
    std::cout << "PASS catalog_native_security_record: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& e) {
    allocation_budget = -1;
    std::cerr << "FAIL catalog_native_security_record: " << e.what() << '\n';
    return 1;
  }
}
