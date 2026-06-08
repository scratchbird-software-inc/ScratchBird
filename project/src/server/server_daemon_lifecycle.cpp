// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_DAEMON_LIFECYCLE_SUPERVISION

#include "server_daemon_lifecycle.hpp"

#include <filesystem>
#include <sstream>
#include <utility>

namespace scratchbird::server {

namespace {

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

ServerDiagnostic DaemonDiagnostic(std::string code,
                                  std::string message,
                                  std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

bool IsDedicatedScope(const ServerBootstrapConfig& config) {
  return config.database_daemon_scope == "dedicated";
}

bool RequiresHostedDatabase(const ServerBootstrapConfig& config) {
  return !config.database_default_path.empty();
}

}  // namespace

ServerDaemonLifecycleSnapshot EvaluateServerDaemonLifecycle(
    const ServerBootstrapConfig& config,
    const ServerLifecycleArtifacts& artifacts,
    const HostedEngineState& engine_state) {
  ServerDaemonLifecycleSnapshot snapshot;
  snapshot.lifecycle_generation = artifacts.generation;
  snapshot.daemon_scope = config.database_daemon_scope.empty() ? "shared" : config.database_daemon_scope;
  snapshot.dedicated_database_daemon = IsDedicatedScope(config);
  const bool artifacts_publishable = !artifacts.pid_file.empty() ||
                                     !artifacts.owner_token_file.empty() ||
                                     !artifacts.lifecycle_state_file.empty();

  if (snapshot.daemon_scope != "shared" && snapshot.daemon_scope != "dedicated") {
    snapshot.scope_ambiguous = true;
    snapshot.state = "failed";
    snapshot.diagnostics.push_back(DaemonDiagnostic(
        "SERVER.DAEMON.SCOPE_INVALID",
        "The server daemon database scope is invalid.",
        {{"daemon_scope", snapshot.daemon_scope}}));
    return snapshot;
  }

  if (artifacts_publishable) {
    const auto validation = ValidateServerRuntimeArtifacts(config, artifacts, true);
    snapshot.runtime_directories_valid = validation.directories_valid;
    snapshot.pid_owner_state_valid = validation.pid_file_valid &&
                                     validation.owner_token_valid &&
                                     validation.lifecycle_state_valid;
    snapshot.endpoint_descriptors_valid = validation.endpoint_descriptor_valid;
    snapshot.database_association_valid = validation.database_association_valid;
    snapshot.diagnostics.insert(snapshot.diagnostics.end(),
                                validation.diagnostics.begin(),
                                validation.diagnostics.end());
    if (!validation.ok()) {
      snapshot.state = "failed";
    }
  } else {
    snapshot.runtime_directories_valid = true;
    snapshot.pid_owner_state_valid = true;
    snapshot.endpoint_descriptors_valid = true;
    snapshot.database_association_valid = true;
  }

  for (const auto& database : engine_state.databases) {
    ServerDaemonDatabaseAssociation association;
    association.database_uuid = database.database_uuid;
    association.database_path = database.database_path;
    association.state = HostedDatabaseStateName(database.state);
    association.database_open = database.database_open;
    association.failed = database.state == HostedDatabaseState::kFailed;
    association.quarantined = database.state == HostedDatabaseState::kQuarantined;
    snapshot.databases.push_back(association);
    ++snapshot.hosted_database_count;
    if (association.database_open) ++snapshot.open_database_count;
    if (association.failed) ++snapshot.failed_database_count;
    if (association.failed) snapshot.hosted_database_failure = true;
    if (association.failed || association.quarantined) snapshot.quarantine_required = true;
    if (database.cluster_authority_required) {
      snapshot.standalone_cluster_path_refused = true;
    }
  }

  if (snapshot.dedicated_database_daemon && snapshot.hosted_database_count > 1) {
    snapshot.scope_ambiguous = true;
    snapshot.state = "failed";
    snapshot.diagnostics.push_back(DaemonDiagnostic(
        "SERVER.DAEMON.SCOPE_AMBIGUOUS",
        "A dedicated database server daemon cannot supervise more than one hosted database.",
        {{"hosted_database_count", std::to_string(snapshot.hosted_database_count)}}));
  }

  if (RequiresHostedDatabase(config) && snapshot.open_database_count == 0) {
    snapshot.state = "failed";
    snapshot.diagnostics.push_back(DaemonDiagnostic(
        "SERVER.DAEMON.HOSTED_DATABASE_UNAVAILABLE",
        "The server daemon cannot become service-ready because no required hosted database is open.",
        {{"database_ref", "[path-redacted]"}}));
  }

  if (RequiresHostedDatabase(config) && artifacts_publishable) {
    bool matched = false;
    const auto expected = config.database_default_path.lexically_normal();
    for (const auto& database : snapshot.databases) {
      if (!database.database_path.empty() &&
          std::filesystem::path(database.database_path).lexically_normal() == expected) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      snapshot.database_association_valid = false;
      snapshot.state = "failed";
      snapshot.diagnostics.push_back(DaemonDiagnostic(
          "SERVER.DAEMON.DATABASE_SCOPE_INVALID",
          "The server daemon cannot become service-ready because the hosted database association is not proven.",
          {{"database_ref", "[path-redacted]"}}));
    }
  }

  if (snapshot.hosted_database_failure) {
    snapshot.state = "failed";
    snapshot.diagnostics.push_back(DaemonDiagnostic(
        "SERVER.DAEMON.HOSTED_DATABASE_FAILED",
        "The server daemon cannot become service-ready because a hosted database failed.",
        {{"failed_database_count", std::to_string(snapshot.failed_database_count)}}));
  }

  if (snapshot.standalone_cluster_path_refused) {
    snapshot.state = "failed";
    snapshot.diagnostics.push_back(DaemonDiagnostic(
        "SERVER.DAEMON.CLUSTER_AUTHORITY_REQUIRED",
        "Standalone service startup refused a cluster-private database path.",
        {{"daemon_scope", snapshot.daemon_scope}}));
  }

  snapshot.daemon_exclusive_to_database =
      snapshot.dedicated_database_daemon && snapshot.open_database_count == 1 &&
      !snapshot.scope_ambiguous;
  snapshot.shared_daemon_has_other_databases =
      !snapshot.dedicated_database_daemon && snapshot.open_database_count > 1;

  if (snapshot.diagnostics.empty()) {
    snapshot.service_ready = true;
    snapshot.state = "service_ready";
  } else if (snapshot.quarantine_required) {
    snapshot.state = "quarantined";
  }
  return snapshot;
}

bool ServerDaemonShouldStopForDatabaseShutdown(
    const ServerDaemonLifecycleSnapshot& snapshot,
    const std::string& target_database_uuid) {
  if (!snapshot.daemon_exclusive_to_database || target_database_uuid.empty()) return false;
  return snapshot.databases.size() == 1 &&
         snapshot.databases.front().database_uuid == target_database_uuid;
}

std::string ServerDaemonLifecycleStatusJson(
    const ServerDaemonLifecycleSnapshot& snapshot) {
  std::ostringstream out;
  out << "{\"server_daemon_lifecycle\":{\"state\":\"" << JsonEscape(snapshot.state)
      << "\",\"service_ready\":" << (snapshot.service_ready ? "true" : "false")
      << ",\"daemon_scope\":\"" << JsonEscape(snapshot.daemon_scope)
      << "\",\"lifecycle_generation\":" << snapshot.lifecycle_generation
      << ",\"hosted_database_count\":" << snapshot.hosted_database_count
      << ",\"open_database_count\":" << snapshot.open_database_count
      << ",\"failed_database_count\":" << snapshot.failed_database_count
      << ",\"scope_ambiguous\":" << (snapshot.scope_ambiguous ? "true" : "false")
      << ",\"dedicated_database_daemon\":"
      << (snapshot.dedicated_database_daemon ? "true" : "false")
      << ",\"daemon_exclusive_to_database\":"
      << (snapshot.daemon_exclusive_to_database ? "true" : "false")
      << ",\"shared_daemon_has_other_databases\":"
      << (snapshot.shared_daemon_has_other_databases ? "true" : "false")
      << ",\"hosted_database_failure\":"
      << (snapshot.hosted_database_failure ? "true" : "false")
      << ",\"quarantine_required\":"
      << (snapshot.quarantine_required ? "true" : "false")
      << ",\"runtime_directories_valid\":"
      << (snapshot.runtime_directories_valid ? "true" : "false")
      << ",\"pid_owner_state_valid\":"
      << (snapshot.pid_owner_state_valid ? "true" : "false")
      << ",\"endpoint_descriptors_valid\":"
      << (snapshot.endpoint_descriptors_valid ? "true" : "false")
      << ",\"database_association_valid\":"
      << (snapshot.database_association_valid ? "true" : "false")
      << ",\"standalone_cluster_path_refused\":"
      << (snapshot.standalone_cluster_path_refused ? "true" : "false")
      << ",\"databases\":[";
  for (std::size_t i = 0; i < snapshot.databases.size(); ++i) {
    if (i != 0) out << ',';
    const auto& database = snapshot.databases[i];
    out << "{\"database_uuid\":\"" << JsonEscape(database.database_uuid)
        << "\",\"database_ref\":\""
        << JsonEscape(database.database_path.empty() ? "" : "[path-redacted]")
        << "\",\"state\":\"" << JsonEscape(database.state)
        << "\",\"database_open\":" << (database.database_open ? "true" : "false")
        << ",\"failed\":" << (database.failed ? "true" : "false")
        << ",\"quarantined\":" << (database.quarantined ? "true" : "false")
        << "}";
  }
  out << "]}}\n";
  return out.str();
}

}  // namespace scratchbird::server
