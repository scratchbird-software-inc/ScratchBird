// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_dependency_lifecycle.hpp"
#include "agent_runtime_manager.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

bool HasDiagnostic(const std::vector<std::string>& diagnostics,
                   std::string_view expected) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic == expected) { return true; }
  }
  return false;
}

agents::AgentDependencyLifecycleRequest BaseRequest(
    agents::AgentDependencyLifecycleMode mode =
        agents::AgentDependencyLifecycleMode::normal) {
  agents::AgentDependencyLifecycleRequest request;
  request.mode = mode;
  request.database_uuid = "018f1000-0000-7000-8000-000000078001";
  request.agent_type_id = "backup_manager";
  request.dependency_graph_digest = "sha256:agent-dependency-graph-ceic-078";
  request.lifecycle_evidence_uuid = "018f1000-0000-7000-8000-000000078002";
  request.generations.required_policy_generation = 8;
  request.generations.required_metric_generation = 9;
  request.generations.required_catalog_generation = 10;
  request.generations.required_security_generation = 11;
  request.generations.required_filespace_generation = 12;
  request.generations.required_agent_set_generation = 13;
  request.generations.observed_policy_generation = 18;
  request.generations.observed_metric_generation = 19;
  request.generations.observed_catalog_generation = 20;
  request.generations.observed_security_generation = 21;
  request.generations.observed_filespace_generation = 22;
  request.generations.observed_agent_set_generation = 23;
  return request;
}

agents::AgentRuntimeActivationEvidence RuntimeEvidence(
    agents::AgentLifecycleMode mode) {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = "018f1000-0000-7000-8000-000000078101";
  evidence.engine_instance_uuid = "018f1000-0000-7000-8000-000000078102";
  evidence.lifecycle_mode = mode;
  evidence.policy_generation = 8;
  evidence.catalog_generation = 10;
  evidence.security_generation = 11;
  evidence.filespace_generation = 12;
  evidence.agent_set_generation = 13;
  evidence.health_generation = 14;
  evidence.tx1_bootstrap_visible = true;
  evidence.tx2_activation_committed = true;
  evidence.startup_admitted = true;
  evidence.health_publication_allowed = true;
  evidence.health_publication_persisted = true;
  evidence.dependency_graph_consistent = true;
  return evidence;
}

void AddModeEvidence(agents::AgentDependencyLifecycleRequest* request) {
  switch (request->mode) {
    case agents::AgentDependencyLifecycleMode::normal:
    case agents::AgentDependencyLifecycleMode::read_only:
      return;
    case agents::AgentDependencyLifecycleMode::restricted_open:
    case agents::AgentDependencyLifecycleMode::maintenance:
      request->restricted_maintenance_evidence_uuid =
          "018f1000-0000-7000-8000-000000078010";
      return;
    case agents::AgentDependencyLifecycleMode::repair:
      request->restricted_maintenance_evidence_uuid =
          "018f1000-0000-7000-8000-000000078011";
      request->repair_plan_uuid =
          "018f1000-0000-7000-8000-000000078012";
      return;
    case agents::AgentDependencyLifecycleMode::backup:
      request->backup_manifest_uuid =
          "018f1000-0000-7000-8000-000000078013";
      return;
    case agents::AgentDependencyLifecycleMode::restore:
      request->backup_manifest_uuid =
          "018f1000-0000-7000-8000-000000078014";
      request->restore_plan_uuid =
          "018f1000-0000-7000-8000-000000078015";
      return;
    case agents::AgentDependencyLifecycleMode::archive_hold:
      request->archive_hold_evidence_uuid =
          "018f1000-0000-7000-8000-000000078016";
      return;
    case agents::AgentDependencyLifecycleMode::shutdown:
      request->shutdown_drain_evidence_uuid =
          "018f1000-0000-7000-8000-000000078017";
      return;
    case agents::AgentDependencyLifecycleMode::crash_recovery:
      request->crash_recovery_evidence_uuid =
          "018f1000-0000-7000-8000-000000078018";
      return;
    case agents::AgentDependencyLifecycleMode::pitr:
      request->restore_plan_uuid =
          "018f1000-0000-7000-8000-000000078019";
      request->pitr_recovery_point_uuid =
          "018f1000-0000-7000-8000-000000078020";
      return;
    case agents::AgentDependencyLifecycleMode::clone:
      request->clone_manifest_uuid =
          "018f1000-0000-7000-8000-000000078021";
      request->clone_source_database_uuid =
          "018f1000-0000-7000-8000-000000078022";
      request->clone_target_database_uuid =
          "018f1000-0000-7000-8000-000000078023";
      return;
    case agents::AgentDependencyLifecycleMode::role_change:
      request->role_change_evidence_uuid =
          "018f1000-0000-7000-8000-000000078024";
      request->target_role = "single_node_primary";
      return;
  }
}

