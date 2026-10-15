// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_named_creation.hpp"

namespace scratchbird::storage::database {
enum class NativeInitialPolicyError {
  none, invalid_request, source_invalid, invalid_definition, invalid_names,
  duplicate_identity, resource_exhausted
};
struct NativeInitialPolicyRequest {
  std::string_view policy_seed_pack_root;
  core::platform::Uuid database_uuid;
  core::platform::TypedUuid construction_transaction;
  // Actual owner-issued identity of the materialized memory configuration.
  core::platform::Uuid memory_configuration_uuid;
  std::string_view database_default_language;
  // Exact complete metadata/names, with empty definition payloads. The source
  // pack supplies identities/typed values, never owner/audit/name authority.
  std::span<const core::catalog::CatalogNamedCreationInput> definitions;
  core::catalog::CatalogNamedCreationLimits limits;
};
struct NativeInitialPolicyResult {
  NativeInitialPolicyError error=NativeInitialPolicyError::invalid_request;
  core::catalog::CatalogNamedCreationError name_error=core::catalog::CatalogNamedCreationError::none;
  std::optional<core::platform::DiagnosticRecord> source_diagnostic;
  std::optional<core::catalog::CatalogNamedCreationSet> records;
  bool ok() const noexcept {return error==NativeInitialPolicyError::none&&records.has_value();}
};
// Owning creation-plan producer for all imported pack/provider/profile/memory
// definitions and their actual supplied aliases. Consumes exact hash-admitted
// source bytes and the same typed translators as production creation, not
// legacy catalog rows or a prototype schema cache. No physical I/O except pack
// intake; no partial package on refusal. Caller retains actual source/input/
// output/scratch memory and admits the referenced namespaces/governance/profiles.
// These limits are not grants. This is not the complete node/security/grant
// cohort, authentication, activation or serving success. Never enter under a
// storage guard: source intake and owning result assembly can allocate.
NativeInitialPolicyResult AssembleNativeInitialPolicyRecords(const NativeInitialPolicyRequest&) noexcept;

// Imported group-to-role membership declarations have no source object UUID.
// The actual construction owner supplies newly issued object identities and
// binds each to an exact binary source pair. No synthesized/hash identity and
// no caller-selected replacement for a source role/group/grant is admitted.
struct NativeInitialMembershipIdentity {
  core::platform::Uuid membership_uuid,member_uuid,parent_uuid;
};
struct NativeInitialBootstrapIdentity {
  core::platform::Uuid principal_uuid,membership_uuid;
  std::string_view principal_name,credential_fingerprint;
};
struct NativeInitialPolicySecurityRequest {
  NativeInitialPolicyRequest policy;
  std::span<const NativeInitialMembershipIdentity> group_memberships;
  // Explicit credentialed bootstrap enrollment, never a service identity or
  // authentication proof. When supplied, complete metadata/names must also
  // cover the canonical SYSARCH role, this principal and its membership.
  std::optional<NativeInitialBootstrapIdentity> bootstrap=std::nullopt;
};
// Complete imported policy AND security declaration cohort from one admitted
// source snapshot, with all complete caller metadata/names. Bootstrap is
// enrolled only from explicit validated inputs; no service principal,
// credential, authentication, effective grant or activation is inferred.
// Physical publication and native reference closure remain owned
// by the enclosing actual node constructor.
NativeInitialPolicyResult AssembleNativeInitialPolicySecurityRecords(
    const NativeInitialPolicySecurityRequest&) noexcept;
} // namespace scratchbird::storage::database
