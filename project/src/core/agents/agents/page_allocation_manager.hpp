// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"
#include "page_allocation_lifecycle.hpp"
#include "page_filespace_handoff.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class PageAllocationManagerDecisionKind : u32 {
  no_action,
  capacity_request_queued,
  capacity_request_refused,
  preallocation_completed,
  preallocation_recommended,
  preallocation_suppressed,
  refused
};

struct PageAllocationManagerMetricSnapshot {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  std::string page_family;
  u64 free_pages = 0;
  u64 released_pages = 0;
  u64 reserved_pages = 0;
  u64 preallocated_pages = 0;
  u64 allocated_pages = 0;
  u64 target_free_deficit_pages = 0;
  u64 preallocation_target_pages = 0;
  u64 preallocation_deficit_pages = 0;
  bool metrics_present = true;
  bool metrics_fresh = true;
  bool metrics_trusted = true;
  bool scope_compatible = true;
  bool allocation_failure_signal = false;
  u64 allocation_failures_total = 0;
};

struct PageAllocationManagerPolicy {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  bool valid = true;
  bool scope_compatible = true;
  bool capacity_request_allowed = false;
  bool capacity_request_policy_explicit = false;
  bool live_preallocation_allowed = false;
  bool live_preallocation_policy_explicit = false;
  u64 minimum_free_pages = 4;
  u64 target_free_pages = 8;
  double low_water_notify_ratio = 0.50;
  bool capacity_evidence_required = true;
  std::vector<std::string> allowed_page_families = {"data", "index", "blob", "metrics"};
};

struct PageAllocationManagerActionContext {
  bool present = false;
  bool engine_authoritative = false;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  u64 page_generation = 0;
  bool durability_fence_satisfied = false;
  bool capacity_evidence_present = false;
  bool capacity_evidence_fresh = false;
  bool capacity_evidence_scope_compatible = false;
  TypedUuid capacity_evidence_uuid;
  u64 capacity_evidence_free_pages = 0;
  bool cluster_route_requested = false;
};

struct PageAllocationManagerTickResult {
  Status status;
  PageAllocationManagerDecisionKind decision = PageAllocationManagerDecisionKind::refused;
  DiagnosticRecord diagnostic;
  scratchbird::storage::page::PageFilespaceHandoffResult handoff;
  scratchbird::storage::page::PageAllocationEvidenceRecord preallocation_evidence;
  u64 released_free_pages = 0;
  u64 low_water_pages = 0;
  u64 requested_pages = 0;
  u64 preallocated_pages = 0;
  u64 preallocation_deficit_pages = 0;
  TypedUuid preallocation_uuid;
  bool fail_closed = false;
  bool accepted_evidence = false;
  bool capacity_request_enqueued = false;
  bool queue_mutated = false;
  bool ledger_state_changed = false;
  bool preallocation_recommended = false;
  bool preallocation_suppressed = false;
  bool direct_action_attempted = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* PageAllocationManagerDecisionKindName(PageAllocationManagerDecisionKind kind);
PageAllocationManagerPolicy DefaultPageAllocationManagerPolicy();
u64 PageAllocationManagerReleasedFreePages(const PageAllocationManagerMetricSnapshot& snapshot);
u64 PageAllocationManagerLowWaterPages(const PageAllocationManagerPolicy& policy);
PageAllocationManagerTickResult EvaluatePageAllocationManagerTick(
    scratchbird::storage::page::PageFilespaceAgentRequestQueue* queue,
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy);
PageAllocationManagerTickResult EvaluatePageAllocationManagerTick(
    scratchbird::storage::page::PageFilespaceAgentRequestQueue* queue,
    scratchbird::storage::page::PageAllocationLedger* page_ledger,
    const PageAllocationManagerMetricSnapshot& snapshot,
    const PageAllocationManagerPolicy& policy,
    const PageAllocationManagerActionContext& action_context);
DiagnosticRecord MakePageAllocationManagerDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {});

const char* page_allocation_manager_implementation_anchor();
StorageSpaceAgentDefaults page_allocation_manager_default_space_policy();
u64 page_allocation_manager_released_free_notify_threshold_pages();
bool page_allocation_manager_should_notify_filespace_manager(u64 released_free_pages);

}  // namespace scratchbird::core::agents::implemented_agents
