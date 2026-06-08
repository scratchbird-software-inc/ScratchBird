// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_filespace_handoff.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1830000000000ull + seed);
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

page::PageFilespaceAgentRequestPolicy Policy(platform::u64 seed = 1) {
  page::PageFilespaceAgentRequestPolicy policy;
  policy.policy_uuid = MakeUuid(platform::UuidKind::object, 100 + seed);
  policy.filespace_agent_may_grow_files = true;
  policy.filespace_agent_may_shrink_files = true;
  policy.page_agent_may_allocate_pages = true;
  policy.page_agent_may_relocate_pages = true;
  return policy;
}

page::PageFilespaceAgentRequest BaseRequest(page::PageFilespaceAgentRequestKind kind,
                                            platform::u64 seed) {
  page::PageFilespaceAgentRequest request;
  request.database_uuid = MakeUuid(platform::UuidKind::database, 200 + seed);
  request.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 300 + seed);
  request.policy_uuid = MakeUuid(platform::UuidKind::object, 400 + seed);
  request.kind = kind;
  request.state = page::PageFilespaceAgentRequestState::created;
  request.page_family = "data";
  request.requested_pages = 8 + seed;
  request.released_free_pages = 2;
  request.target_reserve_pages = 8;
  request.threshold_pages = 4;
  request.free_pages = 2;
  request.preallocated_pages = 1;
  request.allocated_pages = 64;
  request.reserved_pages = 3;
  request.relocated_pages = 0;
  request.pinned_pages = 0;
  request.active_pages = 0;
  request.reason = "page_filespace_agent_handoff_gate";
  return request;
}

page::PageFilespaceAgentRequest PageAgentRequest(platform::u64 seed = 1) {
  auto request = BaseRequest(page::PageFilespaceAgentRequestKind::reserve_pages, seed);
  request.requesting_agent = "filespace_capacity_manager";
  request.responding_agent = "page_allocation_manager";
  return request;
}

page::PageFilespaceAgentRequest FilespaceAgentRequest(platform::u64 seed = 2) {
  auto request = BaseRequest(page::PageFilespaceAgentRequestKind::extend_filespace, seed);
  request.requesting_agent = "page_allocation_manager";
  request.responding_agent = "filespace_capacity_manager";
  return request;
}

bool SameUuid(const platform::TypedUuid& left, const platform::TypedUuid& right) {
  return left.kind == right.kind &&
         uuid::UuidToString(left.value) == uuid::UuidToString(right.value);
}

page::PageFilespaceLowReserveEvent LowReserveEvent(platform::u64 seed,
                                                   platform::u64 released_free_pages) {
  page::PageFilespaceLowReserveEvent event;
  event.database_uuid = MakeUuid(platform::UuidKind::database, 1000 + seed);
  event.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 1100 + seed);
  event.page_family = "data";
  event.released_free_pages = released_free_pages;
  event.allocated_pages = 96 + seed;
  event.reserved_pages = 2 + seed;
  event.policy.target_reserve_pages = 12;
  event.policy.min_target_reserve_pages = 8;
  event.policy.max_target_reserve_pages = 16;
  event.policy.advisory_events = false;
  event.policy.policy_uuid = MakeUuid(platform::UuidKind::object, 1200 + seed);
  event.reason = "queue-backed low reserve handoff";
  return event;
}

page::PageFilespaceShrinkReadyEvent ShrinkEvent(platform::u64 seed,
                                                platform::u64 pinned_pages,
                                                platform::u64 active_pages,
                                                platform::u64 reserved_pages) {
  page::PageFilespaceShrinkReadyEvent event;
  event.database_uuid = MakeUuid(platform::UuidKind::database, 1300 + seed);
  event.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 1400 + seed);
  event.page_family = "data";
  event.relocated_pages = 4 + seed;
  event.pinned_pages = pinned_pages;
  event.active_pages = active_pages;
  event.reserved_pages = reserved_pages;
  event.policy_uuid = MakeUuid(platform::UuidKind::object, 1500 + seed);
  event.reason = "queue-backed shrink handoff";
  return event;
}

