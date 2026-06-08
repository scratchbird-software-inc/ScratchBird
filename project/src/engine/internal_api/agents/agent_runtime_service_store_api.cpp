// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_runtime_service_store_api.hpp"

// SEARCH_KEY: AEIC_ENGINE_OWNED_AGENT_RUNTIME_SERVICE
// SEARCH_KEY: AEIC_DURABLE_AGENT_SCHEDULER_LEASES

#include "metric_contracts.hpp"

#include <array>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents;

agents::AgentRuntimeServiceResult ServiceStoreError(std::string event,
                                                    std::string code,
                                                    std::string detail) {
  agents::AgentRuntimeServiceResult result;
  result.status = agents::AgentError(std::move(code), std::move(detail));
  result.evidence.lifecycle_event = std::move(event);
  result.evidence.diagnostic_code = result.status.diagnostic_code;
  return result;
}

std::string DiagnosticDetail(const EngineApiDiagnostic& diagnostic) {
  if (!diagnostic.detail.empty()) { return diagnostic.detail; }
  if (!diagnostic.message_key.empty()) { return diagnostic.message_key; }
  return diagnostic.code;
}

void PublishRuntimeServiceMetrics(
    const agents::AgentRuntimeServiceResult& result) {
  const std::string event = result.evidence.lifecycle_event.empty()
                                ? "unknown"
                                : result.evidence.lifecycle_event;
  const std::string decision = result.status.ok ? "ok" : "refused";
  (void)scratchbird::core::metrics::RecordAgentRuntimeServiceEvent(
      event, decision, result.status.diagnostic_code);

  const std::array<agents::DurableAgentLeaseState, 7> lease_states = {
      agents::DurableAgentLeaseState::none,
      agents::DurableAgentLeaseState::acquired,
      agents::DurableAgentLeaseState::draining,
      agents::DurableAgentLeaseState::cancelled,
      agents::DurableAgentLeaseState::quarantined,
      agents::DurableAgentLeaseState::replay_pending,
      agents::DurableAgentLeaseState::expired};
  for (const auto state : lease_states) {
    double count = 0.0;
    for (const auto& lease : result.catalog.leases) {
      if (lease.state == state) { count += 1.0; }
    }
    (void)scratchbird::core::metrics::PublishAgentRuntimeServiceLeaseCount(
        count, agents::DurableAgentLeaseStateName(state));
  }

  const std::array<agents::DurableAgentActionState, 6> action_states = {
      agents::DurableAgentActionState::pending,
      agents::DurableAgentActionState::running,
      agents::DurableAgentActionState::completed,
      agents::DurableAgentActionState::cancelled,
      agents::DurableAgentActionState::replay_pending,
      agents::DurableAgentActionState::quarantined};
  for (const auto state : action_states) {
    double count = 0.0;
    for (const auto& action : result.catalog.actions) {
      if (action.state == state) { count += 1.0; }
    }
    (void)scratchbird::core::metrics::PublishAgentRuntimeServiceActionCount(
        count, agents::DurableAgentActionStateName(state));
  }

  (void)scratchbird::core::metrics::PublishAgentRuntimeServiceHistoryCount(
      static_cast<double>(result.catalog.retained_history.size()));
  (void)scratchbird::core::metrics::PublishAgentRuntimeServiceCatalogGeneration(
      static_cast<double>(result.catalog.authority.catalog_generation));
}

}  // namespace

agents::AgentRuntimeServiceResult AgentRuntimeServiceStore::Open(
    AgentRuntimeServiceStoreOpenRequest request) {
  context_ = request.context;
  production_live_path_ = request.production_live_path;
  opened_ = false;

  auto loaded = LoadAgentDurableCatalogImage(context_, request.production_live_path);
  if (!loaded.ok) {
    return ServiceStoreError("open",
                             "SB_AGENT_SERVICE.DURABLE_CATALOG_STORE_LOAD_FAILED",
                             DiagnosticDetail(loaded.diagnostic));
  }
  last_persisted_catalog_root_digest_ =
      loaded.image.authority.catalog_root_digest;

  agents::AgentRuntimeServiceOpenRequest core_request;
  core_request.manifest = std::move(request.manifest);
  core_request.catalog = std::move(loaded.image);
  core_request.production_live_path = request.production_live_path;
  core_request.worker_foreground_protection_enabled =
      request.worker_foreground_protection_enabled;
  core_request.crash_recovery_mode = request.crash_recovery_mode;
  core_request.service_owner_uuid = std::move(request.service_owner_uuid);
  core_request.evidence_uuid = std::move(request.evidence_uuid);
  auto result = service_.Open(std::move(core_request));
  if (!result.status.ok) { return result; }
  opened_ = true;
  return PersistResult(std::move(result),
                       request.fsync_or_checkpoint_evidence);
}

void AgentRuntimeServiceStore::SetContext(EngineRequestContext context) {
  context_ = std::move(context);
}

