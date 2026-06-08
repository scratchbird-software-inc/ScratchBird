// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_workload_resource_quota.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

bool AddWouldOverflow(u64 left, u64 right) {
  return right > 0 && left > (~u64{0} - right);
}

bool IsZero(const WorkloadResourceVector& value) {
  return value.memory_bytes == 0 && value.worker_slots == 0 && value.temp_bytes == 0 &&
         value.filespace_bytes == 0 && value.active_requests == 0 && value.open_cursors == 0 &&
         value.transaction_slots == 0 && value.buffer_bytes == 0 && value.udr_bytes == 0;
}

bool FieldExceedsHard(u64 used, u64 requested, u64 limit) {
  if (requested == 0) return false;
  if (AddWouldOverflow(used, requested)) return true;
  return used + requested > limit;
}

bool FieldExceedsSoft(u64 used, u64 requested, u64 limit) {
  if (requested == 0 || limit == 0) return false;
  if (AddWouldOverflow(used, requested)) return true;
  return used + requested > limit;
}

std::string FirstHardExceeded(const WorkloadResourceVector& used,
                              const WorkloadResourceVector& requested,
                              const WorkloadResourceVector& limit) {
  if (FieldExceedsHard(used.memory_bytes, requested.memory_bytes, limit.memory_bytes)) return "memory_bytes";
  if (FieldExceedsHard(used.worker_slots, requested.worker_slots, limit.worker_slots)) return "worker_slots";
  if (FieldExceedsHard(used.temp_bytes, requested.temp_bytes, limit.temp_bytes)) return "temp_bytes";
  if (FieldExceedsHard(used.filespace_bytes, requested.filespace_bytes, limit.filespace_bytes)) return "filespace_bytes";
  if (FieldExceedsHard(used.active_requests, requested.active_requests, limit.active_requests)) return "active_requests";
  if (FieldExceedsHard(used.open_cursors, requested.open_cursors, limit.open_cursors)) return "open_cursors";
  if (FieldExceedsHard(used.transaction_slots, requested.transaction_slots, limit.transaction_slots)) {
    return "transaction_slots";
  }
  if (FieldExceedsHard(used.buffer_bytes, requested.buffer_bytes, limit.buffer_bytes)) return "buffer_bytes";
  if (FieldExceedsHard(used.udr_bytes, requested.udr_bytes, limit.udr_bytes)) return "udr_bytes";
  return {};
}

std::string FirstSoftExceeded(const WorkloadResourceVector& used,
                              const WorkloadResourceVector& requested,
                              const WorkloadResourceVector& limit) {
  if (FieldExceedsSoft(used.memory_bytes, requested.memory_bytes, limit.memory_bytes)) return "memory_bytes";
  if (FieldExceedsSoft(used.worker_slots, requested.worker_slots, limit.worker_slots)) return "worker_slots";
  if (FieldExceedsSoft(used.temp_bytes, requested.temp_bytes, limit.temp_bytes)) return "temp_bytes";
  if (FieldExceedsSoft(used.filespace_bytes, requested.filespace_bytes, limit.filespace_bytes)) return "filespace_bytes";
  if (FieldExceedsSoft(used.active_requests, requested.active_requests, limit.active_requests)) return "active_requests";
  if (FieldExceedsSoft(used.open_cursors, requested.open_cursors, limit.open_cursors)) return "open_cursors";
  if (FieldExceedsSoft(used.transaction_slots, requested.transaction_slots, limit.transaction_slots)) {
    return "transaction_slots";
  }
  if (FieldExceedsSoft(used.buffer_bytes, requested.buffer_bytes, limit.buffer_bytes)) return "buffer_bytes";
  if (FieldExceedsSoft(used.udr_bytes, requested.udr_bytes, limit.udr_bytes)) return "udr_bytes";
  return {};
}

void Add(WorkloadResourceVector* used, const WorkloadResourceVector& requested) {
  used->memory_bytes += requested.memory_bytes;
  used->worker_slots += requested.worker_slots;
  used->temp_bytes += requested.temp_bytes;
  used->filespace_bytes += requested.filespace_bytes;
  used->active_requests += requested.active_requests;
  used->open_cursors += requested.open_cursors;
  used->transaction_slots += requested.transaction_slots;
  used->buffer_bytes += requested.buffer_bytes;
  used->udr_bytes += requested.udr_bytes;
}

