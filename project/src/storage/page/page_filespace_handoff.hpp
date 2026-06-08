// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PAGE-FILESPACE-HANDOFF-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class PageFilespaceHandoffKind : u32 {
  low_reserve,
  shrink_ready,
  shrink_blocked
};

enum class PageFilespaceHandoffState : u32 {
  absent,
  advisory_recorded,
  durable_recorded,
  refused
};

enum class PageFilespaceHandoffRecoveryAction : u32 {
  no_action,
  replay_advisory,
  reconcile_durable,
  fail_closed
};

enum class PageFilespaceAgentRequestKind : u32 {
  reserve_pages,
  extend_filespace,
  shrink_candidate,
  truncate_filespace,
  relocate_pages,
  release_pages,
  deny_request
};

enum class PageFilespaceAgentRequestState : u32 {
  created,
  validated,
  refused,
  waiting_filespace_agent,
  waiting_page_agent,
  approved,
  in_flight,
  completed,
  cancelled,
  recovery_required
};

enum class PageFilespaceAgentBoundaryViolation : u32 {
  none,
  filespace_agent_manages_pages,
  page_agent_manages_files,
  missing_agent_identity,
  missing_policy,
  missing_evidence,
  invalid_filespace_identity,
  invalid_page_family
};

struct PageFilespaceReservePolicy {
  u64 target_reserve_pages = 8;
  u64 min_target_reserve_pages = 4;
  u64 max_target_reserve_pages = 8;
  bool advisory_events = true;
  TypedUuid policy_uuid;
};

struct PageFilespaceLowReserveEvent {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string page_family;
  u64 released_free_pages = 0;
  u64 allocated_pages = 0;
  u64 reserved_pages = 0;
  PageFilespaceReservePolicy policy;
  std::string reason;
};

struct PageFilespaceShrinkReadyEvent {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string page_family;
  u64 relocated_pages = 0;
  u64 pinned_pages = 0;
  u64 active_pages = 0;
  u64 reserved_pages = 0;
  TypedUuid policy_uuid;
  std::string reason;
};

struct PageFilespaceHandoffEvidenceRecord {
  u64 sequence = 0;
  PageFilespaceHandoffKind kind = PageFilespaceHandoffKind::low_reserve;
  PageFilespaceHandoffState previous_state = PageFilespaceHandoffState::absent;
  PageFilespaceHandoffState new_state = PageFilespaceHandoffState::absent;
  TypedUuid request_uuid;
  TypedUuid evidence_id;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  PageFilespaceAgentRequestKind request_kind = PageFilespaceAgentRequestKind::reserve_pages;
  std::string page_family;
  u64 requested_pages = 0;
  u64 released_free_pages = 0;
  u64 target_reserve_pages = 0;
  u64 threshold_pages = 0;
  u64 allocated_pages = 0;
  u64 reserved_pages = 0;
  u64 relocated_pages = 0;
  u64 pinned_pages = 0;
  u64 active_pages = 0;
  std::string reason;
  std::string diagnostic_code;
  bool advisory = true;
  bool durable_state_changed = false;
};

struct PageFilespaceAgentRequestPolicy {
  u64 min_free_pages = 4;
  u64 target_free_pages = 8;
  double low_water_ratio = 0.50;
  bool filespace_agent_may_grow_files = true;
  bool filespace_agent_may_shrink_files = false;
  bool page_agent_may_allocate_pages = true;
  bool page_agent_may_relocate_pages = true;
  TypedUuid policy_uuid;
};

struct PageFilespaceAgentRequest {
  TypedUuid request_uuid;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  PageFilespaceAgentRequestKind kind = PageFilespaceAgentRequestKind::reserve_pages;
  PageFilespaceAgentRequestState state = PageFilespaceAgentRequestState::created;
  std::string requesting_agent;
  std::string responding_agent;
  std::string page_family;
  u64 requested_pages = 0;
  u64 released_free_pages = 0;
  u64 target_reserve_pages = 0;
  u64 threshold_pages = 0;
  u64 free_pages = 0;
  u64 preallocated_pages = 0;
  u64 allocated_pages = 0;
  u64 reserved_pages = 0;
  u64 relocated_pages = 0;
  u64 pinned_pages = 0;
  u64 active_pages = 0;
  std::string reason;
};

