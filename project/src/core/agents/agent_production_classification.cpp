// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_production_classification.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace scratchbird::core::agents {
namespace {

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool IsAnchorOnlyAgent(const std::string& agent_type_id) {
  static const std::set<std::string> anchor_only = {
      "cluster_autoscale_manager",
      "distributed_query_metrics_agent",
      "remote_query_routing_agent",
      "cluster_scheduler_manager",
      "cluster_upgrade_manager"};
  return anchor_only.find(agent_type_id) != anchor_only.end();
}

bool IsBackupArchiveRestoreExportAgent(const std::string& agent_type_id) {
  static const std::set<std::string> agents = {
      "backup_manager",
      "archive_manager",
      "restore_drill_manager",
      "pitr_manager",
      "export_adapter_manager"};
  return agents.find(agent_type_id) != agents.end();
}

bool IsIdentitySessionJobAgent(const std::string& agent_type_id) {
  static const std::set<std::string> agents = {
      "identity_manager",
      "session_control_manager",
      "job_control_manager"};
  return agents.find(agent_type_id) != agents.end();
}

bool IsClusterAgent(const CanonicalAgentManifestEntry& entry) {
  return entry.cluster_only || entry.deployment == AgentDeployment::cluster;
}

std::vector<AgentActionContractDescriptor> ContractsForAgent(
    const std::string& agent_type_id);

const AgentProductionRouteProofInputs::RouteProof* RouteProofFor(
    const AgentProductionRouteProofInputs& inputs,
    const std::string& agent_type_id,
    const std::string& action_id) {
  for (const auto& proof : inputs.route_proofs) {
    if (proof.agent_type_id == agent_type_id && proof.action_id == action_id) {
      return &proof;
    }
  }
  return nullptr;
}

bool RouteProofValidForContract(
    const AgentProductionRouteProofInputs::RouteProof* proof,
    const AgentActionContractDescriptor& contract) {
  if (proof == nullptr) { return false; }
  if (!proof->live_route_available ||
      !proof->real_subsystem_handler ||
      !proof->idempotent ||
      !proof->supports_retry ||
      !proof->supports_rollback_compensation ||
      !proof->requires_outcome_verification ||
      proof->provider_id.empty() ||
      proof->subsystem_handler_id.empty() ||
      proof->handler_provenance.empty() ||
      proof->handler_evidence_uuid.empty()) {
    return false;
  }
  if (proof->agent_type_id != contract.owning_agent ||
      proof->action_id != contract.action_id ||
      proof->actuator_id != contract.actuator) {
    return false;
  }
  if (proof->authority_domain != ActuatorAuthorityDomainForId(contract.actuator)) {
    return false;
  }
  if (contract.cluster_scoped && !proof->external_cluster_provider) {
    return false;
  }
  if (proof->authority_domain == AgentActuatorAuthorityDomain::cluster_provider &&
      !proof->external_cluster_provider) {
    return false;
  }
  return true;
}

bool LiveProofPresentForAction(
    const std::string& agent_type_id,
    const std::string& action_id,
    const AgentProductionRouteProofInputs& inputs) {
  const auto contract = FindAgentActionContract(agent_type_id, action_id);
  if (!contract.has_value()) { return false; }
  return RouteProofValidForContract(
      RouteProofFor(inputs, agent_type_id, action_id), *contract);
}

bool AnyLiveProofPresentForAgent(
    const std::string& agent_type_id,
    const AgentProductionRouteProofInputs& inputs) {
  for (const auto& contract : ContractsForAgent(agent_type_id)) {
    if (LiveProofPresentForAction(agent_type_id, contract.action_id, inputs)) {
      return true;
    }
  }
  return false;
}

bool AnyPhysicalMutationProofForAgent(
    const std::string& agent_type_id,
    const AgentProductionRouteProofInputs& inputs) {
  for (const auto& proof : inputs.route_proofs) {
    if (proof.agent_type_id == agent_type_id &&
        proof.physical_mutation_route &&
        LiveProofPresentForAction(agent_type_id, proof.action_id, inputs)) {
      return true;
    }
  }
  return false;
}

void AddRouteProofEvidenceFields(
    std::vector<std::string>* fields,
    const AgentProductionRouteProofInputs::RouteProof& proof) {
  fields->push_back("provider_id=" + proof.provider_id);
  fields->push_back("actuator_id=" + proof.actuator_id);
  fields->push_back(
      "authority_domain=" +
      std::string(AgentActuatorAuthorityDomainName(proof.authority_domain)));
  fields->push_back("subsystem_handler_id=" + proof.subsystem_handler_id);
  fields->push_back("handler_provenance=" + proof.handler_provenance);
  fields->push_back("handler_evidence_uuid=" + proof.handler_evidence_uuid);
  fields->push_back("real_subsystem_handler=true");
  fields->push_back("idempotent=true");
  fields->push_back("supports_retry=true");
  fields->push_back("supports_rollback_compensation=true");
  fields->push_back("requires_outcome_verification=true");
}

std::string RouteEvidenceKindForAgent(
    const std::string& agent_type_id,
    const AgentProductionRouteProofInputs& inputs,
    const std::string& fallback) {
  for (const auto& proof : inputs.route_proofs) {
    if (proof.agent_type_id == agent_type_id &&
        !proof.handler_provenance.empty()) {
      return proof.handler_provenance;
    }
  }
  return fallback;
}

std::vector<std::string> BaseEvidenceFields(
    const CanonicalAgentManifestEntry& entry,
    const std::string& route_evidence_kind) {
  return {
      "source=canonical_agent_manifest",
      "agent_type_id=" + entry.type_id,
      "implementation_anchor=" + entry.implementation_anchor,
      "deployment=" + std::string(AgentDeploymentName(entry.deployment)),
      "manifest_default_activation=" +
          std::string(AgentActivationProfileName(entry.default_activation)),
      "route_evidence_kind=" + route_evidence_kind,
      "parser_authority=false",
      "client_authority=false",
      "donor_authority=false",
      "sidecar_authority=false",
      "transaction_finality_authority=false",
      "visibility_authority=false",
      "recovery_authority=false"};
}

AgentProductionExposureClass ExposureFromManifestDefault(
    AgentActivationProfile activation) {
  switch (activation) {
    case AgentActivationProfile::live_action:
      return AgentProductionExposureClass::live_action;
    case AgentActivationProfile::dry_run:
      return AgentProductionExposureClass::dry_run_only;
    case AgentActivationProfile::recommend_only:
    case AgentActivationProfile::observe_only:
      return AgentProductionExposureClass::recommendation_only;
    case AgentActivationProfile::disabled:
      return AgentProductionExposureClass::disabled_blocked;
  }
  return AgentProductionExposureClass::disabled_blocked;
}

std::vector<AgentActionContractDescriptor> ContractsForAgent(
    const std::string& agent_type_id) {
  std::vector<AgentActionContractDescriptor> contracts;
  for (const auto& contract : AgentActionContractRegistry()) {
    if (contract.owning_agent == agent_type_id) {
      contracts.push_back(contract);
    }
  }
  return contracts;
}

bool LiveActionAllowedForAgent(const std::string& agent_type_id,
                               const std::string& action_id,
                               const AgentProductionRouteProofInputs& inputs) {
  return LiveProofPresentForAction(agent_type_id, action_id, inputs);
}

AgentProductionActionExposure BuildActionExposure(
    const AgentProductionExposureRecord& record,
    const AgentActionContractDescriptor& contract,
    const AgentProductionRouteProofInputs& inputs) {
  AgentProductionActionExposure action;
  action.agent_type_id = record.agent_type_id;
  action.action_id = contract.action_id;
  action.actuator_id = contract.actuator;
  action.action_contract_present = true;
  action.action_contract_implies_live_route = false;
  action.authority_safety = record.authority_safety;
  action.route_evidence_fields = record.route_evidence_fields;
  action.route_evidence_fields.push_back("action_contract_present=true");
  action.route_evidence_fields.push_back("action_contract_implies_live_route=false");
  action.route_evidence_fields.push_back("action_id=" + contract.action_id);
  action.route_evidence_fields.push_back("actuator_id=" + contract.actuator);

  const auto* proof = RouteProofFor(inputs, record.agent_type_id,
                                    contract.action_id);
  if (LiveActionAllowedForAgent(record.agent_type_id, contract.action_id, inputs)) {
    action.exposure = AgentProductionExposureClass::live_action;
    action.live_route_available = true;
    action.route_evidence_kind =
        proof == nullptr || proof->handler_provenance.empty()
            ? record.route_evidence_kind
            : proof->handler_provenance;
    action.diagnostic_code = record.diagnostic_code;
    AddRouteProofEvidenceFields(&action.route_evidence_fields, *proof);
    return action;
  }

  if (record.exposure == AgentProductionExposureClass::dry_run_only) {
    action.exposure = AgentProductionExposureClass::dry_run_only;
    action.dry_run_route_available = true;
    action.route_evidence_kind = "dry_run_only_contract";
    action.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.ACTION_DRY_RUN_ONLY";
    return action;
  }
  if (record.exposure == AgentProductionExposureClass::workflow_only) {
    action.exposure = AgentProductionExposureClass::workflow_only;
    action.workflow_route_available = record.workflow_route_proven;
    action.route_evidence_kind = "local_workflow_action_contract";
    action.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.ACTION_WORKFLOW_ONLY";
    action.route_evidence_fields.push_back(
        std::string("workflow_route_proven=") +
        (record.workflow_route_proven ? "true" : "false"));
    return action;
  }
  if (record.exposure == AgentProductionExposureClass::recommendation_only ||
      record.exposure == AgentProductionExposureClass::live_action) {
    action.exposure = AgentProductionExposureClass::recommendation_only;
    action.recommendation_route_available = true;
    action.route_evidence_kind = "recommendation_or_evidence_only_contract";
    action.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.ACTION_RECOMMENDATION_ONLY";
    return action;
  }

  action.exposure = AgentProductionExposureClass::disabled_blocked;
  action.route_evidence_kind = "production_live_route_blocked";
  action.diagnostic_code =
      "SB_AGENT_PRODUCTION_EXPOSURE.ACTION_LIVE_ROUTE_BLOCKED";
  return action;
}

}  // namespace

