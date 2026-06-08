// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-012: reservation-backed memory resource for hot temporary arenas.
#include "reservation_backed_memory_resource.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kEvidenceAnchor =
    "CEIC-012_QUERY_OPERATOR_PLANNER_PARSER_ARENAS";
constexpr const char* kAuthorityScope =
    "reservation_backed_memory.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_donor_benchmark_cluster_optimizer_index_or_agent_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request) {
  return {code, Severity::error, Subsystem::memory};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool Blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool UnsafeAuthority(const ReservationBackedMemoryResourceRequest& request,
                     std::string* reason) {
  const auto& authority = request.authority;
  if (!authority.engine_mga_authoritative) {
    *reason = "engine_mga_authority_required";
    return true;
  }
  if (request.production_like && !authority.transaction_inventory_authoritative) {
    *reason = "transaction_inventory_authority_required";
    return true;
  }
  if (!authority.security_or_policy_checked) {
    *reason = "security_or_policy_check_required";
    return true;
  }
  if (authority.parser_or_donor_finality_authority ||
      authority.memory_visibility_or_finality_authority ||
      authority.memory_recovery_authority ||
      authority.memory_authorization_authority ||
      authority.benchmark_authority ||
      authority.cluster_authority ||
      authority.debug_or_relaxed_path ||
      authority.optimizer_plan_authority ||
      authority.index_finality_authority ||
      authority.agent_action_authority) {
    *reason = "unsafe_authority_claim_refused";
    return true;
  }
  return false;
}

MemoryCategory DefaultCategoryFor(ReservationBackedMemoryConsumerKind kind,
                                  MemoryCategory requested) {
  if (requested != MemoryCategory::unknown) {
    return requested;
  }
  switch (kind) {
    case ReservationBackedMemoryConsumerKind::parser_handoff:
    case ReservationBackedMemoryConsumerKind::sblr_handoff:
      return MemoryCategory::parser_handoff_reserved;
    case ReservationBackedMemoryConsumerKind::udr_invocation:
      return MemoryCategory::udr_reserved;
    case ReservationBackedMemoryConsumerKind::background_maintenance:
      return MemoryCategory::cleanup;
    case ReservationBackedMemoryConsumerKind::executor_operator:
    case ReservationBackedMemoryConsumerKind::planner_temporary:
    case ReservationBackedMemoryConsumerKind::optimizer_temporary:
    case ReservationBackedMemoryConsumerKind::result_frame:
      return MemoryCategory::executor_query_reserved;
  }
  return MemoryCategory::executor_query_reserved;
}

Subsystem SubsystemFor(ReservationBackedMemoryConsumerKind kind) {
  switch (kind) {
    case ReservationBackedMemoryConsumerKind::parser_handoff:
    case ReservationBackedMemoryConsumerKind::sblr_handoff:
      return Subsystem::parser;
    case ReservationBackedMemoryConsumerKind::executor_operator:
    case ReservationBackedMemoryConsumerKind::planner_temporary:
    case ReservationBackedMemoryConsumerKind::optimizer_temporary:
    case ReservationBackedMemoryConsumerKind::udr_invocation:
    case ReservationBackedMemoryConsumerKind::result_frame:
      return Subsystem::engine;
    case ReservationBackedMemoryConsumerKind::background_maintenance:
      return Subsystem::memory;
  }
  return Subsystem::memory;
}

void AppendBaseEvidence(std::vector<std::string>* evidence,
                        const ReservationBackedMemoryResourceRequest& request) {
  evidence->push_back(kEvidenceAnchor);
  evidence->push_back(kAuthorityScope);
  evidence->push_back("reservation_backed_memory.consumer=" +
                      std::string(ReservationBackedMemoryConsumerKindName(
                          request.consumer_kind)));
  evidence->push_back("reservation_backed_memory.route_label=" +
                      request.route_label);
  evidence->push_back("reservation_backed_memory.operation_id=" +
                      request.operation_id);
  evidence->push_back("reservation_backed_memory.owner_id=" +
                      request.owner_id);
  evidence->push_back("reservation_backed_memory.requested_bytes=" +
                      std::to_string(request.requested_bytes));
  evidence->push_back(
      "reservation_backed_memory.reservation_ledger=hierarchical_memory_budget_ledger");
  evidence->push_back(
      "reservation_backed_memory.allocate_after_reservation=true");
  evidence->push_back("reservation_backed_memory.engine_mga_authoritative=" +
                      BoolText(request.authority.engine_mga_authoritative));
  evidence->push_back(
      "reservation_backed_memory.transaction_inventory_authoritative=" +
      BoolText(request.authority.transaction_inventory_authoritative));
  evidence->push_back(
      "reservation_backed_memory.security_or_policy_checked=" +
      BoolText(request.authority.security_or_policy_checked));
}

