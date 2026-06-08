// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto auth=Request<EngineAuthenticateProviderRequest>("token_api_key"); auto auth_r=EngineAuthenticateProvider(auth); auto revoke=Request<EngineRevokeTokenRequest>("token_api_key"); auto rev_r=EngineRevokeToken(revoke); auto unsync=Request<EngineAuthenticateProviderRequest>("token_api_key"); unsync.option_envelopes.clear(); unsync.option_envelopes={"provider:token_api_key","credential:valid","fixture:success","principal:alice"}; auto unsync_r=EngineAuthenticateProvider(unsync); return Finish({{"token_auth",auth_r.ok},{"revoked",rev_r.ok&&rev_r.revoked},{"unsynced_rejected",!unsync_r.ok}});}