agents::AgentDependencyLifecycleDecision RequireOk(
    agents::AgentDependencyLifecycleMode mode,
    const std::string& label) {
  auto request = BaseRequest(mode);
  AddModeEvidence(&request);
  auto decision = agents::EvaluateAgentDependencyLifecycle(request);
  Require(decision.status.ok,
          label + " failed: " + decision.status.diagnostic_code + " " +
              decision.status.detail);
  Require(decision.generation_window_valid,
          label + " did not validate generation window");
  Require(decision.dependency_graph_valid,
          label + " did not validate dependency graph");
  Require(decision.evidence_non_authoritative,
          label + " evidence was not marked non-authoritative");
  Require(!decision.evidence_authority_boundary.transaction_finality_authority &&
              !decision.evidence_authority_boundary.visibility_authority &&
              !decision.evidence_authority_boundary.recovery_authority &&
              !decision.evidence_authority_boundary.security_authority &&
              !decision.evidence_authority_boundary.parser_authority &&
              !decision.evidence_authority_boundary.reference_authority &&
              !decision.evidence_authority_boundary.write_ahead_log_authority &&
              !decision.evidence_authority_boundary.benchmark_authority &&
              !decision.evidence_authority_boundary.optimizer_plan_authority &&
              !decision.evidence_authority_boundary.index_finality_authority &&
              !decision.evidence_authority_boundary.provider_finality_authority &&
              !decision.evidence_authority_boundary.cluster_authority &&
              !decision.evidence_authority_boundary.memory_authority &&
              !decision.evidence_authority_boundary.agent_action_authority,
          label + " leaked authority boundary");
  return decision;
}

void RequireCode(const agents::AgentDependencyLifecycleRequest& request,
                 std::string_view expected_code,
                 const std::string& label) {
  const auto decision = agents::EvaluateAgentDependencyLifecycle(request);
  Require(!decision.status.ok, label + " unexpectedly passed");
  Require(decision.status.diagnostic_code == expected_code,
          label + " expected " + std::string(expected_code) + " got " +
              decision.status.diagnostic_code + " " + decision.status.detail);
}

void TestEveryLifecycleMode() {
  const auto normal = RequireOk(agents::AgentDependencyLifecycleMode::normal,
                                "normal");
  Require(normal.maximum_activation == agents::AgentActivationProfile::live_action,
          "normal did not admit live maximum activation");
  Require(normal.ordinary_work_admitted && normal.mutable_action_admitted,
          "normal did not admit ordinary mutable work");

  const auto read_only =
      RequireOk(agents::AgentDependencyLifecycleMode::read_only, "read_only");
  Require(read_only.inspect_only && !read_only.mutable_action_admitted,
          "read_only was not inspect-only");

  const auto restricted = RequireOk(
      agents::AgentDependencyLifecycleMode::restricted_open,
      "restricted_open");
  Require(restricted.safe_maintenance_only,
          "restricted_open was not safe-maintenance-only");

  const auto maintenance = RequireOk(
      agents::AgentDependencyLifecycleMode::maintenance,
      "maintenance");
  Require(maintenance.safe_maintenance_only,
          "maintenance was not safe-maintenance-only");

  const auto repair =
      RequireOk(agents::AgentDependencyLifecycleMode::repair, "repair");
  Require(repair.safe_maintenance_only,
          "repair was not safe-maintenance-only");

  const auto backup =
      RequireOk(agents::AgentDependencyLifecycleMode::backup, "backup");
  Require(backup.inspect_only, "backup was not inspect-only");

  const auto restore =
      RequireOk(agents::AgentDependencyLifecycleMode::restore, "restore");
  Require(restore.restore_or_clone_mode && restore.inspect_only,
          "restore did not enter DR inspect-only mode");

  const auto archive =
      RequireOk(agents::AgentDependencyLifecycleMode::archive_hold,
                "archive_hold");
  Require(archive.inspect_only, "archive_hold was not inspect-only");

  const auto shutdown =
      RequireOk(agents::AgentDependencyLifecycleMode::shutdown, "shutdown");
  Require(shutdown.drain_only &&
              shutdown.maximum_activation == agents::AgentActivationProfile::disabled,
          "shutdown was not drain-only disabled");

  const auto recovery = RequireOk(
      agents::AgentDependencyLifecycleMode::crash_recovery,
      "crash_recovery");
  Require(recovery.recovery_only &&
              recovery.maximum_activation == agents::AgentActivationProfile::disabled,
          "crash_recovery was not recovery-only disabled");

  const auto pitr = RequireOk(agents::AgentDependencyLifecycleMode::pitr,
                              "pitr");
  Require(pitr.restore_or_clone_mode && pitr.inspect_only,
          "pitr did not enter DR inspect-only mode");

  const auto clone =
      RequireOk(agents::AgentDependencyLifecycleMode::clone, "clone");
  Require(clone.restore_or_clone_mode && clone.inspect_only,
          "clone did not enter DR inspect-only mode");

  const auto role_change = RequireOk(
      agents::AgentDependencyLifecycleMode::role_change,
      "role_change");
  Require(role_change.inspect_only,
          "role_change was not inspect-only without provider proof");
}

