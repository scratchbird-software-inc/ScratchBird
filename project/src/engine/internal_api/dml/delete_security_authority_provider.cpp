// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_security_authority_provider.hpp"
#include "api_diagnostics.hpp"
#include "security/authorization_api.hpp"
#include "dml/update_resource_authority_provider.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <tuple>

namespace scratchbird::engine::internal_api {
struct EngineDmlDeleteSecurityAuthorityHandleV1::Authority {
  EngineRequestContext owner;
  EngineSecurityPolicySnapshotAuthorityV1 snapshot;
  std::vector<std::string> grants;
};
namespace {
EngineApiDiagnostic Refuse(std::string field, std::string code = "SECURITY.ACCESS_DENIED") {
  return MakeEngineApiDiagnostic(std::move(code),
      "sblr.dml_delete_rows.security_authority_invalid", std::move(field), true);
}
EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}
bool ExactUuid(const std::string& text) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(text);
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
      scratchbird::core::uuid::UuidToString(parsed.value) == text;
}
EngineApiDiagnostic ValidateContext(const EngineRequestContext& c, std::string_view phase) {
  const auto has = [&](std::string_view tag) {
    return std::find(c.trace_tags.begin(), c.trace_tags.end(), tag) != c.trace_tags.end();
  };
  if (!has(phase) || !c.security_context_present || !c.authorization_context.present ||
      !c.statement_metadata_snapshot_engine_owned || c.database_path.empty())
    return Refuse("private_authenticated_DELETE_phase_required");
  for (const auto tag : {"private_dml_delete_rows_binder", "private_dml_delete_rows_consumer",
                         "private_dml_delete_rows_recovery", "private_dml_update_rows_binder",
                         "private_dml_update_rows_consumer", "private_dml_update_rows_recovery"})
    if (tag != phase && has(tag)) return Refuse("mixed_operation_or_phase");
  for (const auto* id : {&c.database_uuid, &c.session_uuid, &c.principal_uuid,
                        &c.transaction_uuid, &c.statement_receipt_uuid,
                        &c.statement_snapshot_uuid, &c.statement_metadata_snapshot_uuid,
                        &c.authorization_context.authority_uuid})
    if (!ExactUuid(id->canonical)) return Refuse("owner_identity");
  if (!c.local_transaction_id || !c.catalog_generation_id || !c.security_epoch ||
      !c.authorization_context.security_context_generation ||
      c.authorization_context.principal_uuid != c.principal_uuid ||
      c.read_only_mode || c.cluster_transaction_active || c.route_fence_present)
    return Refuse("owner_generation_or_write_fence");
  return Ok();
}
auto OwnerKey(const EngineRequestContext& c) {
  return std::tie(c.database_path, c.database_uuid, c.session_uuid,
      c.principal_uuid, c.current_role_uuid, c.transaction_uuid,
      c.local_transaction_id, c.statement_receipt_uuid,
      c.statement_snapshot_uuid, c.statement_snapshot_generation,
      c.snapshot_visible_through_local_transaction_id,
      c.statement_metadata_snapshot_uuid,
      c.statement_metadata_snapshot_visible_through_local_transaction_id,
      c.catalog_generation_id, c.security_epoch, c.authorization_context.authority_uuid,
      c.authorization_context.security_context_generation, c.authorization_context.policy_epoch);
}
EngineApiDiagnostic Resolve(const EngineRequestContext& c, const std::string& target,
                            std::vector<std::string>* grants, std::uint64_t* generation) {
  if (!ExactUuid(target)) return Refuse("target_identity");
  EngineAuthorizeRequest materialized;
  materialized.context = c;
  materialized.target_object.uuid = target;
  materialized.required_right = "DELETE";
  const auto admitted = EngineAuthorize(materialized);
  if (!admitted.ok || !admitted.authorized || admitted.policy_recheck_required)
    return Refuse("materialized_DELETE_privilege_required");

  EngineSecurityEvaluatePrivilegeRequest durable;
  durable.context = c;
  durable.principal_uuid = c.principal_uuid;
  durable.target_object_uuid = target;
  durable.privilege = "DELETE";
  const auto authorized = EngineSecurityEvaluatePrivilege(durable);
  const auto state = LoadSecurityPrincipalLifecycleState(c);
  if (!state.ok) return state.diagnostic;
  std::vector<std::string> privilege_sources;
  if (authorized.ok && authorized.authorized) {
    privilege_sources = authorized.matched_grant_uuids;
  } else {
    // SysArch's rights are derived from its immutable page-backed bootstrap
    // membership, not synthetic textual "engine-owned-sysarch-grant" tokens.
    // Require the actual receipt and both independently read identity sources.
    const auto receipt = c.dml_update_resource_receipt.lock();
    if (!receipt || !receipt->AuthenticatesContext(c) || authorized.diagnostics.size() != 1 ||
        authorized.diagnostics.front().code != kSecurityPrincipalDiagnosticDefaultDeny)
      return Refuse("durable_DELETE_privilege_required");
    const auto bootstrap = ResolveEngineOwnedSysarchRoleIdentity(c);
    if (!bootstrap.ok || !bootstrap.present || bootstrap.principal_uuid != c.principal_uuid)
      return Refuse("durable_DELETE_privilege_required");
    for (const auto& membership : state.state.memberships) {
      if (!membership.revoked && membership.member_principal_uuid == bootstrap.principal_uuid &&
          membership.container_uuid == bootstrap.role_uuid && membership.container_kind == "role")
        privilege_sources.push_back(membership.membership_uuid);
    }
    if (privilege_sources.size() != 1) return Refuse("durable_DELETE_bootstrap_membership_required");
  }
  if (state.state.security_generation != authorized.security_generation ||
      state.state.security_context_generation != c.authorization_context.security_context_generation)
    return Refuse("durable_materialized_generation_mismatch", "MGA.TRANSACTION.STALE");
  for (const auto& policy : state.state.row_policies)
    if (!policy.deleted && (policy.target_object_uuid.empty() || policy.target_object_uuid == target))
      return Refuse("DELETE_USING_provider_required", "SBLR.OPERATION_UNSUPPORTED");
  for (const auto& policy : c.authorization_context.policies)
    if (policy.target_uuid.is_nil() || policy.target_uuid == target)
      return Refuse("DELETE_USING_provider_required", "SBLR.OPERATION_UNSUPPORTED");
  *grants = std::move(privilege_sources);
  for (const auto& grant : *grants)
    if (!ExactUuid(grant)) return Refuse("durable_grant_identity");
  std::sort(grants->begin(), grants->end());
  if (std::adjacent_find(grants->begin(), grants->end()) != grants->end())
    return Refuse("duplicate_durable_grant_identity");
  *generation = authorized.security_generation;
  return Ok();
}
}  // namespace

