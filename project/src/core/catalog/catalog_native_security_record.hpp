// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_security_constants.hpp"
#include "catalog_value_codec.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace scratchbird::core::catalog {
// SEARCH_KEY: SB_CATALOG_NATIVE_SECURITY_RECORDS (owner Q61 = 1, Q62 = 1,
// Q65 = 1, 2026-11-12). Typed native security catalog records: principal
// (user_account), role (role_account), group (group_account) and grant
// (grant_record), each a fixed-schema SBCV value block. Every identifier is a
// binary(16) engine identity or a coding-time constant (the enums of
// catalog_security_constants.hpp); no field carries text. A principal's, role's
// or group's human names live only in the names table. These replace, for the
// native create path, the legacy text attribute-bag codec
// (catalog_security_record_codec), which is unchanged. Not yet wired: no writer,
// reader or dispatcher consults these codecs.
//
// Common to all four: the primary identity (kind object), the origin
// transaction (UUID, kind transaction, and its nonzero local id), creator_tx
// (the creating local transaction id; any value, transaction 0 included),
// policy_generation (any value) and an optional policy pack (kind object).
// Enumerations must be a listed nonzero value; a wire value wider than the
// enum's u16 is refused, never truncated.
inline constexpr u32 kNativeSecurityPrincipalSchemaId = 65635;
inline constexpr u32 kNativeSecurityRoleSchemaId = 65636;
inline constexpr u32 kNativeSecurityGroupSchemaId = 65637;
inline constexpr u32 kNativeSecurityGrantSchemaId = 65638;
inline constexpr u16 kNativeSecuritySchemaVersion = 1;
// Upper bounds of an encoded block (every optional field present).
inline constexpr std::size_t kNativeSecurityPrincipalMaxBytes = 347;
inline constexpr std::size_t kNativeSecurityRoleMaxBytes = 237;
inline constexpr std::size_t kNativeSecurityGroupMaxBytes = 162;
inline constexpr std::size_t kNativeSecurityGrantMaxBytes = 424;
inline constexpr u64 kNativeSecurityCredentialMinIterations = 600000;
inline constexpr u64 kNativeSecurityCredentialMaxIterations = 10000000;

// A stored password verifier: PBKDF2-SHA256 v1, salt and verifier never all
// zero. Present as a whole or absent as a whole (fields 15..18).
struct NativeSecurityCredential {
  CatalogSecurityCredentialScheme scheme = CatalogSecurityCredentialScheme::none;
  u64 iterations = 0;
  std::array<byte, 16> salt{};
  std::array<byte, 32> verifier{};
};
struct NativeSecurityPrincipal {
  Uuid principal_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0, creator_tx = 0, policy_generation = 0;
  std::optional<Uuid> policy_pack_uuid;
  CatalogSecurityPrincipalKind principal_kind = CatalogSecurityPrincipalKind::none;
  std::optional<CatalogSecurityAuthorityClass> authority_class;
  bool active = false, create_time_only = false, bootstrap_principal = false;
  u64 security_generation = 0, security_context_authority_version = 0, security_context_generation = 0;
  std::optional<NativeSecurityCredential> credential;
};
struct NativeSecurityRole {
  Uuid role_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0, creator_tx = 0, policy_generation = 0;
  std::optional<Uuid> policy_pack_uuid;
  std::optional<CatalogSecurityAuthorityClass> authority_class;
  bool engine_owned = false, immutable = false, active = false, loaded_at_database_create = false,
       create_time_only = false;
  u64 security_context_authority_version = 0, security_context_generation = 0;
};
struct NativeSecurityGroup {
  Uuid group_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0, creator_tx = 0, policy_generation = 0;
  std::optional<Uuid> policy_pack_uuid;
  bool engine_owned = false, loaded_at_database_create = false;
};
// Class rules (enforced on encode and decode):
//  privilege:              subject_uuid, subject_kind, right (listed), effect;
//                          no member/parent identity or kind.
//  role_membership:        member_uuid, parent_uuid, member_kind, parent_kind
//                          = role; effect allow if present; no subject or right.
//  group_membership:       as role_membership with parent_kind = role and
//                          member_kind = group.
//  agent_principal_binding: subject_uuid, subject_kind = principal, target_uuid;
//                          no right, effect, member or parent.
// Every present identity differs from grant_uuid; member differs from parent.
struct NativeSecurityGrant {
  Uuid grant_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0, creator_tx = 0, policy_generation = 0;
  std::optional<Uuid> policy_pack_uuid;
  CatalogSecurityGrantClass grant_class = CatalogSecurityGrantClass::none;
  std::optional<Uuid> subject_uuid, member_uuid, parent_uuid, target_uuid, membership_uuid, principal_uuid,
      role_uuid;
  std::optional<CatalogSecuritySubjectKind> subject_kind, member_kind, parent_kind;
  std::optional<CatalogSecurityEffect> effect;
  std::optional<CatalogSecurityRight> right;
  u64 security_generation = 0;
};
template <class Record>
struct NativeSecurityDecodeResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<Record> record;
  bool ok() const noexcept { return error == CatalogValueError::none && record.has_value(); }
};

