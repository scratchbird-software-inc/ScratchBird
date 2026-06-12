// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_production_classification.hpp"
#include "agent_runtime.hpp"
#include "agent_runtime_manifest.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

struct MatrixRow {
  std::string scenario;
  std::string agent_type_id;
  std::string deployment;
  std::string scope;
  std::string authority;
  std::string manifest_default_activation;
  std::string readiness_status;
  std::string exposure_class;
  std::string implementation_anchor;
  std::string diagnostic_code;
  std::string route_evidence_kind;
  std::string action_statuses;
  std::size_t action_contract_count = 0;
  bool source_manifest_present = false;
  bool implementation_anchor_only = false;
  bool cluster_provider_required = false;
  bool cluster_provider_authority_available = false;
  bool cluster_provider_stub = false;
  bool local_evaluator_status = false;
  bool recommendation_status = false;
  bool dry_run_status = false;
  bool workflow_status = false;
  bool ledger_status = false;
  bool actuator_required_status = false;
  bool production_live_enabled = false;
  bool production_surface_visible = false;
  bool real_subsystem_route_proven = false;
  bool workflow_route_proven = false;
  bool physical_mutation_route_proven = false;
  bool action_contract_implies_live_route = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool reference_authority = false;
  bool sidecar_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

const char* BoolText(bool value) {
  return value ? "true" : "false";
}

std::string Csv(std::string_view value) {
  const bool quote = value.find_first_of(",\"\n") != std::string_view::npos;
  if (!quote) {
    return std::string(value);
  }
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"') {
      out += "\"\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

std::string JoinActionStatuses(
    const std::vector<agents::AgentProductionActionExposure>& actions) {
  std::string out;
  for (const auto& action : actions) {
    if (!out.empty()) {
      out += '|';
    }
    out += action.action_id;
    out += ':';
    out += agents::AgentProductionExposureClassName(action.exposure);
    out += ':';
    out += action.diagnostic_code;
  }
  return out;
}

bool Contains(std::string_view value, std::string_view token) {
  return value.find(token) != std::string_view::npos;
}

std::string ReadinessStatus(
    const agents::AgentProductionExposureRecord& record) {
  if (record.production_live_route_available &&
      record.real_subsystem_route_proven &&
      record.exposure == agents::AgentProductionExposureClass::live_action) {
    return "production_live_enabled";
  }
  if (record.cluster_provider_authority_required &&
      !record.cluster_provider_authority_available) {
    return "cluster_provider_stub";
  }
  if (record.exposure == agents::AgentProductionExposureClass::workflow_only) {
    return "workflow_ledger_actuator_required";
  }
  if (record.exposure == agents::AgentProductionExposureClass::dry_run_only) {
    return "dry_run_evaluator";
  }
  if (record.exposure ==
      agents::AgentProductionExposureClass::recommendation_only) {
    return "recommendation_evaluator";
  }
  return "disabled_blocked";
}

bool HasForbiddenAuthority(
    const agents::AgentProductionAuthoritySafety& authority) {
  return authority.parser_authority ||
         authority.client_authority ||
         authority.reference_authority ||
         authority.sidecar_authority ||
         authority.transaction_finality_authority ||
         authority.visibility_authority ||
         authority.recovery_authority;
}

MatrixRow BuildMatrixRow(
    std::string scenario,
    const agents::AgentProductionExposureRecord& record) {
  const std::string readiness_status = ReadinessStatus(record);
  MatrixRow row;
  row.scenario = std::move(scenario);
  row.agent_type_id = record.agent_type_id;
  row.deployment = agents::AgentDeploymentName(record.deployment);
  row.scope = record.agent_type_id.empty() ? "" : record.implementation_anchor;
  for (const auto& entry : agents::CanonicalAgentManifest()) {
    if (entry.type_id == record.agent_type_id) {
      row.scope = entry.scope;
      break;
    }
  }
  row.authority = agents::AgentAuthorityClassName(record.authority);
  row.manifest_default_activation =
      agents::AgentActivationProfileName(record.manifest_default_activation);
  row.readiness_status = readiness_status;
  row.exposure_class =
      agents::AgentProductionExposureClassName(record.exposure);
  row.implementation_anchor = record.implementation_anchor;
  row.diagnostic_code = record.diagnostic_code;
  row.route_evidence_kind = record.route_evidence_kind;
  row.action_statuses = JoinActionStatuses(record.actions);
  row.action_contract_count = record.actions.size();
  row.source_manifest_present = record.source_manifest_present;
  row.implementation_anchor_only = record.implementation_anchor_only;
  row.cluster_provider_required =
      record.cluster_provider_authority_required;
  row.cluster_provider_authority_available =
      record.cluster_provider_authority_available;
  row.cluster_provider_stub = readiness_status == "cluster_provider_stub";
  row.local_evaluator_status =
      Contains(record.route_evidence_kind, "evaluator") ||
      Contains(readiness_status, "evaluator");
  row.recommendation_status =
      readiness_status == "recommendation_evaluator";
  row.dry_run_status = readiness_status == "dry_run_evaluator";
  row.workflow_status =
      readiness_status == "workflow_ledger_actuator_required";
  row.ledger_status = row.workflow_status;
  row.actuator_required_status =
      row.workflow_status ||
      (record.action_contract_present &&
       !record.production_live_route_available);
  row.production_live_enabled =
      readiness_status == "production_live_enabled";
  row.production_surface_visible = record.production_surface_visible;
  row.real_subsystem_route_proven = record.real_subsystem_route_proven;
  row.workflow_route_proven = record.workflow_route_proven;
  row.physical_mutation_route_proven = record.physical_mutation_route_proven;
  row.action_contract_implies_live_route =
      record.action_contract_implies_live_route;
  row.parser_authority = record.authority_safety.parser_authority;
  row.client_authority = record.authority_safety.client_authority;
  row.reference_authority = record.authority_safety.reference_authority;
  row.sidecar_authority = record.authority_safety.sidecar_authority;
  row.transaction_finality_authority =
      record.authority_safety.transaction_finality_authority;
  row.visibility_authority = record.authority_safety.visibility_authority;
  row.recovery_authority = record.authority_safety.recovery_authority;
  return row;
}

agents::AgentProductionRouteProofInputs LocalLiveRouteProofInputs() {
  agents::AgentProductionRouteProofInputs inputs;
  const auto contract = agents::FindAgentActionContract(
      "page_allocation_manager", "preallocate_page_family");
  Require(contract.has_value(),
          "PCR-080 page allocation action contract should exist");

  agents::AgentProductionRouteProofInputs::RouteProof proof;
  proof.agent_type_id = contract->owning_agent;
  proof.action_id = contract->action_id;
  proof.provider_id = "local_storage_page_preallocator";
  proof.actuator_id = contract->actuator;
  proof.authority_domain =
      agents::ActuatorAuthorityDomainForId(contract->actuator);
  proof.subsystem_handler_id = "storage_page_preallocation_handler";
  proof.handler_provenance = "public_local_page_preallocation_route";
  proof.handler_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "PCR-080|page_allocation_manager|preallocate_page_family");
  proof.live_route_available = true;
  proof.real_subsystem_handler = true;
  proof.idempotent = true;
  proof.supports_retry = true;
  proof.supports_rollback_compensation = true;
  proof.requires_outcome_verification = true;
  proof.physical_mutation_route = true;
  proof.external_cluster_provider = false;
  inputs.route_proofs.push_back(std::move(proof));
  return inputs;
}

std::vector<MatrixRow> BuildRows(
    const std::string& scenario,
    const agents::AgentProductionRouteProofInputs& inputs) {
  const auto status = agents::ValidateAgentProductionExposureMatrix(inputs);
  Require(status.ok, "PCR-080 production exposure matrix should validate");

  std::vector<MatrixRow> rows;
  for (const auto& record :
       agents::ClassifyAllCanonicalAgentProductionExposures(inputs)) {
    rows.push_back(BuildMatrixRow(scenario, record));
  }
  return rows;
}

void ValidateRows(const std::vector<MatrixRow>& rows,
                  bool expect_live_route_row) {
  Require(rows.size() == agents::CanonicalAgentManifestCount(),
          "PCR-080 matrix row count should match canonical manifest");

  std::set<std::string> seen_agents;
  std::set<std::string> statuses;
  std::size_t action_contract_rows = 0;
  for (const auto& row : rows) {
    Require(!row.agent_type_id.empty(),
            "PCR-080 matrix agent id should be populated");
    Require(seen_agents.insert(row.agent_type_id).second,
            "PCR-080 matrix should not duplicate agents per scenario");
    Require(row.source_manifest_present,
            "PCR-080 matrix row should come from canonical manifest");
    Require(!row.implementation_anchor.empty(),
            "PCR-080 matrix row should name implementation anchor");
    Require(!row.action_contract_implies_live_route,
            "PCR-080 action contracts must not imply live routes");
    Require(!row.parser_authority && !row.client_authority &&
                !row.reference_authority && !row.sidecar_authority &&
                !row.transaction_finality_authority &&
                !row.visibility_authority && !row.recovery_authority,
            "PCR-080 agent readiness must not claim engine authority");
    if (row.action_contract_count != 0) {
      ++action_contract_rows;
      Require(!row.action_statuses.empty(),
              "PCR-080 action contract rows should expose action statuses");
    }
    if (row.cluster_provider_stub) {
      Require(row.cluster_provider_required &&
                  !row.cluster_provider_authority_available,
              "PCR-080 cluster provider stub row should fail closed locally");
      Require(!row.production_surface_visible,
              "PCR-080 cluster-only agent should not expose local production surface");
    }
    if (row.production_live_enabled) {
      Require(row.real_subsystem_route_proven,
              "PCR-080 live row requires real subsystem route proof");
      Require(row.physical_mutation_route_proven,
              "PCR-080 live storage route should prove physical mutation route");
      Require(!row.cluster_provider_required,
              "PCR-080 local live row must not be cluster-provider-owned");
    }
    statuses.insert(row.readiness_status);
  }

  Require(seen_agents.size() == agents::CanonicalAgentManifestCount(),
          "PCR-080 canonical agent coverage mismatch");
  Require(action_contract_rows != 0,
          "PCR-080 matrix should include action contract status coverage");
  Require(statuses.count("cluster_provider_stub") != 0,
          "PCR-080 matrix missing cluster-provider-stub status");
  Require(statuses.count("dry_run_evaluator") != 0,
          "PCR-080 matrix missing dry-run evaluator status");
  Require(statuses.count("recommendation_evaluator") != 0,
          "PCR-080 matrix missing recommendation evaluator status");
  Require(statuses.count("workflow_ledger_actuator_required") != 0,
          "PCR-080 matrix missing workflow ledger actuator-required status");
  if (expect_live_route_row) {
    Require(statuses.count("production_live_enabled") != 0,
            "PCR-080 proofed matrix missing production-live-enabled status");
  } else {
    Require(statuses.count("production_live_enabled") == 0,
            "PCR-080 default matrix overclaimed production-live-enabled status");
  }
}

void ValidateContractsCovered(
    const std::vector<agents::AgentProductionExposureRecord>& records) {
  std::map<std::string, std::set<std::string>> action_status_by_agent;
  for (const auto& record : records) {
    for (const auto& action : record.actions) {
      Require(action.action_contract_present,
              "PCR-080 classified action should keep contract marker");
      Require(!action.action_contract_implies_live_route,
              "PCR-080 classified action should not imply live route");
      Require(!HasForbiddenAuthority(action.authority_safety),
              "PCR-080 action status should not claim engine authority");
      action_status_by_agent[record.agent_type_id].insert(action.action_id);
    }
  }

  for (const auto& contract : agents::AgentActionContractRegistry()) {
    const auto agent = action_status_by_agent.find(contract.owning_agent);
    Require(agent != action_status_by_agent.end(),
            "PCR-080 action contract owner missing from matrix");
    Require(agent->second.count(contract.action_id) != 0,
            "PCR-080 action contract missing from action status list");
  }
}

void WriteMatrix(const std::filesystem::path& path,
                 const std::vector<MatrixRow>& rows) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(out.good(), "PCR-080 could not open agent readiness matrix output");
  out << "scenario,agent_type_id,deployment,scope,authority,"
         "manifest_default_activation,readiness_status,exposure_class,"
         "implementation_anchor,diagnostic_code,route_evidence_kind,"
         "action_statuses,action_contract_count,source_manifest_present,"
         "implementation_anchor_only,cluster_provider_required,"
         "cluster_provider_authority_available,cluster_provider_stub,"
         "local_evaluator_status,recommendation_status,dry_run_status,"
         "workflow_status,ledger_status,actuator_required_status,"
         "production_live_enabled,production_surface_visible,"
         "real_subsystem_route_proven,workflow_route_proven,"
         "physical_mutation_route_proven,action_contract_implies_live_route,"
         "parser_authority,client_authority,reference_authority,sidecar_authority,"
         "transaction_finality_authority,visibility_authority,"
         "recovery_authority\n";
  for (const auto& row : rows) {
    out << Csv(row.scenario) << ','
        << Csv(row.agent_type_id) << ','
        << Csv(row.deployment) << ','
        << Csv(row.scope) << ','
        << Csv(row.authority) << ','
        << Csv(row.manifest_default_activation) << ','
        << Csv(row.readiness_status) << ','
        << Csv(row.exposure_class) << ','
        << Csv(row.implementation_anchor) << ','
        << Csv(row.diagnostic_code) << ','
        << Csv(row.route_evidence_kind) << ','
        << Csv(row.action_statuses) << ','
        << row.action_contract_count << ','
        << BoolText(row.source_manifest_present) << ','
        << BoolText(row.implementation_anchor_only) << ','
        << BoolText(row.cluster_provider_required) << ','
        << BoolText(row.cluster_provider_authority_available) << ','
        << BoolText(row.cluster_provider_stub) << ','
        << BoolText(row.local_evaluator_status) << ','
        << BoolText(row.recommendation_status) << ','
        << BoolText(row.dry_run_status) << ','
        << BoolText(row.workflow_status) << ','
        << BoolText(row.ledger_status) << ','
        << BoolText(row.actuator_required_status) << ','
        << BoolText(row.production_live_enabled) << ','
        << BoolText(row.production_surface_visible) << ','
        << BoolText(row.real_subsystem_route_proven) << ','
        << BoolText(row.workflow_route_proven) << ','
        << BoolText(row.physical_mutation_route_proven) << ','
        << BoolText(row.action_contract_implies_live_route) << ','
        << BoolText(row.parser_authority) << ','
        << BoolText(row.client_authority) << ','
        << BoolText(row.reference_authority) << ','
        << BoolText(row.sidecar_authority) << ','
        << BoolText(row.transaction_finality_authority) << ','
        << BoolText(row.visibility_authority) << ','
        << BoolText(row.recovery_authority) << '\n';
  }
  out.close();
  Require(out.good(), "PCR-080 could not write agent readiness matrix");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_agent_readiness_matrix_gate <matrix-csv>\n";
    return EXIT_FAILURE;
  }

  Require(agents::ValidateCanonicalAgentRegistry().ok,
          "PCR-080 canonical agent registry should validate");
  Require(agents::ValidateAgentActuatorAuthorityRegistry().ok,
          "PCR-080 actuator authority registry should validate");
  Require(agents::CanonicalAgentManifestCount() == 29,
          "PCR-080 canonical manifest should contain 29 agents");

  agents::AgentProductionRouteProofInputs default_inputs;
  const auto default_rows = BuildRows("default_local_no_cluster_provider",
                                      default_inputs);
  ValidateRows(default_rows, false);
  ValidateContractsCovered(
      agents::ClassifyAllCanonicalAgentProductionExposures(default_inputs));

  const auto live_inputs = LocalLiveRouteProofInputs();
  const auto live_rows = BuildRows("local_live_route_proof",
                                   live_inputs);
  ValidateRows(live_rows, true);

  std::vector<MatrixRow> all_rows = default_rows;
  all_rows.insert(all_rows.end(), live_rows.begin(), live_rows.end());
  WriteMatrix(argv[1], all_rows);

  std::cout << "public_agent_readiness_matrix_gate=passed rows="
            << all_rows.size() << '\n';
  return EXIT_SUCCESS;
}