DiagnosticRecord MakeResourceDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"authority_scope", kAuthorityScope});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.reservation_backed_resource",
                        "Reserve with HierarchicalMemoryBudgetLedger and pass the resulting memory resource to the hot path.");
}

ReservationBackedMemoryResourceAcquireResult RefuseAcquire(
    ReservationBackedMemoryResourceRequest request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code = StatusCode::memory_invalid_request) {
  ReservationBackedMemoryResourceAcquireResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.diagnostic = MakeResourceDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"reason", std::move(reason)},
       {"consumer", ReservationBackedMemoryConsumerKindName(request.consumer_kind)},
       {"route_label", request.route_label},
       {"operation_id", request.operation_id}});
  AppendBaseEvidence(&result.evidence, request);
  result.evidence.push_back("reservation_backed_memory.fail_closed=true");
  result.evidence.push_back("reservation_backed_memory.reservation_created=false");
  result.evidence.push_back("reservation_backed_memory.refused=" +
                            result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace

const char* ReservationBackedMemoryConsumerKindName(
    ReservationBackedMemoryConsumerKind kind) {
  switch (kind) {
    case ReservationBackedMemoryConsumerKind::executor_operator:
      return "executor_operator";
    case ReservationBackedMemoryConsumerKind::planner_temporary:
      return "planner_temporary";
    case ReservationBackedMemoryConsumerKind::optimizer_temporary:
      return "optimizer_temporary";
    case ReservationBackedMemoryConsumerKind::parser_handoff:
      return "parser_handoff";
    case ReservationBackedMemoryConsumerKind::sblr_handoff:
      return "sblr_handoff";
    case ReservationBackedMemoryConsumerKind::udr_invocation:
      return "udr_invocation";
    case ReservationBackedMemoryConsumerKind::background_maintenance:
      return "background_maintenance";
    case ReservationBackedMemoryConsumerKind::result_frame:
      return "result_frame";
  }
  return "unknown";
}

ReservationBackedMemoryResource::ReservationBackedMemoryResource(
    ReservationBackedMemoryResourceRequest request,
    HierarchicalMemoryReservationToken token)
    : request_(std::move(request)), token_(token) {}

ReservationBackedMemoryResource::~ReservationBackedMemoryResource() {
  (void)Release();
}

bool ReservationBackedMemoryResource::active() const {
  return !released_ && token_.valid();
}

const ReservationBackedMemoryResourceRequest&
ReservationBackedMemoryResource::request() const {
  return request_;
}

const HierarchicalMemoryReservationToken&
ReservationBackedMemoryResource::reservation_token() const {
  return token_;
}

MemoryTag ReservationBackedMemoryResource::TagForAllocation(
    const ReservationBackedMemoryAllocationRequest& allocation) const {
  MemoryTag tag;
  tag.subsystem = SubsystemFor(request_.consumer_kind);
  tag.purpose = allocation.purpose.empty() ? request_.purpose : allocation.purpose;
  tag.category = DefaultCategoryFor(request_.consumer_kind, request_.category);
  tag.lifetime = MemoryLifetime::arena;
  tag.owner = request_.owner_id;
  tag.context_id = request_.route_label;
  tag.statement_id = request_.route_label;
  tag.query_id = request_.route_label;
  tag.callsite = "core.memory.reservation_backed_resource";
  return tag;
}

AllocationResult ReservationBackedMemoryResource::Allocate(
    ReservationBackedMemoryAllocationRequest allocation) {
  AllocationResult result;
  if (!active()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.RELEASED",
        "memory.ceic_012.resource.released",
        {{"reason", "resource_released"},
         {"consumer", ReservationBackedMemoryConsumerKindName(request_.consumer_kind)}});
    return result;
  }
  if (request_.memory_manager == nullptr) {
    result.status = ErrorStatus();
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.MEMORY_MANAGER_REQUIRED",
        "memory.ceic_012.resource.memory_manager_required",
        {{"reason", "memory_manager_required"}});
    return result;
  }
  if (allocation.bytes == 0 ||
      allocation.bytes > static_cast<u64>(std::numeric_limits<usize>::max())) {
    result.status = ErrorStatus(allocation.bytes == 0
                                    ? StatusCode::memory_invalid_request
                                    : StatusCode::memory_limit_exceeded);
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.ALLOCATION_SIZE_INVALID",
        "memory.ceic_012.resource.allocation_size_invalid",
        {{"requested_bytes", std::to_string(allocation.bytes)}});
    return result;
  }
  if (allocation.bytes > request_.requested_bytes ||
      allocated_bytes_ > request_.requested_bytes - allocation.bytes) {
    result.status = ErrorStatus(StatusCode::memory_limit_exceeded);
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.RESERVATION_EXCEEDED",
        "memory.ceic_012.resource.reservation_exceeded",
        {{"reserved_bytes", std::to_string(request_.requested_bytes)},
         {"allocated_bytes", std::to_string(allocated_bytes_)},
         {"requested_bytes", std::to_string(allocation.bytes)}});
    return result;
  }

  MemoryTag tag = TagForAllocation(allocation);
  result = request_.memory_manager->Allocate(
      static_cast<usize>(allocation.bytes),
      allocation.alignment,
      tag);
  if (!result.ok()) {
    return result;
  }
  allocations_.push_back(
      AllocationRecord{result.pointer, result.bytes, result.alignment, tag});
  allocated_bytes_ += result.bytes;
  peak_allocated_bytes_ = std::max(peak_allocated_bytes_, allocated_bytes_);
  return result;
}

