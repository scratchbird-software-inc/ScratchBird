// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

#if defined(SCRATCHBIRD_AUTH_PROVIDER_TESTING)
class AuthProviderTrustedResultTestFactory;
#endif

// Opaque authentication authority emitted only inside the engine-owned
// provider invocation boundary. Request option envelopes and provider_payload
// text cannot construct or populate this result.
class AuthProviderTrustedResult final {
 public:
  bool authenticated() const { return authenticated_; }
  bool groups_materialized() const { return groups_materialized_; }
  bool membership_explainable() const { return membership_explainable_; }
  bool mfa_verified() const { return mfa_verified_; }
  const std::string& provider_family() const { return provider_family_; }
  const std::string& principal() const { return principal_; }
  const std::string& credential_kind() const { return credential_kind_; }
  const std::vector<std::pair<std::string, std::string>>& rows() const {
    return rows_;
  }
  const std::vector<EngineEvidenceReference>& evidence() const {
    return evidence_;
  }

 private:
  AuthProviderTrustedResult() = default;

  bool authenticated_ = false;
  bool groups_materialized_ = false;
  bool membership_explainable_ = false;
  bool mfa_verified_ = false;
  std::string provider_family_;
  std::string principal_;
  std::string credential_kind_;
  std::vector<std::pair<std::string, std::string>> rows_;
  std::vector<EngineEvidenceReference> evidence_;

  friend std::optional<AuthProviderTrustedResult>
  InvokeTrustedAuthProvider(const EngineApiRequest& request);
#if defined(SCRATCHBIRD_AUTH_PROVIDER_TESTING)
  friend class AuthProviderTrustedResultTestFactory;
#endif
};

// Returns no result until a real LDAP/PAM/GSSAPI/OIDC/etc. implementation is
// wired into this engine-owned boundary. Absence must always fail closed.
std::optional<AuthProviderTrustedResult> InvokeTrustedAuthProvider(
    const EngineApiRequest& request);

#if defined(SCRATCHBIRD_AUTH_PROVIDER_TESTING)
// This seam is compiled only when an authentication-provider probe option is
// enabled. It is absent from production builds and cannot be selected through
// request option envelopes.
class AuthProviderTrustedResultTestFactory final {
 public:
  static AuthProviderTrustedResult Authenticated(
      std::string provider_family,
      std::string principal,
      bool groups_materialized = false,
      bool membership_explainable = false,
      bool mfa_verified = false,
      std::string credential_kind = "test_verifier");
};

using AuthProviderTrustedResultTestHook =
    std::optional<AuthProviderTrustedResult> (*)(const EngineApiRequest&);

void InstallAuthProviderTrustedResultTestHook(
    AuthProviderTrustedResultTestHook hook);
void ClearAuthProviderTrustedResultTestHook();
#endif

}  // namespace scratchbird::engine::internal_api
