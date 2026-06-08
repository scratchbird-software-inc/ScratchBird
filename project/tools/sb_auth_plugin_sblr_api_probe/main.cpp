// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"
#include <iostream>
namespace api=scratchbird::engine::internal_api; namespace sblr=scratchbird::engine::sblr;
int main(){api::EngineRequestContext ctx; ctx.database_path="/tmp/sb_auth_provider_sblr_probe.sbdb"; ctx.local_transaction_id=500; ctx.security_context_present=true; ctx.trace_tags={"security.bootstrap","right:AUTH_PROVIDER_ADMIN","right:OBS_METRICS_READ_FAMILY"}; sblr::SblrDispatchRequest req; req.context=ctx; req.envelope=sblr::MakeSblrEnvelope("security.register_auth_provider","security.register_auth_provider","TRACE-AUTHP-REGISTER"); req.api_request.option_envelopes={"provider:ldap_ad","provider_enabled:true","signature_valid:true","provenance_valid:true"}; auto reg=sblr::DispatchSblrOperation(req); req.api_request=api::EngineApiRequest{}; req.envelope=sblr::MakeSblrEnvelope("security.authenticate_provider","security.authenticate_provider","TRACE-AUTHP-AUTH"); req.api_request.option_envelopes={"provider:ldap_ad","provider_enabled:true","credential:valid","fixture:success","principal:alice","groups:materialized"}; auto auth=sblr::DispatchSblrOperation(req); req.api_request=api::EngineApiRequest{}; req.envelope=sblr::MakeSblrEnvelope("security.revoke_token","security.revoke_token","TRACE-AUTHP-REVOKE"); req.api_request.option_envelopes={"provider:token_api_key","token_uuid:018f0000-0000-7000-8000-0000000b0001"}; auto revoke=sblr::DispatchSblrOperation(req); bool ok=reg.accepted&&reg.api_result.ok&&auth.accepted&&auth.api_result.ok&&revoke.accepted&&revoke.api_result.ok; std::cout<<"{\"register\":"<<(reg.api_result.ok?"true":"false")<<",\"authenticate\":"<<(auth.api_result.ok?"true":"false")<<",\"revoke\":"<<(revoke.api_result.ok?"true":"false")<<",\"ok\":"<<(ok?"true":"false")<<"}\n"; return ok?0:1;}
