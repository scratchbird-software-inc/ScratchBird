// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_allocation_lifecycle.hpp"

#include "metric_producer.hpp"
#include "page_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace scratchbird::storage::page {
namespace {

namespace filespace = scratchbird::storage::filespace;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::core::uuid::IsEngineIdentityUuid;

Status AllocationOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status AllocationErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool IsOptionalObjectIdentity(const TypedUuid& uuid) {
  return !uuid.valid() || IsTypedEngineIdentity(uuid, UuidKind::object);
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool IsKnownPageFamilyForAllocation(const std::string& family) {
  return IsKnownPageFamilyName(family) || family == "overflow" ||
         family == "toast";
}

TypedUuid MakeAllocationUuid(const PageAllocationLedger* ledger, const PageAllocationRequest& request) {
  const u64 allocation_seed = ledger == nullptr ? 1 : ledger->next_allocation_seed;
  const u64 seed = allocation_seed + request.creator_local_transaction_id + request.page_count;
  const auto generated = GenerateEngineIdentityV7(UuidKind::object, seed == 0 ? 1 : seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

void AdvanceAllocationSeed(PageAllocationLedger* ledger) {
  if (ledger != nullptr) {
    ++ledger->next_allocation_seed;
  }
}

PageAllocationEvidenceRecord MakeEvidence(PageAllocationLedger* ledger,
                                          const PageAllocationEntry& before,
                                          const PageAllocationEntry& after,
                                          std::string action,
                                          std::string reason,
                                          std::string diagnostic_code) {
  PageAllocationEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.action = std::move(action);
  evidence.allocation_uuid = after.allocation_uuid.valid() ? after.allocation_uuid : before.allocation_uuid;
  evidence.database_uuid = after.database_uuid.valid() ? after.database_uuid : before.database_uuid;
  evidence.filespace_uuid = after.filespace_uuid.valid() ? after.filespace_uuid : before.filespace_uuid;
  evidence.owner_object_uuid = after.owner_object_uuid.valid() ? after.owner_object_uuid : before.owner_object_uuid;
  evidence.policy_uuid = after.policy_uuid.valid() ? after.policy_uuid : before.policy_uuid;
  evidence.capacity_evidence_uuid = after.capacity_evidence_uuid.valid()
                                        ? after.capacity_evidence_uuid
                                        : before.capacity_evidence_uuid;
  evidence.local_transaction_id = after.creator_local_transaction_id != 0
                                      ? after.creator_local_transaction_id
                                      : before.creator_local_transaction_id;
  evidence.start_page = after.start_page != 0 ? after.start_page : before.start_page;
  evidence.page_count = after.page_count != 0 ? after.page_count : before.page_count;
  evidence.durable_page_generation = after.durable_page_generation != 0
                                         ? after.durable_page_generation
                                         : before.durable_page_generation;
  evidence.published_page_generation = after.published_page_generation != 0
                                           ? after.published_page_generation
                                           : before.published_page_generation;
  evidence.filespace_class = !after.filespace_class.empty()
                                 ? after.filespace_class
                                 : before.filespace_class;
  evidence.filespace_class_reason = !after.filespace_class_reason.empty()
                                        ? after.filespace_class_reason
                                        : before.filespace_class_reason;
  evidence.previous_state = before.state;
  evidence.new_state = after.state;
  evidence.durable_state_changed = before.state != after.state || action.find("refuse") == std::string::npos;
  evidence.durability_fence_satisfied = after.durability_fence_satisfied ||
                                        before.durability_fence_satisfied;
  evidence.capacity_evidence_accepted = after.capacity_evidence_uuid.valid() ||
                                        before.capacity_evidence_uuid.valid();
  evidence.reason = std::move(reason);
  evidence.diagnostic_code = std::move(diagnostic_code);
  if (ledger != nullptr) {
    ledger->evidence.push_back(evidence);
  }
  return evidence;
}

void ApplyFilespaceClassEvidence(PageAllocationEvidenceRecord* evidence,
                                 const filespace::FilespaceClassDecision& decision) {
  if (evidence == nullptr || !decision.ok()) {
    return;
  }
  evidence->filespace_class = filespace::FilespaceClassName(decision.filespace_class);
  evidence->filespace_class_reason = decision.diagnostic.diagnostic_code;
  if (!evidence->reason.empty()) {
    evidence->reason += ";";
  }
  evidence->reason += "filespace_class=";
  evidence->reason += filespace::FilespaceClassName(decision.filespace_class);
  evidence->reason += ";object_class=";
  evidence->reason += filespace::FilespaceObjectClassName(decision.object_class);
  evidence->reason += ";mga_visibility_authority=durable_transaction_inventory";
  evidence->reason += ";finality_authority=false";
}

filespace::FilespaceClassDecision ResolveAllocationFilespaceClass(
    const PageAllocationRequest& request) {
  filespace::FilespaceClassRequest class_request;
  class_request.database_uuid = request.database_uuid;
  class_request.filespace_uuid = request.filespace_uuid;
  class_request.owner_object_uuid = request.owner_object_uuid;
  class_request.object_class = request.object_class;
  class_request.page_family = request.page_family;
  class_request.reason = "page_allocation_lifecycle";
  class_request.explicit_object_class =
      request.explicit_filespace_class ||
      request.object_class != filespace::FilespaceObjectClass::unspecified;
  return filespace::ResolveFilespaceClass(class_request);
}

filespace::FilespaceClassDecision ResolvePreallocationFilespaceClass(
    const PagePreallocationRequest& request) {
  filespace::FilespaceClassRequest class_request;
  class_request.database_uuid = request.database_uuid;
  class_request.filespace_uuid = request.filespace_uuid;
  class_request.owner_object_uuid = request.policy_uuid;
  class_request.object_class = request.object_class;
  class_request.page_family = request.page_family;
  class_request.reason = "page_preallocation_lifecycle";
  class_request.explicit_object_class =
      request.explicit_filespace_class ||
      request.object_class != filespace::FilespaceObjectClass::unspecified;
  return filespace::ResolveFilespaceClass(class_request);
}

PageAllocationResult AllocationError(PageAllocationLedger* ledger,
                                     const PageAllocationRequest& request,
                                     std::string diagnostic_code,
                                     std::string message_key,
                                     std::string detail = {}) {
  (void)ledger;
  PageAllocationResult result;
  result.status = AllocationErrorStatus();
  result.diagnostic = MakePageAllocationLifecycleDiagnostic(result.status,
                                                            std::move(diagnostic_code),
                                                            std::move(message_key),
                                                            std::move(detail));
  PageAllocationEntry request_entry;
  request_entry.database_uuid = request.database_uuid;
  request_entry.filespace_uuid = request.filespace_uuid;
  request_entry.owner_object_uuid = request.owner_object_uuid;
  request_entry.creator_transaction_uuid = request.creator_transaction_uuid;
  request_entry.creator_local_transaction_id = request.creator_local_transaction_id;
  request_entry.durable_page_generation = request.durability_fence_satisfied
      ? request.page_generation
      : 0;
  request_entry.published_page_generation = request.page_generation;
  request_entry.durability_fence_satisfied = request.durability_fence_satisfied;
  request_entry.page_family = request.page_family;
  request_entry.filespace_class = "forbidden";
  request_entry.filespace_class_reason = result.diagnostic.diagnostic_code;
  result.evidence = MakeEvidence(nullptr,
                                 PageAllocationEntry{},
                                 request_entry,
                                 "allocate_refuse",
                                 result.diagnostic.diagnostic_code,
                                 result.diagnostic.diagnostic_code);
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_page_allocation_lifecycle_refused_total",
      scratchbird::core::metrics::Labels({{"component", "storage.page_allocation"},
                                          {"reason", result.diagnostic.diagnostic_code},
                                          {"page_family", request.page_family}}),
      1.0,
      "storage_page_allocation");
  return result;
}

PageAllocationResult PreallocationError(const PagePreallocationRequest& request,
                                        std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail = {}) {
  PageAllocationResult result;
  result.status = AllocationErrorStatus();
  result.diagnostic = MakePageAllocationLifecycleDiagnostic(result.status,
                                                            std::move(diagnostic_code),
                                                            std::move(message_key),
                                                            std::move(detail));
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_page_preallocation_lifecycle_refused_total",
      scratchbird::core::metrics::Labels({{"component", "storage.page_allocation"},
                                          {"reason", result.diagnostic.diagnostic_code},
                                          {"page_family", request.page_family}}),
      1.0,
      "storage_page_allocation");
  return result;
}

PageAllocationMutationResult MutationError(std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {}) {
  PageAllocationMutationResult result;
  result.status = AllocationErrorStatus();
  result.diagnostic = MakePageAllocationLifecycleDiagnostic(result.status,
                                                            std::move(diagnostic_code),
                                                            std::move(message_key),
                                                            std::move(detail));
  return result;
}

PageAllocationEntry* FindMutable(PageAllocationLedger* ledger, const TypedUuid& allocation_uuid) {
  if (ledger == nullptr) {
    return nullptr;
  }
  for (auto& allocation : ledger->allocations) {
    if (allocation.allocation_uuid.value == allocation_uuid.value) {
      return &allocation;
    }
  }
  return nullptr;
}

bool TakeFreeExtent(PageAllocationLedger* ledger, u64 page_count, u64* start_page) {
  if (ledger == nullptr || start_page == nullptr || page_count == 0) {
    return false;
  }
  for (auto it = ledger->free_extents.begin(); it != ledger->free_extents.end(); ++it) {
    if (it->page_count < page_count) {
      continue;
    }
    *start_page = it->start_page;
    it->start_page += page_count;
    it->page_count -= page_count;
    if (it->page_count == 0) {
      ledger->free_extents.erase(it);
    }
    return true;
  }
  return false;
}

constexpr std::size_t kNoAllocationIndex = static_cast<std::size_t>(-1);

bool PreallocatedPoolScopeMatches(const PageAllocationEntry& allocation,
                                  const PageAllocationRequest& request,
                                  const std::string& requested_filespace_class) {
  return allocation.state == PageAllocationLifecycleState::preallocated &&
         SameTypedUuid(allocation.database_uuid, request.database_uuid) &&
         SameTypedUuid(allocation.filespace_uuid, request.filespace_uuid) &&
         allocation.page_family == request.page_family &&
         allocation.filespace_class == requested_filespace_class;
}

bool PreallocatedPoolDurable(const PageAllocationEntry& allocation) {
  return allocation.durability_fence_satisfied &&
         allocation.published_page_generation != 0 &&
         allocation.durable_page_generation >= allocation.published_page_generation;
}

std::size_t FindSufficientPreallocatedPoolIndex(const PageAllocationLedger& ledger,
                                                const PageAllocationRequest& request,
                                                const std::string& requested_filespace_class) {
  for (std::size_t index = 0; index < ledger.allocations.size(); ++index) {
    const auto& allocation = ledger.allocations[index];
    if (PreallocatedPoolScopeMatches(allocation, request, requested_filespace_class) &&
        PreallocatedPoolDurable(allocation) &&
        allocation.page_count >= request.page_count) {
      return index;
    }
  }
  return kNoAllocationIndex;
}

bool HasUnsafeSufficientPreallocatedPool(const PageAllocationLedger& ledger,
                                         const PageAllocationRequest& request,
                                         const std::string& requested_filespace_class) {
  for (const auto& allocation : ledger.allocations) {
    if (PreallocatedPoolScopeMatches(allocation, request, requested_filespace_class) &&
        !PreallocatedPoolDurable(allocation) &&
        allocation.page_count >= request.page_count) {
      return true;
    }
  }
  return false;
}

std::size_t FindSufficientFreeExtentIndex(const PageAllocationLedger& ledger,
                                          u64 page_count) {
  for (std::size_t index = 0; index < ledger.free_extents.size(); ++index) {
    if (ledger.free_extents[index].page_count >= page_count) {
      return index;
    }
  }
  return kNoAllocationIndex;
}

bool TakeFreeExtentAtIndex(PageAllocationLedger* ledger,
                           std::size_t extent_index,
                           u64 page_count,
                           u64* start_page) {
  if (ledger == nullptr || start_page == nullptr ||
      extent_index >= ledger->free_extents.size() || page_count == 0 ||
      ledger->free_extents[extent_index].page_count < page_count) {
    return false;
  }
  auto& extent = ledger->free_extents[extent_index];
  *start_page = extent.start_page;
  extent.start_page += page_count;
  extent.page_count -= page_count;
  if (extent.page_count == 0) {
    ledger->free_extents.erase(ledger->free_extents.begin() +
                               static_cast<std::ptrdiff_t>(extent_index));
  }
  return true;
}

bool ConsumePreallocatedPoolAtIndex(PageAllocationLedger* ledger,
                                    std::size_t allocation_index,
                                    u64 page_count,
                                    u64* start_page,
                                    PageAllocationEntry* consumed_pool) {
  if (ledger == nullptr || start_page == nullptr || consumed_pool == nullptr ||
      allocation_index >= ledger->allocations.size() || page_count == 0 ||
      ledger->allocations[allocation_index].page_count < page_count) {
    return false;
  }
  *consumed_pool = ledger->allocations[allocation_index];
  *start_page = consumed_pool->start_page;
  if (consumed_pool->page_count == page_count) {
    ledger->allocations.erase(ledger->allocations.begin() +
                              static_cast<std::ptrdiff_t>(allocation_index));
  } else {
    auto& pool = ledger->allocations[allocation_index];
    pool.start_page += page_count;
    pool.page_count -= page_count;
  }
  return true;
}

void AddFreeExtent(PageAllocationLedger* ledger, u64 start_page, u64 page_count) {
  if (ledger == nullptr || start_page == 0 || page_count == 0) {
    return;
  }
  ledger->free_extents.push_back({start_page, page_count});
  std::sort(ledger->free_extents.begin(),
            ledger->free_extents.end(),
            [](const PageFreeExtent& left, const PageFreeExtent& right) {
              return left.start_page < right.start_page;
            });

  std::vector<PageFreeExtent> merged;
  for (const auto& extent : ledger->free_extents) {
    if (merged.empty()) {
      merged.push_back(extent);
      continue;
    }
    auto& last = merged.back();
    if (last.start_page + last.page_count == extent.start_page) {
      last.page_count += extent.page_count;
    } else {
      merged.push_back(extent);
    }
  }
  ledger->free_extents = std::move(merged);
}

bool HasPendingMgaReusablePages(const PageAllocationLedger& ledger) {
  for (const auto& allocation : ledger.allocations) {
    if (allocation.state == PageAllocationLifecycleState::reusable_pending_mga) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* PageAllocationLifecycleStateName(PageAllocationLifecycleState state) {
  switch (state) {
    case PageAllocationLifecycleState::free: return "free";
    case PageAllocationLifecycleState::reserved: return "reserved";
    case PageAllocationLifecycleState::preallocated: return "preallocated";
    case PageAllocationLifecycleState::allocated: return "allocated";
    case PageAllocationLifecycleState::reusable_pending_mga: return "reusable_pending_mga";
    case PageAllocationLifecycleState::reusable_free: return "reusable_free";
    case PageAllocationLifecycleState::compacting: return "compacting";
    case PageAllocationLifecycleState::quarantined: return "quarantined";
  }
  return "unknown";
}

const char* PageAllocationRecoveryActionName(PageAllocationRecoveryAction action) {
  switch (action) {
    case PageAllocationRecoveryAction::no_action: return "no_action";
    case PageAllocationRecoveryAction::retain: return "retain";
    case PageAllocationRecoveryAction::release_to_free_map: return "release_to_free_map";
    case PageAllocationRecoveryAction::quarantine: return "quarantine";
    case PageAllocationRecoveryAction::fail_closed: return "fail_closed";
  }
  return "unknown";
}

// SEARCH_KEY: SB_PAGE_ALLOCATION_LIFECYCLE_RESERVE
PageAllocationResult ReservePageAllocation(PageAllocationLedger* ledger,
                                           const PageAllocationRequest& request) {
  if (ledger == nullptr) {
    return AllocationError(nullptr,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-LEDGER-REQUIRED",
                           "storage.page_allocation.ledger_required");
  }
  if (!request.engine_authoritative) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-NOT-ENGINE-AUTHORITATIVE",
                           "storage.page_allocation.not_engine_authoritative");
  }
  if (request.cluster_route_requested) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-CLUSTER-ROUTE-UNAVAILABLE",
                           "storage.page_allocation.cluster_route_unavailable");
  }
  if (!IsTypedEngineIdentity(request.database_uuid, UuidKind::database) ||
      !IsTypedEngineIdentity(request.filespace_uuid, UuidKind::filespace) ||
      !IsTypedEngineIdentity(request.creator_transaction_uuid, UuidKind::transaction) ||
      !IsOptionalObjectIdentity(request.owner_object_uuid)) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-UUID-INVALID",
                           "storage.page_allocation.uuid_invalid");
  }
  if (ledger->database_uuid.valid() && ledger->database_uuid.value != request.database_uuid.value) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-DATABASE-MISMATCH",
                           "storage.page_allocation.database_mismatch");
  }
  if (ledger->filespace_uuid.valid() && ledger->filespace_uuid.value != request.filespace_uuid.value) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-FILESPACE-MISMATCH",
                           "storage.page_allocation.filespace_mismatch");
  }
  if (request.creator_local_transaction_id == 0 || request.page_count == 0 ||
      !IsKnownPageFamilyForAllocation(request.page_family)) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-REQUEST-INVALID",
                           "storage.page_allocation.request_invalid");
  }
  if (request.page_generation == 0 || !request.durability_fence_satisfied) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-DURABILITY-FENCE-REQUIRED",
                           "storage.page_allocation.durability_fence_required");
  }
  const auto filespace_class = ResolveAllocationFilespaceClass(request);
  if (!filespace_class.ok()) {
    return AllocationError(ledger,
                           request,
                           filespace_class.diagnostic.diagnostic_code,
                           filespace_class.diagnostic.message_key);
  }
  const std::string requested_filespace_class =
      filespace::FilespaceClassName(filespace_class.filespace_class);

  const std::size_t preallocated_pool_index =
      FindSufficientPreallocatedPoolIndex(*ledger,
                                          request,
                                          requested_filespace_class);
  const bool allocate_from_preallocated_pool =
      preallocated_pool_index != kNoAllocationIndex;
  if (!allocate_from_preallocated_pool &&
      HasUnsafeSufficientPreallocatedPool(*ledger,
                                          request,
                                          requested_filespace_class)) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-NOT-DURABLE",
                           "storage.page_allocation.preallocated_pool_not_durable");
  }
  const std::size_t free_extent_index =
      allocate_from_preallocated_pool
          ? kNoAllocationIndex
          : FindSufficientFreeExtentIndex(*ledger, request.page_count);
  if (!allocate_from_preallocated_pool && free_extent_index == kNoAllocationIndex) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-INSUFFICIENT-FREE-SPACE",
                           "storage.page_allocation.insufficient_free_space");
  }

  PageAllocationEntry consumed_pool;
  u64 start_page = 0;
  const TypedUuid allocation_uuid = MakeAllocationUuid(ledger, request);
  if (!allocation_uuid.valid()) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-IDENTITY-INVALID",
                           "storage.page_allocation.identity_invalid");
  }
  const bool source_consumed = allocate_from_preallocated_pool
      ? ConsumePreallocatedPoolAtIndex(ledger,
                                       preallocated_pool_index,
                                       request.page_count,
                                       &start_page,
                                       &consumed_pool)
      : TakeFreeExtentAtIndex(ledger, free_extent_index, request.page_count, &start_page);
  if (!source_consumed) {
    return AllocationError(ledger,
                           request,
                           "SB-STORAGE-PAGE-ALLOCATION-SOURCE-STALE",
                           "storage.page_allocation.source_stale");
  }
  AdvanceAllocationSeed(ledger);

  PageAllocationEntry allocation;
  allocation.allocation_uuid = allocation_uuid;
  allocation.database_uuid = request.database_uuid;
  allocation.filespace_uuid = request.filespace_uuid;
  allocation.owner_object_uuid = request.owner_object_uuid;
  allocation.policy_uuid = consumed_pool.policy_uuid;
  allocation.capacity_evidence_uuid = consumed_pool.capacity_evidence_uuid;
  allocation.creator_transaction_uuid = request.creator_transaction_uuid;
  allocation.creator_local_transaction_id = request.creator_local_transaction_id;
  allocation.start_page = start_page;
  allocation.page_count = request.page_count;
  allocation.durable_page_generation = request.page_generation;
  allocation.published_page_generation = request.page_generation;
  allocation.durability_fence_satisfied = true;
  allocation.page_family = request.page_family;
  allocation.filespace_class = filespace::FilespaceClassName(filespace_class.filespace_class);
  allocation.filespace_class_reason = filespace_class.diagnostic.diagnostic_code;
  allocation.state = PageAllocationLifecycleState::allocated;
  ledger->database_uuid = request.database_uuid;
  ledger->filespace_uuid = request.filespace_uuid;
  ledger->allocations.push_back(allocation);

  PageAllocationResult result;
  result.status = AllocationOkStatus();
  result.admitted = true;
  result.allocation = allocation;
  const std::string diagnostic_code =
      allocate_from_preallocated_pool
          ? "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"
          : "SB-STORAGE-PAGE-ALLOCATION-FREE-EXTENT-FALLBACK";
  result.diagnostic = MakePageAllocationLifecycleDiagnostic(
      result.status,
      diagnostic_code,
      allocate_from_preallocated_pool
          ? "storage.page_allocation.preallocated_pool_hit"
          : "storage.page_allocation.free_extent_fallback");
  result.evidence = MakeEvidence(
      ledger,
      allocate_from_preallocated_pool ? consumed_pool : PageAllocationEntry{},
      allocation,
      allocate_from_preallocated_pool ? "allocate_from_preallocated_pool"
                                      : "allocate_from_free_extent",
      allocate_from_preallocated_pool
          ? "allocated_from_preallocated_page_family_pool"
          : "allocated_from_free_extent_after_preallocation_miss",
      diagnostic_code);
  ApplyFilespaceClassEvidence(&result.evidence, filespace_class);
  if (!ledger->evidence.empty()) {
    ApplyFilespaceClassEvidence(&ledger->evidence.back(), filespace_class);
  }
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_page_allocation_lifecycle_allocated_total",
      scratchbird::core::metrics::Labels({{"component", "storage.page_allocation"},
                                          {"page_family", request.page_family},
                                          {"filespace_class", allocation.filespace_class},
                                          {"source", allocate_from_preallocated_pool
                                                         ? "preallocated_pool"
                                                         : "free_extent"}}),
      static_cast<double>(request.page_count),
      "storage_page_allocation");
  return result;
}

