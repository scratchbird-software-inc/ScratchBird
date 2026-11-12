// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SEARCH_KEY: SB_CATALOG_NATIVE_SECURITY_RECORDS
#include "catalog_native_security_record.hpp"
#include "catalog_security_record_codec.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace scratchbird::core::catalog {
namespace {
using E = CatalogValueError;
using T = CatalogValueType;
constexpr UuidKind kObj = UuidKind::object;

// Fields 1..6 of every schema: primary, origin transaction (UUID, local id),
// creator_tx, policy_generation, optional policy pack.
#define SB_NATIVE_SECURITY_COMMON_FIELDS                                                              \
  {1, T::engine_identity, true, 16, kObj}, {2, T::engine_identity, true, 16, UuidKind::transaction}, \
      {3, T::unsigned_integer, true, 8}, {4, T::unsigned_integer, true, 8},                          \
      {5, T::unsigned_integer, true, 8}, {6, T::engine_identity, false, 16, kObj}
constexpr CatalogValueFieldSchema principal_fields[]{
    SB_NATIVE_SECURITY_COMMON_FIELDS,
    {7, T::unsigned_integer, true, 8},   // principal_kind
    {8, T::unsigned_integer, false, 8},  // authority_class
    {9, T::boolean, true, 1},            // active
    {10, T::boolean, true, 1},           // create_time_only
    {11, T::boolean, true, 1},           // bootstrap_principal
    {12, T::unsigned_integer, true, 8},  // security_generation
    {13, T::unsigned_integer, true, 8},  // security_context_authority_version
    {14, T::unsigned_integer, true, 8},  // security_context_generation
    {15, T::unsigned_integer, false, 8}, // credential_scheme
    {16, T::unsigned_integer, false, 8}, // credential_iterations
    {17, T::opaque_bytes, false, 16},    // credential_salt
    {18, T::opaque_bytes, false, 32}};   // credential_verifier
constexpr CatalogValueFieldSchema role_fields[]{
    SB_NATIVE_SECURITY_COMMON_FIELDS,
    {7, T::unsigned_integer, false, 8},  // authority_class
    {8, T::boolean, true, 1},            // engine_owned
    {9, T::boolean, true, 1},            // immutable
    {10, T::boolean, true, 1},           // active
    {11, T::boolean, true, 1},           // loaded_at_database_create
    {12, T::boolean, true, 1},           // create_time_only
    {13, T::unsigned_integer, true, 8},  // security_context_authority_version
    {14, T::unsigned_integer, true, 8}}; // security_context_generation
constexpr CatalogValueFieldSchema group_fields[]{
    SB_NATIVE_SECURITY_COMMON_FIELDS,
    {7, T::boolean, true, 1},   // engine_owned
    {8, T::boolean, true, 1}};  // loaded_at_database_create
constexpr CatalogValueFieldSchema grant_fields[]{
    SB_NATIVE_SECURITY_COMMON_FIELDS,
    {7, T::unsigned_integer, true, 8},       // grant_class
    {8, T::engine_identity, false, 16, kObj},   // subject_uuid
    {9, T::engine_identity, false, 16, kObj},   // member_uuid
    {10, T::engine_identity, false, 16, kObj},  // parent_uuid
    {11, T::engine_identity, false, 16, kObj},  // target_uuid
    {12, T::engine_identity, false, 16, kObj},  // membership_uuid
    {13, T::engine_identity, false, 16, kObj},  // principal_uuid
    {14, T::engine_identity, false, 16, kObj},  // role_uuid
    {15, T::unsigned_integer, false, 8},     // subject_kind
    {16, T::unsigned_integer, false, 8},     // member_kind
    {17, T::unsigned_integer, false, 8},     // parent_kind
    {18, T::unsigned_integer, false, 8},     // effect
    {19, T::unsigned_integer, false, 8},     // right
    {20, T::unsigned_integer, true, 8}};     // security_generation
#undef SB_NATIVE_SECURITY_COMMON_FIELDS
constexpr CatalogValueSchemaView principal_schema{kNativeSecurityPrincipalSchemaId, kNativeSecuritySchemaVersion,
                                                  principal_fields};
constexpr CatalogValueSchemaView role_schema{kNativeSecurityRoleSchemaId, kNativeSecuritySchemaVersion, role_fields};
constexpr CatalogValueSchemaView group_schema{kNativeSecurityGroupSchemaId, kNativeSecuritySchemaVersion, group_fields};
constexpr CatalogValueSchemaView grant_schema{kNativeSecurityGrantSchemaId, kNativeSecuritySchemaVersion, grant_fields};
constexpr std::size_t kMaxFields = 20;

bool Id(const Uuid& id) { return uuid::IsEngineIdentityUuid(id); }
bool Typed(const TypedUuid& id, UuidKind kind) { return id.kind == kind && Id(id.value); }
bool Known(CatalogSecurityPrincipalKind v) {
  return v == CatalogSecurityPrincipalKind::user || v == CatalogSecurityPrincipalKind::service;
}
bool Known(CatalogSecurityAuthorityClass v) {
  switch (v) {
    case CatalogSecurityAuthorityClass::engine_owned_sysarch:
    case CatalogSecurityAuthorityClass::l5_database:
    case CatalogSecurityAuthorityClass::direct_bounded_action:
    case CatalogSecurityAuthorityClass::observe_only:
    case CatalogSecurityAuthorityClass::recommend_only:
    case CatalogSecurityAuthorityClass::request_action: return true;
    default: return false;
  }
}
bool Known(CatalogSecurityGrantClass v) {
  switch (v) {
    case CatalogSecurityGrantClass::privilege:
    case CatalogSecurityGrantClass::role_membership:
    case CatalogSecurityGrantClass::group_membership:
    case CatalogSecurityGrantClass::agent_principal_binding: return true;
    default: return false;
  }
}
bool Known(CatalogSecuritySubjectKind v) {
  return v == CatalogSecuritySubjectKind::principal || v == CatalogSecuritySubjectKind::role ||
         v == CatalogSecuritySubjectKind::group;
}
bool Known(CatalogSecurityEffect v) { return v == CatalogSecurityEffect::allow || v == CatalogSecurityEffect::deny; }
bool Known(CatalogSecurityRight v) { return v != CatalogSecurityRight::none && IsKnownCatalogSecurityRight(v); }
template <class En>
bool KnownIfPresent(const std::optional<En>& v) { return !v || Known(*v); }
template <std::size_t N>
bool AllZero(const std::array<byte, N>& b) {
  return std::all_of(b.begin(), b.end(), [](byte x) { return x == 0; });
}

// The common fields, given the record's primary identity.
template <class R>
bool CommonValid(const R& r, const Uuid& primary) {
  return Id(primary) && Typed(r.origin_transaction_uuid, UuidKind::transaction) &&
         r.origin_transaction_uuid.value != primary && r.origin_local_transaction_id &&
         (!r.policy_pack_uuid || (Id(*r.policy_pack_uuid) && *r.policy_pack_uuid != primary &&
                                  *r.policy_pack_uuid != r.origin_transaction_uuid.value));
}
bool Valid(const NativeSecurityCredential& c) {
  return c.scheme == CatalogSecurityCredentialScheme::pbkdf2_sha256_v1 &&
         c.iterations >= kNativeSecurityCredentialMinIterations &&
         c.iterations <= kNativeSecurityCredentialMaxIterations && !AllZero(c.salt) && !AllZero(c.verifier);
}
bool Valid(const NativeSecurityPrincipal& r) {
  return CommonValid(r, r.principal_uuid) && Known(r.principal_kind) && KnownIfPresent(r.authority_class) &&
         (!r.credential || Valid(*r.credential));
}
bool Valid(const NativeSecurityRole& r) { return CommonValid(r, r.role_uuid) && KnownIfPresent(r.authority_class); }
bool Valid(const NativeSecurityGroup& r) { return CommonValid(r, r.group_uuid); }
bool Valid(const NativeSecurityGrant& r) {
  if (!CommonValid(r, r.grant_uuid) || !Known(r.grant_class) || !KnownIfPresent(r.subject_kind) ||
      !KnownIfPresent(r.member_kind) || !KnownIfPresent(r.parent_kind) || !KnownIfPresent(r.effect) ||
      !KnownIfPresent(r.right))
    return false;
  for (const auto* id : {&r.subject_uuid, &r.member_uuid, &r.parent_uuid, &r.target_uuid, &r.membership_uuid,
                         &r.principal_uuid, &r.role_uuid})
    if (*id && (!Id(**id) || **id == r.grant_uuid || **id == r.origin_transaction_uuid.value)) return false;
  const bool no_member = !r.member_uuid && !r.parent_uuid && !r.member_kind && !r.parent_kind;
  switch (r.grant_class) {
    case CatalogSecurityGrantClass::privilege:
      return r.subject_uuid && r.subject_kind && r.right && r.effect && no_member;
    case CatalogSecurityGrantClass::role_membership:
    case CatalogSecurityGrantClass::group_membership:
      // Coordinator correction 2026-11-12 (3a-2 follow-up): the class name is
      // a legacy label; the member and parent kinds decide the edge. A member
      // may be a principal, role or group; a parent a role or group (a
      // principal is never a parent). This matches what the legacy projection
      // accepted, so memberships into groups (principal->group, group->group)
      // are writable and a group's grants reach its member principals.
      return r.member_uuid && r.parent_uuid && r.member_kind && r.parent_kind && *r.member_uuid != *r.parent_uuid &&
             *r.parent_kind != CatalogSecuritySubjectKind::principal && !r.subject_uuid && !r.subject_kind && !r.right &&
             (!r.effect || *r.effect == CatalogSecurityEffect::allow);
    case CatalogSecurityGrantClass::agent_principal_binding:
      return r.subject_uuid && r.subject_kind && *r.subject_kind == CatalogSecuritySubjectKind::principal &&
             r.target_uuid && !r.right && !r.effect && no_member;
    default: return false;
  }
}

// Borrowed field views over the record (identities, salt, verifier) and over
// this object's own storage (numbers, flags). Built in ascending field order.
// Not copyable: its views point into itself.
struct Fields {
  std::array<CatalogValueFieldView, kMaxFields> views{};
  std::array<std::array<byte, 8>, kMaxFields> numbers{};
  std::array<byte, kMaxFields> flags{};
  std::size_t size = 0, number_count = 0, flag_count = 0;
  Fields() = default;
  Fields(const Fields&) = delete;
  Fields& operator=(const Fields&) = delete;
  void Identity(u16 tag, const Uuid& id, UuidKind kind) { views[size++] = {tag, T::engine_identity, kind, id.bytes}; }
  void OptionalIdentity(u16 tag, const std::optional<Uuid>& id) {
    if (id) Identity(tag, *id, kObj);
  }
  void Number(u16 tag, u64 value) {
    auto& b = numbers[number_count++];
    platform::StoreLittle64(b.data(), value);
    views[size++] = {tag, T::unsigned_integer, UuidKind::unknown, b};
  }
  template <class En>
  void OptionalEnum(u16 tag, const std::optional<En>& v) {
    if (v) Number(tag, static_cast<u64>(*v));
  }
  void Flag(u16 tag, bool v) {
    auto& b = flags[flag_count++];
    b = v ? 1 : 0;
    views[size++] = {tag, T::boolean, UuidKind::unknown, std::span<const byte>(&b, 1)};
  }
  void Opaque(u16 tag, std::span<const byte> bytes) { views[size++] = {tag, T::opaque_bytes, UuidKind::unknown, bytes}; }
  template <class R>
  void Common(const R& r, const Uuid& primary) {
    Identity(1, primary, kObj);
    Identity(2, r.origin_transaction_uuid.value, UuidKind::transaction);
    Number(3, r.origin_local_transaction_id);
    Number(4, r.creator_tx);
    Number(5, r.policy_generation);
    OptionalIdentity(6, r.policy_pack_uuid);
  }
  explicit Fields(const NativeSecurityPrincipal& r) {
    Common(r, r.principal_uuid);
    Number(7, static_cast<u64>(r.principal_kind));
    OptionalEnum(8, r.authority_class);
    Flag(9, r.active);
    Flag(10, r.create_time_only);
    Flag(11, r.bootstrap_principal);
    Number(12, r.security_generation);
    Number(13, r.security_context_authority_version);
    Number(14, r.security_context_generation);
    if (r.credential) {
      Number(15, static_cast<u64>(r.credential->scheme));
      Number(16, r.credential->iterations);
      Opaque(17, r.credential->salt);
      Opaque(18, r.credential->verifier);
    }
  }
  explicit Fields(const NativeSecurityRole& r) {
    Common(r, r.role_uuid);
    OptionalEnum(7, r.authority_class);
    Flag(8, r.engine_owned);
    Flag(9, r.immutable);
    Flag(10, r.active);
    Flag(11, r.loaded_at_database_create);
    Flag(12, r.create_time_only);
    Number(13, r.security_context_authority_version);
    Number(14, r.security_context_generation);
  }
  explicit Fields(const NativeSecurityGroup& r) {
    Common(r, r.group_uuid);
    Flag(7, r.engine_owned);
    Flag(8, r.loaded_at_database_create);
  }
  explicit Fields(const NativeSecurityGrant& r) {
    Common(r, r.grant_uuid);
    Number(7, static_cast<u64>(r.grant_class));
    OptionalIdentity(8, r.subject_uuid);
    OptionalIdentity(9, r.member_uuid);
    OptionalIdentity(10, r.parent_uuid);
    OptionalIdentity(11, r.target_uuid);
    OptionalIdentity(12, r.membership_uuid);
    OptionalIdentity(13, r.principal_uuid);
    OptionalIdentity(14, r.role_uuid);
    OptionalEnum(15, r.subject_kind);
    OptionalEnum(16, r.member_kind);
    OptionalEnum(17, r.parent_kind);
    OptionalEnum(18, r.effect);
    OptionalEnum(19, r.right);
    Number(20, r.security_generation);
  }
  std::span<const CatalogValueFieldView> View() const { return std::span<const CatalogValueFieldView>(views).first(size); }
  std::size_t EncodedSize() const {
    std::size_t total = kCatalogValueBlockHeaderBytes;
    for (const auto& v : View()) total += 8 + v.bytes.size();
    return total;
  }
};

template <class R>
std::optional<std::size_t> EncodedBytes(const R& r, CatalogValueSchemaView schema) {
  if (!Valid(r)) return {};
  const Fields f(r);
  if (ValidateCatalogValueFields(schema, f.View()) != E::none) return {};
  return f.EncodedSize();
}
template <class R>
CatalogValueEncodeIntoResult EncodeInto(const R& r, CatalogValueSchemaView schema, std::span<byte> out) {
  if (!Valid(r)) return {E::invalid_value, {}};
  const auto dest = reinterpret_cast<std::uintptr_t>(out.data()), input = reinterpret_cast<std::uintptr_t>(&r);
  if ((out.size() && !out.data()) || out.size() > std::numeric_limits<std::uintptr_t>::max() - dest ||
      (dest < input + sizeof(r) && input < dest + out.size()))
    return {E::invalid_backing, {}};
  const Fields f(r);
  return EncodeCatalogValueBlockInto(schema, f.View(), out);
}
template <class R>
CatalogValueEncodeResult EncodeOwning(const R& r, CatalogValueSchemaView schema) {
  const auto size = EncodedBytes(r, schema);
  if (!size) return {E::invalid_value, {}};
  std::vector<byte> bytes(*size);
  const auto encoded = EncodeInto(r, schema, bytes);
  if (!encoded.ok() || encoded.bytes.size() != bytes.size()) return {encoded.ok() ? E::invalid_value : encoded.error, {}};
  return {E::none, std::move(bytes)};
}

// Decoding helpers. A u16 enumeration carried in a u64 field is refused when
// wider than u16, never truncated.
template <class En>
bool DecodeEnum(const CatalogValueFieldView& f, En& out) {
  const auto v = *f.unsigned_value();
  if (v > std::numeric_limits<u16>::max()) return false;
  out = static_cast<En>(v);
  return true;
}
template <class En>
bool DecodeEnum(const CatalogValueFieldView& f, std::optional<En>& out) {
  En v{};
  if (!DecodeEnum(f, v)) return false;
  out = v;
  return true;
}
bool Flag(const CatalogValueFieldView& f) { return f.bytes[0] != 0; }
// Fields 1..6. Returns true when the field was one of them.
template <class R>
bool DecodeCommon(const CatalogValueFieldView& f, R& r, Uuid& primary) {
  switch (f.id) {
    case 1: primary = f.identity()->value; return true;
    case 2: r.origin_transaction_uuid = *f.identity(); return true;
    case 3: r.origin_local_transaction_id = *f.unsigned_value(); return true;
    case 4: r.creator_tx = *f.unsigned_value(); return true;
    case 5: r.policy_generation = *f.unsigned_value(); return true;
    case 6: r.policy_pack_uuid = f.identity()->value; return true;
    default: return false;
  }
}
std::span<const byte> Bytes(std::string_view b) { return {reinterpret_cast<const byte*>(b.data()), b.size()}; }

template <class R>
bool Header(const CatalogMetadataVersionView& m, const R& r, CatalogRecordKind kind, const Uuid& primary) {
  return m.record.header.kind == kind && Typed(m.record.header.object_uuid, kObj) &&
         m.record.header.object_uuid.value == primary && m.object_subtype == CatalogSecuritySubtype(kind) &&
         m.authority_scope == CatalogAuthorityScope::local && m.definition_version &&
         Typed(m.creator_transaction_uuid, UuidKind::transaction) &&
         (m.definition_version != 1 || (m.creator_transaction_uuid.value == r.origin_transaction_uuid.value &&
                                         m.creator_local_transaction_id == r.origin_local_transaction_id));
}
}  // namespace

