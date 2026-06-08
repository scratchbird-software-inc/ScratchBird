// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PAGE-ALLOCATION-LIFECYCLE-ANCHOR
#include "runtime_platform.hpp"
#include "filespace_lifecycle.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class PageAllocationLifecycleState : u32 {
  free = 0,
  reserved = 1,
  allocated = 2,
  reusable_pending_mga = 3,
  reusable_free = 4,
  compacting = 5,
  quarantined = 6,
  preallocated = 7
};

enum class PageAllocationRecoveryAction : u32 {
  no_action,
  retain,
  release_to_free_map,
  quarantine,
  fail_closed
};

struct PageFreeExtent {
  u64 start_page = 0;
  u64 page_count = 0;
};

struct PageAllocationEntry {
  TypedUuid allocation_uuid;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid owner_object_uuid;
  TypedUuid policy_uuid;
  TypedUuid capacity_evidence_uuid;
  TypedUuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  u64 start_page = 0;
  u64 page_count = 0;
  u64 durable_page_generation = 0;
  u64 published_page_generation = 0;
  std::string page_family;
  std::string filespace_class;
  std::string filespace_class_reason;
  PageAllocationLifecycleState state = PageAllocationLifecycleState::free;
  u64 reusable_after_local_transaction_id = 0;
  bool durability_fence_satisfied = false;
};

struct PageAllocationEvidenceRecord {
  u64 sequence = 0;
  std::string action;
  TypedUuid allocation_uuid;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid owner_object_uuid;
  TypedUuid policy_uuid;
  TypedUuid capacity_evidence_uuid;
  u64 local_transaction_id = 0;
  u64 start_page = 0;
  u64 page_count = 0;
  u64 durable_page_generation = 0;
  u64 published_page_generation = 0;
  PageAllocationLifecycleState previous_state = PageAllocationLifecycleState::free;
  PageAllocationLifecycleState new_state = PageAllocationLifecycleState::free;
  std::string filespace_class;
  std::string filespace_class_reason;
  bool durable_state_changed = false;
  bool durability_fence_satisfied = false;
  bool capacity_evidence_accepted = false;
  std::string diagnostic_code;
  std::string reason;
};

struct PageAllocationLedger {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::vector<PageFreeExtent> free_extents;
  std::vector<PageAllocationEntry> allocations;
  std::vector<PageAllocationEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
  u64 next_allocation_seed = 1;
};

struct PageAllocationRequest {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid owner_object_uuid;
  TypedUuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  std::string page_family;
  scratchbird::storage::filespace::FilespaceObjectClass object_class =
      scratchbird::storage::filespace::FilespaceObjectClass::unspecified;
  u64 page_count = 0;
  u64 page_generation = 1;
  bool engine_authoritative = false;
  bool durability_fence_satisfied = true;
  bool cluster_route_requested = false;
  bool explicit_filespace_class = false;
};

struct PagePreallocationRequest {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid capacity_evidence_uuid;
  TypedUuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  std::string page_family;
  scratchbird::storage::filespace::FilespaceObjectClass object_class =
      scratchbird::storage::filespace::FilespaceObjectClass::unspecified;
  u64 page_count = 0;
  u64 page_generation = 1;
  bool engine_authoritative = false;
  bool capacity_evidence_accepted = false;
  bool durability_fence_satisfied = false;
  bool cluster_route_requested = false;
  bool explicit_filespace_class = false;
};

struct PageReleaseRequest {
  TypedUuid allocation_uuid;
  u64 cleanup_horizon_local_transaction_id = 0;
  bool engine_mga_authoritative = false;
  std::string reason;
};

struct PageCompactionRequest {
  bool engine_authoritative = false;
  bool shutdown_or_maintenance_fenced = false;
  std::string reason;
};

struct PageAllocationResult {
  Status status;
  bool admitted = false;
  PageAllocationEntry allocation;
  PageAllocationEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && admitted;
  }
};

struct PageAllocationMutationResult {
  Status status;
  bool changed = false;
  PageAllocationEntry allocation;
  PageAllocationEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct PageAllocationRecoveryClassification {
  TypedUuid allocation_uuid;
  PageAllocationLifecycleState observed_state = PageAllocationLifecycleState::free;
  PageAllocationRecoveryAction action = PageAllocationRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct PageAllocationRecoveryResult {
  Status status;
  std::vector<PageAllocationRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* PageAllocationLifecycleStateName(PageAllocationLifecycleState state);
const char* PageAllocationRecoveryActionName(PageAllocationRecoveryAction action);

PageAllocationResult ReservePageAllocation(PageAllocationLedger* ledger,
                                           const PageAllocationRequest& request);
PageAllocationResult PreallocatePageFamilyPool(PageAllocationLedger* ledger,
                                               const PagePreallocationRequest& request);
PageAllocationMutationResult MarkPageAllocationReusable(PageAllocationLedger* ledger,
                                                        const PageReleaseRequest& request);
PageAllocationMutationResult ReclaimReusablePageAllocation(PageAllocationLedger* ledger,
                                                           const PageReleaseRequest& request);
PageAllocationMutationResult CompactPageFreeSpace(PageAllocationLedger* ledger,
                                                  const PageCompactionRequest& request);
PageAllocationRecoveryClassification ClassifyPageAllocationForRecovery(
    const PageAllocationEntry& allocation);
PageAllocationRecoveryResult ClassifyPageAllocationLedgerForRecovery(
    const PageAllocationLedger& ledger);
const PageAllocationEntry* FindPageAllocation(const PageAllocationLedger& ledger,
                                              const TypedUuid& allocation_uuid);
DiagnosticRecord MakePageAllocationLifecycleDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail = {});

}  // namespace scratchbird::storage::page
