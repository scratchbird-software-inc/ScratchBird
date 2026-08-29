// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_PRINCIPAL_LIFECYCLE
// Engine-owned user, role, group, privilege, row-policy, definer-rights cache,
// audit, redaction, cache-invalidation, and MGA-visible security lifecycle.

inline constexpr const char* kSecurityPrincipalLifecycleEventMagic = "SBSECPL1";

inline constexpr const char* kSecurityPrincipalDiagnosticDatabasePathRequired =
    "SECURITY.PRINCIPAL.DATABASE_PATH_REQUIRED";
inline constexpr const char* kSecurityPrincipalDiagnosticDatabaseWriteFailed =
    "SECURITY.PRINCIPAL.DATABASE_WRITE_FAILED";
inline constexpr const char* kSecurityPrincipalDiagnosticMgaTransactionRequired =
    "SECURITY.PRINCIPAL.MGA_TRANSACTION_REQUIRED";
inline constexpr const char* kSecurityPrincipalDiagnosticAuthorityRequired =
    "SECURITY.PRINCIPAL.AUTHORITY_REQUIRED";
inline constexpr const char* kSecurityPrincipalDiagnosticAuthorityBypassRefused =
    "SECURITY.PRINCIPAL.AUTHORITY_BYPASS_REFUSED";
inline constexpr const char* kSecurityPrincipalDiagnosticPrincipalInvalid =
    "SECURITY.PRINCIPAL_INVALID";
inline constexpr const char* kSecurityPrincipalDiagnosticPrincipalDisabled =
    "SECURITY.PRINCIPAL_DISABLED";
inline constexpr const char* kSecurityPrincipalDiagnosticPrincipalDuplicate =
    "SECURITY.PRINCIPAL.DUPLICATE";
inline constexpr const char* kSecurityPrincipalDiagnosticRoleInvalid =
    "SECURITY.ROLE_INVALID";
inline constexpr const char* kSecurityPrincipalDiagnosticGroupInvalid =
    "SECURITY.GROUP_INVALID";
inline constexpr const char* kSecurityPrincipalDiagnosticGrantInvalid =
    "SECURITY.GRANT_INVALID";
inline constexpr const char* kSecurityPrincipalDiagnosticAccessDenied =
    "SECURITY.ACCESS_DENIED";
inline constexpr const char* kSecurityPrincipalDiagnosticDefaultDeny =
    "SECURITY.PRIVILEGE.DEFAULT_DENY";
inline constexpr const char* kSecurityPrincipalDiagnosticGrantNotVisible =
    "SECURITY.PRIVILEGE.GRANT_NOT_VISIBLE";
inline constexpr const char* kSecurityPrincipalDiagnosticPolicyMissing =
    "SECURITY.POLICY_MISSING";
inline constexpr const char* kSecurityPrincipalDiagnosticPolicyDuplicate =
    "SECURITY.POLICY.DUPLICATE";
inline constexpr const char* kSecurityPrincipalDiagnosticPolicyStale =
    "SECURITY.POLICY.STALE";
inline constexpr const char* kSecurityPrincipalDiagnosticCacheStale =
    "SECURITY.POLICY.CACHE_STALE";
inline constexpr const char* kSecurityPrincipalDiagnosticCacheMissing =
    "SECURITY.POLICY.CACHE_MISSING";
inline constexpr const char* kSecurityPrincipalDiagnosticProtectedMaterialPlaintextRefused =
    "SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED";
inline constexpr const char* kSecurityPrincipalDiagnosticAuditEvidenceRequired =
    "SECURITY.AUDIT.EVIDENCE_REQUIRED";
inline constexpr const char* kSecurityPrincipalDiagnosticCatalogAuthorityRequired =
    "SECURITY.CATALOG_AUTHORITY_REQUIRED";

struct EngineSecurityPrincipalRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string principal_uuid;
  std::string principal_name;
  std::string principal_kind = "user";
  std::string lifecycle_state = "active";
  std::string credential_fingerprint;
  std::uint64_t security_generation = 0;
  bool deleted = false;
};

struct EngineSecurityRoleRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string role_uuid;
  std::string role_name;
  std::string owner_principal_uuid;
  std::string lifecycle_state = "active";
  std::uint64_t security_generation = 0;
  bool deleted = false;
};

struct EngineSecurityGroupRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string group_uuid;
  std::string group_name;
  std::string external_authority_ref;
  std::string lifecycle_state = "active";
  std::uint64_t security_generation = 0;
  bool deleted = false;
};

