// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto scram=Request<EngineAuthenticateProviderRequest>("scram_sha256"); auto scram_r=EngineAuthenticateProvider(scram); auto compat=Request<EngineAuthenticateProviderRequest>("password_compat"); auto compat_r=EngineAuthenticateProvider(compat); auto compat_ok=Request<EngineAuthenticateProviderRequest>("password_compat"); compat_ok.option_envelopes.push_back("allow_password_compat:true"); auto compat_ok_r=EngineAuthenticateProvider(compat_ok); auto downgrade=Request<EngineAuthenticateProviderRequest>("scram_sha512"); downgrade.option_envelopes.push_back("downgrade_attempt:true"); auto down_r=EngineAuthenticateProvider(downgrade); return Finish({{"scram_ok",scram_r.ok&&scram_r.authenticated},{"compat_default_rejected",!compat_r.ok},{"compat_allowed",compat_ok_r.ok},{"downgrade_rejected",!down_r.ok}});}