std::optional<std::size_t> NativeSecurityPrincipalEncodedBytes(const NativeSecurityPrincipal& r) {
  return EncodedBytes(r, principal_schema);
}
CatalogValueEncodeIntoResult EncodeNativeSecurityPrincipalInto(const NativeSecurityPrincipal& r, std::span<byte> out) {
  return EncodeInto(r, principal_schema, out);
}
CatalogValueEncodeResult EncodeNativeSecurityPrincipal(const NativeSecurityPrincipal& r) {
  return EncodeOwning(r, principal_schema);
}
NativeSecurityDecodeResult<NativeSecurityPrincipal> DecodeNativeSecurityPrincipal(std::string_view bytes) {
  std::array<CatalogValueFieldView, kMaxFields> backing;
  const auto d = DecodeCatalogValueBlockInto(principal_schema, Bytes(bytes), backing);
  if (!d.ok()) return {d.error, {}};
  NativeSecurityPrincipal r;
  NativeSecurityCredential c;
  unsigned credential_fields = 0;
  for (const auto& f : d.fields) {
    if (DecodeCommon(f, r, r.principal_uuid)) continue;
    bool ok = true;
    switch (f.id) {
      case 7: ok = DecodeEnum(f, r.principal_kind); break;
      case 8: ok = DecodeEnum(f, r.authority_class); break;
      case 9: r.active = Flag(f); break;
      case 10: r.create_time_only = Flag(f); break;
      case 11: r.bootstrap_principal = Flag(f); break;
      case 12: r.security_generation = *f.unsigned_value(); break;
      case 13: r.security_context_authority_version = *f.unsigned_value(); break;
      case 14: r.security_context_generation = *f.unsigned_value(); break;
      case 15: ok = DecodeEnum(f, c.scheme); ++credential_fields; break;
      case 16: c.iterations = *f.unsigned_value(); ++credential_fields; break;
      case 17:
        ok = f.bytes.size() == c.salt.size();
        if (ok) std::copy(f.bytes.begin(), f.bytes.end(), c.salt.begin());
        ++credential_fields;
        break;
      case 18:
        ok = f.bytes.size() == c.verifier.size();
        if (ok) std::copy(f.bytes.begin(), f.bytes.end(), c.verifier.begin());
        ++credential_fields;
        break;
      default: ok = false;
    }
    if (!ok) return {E::invalid_value, {}};
  }
  if (credential_fields != 0 && credential_fields != 4) return {E::invalid_value, {}};
  if (credential_fields) r.credential = c;
  if (!Valid(r)) return {E::invalid_value, {}};
  return {E::none, r};
}