struct EngineSecurityMembershipRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string membership_uuid;
  std::string member_principal_uuid;
  std::string container_uuid;
  std::string container_kind;
  std::string grantor_principal_uuid;
  std::uint64_t security_generation = 0;
  bool revoked = false;
};

struct EngineSecurityPrivilegeGrantRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string grant_uuid;
  std::string grantee_uuid;
  std::string grantee_kind = "principal";
  std::string target_object_uuid;
  std::string target_object_kind;
  std::string privilege;
  std::string grantor_principal_uuid;
  std::string grant_effect = "allow";
  std::uint64_t security_generation = 0;
  bool revoked = false;
};

struct EngineSecurityRowPolicyRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string policy_uuid;
  std::string target_object_uuid;
  std::string target_object_kind;
  std::string policy_effect = "deny_all";
  std::string predicate_envelope;
  std::string definer_principal_uuid;
  std::string lifecycle_state = "active";
  std::uint64_t policy_generation = 0;
  // Durable native catalog authority required by DUSV/DUSR recovery.  Legacy
  // rows with any zero/empty field remain valid for their historical security
  // surface but are not admissible as typed UPDATE policy authority.
  std::string policy_version_uuid;
  std::uint64_t effective_transaction_number = 0;
  std::uint64_t target_object_generation = 0;
  std::uint8_t update_policy_phase = 0;  // 1 USING, 2 WITH CHECK.
  std::string effective_policy_uuid;
  std::uint64_t effective_policy_generation = 0;
  std::string effective_expression_uuid;
  std::uint64_t effective_expression_generation = 0;
  std::array<std::uint8_t, 32> effective_expression_evidence_sha256{};
  std::string source_expression_uuid;
  std::uint64_t source_expression_generation = 0;
  std::array<std::uint8_t, 32> source_expression_evidence_sha256{};
  std::string source_catalog_snapshot_uuid;
  std::uint64_t source_catalog_generation = 0;
  std::uint64_t source_security_generation = 0;
  bool deleted = false;
};

struct EngineSecurityRowPolicyNativeAuthorityV1 {
  bool present = false;
  std::string policy_version_uuid;
  std::uint64_t target_relation_generation = 0;
  std::uint8_t phase = 0;
  std::string effective_policy_uuid;
  std::uint64_t effective_policy_generation = 0;
  std::string effective_expression_uuid;
  std::uint64_t effective_expression_generation = 0;
  std::array<std::uint8_t, 32> effective_expression_evidence_sha256{};
  std::string source_expression_uuid;
  std::uint64_t source_expression_generation = 0;
  std::array<std::uint8_t, 32> source_expression_evidence_sha256{};
  std::string catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_generation = 0;
};

struct EngineSecurityDefinerRightsCacheRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string cache_key;
  std::string definer_principal_uuid;
  std::string target_object_uuid;
  std::string privilege;
  std::string decision = "deny";
  std::uint64_t policy_generation = 0;
};

struct EngineSecurityAuditRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string audit_uuid;
  std::string operation_id;
  std::string actor_principal_uuid;
  std::string target_uuid;
  std::string outcome;
  std::string redacted_detail;
  std::uint64_t security_generation = 0;
};

struct EngineSecurityPrincipalLifecycleState {
  std::vector<EngineSecurityPrincipalRecord> principals;
  std::vector<EngineSecurityRoleRecord> roles;
  std::vector<EngineSecurityGroupRecord> groups;
  std::vector<EngineSecurityMembershipRecord> memberships;
  std::vector<EngineSecurityPrivilegeGrantRecord> grants;
  std::vector<EngineSecurityRowPolicyRecord> row_policies;
  std::vector<EngineSecurityDefinerRightsCacheRecord> definer_rights_cache;
  std::vector<EngineSecurityAuditRecord> audit_records;
  // Independently issued durable generation of the materialized
  // authorization-context authority.  It is never inferred from a policy or
  // security epoch.
  std::uint64_t security_context_generation = 0;
  std::uint64_t security_generation = 0;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};

struct EngineLoadSecurityPrincipalLifecycleStateResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineSecurityPrincipalLifecycleState state;
};

