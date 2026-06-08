// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "resource_governance_admission.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string BoolText(bool value) { return value ? "true" : "false"; }

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

AgentRuntimeStatus LocalAgentOk() {
  return {true, "SB_AGENT_OK", {}};
}

AgentRuntimeStatus LocalAgentError(std::string code, std::string detail = {}) {
  return {false, std::move(code), std::move(detail)};
}

bool SourceIsRuntime(ResourceGovernanceDescriptorSource source) {
  switch (source) {
    case ResourceGovernanceDescriptorSource::kRuntimePolicy:
    case ResourceGovernanceDescriptorSource::kServerRuntimeApi:
    case ResourceGovernanceDescriptorSource::kAgentRuntime:
      return true;
    case ResourceGovernanceDescriptorSource::kUnknown:
    case ResourceGovernanceDescriptorSource::kExecution_PlanEvidence:
      return false;
  }
  return false;
}

bool ContainsExecution_PlanSource(const std::string& source) {
  return source.find("docs/") != std::string::npos ||
         source.find("execution-plans") != std::string::npos ||
         source.find("contracts") != std::string::npos ||
         source.find("references") != std::string::npos ||
         source.find("findings") != std::string::npos;
}

bool KnownFamily(ResourceGovernanceFamily family) {
  switch (family) {
    case ResourceGovernanceFamily::kQueryMemoryArena:
    case ResourceGovernanceFamily::kAdaptiveTuningKnob:
    case ResourceGovernanceFamily::kAsyncPageIo:
    case ResourceGovernanceFamily::kParallelPhysicalPipeline:
    case ResourceGovernanceFamily::kPreparedNativeSpecialization:
    case ResourceGovernanceFamily::kScoringKernelAccelerator:
    case ResourceGovernanceFamily::kAcceleratorProviderCache:
    case ResourceGovernanceFamily::kBulkCopyLane:
    case ResourceGovernanceFamily::kOptimizedCompression:
    case ResourceGovernanceFamily::kOptimizedCache:
    case ResourceGovernanceFamily::kOptimizedVectorMaintenance:
    case ResourceGovernanceFamily::kOptimizedNoSqlProvider:
    case ResourceGovernanceFamily::kStreamingCursor:
    case ResourceGovernanceFamily::kBackgroundJob:
      return true;
    case ResourceGovernanceFamily::kUnknown:
      return false;
  }
  return false;
}

bool KnownAction(ResourceGovernanceAction action) {
  switch (action) {
    case ResourceGovernanceAction::kAdmit:
    case ResourceGovernanceAction::kSlowdownDegrade:
    case ResourceGovernanceAction::kExactScalarFallback:
    case ResourceGovernanceAction::kCancel:
    case ResourceGovernanceAction::kFailClosed:
      return true;
  }
  return false;
}

std::string FirstNegativeField(const ResourceGovernanceQuotaVector& value) {
  if (value.memory_bytes < 0) return "memory_bytes";
  if (value.device_memory_bytes < 0) return "device_memory_bytes";
  if (value.pinned_memory_bytes < 0) return "pinned_memory_bytes";
  if (value.io_bytes < 0) return "io_bytes";
  if (value.io_ops < 0) return "io_ops";
  if (value.worker_threads < 0) return "worker_threads";
  if (value.backlog_items < 0) return "backlog_items";
  if (value.candidate_rows < 0) return "candidate_rows";
  if (value.cache_entries < 0) return "cache_entries";
  if (value.batch_rows < 0) return "batch_rows";
  if (value.fragments < 0) return "fragments";
  if (value.lanes < 0) return "lanes";
  if (value.time_budget_microseconds < 0) return "time_budget_microseconds";
  return {};
}

std::string FirstUnboundedLimit(const ResourceGovernanceQuotaVector& value) {
  if (value.memory_bytes <= 0) return "memory_bytes";
  if (value.device_memory_bytes <= 0) return "device_memory_bytes";
  if (value.pinned_memory_bytes <= 0) return "pinned_memory_bytes";
  if (value.io_bytes <= 0) return "io_bytes";
  if (value.io_ops <= 0) return "io_ops";
  if (value.worker_threads <= 0) return "worker_threads";
  if (value.backlog_items <= 0) return "backlog_items";
  if (value.candidate_rows <= 0) return "candidate_rows";
  if (value.cache_entries <= 0) return "cache_entries";
  if (value.batch_rows <= 0) return "batch_rows";
  if (value.fragments <= 0) return "fragments";
  if (value.lanes <= 0) return "lanes";
  if (value.time_budget_microseconds <= 0) return "time_budget_microseconds";
  return {};
}

std::string FirstExceeded(const ResourceGovernanceQuotaVector& requested,
                          const ResourceGovernanceQuotaVector& limits) {
  if (requested.memory_bytes > limits.memory_bytes) return "memory_bytes";
  if (requested.device_memory_bytes > limits.device_memory_bytes) {
    return "device_memory_bytes";
  }
  if (requested.pinned_memory_bytes > limits.pinned_memory_bytes) {
    return "pinned_memory_bytes";
  }
  if (requested.io_bytes > limits.io_bytes) return "io_bytes";
  if (requested.io_ops > limits.io_ops) return "io_ops";
  if (requested.worker_threads > limits.worker_threads) {
    return "worker_threads";
  }
  if (requested.backlog_items > limits.backlog_items) return "backlog_items";
  if (requested.candidate_rows > limits.candidate_rows) {
    return "candidate_rows";
  }
  if (requested.cache_entries > limits.cache_entries) {
    return "cache_entries";
  }
  if (requested.batch_rows > limits.batch_rows) return "batch_rows";
  if (requested.fragments > limits.fragments) return "fragments";
  if (requested.lanes > limits.lanes) return "lanes";
  if (requested.time_budget_microseconds >
      limits.time_budget_microseconds) {
    return "time_budget_microseconds";
  }
  return {};
}

