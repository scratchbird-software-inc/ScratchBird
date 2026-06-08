// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_action_dispatch.hpp"
#include "agent_production_classification.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
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

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::map<std::string, agents::AgentProductionExposureRecord> ExposureByAgent(
    const agents::AgentProductionRouteProofInputs& inputs = {}) {
  std::map<std::string, agents::AgentProductionExposureRecord> by_agent;
  for (const auto& record :
       agents::ClassifyAllCanonicalAgentProductionExposures(inputs)) {
    Require(by_agent.emplace(record.agent_type_id, record).second,
            "duplicate production exposure record: " + record.agent_type_id);
  }
  return by_agent;
}

const agents::AgentProductionExposureRecord& Record(
    const std::map<std::string, agents::AgentProductionExposureRecord>& by_agent,
    const std::string& agent_type_id) {
  const auto found = by_agent.find(agent_type_id);
  Require(found != by_agent.end(),
          "missing production exposure record: " + agent_type_id);
  return found->second;
}

const agents::AgentProductionActionExposure& Action(
    const agents::AgentProductionExposureRecord& record,
    const std::string& action_id) {
  for (const auto& action : record.actions) {
    if (action.action_id == action_id) { return action; }
  }
  Fail("missing action exposure record: " + record.agent_type_id + ":" +
       action_id);
}

agents::AgentProductionRouteProofInputs::RouteProof PagePreallocationProof() {
  agents::AgentProductionRouteProofInputs::RouteProof proof;
  proof.agent_type_id = "page_allocation_manager";
  proof.action_id = "preallocate_page_family";
  proof.provider_id = "page_manager:preallocate_page_family";
  proof.actuator_id = "page_manager";
  proof.authority_domain = agents::AgentActuatorAuthorityDomain::page;
  proof.subsystem_handler_id = "storage.page.preallocate_page_family";
  proof.handler_provenance = "storage_page_preallocation_route";
  proof.handler_evidence_uuid = "018f0000-0000-7000-8000-00000000ee01";
  proof.live_route_available = true;
  proof.real_subsystem_handler = true;
  proof.idempotent = true;
  proof.supports_retry = true;
  proof.supports_rollback_compensation = true;
  proof.requires_outcome_verification = true;
  proof.physical_mutation_route = true;
  return proof;
}

void RequireNoAuthorityDrift(const agents::AgentProductionExposureRecord& record) {
  Require(!record.authority_safety.parser_authority,
          "parser authority drift: " + record.agent_type_id);
  Require(!record.authority_safety.client_authority,
          "client authority drift: " + record.agent_type_id);
  Require(!record.authority_safety.donor_authority,
          "donor authority drift: " + record.agent_type_id);
  Require(!record.authority_safety.sidecar_authority,
          "sidecar authority drift: " + record.agent_type_id);
  Require(!record.authority_safety.transaction_finality_authority,
          "transaction authority drift: " + record.agent_type_id);
  Require(!record.authority_safety.visibility_authority,
          "visibility authority drift: " + record.agent_type_id);
  Require(!record.authority_safety.recovery_authority,
          "recovery authority drift: " + record.agent_type_id);
  Require(Contains(record.route_evidence_fields, "parser_authority=false"),
          "parser authority evidence missing: " + record.agent_type_id);
  Require(Contains(record.route_evidence_fields,
                   "action_contract_implies_live_route=false"),
          "contract/live evidence missing: " + record.agent_type_id);
}