void SubtractField(u64* used, u64 released) {
  *used = released > *used ? 0 : *used - released;
}

void Subtract(WorkloadResourceVector* used, const WorkloadResourceVector& released) {
  SubtractField(&used->memory_bytes, released.memory_bytes);
  SubtractField(&used->worker_slots, released.worker_slots);
  SubtractField(&used->temp_bytes, released.temp_bytes);
  SubtractField(&used->filespace_bytes, released.filespace_bytes);
  SubtractField(&used->active_requests, released.active_requests);
  SubtractField(&used->open_cursors, released.open_cursors);
  SubtractField(&used->transaction_slots, released.transaction_slots);
  SubtractField(&used->buffer_bytes, released.buffer_bytes);
  SubtractField(&used->udr_bytes, released.udr_bytes);
}

void AddResourceEvidence(std::vector<std::pair<std::string, std::string>>* fields,
                         const WorkloadResourceVector& resources) {
  fields->push_back({"memory_bytes", std::to_string(resources.memory_bytes)});
  fields->push_back({"worker_slots", std::to_string(resources.worker_slots)});
  fields->push_back({"temp_bytes", std::to_string(resources.temp_bytes)});
  fields->push_back({"filespace_bytes", std::to_string(resources.filespace_bytes)});
  fields->push_back({"active_requests", std::to_string(resources.active_requests)});
  fields->push_back({"open_cursors", std::to_string(resources.open_cursors)});
  fields->push_back({"transaction_slots", std::to_string(resources.transaction_slots)});
  fields->push_back({"buffer_bytes", std::to_string(resources.buffer_bytes)});
  fields->push_back({"udr_bytes", std::to_string(resources.udr_bytes)});
}

}  // namespace

const char* WorkloadClassName(WorkloadClass workload_class) {
  switch (workload_class) {
    case WorkloadClass::foreground: return "foreground";
    case WorkloadClass::background: return "background";
    case WorkloadClass::maintenance: return "maintenance";
    case WorkloadClass::parser: return "parser";
    case WorkloadClass::listener: return "listener";
    case WorkloadClass::manager: return "manager";
    case WorkloadClass::udr: return "udr";
    case WorkloadClass::cluster: return "cluster";
  }
  return "unknown";
}

const char* WorkloadAdmissionSourceName(WorkloadAdmissionSource source) {
  switch (source) {
    case WorkloadAdmissionSource::engine: return "engine";
    case WorkloadAdmissionSource::parser: return "parser";
    case WorkloadAdmissionSource::listener: return "listener";
    case WorkloadAdmissionSource::manager: return "manager";
    case WorkloadAdmissionSource::udr_runtime: return "udr_runtime";
    case WorkloadAdmissionSource::cluster_remote: return "cluster_remote";
  }
  return "unknown";
}

const char* WorkloadAdmissionDecisionName(WorkloadAdmissionDecisionClass decision) {
  switch (decision) {
    case WorkloadAdmissionDecisionClass::admitted: return "admitted";
    case WorkloadAdmissionDecisionClass::throttled: return "throttled";
    case WorkloadAdmissionDecisionClass::queued: return "queued";
    case WorkloadAdmissionDecisionClass::rejected: return "rejected";
    case WorkloadAdmissionDecisionClass::drain_refused: return "drain_refused";
    case WorkloadAdmissionDecisionClass::failed_closed: return "failed_closed";
  }
  return "unknown";
}

const char* WorkloadReleaseReasonName(WorkloadReleaseReason reason) {
  switch (reason) {
    case WorkloadReleaseReason::success: return "success";
    case WorkloadReleaseReason::failure: return "failure";
    case WorkloadReleaseReason::cancellation: return "cancellation";
    case WorkloadReleaseReason::shutdown: return "shutdown";
  }
  return "unknown";
}

