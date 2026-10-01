// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "security_model.hpp"
#include "security_principal_lifecycle.hpp"
#include <cctype>

namespace scratchbird::engine::internal_api {

// Projection inputs only, NOT authentication or an admission receipt.
// The owning caller supplies the admitted snapshot's identities and epochs;
// zero remains zero so the existing materializer can refuse missing authority.
struct DurableAuthorizationProjectionBinding {
  EngineUuid authority_uuid;
  EngineUuid principal_uuid;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t catalog_generation_id = 0;
};

namespace projection_detail {
inline std::string LowerAscii(std::string value) {
  for (char& ch : value)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}
inline std::string NormalizeAuthorizationSubjectKind(std::string kind) {
  kind = LowerAscii(std::move(kind));
  if (kind.empty() || kind == "user" || kind == "service" ||
      kind == "system_actor") {
    return "principal";
  }
  return kind;
}
inline std::string InferLifecycleSubjectKind(
    const EngineSecurityPrincipalLifecycleState& state,
    const EngineUuid& uuid) {
  for (const auto& role : state.roles) {
    if (role.role_uuid == uuid) return "role";
  }
  for (const auto& group : state.groups) {
    if (group.group_uuid == uuid) return "group";
  }
  return "principal";
}
} // namespace projection_detail

// One shared conversion for server sessions and engine-owned admission.
// No database read, grant fabrication, SYSARCH substitution or epoch fallback.
// Callers must authenticate and bind/revalidate source state before using it.
inline DurableAuthorizationState ProjectDurableAuthorizationState(
    const EngineSecurityPrincipalLifecycleState& lifecycle,
    const DurableAuthorizationProjectionBinding& binding) {
  DurableAuthorizationState state;
  state.authority_uuid = binding.authority_uuid;
  state.security_context_generation = lifecycle.security_context_generation;
  state.security_epoch = binding.security_epoch;
  state.policy_epoch = binding.policy_epoch;
  state.catalog_generation_id = binding.catalog_generation_id;
  for (const auto& principal : lifecycle.principals) {
    if (principal.deleted || principal.lifecycle_state != "active") continue;
    DurableAuthorizationPrincipalRecord record;
    record.principal_uuid = principal.principal_uuid;
    record.principal_kind = "principal";
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.principals.push_back(std::move(record));
  }
  for (const auto& role : lifecycle.roles) {
    if (role.deleted || role.lifecycle_state != "active") continue;
    DurableAuthorizationRoleRecord record;
    record.role_uuid = role.role_uuid;
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.roles.push_back(std::move(record));
  }
  for (const auto& group : lifecycle.groups) {
    if (group.deleted || group.lifecycle_state != "active") continue;
    DurableAuthorizationGroupRecord record;
    record.group_uuid = group.group_uuid;
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.groups.push_back(std::move(record));
  }
  for (const auto& membership : lifecycle.memberships) {
    if (membership.revoked || membership.member_principal_uuid.is_nil() ||
        membership.container_uuid.is_nil()) {
      continue;
    }
    DurableAuthorizationMembershipRecord record;
    record.member_uuid = membership.member_principal_uuid;
    record.member_kind = projection_detail::InferLifecycleSubjectKind(lifecycle,
                                                   membership.member_principal_uuid);
    record.parent_uuid = membership.container_uuid;
    record.parent_kind = projection_detail::NormalizeAuthorizationSubjectKind(membership.container_kind);
    if (record.parent_kind == "principal") {
      record.parent_kind = projection_detail::InferLifecycleSubjectKind(lifecycle, membership.container_uuid);
    }
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.memberships.push_back(std::move(record));
  }
  for (const auto& grant : lifecycle.grants) {
    if (grant.revoked || grant.privilege.empty()) continue;
    DurableAuthorizationGrantRecord record;
    record.grant_uuid = grant.grant_uuid;
    record.subject_uuid = grant.grantee_uuid;
    record.subject_kind = projection_detail::NormalizeAuthorizationSubjectKind(grant.grantee_kind);
    record.target_uuid = grant.target_object_uuid;
    record.right = grant.privilege;
    record.deny = projection_detail::LowerAscii(grant.grant_effect) == "deny";
    record.active = true;
    record.security_epoch = state.security_epoch;
    state.grants.push_back(std::move(record));
  }
  for (const auto& policy : lifecycle.row_policies) {
    if (policy.deleted || policy.lifecycle_state != "active") continue;
    DurableAuthorizationPolicyRecord record;
    record.policy_uuid = policy.policy_uuid;
    // A durable row policy is materialized for the authenticated principal
    // whose effective policy set is being built.  The source catalog row,
    // target and native expression identities remain byte-for-byte provider
    // authority; no predicate envelope or display text is consulted.
    record.subject_uuid = binding.principal_uuid;
    record.subject_kind = "principal";
    record.target_uuid = policy.target_object_uuid;
    record.right = "UPDATE";
    record.policy_kind = "row_policy";
    record.requires_runtime_recheck = true;
    record.active = true;
    record.source_policy_generation = policy.policy_generation;
    record.policy_epoch = state.policy_epoch;
    record.update_policy_phase = policy.update_policy_phase;
    record.effective_policy_uuid =
        policy.effective_policy_uuid;
    record.effective_policy_generation =
        policy.effective_policy_generation;
    record.effective_expression_uuid =
        policy.effective_expression_uuid;
    record.effective_expression_generation =
        policy.effective_expression_generation;
    record.effective_expression_evidence_sha256 =
        policy.effective_expression_evidence_sha256;
    state.policies.push_back(std::move(record));
  }
  return state;
}

} // namespace scratchbird::engine::internal_api
