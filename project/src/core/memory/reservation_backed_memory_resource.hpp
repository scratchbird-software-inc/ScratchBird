// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-012: reservation-backed memory resource for hot temporary arenas.
#include "hierarchical_memory_budget_ledger.hpp"
#include "memory.hpp"

#include <memory_resource>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

enum class ReservationBackedMemoryConsumerKind {
  executor_operator,
  planner_temporary,
  optimizer_temporary,
  parser_handoff,
  sblr_handoff,
  udr_invocation,
  background_maintenance,
  result_frame
};

struct ReservationBackedMemoryAuthority {
  bool engine_mga_authoritative = true;
  bool transaction_inventory_authoritative = true;
  bool security_or_policy_checked = true;
  bool parser_or_reference_finality_authority = false;
  bool memory_visibility_or_finality_authority = false;
  bool memory_recovery_authority = false;
  bool memory_authorization_authority = false;
  bool benchmark_authority = false;
  bool cluster_authority = false;
  bool debug_or_relaxed_path = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool agent_action_authority = false;
};

struct ReservationBackedMemoryResourceRequest {
  ReservationBackedMemoryConsumerKind consumer_kind =
      ReservationBackedMemoryConsumerKind::executor_operator;
  HierarchicalMemoryBudgetLedger* reservation_ledger = nullptr;
  MemoryManager* memory_manager = nullptr;
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  MemoryCategory category = MemoryCategory::unknown;
  std::string memory_class = "query_scratch";
  u64 requested_bytes = 0;
  std::string owner_id;
  std::string route_label;
  std::string operation_id;
  std::string purpose;
  bool spillable = false;
  bool cancelable = true;
  int priority = 0;
  u64 weight = 1;
  u64 lease_expires_at_ms = 0;
  bool production_like = true;
  HierarchicalMemoryBudgetProvenance provenance;
  ReservationBackedMemoryAuthority authority;
};

struct ReservationBackedMemoryAllocationRequest {
  u64 bytes = 0;
  usize alignment = 0;
  std::string purpose;
};

struct ReservationBackedMemoryResourceSnapshot {
  ReservationBackedMemoryConsumerKind consumer_kind =
      ReservationBackedMemoryConsumerKind::executor_operator;
  std::string route_label;
  std::string operation_id;
  u64 reserved_bytes = 0;
  u64 allocated_bytes = 0;
  u64 peak_allocated_bytes = 0;
  u64 allocation_count = 0;
  u64 release_count = 0;
  bool active = false;
};

struct ReservationBackedMemoryResourceReleaseResult {
  Status status;
  bool fail_closed = false;
  bool released = false;
  DiagnosticRecord diagnostic;
  ReservationBackedMemoryResourceSnapshot snapshot;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct ReservationBackedPmrMemoryResourceSnapshot {
  bool bound_to_active_resource = false;
  u64 allocation_count = 0;
  u64 deallocation_count = 0;
  u64 failed_allocation_count = 0;
  u64 failed_deallocation_count = 0;
  u64 allocated_bytes = 0;
  u64 peak_allocated_bytes = 0;
  DiagnosticRecord last_failure;
};

class ReservationBackedMemoryResource;
struct ReservationBackedMemoryResourceAcquireResult;
ReservationBackedMemoryResourceAcquireResult AcquireReservationBackedMemoryResource(
    ReservationBackedMemoryResourceRequest request);

class ReservationBackedMemoryResource {
 public:
  ReservationBackedMemoryResource(const ReservationBackedMemoryResource&) = delete;
  ReservationBackedMemoryResource& operator=(const ReservationBackedMemoryResource&) = delete;
  ~ReservationBackedMemoryResource();

  AllocationResult Allocate(ReservationBackedMemoryAllocationRequest request);
  DeallocationResult Deallocate(void* pointer, usize bytes, usize alignment);
  ReservationBackedMemoryResourceReleaseResult Release();
  ReservationBackedMemoryResourceSnapshot Snapshot() const;

  bool active() const;
  const ReservationBackedMemoryResourceRequest& request() const;
  const HierarchicalMemoryReservationToken& reservation_token() const;

 private:
  struct AllocationRecord {
    void* pointer = nullptr;
    usize bytes = 0;
    usize alignment = 0;
    MemoryTag tag;
  };

  ReservationBackedMemoryResource(ReservationBackedMemoryResourceRequest request,
                                  HierarchicalMemoryReservationToken token);

  MemoryTag TagForAllocation(const ReservationBackedMemoryAllocationRequest& request) const;

  ReservationBackedMemoryResourceRequest request_;
  HierarchicalMemoryReservationToken token_;
  std::vector<AllocationRecord> allocations_;
  u64 allocated_bytes_ = 0;
  u64 peak_allocated_bytes_ = 0;
  u64 release_count_ = 0;
  bool released_ = false;

  friend ReservationBackedMemoryResourceAcquireResult
  AcquireReservationBackedMemoryResource(ReservationBackedMemoryResourceRequest request);
};

class ReservationBackedPmrMemoryResource final : public std::pmr::memory_resource {
 public:
  ReservationBackedPmrMemoryResource(ReservationBackedMemoryResource* resource,
                                     std::string purpose_prefix);

  ReservationBackedPmrMemoryResource(const ReservationBackedPmrMemoryResource&) = delete;
  ReservationBackedPmrMemoryResource& operator=(const ReservationBackedPmrMemoryResource&) = delete;

  ReservationBackedPmrMemoryResourceSnapshot Snapshot() const;

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override;
  void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

  ReservationBackedMemoryResource* resource_ = nullptr;
  std::string purpose_prefix_;
  u64 allocation_count_ = 0;
  u64 deallocation_count_ = 0;
  u64 failed_allocation_count_ = 0;
  u64 failed_deallocation_count_ = 0;
  u64 allocated_bytes_ = 0;
  u64 peak_allocated_bytes_ = 0;
  DiagnosticRecord last_failure_;
};

struct ReservationBackedMemoryResourceAcquireResult {
  Status status;
  bool fail_closed = false;
  std::unique_ptr<ReservationBackedMemoryResource> resource;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed && resource != nullptr; }
};

const char* ReservationBackedMemoryConsumerKindName(
    ReservationBackedMemoryConsumerKind kind);

ReservationBackedMemoryResourceAcquireResult AcquireReservationBackedMemoryResource(
    ReservationBackedMemoryResourceRequest request);

}  // namespace scratchbird::core::memory