DeallocationResult ReservationBackedMemoryResource::Deallocate(
    void* pointer,
    usize bytes,
    usize alignment) {
  (void)alignment;
  DeallocationResult result;
  result.status = OkStatus();
  if (pointer == nullptr) {
    return result;
  }
  if (!active()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.DEALLOCATE_RELEASED",
        "memory.ceic_012.resource.deallocate_released",
        {{"reason", "resource_released"},
         {"consumer", ReservationBackedMemoryConsumerKindName(request_.consumer_kind)}});
    return result;
  }
  if (request_.memory_manager == nullptr) {
    result.status = ErrorStatus();
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.MEMORY_MANAGER_REQUIRED_ON_DEALLOCATE",
        "memory.ceic_012.resource.memory_manager_required_on_deallocate",
        {{"reason", "memory_manager_required_on_deallocate"}});
    return result;
  }

  auto it = std::find_if(
      allocations_.begin(),
      allocations_.end(),
      [pointer](const AllocationRecord& record) {
        return record.pointer == pointer;
      });
  if (it == allocations_.end()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.DEALLOCATE_UNKNOWN_POINTER",
        "memory.ceic_012.resource.deallocate_unknown_pointer",
        {{"reason", "resource_allocation_record_required"}});
    return result;
  }
  if (bytes != 0 && it->bytes != bytes) {
    result.status = ErrorStatus();
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.DEALLOCATE_SIZE_MISMATCH",
        "memory.ceic_012.resource.deallocate_size_mismatch",
        {{"reason", "allocation_size_mismatch"},
         {"expected_bytes", std::to_string(it->bytes)},
         {"requested_bytes", std::to_string(bytes)}});
    return result;
  }

  AllocationRecord record = *it;
  auto deallocated = request_.memory_manager->Deallocate(record.pointer, record.tag);
  if (!deallocated.ok()) {
    return deallocated;
  }
  allocations_.erase(it);
  allocated_bytes_ =
      allocated_bytes_ >= record.bytes ? allocated_bytes_ - record.bytes : 0;
  return deallocated;
}