std::string FirstExceededWithActive(const ResourceGovernanceQuotaVector& active,
                                    const ResourceGovernanceQuotaVector& requested,
                                    const ResourceGovernanceQuotaVector& limits) {
  auto exceeds = [](std::int64_t used, std::int64_t next, std::int64_t limit) {
    return next > 0 && (next > limit || used > limit - next);
  };
  if (exceeds(active.memory_bytes, requested.memory_bytes, limits.memory_bytes)) return "memory_bytes";
  if (exceeds(active.device_memory_bytes, requested.device_memory_bytes, limits.device_memory_bytes)) return "device_memory_bytes";
  if (exceeds(active.pinned_memory_bytes, requested.pinned_memory_bytes, limits.pinned_memory_bytes)) return "pinned_memory_bytes";
  if (exceeds(active.io_bytes, requested.io_bytes, limits.io_bytes)) return "io_bytes";
  if (exceeds(active.io_ops, requested.io_ops, limits.io_ops)) return "io_ops";
  if (exceeds(active.worker_threads, requested.worker_threads, limits.worker_threads)) return "worker_threads";
  if (exceeds(active.backlog_items, requested.backlog_items, limits.backlog_items)) return "backlog_items";
  if (exceeds(active.candidate_rows, requested.candidate_rows, limits.candidate_rows)) return "candidate_rows";
  if (exceeds(active.cache_entries, requested.cache_entries, limits.cache_entries)) return "cache_entries";
  if (exceeds(active.batch_rows, requested.batch_rows, limits.batch_rows)) return "batch_rows";
  if (exceeds(active.fragments, requested.fragments, limits.fragments)) return "fragments";
  if (exceeds(active.lanes, requested.lanes, limits.lanes)) return "lanes";
  if (exceeds(active.time_budget_microseconds, requested.time_budget_microseconds, limits.time_budget_microseconds)) {
    return "time_budget_microseconds";
  }
  return {};
}

void AddQuota(ResourceGovernanceQuotaVector* target,
              const ResourceGovernanceQuotaVector& value) {
  target->memory_bytes += value.memory_bytes;
  target->device_memory_bytes += value.device_memory_bytes;
  target->pinned_memory_bytes += value.pinned_memory_bytes;
  target->io_bytes += value.io_bytes;
  target->io_ops += value.io_ops;
  target->worker_threads += value.worker_threads;
  target->backlog_items += value.backlog_items;
  target->candidate_rows += value.candidate_rows;
  target->cache_entries += value.cache_entries;
  target->batch_rows += value.batch_rows;
  target->fragments += value.fragments;
  target->lanes += value.lanes;
  target->time_budget_microseconds += value.time_budget_microseconds;
}

void SubtractQuota(ResourceGovernanceQuotaVector* target,
                   const ResourceGovernanceQuotaVector& value) {
  target->memory_bytes -= value.memory_bytes;
  target->device_memory_bytes -= value.device_memory_bytes;
  target->pinned_memory_bytes -= value.pinned_memory_bytes;
  target->io_bytes -= value.io_bytes;
  target->io_ops -= value.io_ops;
  target->worker_threads -= value.worker_threads;
  target->backlog_items -= value.backlog_items;
  target->candidate_rows -= value.candidate_rows;
  target->cache_entries -= value.cache_entries;
  target->batch_rows -= value.batch_rows;
  target->fragments -= value.fragments;
  target->lanes -= value.lanes;
  target->time_budget_microseconds -= value.time_budget_microseconds;
}

void AddVectorEvidence(std::vector<std::string>* evidence,
                       const std::string& prefix,
                       const ResourceGovernanceQuotaVector& value) {
  Add(evidence, prefix + ".memory_bytes=" + std::to_string(value.memory_bytes));
  Add(evidence, prefix + ".device_memory_bytes=" +
                    std::to_string(value.device_memory_bytes));
  Add(evidence, prefix + ".pinned_memory_bytes=" +
                    std::to_string(value.pinned_memory_bytes));
  Add(evidence, prefix + ".io_bytes=" + std::to_string(value.io_bytes));
  Add(evidence, prefix + ".io_ops=" + std::to_string(value.io_ops));
  Add(evidence, prefix + ".worker_threads=" +
                    std::to_string(value.worker_threads));
  Add(evidence, prefix + ".backlog_items=" +
                    std::to_string(value.backlog_items));
  Add(evidence, prefix + ".candidate_rows=" +
                    std::to_string(value.candidate_rows));
  Add(evidence, prefix + ".cache_entries=" +
                    std::to_string(value.cache_entries));
  Add(evidence, prefix + ".batch_rows=" + std::to_string(value.batch_rows));
  Add(evidence, prefix + ".fragments=" + std::to_string(value.fragments));
  Add(evidence, prefix + ".lanes=" + std::to_string(value.lanes));
  Add(evidence, prefix + ".time_budget_microseconds=" +
                    std::to_string(value.time_budget_microseconds));
}

