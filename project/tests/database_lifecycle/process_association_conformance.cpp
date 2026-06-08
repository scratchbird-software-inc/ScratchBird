// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "maintenance_coordinator.hpp"
#include "process_association_registry.hpp"
#include "sbps.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace sbps = scratchbird::server::sbps;
using scratchbird::server::ApplyProcessAssociationScopeToShutdownSnapshot;
using scratchbird::server::ApplyServerMaintenanceOperation;
using scratchbird::server::BuildMaintenanceCoordinator;
using scratchbird::server::ProcessAssociationKind;
using scratchbird::server::ProcessAssociationRecord;
using scratchbird::server::ProcessAssociationRegistry;
using scratchbird::server::RegisterProcessAssociation;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerLifecycleArtifacts;
using scratchbird::server::ServerMaintenanceCoordinator;
using scratchbird::server::ServerMaintenanceOperationRequest;
using scratchbird::server::ServerShutdownRuntimeSnapshot;

constexpr std::string_view kDatabaseUuid = "019e1300-0000-7000-8000-000000000001";
constexpr std::string_view kDatabasePath = "/tmp/sb_dblc013d_process_association.sbdb";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasDiagnostic(const scratchbird::server::ServerMaintenanceOperationResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

ProcessAssociationRecord Base(ProcessAssociationKind kind, std::string_view component) {
  ProcessAssociationRecord record;
  record.kind = kind;
  record.database_uuid = std::string(kDatabaseUuid);
  record.database_path = std::string(kDatabasePath);
  record.component_uuid = std::string(component);
  record.process_uuid = std::string(component);
  record.lifecycle_generation = 13;
  record.association_generation = 13;
  record.heartbeat_generation = 13;
  record.policy_generation = 13;
  record.state = "associated";
  return record;
}

ProcessAssociationRegistry FreshRegistry() {
  ProcessAssociationRegistry registry;
  registry.generation = 13;

  auto manager = Base(ProcessAssociationKind::kManager, "manager-013d");
  manager.manager_uuid = manager.component_uuid;
  RegisterProcessAssociation(&registry, manager);

  auto listener = Base(ProcessAssociationKind::kListener, "listener-013d");
  listener.listener_uuid = listener.component_uuid;
  listener.state = "running";
  RegisterProcessAssociation(&registry, listener);

  auto parser = Base(ProcessAssociationKind::kParser, "parser-013d");
  parser.listener_uuid = "listener-013d";
  parser.parser_instance_uuid = parser.component_uuid;
  RegisterProcessAssociation(&registry, parser);

  auto ipc = Base(ProcessAssociationKind::kIpcEndpoint, "ipc-013d");
  ipc.ipc_endpoint = "ipc-013d";
  RegisterProcessAssociation(&registry, ipc);

  auto session = Base(ProcessAssociationKind::kSession, "session-013d");
  session.session_uuid = session.component_uuid;
  session.attachment_uuid = "attachment-013d";
  session.route_uuid = "route-013d";
  session.active_local_transaction_id = 9013;
  session.state = "transaction_active";
  RegisterProcessAssociation(&registry, session);

  return registry;
}

ServerShutdownRuntimeSnapshot Snapshot() {
  ServerShutdownRuntimeSnapshot snapshot;
  snapshot.database_uuid = std::string(kDatabaseUuid);
  snapshot.database_path = std::string(kDatabasePath);
  return snapshot;
}

void TestFreshRegistryScopesAllRuntimeFamilies() {
  auto registry = FreshRegistry();
  auto snapshot = Snapshot();
  const auto result = ApplyProcessAssociationScopeToShutdownSnapshot(
      registry,
      std::string(kDatabaseUuid),
      std::string(kDatabasePath),
      0,
      false,
      &snapshot);
  Require(result.scope_proven, "fresh process association registry did not prove scope");
  Require(snapshot.association_scope_proven, "snapshot scope not proven");
  Require(snapshot.associated_manager_count == 1, "manager association count mismatch");
  Require(snapshot.associated_listener_count == 1, "listener association count mismatch");
  Require(snapshot.associated_parser_count == 1, "parser association count mismatch");
  Require(snapshot.associated_ipc_endpoint_count == 1, "IPC association count mismatch");
  Require(snapshot.associated_session_count == 1, "session association count mismatch");
  Require(snapshot.associated_client_count == 1, "client association count mismatch");
  Require(snapshot.active_transaction_session_count == 1,
          "active transaction session count mismatch");
  Require(snapshot.required_acknowledgement_count == 5,
          "required acknowledgement count mismatch");
}

void TestListenerFailureParserFallbackUsesEngineVisibleRegistry() {
  auto registry = FreshRegistry();
  for (auto& record : registry.records) {
    if (record.kind == ProcessAssociationKind::kListener) {
      record.state = "failed";
      record.healthy = false;
    }
  }
  auto snapshot = Snapshot();
  const auto result = ApplyProcessAssociationScopeToShutdownSnapshot(
      registry,
      std::string(kDatabaseUuid),
      std::string(kDatabasePath),
      0,
      true,
      &snapshot);
  Require(result.scope_proven, "listener-failure parser fallback did not prove scope");
  Require(snapshot.listener_unavailable, "listener failure was not surfaced");
  Require(snapshot.parser_association_registry_available,
          "parser fallback association was not available");
}

void TestMissingAndStaleParserAssociationsFailClosed() {
  auto missing = FreshRegistry();
  missing.records.erase(
      std::remove_if(missing.records.begin(),
                     missing.records.end(),
                     [](const ProcessAssociationRecord& record) {
                       return record.kind == ProcessAssociationKind::kParser;
                     }),
      missing.records.end());
  auto missing_snapshot = Snapshot();
  const auto missing_result = ApplyProcessAssociationScopeToShutdownSnapshot(
      missing,
      std::string(kDatabaseUuid),
      std::string(kDatabasePath),
      0,
      true,
      &missing_snapshot);
  Require(!missing_result.scope_proven, "missing parser association was admitted");
  Require(missing_snapshot.association_diagnostic_code ==
              "ENGINE.SHUTDOWN_PARSER_ASSOCIATION_MISSING",
          "missing parser association diagnostic mismatch");

  auto stale = FreshRegistry();
  for (auto& record : stale.records) {
    if (record.kind == ProcessAssociationKind::kParser) record.state = "stale";
  }
  auto stale_snapshot = Snapshot();
  const auto stale_result = ApplyProcessAssociationScopeToShutdownSnapshot(
      stale,
      std::string(kDatabaseUuid),
      std::string(kDatabasePath),
      0,
      true,
      &stale_snapshot);
  Require(!stale_result.scope_proven, "stale parser association was admitted");
  Require(stale_snapshot.association_diagnostic_code ==
              "ENGINE.SHUTDOWN_PARSER_ASSOCIATION_STALE",
          "stale parser association diagnostic mismatch");
}

void TestCrossDatabaseAndStaleSessionAssociationsFailClosed() {
  auto registry = FreshRegistry();
  auto duplicate_route = Base(ProcessAssociationKind::kRoute, "route-013d-collision");
  duplicate_route.database_uuid = "019e1300-0000-7000-8000-000000000002";
  duplicate_route.database_path = "/tmp/sb_dblc013d_other.sbdb";
  duplicate_route.route_uuid = "route-013d";
  RegisterProcessAssociation(&registry, duplicate_route);

  auto snapshot = Snapshot();
  const auto collision = ApplyProcessAssociationScopeToShutdownSnapshot(
      registry,
      std::string(kDatabaseUuid),
      std::string(kDatabasePath),
      0,
      false,
      &snapshot);
  Require(!collision.scope_proven, "cross-database route collision was admitted");
  Require(snapshot.association_diagnostic_code == "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS",
          "cross-database collision diagnostic mismatch");

  auto stale_attachment = FreshRegistry();
  auto attachment = Base(ProcessAssociationKind::kAttachment, "attachment-013d");
  attachment.attachment_uuid = "attachment-013d";
  attachment.state = "stale";
  RegisterProcessAssociation(&stale_attachment, attachment);
  auto stale_snapshot = Snapshot();
  const auto stale_result = ApplyProcessAssociationScopeToShutdownSnapshot(
      stale_attachment,
      std::string(kDatabaseUuid),
      std::string(kDatabasePath),
      0,
      false,
      &stale_snapshot);
  Require(!stale_result.scope_proven, "stale attachment association was admitted");
  Require(stale_snapshot.association_diagnostic_code ==
              "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS",
          "stale attachment diagnostic mismatch");
}

void TestClusterRouteAndWrongAckGenerationFailClosed() {
  auto registry = FreshRegistry();
  auto cluster_route = Base(ProcessAssociationKind::kRoute, "cluster-route-013d");
  cluster_route.route_uuid = cluster_route.component_uuid;
  cluster_route.cluster_authority_required = true;
  cluster_route.cluster_authority_available = false;
  RegisterProcessAssociation(&registry, cluster_route);
  auto snapshot = Snapshot();
  const auto cluster_result = ApplyProcessAssociationScopeToShutdownSnapshot(
      registry,
      std::string(kDatabaseUuid),
      std::string(kDatabasePath),
      0,
      false,
      &snapshot);
  Require(!cluster_result.scope_proven, "cluster route was admitted without authority");
  Require(snapshot.association_diagnostic_code ==
              "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
          "cluster fail-closed diagnostic mismatch");

  ServerBootstrapConfig config;
  config.database_default_path = std::string(kDatabasePath);
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = 13;
  auto coordinator = BuildMaintenanceCoordinator(config, artifacts);
  coordinator.shutdown_generation = 42;

  ServerMaintenanceOperationRequest ack;
  ack.operation_key = "ack_database_shutdown";
  ack.mode =
      "acknowledger_kind:parser;"
      "acknowledger_uuid:parser-013d;"
      "acknowledgement_generation:41;"
      "acknowledgement_state:acknowledged";
  ack.request_uuid = sbps::MakeUuidV7Bytes();
  ack.session_uuid = sbps::MakeUuidV7Bytes();
  const auto refused = ApplyServerMaintenanceOperation(&coordinator, config, ack);
  Require(!refused.ok, "wrong-generation shutdown acknowledgement was admitted");
  Require(HasDiagnostic(refused, "ENGINE.SHUTDOWN_ACK_INVALID"),
          "wrong-generation acknowledgement diagnostic mismatch");
}

}  // namespace

int main() {
  TestFreshRegistryScopesAllRuntimeFamilies();
  TestListenerFailureParserFallbackUsesEngineVisibleRegistry();
  TestMissingAndStaleParserAssociationsFailClosed();
  TestCrossDatabaseAndStaleSessionAssociationsFailClosed();
  TestClusterRouteAndWrongAckGenerationFailClosed();
  return EXIT_SUCCESS;
}