void TestAllCanonicalAgentsClassified() {
  const auto records = agents::ClassifyAllCanonicalAgentProductionExposures();
  Require(records.size() == agents::CanonicalAgentManifestCount(),
          "not every canonical agent was classified");
  Require(records.size() == 29, "canonical production exposure count drifted");
  const auto status = agents::ValidateAgentProductionExposureMatrix();
  Require(status.ok, "production exposure validation failed: " +
                         status.diagnostic_code + " " + status.detail);

  std::set<std::string> manifest_names;
  for (const auto& entry : agents::CanonicalAgentManifest()) {
    manifest_names.insert(entry.type_id);
  }

  int live_agents = 0;
  int recommendation_agents = 0;
  int dry_run_agents = 0;
  int workflow_agents = 0;
  int disabled_agents = 0;
  int live_actions = 0;
  for (const auto& record : records) {
    Require(manifest_names.count(record.agent_type_id) == 1,
            "classified non-canonical agent: " + record.agent_type_id);
    Require(record.source_manifest_present,
            "manifest source flag missing: " + record.agent_type_id);
    Require(!record.action_contract_implies_live_route,
            "action contract implied live route: " + record.agent_type_id);
    RequireNoAuthorityDrift(record);
    switch (record.exposure) {
      case agents::AgentProductionExposureClass::live_action:
        ++live_agents;
        Require(record.production_live_route_available,
                "live agent lacks live route: " + record.agent_type_id);
        Require(record.real_subsystem_route_proven,
                "live agent lacks subsystem proof: " + record.agent_type_id);
        break;
      case agents::AgentProductionExposureClass::recommendation_only:
        ++recommendation_agents;
        Require(!record.production_live_route_available,
                "recommendation-only agent has live route: " + record.agent_type_id);
        break;
      case agents::AgentProductionExposureClass::dry_run_only:
        ++dry_run_agents;
        Require(!record.production_live_route_available,
                "dry-run-only agent has live route: " + record.agent_type_id);
        break;
      case agents::AgentProductionExposureClass::workflow_only:
        ++workflow_agents;
        Require(record.workflow_route_proven,
                "workflow-only agent lacks local workflow proof: " +
                    record.agent_type_id);
        Require(!record.production_live_route_available,
                "workflow-only agent has live route: " + record.agent_type_id);
        break;
      case agents::AgentProductionExposureClass::disabled_blocked:
        ++disabled_agents;
        Require(!record.production_live_route_available,
                "disabled agent has live route: " + record.agent_type_id);
        break;
    }
    for (const auto& action : record.actions) {
      Require(action.action_contract_present,
              "missing action contract flag: " + record.agent_type_id);
      Require(!action.action_contract_implies_live_route,
              "action contract implied action live route: " +
                  record.agent_type_id + ":" + action.action_id);
      if (action.live_route_available) { ++live_actions; }
    }
  }
  Require(live_agents == 0, "default proof set exposed live agents");
  Require(live_actions == 0, "default proof set exposed live actions");
  Require(recommendation_agents > 0, "recommendation class missing");
  Require(dry_run_agents > 0, "dry-run class missing");
  Require(workflow_agents > 0, "workflow-only class missing");
  Require(disabled_agents > 0, "disabled class missing");
}