std::optional<std::size_t> NativeSecurityRoleEncodedBytes(const NativeSecurityRole& r) {
  return EncodedBytes(r, role_schema);
}
CatalogValueEncodeIntoResult EncodeNativeSecurityRoleInto(const NativeSecurityRole& r, std::span<byte> out) {
  return EncodeInto(r, role_schema, out);
}
CatalogValueEncodeResult EncodeNativeSecurityRole(const NativeSecurityRole& r) { return EncodeOwning(r, role_schema); }
NativeSecurityDecodeResult<NativeSecurityRole> DecodeNativeSecurityRole(std::string_view bytes) {
  std::array<CatalogValueFieldView, kMaxFields> backing;
  const auto d = DecodeCatalogValueBlockInto(role_schema, Bytes(bytes), backing);
  if (!d.ok()) return {d.error, {}};
  NativeSecurityRole r;
  for (const auto& f : d.fields) {
    if (DecodeCommon(f, r, r.role_uuid)) continue;
    bool ok = true;
    switch (f.id) {
      case 7: ok = DecodeEnum(f, r.authority_class); break;
      case 8: r.engine_owned = Flag(f); break;
      case 9: r.immutable = Flag(f); break;
      case 10: r.active = Flag(f); break;
      case 11: r.loaded_at_database_create = Flag(f); break;
      case 12: r.create_time_only = Flag(f); break;
      case 13: r.security_context_authority_version = *f.unsigned_value(); break;
      case 14: r.security_context_generation = *f.unsigned_value(); break;
      default: ok = false;
    }
    if (!ok) return {E::invalid_value, {}};
  }
  if (!Valid(r)) return {E::invalid_value, {}};
  return {E::none, r};
}

