// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto ok=Request<EngineAuthenticateProviderRequest>("proxy_assertion"); auto ok_r=EngineAuthenticateProvider(ok); auto replay=Request<EngineAuthenticateProviderRequest>("proxy_assertion"); replay.option_envelopes.push_back("replayed:true"); auto replay_r=EngineAuthenticateProvider(replay); return Finish({{"proxy_ok",ok_r.ok},{"replay_rejected",!replay_r.ok}});}