ResourceGovernanceAdmissionResult Finish(
    const ResourceGovernanceAdmissionRequest& request,
    ResourceGovernanceAction action,
    bool ok,
    bool fail_closed,
    std::string diagnostic_code,
    std::string diagnostic_detail,
    std::string exceeded_quota = {}) {
  ResourceGovernanceAdmissionResult result;
  result.ok = ok;
  result.fail_closed = fail_closed;
  result.reservation_created = ok && action == ResourceGovernanceAction::kAdmit;
  result.action = action;
  result.diagnostic_code = std::move(diagnostic_code);
  result.diagnostic_detail = std::move(diagnostic_detail);
  result.status.ok = ok;
  result.status.diagnostic_code = result.diagnostic_code;
  result.status.detail = result.diagnostic_detail;
  result.exceeded_quota = std::move(exceeded_quota);
  Add(&result.evidence, "resource_governance.route=odf106");
  Add(&result.evidence, "resource_governance.family=" +
                            std::string(ResourceGovernanceFamilyName(
                                request.descriptor.family)));
  Add(&result.evidence, "resource_governance.operation_id=" +
                            request.operation_id);
  Add(&result.evidence, "resource_governance.descriptor_id=" +
                            request.descriptor.descriptor_id);
  Add(&result.evidence, "resource_governance.action=" +
                            std::string(ResourceGovernanceActionName(action)));
  Add(&result.evidence, "resource_governance.ok=" + BoolText(ok));
  Add(&result.evidence,
      "resource_governance.fail_closed=" + BoolText(fail_closed));
  Add(&result.evidence,
      "resource_governance.reservation_created=" +
          BoolText(result.reservation_created));
  Add(&result.evidence, "resource_governance.diagnostic_code=" +
                            result.diagnostic_code);
  if (!result.exceeded_quota.empty()) {
    Add(&result.evidence, "resource_governance.exceeded_quota=" +
                              result.exceeded_quota);
  }
  Add(&result.evidence, "resource_governance.descriptor_source=" +
                            std::string(ResourceGovernanceDescriptorSourceName(
                                request.descriptor.source)));
  Add(&result.evidence, "resource_governance.runtime_dependency_present=" +
                            BoolText(request.descriptor.runtime_dependency_present));
  Add(&result.evidence, "resource_governance.benchmark_clean=" +
                            BoolText(request.descriptor.benchmark_clean));
  Add(&result.evidence,
      "resource_governance.all_quota_dimensions_bounded=" +
          BoolText(FirstUnboundedLimit(request.descriptor.limits).empty()));
  Add(&result.evidence,
      "resource_governance.hidden_unlimited_default=false");
  Add(&result.evidence,
      "resource_governance.mga_finality_authority=engine_transaction_inventory");
  Add(&result.evidence, "resource_governance.security_authority=engine");
  Add(&result.evidence, "resource_governance.parser_or_donor_authority=false");
  Add(&result.evidence,
      "resource_governance.provider_transaction_finality_authority=false");
  Add(&result.evidence,
      "resource_governance.provider_visibility_authority=false");
  Add(&result.evidence,
      "resource_governance.provider_recovery_authority=false");
  Add(&result.evidence, "resource_governance.wal_recovery_authority=false");
  AddVectorEvidence(&result.evidence, "resource_governance.requested",
                    request.requested);
  AddVectorEvidence(&result.evidence, "resource_governance.limit",
                    request.descriptor.limits);
  return result;
}

ResourceGovernanceAdmissionResult Refuse(
    const ResourceGovernanceAdmissionRequest& request,
    std::string diagnostic_code,
    std::string diagnostic_detail,
    std::string exceeded_quota = {}) {
  return Finish(request, ResourceGovernanceAction::kFailClosed, false, true,
                std::move(diagnostic_code), std::move(diagnostic_detail),
                std::move(exceeded_quota));
}

}  // namespace

const char* ResourceGovernanceFamilyName(ResourceGovernanceFamily family) {
  switch (family) {
    case ResourceGovernanceFamily::kQueryMemoryArena:
      return "query_memory_arena";
    case ResourceGovernanceFamily::kAdaptiveTuningKnob:
      return "adaptive_tuning_knob";
    case ResourceGovernanceFamily::kAsyncPageIo:
      return "async_page_io";
    case ResourceGovernanceFamily::kParallelPhysicalPipeline:
      return "parallel_physical_pipeline";
    case ResourceGovernanceFamily::kPreparedNativeSpecialization:
      return "prepared_native_specialization";
    case ResourceGovernanceFamily::kScoringKernelAccelerator:
      return "scoring_kernel_accelerator";
    case ResourceGovernanceFamily::kAcceleratorProviderCache:
      return "accelerator_provider_cache";
    case ResourceGovernanceFamily::kBulkCopyLane:
      return "bulk_copy_lane";
    case ResourceGovernanceFamily::kOptimizedCompression:
      return "optimized_compression";
    case ResourceGovernanceFamily::kOptimizedCache:
      return "optimized_cache";
    case ResourceGovernanceFamily::kOptimizedVectorMaintenance:
      return "optimized_vector_maintenance";
    case ResourceGovernanceFamily::kOptimizedNoSqlProvider:
      return "optimized_nosql_provider";
    case ResourceGovernanceFamily::kStreamingCursor:
      return "streaming_cursor";
    case ResourceGovernanceFamily::kBackgroundJob:
      return "background_job";
    case ResourceGovernanceFamily::kUnknown:
      break;
  }
  return "unknown";
}

const char* ResourceGovernanceActionName(ResourceGovernanceAction action) {
  switch (action) {
    case ResourceGovernanceAction::kAdmit:
      return "admit";
    case ResourceGovernanceAction::kSlowdownDegrade:
      return "slowdown_degrade";
    case ResourceGovernanceAction::kExactScalarFallback:
      return "exact_scalar_fallback";
    case ResourceGovernanceAction::kCancel:
      return "cancel";
    case ResourceGovernanceAction::kFailClosed:
      return "fail_closed";
  }
  return "fail_closed";
}

const char* ResourceGovernanceDescriptorSourceName(
    ResourceGovernanceDescriptorSource source) {
  switch (source) {
    case ResourceGovernanceDescriptorSource::kRuntimePolicy:
      return "runtime_policy";
    case ResourceGovernanceDescriptorSource::kServerRuntimeApi:
      return "server_runtime_api";
    case ResourceGovernanceDescriptorSource::kAgentRuntime:
      return "agent_runtime";
    case ResourceGovernanceDescriptorSource::kExecution_PlanEvidence:
      return "execution_plan_evidence";
    case ResourceGovernanceDescriptorSource::kUnknown:
      break;
  }
  return "unknown";
}

const char* ResourceGovernanceReservationReleaseReasonName(
    ResourceGovernanceReservationReleaseReason reason) {
  switch (reason) {
    case ResourceGovernanceReservationReleaseReason::kRelease:
      return "release";
    case ResourceGovernanceReservationReleaseReason::kCancel:
      return "cancel";
    case ResourceGovernanceReservationReleaseReason::kTimeout:
      return "timeout";
    case ResourceGovernanceReservationReleaseReason::kDisconnect:
      return "disconnect";
    case ResourceGovernanceReservationReleaseReason::kShutdown:
      return "shutdown";
  }
  return "release";
}

