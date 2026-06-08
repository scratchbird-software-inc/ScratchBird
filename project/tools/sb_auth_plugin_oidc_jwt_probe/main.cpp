// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto auth=Request<EngineAuthenticateProviderRequest>("oidc_jwt"); auto auth_r=EngineAuthenticateProvider(auth); auto over=Request<EngineAuthenticateProviderRequest>("oidc_jwt"); over.option_envelopes={"provider:oidc_jwt","credential:valid","fixture:success","principal:sub","groups_overage:true"}; auto over_r=EngineAuthenticateProvider(over); auto oauth=Request<EngineAuthenticateProviderRequest>("oauth_validator"); auto oauth_r=EngineAuthenticateProvider(oauth); return Finish({{"oidc_auth",auth_r.ok},{"overage_requires_sync",!over_r.ok},{"oauth_not_login",!oauth_r.ok}});}
