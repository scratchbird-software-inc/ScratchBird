// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/page_allocation_manager.hpp"

#include "page_registry.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::storage::page::PageFilespaceAgentRequestPolicy;
using scratchbird::storage::page::PageFilespaceAgentRequestQueue;
using scratchbird::storage::page::PageFilespaceHandoffResult;
using scratchbird::storage::page::PageFilespaceLowReserveEvent;
using scratchbird::storage::page::PageAllocationLedger;
using scratchbird::storage::page::PagePreallocationRequest;

Status PageAllocationOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status PageAllocationRefuseStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool AddWouldOverflow(u64 left, u64 right) {
  return right > 0 && left > (~u64{0} - right);
}

bool IsKnownAllocationPageFamily(const std::string& page_family) {
  return scratchbird::storage::page::IsKnownPageFamilyName(page_family) ||
         page_family == "overflow" ||
         page_family == "toast";
}

bool PolicyAllowsPageFamily(const PageAllocationManagerPolicy& policy,
                            const std::string& page_family) {
  return std::find(policy.allowed_page_families.begin(),
                   policy.allowed_page_families.end(),
                   page_family) != policy.allowed_page_families.end();
}

PageAllocationManagerTickResult Finish(Status status,
                                       PageAllocationManagerDecisionKind decision,
                                       std::string diagnostic_code,
                                       std::string message_key,
                                       std::string detail,
                                       bool fail_closed) {
  PageAllocationManagerTickResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakePageAllocationManagerDiagnostic(status,
                                                         std::move(diagnostic_code),
                                                         std::move(message_key),
                                                         std::move(detail));
  return result;
}

PageAllocationManagerTickResult FailClosed(std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail) {
  return Finish(PageAllocationRefuseStatus(),
                PageAllocationManagerDecisionKind::refused,
                std::move(diagnostic_code),
                std::move(message_key),
                std::move(detail),
                true);
}

bool PolicyUuidScopeMatches(const PageAllocationManagerMetricSnapshot& snapshot,
                            const PageAllocationManagerPolicy& policy) {
  return SameTypedUuid(snapshot.policy_uuid, policy.policy_uuid) &&
         SameTypedUuid(snapshot.database_uuid, policy.database_uuid) &&
         SameTypedUuid(snapshot.filespace_uuid, policy.filespace_uuid);
}

bool PolicyShapeValid(const PageAllocationManagerPolicy& policy) {
  if (!policy.valid || !policy.database_uuid.valid() || !policy.filespace_uuid.valid() ||
      !policy.policy_uuid.valid()) {
    return false;
  }
  if (policy.target_free_pages == 0 ||
      policy.target_free_pages < policy.minimum_free_pages ||
      policy.low_water_notify_ratio < 0.0 ||
      policy.low_water_notify_ratio > 1.0 ||
      policy.allowed_page_families.empty()) {
    return false;
  }
  for (const auto& family : policy.allowed_page_families) {
    if (!IsKnownAllocationPageFamily(family)) {
      return false;
    }
  }
  return true;
}

u64 PageLedgerFreeExtentPages(const PageAllocationLedger& ledger) {
  u64 total = 0;
  for (const auto& extent : ledger.free_extents) {
    if (AddWouldOverflow(total, extent.page_count)) {
      return ~u64{0};
    }
    total += extent.page_count;
  }
  return total;
}

PageFilespaceLowReserveEvent LowReserveEventFromSnapshot(
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy,
    u64 released_free_pages) {
  PageFilespaceLowReserveEvent event;
  event.database_uuid = snapshot.database_uuid;
  event.filespace_uuid = snapshot.filespace_uuid;
  event.page_family = snapshot.page_family;
  event.released_free_pages = released_free_pages;
  event.allocated_pages = snapshot.allocated_pages;
  event.reserved_pages = snapshot.reserved_pages;
  event.policy.target_reserve_pages = policy.target_free_pages;
  event.policy.min_target_reserve_pages = policy.minimum_free_pages;
  event.policy.max_target_reserve_pages = std::max(policy.minimum_free_pages,
                                                   policy.target_free_pages);
  event.policy.advisory_events = false;
  event.policy.policy_uuid = policy.policy_uuid;
  event.reason = "page allocation manager low reserve capacity request";
  return event;
}

