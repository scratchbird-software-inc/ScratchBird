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
#include <type_traits>
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
    "reservation_backed_memory.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_optimizer_index_or_agent_authority";

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
  if (authority.parser_or_reference_finality_authority ||
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
    StatusCode code = StatusCode::memory_invalid_request,
    Severity severity = Severity::error) {
  ReservationBackedMemoryResourceAcquireResult result;
  result.status = ErrorStatus(code);
  result.status.severity = severity;
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
    HierarchicalMemoryReservationToken token,
    HierarchicalMemoryReservationLease lease)
    : request_(std::move(request)), token_(token), lease_(std::move(lease)) {}

ReservationBackedMemoryResource::~ReservationBackedMemoryResource() {
  (void)ReleaseNoAlloc();
}

bool ReservationBackedMemoryResource::active() const {
  std::lock_guard lock(mutex_);
  return ActiveLocked();
}

bool ReservationBackedMemoryResource::ActiveLocked() const {
  return !released_ && token_.valid() && lease_.live();
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
  if (!request_.binary_ownership.empty()) {
    tag.binary_ownership = request_.binary_ownership;
    tag.callsite = "core.memory.reservation_backed_resource";
    return tag;
  }
  tag.owner = request_.owner_id;
  tag.context_id = request_.route_label;
  for (const auto& scope : request_.scope_chain) {
    switch (scope.kind) {
      case HierarchicalMemoryScopeKind::database: tag.database_id = scope.scope_id; break;
      case HierarchicalMemoryScopeKind::session: tag.session_id = scope.scope_id; break;
      case HierarchicalMemoryScopeKind::transaction: tag.transaction_id = scope.scope_id; break;
      case HierarchicalMemoryScopeKind::statement: tag.statement_id = scope.scope_id; break;
      case HierarchicalMemoryScopeKind::query: tag.query_id = scope.scope_id; break;
      default: break;
    }
  }
  tag.callsite = "core.memory.reservation_backed_resource";
  return tag;
}

