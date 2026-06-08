// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_action_dispatch.hpp"

#include "agent_commercial_evidence.hpp"
#include "agent_production_classification.hpp"
#include "agent_production_fixture_separation.hpp"

#include <algorithm>
#include <iomanip>
#include <openssl/sha.h>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::string HexBytes(const unsigned char* bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

std::string Sha256Digest(const std::string& payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
         payload.size(),
         digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

std::string ProviderKey(const std::string& owning_agent,
                        const std::string& actuator_id,
                        const std::string& operation_id) {
  return owning_agent + "|" + actuator_id + "|" + operation_id;
}

AgentActuatorProviderResult DefaultProviderExecute(
    const AgentActuatorProviderDescriptor& descriptor,
    const AgentActuatorProviderRequest& request) {
  AgentActuatorProviderResult result;
  result.dry_run = request.dry_run;
  if (request.dry_run) {
    if (!descriptor.supports_dry_run) {
      result.status = AgentError("SB_AGENT_ACTUATOR_PROVIDER.DRY_RUN_UNSUPPORTED",
                                 descriptor.provider_id);
      return result;
    }
    result.status = {true, "SB_AGENT_ACTION.DRY_RUN_ONLY", descriptor.provider_id};
    result.outcome_verified = true;
    result.verification_evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
        "agent_action_dry_run_verification|" + request.action.action_uuid);
    return result;
  }
  if (!descriptor.live_route_available) {
    result.status = AgentError("SB_AGENT_ACTUATOR_PROVIDER.LIVE_ROUTE_UNAVAILABLE",
                               descriptor.provider_id);
    return result;
  }
  result.dispatched = true;
  result.mutation_attempted = true;
  result.outcome_verified =
      !descriptor.requires_outcome_verification ||
      (request.subsystem_reported_success && request.intended_state_observed);
  result.verification_evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_action_outcome_verification|" + request.action.action_uuid + "|" +
      descriptor.provider_id);
  if (!result.outcome_verified) {
    result.compensation_required = true;
    result.compensation_attempted = descriptor.supports_rollback_compensation;
    result.status = AgentError(
        descriptor.supports_rollback_compensation
            ? "SB_AGENT_ACTION.OUTCOME_UNVERIFIED_COMPENSATION_REQUIRED"
            : "SB_AGENT_ACTION.OUTCOME_UNVERIFIED_REPLAY_REQUIRED",
        descriptor.provider_id);
    return result;
  }
  result.status = {true, "SB_AGENT_ACTION.OUTCOME_VERIFIED", descriptor.provider_id};
  return result;
}

DurableAgentActionRecord BuildActionRecord(
    const AgentActionRequest& action,
    const AgentActionAuthorityProvenance& authority,
    const AgentActuatorProviderDescriptor* provider,
    const std::string& digest,
    DurableAgentActionState state,
    const std::string& diagnostic_code,
    const std::string& evidence_uuid,
    const std::string& verification_evidence_uuid,
    bool outcome_verified,
    bool compensation_required,
    bool compensation_attempted,
    bool retry_scheduled,
    u64 retry_after_microseconds,
    const std::string& retry_evidence_uuid,
    const std::string& compensation_executor_id,
    const std::string& compensation_evidence_uuid) {
  DurableAgentActionRecord record;
  record.action_uuid = action.action_uuid;
  record.instance_uuid = action.instance_uuid;
  record.owner_uuid = authority.principal_uuid;
  record.operation_id = action.operation_id;
  record.actuator_provider_id = provider == nullptr ? std::string{} : provider->provider_id;
  record.state = state;
  record.idempotency_key = action.idempotency_key;
  record.input_evidence_digest = digest;
  record.evidence_uuid = evidence_uuid;
  record.verification_evidence_uuid = verification_evidence_uuid;
  record.diagnostic_code = diagnostic_code;
  record.generation = 1;
  record.outcome_verified = outcome_verified;
  record.compensation_required = compensation_required;
  record.compensation_attempted = compensation_attempted;
  record.retry_scheduled = retry_scheduled;
  record.retry_after_microseconds = retry_after_microseconds;
  record.retry_evidence_uuid = retry_evidence_uuid;
  record.compensation_executor_id = compensation_executor_id;
  record.compensation_evidence_uuid = compensation_evidence_uuid;
  record.parser_authority = authority.parser_authority;
  record.client_authority = authority.client_authority;
  record.donor_authority = authority.donor_authority;
  record.sidecar_authority = authority.sidecar_authority;
  return record;
}

u64 PolicyGenerationForAction(const DurableAgentCatalogImage& catalog,
                              const AgentActionRequest& action) {
  for (const auto& instance : catalog.instances) {
    if (instance.instance_uuid == action.instance_uuid &&
        instance.agent_type_id == action.agent_type_id) {
      return instance.policy_generation;
    }
  }
  return 0;
}

std::string ActionInputValue(const AgentActionRequest& action,
                             const std::string& key) {
  const auto it = action.inputs.find(key);
  return it == action.inputs.end() ? std::string() : it->second;
}