std::string SerializeWorkloadQuotaEvidence(const WorkloadQuotaEvidence& evidence) {
  std::ostringstream out;
  out << "operation_uuid=" << evidence.operation_uuid << '\n';
  out << "pool_id=" << evidence.pool_id << '\n';
  out << "workload_class=" << evidence.workload_class << '\n';
  out << "source=" << evidence.source << '\n';
  out << "decision=" << evidence.decision << '\n';
  out << "diagnostic_code=" << evidence.diagnostic_code << '\n';
  out << "redaction_class=" << evidence.redaction_class << '\n';
  out << "reservation_created=" << (evidence.reservation_created ? "true" : "false") << '\n';
  out << "maintenance_override=" << (evidence.maintenance_override ? "true" : "false") << '\n';
  out << "principal=redacted\n";
  for (const auto& field : evidence.fields) {
    out << field.first << '=' << field.second << '\n';
  }
  return out.str();
}

AgentRuntimeStatus WorkloadResourceQuotaController::RegisterPool(WorkloadResourcePoolConfig config) {
  if (config.pool_id.empty()) {
    return AgentError("WORKLOAD_RESOURCE.POOL_ID_REQUIRED");
  }
  const auto existing = std::find_if(pools_.begin(), pools_.end(), [&](const PoolState& pool) {
    return pool.config.pool_id == config.pool_id;
  });
  if (existing != pools_.end()) {
    return AgentError("WORKLOAD_RESOURCE.POOL_ALREADY_REGISTERED", config.pool_id);
  }
  pools_.push_back({std::move(config), {}, 0});
  return AgentOk();
}

WorkloadAdmissionResult WorkloadResourceQuotaController::Refuse(const WorkloadAdmissionRequest& request,
                                                                WorkloadAdmissionDecisionClass decision,
                                                                std::string code,
                                                                std::string detail,
                                                                std::string resource_name) {
  WorkloadAdmissionResult result;
  result.decision = decision;
  result.status = AgentError(code, detail);
  result.diagnostic = {std::move(code), std::move(detail), std::move(resource_name)};
  result.evidence = BuildEvidence(request, decision, result.diagnostic.diagnostic_code, false);
  if (!result.diagnostic.resource_name.empty()) {
    result.evidence.fields.push_back({"resource_name", result.diagnostic.resource_name});
  }
  evidence_log_.push_back(result.evidence);
  return result;
}

WorkloadQuotaEvidence WorkloadResourceQuotaController::BuildEvidence(
    const WorkloadAdmissionRequest& request,
    WorkloadAdmissionDecisionClass decision,
    const std::string& code,
    bool reservation_created) const {
  WorkloadQuotaEvidence evidence;
  evidence.operation_uuid = request.request_uuid;
  evidence.pool_id = request.pool_id;
  evidence.workload_class = WorkloadClassName(request.workload_class);
  evidence.source = WorkloadAdmissionSourceName(request.source);
  evidence.decision = WorkloadAdmissionDecisionName(decision);
  evidence.diagnostic_code = code;
  evidence.reservation_created = reservation_created;
  evidence.maintenance_override = request.maintenance_override;
  evidence.fields.push_back({"principal", "redacted"});
  AddResourceEvidence(&evidence.fields, request.requested);
  return evidence;
}

