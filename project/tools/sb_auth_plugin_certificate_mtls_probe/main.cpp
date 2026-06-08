// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto ok=Request<EngineAuthenticateProviderRequest>("certificate_mtls"); auto ok_r=EngineAuthenticateProvider(ok); auto nogroup=Request<EngineAuthenticateProviderRequest>("certificate_mtls"); nogroup.option_envelopes={"provider:certificate_mtls","credential:valid","fixture:success","principal:cert-subject"}; auto no_r=EngineAuthenticateProvider(nogroup); return Finish({{"mtls_ok",ok_r.ok},{"group_materialization_required",!no_r.ok}});}
