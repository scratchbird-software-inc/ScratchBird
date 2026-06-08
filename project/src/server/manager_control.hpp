// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_MANAGEMENT_LISTENER_COORDINATION

#pragma once

#include "config.hpp"
#include "engine_host.hpp"
#include "listener_orchestrator.hpp"
#include "maintenance_coordinator.hpp"
#include "parser_package_registry.hpp"
#include "sbps.hpp"
#include "server_observability.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::server {

struct ServerManagementRequest {
  std::string operation_key;
  std::string target_uuid;
  std::string mode;
  std::string audit_reason;
  std::uint64_t timeout_ms = 30000;
  bool include_history = false;
};

struct ServerManagementResponse {
  bool accepted = false;
  bool error = false;
  std::uint16_t response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kManagementResult);
  std::uint32_t response_schema_id = sbps::kSchemaManagementResponseV1;
  std::array<std::uint8_t, 16> session_uuid{};
  std::vector<std::uint8_t> payload;
  std::vector<ServerDiagnostic> diagnostics;
};

struct ServerManagementContext {
  const ServerBootstrapConfig* config = nullptr;
  const ServerLifecycleArtifacts* artifacts = nullptr;
  const HostedEngineState* engine_state = nullptr;
  ServerSessionRegistry* session_registry = nullptr;
  const ParserPackageRegistry* parser_registry = nullptr;
  ServerListenerOrchestrator* listener_orchestrator = nullptr;
  ServerMaintenanceCoordinator* maintenance_coordinator = nullptr;
  ServerObservabilityState* observability = nullptr;
};

std::vector<std::uint8_t> EncodeServerManagementRequestForTest(
    const ServerManagementRequest& request);
std::optional<ServerManagementRequest> DecodeServerManagementRequest(
    const std::vector<std::uint8_t>& payload);
ServerManagementResponse HandleServerManagementRequest(const ServerManagementContext& context,
                                                       const sbps::Frame& request);
std::string ServerManagementRightsMatrixJson();

}  // namespace scratchbird::server
