// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/security_model.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

bool UuidPresent(const EngineUuid& uuid) {
  return !uuid.canonical.empty();
}

bool CanonicalNonzeroUuid(const EngineUuid& uuid) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(uuid.canonical);
  return parsed.ok() && !parsed.value.is_nil() &&
         scratchbird::core::uuid::UuidToString(parsed.value) ==
             uuid.canonical;
}

bool NonzeroSha256(const std::array<std::uint8_t, 32>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool UuidEquals(const EngineUuid& lhs, const EngineUuid& rhs) {
  return lhs.canonical == rhs.canonical;
}

std::string SubjectKey(const EngineUuid& uuid, const std::string& kind) {
  return kind + ":" + uuid.canonical;
}

bool TargetMatches(const EngineUuid& candidate, const std::string& target_uuid) {
  return candidate.canonical.empty() || candidate.canonical == target_uuid;
}

bool SubjectMatches(const EngineAuthorizationSubject& subject,
                    const EngineUuid& subject_uuid,
                    const std::string& subject_kind) {
  return subject.subject_uuid.canonical == subject_uuid.canonical &&
         subject.subject_kind == subject_kind;
}

bool HasEffectiveSubject(const EngineMaterializedAuthorizationContext& context,
                         const EngineUuid& subject_uuid,
                         const std::string& subject_kind) {
  for (const auto& subject : context.effective_subjects) {
    if (SubjectMatches(subject, subject_uuid, subject_kind)) { return true; }
  }
  return false;
}

bool HasPrincipal(const DurableAuthorizationState& state,
                  const EngineUuid& principal_uuid,
                  DurableAuthorizationPrincipalRecord* principal) {
  for (const auto& candidate : state.principals) {
    if (UuidEquals(candidate.principal_uuid, principal_uuid)) {
      if (principal != nullptr) { *principal = candidate; }
      return true;
    }
  }
  return false;
}

bool HasRole(const DurableAuthorizationState& state,
             const EngineUuid& role_uuid,
             DurableAuthorizationRoleRecord* role_record) {
  for (const auto& role : state.roles) {
    if (UuidEquals(role.role_uuid, role_uuid)) {
      if (role_record != nullptr) { *role_record = role; }
      return true;
    }
  }
  return false;
}

bool HasActiveRole(const DurableAuthorizationState& state, const EngineUuid& role_uuid) {
  DurableAuthorizationRoleRecord role;
  return HasRole(state, role_uuid, &role) && role.active;
}

bool HasGroup(const DurableAuthorizationState& state,
              const EngineUuid& group_uuid,
              DurableAuthorizationGroupRecord* group_record) {
  for (const auto& group : state.groups) {
    if (UuidEquals(group.group_uuid, group_uuid)) {
      if (group_record != nullptr) { *group_record = group; }
      return true;
    }
  }
  return false;
}

bool HasActiveGroup(const DurableAuthorizationState& state, const EngineUuid& group_uuid) {
  DurableAuthorizationGroupRecord group;
  return HasGroup(state, group_uuid, &group) && group.active;
}

bool SubjectRecordActive(const DurableAuthorizationState& state,
                         const EngineUuid& subject_uuid,
                         const std::string& subject_kind) {
  if (subject_kind == "principal") {
    DurableAuthorizationPrincipalRecord principal;
    return HasPrincipal(state, subject_uuid, &principal) && principal.active;
  }
  if (subject_kind == "role") { return HasActiveRole(state, subject_uuid); }
  if (subject_kind == "group") { return HasActiveGroup(state, subject_uuid); }
  return false;
}

std::set<std::string> KnownRights() {
  return {
      "CONNECT", "VISIBLE", "DISCOVER", "LIST_CHILD", "SELECT", "INSERT", "UPDATE", "DELETE", "EXECUTE",
      "CREATE", "ALTER", "DROP", "CATALOG_MUTATE", "USAGE", "TYPE_DDL",
      "DOMAIN_USE", "DOMAIN_CAST", "DOMAIN_METHOD", "DOMAIN_POLICY_ADMIN",
      "DOMAIN_UNMASK", "UNMASK", "POLICY_ADMIN", "OBS_METRICS_READ_SELF", "OBS_METRICS_READ_ALL",
      "OBS_METRICS_READ_FAMILY", "OBS_METRICS_READ_DATABASE", "OBS_METRICS_READ_NODE",
      "OBS_METRICS_READ_CLUSTER", "OBS_METRICS_EXPORT", "OBS_METRICS_EXPORT_READ", "SEC_AUTH_METRICS_READ",
      "OBS_POLICY_READ", "OBS_AGENT_STATE_READ", "OBS_AGENT_EVIDENCE_READ", "OBS_AGENT_CONTROL",
      "OBS_AGENT_POLICY_CONTROL", "OBS_AGENT_OVERRIDE", "OBS_AGENT_RECOMMENDATION_READ",
      "OBS_POLICY_VALIDATE", "OBS_METRICS_POLICY_INSPECT",
      "OBS_METRICS_POLICY_CONTROL", "OBS_METRICS_EXPORT_CONTROL", "OBS_METRICS_RETENTION_CONTROL", "OBS_RUNTIME_SELF",
      "OBS_RUNTIME_ALL", "OBS_INDEX_PROFILE_READ", "OBS_MANAGEMENT_INSPECT", "OBS_MANAGEMENT_CONTROL",
      "OBS_CONFIG_INSPECT", "OBS_CONFIG_CONTROL", "OBS_CLUSTER_HEALTH_INSPECT", "OBS_CLUSTER_CONTROL",
      "OBS_DATA_MOVEMENT_INSPECT", "SEC_IDENTITY_ADMIN", "SEC_MEMBERSHIP_ADMIN", "SEC_GRANT_ADMIN",
      "MGA_TRANSACTION_INSPECT", "MGA_RECOVERY_INSPECT", "MGA_CLEANUP_INSPECT", "MGA_CLEANUP_CONTROL",
      "MGA_HORIZON_INSPECT", "MGA_LINEAGE_INSPECT", "MGA_FORENSIC_INSPECT", "MGA_METRICS_READ",
      "AUTH_PROVIDER_ADMIN", "AUDIT_READ", "AUDIT_ADMIN", "PROTECTED_MATERIAL_RELEASE",
      "KEY_RELEASE_APPROVE", "SUPPORT_EXPORT", "UDR_TRUST_ADMIN", "UDR_MANAGE", "UDR_INSPECT", "UDR_INVOKE",
      "FILESPACE_LIFECYCLE_CONTROL", "MIGRATE_DATABASE",
      "BACKUP_CREATE", "BACKUP_RESTORE", "BACKUP_CONTROL", "BACKUP_INSPECT", "SYS_BACKUP",
      "EVENT_ADMIN", "EVENT_CREATE", "EVENT_ALTER", "EVENT_DROP", "EVENT_SUBSCRIBE", "EVENT_PUBLISH",
      "EVENT_DELIVERY_READ", "EVENT_DELIVERY_ACK",
      "MANAGER_ADMISSION_ADMIN"};
}

}  // namespace

