// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto auth=Request<EngineAuthenticateProviderRequest>("workload_identity"); auto auth_r=EngineAuthenticateProvider(auth); auto nogroup=Request<EngineAuthenticateProviderRequest>("workload_identity"); nogroup.option_envelopes={"provider:workload_identity","principal:spiffe://example/service"}; auto no_r=EngineAuthenticateProvider(nogroup); return Finish({{"workload_auth",auth_r.ok},{"service_mapping_required",!no_r.ok}});}