struct PageFilespaceAgentDecision {
  Status status;
  PageFilespaceAgentRequestState state = PageFilespaceAgentRequestState::refused;
  PageFilespaceAgentBoundaryViolation violation = PageFilespaceAgentBoundaryViolation::none;
  bool allowed = false;
  bool filespace_agent_action_required = false;
  bool page_agent_action_required = false;
  u64 target_free_pages = 8;
  u64 low_water_pages = 4;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && allowed; }
};

enum class PageFilespaceAgentRequestRecoveryAction : u32 {
  no_action,
  resume_revalidate,
  fail_closed
};

struct PageFilespaceAgentTransitionRecord {
  u64 sequence = 0;
  PageFilespaceAgentRequestState previous_state = PageFilespaceAgentRequestState::created;
  PageFilespaceAgentRequestState new_state = PageFilespaceAgentRequestState::created;
  TypedUuid evidence_id;
  std::string diagnostic_code;
  std::string reason;
  bool explicit_evidence = false;
};

struct PageFilespaceAgentQueueRecord {
  u64 sequence = 0;
  PageFilespaceAgentRequest request;
  PageFilespaceAgentBoundaryViolation violation = PageFilespaceAgentBoundaryViolation::none;
  bool allowed = false;
  bool filespace_agent_action_required = false;
  bool page_agent_action_required = false;
  u64 target_free_pages = 8;
  u64 low_water_pages = 4;
  std::string diagnostic_code;
  std::string evidence_state;
  TypedUuid evidence_id;
  bool explicit_evidence = false;
  std::vector<PageFilespaceAgentTransitionRecord> transitions;
};

struct PageFilespaceAgentRequestQueue {
  std::vector<PageFilespaceAgentQueueRecord> records;
  u64 next_sequence = 1;
};