std::string SecurityLower(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

std::string SecurityOptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

bool SecurityOptionPresent(const EngineApiRequest& request, const std::string& value) {
  for (const auto& option : request.option_envelopes) {
    if (option == value) { return true; }
  }
  return false;
}

bool SecurityOptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback) {
  const auto value = SecurityLower(SecurityOptionValue(request, prefix));
  if (value.empty()) { return fallback; }
  if (value == "1" || value == "true" || value == "yes" || value == "on") { return true; }
  if (value == "0" || value == "false" || value == "no" || value == "off") { return false; }
  return fallback;
}

EngineApiDiagnostic MakeSecurityDiagnostic(std::string code, std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), "security.diagnostic", std::move(detail), true);
}

bool SecurityContextHasTag(const EngineRequestContext& context, const std::string& tag) {
  for (const auto& candidate : context.trace_tags) {
    if (candidate == tag) { return true; }
  }
  return false;
}

// SEARCH_KEY: SB_ENGINE_SECURITY_MATERIALIZED_AUTHORIZATION_BOUNDARY
bool SecurityTraceAuthorizationFallbackAllowed(const EngineRequestContext& context) {
  return context.trust_mode == EngineTrustMode::embedded_in_process &&
         SecurityContextHasTag(context, "security.fixture_trace_authority");
}

bool IsKnownSecurityRight(const std::string& right) {
  return KnownRights().count(right) != 0;
}

