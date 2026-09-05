// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_provider_trusted_result.hpp"

#if defined(SCRATCHBIRD_AUTH_PROVIDER_TESTING)
#include <atomic>
#endif

namespace scratchbird::engine::internal_api {

#if defined(SCRATCHBIRD_AUTH_PROVIDER_TESTING)
namespace {
std::atomic<AuthProviderTrustedResultTestHook> g_test_hook{nullptr};
}

AuthProviderTrustedResult AuthProviderTrustedResultTestFactory::Authenticated(
    std::string provider_family,
    std::string principal,
    bool groups_materialized,
    bool membership_explainable,
    bool mfa_verified,
    std::string credential_kind) {
  AuthProviderTrustedResult result;
  result.authenticated_ = true;
  result.groups_materialized_ = groups_materialized;
  result.membership_explainable_ = membership_explainable;
  result.mfa_verified_ = mfa_verified;
  result.provider_family_ = std::move(provider_family);
  result.principal_ = std::move(principal);
  result.credential_kind_ = std::move(credential_kind);
  result.evidence_.push_back(
      {"auth_provider_trusted_result", "test_only_engine_boundary"});
  result.rows_.push_back({"trusted_provider_result", "authenticated"});
  return result;
}

void InstallAuthProviderTrustedResultTestHook(
    AuthProviderTrustedResultTestHook hook) {
  g_test_hook.store(hook, std::memory_order_release);
}

void ClearAuthProviderTrustedResultTestHook() {
  g_test_hook.store(nullptr, std::memory_order_release);
}
#endif

std::optional<AuthProviderTrustedResult> InvokeTrustedAuthProvider(
    const EngineApiRequest& request) {
#if defined(SCRATCHBIRD_AUTH_PROVIDER_TESTING)
  if (const auto hook = g_test_hook.load(std::memory_order_acquire)) {
    return hook(request);
  }
#endif
  (void)request;
  // No production external-provider implementation is registered. In
  // particular, option_envelopes and provider_payload are deliberately not
  // interpreted as authentication authority here.
  return std::nullopt;
}

}  // namespace scratchbird::engine::internal_api
