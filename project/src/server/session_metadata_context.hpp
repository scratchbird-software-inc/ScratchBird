// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "session_registry.hpp"

namespace scratchbird::server {
// Internal metadata lease. Caller holds the selected session transaction mutex;
// the returned engine receipt must be released before that lock is released.
// No receipt or statement authority is exported by name resolution.
bool AcquireServerMetadataContext(
    ServerSessionRegistry* registry, ServerSessionRecord session,
    const HostedEngineState& engine_state, const sbps::Frame& request,
    scratchbird::engine::internal_api::EngineRequestContext* context,
    scratchbird::server_engine_bridge::StatementContextReceiptHandle* receipt,
    ServerDiagnostic* diagnostic);
}