WorkloadAdmissionResult WorkloadResourceQuotaController::Admit(const WorkloadAdmissionRequest& request) {
  if (request.request_uuid.empty()) {
    return Refuse(request, WorkloadAdmissionDecisionClass::failed_closed,
                  "WORKLOAD_RESOURCE.REQUEST_ID_REQUIRED", "request UUID is required");
  }
  if (shutdown_draining_) {
    return Refuse(request, WorkloadAdmissionDecisionClass::drain_refused,
                  "WORKLOAD_RESOURCE.SHUTDOWN_DRAIN_NO_NEW_ADMISSION", shutdown_reason_);
  }
  if (request.cancellation_requested) {
    return Refuse(request, WorkloadAdmissionDecisionClass::rejected,
                  "WORKLOAD_RESOURCE.ADMISSION_CANCELLED", "request was cancelled before reservation");
  }
  if (IsZero(request.requested)) {
    return Refuse(request, WorkloadAdmissionDecisionClass::failed_closed,
                  "WORKLOAD_RESOURCE.EMPTY_RESERVATION_REFUSED",
                  "all routed work must carry a non-empty resource reservation");
  }
  if (request.cluster_scoped && !request.cluster_authority_available) {
    return Refuse(request, WorkloadAdmissionDecisionClass::failed_closed,
                  "WORKLOAD_RESOURCE.CLUSTER_AUTHORITY_UNAVAILABLE",
                  "cluster-scoped quota path is unavailable in standalone mode");
  }

  auto pool = std::find_if(pools_.begin(), pools_.end(), [&](const PoolState& candidate) {
    return candidate.config.pool_id == request.pool_id;
  });
  if (pool == pools_.end()) {
    return Refuse(request, WorkloadAdmissionDecisionClass::failed_closed,
                  "WORKLOAD_RESOURCE.POOL_NOT_FOUND", request.pool_id);
  }
  if (pool->config.cluster_only && !request.cluster_authority_available) {
    return Refuse(request, WorkloadAdmissionDecisionClass::failed_closed,
                  "WORKLOAD_RESOURCE.CLUSTER_POOL_UNAVAILABLE",
                  "cluster-only quota pool has no cluster authority");
  }
  if (pool->config.workload_class != request.workload_class) {
    return Refuse(request, WorkloadAdmissionDecisionClass::failed_closed,
                  "WORKLOAD_RESOURCE.WORKLOAD_CLASS_MISMATCH", request.pool_id);
  }
  const auto duplicate = std::find_if(active_reservations_.begin(), active_reservations_.end(),
                                      [&](const WorkloadReservation& reservation) {
                                        return reservation.request_uuid == request.request_uuid;
                                      });
  if (duplicate != active_reservations_.end()) {
    return Refuse(request, WorkloadAdmissionDecisionClass::failed_closed,
                  "WORKLOAD_RESOURCE.DUPLICATE_REQUEST", request.request_uuid);
  }

  const std::string hard_exceeded =
      FirstHardExceeded(pool->used, request.requested, pool->config.limits.hard);
  if (!hard_exceeded.empty()) {
    return Refuse(request, WorkloadAdmissionDecisionClass::rejected,
                  "WORKLOAD_RESOURCE.HARD_DENIED", "hard quota exceeded", hard_exceeded);
  }

  WorkloadAdmissionDecisionClass decision = WorkloadAdmissionDecisionClass::admitted;
  std::string diagnostic_code = "WORKLOAD_RESOURCE.ADMITTED";
  const std::string soft_exceeded =
      FirstSoftExceeded(pool->used, request.requested, pool->config.limits.soft);
  const bool maintenance_override =
      request.maintenance_override && pool->config.limits.maintenance_override_allowed;
  if (!soft_exceeded.empty() && !maintenance_override) {
    if (pool->config.limits.queue_on_soft_limit) {
      if (pool->queued_requests >= pool->config.limits.max_queued_requests) {
        return Refuse(request, WorkloadAdmissionDecisionClass::rejected,
                      "WORKLOAD_RESOURCE.QUEUE_FULL", "quota queue is full", soft_exceeded);
      }
      ++pool->queued_requests;
      WorkloadAdmissionResult result;
      result.decision = WorkloadAdmissionDecisionClass::queued;
      result.status = AgentOk();
      result.diagnostic = {"WORKLOAD_RESOURCE.SOFT_QUEUED", "soft quota queued", soft_exceeded};
      result.evidence = BuildEvidence(request, result.decision, result.diagnostic.diagnostic_code, false);
      result.evidence.fields.push_back({"resource_name", soft_exceeded});
      evidence_log_.push_back(result.evidence);
      return result;
    }
    decision = WorkloadAdmissionDecisionClass::throttled;
    diagnostic_code = "WORKLOAD_RESOURCE.SOFT_THROTTLE";
  } else if (!soft_exceeded.empty() && maintenance_override) {
    diagnostic_code = "WORKLOAD_RESOURCE.MAINTENANCE_OVERRIDE_ADMITTED";
  }

  WorkloadReservation reservation;
  reservation.token_id = request.request_uuid + ":reservation";
  reservation.request_uuid = request.request_uuid;
  reservation.pool_id = request.pool_id;
  reservation.workload_class = request.workload_class;
  reservation.source = request.source;
  reservation.resources = request.requested;
  reservation.active = true;
  Add(&pool->used, request.requested);
  active_reservations_.push_back(reservation);

  WorkloadAdmissionResult result;
  result.decision = decision;
  result.status = AgentOk();
  result.reservation = std::move(reservation);
  result.diagnostic = {diagnostic_code, decision == WorkloadAdmissionDecisionClass::throttled
                                            ? "soft quota throttle required"
                                            : "quota reservation admitted",
                       soft_exceeded};
  result.evidence = BuildEvidence(request, decision, diagnostic_code, true);
  if (!soft_exceeded.empty()) {
    result.evidence.fields.push_back({"resource_name", soft_exceeded});
  }
  evidence_log_.push_back(result.evidence);
  return result;
}