agents::AgentRuntimeServiceResult AgentRuntimeServiceStore::Start(
    const std::string& evidence_uuid,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.Start(evidence_uuid);
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult AgentRuntimeServiceStore::Drain(
    const std::string& evidence_uuid,
    agents::u64 now_microseconds,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.Drain(evidence_uuid, now_microseconds);
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult AgentRuntimeServiceStore::Shutdown(
    const std::string& evidence_uuid,
    agents::u64 now_microseconds,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.Shutdown(evidence_uuid, now_microseconds);
  auto persisted = PersistResult(std::move(result), fsync_or_checkpoint_evidence);
  if (persisted.status.ok) { opened_ = false; }
  return persisted;
}

agents::AgentRuntimeServiceResult AgentRuntimeServiceStore::Recover(
    const std::string& evidence_uuid,
    agents::u64 now_microseconds,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.Recover(evidence_uuid, now_microseconds);
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult AgentRuntimeServiceStore::AcquireLease(
    agents::DurableLeaseRequest request,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.AcquireLease(std::move(request));
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult AgentRuntimeServiceStore::HeartbeatLease(
    agents::DurableLeaseRequest request,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.HeartbeatLease(std::move(request));
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult AgentRuntimeServiceStore::CancelLease(
    agents::DurableLeaseRequest request,
    agents::DurableAgentLeaseState terminal_state,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.CancelLease(std::move(request), terminal_state);
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult
AgentRuntimeServiceStore::RecordSupervisionFailure(
    const std::string& instance_uuid,
    const agents::AgentPolicy& policy,
    agents::AgentSupervisionFailureKind failure_kind,
    agents::u64 now_microseconds,
    std::string detail,
    const std::string& evidence_uuid,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.RecordSupervisionFailure(instance_uuid,
                                                 policy,
                                                 failure_kind,
                                                 now_microseconds,
                                                 std::move(detail),
                                                 evidence_uuid);
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult
AgentRuntimeServiceStore::RequestSupervisionRestart(
    const std::string& instance_uuid,
    const agents::AgentPolicy& policy,
    agents::u64 now_microseconds,
    const std::string& evidence_uuid,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.RequestSupervisionRestart(instance_uuid,
                                                  policy,
                                                  now_microseconds,
                                                  evidence_uuid);
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult
AgentRuntimeServiceStore::CancelAgentExecution(
    const std::string& instance_uuid,
    agents::u64 now_microseconds,
    std::string reason,
    const std::string& evidence_uuid,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.CancelAgentExecution(instance_uuid,
                                             now_microseconds,
                                             std::move(reason),
                                             evidence_uuid);
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult
AgentRuntimeServiceStore::QuarantineAgentExecution(
    const std::string& instance_uuid,
    agents::u64 now_microseconds,
    std::string reason,
    const std::string& evidence_uuid,
    bool fsync_or_checkpoint_evidence) {
  auto result = service_.QuarantineAgentExecution(instance_uuid,
                                                 now_microseconds,
                                                 std::move(reason),
                                                 evidence_uuid);
  return PersistResult(std::move(result), fsync_or_checkpoint_evidence);
}

agents::AgentRuntimeServiceResult AgentRuntimeServiceStore::PersistResult(
    agents::AgentRuntimeServiceResult result,
    bool fsync_or_checkpoint_evidence) {
  if (!result.status.ok) { return result; }
  if (!opened_ && result.evidence.lifecycle_event != "open") {
    return ServiceStoreError(result.evidence.lifecycle_event,
                             "SB_AGENT_SERVICE_STORE.NOT_OPEN",
                             "runtime service store is not open");
  }

  AgentDurableCatalogStoreRequest store_request;
  store_request.context = context_;
  store_request.image = result.catalog;
  store_request.evidence_uuid = result.evidence.evidence_uuid;
  store_request.expected_catalog_root_digest =
      last_persisted_catalog_root_digest_;
  store_request.production_live_path = production_live_path_;
  store_request.fsync_or_checkpoint_evidence = fsync_or_checkpoint_evidence;
  const auto stored = PersistAgentDurableCatalogImage(store_request);
  if (!stored.ok) {
    return ServiceStoreError(result.evidence.lifecycle_event,
                             "SB_AGENT_SERVICE_STORE.PERSIST_FAILED",
                             DiagnosticDetail(stored.diagnostic));
  }
  service_.AdoptPersistedCatalog(stored.image);
  last_persisted_catalog_root_digest_ =
      stored.image.authority.catalog_root_digest;
  result.catalog = stored.image;
  result.evidence.catalog_authority_evidence_uuid =
      stored.image.authority.evidence_uuid;
  result.evidence.mga_transaction_uuid =
      stored.image.authority.mga_transaction_uuid;
  result.evidence.durable_catalog_authority =
      stored.image.authority.durable_catalog_authority;
  result.evidence.mga_transaction_evidence =
      stored.image.authority.mga_transaction_evidence;
  PublishRuntimeServiceMetrics(result);
  return result;
}

}  // namespace scratchbird::engine::internal_api
