// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "background_memory_reclamation.hpp"

#include "metric_producer.hpp"
#include "reservation_backed_memory_resource.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status ReclamationOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ReclamationErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
}

DiagnosticRecord ReclamationDiagnostic(Status status,
                                       std::string code,
                                       std::string message,
                                       std::vector<DiagnosticArgument> arguments = {}) {
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(code),
                        std::move(message),
                        std::move(arguments),
                        {},
                        "core.memory.background_reclamation",
                        "background_reclamation_requires_route_evidence_and_engine_mga_safe_authority");
}

void AppendBaseEvidence(BackgroundMemoryReclamationResult* result,
                        const BackgroundMemoryReclamationRequest& request) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back("MMCH_BACKGROUND_MEMORY_RECLAMATION");
  result->evidence.push_back("background_reclamation.route_label=" + request.route_label);
  result->evidence.push_back("background_reclamation.operation_id=" + request.operation_id);
  result->evidence.push_back(
      "background_reclamation.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");
  result->evidence.push_back("background_reclamation.engine_mga_authoritative=" +
                             std::string(request.engine_mga_authoritative ? "true" : "false"));
}

BackgroundMemoryReclamationResult ErrorResult(
    const BackgroundMemoryReclamationRequest& request,
    std::string code,
    std::string message,
    std::vector<DiagnosticArgument> arguments = {}) {
  BackgroundMemoryReclamationResult result;
  result.status = ReclamationErrorStatus();
  result.fail_closed = true;
  result.diagnostic =
      ReclamationDiagnostic(result.status, std::move(code), std::move(message), std::move(arguments));
  AppendBaseEvidence(&result, request);
  result.evidence.push_back("background_reclamation.fail_closed=true");
  return result;
}

bool UnsafeAuthority(const BackgroundMemoryReclamationWorkItem& item) {
  return item.parser_or_donor_authority ||
         item.client_authority ||
         item.provider_authority ||
         item.wal_authority;
}

bool AddWouldExceed(u64 current, u64 add, u64 limit) {
  if (limit == 0) {
    return false;
  }
  return current > limit || add > limit - current;
}

void PublishReclamationMetrics(const BackgroundMemoryReclamationRequest& request,
                               const BackgroundMemoryReclamationCounters& counters) {
  auto labels = scratchbird::core::metrics::Labels(
      {{"component", "core.memory"},
       {"operation", "background_reclamation"},
       {"route_class", request.route_label.empty() ? "unknown" : request.route_label}});
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_memory_background_reclamation_runs_total",
      labels,
      1.0,
      "core_memory");
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_memory_background_reclaimed_bytes_total",
      labels,
      static_cast<double>(counters.reclaimed_bytes),
      "core_memory");
  (void)scratchbird::core::metrics::SetGauge(
      "sb_memory_background_reclamation_retained_items",
      labels,
      static_cast<double>(counters.retained_count),
      "core_memory");
}

}  // namespace

const char* BackgroundMemoryReclamationWorkKindName(
    BackgroundMemoryReclamationWorkKind kind) {
  switch (kind) {
    case BackgroundMemoryReclamationWorkKind::idle_arena: return "idle_arena";
    case BackgroundMemoryReclamationWorkKind::clean_page_cache_frame: return "clean_page_cache_frame";
    case BackgroundMemoryReclamationWorkKind::spill_record: return "spill_record";
    case BackgroundMemoryReclamationWorkKind::old_diagnostic: return "old_diagnostic";
    case BackgroundMemoryReclamationWorkKind::completed_query_state: return "completed_query_state";
  }
  return "unknown";
}

