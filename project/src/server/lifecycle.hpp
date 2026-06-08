// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_CONFIG_LIFECYCLE_ARTIFACTS

#pragma once

#include "diagnostics.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server {

struct ServerBootstrapConfig;

struct ServerLifecycleArtifacts {
  std::uint64_t generation = 0;
  std::string state;
  std::string pid_file;
  std::string owner_token_file;
  std::string lifecycle_state_file;
  std::string lifecycle_journal_file;
  std::string database_runtime_scope_id;
  std::string database_path;
  std::string daemon_scope;
  std::string sbps_endpoint;
};

struct ServerLifecycleResult {
  ServerLifecycleArtifacts artifacts;
  std::vector<ServerDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct ServerRuntimeArtifactValidation {
  bool directories_valid = false;
  bool pid_file_valid = false;
  bool owner_token_valid = false;
  bool lifecycle_state_valid = false;
  bool endpoint_descriptor_valid = false;
  bool database_association_valid = false;
  bool cleanup_safe = false;
  std::vector<ServerDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

enum class ServerRuntimeCleanupOperation {
  kStop,
  kRestart,
  kUninstall,
};

ServerLifecycleResult WriteStartupLifecycleArtifacts(const ServerBootstrapConfig& config,
                                                     const std::string& target_state);
ServerLifecycleResult WriteStoppedLifecycleArtifacts(const ServerBootstrapConfig& config,
                                                     std::uint64_t generation);
ServerRuntimeArtifactValidation ValidateServerRuntimeArtifacts(const ServerBootstrapConfig& config,
                                                               const ServerLifecycleArtifacts& artifacts,
                                                               bool require_existing_files);
ServerLifecycleResult CleanupServerRuntimeArtifacts(const ServerBootstrapConfig& config,
                                                    const ServerLifecycleArtifacts& artifacts,
                                                    ServerRuntimeCleanupOperation operation);

}  // namespace scratchbird::server
