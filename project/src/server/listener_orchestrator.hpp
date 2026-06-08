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
#include "diagnostics.hpp"
#include "lifecycle.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server {

enum class ServerListenerState {
  kDisabled,
  kStopped,
  kStarting,
  kRunning,
  kDraining,
  kFailed,
};

struct ServerListenerProfileRuntime {
  std::string listener_uuid;
  std::string profile_name = "native";
  std::string state = "disabled";
  std::string bind_host = "127.0.0.1";
  std::uint64_t port = 3050;
  bool enabled = false;
  bool reloadable = true;
  std::string parser_package_ref = "builtin_test_package";
  std::string engine_endpoint;
  std::string database_selector;
  std::string listener_executable_path;
  std::string parser_executable_path;
  std::string control_dir;
  std::string runtime_dir;
  bool tls_required = true;
  std::string tls_cert_file;
  std::string tls_key_file;
  std::string tls_ca_file;
  std::string management_socket_path;
  std::int64_t pid = -1;
  std::uint64_t ready_timeout_ms = 5000;
  std::string last_transition = "bootstrap";
  std::string diagnostic_code;
  std::string last_management_response;
};

struct ServerListenerOperationResult {
  bool ok = false;
  std::string outcome = "refused";
  std::string state_before;
  std::string state_after;
  std::string target_uuid;
  std::uint64_t generation = 1;
  std::vector<ServerDiagnostic> diagnostics;
};

struct ServerListenerOrchestrator {
  std::uint64_t generation = 1;
  std::string engine_endpoint;
  std::vector<ServerListenerProfileRuntime> profiles;
  std::vector<ServerDiagnostic> diagnostics;
};

ServerListenerOrchestrator BuildListenerOrchestrator(const ServerBootstrapConfig& config,
                                                     const ServerLifecycleArtifacts& artifacts);
std::string ListenerStateName(ServerListenerState state);
std::string ListenerOrchestratorStatusJson(const ServerListenerOrchestrator& orchestrator);
ServerListenerOperationResult StartEnabledServerListeners(ServerListenerOrchestrator* orchestrator,
                                                          const ServerBootstrapConfig& config,
                                                          const ServerLifecycleArtifacts& artifacts);
ServerListenerOperationResult StopManagedServerListeners(ServerListenerOrchestrator* orchestrator,
                                                         const std::string& mode);
ServerListenerOperationResult ApplyListenerOperation(ServerListenerOrchestrator* orchestrator,
                                                     const ServerBootstrapConfig& config,
                                                     const ServerLifecycleArtifacts& artifacts,
                                                     const std::string& operation_key,
                                                     const std::string& target_uuid,
                                                     const std::string& mode);
ServerDiagnostic ListenerDiagnostic(std::string code,
                                    std::string safe_message,
                                    std::vector<ServerDiagnosticField> fields = {});

}  // namespace scratchbird::server