std::vector<std::string> ActionScopeUuids(
    const AgentActionAuthorityProvenance& authority,
    const AgentActionRequest& action) {
  std::vector<std::string> scopes;
  if (!authority.scope_uuid.empty()) { scopes.push_back(authority.scope_uuid); }
  const std::string input_scope = ActionInputValue(action, "scope_uuid");
  if (!input_scope.empty()) { scopes.push_back(input_scope); }
  return scopes;
}

AgentActionDispatchResult FinishFailure(std::string code,
                                        std::string detail,
                                        const AgentActionRequest& action) {
  AgentActionDispatchResult result;
  result.status = AgentError(std::move(code), std::move(detail));
  result.decision.result_class = AgentActionResultClass::failed_closed;
  result.decision.diagnostic_code = result.status.diagnostic_code;
  result.decision.detail = result.status.detail;
  result.decision.evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_action_dispatch_refused|" + action.action_uuid + "|" +
      result.status.diagnostic_code);
  result.decision.mutates_state = false;
  return result;
}

AgentRuntimeStatus ValidateProviderExecutionContext(
    const AgentActuatorProviderExecutionContext& context) {
  if (!context.engine_owned_registry ||
      !context.durable_catalog_store_context ||
      !context.engine_request_context_present ||
      context.request_id.empty() ||
      context.database_uuid.empty() ||
      context.transaction_uuid.empty() ||
      context.local_transaction_id == 0 ||
      context.registry_provenance.empty() ||
      context.registry_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_ACTUATOR_PROVIDER.ENGINE_CONTEXT_REQUIRED");
  }
  if (!context.fsync_or_checkpoint_evidence) {
    return AgentError("SB_AGENT_ACTUATOR_PROVIDER.CHECKPOINT_EVIDENCE_REQUIRED");
  }
  return AgentOk();
}

}  // namespace

const char* AgentActionAuthoritySourceName(AgentActionAuthoritySource source) {
  switch (source) {
    case AgentActionAuthoritySource::none: return "none";
    case AgentActionAuthoritySource::sealed_internal_bootstrap:
      return "sealed_internal_bootstrap";
    case AgentActionAuthoritySource::operator_request: return "operator_request";
  }
  return "none";
}

AgentRuntimeStatus ValidateAgentActionAuthorityProvenance(
    const AgentActionAuthorityProvenance& provenance) {
  if (provenance.parser_authority || provenance.client_authority ||
      provenance.donor_authority || provenance.sidecar_authority) {
    return AgentError("SB_AGENT_ACTION_AUTHORITY.UNTRUSTED_SOURCE",
                      AgentActionAuthoritySourceName(provenance.source));
  }
  if (provenance.principal_uuid.empty() || provenance.scope_uuid.empty() ||
      provenance.provenance_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_ACTION_AUTHORITY.PROVENANCE_REQUIRED");
  }
  if (provenance.source == AgentActionAuthoritySource::sealed_internal_bootstrap) {
    if (!provenance.sealed_bootstrap_authority) {
      return AgentError("SB_AGENT_ACTION_AUTHORITY.SEALED_BOOTSTRAP_REQUIRED",
                        provenance.principal_uuid);
    }
    return {true, "SB_AGENT_ACTION_AUTHORITY.SEALED_BOOTSTRAP_ACCEPTED",
            provenance.provenance_evidence_uuid};
  }
  if (provenance.source == AgentActionAuthoritySource::operator_request) {
    if (!provenance.operator_authority ||
        !Contains(provenance.rights, "OBS_AGENT_CONTROL")) {
      return AgentError("SB_AGENT_ACTION_AUTHORITY.OPERATOR_CONTROL_REQUIRED",
                        provenance.principal_uuid);
    }
    return {true, "SB_AGENT_ACTION_AUTHORITY.OPERATOR_ACCEPTED",
            provenance.provenance_evidence_uuid};
  }
  return AgentError("SB_AGENT_ACTION_AUTHORITY.SOURCE_REQUIRED");
}

