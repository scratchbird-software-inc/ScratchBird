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

#include <memory>

namespace scratchbird::engine::internal_api {

struct EngineSecurityPrincipalLifecycleState;

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_AUTHENTICATION_API
struct EngineAuthenticateRequest : EngineApiRequest {
  std::string provider_family;
  std::string principal_claim;
  std::string credential_evidence;
  bool credential_evidence_present = false;
  bool credential_invalid_claim = false;
  bool mfa_evidence_present = false;
};
struct EngineAuthenticateResult : EngineApiResult {
  ConnectionSecurityContextRecord connection_security_context;
  // One immutable view loaded by the authentication authority.  Server-side
  // role/group projection for this same handoff may consume it, avoiding a
  // second and third traversal of the identical durable security chain.  It
  // is never retained as statement authority or reused across authentication
  // attempts.
  std::shared_ptr<const EngineSecurityPrincipalLifecycleState>
      durable_security_state;
  bool authenticated = false;
};
EngineAuthenticateResult EngineAuthenticate(const EngineAuthenticateRequest& request);

struct EngineRefreshSecurityContextRequest : EngineApiRequest {};
struct EngineRefreshSecurityContextResult : EngineApiResult {
  bool refreshed = false;
};
EngineRefreshSecurityContextResult EngineRefreshSecurityContext(
    const EngineRefreshSecurityContextRequest& request);

}  // namespace scratchbird::engine::internal_api
