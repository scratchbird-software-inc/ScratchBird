// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PROCESS_ASSOCIATION_REGISTRY

#include "process_association_registry.hpp"

#include <map>
#include <set>
#include <utility>

namespace scratchbird::server {

namespace {

bool EmptyScope(const ProcessAssociationRecord& record) {
  return record.database_uuid.empty() && record.database_path.empty();
}

bool DatabaseMatches(const ProcessAssociationRecord& record,
                     const std::string& database_uuid,
                     const std::string& database_path) {
  if (!database_uuid.empty() && !record.database_uuid.empty() &&
      record.database_uuid == database_uuid) {
    return true;
  }
  if (!database_path.empty() && !record.database_path.empty() &&
      record.database_path == database_path) {
    return true;
  }
  return database_uuid.empty() && database_path.empty() && !EmptyScope(record);
}

bool SameDatabaseScope(const ProcessAssociationRecord& left,
                       const ProcessAssociationRecord& right) {
  if (!left.database_uuid.empty() && !right.database_uuid.empty() &&
      left.database_uuid != right.database_uuid) {
    return false;
  }
  if (!left.database_path.empty() && !right.database_path.empty() &&
      left.database_path != right.database_path) {
    return false;
  }
  return true;
}

bool IsStale(const ProcessAssociationRecord& record, std::uint64_t shutdown_generation) {
  if (record.state == "stale" || record.state == "expired" || record.state == "cache_stale") {
    return true;
  }
  if (record.association_generation == 0 || record.heartbeat_generation == 0) {
    return true;
  }
  return shutdown_generation != 0 && record.shutdown_generation != 0 &&
         record.shutdown_generation != shutdown_generation;
}

bool ListenerUnavailable(const ProcessAssociationRecord& record) {
  return !record.healthy || record.state == "failed" || record.state == "unavailable" ||
         record.state == "timed_out";
}

void AddIdentity(std::map<std::string, const ProcessAssociationRecord*>* identities,
                 ProcessAssociationScopeResult* result,
                 const ProcessAssociationRecord& record,
                 const std::string& label,
                 const std::string& value) {
  if (value.empty()) return;
  const std::string key = label + ":" + value;
  const auto found = identities->find(key);
  if (found == identities->end()) {
    (*identities)[key] = &record;
    return;
  }
  if (!SameDatabaseScope(*found->second, record)) {
    result->ambiguous = true;
    result->diagnostic_code = "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS";
    result->diagnostic_detail = "association_identity_cross_database_collision";
  }
}

void AddRecordIdentities(std::map<std::string, const ProcessAssociationRecord*>* identities,
                         ProcessAssociationScopeResult* result,
                         const ProcessAssociationRecord& record) {
  AddIdentity(identities, result, record, "component", record.component_uuid);
  AddIdentity(identities, result, record, "process", record.process_uuid);
  AddIdentity(identities, result, record, "route", record.route_uuid);
  AddIdentity(identities, result, record, "listener", record.listener_uuid);
  AddIdentity(identities, result, record, "parser", record.parser_instance_uuid);
  AddIdentity(identities, result, record, "manager", record.manager_uuid);
  AddIdentity(identities, result, record, "session", record.session_uuid);
  AddIdentity(identities, result, record, "attachment", record.attachment_uuid);
  AddIdentity(identities, result, record, "ipc", record.ipc_endpoint);
}

void CountRecord(ProcessAssociationScopeResult* result,
                 const ProcessAssociationRecord& record) {
  switch (record.kind) {
    case ProcessAssociationKind::kManager:
      ++result->associated_manager_count;
      break;
    case ProcessAssociationKind::kListener:
      ++result->associated_listener_count;
      if (ListenerUnavailable(record)) result->listener_unavailable = true;
      break;
    case ProcessAssociationKind::kParser:
      ++result->associated_parser_count;
      if (!IsStale(record, 0)) result->parser_fallback_available = true;
      break;
    case ProcessAssociationKind::kIpcEndpoint:
      ++result->associated_ipc_endpoint_count;
      break;
    case ProcessAssociationKind::kSession:
      ++result->associated_session_count;
      ++result->associated_client_count;
      if (record.active_local_transaction_id != 0) {
        ++result->active_transaction_session_count;
      }
      break;
    case ProcessAssociationKind::kClientConnection:
      ++result->associated_client_count;
      break;
    case ProcessAssociationKind::kAttachment:
    case ProcessAssociationKind::kRoute:
    case ProcessAssociationKind::kServerProcess:
    case ProcessAssociationKind::kWorkerProcess:
      break;
  }
}

}  // namespace

const char* ProcessAssociationKindName(ProcessAssociationKind kind) {
  switch (kind) {
    case ProcessAssociationKind::kManager:
      return "manager";
    case ProcessAssociationKind::kListener:
      return "listener";
    case ProcessAssociationKind::kParser:
      return "parser";
    case ProcessAssociationKind::kIpcEndpoint:
      return "ipc_endpoint";
    case ProcessAssociationKind::kServerProcess:
      return "server_process";
    case ProcessAssociationKind::kWorkerProcess:
      return "worker_process";
    case ProcessAssociationKind::kSession:
      return "session";
    case ProcessAssociationKind::kAttachment:
      return "attachment";
    case ProcessAssociationKind::kClientConnection:
      return "client_connection";
    case ProcessAssociationKind::kRoute:
      return "route";
  }
  return "unknown";
}

void RegisterProcessAssociation(ProcessAssociationRegistry* registry,
                                ProcessAssociationRecord record) {
  if (registry == nullptr || EmptyScope(record)) return;
  if (record.component_uuid.empty()) {
    if (!record.parser_instance_uuid.empty()) {
      record.component_uuid = record.parser_instance_uuid;
    } else if (!record.listener_uuid.empty()) {
      record.component_uuid = record.listener_uuid;
    } else if (!record.session_uuid.empty()) {
      record.component_uuid = record.session_uuid;
    } else if (!record.route_uuid.empty()) {
      record.component_uuid = record.route_uuid;
    } else if (!record.ipc_endpoint.empty()) {
      record.component_uuid = record.ipc_endpoint;
    } else if (!record.manager_uuid.empty()) {
      record.component_uuid = record.manager_uuid;
    }
  }
  if (record.process_uuid.empty()) record.process_uuid = record.component_uuid;
  if (record.association_generation == 0) record.association_generation = registry->generation;
  if (record.heartbeat_generation == 0) record.heartbeat_generation = registry->generation;
  if (record.lifecycle_generation == 0) record.lifecycle_generation = registry->generation;
  if (record.policy_generation == 0) record.policy_generation = registry->generation;
  registry->records.push_back(std::move(record));
}

ProcessAssociationScopeResult EvaluateProcessAssociationsForDatabase(
    const ProcessAssociationRegistry& registry,
    const std::string& database_uuid,
    const std::string& database_path,
    std::uint64_t shutdown_generation,
    bool parser_fallback_required) {
  ProcessAssociationScopeResult result;
  if (registry.records.empty()) {
    result.diagnostic_code = "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS";
    result.diagnostic_detail = "association_registry_empty";
    return result;
  }

  std::set<std::pair<std::string, std::string>> database_scopes;
  std::map<std::string, const ProcessAssociationRecord*> identities;
  for (const auto& record : registry.records) {
    if (!EmptyScope(record)) {
      database_scopes.insert({record.database_uuid, record.database_path});
    }
    AddRecordIdentities(&identities, &result, record);
  }

  if (database_uuid.empty() && database_path.empty() && database_scopes.size() != 1) {
    result.ambiguous = true;
    result.diagnostic_code = "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS";
    result.diagnostic_detail = "target_database_not_unique";
  }

  bool any_match = false;
  bool any_parser = false;
  for (const auto& record : registry.records) {
    if (!DatabaseMatches(record, database_uuid, database_path)) continue;
    any_match = true;
    if (record.cluster_authority_required && !record.cluster_authority_available) {
      result.cluster_fail_closed = true;
      result.diagnostic_code = "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED";
      result.diagnostic_detail = "cluster_authority_required_without_cluster_authority";
    }
    const bool stale = IsStale(record, shutdown_generation);
    if (stale) {
      result.stale = true;
      if (record.kind == ProcessAssociationKind::kParser) {
        result.parser_fallback_stale = true;
        result.diagnostic_code = "ENGINE.SHUTDOWN_PARSER_ASSOCIATION_STALE";
        result.diagnostic_detail = "parser_association_stale";
      } else if (result.diagnostic_code.empty()) {
        result.diagnostic_code = "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS";
        result.diagnostic_detail =
            std::string(ProcessAssociationKindName(record.kind)) + "_association_stale";
      }
    }
    if (record.kind == ProcessAssociationKind::kParser) {
      any_parser = true;
      if (!stale) result.parser_fallback_available = true;
    }
    CountRecord(&result, record);
  }

  if (!any_match) {
    result.diagnostic_code = "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS";
    result.diagnostic_detail = "target_database_has_no_associations";
    return result;
  }
  if (parser_fallback_required && !any_parser) {
    result.parser_fallback_missing = true;
    result.diagnostic_code = "ENGINE.SHUTDOWN_PARSER_ASSOCIATION_MISSING";
    result.diagnostic_detail = "target_database_parser_association_missing";
  }
  if (parser_fallback_required && any_parser && !result.parser_fallback_available &&
      !result.parser_fallback_stale) {
    result.parser_fallback_missing = true;
    result.diagnostic_code = "ENGINE.SHUTDOWN_PARSER_ASSOCIATION_MISSING";
    result.diagnostic_detail = "target_database_parser_association_unavailable";
  }

  result.required_acknowledgement_count =
      result.associated_manager_count + result.associated_listener_count +
      result.associated_parser_count + result.associated_ipc_endpoint_count +
      result.associated_session_count;

  result.scope_proven = any_match && !result.ambiguous && !result.cluster_fail_closed &&
                        !result.stale && !result.parser_fallback_missing;
  if (!result.scope_proven && result.diagnostic_code.empty()) {
    result.diagnostic_code = "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS";
    result.diagnostic_detail = "association_scope_not_proven";
  }
  return result;
}

ProcessAssociationScopeResult ApplyProcessAssociationScopeToShutdownSnapshot(
    const ProcessAssociationRegistry& registry,
    const std::string& database_uuid,
    const std::string& database_path,
    std::uint64_t shutdown_generation,
    bool parser_fallback_required,
    ServerShutdownRuntimeSnapshot* snapshot) {
  const auto result = EvaluateProcessAssociationsForDatabase(
      registry,
      database_uuid,
      database_path,
      shutdown_generation,
      parser_fallback_required);
  if (snapshot == nullptr) return result;

  snapshot->associated_manager_count = result.associated_manager_count;
  snapshot->associated_listener_count = result.associated_listener_count;
  snapshot->associated_parser_count = result.associated_parser_count;
  snapshot->associated_ipc_endpoint_count = result.associated_ipc_endpoint_count;
  snapshot->associated_session_count = result.associated_session_count;
  snapshot->associated_client_count = result.associated_client_count;
  snapshot->active_transaction_session_count = result.active_transaction_session_count;
  snapshot->required_acknowledgement_count = result.required_acknowledgement_count;
  snapshot->association_scope_proven = result.scope_proven;
  snapshot->listener_unavailable = result.listener_unavailable;
  snapshot->parser_association_registry_available = result.parser_fallback_available;
  snapshot->parser_association_registry_stale = result.parser_fallback_stale;
  snapshot->association_diagnostic_code = result.diagnostic_code;
  snapshot->association_diagnostic_detail = result.diagnostic_detail;
  return result;
}

}  // namespace scratchbird::server