std::optional<std::size_t> NativeSecurityGroupEncodedBytes(const NativeSecurityGroup& r) {
  return EncodedBytes(r, group_schema);
}
CatalogValueEncodeIntoResult EncodeNativeSecurityGroupInto(const NativeSecurityGroup& r, std::span<byte> out) {
  return EncodeInto(r, group_schema, out);
}
CatalogValueEncodeResult EncodeNativeSecurityGroup(const NativeSecurityGroup& r) { return EncodeOwning(r, group_schema); }
NativeSecurityDecodeResult<NativeSecurityGroup> DecodeNativeSecurityGroup(std::string_view bytes) {
  std::array<CatalogValueFieldView, kMaxFields> backing;
  const auto d = DecodeCatalogValueBlockInto(group_schema, Bytes(bytes), backing);
  if (!d.ok()) return {d.error, {}};
  NativeSecurityGroup r;
  for (const auto& f : d.fields) {
    if (DecodeCommon(f, r, r.group_uuid)) continue;
    switch (f.id) {
      case 7: r.engine_owned = Flag(f); break;
      case 8: r.loaded_at_database_create = Flag(f); break;
      default: return {E::invalid_value, {}};
    }
  }
  if (!Valid(r)) return {E::invalid_value, {}};
  return {E::none, r};
}