ReservationBackedMemoryResourceReleaseResult
ReservationBackedMemoryResource::Release() {
  ReservationBackedMemoryResourceReleaseResult result;
  result.status = OkStatus();
  AppendBaseEvidence(&result.evidence, request_);

  if (released_) {
    result.released = true;
    result.snapshot = Snapshot();
    result.evidence.push_back("reservation_backed_memory.release.already_released=true");
    return result;
  }

  std::vector<AllocationRecord> retained_allocations;
  u64 retained_bytes = 0;
  for (auto it = allocations_.rbegin(); it != allocations_.rend(); ++it) {
    if (it->pointer == nullptr) {
      continue;
    }
    if (request_.memory_manager == nullptr) {
      retained_allocations.push_back(*it);
      retained_bytes += it->bytes;
      result.status = ErrorStatus();
      result.fail_closed = true;
      result.diagnostic = MakeResourceDiagnostic(
          result.status,
          "SB_CEIC_012_MEMORY_RESOURCE.MEMORY_MANAGER_REQUIRED_ON_RELEASE",
          "memory.ceic_012.resource.memory_manager_required_on_release",
          {{"reason", "memory_manager_required_on_release"}});
      continue;
    }
    auto deallocated = request_.memory_manager->Deallocate(it->pointer, it->tag);
    if (!deallocated.ok()) {
      retained_allocations.push_back(*it);
      retained_bytes += it->bytes;
      result.status = deallocated.status;
      result.fail_closed = true;
      result.diagnostic = deallocated.diagnostic;
    }
  }
  std::reverse(retained_allocations.begin(), retained_allocations.end());
  allocations_ = std::move(retained_allocations);
  allocated_bytes_ = retained_bytes;

  if (!result.fail_closed && request_.reservation_ledger != nullptr &&
      token_.valid()) {
    auto released = request_.reservation_ledger->Release(token_);
    if (!released.ok()) {
      result.status = released.status;
      result.fail_closed = true;
      result.diagnostic = released.diagnostic;
    }
  } else {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.RESERVATION_PROOF_MISSING",
        "memory.ceic_012.resource.reservation_proof_missing",
        {{"reason", "reservation_token_or_ledger_missing_on_release"}});
  }

  result.released = result.status.ok() && !result.fail_closed;
  if (result.released) {
    released_ = true;
    ++release_count_;
  }
  result.snapshot = Snapshot();
  result.evidence.push_back("reservation_backed_memory.release.routed=true");
  result.evidence.push_back("reservation_backed_memory.active=" +
                            BoolText(active()));
  result.evidence.push_back("reservation_backed_memory.allocated_bytes=" +
                            std::to_string(allocated_bytes_));
  result.evidence.push_back("reservation_backed_memory.reservation_released=" +
                            BoolText(result.released));
  return result;
}

ReservationBackedMemoryResourceSnapshot
ReservationBackedMemoryResource::Snapshot() const {
  ReservationBackedMemoryResourceSnapshot snapshot;
  snapshot.consumer_kind = request_.consumer_kind;
  snapshot.route_label = request_.route_label;
  snapshot.operation_id = request_.operation_id;
  snapshot.reserved_bytes = request_.requested_bytes;
  snapshot.allocated_bytes = allocated_bytes_;
  snapshot.peak_allocated_bytes = peak_allocated_bytes_;
  snapshot.allocation_count = static_cast<u64>(allocations_.size());
  snapshot.release_count = release_count_;
  snapshot.active = active();
  return snapshot;
}

ReservationBackedPmrMemoryResource::ReservationBackedPmrMemoryResource(
    ReservationBackedMemoryResource* resource,
    std::string purpose_prefix)
    : resource_(resource), purpose_prefix_(std::move(purpose_prefix)) {}

