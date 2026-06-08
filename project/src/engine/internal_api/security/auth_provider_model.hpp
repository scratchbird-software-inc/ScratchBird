// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "security/security_model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AUTH_PROVIDER_MODEL
// Engine-owned auth provider model. Providers supply evidence only; this model
// performs deterministic admission, policy, fixture-backed, and live-evidence
// authentication decisions for the current implementation slice.

struct AuthProviderDescriptor {
  EngineUuid provider_uuid;
  std::string provider_family;
  std::string provider_version;
  std::string implementation_version;
  SecurityProviderCapabilities capabilities;
  std::uint64_t policy_epoch = 1;
  std::string trust_state = "untrusted";
  std::string rollout_state = "disabled";
  std::vector<std::string> required_libraries;
  std::vector<std::string> allowed_policy_scopes;
};

struct AuthProviderPolicy {
  EngineUuid policy_uuid;
  EngineUuid provider_uuid;
  std::string provider_family;
  bool enabled = false;
  bool allow_password_compat = false;
  bool require_mfa = false;
  bool require_group_sync = false;
  bool allow_cache_stale = false;
  bool allow_fixture = true;
  std::string stale_behavior = "deny";
  std::string group_behavior = "none";
  std::string cache_bounds = "deny_when_expired";
  std::string audit_policy_ref;
  std::string redaction_policy_ref;
};

struct AuthProviderDecision {
  bool ok = false;
  bool authenticated = false;
  bool admitted = false;
  bool materialized = false;
  bool explainable = false;
  bool redacted = false;
  bool token_revoked = false;
  bool challenge_accepted = false;
  bool credential_rotated = false;
  bool cluster_authority_required = false;
  std::string provider_family;
  std::string principal;
  std::string decision;
  EngineApiDiagnostic diagnostic;
  std::vector<std::pair<std::string, std::string>> rows;
  std::vector<EngineEvidenceReference> evidence;
};

std::string CanonicalAuthProviderFamily(std::string provider_family);
bool IsKnownAuthProviderFamily(const std::string& provider_family);
bool AuthProviderFamilySupportsAuthn(const std::string& provider_family);
AuthProviderDescriptor AuthProviderDescriptorFromRequest(const EngineApiRequest& request);
AuthProviderPolicy AuthProviderPolicyFromRequest(const EngineApiRequest& request);
std::string AuthProviderOptionValue(const EngineApiRequest& request, const std::string& prefix);
bool AuthProviderOptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback = false);
bool AuthProviderOptionPresent(const EngineApiRequest& request, const std::string& exact_value);
EngineApiDiagnostic AuthProviderNoPlaintextDiagnostic(const EngineApiRequest& request);
AuthProviderDecision AdmitAuthProvider(const EngineApiRequest& request);
AuthProviderDecision EvaluateAuthProviderPolicy(const EngineApiRequest& request);
AuthProviderDecision AuthenticateWithProvider(const EngineApiRequest& request);
AuthProviderDecision ContinueAuthChallenge(const EngineApiRequest& request);
AuthProviderDecision RotateAuthCredential(const EngineApiRequest& request);
AuthProviderDecision RevokeAuthToken(const EngineApiRequest& request);
AuthProviderDecision SyncAuthProviderGroups(const EngineApiRequest& request);
AuthProviderDecision ExplainAuthProviderMembership(const EngineApiRequest& request);
AuthProviderDecision InspectAuthProviderMetrics(const EngineApiRequest& request);
void ApplyAuthProviderDecision(EngineApiResult* result, const AuthProviderDecision& decision);

}  // namespace scratchbird::engine::internal_api