DurableAuthorizationMaterializeResult MaterializeDurableAuthorizationContext(
    const DurableAuthorizationState& state,
    const DurableAuthorizationMaterializeRequest& request) {
  DurableAuthorizationMaterializeResult result;
  if (!UuidPresent(state.authority_uuid)) {
    result.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.AUTHORIZATION.AUTHORITY_REQUIRED",
        "authority_uuid_required"));
    return result;
  }
  if (!UuidPresent(request.principal_uuid)) {
    result.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.AUTHENTICATION.REQUEST_INVALID",
        "principal_uuid_required"));
    return result;
  }
  if (state.security_context_generation == 0 || state.security_epoch == 0 ||
      state.policy_epoch == 0 ||
      state.catalog_generation_id == 0) {
    result.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.AUTHORIZATION.STATE_EPOCH_REQUIRED",
        "security_context_policy_catalog_generations_required"));
    return result;
  }
  if ((request.observed_security_epoch != 0 &&
       request.observed_security_epoch != state.security_epoch) ||
      (request.observed_policy_epoch != 0 &&
       request.observed_policy_epoch != state.policy_epoch) ||
      (request.observed_catalog_generation_id != 0 &&
       request.observed_catalog_generation_id != state.catalog_generation_id)) {
    result.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.CONTEXT.EXPIRED",
        "observed_epoch_mismatch"));
    return result;
  }

  DurableAuthorizationPrincipalRecord principal;
  if (!HasPrincipal(state, request.principal_uuid, &principal) ||
      !principal.active) {
    result.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.AUTHENTICATION.REQUEST_INVALID",
        "durable_principal_missing_or_inactive"));
    return result;
  }
  if (principal.security_epoch != 0 &&
      principal.security_epoch != state.security_epoch) {
    result.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.CONTEXT.EXPIRED",
        "principal_epoch_mismatch"));
    return result;
  }

  EngineMaterializedAuthorizationContext context;
  context.present = true;
  context.authority_uuid = state.authority_uuid;
  context.security_context_generation = state.security_context_generation;
  context.principal_uuid = request.principal_uuid;
  context.security_epoch = state.security_epoch;
  context.policy_epoch = state.policy_epoch;
  context.catalog_generation_id = state.catalog_generation_id;

  std::set<std::string> resolved;
  std::set<std::string> visiting;
  std::vector<EngineAuthorizationSubject> subjects;

  auto resolve_subject = [&](auto&& self,
                             const EngineUuid& subject_uuid,
                             const std::string& subject_kind) -> bool {
    if (!UuidPresent(subject_uuid) || subject_kind.empty()) {
      result.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.AUTHORIZATION.SUBJECT_INVALID",
          "subject_uuid_and_kind_required"));
      return false;
    }
    if (!SubjectRecordActive(state, subject_uuid, subject_kind)) {
      result.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.AUTHORIZATION.SUBJECT_MISSING",
          subject_kind + ":" + subject_uuid.canonical));
      return false;
    }
    if (subject_kind == "role") {
      DurableAuthorizationRoleRecord role;
      if (HasRole(state, subject_uuid, &role) && role.security_epoch != 0 &&
          role.security_epoch != state.security_epoch) {
        result.diagnostics.push_back(MakeSecurityDiagnostic(
            "SECURITY.CONTEXT.EXPIRED",
            "role_epoch_mismatch:" + subject_uuid.canonical));
        return false;
      }
    }
    if (subject_kind == "group") {
      DurableAuthorizationGroupRecord group;
      if (HasGroup(state, subject_uuid, &group) && group.security_epoch != 0 &&
          group.security_epoch != state.security_epoch) {
        result.diagnostics.push_back(MakeSecurityDiagnostic(
            "SECURITY.CONTEXT.EXPIRED",
            "group_epoch_mismatch:" + subject_uuid.canonical));
        return false;
      }
    }
    const std::string key = SubjectKey(subject_uuid, subject_kind);
    if (visiting.count(key) != 0) {
      result.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.AUTHORIZATION.MEMBERSHIP_CYCLE",
          key));
      return false;
    }
    if (resolved.count(key) != 0) { return true; }
    visiting.insert(key);
    subjects.push_back({subject_uuid, subject_kind});
    for (const auto& edge : state.memberships) {
      if (!edge.active || edge.member_uuid.canonical != subject_uuid.canonical ||
          edge.member_kind != subject_kind) {
        continue;
      }
      if (edge.security_epoch != 0 && edge.security_epoch != state.security_epoch) {
        result.diagnostics.push_back(MakeSecurityDiagnostic(
            "SECURITY.CONTEXT.EXPIRED",
            "membership_epoch_mismatch:" + edge.parent_uuid.canonical));
        return false;
      }
      if (!self(self, edge.parent_uuid, edge.parent_kind)) { return false; }
    }
    visiting.erase(key);
    resolved.insert(key);
    return true;
  };

  if (!resolve_subject(resolve_subject, request.principal_uuid, "principal")) {
    return result;
  }

  context.effective_subjects = subjects;

  // SysArch authority is keyed only by the immutable engine-owned bootstrap
  // catalog UUID.  Display names such as ROOT or ROOT_ROLE are not authority.
  // Explicit durable denies below still take precedence during evaluation.
  for (const auto& subject : context.effective_subjects) {
    if (subject.subject_kind != "role" ||
        !UuidPresent(state.engine_owned_sysarch_role_uuid) ||
        !UuidEquals(subject.subject_uuid, state.engine_owned_sysarch_role_uuid)) {
      continue;
    }
    for (const auto& right : KnownRights()) {
      EngineMaterializedAuthorizationGrant grant;
      grant.grant_uuid.canonical =
          "engine-owned-sysarch-grant:" + right;
      grant.subject_uuid = subject.subject_uuid;
      grant.subject_kind = "role";
      grant.right = right;
      grant.security_epoch = state.security_epoch;
      context.grants.push_back(std::move(grant));
    }
    context.evidence_tags.push_back("engine_owned_sysarch_bundle");
  }

  for (const auto& grant : state.grants) {
    if (!grant.active) { continue; }
    if (!HasEffectiveSubject(context, grant.subject_uuid, grant.subject_kind)) {
      continue;
    }
    if (!IsKnownSecurityRight(grant.right)) {
      result.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.RIGHT.UNKNOWN",
          grant.right));
      return result;
    }
    if (grant.security_epoch == 0 || grant.security_epoch != state.security_epoch) {
      result.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.CONTEXT.EXPIRED",
          "grant_epoch_mismatch:" + grant.grant_uuid.canonical));
      return result;
    }
    context.grants.push_back({grant.grant_uuid,
                              grant.subject_uuid,
                              grant.subject_kind,
                              grant.target_uuid,
                              grant.right,
                              grant.deny,
                              grant.security_epoch});
  }

  for (const auto& policy : state.policies) {
    if (!policy.active) { continue; }
    if (!HasEffectiveSubject(context, policy.subject_uuid, policy.subject_kind)) {
      continue;
    }
    if (!policy.right.empty() && !IsKnownSecurityRight(policy.right)) {
      result.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.RIGHT.UNKNOWN",
          policy.right));
      return result;
    }
    if (policy.policy_epoch == 0 || policy.policy_epoch != state.policy_epoch) {
      result.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.CONTEXT.EXPIRED",
          "policy_epoch_mismatch:" + policy.policy_uuid.canonical));
      return result;
    }
    if (policy.policy_kind == "row_policy" &&
        (policy.source_policy_generation == 0 ||
         (policy.update_policy_phase != 1 &&
          policy.update_policy_phase != 2) ||
         !CanonicalNonzeroUuid(policy.effective_policy_uuid) ||
         policy.effective_policy_generation == 0 ||
         !CanonicalNonzeroUuid(policy.effective_expression_uuid) ||
         policy.effective_expression_generation == 0 ||
         !NonzeroSha256(policy.effective_expression_evidence_sha256))) {
      result.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.CONTEXT.EXPIRED",
          "row_policy_native_authority_missing:" +
              policy.policy_uuid.canonical));
      return result;
    }
    EngineMaterializedAuthorizationPolicy materialized;
    materialized.policy_uuid = policy.policy_uuid;
    materialized.subject_uuid = policy.subject_uuid;
    materialized.subject_kind = policy.subject_kind;
    materialized.target_uuid = policy.target_uuid;
    materialized.right = policy.right;
    materialized.policy_kind = policy.policy_kind;
    materialized.deny = policy.deny;
    materialized.requires_runtime_recheck =
        policy.requires_runtime_recheck;
    materialized.source_policy_generation =
        policy.source_policy_generation;
    materialized.policy_epoch = policy.policy_epoch;
    materialized.canonical_policy_envelope =
        policy.canonical_policy_envelope;
    materialized.update_policy_phase = policy.update_policy_phase;
    materialized.effective_policy_uuid = policy.effective_policy_uuid;
    materialized.effective_policy_generation =
        policy.effective_policy_generation;
    materialized.effective_expression_uuid =
        policy.effective_expression_uuid;
    materialized.effective_expression_generation =
        policy.effective_expression_generation;
    materialized.effective_expression_evidence_sha256 =
        policy.effective_expression_evidence_sha256;
    context.policies.push_back(std::move(materialized));
  }

  context.evidence_tags.push_back("durable_authorization_context");
  context.evidence_tags.push_back("subjects:" + std::to_string(context.effective_subjects.size()));
  context.evidence_tags.push_back("grants:" + std::to_string(context.grants.size()));
  context.evidence_tags.push_back("policies:" + std::to_string(context.policies.size()));
  result.context = std::move(context);
  result.ok = true;
  return result;
}

