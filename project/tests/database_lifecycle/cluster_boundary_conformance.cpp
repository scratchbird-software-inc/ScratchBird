// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_engine_lifecycle.hpp"
#include "cluster/cluster_control_api.hpp"
#include "cluster/cluster_insert_route_api.hpp"
#include "cluster/cluster_inspect_api.hpp"
#include "cluster/placement_api.hpp"
#include "cluster/remote_participant_insert_api.hpp"
#include "cluster/replication_api.hpp"
#include "index_transaction.hpp"
#include "metric_registry.hpp"
#include "sblr_admission.hpp"
#include "cluster_transaction_fail_closed.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace index = scratchbird::core::index;
namespace metrics = scratchbird::core::metrics;
namespace mga = scratchbird::transaction::mga;
namespace server = scratchbird::server;

using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

TypedUuid TestTypedUuid(UuidKind kind) {
  TypedUuid uuid;
  uuid.kind = kind;
  uuid.value.bytes[0] = 0x01;
  uuid.value.bytes[6] = 0x70;
  uuid.value.bytes[8] = 0x80;
  return uuid;
}

api::EngineUuid TestEngineUuid(std::string_view suffix) {
  api::EngineUuid uuid;
  uuid.canonical = "019e0f70-0000-7000-8000-" + std::string(suffix);
  return uuid;
}

api::EngineObjectReference TestObject(std::string_view suffix,
                                      std::string_view kind = "object") {
  api::EngineObjectReference object;
  object.uuid = TestEngineUuid(suffix);
  object.object_kind = std::string(kind);
  return object;
}

api::EngineRequestContext StandaloneContext() {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.database_uuid = TestEngineUuid("000000000013");
  context.transaction_uuid = TestEngineUuid("000000000014");
  context.local_transaction_id = 0;
  context.security_context_present = true;
  context.cluster_authority_available = false;
  return context;
}

api::EngineRequestContext ClusterAuthorityContext() {
  api::EngineRequestContext context = StandaloneContext();
  context.cluster_authority_available = true;
  context.local_transaction_id = 77;
  context.transaction_uuid = TestEngineUuid("000000000077");
  return context;
}

api::EngineClusterInsertRouteFenceRequest ValidRouteFenceRequest() {
  api::EngineClusterInsertRouteFenceRequest route;
  route.context = ClusterAuthorityContext();
  route.target_table = TestObject("000000000015", "table");
  route.target_shard = TestObject("000000000016", "shard");
  route.target_range = TestObject("000000000017", "range");
  route.owner_node_uuid = TestEngineUuid("000000000018");
  route.participant_node_uuid = TestEngineUuid("000000000019");
  route.route_epoch_uuid = TestEngineUuid("000000000020");
  route.participant_uuid = TestEngineUuid("000000000021");
  route.policy_snapshot_uuid = TestEngineUuid("000000000022");
  route.finality_service_uuid = TestEngineUuid("000000000023");
  route.route_epoch = 9;
  route.route_generation = 2;
  route.idempotency_key = "cluster-boundary-provider-route";
  route.remote_participant_requested = true;
  return route;
}

api::EngineRemoteParticipantInsertRequest ValidRemoteParticipantRequest() {
  api::EngineRemoteParticipantInsertRequest participant;
  participant.context = ClusterAuthorityContext();
  participant.remote_request_uuid = TestEngineUuid("000000000024");
  participant.target_database = TestObject("000000000025", "database");
  participant.target_table = TestObject("000000000026", "table");
  participant.target_shard = TestObject("000000000027", "shard");
  participant.target_range = TestObject("000000000028", "range");
  participant.owner_node_uuid = TestEngineUuid("000000000029");
  participant.participant_node_uuid = TestEngineUuid("000000000030");
  participant.route_epoch_uuid = TestEngineUuid("000000000031");
  participant.participant_uuid = TestEngineUuid("000000000032");
  participant.policy_snapshot_uuid = TestEngineUuid("000000000033");
  participant.finality_service_uuid = TestEngineUuid("000000000034");
  participant.route_epoch = 9;
  participant.route_generation = 2;
  participant.idempotency_key = "cluster-boundary-provider-participant";
  participant.route_fence_validated = true;
  participant.participant_admission_durable = true;
  participant.finality_proof_available = true;
  participant.canonical_rows.push_back({});
  return participant;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
    if (diagnostic.detail == code) return true;
    if (diagnostic.detail.size() > code.size() &&
        diagnostic.detail.compare(diagnostic.detail.size() - code.size(), code.size(), code) == 0) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind, std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

bool HasEvidenceKind(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return true;
  }
  return false;
}