// Receipt-scoped immutable view of the durable row-policy catalog.  The
// snapshot identity is issued by the security lifecycle authority after it
// has loaded MGA-visible durable policy events and cross-checked the admitted
// rows against the authenticated materialized authorization context.  Callers
// may not supply or derive snapshot identity from a security-context UUID.
struct EngineSecurityPolicyCatalogRowIdentityV1 {
  std::string policy_uuid;
  std::uint64_t policy_generation = 0;
  std::string policy_version_uuid;
  std::uint64_t effective_transaction_number = 0;
  std::string target_relation_uuid;
  std::uint64_t target_relation_generation = 0;
  std::uint8_t phase = 0;
  std::string effective_policy_uuid;
  std::uint64_t effective_policy_generation = 0;
  std::string effective_expression_uuid;
  std::uint64_t effective_expression_generation = 0;
  std::array<std::uint8_t, 32> effective_expression_evidence_sha256{};
  std::string source_expression_uuid;
  std::uint64_t source_expression_generation = 0;
  std::array<std::uint8_t, 32> source_expression_evidence_sha256{};
  std::string catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_generation = 0;

  bool operator==(const EngineSecurityPolicyCatalogRowIdentityV1&) const =
      default;
};

struct EngineSecurityPolicySnapshotAuthorityV1 {
  std::string snapshot_uuid;
  std::uint64_t snapshot_generation = 0;
  std::string authenticated_statement_receipt_uuid;
  std::string security_context_uuid;
  std::uint64_t security_context_generation = 0;
  std::uint64_t security_generation = 0;
  std::uint64_t policy_generation = 0;
  std::string target_relation_uuid;
  std::vector<EngineSecurityPolicyCatalogRowIdentityV1> admitted_policy_rows;

  bool operator==(const EngineSecurityPolicySnapshotAuthorityV1&) const =
      default;
};

struct EngineSecurityPolicySnapshotAuthorityResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineSecurityPolicySnapshotAuthorityV1 snapshot;
};

class MgaDmlUpdateValidatedDurableAuthorityHandleV1;

struct EngineSecurityPolicySnapshotRecoveryResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineSecurityPolicySnapshotAuthorityV1 snapshot;
};

EngineSecurityPolicySnapshotAuthorityResultV1
IssueEngineSecurityPolicySnapshotAuthorityV1(
    const EngineRequestContext& context,
    const std::string& target_relation_uuid);
EngineApiDiagnostic RevalidateEngineSecurityPolicySnapshotAuthorityV1(
    const EngineRequestContext& context,
    const EngineSecurityPolicySnapshotAuthorityV1& admitted);
// Engine-private recovery accepts only the move-only MGA-validated handle.
// Raw DUSP/DUSV bytes or parser/public projections cannot register authority.
EngineSecurityPolicySnapshotRecoveryResultV1
RecoverEngineSecurityPolicySnapshotFromValidatedDmlUpdateDurableAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle,
    std::span<const std::uint8_t> exact_dumo_bytes);
void ResetEngineSecurityPolicySnapshotAuthorityForTestV1();

struct EngineOwnedSysarchRoleIdentityResult {
  bool ok = false;
  bool present = false;
  std::string role_uuid;
  std::string principal_uuid;
  std::uint64_t policy_generation = 0;
  EngineApiDiagnostic diagnostic;
};