const page::PageFilespaceAgentRequestRecoveryClassification* FindClassification(
    const page::PageFilespaceAgentRequestRecoveryResult& result,
    page::PageFilespaceAgentRequestState state) {
  for (const auto& classification : result.classifications) {
    if (classification.observed_state == state) {
      return &classification;
    }
  }
  return nullptr;
}

const page::PageFilespaceAgentRequestRecoveryClassification* FindClassificationByRequest(
    const page::PageFilespaceAgentRequestRecoveryResult& result,
    const platform::TypedUuid& request_uuid) {
  for (const auto& classification : result.classifications) {
    if (SameUuid(classification.request_uuid, request_uuid)) {
      return &classification;
    }
  }
  return nullptr;
}

void TestAcceptedPageAndFilespaceRequests() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto policy = Policy();

  const auto page_result =
      page::EvaluatePageFilespaceAgentRequest(&queue, PageAgentRequest(), policy);
  Require(page_result.ok(), "page-agent request was not accepted");
  Require(page_result.record.request.state ==
              page::PageFilespaceAgentRequestState::waiting_page_agent,
          "page-agent request state mismatch");
  Require(page_result.record.page_agent_action_required,
          "page-agent action flag missing");
  Require(page_result.record.explicit_evidence,
          "page-agent request did not record durable evidence");

  const auto filespace_result =
      page::EvaluatePageFilespaceAgentRequest(&queue, FilespaceAgentRequest(), policy);
  Require(filespace_result.ok(), "filespace-agent request was not accepted");
  Require(filespace_result.record.request.state ==
              page::PageFilespaceAgentRequestState::waiting_filespace_agent,
          "filespace-agent request state mismatch");
  Require(filespace_result.record.filespace_agent_action_required,
          "filespace-agent action flag missing");
  Require(queue.records.size() == 2, "accepted requests were not queued");
}

void TestRefusedBoundaryViolation() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto policy = Policy();
  auto request = PageAgentRequest(10);
  request.responding_agent = "filespace_capacity_manager";

  const auto result = page::EvaluatePageFilespaceAgentRequest(&queue, request, policy);
  Require(!result.ok(), "boundary violation unexpectedly accepted");
  Require(result.record.request.state == page::PageFilespaceAgentRequestState::refused,
          "boundary violation did not persist refused state");
  Require(result.record.violation ==
              page::PageFilespaceAgentBoundaryViolation::filespace_agent_manages_pages,
          "boundary violation kind mismatch");
  Require(result.diagnostic.diagnostic_code ==
              "page_filespace_agent_boundary_filespace_manages_pages",
          "boundary violation diagnostic mismatch");
  Require(queue.records.size() == 1, "refused request was not durably queued");
}

void TestIdempotency() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto policy = Policy();
  const auto request = FilespaceAgentRequest(20);

  const auto first = page::EvaluatePageFilespaceAgentRequest(&queue, request, policy);
  const auto second = page::EvaluatePageFilespaceAgentRequest(&queue, request, policy);
  Require(first.ok() && second.ok(), "idempotent accepted request failed");
  Require(second.idempotent, "second enqueue was not idempotent");
  Require(queue.records.size() == 1, "idempotent enqueue duplicated the queue record");
  Require(uuid::UuidToString(first.request_uuid.value) ==
              uuid::UuidToString(second.request_uuid.value),
          "idempotent enqueue returned a different request UUID");

  auto conflict = request;
  conflict.request_uuid = first.request_uuid;
  conflict.requested_pages += 1;
  const auto conflicted = page::EvaluatePageFilespaceAgentRequest(&queue, conflict, policy);
  Require(!conflicted.ok(), "same request UUID with different payload was accepted");
  Require(conflicted.diagnostic.diagnostic_code ==
              "page_filespace_agent_request_idempotency_conflict",
          "idempotency conflict diagnostic mismatch");
  Require(queue.records.size() == 1, "idempotency conflict duplicated the queue record");
}