PageFilespaceAgentRequestPolicy HandoffPolicyFromPagePolicy(
    const PageAllocationManagerPolicy& policy) {
  PageFilespaceAgentRequestPolicy handoff_policy;
  handoff_policy.min_free_pages = policy.minimum_free_pages;
  handoff_policy.target_free_pages = policy.target_free_pages;
  handoff_policy.low_water_ratio = policy.low_water_notify_ratio;
  handoff_policy.filespace_agent_may_grow_files = true;
  handoff_policy.filespace_agent_may_shrink_files = false;
  handoff_policy.page_agent_may_allocate_pages = false;
  handoff_policy.page_agent_may_relocate_pages = false;
  handoff_policy.policy_uuid = policy.policy_uuid;
  return handoff_policy;
}

scratchbird::storage::page::PageFilespaceAgentRequest DeficitCapacityRequestFromSnapshot(
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy,
    u64 released_free_pages,
    u64 low_water_pages) {
  scratchbird::storage::page::PageFilespaceAgentRequest request;
  request.database_uuid = snapshot.database_uuid;
  request.filespace_uuid = snapshot.filespace_uuid;
  request.policy_uuid = policy.policy_uuid;
  request.kind = scratchbird::storage::page::PageFilespaceAgentRequestKind::extend_filespace;
  request.requesting_agent = "page_allocation_manager";
  request.responding_agent = "filespace_capacity_manager";
  request.page_family = snapshot.page_family;
  request.requested_pages = snapshot.target_free_deficit_pages;
  request.released_free_pages = released_free_pages;
  request.target_reserve_pages = policy.target_free_pages;
  request.threshold_pages = low_water_pages;
  request.free_pages = snapshot.free_pages;
  request.preallocated_pages = snapshot.preallocated_pages;
  request.allocated_pages = snapshot.allocated_pages;
  request.reserved_pages = snapshot.reserved_pages;
  request.reason = "page allocation manager target free deficit capacity request";
  return request;
}

PageAllocationManagerTickResult CapacityRequestResult(
    PageFilespaceAgentRequestQueue* queue,
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy,
    u64 released_free_pages,
    u64 low_water_pages) {
  if (!policy.capacity_request_allowed || !policy.capacity_request_policy_explicit) {
    auto result = FailClosed("PAGE_AGENT.PERMISSION_DENIED",
                             "agents.page_allocation.capacity_request_denied",
                             "capacity request policy is not explicitly enabled");
    result.released_free_pages = released_free_pages;
    result.low_water_pages = low_water_pages;
    return result;
  }
  if (queue == nullptr) {
    auto result = FailClosed("PAGE_AGENT.FILESPACE_CAPACITY_REQUIRED",
                             "agents.page_allocation.capacity_queue_required",
                             "low reserve requires durable page/filespace request queue");
    result.released_free_pages = released_free_pages;
    result.low_water_pages = low_water_pages;
    return result;
  }

  const std::size_t queue_size_before = queue->records.size();
  PageFilespaceHandoffResult handoff = scratchbird::storage::page::NotifyFilespaceLowReserve(
      queue,
      LowReserveEventFromSnapshot(snapshot, policy, released_free_pages),
      HandoffPolicyFromPagePolicy(policy));

  PageAllocationManagerTickResult result;
  result.status = handoff.status;
  result.handoff = handoff;
  result.released_free_pages = released_free_pages;
  result.low_water_pages = low_water_pages;
  result.requested_pages = handoff.evidence.requested_pages;
  result.queue_mutated = queue->records.size() != queue_size_before;
  result.capacity_request_enqueued = handoff.ok() &&
                                     handoff.accepted &&
                                     handoff.request_uuid.valid();
  result.accepted_evidence = result.capacity_request_enqueued &&
                             handoff.evidence.evidence_id.valid() &&
                             handoff.evidence.durable_state_changed;
  result.fail_closed = !handoff.ok();
  result.decision = result.capacity_request_enqueued
                        ? PageAllocationManagerDecisionKind::capacity_request_queued
                        : PageAllocationManagerDecisionKind::capacity_request_refused;
  result.diagnostic = result.capacity_request_enqueued
                          ? MakePageAllocationManagerDiagnostic(
                                result.status,
                                "PAGE_AGENT.FILESPACE_CAPACITY_REQUIRED",
                                "agents.page_allocation.capacity_request_queued",
                                "low reserve capacity request queued for filespace manager")
                          : handoff.diagnostic;
  return result;
}

