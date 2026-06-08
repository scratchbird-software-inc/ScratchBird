// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_dispatch_store_api.hpp"

// SEARCH_KEY: AEIC_REAL_ACTUATOR_PROVIDER_FRAMEWORK
// SEARCH_KEY: AEIC_DURABLE_AGENT_ACTION_DISPATCH_STORE

#include <algorithm>
#include <optional>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents;

std::string DiagnosticDetail(const EngineApiDiagnostic& diagnostic) {
  if (!diagnostic.detail.empty()) { return diagnostic.detail; }
  if (!diagnostic.message_key.empty()) { return diagnostic.message_key; }
  return diagnostic.code;
}

bool RouteProofMatchesAction(
    const agents::AgentProductionRouteProofInputs::RouteProof& proof,
    const std::string& agent_type_id,
    const std::string& action_id,
    const std::string& actuator_id) {
  if (proof.agent_type_id != agent_type_id ||
      proof.action_id != action_id ||
      proof.actuator_id != actuator_id) {
    return false;
  }
  const auto contract = agents::FindAgentActionContract(agent_type_id,
                                                        action_id);
  if (!contract.has_value()) { return false; }
  if (contract->actuator != actuator_id ||
      proof.authority_domain !=
          agents::ActuatorAuthorityDomainForId(contract->actuator)) {
    return false;
  }
  return proof.live_route_available &&
         proof.real_subsystem_handler &&
         proof.idempotent &&
         proof.supports_retry &&
         proof.supports_rollback_compensation &&
         !proof.provider_id.empty() &&
         !proof.subsystem_handler_id.empty() &&
         !proof.handler_provenance.empty() &&
         !proof.handler_evidence_uuid.empty() &&
         !contract->cluster_scoped &&
         proof.authority_domain !=
             agents::AgentActuatorAuthorityDomain::cluster_provider;
}

agents::AgentRuntimeStatus ValidateEngineRequestForRegistrySeal(
    const EngineRequestContext& context) {
  if (context.request_id.empty() ||
      context.database_uuid.canonical.empty() ||
      context.transaction_uuid.canonical.empty() ||
      context.local_transaction_id == 0 ||
      !context.security_context_present) {
    return agents::AgentError(
        "SB_AGENT_ACTION_STORE.ENGINE_REGISTRY_CONTEXT_REQUIRED");
  }
  return agents::AgentOk();
}

agents::AgentRuntimeStatus ValidateEngineRequestForStoreDispatch(
    const EngineRequestContext& context) {
  if (context.request_id.empty() ||
      context.database_uuid.canonical.empty() ||
      context.transaction_uuid.canonical.empty() ||
      context.local_transaction_id == 0 ||
      !context.security_context_present) {
    return agents::AgentError(
        "SB_AGENT_ACTION_STORE.ENGINE_REQUEST_CONTEXT_REQUIRED");
  }
  return agents::AgentOk();
}

agents::AgentActionDispatchResult StoreFailure(std::string code,
                                               std::string detail,
                                               const agents::AgentActionRequest& action) {
  agents::AgentActionDispatchResult result;
  result.status = agents::AgentError(std::move(code), std::move(detail));
  result.decision.result_class = agents::AgentActionResultClass::failed_closed;
  result.decision.diagnostic_code = result.status.diagnostic_code;
  result.decision.detail = result.status.detail;
  result.decision.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_action_store_refused|" + action.action_uuid + "|" +
          result.status.diagnostic_code);
  result.decision.mutates_state = false;
  return result;
}

agents::DurableAgentResourceReservationRequest ResourceReservationForAction(
    const AgentActionDispatchStoreRequest& request) {
  agents::DurableAgentResourceReservationRequest reservation;
  const std::string key = request.action.idempotency_key.empty()
                              ? request.action.action_uuid
                              : request.action.idempotency_key;
  reservation.reservation_key =
      "action_dispatch:" + request.action.agent_type_id + ":" + key;
  reservation.reservation_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_action_resource_reservation|" + reservation.reservation_key);
  reservation.owner_scope = request.context.principal_uuid.canonical.empty()
                                ? request.authority.principal_uuid
                                : request.context.principal_uuid.canonical;
  reservation.agent_type_id = request.action.agent_type_id;
  reservation.operation_id = request.action.operation_id;
  reservation.now_microseconds =
      request.metric_context.wall_now_microseconds == 0
          ? 1
          : request.metric_context.wall_now_microseconds;
  reservation.memory_bytes = request.resource_reservation_memory_bytes;
  reservation.worker_slots = request.resource_reservation_worker_slots;
  reservation.overhead_microseconds =
      request.resource_reservation_overhead_microseconds;
  reservation.max_active_reservations =
      request.resource_reservation_max_active;
  reservation.max_memory_bytes =
      request.resource_reservation_max_memory_bytes;
  reservation.max_worker_slots =
      request.resource_reservation_max_worker_slots;
  reservation.max_overhead_microseconds =
      request.resource_reservation_max_overhead_microseconds;
  reservation.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_action_resource_reservation_evidence|" +
          reservation.reservation_uuid);
  return reservation;
}