void TestStorageClassifications() {
  const auto by_agent = ExposureByAgent();
  const auto& page = Record(by_agent, "page_allocation_manager");
  Require(page.exposure ==
              agents::AgentProductionExposureClass::recommendation_only,
          "page allocation manager default route should require explicit proof");
  Require(!page.physical_mutation_route_proven,
          "page allocation default route overclaimed physical mutation proof");
  Require(page.diagnostic_code ==
              "SB_AGENT_PRODUCTION_EXPOSURE.PAGE_PREALLOCATION_PROOF_REQUIRED",
          "page allocation diagnostic mismatch");
  Require(!Action(page, "preallocate_page_family").live_route_available,
          "page preallocation action was live without route proof");

  agents::AgentProductionRouteProofInputs proven_inputs;
  proven_inputs.route_proofs.push_back(PagePreallocationProof());
  const auto proven_by_agent = ExposureByAgent(proven_inputs);
  const auto& proven_page =
      Record(proven_by_agent, "page_allocation_manager");
  const auto proof_status =
      agents::ValidateAgentProductionExposureMatrix(proven_inputs);
  Require(proof_status.ok, "provider proof exposure validation failed: " +
                               proof_status.diagnostic_code);
  Require(proven_page.exposure == agents::AgentProductionExposureClass::live_action,
          "page allocation manager should become live with explicit proof");
  Require(proven_page.physical_mutation_route_proven,
          "page allocation manager live route lacks physical mutation proof");
  Require(proven_page.diagnostic_code ==
              "SB_AGENT_PRODUCTION_EXPOSURE.LIVE_STORAGE_PAGE_PREALLOCATION_PROVEN",
          "page allocation proof diagnostic mismatch");
  Require(Action(proven_page, "preallocate_page_family").live_route_available,
          "page preallocation action is not live with proof");
  Require(Contains(Action(proven_page, "preallocate_page_family").route_evidence_fields,
                   "provider_id=page_manager:preallocate_page_family"),
          "page preallocation action lacks provider proof evidence");
  Require(Contains(Action(proven_page, "preallocate_page_family").route_evidence_fields,
                   "handler_provenance=storage_page_preallocation_route"),
          "page preallocation action lacks handler provenance evidence");
  Require(Contains(Action(proven_page, "preallocate_page_family").route_evidence_fields,
                   "authority_domain=page"),
          "page preallocation action lacks authority-domain evidence");
  Require(Contains(Action(proven_page, "preallocate_page_family").route_evidence_fields,
                   "supports_retry=true"),
          "page preallocation action lacks retry proof evidence");
  Require(Contains(Action(proven_page, "preallocate_page_family").route_evidence_fields,
                   "supports_rollback_compensation=true"),
          "page preallocation action lacks compensation proof evidence");
  Require(Contains(Action(proven_page, "preallocate_page_family").route_evidence_fields,
                   "requires_outcome_verification=true"),
          "page preallocation action lacks outcome-verification proof evidence");
  Require(!Action(page, "relocate_pages").live_route_available,
          "page relocation incorrectly exposed as live");
  Require(!Action(page, "defragment_page_family").live_route_available,
          "page defragment incorrectly exposed as live");

  const auto& filespace = Record(by_agent, "filespace_capacity_manager");
  Require(filespace.exposure ==
              agents::AgentProductionExposureClass::recommendation_only,
          "filespace capacity manager should be recommendation/capacity-window only");
  Require(!filespace.route_evidence_kind.empty(),
          "filespace capacity handoff evidence classification missing");
  Require(!filespace.physical_mutation_route_proven,
          "filespace capacity manager should not claim physical mutation route");
  Require(!Action(filespace, "request_filespace_expand").live_route_available,
          "filespace expansion request incorrectly exposed as live");

  const auto& health = Record(by_agent, "storage_health_manager");
  Require(health.exposure ==
              agents::AgentProductionExposureClass::recommendation_only,
          "storage health manager should be evidence/recommendation only");
  Require(!Action(health, "request_filespace_quarantine").live_route_available,
          "storage health quarantine request incorrectly exposed as live");

  const auto& cleanup = Record(by_agent, "storage_version_cleanup_agent");
  Require(cleanup.exposure == agents::AgentProductionExposureClass::dry_run_only,
          "storage version cleanup should remain dry-run only without live route proof");
  Require(!Action(cleanup, "cleanup_storage_versions").live_route_available,
          "storage version cleanup incorrectly exposed as live");
}

void TestRouteProofCannotBecomeLiveFromIncompleteProviderEvidence() {
  agents::AgentProductionRouteProofInputs inputs;
  auto proof = PagePreallocationProof();
  proof.handler_evidence_uuid.clear();
  inputs.route_proofs.push_back(proof);
  const auto status = agents::ValidateAgentProductionExposureMatrix(inputs);
  Require(!status.ok &&
              status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_EXPOSURE.ROUTE_PROOF_INVALID",
          "incomplete route proof passed production exposure validation");
  const auto by_agent = ExposureByAgent(inputs);
  const auto& page = Record(by_agent, "page_allocation_manager");
  Require(!page.production_live_route_available,
          "incomplete provider proof exposed page allocation as live");
  Require(!Action(page, "preallocate_page_family").live_route_available,
          "incomplete provider proof exposed preallocation action as live");
}