PageAllocationManagerTickResult DeficitCapacityRequestResult(
    PageFilespaceAgentRequestQueue* queue,
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy,
    u64 released_free_pages,
    u64 low_water_pages) {
  if (!policy.capacity_request_allowed || !policy.capacity_request_policy_explicit) {
    auto result = FailClosed("PAGE_AGENT.PERMISSION_DENIED",
                             "agents.page_allocation.capacity_request_denied",
                             "capacity request policy is not explicitly enabled");
    result.released_free_pages = released_free_pages;
    result.low_water_pages = low_water_pages;
    result.requested_pages = snapshot.target_free_deficit_pages;
    return result;
  }
  if (queue == nullptr) {
    auto result = FailClosed("PAGE_AGENT.FILESPACE_CAPACITY_REQUIRED",
                             "agents.page_allocation.capacity_queue_required",
                             "target free deficit requires durable page/filespace request queue");
    result.released_free_pages = released_free_pages;
    result.low_water_pages = low_water_pages;
    result.requested_pages = snapshot.target_free_deficit_pages;
    return result;
  }

  const std::size_t queue_size_before = queue->records.size();
  const auto queued = scratchbird::storage::page::EnqueuePageFilespaceAgentRequest(
      queue,
      DeficitCapacityRequestFromSnapshot(snapshot, policy, released_free_pages, low_water_pages),
      HandoffPolicyFromPagePolicy(policy));

  PageAllocationManagerTickResult result;
  result.status = queued.status;
  result.released_free_pages = released_free_pages;
  result.low_water_pages = low_water_pages;
  result.requested_pages = snapshot.target_free_deficit_pages;
  result.queue_mutated = queue->records.size() != queue_size_before;
  result.capacity_request_enqueued = queued.ok() &&
                                     queued.record.request.request_uuid.valid() &&
                                     queued.record.explicit_evidence &&
                                     queued.record.evidence_id.valid();
  result.accepted_evidence = result.capacity_request_enqueued;
  result.fail_closed = !queued.ok();
  result.decision = result.capacity_request_enqueued
                        ? PageAllocationManagerDecisionKind::capacity_request_queued
                        : PageAllocationManagerDecisionKind::capacity_request_refused;
  result.diagnostic = result.capacity_request_enqueued
                          ? MakePageAllocationManagerDiagnostic(
                                result.status,
                                "PAGE_AGENT.FILESPACE_CAPACITY_REQUIRED",
                                "agents.page_allocation.capacity_deficit_request_queued",
                                "target free deficit capacity request queued for filespace manager")
                          : queued.diagnostic;
  return result;
}

PageAllocationManagerTickResult PreallocationSuppressedResult(
    const PageAllocationManagerMetricSnapshot& snapshot,
    u64 released_free_pages,
    u64 low_water_pages,
    u64 deficit_pages) {
  auto result = Finish(PageAllocationOkStatus(),
                       PageAllocationManagerDecisionKind::preallocation_suppressed,
                       "PAGE_AGENT.PREALLOCATION_SUPPRESSED",
                       "agents.page_allocation.preallocation_suppressed",
                       "live preallocation is disabled by the default page preallocation policy",
                       false);
  result.released_free_pages = released_free_pages;
  result.low_water_pages = low_water_pages;
  result.preallocation_deficit_pages = deficit_pages;
  result.preallocation_recommended = snapshot.preallocation_target_pages > snapshot.preallocated_pages ||
                                     snapshot.preallocation_deficit_pages > 0;
  result.preallocation_suppressed = true;
  return result;
}