std::optional<std::size_t> NativeSecurityGrantEncodedBytes(const NativeSecurityGrant& r) {
  return EncodedBytes(r, grant_schema);
}
CatalogValueEncodeIntoResult EncodeNativeSecurityGrantInto(const NativeSecurityGrant& r, std::span<byte> out) {
  return EncodeInto(r, grant_schema, out);
}
CatalogValueEncodeResult EncodeNativeSecurityGrant(const NativeSecurityGrant& r) { return EncodeOwning(r, grant_schema); }
NativeSecurityDecodeResult<NativeSecurityGrant> DecodeNativeSecurityGrant(std::string_view bytes) {
  std::array<CatalogValueFieldView, kMaxFields> backing;
  const auto d = DecodeCatalogValueBlockInto(grant_schema, Bytes(bytes), backing);
  if (!d.ok()) return {d.error, {}};
  NativeSecurityGrant r;
  for (const auto& f : d.fields) {
    if (DecodeCommon(f, r, r.grant_uuid)) continue;
    bool ok = true;
    switch (f.id) {
      case 7: ok = DecodeEnum(f, r.grant_class); break;
      case 8: r.subject_uuid = f.identity()->value; break;
      case 9: r.member_uuid = f.identity()->value; break;
      case 10: r.parent_uuid = f.identity()->value; break;
      case 11: r.target_uuid = f.identity()->value; break;
      case 12: r.membership_uuid = f.identity()->value; break;
      case 13: r.principal_uuid = f.identity()->value; break;
      case 14: r.role_uuid = f.identity()->value; break;
      case 15: ok = DecodeEnum(f, r.subject_kind); break;
      case 16: ok = DecodeEnum(f, r.member_kind); break;
      case 17: ok = DecodeEnum(f, r.parent_kind); break;
      case 18: ok = DecodeEnum(f, r.effect); break;
      case 19: ok = DecodeEnum(f, r.right); break;
      case 20: r.security_generation = *f.unsigned_value(); break;
      default: ok = false;
    }
    if (!ok) return {E::invalid_value, {}};
  }
  if (!Valid(r)) return {E::invalid_value, {}};
  return {E::none, r};
}

bool NativeSecurityPrincipalMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto r = DecodeNativeSecurityPrincipal(m.record.payload);
  return r.ok() && Header(m, *r.record, CatalogRecordKind::user_account, r.record->principal_uuid);
}
bool NativeSecurityRoleMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto r = DecodeNativeSecurityRole(m.record.payload);
  return r.ok() && Header(m, *r.record, CatalogRecordKind::role_account, r.record->role_uuid);
}
bool NativeSecurityGroupMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto r = DecodeNativeSecurityGroup(m.record.payload);
  return r.ok() && Header(m, *r.record, CatalogRecordKind::group_account, r.record->group_uuid);
}
bool NativeSecurityGrantMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto r = DecodeNativeSecurityGrant(m.record.payload);
  return r.ok() && Header(m, *r.record, CatalogRecordKind::grant_record, r.record->grant_uuid);
}

// ---- SB_CATALOG_NATIVE_SECURITY_DISPATCH (commit 3a-2) ----
namespace {
template <class F>
auto Common(const NativeSecurityRecord& r, F f) noexcept {
  return std::visit([&](const auto& v) -> decltype(f(std::declval<const NativeSecurityPrincipal&>())) {
    if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::monostate>) return {};
    else return f(v);
  }, r.value);
}
Uuid PrimaryOf(const NativeSecurityPrincipal& r) { return r.principal_uuid; }
Uuid PrimaryOf(const NativeSecurityRole& r) { return r.role_uuid; }
Uuid PrimaryOf(const NativeSecurityGroup& r) { return r.group_uuid; }
Uuid PrimaryOf(const NativeSecurityGrant& r) { return r.grant_uuid; }
u32 PayloadSchema(std::string_view bytes) noexcept {
  if (bytes.size() < kCatalogValueBlockHeaderBytes || bytes.substr(0, 4) != "SBCV") return 0;
  return platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data()) + 16);
}
// The legacy matcher's UUIDv7 typed-reference test, repeated exactly.
bool V7(const TypedUuid& id, UuidKind kind) {
  return id.kind == kind && !id.value.is_nil() && (id.value.bytes[6] >> 4) == 7 && (id.value.bytes[8] & 0xc0) == 0x80;
}
}  // namespace