MaterializedAuthorizationDecision EvaluateMaterializedAuthorization(
    const EngineRequestContext& request_context,
    const EngineMaterializedAuthorizationContext& authorization_context,
    const std::string& right,
    const std::string& target_uuid) {
  MaterializedAuthorizationDecision decision;
  if (right.empty() || !IsKnownSecurityRight(right)) {
    decision.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.AUTHORIZATION.DENIED",
        "unknown_or_missing_right:" + right));
    return decision;
  }
  if (!authorization_context.present) {
    decision.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.AUTHORIZATION.CONTEXT_REQUIRED",
        right));
    return decision;
  }
  if (!UuidPresent(authorization_context.principal_uuid) ||
      !UuidPresent(request_context.principal_uuid) ||
      !UuidEquals(authorization_context.principal_uuid,
                  request_context.principal_uuid)) {
    decision.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.AUTHORIZATION.PRINCIPAL_MISMATCH",
        right));
    return decision;
  }
  if (authorization_context.security_epoch == 0 ||
      authorization_context.policy_epoch == 0 ||
      authorization_context.catalog_generation_id == 0 ||
      (request_context.security_epoch != 0 &&
       request_context.security_epoch != authorization_context.security_epoch) ||
      (request_context.catalog_generation_id != 0 &&
       request_context.catalog_generation_id != authorization_context.catalog_generation_id)) {
    decision.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.CONTEXT.EXPIRED",
        right));
    return decision;
  }

  for (const auto& grant : authorization_context.grants) {
    if (grant.deny && grant.right == right && TargetMatches(grant.target_uuid, target_uuid) &&
        HasEffectiveSubject(authorization_context, grant.subject_uuid, grant.subject_kind)) {
      decision.denied = true;
      decision.decision = "deny";
      decision.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.AUTHORIZATION.DENIED",
          "explicit_deny:" + right));
      return decision;
    }
  }

  bool allowed = false;
  for (const auto& grant : authorization_context.grants) {
    if (!grant.deny && grant.right == right && TargetMatches(grant.target_uuid, target_uuid) &&
        HasEffectiveSubject(authorization_context, grant.subject_uuid, grant.subject_kind)) {
      allowed = true;
      break;
    }
  }
  if (!allowed) {
    decision.diagnostics.push_back(MakeSecurityDiagnostic(
        "SECURITY.AUTHORIZATION.DENIED",
        right));
    return decision;
  }

  for (const auto& policy : authorization_context.policies) {
    if ((!policy.right.empty() && policy.right != right) ||
        !TargetMatches(policy.target_uuid, target_uuid) ||
        !HasEffectiveSubject(authorization_context,
                             policy.subject_uuid,
                             policy.subject_kind)) {
      continue;
    }
    if (policy.deny) {
      decision.denied = true;
      decision.decision = "deny";
      decision.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.POLICY.DENIED",
          policy.policy_kind.empty() ? right : policy.policy_kind));
      return decision;
    }
    if (policy.requires_runtime_recheck) {
      decision.policy_recheck_required = true;
      decision.policy_recheck_reasons.push_back(
          policy.policy_kind.empty() ? policy.policy_uuid.canonical : policy.policy_kind);
    }
  }

  decision.authorized = true;
  decision.decision = decision.policy_recheck_required ? "allow_recheck_required" : "allow";
  return decision;
}