struct EngineSecurityCreatePrincipalRequest : EngineApiRequest {
  std::string principal_uuid;
  std::string principal_name;
  std::string principal_kind = "user";
  std::string credential_protected_material_ref;
  std::string credential_fingerprint;
};
struct EngineSecurityCreatePrincipalResult : EngineApiResult {
  bool principal_created = false;
  bool plaintext_material_stored = false;
  bool protected_material_redacted = true;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityCreatePrincipalResult EngineSecurityCreatePrincipal(
    const EngineSecurityCreatePrincipalRequest& request);

struct EngineSecurityAlterPrincipalRequest : EngineApiRequest {
  std::string principal_uuid;
  std::string principal_name;
  std::string principal_kind;
  std::string lifecycle_state;
  std::string credential_protected_material_ref;
  std::string credential_fingerprint;
};
struct EngineSecurityAlterPrincipalResult : EngineApiResult {
  bool principal_altered = false;
  bool plaintext_material_stored = false;
  bool protected_material_redacted = true;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityAlterPrincipalResult EngineSecurityAlterPrincipal(
    const EngineSecurityAlterPrincipalRequest& request);

struct EngineSecurityCreateRoleRequest : EngineApiRequest {
  std::string role_uuid;
  std::string role_name;
};
struct EngineSecurityCreateRoleResult : EngineApiResult {
  bool role_created = false;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityCreateRoleResult EngineSecurityCreateRole(
    const EngineSecurityCreateRoleRequest& request);

struct EngineSecurityDropRoleRequest : EngineApiRequest {
  std::string role_uuid;
};
struct EngineSecurityDropRoleResult : EngineApiResult {
  bool role_dropped = false;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityDropRoleResult EngineSecurityDropRole(
    const EngineSecurityDropRoleRequest& request);

struct EngineSecurityCreateGroupRequest : EngineApiRequest {
  std::string group_uuid;
  std::string group_name;
  std::string external_authority_ref;
};
struct EngineSecurityCreateGroupResult : EngineApiResult {
  bool group_created = false;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityCreateGroupResult EngineSecurityCreateGroup(
    const EngineSecurityCreateGroupRequest& request);

struct EngineSecurityDropGroupRequest : EngineApiRequest {
  std::string group_uuid;
};
struct EngineSecurityDropGroupResult : EngineApiResult {
  bool group_dropped = false;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityDropGroupResult EngineSecurityDropGroup(
    const EngineSecurityDropGroupRequest& request);

struct EngineSecurityGrantMembershipRequest : EngineApiRequest {
  std::string membership_uuid;
  std::string member_principal_uuid;
  std::string container_uuid;
  std::string container_kind;
};
struct EngineSecurityGrantMembershipResult : EngineApiResult {
  bool membership_granted = false;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityGrantMembershipResult EngineSecurityGrantMembership(
    const EngineSecurityGrantMembershipRequest& request);

struct EngineSecurityRevokeMembershipRequest : EngineApiRequest {
  std::string member_principal_uuid;
  std::string container_uuid;
  std::string container_kind;
};
struct EngineSecurityRevokeMembershipResult : EngineApiResult {
  bool membership_revoked = false;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityRevokeMembershipResult EngineSecurityRevokeMembership(
    const EngineSecurityRevokeMembershipRequest& request);

struct EngineSecurityGrantPrivilegeRequest : EngineApiRequest {
  std::string grant_uuid;
  std::string grantee_uuid;
  std::string grantee_kind = "principal";
  std::string target_object_uuid;
  std::string target_object_kind;
  std::string privilege;
  std::string grant_effect = "allow";
};
struct EngineSecurityGrantPrivilegeResult : EngineApiResult {
  bool privilege_granted = false;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityGrantPrivilegeResult EngineSecurityGrantPrivilege(
    const EngineSecurityGrantPrivilegeRequest& request);

struct EngineSecurityRevokePrivilegeRequest : EngineApiRequest {
  std::string grantee_uuid;
  std::string target_object_uuid;
  std::string privilege;
};
struct EngineSecurityRevokePrivilegeResult : EngineApiResult {
  bool privilege_revoked = false;
  std::uint64_t security_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityRevokePrivilegeResult EngineSecurityRevokePrivilege(
    const EngineSecurityRevokePrivilegeRequest& request);

struct EngineSecuritySetRoleRequest : EngineApiRequest {
  std::string role_uuid;
  std::string role_mode = "explicit";
};
struct EngineSecuritySetRoleResult : EngineApiResult {
  bool role_set = false;
  std::string active_role_uuid;
  std::uint64_t security_generation = 0;
};
EngineSecuritySetRoleResult EngineSecuritySetRole(
    const EngineSecuritySetRoleRequest& request);

struct EngineSecurityAttachPolicyRequest : EngineApiRequest {
  std::string policy_uuid;
  std::string target_object_uuid;
  std::string target_object_kind = "object";
  std::string policy_scope = "object";
  std::string policy_effect = "attach";
  std::string predicate_envelope;
  std::string definer_principal_uuid;
  EngineSecurityRowPolicyNativeAuthorityV1 native_authority;
};
struct EngineSecurityAttachPolicyResult : EngineApiResult {
  bool policy_attached = false;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityAttachPolicyResult EngineSecurityAttachPolicy(
    const EngineSecurityAttachPolicyRequest& request);

struct EngineSecurityCreatePolicyRequest : EngineApiRequest {
  std::string policy_uuid;
  std::string policy_name;
  std::string target_schema_uuid;
  std::string target_object_uuid;
  std::string target_object_kind = "object";
  std::string policy_effect = "row_filter";
  std::string predicate_envelope;
  std::string definer_principal_uuid;
  EngineSecurityRowPolicyNativeAuthorityV1 native_authority;
};
struct EngineSecurityCreatePolicyResult : EngineApiResult {
  bool policy_created = false;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityCreatePolicyResult EngineSecurityCreatePolicy(
    const EngineSecurityCreatePolicyRequest& request);

struct EngineSecurityAlterPolicyRequest : EngineApiRequest {
  std::string policy_uuid;
  std::string target_object_uuid;
  std::string target_object_kind;
  std::string policy_effect;
  std::string predicate_envelope;
  std::string definer_principal_uuid;
  std::string lifecycle_state;
  EngineSecurityRowPolicyNativeAuthorityV1 native_authority;
};
struct EngineSecurityAlterPolicyResult : EngineApiResult {
  bool policy_altered = false;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityAlterPolicyResult EngineSecurityAlterPolicy(
    const EngineSecurityAlterPolicyRequest& request);

struct EngineSecurityDropPolicyRequest : EngineApiRequest {
  std::string policy_uuid;
};
struct EngineSecurityDropPolicyResult : EngineApiResult {
  bool policy_dropped = false;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityDropPolicyResult EngineSecurityDropPolicy(
    const EngineSecurityDropPolicyRequest& request);

struct EngineSecurityDropMaskRequest : EngineApiRequest {
  std::string mask_uuid;
};
struct EngineSecurityDropMaskResult : EngineApiResult {
  bool mask_dropped = false;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityDropMaskResult EngineSecurityDropMask(
    const EngineSecurityDropMaskRequest& request);

struct EngineSecurityDropRlsRequest : EngineApiRequest {
  std::string rls_uuid;
};
struct EngineSecurityDropRlsResult : EngineApiResult {
  bool rls_dropped = false;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityDropRlsResult EngineSecurityDropRls(
    const EngineSecurityDropRlsRequest& request);

struct EngineSecurityActivatePolicyRequest : EngineApiRequest {
  std::string policy_uuid;
};
struct EngineSecurityActivatePolicyResult : EngineApiResult {
  bool policy_activated = false;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityActivatePolicyResult EngineSecurityActivatePolicy(
    const EngineSecurityActivatePolicyRequest& request);

struct EngineSecurityDeactivatePolicyRequest : EngineApiRequest {
  std::string policy_uuid;
};
struct EngineSecurityDeactivatePolicyResult : EngineApiResult {
  bool policy_deactivated = false;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityDeactivatePolicyResult EngineSecurityDeactivatePolicy(
    const EngineSecurityDeactivatePolicyRequest& request);

struct EngineSecurityValidatePolicyRequest : EngineApiRequest {
  std::string policy_uuid;
  std::uint64_t observed_policy_generation = 0;
  std::uint64_t observed_cache_invalidation_epoch = 0;
};
struct EngineSecurityValidatePolicyResult : EngineApiResult {
  bool policy_valid = false;
  bool stale_policy_refused = false;
  std::uint64_t current_policy_generation = 0;
  std::uint64_t current_cache_invalidation_epoch = 0;
};
EngineSecurityValidatePolicyResult EngineSecurityValidatePolicy(
    const EngineSecurityValidatePolicyRequest& request);

struct EngineSecurityShowPolicyRequest : EngineApiRequest {
  std::string policy_uuid;
  bool include_rows = true;
};
struct EngineSecurityShowPolicyResult : EngineApiResult {
  bool policy_found = false;
  EngineSecurityRowPolicyRecord policy;
  std::uint64_t current_policy_generation = 0;
  std::uint64_t current_cache_invalidation_epoch = 0;
};
EngineSecurityShowPolicyResult EngineSecurityShowPolicy(
    const EngineSecurityShowPolicyRequest& request);

struct EngineSecurityEvaluatePrivilegeRequest : EngineApiRequest {
  std::string principal_uuid;
  std::string target_object_uuid;
  std::string target_object_kind;
  std::string privilege;
};
struct EngineSecurityEvaluatePrivilegeResult : EngineApiResult {
  bool authorized = false;
  std::string decision = "deny";
  std::vector<std::string> matched_grant_uuids;
  std::uint64_t security_generation = 0;
};
EngineSecurityEvaluatePrivilegeResult EngineSecurityEvaluatePrivilege(
    const EngineSecurityEvaluatePrivilegeRequest& request);

struct EngineSecurityPutRowPolicyRequest : EngineApiRequest {
  std::string policy_uuid;
  std::string target_object_uuid;
  std::string target_object_kind;
  std::string policy_effect = "deny_all";
  std::string predicate_envelope;
  std::string definer_principal_uuid;
  EngineSecurityRowPolicyNativeAuthorityV1 native_authority;
};
struct EngineSecurityPutRowPolicyResult : EngineApiResult {
  bool policy_persisted = false;
  std::uint64_t policy_generation = 0;
  std::uint64_t cache_invalidation_epoch = 0;
};
EngineSecurityPutRowPolicyResult EngineSecurityPutRowPolicy(
    const EngineSecurityPutRowPolicyRequest& request);

struct EngineSecurityEvaluateRowPolicyRequest : EngineApiRequest {
  std::string principal_uuid;
  std::string target_object_uuid;
  std::string row_owner_principal_uuid;
  std::uint64_t observed_policy_generation = 0;
};
struct EngineSecurityEvaluateRowPolicyResult : EngineApiResult {
  bool row_visible = false;
  bool stale_policy_refused = false;
  std::string decision = "deny";
  std::uint64_t policy_generation = 0;
};
EngineSecurityEvaluateRowPolicyResult EngineSecurityEvaluateRowPolicy(
    const EngineSecurityEvaluateRowPolicyRequest& request);

struct EngineSecurityPrimeDefinerRightsCacheRequest : EngineApiRequest {
  std::string definer_principal_uuid;
  std::string target_object_uuid;
  std::string privilege;
};
struct EngineSecurityPrimeDefinerRightsCacheResult : EngineApiResult {
  bool cached = false;
  std::string cache_key;
  std::uint64_t policy_generation = 0;
};
EngineSecurityPrimeDefinerRightsCacheResult EngineSecurityPrimeDefinerRightsCache(
    const EngineSecurityPrimeDefinerRightsCacheRequest& request);

struct EngineSecurityValidateDefinerRightsCacheRequest : EngineApiRequest {
  std::string cache_key;
  std::string definer_principal_uuid;
  std::string target_object_uuid;
  std::string privilege;
  std::uint64_t observed_policy_generation = 0;
  std::uint64_t observed_cache_invalidation_epoch = 0;
};
struct EngineSecurityValidateDefinerRightsCacheResult : EngineApiResult {
  bool cache_valid = false;
  bool stale_policy_refused = false;
  std::uint64_t current_policy_generation = 0;
  std::uint64_t current_cache_invalidation_epoch = 0;
};
EngineSecurityValidateDefinerRightsCacheResult EngineSecurityValidateDefinerRightsCache(
    const EngineSecurityValidateDefinerRightsCacheRequest& request);

struct EngineSecurityValidatePolicyCacheRequest : EngineApiRequest {
  std::uint64_t observed_policy_generation = 0;
  std::uint64_t observed_cache_invalidation_epoch = 0;
};
struct EngineSecurityValidatePolicyCacheResult : EngineApiResult {
  bool cache_valid = false;
  bool stale_policy_refused = false;
  std::uint64_t current_policy_generation = 0;
  std::uint64_t current_cache_invalidation_epoch = 0;
};
EngineSecurityValidatePolicyCacheResult EngineSecurityValidatePolicyCache(
    const EngineSecurityValidatePolicyCacheRequest& request);

struct EngineSecurityInspectAuditRequest : EngineApiRequest {
  bool include_rows = true;
};
struct EngineSecurityInspectAuditResult : EngineApiResult {
  std::vector<EngineSecurityAuditRecord> audit_records;
  bool protected_material_redacted = true;
};
EngineSecurityInspectAuditResult EngineSecurityInspectAudit(
    const EngineSecurityInspectAuditRequest& request);

struct EngineSecurityInspectOperationRequest : EngineApiRequest {};
struct EngineSecurityInspectOperationResult : EngineApiResult {};
EngineSecurityInspectOperationResult EngineSecurityInspectOperation(
    const EngineSecurityInspectOperationRequest& request);

EngineLoadSecurityPrincipalLifecycleStateResult LoadSecurityPrincipalLifecycleState(
    const EngineRequestContext& context);

EngineOwnedSysarchRoleIdentityResult ResolveEngineOwnedSysarchRoleIdentity(
    const EngineRequestContext& context);

std::string RedactSecurityPrincipalProtectedMaterialForDiagnostics(std::string text);

}  // namespace scratchbird::engine::internal_api