bool HasProviderBoundaryRefusal(const api::EngineApiResult& result) {
  return !result.ok &&
         result.cluster_authority_required &&
         HasEvidence(result, "cluster_provider_boundary", "provider_invoked") &&
         HasEvidence(result, "cluster_provider_boundary_result", "failed_closed") &&
         HasEvidenceKind(result, "cluster_provider_type") &&
         HasEvidenceKind(result, "cluster_provider_route_admission_diagnostic");
}

bool HasClusterMetricSamples() {
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(true)) {
    if (value.family.rfind("sb_cluster_", 0) == 0) return true;
  }
  return false;
}

void TestStandaloneClusterApiFailsBeforeRouteDetails() {
  api::EngineClusterInsertRouteFenceRequest route;
  route.context = StandaloneContext();
  auto route_result = api::EngineValidateClusterInsertRouteFence(route);
  Require(!route_result.ok, "standalone cluster route fence was admitted");
  Require(route_result.cluster_authority_required,
          "standalone cluster route did not require cluster authority");
  Require(route_result.refusal_reason == "cluster_authority_unavailable",
          "standalone route checked local transaction before cluster authority");
  Require(HasDiagnostic(route_result, "cluster_authority_unavailable"),
          "standalone route missing cluster authority diagnostic");
  Require(HasEvidence(route_result, "standalone_cluster_boundary", "cluster_metric_path_skipped"),
          "standalone route did not prove metric path was skipped");
  Require(HasEvidence(route_result, "standalone_cluster_boundary", "cluster_route_not_entered"),
          "standalone route did not prove route path was not entered");

  api::EngineRemoteParticipantInsertRequest participant;
  participant.context = StandaloneContext();
  auto participant_result = api::EnginePrepareRemoteParticipantInsert(participant);
  Require(!participant_result.ok, "standalone remote participant path was admitted");
  Require(participant_result.cluster_authority_required,
          "standalone remote participant did not require cluster authority");
  Require(participant_result.refusal_reason == "cluster_authority_unavailable",
          "standalone remote participant checked route details before cluster authority");
  Require(HasEvidence(participant_result, "standalone_cluster_boundary", "cluster_metric_path_skipped"),
          "standalone participant did not prove metric path was skipped");
  Require(HasEvidence(participant_result, "standalone_cluster_boundary", "remote_participant_path_not_entered"),
          "standalone participant did not prove participant path was not entered");

  Require(!HasClusterMetricSamples(),
          "standalone cluster boundary emitted cluster metric samples");
}

