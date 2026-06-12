// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_RUNTIME_MANAGER_AUTHORITY_PROVENANCE
// SEARCH_KEY: ARHC_ACTUATOR_PROVIDER_REGISTRY
// SEARCH_KEY: ARHC_ACTION_DISPATCH_IDEMPOTENCY_OUTCOME

#include "agent_durable_catalog.hpp"
#include "agent_metric_runtime.hpp"
#include "agent_action_safety.hpp"
#include "agent_manual_approval.hpp"
#include "agent_package_provenance.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentActionAuthoritySource {
  none,
  sealed_internal_bootstrap,
  operator_request
};

struct AgentActionAuthorityProvenance {
  AgentActionAuthoritySource source = AgentActionAuthoritySource::none;
  std::string principal_uuid;
  std::string scope_uuid;
  std::string provenance_evidence_uuid;
  std::vector<std::string> rights;
  bool sealed_bootstrap_authority = false;
  bool operator_authority = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool reference_authority = false;
  bool sidecar_authority = false;
};

struct AgentActuatorProviderDescriptor {
  std::string provider_id;
  std::string owning_agent;
  std::string actuator_id;
  std::string operation_id;
  AgentActuatorAuthorityDomain authority_domain = AgentActuatorAuthorityDomain::unknown;
  bool supports_dry_run = true;
  bool live_route_available = false;
  bool real_subsystem_handler = false;
  bool fixture_or_simulated_provider = false;
  bool idempotent = true;
  bool supports_retry = false;
  bool supports_rollback_compensation = false;
  bool requires_outcome_verification = true;
  std::string subsystem_handler_id;
  std::string handler_provenance;
  std::string handler_evidence_uuid;
  std::vector<std::string> required_evidence_fields;
  AgentPackageProvenanceBundle package_provenance;
};

struct AgentActuatorProviderExecutionContext {
  bool engine_owned_registry = false;
  bool durable_catalog_store_context = false;
  bool engine_request_context_present = false;
  bool fsync_or_checkpoint_evidence = false;
  std::string request_id;
  std::string database_uuid;
  std::string transaction_uuid;
  u64 local_transaction_id = 0;
  std::string registry_provenance;
  std::string registry_evidence_uuid;
};

struct AgentActuatorProviderRequest {
  AgentActionRequest action;
  AgentActionAuthorityProvenance authority;
  AgentActuatorProviderExecutionContext execution_context;
  std::string input_evidence_digest;
  bool dry_run = true;
  bool subsystem_reported_success = true;
  bool intended_state_observed = true;
};

struct AgentActuatorProviderResult {
  AgentRuntimeStatus status;
  bool dispatched = false;
  bool dry_run = false;
  bool mutation_attempted = false;
  bool outcome_verified = false;
  bool compensation_required = false;
  bool compensation_attempted = false;
  std::string verification_evidence_uuid;
  std::string compensation_executor_id;
  std::string compensation_evidence_uuid;
  bool retry_scheduled = false;
  u64 retry_after_microseconds = 0;
  std::string retry_evidence_uuid;
};

using AgentActuatorProviderExecute =
    std::function<AgentActuatorProviderResult(const AgentActuatorProviderRequest&)>;

class AgentActuatorProviderRegistry {
 public:
  AgentRuntimeStatus Register(AgentActuatorProviderDescriptor descriptor,
                              AgentActuatorProviderExecute execute = {});
  std::optional<AgentActuatorProviderDescriptor> Find(
      const std::string& owning_agent,
      const std::string& actuator_id,
      const std::string& operation_id) const;
  std::optional<AgentActuatorProviderDescriptor> Find(
      const std::string& actuator_id,
      const std::string& operation_id) const;
  AgentActuatorProviderResult Execute(
      const AgentActuatorProviderDescriptor& descriptor,
      const AgentActuatorProviderRequest& request) const;
  std::vector<AgentActuatorProviderDescriptor> ListDescriptors() const;

 private:
  struct Entry {
    AgentActuatorProviderDescriptor descriptor;
    AgentActuatorProviderExecute execute;
  };
  std::vector<Entry> entries_;
};

struct AgentActionDispatchRequest {
  DurableAgentCatalogImage* catalog = nullptr;
  AgentActionRequest action;
  AgentActionAuthorityProvenance authority;
  const AgentActuatorProviderRegistry* registry = nullptr;
  AgentRuntimeContext metric_context;
  AgentMetricSnapshotEvaluationOptions metric_snapshot_options;
  std::vector<AgentObservedMetricSnapshot> observed_metric_snapshots;
  AgentActuatorProviderExecutionContext provider_execution_context;
  bool production_live_path = true;
  bool subsystem_reported_success = true;
  bool intended_state_observed = true;
};

struct AgentActionDispatchResult {
  AgentRuntimeStatus status;
  AgentActionDecision decision;
  DurableAgentActionRecord action_record;
  std::string input_evidence_digest;
  AgentActionSafetyEnvelope safety_envelope;
  AgentManualApprovalWorkflowEvaluation approval_workflow;
  bool duplicate_idempotency_key = false;
  bool provider_dispatched = false;
  bool durable_record_written = false;
  bool commercial_evidence_written_before_action_record = false;
  bool safety_envelope_validated = false;
  bool approval_workflow_validated = false;
  bool package_provenance_validated = false;
  bool dry_run = false;
  bool outcome_verified = false;
  bool compensation_required = false;
  bool quarantined_or_replay_pending = false;
};

const char* AgentActionAuthoritySourceName(AgentActionAuthoritySource source);
AgentRuntimeStatus ValidateAgentActionAuthorityProvenance(
    const AgentActionAuthorityProvenance& provenance);
AgentActuatorProviderRegistry DefaultAgentActuatorProviderRegistry();
std::string AgentActionInputEvidenceDigest(const AgentActionRequest& action);
AgentRuntimeStatus ValidateAgentActionPolicyGeneration(
    const DurableAgentCatalogImage& catalog,
    const AgentActionRequest& action);
AgentActionDispatchResult DispatchAgentAction(
    const AgentActionDispatchRequest& request);

}  // namespace scratchbird::core::agents