void TestModeEvidenceRefusals() {
  RequireCode(BaseRequest(agents::AgentDependencyLifecycleMode::restore),
              "SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
              "restore missing evidence");
  RequireCode(BaseRequest(agents::AgentDependencyLifecycleMode::clone),
              "SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
              "clone missing evidence");
  RequireCode(BaseRequest(agents::AgentDependencyLifecycleMode::pitr),
              "SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
              "pitr missing evidence");

  auto clone = BaseRequest(agents::AgentDependencyLifecycleMode::clone);
  AddModeEvidence(&clone);
  clone.clone_target_database_uuid = clone.clone_source_database_uuid;
  RequireCode(clone,
              "SB_AGENT_DEP_LIFECYCLE.CLONE_IDENTITY_INVALID",
              "clone same source and target");
}

void TestGenerationAndDependencyRefusals() {
  auto metric_gap = BaseRequest();
  metric_gap.generations.observed_metric_generation = 0;
  RequireCode(metric_gap,
              "SB_AGENT_DEP_LIFECYCLE.GENERATION_GAP",
              "missing metric generation");

  auto catalog_gap = BaseRequest();
  catalog_gap.generations.observed_catalog_generation =
      catalog_gap.generations.required_catalog_generation - 1;
  RequireCode(catalog_gap,
              "SB_AGENT_DEP_LIFECYCLE.GENERATION_GAP",
              "stale catalog generation");

  auto graph = BaseRequest();
  graph.dependency_graph_consistent = false;
  RequireCode(graph,
              "SB_AGENT_DEP_LIFECYCLE.DEPENDENCY_GRAPH_INVALID",
              "dependency graph invalid");

  auto identity = BaseRequest();
  identity.dependency_graph_digest.clear();
  RequireCode(identity,
              "SB_AGENT_DEP_LIFECYCLE.IDENTITY_REQUIRED",
              "missing dependency graph digest");
}

void TestAuthorityRefusals() {
  auto request = BaseRequest();
  request.authority_boundary.transaction_finality_authority = true;
  request.authority_boundary.visibility_authority = true;
  request.authority_boundary.recovery_authority = true;
  request.authority_boundary.security_authority = true;
  request.authority_boundary.parser_authority = true;
  request.authority_boundary.reference_authority = true;
  request.authority_boundary.write_ahead_log_authority = true;
  request.authority_boundary.benchmark_authority = true;
  request.authority_boundary.optimizer_plan_authority = true;
  request.authority_boundary.index_finality_authority = true;
  request.authority_boundary.provider_finality_authority = true;
  request.authority_boundary.cluster_authority = true;
  request.authority_boundary.memory_authority = true;
  request.authority_boundary.agent_action_authority = true;
  RequireCode(request,
              "SB_AGENT_DEP_LIFECYCLE.AUTHORITY_DENIED",
              "forbidden authority boundary");
}

