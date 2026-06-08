// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_dependency_lifecycle.hpp"

#include <utility>

namespace scratchbird::core::agents {
namespace {

void AddDiagnostic(AgentDependencyLifecycleDecision* decision,
                   std::string diagnostic) {
  if (decision != nullptr) {
    decision->diagnostics.push_back(std::move(diagnostic));
  }
}

AgentDependencyLifecycleDecision Refuse(
    const AgentDependencyLifecycleRequest& request,
    std::string code,
    std::string detail = {}) {
  AgentDependencyLifecycleDecision decision;
  decision.mode = request.mode;
  decision.status = AgentError(std::move(code), std::move(detail));
  decision.cluster_path_failed_closed = true;
  AddDiagnostic(&decision, decision.status.diagnostic_code);
  return decision;
}

bool MissingIdentity(const AgentDependencyLifecycleRequest& request) {
  return request.database_uuid.empty() ||
         request.agent_type_id.empty() ||
         request.lifecycle_evidence_uuid.empty() ||
         request.dependency_graph_digest.empty();
}

bool GenerationMissingOrBehind(u64 observed, u64 required) {
  return required == 0 || observed == 0 || observed < required;
}

AgentRuntimeStatus ValidateGenerationWindow(
    const AgentDependencyLifecycleGenerationWindow& generations) {
  if (GenerationMissingOrBehind(generations.observed_policy_generation,
                                generations.required_policy_generation)) {
    return AgentError("SB_AGENT_DEP_LIFECYCLE.GENERATION_GAP",
                      "policy_generation");
  }
  if (GenerationMissingOrBehind(generations.observed_metric_generation,
                                generations.required_metric_generation)) {
    return AgentError("SB_AGENT_DEP_LIFECYCLE.GENERATION_GAP",
                      "metric_generation");
  }
  if (GenerationMissingOrBehind(generations.observed_catalog_generation,
                                generations.required_catalog_generation)) {
    return AgentError("SB_AGENT_DEP_LIFECYCLE.GENERATION_GAP",
                      "catalog_generation");
  }
  if (GenerationMissingOrBehind(generations.observed_security_generation,
                                generations.required_security_generation)) {
    return AgentError("SB_AGENT_DEP_LIFECYCLE.GENERATION_GAP",
                      "security_generation");
  }
  if (GenerationMissingOrBehind(generations.observed_filespace_generation,
                                generations.required_filespace_generation)) {
    return AgentError("SB_AGENT_DEP_LIFECYCLE.GENERATION_GAP",
                      "filespace_generation");
  }
  if (GenerationMissingOrBehind(generations.observed_agent_set_generation,
                                generations.required_agent_set_generation)) {
    return AgentError("SB_AGENT_DEP_LIFECYCLE.GENERATION_GAP",
                      "agent_set_generation");
  }
  return AgentOk();
}

bool ExternalClusterProviderProofPresent(
    const AgentDependencyLifecycleRequest& request) {
  return request.external_cluster_provider_proof_present &&
         !request.external_cluster_provider_id.empty() &&
         !request.external_cluster_provider_evidence_uuid.empty();
}

AgentRuntimeStatus ValidateModeSpecificEvidence(
    const AgentDependencyLifecycleRequest& request) {
  switch (request.mode) {
    case AgentDependencyLifecycleMode::normal:
    case AgentDependencyLifecycleMode::read_only:
      return AgentOk();
    case AgentDependencyLifecycleMode::restricted_open:
    case AgentDependencyLifecycleMode::maintenance:
      if (request.restricted_maintenance_evidence_uuid.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          AgentDependencyLifecycleModeName(request.mode));
      }
      return AgentOk();
    case AgentDependencyLifecycleMode::repair:
      if (request.repair_plan_uuid.empty() ||
          request.restricted_maintenance_evidence_uuid.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          "repair_plan_and_restricted_evidence");
      }
      return AgentOk();
    case AgentDependencyLifecycleMode::backup:
      if (request.backup_manifest_uuid.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          "backup_manifest");
      }
      return AgentOk();
    case AgentDependencyLifecycleMode::restore:
      if (request.restore_plan_uuid.empty() ||
          request.backup_manifest_uuid.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          "restore_plan_and_backup_manifest");
      }
      return AgentOk();
    case AgentDependencyLifecycleMode::archive_hold:
      if (request.archive_hold_evidence_uuid.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          "archive_hold_evidence");
      }
      return AgentOk();
    case AgentDependencyLifecycleMode::shutdown:
      if (request.shutdown_drain_evidence_uuid.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          "shutdown_drain_evidence");
      }
      return AgentOk();
    case AgentDependencyLifecycleMode::crash_recovery:
      if (request.crash_recovery_evidence_uuid.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          "crash_recovery_evidence");
      }
      return AgentOk();
    case AgentDependencyLifecycleMode::pitr:
      if (request.pitr_recovery_point_uuid.empty() ||
          request.restore_plan_uuid.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          "pitr_recovery_point_and_restore_plan");
      }
      return AgentOk();
    case AgentDependencyLifecycleMode::clone:
      if (request.clone_manifest_uuid.empty() ||
          request.clone_source_database_uuid.empty() ||
          request.clone_target_database_uuid.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          "clone_manifest_source_and_target");
      }
      if (request.clone_source_database_uuid == request.clone_target_database_uuid) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.CLONE_IDENTITY_INVALID",
                          "source_and_target_must_differ");
      }
      return AgentOk();
    case AgentDependencyLifecycleMode::role_change:
      if (request.role_change_evidence_uuid.empty() ||
          request.target_role.empty()) {
        return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
                          "role_change_evidence_and_target_role");
      }
      return AgentOk();
  }
  return AgentError("SB_AGENT_DEP_LIFECYCLE.MODE_INVALID");
}