PageAllocationResult PreallocatePageFamilyPool(PageAllocationLedger* ledger,
                                               const PagePreallocationRequest& request) {
  if (ledger == nullptr) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-LEDGER-REQUIRED",
                              "storage.page_allocation.preallocation_ledger_required");
  }
  if (!request.engine_authoritative) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-NOT-ENGINE-AUTHORITATIVE",
                              "storage.page_allocation.preallocation_not_engine_authoritative");
  }
  if (request.cluster_route_requested) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-CLUSTER-ROUTE-UNAVAILABLE",
                              "storage.page_allocation.preallocation_cluster_route_unavailable");
  }
  if (!IsTypedEngineIdentity(request.database_uuid, UuidKind::database) ||
      !IsTypedEngineIdentity(request.filespace_uuid, UuidKind::filespace) ||
      !IsTypedEngineIdentity(request.policy_uuid, UuidKind::object) ||
      !IsTypedEngineIdentity(request.creator_transaction_uuid, UuidKind::transaction)) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-UUID-INVALID",
                              "storage.page_allocation.preallocation_uuid_invalid");
  }
  if (request.capacity_evidence_uuid.valid() &&
      !IsTypedEngineIdentity(request.capacity_evidence_uuid, UuidKind::object)) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-CAPACITY-EVIDENCE-INVALID",
                              "storage.page_allocation.preallocation_capacity_evidence_invalid");
  }
  if (ledger->database_uuid.valid() && ledger->database_uuid.value != request.database_uuid.value) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-DATABASE-MISMATCH",
                              "storage.page_allocation.preallocation_database_mismatch");
  }
  if (ledger->filespace_uuid.valid() && ledger->filespace_uuid.value != request.filespace_uuid.value) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-FILESPACE-MISMATCH",
                              "storage.page_allocation.preallocation_filespace_mismatch");
  }
  if (request.creator_local_transaction_id == 0 || request.page_count == 0 ||
      !IsKnownPageFamilyForAllocation(request.page_family)) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-REQUEST-INVALID",
                              "storage.page_allocation.preallocation_request_invalid");
  }
  if (request.page_generation == 0 || !request.durability_fence_satisfied) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-DURABILITY-FENCE-REQUIRED",
                              "storage.page_allocation.preallocation_durability_fence_required");
  }
  const auto filespace_class = ResolvePreallocationFilespaceClass(request);
  if (!filespace_class.ok()) {
    return PreallocationError(request,
                              filespace_class.diagnostic.diagnostic_code,
                              filespace_class.diagnostic.message_key);
  }

  const u64 seed = ledger->next_allocation_seed +
                   request.creator_local_transaction_id + request.page_count;
  const auto generated = GenerateEngineIdentityV7(UuidKind::object, seed == 0 ? 1 : seed);
  if (!generated.ok()) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-IDENTITY-INVALID",
                              "storage.page_allocation.preallocation_identity_invalid");
  }

  u64 start_page = 0;
  if (!TakeFreeExtent(ledger, request.page_count, &start_page)) {
    return PreallocationError(request,
                              "SB-STORAGE-PAGE-PREALLOCATION-INSUFFICIENT-FREE-SPACE",
                              "storage.page_allocation.preallocation_insufficient_free_space");
  }
  ++ledger->next_allocation_seed;

  PageAllocationEntry allocation;
  allocation.allocation_uuid = generated.value;
  allocation.database_uuid = request.database_uuid;
  allocation.filespace_uuid = request.filespace_uuid;
  allocation.policy_uuid = request.policy_uuid;
  allocation.capacity_evidence_uuid = request.capacity_evidence_uuid;
  allocation.creator_transaction_uuid = request.creator_transaction_uuid;
  allocation.creator_local_transaction_id = request.creator_local_transaction_id;
  allocation.start_page = start_page;
  allocation.page_count = request.page_count;
  allocation.durable_page_generation = request.page_generation;
  allocation.published_page_generation = request.page_generation;
  allocation.durability_fence_satisfied = true;
  allocation.page_family = request.page_family;
  allocation.filespace_class = filespace::FilespaceClassName(filespace_class.filespace_class);
  allocation.filespace_class_reason = filespace_class.diagnostic.diagnostic_code;
  allocation.state = PageAllocationLifecycleState::preallocated;
  ledger->database_uuid = request.database_uuid;
  ledger->filespace_uuid = request.filespace_uuid;
  ledger->allocations.push_back(allocation);

  PageAllocationResult result;
  result.status = AllocationOkStatus();
  result.allocation = allocation;
  result.evidence = MakeEvidence(
      ledger,
      PageAllocationEntry{},
      allocation,
      "preallocate_page_family_pool",
      request.capacity_evidence_accepted
          ? "preallocated_by_engine_page_allocation_lifecycle_with_capacity_evidence"
          : "preallocated_by_engine_page_allocation_lifecycle",
      "SB-STORAGE-PAGE-PREALLOCATION-PREALLOCATED");
  ApplyFilespaceClassEvidence(&result.evidence, filespace_class);
  result.evidence.capacity_evidence_accepted = request.capacity_evidence_accepted;
  if (!ledger->evidence.empty()) {
    ledger->evidence.back().capacity_evidence_accepted =
        request.capacity_evidence_accepted;
    ApplyFilespaceClassEvidence(&ledger->evidence.back(), filespace_class);
    result.evidence = ledger->evidence.back();
  }
  result.admitted = result.evidence.durable_state_changed &&
                    result.evidence.durability_fence_satisfied &&
                    result.evidence.new_state == PageAllocationLifecycleState::preallocated;
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_page_preallocation_lifecycle_preallocated_total",
      scratchbird::core::metrics::Labels({{"component", "storage.page_allocation"},
                                          {"page_family", request.page_family},
                                          {"filespace_class", allocation.filespace_class}}),
      static_cast<double>(request.page_count),
      "storage_page_allocation");
  return result;
}