AllocationResult ReservationBackedMemoryResource::Allocate(
    ReservationBackedMemoryAllocationRequest allocation) try {
  std::lock_guard lock(mutex_);
  auto grant_use = lease_.Use();
  AllocationResult result;
  if (released_) {
    result.status = ErrorStatus();
    result.diagnostic = MakeResourceDiagnostic(
        result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.RELEASED",
        "memory.ceic_012.resource.released",
        {{"reason", "resource_released"},
         {"consumer", ReservationBackedMemoryConsumerKindName(request_.consumer_kind)}});
    return result;
  }
  if (!grant_use.live()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeResourceDiagnostic(result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.REVOKED",
        "memory.ceic_012.resource.revoked",
        {{"reason", "parent_grant_revoked_payload_still_owned"}});
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

  AllocationRecord prepared;
  prepared.tag = TagForAllocation(allocation);
  allocations_.reserve(allocations_.size() + 1);
  result = physical_capacity_->Allocate(
      static_cast<usize>(allocation.bytes),
      allocation.alignment);
  if (!result.ok()) {
    return result;
  }
  prepared.pointer = result.pointer;
  prepared.bytes = result.bytes;
  prepared.alignment = result.alignment;
  static_assert(std::is_nothrow_move_constructible_v<AllocationRecord>);
  allocations_.push_back(std::move(prepared));
  allocated_bytes_ += result.bytes;
  peak_allocated_bytes_ = std::max(peak_allocated_bytes_, allocated_bytes_);
  return result;
} catch (const std::bad_alloc&) {
  AllocationResult result;
  result.status = ErrorStatus(StatusCode::memory_allocation_failed);
  return result;
}

DeallocationResult ReservationBackedMemoryResource::Deallocate(
    void* pointer,
    usize bytes,
    usize alignment) try {
  std::lock_guard lock(mutex_);
  DeallocationResult result;
  result.status = OkStatus();
  if (pointer == nullptr) {
    return result;
  }
  if (released_) {
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

  if (alignment != 0 && std::max(alignment, alignof(std::max_align_t)) != it->alignment) {
    result.status = ErrorStatus(StatusCode::memory_invalid_request);
    result.diagnostic = MakeResourceDiagnostic(result.status,
        "SB_CEIC_012_MEMORY_RESOURCE.DEALLOCATE_ALIGNMENT_MISMATCH",
        "memory.ceic_012.resource.deallocate_alignment_mismatch");
    return result;
  }
  const auto recorded_bytes = it->bytes;
  auto deallocated = request_.memory_manager->Deallocate(it->pointer, it->tag);
  if (!deallocated.ok()) {
    return deallocated;
  }
  allocations_.erase(it);
  allocated_bytes_ =
      allocated_bytes_ - recorded_bytes;
  return deallocated;
} catch (const std::bad_alloc&) {
  return {ErrorStatus(StatusCode::memory_allocation_failed), {}};
}

Status ReservationBackedMemoryResource::DeallocateNoAlloc(
    void* pointer, usize bytes, usize alignment) {
  if (!pointer) return OkStatus();
  std::lock_guard lock(mutex_);
  if (released_ || !request_.memory_manager) return ErrorStatus();
  auto it = std::find_if(allocations_.begin(), allocations_.end(),
                        [pointer](const auto& record) { return record.pointer == pointer; });
  if (it == allocations_.end()) return ErrorStatus(StatusCode::memory_unknown_pointer);
  if ((bytes != 0 && bytes != it->bytes) ||
      (alignment != 0 && std::max(alignment, alignof(std::max_align_t)) != it->alignment))
    return ErrorStatus(StatusCode::memory_invalid_request);
  const auto status = request_.memory_manager->allocator()->DeallocateNoAlloc(pointer);
  if (!status.ok()) return status;
  allocated_bytes_ -= it->bytes;
  allocations_.erase(it);
  return status;
}

Status ReservationBackedMemoryResource::ReleaseNoAllocLocked() {
  if (released_) return OkStatus();
  if (!request_.memory_manager || !request_.reservation_ledger) return ErrorStatus();
  while (!allocations_.empty()) {
    const auto& record = allocations_.back();
    const auto status = request_.memory_manager->allocator()->DeallocateNoAlloc(record.pointer);
    if (!status.ok()) return status;
    allocated_bytes_ -= record.bytes;
    allocations_.pop_back();
  }
  // Only this retained owning context can uncharge the grant, and only after
  // every actual physical buffer has been freed. Revocation alone cannot.
  physical_capacity_.reset();
  const auto status = lease_.Reset();
  if (status.ok()) {
    released_ = true;
    ++release_count_;
  }
  return status;
}

Status ReservationBackedMemoryResource::ReleaseNoAlloc() {
  std::lock_guard lock(mutex_);
  return ReleaseNoAllocLocked();
}

ReservationBackedMemoryResourceReleaseResult
ReservationBackedMemoryResource::Release() try {
  std::lock_guard lock(mutex_);
  ReservationBackedMemoryResourceReleaseResult result;
  AppendBaseEvidence(&result.evidence, request_);
  result.snapshot = SnapshotLocked();
  // Build response storage before consuming any owner. No rich strings,
  // allocation records or snapshots are copied after the noalloc transition.
  result.evidence.push_back(released_
      ? "reservation_backed_memory.release.already_released=true"
      : "reservation_backed_memory.release.routed=true");
  const auto status = ReleaseNoAllocLocked();
  result.status = status;
  result.fail_closed = !status.ok();
  result.released = released_;
  result.snapshot.reserved_bytes = released_ ? 0 : token_.bytes;
  result.snapshot.allocated_bytes = allocated_bytes_;
  result.snapshot.allocation_count = allocations_.size();
  result.snapshot.release_count = release_count_;
  result.snapshot.active = ActiveLocked();
  return result;
} catch (const std::bad_alloc&) {
  ReservationBackedMemoryResourceReleaseResult result;
  result.status = ErrorStatus(StatusCode::memory_allocation_failed);
  result.fail_closed = true;
  return result;
}

ReservationBackedMemoryResourceSnapshot
ReservationBackedMemoryResource::Snapshot() const {
  std::lock_guard lock(mutex_);
  return SnapshotLocked();
}

ReservationBackedMemoryResourceSnapshot
ReservationBackedMemoryResource::SnapshotLocked() const {
  ReservationBackedMemoryResourceSnapshot snapshot;
  snapshot.consumer_kind = request_.consumer_kind;
  snapshot.route_label = request_.route_label;
  snapshot.operation_id = request_.operation_id;
  snapshot.reserved_bytes = released_ ? 0 : request_.requested_bytes;
  snapshot.allocated_bytes = allocated_bytes_;
  snapshot.peak_allocated_bytes = peak_allocated_bytes_;
  snapshot.allocation_count = static_cast<u64>(allocations_.size());
  snapshot.release_count = release_count_;
  snapshot.active = ActiveLocked();
  return snapshot;
}

ReservationBackedPmrMemoryResource::ReservationBackedPmrMemoryResource(
    ReservationBackedMemoryResource* resource,
    std::string purpose_prefix)
    : resource_(resource), purpose_prefix_(std::move(purpose_prefix)) {}

ReservationBackedPmrMemoryResourceSnapshot
ReservationBackedPmrMemoryResource::Snapshot() const {
  std::lock_guard lock(mutex_);
  ReservationBackedPmrMemoryResourceSnapshot snapshot;
  snapshot.bound_to_active_resource = resource_ != nullptr && resource_->active();
  snapshot.allocation_count = allocation_count_;
  snapshot.deallocation_count = deallocation_count_;
  snapshot.failed_allocation_count = failed_allocation_count_;
  snapshot.failed_deallocation_count = failed_deallocation_count_;
  snapshot.allocated_bytes = allocated_bytes_;
  snapshot.peak_allocated_bytes = peak_allocated_bytes_;
  snapshot.last_failure = last_failure_;
  if (!last_failure_status_.ok() && snapshot.last_failure.diagnostic_code.empty()) {
    snapshot.last_failure = MakeResourceDiagnostic(last_failure_status_,
        "SB_CEIC_012_MEMORY_RESOURCE.PMR_OPERATION_FAILED",
        "memory.ceic_012.resource.pmr_operation_failed");
  }
  return snapshot;
}

void* ReservationBackedPmrMemoryResource::do_allocate(
    std::size_t bytes, std::size_t alignment) {
  std::lock_guard lock(mutex_);
  Status failure = ErrorStatus(StatusCode::memory_allocation_failed);
  DiagnosticRecord failure_detail;
  try {
    if (!resource_ || !resource_->active()) {
      failure = ErrorStatus();
      throw std::bad_alloc();
    }
    ReservationBackedMemoryAllocationRequest request;
    request.bytes = static_cast<u64>(bytes);
    request.alignment = alignment;
    request.purpose = purpose_prefix_.empty() ? "reservation_backed_pmr" : purpose_prefix_ + ".pmr";
    auto allocated = resource_->Allocate(std::move(request));
    if (!allocated.ok()) {
      failure_detail = std::move(allocated.diagnostic);
      failure = allocated.status;
      throw std::bad_alloc();
    }
    ++allocation_count_;
    allocated_bytes_ += allocated.bytes;
    peak_allocated_bytes_ = std::max(peak_allocated_bytes_, allocated_bytes_);
    return allocated.pointer;
  } catch (const std::bad_alloc&) {
    ++failed_allocation_count_;
    last_failure_status_ = failure;
    last_failure_ = std::move(failure_detail);
    throw;
  }
}

void ReservationBackedPmrMemoryResource::do_deallocate(
    void* pointer, std::size_t bytes, std::size_t alignment) {
  if (!pointer) return;
  std::lock_guard lock(mutex_);
  const auto status = resource_
      ? resource_->DeallocateNoAlloc(pointer, static_cast<usize>(bytes), static_cast<usize>(alignment))
      : ErrorStatus();
  if (!status.ok()) {
    ++failed_deallocation_count_;
    last_failure_status_ = status;
    last_failure_ = {};
    return;
  }
  ++deallocation_count_;
  allocated_bytes_ -= static_cast<u64>(bytes);
}

bool ReservationBackedPmrMemoryResource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
  return this == &other;
}

ReservationBackedMemoryResourceAcquireResult
AcquireReservationBackedMemoryResource(
    ReservationBackedMemoryResourceRequest request) try {
  if (static_cast<unsigned>(request.consumer_kind) >
      static_cast<unsigned>(ReservationBackedMemoryConsumerKind::result_frame) ||
      static_cast<unsigned>(request.category) > static_cast<unsigned>(MemoryCategory::test_probe)) {
    return RefuseAcquire(std::move(request), "SB_CEIC_012_MEMORY_RESOURCE.PROFILE_INVALID",
        "memory.ceic_012.resource.profile_invalid", "unknown_consumer_or_category");
  }
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
  if ((request.binary_ownership.empty() && Blank(request.owner_id)) || Blank(request.route_label) ||
      Blank(request.operation_id)) {
    return RefuseAcquire(
        std::move(request),
        "SB_CEIC_012_MEMORY_RESOURCE.IDENTITY_REQUIRED",
        "memory.ceic_012.resource.identity_required",
        "owner_route_and_operation_required");
  }
  if (!request.binary_ownership.empty()) {
    MemoryTag tag;
    tag.owner = request.owner_id;
    tag.binary_ownership = request.binary_ownership;
    bool valid = MemoryBinaryOwnershipValid(tag);
    std::array<bool, 7> seen{};
    for (const auto& scope : request.scope_chain) {
      if (!scope.scope_id.empty() || !MemorySystemUuidValid(scope.binary_scope_uuid)) valid = false;
      const auto kind = HierarchicalMemoryBinaryScopeKind(scope.kind);
      const auto index = static_cast<usize>(kind);
      if (index >= 2 && index < seen.size()) {
        if (request.binary_ownership[kind] != scope.binary_scope_uuid) valid = false;
        seen[index] = true;
      }
    }
    for (usize index = 2; index < seen.size(); ++index)
      if (MemoryUuidPresent(request.binary_ownership.scopes[index]) != seen[index]) valid = false;
    if (!valid)
      return RefuseAcquire(std::move(request), "SB_CEIC_012_MEMORY_RESOURCE.IDENTITY_REQUIRED",
          "memory.ceic_012.resource.identity_required", "binary_owner_scope_tuple_invalid");
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
  reservation.binary_owner_uuid = request.binary_ownership[MemoryBinaryScopeKind::owner];
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

  struct PendingReservation {
    HierarchicalMemoryBudgetLedger* ledger;
    HierarchicalMemoryReservationToken token;
    bool owned = true;
    ~PendingReservation() { if (owned) (void)ledger->ReleaseNoAlloc(token); }
  } pending{request.reservation_ledger, reserved.token};
  auto committed = request.reservation_ledger->Commit(reserved.token);
  if (!committed.ok()) {
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

  auto retained = request.reservation_ledger->Retain(reserved.token);
  if (!retained.ok()) {
    return RefuseAcquire(std::move(request), "SB_CEIC_012_MEMORY_RESOURCE.RETAIN_REFUSED",
        "memory.ceic_012.resource.retain_refused", "exact_live_parent_owner_required",
        retained.status.code);
  }
  pending.owned = false; // The move-only retained lease now owns rollback.
  ReservationBackedMemoryResourceAcquireResult result;
  // Declared after result: on failure this lock dies before result's owning
  // destructor needs to release the token. Publication and revocation share
  // the same real lease state, rather than observing a stale token number.
  auto publication = retained.lease.Use();
  if (!publication.live()) {
    return RefuseAcquire(std::move(request), "SB_CEIC_012_MEMORY_RESOURCE.RETAIN_REFUSED",
        "memory.ceic_012.resource.retain_refused", "revoked_before_publication");
  }
  result.status = OkStatus();
  result.resource.reset(
      new ReservationBackedMemoryResource(std::move(request), reserved.token, std::move(retained.lease)));
  auto& owner = *result.resource;
  if (owner.request_.requested_bytes > std::numeric_limits<usize>::max())
    return RefuseAcquire(owner.request_, "SB_CEIC_012_MEMORY_RESOURCE.RESERVATION_REFUSED",
        "memory.ceic_012.resource.reservation_refused", "physical_capacity_size_unrepresentable",
        StatusCode::memory_limit_exceeded);
  auto physical = owner.request_.memory_manager->allocator()->ReserveCapacity(
      static_cast<usize>(owner.request_.requested_bytes), owner.TagForAllocation({}));
  if (!physical.ok())
    return RefuseAcquire(owner.request_, "SB_CEIC_012_MEMORY_RESOURCE.RESERVATION_REFUSED",
        "memory.ceic_012.resource.reservation_refused", "shared_physical_capacity_refused",
        physical.status.code, physical.status.severity);
  owner.physical_capacity_ = std::move(physical.reservation);
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
} catch (const std::bad_alloc&) {
  ReservationBackedMemoryResourceAcquireResult result;
  result.status = ErrorStatus(StatusCode::memory_allocation_failed);
  result.fail_closed = true;
  return result;
}

}  // namespace scratchbird::core::memory
