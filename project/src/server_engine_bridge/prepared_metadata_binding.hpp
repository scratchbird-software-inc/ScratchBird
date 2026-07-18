// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/engine.h"

#include <string_view>

namespace scratchbird::server_engine_bridge {

// Private server-to-engine handle.  It is deliberately absent from the
// frozen public C ABI and cannot be encoded into an SBLR envelope.
struct PreparedMetadataBindingOpaque;
using PreparedMetadataBindingHandle = PreparedMetadataBindingOpaque*;

// Private deterministic hook for the concurrency proof. Production routes do
// not install a hook; it is deliberately outside the frozen public C ABI.
using PreparedMetadataBindingDispatchTestHook = void (*)(
    std::string_view phase,
    void* context);
void SetPreparedMetadataBindingDispatchTestHookForTesting(
    PreparedMetadataBindingDispatchTestHook hook,
    void* context);

sb_engine_status_t CreatePreparedMetadataBinding(
    sb_engine_session_t session,
    const sb_engine_request_context_v1_t* prepare_context,
    std::string_view sealed_prepare_transaction_uuid,
    const sb_engine_sblr_dispatch_params_v1_t* invoke_params,
    PreparedMetadataBindingHandle* out_binding,
    sb_engine_result_t* out_result);

sb_engine_status_t ReleasePreparedMetadataBinding(
    PreparedMetadataBindingHandle binding);

sb_engine_status_t DispatchWithPreparedMetadataBinding(
    sb_engine_session_t session,
    sb_engine_transaction_t transaction,
    const sb_engine_request_context_v1_t* context,
    const sb_engine_sblr_dispatch_params_v1_t* params,
    PreparedMetadataBindingHandle binding,
    sb_engine_result_t* out_result);

}  // namespace scratchbird::server_engine_bridge