void TestClusterExternalProviderBoundary() {
  auto local_cluster =
      BaseRequest(agents::AgentDependencyLifecycleMode::role_change);
  AddModeEvidence(&local_cluster);
  local_cluster.local_cluster_path_requested = true;
  local_cluster.target_role = "cluster_primary";
  RequireCode(local_cluster,
              "SB_AGENT_DEP_LIFECYCLE.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
              "local cluster role change");

  auto external = local_cluster;
  external.external_cluster_provider_proof_present = true;
  external.external_cluster_provider_id = "external_cluster_provider";
  external.external_cluster_provider_evidence_uuid =
      "018f1000-0000-7000-8000-000000078090";
  const auto decision = agents::EvaluateAgentDependencyLifecycle(external);
  Require(decision.status.ok,
          "external cluster provider proof was refused: " +
              decision.status.diagnostic_code);
  Require(decision.external_provider_only,
          "external provider role change was not provider-only");
  Require(HasDiagnostic(decision.diagnostics,
                        "SB_AGENT_DEP_LIFECYCLE.EXTERNAL_CLUSTER_PROVIDER_ONLY"),
          "external provider diagnostic missing");
}

void TestRuntimeModeMapping() {
  Require(agents::AgentDependencyLifecycleModeFromRuntime(
              agents::AgentLifecycleMode::pitr) ==
              agents::AgentDependencyLifecycleMode::pitr,
          "runtime pitr mapping failed");
  Require(agents::AgentDependencyLifecycleModeFromRuntime(
              agents::AgentLifecycleMode::clone) ==
              agents::AgentDependencyLifecycleMode::clone,
          "runtime clone mapping failed");
  Require(agents::AgentDependencyLifecycleModeFromRuntime(
              agents::AgentLifecycleMode::role_change) ==
              agents::AgentDependencyLifecycleMode::role_change,
          "runtime role_change mapping failed");
  Require(std::string(agents::AgentLifecycleModeName(
              agents::AgentLifecycleMode::pitr)) == "pitr",
          "pitr lifecycle mode name missing");
  Require(std::string(agents::AgentLifecycleModeName(
              agents::AgentLifecycleMode::clone)) == "clone",
          "clone lifecycle mode name missing");
  Require(std::string(agents::AgentLifecycleModeName(
              agents::AgentLifecycleMode::role_change)) == "role_change",
          "role_change lifecycle mode name missing");
}

void TestRuntimeManagerConsumesDependencyLifecycleRequest() {
  agents::DatabaseLocalAgentRuntimeManager manager;
  agents::AgentRuntimeManagerConfig config;
  config.standalone_edition = true;
  config.cluster_authority_available = false;
  config.enforce_dependency_lifecycle = true;
  config.dependency_lifecycle_request =
      BaseRequest(agents::AgentDependencyLifecycleMode::restore);

  auto refused = manager.Start(
      RuntimeEvidence(agents::AgentLifecycleMode::restore), config);
  Require(!refused.status.ok,
          "runtime manager accepted restore without dependency lifecycle proof");
  Require(refused.status.diagnostic_code ==
              "ENGINE.AGENT_RUNTIME_MANAGER_DEPENDENCY_LIFECYCLE_REFUSED",
          "runtime manager did not surface dependency lifecycle refusal");

  AddModeEvidence(&config.dependency_lifecycle_request);
  auto admitted = manager.Start(
      RuntimeEvidence(agents::AgentLifecycleMode::restore), config);
  Require(admitted.status.ok,
          "runtime manager rejected restore with dependency lifecycle proof");
  Require(admitted.state == agents::AgentRuntimeManagerState::safe_mode,
          "restore dependency lifecycle did not enter safe inspect state");
  Require(!admitted.ordinary_admission_allowed,
          "restore dependency lifecycle admitted ordinary work");
  Require(HasDiagnostic(
              admitted.diagnostics,
              "SB_AGENT_DEP_LIFECYCLE.DR_RESTORE_CLONE_INSPECT_ONLY"),
          "runtime manager did not retain dependency lifecycle diagnostic");
}

}  // namespace

int main() {
  TestEveryLifecycleMode();
  TestModeEvidenceRefusals();
  TestGenerationAndDependencyRefusals();
  TestAuthorityRefusals();
  TestClusterExternalProviderBoundary();
  TestRuntimeModeMapping();
  TestRuntimeManagerConsumesDependencyLifecycleRequest();
  return EXIT_SUCCESS;
}
