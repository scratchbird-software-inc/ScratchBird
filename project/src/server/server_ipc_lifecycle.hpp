// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_IPC_LIFECYCLE_CLOSURE

#pragma once

#include "config.hpp"
#include "diagnostics.hpp"
#include "engine_host.hpp"
#include "lifecycle.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scratchbird::server {

inline constexpr std::uint32_t kServerIpcEndpointDescriptorFormatCurrent = 1;
inline constexpr std::uint32_t kServerIpcEndpointDescriptorFormatMinSupported = 1;
inline constexpr std::uint32_t kServerIpcEndpointDescriptorFormatMaxSupported = 1;
inline constexpr std::uint32_t kServerLifecycleStateFileFormatCurrent = 1;
inline constexpr std::uint32_t kServerLifecycleStateFileFormatMinSupported = 1;
inline constexpr std::uint32_t kServerLifecycleStateFileFormatMaxSupported = 1;

enum class ServerIpcEndpointClass {
  kParserServer,
  kServerManagement,
  kListenerManagement,
  kManagerInternal,
  kEventInternal,
};

enum class ServerIpcEndpointOperation {
  kConnect,
  kParserHello,
  kAuthenticatedRequest,
  kManagementRequest,
  kEventRequest,
  kDrain,
  kShutdown,
};

enum class ServerLifecycleArtifactCompatibilityClass {
  kSupportedCurrent,
  kSupportedMigration,
  kUnsupportedOld,
  kUnsupportedNew,
  kDowngradeRefused,
  kNewerThanSupportedRefused,
  kAmbiguousIdentityRefused,
  kMissingMigrationPlanRefused,
  kMigrationRequiredWithoutPlanRefused,
};

struct ServerLifecycleArtifactMigrationRequest {
  std::string artifact_kind;
  std::uint32_t format_version = 0;
  std::uint32_t min_supported_version = 0;
  std::uint32_t current_version = 0;
  std::uint32_t max_supported_version = 0;
  bool identity_proven = true;
  bool downgrade_requested = false;
  bool migration_plan_required = false;
  std::string migration_plan_id;
};

struct ServerLifecycleArtifactMigrationEvaluation {
  bool accepted = false;
  bool migration_required = false;
  ServerLifecycleArtifactCompatibilityClass compatibility_class =
      ServerLifecycleArtifactCompatibilityClass::kUnsupportedNew;
  ServerDiagnostic diagnostic;
};

struct ServerIpcEndpointDescriptor {
  ServerIpcEndpointClass endpoint_class = ServerIpcEndpointClass::kParserServer;
  std::string endpoint_id;
  std::string protocol_family;
  std::string transport;
  std::filesystem::path endpoint_path;
  std::filesystem::path descriptor_path;
  std::uint32_t descriptor_format_version = kServerIpcEndpointDescriptorFormatCurrent;
  std::uint16_t protocol_major = 1;
  std::uint16_t protocol_minor = 0;
  std::string database_uuid;
  std::string database_path;
  std::uint64_t lifecycle_generation = 0;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t config_source_epoch = 1;
  std::uint64_t current_config_source_epoch = 1;
  std::uint64_t config_reload_generation = 1;
  std::uint64_t current_config_reload_generation = 1;
  std::uint64_t capability_policy_generation = 1;
  std::uint64_t current_capability_policy_generation = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t current_policy_generation = 1;
  std::uint64_t security_epoch = 1;
  std::uint64_t current_security_epoch = 1;
  std::uint64_t resource_epoch = 1;
  std::uint64_t current_resource_epoch = 1;
  std::uint64_t cache_invalidation_epoch = 1;
  std::uint32_t file_mode = 0600;
  std::uint64_t max_frame_bytes = 0;
  std::uint64_t max_streams = 0;
  std::uint64_t max_active_channels = 0;
  std::uint64_t active_channels = 0;
  std::uint64_t max_queued_frames = 0;
  std::uint64_t queued_frames = 0;
  std::uint64_t max_queued_bytes = 0;
  std::uint64_t queued_bytes = 0;
  bool descriptor_present = true;
  bool descriptor_owner_matches = true;
  bool bound = true;
  bool service_ready = true;
  bool frame_valid = true;
  bool frame_schema_valid = true;
  bool authenticated = false;
  bool authorized = false;
  bool session_bound = false;
  bool draining = false;
  bool shutting_down = false;
  bool stale = false;
  bool failed = false;
  bool quarantine_requested = false;
  bool cluster_private = false;
  bool cluster_authority_available = false;
};

struct ServerIpcEndpointLifecycleEvaluation {
  bool admitted = false;
  bool descriptor_valid = false;
  bool backpressure_required = false;
  bool drain_required = false;
  bool stale_cleanup_required = false;
  bool quarantine_required = false;
  bool shutdown_required = false;
  std::vector<ServerDiagnostic> diagnostics;
};

const char* ServerIpcEndpointClassName(ServerIpcEndpointClass endpoint_class);
const char* ServerIpcEndpointOperationName(ServerIpcEndpointOperation operation);
const char* ServerLifecycleArtifactCompatibilityClassName(
    ServerLifecycleArtifactCompatibilityClass compatibility_class);
ServerLifecycleArtifactMigrationEvaluation EvaluateServerLifecycleArtifactMigration(
    const ServerLifecycleArtifactMigrationRequest& request);

std::filesystem::path ParserServerEndpointDescriptorPath(const ServerBootstrapConfig& config);
ServerIpcEndpointDescriptor BuildParserServerEndpointDescriptor(
    const ServerBootstrapConfig& config,
    const ServerLifecycleArtifacts& artifacts,
    const HostedEngineState& engine_state);

bool WriteServerIpcEndpointDescriptor(const ServerIpcEndpointDescriptor& descriptor,
                                      std::vector<ServerDiagnostic>* diagnostics);

ServerIpcEndpointLifecycleEvaluation EvaluateServerIpcEndpointLifecycle(
    const ServerIpcEndpointDescriptor& descriptor,
    ServerIpcEndpointOperation operation);

std::string ServerIpcEndpointDescriptorText(const ServerIpcEndpointDescriptor& descriptor);

}  // namespace scratchbird::server