void ApplyModeDecision(AgentDependencyLifecycleDecision* decision) {
  if (decision == nullptr) {
    return;
  }
  switch (decision->mode) {
    case AgentDependencyLifecycleMode::normal:
      decision->maximum_activation = AgentActivationProfile::live_action;
      decision->ordinary_work_admitted = true;
      decision->mutable_action_admitted = true;
      AddDiagnostic(decision, "SB_AGENT_DEP_LIFECYCLE.NORMAL_ADMITTED");
      return;
    case AgentDependencyLifecycleMode::read_only:
      decision->maximum_activation = AgentActivationProfile::dry_run;
      decision->inspect_only = true;
      AddDiagnostic(decision, "SB_AGENT_DEP_LIFECYCLE.READ_ONLY_INSPECT_ONLY");
      return;
    case AgentDependencyLifecycleMode::restricted_open:
    case AgentDependencyLifecycleMode::maintenance:
    case AgentDependencyLifecycleMode::repair:
      decision->maximum_activation = AgentActivationProfile::dry_run;
      decision->safe_maintenance_only = true;
      AddDiagnostic(decision, "SB_AGENT_DEP_LIFECYCLE.SAFE_MAINTENANCE_ONLY");
      return;
    case AgentDependencyLifecycleMode::backup:
    case AgentDependencyLifecycleMode::archive_hold:
      decision->maximum_activation = AgentActivationProfile::dry_run;
      decision->inspect_only = true;
      AddDiagnostic(decision, "SB_AGENT_DEP_LIFECYCLE.HOLD_INSPECT_ONLY");
      return;
    case AgentDependencyLifecycleMode::restore:
    case AgentDependencyLifecycleMode::pitr:
    case AgentDependencyLifecycleMode::clone:
      decision->maximum_activation = AgentActivationProfile::dry_run;
      decision->inspect_only = true;
      decision->restore_or_clone_mode = true;
      AddDiagnostic(decision, "SB_AGENT_DEP_LIFECYCLE.DR_RESTORE_CLONE_INSPECT_ONLY");
      return;
    case AgentDependencyLifecycleMode::shutdown:
      decision->maximum_activation = AgentActivationProfile::disabled;
      decision->drain_only = true;
      AddDiagnostic(decision, "SB_AGENT_DEP_LIFECYCLE.SHUTDOWN_DRAIN_ONLY");
      return;
    case AgentDependencyLifecycleMode::crash_recovery:
      decision->maximum_activation = AgentActivationProfile::disabled;
      decision->recovery_only = true;
      AddDiagnostic(decision, "SB_AGENT_DEP_LIFECYCLE.CRASH_RECOVERY_ONLY");
      return;
    case AgentDependencyLifecycleMode::role_change:
      decision->maximum_activation = AgentActivationProfile::dry_run;
      decision->inspect_only = true;
      AddDiagnostic(decision, "SB_AGENT_DEP_LIFECYCLE.ROLE_CHANGE_INSPECT_ONLY");
      return;
  }
}

}  // namespace