ReservationBackedPmrMemoryResourceSnapshot
ReservationBackedPmrMemoryResource::Snapshot() const {
  ReservationBackedPmrMemoryResourceSnapshot snapshot;
  snapshot.bound_to_active_resource = resource_ != nullptr && resource_->active();
  snapshot.allocation_count = allocation_count_;
  snapshot.deallocation_count = deallocation_count_;
  snapshot.failed_allocation_count = failed_allocation_count_;
  snapshot.failed_deallocation_count = failed_deallocation_count_;
  snapshot.allocated_bytes = allocated_bytes_;
  snapshot.peak_allocated_bytes = peak_allocated_bytes_;
  snapshot.last_failure = last_failure_;
  return snapshot;
}

void* ReservationBackedPmrMemoryResource::do_allocate(
    std::size_t bytes,
    std::size_t alignment) {
  if (resource_ == nullptr || !resource_->active()) {
    ++failed_allocation_count_;
    const Status status = ErrorStatus();
    last_failure_ = MakeResourceDiagnostic(
        status,
        "SB_CEIC_012_MEMORY_RESOURCE.PMR_RESOURCE_REQUIRED",
        "memory.ceic_012.resource.pmr_resource_required",
        {{"reason", "active_reservation_backed_resource_required"}});
    throw std::bad_alloc();
  }

  ReservationBackedMemoryAllocationRequest request;
  request.bytes = static_cast<u64>(bytes);
  request.alignment = alignment;
  request.purpose = purpose_prefix_.empty()
                        ? "reservation_backed_pmr"
                        : purpose_prefix_ + ".pmr";
  auto allocated = resource_->Allocate(std::move(request));
  if (!allocated.ok()) {
    ++failed_allocation_count_;
    last_failure_ = allocated.diagnostic;
    throw std::bad_alloc();
  }
  ++allocation_count_;
  allocated_bytes_ += allocated.bytes;
  peak_allocated_bytes_ = std::max(peak_allocated_bytes_, allocated_bytes_);
  return allocated.pointer;
}

void ReservationBackedPmrMemoryResource::do_deallocate(
    void* pointer,
    std::size_t bytes,
    std::size_t alignment) {
  if (pointer == nullptr) {
    return;
  }
  if (resource_ == nullptr) {
    ++failed_deallocation_count_;
    const Status status = ErrorStatus();
    last_failure_ = MakeResourceDiagnostic(
        status,
        "SB_CEIC_012_MEMORY_RESOURCE.PMR_RESOURCE_REQUIRED_ON_DEALLOCATE",
        "memory.ceic_012.resource.pmr_resource_required_on_deallocate",
        {{"reason", "active_reservation_backed_resource_required_on_deallocate"}});
    return;
  }
  auto deallocated = resource_->Deallocate(
      pointer, static_cast<usize>(bytes), static_cast<usize>(alignment));
  if (!deallocated.ok()) {
    ++failed_deallocation_count_;
    last_failure_ = deallocated.diagnostic;
    return;
  }
  ++deallocation_count_;
  allocated_bytes_ =
      allocated_bytes_ >= bytes ? allocated_bytes_ - static_cast<u64>(bytes) : 0;
}

bool ReservationBackedPmrMemoryResource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
  return this == &other;
}

