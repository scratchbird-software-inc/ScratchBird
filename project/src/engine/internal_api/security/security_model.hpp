// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_MODEL
// Shared security model support. This is engine-owned behavior; parser,
// driver, listener, manager, and provider plugins can supply evidence but do
// not grant final rights.

struct SecurityAuthorityDescriptor {
  EngineUuid authority_uuid;
  std::string authority_class;
  EngineUuid database_uuid;
  EngineUuid security_database_uuid;
  EngineUuid cluster_uuid;
  std::uint64_t policy_epoch = 0;
  std::vector<std::string> provider_set;
  std::string cache_validity;
  std::string offline_behavior = "deny_new_connections";
  std::string audit_policy_ref;
  std::string protected_material_policy_ref;
};

struct SecurityProviderCapabilities {
  std::string provider_family;
  bool supports_authn = false;
  bool supports_authz_claims = false;
  bool supports_group_query = false;
  bool supports_transitive_group_expansion = false;
  bool supports_membership_path_explain = false;
  bool supports_mfa = false;
  bool supports_token_introspection = false;
  bool supports_credential_rotation = false;
};

struct ConnectionSecurityContextRecord {
  EngineUuid connection_uuid;
  EngineUuid effective_user_uuid;
  EngineUuid authority_uuid;
  std::uint64_t policy_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::vector<EngineUuid> active_roles;
  std::vector<EngineUuid> effective_groups;
  std::vector<std::string> authorization_trace_tags;
  std::vector<std::string> external_provider_evidence;
  std::string cache_expiry;
  std::string transaction_start_policy;
  std::string disclosure_policy;
  std::string audit_policy_ref;
};

struct DurableAuthorizationPrincipalRecord {
  EngineUuid principal_uuid;
  std::string principal_kind = "principal";
  bool active = true;
  std::uint64_t security_epoch = 0;
};

struct DurableAuthorizationRoleRecord {
  EngineUuid role_uuid;
  bool active = true;
  std::uint64_t security_epoch = 0;
};

struct DurableAuthorizationGroupRecord {
  EngineUuid group_uuid;
  bool active = true;
  std::uint64_t security_epoch = 0;
};

struct DurableAuthorizationMembershipRecord {
  EngineUuid member_uuid;
  std::string member_kind;
  EngineUuid parent_uuid;
  std::string parent_kind;
  bool active = true;
  std::uint64_t security_epoch = 0;
};

struct DurableAuthorizationGrantRecord {
  EngineUuid grant_uuid;
  EngineUuid subject_uuid;
  std::string subject_kind;
  EngineUuid target_uuid;
  std::string right;
  bool deny = false;
  bool active = true;
  std::uint64_t security_epoch = 0;
};

struct DurableAuthorizationPolicyRecord {
  EngineUuid policy_uuid;
  EngineUuid subject_uuid;
  std::string subject_kind;
  EngineUuid target_uuid;
  std::string right;
  std::string policy_kind;
  bool deny = false;
  bool requires_runtime_recheck = false;
  bool active = true;
  std::uint64_t policy_epoch = 0;
  std::string canonical_policy_envelope;
};

struct DurableAuthorizationState {
  EngineUuid authority_uuid;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t catalog_generation_id = 0;
  std::vector<DurableAuthorizationPrincipalRecord> principals;
  std::vector<DurableAuthorizationRoleRecord> roles;
  std::vector<DurableAuthorizationGroupRecord> groups;
  std::vector<DurableAuthorizationMembershipRecord> memberships;
  std::vector<DurableAuthorizationGrantRecord> grants;
  std::vector<DurableAuthorizationPolicyRecord> policies;
};

struct DurableAuthorizationMaterializeRequest {
  EngineUuid principal_uuid;
  std::uint64_t observed_security_epoch = 0;
  std::uint64_t observed_policy_epoch = 0;
  std::uint64_t observed_catalog_generation_id = 0;
};

struct DurableAuthorizationMaterializeResult {
  bool ok = false;
  EngineMaterializedAuthorizationContext context;
  std::vector<EngineApiDiagnostic> diagnostics;
};

struct MaterializedAuthorizationDecision {
  bool authorized = false;
  bool denied = false;
  bool policy_recheck_required = false;
  std::string decision = "deny";
  std::vector<std::string> policy_recheck_reasons;
  std::vector<EngineApiDiagnostic> diagnostics;
};

std::string SecurityOptionValue(const EngineApiRequest& request, const std::string& prefix);
bool SecurityOptionPresent(const EngineApiRequest& request, const std::string& value);
bool SecurityOptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback = false);
std::string SecurityLower(std::string value);
EngineApiDiagnostic MakeSecurityDiagnostic(std::string code, std::string detail = {});
bool SecurityContextHasTag(const EngineRequestContext& context, const std::string& tag);
bool SecurityTraceAuthorizationFallbackAllowed(const EngineRequestContext& context);
bool SecurityContextHasRight(const EngineRequestContext& context,
                             const std::string& right,
                             const std::string& target_uuid = {});
bool SecurityContextHasAnyAdmin(const EngineRequestContext& context,
                                const std::vector<std::string>& rights);
bool IsKnownSecurityRight(const std::string& right);
bool IsSupportedSecurityAuthorityClass(const std::string& authority_class);
bool IsClusterSecurityAuthorityClass(const std::string& authority_class);
SecurityProviderCapabilities SecurityProviderCapabilitiesFor(std::string provider_family);
SecurityAuthorityDescriptor SecurityAuthorityDescriptorFromRequest(const EngineApiRequest& request);
ConnectionSecurityContextRecord ConnectionSecurityContextFromRequest(const EngineApiRequest& request);
DurableAuthorizationMaterializeResult MaterializeDurableAuthorizationContext(
    const DurableAuthorizationState& state,
    const DurableAuthorizationMaterializeRequest& request);
MaterializedAuthorizationDecision EvaluateMaterializedAuthorization(
    const EngineRequestContext& request_context,
    const EngineMaterializedAuthorizationContext& authorization_context,
    const std::string& right,
    const std::string& target_uuid);
void AddSecurityEvidence(EngineApiResult* result, std::string kind, std::string id);
void AddSecurityRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields);
EngineApiDiagnostic AppendSecurityEvidenceEvent(const EngineRequestContext& context,
                                                const std::string& operation_id,
                                                const std::string& evidence_kind,
                                                const std::string& evidence_detail);

template <typename TResult>
TResult SecurityFailure(const EngineRequestContext& context,
                        std::string operation_id,
                        EngineApiDiagnostic diagnostic) {
  TResult result;
  result.ok = false;
  result.operation_id = std::move(operation_id);
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

template <typename TResult>
TResult SecuritySuccess(const EngineRequestContext& context, std::string operation_id) {
  TResult result;
  result.ok = true;
  result.operation_id = std::move(operation_id);
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  return result;
}

}  // namespace scratchbird::engine::internal_api