PageAllocationMutationResult MarkPageAllocationReusable(PageAllocationLedger* ledger,
                                                        const PageReleaseRequest& request) {
  if (ledger == nullptr) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-LEDGER-REQUIRED",
                         "storage.page_allocation.ledger_required");
  }
  if (!request.engine_mga_authoritative) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-MGA-AUTHORITY-REQUIRED",
                         "storage.page_allocation.mga_authority_required");
  }
  auto* allocation = FindMutable(ledger, request.allocation_uuid);
  if (allocation == nullptr) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-NOT-FOUND",
                         "storage.page_allocation.not_found");
  }
  if (allocation->state != PageAllocationLifecycleState::allocated) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-STATE-INVALID",
                         "storage.page_allocation.state_invalid",
                         PageAllocationLifecycleStateName(allocation->state));
  }
  if (request.cleanup_horizon_local_transaction_id == 0) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-CLEANUP-HORIZON-REQUIRED",
                         "storage.page_allocation.cleanup_horizon_required");
  }

  const PageAllocationEntry before = *allocation;
  allocation->state = PageAllocationLifecycleState::reusable_pending_mga;
  allocation->reusable_after_local_transaction_id = allocation->creator_local_transaction_id;

  PageAllocationMutationResult result;
  result.status = AllocationOkStatus();
  result.changed = true;
  result.allocation = *allocation;
  result.evidence = MakeEvidence(ledger,
                                 before,
                                 *allocation,
                                 "mark_reusable_pending_mga",
                                 request.reason.empty() ? "release_waits_for_mga_cleanup_horizon" : request.reason,
                                 "SB-STORAGE-PAGE-ALLOCATION-REUSABLE-PENDING-MGA");
  return result;
}

