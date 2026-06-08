// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_PER_AGENT_LIVE_RECOMMEND_DISABLED_CLASSIFICATION
// SEARCH_KEY: ARHC_STORAGE_PAGE_FILESPACE_AGENT_INTEGRATION
// SEARCH_KEY: ARHC_BACKUP_ARCHIVE_PITR_RESTORE_AGENT_INTEGRATION
// SEARCH_KEY: ARHC_IDENTITY_SESSION_JOB_CLUSTER_AGENT_INTEGRATION

#include "agent_runtime.hpp"
#include "agent_runtime_manifest.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentProductionExposureClass {
  live_action,
  recommendation_only,
  dry_run_only,
  workflow_only,
  disabled_blocked
};

struct AgentProductionRouteProofInputs {
  struct RouteProof {
    std::string agent_type_id;
    std::string action_id;
    std::string provider_id;
    std::string actuator_id;
    AgentActuatorAuthorityDomain authority_domain =
        AgentActuatorAuthorityDomain::unknown;
    std::string subsystem_handler_id;
    std::string handler_provenance;
    std::string handler_evidence_uuid;
    bool live_route_available = false;
    bool real_subsystem_handler = false;
    bool idempotent = false;
    bool supports_retry = false;
    bool supports_rollback_compensation = false;
    bool requires_outcome_verification = true;
    bool physical_mutation_route = false;
    bool external_cluster_provider = false;
  };
  std::vector<RouteProof> route_proofs;
  bool real_cluster_provider_authority = false;
};

struct AgentProductionAuthoritySafety {
  bool parser_authority = false;
  bool client_authority = false;
  bool donor_authority = false;
  bool sidecar_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
};

struct AgentProductionActionExposure {
  std::string agent_type_id;
  std::string action_id;
  std::string actuator_id;
  AgentProductionExposureClass exposure =
      AgentProductionExposureClass::disabled_blocked;
  bool action_contract_present = false;
  bool action_contract_implies_live_route = false;
  bool live_route_available = false;
  bool dry_run_route_available = false;
  bool recommendation_route_available = false;
  bool workflow_route_available = false;
  std::string diagnostic_code;
  std::string route_evidence_kind;
  std::vector<std::string> route_evidence_fields;
  AgentProductionAuthoritySafety authority_safety;
};

struct AgentProductionExposureRecord {
  std::string agent_type_id;
  std::string implementation_anchor;
  AgentDeployment deployment = AgentDeployment::local;
  AgentAuthorityClass authority = AgentAuthorityClass::observe_only;
  AgentActivationProfile manifest_default_activation =
      AgentActivationProfile::observe_only;
  AgentProductionExposureClass exposure =
      AgentProductionExposureClass::disabled_blocked;
  bool source_manifest_present = false;
  bool cluster_only = false;
  bool cluster_provider_authority_required = false;
  bool cluster_provider_authority_available = false;
  bool implementation_anchor_only = false;
  bool real_subsystem_route_proven = false;
  bool workflow_route_proven = false;
  bool physical_mutation_route_proven = false;
  bool production_live_route_available = false;
  bool production_surface_visible = true;
  bool action_contract_present = false;
  bool action_contract_implies_live_route = false;
  std::string diagnostic_code;
  std::string route_evidence_kind;
  std::vector<std::string> route_evidence_fields;
  std::vector<AgentProductionActionExposure> actions;
  AgentProductionAuthoritySafety authority_safety;
};

const char* AgentProductionExposureClassName(AgentProductionExposureClass value);
AgentProductionExposureRecord ClassifyCanonicalAgentProductionExposure(
    const CanonicalAgentManifestEntry& entry,
    const AgentProductionRouteProofInputs& route_inputs =
        AgentProductionRouteProofInputs{});
std::vector<AgentProductionExposureRecord>
ClassifyAllCanonicalAgentProductionExposures(
    const AgentProductionRouteProofInputs& route_inputs =
        AgentProductionRouteProofInputs{});
AgentProductionActionExposure ClassifyAgentProductionActionExposure(
    const std::string& agent_type_id,
    const std::string& action_id,
    const AgentProductionRouteProofInputs& route_inputs =
        AgentProductionRouteProofInputs{});
AgentRuntimeStatus ValidateAgentProductionExposureMatrix(
    const AgentProductionRouteProofInputs& route_inputs =
        AgentProductionRouteProofInputs{});

}  // namespace scratchbird::core::agents