void TestClusterAuthorityRoutesReachCompileTimeProviderBoundary() {
  api::EngineControlClusterRequest control;
  control.context = ClusterAuthorityContext();
  auto control_result = api::EngineControlCluster(control);
  Require(HasProviderBoundaryRefusal(control_result),
          "cluster control did not route through compile-time provider boundary");

  api::EngineInspectClusterStateRequest inspect_state;
  inspect_state.context = ClusterAuthorityContext();
  auto state_result = api::EngineInspectClusterState(inspect_state);
  Require(HasProviderBoundaryRefusal(state_result),
          "cluster state inspect did not route through compile-time provider boundary");

  api::EngineInspectClusterRoutingPlanRequest inspect_route;
  inspect_route.context = ClusterAuthorityContext();
  auto routing_result = api::EngineInspectClusterRoutingPlan(inspect_route);
  Require(HasProviderBoundaryRefusal(routing_result),
          "cluster routing inspect did not route through compile-time provider boundary");

  api::EnginePlaceClusterObjectRequest placement;
  placement.context = ClusterAuthorityContext();
  auto placement_result = api::EnginePlaceClusterObject(placement);
  Require(HasProviderBoundaryRefusal(placement_result),
          "cluster placement did not route through compile-time provider boundary");

  auto route_result =
      api::EngineValidateClusterInsertRouteFence(ValidRouteFenceRequest());
  Require(HasProviderBoundaryRefusal(route_result),
          "cluster route fence did not route through compile-time provider boundary");
  Require(route_result.refusal_reason == "cluster_provider_boundary_closed",
          "cluster route fence did not preserve provider-boundary refusal reason");
  Require(HasEvidence(route_result, "cluster_insert_route_not_entered",
                      "provider_boundary_failed_closed"),
          "cluster route fence entered local route path after provider refusal");

  auto participant_result =
      api::EnginePrepareRemoteParticipantInsert(ValidRemoteParticipantRequest());
  Require(HasProviderBoundaryRefusal(participant_result),
          "remote participant insert did not route through compile-time provider boundary");
  Require(participant_result.refusal_reason == "cluster_provider_boundary_closed",
          "remote participant insert did not preserve provider-boundary refusal reason");
  Require(HasEvidence(participant_result, "remote_participant_path_not_entered",
                      "provider_boundary_failed_closed"),
          "remote participant path entered local route path after provider refusal");

  api::EngineInspectReplicationRequest replication;
  replication.context = ClusterAuthorityContext();
  auto replication_result = api::EngineInspectReplication(replication);
  Require(HasProviderBoundaryRefusal(replication_result),
          "cluster replication inspect did not route through compile-time provider boundary");
  Require(replication_result.standalone_fail_closed,
          "cluster replication inspect did not fail closed on provider refusal");
}

void TestClusterInspectionControlAndReplicationFailClosed() {
  api::EngineControlClusterRequest control;
  control.context = StandaloneContext();
  auto control_result = api::EngineControlCluster(control);
  Require(HasProviderBoundaryRefusal(control_result),
          "cluster control did not fail closed at provider boundary");

  api::EngineInspectClusterStateRequest inspect_state;
  inspect_state.context = StandaloneContext();
  auto state_result = api::EngineInspectClusterState(inspect_state);
  Require(HasProviderBoundaryRefusal(state_result),
          "cluster state inspect did not fail closed at provider boundary");

  api::EngineInspectClusterRoutingPlanRequest inspect_route;
  inspect_route.context = StandaloneContext();
  auto route_result = api::EngineInspectClusterRoutingPlan(inspect_route);
  Require(HasProviderBoundaryRefusal(route_result),
          "cluster routing inspect did not fail closed at provider boundary");

  api::EngineInspectReplicationRequest replication;
  replication.context = StandaloneContext();
  auto replication_result = api::EngineInspectReplication(replication);
  Require(!replication_result.ok && replication_result.cluster_authority_required,
          "cluster replication inspect did not fail closed without authority");
}

api::EngineShardPlacementDescriptor ShardPlacementDescriptor() {
  api::EngineShardPlacementDescriptor descriptor;
  descriptor.shard_uuid = TestEngineUuid("000000000041").canonical;
  descriptor.source_filespace_uuid = TestEngineUuid("000000000042").canonical;
  descriptor.target_filespace_uuid = TestEngineUuid("000000000043").canonical;
  descriptor.range_begin = "00000000";
  descriptor.range_end = "ffffffff";
  descriptor.placement_epoch = 9;
  descriptor.placement_generation = 2;
  return descriptor;
}

