// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::agents {

// SEARCH_KEY: DBLC_013M_WORKLOAD_RESOURCE_QUOTA_LIFECYCLE
// Engine-owned workload resource quota primitive. Route, parser, listener,
// manager, and extension callers must receive a reservation before work starts.

enum class WorkloadClass {
  foreground,
  background,
  maintenance,
  parser,
  listener,
  manager,
  udr,
  cluster
};

enum class WorkloadAdmissionSource {
  engine,
  parser,
  listener,
  manager,
  udr_runtime,
  cluster_remote
};

enum class WorkloadAdmissionDecisionClass {
  admitted,
  throttled,
  queued,
  rejected,
  drain_refused,
  failed_closed
};

enum class WorkloadReleaseReason {
  success,
  failure,
  cancellation,
  shutdown
};

struct WorkloadResourceVector {
  u64 memory_bytes = 0;
  u64 worker_slots = 0;
  u64 temp_bytes = 0;
  u64 filespace_bytes = 0;
  u64 active_requests = 0;
  u64 open_cursors = 0;
  u64 transaction_slots = 0;
  u64 buffer_bytes = 0;
  u64 udr_bytes = 0;
};

struct WorkloadQuotaLimits {
  WorkloadResourceVector hard;
  WorkloadResourceVector soft;
  bool queue_on_soft_limit = false;
  bool maintenance_override_allowed = true;
  u64 max_queued_requests = 0;
};

struct WorkloadResourcePoolConfig {
  std::string pool_id;
  WorkloadClass workload_class = WorkloadClass::foreground;
  WorkloadQuotaLimits limits;
  bool cluster_only = false;
};

struct WorkloadAdmissionRequest {
  std::string request_uuid;
  std::string pool_id;
  WorkloadClass workload_class = WorkloadClass::foreground;
  WorkloadAdmissionSource source = WorkloadAdmissionSource::engine;
  WorkloadResourceVector requested;
  bool maintenance_override = false;
  bool cancellation_requested = false;
  bool cluster_scoped = false;
  bool cluster_authority_available = false;
  std::string principal_tag;
};

struct WorkloadQuotaDiagnostic {
  std::string diagnostic_code;
  std::string detail;
  std::string resource_name;
};

struct WorkloadQuotaEvidence {
  std::string operation_uuid;
  std::string pool_id;
  std::string workload_class;
  std::string source;
  std::string decision;
  std::string diagnostic_code;
  std::string redaction_class = "operational_redacted";
  bool reservation_created = false;
  bool maintenance_override = false;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct WorkloadReservation {
  std::string token_id;
  std::string request_uuid;
  std::string pool_id;
  WorkloadClass workload_class = WorkloadClass::foreground;
  WorkloadAdmissionSource source = WorkloadAdmissionSource::engine;
  WorkloadResourceVector resources;
  bool active = false;
};

struct WorkloadAdmissionResult {
  WorkloadAdmissionDecisionClass decision = WorkloadAdmissionDecisionClass::failed_closed;
  AgentRuntimeStatus status;
  WorkloadReservation reservation;
  WorkloadQuotaEvidence evidence;
  WorkloadQuotaDiagnostic diagnostic;

  bool reservation_created() const { return reservation.active; }
};

const char* WorkloadClassName(WorkloadClass workload_class);
const char* WorkloadAdmissionSourceName(WorkloadAdmissionSource source);
const char* WorkloadAdmissionDecisionName(WorkloadAdmissionDecisionClass decision);
const char* WorkloadReleaseReasonName(WorkloadReleaseReason reason);

std::string SerializeWorkloadQuotaEvidence(const WorkloadQuotaEvidence& evidence);

class WorkloadResourceQuotaController {
 public:
  AgentRuntimeStatus RegisterPool(WorkloadResourcePoolConfig config);
  WorkloadAdmissionResult Admit(const WorkloadAdmissionRequest& request);
  AgentRuntimeStatus Release(const std::string& token_id, WorkloadReleaseReason reason);
  AgentRuntimeStatus Cancel(const std::string& token_id);
  void BeginShutdownDrain(std::string reason);
  std::vector<AgentRuntimeStatus> DrainForShutdown();

  WorkloadResourceVector UsageForPool(const std::string& pool_id) const;
  u64 ActiveReservationCount() const;
  u64 QueuedRequestCount(const std::string& pool_id) const;
  bool shutdown_draining() const { return shutdown_draining_; }
  const std::vector<WorkloadQuotaEvidence>& evidence_log() const { return evidence_log_; }

 private:
  struct PoolState {
    WorkloadResourcePoolConfig config;
    WorkloadResourceVector used;
    u64 queued_requests = 0;
  };

  WorkloadAdmissionResult Refuse(const WorkloadAdmissionRequest& request,
                                 WorkloadAdmissionDecisionClass decision,
                                 std::string code,
                                 std::string detail,
                                 std::string resource_name = {});
  WorkloadQuotaEvidence BuildEvidence(const WorkloadAdmissionRequest& request,
                                      WorkloadAdmissionDecisionClass decision,
                                      const std::string& code,
                                      bool reservation_created) const;
  AgentRuntimeStatus ReleaseInternal(const std::string& token_id,
                                     WorkloadReleaseReason reason);

  std::vector<PoolState> pools_;
  std::vector<WorkloadReservation> active_reservations_;
  std::vector<WorkloadReservation> released_reservations_;
  std::vector<WorkloadQuotaEvidence> evidence_log_;
  bool shutdown_draining_ = false;
  std::string shutdown_reason_;
};

}  // namespace scratchbird::core::agents
