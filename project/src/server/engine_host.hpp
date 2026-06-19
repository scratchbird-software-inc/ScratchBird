// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_ENGINE_HOST_DATABASE_SUPERVISION

#pragma once

#include "config.hpp"
#include "database_ownership.hpp"
#include "diagnostics.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::server {

enum class HostedDatabaseState {
  kNotConfigured,
  kOpening,
  kOpen,
  kReadOnly,
  kRestrictedOpen,
  kMaintenance,
  kFailed,
  kDetached,
  kQuarantined,
};

struct HostedDatabaseSnapshot {
  HostedDatabaseState state = HostedDatabaseState::kNotConfigured;
  std::string database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string resource_seed_pack_root;
  std::string policy_seed_pack_root;
  bool database_created = false;
  bool database_open = false;
  bool read_only = false;
  bool write_admission_fenced = true;
  bool cluster_structures_present = false;
  bool cluster_authority_required = false;
  std::string startup_recovery_classification;
  std::string lifecycle_mode;
  std::string database_engine_agent_state = "not_started";
  std::string database_engine_agent_instance_uuid;
  std::vector<std::string> selected_agent_type_ids;
  std::uint64_t database_engine_agent_health_generation = 0;
  bool database_engine_agent_ordinary_admission_allowed = false;
  std::string database_engine_agent_health_json;
  bool config_policy_security_lifecycle_present = false;
  std::uint64_t config_source_epoch = 1;
  std::uint64_t config_reload_generation = 1;
  std::uint64_t capability_policy_generation = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t security_epoch = 1;
  std::string security_provider_family = "local_password";
  std::uint64_t security_provider_generation = 1;
  std::string security_provider_state = "healthy";
  bool default_policy_installed = true;
  std::uint64_t cache_invalidation_epoch = 1;
  std::string config_policy_security_lifecycle_json;
  std::string diagnostic_code;
  std::string diagnostic_message_key;
};

struct HostedEngineState {
  bool engine_context_active = false;
  std::vector<HostedDatabaseSnapshot> databases;
  std::vector<std::shared_ptr<DatabaseOwnershipLock>> database_ownership_locks;
};

struct HostedEngineResult {
  HostedEngineState state;
  std::vector<ServerDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

const char* HostedDatabaseStateName(HostedDatabaseState state);
HostedEngineResult StartHostedEngine(const ServerBootstrapConfig& config);
std::string HostedEngineStatusJson(const HostedEngineState& state);

}  // namespace scratchbird::server