const char* AgentProductionExposureClassName(
    AgentProductionExposureClass value) {
  switch (value) {
    case AgentProductionExposureClass::live_action: return "live_action";
    case AgentProductionExposureClass::recommendation_only:
      return "recommendation_only";
    case AgentProductionExposureClass::dry_run_only: return "dry_run_only";
    case AgentProductionExposureClass::workflow_only: return "workflow_only";
    case AgentProductionExposureClass::disabled_blocked:
      return "disabled_blocked";
  }
  return "disabled_blocked";
}

AgentProductionExposureRecord ClassifyCanonicalAgentProductionExposure(
    const CanonicalAgentManifestEntry& entry,
    const AgentProductionRouteProofInputs& route_inputs) {
  AgentProductionExposureRecord record;
  record.agent_type_id = entry.type_id;
  record.implementation_anchor = entry.implementation_anchor;
  record.deployment = entry.deployment;
  record.authority = entry.authority;
  record.manifest_default_activation = entry.default_activation;
  record.source_manifest_present = true;
  record.cluster_only = entry.cluster_only;
  record.cluster_provider_authority_required = IsClusterAgent(entry);
  record.cluster_provider_authority_available =
      route_inputs.real_cluster_provider_authority;
  record.implementation_anchor_only = IsAnchorOnlyAgent(entry.type_id);
  record.exposure = ExposureFromManifestDefault(entry.default_activation);
  record.diagnostic_code =
      "SB_AGENT_PRODUCTION_EXPOSURE.MANIFEST_DEFAULT_NONLIVE";
  record.route_evidence_kind = "manifest_default_nonlive";

  if (IsClusterAgent(entry)) {
    record.exposure = AgentProductionExposureClass::disabled_blocked;
    record.production_surface_visible = false;
    record.diagnostic_code =
        route_inputs.real_cluster_provider_authority
            ? "SB_AGENT_PRODUCTION_EXPOSURE.CLUSTER_HANDLER_NOT_CLASSIFIED"
            : "SB_AGENT_PRODUCTION_EXPOSURE.CLUSTER_PROVIDER_REQUIRED";
    record.route_evidence_kind = "cluster_provider_required";
  } else if (entry.type_id == "page_allocation_manager") {
    const auto* proof = RouteProofFor(route_inputs, entry.type_id,
                                      "preallocate_page_family");
    const bool live = LiveProofPresentForAction(
        entry.type_id, "preallocate_page_family", route_inputs);
    record.real_subsystem_route_proven =
        live;
    record.physical_mutation_route_proven =
        live && proof != nullptr && proof->physical_mutation_route;
    record.production_live_route_available =
        live;
    record.exposure =
        live
            ? AgentProductionExposureClass::live_action
            : AgentProductionExposureClass::recommendation_only;
    record.diagnostic_code =
        live
            ? "SB_AGENT_PRODUCTION_EXPOSURE.LIVE_STORAGE_PAGE_PREALLOCATION_PROVEN"
            : "SB_AGENT_PRODUCTION_EXPOSURE.PAGE_PREALLOCATION_PROOF_REQUIRED";
    record.route_evidence_kind =
        live
            ? RouteEvidenceKindForAgent(entry.type_id, route_inputs,
                                        "storage_page_preallocation_route")
            : "storage_page_recommendation_only";
  } else if (entry.type_id == "filespace_capacity_manager") {
    record.exposure = AgentProductionExposureClass::recommendation_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.FILESPACE_CAPACITY_WINDOW_RECOMMENDATION_ONLY";
    record.route_evidence_kind = "filespace_capacity_window_handoff_only";
  } else if (entry.type_id == "storage_health_manager") {
    record.exposure = AgentProductionExposureClass::recommendation_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.STORAGE_HEALTH_EVIDENCE_ONLY";
    record.route_evidence_kind = "storage_health_recommendation_evidence";
  } else if (entry.type_id == "node_resource_agent") {
    record.exposure = AgentProductionExposureClass::recommendation_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.NODE_RESOURCE_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "node_resource_local_metric_evaluator";
  } else if (entry.type_id == "metrics_registry_manager") {
    record.exposure = AgentProductionExposureClass::dry_run_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.METRICS_REGISTRY_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "metrics_registry_local_integrity_evaluator";
  } else if (entry.type_id == "memory_governor") {
    record.exposure = AgentProductionExposureClass::dry_run_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.MEMORY_GOVERNOR_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "memory_governor_local_resource_evaluator";
  } else if (entry.type_id == "admission_control_manager") {
    record.exposure = AgentProductionExposureClass::dry_run_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.ADMISSION_CONTROL_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "admission_control_local_pressure_evaluator";
  } else if (entry.type_id == "alert_manager") {
    record.exposure = AgentProductionExposureClass::dry_run_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.ALERT_MANAGER_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "alert_manager_local_delivery_evaluator";
  } else if (entry.type_id == "index_health_manager") {
    record.exposure = AgentProductionExposureClass::recommendation_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.INDEX_HEALTH_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "index_health_local_advisory_evaluator";
  } else if (entry.type_id == "cleanup_archive_manager") {
    record.exposure = AgentProductionExposureClass::dry_run_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.CLEANUP_ARCHIVE_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "cleanup_archive_local_horizon_evaluator";
  } else if (entry.type_id == "runtime_learning_agent") {
    record.exposure = AgentProductionExposureClass::recommendation_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.RUNTIME_LEARNING_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "runtime_learning_local_advisory_evaluator";
  } else if (entry.type_id == "policy_recommendation_manager") {
    record.exposure = AgentProductionExposureClass::recommendation_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.POLICY_RECOMMENDATION_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "policy_recommendation_local_advisory_evaluator";
  } else if (entry.type_id == "parser_interface_manager") {
    record.exposure = AgentProductionExposureClass::recommendation_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.PARSER_INTERFACE_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "parser_interface_local_non_authority_evaluator";
  } else if (entry.type_id == "support_bundle_triage_agent") {
    record.exposure = AgentProductionExposureClass::recommendation_only;
    record.real_subsystem_route_proven = false;
    record.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.SUPPORT_TRIAGE_LOCAL_EVALUATOR_PROVEN";
    record.route_evidence_kind = "support_triage_local_redaction_evaluator";
  } else if (entry.type_id == "storage_version_cleanup_agent") {
    const auto* proof = RouteProofFor(route_inputs, entry.type_id,
                                      "cleanup_storage_versions");
    const bool live = LiveProofPresentForAction(
        entry.type_id, "cleanup_storage_versions", route_inputs);
    record.real_subsystem_route_proven =
        live;
    record.production_live_route_available =
        live;
    record.exposure =
        live
            ? AgentProductionExposureClass::live_action
            : AgentProductionExposureClass::dry_run_only;
    record.diagnostic_code =
        live
            ? "SB_AGENT_PRODUCTION_EXPOSURE.LIVE_STORAGE_VERSION_CLEANUP_PROVEN"
            : "SB_AGENT_PRODUCTION_EXPOSURE.STORAGE_VERSION_CLEANUP_DRY_RUN_ONLY";
    record.route_evidence_kind =
        live
            ? (proof == nullptr || proof->handler_provenance.empty()
                   ? "storage_version_cleanup_live_route"
                   : proof->handler_provenance)
            : "bounded_cleanup_dry_run_only";
  } else if (IsBackupArchiveRestoreExportAgent(entry.type_id)) {
    const bool live = AnyLiveProofPresentForAgent(entry.type_id, route_inputs);
    record.exposure = live ? AgentProductionExposureClass::live_action
                           : AgentProductionExposureClass::workflow_only;
    record.production_live_route_available = live;
    record.production_surface_visible = true;
    record.real_subsystem_route_proven = live;
    record.workflow_route_proven = true;
    record.physical_mutation_route_proven =
        AnyPhysicalMutationProofForAgent(entry.type_id, route_inputs);
    record.diagnostic_code =
        live ? "SB_AGENT_PRODUCTION_EXPOSURE.LIVE_SUBSYSTEM_HANDLER_PROVEN"
             : "SB_AGENT_PRODUCTION_EXPOSURE.LOCAL_WORKFLOW_HANDLER_PROVEN_LIVE_ACTUATOR_REQUIRED";
    record.route_evidence_kind =
        live ? RouteEvidenceKindForAgent(entry.type_id, route_inputs,
                                         "real_subsystem_handler_route")
             : "local_workflow_handler_route";
  } else if (IsIdentitySessionJobAgent(entry.type_id)) {
    const bool live = AnyLiveProofPresentForAgent(entry.type_id, route_inputs);
    record.exposure = live ? AgentProductionExposureClass::live_action
                           : AgentProductionExposureClass::workflow_only;
    record.production_live_route_available = live;
    record.production_surface_visible = true;
    record.real_subsystem_route_proven = live;
    record.workflow_route_proven = true;
    record.physical_mutation_route_proven =
        AnyPhysicalMutationProofForAgent(entry.type_id, route_inputs);
    record.diagnostic_code =
        live ? "SB_AGENT_PRODUCTION_EXPOSURE.LIVE_SUBSYSTEM_HANDLER_PROVEN"
             : "SB_AGENT_PRODUCTION_EXPOSURE.LOCAL_WORKFLOW_HANDLER_PROVEN_LIVE_ACTUATOR_REQUIRED";
    record.route_evidence_kind =
        live ? RouteEvidenceKindForAgent(entry.type_id, route_inputs,
                                         "real_subsystem_handler_route")
             : "local_workflow_handler_route";
  } else if (record.implementation_anchor_only) {
    record.exposure = ExposureFromManifestDefault(entry.default_activation);
    if (record.exposure == AgentProductionExposureClass::live_action) {
      record.exposure = AgentProductionExposureClass::disabled_blocked;
    }
    record.production_live_route_available = false;
    record.diagnostic_code =
        record.exposure == AgentProductionExposureClass::dry_run_only
            ? "SB_AGENT_PRODUCTION_EXPOSURE.ANCHOR_ONLY_DRY_RUN_ONLY"
            : "SB_AGENT_PRODUCTION_EXPOSURE.ANCHOR_ONLY_RECOMMENDATION_ONLY";
    record.route_evidence_kind =
        record.exposure == AgentProductionExposureClass::dry_run_only
            ? "anchor_only_dry_run_contract"
            : "anchor_only_recommendation_contract";
  }

  record.route_evidence_fields = BaseEvidenceFields(entry,
                                                    record.route_evidence_kind);
  record.route_evidence_fields.push_back(
      std::string("production_exposure=") +
      AgentProductionExposureClassName(record.exposure));
  record.route_evidence_fields.push_back(
      std::string("production_live_route_available=") +
      (record.production_live_route_available ? "true" : "false"));
  record.route_evidence_fields.push_back(
      std::string("real_subsystem_route_proven=") +
      (record.real_subsystem_route_proven ? "true" : "false"));
  record.route_evidence_fields.push_back(
      std::string("workflow_route_proven=") +
      (record.workflow_route_proven ? "true" : "false"));
  record.route_evidence_fields.push_back(
      std::string("physical_mutation_route_proven=") +
      (record.physical_mutation_route_proven ? "true" : "false"));
  record.route_evidence_fields.push_back(
      std::string("implementation_anchor_only=") +
      (record.implementation_anchor_only ? "true" : "false"));
  record.route_evidence_fields.push_back(
      std::string("action_contract_implies_live_route=false"));
  for (const auto& proof : route_inputs.route_proofs) {
    const auto contract = FindAgentActionContract(proof.agent_type_id,
                                                  proof.action_id);
    if (proof.agent_type_id == entry.type_id && contract.has_value() &&
        RouteProofValidForContract(&proof, *contract)) {
      record.route_evidence_fields.push_back(
          "live_route_proof_action_id=" + proof.action_id);
      AddRouteProofEvidenceFields(&record.route_evidence_fields, proof);
    }
  }

  for (const auto& contract : ContractsForAgent(entry.type_id)) {
    record.action_contract_present = true;
    record.actions.push_back(BuildActionExposure(record, contract,
                                                 route_inputs));
  }
  return record;
}

