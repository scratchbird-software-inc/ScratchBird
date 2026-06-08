// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PROCESS_ASSOCIATION_REGISTRY

#pragma once

#include "maintenance_coordinator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server {

enum class ProcessAssociationKind {
  kManager,
  kListener,
  kParser,
  kIpcEndpoint,
  kServerProcess,
  kWorkerProcess,
  kSession,
  kAttachment,
  kClientConnection,
  kRoute,
};

struct ProcessAssociationRecord {
  ProcessAssociationKind kind = ProcessAssociationKind::kWorkerProcess;
  std::string database_uuid;
  std::string database_path;
  std::string engine_instance_uuid;
  std::string component_uuid;
  std::string process_uuid;
  std::int64_t pid = -1;
  std::string route_uuid;
  std::uint64_t route_generation = 0;
  std::string listener_uuid;
  std::string parser_instance_uuid;
  std::string manager_uuid;
  std::string session_uuid;
  std::string attachment_uuid;
  std::string ipc_endpoint;
  std::uint64_t lifecycle_generation = 0;
  std::uint64_t association_generation = 0;
  std::uint64_t heartbeat_generation = 0;
  std::uint64_t policy_generation = 0;
  std::uint64_t shutdown_generation = 0;
  std::uint64_t active_local_transaction_id = 0;
  std::string state = "associated";
  bool healthy = true;
  bool cluster_authority_required = false;
  bool cluster_authority_available = false;
};

struct ProcessAssociationRegistry {
  std::uint64_t generation = 1;
  std::vector<ProcessAssociationRecord> records;
};

struct ProcessAssociationScopeResult {
  bool scope_proven = false;
  bool ambiguous = false;
  bool stale = false;
  bool parser_fallback_available = false;
  bool parser_fallback_missing = false;
  bool parser_fallback_stale = false;
  bool listener_unavailable = false;
  bool cluster_fail_closed = false;
  std::uint64_t associated_manager_count = 0;
  std::uint64_t associated_listener_count = 0;
  std::uint64_t associated_parser_count = 0;
  std::uint64_t associated_ipc_endpoint_count = 0;
  std::uint64_t associated_session_count = 0;
  std::uint64_t associated_client_count = 0;
  std::uint64_t active_transaction_session_count = 0;
  std::uint64_t required_acknowledgement_count = 0;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

const char* ProcessAssociationKindName(ProcessAssociationKind kind);
void RegisterProcessAssociation(ProcessAssociationRegistry* registry,
                                ProcessAssociationRecord record);
ProcessAssociationScopeResult EvaluateProcessAssociationsForDatabase(
    const ProcessAssociationRegistry& registry,
    const std::string& database_uuid,
    const std::string& database_path,
    std::uint64_t shutdown_generation,
    bool parser_fallback_required);
ProcessAssociationScopeResult ApplyProcessAssociationScopeToShutdownSnapshot(
    const ProcessAssociationRegistry& registry,
    const std::string& database_uuid,
    const std::string& database_path,
    std::uint64_t shutdown_generation,
    bool parser_fallback_required,
    ServerShutdownRuntimeSnapshot* snapshot);

}  // namespace scratchbird::server
