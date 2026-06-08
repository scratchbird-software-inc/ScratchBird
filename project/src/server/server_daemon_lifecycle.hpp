// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_DAEMON_LIFECYCLE_SUPERVISION

#pragma once

#include "config.hpp"
#include "diagnostics.hpp"
#include "engine_host.hpp"
#include "lifecycle.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server {

struct ServerDaemonDatabaseAssociation {
  std::string database_uuid;
  std::string database_path;
  std::string state;
  bool database_open = false;
  bool failed = false;
  bool quarantined = false;
};

struct ServerDaemonLifecycleSnapshot {
  std::uint64_t lifecycle_generation = 0;
  std::string daemon_scope = "shared";
  std::string state = "initializing";
  bool service_ready = false;
  bool scope_ambiguous = false;
  bool dedicated_database_daemon = false;
  bool daemon_exclusive_to_database = false;
  bool shared_daemon_has_other_databases = false;
  bool hosted_database_failure = false;
  bool quarantine_required = false;
  bool orphan_cleanup_required = false;
  bool runtime_directories_valid = false;
  bool pid_owner_state_valid = false;
  bool endpoint_descriptors_valid = false;
  bool database_association_valid = false;
  bool standalone_cluster_path_refused = false;
  std::uint64_t hosted_database_count = 0;
  std::uint64_t open_database_count = 0;
  std::uint64_t failed_database_count = 0;
  std::vector<ServerDaemonDatabaseAssociation> databases;
  std::vector<ServerDiagnostic> diagnostics;
};

ServerDaemonLifecycleSnapshot EvaluateServerDaemonLifecycle(
    const ServerBootstrapConfig& config,
    const ServerLifecycleArtifacts& artifacts,
    const HostedEngineState& engine_state);

bool ServerDaemonShouldStopForDatabaseShutdown(
    const ServerDaemonLifecycleSnapshot& snapshot,
    const std::string& target_database_uuid);

std::string ServerDaemonLifecycleStatusJson(
    const ServerDaemonLifecycleSnapshot& snapshot);

}  // namespace scratchbird::server