PageAllocationMutationResult ReclaimReusablePageAllocation(PageAllocationLedger* ledger,
                                                           const PageReleaseRequest& request) {
  if (ledger == nullptr) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-LEDGER-REQUIRED",
                         "storage.page_allocation.ledger_required");
  }
  if (!request.engine_mga_authoritative) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-MGA-AUTHORITY-REQUIRED",
                         "storage.page_allocation.mga_authority_required");
  }
  auto* allocation = FindMutable(ledger, request.allocation_uuid);
  if (allocation == nullptr) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-NOT-FOUND",
                         "storage.page_allocation.not_found");
  }
  if (allocation->state != PageAllocationLifecycleState::reusable_pending_mga) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-STATE-INVALID",
                         "storage.page_allocation.state_invalid",
                         PageAllocationLifecycleStateName(allocation->state));
  }
  if (request.cleanup_horizon_local_transaction_id <= allocation->creator_local_transaction_id) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-BLOCKED-BY-MGA-HORIZON",
                         "storage.page_allocation.blocked_by_mga_horizon",
                         std::to_string(allocation->creator_local_transaction_id));
  }

  const PageAllocationEntry before = *allocation;
  allocation->state = PageAllocationLifecycleState::reusable_free;
  AddFreeExtent(ledger, allocation->start_page, allocation->page_count);

  PageAllocationMutationResult result;
  result.status = AllocationOkStatus();
  result.changed = true;
  result.allocation = *allocation;
  result.evidence = MakeEvidence(ledger,
                                 before,
                                 *allocation,
                                 "reclaim_to_free_map",
                                 request.reason.empty() ? "mga_cleanup_horizon_authorized_reuse" : request.reason,
                                 "SB-STORAGE-PAGE-ALLOCATION-RECLAIMED");
  return result;
}