BackgroundMemoryReclamationResult RunBackgroundMemoryReclamation(
    const BackgroundMemoryReclamationPolicy& policy,
    const BackgroundMemoryReclamationRequest& request) {
  if (policy.require_route_label && request.route_label.empty()) {
    return ErrorResult(
        request,
        "background_reclamation_missing_route_label",
        "memory.background_reclamation.missing_route_label",
        {{"operation_id", request.operation_id}});
  }
  if (policy.require_engine_mga_authority && !request.engine_mga_authoritative) {
    return ErrorResult(
        request,
        "background_reclamation_missing_engine_mga_authority",
        "memory.background_reclamation.missing_engine_mga_authority",
        {{"route_label", request.route_label}});
  }

  BackgroundMemoryReclamationResult result;
  result.status = ReclamationOkStatus();
  result.diagnostic = ReclamationDiagnostic(
      result.status,
      "background_reclamation_completed",
      "memory.background_reclamation.completed",
      {{"route_label", request.route_label},
       {"operation_id", request.operation_id}});
  AppendBaseEvidence(&result, request);

  u64 processed_items = 0;
  for (const auto& item : request.work_items) {
    if (policy.max_items_per_run != 0 && processed_items >= policy.max_items_per_run) {
      ++result.counters.retained_count;
      result.evidence.push_back("background_reclamation.retained_reason=item_budget");
      continue;
    }
    if (AddWouldExceed(result.counters.reclaimed_bytes,
                       item.estimated_reclaim_bytes,
                       policy.max_reclaim_bytes_per_run)) {
      ++result.counters.retained_count;
      result.evidence.push_back("background_reclamation.retained_reason=byte_budget");
      continue;
    }
    ++processed_items;
    ++result.counters.scanned_count;

    result.evidence.push_back(std::string("background_reclamation.scanned_kind=") +
                              BackgroundMemoryReclamationWorkKindName(item.kind));
    result.evidence.push_back("background_reclamation.scanned_label=" + item.label);

    if (UnsafeAuthority(item)) {
      result.fail_closed = true;
      ++result.counters.failed_count;
      result.status = ReclamationErrorStatus();
      result.diagnostic = ReclamationDiagnostic(
          result.status,
          "background_reclamation_unsafe_authority",
          "memory.background_reclamation.unsafe_authority",
          {{"label", item.label}});
      result.evidence.push_back("background_reclamation.fail_closed=true");
      break;
    }
    if (!item.eligible) {
      ++result.counters.retained_count;
      result.evidence.push_back("background_reclamation.retained_reason=ineligible");
      continue;
    }
    if (policy.cancellation_requested && !item.cancellation_safe) {
      result.cancelled = true;
      ++result.counters.cancelled_count;
      ++result.counters.retained_count;
      result.evidence.push_back("background_reclamation.retained_reason=cancellation_not_safe");
      continue;
    }
    if (policy.cancellation_requested) {
      result.cancelled = true;
    }
    if (!item.reclaim) {
      ++result.counters.retained_count;
      result.evidence.push_back("background_reclamation.retained_reason=no_reclaim_callback");
      continue;
    }

    auto status = item.reclaim(&result.evidence);
    if (!status.ok()) {
      ++result.counters.failed_count;
      result.status = status;
      result.diagnostic = ReclamationDiagnostic(
          status,
          "background_reclamation_callback_failed",
          "memory.background_reclamation.callback_failed",
          {{"label", item.label}});
      continue;
    }
    ++result.counters.reclaimed_count;
    result.counters.reclaimed_bytes += item.estimated_reclaim_bytes;
    result.evidence.push_back("background_reclamation.reclaimed_label=" + item.label);
  }

  result.evidence.push_back("background_reclamation.scanned_count=" +
                            std::to_string(result.counters.scanned_count));
  result.evidence.push_back("background_reclamation.reclaimed_count=" +
                            std::to_string(result.counters.reclaimed_count));
  result.evidence.push_back("background_reclamation.retained_count=" +
                            std::to_string(result.counters.retained_count));
  result.evidence.push_back("background_reclamation.failed_count=" +
                            std::to_string(result.counters.failed_count));
  result.evidence.push_back("background_reclamation.reclaimed_bytes=" +
                            std::to_string(result.counters.reclaimed_bytes));

  PublishReclamationMetrics(request, result.counters);
  result.evidence.push_back("background_reclamation.metrics_published=true");
  return result;
}

BackgroundMemoryReclamationResult RunBackgroundMemoryReclamationWithReservedResource(
    const BackgroundMemoryReclamationPolicy& policy,
    const BackgroundMemoryReclamationRequest& request,
    ReservationBackedMemoryResource* resource,
    u64 scratch_bytes) {
  if (resource == nullptr || !resource->active()) {
    return ErrorResult(
        request,
        "background_reclamation_reserved_resource_required",
        "memory.background_reclamation.reserved_resource_required",
        {{"operation_id", request.operation_id}});
  }
  if (scratch_bytes == 0) {
    return ErrorResult(
        request,
        "background_reclamation_reserved_scratch_required",
        "memory.background_reclamation.reserved_scratch_required",
        {{"operation_id", request.operation_id}});
  }

  ReservationBackedMemoryAllocationRequest allocation;
  allocation.bytes = scratch_bytes;
  allocation.alignment = alignof(std::max_align_t);
  allocation.purpose = "background.maintenance_scratch";
  auto scratch = resource->Allocate(std::move(allocation));
  if (!scratch.ok()) {
    auto result = ErrorResult(
        request,
        "background_reclamation_reserved_allocation_refused",
        "memory.background_reclamation.reserved_allocation_refused",
        {{"operation_id", request.operation_id}});
    result.diagnostic = scratch.diagnostic;
    return result;
  }

  auto result = RunBackgroundMemoryReclamation(policy, request);
  result.evidence.push_back("background_reclamation.reserved_resource_passed=true");
  result.evidence.push_back("background_reclamation.after_reservation=true");
  result.evidence.push_back("background_reclamation.reserved_scratch_bytes=" +
                            std::to_string(scratch.bytes));
  return result;
}

}  // namespace scratchbird::core::memory