bool SecurityContextHasRight(const EngineRequestContext& context,
                             const std::string& right,
                             const std::string& target_uuid) {
  if (!context.security_context_present) { return false; }
  if (context.authorization_context.present) {
    return EvaluateMaterializedAuthorization(context,
                                             context.authorization_context,
                                             right,
                                             target_uuid).authorized;
  }
  if (!SecurityTraceAuthorizationFallbackAllowed(context)) { return false; }
  if (SecurityContextHasTag(context, "deny:" + right)) { return false; }
  if (!target_uuid.empty() && SecurityContextHasTag(context, "deny:" + right + ":" + target_uuid)) { return false; }
  if (SecurityContextHasTag(context, "right:" + right)) { return true; }
  if (!target_uuid.empty() && SecurityContextHasTag(context, "right:" + right + ":" + target_uuid)) { return true; }
  return false;
}

bool SecurityContextHasAnyAdmin(const EngineRequestContext& context,
                                const std::vector<std::string>& rights) {
  for (const auto& right : rights) {
    if (SecurityContextHasRight(context, right)) { return true; }
  }
  return false;
}

bool IsSupportedSecurityAuthorityClass(const std::string& authority_class) {
  return authority_class == "database_local" || authority_class == "security_database" ||
         authority_class == "remote_security_database" || authority_class == "cluster_security" ||
         authority_class == "node_only_cluster_cache" || authority_class == "internal_default_bootstrap";
}