AgentRuntimeStatus AgentActuatorProviderRegistry::Register(
    AgentActuatorProviderDescriptor descriptor,
    AgentActuatorProviderExecute execute) {
  if (descriptor.provider_id.empty() || descriptor.owning_agent.empty() ||
      descriptor.actuator_id.empty() ||
      descriptor.operation_id.empty()) {
    return AgentError("SB_AGENT_ACTUATOR_PROVIDER.DESCRIPTOR_REQUIRED");
  }
  if (descriptor.authority_domain == AgentActuatorAuthorityDomain::transaction ||
      descriptor.authority_domain == AgentActuatorAuthorityDomain::unknown) {
    return AgentError("SB_AGENT_ACTUATOR_PROVIDER.AUTHORITY_DOMAIN_FORBIDDEN",
                      descriptor.provider_id);
  }
  const auto contract =
      FindAgentActionContract(descriptor.owning_agent,
                              descriptor.operation_id);
  if (!contract.has_value()) {
    return AgentError("SB_AGENT_ACTUATOR_PROVIDER.CONTRACT_REQUIRED",
                      descriptor.owning_agent + ":" +
                          descriptor.operation_id);
  }
  if (contract->actuator != descriptor.actuator_id) {
    return AgentError("SB_AGENT_ACTUATOR_PROVIDER.CONTRACT_ACTUATOR_MISMATCH",
                      descriptor.owning_agent + ":" +
                          descriptor.operation_id);
  }
  const auto expected_domain =
      ActuatorAuthorityDomainForId(contract->actuator);
  if (descriptor.authority_domain != expected_domain) {
    return AgentError("SB_AGENT_ACTUATOR_PROVIDER.AUTHORITY_DOMAIN_MISMATCH",
                      descriptor.provider_id);
  }
  if (descriptor.live_route_available &&
      (contract->cluster_scoped ||
       descriptor.authority_domain ==
           AgentActuatorAuthorityDomain::cluster_provider)) {
    return AgentError(
        "SB_AGENT_ACTUATOR_PROVIDER.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
        descriptor.provider_id);
  }
  if (!descriptor.idempotent) {
    return AgentError("SB_AGENT_ACTUATOR_PROVIDER.IDEMPOTENCY_REQUIRED",
                      descriptor.provider_id);
  }
  if (descriptor.live_route_available) {
    if (descriptor.fixture_or_simulated_provider) {
      return AgentError(
          "SB_AGENT_ACTUATOR_PROVIDER.FIXTURE_PROVIDER_REFUSED",
          descriptor.provider_id);
    }
    if (!descriptor.real_subsystem_handler ||
        descriptor.subsystem_handler_id.empty() ||
        descriptor.handler_provenance.empty() ||
        descriptor.handler_evidence_uuid.empty()) {
      return AgentError(
          "SB_AGENT_ACTUATOR_PROVIDER.REAL_SUBSYSTEM_HANDLER_REQUIRED",
          descriptor.provider_id);
    }
    if (!execute) {
      return AgentError(
          "SB_AGENT_ACTUATOR_PROVIDER.LIVE_EXECUTOR_REQUIRED",
          descriptor.provider_id);
    }
    if (!descriptor.supports_retry) {
      return AgentError(
          "SB_AGENT_ACTUATOR_PROVIDER.RETRY_CAPABILITY_REQUIRED",
          descriptor.provider_id);
    }
    if (!descriptor.supports_rollback_compensation) {
      return AgentError(
          "SB_AGENT_ACTUATOR_PROVIDER.COMPENSATION_CAPABILITY_REQUIRED",
          descriptor.provider_id);
    }
    const auto provenance =
        ValidateAgentPackageProvenanceBundle(descriptor.package_provenance);
    if (!provenance.accepted) {
      return AgentError(provenance.status.diagnostic_code,
                        descriptor.provider_id + ":" + provenance.status.detail);
    }
  }
  for (const auto& entry : entries_) {
    if (entry.descriptor.provider_id == descriptor.provider_id ||
        ProviderKey(entry.descriptor.owning_agent,
                    entry.descriptor.actuator_id,
                    entry.descriptor.operation_id) ==
            ProviderKey(descriptor.owning_agent,
                        descriptor.actuator_id,
                        descriptor.operation_id)) {
      return AgentError("SB_AGENT_ACTUATOR_PROVIDER.DUPLICATE", descriptor.provider_id);
    }
  }
  entries_.push_back({std::move(descriptor), std::move(execute)});
  return {true, "SB_AGENT_ACTUATOR_PROVIDER.REGISTERED", entries_.back().descriptor.provider_id};
}

std::optional<AgentActuatorProviderDescriptor> AgentActuatorProviderRegistry::Find(
    const std::string& owning_agent,
    const std::string& actuator_id,
    const std::string& operation_id) const {
  for (const auto& entry : entries_) {
    if (entry.descriptor.owning_agent == owning_agent &&
        entry.descriptor.actuator_id == actuator_id &&
        entry.descriptor.operation_id == operation_id) {
      return entry.descriptor;
    }
  }
  return std::nullopt;
}

std::optional<AgentActuatorProviderDescriptor> AgentActuatorProviderRegistry::Find(
    const std::string& actuator_id,
    const std::string& operation_id) const {
  for (const auto& entry : entries_) {
    if (entry.descriptor.actuator_id == actuator_id &&
        entry.descriptor.operation_id == operation_id) {
      return entry.descriptor;
    }
  }
  return std::nullopt;
}