const char* AgentDependencyLifecycleModeName(
    AgentDependencyLifecycleMode mode) {
  switch (mode) {
    case AgentDependencyLifecycleMode::normal: return "normal";
    case AgentDependencyLifecycleMode::read_only: return "read_only";
    case AgentDependencyLifecycleMode::restricted_open: return "restricted_open";
    case AgentDependencyLifecycleMode::maintenance: return "maintenance";
    case AgentDependencyLifecycleMode::repair: return "repair";
    case AgentDependencyLifecycleMode::backup: return "backup";
    case AgentDependencyLifecycleMode::restore: return "restore";
    case AgentDependencyLifecycleMode::archive_hold: return "archive_hold";
    case AgentDependencyLifecycleMode::shutdown: return "shutdown";
    case AgentDependencyLifecycleMode::crash_recovery: return "crash_recovery";
    case AgentDependencyLifecycleMode::pitr: return "pitr";
    case AgentDependencyLifecycleMode::clone: return "clone";
    case AgentDependencyLifecycleMode::role_change: return "role_change";
  }
  return "normal";
}

AgentDependencyLifecycleMode AgentDependencyLifecycleModeFromRuntime(
    AgentLifecycleMode mode) {
  switch (mode) {
    case AgentLifecycleMode::normal:
    case AgentLifecycleMode::database_create:
    case AgentLifecycleMode::database_open:
      return AgentDependencyLifecycleMode::normal;
    case AgentLifecycleMode::database_close:
    case AgentLifecycleMode::shutdown:
      return AgentDependencyLifecycleMode::shutdown;
    case AgentLifecycleMode::backup:
      return AgentDependencyLifecycleMode::backup;
    case AgentLifecycleMode::restore:
      return AgentDependencyLifecycleMode::restore;
    case AgentLifecycleMode::crash_recovery:
      return AgentDependencyLifecycleMode::crash_recovery;
    case AgentLifecycleMode::restricted_open:
      return AgentDependencyLifecycleMode::restricted_open;
    case AgentLifecycleMode::read_only:
      return AgentDependencyLifecycleMode::read_only;
    case AgentLifecycleMode::maintenance:
      return AgentDependencyLifecycleMode::maintenance;
    case AgentLifecycleMode::repair:
      return AgentDependencyLifecycleMode::repair;
    case AgentLifecycleMode::archive_hold:
      return AgentDependencyLifecycleMode::archive_hold;
    case AgentLifecycleMode::pitr:
      return AgentDependencyLifecycleMode::pitr;
    case AgentLifecycleMode::clone:
      return AgentDependencyLifecycleMode::clone;
    case AgentLifecycleMode::role_change:
      return AgentDependencyLifecycleMode::role_change;
  }
  return AgentDependencyLifecycleMode::normal;
}

AgentRuntimeStatus ValidateAgentDependencyLifecycleAuthorityBoundary(
    const AgentDependencyLifecycleAuthorityBoundary& boundary) {
  if (boundary.transaction_finality_authority ||
      boundary.visibility_authority ||
      boundary.recovery_authority ||
      boundary.security_authority ||
      boundary.parser_authority ||
      boundary.donor_authority ||
      boundary.write_ahead_log_authority ||
      boundary.benchmark_authority ||
      boundary.optimizer_plan_authority ||
      boundary.index_finality_authority ||
      boundary.provider_finality_authority ||
      boundary.cluster_authority ||
      boundary.memory_authority ||
      boundary.agent_action_authority) {
    return AgentError("SB_AGENT_DEP_LIFECYCLE.AUTHORITY_DENIED",
                      "dependency_lifecycle_evidence_is_non_authoritative");
  }
  return AgentOk();
}

