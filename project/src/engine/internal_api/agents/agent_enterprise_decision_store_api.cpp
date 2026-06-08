// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_enterprise_decision_store_api.hpp"

// SEARCH_KEY: AEIC_ENTERPRISE_DECISION_EVIDENCE_STORE

#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents;

std::string DiagnosticDetail(const EngineApiDiagnostic& diagnostic) {
  if (!diagnostic.detail.empty()) { return diagnostic.detail; }
  if (!diagnostic.message_key.empty()) { return diagnostic.message_key; }
  return diagnostic.code;
}

agents::AgentEnterpriseDecisionEvidenceResult StoreFailure(
    std::string code,
    std::string detail) {
  agents::AgentEnterpriseDecisionEvidenceResult result;
  result.status = agents::AgentError(std::move(code), std::move(detail));
  return result;
}

agents::DurableAgentResourceReservationRequest ResourceReservationForDecision(
    const AgentEnterpriseDecisionStoreRequest& request) {
  agents::DurableAgentResourceReservationRequest reservation;
  reservation.reservation_key =
      "enterprise_decision:" + request.decision.agent_type_id + ":" +
      request.decision.operation_id + ":" + request.decision.diagnostic_code;
  reservation.reservation_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_enterprise_decision_resource_reservation|" +
          reservation.reservation_key);
  reservation.owner_scope = request.decision.principal_uuid.empty()
                                ? request.context.principal_uuid.canonical
                                : request.decision.principal_uuid;
  reservation.agent_type_id = request.decision.agent_type_id;
  reservation.operation_id = request.decision.operation_id;
  reservation.now_microseconds =
      request.decision.created_at_microseconds == 0
          ? 1
          : request.decision.created_at_microseconds;
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
          "agent_enterprise_decision_resource_reservation_evidence|" +
          reservation.reservation_uuid);
  return reservation;
}

}  // namespace

AgentEnterpriseDecisionStoreResult AppendEnterpriseAgentDecisionEvidenceToStore(
    const AgentEnterpriseDecisionStoreRequest& request) {
  AgentEnterpriseDecisionStoreResult result;
  result.loaded_catalog =
      LoadAgentDurableCatalogImage(request.context, request.production_live_path);
  if (!result.loaded_catalog.ok) {
    result.decision =
        StoreFailure("SB_AGENT_ENTERPRISE_EVIDENCE_STORE.LOAD_FAILED",
                     DiagnosticDetail(result.loaded_catalog.diagnostic));
    return result;
  }
  result.loaded_from_store = true;

  agents::DurableAgentCatalogImage catalog = result.loaded_catalog.image;
  agents::AgentEnterpriseDecisionEvidenceRequest decision = request.decision;
  decision.catalog = &catalog;
  result.decision = agents::AppendEnterpriseAgentDecisionEvidence(decision);
  if (!result.decision.status.ok || result.decision.idempotent_replay) {
    return result;
  }
  if (request.production_live_path &&
      request.durable_resource_reservation_required) {
    const auto reservation = ResourceReservationForDecision(request);
    const auto acquired =
        agents::AcquireDurableAgentResourceReservation(&catalog, reservation);
    if (!acquired.ok) {
      result.decision =
          StoreFailure(acquired.diagnostic_code, acquired.detail);
      return result;
    }
    result.resource_reservation_acquired = true;
    result.resource_reservation_uuid = reservation.reservation_uuid;
    const auto released = agents::ReleaseDurableAgentResourceReservation(
        &catalog,
        reservation.reservation_uuid,
        result.decision.evidence_uuid,
        reservation.now_microseconds + 1,
        agents::DurableAgentResourceReservationState::released);
    if (!released.ok) {
      result.decision =
          StoreFailure(released.diagnostic_code, released.detail);
      return result;
    }
    result.resource_reservation_released = true;
  }

  AgentDurableCatalogStoreRequest store_request;
  store_request.context = request.context;
  store_request.image = std::move(catalog);
  store_request.evidence_uuid = result.decision.evidence_uuid;
  store_request.expected_catalog_root_digest =
      result.loaded_catalog.image.authority.catalog_root_digest;
  store_request.production_live_path = request.production_live_path;
  store_request.fsync_or_checkpoint_evidence =
      request.fsync_or_checkpoint_evidence;
  result.persisted_catalog = PersistAgentDurableCatalogImage(store_request);
  if (!result.persisted_catalog.ok) {
    result.decision =
        StoreFailure("SB_AGENT_ENTERPRISE_EVIDENCE_STORE.PERSIST_FAILED",
                     DiagnosticDetail(result.persisted_catalog.diagnostic));
    return result;
  }
  result.persisted_to_store = true;
  return result;
}

}  // namespace scratchbird::engine::internal_api