// EncodedBytes/EncodeInto/Decode are allocation-free; Encode owns its bytes.
// Encoding alone is not authorization or publication.
std::optional<std::size_t> NativeSecurityPrincipalEncodedBytes(const NativeSecurityPrincipal&);
CatalogValueEncodeIntoResult EncodeNativeSecurityPrincipalInto(const NativeSecurityPrincipal&, std::span<byte>);
CatalogValueEncodeResult EncodeNativeSecurityPrincipal(const NativeSecurityPrincipal&);
NativeSecurityDecodeResult<NativeSecurityPrincipal> DecodeNativeSecurityPrincipal(std::string_view);

std::optional<std::size_t> NativeSecurityRoleEncodedBytes(const NativeSecurityRole&);
CatalogValueEncodeIntoResult EncodeNativeSecurityRoleInto(const NativeSecurityRole&, std::span<byte>);
CatalogValueEncodeResult EncodeNativeSecurityRole(const NativeSecurityRole&);
NativeSecurityDecodeResult<NativeSecurityRole> DecodeNativeSecurityRole(std::string_view);

std::optional<std::size_t> NativeSecurityGroupEncodedBytes(const NativeSecurityGroup&);
CatalogValueEncodeIntoResult EncodeNativeSecurityGroupInto(const NativeSecurityGroup&, std::span<byte>);
CatalogValueEncodeResult EncodeNativeSecurityGroup(const NativeSecurityGroup&);
NativeSecurityDecodeResult<NativeSecurityGroup> DecodeNativeSecurityGroup(std::string_view);

std::optional<std::size_t> NativeSecurityGrantEncodedBytes(const NativeSecurityGrant&);
CatalogValueEncodeIntoResult EncodeNativeSecurityGrantInto(const NativeSecurityGrant&, std::span<byte>);
CatalogValueEncodeResult EncodeNativeSecurityGrant(const NativeSecurityGrant&);
NativeSecurityDecodeResult<NativeSecurityGrant> DecodeNativeSecurityGrant(std::string_view);

// The definition row binds its payload: header kind the record's kind, header
// object (kind object) = the primary identity, object_subtype =
// CatalogSecuritySubtype(kind), local authority scope, a nonzero definition
// version, a creator transaction of kind transaction, and for the first
// version (definition_version 1) creator transaction UUID and local id = the
// payload's origin transaction UUID and local id. Nothing else (owner, names,
// schema, audit) is checked here; those stay with the common row checks. Not
// wired yet.
bool NativeSecurityPrincipalMatchesMetadata(const CatalogMetadataVersionView&);
bool NativeSecurityRoleMatchesMetadata(const CatalogMetadataVersionView&);
bool NativeSecurityGroupMatchesMetadata(const CatalogMetadataVersionView&);
bool NativeSecurityGrantMatchesMetadata(const CatalogMetadataVersionView&);
}  // namespace scratchbird::core::catalog
