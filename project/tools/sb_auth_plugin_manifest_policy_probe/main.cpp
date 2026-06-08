// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto sig=Request<EngineRegisterAuthProviderRequest>("oidc_jwt"); sig.option_envelopes.push_back("signature_valid:false"); auto sig_r=EngineRegisterAuthProvider(sig); auto dep=Request<EngineRegisterAuthProviderRequest>("ldap_ad"); dep.option_envelopes.push_back("missing_dependency:true"); auto dep_r=EngineRegisterAuthProvider(dep); auto abi=Request<EngineRegisterAuthProviderRequest>("saml"); abi.option_envelopes.push_back("abi_supported:false"); auto abi_r=EngineRegisterAuthProvider(abi); auto stale=Request<EngineRegisterAuthProviderRequest>("radius"); stale.option_envelopes.push_back("stale_implementation:true"); auto stale_r=EngineRegisterAuthProvider(stale); return Finish({{"signature_rejected",!sig_r.ok},{"dependency_rejected",!dep_r.ok},{"abi_rejected",!abi_r.ok},{"stale_rejected",!stale_r.ok}});}
