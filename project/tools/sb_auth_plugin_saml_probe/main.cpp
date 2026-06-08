// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto auth=Request<EngineAuthenticateProviderRequest>("saml"); auto auth_r=EngineAuthenticateProvider(auth); auto bad=Request<EngineAuthenticateProviderRequest>("saml"); bad.option_envelopes.push_back("freshness:stale"); auto bad_r=EngineAuthenticateProvider(bad); return Finish({{"saml_auth",auth_r.ok},{"stale_assertion_rejected",!bad_r.ok}});}