AgentActuatorProviderResult AgentActuatorProviderRegistry::Execute(
    const AgentActuatorProviderDescriptor& descriptor,
    const AgentActuatorProviderRequest& request) const {
  for (const auto& entry : entries_) {
    if (entry.descriptor.provider_id != descriptor.provider_id) { continue; }
    if (!request.dry_run && !entry.descriptor.live_route_available) {
      AgentActuatorProviderResult result;
      result.status = AgentError("SB_AGENT_ACTUATOR_PROVIDER.LIVE_ROUTE_UNAVAILABLE",
                                 descriptor.provider_id);
      return result;
    }
    if (entry.execute) { return entry.execute(request); }
    if (!request.dry_run) {
      AgentActuatorProviderResult result;
      result.status = AgentError(
          "SB_AGENT_ACTUATOR_PROVIDER.LIVE_EXECUTOR_REQUIRED",
          descriptor.provider_id);
      return result;
    }
    return DefaultProviderExecute(entry.descriptor, request);
  }
  AgentActuatorProviderResult result;
  result.status = AgentError("SB_AGENT_ACTUATOR_PROVIDER.UNREGISTERED",
                             descriptor.provider_id);
  return result;
}

std::vector<AgentActuatorProviderDescriptor>
AgentActuatorProviderRegistry::ListDescriptors() const {
  std::vector<AgentActuatorProviderDescriptor> descriptors;
  descriptors.reserve(entries_.size());
  for (const auto& entry : entries_) {
    descriptors.push_back(entry.descriptor);
  }
  return descriptors;
}

AgentActuatorProviderRegistry DefaultAgentActuatorProviderRegistry() {
  AgentActuatorProviderRegistry registry;
  for (const auto& contract : AgentActionContractRegistry()) {
    if (contract.cluster_scoped) { continue; }
    AgentActuatorProviderDescriptor descriptor;
    descriptor.provider_id = contract.actuator + ":" + contract.action_id;
    descriptor.owning_agent = contract.owning_agent;
    descriptor.actuator_id = contract.actuator;
    descriptor.operation_id = contract.action_id;
    descriptor.authority_domain = ActuatorAuthorityDomainForId(contract.actuator);
    descriptor.supports_dry_run = true;
    const auto exposure = ClassifyAgentProductionActionExposure(
        contract.owning_agent, contract.action_id);
    descriptor.live_route_available = exposure.live_route_available;
    descriptor.real_subsystem_handler =
        exposure.live_route_available &&
        exposure.route_evidence_kind != "default_contract_registry";
    if (descriptor.real_subsystem_handler) {
      descriptor.subsystem_handler_id =
          contract.owning_agent + ":" + contract.action_id;
      descriptor.handler_provenance = exposure.route_evidence_kind;
      descriptor.handler_evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_actuator_provider_route|" + descriptor.subsystem_handler_id +
          "|" + exposure.route_evidence_kind);
    }
    descriptor.idempotent = true;
    descriptor.supports_retry = true;
    descriptor.supports_rollback_compensation =
        contract.failure_behavior.find("rollback") != std::string::npos ||
        contract.failure_behavior.find("local event only") != std::string::npos;
    descriptor.requires_outcome_verification = true;
    descriptor.required_evidence_fields = {"evidence_uuid", "metric_digest"};
    (void)registry.Register(std::move(descriptor));
  }
  return registry;
}

std::string AgentActionInputEvidenceDigest(const AgentActionRequest& action) {
  std::ostringstream payload;
  payload << action.action_uuid << '\n'
          << action.agent_type_id << '\n'
          << action.instance_uuid << '\n'
          << action.actuator_id << '\n'
          << action.operation_id << '\n'
          << action.idempotency_key << '\n';
  for (const auto& [key, value] : action.inputs) {
    payload << key << '=' << value << '\n';
  }
  return Sha256Digest(payload.str());
}

AgentRuntimeStatus ValidateAgentActionPolicyGeneration(
    const DurableAgentCatalogImage& catalog,
    const AgentActionRequest& action) {
  const std::string expected_text =
      ActionInputValue(action, "expected_policy_generation");
  if (expected_text.empty()) { return AgentOk(); }
  u64 expected = 0;
  try {
    expected = static_cast<u64>(std::stoull(expected_text));
  } catch (const std::exception&) {
    return AgentError("SB_AGENT_ACTION.POLICY_GENERATION_INVALID",
                      action.action_uuid);
  }
  const u64 current = PolicyGenerationForAction(catalog, action);
  if (current == 0) {
    return AgentError(
        "SB_AGENT_COMMERCIAL_EVIDENCE.POLICY_GENERATION_REQUIRED",
        action.instance_uuid);
  }
  if (expected != current) {
    return AgentError("SB_AGENT_ACTION.POLICY_GENERATION_CHANGED",
                      std::to_string(current));
  }
  return AgentOk();
}