void TestRouteProofRequiresCompleteLiveProofFields() {
  auto require_rejected = [](const std::string& label, auto mutate) {
    agents::AgentProductionRouteProofInputs inputs;
    auto proof = PagePreallocationProof();
    mutate(&proof);
    inputs.route_proofs.push_back(proof);
    const auto status = agents::ValidateAgentProductionExposureMatrix(inputs);
    Require(!status.ok &&
                status.diagnostic_code ==
                    "SB_AGENT_PRODUCTION_EXPOSURE.ROUTE_PROOF_INVALID",
            "invalid live route proof was accepted: " + label);
    const auto by_agent = ExposureByAgent(inputs);
    const auto& page = Record(by_agent, "page_allocation_manager");
    Require(!page.production_live_route_available,
            "invalid live route proof exposed page allocation: " + label);
    Require(!Action(page, "preallocate_page_family").live_route_available,
            "invalid live route proof exposed action: " + label);
  };

  require_rejected("provider_id",
                   [](auto* proof) { proof->provider_id.clear(); });
  require_rejected("actuator_id",
                   [](auto* proof) { proof->actuator_id = "storage_manager"; });
  require_rejected("authority_domain",
                   [](auto* proof) {
                     proof->authority_domain =
                         agents::AgentActuatorAuthorityDomain::storage;
                   });
  require_rejected("subsystem_handler_id",
                   [](auto* proof) { proof->subsystem_handler_id.clear(); });
  require_rejected("handler_provenance",
                   [](auto* proof) { proof->handler_provenance.clear(); });
  require_rejected("handler_evidence_uuid",
                   [](auto* proof) { proof->handler_evidence_uuid.clear(); });
  require_rejected("real_subsystem_handler",
                   [](auto* proof) { proof->real_subsystem_handler = false; });
  require_rejected("idempotency",
                   [](auto* proof) { proof->idempotent = false; });
  require_rejected("retry",
                   [](auto* proof) { proof->supports_retry = false; });
  require_rejected("compensation",
                   [](auto* proof) {
                     proof->supports_rollback_compensation = false;
                   });
  require_rejected("outcome_verification",
                   [](auto* proof) {
                     proof->requires_outcome_verification = false;
                   });
}

void TestAnchorOnlyAndClusterFamiliesBlocked() {
  const auto by_agent = ExposureByAgent();
  for (const std::string agent : {
           "backup_manager",
           "archive_manager",
           "restore_drill_manager",
           "pitr_manager",
           "export_adapter_manager",
           "identity_manager",
           "session_control_manager",
           "job_control_manager"}) {
    const auto& record = Record(by_agent, agent);
    Require(!record.implementation_anchor_only,
            "enterprise local workflow still marked anchor-only: " + agent);
    Require(!record.route_evidence_kind.empty(),
            "enterprise local workflow route classification missing: " + agent);
    Require(!record.production_live_route_available,
            "enterprise local workflow exposed live route without actuator proof: " +
                agent);
    Require(record.workflow_route_proven,
            "enterprise local workflow proof missing: " + agent);
    Require(record.exposure ==
                agents::AgentProductionExposureClass::workflow_only,
            "enterprise local workflow not classified workflow-only: " + agent);
    Require(Contains(record.route_evidence_fields, "workflow_route_proven=true"),
            "enterprise local workflow evidence missing: " + agent);
    Require(record.diagnostic_code ==
                "SB_AGENT_PRODUCTION_EXPOSURE.LOCAL_WORKFLOW_HANDLER_PROVEN_LIVE_ACTUATOR_REQUIRED",
            "local workflow diagnostic mismatch: " + agent);
  }

  for (const std::string agent : {
           "cluster_autoscale_manager",
           "distributed_query_metrics_agent",
           "remote_query_routing_agent",
           "cluster_scheduler_manager",
           "cluster_upgrade_manager"}) {
    const auto& record = Record(by_agent, agent);
    Require(record.cluster_only, "cluster-only flag missing: " + agent);
    Require(record.cluster_provider_authority_required,
            "cluster provider requirement missing: " + agent);
    Require(!record.cluster_provider_authority_available,
            "test unexpectedly has cluster provider authority: " + agent);
    Require(record.exposure ==
                agents::AgentProductionExposureClass::disabled_blocked,
            "cluster agent was not blocked: " + agent);
    Require(record.diagnostic_code ==
                "SB_AGENT_PRODUCTION_EXPOSURE.CLUSTER_PROVIDER_REQUIRED",
            "cluster diagnostic mismatch: " + agent);
  }

  agents::AgentProductionRouteProofInputs external_cluster_inputs;
  external_cluster_inputs.real_cluster_provider_authority = true;
  const auto external_cluster_by_agent = ExposureByAgent(external_cluster_inputs);
  for (const std::string agent : {
           "cluster_autoscale_manager",
           "distributed_query_metrics_agent",
           "remote_query_routing_agent",
           "cluster_scheduler_manager",
           "cluster_upgrade_manager"}) {
    const auto& record = Record(external_cluster_by_agent, agent);
    Require(record.cluster_provider_authority_available,
            "cluster provider authority flag missing: " + agent);
    Require(record.exposure ==
                agents::AgentProductionExposureClass::disabled_blocked,
            "external provider authority exposed local cluster live route: " +
                agent);
    Require(!record.production_surface_visible,
            "cluster agent became locally visible without classified provider route: " +
                agent);
    Require(!record.production_live_route_available,
            "cluster agent exposed local live route under provider authority: " +
                agent);
  }
}