EngineDmlDeleteSecurityAuthorityResultV1 CaptureDmlDeleteSecurityAuthorityV1(
    const EngineRequestContext& context, const std::string& target) {
  EngineDmlDeleteSecurityAuthorityResultV1 result;
  result.diagnostic = ValidateContext(context, "private_dml_delete_rows_binder");
  if (result.diagnostic.error) return result;
  std::uint64_t generation = 0;
  result.diagnostic = Resolve(context, target, &result.matched_grant_uuids, &generation);
  if (result.diagnostic.error) return result;
  const auto snapshot = IssueEngineSecurityPolicySnapshotAuthorityV1(context, target);
  if (!snapshot.ok) { result.diagnostic = snapshot.diagnostic; return result; }
  if (!snapshot.snapshot.admitted_policy_rows.empty() || snapshot.snapshot.security_generation != generation) {
    result.diagnostic = Refuse("security_source_changed_during_capture", "MGA.TRANSACTION.STALE");
    return result;
  }
  auto authority = std::make_shared<EngineDmlDeleteSecurityAuthorityHandleV1::Authority>();
  authority->owner = context;
  authority->snapshot = snapshot.snapshot;
  authority->grants = result.matched_grant_uuids;
  result.snapshot = snapshot.snapshot;
  result.handle.authority_ = std::move(authority);
  result.ok = true;
  result.diagnostic = Ok();
  return result;
}

EngineApiDiagnostic RevalidateDmlDeleteSecurityAuthorityV1(
    const EngineRequestContext& context, const EngineDmlDeleteSecurityAuthorityResultV1& captured) {
  const auto valid = ValidateContext(context, "private_dml_delete_rows_consumer");
  if (valid.error) return valid;
  if (!captured.ok || !captured.handle.valid()) return Refuse("engine_DELETE_handle_required");
  const auto& authority = *captured.handle.authority_;
  if (OwnerKey(context) != OwnerKey(authority.owner) || captured.snapshot != authority.snapshot ||
      captured.matched_grant_uuids != authority.grants)
    return Refuse("captured_owner_or_projection_changed", "MGA.TRANSACTION.STALE");
  std::vector<std::string> grants;
  std::uint64_t generation = 0;
  const auto resolved = Resolve(context, authority.snapshot.target_relation_uuid, &grants, &generation);
  if (resolved.error) return resolved;
  if (grants != authority.grants || generation != authority.snapshot.security_generation)
    return Refuse("DELETE_privilege_source_changed", "MGA.TRANSACTION.STALE");
  return RevalidateEngineSecurityPolicySnapshotAuthorityV1(context, authority.snapshot);
}
EngineApiDiagnostic RevalidateRecoveredDmlDeleteSecurityProjectionV1(
    const EngineRequestContext& c, const EngineSecurityPolicySnapshotAuthorityV1& s,
    const std::vector<std::string>& expected_grants) {
  const auto valid = ValidateContext(c, "private_dml_delete_rows_recovery");
  if (valid.error) return valid;
  if (!ExactUuid(s.snapshot_uuid) || s.snapshot_generation != 1 ||
      s.authenticated_statement_receipt_uuid != c.statement_receipt_uuid ||
      s.security_context_uuid != c.authorization_context.authority_uuid ||
      s.security_context_generation != c.authorization_context.security_context_generation ||
      !s.admitted_policy_rows.empty() ||
      c.authorization_context.security_epoch != c.security_epoch ||
      c.authorization_context.catalog_generation_id != c.catalog_generation_id ||
      !c.authorization_context.policy_epoch)
    return Refuse("recovered_security_projection_owner", "MGA.TRANSACTION.STALE");
  std::vector<std::string> grants;
  std::uint64_t generation = 0;
  const auto resolved = Resolve(c, s.target_relation_uuid, &grants, &generation);
  if (resolved.error) return resolved;
  const auto state = LoadSecurityPrincipalLifecycleState(c);
  if (!state.ok) return state.diagnostic;
  if (grants != expected_grants || generation != s.security_generation ||
      state.state.security_generation != s.security_generation ||
      state.state.security_context_generation != s.security_context_generation ||
      state.state.policy_generation != s.policy_generation)
    return Refuse("recovered_DELETE_security_source_changed", "MGA.TRANSACTION.STALE");
  return Ok();
}
}  // namespace scratchbird::engine::internal_api