PagePreallocationRequest PreallocationRequestFromContext(
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy,
    const PageAllocationManagerActionContext& action_context,
    u64 preallocation_deficit_pages) {
  PagePreallocationRequest request;
  request.database_uuid = snapshot.database_uuid;
  request.filespace_uuid = snapshot.filespace_uuid;
  request.policy_uuid = policy.policy_uuid;
  request.capacity_evidence_uuid = action_context.capacity_evidence_uuid;
  request.creator_transaction_uuid = action_context.transaction_uuid;
  request.creator_local_transaction_id = action_context.local_transaction_id;
  request.page_family = snapshot.page_family;
  request.page_count = preallocation_deficit_pages;
  request.page_generation = action_context.page_generation;
  request.engine_authoritative = action_context.engine_authoritative;
  request.capacity_evidence_accepted = !policy.capacity_evidence_required ||
                                       (action_context.capacity_evidence_present &&
                                        action_context.capacity_evidence_fresh &&
                                        action_context.capacity_evidence_scope_compatible &&
                                        action_context.capacity_evidence_uuid.valid());
  request.durability_fence_satisfied = action_context.durability_fence_satisfied;
  request.cluster_route_requested = action_context.cluster_route_requested;
  return request;
}

PageAllocationManagerTickResult DirectPreallocationResult(
    PageAllocationLedger* page_ledger,
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy,
    const PageAllocationManagerActionContext& action_context,
    u64 released_free_pages,
    u64 low_water_pages,
    u64 preallocation_deficit_pages) {
  const std::size_t allocations_before =
      page_ledger == nullptr ? 0 : page_ledger->allocations.size();
  const std::size_t evidence_before =
      page_ledger == nullptr ? 0 : page_ledger->evidence.size();
  const u64 free_pages_before =
      page_ledger == nullptr ? 0 : PageLedgerFreeExtentPages(*page_ledger);
  const auto preallocation = scratchbird::storage::page::PreallocatePageFamilyPool(
      page_ledger,
      PreallocationRequestFromContext(snapshot, policy, action_context, preallocation_deficit_pages));

  PageAllocationManagerTickResult result;
  result.status = preallocation.status;
  result.released_free_pages = released_free_pages;
  result.low_water_pages = low_water_pages;
  result.requested_pages = preallocation_deficit_pages;
  result.preallocation_deficit_pages = preallocation_deficit_pages;
  result.direct_action_attempted = true;
  result.preallocation_recommended = true;
  result.preallocation_evidence = preallocation.evidence;
  result.preallocation_uuid = preallocation.allocation.allocation_uuid;
  result.preallocated_pages = preallocation.ok() ? preallocation.allocation.page_count : 0;
  result.ledger_state_changed =
      page_ledger != nullptr &&
      (page_ledger->allocations.size() != allocations_before ||
       page_ledger->evidence.size() != evidence_before ||
       PageLedgerFreeExtentPages(*page_ledger) != free_pages_before);
  result.accepted_evidence = preallocation.ok() &&
                             preallocation.evidence.durable_state_changed &&
                             preallocation.evidence.durability_fence_satisfied &&
                             preallocation.evidence.new_state ==
                                 scratchbird::storage::page::PageAllocationLifecycleState::preallocated;
  result.fail_closed = !preallocation.ok();
  result.decision = preallocation.ok()
                        ? PageAllocationManagerDecisionKind::preallocation_completed
                        : PageAllocationManagerDecisionKind::refused;
  result.diagnostic = preallocation.ok()
                          ? MakePageAllocationManagerDiagnostic(
                                result.status,
                                "PAGE_AGENT.PREALLOCATION_COMPLETED",
                                "agents.page_allocation.preallocation_completed",
                                "page-family pool preallocated through engine page lifecycle")
                          : preallocation.diagnostic;
  return result;
}

}  // namespace

// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_page_allocation_manager
// Canonical page_allocation_manager behavior is registered in CanonicalAgentRegistry().
// The implementation anchor owns page-family/page-type allocation, preallocation,
// relocation, fragmentation, and shrink-readiness recommendations only; it never expands,
// truncates, deletes, promotes, or demotes filespaces.
const char* page_allocation_manager_implementation_anchor() { return "page_allocation_manager"; }

const char* PageAllocationManagerDecisionKindName(PageAllocationManagerDecisionKind kind) {
  switch (kind) {
    case PageAllocationManagerDecisionKind::no_action:
      return "no_action";
    case PageAllocationManagerDecisionKind::capacity_request_queued:
      return "capacity_request_queued";
    case PageAllocationManagerDecisionKind::capacity_request_refused:
      return "capacity_request_refused";
    case PageAllocationManagerDecisionKind::preallocation_completed:
      return "preallocation_completed";
    case PageAllocationManagerDecisionKind::preallocation_recommended:
      return "preallocation_recommended";
    case PageAllocationManagerDecisionKind::preallocation_suppressed:
      return "preallocation_suppressed";
    case PageAllocationManagerDecisionKind::refused:
      return "refused";
  }
  return "unknown";
}

PageAllocationManagerPolicy DefaultPageAllocationManagerPolicy() {
  return PageAllocationManagerPolicy{};
}

u64 PageAllocationManagerReleasedFreePages(
    const PageAllocationManagerMetricSnapshot& snapshot) {
  if (AddWouldOverflow(snapshot.free_pages, snapshot.released_pages)) {
    return ~u64{0};
  }
  return snapshot.free_pages + snapshot.released_pages;
}

u64 PageAllocationManagerLowWaterPages(const PageAllocationManagerPolicy& policy) {
  if (policy.target_free_pages == 0 || policy.low_water_notify_ratio <= 0.0) {
    return 0;
  }
  const double threshold = std::ceil(static_cast<double>(policy.target_free_pages) *
                                     policy.low_water_notify_ratio);
  if (threshold <= 0.0) {
    return 0;
  }
  return static_cast<u64>(threshold);
}

PageAllocationManagerTickResult EvaluatePageAllocationManagerTick(
    PageFilespaceAgentRequestQueue* queue,
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy) {
  return EvaluatePageAllocationManagerTick(queue,
                                           nullptr,
                                           snapshot,
                                           policy,
                                           PageAllocationManagerActionContext{});
}