agents::DurableAgentActionRecord PendingActionIntentForDispatch(
    const AgentActionDispatchStoreRequest& request) {
  agents::DurableAgentActionRecord record;
  record.action_uuid = request.action.action_uuid;
  record.instance_uuid = request.action.instance_uuid;
  record.owner_uuid = request.authority.principal_uuid;
  record.operation_id = request.action.operation_id;
  record.actuator_provider_id =
      request.action.actuator_id + ":" + request.action.operation_id;
  record.state = agents::DurableAgentActionState::pending;
  record.idempotency_key = request.action.idempotency_key;
  record.input_evidence_digest =
      agents::AgentActionInputEvidenceDigest(request.action);
  record.evidence_uuid = agents::DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_action_pending_intent|" + request.action.action_uuid);
  record.diagnostic_code = "SB_AGENT_ACTION_DISPATCH.PENDING_INTENT";
  record.generation = 1;
  return record;
}

agents::AgentRuntimeStatus ValidateEngineOwnedRegistry(
    const AgentActionDispatchStoreRequest& request) {
  if (!request.production_live_path || request.action.dry_run) {
    return agents::AgentOk();
  }
  if (request.engine_registry == nullptr) {
    return agents::AgentError(
        "SB_AGENT_ACTION_STORE.ENGINE_OWNED_REGISTRY_REQUIRED",
        request.action.agent_type_id + ":" + request.action.operation_id);
  }
  if (!request.engine_registry->valid() ||
      request.engine_registry->provenance().empty() ||
      request.engine_registry->evidence_uuid().empty()) {
    return agents::AgentError(
        "SB_AGENT_ACTION_STORE.REGISTRY_PROVENANCE_REQUIRED",
        request.action.agent_type_id + ":" + request.action.operation_id);
  }
  if (!request.engine_registry->HasLiveRouteProofForAction(
          request.action.agent_type_id,
          request.action.operation_id,
          request.action.actuator_id)) {
    return agents::AgentError(
        "SB_AGENT_ACTION_STORE.ROUTE_PROOF_REQUIRED",
        request.action.agent_type_id + ":" + request.action.operation_id);
  }
  return agents::AgentOk();
}

agents::AgentRuntimeStatus ValidateStrictMetricsBeforeActionReservation(
    const AgentActionDispatchStoreRequest& request) {
  if (!request.production_live_path || request.action.dry_run) {
    return agents::AgentOk();
  }
  const auto descriptor = agents::FindAgentType(request.action.agent_type_id);
  if (!descriptor.has_value()) {
    return agents::AgentError(
        "SB_AGENT_ACTION_DISPATCH.AGENT_DESCRIPTOR_REQUIRED",
        request.action.agent_type_id);
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
  metric_options.mode = agents::AgentMetricRuntimeMode::production_strict;
  if (metric_options.expected_scope_uuid.empty()) {
    metric_options.expected_scope_uuid = request.authority.scope_uuid;
  }
  const auto metric_evaluation = agents::EvaluateAgentObservedMetricSnapshots(
      *descriptor,
      metric_context,
      request.observed_metric_snapshots,
      metric_options);
  if (!metric_evaluation.accepted) {
    return metric_evaluation.status;
  }

  const auto metric_digest = request.action.inputs.find("metric_digest");
  if (metric_digest == request.action.inputs.end() ||
      metric_digest->second.empty()) {
    return agents::AgentError(
        "SB_AGENT_COMMERCIAL_EVIDENCE.INPUT_METRIC_DIGEST_REQUIRED",
        request.action.action_uuid);
  }
  if (metric_digest->second != metric_evaluation.input_digest) {
    return agents::AgentError(
        "SB_AGENT_COMMERCIAL_EVIDENCE.METRIC_DIGEST_MISMATCH",
        request.action.action_uuid);
  }
  return agents::AgentOk();
}

}  // namespace