void TestTransitionsAndCancel() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto policy = Policy();
  const auto accepted =
      page::EvaluatePageFilespaceAgentRequest(&queue, PageAgentRequest(30), policy);
  Require(accepted.ok(), "transition fixture request failed");

  const auto invalid = page::TransitionPageFilespaceAgentRequest(
      &queue,
      accepted.request_uuid,
      page::PageFilespaceAgentRequestState::completed,
      "invalid direct completion");
  Require(!invalid.ok(), "invalid waiting->completed transition succeeded");
  Require(invalid.diagnostic.diagnostic_code == "page_filespace_agent_invalid_transition",
          "invalid transition diagnostic mismatch");

  const auto approved = page::TransitionPageFilespaceAgentRequest(
      &queue,
      accepted.request_uuid,
      page::PageFilespaceAgentRequestState::approved,
      "owner approved");
  Require(approved.ok(), "waiting->approved transition failed");
  const auto inflight = page::TransitionPageFilespaceAgentRequest(
      &queue,
      accepted.request_uuid,
      page::PageFilespaceAgentRequestState::in_flight,
      "owner started");
  Require(inflight.ok(), "approved->in_flight transition failed");
  const auto completed = page::TransitionPageFilespaceAgentRequest(
      &queue,
      accepted.request_uuid,
      page::PageFilespaceAgentRequestState::completed,
      "owner completed");
  Require(completed.ok(), "in_flight->completed transition failed");
  Require(completed.record.transitions.size() == 4,
          "transition history did not persist all state changes");

  const auto cancel_fixture =
      page::EvaluatePageFilespaceAgentRequest(&queue, FilespaceAgentRequest(31), policy);
  const auto cancelled = page::CancelPageFilespaceAgentRequest(
      &queue,
      cancel_fixture.request_uuid,
      "operator cancelled before owner action");
  Require(cancelled.ok() && cancelled.cancelled, "waiting cancellation failed");
  Require(cancelled.record.request.state == page::PageFilespaceAgentRequestState::cancelled,
          "cancelled state was not persisted");

  const auto unsafe_fixture =
      page::EvaluatePageFilespaceAgentRequest(&queue, PageAgentRequest(32), policy);
  Require(page::TransitionPageFilespaceAgentRequest(&queue,
                                                   unsafe_fixture.request_uuid,
                                                   page::PageFilespaceAgentRequestState::approved,
                                                   "owner approved")
              .ok(),
          "unsafe cancel fixture approval failed");
  const auto unsafe_cancel = page::CancelPageFilespaceAgentRequest(
      &queue,
      unsafe_fixture.request_uuid,
      "cancel after owner approval");
  Require(!unsafe_cancel.ok(), "approved request cancelled unsafely");
  Require(unsafe_cancel.diagnostic.diagnostic_code == "page_filespace_agent_cancel_unsafe",
          "unsafe cancel diagnostic mismatch");
}

void TestSerializeRestoreRoundTrip() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto policy = Policy();
  const auto accepted =
      page::EvaluatePageFilespaceAgentRequest(&queue, FilespaceAgentRequest(40), policy);
  Require(accepted.ok(), "serialize fixture enqueue failed");
  Require(page::TransitionPageFilespaceAgentRequest(&queue,
                                                   accepted.request_uuid,
                                                   page::PageFilespaceAgentRequestState::approved,
                                                   "approved for round trip")
              .ok(),
          "serialize fixture transition failed");

  const auto encoded = page::SerializePageFilespaceAgentRequestQueue(queue);
  const auto restored = page::RestorePageFilespaceAgentRequestQueue(encoded);
  Require(restored.ok(), "queue restore failed: " + restored.diagnostic.diagnostic_code);
  Require(restored.queue.records.size() == queue.records.size(),
          "restored queue record count mismatch");
  Require(restored.queue.records[0].sequence == queue.records[0].sequence,
          "restored sequence mismatch");
  Require(restored.queue.records[0].request.kind == queue.records[0].request.kind,
          "restored request kind mismatch");
  Require(restored.queue.records[0].request.state == page::PageFilespaceAgentRequestState::approved,
          "restored request state mismatch");
  Require(restored.queue.records[0].request.requested_pages ==
              queue.records[0].request.requested_pages,
          "restored page counts mismatch");
  Require(restored.queue.records[0].diagnostic_code == queue.records[0].diagnostic_code,
          "restored diagnostic state mismatch");
  Require(restored.queue.records[0].transitions.size() == queue.records[0].transitions.size(),
          "restored transition history mismatch");
  Require(page::SerializePageFilespaceAgentRequestQueue(restored.queue) == encoded,
          "queue serialization was not deterministic after restore");

  auto invalid_chain = restored.queue;
  invalid_chain.records[0].transitions[0].new_state =
      page::PageFilespaceAgentRequestState::completed;
  const auto invalid_restore = page::RestorePageFilespaceAgentRequestQueue(
      page::SerializePageFilespaceAgentRequestQueue(invalid_chain));
  Require(!invalid_restore.ok(), "invalid transition chain restored successfully");
  Require(invalid_restore.diagnostic.diagnostic_code ==
              "page_filespace_agent_queue_restore_invalid_record",
          "invalid transition chain diagnostic mismatch");
}