const char* HierarchicalMemoryBudgetScopeKindName(
    HierarchicalMemoryBudgetScopeKind kind) {
  switch (kind) {
    case HierarchicalMemoryBudgetScopeKind::kDatabase:
      return "database";
    case HierarchicalMemoryBudgetScopeKind::kTenant:
      return "tenant";
    case HierarchicalMemoryBudgetScopeKind::kUser:
      return "user";
    case HierarchicalMemoryBudgetScopeKind::kRole:
      return "role";
    case HierarchicalMemoryBudgetScopeKind::kSession:
      return "session";
    case HierarchicalMemoryBudgetScopeKind::kTransaction:
      return "transaction";
    case HierarchicalMemoryBudgetScopeKind::kStatement:
      return "statement";
    case HierarchicalMemoryBudgetScopeKind::kQuery:
      return "query";
    case HierarchicalMemoryBudgetScopeKind::kOperator:
      return "operator";
    case HierarchicalMemoryBudgetScopeKind::kBackground:
      return "background";
    case HierarchicalMemoryBudgetScopeKind::kUnknown:
      break;
  }
  return "unknown";
}

ResourceGovernanceAdmissionResult AdmitResourceGovernance(
    const ResourceGovernanceAdmissionRequest& request) {
  if (request.operation_id.empty()) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.OPERATION_ID_REQUIRED",
                  "operation_id_required");
  }
  if (!KnownFamily(request.descriptor.family) ||
      request.descriptor.descriptor_id.empty()) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.DESCRIPTOR_REQUIRED",
                  "known_runtime_descriptor_required");
  }
  if (request.expected_family != ResourceGovernanceFamily::kUnknown &&
      request.descriptor.family != request.expected_family) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.FAMILY_MISMATCH_REFUSED",
                  "quota_descriptor_family_does_not_match_route");
  }
  if (!request.descriptor.runtime_dependency_present ||
      !SourceIsRuntime(request.descriptor.source) ||
      ContainsExecution_PlanSource(request.descriptor.source_path_or_label)) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.EXECUTION_PLAN_DESCRIPTOR_REFUSED",
                  "runtime_policy_descriptor_required_no_execution_plan_dependency");
  }
  if (!request.descriptor.benchmark_clean) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.BENCHMARK_DIRTY",
                  "benchmark_clean_quota_descriptor_required");
  }
  if (request.descriptor.corrupt || !KnownAction(request.descriptor.over_limit_action)) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.CORRUPT_DESCRIPTOR_REFUSED",
                  "quota_descriptor_corrupt");
  }
  if (request.descriptor.descriptor_generation == 0 ||
      request.descriptor.expected_generation == 0 ||
      request.descriptor.descriptor_generation !=
          request.descriptor.expected_generation ||
      request.stale_runtime_epoch) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.STALE_DESCRIPTOR_REFUSED",
                  "quota_descriptor_generation_stale");
  }
  if (!request.descriptor.engine_mga_authoritative ||
      !request.descriptor.security_authoritative ||
      request.descriptor.parser_or_donor_authority ||
      request.descriptor.provider_transaction_finality_authority ||
      request.descriptor.provider_visibility_authority ||
      request.descriptor.provider_recovery_authority ||
      request.descriptor.wal_recovery_authority) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.UNSAFE_AUTHORITY_REFUSED",
                  "engine_mga_security_authority_required");
  }
  if (const auto field = FirstNegativeField(request.descriptor.limits);
      !field.empty()) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.NEGATIVE_LIMIT_REFUSED",
                  "negative_quota_limit", field);
  }
  if (const auto field = FirstNegativeField(request.requested);
      !field.empty()) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.NEGATIVE_REQUEST_REFUSED",
                  "negative_quota_request", field);
  }
  if (const auto field = FirstUnboundedLimit(request.descriptor.limits);
      !field.empty()) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.UNBOUNDED_LIMIT_REFUSED",
                  "all_quota_dimensions_require_positive_bounds", field);
  }
  if (request.require_exact_scalar_fallback_available &&
      !request.exact_scalar_fallback_available) {
    return Refuse(request, "SB_RESOURCE_GOVERNANCE.SCALAR_FALLBACK_REQUIRED",
                  "exact_scalar_fallback_required_for_accelerator_route");
  }
  if (request.cancellation_requested) {
    return Finish(request, ResourceGovernanceAction::kCancel, false, false,
                  "SB_RESOURCE_GOVERNANCE.CANCELLED",
                  "quota_admission_cancelled_before_start");
  }

  const auto exceeded =
      FirstExceeded(request.requested, request.descriptor.limits);
  if (!exceeded.empty()) {
    const auto action = request.descriptor.over_limit_action;
    if (action == ResourceGovernanceAction::kExactScalarFallback &&
        !request.exact_scalar_fallback_available) {
      return Refuse(request,
                    "SB_RESOURCE_GOVERNANCE.SCALAR_FALLBACK_REQUIRED",
                    "exact_scalar_fallback_required_for_accelerator_route",
                    exceeded);
    }
    if (action == ResourceGovernanceAction::kAdmit) {
      return Refuse(request,
                    "SB_RESOURCE_GOVERNANCE.OVER_LIMIT_ADMIT_REFUSED",
                    "over_limit_action_cannot_admit", exceeded);
    }
    return Finish(request, action, false,
                  action == ResourceGovernanceAction::kFailClosed,
                  action == ResourceGovernanceAction::kSlowdownDegrade
                      ? "SB_RESOURCE_GOVERNANCE.SLOWDOWN_DEGRADE"
                      : action == ResourceGovernanceAction::kExactScalarFallback
                            ? "SB_RESOURCE_GOVERNANCE.EXACT_SCALAR_FALLBACK"
                            : action == ResourceGovernanceAction::kCancel
                                  ? "SB_RESOURCE_GOVERNANCE.CANCELLED"
                                  : "SB_RESOURCE_GOVERNANCE.QUOTA_REFUSED",
                  "quota_exceeded", exceeded);
  }

  return Finish(request, ResourceGovernanceAction::kAdmit, true, false,
                "SB_RESOURCE_GOVERNANCE.ADMITTED",
                "quota_reservation_admitted");
}

