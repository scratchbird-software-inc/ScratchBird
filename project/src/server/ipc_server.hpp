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

#include <vector>

namespace scratchbird::server {

struct ServerIpcEndpointResult {
  int exit_code = 0;
  std::vector<ServerDiagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

ServerIpcEndpointResult RunParserServerIpcEndpoint(const ServerBootstrapConfig& config,
                                                   const ServerLifecycleArtifacts& artifacts,
                                                   const HostedEngineState& engine_state);

std::vector<std::uint8_t> ResolveNamePublicFrameForEmbedded(
    const sbps::Frame& frame,
    const HostedEngineState& engine_state,
    const ServerSessionRegistry* session_registry);
std::vector<std::uint8_t> RenderUuidPublicFrameForEmbedded(
    const sbps::Frame& frame,
    const ServerSessionRegistry* session_registry);

}  // namespace scratchbird::server