void TestRecoveryClassificationWaitingEvidence() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto accepted =
      page::EvaluatePageFilespaceAgentRequest(&queue, FilespaceAgentRequest(50), Policy());
  Require(accepted.ok(), "waiting recovery fixture failed");
  const auto restored =
      page::RestorePageFilespaceAgentRequestQueue(page::SerializePageFilespaceAgentRequestQueue(queue));
  Require(restored.ok(), "waiting recovery restore failed");
  const auto classified =
      page::ClassifyPageFilespaceAgentRequestQueueForRecovery(restored.queue);
  Require(classified.ok(), "waiting request with evidence failed recovery classification");
  const auto* waiting = FindClassification(
      classified, page::PageFilespaceAgentRequestState::waiting_filespace_agent);
  Require(waiting != nullptr, "waiting classification missing");
  Require(waiting->action == page::PageFilespaceAgentRequestRecoveryAction::resume_revalidate,
          "waiting request did not classify as resume/revalidate");

  auto missing_evidence = restored.queue;
  missing_evidence.records[0].explicit_evidence = false;
  missing_evidence.records[0].evidence_id = {};
  missing_evidence.records[0].transitions.clear();
  const auto failed =
      page::ClassifyPageFilespaceAgentRequestQueueForRecovery(missing_evidence);
  Require(!failed.ok(), "waiting request without evidence did not fail closed");
  Require(failed.classifications[0].diagnostic_code ==
              "page_filespace_agent_recovery_evidence_required",
          "waiting missing-evidence diagnostic mismatch");
}

void TestRecoveryClassificationPartialStates() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto policy = Policy();
  const auto approved_request =
      page::EvaluatePageFilespaceAgentRequest(&queue, PageAgentRequest(60), policy);
  const auto inflight_request =
      page::EvaluatePageFilespaceAgentRequest(&queue, FilespaceAgentRequest(61), policy);
  Require(approved_request.ok() && inflight_request.ok(), "partial recovery fixture failed");
  Require(page::TransitionPageFilespaceAgentRequest(&queue,
                                                   approved_request.request_uuid,
                                                   page::PageFilespaceAgentRequestState::approved,
                                                   "owner approved")
              .ok(),
          "approved recovery fixture transition failed");
  Require(page::TransitionPageFilespaceAgentRequest(&queue,
                                                   inflight_request.request_uuid,
                                                   page::PageFilespaceAgentRequestState::approved,
                                                   "owner approved")
              .ok(),
          "inflight recovery fixture approval failed");
  Require(page::TransitionPageFilespaceAgentRequest(&queue,
                                                   inflight_request.request_uuid,
                                                   page::PageFilespaceAgentRequestState::in_flight,
                                                   "owner started")
              .ok(),
          "inflight recovery fixture transition failed");

  const auto classified = page::ClassifyPageFilespaceAgentRequestQueueForRecovery(queue);
  Require(!classified.ok(), "partial states did not fail closed");
  const auto* approved =
      FindClassification(classified, page::PageFilespaceAgentRequestState::approved);
  const auto* inflight =
      FindClassification(classified, page::PageFilespaceAgentRequestState::in_flight);
  Require(approved != nullptr && approved->fail_closed,
          "approved state did not fail closed");
  Require(inflight != nullptr && inflight->fail_closed,
          "in-flight state did not fail closed");
}