std::string SerializeResourceGovernanceEvidence(
    const ResourceGovernanceAdmissionResult& result) {
  std::ostringstream out;
  for (const auto& item : result.evidence) {
    out << item << '\n';
  }
  return out.str();
}

ResourceGovernanceReservationLedger::ResourceGovernanceReservationLedger(
    std::string ledger_id)
    : ledger_id_(std::move(ledger_id)) {}

ResourceGovernanceReservationSnapshot
ResourceGovernanceReservationLedger::SnapshotLocked() const {
  ResourceGovernanceReservationSnapshot snapshot;
  snapshot.ledger_id = ledger_id_;
  snapshot.active_reservation_count = active_.size();
  snapshot.created_reservation_count = next_sequence_;
  snapshot.released_reservation_count = released_count_;
  snapshot.active = active_usage_;
  return snapshot;
}

ResourceGovernanceReservationSnapshot
ResourceGovernanceReservationLedger::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotLocked();
}

ResourceGovernanceReservationAcquireResult
ResourceGovernanceReservationLedger::Acquire(
    ResourceGovernanceReservationAcquireRequest request) {
  ResourceGovernanceReservationAcquireResult result;
  result.admission = AdmitResourceGovernance(request.admission);
  Add(&result.evidence, "MMCH_RESOURCE_RESERVATION_LIFECYCLE");
  Add(&result.evidence, "resource_reservation.ledger_id=" + ledger_id_);
  Add(&result.evidence,
      "resource_reservation.owner_scope=" +
          (request.owner_scope.empty() ? std::string("none")
                                       : request.owner_scope));
  Add(&result.evidence,
      "resource_reservation.operation_id=" +
          request.admission.operation_id);
  Add(&result.evidence,
      "resource_reservation.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");
  for (const auto& item : result.admission.evidence) {
    Add(&result.evidence, "resource_reservation.admission." + item);
  }

  if (!result.admission.ok) {
    result.ok = false;
    result.fail_closed = result.admission.fail_closed;
    result.diagnostic_code = result.admission.diagnostic_code;
    result.diagnostic_detail = result.admission.diagnostic_detail;
    result.exceeded_quota = result.admission.exceeded_quota;
    result.status =
        LocalAgentError(result.diagnostic_code, result.diagnostic_detail);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result.snapshot = SnapshotLocked();
    }
    Add(&result.evidence, "resource_reservation.created=false");
    Add(&result.evidence,
        "resource_reservation.diagnostic_code=" + result.diagnostic_code);
    return result;
  }

  if (request.owner_scope.empty()) {
    result.ok = false;
    result.fail_closed = true;
    result.diagnostic_code =
        "SB_RESOURCE_GOVERNANCE.RESERVATION_OWNER_REQUIRED";
    result.diagnostic_detail = "reservation_owner_scope_required";
    result.status = LocalAgentError(result.diagnostic_code, result.diagnostic_detail);
    std::lock_guard<std::mutex> lock(mutex_);
    result.snapshot = SnapshotLocked();
    Add(&result.evidence, "resource_reservation.created=false");
    Add(&result.evidence,
        "resource_reservation.diagnostic_code=" + result.diagnostic_code);
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const std::string exceeded =
      FirstExceededWithActive(active_usage_,
                              request.admission.requested,
                              request.admission.descriptor.limits);
  if (!exceeded.empty()) {
    result.ok = false;
    result.fail_closed = true;
    result.diagnostic_code =
        "SB_RESOURCE_GOVERNANCE.RESERVATION_LEDGER_LIMIT_EXCEEDED";
    result.diagnostic_detail = "active_reservations_exceed_quota_limit";
    result.exceeded_quota = exceeded;
    result.status = LocalAgentError(result.diagnostic_code, result.diagnostic_detail);
    result.snapshot = SnapshotLocked();
    Add(&result.evidence, "resource_reservation.created=false");
    Add(&result.evidence,
        "resource_reservation.diagnostic_code=" + result.diagnostic_code);
    Add(&result.evidence, "resource_reservation.exceeded_quota=" + exceeded);
    AddVectorEvidence(&result.evidence,
                      "resource_reservation.active",
                      active_usage_);
    return result;
  }

  ResourceGovernanceReservationToken token;
  token.created_sequence = ++next_sequence_;
  token.token_id = ledger_id_ + ":" + request.admission.operation_id + ":" +
                   std::to_string(token.created_sequence);
  token.operation_id = request.admission.operation_id;
  token.descriptor_id = request.admission.descriptor.descriptor_id;
  token.family = request.admission.descriptor.family;
  token.reserved = request.admission.requested;
  token.owner_scope = std::move(request.owner_scope);
  token.lease_deadline_tick = request.lease_deadline_tick;
  AddQuota(&active_usage_, token.reserved);
  active_[token.token_id] = ActiveReservation{token};

  result.reservation = token;
  result.snapshot = SnapshotLocked();
  result.ok = true;
  result.reservation_created = true;
  result.status = LocalAgentOk();
  result.diagnostic_code = "SB_RESOURCE_GOVERNANCE.RESERVATION_CREATED";
  result.diagnostic_detail = "quota_reservation_token_created";
  Add(&result.evidence, "resource_reservation.created=true");
  Add(&result.evidence, "resource_reservation.token_id=" + token.token_id);
  Add(&result.evidence,
      "resource_reservation.diagnostic_code=" + result.diagnostic_code);
  AddVectorEvidence(&result.evidence,
                    "resource_reservation.active",
                    active_usage_);
  return result;
}

