// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto sync=Request<EngineSyncExternalGroupsRequest>("ldap_ad"); auto sync_r=EngineSyncExternalGroups(sync); auto explain=Request<EngineExplainMembershipRequest>("ldap_ad"); auto exp_r=EngineExplainMembership(explain); auto token=Request<EngineExplainMembershipRequest>("oidc_jwt"); auto tok_r=EngineExplainMembership(token); return Finish({{"sync",sync_r.ok},{"explain",exp_r.ok},{"effective_only_rejected",!tok_r.ok}});}