std::vector<AgentProductionExposureRecord>
ClassifyAllCanonicalAgentProductionExposures(
    const AgentProductionRouteProofInputs& route_inputs) {
  std::vector<AgentProductionExposureRecord> records;
  for (const auto& entry : CanonicalAgentManifest()) {
    records.push_back(ClassifyCanonicalAgentProductionExposure(entry,
                                                               route_inputs));
  }
  return records;
}

AgentProductionActionExposure ClassifyAgentProductionActionExposure(
    const std::string& agent_type_id,
    const std::string& action_id,
    const AgentProductionRouteProofInputs& route_inputs) {
  for (const auto& record :
       ClassifyAllCanonicalAgentProductionExposures(route_inputs)) {
    if (record.agent_type_id != agent_type_id) { continue; }
    for (const auto& action : record.actions) {
      if (action.action_id == action_id) { return action; }
    }
    AgentProductionActionExposure missing;
    missing.agent_type_id = agent_type_id;
    missing.action_id = action_id;
    missing.exposure = AgentProductionExposureClass::disabled_blocked;
    missing.diagnostic_code =
        "SB_AGENT_PRODUCTION_EXPOSURE.ACTION_CONTRACT_REQUIRED";
    missing.route_evidence_kind = "missing_action_contract";
    missing.route_evidence_fields = record.route_evidence_fields;
    missing.route_evidence_fields.push_back("action_contract_present=false");
    missing.route_evidence_fields.push_back(
        "action_contract_implies_live_route=false");
    return missing;
  }
  AgentProductionActionExposure missing_agent;
  missing_agent.agent_type_id = agent_type_id;
  missing_agent.action_id = action_id;
  missing_agent.exposure = AgentProductionExposureClass::disabled_blocked;
  missing_agent.diagnostic_code =
      "SB_AGENT_PRODUCTION_EXPOSURE.AGENT_NOT_CANONICAL";
  missing_agent.route_evidence_kind = "missing_canonical_agent";
  missing_agent.route_evidence_fields = {
      "source=canonical_agent_manifest",
      "source_manifest_present=false",
      "action_contract_implies_live_route=false",
      "parser_authority=false",
      "client_authority=false",
      "donor_authority=false",
      "sidecar_authority=false",
      "transaction_finality_authority=false",
      "visibility_authority=false",
      "recovery_authority=false"};
  return missing_agent;
}