ResourceGovernanceReservationReleaseResult
ResourceGovernanceReservationLedger::Release(
    const std::string& token_id,
    ResourceGovernanceReservationReleaseReason reason) {
  ResourceGovernanceReservationReleaseResult result;
  result.reason = reason;
  Add(&result.evidence, "MMCH_RESOURCE_RESERVATION_LIFECYCLE");
  Add(&result.evidence, "resource_reservation.ledger_id=" + ledger_id_);
  Add(&result.evidence, "resource_reservation.token_id=" + token_id);
  Add(&result.evidence,
      "resource_reservation.release_reason=" +
          std::string(ResourceGovernanceReservationReleaseReasonName(reason)));
  Add(&result.evidence,
      "resource_reservation.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_.find(token_id);
  if (it == active_.end()) {
    result.ok = false;
    result.not_found = true;
    result.status = LocalAgentError("SB_RESOURCE_GOVERNANCE.RESERVATION_NOT_FOUND",
                                    "reservation_token_not_found");
    result.diagnostic_code = result.status.diagnostic_code;
    result.diagnostic_detail = result.status.detail;
    result.snapshot = SnapshotLocked();
    Add(&result.evidence, "resource_reservation.released=false");
    Add(&result.evidence,
        "resource_reservation.diagnostic_code=" + result.diagnostic_code);
    return result;
  }

  result.reservation = it->second.token;
  SubtractQuota(&active_usage_, it->second.token.reserved);
  active_.erase(it);
  ++released_count_;
  result.snapshot = SnapshotLocked();
  result.ok = true;
  result.released = true;
  result.status = LocalAgentOk();
  result.diagnostic_code = "SB_RESOURCE_GOVERNANCE.RESERVATION_RELEASED";
  result.diagnostic_detail = "quota_reservation_token_released";
  Add(&result.evidence, "resource_reservation.released=true");
  Add(&result.evidence,
      "resource_reservation.diagnostic_code=" + result.diagnostic_code);
  AddVectorEvidence(&result.evidence,
                    "resource_reservation.active",
                    active_usage_);
  return result;
}

ResourceGovernanceReservationCleanupResult
ResourceGovernanceReservationLedger::ReleaseOwnerReservations(
    const std::string& owner_scope,
    ResourceGovernanceReservationReleaseReason reason) {
  ResourceGovernanceReservationCleanupResult result;
  result.owner_scope = owner_scope;
  result.reason = reason;
  Add(&result.evidence, "MMCH_RESOURCE_RESERVATION_LIFECYCLE");
  Add(&result.evidence, "resource_reservation.ledger_id=" + ledger_id_);
  Add(&result.evidence, "resource_reservation.owner_scope=" + owner_scope);
  Add(&result.evidence,
      "resource_reservation.cleanup_reason=" +
          std::string(ResourceGovernanceReservationReleaseReasonName(reason)));
  Add(&result.evidence,
      "resource_reservation.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = active_.begin(); it != active_.end();) {
    if (it->second.token.owner_scope != owner_scope) {
      ++it;
      continue;
    }
    SubtractQuota(&active_usage_, it->second.token.reserved);
    it = active_.erase(it);
    ++released_count_;
    ++result.released_count;
  }
  result.snapshot = SnapshotLocked();
  result.ok = true;
  result.status = LocalAgentOk();
  result.diagnostic_code = "SB_RESOURCE_GOVERNANCE.RESERVATION_CLEANUP";
  Add(&result.evidence,
      "resource_reservation.released_count=" +
          std::to_string(result.released_count));
  Add(&result.evidence,
      "resource_reservation.diagnostic_code=" + result.diagnostic_code);
  AddVectorEvidence(&result.evidence,
                    "resource_reservation.active",
                    active_usage_);
  return result;
}

ResourceGovernanceReservationCleanupResult
ResourceGovernanceReservationLedger::ExpireReservations(std::uint64_t now_tick) {
  ResourceGovernanceReservationCleanupResult result;
  result.reason = ResourceGovernanceReservationReleaseReason::kTimeout;
  Add(&result.evidence, "MMCH_RESOURCE_RESERVATION_LIFECYCLE");
  Add(&result.evidence, "resource_reservation.ledger_id=" + ledger_id_);
  Add(&result.evidence,
      "resource_reservation.cleanup_reason=timeout");
  Add(&result.evidence,
      "resource_reservation.now_tick=" + std::to_string(now_tick));
  Add(&result.evidence,
      "resource_reservation.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = active_.begin(); it != active_.end();) {
    const auto deadline = it->second.token.lease_deadline_tick;
    if (deadline == 0 || deadline > now_tick) {
      ++it;
      continue;
    }
    SubtractQuota(&active_usage_, it->second.token.reserved);
    it = active_.erase(it);
    ++released_count_;
    ++result.released_count;
  }
  result.snapshot = SnapshotLocked();
  result.ok = true;
  result.status = LocalAgentOk();
  result.diagnostic_code = "SB_RESOURCE_GOVERNANCE.RESERVATION_TIMEOUT_CLEANUP";
  Add(&result.evidence,
      "resource_reservation.released_count=" +
          std::to_string(result.released_count));
  Add(&result.evidence,
      "resource_reservation.diagnostic_code=" + result.diagnostic_code);
  AddVectorEvidence(&result.evidence,
                    "resource_reservation.active",
                    active_usage_);
  return result;
}

HierarchicalMemoryBudgetLedger::HierarchicalMemoryBudgetLedger(
    std::string ledger_id)
    : ledger_id_(std::move(ledger_id)) {}

AgentRuntimeStatus HierarchicalMemoryBudgetLedger::RegisterScope(
    HierarchicalMemoryBudgetScope scope) {
  if (scope.scope_id.empty() ||
      scope.kind == HierarchicalMemoryBudgetScopeKind::kUnknown ||
      scope.limit_bytes == 0 ||
      !scope.active) {
    return LocalAgentError("SB_RESOURCE_GOVERNANCE.HIERARCHICAL_SCOPE_INVALID",
                           "scope_id_kind_limit_and_active_state_required");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!scope.parent_scope_id.empty() &&
      scopes_.find(scope.parent_scope_id) == scopes_.end()) {
    return LocalAgentError("SB_RESOURCE_GOVERNANCE.HIERARCHICAL_PARENT_MISSING",
                           scope.parent_scope_id);
  }
  if (scopes_.find(scope.scope_id) != scopes_.end()) {
    return LocalAgentError("SB_RESOURCE_GOVERNANCE.HIERARCHICAL_SCOPE_DUPLICATE",
                           scope.scope_id);
  }
  const std::string scope_id = scope.scope_id;
  scopes_[scope_id] = ScopeState{std::move(scope), 0, 0, 0};
  return LocalAgentOk();
}

std::vector<std::string> HierarchicalMemoryBudgetLedger::ScopeChainLocked(
    const std::string& leaf_scope_id,
    std::string* diagnostic) const {
  std::vector<std::string> chain;
  std::string current = leaf_scope_id;
  for (std::uint32_t depth = 0; depth < 64; ++depth) {
    auto it = scopes_.find(current);
    if (it == scopes_.end()) {
      if (diagnostic) {
        *diagnostic = "scope_not_registered:" + current;
      }
      return {};
    }
    chain.push_back(current);
    if (it->second.scope.parent_scope_id.empty()) {
      return chain;
    }
    if (std::find(chain.begin(), chain.end(),
                  it->second.scope.parent_scope_id) != chain.end()) {
      if (diagnostic) {
        *diagnostic = "scope_cycle_detected:" + it->second.scope.parent_scope_id;
      }
      return {};
    }
    current = it->second.scope.parent_scope_id;
  }
  if (diagnostic) {
    *diagnostic = "scope_chain_too_deep";
  }
  return {};
}

std::vector<HierarchicalMemoryBudgetScopeSnapshot>
HierarchicalMemoryBudgetLedger::SnapshotLocked() const {
  std::vector<HierarchicalMemoryBudgetScopeSnapshot> snapshots;
  snapshots.reserve(scopes_.size());
  for (const auto& entry : scopes_) {
    HierarchicalMemoryBudgetScopeSnapshot snapshot;
    snapshot.scope_id = entry.first;
    snapshot.parent_scope_id = entry.second.scope.parent_scope_id;
    snapshot.kind = entry.second.scope.kind;
    snapshot.limit_bytes = entry.second.scope.limit_bytes;
    snapshot.current_bytes = entry.second.current_bytes;
    snapshot.peak_bytes = entry.second.peak_bytes;
    snapshot.active_reservation_count =
        entry.second.active_reservation_count;
    snapshots.push_back(std::move(snapshot));
  }
  return snapshots;
}

std::vector<HierarchicalMemoryBudgetScopeSnapshot>
HierarchicalMemoryBudgetLedger::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotLocked();
}

