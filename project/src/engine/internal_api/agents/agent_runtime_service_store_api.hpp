// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: AEIC_ENGINE_OWNED_AGENT_RUNTIME_SERVICE
// SEARCH_KEY: AEIC_DURABLE_AGENT_SCHEDULER_LEASES

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agent_runtime_service.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct AgentRuntimeServiceStoreOpenRequest {
  EngineRequestContext context;
  std::vector<scratchbird::core::agents::CanonicalAgentManifestEntry> manifest;
  bool production_live_path = true;
  bool worker_foreground_protection_enabled = false;
  bool crash_recovery_mode = false;
  std::string service_owner_uuid;
  std::string evidence_uuid;
  bool fsync_or_checkpoint_evidence = false;
};

class AgentRuntimeServiceStore {
 public:
  scratchbird::core::agents::AgentRuntimeServiceResult Open(
      AgentRuntimeServiceStoreOpenRequest request);
  void SetContext(EngineRequestContext context);
  scratchbird::core::agents::AgentRuntimeServiceResult Start(
      const std::string& evidence_uuid,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult Drain(
      const std::string& evidence_uuid,
      scratchbird::core::agents::u64 now_microseconds,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult Shutdown(
      const std::string& evidence_uuid,
      scratchbird::core::agents::u64 now_microseconds,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult Recover(
      const std::string& evidence_uuid,
      scratchbird::core::agents::u64 now_microseconds,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult AcquireLease(
      scratchbird::core::agents::DurableLeaseRequest request,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult HeartbeatLease(
      scratchbird::core::agents::DurableLeaseRequest request,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult CancelLease(
      scratchbird::core::agents::DurableLeaseRequest request,
      scratchbird::core::agents::DurableAgentLeaseState terminal_state,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult RecordSupervisionFailure(
      const std::string& instance_uuid,
      const scratchbird::core::agents::AgentPolicy& policy,
      scratchbird::core::agents::AgentSupervisionFailureKind failure_kind,
      scratchbird::core::agents::u64 now_microseconds,
      std::string detail,
      const std::string& evidence_uuid,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult RequestSupervisionRestart(
      const std::string& instance_uuid,
      const scratchbird::core::agents::AgentPolicy& policy,
      scratchbird::core::agents::u64 now_microseconds,
      const std::string& evidence_uuid,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult CancelAgentExecution(
      const std::string& instance_uuid,
      scratchbird::core::agents::u64 now_microseconds,
      std::string reason,
      const std::string& evidence_uuid,
      bool fsync_or_checkpoint_evidence);
  scratchbird::core::agents::AgentRuntimeServiceResult QuarantineAgentExecution(
      const std::string& instance_uuid,
      scratchbird::core::agents::u64 now_microseconds,
      std::string reason,
      const std::string& evidence_uuid,
      bool fsync_or_checkpoint_evidence);

  const scratchbird::core::agents::AgentRuntimeService& service() const {
    return service_;
  }

 private:
  scratchbird::core::agents::AgentRuntimeServiceResult PersistResult(
      scratchbird::core::agents::AgentRuntimeServiceResult result,
      bool fsync_or_checkpoint_evidence);

  EngineRequestContext context_;
  scratchbird::core::agents::AgentRuntimeService service_;
  std::string last_persisted_catalog_root_digest_;
  bool opened_ = false;
  bool production_live_path_ = true;
};

}  // namespace scratchbird::engine::internal_api