void TestRecoveryClassificationTerminalStates() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto policy = Policy();

  auto refused_request = PageAgentRequest(70);
  refused_request.responding_agent = "filespace_capacity_manager";
  (void)page::EvaluatePageFilespaceAgentRequest(&queue, refused_request, policy);

  const auto completed_request =
      page::EvaluatePageFilespaceAgentRequest(&queue, PageAgentRequest(71), policy);
  Require(page::TransitionPageFilespaceAgentRequest(&queue,
                                                   completed_request.request_uuid,
                                                   page::PageFilespaceAgentRequestState::approved,
                                                   "owner approved")
              .ok(),
          "terminal completed approval failed");
  Require(page::TransitionPageFilespaceAgentRequest(&queue,
                                                   completed_request.request_uuid,
                                                   page::PageFilespaceAgentRequestState::in_flight,
                                                   "owner started")
              .ok(),
          "terminal completed in-flight failed");
  Require(page::TransitionPageFilespaceAgentRequest(&queue,
                                                   completed_request.request_uuid,
                                                   page::PageFilespaceAgentRequestState::completed,
                                                   "owner completed")
              .ok(),
          "terminal completed transition failed");

  const auto cancelled_request =
      page::EvaluatePageFilespaceAgentRequest(&queue, FilespaceAgentRequest(72), policy);
  Require(page::CancelPageFilespaceAgentRequest(&queue,
                                               cancelled_request.request_uuid,
                                               "cancel terminal fixture")
              .ok(),
          "terminal cancel failed");

  const auto classified = page::ClassifyPageFilespaceAgentRequestQueueForRecovery(queue);
  Require(classified.ok(), "terminal queue unexpectedly failed recovery classification");
  Require(classified.classifications.size() == 3, "terminal classification count mismatch");
  for (const auto& classification : classified.classifications) {
    Require(!classification.fail_closed, "terminal classification failed closed");
    Require(classification.action == page::PageFilespaceAgentRequestRecoveryAction::no_action,
            "terminal classification was not no-op");
  }
}