bool IsClusterSecurityAuthorityClass(const std::string& authority_class) {
  return authority_class == "cluster_security" || authority_class == "node_only_cluster_cache";
}

SecurityProviderCapabilities SecurityProviderCapabilitiesFor(std::string provider_family) {
  provider_family = SecurityLower(std::move(provider_family));
  SecurityProviderCapabilities caps;
  caps.provider_family = provider_family;
  if (provider_family == "local_password" || provider_family == "password_compat" ||
      provider_family == "scram" || provider_family == "scram_sha256" ||
      provider_family == "scram_sha512") {
    caps.supports_authn = true;
    caps.supports_credential_rotation = true;
  }
  else if (provider_family == "internal_server_authority" ||
           provider_family == "remote_security_database" ||
           provider_family == "cluster_security") {
    caps.supports_authn = true;
    caps.supports_authz_claims = provider_family != "internal_server_authority";
    caps.supports_group_query = provider_family != "internal_server_authority";
  }
  else if (provider_family == "peer" || provider_family == "ident" ||
           provider_family == "webauthn" || provider_family == "factor_chain" ||
           provider_family == "workload_identity" || provider_family == "managed_identity") {
    caps.supports_authn = true;
    caps.supports_authz_claims =
        provider_family == "workload_identity" || provider_family == "managed_identity";
    caps.supports_mfa = provider_family == "webauthn" || provider_family == "factor_chain";
  } else if (provider_family == "certificate_mtls" || provider_family == "radius" ||
             provider_family == "proxy_assertion" || provider_family == "token_api_key" ||
             provider_family == "bearer_token" || provider_family == "token_refresh_reauth") {
    caps.supports_authn = true;
    caps.supports_authz_claims = true;
    caps.supports_credential_rotation = provider_family == "certificate_mtls" ||
                                        provider_family == "token_api_key" ||
                                        provider_family == "token_refresh_reauth";
    caps.supports_token_introspection = provider_family == "token_api_key" ||
                                        provider_family == "bearer_token" ||
                                        provider_family == "token_refresh_reauth";
  } else if (provider_family == "ldap_ad") {
    caps.supports_authn = true;
    caps.supports_authz_claims = true;
    caps.supports_group_query = true;
    caps.supports_transitive_group_expansion = true;
    caps.supports_membership_path_explain = true;
    caps.supports_credential_rotation = true;
  } else if (provider_family == "kerberos_pac") {
    caps.supports_authn = true;
    caps.supports_authz_claims = true;
    caps.supports_transitive_group_expansion = true;
  } else if (provider_family == "pam" || provider_family == "oidc_jwt" ||
             provider_family == "saml" || provider_family == "custom_cpp_plugin") {
    caps.supports_authn = true;
    caps.supports_authz_claims = true;
    caps.supports_token_introspection = provider_family == "oidc_jwt";
  } else if (provider_family == "oauth_validator") {
    caps.supports_authz_claims = true;
    caps.supports_token_introspection = true;
  }
  return caps;
}

