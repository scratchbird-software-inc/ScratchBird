// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto peer=Request<EngineAuthenticateProviderRequest>("peer"); peer.option_envelopes={"provider:peer","principal:local_user"}; auto peer_r=EngineAuthenticateProvider(peer); auto ident=Request<EngineAuthenticateProviderRequest>("ident"); auto ident_r=EngineAuthenticateProvider(ident); auto spoof=Request<EngineAuthenticateProviderRequest>("ident"); spoof.option_envelopes.push_back("freshness:stale"); auto spoof_r=EngineAuthenticateProvider(spoof); return Finish({{"peer_ok",peer_r.ok},{"ident_ok",ident_r.ok},{"spoof_rejected",!spoof_r.ok}});}