PageAllocationManagerTickResult EvaluatePageAllocationManagerTick(
    PageFilespaceAgentRequestQueue* queue,
    PageAllocationLedger* page_ledger,
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy,
    const PageAllocationManagerActionContext& action_context) {
  const u64 released_free_pages = PageAllocationManagerReleasedFreePages(snapshot);
  const u64 low_water_pages = PageAllocationManagerLowWaterPages(policy);

  auto fail_with_counts = [&](std::string code,
                              std::string key,
                              std::string detail) {
    auto result = FailClosed(std::move(code), std::move(key), std::move(detail));
    result.released_free_pages = released_free_pages;
    result.low_water_pages = low_water_pages;
    result.preallocation_deficit_pages = snapshot.preallocation_deficit_pages;
    return result;
  };

  if (!snapshot.database_uuid.valid() || !snapshot.filespace_uuid.valid() ||
      !snapshot.policy_uuid.valid()) {
    return fail_with_counts("PAGE_AGENT.POLICY_SCOPE_INCOMPATIBLE",
                            "agents.page_allocation.scope_identity_invalid",
                            "database_uuid, filespace_uuid, and policy_uuid are required");
  }
  if (!snapshot.metrics_present) {
    return fail_with_counts("PAGE_AGENT.METRIC_MISSING",
                            "agents.page_allocation.metric_missing",
                            "required page allocation metric snapshot is missing");
  }
  if (!snapshot.metrics_fresh) {
    return fail_with_counts("PAGE_AGENT.METRIC_STALE",
                            "agents.page_allocation.metric_stale",
                            "required page allocation metrics are stale");
  }
  if (!snapshot.metrics_trusted) {
    return fail_with_counts("PAGE_AGENT.METRIC_UNTRUSTED",
                            "agents.page_allocation.metric_untrusted",
                            "required page allocation metrics are not trusted");
  }
  if (!snapshot.scope_compatible || !policy.scope_compatible) {
    return fail_with_counts("PAGE_AGENT.POLICY_SCOPE_INCOMPATIBLE",
                            "agents.page_allocation.scope_incompatible",
                            "metric snapshot and policy scope are incompatible");
  }
  if (!PolicyShapeValid(policy)) {
    return fail_with_counts("PAGE_AGENT.POLICY_INVALID",
                            "agents.page_allocation.policy_invalid",
                            "page allocation policy is invalid");
  }
  if (!PolicyUuidScopeMatches(snapshot, policy)) {
    return fail_with_counts("PAGE_AGENT.POLICY_SCOPE_INCOMPATIBLE",
                            "agents.page_allocation.policy_scope_incompatible",
                            "policy UUID or filespace scope does not match metric snapshot");
  }
  if (!IsKnownAllocationPageFamily(snapshot.page_family)) {
    return fail_with_counts("PAGE_AGENT.UNKNOWN_PAGE_FAMILY",
                            "agents.page_allocation.unknown_page_family",
                            snapshot.page_family);
  }
  if (!PolicyAllowsPageFamily(policy, snapshot.page_family)) {
    return fail_with_counts("PAGE_AGENT.POLICY_SCOPE_INCOMPATIBLE",
                            "agents.page_allocation.page_family_scope_incompatible",
                            "page family is not enabled by the active policy");
  }
  if (snapshot.allocation_failure_signal || snapshot.allocation_failures_total > 0) {
    return fail_with_counts("PAGE_AGENT.ALLOCATION_FAILURE",
                            "agents.page_allocation.allocation_failure",
                            "allocator failure signal prevents live capacity or preallocation action");
  }

  if (released_free_pages <= low_water_pages) {
    return CapacityRequestResult(queue,
                                 snapshot,
                                 policy,
                                 released_free_pages,
                                 low_water_pages);
  }
  if (snapshot.target_free_deficit_pages > 0) {
    return DeficitCapacityRequestResult(queue,
                                        snapshot,
                                        policy,
                                        released_free_pages,
                                        low_water_pages);
  }

  const u64 computed_preallocation_deficit =
      snapshot.preallocation_target_pages > snapshot.preallocated_pages
          ? snapshot.preallocation_target_pages - snapshot.preallocated_pages
          : 0;
  const u64 preallocation_deficit_pages =
      std::max(snapshot.preallocation_deficit_pages, computed_preallocation_deficit);
  if (preallocation_deficit_pages > 0) {
    if (!policy.live_preallocation_allowed) {
      if (policy.live_preallocation_policy_explicit) {
        auto result = fail_with_counts("PAGE_AGENT.PREALLOCATION_POLICY_DISABLED",
                                       "agents.page_allocation.preallocation_policy_disabled",
                                       "explicit page preallocation policy disables live preallocation");
        result.requested_pages = preallocation_deficit_pages;
        result.preallocation_deficit_pages = preallocation_deficit_pages;
        return result;
      }
      return PreallocationSuppressedResult(snapshot,
                                          released_free_pages,
                                          low_water_pages,
                                          preallocation_deficit_pages);
    }
    auto fail_preallocation = [&](std::string code,
                                  std::string key,
                                  std::string detail) {
      auto result = fail_with_counts(std::move(code), std::move(key), std::move(detail));
      result.requested_pages = preallocation_deficit_pages;
      result.preallocation_deficit_pages = preallocation_deficit_pages;
      return result;
    };
    if (!policy.live_preallocation_policy_explicit) {
      return fail_preallocation("PAGE_AGENT.PERMISSION_DENIED",
                                "agents.page_allocation.preallocation_policy_not_explicit",
                                "live preallocation requires an explicit page_preallocation_policy");
    }
    if (page_ledger == nullptr || !action_context.present) {
      return fail_preallocation("PAGE_AGENT.ACTION_CONTEXT_REQUIRED",
                                "agents.page_allocation.action_context_required",
                                "live preallocation requires page ledger and action context");
    }
    if (!action_context.engine_authoritative) {
      return fail_preallocation("PAGE_AGENT.ACTION_CONTEXT_NOT_ENGINE_AUTHORITY",
                                "agents.page_allocation.action_context_not_engine_authority",
                                "live preallocation requires engine-owned action authority");
    }
    if (action_context.cluster_route_requested) {
      return fail_preallocation("PAGE_AGENT.CLUSTER_ROUTE_UNAVAILABLE",
                                "agents.page_allocation.cluster_route_unavailable",
                                "cluster-routed page preallocation is not available in this path");
    }
    if (!action_context.transaction_uuid.valid() ||
        action_context.transaction_uuid.kind !=
            scratchbird::core::platform::UuidKind::transaction ||
        action_context.local_transaction_id == 0) {
      return fail_preallocation("PAGE_AGENT.TRANSACTION_ID_INVALID",
                                "agents.page_allocation.transaction_id_invalid",
                                "engine transaction UUID and local transaction id are required");
    }
    if (action_context.page_generation == 0 ||
        !action_context.durability_fence_satisfied) {
      return fail_preallocation("PAGE_AGENT.DURABILITY_FENCE_REQUIRED",
                                "agents.page_allocation.durability_fence_required",
                                "page generation and durability fence are required before preallocation success");
    }
    if (policy.capacity_evidence_required) {
      if (!action_context.capacity_evidence_present ||
          !action_context.capacity_evidence_fresh ||
          !action_context.capacity_evidence_scope_compatible ||
          !action_context.capacity_evidence_uuid.valid()) {
        return fail_preallocation("PAGE_AGENT.CAPACITY_EVIDENCE_REQUIRED",
                                  "agents.page_allocation.capacity_evidence_required",
                                  "fresh scope-compatible filespace capacity evidence is required");
      }
      if (action_context.capacity_evidence_free_pages < preallocation_deficit_pages) {
        return fail_preallocation("PAGE_AGENT.FILESPACE_CAPACITY_REQUIRED",
                                  "agents.page_allocation.capacity_evidence_insufficient",
                                  "capacity evidence does not cover requested preallocation pages");
      }
    }
    if (PageLedgerFreeExtentPages(*page_ledger) < preallocation_deficit_pages) {
      return fail_preallocation("PAGE_AGENT.FILESPACE_CAPACITY_REQUIRED",
                                "agents.page_allocation.free_extent_capacity_insufficient",
                                "page ledger free extents cannot satisfy requested preallocation pages");
    }
    return DirectPreallocationResult(page_ledger,
                                     snapshot,
                                     policy,
                                     action_context,
                                     released_free_pages,
                                     low_water_pages,
                                     preallocation_deficit_pages);
  }

  auto result = Finish(PageAllocationOkStatus(),
                       PageAllocationManagerDecisionKind::no_action,
                       "PAGE_AGENT.NO_ACTION",
                       "agents.page_allocation.no_action",
                       "page allocation metrics are healthy",
                       false);
  result.released_free_pages = released_free_pages;
  result.low_water_pages = low_water_pages;
  result.preallocation_deficit_pages = preallocation_deficit_pages;
  return result;
}

DiagnosticRecord MakePageAllocationManagerDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.agents.page_allocation_manager",
                        status.ok() ? std::string{}
                                    : "refresh trusted page metrics and attach a valid explicit policy");
}

StorageSpaceAgentDefaults page_allocation_manager_default_space_policy() {
  return DefaultStorageSpaceAgentDefaults();
}

u64 page_allocation_manager_released_free_notify_threshold_pages() {
  return DefaultStorageSpaceAgentDefaults().page_allocation_notify_released_free_pages;
}

bool page_allocation_manager_should_notify_filespace_manager(u64 released_free_pages) {
  const auto defaults = DefaultStorageSpaceAgentDefaults();
  return released_free_pages <= defaults.page_allocation_notify_released_free_pages;
}

}  // namespace scratchbird::core::agents::implemented_agents