SecurityAuthorityDescriptor SecurityAuthorityDescriptorFromRequest(const EngineApiRequest& request) {
  SecurityAuthorityDescriptor descriptor;
  descriptor.authority_uuid = request.target_object.uuid;
  descriptor.authority_class = SecurityOptionValue(request, "authority_class:");
  if (descriptor.authority_class.empty()) { descriptor.authority_class = "database_local"; }
  descriptor.database_uuid = request.context.database_uuid;
  descriptor.security_database_uuid = request.target_database.uuid;
  descriptor.cluster_uuid = request.context.cluster_uuid;
  descriptor.policy_epoch = 1;
  const auto policy_epoch = SecurityOptionValue(request, "policy_epoch:");
  if (!policy_epoch.empty()) {
    try { descriptor.policy_epoch = static_cast<std::uint64_t>(std::stoull(policy_epoch)); } catch (...) {}
  }
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "provider:")) { descriptor.provider_set.push_back(option.substr(9)); }
  }
  descriptor.cache_validity = SecurityOptionValue(request, "cache_validity:");
  descriptor.offline_behavior = SecurityOptionValue(request, "offline_behavior:");
  if (descriptor.offline_behavior.empty()) { descriptor.offline_behavior = "deny_new_connections"; }
  descriptor.audit_policy_ref = SecurityOptionValue(request, "audit_policy_ref:");
  descriptor.protected_material_policy_ref = SecurityOptionValue(request, "protected_material_policy_ref:");
  return descriptor;
}

ConnectionSecurityContextRecord ConnectionSecurityContextFromRequest(const EngineApiRequest& request) {
  ConnectionSecurityContextRecord record;
  record.connection_uuid = request.context.session_uuid;
  record.effective_user_uuid = request.context.principal_uuid;
  record.authority_uuid = request.target_object.uuid;
  record.policy_epoch = request.context.catalog_generation_id == 0
      ? 1
      : request.context.catalog_generation_id;
  record.security_epoch = request.context.security_epoch == 0 ? 1 : request.context.security_epoch;
  const auto policy_generation = SecurityOptionValue(request, "policy_generation_current:");
  const auto security_epoch = SecurityOptionValue(request, "security_epoch_current:");
  if (!policy_generation.empty()) {
    try { record.policy_epoch = static_cast<std::uint64_t>(std::stoull(policy_generation)); } catch (...) {}
  }
  if (!security_epoch.empty()) {
    try { record.security_epoch = static_cast<std::uint64_t>(std::stoull(security_epoch)); } catch (...) {}
  }
  record.cache_expiry = SecurityOptionValue(request, "cache_expiry:");
  record.transaction_start_policy = SecurityOptionValue(request, "transaction_start_policy:");
  if (record.transaction_start_policy.empty()) { record.transaction_start_policy = "deny_when_expired"; }
  record.disclosure_policy = SecurityOptionValue(request, "disclosure_policy:");
  if (record.disclosure_policy.empty()) { record.disclosure_policy = "hidden_as_missing"; }
  record.audit_policy_ref = SecurityOptionValue(request, "audit_policy_ref:");
  for (const auto& tag : request.context.trace_tags) {
    if (StartsWith(tag, "role_uuid:")) { record.active_roles.push_back({tag.substr(10)}); }
    if (StartsWith(tag, "group_uuid:")) { record.effective_groups.push_back({tag.substr(11)}); }
    if (StartsWith(tag, "external_evidence:")) { record.external_provider_evidence.push_back(tag.substr(18)); }
  }
  return record;
}

void AddSecurityEvidence(EngineApiResult* result, std::string kind, std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

void AddSecurityRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields) {
  AddApiBehaviorRow(result, std::move(fields));
}

EngineApiDiagnostic AppendSecurityEvidenceEvent(const EngineRequestContext& context,
                                                const std::string& operation_id,
                                                const std::string& evidence_kind,
                                                const std::string& evidence_detail) {
  if (context.database_path.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUDIT.EVIDENCE_REQUIRED", "database_path_required");
  }
  const std::string event = std::string("SBSEC1\tEVIDENCE\t") + std::to_string(context.local_transaction_id) + "\t" +
                            operation_id + "\t" + evidence_kind + "\t" + EncodeCrudText(evidence_detail);
  return AppendApiBehaviorEvent(context, event);
}

}  // namespace scratchbird::engine::internal_api
