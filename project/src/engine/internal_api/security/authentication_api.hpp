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
#include <string_view>

namespace scratchbird::engine::internal_api {

struct EngineSecurityPrincipalLifecycleState;

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_AUTHENTICATION_API
struct EngineAuthenticateRequest : EngineApiRequest {
  std::string provider_family;
  std::string principal_claim;
  // Candidate identity; authentication must match it to the durable principal record.
  EngineUuid durable_principal_uuid;
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

// SEARCH_KEY: SB_AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED (owner Q274,
// 2026-11-19: agents "can not have a connection outside the database itself
// ie even if you have the password, you can not run it from any location
// except inside the program itself"). The one decision every EXTERNAL
// authentication (server session attach, the SBLR authenticate operation,
// embedded attach - all of which reach EngineAuthenticate) applies to the
// principal it resolved, BEFORE any password is verified: a service principal
// (the L5 principal and every agent principal) is internal and is refused
// SECURITY.AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED. Internal startup
// authentication (VerifyNativeRuntimePrincipalCredential, the L5 binding)
// does not pass through here. `principal_kind` is the durable record's kind
// text ("user" | "service"). The result's `error` is false when admitted.
// SEARCH_KEY: SB_AGENT_PRINCIPAL_EXTERNAL_BINDING_KIND_KEYED (owner Q278 = 1,
// 2026-11-19): the refusal is keyed on principal_kind "service" ONLY because the
// legacy principal record carries no authority_class; it therefore also refuses
// an operator-created service-kind principal. When the native security loader
// supplies authority_class (S4), re-key it on authority_class (l5_database and
// the agent classes) and decide the application-service-account policy.
EngineApiDiagnostic CheckExternalAuthenticationBinding(std::string_view principal_kind);

struct EngineRefreshSecurityContextRequest : EngineApiRequest {};
struct EngineRefreshSecurityContextResult : EngineApiResult {
  bool refreshed = false;
};
EngineRefreshSecurityContextResult EngineRefreshSecurityContext(
    const EngineRefreshSecurityContextRequest& request);

}  // namespace scratchbird::engine::internal_api
