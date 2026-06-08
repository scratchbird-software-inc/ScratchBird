// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto policy=Request<EngineReloadAuthProviderPolicyRequest>("ldap_ad"); auto policy_r=EngineReloadAuthProviderPolicy(policy); auto disabled=Request<EngineReloadAuthProviderPolicyRequest>("ldap_ad"); disabled.option_envelopes.push_back("provider_enabled:false"); auto dis_r=EngineReloadAuthProviderPolicy(disabled); auto cluster=Request<EngineReloadAuthProviderPolicyRequest>("ldap_ad"); cluster.option_envelopes.push_back("cluster_policy:true"); auto cl_r=EngineReloadAuthProviderPolicy(cluster); return Finish({{"policy_reload",policy_r.ok&&policy_r.reloaded},{"disabled_denied",!dis_r.ok},{"cluster_fail_closed",!cl_r.ok&&cl_r.cluster_authority_required}});}