Uuid NativeSecurityRecord::primary() const noexcept {
  return Common(*this, [](const auto& v) { return PrimaryOf(v); });
}
Uuid NativeSecurityRecord::origin_transaction_uuid() const noexcept {
  return Common(*this, [](const auto& v) { return v.origin_transaction_uuid.value; });
}
u64 NativeSecurityRecord::origin_local_transaction_id() const noexcept {
  return Common(*this, [](const auto& v) { return v.origin_local_transaction_id; });
}
u64 NativeSecurityRecord::creator_tx() const noexcept {
  return Common(*this, [](const auto& v) { return v.creator_tx; });
}
u64 NativeSecurityRecord::policy_generation() const noexcept {
  return Common(*this, [](const auto& v) { return v.policy_generation; });
}

u32 NativeSecuritySchemaIdFor(CatalogRecordKind kind) noexcept {
  switch (kind) {
    case CatalogRecordKind::user_account: return kNativeSecurityPrincipalSchemaId;
    case CatalogRecordKind::role_account: return kNativeSecurityRoleSchemaId;
    case CatalogRecordKind::group_account: return kNativeSecurityGroupSchemaId;
    case CatalogRecordKind::grant_record: return kNativeSecurityGrantSchemaId;
    default: return 0;
  }
}
bool IsNativeSecurityPayload(std::string_view payload) noexcept {
  const auto id = PayloadSchema(payload);
  return id >= kNativeSecurityPrincipalSchemaId && id <= kNativeSecurityGrantSchemaId;
}
NativeSecurityDecodeResult<NativeSecurityRecord> DecodeNativeSecurityRecord(CatalogRecordKind kind,
                                                                            std::string_view payload) {
  const auto schema = NativeSecuritySchemaIdFor(kind);
  if (!schema) return {E::invalid_schema, {}};
  if (PayloadSchema(payload) != schema) return {E::invalid_schema, {}};
  const auto wrap = [&](auto decoded) -> NativeSecurityDecodeResult<NativeSecurityRecord> {
    if (!decoded.ok()) return {decoded.error, {}};
    NativeSecurityRecord r;
    r.kind = kind;
    r.value = *decoded.record;
    return {E::none, r};
  };
  switch (kind) {
    case CatalogRecordKind::user_account: return wrap(DecodeNativeSecurityPrincipal(payload));
    case CatalogRecordKind::role_account: return wrap(DecodeNativeSecurityRole(payload));
    case CatalogRecordKind::group_account: return wrap(DecodeNativeSecurityGroup(payload));
    default: return wrap(DecodeNativeSecurityGrant(payload));
  }
}
bool NativeSecurityPayloadMatchesHeader(const CatalogTypedRecordView& record) {
  const auto decoded = DecodeNativeSecurityRecord(record.header.kind, record.payload);
  if (!decoded.ok()) return false;
  const auto primary = decoded.record->primary();
  return record.header.object_uuid.kind == UuidKind::object && record.header.object_uuid.value == primary &&
         record.header.row_uuid.value != primary &&
         record.header.row_uuid.value != decoded.record->origin_transaction_uuid();
}
bool NativeSecurityRecordMatchesMetadata(const CatalogMetadataVersionView& m) {
  bool bound = false;
  switch (m.record.header.kind) {
    case CatalogRecordKind::user_account: bound = NativeSecurityPrincipalMatchesMetadata(m); break;
    case CatalogRecordKind::role_account: bound = NativeSecurityRoleMatchesMetadata(m); break;
    case CatalogRecordKind::group_account: bound = NativeSecurityGroupMatchesMetadata(m); break;
    case CatalogRecordKind::grant_record: bound = NativeSecurityGrantMatchesMetadata(m); break;
    default: return false;
  }
  if (!bound || !NativeSecurityPayloadMatchesHeader(m.record) || !V7(m.owner_uuid, UuidKind::principal) ||
      !V7(m.audit_uuid, kObj) || !V7(m.security_policy_uuid, kObj) || !V7(m.default_name_uuid, kObj) ||
      !V7(m.name_vector_uuid, kObj) || !V7(m.owning_schema_uuid, UuidKind::schema) ||
      !V7(m.creator_transaction_uuid, UuidKind::transaction) ||
      m.record.header.parent_uuid.value != m.owning_schema_uuid.value)
    return false;
  const auto r = DecodeNativeSecurityRecord(m.record.header.kind, m.record.payload);
  return r.ok() && r.record->origin_local_transaction_id() <= m.creator_local_transaction_id;
}
bool NativeSecurityPreservesOrigin(const CatalogMetadataVersionView& a, const CatalogMetadataVersionView& b) {
  if (!NativeSecurityRecordMatchesMetadata(a) || !NativeSecurityRecordMatchesMetadata(b)) return false;
  const auto x = DecodeNativeSecurityRecord(a.record.header.kind, a.record.payload);
  const auto y = DecodeNativeSecurityRecord(b.record.header.kind, b.record.payload);
  return x.ok() && y.ok() && a.record.header.kind == b.record.header.kind &&
         a.record.header.object_uuid.value == b.record.header.object_uuid.value &&
         x.record->origin_transaction_uuid() == y.record->origin_transaction_uuid() &&
         x.record->origin_local_transaction_id() == y.record->origin_local_transaction_id() &&
         x.record->creator_tx() == y.record->creator_tx();
}

bool CatalogSecurityDefinitionMatchesHeader(const CatalogTypedRecordView& record) {
  return IsNativeSecurityPayload(record.payload) ? NativeSecurityPayloadMatchesHeader(record)
                                                 : CatalogSecurityPayloadMatchesHeader(record);
}
bool CatalogSecurityDefinitionMatchesMetadata(const CatalogMetadataVersionView& m) {
  return IsNativeSecurityPayload(m.record.payload) ? NativeSecurityRecordMatchesMetadata(m)
                                                   : CatalogSecurityPayloadMatchesMetadata(m);
}
bool CatalogSecurityDefinitionPreservesOrigin(const CatalogMetadataVersionView& a, const CatalogMetadataVersionView& b) {
  if (!IsCatalogSecurityRecordKind(a.record.header.kind) && !IsCatalogSecurityRecordKind(b.record.header.kind))
    return true;
  const bool typed = IsNativeSecurityPayload(a.record.payload);
  // A version may not change between the typed record and the legacy bag.
  if (typed != IsNativeSecurityPayload(b.record.payload)) return false;
  return typed ? NativeSecurityPreservesOrigin(a, b) : CatalogSecurityPreservesOrigin(a, b);
}
}  // namespace scratchbird::core::catalog
