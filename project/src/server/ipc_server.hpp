// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_IPC_FOUNDATION_ENDPOINT

#pragma once

#include "config.hpp"
#include "diagnostics.hpp"
#include "engine_host.hpp"
#include "lifecycle.hpp"
#include "session_registry.hpp"
#include "sbps.hpp"

#include <functional>
#include <vector>

namespace scratchbird::server {

struct ServerIpcEndpointResult {
  int exit_code = 0;
  std::vector<ServerDiagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

struct ParserServerIpcLifecycleCallbacks {
  std::function<void()> on_ready;
  std::function<void()> on_stopping;
};

// Process-lifetime stop state for the parser-server endpoint. The request API
// is safe to call from service-control and worker threads; the endpoint polls
// it and performs its normal listener/session/lifecycle cleanup before return.
void ResetParserServerStopRequest();
void RequestParserServerStop();
bool ParserServerStopRequested();

ServerIpcEndpointResult RunParserServerIpcEndpoint(const ServerBootstrapConfig& config,
                                                   const ServerLifecycleArtifacts& artifacts,
                                                   const HostedEngineState& engine_state,
                                                   const ParserServerIpcLifecycleCallbacks& callbacks = {});

std::vector<std::uint8_t> ResolveNamePublicFrameForEmbedded(
    const sbps::Frame& frame,
    const HostedEngineState& engine_state,
    ServerSessionRegistry* session_registry);
std::vector<std::uint8_t> RenderUuidPublicFrameForEmbedded(
    const sbps::Frame& frame,
    const ServerSessionRegistry* session_registry);

// Server-owned teardown for a physical parser channel that closes without a
// trustworthy disconnect frame.  Session and transaction identities are
// selected exclusively from the registry's channel binding.
std::vector<SessionOperationResult> HandleUnexpectedParserChannelClose(
    ServerSessionRegistry* session_registry,
    const std::array<std::uint8_t, 16>& server_channel_uuid);
bool ParserChannelHelloMayBeAdmittedForTest(
    bool hello_already_admitted,
    bool capability_bitmap_unchanged,
    bool hello_identity_unchanged,
    bool connection_authenticated = false);

}  // namespace scratchbird::server