struct PageFilespaceAgentQueueResult {
  Status status;
  PageFilespaceAgentDecision decision;
  PageFilespaceAgentQueueRecord record;
  TypedUuid request_uuid;
  bool enqueued = false;
  bool idempotent = false;
  bool transitioned = false;
  bool cancelled = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct PageFilespaceAgentRequestRecoveryClassification {
  TypedUuid request_uuid;
  u64 sequence = 0;
  PageFilespaceAgentRequestState observed_state = PageFilespaceAgentRequestState::created;
  PageFilespaceAgentRequestRecoveryAction action = PageFilespaceAgentRequestRecoveryAction::fail_closed;
  bool fail_closed = true;
  std::string stable_reason;
  std::string diagnostic_code;
};

struct PageFilespaceAgentRequestRecoveryResult {
  Status status;
  std::vector<PageFilespaceAgentRequestRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct PageFilespaceAgentQueueRestoreResult {
  Status status;
  PageFilespaceAgentRequestQueue queue;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct PageFilespaceHandoffLedger {
  std::vector<PageFilespaceHandoffEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
};

struct PageFilespaceHandoffResult {
  Status status;
  bool notified = false;
  bool accepted = false;
  bool shrink_ready = false;
  bool advisory = true;
  bool queue_backed = false;
  PageFilespaceHandoffEvidenceRecord evidence;
  PageFilespaceAgentQueueRecord queue_record;
  TypedUuid request_uuid;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && notified; }
};

struct PageFilespaceHandoffRecoveryClassification {
  TypedUuid evidence_id;
  PageFilespaceHandoffKind kind = PageFilespaceHandoffKind::low_reserve;
  PageFilespaceHandoffState observed_state = PageFilespaceHandoffState::absent;
  PageFilespaceHandoffRecoveryAction action = PageFilespaceHandoffRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct PageFilespaceHandoffRecoveryResult {
  Status status;
  std::vector<PageFilespaceHandoffRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* PageFilespaceHandoffKindName(PageFilespaceHandoffKind kind);
const char* PageFilespaceHandoffStateName(PageFilespaceHandoffState state);
const char* PageFilespaceHandoffRecoveryActionName(PageFilespaceHandoffRecoveryAction action);
const char* PageFilespaceAgentRequestKindName(PageFilespaceAgentRequestKind kind);
const char* PageFilespaceAgentRequestStateName(PageFilespaceAgentRequestState state);
const char* PageFilespaceAgentBoundaryViolationName(PageFilespaceAgentBoundaryViolation violation);
const char* PageFilespaceAgentRequestRecoveryActionName(PageFilespaceAgentRequestRecoveryAction action);

u64 NormalizeFilespaceTargetReservePages(const PageFilespaceReservePolicy& policy);
u64 FilespaceLowReserveThresholdPages(const PageFilespaceReservePolicy& policy);
bool ShouldNotifyFilespaceLowReserve(const PageFilespaceLowReserveEvent& event);
u64 NormalizeAgentFilespaceTargetFreePages(const PageFilespaceAgentRequestPolicy& policy);
u64 AgentFilespaceLowWaterPages(const PageFilespaceAgentRequestPolicy& policy);
PageFilespaceAgentDecision EvaluatePageFilespaceAgentRequest(const PageFilespaceAgentRequest& request,
                                                             const PageFilespaceAgentRequestPolicy& policy);
PageFilespaceAgentQueueResult EvaluatePageFilespaceAgentRequest(PageFilespaceAgentRequestQueue* queue,
                                                                const PageFilespaceAgentRequest& request,
                                                                const PageFilespaceAgentRequestPolicy& policy);
PageFilespaceAgentQueueResult EnqueuePageFilespaceAgentRequest(PageFilespaceAgentRequestQueue* queue,
                                                               const PageFilespaceAgentRequest& request,
                                                               const PageFilespaceAgentRequestPolicy& policy);
PageFilespaceAgentQueueResult TransitionPageFilespaceAgentRequest(PageFilespaceAgentRequestQueue* queue,
                                                                  const TypedUuid& request_uuid,
                                                                  PageFilespaceAgentRequestState new_state,
                                                                  std::string reason,
                                                                  TypedUuid evidence_id = {});
PageFilespaceAgentQueueResult TransitionPageFilespaceAgentRequestWithEvidence(
    PageFilespaceAgentRequestQueue* queue,
    const TypedUuid& request_uuid,
    PageFilespaceAgentRequestState new_state,
    std::string reason,
    std::string diagnostic_code,
    std::string evidence_state,
    TypedUuid evidence_id = {});
PageFilespaceAgentQueueResult CancelPageFilespaceAgentRequest(PageFilespaceAgentRequestQueue* queue,
                                                              const TypedUuid& request_uuid,
                                                              std::string reason,
                                                              TypedUuid evidence_id = {});
std::vector<byte> SerializePageFilespaceAgentRequestQueue(const PageFilespaceAgentRequestQueue& queue);
PageFilespaceAgentQueueRestoreResult RestorePageFilespaceAgentRequestQueue(const std::vector<byte>& encoded);
PageFilespaceAgentRequestRecoveryResult ClassifyPageFilespaceAgentRequestQueueForRecovery(
    const PageFilespaceAgentRequestQueue& queue);

PageFilespaceHandoffResult NotifyFilespaceLowReserve(PageFilespaceHandoffLedger* ledger,
                                                     const PageFilespaceLowReserveEvent& event);
PageFilespaceHandoffResult NotifyFilespaceLowReserve(PageFilespaceAgentRequestQueue* queue,
                                                     const PageFilespaceLowReserveEvent& event,
                                                     const PageFilespaceAgentRequestPolicy& policy);
PageFilespaceHandoffResult NotifyFilespaceShrinkReady(PageFilespaceHandoffLedger* ledger,
                                                      const PageFilespaceShrinkReadyEvent& event);
PageFilespaceHandoffResult NotifyFilespaceShrinkReady(PageFilespaceAgentRequestQueue* queue,
                                                      const PageFilespaceShrinkReadyEvent& event,
                                                      const PageFilespaceAgentRequestPolicy& policy);
PageFilespaceHandoffResult NotifyFilespaceShrinkBlocked(PageFilespaceAgentRequestQueue* queue,
                                                        const PageFilespaceShrinkReadyEvent& event,
                                                        const PageFilespaceAgentRequestPolicy& policy);
PageFilespaceHandoffRecoveryResult ClassifyPageFilespaceHandoffLedgerForRecovery(
    const PageFilespaceHandoffLedger& ledger);
DiagnosticRecord MakePageFilespaceHandoffDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail = {});

}  // namespace scratchbird::storage::page
