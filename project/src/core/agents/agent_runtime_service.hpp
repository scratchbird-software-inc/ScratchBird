// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_DURABLE_AGENT_RUNTIME_SERVICE

#include "agent_durable_catalog.hpp"
#include "agent_runtime_manifest.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

struct AgentRuntimeServiceOpenRequest {
  std::vector<CanonicalAgentManifestEntry> manifest;
  DurableAgentCatalogImage catalog;
  bool production_live_path = true;
  bool worker_foreground_protection_enabled = false;
  bool crash_recovery_mode = false;
  std::string service_owner_uuid;
  std::string evidence_uuid;
};

struct AgentRuntimeServiceEvidence {
  std::string evidence_uuid;
  std::string lifecycle_event;
  std::string diagnostic_code;
  std::string catalog_authority_evidence_uuid;
  std::string mga_transaction_uuid;
  bool durable_catalog_authority = false;
  bool mga_transaction_evidence = false;
  bool worker_foreground_protection_enabled = false;
  bool crash_recovery_mode = false;
  bool agents_are_transaction_authority = false;
  bool agents_are_finality_authority = false;
  bool agents_are_visibility_authority = false;
  bool agents_are_recovery_authority = false;
  bool agents_are_security_authority = false;
};

struct AgentRuntimeServiceResult {
  AgentRuntimeStatus status;
  AgentRuntimeServiceEvidence evidence;
  DurableAgentCatalogImage catalog;
};

class AgentRuntimeService {
 public:
  AgentRuntimeServiceResult Open(AgentRuntimeServiceOpenRequest request);
  AgentRuntimeServiceResult Start(const std::string& evidence_uuid);
  AgentRuntimeServiceResult Drain(const std::string& evidence_uuid, u64 now_microseconds);
  AgentRuntimeServiceResult Shutdown(const std::string& evidence_uuid, u64 now_microseconds);
  AgentRuntimeServiceResult Recover(const std::string& evidence_uuid, u64 now_microseconds);
  AgentRuntimeServiceResult AcquireLease(DurableLeaseRequest request);
  AgentRuntimeServiceResult HeartbeatLease(DurableLeaseRequest request);
  AgentRuntimeServiceResult CancelLease(DurableLeaseRequest request,
                                        DurableAgentLeaseState terminal_state);
  AgentRuntimeServiceResult RecordSupervisionFailure(
      const std::string& instance_uuid,
      const AgentPolicy& policy,
      AgentSupervisionFailureKind failure_kind,
      u64 now_microseconds,
      std::string detail,
      const std::string& evidence_uuid);
  AgentRuntimeServiceResult RequestSupervisionRestart(
      const std::string& instance_uuid,
      const AgentPolicy& policy,
      u64 now_microseconds,
      const std::string& evidence_uuid);
  AgentRuntimeServiceResult CancelAgentExecution(
      const std::string& instance_uuid,
      u64 now_microseconds,
      std::string reason,
      const std::string& evidence_uuid);
  AgentRuntimeServiceResult QuarantineAgentExecution(
      const std::string& instance_uuid,
      u64 now_microseconds,
      std::string reason,
      const std::string& evidence_uuid);

  const DurableAgentCatalogImage& catalog() const { return catalog_; }
  const std::vector<AgentRuntimeServiceEvidence>& persisted_evidence() const {
    return persisted_evidence_;
  }
  void AdoptPersistedCatalog(DurableAgentCatalogImage catalog);

 private:
  AgentRuntimeServiceResult Finish(std::string event,
                                   AgentRuntimeStatus status,
                                   std::string evidence_uuid);

  DurableAgentCatalogImage catalog_;
  std::vector<CanonicalAgentManifestEntry> manifest_;
  std::vector<AgentRuntimeServiceEvidence> persisted_evidence_;
  std::string service_owner_uuid_;
  bool opened_ = false;
  bool started_ = false;
  bool production_live_path_ = true;
  bool worker_foreground_protection_enabled_ = false;
  bool crash_recovery_mode_ = false;
};

}  // namespace scratchbird::core::agents