void TestShardPlacementDescriptorWorkflow() {
  const std::string operations[] = {
      "create", "verify", "move", "split", "merge", "rebalance",
      "freeze", "archive", "reattach", "quarantine", "reconcile", "drop"};
  for (const auto& operation : operations) {
    api::EngineShardPlacementOperationRequest request;
    request.context = operation == "verify" ? StandaloneContext() : ClusterAuthorityContext();
    request.placement_operation = operation;
    request.descriptor = ShardPlacementDescriptor();
    request.operator_authorized = true;
    if (operation == "merge") {
      auto left = ShardPlacementDescriptor();
      left.shard_uuid = TestEngineUuid("000000000044").canonical;
      auto right = ShardPlacementDescriptor();
      right.shard_uuid = TestEngineUuid("000000000045").canonical;
      request.merge_inputs = {left, right};
    }

    const auto planned = api::EnginePlanShardPlacementOperation(request);
    Require(planned.ok, "shard placement descriptor operation failed");
    Require(planned.descriptor_validated, "shard placement descriptor was not validated");
    Require(!planned.durable_state_changed,
            "shard placement descriptor planner mutated durable state");
    Require(!planned.private_cluster_execution && !planned.cluster_provider_dispatch,
            "shard placement descriptor planner entered private cluster execution");
    Require(HasEvidence(planned, "mga_visibility_authority", "durable_transaction_inventory"),
            "shard placement descriptor planner lost MGA authority evidence");
    if (operation == "verify") {
      Require(planned.operation_verified && !planned.operator_review_required,
              "verify shard placement did not remain descriptor-only");
    } else {
      Require(planned.operation_planned && planned.operator_review_required,
              "mutating shard placement operation did not require operator review");
    }
    if (operation == "move" || operation == "split" || operation == "merge" ||
        operation == "rebalance" || operation == "reattach") {
      Require(planned.physical_data_movement_required,
              "physical shard placement operation did not expose movement requirement");
    }
  }

  api::EngineShardPlacementOperationRequest physical;
  physical.context = ClusterAuthorityContext();
  physical.placement_operation = "move";
  physical.descriptor = ShardPlacementDescriptor();
  physical.physical_data_movement_requested = true;
  const auto physical_refused = api::EnginePlanShardPlacementOperation(physical);
  Require(!physical_refused.ok &&
              HasDiagnostic(physical_refused,
                            "physical_data_movement_not_available_in_open_core_descriptor_plan"),
          "shard placement physical movement was not refused");

  api::EngineShardPlacementOperationRequest missing_tx;
  missing_tx.context = StandaloneContext();
  missing_tx.placement_operation = "archive";
  missing_tx.descriptor = ShardPlacementDescriptor();
  const auto missing_tx_refused = api::EnginePlanShardPlacementOperation(missing_tx);
  Require(!missing_tx_refused.ok && HasDiagnostic(missing_tx_refused, "local_transaction_id_required"),
          "shard placement mutation did not require local transaction context");
}

void TestMGAAndIndexClusterTransactionsFailClosed() {
  mga::ClusterTransactionMetadata metadata;
  metadata.transaction_uuid = TestTypedUuid(UuidKind::transaction);
  metadata.cluster_authority_active = false;
  auto begin = mga::BeginClusterTransactionFailClosed(metadata);
  Require(!begin.ok(), "cluster transaction begin succeeded in standalone mode");
  Require(begin.status.code == StatusCode::platform_required_feature_missing,
          "cluster transaction begin diagnostic status mismatch");
  Require(begin.status.subsystem == Subsystem::cluster_private,
          "cluster transaction begin used non-cluster subsystem");

  auto commit = mga::CommitClusterTransactionFailClosed(metadata);
  Require(!commit.ok(), "cluster transaction commit succeeded in standalone mode");

  index::IndexTransactionalRequest plan_request;
  plan_request.index_uuid = TestTypedUuid(UuidKind::object);
  plan_request.family = index::IndexFamily::hash;
  plan_request.operation = index::IndexTransactionalOperation::insert_entry;
  plan_request.local_transaction_id = 10;
  plan_request.cluster_authority_active = true;
  const auto cluster_active = index::PlanIndexTransactionalOperation(plan_request);
  Require(!cluster_active.ok(), "index transaction admitted cluster-authority path");
  Require(!cluster_active.admitted && !cluster_active.cluster_todo_only,
          "index transaction kept a cluster todo path active");
  Require(cluster_active.diagnostic.diagnostic_code == "SB-INDEX-TX-CLUSTER-MAPPING-UNAVAILABLE",
          "index cluster-authority refusal diagnostic mismatch");

  plan_request.cluster_authority_active = false;
  plan_request.operation = index::IndexTransactionalOperation::cluster_prepare_todo;
  const auto cluster_prepare = index::PlanIndexTransactionalOperation(plan_request);
  Require(!cluster_prepare.ok(), "index transaction admitted reserved cluster prepare");
  Require(!cluster_prepare.cluster_todo_only,
          "index transaction left cluster prepare todo path active");
}