ReservationBackedMemoryResourceAcquireResult
AcquireReservationBackedMemoryResource(
    ReservationBackedMemoryResourceRequest request) {
  if (request.reservation_ledger == nullptr) {
    return RefuseAcquire(
        std::move(request),
        "SB_CEIC_012_MEMORY_RESOURCE.RESERVATION_LEDGER_REQUIRED",
        "memory.ceic_012.resource.reservation_ledger_required",
        "hierarchical_memory_budget_ledger_required");
  }
  if (request.memory_manager == nullptr) {
    return RefuseAcquire(
        std::move(request),
        "SB_CEIC_012_MEMORY_RESOURCE.MEMORY_MANAGER_REQUIRED",
        "memory.ceic_012.resource.memory_manager_required",
        "memory_manager_required");
  }
  if (request.scope_chain.empty()) {
    return RefuseAcquire(
        std::move(request),
        "SB_CEIC_012_MEMORY_RESOURCE.SCOPE_CHAIN_REQUIRED",
        "memory.ceic_012.resource.scope_chain_required",
        "scope_chain_required");
  }
  if (Blank(request.owner_id) || Blank(request.route_label) ||
      Blank(request.operation_id)) {
    return RefuseAcquire(
        std::move(request),
        "SB_CEIC_012_MEMORY_RESOURCE.IDENTITY_REQUIRED",
        "memory.ceic_012.resource.identity_required",
        "owner_route_and_operation_required");
  }
  if (request.requested_bytes == 0) {
    return RefuseAcquire(
        std::move(request),
        "SB_CEIC_012_MEMORY_RESOURCE.ZERO_RESERVATION",
        "memory.ceic_012.resource.zero_reservation",
        "requested_bytes_required");
  }
  std::string unsafe_reason;
  if (UnsafeAuthority(request, &unsafe_reason)) {
    return RefuseAcquire(
        std::move(request),
        "SB_CEIC_012_MEMORY_RESOURCE.UNSAFE_AUTHORITY",
        "memory.ceic_012.resource.unsafe_authority",
        std::move(unsafe_reason));
  }

  if (request.memory_class.empty()) {
    request.memory_class =
        std::string("ceic_012.") +
        ReservationBackedMemoryConsumerKindName(request.consumer_kind);
  }
  if (request.purpose.empty()) {
    request.purpose =
        std::string("ceic_012.") +
        ReservationBackedMemoryConsumerKindName(request.consumer_kind);
  }
  request.category = DefaultCategoryFor(request.consumer_kind, request.category);
  if (request.weight == 0) {
    request.weight = 1;
  }

  HierarchicalMemoryReservationRequest reservation;
  reservation.scope_chain = request.scope_chain;
  reservation.category = request.category;
  reservation.memory_class = request.memory_class;
  reservation.requested_bytes = request.requested_bytes;
  reservation.owner_id = request.owner_id;
  reservation.spillable = request.spillable;
  reservation.cancelable = request.cancelable;
  reservation.priority = request.priority;
  reservation.weight = request.weight;
  reservation.lease_expires_at_ms = request.lease_expires_at_ms;
  reservation.provenance = request.provenance;

  auto reserved = request.reservation_ledger->Reserve(std::move(reservation));
  if (!reserved.ok()) {
    auto result = RefuseAcquire(
        request,
        "SB_CEIC_012_MEMORY_RESOURCE.RESERVATION_REFUSED",
        "memory.ceic_012.resource.reservation_refused",
        reserved.diagnostic.diagnostic_code.empty()
            ? "reservation_refused"
            : reserved.diagnostic.diagnostic_code,
        reserved.status.code);
    result.diagnostic = reserved.diagnostic;
    return result;
  }

  auto committed = request.reservation_ledger->Commit(reserved.token);
  if (!committed.ok()) {
    (void)request.reservation_ledger->Release(reserved.token);
    auto result = RefuseAcquire(
        request,
        "SB_CEIC_012_MEMORY_RESOURCE.RESERVATION_COMMIT_REFUSED",
        "memory.ceic_012.resource.reservation_commit_refused",
        committed.diagnostic.diagnostic_code.empty()
            ? "reservation_commit_refused"
            : committed.diagnostic.diagnostic_code,
        committed.status.code);
    result.diagnostic = committed.diagnostic;
    return result;
  }

  ReservationBackedMemoryResourceAcquireResult result;
  result.status = OkStatus();
  result.resource.reset(
      new ReservationBackedMemoryResource(std::move(request), reserved.token));
  AppendBaseEvidence(&result.evidence, result.resource->request());
  result.evidence.push_back("reservation_backed_memory.reservation_created=true");
  result.evidence.push_back("reservation_backed_memory.reservation_committed=true");
  result.evidence.push_back("reservation_backed_memory.reservation_token_id=" +
                            std::to_string(reserved.token.token_id));
  result.diagnostic = MakeResourceDiagnostic(
      result.status,
      "SB_CEIC_012_MEMORY_RESOURCE.OK",
      "memory.ceic_012.resource.ok",
      {{"consumer", ReservationBackedMemoryConsumerKindName(
                        result.resource->request().consumer_kind)},
       {"reserved_bytes", std::to_string(
                              result.resource->request().requested_bytes)}});
  return result;
}

}  // namespace scratchbird::core::memory
