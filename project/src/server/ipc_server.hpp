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
#include "listener_orchestrator.hpp"
#include "parser_server_event_ipc.hpp"
#include "server_agent_runtime.hpp"
#include "catalog/name_list_api.hpp"

#include <functional>
#include <vector>
#include <exception>

namespace scratchbird::server {

struct ServerIpcEndpointResult {
  int exit_code = 0;
  std::vector<ServerDiagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

struct ServerIpcClientCohort;

// Caller-owned, stable-address lifetime cohort. The host must retain this owner
// until DrainServerIpcEndpoint reports completion, even after endpoint throws.
// In particular, an embedded library never terminates its host on drain failure.
// Endpoint and Drain calls require exclusive host access; no detached work.
struct ServerIpcEndpointOwner {
  explicit ServerIpcEndpointOwner(const HostedEngineState& state);
  ~ServerIpcEndpointOwner();
  ServerIpcEndpointOwner(const ServerIpcEndpointOwner&) = delete;
  ServerIpcEndpointOwner& operator=(const ServerIpcEndpointOwner&) = delete;
  // Declaration order keeps actual database locks/runtime references alive
  // until all dependent service and native-resource records have been retired.
  HostedEngineState engine_state;
  ServerSessionRegistry sessions;
  ServerListenerOrchestrator listeners;
  ServerAgentRuntime agents;
  std::exception_ptr first_failure;
  bool entered = false;
  bool endpoint_active = false;
  std::unique_ptr<ServerIpcClientCohort> clients;
};

struct ServerIpcDrainResult {
  bool complete = false;
  bool clients_complete = false;
  bool agents_complete = false;
  bool listeners_complete = false;
  bool sessions_complete = false;
  std::vector<ServerDiagnostic> diagnostics;
};

// One cooperative reconciliation pass, not a hard I/O deadline. The standalone
// process supervisor enforces the approved terminal deadline independently.
ServerIpcDrainResult DrainServerIpcEndpoint(ServerIpcEndpointOwner& owner);

struct ParserServerIpcLifecycleCallbacks {
  std::function<void()> on_ready;
  std::function<void()> on_stopping;
};

// Process-lifetime stop state for the parser-server endpoint. The request API
// is safe to call from service-control and worker threads; the endpoint polls
// it and performs its normal listener/session/lifecycle cleanup before return.
void ResetParserServerStopRequest();
void RequestParserServerStop() noexcept;
bool ParserServerStopRequested();

ServerIpcEndpointResult RunParserServerIpcEndpoint(const ServerBootstrapConfig& config,
                                                   const ServerLifecycleArtifacts& artifacts,
                                                   ServerIpcEndpointOwner& owner,
                                                   const ParserServerIpcLifecycleCallbacks& callbacks = {});

std::vector<std::uint8_t> ResolveNamePublicFrameForEmbedded(
    const sbps::Frame& frame,
    const HostedEngineState& engine_state,
    ServerSessionRegistry* session_registry);
// SB_ENGINE_NAME_LIST_API (Part B S4): the engine's ListNames and UuidToName for an in-process caller, with no frame
// and no wire schema (S5 owns the wire). The session is looked up by UUID in the registry; the engine context is its
// native served database's (a session of a database that is not native served is refused by the engine with
// CATALOG.NAMES_LIST_NOT_NATIVE). Returns the engine's result unchanged; a session that is not in the registry is a
// refusal with CATALOG.NAMES_LIST_SESSION_UNKNOWN.
scratchbird::engine::internal_api::EngineListNamesResult ListNamesForEmbedded(
    const ServerSessionRegistry* session_registry,
    const scratchbird::core::platform::Uuid& session_uuid,
    const scratchbird::core::platform::Uuid& scope_uuid,
    const scratchbird::core::platform::Uuid& language_uuid,
    const std::string& continuation,
    std::uint32_t max_entries);
scratchbird::engine::internal_api::EngineUuidToNameResult UuidToNameForEmbedded(
    const ServerSessionRegistry* session_registry,
    const scratchbird::core::platform::Uuid& session_uuid,
    const scratchbird::core::platform::Uuid& object_uuid,
    const scratchbird::core::platform::Uuid& language_uuid);
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

// The parser event engine context a bound session produces (S4g part 1: it carries session.native_served_facts, or the facts of the served
// database it chose from the engine state when the session names no path). Test seam as well as production helper.
ParserServerEventEngineContext EventEngineContextFromSession(const ServerSessionRecord& session,
                                                             const HostedEngineState& engine_state,
                                                             const sbps::Frame& request);

}  // namespace scratchbird::server