PageAllocationMutationResult CompactPageFreeSpace(PageAllocationLedger* ledger,
                                                  const PageCompactionRequest& request) {
  if (ledger == nullptr) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-LEDGER-REQUIRED",
                         "storage.page_allocation.ledger_required");
  }
  if (!request.engine_authoritative || !request.shutdown_or_maintenance_fenced) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-COMPACTION-FENCE-REQUIRED",
                         "storage.page_allocation.compaction_fence_required");
  }
  if (HasPendingMgaReusablePages(*ledger)) {
    return MutationError("SB-STORAGE-PAGE-ALLOCATION-COMPACTION-BLOCKED-BY-MGA",
                         "storage.page_allocation.compaction_blocked_by_mga");
  }
  const auto before_count = ledger->free_extents.size();
  AddFreeExtent(ledger, 0, 0);

  PageAllocationEntry before;
  PageAllocationEntry after;
  after.database_uuid = ledger->database_uuid;
  after.filespace_uuid = ledger->filespace_uuid;
  after.state = PageAllocationLifecycleState::compacting;
  PageAllocationMutationResult result;
  result.status = AllocationOkStatus();
  result.changed = ledger->free_extents.size() != before_count;
  result.allocation = after;
  result.evidence = MakeEvidence(ledger,
                                 before,
                                 after,
                                 "compact_free_space",
                                 request.reason.empty() ? "free_space_compacted_under_fence" : request.reason,
                                 "SB-STORAGE-PAGE-ALLOCATION-COMPACTED");
  return result;
}

