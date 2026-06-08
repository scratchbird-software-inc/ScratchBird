// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto auth=Request<EngineAuthenticateProviderRequest>("webauthn"); auth.option_envelopes.push_back("mfa:present"); auto auth_r=EngineAuthenticateProvider(auth); auto missing=Request<EngineAuthenticateProviderRequest>("webauthn"); auto miss_r=EngineAuthenticateProvider(missing); auto factor=Request<EngineAuthenticateProviderRequest>("factor_chain"); auto factor_r=EngineAuthenticateProvider(factor); return Finish({{"webauthn_auth",auth_r.ok},{"missing_mfa_rejected",!miss_r.ok},{"factor_not_login",!factor_r.ok}});}