bool EngineOwnedAgentActuatorRegistry::HasLiveRouteProofForAction(
    const std::string& agent_type_id,
    const std::string& action_id,
    const std::string& actuator_id) const {
  if (!sealed_) { return false; }
  const auto provider = registry_.Find(agent_type_id, actuator_id, action_id);
  if (!provider.has_value() || !provider->live_route_available ||
      !provider->real_subsystem_handler) {
    return false;
  }
  for (const auto& proof : route_proofs_.route_proofs) {
    if (!RouteProofMatchesAction(proof, agent_type_id, action_id,
                                 actuator_id)) {
      continue;
    }
    return proof.provider_id == provider->provider_id &&
           proof.subsystem_handler_id == provider->subsystem_handler_id &&
           proof.handler_provenance == provider->handler_provenance &&
           proof.handler_evidence_uuid == provider->handler_evidence_uuid;
  }
  return false;
}

EngineOwnedAgentActuatorRegistryBuildResult
BuildEngineOwnedAgentActuatorRegistry(
    const EngineRequestContext& context,
    agents::AgentActuatorProviderRegistry registry,
    agents::AgentProductionRouteProofInputs route_proofs) {
  EngineOwnedAgentActuatorRegistryBuildResult result;
  const auto context_status = ValidateEngineRequestForRegistrySeal(context);
  if (!context_status.ok) {
    result.status = context_status;
    return result;
  }
  const auto exposure_status =
      agents::ValidateAgentProductionExposureMatrix(route_proofs);
  if (!exposure_status.ok) {
    result.status = exposure_status;
    return result;
  }
  for (const auto& descriptor : registry.ListDescriptors()) {
    if (!descriptor.live_route_available) { continue; }
    bool proof_found = false;
    for (const auto& proof : route_proofs.route_proofs) {
      if (!RouteProofMatchesAction(proof,
                                   descriptor.owning_agent,
                                   descriptor.operation_id,
                                   descriptor.actuator_id)) {
        continue;
      }
      proof_found =
          proof.provider_id == descriptor.provider_id &&
          proof.subsystem_handler_id == descriptor.subsystem_handler_id &&
          proof.handler_provenance == descriptor.handler_provenance &&
          proof.handler_evidence_uuid == descriptor.handler_evidence_uuid;
      if (proof_found) { break; }
    }
    if (!proof_found) {
      result.status = agents::AgentError(
          "SB_AGENT_ACTION_STORE.ROUTE_PROOF_REQUIRED",
          descriptor.owning_agent + ":" + descriptor.operation_id);
      return result;
    }
  }

  result.registry.sealed_ = true;
  result.registry.provenance_ =
      "engine_internal_api_registered_provider_registry.v1";
  result.registry.evidence_uuid_ =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "engine_owned_agent_actuator_registry|" +
          context.database_uuid.canonical + "|" +
          context.transaction_uuid.canonical + "|" +
          std::to_string(context.local_transaction_id) + "|" +
          std::to_string(route_proofs.route_proofs.size()));
  result.registry.registry_ = std::move(registry);
  result.registry.route_proofs_ = std::move(route_proofs);
  result.status = {true, "SB_AGENT_ACTION_STORE.ENGINE_REGISTRY_SEALED",
                   result.registry.evidence_uuid_};
  return result;
}

