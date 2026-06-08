// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime_service.hpp"

#include <utility>

namespace scratchbird::core::agents {
namespace {

AgentInstanceRecord* FindServiceInstance(DurableAgentCatalogImage* catalog,
                                         const std::string& instance_uuid) {
  if (catalog == nullptr || instance_uuid.empty()) { return nullptr; }
  for (auto& instance : catalog->instances) {
    if (instance.instance_uuid == instance_uuid) { return &instance; }
  }
  return nullptr;
}

}  // namespace

AgentRuntimeServiceResult AgentRuntimeService::Open(
    AgentRuntimeServiceOpenRequest request) {
  manifest_ = std::move(request.manifest);
  catalog_ = std::move(request.catalog);
  production_live_path_ = request.production_live_path;
  worker_foreground_protection_enabled_ =
      request.worker_foreground_protection_enabled;
  crash_recovery_mode_ = request.crash_recovery_mode;
  service_owner_uuid_ = request.service_owner_uuid;
  opened_ = false;
  started_ = false;

  if (manifest_.empty()) {
    return Finish("open", AgentError("SB_AGENT_SERVICE.MANIFEST_REQUIRED"),
                  request.evidence_uuid);
  }
  if (request.service_owner_uuid.empty() || request.evidence_uuid.empty()) {
    return Finish("open", AgentError("SB_AGENT_SERVICE.EVIDENCE_REQUIRED"),
                  request.evidence_uuid);
  }
  if (production_live_path_) {
    const auto status = ValidateDurableAgentCatalogForProduction(catalog_);
    if (!status.ok) { return Finish("open", status, request.evidence_uuid); }
    if (!worker_foreground_protection_enabled_) {
      return Finish("open",
                    AgentError("SB_AGENT_SERVICE.FOREGROUND_PROTECTION_REQUIRED"),
                    request.evidence_uuid);
    }
  }

  opened_ = true;
  return Finish("open", {true, "SB_AGENT_SERVICE.OPENED", request.service_owner_uuid},
                request.evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::Start(const std::string& evidence_uuid) {
  if (!opened_) {
    return Finish("start", AgentError("SB_AGENT_SERVICE.NOT_OPEN"), evidence_uuid);
  }
  if (production_live_path_) {
    const auto status = ValidateDurableAgentCatalogForProduction(catalog_);
    if (!status.ok) { return Finish("start", status, evidence_uuid); }
  }
  if (!worker_foreground_protection_enabled_) {
    return Finish("start",
                  AgentError("SB_AGENT_SERVICE.FOREGROUND_PROTECTION_REQUIRED"),
                  evidence_uuid);
  }
  started_ = true;
  return Finish("start", {true, "SB_AGENT_SERVICE.STARTED", evidence_uuid},
                evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::Drain(const std::string& evidence_uuid,
                                                     u64 now_microseconds) {
  if (!opened_) {
    return Finish("drain", AgentError("SB_AGENT_SERVICE.NOT_OPEN"), evidence_uuid);
  }
  for (auto& lease : catalog_.leases) {
    if (lease.state == DurableAgentLeaseState::acquired) {
      lease.state = DurableAgentLeaseState::draining;
      lease.expires_at_microseconds = now_microseconds;
      lease.evidence_uuid = evidence_uuid;
    }
  }
  started_ = false;
  return Finish("drain", {true, "SB_AGENT_SERVICE.DRAINED", evidence_uuid},
                evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::Shutdown(const std::string& evidence_uuid,
                                                        u64 now_microseconds) {
  if (!opened_) {
    return Finish("shutdown", AgentError("SB_AGENT_SERVICE.NOT_OPEN"), evidence_uuid);
  }
  for (auto& lease : catalog_.leases) {
    if (lease.state == DurableAgentLeaseState::acquired ||
        lease.state == DurableAgentLeaseState::draining) {
      lease.state = DurableAgentLeaseState::cancelled;
      lease.expires_at_microseconds = now_microseconds;
      lease.evidence_uuid = evidence_uuid;
    }
  }
  started_ = false;
  opened_ = false;
  return Finish("shutdown", {true, "SB_AGENT_SERVICE.SHUTDOWN", evidence_uuid},
                evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::Recover(const std::string& evidence_uuid,
                                                       u64 now_microseconds) {
  if (!opened_) {
    return Finish("recover", AgentError("SB_AGENT_SERVICE.NOT_OPEN"), evidence_uuid);
  }
  if (!crash_recovery_mode_) {
    return Finish("recover",
                  AgentError("SB_AGENT_SERVICE.CRASH_RECOVERY_MODE_REQUIRED"),
                  evidence_uuid);
  }
  const auto status =
      RecoverDurableAgentCatalogAfterCrash(&catalog_, now_microseconds, evidence_uuid);
  return Finish("recover", status, evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::AcquireLease(
    DurableLeaseRequest request) {
  if (!opened_ || !started_) {
    return Finish("lease_acquire",
                  AgentError("SB_AGENT_SERVICE.NOT_STARTED"),
                  request.evidence_uuid);
  }
  if (production_live_path_) {
    const auto status = ValidateDurableAgentCatalogForProduction(catalog_);
    if (!status.ok) { return Finish("lease_acquire", status, request.evidence_uuid); }
  }
  const auto status = AcquireDurableAgentLease(&catalog_, request);
  return Finish("lease_acquire", status, request.evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::HeartbeatLease(
    DurableLeaseRequest request) {
  if (!opened_ || !started_) {
    return Finish("lease_heartbeat",
                  AgentError("SB_AGENT_SERVICE.NOT_STARTED"),
                  request.evidence_uuid);
  }
  if (production_live_path_) {
    const auto status = ValidateDurableAgentCatalogForProduction(catalog_);
    if (!status.ok) {
      return Finish("lease_heartbeat", status, request.evidence_uuid);
    }
  }
  const auto status = HeartbeatDurableAgentLease(&catalog_, request);
  return Finish("lease_heartbeat", status, request.evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::CancelLease(
    DurableLeaseRequest request,
    DurableAgentLeaseState terminal_state) {
  if (!opened_) {
    return Finish("lease_cancel",
                  AgentError("SB_AGENT_SERVICE.NOT_OPEN"),
                  request.evidence_uuid);
  }
  if (production_live_path_) {
    const auto status = ValidateDurableAgentCatalogForProduction(catalog_);
    if (!status.ok) { return Finish("lease_cancel", status, request.evidence_uuid); }
  }
  const auto status =
      CancelDurableAgentLease(&catalog_, request, terminal_state);
  return Finish("lease_cancel", status, request.evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::RecordSupervisionFailure(
    const std::string& instance_uuid,
    const AgentPolicy& policy,
    AgentSupervisionFailureKind failure_kind,
    u64 now_microseconds,
    std::string detail,
    const std::string& evidence_uuid) {
  if (!opened_) {
    return Finish("supervision_failure",
                  AgentError("SB_AGENT_SERVICE.NOT_OPEN"),
                  evidence_uuid);
  }
  if (production_live_path_) {
    const auto status = ValidateDurableAgentCatalogForProduction(catalog_);
    if (!status.ok) { return Finish("supervision_failure", status, evidence_uuid); }
  }
  auto* instance = FindServiceInstance(&catalog_, instance_uuid);
  if (instance == nullptr) {
    return Finish("supervision_failure",
                  AgentError("SB_AGENT_SUPERVISION.INSTANCE_NOT_FOUND",
                             instance_uuid),
                  evidence_uuid);
  }
  const auto decision = scratchbird::core::agents::RecordAgentSupervisionFailure(
      instance, policy, failure_kind, now_microseconds, std::move(detail));
  (void)decision;
  return Finish("supervision_failure",
                {true, "SB_AGENT_SUPERVISION.FAILURE_RECORDED", instance_uuid},
                evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::RequestSupervisionRestart(
    const std::string& instance_uuid,
    const AgentPolicy& policy,
    u64 now_microseconds,
    const std::string& evidence_uuid) {
  if (!opened_) {
    return Finish("supervision_restart",
                  AgentError("SB_AGENT_SERVICE.NOT_OPEN"),
                  evidence_uuid);
  }
  if (production_live_path_) {
    const auto status = ValidateDurableAgentCatalogForProduction(catalog_);
    if (!status.ok) { return Finish("supervision_restart", status, evidence_uuid); }
  }
  auto* instance = FindServiceInstance(&catalog_, instance_uuid);
  if (instance == nullptr) {
    return Finish("supervision_restart",
                  AgentError("SB_AGENT_SUPERVISION.INSTANCE_NOT_FOUND",
                             instance_uuid),
                  evidence_uuid);
  }
  const auto status =
      scratchbird::core::agents::RequestAgentSupervisionRestart(
          instance, policy, now_microseconds);
  return Finish("supervision_restart", status, evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::CancelAgentExecution(
    const std::string& instance_uuid,
    u64 now_microseconds,
    std::string reason,
    const std::string& evidence_uuid) {
  if (!opened_) {
    return Finish("agent_cancel",
                  AgentError("SB_AGENT_SERVICE.NOT_OPEN"),
                  evidence_uuid);
  }
  if (production_live_path_) {
    const auto status = ValidateDurableAgentCatalogForProduction(catalog_);
    if (!status.ok) { return Finish("agent_cancel", status, evidence_uuid); }
  }
  auto* instance = FindServiceInstance(&catalog_, instance_uuid);
  if (instance == nullptr) {
    return Finish("agent_cancel",
                  AgentError("SB_AGENT_SUPERVISION.INSTANCE_NOT_FOUND",
                             instance_uuid),
                  evidence_uuid);
  }
  const auto status = scratchbird::core::agents::CancelAgentRun(
      instance, now_microseconds, std::move(reason));
  return Finish("agent_cancel", status, evidence_uuid);
}

AgentRuntimeServiceResult AgentRuntimeService::QuarantineAgentExecution(
    const std::string& instance_uuid,
    u64 now_microseconds,
    std::string reason,
    const std::string& evidence_uuid) {
  if (!opened_) {
    return Finish("agent_quarantine",
                  AgentError("SB_AGENT_SERVICE.NOT_OPEN"),
                  evidence_uuid);
  }
  if (production_live_path_) {
    const auto status = ValidateDurableAgentCatalogForProduction(catalog_);
    if (!status.ok) { return Finish("agent_quarantine", status, evidence_uuid); }
  }
  auto* instance = FindServiceInstance(&catalog_, instance_uuid);
  if (instance == nullptr) {
    return Finish("agent_quarantine",
                  AgentError("SB_AGENT_SUPERVISION.INSTANCE_NOT_FOUND",
                             instance_uuid),
                  evidence_uuid);
  }
  const auto status = scratchbird::core::agents::QuarantineAgentInstance(
      instance, now_microseconds, std::move(reason));
  return Finish("agent_quarantine", status, evidence_uuid);
}

void AgentRuntimeService::AdoptPersistedCatalog(
    DurableAgentCatalogImage catalog) {
  catalog_ = std::move(catalog);
}

AgentRuntimeServiceResult AgentRuntimeService::Finish(std::string event,
                                                      AgentRuntimeStatus status,
                                                      std::string evidence_uuid) {
  AgentRuntimeServiceEvidence evidence;
  evidence.evidence_uuid = std::move(evidence_uuid);
  evidence.lifecycle_event = std::move(event);
  evidence.diagnostic_code = status.diagnostic_code;
  evidence.catalog_authority_evidence_uuid = catalog_.authority.evidence_uuid;
  evidence.mga_transaction_uuid = catalog_.authority.mga_transaction_uuid;
  evidence.durable_catalog_authority = catalog_.authority.durable_catalog_authority;
  evidence.mga_transaction_evidence = catalog_.authority.mga_transaction_evidence;
  evidence.worker_foreground_protection_enabled =
      worker_foreground_protection_enabled_;
  evidence.crash_recovery_mode = crash_recovery_mode_;
  evidence.agents_are_transaction_authority = false;
  evidence.agents_are_finality_authority = false;
  evidence.agents_are_visibility_authority = false;
  evidence.agents_are_recovery_authority = false;
  evidence.agents_are_security_authority = false;
  persisted_evidence_.push_back(evidence);
  if (catalog_.source == AgentCatalogStateSource::durable_catalog_image &&
      catalog_.authority.durable_catalog_authority &&
      catalog_.authority.mga_transaction_evidence &&
      !evidence.evidence_uuid.empty() &&
      !catalog_.authority.catalog_storage_uuid.empty()) {
    DurableAgentHistoryRecord history;
    history.history_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
        "agent_runtime_service_history|" + evidence.lifecycle_event + "|" +
        evidence.evidence_uuid + "|" + status.diagnostic_code);
    history.subject_uuid = service_owner_uuid_.empty() ? evidence.evidence_uuid
                                                       : service_owner_uuid_;
    history.event_kind = "agent_runtime_service." + evidence.lifecycle_event;
    history.diagnostic_code = status.diagnostic_code;
    history.evidence_uuid = evidence.evidence_uuid;
    history.recorded_at_microseconds =
        catalog_.retained_history.size() + catalog_.authority.catalog_generation + 1;
    catalog_.retained_history.push_back(std::move(history));
    const auto refresh =
        RefreshDurableAgentCatalogAuthorityDigest(&catalog_, evidence.evidence_uuid);
    if (!refresh.ok && status.ok) {
      status = refresh;
      evidence.diagnostic_code = refresh.diagnostic_code;
      persisted_evidence_.back().diagnostic_code = refresh.diagnostic_code;
    }
  }

  AgentRuntimeServiceResult result;
  result.status = std::move(status);
  result.evidence = evidence;
  result.catalog = catalog_;
  return result;
}

}  // namespace scratchbird::core::agents