PageAllocationRecoveryClassification ClassifyPageAllocationForRecovery(
    const PageAllocationEntry& allocation) {
  PageAllocationRecoveryClassification classification;
  classification.allocation_uuid = allocation.allocation_uuid;
  classification.observed_state = allocation.state;
  if (!allocation.durability_fence_satisfied ||
      allocation.published_page_generation == 0 ||
      allocation.durable_page_generation < allocation.published_page_generation) {
    classification.action = PageAllocationRecoveryAction::fail_closed;
    classification.fail_closed = true;
    classification.stable_reason = "allocation references non-durable page generation";
    return classification;
  }
  switch (allocation.state) {
    case PageAllocationLifecycleState::free:
    case PageAllocationLifecycleState::reusable_free:
      classification.action = PageAllocationRecoveryAction::release_to_free_map;
      classification.stable_reason = "free allocation can rebuild free-space map";
      break;
    case PageAllocationLifecycleState::allocated:
      classification.action = PageAllocationRecoveryAction::retain;
      classification.stable_reason = "allocated page ownership must be retained";
      break;
    case PageAllocationLifecycleState::preallocated:
      classification.action = PageAllocationRecoveryAction::retain;
      classification.stable_reason = "preallocated page-family pool must not become unsafe reuse";
      break;
    case PageAllocationLifecycleState::reusable_pending_mga:
      classification.action = PageAllocationRecoveryAction::retain;
      classification.stable_reason = "MGA cleanup horizon has not authorized reuse";
      break;
    case PageAllocationLifecycleState::reserved:
    case PageAllocationLifecycleState::compacting:
      classification.action = PageAllocationRecoveryAction::fail_closed;
      classification.fail_closed = true;
      classification.stable_reason = "incomplete allocation transition requires recovery fence";
      break;
    case PageAllocationLifecycleState::quarantined:
      classification.action = PageAllocationRecoveryAction::quarantine;
      classification.fail_closed = true;
      classification.stable_reason = "quarantined page ownership cannot be reused";
      break;
  }
  return classification;
}

PageAllocationRecoveryResult ClassifyPageAllocationLedgerForRecovery(
    const PageAllocationLedger& ledger) {
  PageAllocationRecoveryResult result;
  result.status = AllocationOkStatus();
  for (const auto& allocation : ledger.allocations) {
    result.classifications.push_back(ClassifyPageAllocationForRecovery(allocation));
  }
  return result;
}

const PageAllocationEntry* FindPageAllocation(const PageAllocationLedger& ledger,
                                              const TypedUuid& allocation_uuid) {
  for (const auto& allocation : ledger.allocations) {
    if (allocation.allocation_uuid.value == allocation_uuid.value) {
      return &allocation;
    }
  }
  return nullptr;
}

DiagnosticRecord MakePageAllocationLifecycleDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page_allocation_lifecycle");
}

}  // namespace scratchbird::storage::page