AgentActionDispatchStoreResult DispatchAgentActionWithDurableCatalogStore(
    const AgentActionDispatchStoreRequest& request) {
  AgentActionDispatchStoreResult result;
  if (request.production_live_path && !request.action.dry_run) {
    const auto context_status =
        ValidateEngineRequestForStoreDispatch(request.context);
    if (!context_status.ok) {
      result.dispatch = StoreFailure(context_status.diagnostic_code,
                                     context_status.detail,
                                     request.action);
      return result;
    }
  }

  const auto registry_status = ValidateEngineOwnedRegistry(request);
  if (!registry_status.ok) {
    result.dispatch = StoreFailure(registry_status.diagnostic_code,
                                   registry_status.detail,
                                   request.action);
    return result;
  }
  result.engine_owned_registry_validated =
      request.production_live_path && !request.action.dry_run;

  const auto authority_status =
      agents::ValidateAgentActionAuthorityProvenance(request.authority);
  if (!authority_status.ok) {
    result.dispatch = StoreFailure(authority_status.diagnostic_code,
                                   authority_status.detail,
                                   request.action);
    return result;
  }

  const auto metric_status =
      ValidateStrictMetricsBeforeActionReservation(request);
  if (!metric_status.ok) {
    result.dispatch = StoreFailure(metric_status.diagnostic_code,
                                   metric_status.detail,
                                   request.action);
    return result;
  }

  result.loaded_catalog =
      LoadAgentDurableCatalogImage(request.context, request.production_live_path);
  if (!result.loaded_catalog.ok) {
    result.dispatch = StoreFailure(
        "SB_AGENT_ACTION_STORE.LOAD_FAILED",
        DiagnosticDetail(result.loaded_catalog.diagnostic),
        request.action);
    return result;
  }
  result.loaded_from_store = true;

  agents::DurableAgentCatalogImage catalog = result.loaded_catalog.image;
  const auto policy_generation_status =
      agents::ValidateAgentActionPolicyGeneration(catalog, request.action);
  if (!policy_generation_status.ok) {
    result.dispatch = StoreFailure(policy_generation_status.diagnostic_code,
                                   policy_generation_status.detail,
                                   request.action);
    return result;
  }

  std::optional<agents::DurableAgentResourceReservationRequest>
      resource_reservation;
  if (request.production_live_path &&
      request.durable_resource_reservation_required) {
    resource_reservation = ResourceReservationForAction(request);
    const auto acquired = agents::AcquireDurableAgentResourceReservation(
        &catalog, *resource_reservation);
    if (!acquired.ok) {
      result.dispatch = StoreFailure(acquired.diagnostic_code,
                                     acquired.detail,
                                     request.action);
      return result;
    }
    result.resource_reservation_acquired = true;
    result.resource_reservation_uuid = resource_reservation->reservation_uuid;

    AgentDurableCatalogStoreRequest reservation_store;
    reservation_store.context = request.context;
    reservation_store.image = catalog;
    reservation_store.evidence_uuid = resource_reservation->evidence_uuid;
    reservation_store.production_live_path = request.production_live_path;
    reservation_store.fsync_or_checkpoint_evidence =
        request.fsync_or_checkpoint_evidence;
    result.persisted_catalog =
        PersistAgentDurableCatalogImage(reservation_store);
    if (!result.persisted_catalog.ok) {
      result.dispatch = StoreFailure(
          "SB_AGENT_ACTION_STORE.RESOURCE_RESERVATION_PERSIST_FAILED",
          DiagnosticDetail(result.persisted_catalog.diagnostic),
          request.action);
      return result;
    }
    result.resource_reservation_persisted_before_dispatch = true;
    catalog = result.persisted_catalog.image;
  }

  if (request.production_live_path && !request.action.dry_run) {
    const auto existing_intent =
        std::find_if(catalog.actions.begin(), catalog.actions.end(),
                     [&request](const agents::DurableAgentActionRecord& action) {
                       return action.idempotency_key ==
                              request.action.idempotency_key;
                     });
    if (existing_intent == catalog.actions.end()) {
      catalog.actions.push_back(PendingActionIntentForDispatch(request));
      AgentDurableCatalogStoreRequest intent_store;
      intent_store.context = request.context;
      intent_store.image = catalog;
      intent_store.evidence_uuid = catalog.actions.back().evidence_uuid;
      intent_store.production_live_path = request.production_live_path;
      intent_store.fsync_or_checkpoint_evidence =
          request.fsync_or_checkpoint_evidence;
      result.persisted_catalog =
          PersistAgentDurableCatalogImage(intent_store);
      if (!result.persisted_catalog.ok) {
        result.dispatch = StoreFailure(
            "SB_AGENT_ACTION_STORE.PENDING_INTENT_PERSIST_FAILED",
            DiagnosticDetail(result.persisted_catalog.diagnostic),
            request.action);
        return result;
      }
      result.pending_action_intent_persisted_before_dispatch = true;
      catalog = result.persisted_catalog.image;
    }
  }

  agents::AgentActionDispatchRequest dispatch_request;
  dispatch_request.catalog = &catalog;
  dispatch_request.action = request.action;
  dispatch_request.authority = request.authority;
  dispatch_request.registry =
      request.engine_registry == nullptr ? nullptr
                                         : &request.engine_registry->registry();
  dispatch_request.metric_context = request.metric_context;
  dispatch_request.metric_snapshot_options = request.metric_snapshot_options;
  dispatch_request.observed_metric_snapshots = request.observed_metric_snapshots;
  dispatch_request.provider_execution_context.engine_owned_registry =
      request.engine_registry != nullptr && request.engine_registry->valid();
  dispatch_request.provider_execution_context.durable_catalog_store_context = true;
  dispatch_request.provider_execution_context.engine_request_context_present = true;
  dispatch_request.provider_execution_context.fsync_or_checkpoint_evidence =
      request.fsync_or_checkpoint_evidence;
  dispatch_request.provider_execution_context.request_id = request.context.request_id;
  dispatch_request.provider_execution_context.database_uuid =
      request.context.database_uuid.canonical;
  dispatch_request.provider_execution_context.transaction_uuid =
      request.context.transaction_uuid.canonical;
  dispatch_request.provider_execution_context.local_transaction_id =
      request.context.local_transaction_id;
  dispatch_request.provider_execution_context.registry_provenance =
      request.engine_registry == nullptr ? std::string()
                                         : request.engine_registry->provenance();
  dispatch_request.provider_execution_context.registry_evidence_uuid =
      request.engine_registry == nullptr ? std::string()
                                         : request.engine_registry->evidence_uuid();
  dispatch_request.production_live_path = request.production_live_path;
  dispatch_request.subsystem_reported_success =
      request.subsystem_reported_success;
  dispatch_request.intended_state_observed = request.intended_state_observed;
  result.dispatch = agents::DispatchAgentAction(dispatch_request);

  if (resource_reservation.has_value()) {
    const auto released = agents::ReleaseDurableAgentResourceReservation(
        &catalog,
        resource_reservation->reservation_uuid,
        result.dispatch.decision.evidence_uuid.empty()
            ? resource_reservation->evidence_uuid
            : result.dispatch.decision.evidence_uuid,
        resource_reservation->now_microseconds + 1,
        result.dispatch.status.ok
            ? agents::DurableAgentResourceReservationState::released
            : agents::DurableAgentResourceReservationState::cancelled);
    if (!released.ok) {
      result.dispatch = StoreFailure(released.diagnostic_code,
                                     released.detail,
                                     request.action);
      return result;
    }
    result.resource_reservation_released = true;
  }

  if (!result.dispatch.durable_record_written &&
      !result.dispatch.commercial_evidence_written_before_action_record) {
    if (resource_reservation.has_value()) {
      AgentDurableCatalogStoreRequest store_request;
      store_request.context = request.context;
      store_request.image = std::move(catalog);
      if (!result.persisted_catalog.image.authority.catalog_root_digest.empty()) {
        store_request.image.authority.previous_catalog_root_digest =
            result.persisted_catalog.image.authority.catalog_root_digest;
      }
      store_request.evidence_uuid = resource_reservation->evidence_uuid;
      store_request.production_live_path = request.production_live_path;
      store_request.fsync_or_checkpoint_evidence =
          request.fsync_or_checkpoint_evidence;
      result.persisted_catalog =
          PersistAgentDurableCatalogImage(store_request);
      if (!result.persisted_catalog.ok) {
        result.dispatch = StoreFailure(
            "SB_AGENT_ACTION_STORE.RESOURCE_RESERVATION_RELEASE_PERSIST_FAILED",
            DiagnosticDetail(result.persisted_catalog.diagnostic),
            request.action);
        return result;
      }
      result.persisted_to_store = true;
    }
    return result;
  }

  AgentDurableCatalogStoreRequest store_request;
  store_request.context = request.context;
  store_request.image = std::move(catalog);
  if (!result.persisted_catalog.image.authority.catalog_root_digest.empty()) {
    store_request.image.authority.previous_catalog_root_digest =
        result.persisted_catalog.image.authority.catalog_root_digest;
  }
  store_request.evidence_uuid = result.dispatch.decision.evidence_uuid;
  if (store_request.evidence_uuid.empty()) {
    store_request.evidence_uuid = result.dispatch.action_record.evidence_uuid;
  }
  if (store_request.evidence_uuid.empty()) {
    store_request.evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "agent_action_store_persist|" + request.action.action_uuid);
  }
  store_request.production_live_path = request.production_live_path;
  store_request.fsync_or_checkpoint_evidence =
      request.fsync_or_checkpoint_evidence;
  result.persisted_catalog = PersistAgentDurableCatalogImage(store_request);
  if (!result.persisted_catalog.ok) {
    result.dispatch = StoreFailure(
        "SB_AGENT_ACTION_STORE.PERSIST_FAILED",
        DiagnosticDetail(result.persisted_catalog.diagnostic),
        request.action);
    return result;
  }

  result.persisted_to_store = true;
  return result;
}

}  // namespace scratchbird::engine::internal_api
