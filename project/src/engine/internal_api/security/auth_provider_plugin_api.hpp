// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "security/auth_provider_model.hpp"
#include "security/security_model.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AUTH_PROVIDER_PLUGIN_API
struct EngineRegisterAuthProviderRequest : EngineApiRequest {};
struct EngineRegisterAuthProviderResult : EngineApiResult {
  AuthProviderDescriptor provider;
  bool admitted = false;
};
EngineRegisterAuthProviderResult EngineRegisterAuthProvider(const EngineRegisterAuthProviderRequest& request);

struct EngineInspectAuthProviderRequest : EngineApiRequest {};
struct EngineInspectAuthProviderResult : EngineApiResult {
  AuthProviderDescriptor provider;
  bool visible = false;
};
EngineInspectAuthProviderResult EngineInspectAuthProvider(const EngineInspectAuthProviderRequest& request);

struct EngineDisableAuthProviderRequest : EngineApiRequest {};
struct EngineDisableAuthProviderResult : EngineApiResult {
  bool disabled = false;
};
EngineDisableAuthProviderResult EngineDisableAuthProvider(const EngineDisableAuthProviderRequest& request);

struct EngineAuthenticateProviderRequest : EngineApiRequest {};
struct EngineAuthenticateProviderResult : EngineApiResult {
  ConnectionSecurityContextRecord connection_security_context;
  bool authenticated = false;
};
EngineAuthenticateProviderResult EngineAuthenticateProvider(const EngineAuthenticateProviderRequest& request);

}  // namespace scratchbird::engine::internal_api