HierarchicalMemoryBudgetReserveResult HierarchicalMemoryBudgetLedger::Reserve(
    HierarchicalMemoryBudgetReserveRequest request) {
  HierarchicalMemoryBudgetReserveResult result;
  Add(&result.evidence, "MMCH_HIERARCHICAL_MEMORY_BUDGETS");
  Add(&result.evidence, "hierarchical_memory.ledger_id=" + ledger_id_);
  Add(&result.evidence,
      "hierarchical_memory.operation_id=" + request.operation_id);
  Add(&result.evidence,
      "hierarchical_memory.owner_scope=" +
          (request.owner_scope.empty() ? std::string("none")
                                       : request.owner_scope));
  Add(&result.evidence,
      "hierarchical_memory.leaf_scope_id=" + request.leaf_scope_id);
  Add(&result.evidence,
      "hierarchical_memory.requested_bytes=" +
          std::to_string(request.bytes));
  Add(&result.evidence,
      "hierarchical_memory.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");

  if (request.operation_id.empty() || request.owner_scope.empty() ||
      request.leaf_scope_id.empty() || request.bytes == 0) {
    result.fail_closed = true;
    result.diagnostic_code =
        "SB_RESOURCE_GOVERNANCE.HIERARCHICAL_REQUEST_INVALID";
    result.diagnostic_detail =
        "operation_owner_leaf_scope_and_positive_bytes_required";
    result.status = LocalAgentError(result.diagnostic_code,
                                    result.diagnostic_detail);
    std::lock_guard<std::mutex> lock(mutex_);
    result.snapshots = SnapshotLocked();
    Add(&result.evidence,
        "hierarchical_memory.diagnostic_code=" + result.diagnostic_code);
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  std::string chain_diagnostic;
  const auto chain = ScopeChainLocked(request.leaf_scope_id, &chain_diagnostic);
  if (chain.empty()) {
    result.fail_closed = true;
    result.diagnostic_code =
        "SB_RESOURCE_GOVERNANCE.HIERARCHICAL_SCOPE_CHAIN_INVALID";
    result.diagnostic_detail = chain_diagnostic;
    result.status = LocalAgentError(result.diagnostic_code,
                                    result.diagnostic_detail);
    result.snapshots = SnapshotLocked();
    Add(&result.evidence,
        "hierarchical_memory.diagnostic_code=" + result.diagnostic_code);
    Add(&result.evidence,
        "hierarchical_memory.chain_diagnostic=" + chain_diagnostic);
    return result;
  }

  for (const auto& scope_id : chain) {
    const auto& state = scopes_.at(scope_id);
    if (request.bytes > state.scope.limit_bytes ||
        state.current_bytes > state.scope.limit_bytes - request.bytes) {
      result.fail_closed = true;
      result.refused_scope_id = scope_id;
      result.diagnostic_code =
          "SB_RESOURCE_GOVERNANCE.HIERARCHICAL_BUDGET_EXCEEDED";
      result.diagnostic_detail =
          std::string(HierarchicalMemoryBudgetScopeKindName(state.scope.kind)) +
          "_scope_budget_exceeded";
      result.status = LocalAgentError(result.diagnostic_code,
                                      result.diagnostic_detail);
      result.snapshots = SnapshotLocked();
      Add(&result.evidence,
          "hierarchical_memory.diagnostic_code=" + result.diagnostic_code);
      Add(&result.evidence, "hierarchical_memory.refused_scope_id=" + scope_id);
      Add(&result.evidence,
          "hierarchical_memory.refused_scope_kind=" +
              std::string(HierarchicalMemoryBudgetScopeKindName(
                  state.scope.kind)));
      return result;
    }
  }

  HierarchicalMemoryBudgetReservationToken token;
  token.created_sequence = ++next_sequence_;
  token.token_id = ledger_id_ + ":" + request.operation_id + ":" +
                   std::to_string(token.created_sequence);
  token.operation_id = std::move(request.operation_id);
  token.owner_scope = std::move(request.owner_scope);
  token.leaf_scope_id = std::move(request.leaf_scope_id);
  token.bytes = request.bytes;
  token.debited_scope_chain = chain;

  for (const auto& scope_id : chain) {
    auto& state = scopes_[scope_id];
    state.current_bytes += token.bytes;
    state.peak_bytes = std::max(state.peak_bytes, state.current_bytes);
    ++state.active_reservation_count;
  }
  active_[token.token_id] = ActiveHierarchicalReservation{token};

  result.reservation = token;
  result.snapshots = SnapshotLocked();
  result.ok = true;
  result.reservation_created = true;
  result.status = LocalAgentOk();
  result.diagnostic_code =
      "SB_RESOURCE_GOVERNANCE.HIERARCHICAL_RESERVATION_CREATED";
  result.diagnostic_detail = "hierarchical_memory_budget_reserved";
  Add(&result.evidence,
      "hierarchical_memory.diagnostic_code=" + result.diagnostic_code);
  Add(&result.evidence, "hierarchical_memory.token_id=" + token.token_id);
  Add(&result.evidence, "hierarchical_memory.reservation_created=true");
  for (const auto& scope_id : chain) {
    Add(&result.evidence, "hierarchical_memory.debited_scope=" + scope_id);
  }
  return result;
}

HierarchicalMemoryBudgetReleaseResult HierarchicalMemoryBudgetLedger::Release(
    const std::string& token_id) {
  HierarchicalMemoryBudgetReleaseResult result;
  Add(&result.evidence, "MMCH_HIERARCHICAL_MEMORY_BUDGETS");
  Add(&result.evidence, "hierarchical_memory.ledger_id=" + ledger_id_);
  Add(&result.evidence, "hierarchical_memory.token_id=" + token_id);
  Add(&result.evidence,
      "hierarchical_memory.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_.find(token_id);
  if (it == active_.end()) {
    result.not_found = true;
    result.diagnostic_code =
        "SB_RESOURCE_GOVERNANCE.HIERARCHICAL_RESERVATION_NOT_FOUND";
    result.diagnostic_detail = "hierarchical_memory_budget_token_not_found";
    result.status = LocalAgentError(result.diagnostic_code,
                                    result.diagnostic_detail);
    result.snapshots = SnapshotLocked();
    Add(&result.evidence,
        "hierarchical_memory.diagnostic_code=" + result.diagnostic_code);
    Add(&result.evidence, "hierarchical_memory.released=false");
    return result;
  }

  result.reservation = it->second.token;
  for (const auto& scope_id : it->second.token.debited_scope_chain) {
    auto scope = scopes_.find(scope_id);
    if (scope == scopes_.end()) {
      continue;
    }
    scope->second.current_bytes -= it->second.token.bytes;
    if (scope->second.active_reservation_count > 0) {
      --scope->second.active_reservation_count;
    }
  }
  active_.erase(it);
  result.snapshots = SnapshotLocked();
  result.ok = true;
  result.released = true;
  result.status = LocalAgentOk();
  result.diagnostic_code =
      "SB_RESOURCE_GOVERNANCE.HIERARCHICAL_RESERVATION_RELEASED";
  result.diagnostic_detail = "hierarchical_memory_budget_released";
  Add(&result.evidence,
      "hierarchical_memory.diagnostic_code=" + result.diagnostic_code);
  Add(&result.evidence, "hierarchical_memory.released=true");
  return result;
}

HierarchicalMemoryBudgetReleaseResult
HierarchicalMemoryBudgetLedger::ReleaseOwnerReservations(
    const std::string& owner_scope) {
  HierarchicalMemoryBudgetReleaseResult combined;
  Add(&combined.evidence, "MMCH_HIERARCHICAL_MEMORY_BUDGETS");
  Add(&combined.evidence, "hierarchical_memory.ledger_id=" + ledger_id_);
  Add(&combined.evidence, "hierarchical_memory.owner_scope=" + owner_scope);
  Add(&combined.evidence,
      "hierarchical_memory.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");

  std::vector<std::string> tokens;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : active_) {
      if (entry.second.token.owner_scope == owner_scope) {
        tokens.push_back(entry.first);
      }
    }
  }

  std::uint64_t released_count = 0;
  for (const auto& token : tokens) {
    auto released = Release(token);
    if (released.ok) {
      ++released_count;
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    combined.snapshots = SnapshotLocked();
  }
  combined.ok = true;
  combined.released = released_count > 0;
  combined.status = LocalAgentOk();
  combined.diagnostic_code =
      "SB_RESOURCE_GOVERNANCE.HIERARCHICAL_OWNER_RELEASED";
  combined.diagnostic_detail = "hierarchical_memory_owner_reservations_released";
  Add(&combined.evidence,
      "hierarchical_memory.diagnostic_code=" + combined.diagnostic_code);
  Add(&combined.evidence,
      "hierarchical_memory.released_count=" + std::to_string(released_count));
  return combined;
}

}  // namespace scratchbird::core::agents