void TestQueueBackedHandoffIntegration() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto policy = Policy(90);

  const auto low_event = LowReserveEvent(1, 4);
  const auto low_result =
      page::NotifyFilespaceLowReserve(&queue, low_event, policy);
  Require(low_result.ok(), "queue-backed low reserve was not accepted");
  Require(low_result.queue_backed && low_result.accepted && low_result.notified,
          "low reserve did not report accepted queue-backed notification");
  Require(queue.records.size() == 1, "low reserve did not enqueue exactly one request");
  Require(queue.records[0].request.kind == page::PageFilespaceAgentRequestKind::extend_filespace,
          "low reserve request kind mismatch");
  Require(queue.records[0].request.responding_agent == "filespace_capacity_manager",
          "low reserve did not target filespace capacity manager");
  Require(queue.records[0].request.state ==
              page::PageFilespaceAgentRequestState::waiting_filespace_agent,
          "low reserve request was not waiting on filespace agent");
  Require(queue.records[0].explicit_evidence && queue.records[0].evidence_id.valid(),
          "low reserve queue evidence missing");
  Require(low_result.evidence.request_uuid.valid() &&
              SameUuid(low_result.evidence.request_uuid, queue.records[0].request.request_uuid),
          "low reserve evidence request UUID missing");
  Require(low_result.evidence.evidence_id.valid(),
          "low reserve evidence ID missing");
  Require(low_result.evidence.diagnostic_code == "ok",
          "low reserve diagnostic mismatch");
  Require(SameUuid(queue.records[0].request.database_uuid, low_event.database_uuid),
          "low reserve database UUID mismatch");
  Require(SameUuid(queue.records[0].request.filespace_uuid, low_event.filespace_uuid),
          "low reserve filespace UUID mismatch");
  Require(SameUuid(queue.records[0].request.policy_uuid, low_event.policy.policy_uuid),
          "low reserve policy UUID mismatch");
  Require(queue.records[0].request.page_family == low_event.page_family,
          "low reserve page family mismatch");
  Require(queue.records[0].request.requested_pages == 8 &&
              queue.records[0].request.released_free_pages == 4 &&
              queue.records[0].request.target_reserve_pages == 12 &&
              queue.records[0].request.threshold_pages == 6 &&
              queue.records[0].request.allocated_pages == low_event.allocated_pages &&
              queue.records[0].request.reserved_pages == low_event.reserved_pages,
          "low reserve page counts were not preserved");

  const auto above_event = LowReserveEvent(2, 7);
  const auto above_result =
      page::NotifyFilespaceLowReserve(&queue, above_event, policy);
  Require(!above_result.ok(), "above-threshold low reserve unexpectedly succeeded");
  Require(above_result.diagnostic.diagnostic_code ==
              "page_filespace_low_reserve_threshold_not_met",
          "above-threshold diagnostic changed");
  Require(!above_result.evidence.evidence_id.valid() &&
              !above_result.evidence.durable_state_changed,
          "above-threshold low reserve recorded durable evidence");
  Require(queue.records.size() == 1, "above-threshold low reserve mutated queue");

  const auto shrink_ready_event = ShrinkEvent(3, 0, 0, 0);
  const auto shrink_ready_result =
      page::NotifyFilespaceShrinkReady(&queue, shrink_ready_event, policy);
  Require(shrink_ready_result.ok(), "queue-backed shrink-ready was not accepted");
  Require(shrink_ready_result.shrink_ready,
          "shrink-ready result did not report shrink readiness");
  Require(queue.records.size() == 2, "shrink-ready did not enqueue one request");
  Require(queue.records[1].request.kind == page::PageFilespaceAgentRequestKind::truncate_filespace,
          "shrink-ready request kind mismatch");
  Require(queue.records[1].request.responding_agent == "filespace_capacity_manager",
          "shrink-ready did not target filespace capacity manager");
  Require(queue.records[1].request.state ==
              page::PageFilespaceAgentRequestState::waiting_filespace_agent,
          "shrink-ready request was not waiting on filespace agent");
  Require(queue.records[1].request.relocated_pages == shrink_ready_event.relocated_pages &&
              queue.records[1].request.pinned_pages == 0 &&
              queue.records[1].request.active_pages == 0 &&
              queue.records[1].request.reserved_pages == 0,
          "shrink-ready page counts were not preserved");
  Require(queue.records[1].explicit_evidence && queue.records[1].evidence_id.valid(),
          "shrink-ready queue evidence missing");

  const auto shrink_blocked_event = ShrinkEvent(4, 1, 2, 3);
  const auto shrink_blocked_result =
      page::NotifyFilespaceShrinkReady(&queue, shrink_blocked_event, policy);
  Require(!shrink_blocked_result.ok(), "shrink-blocked handoff reported ready");
  Require(!shrink_blocked_result.notified && !shrink_blocked_result.shrink_ready,
          "shrink-blocked result was silently marked shrink-ready");
  Require(queue.records.size() == 3, "shrink-blocked did not record durable request");
  Require(queue.records[2].request.kind == page::PageFilespaceAgentRequestKind::deny_request,
          "shrink-blocked request kind mismatch");
  Require(queue.records[2].request.state == page::PageFilespaceAgentRequestState::refused,
          "shrink-blocked request was not terminal refused");
  Require(queue.records[2].diagnostic_code == "page_filespace_shrink_blocked" &&
              queue.records[2].evidence_state == "shrink_blocked_refused",
          "shrink-blocked deterministic evidence mismatch");
  Require(queue.records[2].request.pinned_pages == 1 &&
              queue.records[2].request.active_pages == 2 &&
              queue.records[2].request.reserved_pages == 3,
          "shrink-blocked blocker counts were not preserved");
  Require(shrink_blocked_result.evidence.kind == page::PageFilespaceHandoffKind::shrink_blocked &&
              shrink_blocked_result.evidence.diagnostic_code == "page_filespace_shrink_blocked" &&
              shrink_blocked_result.evidence.durable_state_changed,
          "shrink-blocked handoff evidence mismatch");

  const auto encoded = page::SerializePageFilespaceAgentRequestQueue(queue);
  const auto restored = page::RestorePageFilespaceAgentRequestQueue(encoded);
  Require(restored.ok(), "queue-backed handoff restore failed: " +
                             restored.diagnostic.diagnostic_code);
  Require(restored.queue.records.size() == 3,
          "queue-backed handoff restore record count mismatch");
  Require(restored.queue.records[0].request.kind ==
              page::PageFilespaceAgentRequestKind::extend_filespace &&
              restored.queue.records[0].request.requested_pages == 8 &&
              restored.queue.records[0].request.threshold_pages == 6,
          "restored low-reserve queue record mismatch");
  Require(restored.queue.records[1].request.kind ==
              page::PageFilespaceAgentRequestKind::truncate_filespace &&
              restored.queue.records[1].request.relocated_pages ==
                  shrink_ready_event.relocated_pages,
          "restored shrink-ready queue record mismatch");
  Require(restored.queue.records[2].request.kind ==
              page::PageFilespaceAgentRequestKind::deny_request &&
              restored.queue.records[2].request.state ==
                  page::PageFilespaceAgentRequestState::refused &&
              restored.queue.records[2].diagnostic_code == "page_filespace_shrink_blocked",
          "restored shrink-blocked queue record mismatch");
  Require(page::SerializePageFilespaceAgentRequestQueue(restored.queue) == encoded,
          "queue-backed handoff serialization was not deterministic after restore");

  const auto classified =
      page::ClassifyPageFilespaceAgentRequestQueueForRecovery(restored.queue);
  Require(classified.ok(), "queue-backed handoff recovery classification failed");
  const auto* low_classification =
      FindClassificationByRequest(classified, restored.queue.records[0].request.request_uuid);
  const auto* ready_classification =
      FindClassificationByRequest(classified, restored.queue.records[1].request.request_uuid);
  const auto* blocked_classification =
      FindClassificationByRequest(classified, restored.queue.records[2].request.request_uuid);
  Require(low_classification != nullptr &&
              low_classification->action ==
                  page::PageFilespaceAgentRequestRecoveryAction::resume_revalidate,
          "low-reserve waiting record did not resume/revalidate");
  Require(ready_classification != nullptr &&
              ready_classification->action ==
                  page::PageFilespaceAgentRequestRecoveryAction::resume_revalidate,
          "shrink-ready waiting record did not resume/revalidate");
  Require(blocked_classification != nullptr &&
              blocked_classification->action ==
                  page::PageFilespaceAgentRequestRecoveryAction::no_action &&
              !blocked_classification->fail_closed,
          "shrink-blocked terminal record did not classify as no-op");

  auto missing_blocked_evidence = restored.queue;
  missing_blocked_evidence.records[2].explicit_evidence = false;
  missing_blocked_evidence.records[2].evidence_id = {};
  missing_blocked_evidence.records[2].transitions.clear();
  const auto failed_blocked =
      page::ClassifyPageFilespaceAgentRequestQueueForRecovery(missing_blocked_evidence);
  Require(!failed_blocked.ok(), "blocked terminal record without evidence did not fail closed");
  const auto* failed_blocked_classification =
      FindClassificationByRequest(failed_blocked,
                                  missing_blocked_evidence.records[2].request.request_uuid);
  Require(failed_blocked_classification != nullptr &&
              failed_blocked_classification->fail_closed &&
              failed_blocked_classification->diagnostic_code ==
                  "page_filespace_agent_recovery_evidence_required",
          "blocked terminal missing-evidence diagnostic mismatch");
}

void TestMalformedRestore() {
  page::PageFilespaceAgentRequestQueue queue;
  const auto accepted =
      page::EvaluatePageFilespaceAgentRequest(&queue, PageAgentRequest(80), Policy());
  Require(accepted.ok(), "malformed restore fixture failed");
  auto encoded = page::SerializePageFilespaceAgentRequestQueue(queue);
  encoded.resize(encoded.size() - 3);
  const auto restored = page::RestorePageFilespaceAgentRequestQueue(encoded);
  Require(!restored.ok(), "truncated queue restored successfully");
  Require(restored.diagnostic.diagnostic_code ==
              "page_filespace_agent_queue_restore_malformed",
          "malformed restore diagnostic mismatch");
}

}  // namespace

int main() {
  TestAcceptedPageAndFilespaceRequests();
  TestRefusedBoundaryViolation();
  TestIdempotency();
  TestTransitionsAndCancel();
  TestSerializeRestoreRoundTrip();
  TestRecoveryClassificationWaitingEvidence();
  TestRecoveryClassificationPartialStates();
  TestRecoveryClassificationTerminalStates();
  TestQueueBackedHandoffIntegration();
  TestMalformedRestore();
  return EXIT_SUCCESS;
}