AgentRuntimeStatus ValidateAgentProductionExposureMatrix(
    const AgentProductionRouteProofInputs& route_inputs) {
  const auto registry_status = ValidateCanonicalAgentRegistry();
  if (!registry_status.ok) { return registry_status; }

  const auto manifest = CanonicalAgentManifest();
  const auto records =
      ClassifyAllCanonicalAgentProductionExposures(route_inputs);
  if (records.size() != manifest.size()) {
    return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.COUNT_MISMATCH");
  }

  for (const auto& proof : route_inputs.route_proofs) {
    const auto contract = FindAgentActionContract(proof.agent_type_id,
                                                  proof.action_id);
    if (!contract.has_value()) {
      return AgentError(
          "SB_AGENT_PRODUCTION_EXPOSURE.ROUTE_PROOF_CONTRACT_REQUIRED",
          proof.agent_type_id + ":" + proof.action_id);
    }
    if (!RouteProofValidForContract(&proof, *contract)) {
      return AgentError(
          "SB_AGENT_PRODUCTION_EXPOSURE.ROUTE_PROOF_INVALID",
          proof.agent_type_id + ":" + proof.action_id);
    }
    if (proof.external_cluster_provider &&
        !route_inputs.real_cluster_provider_authority) {
      return AgentError(
          "SB_AGENT_PRODUCTION_EXPOSURE.CLUSTER_PROVIDER_AUTHORITY_REQUIRED",
          proof.agent_type_id + ":" + proof.action_id);
    }
  }

  std::set<std::string> manifest_names;
  for (const auto& entry : manifest) {
    manifest_names.insert(entry.type_id);
  }

  std::set<std::string> seen;
  for (const auto& record : records) {
    if (!record.source_manifest_present ||
        manifest_names.find(record.agent_type_id) == manifest_names.end()) {
      return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.NON_CANONICAL_AGENT",
                        record.agent_type_id);
    }
    if (!seen.insert(record.agent_type_id).second) {
      return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.DUPLICATE_AGENT",
                        record.agent_type_id);
    }
    if (record.action_contract_implies_live_route) {
      return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.CONTRACT_IMPLIES_LIVE_ROUTE",
                        record.agent_type_id);
    }
    if (record.authority_safety.parser_authority ||
        record.authority_safety.client_authority ||
        record.authority_safety.donor_authority ||
        record.authority_safety.sidecar_authority ||
        record.authority_safety.transaction_finality_authority ||
        record.authority_safety.visibility_authority ||
        record.authority_safety.recovery_authority) {
      return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.AUTHORITY_DRIFT",
                        record.agent_type_id);
    }
    if (record.cluster_only && !route_inputs.real_cluster_provider_authority &&
        record.exposure == AgentProductionExposureClass::live_action) {
      return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.CLUSTER_LIVE_WITHOUT_PROVIDER",
                        record.agent_type_id);
    }
    if (record.implementation_anchor_only &&
        record.exposure == AgentProductionExposureClass::live_action &&
        !record.real_subsystem_route_proven) {
      return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.ANCHOR_ONLY_LIVE_ROUTE",
                        record.agent_type_id);
    }
    if (record.production_live_route_available &&
        !record.real_subsystem_route_proven) {
      return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.LIVE_ROUTE_WITHOUT_PROOF",
                        record.agent_type_id);
    }
    if (record.exposure == AgentProductionExposureClass::workflow_only &&
        (!record.workflow_route_proven ||
         record.production_live_route_available)) {
      return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.WORKFLOW_ONLY_PROOF_INVALID",
                        record.agent_type_id);
    }
    for (const auto& action : record.actions) {
      if (action.action_contract_implies_live_route) {
        return AgentError(
            "SB_AGENT_PRODUCTION_EXPOSURE.ACTION_CONTRACT_IMPLIES_LIVE_ROUTE",
            record.agent_type_id + ":" + action.action_id);
      }
      if (action.live_route_available &&
          !LiveActionAllowedForAgent(record.agent_type_id, action.action_id,
                                     route_inputs)) {
        return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.ACTION_LIVE_WITHOUT_PROOF",
                          record.agent_type_id + ":" + action.action_id);
      }
    }
  }
  if (seen.size() != manifest_names.size()) {
    return AgentError("SB_AGENT_PRODUCTION_EXPOSURE.MISSING_AGENT");
  }
  return {true, "SB_AGENT_PRODUCTION_EXPOSURE.VALIDATED",
          std::to_string(records.size())};
}

}  // namespace scratchbird::core::agents