AgentRuntimeStatus WorkloadResourceQuotaController::ReleaseInternal(const std::string& token_id,
                                                                    WorkloadReleaseReason reason) {
  auto reservation = std::find_if(active_reservations_.begin(), active_reservations_.end(),
                                  [&](const WorkloadReservation& candidate) {
                                    return candidate.token_id == token_id;
                                  });
  if (reservation == active_reservations_.end()) {
    const auto already_released =
        std::find_if(released_reservations_.begin(), released_reservations_.end(),
                     [&](const WorkloadReservation& candidate) { return candidate.token_id == token_id; });
    if (already_released != released_reservations_.end()) {
      return AgentError("WORKLOAD_RESOURCE.RELEASE_ALREADY_RECORDED", token_id);
    }
    return AgentError("WORKLOAD_RESOURCE.RESERVATION_NOT_FOUND", token_id);
  }

  auto pool = std::find_if(pools_.begin(), pools_.end(), [&](const PoolState& candidate) {
    return candidate.config.pool_id == reservation->pool_id;
  });
  if (pool == pools_.end()) {
    return AgentError("WORKLOAD_RESOURCE.RELEASE_POOL_NOT_FOUND", reservation->pool_id);
  }

  Subtract(&pool->used, reservation->resources);
  reservation->active = false;
  WorkloadReservation released = *reservation;
  released_reservations_.push_back(released);
  active_reservations_.erase(reservation);

  WorkloadQuotaEvidence evidence;
  evidence.operation_uuid = released.request_uuid;
  evidence.pool_id = released.pool_id;
  evidence.workload_class = WorkloadClassName(released.workload_class);
  evidence.source = WorkloadAdmissionSourceName(released.source);
  evidence.decision = "released";
  evidence.diagnostic_code = std::string("WORKLOAD_RESOURCE.RELEASED.") + WorkloadReleaseReasonName(reason);
  evidence.reservation_created = true;
  evidence.fields.push_back({"token_id", token_id});
  evidence.fields.push_back({"principal", "redacted"});
  AddResourceEvidence(&evidence.fields, released.resources);
  evidence_log_.push_back(std::move(evidence));
  return AgentOk();
}

AgentRuntimeStatus WorkloadResourceQuotaController::Release(const std::string& token_id,
                                                           WorkloadReleaseReason reason) {
  return ReleaseInternal(token_id, reason);
}

AgentRuntimeStatus WorkloadResourceQuotaController::Cancel(const std::string& token_id) {
  return ReleaseInternal(token_id, WorkloadReleaseReason::cancellation);
}

void WorkloadResourceQuotaController::BeginShutdownDrain(std::string reason) {
  shutdown_draining_ = true;
  shutdown_reason_ = std::move(reason);
}

std::vector<AgentRuntimeStatus> WorkloadResourceQuotaController::DrainForShutdown() {
  std::vector<AgentRuntimeStatus> releases;
  while (!active_reservations_.empty()) {
    releases.push_back(ReleaseInternal(active_reservations_.front().token_id,
                                       WorkloadReleaseReason::shutdown));
  }
  return releases;
}

WorkloadResourceVector WorkloadResourceQuotaController::UsageForPool(const std::string& pool_id) const {
  const auto pool = std::find_if(pools_.begin(), pools_.end(), [&](const PoolState& candidate) {
    return candidate.config.pool_id == pool_id;
  });
  return pool == pools_.end() ? WorkloadResourceVector{} : pool->used;
}

u64 WorkloadResourceQuotaController::ActiveReservationCount() const {
  return static_cast<u64>(active_reservations_.size());
}

u64 WorkloadResourceQuotaController::QueuedRequestCount(const std::string& pool_id) const {
  const auto pool = std::find_if(pools_.begin(), pools_.end(), [&](const PoolState& candidate) {
    return candidate.config.pool_id == pool_id;
  });
  return pool == pools_.end() ? 0 : pool->queued_requests;
}

}  // namespace scratchbird::core::agents