void TestSblrAndAgentClusterBoundary() {
  server::ServerSblrAdmissionRequest cluster_authority_request;
  cluster_authority_request.cluster_authority_active = true;
  cluster_authority_request.encoded_sblr_envelope =
      "operation_id=ddl.create_table\n"
      "result_shape=ddl_result\n"
      "diagnostic_shape=diagnostic_vector\n"
      "parser_resolved_names_to_uuids=true\n";
  auto authority_result = server::AdmitServerSblrEnvelope(cluster_authority_request);
  Require(authority_result.admitted && authority_result.requires_public_abi_dispatch,
          "SBLR admission rejected a non-cluster operation when cluster authority was active");
  Require(authority_result.operation_family == "sblr.catalog.mutation.v3" &&
              authority_result.operation_id == "ddl.create_table",
          "SBLR admission remapped a non-cluster operation into the cluster provider path");

  server::ServerSblrAdmissionRequest requires_cluster;
  requires_cluster.cluster_authority_active = false;
  requires_cluster.encoded_sblr_envelope =
      "operation_id=cluster.inspect_state\n"
      "result_shape=cluster_result\n"
      "diagnostic_shape=diagnostic_vector\n"
      "parser_resolved_names_to_uuids=true\n"
      "requires_cluster_authority=true\n";
  auto required_result = server::AdmitServerSblrEnvelope(requires_cluster);
  Require(required_result.admitted && required_result.requires_public_abi_dispatch,
          "SBLR admission did not route a cluster-required operation to the provider boundary");
  Require(required_result.operation_family == "sblr.cluster.report.v3" &&
              required_result.operation_id == "cluster.inspect_state",
          "SBLR cluster-required operation admission did not preserve the provider-boundary route");

  agents::DatabaseEngineAgentInput input;
  input.database_uuid = "019e0f70-0000-7000-8000-000000000013";
  input.engine_instance_uuid = "engine-instance:019e0f70";
  input.policy_generation = 1;
  input.catalog_generation = 1;
  input.security_generation = 1;
  input.filespace_generation = 1;
  input.agent_set_generation = 1;
  input.health_generation = 1;
  input.tx1_bootstrap_visible = true;
  input.tx2_activation_committed = true;
  input.startup_admitted = true;
  input.cluster_authority_available = false;
  const auto agent = agents::StartDatabaseEngineLifecycleAgent(input);
  Require(agent.ok(), "database engine lifecycle agent failed in standalone mode");
  Require(agent.health.cluster_paths_failed_closed,
          "database engine lifecycle agent did not publish cluster fail-closed state");
  for (const auto& type_id : agent.health.selected_agent_type_ids) {
    Require(type_id.find("cluster") == std::string::npos,
            "database engine lifecycle agent selected a cluster agent in standalone mode");
  }
  const auto health_json = agents::SerializeDatabaseEngineAgentHealthJson(agent.health, true);
  Require(health_json.find("\"cluster_paths_failed_closed\":true") != std::string::npos,
          "agent health JSON omitted cluster fail-closed publication");
}

void TestClusterMetricDescriptorsHiddenFromStandaloneViews() {
  for (const auto& descriptor : metrics::DefaultMetricRegistry().Descriptors(false)) {
    Require(!descriptor.cluster_only,
            "standalone metric descriptor view exposed cluster-only descriptor");
    Require(descriptor.namespace_path.rfind("cluster.sys.metrics", 0) != 0,
            "standalone metric descriptor view exposed cluster namespace");
  }
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
    Require(value.family.rfind("sb_cluster_", 0) != 0,
            "standalone current metric view exposed cluster sample");
  }
}

}  // namespace

int main() {
  TestStandaloneClusterApiFailsBeforeRouteDetails();
  TestClusterAuthorityRoutesReachCompileTimeProviderBoundary();
  TestClusterInspectionControlAndReplicationFailClosed();
  TestShardPlacementDescriptorWorkflow();
  TestMGAAndIndexClusterTransactionsFailClosed();
  TestSblrAndAgentClusterBoundary();
  TestClusterMetricDescriptorsHiddenFromStandaloneViews();
  return EXIT_SUCCESS;
}