AgentDependencyLifecycleDecision EvaluateAgentDependencyLifecycle(
    const AgentDependencyLifecycleRequest& request) {
  if (MissingIdentity(request)) {
    return Refuse(request,
                  "SB_AGENT_DEP_LIFECYCLE.IDENTITY_REQUIRED",
                  "database_agent_lifecycle_and_dependency_digest_required");
  }

  const auto authority_status =
      ValidateAgentDependencyLifecycleAuthorityBoundary(request.authority_boundary);
  if (!authority_status.ok) {
    return Refuse(request, authority_status.diagnostic_code, authority_status.detail);
  }

  if (!request.dependency_graph_consistent) {
    return Refuse(request,
                  "SB_AGENT_DEP_LIFECYCLE.DEPENDENCY_GRAPH_INVALID",
                  request.dependency_graph_digest);
  }

  const auto generation_status = ValidateGenerationWindow(request.generations);
  if (!generation_status.ok) {
    return Refuse(request, generation_status.diagnostic_code, generation_status.detail);
  }

  if (request.local_cluster_path_requested &&
      !ExternalClusterProviderProofPresent(request)) {
    return Refuse(request,
                  "SB_AGENT_DEP_LIFECYCLE.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
                  "local_cluster_dependency_lifecycle_refused");
  }

  const auto mode_status = ValidateModeSpecificEvidence(request);
  if (!mode_status.ok) {
    return Refuse(request, mode_status.diagnostic_code, mode_status.detail);
  }

  AgentDependencyLifecycleDecision decision;
  decision.status = AgentOk();
  decision.mode = request.mode;
  decision.generation_window_valid = true;
  decision.dependency_graph_valid = true;
  decision.cluster_path_failed_closed = request.local_cluster_path_requested
      ? !ExternalClusterProviderProofPresent(request)
      : true;
  decision.external_provider_only = request.local_cluster_path_requested &&
                                    ExternalClusterProviderProofPresent(request);
  ApplyModeDecision(&decision);
  if (decision.external_provider_only) {
    AddDiagnostic(&decision,
                  "SB_AGENT_DEP_LIFECYCLE.EXTERNAL_CLUSTER_PROVIDER_ONLY");
  }
  return decision;
}

std::vector<std::string> AgentDependencyLifecycleDiagnosticCodes() {
  return {
      "SB_AGENT_DEP_LIFECYCLE.IDENTITY_REQUIRED",
      "SB_AGENT_DEP_LIFECYCLE.AUTHORITY_DENIED",
      "SB_AGENT_DEP_LIFECYCLE.DEPENDENCY_GRAPH_INVALID",
      "SB_AGENT_DEP_LIFECYCLE.GENERATION_GAP",
      "SB_AGENT_DEP_LIFECYCLE.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
      "SB_AGENT_DEP_LIFECYCLE.EXTERNAL_CLUSTER_PROVIDER_ONLY",
      "SB_AGENT_DEP_LIFECYCLE.MODE_EVIDENCE_REQUIRED",
      "SB_AGENT_DEP_LIFECYCLE.CLONE_IDENTITY_INVALID",
      "SB_AGENT_DEP_LIFECYCLE.NORMAL_ADMITTED",
      "SB_AGENT_DEP_LIFECYCLE.READ_ONLY_INSPECT_ONLY",
      "SB_AGENT_DEP_LIFECYCLE.SAFE_MAINTENANCE_ONLY",
      "SB_AGENT_DEP_LIFECYCLE.HOLD_INSPECT_ONLY",
      "SB_AGENT_DEP_LIFECYCLE.DR_RESTORE_CLONE_INSPECT_ONLY",
      "SB_AGENT_DEP_LIFECYCLE.SHUTDOWN_DRAIN_ONLY",
      "SB_AGENT_DEP_LIFECYCLE.CRASH_RECOVERY_ONLY",
      "SB_AGENT_DEP_LIFECYCLE.ROLE_CHANGE_INSPECT_ONLY"};
}

}  // namespace scratchbird::core::agents