void TestActionContractPresenceDoesNotImplyLiveRoute() {
  const auto backup_start = agents::ClassifyAgentProductionActionExposure(
      "backup_manager", "start_backup");
  Require(backup_start.action_contract_present,
          "backup action contract should be present");
  Require(!backup_start.action_contract_implies_live_route,
          "backup action contract implied live route");
  Require(!backup_start.live_route_available,
          "backup start action incorrectly exposed as live");
  Require(backup_start.exposure ==
              agents::AgentProductionExposureClass::workflow_only,
          "backup start action should remain workflow-only without live proof");
  Require(backup_start.workflow_route_available,
          "backup start action lacks workflow route proof");
  Require(backup_start.diagnostic_code ==
              "SB_AGENT_PRODUCTION_EXPOSURE.ACTION_WORKFLOW_ONLY",
          "backup start action workflow-only diagnostic mismatch");

  const auto page_relocate = agents::ClassifyAgentProductionActionExposure(
      "page_allocation_manager", "relocate_pages");
  Require(page_relocate.action_contract_present,
          "page relocation contract should be present");
  Require(!page_relocate.live_route_available,
          "page relocation contract incorrectly became live");
  Require(page_relocate.exposure ==
              agents::AgentProductionExposureClass::recommendation_only,
          "page relocation should be recommendation-only");

  const auto provider_registry = agents::DefaultAgentActuatorProviderRegistry();
  const auto backup_provider =
      provider_registry.Find("backup_subsystem", "start_backup");
  Require(backup_provider.has_value(), "backup provider descriptor missing");
  Require(!backup_provider->live_route_available,
          "default provider overclaimed backup live route");
  const auto page_provider =
      provider_registry.Find("page_manager", "preallocate_page_family");
  Require(page_provider.has_value(), "page preallocation provider missing");
  Require(!page_provider->live_route_available,
          "default provider exposed page preallocation without proof");
}

}  // namespace

int main() {
  TestAllCanonicalAgentsClassified();
  TestStorageClassifications();
  TestRouteProofCannotBecomeLiveFromIncompleteProviderEvidence();
  TestRouteProofRequiresCompleteLiveProofFields();
  TestAnchorOnlyAndClusterFamiliesBlocked();
  TestActionContractPresenceDoesNotImplyLiveRoute();
  return EXIT_SUCCESS;
}