AgentActionDispatchResult DispatchAgentAction(
    const AgentActionDispatchRequest& request) {
  std::optional<std::string> strict_metric_input_digest;
  AgentActionSafetyEnvelope accepted_safety_envelope;
  AgentManualApprovalWorkflowEvaluation accepted_approval_workflow;
  bool safety_envelope_validated = false;
  bool approval_workflow_validated = false;
  if (request.catalog == nullptr) {
    return FinishFailure("SB_AGENT_ACTION_DISPATCH.CATALOG_REQUIRED",
                         "durable catalog image required", request.action);
  }
  const auto catalog_status =
      request.production_live_path
          ? ValidateDurableAgentCatalogForProduction(*request.catalog)
          : AgentOk();
  if (!catalog_status.ok) {
    return FinishFailure(catalog_status.diagnostic_code, catalog_status.detail,
                         request.action);
  }
  const auto authority_status =
      ValidateAgentActionAuthorityProvenance(request.authority);
  if (!authority_status.ok) {
    return FinishFailure(authority_status.diagnostic_code, authority_status.detail,
                         request.action);
  }
  if (request.production_live_path) {
    AgentProductionFixtureSeparationInput fixture_separation;
#if defined(SCRATCHBIRD_AGENT_PRODUCTION_BUILD)
    fixture_separation.production_build = true;
#endif
#if defined(SCRATCHBIRD_ENABLE_DEBUG_LOGS) || \
    defined(SCRATCHBIRD_ENABLE_HOTPATH_TRACE) || \
    defined(SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE) || \
    defined(SCRATCHBIRD_ENABLE_PREPARED_TRACE)
    fixture_separation.debug_only_paths_enabled = true;
#endif
    fixture_separation.production_live_path = true;
    fixture_separation.relaxed_metric_path =
        request.metric_snapshot_options.mode !=
        AgentMetricRuntimeMode::production_strict;
    fixture_separation.observed_metric_snapshot_required = true;
    fixture_separation.probe_only_catalog =
        request.catalog->source != AgentCatalogStateSource::durable_catalog_image;
    fixture_separation.durable_runtime_catalog =
        request.catalog->source == AgentCatalogStateSource::durable_catalog_image;
    fixture_separation.sidecar_only_evidence = false;
    fixture_separation.durable_evidence_store = true;
    fixture_separation.simulated_actuator_provider =
        request.registry == nullptr && !request.action.dry_run;
    fixture_separation.synthetic_live_management_state = false;
    fixture_separation.management_state_durable = true;
    fixture_separation.live_agent_surface = !request.action.dry_run;
    const auto separated =
        ValidateAgentProductionFixtureSeparation(fixture_separation);
    if (!separated.ok) {
      return FinishFailure(separated.status.diagnostic_code,
                           separated.status.detail,
                           request.action);
    }
  }
  if (request.action.action_uuid.empty() || request.action.idempotency_key.empty() ||
      request.action.actuator_id.empty() || request.action.operation_id.empty()) {
    return FinishFailure("SB_AGENT_ACTION_DISPATCH.ACTION_IDEMPOTENCY_REQUIRED",
                         request.action.action_uuid, request.action);
  }
  const auto policy_generation_status =
      ValidateAgentActionPolicyGeneration(*request.catalog, request.action);
  if (!policy_generation_status.ok) {
    return FinishFailure(policy_generation_status.diagnostic_code,
                         policy_generation_status.detail,
                         request.action);
  }
  if (request.production_live_path) {
    const auto descriptor = FindAgentType(request.action.agent_type_id);
    if (!descriptor.has_value()) {
      return FinishFailure("SB_AGENT_ACTION_DISPATCH.AGENT_DESCRIPTOR_REQUIRED",
                           request.action.agent_type_id, request.action);
    }
    auto metric_context = request.metric_context;
    if (metric_context.database_uuid.empty()) {
      metric_context.database_uuid = request.authority.scope_uuid;
    }
    if (metric_context.principal_uuid.empty()) {
      metric_context.principal_uuid = request.authority.principal_uuid;
    }
    if (metric_context.wall_now_microseconds == 0) {
      metric_context.wall_now_microseconds = 1;
    }
    metric_context.security_context_present = true;
    auto metric_options = request.metric_snapshot_options;
    metric_options.mode = AgentMetricRuntimeMode::production_strict;
    if (metric_options.expected_scope_uuid.empty()) {
      metric_options.expected_scope_uuid = request.authority.scope_uuid;
    }
    const auto metric_evaluation = EvaluateAgentObservedMetricSnapshots(
        *descriptor,
        metric_context,
        request.observed_metric_snapshots,
        metric_options);
    if (!metric_evaluation.accepted) {
      return FinishFailure(metric_evaluation.status.diagnostic_code,
                           metric_evaluation.status.detail,
                           request.action);
    }
    strict_metric_input_digest = metric_evaluation.input_digest;
  }

  const std::string digest = AgentActionInputEvidenceDigest(request.action);
  std::optional<std::size_t> pending_intent_index;
  for (std::size_t i = 0; i < request.catalog->actions.size(); ++i) {
    const auto& action = request.catalog->actions[i];
    if (action.idempotency_key == request.action.idempotency_key) {
      if (action.action_uuid == request.action.action_uuid &&
          action.state == DurableAgentActionState::pending &&
          action.diagnostic_code ==
              "SB_AGENT_ACTION_DISPATCH.PENDING_INTENT") {
        pending_intent_index = i;
        break;
      }
      AgentActionDispatchResult result;
      result.status = {true, "SB_AGENT_ACTION_DISPATCH.IDEMPOTENT_REPLAY",
                       action.action_uuid};
      result.decision.result_class =
          action.state == DurableAgentActionState::completed
              ? AgentActionResultClass::accepted
              : AgentActionResultClass::failed_closed;
      result.decision.diagnostic_code = result.status.diagnostic_code;
      result.decision.detail = action.diagnostic_code;
      result.decision.evidence_uuid = action.evidence_uuid;
      result.decision.mutates_state = false;
      result.action_record = action;
      result.input_evidence_digest = action.input_evidence_digest;
      result.duplicate_idempotency_key = true;
      result.outcome_verified = action.outcome_verified;
      result.compensation_required = action.compensation_required;
      return result;
    }
  }

  AgentActuatorProviderRegistry default_registry;
  const AgentActuatorProviderRegistry* registry = request.registry;
  if (registry == nullptr) {
    default_registry = DefaultAgentActuatorProviderRegistry();
    registry = &default_registry;
  }
  const auto provider =
      registry->Find(request.action.agent_type_id,
                     request.action.actuator_id,
                     request.action.operation_id);
  if (!provider.has_value()) {
    return FinishFailure("SB_AGENT_ACTUATOR_PROVIDER.UNREGISTERED",
                         request.action.actuator_id + ":" + request.action.operation_id,
                         request.action);
  }
  for (const auto& field : provider->required_evidence_fields) {
    const auto found = request.action.inputs.find(field);
    if (found == request.action.inputs.end() || found->second.empty()) {
      return FinishFailure("SB_AGENT_ACTUATOR_PROVIDER.EVIDENCE_FIELD_REQUIRED",
                           field, request.action);
    }
  }
  bool package_provenance_validated = false;
  if (request.production_live_path && !request.action.dry_run &&
      provider->live_route_available) {
    const auto provenance =
        ValidateAgentPackageProvenanceBundle(provider->package_provenance);
    if (!provenance.accepted) {
      return FinishFailure(provenance.status.diagnostic_code,
                           provenance.status.detail,
                           request.action);
    }
    package_provenance_validated = true;
  }
  const u64 policy_generation =
      PolicyGenerationForAction(*request.catalog, request.action);
  if (policy_generation == 0) {
    return FinishFailure(
        "SB_AGENT_COMMERCIAL_EVIDENCE.POLICY_GENERATION_REQUIRED",
        request.action.instance_uuid, request.action);
  }
  const std::string input_metric_digest =
      ActionInputValue(request.action, "metric_digest");
  if (input_metric_digest.empty()) {
    return FinishFailure(
        "SB_AGENT_COMMERCIAL_EVIDENCE.INPUT_METRIC_DIGEST_REQUIRED",
        request.action.action_uuid, request.action);
  }
  if (strict_metric_input_digest.has_value() &&
      input_metric_digest != *strict_metric_input_digest) {
    return FinishFailure(
        "SB_AGENT_COMMERCIAL_EVIDENCE.METRIC_DIGEST_MISMATCH",
        request.action.action_uuid, request.action);
  }
  if (request.production_live_path && !request.action.dry_run) {
    const auto provider_context_status =
        ValidateProviderExecutionContext(request.provider_execution_context);
    if (!provider_context_status.ok) {
      return FinishFailure(provider_context_status.diagnostic_code,
                           provider_context_status.detail,
                           request.action);
    }
  }
  const auto contract =
      FindAgentActionContract(request.action.agent_type_id,
                              request.action.operation_id);
  if (!contract.has_value()) {
    return FinishFailure("SB_AGENT_ACTION_SAFETY.CONTRACT_REQUIRED",
                         request.action.agent_type_id + ":" +
                             request.action.operation_id,
                         request.action);
  }
  if (request.production_live_path) {
    AgentActionSafetyProviderContext provider_context;
    provider_context.provider_registered = true;
    provider_context.supports_dry_run = provider->supports_dry_run;
    provider_context.live_route_available = provider->live_route_available;
    provider_context.real_subsystem_handler = provider->real_subsystem_handler;
    provider_context.fixture_or_simulated_provider =
        provider->fixture_or_simulated_provider;
    provider_context.supports_retry = provider->supports_retry;
    provider_context.supports_rollback_compensation =
        provider->supports_rollback_compensation;
    provider_context.requires_outcome_verification =
        provider->requires_outcome_verification;
    provider_context.authority_domain = provider->authority_domain;
    provider_context.cluster_scoped_contract = contract->cluster_scoped;
    const auto rollout =
        AgentActionRolloutProfileFromInputs(request.action.inputs);
    const auto safety_envelope = AgentActionSafetyEnvelopeFromInputs(
        request.action,
        provider_context,
        rollout,
        request.authority.scope_uuid,
        digest,
        input_metric_digest,
        request.catalog->authority.catalog_root_digest,
        policy_generation,
        contract->manual_approval_required ||
            contract->operator_approval_required,
        request.authority.parser_authority,
        request.authority.client_authority,
        request.authority.donor_authority,
        request.authority.sidecar_authority);
    const auto safety_status =
        ValidateAgentActionSafetyEnvelope(safety_envelope);
    if (!safety_status.ok) {
      return FinishFailure(safety_status.diagnostic_code,
                           safety_status.detail,
                           request.action);
    }
    const u64 approval_now =
        request.metric_context.wall_now_microseconds == 0
            ? 1
            : request.metric_context.wall_now_microseconds;
    const auto approval_request = AgentManualApprovalWorkflowFromActionInputs(
        request.action,
        request.authority.principal_uuid,
        request.authority.scope_uuid,
        policy_generation,
        approval_now,
        safety_envelope.manual_approval_required,
        safety_envelope.cluster_route_requested,
        safety_envelope.external_cluster_provider_attested);
    const auto approval_status =
        ValidateAgentManualApprovalWorkflow(approval_request);
    if (!approval_status.status.ok) {
      return FinishFailure(approval_status.status.diagnostic_code,
                           approval_status.status.detail,
                           request.action);
    }
    accepted_safety_envelope = safety_envelope;
    safety_envelope_validated = true;
    accepted_approval_workflow = approval_status;
    approval_workflow_validated = approval_status.required;
  }

  AgentActuatorProviderRequest provider_request;
  provider_request.action = request.action;
  provider_request.authority = request.authority;
  provider_request.execution_context = request.provider_execution_context;
  provider_request.input_evidence_digest = digest;
  provider_request.dry_run = request.action.dry_run;
  provider_request.subsystem_reported_success = request.subsystem_reported_success;
  provider_request.intended_state_observed = request.intended_state_observed;
  auto provider_result = registry->Execute(*provider, provider_request);
  if (!request.action.dry_run && provider_result.status.ok &&
      (!provider_result.dispatched || !provider_result.mutation_attempted)) {
    return FinishFailure(
        "SB_AGENT_ACTUATOR_PROVIDER.LIVE_EXECUTION_EVIDENCE_REQUIRED",
        provider->provider_id,
        request.action);
  }
  if (!request.action.dry_run && provider_result.compensation_required) {
    if (provider_result.compensation_attempted) {
      if (provider_result.compensation_executor_id.empty() ||
          provider_result.compensation_evidence_uuid.empty()) {
        return FinishFailure(
            "SB_AGENT_ACTUATOR_PROVIDER.COMPENSATION_EVIDENCE_REQUIRED",
            provider->provider_id,
            request.action);
      }
    } else if (provider->supports_retry) {
      provider_result.retry_scheduled = true;
      if (provider_result.retry_after_microseconds == 0) {
        provider_result.retry_after_microseconds =
            request.provider_execution_context.local_transaction_id + 1;
      }
      if (provider_result.retry_evidence_uuid.empty()) {
        provider_result.retry_evidence_uuid =
            DeterministicAgentRuntimeObjectUuidFromKey(
                "agent_action_retry|" + request.action.action_uuid + "|" +
                provider->provider_id + "|" + digest);
      }
    } else {
      return FinishFailure(
          "SB_AGENT_ACTUATOR_PROVIDER.RETRY_OR_COMPENSATION_REQUIRED",
          provider->provider_id,
          request.action);
    }
  }

  DurableAgentActionState state = DurableAgentActionState::completed;
  AgentActionResultClass result_class = AgentActionResultClass::accepted;
  if (request.action.dry_run) {
    state = DurableAgentActionState::completed;
    result_class = AgentActionResultClass::dry_run_only;
  } else if (!provider_result.status.ok || !provider_result.outcome_verified) {
    state = provider_result.compensation_required
                ? DurableAgentActionState::quarantined
                : DurableAgentActionState::replay_pending;
    result_class = AgentActionResultClass::failed_closed;
  }

  const std::string evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_action_dispatch|" + request.action.action_uuid + "|" +
      provider->provider_id + "|" + digest);
  CommercialAgentEvidenceBuildRequest evidence_request;
  evidence_request.action = request.action;
  evidence_request.authority = request.authority;
  evidence_request.provider_id = provider->provider_id;
  evidence_request.input_evidence_digest = digest;
  evidence_request.input_metric_digest = input_metric_digest;
  evidence_request.policy_generation = policy_generation;
  evidence_request.scope_uuids = ActionScopeUuids(request.authority, request.action);
  evidence_request.result_state = DurableAgentActionStateName(state);
  evidence_request.diagnostic_code = provider_result.status.diagnostic_code;
  evidence_request.outcome_verification_evidence_uuid =
      provider_result.verification_evidence_uuid;
  if (evidence_request.outcome_verification_evidence_uuid.empty()) {
    evidence_request.outcome_verification_evidence_uuid =
        DeterministicAgentRuntimeObjectUuidFromKey(
            "agent_action_outcome_not_verified|" + request.action.action_uuid +
            "|" + provider->provider_id + "|" +
            provider_result.status.diagnostic_code);
  }
  evidence_request.redaction_class =
      ActionInputValue(request.action, "redaction_class").empty()
          ? "standard"
          : ActionInputValue(request.action, "redaction_class");
  evidence_request.retention_class =
      ActionInputValue(request.action, "retention_class").empty()
          ? "audit"
          : ActionInputValue(request.action, "retention_class");
  evidence_request.previous_tamper_digest =
      request.catalog->evidence.empty()
          ? "scratchbird-agent-evidence-ledger-genesis"
          : request.catalog->evidence.back().tamper_chain_digest;
  evidence_request.created_at_microseconds =
      request.metric_context.wall_now_microseconds == 0
          ? 1
          : request.metric_context.wall_now_microseconds;
  evidence_request.tamper_key_id = "agent-evidence-ledger-key-v1";
  evidence_request.tamper_key_provenance =
      "engine_local_protected_hmac_key";
  evidence_request.evidence_key_policy_id =
      "agent-evidence-key-policy-v1";
  evidence_request.tamper_key_generation = 1;
  evidence_request.tamper_key_rotation_epoch = 1;
  evidence_request.tamper_key_not_before_microseconds = 1;
  evidence_request.tamper_key_not_after_microseconds =
      evidence_request.created_at_microseconds + 365ull * 86400000000ull;
  evidence_request.key_residency_class = "engine_local_protected";
  evidence_request.data_residency_class = "engine_local";
  evidence_request.production_key_material = true;
  evidence_request.test_key_material = false;
  evidence_request.key_material_exported = false;
  evidence_request.storage_linkage_digest = Sha256Digest(
      request.catalog->authority.catalog_storage_uuid + "|" +
      request.catalog->authority.catalog_root_digest + "|" +
      request.action.action_uuid);
  evidence_request.protected_material_present =
      evidence_request.redaction_class == "protected_material";
  evidence_request.decision_payload =
      provider->provider_id + "|" + digest + "|" +
      DurableAgentActionStateName(state) + "|" +
      provider_result.status.diagnostic_code + "|" +
      provider_result.verification_evidence_uuid + "|" +
      (package_provenance_validated
           ? ValidateAgentPackageProvenanceBundle(provider->package_provenance)
                 .bundle_digest
           : "dry-run-or-nonproduction-package-provenance-not-consumed");
  AgentEvidenceRecord evidence =
      BuildCommercialAgentEvidence(evidence_request);
  evidence.evidence_uuid = evidence_uuid;
  FinalizeCommercialAgentEvidenceDigests(&evidence);
  const auto evidence_validation = ValidateCommercialAgentEvidence(evidence);
  if (!evidence_validation.status.ok) {
    return FinishFailure(evidence_validation.status.diagnostic_code,
                         evidence_validation.status.detail, request.action);
  }
  request.catalog->evidence.push_back(evidence);

  DurableAgentActionRecord record = BuildActionRecord(
      request.action, request.authority, &*provider, digest, state,
      provider_result.status.diagnostic_code, evidence_uuid,
      provider_result.verification_evidence_uuid, provider_result.outcome_verified,
      provider_result.compensation_required, provider_result.compensation_attempted,
      provider_result.retry_scheduled, provider_result.retry_after_microseconds,
      provider_result.retry_evidence_uuid,
      provider_result.compensation_executor_id,
      provider_result.compensation_evidence_uuid);
  if (pending_intent_index.has_value()) {
    request.catalog->actions[*pending_intent_index] = record;
  } else {
    request.catalog->actions.push_back(record);
  }
  const auto catalog_refresh =
      RefreshDurableAgentCatalogAuthorityDigest(request.catalog, evidence_uuid);
  if (!catalog_refresh.ok) {
    return FinishFailure(catalog_refresh.diagnostic_code,
                         catalog_refresh.detail,
                         request.action);
  }

  AgentActionDispatchResult result;
  result.status = provider_result.status;
  result.decision.result_class = result_class;
  result.decision.diagnostic_code = provider_result.status.diagnostic_code;
  result.decision.detail = provider_result.status.detail;
  result.decision.evidence_uuid = evidence_uuid;
  result.decision.mutates_state = provider_result.mutation_attempted;
  result.action_record = std::move(record);
  result.input_evidence_digest = digest;
  result.safety_envelope = accepted_safety_envelope;
  result.approval_workflow = accepted_approval_workflow;
  result.provider_dispatched = provider_result.dispatched;
  result.durable_record_written = true;
  result.commercial_evidence_written_before_action_record = true;
  result.safety_envelope_validated = safety_envelope_validated;
  result.approval_workflow_validated = approval_workflow_validated;
  result.package_provenance_validated = package_provenance_validated;
  result.dry_run = request.action.dry_run;
  result.outcome_verified = provider_result.outcome_verified;
  result.compensation_required = provider_result.compensation_required;
  result.quarantined_or_replay_pending =
      state == DurableAgentActionState::quarantined ||
      state == DurableAgentActionState::replay_pending;
  return result;
}

}  // namespace scratchbird::core::agents
