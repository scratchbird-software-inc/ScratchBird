// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-PAGE-FILESPACE-HANDOFF-ANCHOR
// SEARCH_KEY: SB_PAGE_FILESPACE_HANDOFF_QUEUE
#include "page_filespace_handoff.hpp"

#include "metric_contracts.hpp"
#include "page_registry.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::HostToLittle32;
using scratchbird::core::platform::HostToLittle64;
using scratchbird::core::platform::LittleToHost32;
using scratchbird::core::platform::LittleToHost64;
using scratchbird::core::uuid::UuidToString;

constexpr byte kQueueMagic[] = {'S', 'B', 'P', 'F', 'A', 'R', '4', 'Q'};
constexpr u32 kQueueMinVersion = 1;
constexpr u32 kQueueVersion = 2;
constexpr u32 kMaxQueueStringBytes = 1024 * 1024;
constexpr u64 kMaxQueueRecords = 100000;
constexpr u64 kMaxQueueTransitionsPerRecord = 100000;

Status HandoffOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status HandoffErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

bool IsKnownInsertPageFamily(const std::string& page_family) {
  return IsKnownPageFamilyName(page_family) || page_family == "overflow" || page_family == "toast";
}

TypedUuid NewEvidenceId(const PageFilespaceHandoffLedger* ledger) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      ledger == nullptr ? 1 : ledger->next_evidence_sequence);
  return generated.ok() ? generated.value : TypedUuid{};
}

TypedUuid NewQueueEvidenceId(u64 sequence) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      1820000000000ull + sequence);
  return generated.ok() ? generated.value : TypedUuid{};
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool IsTerminalRequestState(PageFilespaceAgentRequestState state) {
  return state == PageFilespaceAgentRequestState::refused ||
         state == PageFilespaceAgentRequestState::completed ||
         state == PageFilespaceAgentRequestState::cancelled;
}

bool IsWaitingRequestState(PageFilespaceAgentRequestState state) {
  return state == PageFilespaceAgentRequestState::waiting_filespace_agent ||
         state == PageFilespaceAgentRequestState::waiting_page_agent;
}

bool IsKnownRequestKind(PageFilespaceAgentRequestKind kind) {
  switch (kind) {
    case PageFilespaceAgentRequestKind::reserve_pages:
    case PageFilespaceAgentRequestKind::extend_filespace:
    case PageFilespaceAgentRequestKind::shrink_candidate:
    case PageFilespaceAgentRequestKind::truncate_filespace:
    case PageFilespaceAgentRequestKind::relocate_pages:
    case PageFilespaceAgentRequestKind::release_pages:
    case PageFilespaceAgentRequestKind::deny_request:
      return true;
  }
  return false;
}

bool IsKnownRequestState(PageFilespaceAgentRequestState state) {
  switch (state) {
    case PageFilespaceAgentRequestState::created:
    case PageFilespaceAgentRequestState::validated:
    case PageFilespaceAgentRequestState::refused:
    case PageFilespaceAgentRequestState::waiting_filespace_agent:
    case PageFilespaceAgentRequestState::waiting_page_agent:
    case PageFilespaceAgentRequestState::approved:
    case PageFilespaceAgentRequestState::in_flight:
    case PageFilespaceAgentRequestState::completed:
    case PageFilespaceAgentRequestState::cancelled:
    case PageFilespaceAgentRequestState::recovery_required:
      return true;
  }
  return false;
}

bool IsKnownBoundaryViolation(PageFilespaceAgentBoundaryViolation violation) {
  switch (violation) {
    case PageFilespaceAgentBoundaryViolation::none:
    case PageFilespaceAgentBoundaryViolation::filespace_agent_manages_pages:
    case PageFilespaceAgentBoundaryViolation::page_agent_manages_files:
    case PageFilespaceAgentBoundaryViolation::missing_agent_identity:
    case PageFilespaceAgentBoundaryViolation::missing_policy:
    case PageFilespaceAgentBoundaryViolation::missing_evidence:
    case PageFilespaceAgentBoundaryViolation::invalid_filespace_identity:
    case PageFilespaceAgentBoundaryViolation::invalid_page_family:
      return true;
  }
  return false;
}

bool RequestPayloadMatches(const PageFilespaceAgentRequest& left,
                           const PageFilespaceAgentRequest& right) {
  return SameTypedUuid(left.database_uuid, right.database_uuid) &&
         SameTypedUuid(left.filespace_uuid, right.filespace_uuid) &&
         SameTypedUuid(left.policy_uuid, right.policy_uuid) &&
         left.kind == right.kind &&
         left.requesting_agent == right.requesting_agent &&
         left.responding_agent == right.responding_agent &&
         left.page_family == right.page_family &&
         left.requested_pages == right.requested_pages &&
         left.released_free_pages == right.released_free_pages &&
         left.target_reserve_pages == right.target_reserve_pages &&
         left.threshold_pages == right.threshold_pages &&
         left.free_pages == right.free_pages &&
         left.preallocated_pages == right.preallocated_pages &&
         left.allocated_pages == right.allocated_pages &&
         left.reserved_pages == right.reserved_pages &&
         left.relocated_pages == right.relocated_pages &&
         left.pinned_pages == right.pinned_pages &&
         left.active_pages == right.active_pages &&
         left.reason == right.reason;
}

bool RequestIdentityMatches(const PageFilespaceAgentRequest& left,
                            const PageFilespaceAgentRequest& right) {
  if (left.request_uuid.valid() && right.request_uuid.valid()) {
    return SameTypedUuid(left.request_uuid, right.request_uuid) &&
           RequestPayloadMatches(left, right);
  }
  return RequestPayloadMatches(left, right);
}

PageFilespaceAgentQueueRecord* FindQueueRecord(PageFilespaceAgentRequestQueue* queue,
                                               const TypedUuid& request_uuid) {
  if (queue == nullptr || !request_uuid.valid()) {
    return nullptr;
  }
  for (auto& record : queue->records) {
    if (SameTypedUuid(record.request.request_uuid, request_uuid)) {
      return &record;
    }
  }
  return nullptr;
}

const PageFilespaceAgentQueueRecord* FindQueueRecord(const PageFilespaceAgentRequestQueue& queue,
                                                     const TypedUuid& request_uuid) {
  if (!request_uuid.valid()) {
    return nullptr;
  }
  for (const auto& record : queue.records) {
    if (SameTypedUuid(record.request.request_uuid, request_uuid)) {
      return &record;
    }
  }
  return nullptr;
}

const PageFilespaceAgentQueueRecord* FindQueueRecord(const PageFilespaceAgentRequestQueue& queue,
                                                     const PageFilespaceAgentRequest& request) {
  for (const auto& record : queue.records) {
    if (RequestIdentityMatches(record.request, request)) {
      return &record;
    }
  }
  return nullptr;
}

bool IsAllowedRequestTransition(PageFilespaceAgentRequestState from,
                                PageFilespaceAgentRequestState to) {
  if (from == to) {
    return true;
  }
  if (IsTerminalRequestState(from)) {
    return false;
  }
  switch (from) {
    case PageFilespaceAgentRequestState::created:
      return to == PageFilespaceAgentRequestState::validated ||
             to == PageFilespaceAgentRequestState::waiting_filespace_agent ||
             to == PageFilespaceAgentRequestState::waiting_page_agent ||
             to == PageFilespaceAgentRequestState::refused ||
             to == PageFilespaceAgentRequestState::cancelled;
    case PageFilespaceAgentRequestState::validated:
      return to == PageFilespaceAgentRequestState::waiting_filespace_agent ||
             to == PageFilespaceAgentRequestState::waiting_page_agent ||
             to == PageFilespaceAgentRequestState::refused ||
             to == PageFilespaceAgentRequestState::cancelled;
    case PageFilespaceAgentRequestState::waiting_filespace_agent:
    case PageFilespaceAgentRequestState::waiting_page_agent:
      return to == PageFilespaceAgentRequestState::approved ||
             to == PageFilespaceAgentRequestState::refused ||
             to == PageFilespaceAgentRequestState::cancelled;
    case PageFilespaceAgentRequestState::approved:
      return to == PageFilespaceAgentRequestState::in_flight ||
             to == PageFilespaceAgentRequestState::recovery_required;
    case PageFilespaceAgentRequestState::in_flight:
      return to == PageFilespaceAgentRequestState::completed ||
             to == PageFilespaceAgentRequestState::recovery_required;
    case PageFilespaceAgentRequestState::recovery_required:
    case PageFilespaceAgentRequestState::refused:
    case PageFilespaceAgentRequestState::completed:
    case PageFilespaceAgentRequestState::cancelled:
      return false;
  }
  return false;
}

PageFilespaceAgentQueueResult QueueError(std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail) {
  PageFilespaceAgentQueueResult result;
  result.status = HandoffErrorStatus();
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

PageFilespaceAgentQueueRestoreResult RestoreError(std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail) {
  PageFilespaceAgentQueueRestoreResult result;
  result.status = HandoffErrorStatus();
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

void Store8(std::vector<byte>* out, byte value) {
  out->push_back(value);
}

void Store32(std::vector<byte>* out, u32 value) {
  value = HostToLittle32(value);
  const auto* raw = reinterpret_cast<const byte*>(&value);
  out->insert(out->end(), raw, raw + sizeof(value));
}

void Store64(std::vector<byte>* out, u64 value) {
  value = HostToLittle64(value);
  const auto* raw = reinterpret_cast<const byte*>(&value);
  out->insert(out->end(), raw, raw + sizeof(value));
}

void StoreBool(std::vector<byte>* out, bool value) {
  Store8(out, value ? static_cast<byte>(1) : static_cast<byte>(0));
}

void StoreUuid(std::vector<byte>* out, const TypedUuid& uuid) {
  Store8(out, static_cast<byte>(uuid.kind));
  out->insert(out->end(), uuid.value.bytes.begin(), uuid.value.bytes.end());
}

void StoreString(std::vector<byte>* out, const std::string& value) {
  Store32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

struct QueueReader {
  const std::vector<byte>& data;
  std::size_t offset = 0;
  bool failed = false;
  std::string detail;

  bool Ensure(std::size_t count, const std::string& field) {
    if (failed) {
      return false;
    }
    if (count > data.size() || offset > data.size() - count) {
      failed = true;
      detail = field;
      return false;
    }
    return true;
  }

  byte Read8(const std::string& field) {
    if (!Ensure(1, field)) {
      return 0;
    }
    return data[offset++];
  }

  bool ReadBool(const std::string& field) {
    const byte value = Read8(field);
    if (failed) {
      return false;
    }
    if (value != 0 && value != 1) {
      failed = true;
      detail = field;
      return false;
    }
    return value == 1;
  }

  u32 Read32(const std::string& field) {
    if (!Ensure(sizeof(u32), field)) {
      return 0;
    }
    u32 value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    offset += sizeof(value);
    return LittleToHost32(value);
  }

  u64 Read64(const std::string& field) {
    if (!Ensure(sizeof(u64), field)) {
      return 0;
    }
    u64 value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    offset += sizeof(value);
    return LittleToHost64(value);
  }

  TypedUuid ReadUuid(const std::string& field) {
    TypedUuid uuid;
    uuid.kind = static_cast<UuidKind>(Read8(field + ".kind"));
    if (!Ensure(uuid.value.bytes.size(), field + ".value")) {
      return uuid;
    }
    std::memcpy(uuid.value.bytes.data(), data.data() + offset, uuid.value.bytes.size());
    offset += uuid.value.bytes.size();
    return uuid;
  }

  std::string ReadString(const std::string& field) {
    const u32 length = Read32(field + ".length");
    if (failed) {
      return {};
    }
    if (length > kMaxQueueStringBytes) {
      failed = true;
      detail = field + ".length";
      return {};
    }
    if (!Ensure(length, field + ".value")) {
      return {};
    }
    std::string value(reinterpret_cast<const char*>(data.data() + offset), length);
    offset += length;
    return value;
  }
};

PageFilespaceHandoffResult Refuse(PageFilespaceHandoffLedger* ledger,
                                  PageFilespaceHandoffKind kind,
                                  const TypedUuid& database_uuid,
                                  const TypedUuid& filespace_uuid,
                                  const TypedUuid& policy_uuid,
                                  const std::string& page_family,
                                  std::string diagnostic_code,
                                  std::string message_key,
                                  std::string detail) {
  PageFilespaceHandoffResult result;
  result.status = HandoffErrorStatus();
  result.notified = false;
  result.advisory = true;
  result.evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  result.evidence.kind = kind;
  result.evidence.previous_state = PageFilespaceHandoffState::absent;
  result.evidence.new_state = PageFilespaceHandoffState::refused;
  result.evidence.evidence_id = NewEvidenceId(ledger);
  result.evidence.database_uuid = database_uuid;
  result.evidence.filespace_uuid = filespace_uuid;
  result.evidence.policy_uuid = policy_uuid;
  result.evidence.page_family = page_family;
  result.evidence.reason = detail;
  result.evidence.diagnostic_code = diagnostic_code;
  result.evidence.advisory = true;
  result.evidence.durable_state_changed = false;
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  if (ledger != nullptr) {
    ledger->evidence.push_back(result.evidence);
  }
  return result;
}

u64 RequestedLowReservePages(const PageFilespaceLowReserveEvent& event);

PageFilespaceHandoffEvidenceRecord LowReserveEvidence(PageFilespaceHandoffLedger* ledger,
                                                      const PageFilespaceLowReserveEvent& event,
                                                      bool notified) {
  PageFilespaceHandoffEvidenceRecord evidence;
  evidence.sequence = ledger->next_evidence_sequence++;
  evidence.kind = PageFilespaceHandoffKind::low_reserve;
  evidence.previous_state = PageFilespaceHandoffState::absent;
  evidence.new_state = notified ? PageFilespaceHandoffState::advisory_recorded
                                : PageFilespaceHandoffState::refused;
  evidence.evidence_id = NewEvidenceId(ledger);
  evidence.database_uuid = event.database_uuid;
  evidence.filespace_uuid = event.filespace_uuid;
  evidence.policy_uuid = event.policy.policy_uuid;
  evidence.request_kind = PageFilespaceAgentRequestKind::extend_filespace;
  evidence.page_family = event.page_family;
  evidence.requested_pages = RequestedLowReservePages(event);
  evidence.released_free_pages = event.released_free_pages;
  evidence.target_reserve_pages = NormalizeFilespaceTargetReservePages(event.policy);
  evidence.threshold_pages = FilespaceLowReserveThresholdPages(event.policy);
  evidence.allocated_pages = event.allocated_pages;
  evidence.reserved_pages = event.reserved_pages;
  evidence.reason = event.reason;
  evidence.diagnostic_code = notified ? "ok" : "page_filespace_low_reserve_threshold_not_met";
  evidence.advisory = event.policy.advisory_events;
  evidence.durable_state_changed = !event.policy.advisory_events;
  return evidence;
}

PageFilespaceHandoffResult HandoffInputError(PageFilespaceHandoffKind kind,
                                             const TypedUuid& database_uuid,
                                             const TypedUuid& filespace_uuid,
                                             const TypedUuid& policy_uuid,
                                             const std::string& page_family,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  PageFilespaceHandoffResult result;
  result.status = HandoffErrorStatus();
  result.evidence.kind = kind;
  result.evidence.previous_state = PageFilespaceHandoffState::absent;
  result.evidence.new_state = PageFilespaceHandoffState::refused;
  result.evidence.database_uuid = database_uuid;
  result.evidence.filespace_uuid = filespace_uuid;
  result.evidence.policy_uuid = policy_uuid;
  result.evidence.page_family = page_family;
  result.evidence.reason = detail;
  result.evidence.diagnostic_code = diagnostic_code;
  result.evidence.advisory = false;
  result.evidence.durable_state_changed = false;
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

TypedUuid SelectPolicyUuid(const TypedUuid& event_policy_uuid,
                           const PageFilespaceAgentRequestPolicy& policy) {
  return event_policy_uuid.valid() ? event_policy_uuid : policy.policy_uuid;
}

bool HasQueueEvidence(const PageFilespaceAgentQueueRecord& record) {
  return record.request.request_uuid.valid() &&
         record.explicit_evidence &&
         record.evidence_id.valid() &&
         !record.transitions.empty() &&
         record.transitions.back().explicit_evidence &&
         record.transitions.back().evidence_id.valid();
}

u64 RequestedLowReservePages(const PageFilespaceLowReserveEvent& event) {
  const u64 target = NormalizeFilespaceTargetReservePages(event.policy);
  if (target > event.released_free_pages) {
    return target - event.released_free_pages;
  }
  return target == 0 ? 1 : target;
}

PageFilespaceAgentRequest LowReserveCapacityRequest(
    const PageFilespaceLowReserveEvent& event,
    const PageFilespaceAgentRequestPolicy& policy) {
  PageFilespaceAgentRequest request;
  request.database_uuid = event.database_uuid;
  request.filespace_uuid = event.filespace_uuid;
  request.policy_uuid = SelectPolicyUuid(event.policy.policy_uuid, policy);
  request.kind = PageFilespaceAgentRequestKind::extend_filespace;
  request.requesting_agent = "page_allocation_manager";
  request.responding_agent = "filespace_capacity_manager";
  request.page_family = event.page_family;
  request.requested_pages = RequestedLowReservePages(event);
  request.released_free_pages = event.released_free_pages;
  request.target_reserve_pages = NormalizeFilespaceTargetReservePages(event.policy);
  request.threshold_pages = FilespaceLowReserveThresholdPages(event.policy);
  request.free_pages = event.released_free_pages;
  request.allocated_pages = event.allocated_pages;
  request.reserved_pages = event.reserved_pages;
  request.reason = event.reason.empty() ? "low reserve requires filespace capacity" : event.reason;
  return request;
}

bool ShrinkEventBlocked(const PageFilespaceShrinkReadyEvent& event) {
  return event.pinned_pages > 0 || event.active_pages > 0 || event.reserved_pages > 0;
}

PageFilespaceAgentRequest ShrinkCapacityRequest(
    const PageFilespaceShrinkReadyEvent& event,
    const PageFilespaceAgentRequestPolicy& policy,
    PageFilespaceAgentRequestKind kind) {
  PageFilespaceAgentRequest request;
  request.database_uuid = event.database_uuid;
  request.filespace_uuid = event.filespace_uuid;
  request.policy_uuid = SelectPolicyUuid(event.policy_uuid, policy);
  request.kind = kind;
  request.requesting_agent = "page_allocation_manager";
  request.responding_agent = "filespace_capacity_manager";
  request.page_family = event.page_family;
  request.requested_pages = event.relocated_pages;
  request.relocated_pages = event.relocated_pages;
  request.reserved_pages = event.reserved_pages;
  request.pinned_pages = event.pinned_pages;
  request.active_pages = event.active_pages;
  request.reason = event.reason.empty() ? "page shrink readiness handoff" : event.reason;
  return request;
}

void OverrideQueuedHandoffEvidence(PageFilespaceAgentRequestQueue* queue,
                                   PageFilespaceAgentQueueResult* queue_result,
                                   const std::string& diagnostic_code,
                                   const std::string& evidence_state) {
  if (queue == nullptr || queue_result == nullptr || !queue_result->request_uuid.valid()) {
    return;
  }
  PageFilespaceAgentQueueRecord* record = FindQueueRecord(queue, queue_result->request_uuid);
  if (record == nullptr) {
    return;
  }
  record->diagnostic_code = diagnostic_code;
  record->evidence_state = evidence_state;
  if (!record->transitions.empty()) {
    record->transitions.back().diagnostic_code = diagnostic_code;
  }
  queue_result->record = *record;
}

PageFilespaceHandoffEvidenceRecord EvidenceFromQueueRecord(
    PageFilespaceHandoffKind kind,
    PageFilespaceHandoffState state,
    const PageFilespaceAgentQueueRecord& record,
    const std::string& diagnostic_code) {
  PageFilespaceHandoffEvidenceRecord evidence;
  evidence.sequence = record.sequence;
  evidence.kind = kind;
  evidence.previous_state = PageFilespaceHandoffState::absent;
  evidence.new_state = state;
  evidence.request_uuid = record.request.request_uuid;
  evidence.evidence_id = record.evidence_id;
  evidence.database_uuid = record.request.database_uuid;
  evidence.filespace_uuid = record.request.filespace_uuid;
  evidence.policy_uuid = record.request.policy_uuid;
  evidence.request_kind = record.request.kind;
  evidence.page_family = record.request.page_family;
  evidence.requested_pages = record.request.requested_pages;
  evidence.released_free_pages = record.request.released_free_pages;
  evidence.target_reserve_pages = record.request.target_reserve_pages;
  evidence.threshold_pages = record.request.threshold_pages;
  evidence.allocated_pages = record.request.allocated_pages;
  evidence.reserved_pages = record.request.reserved_pages;
  evidence.relocated_pages = record.request.relocated_pages;
  evidence.pinned_pages = record.request.pinned_pages;
  evidence.active_pages = record.request.active_pages;
  evidence.reason = record.request.reason;
  evidence.diagnostic_code = diagnostic_code;
  evidence.advisory = false;
  evidence.durable_state_changed = HasQueueEvidence(record);
  return evidence;
}

PageFilespaceHandoffResult QueueBackedHandoffResult(
    PageFilespaceAgentQueueResult queue_result,
    PageFilespaceHandoffKind kind,
    bool notify_on_success,
    bool shrink_ready_on_success,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  PageFilespaceHandoffResult result;
  result.status = queue_result.status;
  result.queue_backed = true;
  result.queue_record = queue_result.record;
  result.request_uuid = queue_result.request_uuid;
  const bool has_evidence = HasQueueEvidence(queue_result.record);
  if (queue_result.status.ok() && !has_evidence) {
    result.status = HandoffErrorStatus();
    diagnostic_code = "page_filespace_agent_evidence_required";
    message_key = "storage.page.filespace_agent.evidence_required";
    detail = "queue record lacks explicit request evidence";
  }
  result.accepted = result.status.ok() && has_evidence;
  result.notified = notify_on_success && result.accepted;
  result.shrink_ready = shrink_ready_on_success && result.accepted;
  result.advisory = false;
  const PageFilespaceHandoffState state =
      result.accepted ? PageFilespaceHandoffState::durable_recorded
                      : PageFilespaceHandoffState::refused;
  result.evidence = EvidenceFromQueueRecord(kind, state, queue_result.record, diagnostic_code);
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

}  // namespace

const char* PageFilespaceHandoffKindName(PageFilespaceHandoffKind kind) {
  switch (kind) {
    case PageFilespaceHandoffKind::low_reserve:
      return "low_reserve";
    case PageFilespaceHandoffKind::shrink_ready:
      return "shrink_ready";
    case PageFilespaceHandoffKind::shrink_blocked:
      return "shrink_blocked";
  }
  return "unknown";
}

const char* PageFilespaceHandoffStateName(PageFilespaceHandoffState state) {
  switch (state) {
    case PageFilespaceHandoffState::absent:
      return "absent";
    case PageFilespaceHandoffState::advisory_recorded:
      return "advisory_recorded";
    case PageFilespaceHandoffState::durable_recorded:
      return "durable_recorded";
    case PageFilespaceHandoffState::refused:
      return "refused";
  }
  return "unknown";
}

const char* PageFilespaceHandoffRecoveryActionName(PageFilespaceHandoffRecoveryAction action) {
  switch (action) {
    case PageFilespaceHandoffRecoveryAction::no_action:
      return "no_action";
    case PageFilespaceHandoffRecoveryAction::replay_advisory:
      return "replay_advisory";
    case PageFilespaceHandoffRecoveryAction::reconcile_durable:
      return "reconcile_durable";
    case PageFilespaceHandoffRecoveryAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

const char* PageFilespaceAgentRequestKindName(PageFilespaceAgentRequestKind kind) {
  switch (kind) {
    case PageFilespaceAgentRequestKind::reserve_pages:
      return "reserve_pages";
    case PageFilespaceAgentRequestKind::extend_filespace:
      return "extend_filespace";
    case PageFilespaceAgentRequestKind::shrink_candidate:
      return "shrink_candidate";
    case PageFilespaceAgentRequestKind::truncate_filespace:
      return "truncate_filespace";
    case PageFilespaceAgentRequestKind::relocate_pages:
      return "relocate_pages";
    case PageFilespaceAgentRequestKind::release_pages:
      return "release_pages";
    case PageFilespaceAgentRequestKind::deny_request:
      return "deny_request";
  }
  return "unknown";
}

const char* PageFilespaceAgentRequestStateName(PageFilespaceAgentRequestState state) {
  switch (state) {
    case PageFilespaceAgentRequestState::created:
      return "created";
    case PageFilespaceAgentRequestState::validated:
      return "validated";
    case PageFilespaceAgentRequestState::refused:
      return "refused";
    case PageFilespaceAgentRequestState::waiting_filespace_agent:
      return "waiting_filespace_agent";
    case PageFilespaceAgentRequestState::waiting_page_agent:
      return "waiting_page_agent";
    case PageFilespaceAgentRequestState::approved:
      return "approved";
    case PageFilespaceAgentRequestState::in_flight:
      return "in_flight";
    case PageFilespaceAgentRequestState::completed:
      return "completed";
    case PageFilespaceAgentRequestState::cancelled:
      return "cancelled";
    case PageFilespaceAgentRequestState::recovery_required:
      return "recovery_required";
  }
  return "unknown";
}

const char* PageFilespaceAgentRequestRecoveryActionName(PageFilespaceAgentRequestRecoveryAction action) {
  switch (action) {
    case PageFilespaceAgentRequestRecoveryAction::no_action:
      return "no_action";
    case PageFilespaceAgentRequestRecoveryAction::resume_revalidate:
      return "resume_revalidate";
    case PageFilespaceAgentRequestRecoveryAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

const char* PageFilespaceAgentBoundaryViolationName(PageFilespaceAgentBoundaryViolation violation) {
  switch (violation) {
    case PageFilespaceAgentBoundaryViolation::none:
      return "none";
    case PageFilespaceAgentBoundaryViolation::filespace_agent_manages_pages:
      return "filespace_agent_manages_pages";
    case PageFilespaceAgentBoundaryViolation::page_agent_manages_files:
      return "page_agent_manages_files";
    case PageFilespaceAgentBoundaryViolation::missing_agent_identity:
      return "missing_agent_identity";
    case PageFilespaceAgentBoundaryViolation::missing_policy:
      return "missing_policy";
    case PageFilespaceAgentBoundaryViolation::missing_evidence:
      return "missing_evidence";
    case PageFilespaceAgentBoundaryViolation::invalid_filespace_identity:
      return "invalid_filespace_identity";
    case PageFilespaceAgentBoundaryViolation::invalid_page_family:
      return "invalid_page_family";
  }
  return "unknown";
}

u64 NormalizeFilespaceTargetReservePages(const PageFilespaceReservePolicy& policy) {
  const u64 min_target = policy.min_target_reserve_pages == 0 ? 4 : policy.min_target_reserve_pages;
  const u64 max_target = policy.max_target_reserve_pages == 0 ? 8 : policy.max_target_reserve_pages;
  const u64 lower = std::min(min_target, max_target);
  const u64 upper = std::max(min_target, max_target);
  const u64 requested = policy.target_reserve_pages == 0 ? upper : policy.target_reserve_pages;
  return std::max(lower, std::min(upper, requested));
}

u64 FilespaceLowReserveThresholdPages(const PageFilespaceReservePolicy& policy) {
  const u64 target = NormalizeFilespaceTargetReservePages(policy);
  return target / 2;
}

bool ShouldNotifyFilespaceLowReserve(const PageFilespaceLowReserveEvent& event) {
  return event.released_free_pages <= FilespaceLowReserveThresholdPages(event.policy);
}

u64 NormalizeAgentFilespaceTargetFreePages(const PageFilespaceAgentRequestPolicy& policy) {
  const u64 min_pages = policy.min_free_pages == 0 ? 4 : policy.min_free_pages;
  const u64 target_pages = policy.target_free_pages == 0 ? 8 : policy.target_free_pages;
  return std::max(min_pages, target_pages);
}

u64 AgentFilespaceLowWaterPages(const PageFilespaceAgentRequestPolicy& policy) {
  const u64 target = NormalizeAgentFilespaceTargetFreePages(policy);
  const double ratio = policy.low_water_ratio <= 0.0 ? 0.50 : policy.low_water_ratio;
  const u64 threshold = static_cast<u64>(static_cast<double>(target) * ratio);
  return std::max<u64>(1, threshold);
}

PageFilespaceAgentDecision EvaluatePageFilespaceAgentRequest(const PageFilespaceAgentRequest& request,
                                                             const PageFilespaceAgentRequestPolicy& policy) {
  PageFilespaceAgentDecision decision;
  decision.target_free_pages = NormalizeAgentFilespaceTargetFreePages(policy);
  decision.low_water_pages = AgentFilespaceLowWaterPages(policy);

  auto refuse = [&](PageFilespaceAgentBoundaryViolation violation,
                    std::string code,
                    std::string key,
                    std::string detail) {
    decision.status = HandoffErrorStatus();
    decision.state = PageFilespaceAgentRequestState::refused;
    decision.violation = violation;
    decision.allowed = false;
    decision.diagnostic = MakePageFilespaceHandoffDiagnostic(decision.status,
                                                            std::move(code),
                                                            std::move(key),
                                                            std::move(detail));
    return decision;
  };

  if (request.requesting_agent.empty() || request.responding_agent.empty()) {
    return refuse(PageFilespaceAgentBoundaryViolation::missing_agent_identity,
                  "page_filespace_agent_missing_identity",
                  "storage.page.filespace_agent.missing_identity",
                  "requesting_agent and responding_agent are required");
  }
  if (!request.policy_uuid.valid() && !policy.policy_uuid.valid()) {
    return refuse(PageFilespaceAgentBoundaryViolation::missing_policy,
                  "page_filespace_agent_missing_policy",
                  "storage.page.filespace_agent.missing_policy",
                  "policy UUID is required for page/filespace agent handoff");
  }
  if (!request.database_uuid.valid() || !request.filespace_uuid.valid()) {
    return refuse(PageFilespaceAgentBoundaryViolation::invalid_filespace_identity,
                  "page_filespace_agent_invalid_identity",
                  "storage.page.filespace_agent.invalid_identity",
                  "database_uuid and filespace_uuid must be valid engine UUIDs");
  }
  if (!request.page_family.empty() && !IsKnownInsertPageFamily(request.page_family)) {
    return refuse(PageFilespaceAgentBoundaryViolation::invalid_page_family,
                  "page_filespace_agent_invalid_page_family",
                  "storage.page.filespace_agent.invalid_page_family",
                  request.page_family);
  }

  switch (request.kind) {
    case PageFilespaceAgentRequestKind::reserve_pages:
    case PageFilespaceAgentRequestKind::relocate_pages:
    case PageFilespaceAgentRequestKind::release_pages:
      if (request.responding_agent == "filespace_capacity_manager") {
        return refuse(PageFilespaceAgentBoundaryViolation::filespace_agent_manages_pages,
                      "page_filespace_agent_boundary_filespace_manages_pages",
                      "storage.page.filespace_agent.boundary_filespace_manages_pages",
                      "page allocation manager owns page reservation, relocation, and release");
      }
      decision.page_agent_action_required = true;
      break;
    case PageFilespaceAgentRequestKind::extend_filespace:
    case PageFilespaceAgentRequestKind::shrink_candidate:
    case PageFilespaceAgentRequestKind::truncate_filespace:
      if (request.responding_agent == "page_allocation_manager") {
        return refuse(PageFilespaceAgentBoundaryViolation::page_agent_manages_files,
                      "page_filespace_agent_boundary_page_manages_files",
                      "storage.page.filespace_agent.boundary_page_manages_files",
                      "filespace capacity manager owns file extension, shrink, and truncate");
      }
      decision.filespace_agent_action_required = true;
      break;
    case PageFilespaceAgentRequestKind::deny_request:
      return refuse(PageFilespaceAgentBoundaryViolation::missing_evidence,
                    "page_filespace_agent_denied",
                    "storage.page.filespace_agent.denied",
                    "request is explicitly denied by policy");
  }

  if (decision.filespace_agent_action_required &&
      ((request.kind == PageFilespaceAgentRequestKind::extend_filespace && !policy.filespace_agent_may_grow_files) ||
       ((request.kind == PageFilespaceAgentRequestKind::shrink_candidate ||
         request.kind == PageFilespaceAgentRequestKind::truncate_filespace) &&
        !policy.filespace_agent_may_shrink_files))) {
    return refuse(PageFilespaceAgentBoundaryViolation::missing_policy,
                  "page_filespace_agent_policy_disallows_filespace_action",
                  "storage.page.filespace_agent.policy_disallows_filespace_action",
                  "policy disallows requested filespace capacity action");
  }
  if (decision.page_agent_action_required &&
      ((request.kind == PageFilespaceAgentRequestKind::reserve_pages && !policy.page_agent_may_allocate_pages) ||
       (request.kind == PageFilespaceAgentRequestKind::relocate_pages && !policy.page_agent_may_relocate_pages))) {
    return refuse(PageFilespaceAgentBoundaryViolation::missing_policy,
                  "page_filespace_agent_policy_disallows_page_action",
                  "storage.page.filespace_agent.policy_disallows_page_action",
                  "policy disallows requested page allocation action");
  }

  decision.status = HandoffOkStatus();
  decision.state = decision.filespace_agent_action_required
                       ? PageFilespaceAgentRequestState::waiting_filespace_agent
                       : PageFilespaceAgentRequestState::waiting_page_agent;
  decision.violation = PageFilespaceAgentBoundaryViolation::none;
  decision.allowed = true;
  decision.diagnostic = MakePageFilespaceHandoffDiagnostic(decision.status,
                                                          "ok",
                                                          "storage.page.filespace_agent.request_accepted",
                                                          "page/filespace agent request accepted");
  if (decision.filespace_agent_action_required) {
    (void)scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(UuidToString(request.filespace_uuid.value),
                                                                          PageFilespaceAgentRequestKindName(request.kind),
                                                                          "accepted");
  }
  if (decision.page_agent_action_required) {
    (void)scratchbird::core::metrics::RecordPageAllocationAgentRequest(UuidToString(request.filespace_uuid.value),
                                                                       request.page_family.empty() ? "all" : request.page_family,
                                                                       PageFilespaceAgentRequestKindName(request.kind),
                                                                       "accepted");
  }
  return decision;
}

PageFilespaceAgentQueueResult EnqueuePageFilespaceAgentRequest(PageFilespaceAgentRequestQueue* queue,
                                                               const PageFilespaceAgentRequest& request,
                                                               const PageFilespaceAgentRequestPolicy& policy) {
  if (queue == nullptr) {
    return QueueError("page_filespace_agent_queue_missing",
                      "storage.page.filespace_agent.queue_missing",
                      "page/filespace agent request queue is required");
  }

  PageFilespaceAgentRequest normalized = request;
  if (!normalized.policy_uuid.valid() && policy.policy_uuid.valid()) {
    normalized.policy_uuid = policy.policy_uuid;
  }

  if (normalized.request_uuid.valid()) {
    if (const auto* existing = FindQueueRecord(*queue, normalized.request_uuid);
        existing != nullptr && !RequestPayloadMatches(existing->request, normalized)) {
      return QueueError("page_filespace_agent_request_idempotency_conflict",
                        "storage.page.filespace_agent.request_idempotency_conflict",
                        UuidToString(normalized.request_uuid.value));
    }
  }

  if (const auto* existing = FindQueueRecord(*queue, normalized)) {
    PageFilespaceAgentQueueResult result;
    result.status = existing->request.state == PageFilespaceAgentRequestState::refused
                        ? HandoffErrorStatus()
                        : HandoffOkStatus();
    result.record = *existing;
    result.request_uuid = existing->request.request_uuid;
    result.idempotent = true;
    result.diagnostic = MakePageFilespaceHandoffDiagnostic(
        result.status,
        existing->diagnostic_code.empty() ? "page_filespace_agent_request_idempotent"
                                          : existing->diagnostic_code,
        "storage.page.filespace_agent.request_idempotent",
        "existing durable page/filespace agent request returned");
    return result;
  }

  const u64 record_sequence = queue->next_sequence++;
  if (!normalized.request_uuid.valid()) {
    normalized.request_uuid = NewQueueEvidenceId(record_sequence);
  }

  const PageFilespaceAgentDecision decision =
      EvaluatePageFilespaceAgentRequest(normalized, policy);

  PageFilespaceAgentQueueRecord record;
  record.sequence = record_sequence;
  record.request = normalized;
  record.request.state = decision.state;
  record.violation = decision.violation;
  record.allowed = decision.allowed;
  record.filespace_agent_action_required = decision.filespace_agent_action_required;
  record.page_agent_action_required = decision.page_agent_action_required;
  record.target_free_pages = decision.target_free_pages;
  record.low_water_pages = decision.low_water_pages;
  record.diagnostic_code = decision.diagnostic.diagnostic_code;
  record.evidence_state = decision.allowed ? "request_waiting_for_owner"
                                           : "request_refused";
  record.evidence_id = NewQueueEvidenceId(record_sequence + 1000000ull);
  record.explicit_evidence = true;

  PageFilespaceAgentTransitionRecord transition;
  transition.sequence = queue->next_sequence++;
  transition.previous_state = request.state;
  transition.new_state = decision.state;
  transition.evidence_id = record.evidence_id;
  transition.diagnostic_code = record.diagnostic_code;
  transition.reason = request.reason;
  transition.explicit_evidence = true;
  record.transitions.push_back(transition);

  PageFilespaceAgentQueueResult result;
  result.status = decision.status;
  result.decision = decision;
  result.record = record;
  result.request_uuid = record.request.request_uuid;
  result.enqueued = true;
  result.diagnostic = decision.diagnostic;
  queue->records.push_back(record);
  return result;
}

PageFilespaceAgentQueueResult EvaluatePageFilespaceAgentRequest(PageFilespaceAgentRequestQueue* queue,
                                                                const PageFilespaceAgentRequest& request,
                                                                const PageFilespaceAgentRequestPolicy& policy) {
  return EnqueuePageFilespaceAgentRequest(queue, request, policy);
}

PageFilespaceAgentQueueResult TransitionPageFilespaceAgentRequest(PageFilespaceAgentRequestQueue* queue,
                                                                  const TypedUuid& request_uuid,
                                                                  PageFilespaceAgentRequestState new_state,
                                                                  std::string reason,
                                                                  TypedUuid evidence_id) {
  if (queue == nullptr) {
    return QueueError("page_filespace_agent_queue_missing",
                      "storage.page.filespace_agent.queue_missing",
                      "page/filespace agent request queue is required");
  }
  if (!request_uuid.valid()) {
    return QueueError("page_filespace_agent_request_uuid_invalid",
                      "storage.page.filespace_agent.request_uuid_invalid",
                      "request_uuid must be a valid engine UUID");
  }
  if (!IsKnownRequestState(new_state)) {
    return QueueError("page_filespace_agent_invalid_transition",
                      "storage.page.filespace_agent.invalid_transition",
                      "unknown target state");
  }

  PageFilespaceAgentQueueRecord* record = FindQueueRecord(queue, request_uuid);
  if (record == nullptr) {
    return QueueError("page_filespace_agent_request_not_found",
                      "storage.page.filespace_agent.request_not_found",
                      UuidToString(request_uuid.value));
  }

  const PageFilespaceAgentRequestState previous = record->request.state;
  if (previous == new_state) {
    PageFilespaceAgentQueueResult result;
    result.status = HandoffOkStatus();
    result.record = *record;
    result.request_uuid = record->request.request_uuid;
    result.idempotent = true;
    result.diagnostic = MakePageFilespaceHandoffDiagnostic(
        result.status,
        "page_filespace_agent_transition_idempotent",
        "storage.page.filespace_agent.transition_idempotent",
        PageFilespaceAgentRequestStateName(new_state));
    return result;
  }

  if (!IsAllowedRequestTransition(previous, new_state)) {
    return QueueError("page_filespace_agent_invalid_transition",
                      "storage.page.filespace_agent.invalid_transition",
                      std::string(PageFilespaceAgentRequestStateName(previous)) +
                          "->" + PageFilespaceAgentRequestStateName(new_state));
  }

  if (!evidence_id.valid()) {
    evidence_id = NewQueueEvidenceId(queue->next_sequence + 2000000ull);
  }

  PageFilespaceAgentTransitionRecord transition;
  transition.sequence = queue->next_sequence++;
  transition.previous_state = previous;
  transition.new_state = new_state;
  transition.evidence_id = evidence_id;
  transition.diagnostic_code = "ok";
  transition.reason = std::move(reason);
  transition.explicit_evidence = true;

  record->request.state = new_state;
  record->diagnostic_code = "ok";
  record->evidence_state = "state_transition";
  record->evidence_id = evidence_id;
  record->explicit_evidence = true;
  record->transitions.push_back(transition);

  PageFilespaceAgentQueueResult result;
  result.status = HandoffOkStatus();
  result.record = *record;
  result.request_uuid = record->request.request_uuid;
  result.transitioned = true;
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(
      result.status,
      "ok",
      "storage.page.filespace_agent.transition_recorded",
      std::string(PageFilespaceAgentRequestStateName(previous)) +
          "->" + PageFilespaceAgentRequestStateName(new_state));
  return result;
}

PageFilespaceAgentQueueResult TransitionPageFilespaceAgentRequestWithEvidence(
    PageFilespaceAgentRequestQueue* queue,
    const TypedUuid& request_uuid,
    PageFilespaceAgentRequestState new_state,
    std::string reason,
    std::string diagnostic_code,
    std::string evidence_state,
    TypedUuid evidence_id) {
  if (diagnostic_code.empty()) {
    diagnostic_code = "ok";
  }
  if (evidence_state.empty()) {
    evidence_state = "state_transition";
  }

  PageFilespaceAgentQueueResult result =
      TransitionPageFilespaceAgentRequest(queue,
                                          request_uuid,
                                          new_state,
                                          std::move(reason),
                                          evidence_id);
  if (!result.ok() || !result.transitioned) {
    return result;
  }

  PageFilespaceAgentQueueRecord* record = FindQueueRecord(queue, result.request_uuid);
  if (record == nullptr) {
    return result;
  }

  record->diagnostic_code = diagnostic_code;
  record->evidence_state = evidence_state;
  if (!record->transitions.empty()) {
    record->transitions.back().diagnostic_code = diagnostic_code;
  }
  result.record = *record;
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(
      result.status,
      diagnostic_code,
      "storage.page.filespace_agent.transition_recorded",
      evidence_state);
  return result;
}

PageFilespaceAgentQueueResult CancelPageFilespaceAgentRequest(PageFilespaceAgentRequestQueue* queue,
                                                              const TypedUuid& request_uuid,
                                                              std::string reason,
                                                              TypedUuid evidence_id) {
  if (queue == nullptr) {
    return QueueError("page_filespace_agent_queue_missing",
                      "storage.page.filespace_agent.queue_missing",
                      "page/filespace agent request queue is required");
  }
  PageFilespaceAgentQueueRecord* record = FindQueueRecord(queue, request_uuid);
  if (record == nullptr) {
    return QueueError("page_filespace_agent_request_not_found",
                      "storage.page.filespace_agent.request_not_found",
                      request_uuid.valid() ? UuidToString(request_uuid.value) : "invalid");
  }
  if (record->request.state == PageFilespaceAgentRequestState::cancelled ||
      record->request.state == PageFilespaceAgentRequestState::refused ||
      record->request.state == PageFilespaceAgentRequestState::completed) {
    PageFilespaceAgentQueueResult result;
    result.status = HandoffOkStatus();
    result.record = *record;
    result.request_uuid = record->request.request_uuid;
    result.idempotent = true;
    result.cancelled = record->request.state == PageFilespaceAgentRequestState::cancelled;
    result.diagnostic = MakePageFilespaceHandoffDiagnostic(
        result.status,
        "page_filespace_agent_cancel_noop",
        "storage.page.filespace_agent.cancel_noop",
        PageFilespaceAgentRequestStateName(record->request.state));
    return result;
  }
  if (record->request.state == PageFilespaceAgentRequestState::approved ||
      record->request.state == PageFilespaceAgentRequestState::in_flight ||
      record->request.state == PageFilespaceAgentRequestState::recovery_required) {
    return QueueError("page_filespace_agent_cancel_unsafe",
                      "storage.page.filespace_agent.cancel_unsafe",
                      PageFilespaceAgentRequestStateName(record->request.state));
  }

  PageFilespaceAgentQueueResult result =
      TransitionPageFilespaceAgentRequest(queue,
                                          request_uuid,
                                          PageFilespaceAgentRequestState::cancelled,
                                          std::move(reason),
                                          evidence_id);
  result.cancelled = result.ok();
  return result;
}

std::vector<byte> SerializePageFilespaceAgentRequestQueue(const PageFilespaceAgentRequestQueue& queue) {
  std::vector<byte> encoded;
  encoded.insert(encoded.end(), kQueueMagic, kQueueMagic + sizeof(kQueueMagic));
  Store32(&encoded, kQueueVersion);
  Store64(&encoded, queue.next_sequence);
  Store64(&encoded, static_cast<u64>(queue.records.size()));
  for (const auto& record : queue.records) {
    Store64(&encoded, record.sequence);
    StoreUuid(&encoded, record.request.request_uuid);
    StoreUuid(&encoded, record.request.database_uuid);
    StoreUuid(&encoded, record.request.filespace_uuid);
    StoreUuid(&encoded, record.request.policy_uuid);
    Store32(&encoded, static_cast<u32>(record.request.kind));
    Store32(&encoded, static_cast<u32>(record.request.state));
    StoreString(&encoded, record.request.requesting_agent);
    StoreString(&encoded, record.request.responding_agent);
    StoreString(&encoded, record.request.page_family);
    Store64(&encoded, record.request.requested_pages);
    Store64(&encoded, record.request.released_free_pages);
    Store64(&encoded, record.request.target_reserve_pages);
    Store64(&encoded, record.request.threshold_pages);
    Store64(&encoded, record.request.free_pages);
    Store64(&encoded, record.request.preallocated_pages);
    Store64(&encoded, record.request.allocated_pages);
    Store64(&encoded, record.request.reserved_pages);
    Store64(&encoded, record.request.relocated_pages);
    Store64(&encoded, record.request.pinned_pages);
    Store64(&encoded, record.request.active_pages);
    StoreString(&encoded, record.request.reason);
    Store32(&encoded, static_cast<u32>(record.violation));
    StoreBool(&encoded, record.allowed);
    StoreBool(&encoded, record.filespace_agent_action_required);
    StoreBool(&encoded, record.page_agent_action_required);
    Store64(&encoded, record.target_free_pages);
    Store64(&encoded, record.low_water_pages);
    StoreString(&encoded, record.diagnostic_code);
    StoreString(&encoded, record.evidence_state);
    StoreUuid(&encoded, record.evidence_id);
    StoreBool(&encoded, record.explicit_evidence);
    Store64(&encoded, static_cast<u64>(record.transitions.size()));
    for (const auto& transition : record.transitions) {
      Store64(&encoded, transition.sequence);
      Store32(&encoded, static_cast<u32>(transition.previous_state));
      Store32(&encoded, static_cast<u32>(transition.new_state));
      StoreUuid(&encoded, transition.evidence_id);
      StoreString(&encoded, transition.diagnostic_code);
      StoreString(&encoded, transition.reason);
      StoreBool(&encoded, transition.explicit_evidence);
    }
  }
  return encoded;
}

PageFilespaceAgentQueueRestoreResult RestorePageFilespaceAgentRequestQueue(const std::vector<byte>& encoded) {
  QueueReader reader{encoded};
  if (!reader.Ensure(sizeof(kQueueMagic), "magic")) {
    return RestoreError("page_filespace_agent_queue_restore_malformed",
                        "storage.page.filespace_agent.queue_restore_malformed",
                        reader.detail);
  }
  for (byte value : kQueueMagic) {
    if (reader.Read8("magic") != value) {
      return RestoreError("page_filespace_agent_queue_restore_malformed",
                          "storage.page.filespace_agent.queue_restore_malformed",
                          "bad magic");
    }
  }
  const u32 version = reader.Read32("version");
  if (reader.failed || version < kQueueMinVersion || version > kQueueVersion) {
    return RestoreError("page_filespace_agent_queue_restore_malformed",
                        "storage.page.filespace_agent.queue_restore_malformed",
                        reader.failed ? reader.detail : "unsupported version");
  }

  PageFilespaceAgentQueueRestoreResult result;
  result.status = HandoffOkStatus();
  result.queue.next_sequence = reader.Read64("next_sequence");
  const u64 record_count = reader.Read64("record_count");
  if (reader.failed || result.queue.next_sequence == 0) {
    return RestoreError("page_filespace_agent_queue_restore_malformed",
                        "storage.page.filespace_agent.queue_restore_malformed",
                        reader.failed ? reader.detail : "next_sequence");
  }
  if (record_count > kMaxQueueRecords) {
    return RestoreError("page_filespace_agent_queue_restore_invalid_record",
                        "storage.page.filespace_agent.queue_restore_invalid_record",
                        "record_count");
  }
  result.queue.records.reserve(static_cast<std::size_t>(record_count));
  u64 max_sequence = 0;
  for (u64 index = 0; index < record_count; ++index) {
    PageFilespaceAgentQueueRecord record;
    record.sequence = reader.Read64("record.sequence");
    record.request.request_uuid = reader.ReadUuid("request.uuid");
    record.request.database_uuid = reader.ReadUuid("request.database_uuid");
    record.request.filespace_uuid = reader.ReadUuid("request.filespace_uuid");
    record.request.policy_uuid = reader.ReadUuid("request.policy_uuid");
    record.request.kind = static_cast<PageFilespaceAgentRequestKind>(reader.Read32("request.kind"));
    record.request.state = static_cast<PageFilespaceAgentRequestState>(reader.Read32("request.state"));
    record.request.requesting_agent = reader.ReadString("request.requesting_agent");
    record.request.responding_agent = reader.ReadString("request.responding_agent");
    record.request.page_family = reader.ReadString("request.page_family");
    record.request.requested_pages = reader.Read64("request.requested_pages");
    if (version >= 2) {
      record.request.released_free_pages = reader.Read64("request.released_free_pages");
      record.request.target_reserve_pages = reader.Read64("request.target_reserve_pages");
      record.request.threshold_pages = reader.Read64("request.threshold_pages");
      record.request.free_pages = reader.Read64("request.free_pages");
      record.request.preallocated_pages = reader.Read64("request.preallocated_pages");
      record.request.allocated_pages = reader.Read64("request.allocated_pages");
      record.request.reserved_pages = reader.Read64("request.reserved_pages");
      record.request.relocated_pages = reader.Read64("request.relocated_pages");
      record.request.pinned_pages = reader.Read64("request.pinned_pages");
      record.request.active_pages = reader.Read64("request.active_pages");
    } else {
      record.request.free_pages = reader.Read64("request.free_pages");
      record.request.preallocated_pages = reader.Read64("request.preallocated_pages");
      record.request.allocated_pages = reader.Read64("request.allocated_pages");
      record.request.pinned_pages = reader.Read64("request.pinned_pages");
      record.request.active_pages = reader.Read64("request.active_pages");
    }
    record.request.reason = reader.ReadString("request.reason");
    record.violation = static_cast<PageFilespaceAgentBoundaryViolation>(reader.Read32("violation"));
    record.allowed = reader.ReadBool("allowed");
    record.filespace_agent_action_required = reader.ReadBool("filespace_agent_action_required");
    record.page_agent_action_required = reader.ReadBool("page_agent_action_required");
    record.target_free_pages = reader.Read64("target_free_pages");
    record.low_water_pages = reader.Read64("low_water_pages");
    record.diagnostic_code = reader.ReadString("diagnostic_code");
    record.evidence_state = reader.ReadString("evidence_state");
    record.evidence_id = reader.ReadUuid("evidence_id");
    record.explicit_evidence = reader.ReadBool("explicit_evidence");
    const u64 transition_count = reader.Read64("transition_count");
    if (reader.failed) {
      return RestoreError("page_filespace_agent_queue_restore_malformed",
                          "storage.page.filespace_agent.queue_restore_malformed",
                          reader.detail);
    }
    if (transition_count > kMaxQueueTransitionsPerRecord) {
      return RestoreError("page_filespace_agent_queue_restore_invalid_record",
                          "storage.page.filespace_agent.queue_restore_invalid_record",
                          "transition_count");
    }
    record.transitions.reserve(static_cast<std::size_t>(transition_count));
    max_sequence = std::max(max_sequence, record.sequence);
    for (u64 transition_index = 0; transition_index < transition_count; ++transition_index) {
      PageFilespaceAgentTransitionRecord transition;
      transition.sequence = reader.Read64("transition.sequence");
      transition.previous_state =
          static_cast<PageFilespaceAgentRequestState>(reader.Read32("transition.previous_state"));
      transition.new_state =
          static_cast<PageFilespaceAgentRequestState>(reader.Read32("transition.new_state"));
      transition.evidence_id = reader.ReadUuid("transition.evidence_id");
      transition.diagnostic_code = reader.ReadString("transition.diagnostic_code");
      transition.reason = reader.ReadString("transition.reason");
      transition.explicit_evidence = reader.ReadBool("transition.explicit_evidence");
      if (reader.failed) {
        return RestoreError("page_filespace_agent_queue_restore_malformed",
                            "storage.page.filespace_agent.queue_restore_malformed",
                            reader.detail);
      }
      if (!IsKnownRequestState(transition.previous_state) ||
          !IsKnownRequestState(transition.new_state)) {
        return RestoreError("page_filespace_agent_queue_restore_invalid_record",
                            "storage.page.filespace_agent.queue_restore_invalid_record",
                            "transition state");
      }
      if (transition.explicit_evidence && !transition.evidence_id.valid()) {
        return RestoreError("page_filespace_agent_queue_restore_invalid_record",
                            "storage.page.filespace_agent.queue_restore_invalid_record",
                            "transition evidence");
      }
      max_sequence = std::max(max_sequence, transition.sequence);
      record.transitions.push_back(transition);
    }

    const bool policy_valid_or_refused_missing =
        record.request.policy_uuid.valid() ||
        (record.request.state == PageFilespaceAgentRequestState::refused &&
         record.violation == PageFilespaceAgentBoundaryViolation::missing_policy);
    if (!record.request.request_uuid.valid() ||
        !record.request.database_uuid.valid() ||
        !record.request.filespace_uuid.valid() ||
        !policy_valid_or_refused_missing ||
        !IsKnownRequestKind(record.request.kind) ||
        !IsKnownRequestState(record.request.state) ||
        !IsKnownBoundaryViolation(record.violation) ||
        record.request.requesting_agent.empty() ||
        record.request.responding_agent.empty() ||
        (record.explicit_evidence && !record.evidence_id.valid())) {
      return RestoreError("page_filespace_agent_queue_restore_invalid_record",
                          "storage.page.filespace_agent.queue_restore_invalid_record",
                          "required request field");
    }
    if (!record.transitions.empty()) {
      PageFilespaceAgentRequestState chain_state = record.transitions.front().previous_state;
      for (const auto& transition : record.transitions) {
        if (transition.previous_state != chain_state ||
            !IsAllowedRequestTransition(transition.previous_state, transition.new_state)) {
          return RestoreError("page_filespace_agent_queue_restore_invalid_record",
                              "storage.page.filespace_agent.queue_restore_invalid_record",
                              "transition chain");
        }
        chain_state = transition.new_state;
      }
      if (chain_state != record.request.state) {
        return RestoreError("page_filespace_agent_queue_restore_invalid_record",
                            "storage.page.filespace_agent.queue_restore_invalid_record",
                            "transition final state");
      }
    }
    result.queue.records.push_back(record);
  }
  if (reader.failed || reader.offset != encoded.size()) {
    return RestoreError("page_filespace_agent_queue_restore_malformed",
                        "storage.page.filespace_agent.queue_restore_malformed",
                        reader.failed ? reader.detail : "trailing bytes");
  }
  if (result.queue.next_sequence <= max_sequence) {
    return RestoreError("page_filespace_agent_queue_restore_invalid_record",
                        "storage.page.filespace_agent.queue_restore_invalid_record",
                        "next_sequence");
  }
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(result.status,
                                                        "ok",
                                                        "storage.page.filespace_agent.queue_restored",
                                                        "page/filespace agent request queue restored");
  return result;
}

PageFilespaceAgentRequestRecoveryResult ClassifyPageFilespaceAgentRequestQueueForRecovery(
    const PageFilespaceAgentRequestQueue& queue) {
  PageFilespaceAgentRequestRecoveryResult result;
  result.status = HandoffOkStatus();
  result.classifications.reserve(queue.records.size());
  bool any_fail_closed = false;
  for (const auto& record : queue.records) {
    PageFilespaceAgentRequestRecoveryClassification classification;
    classification.request_uuid = record.request.request_uuid;
    classification.sequence = record.sequence;
    classification.observed_state = record.request.state;

    if (!record.request.request_uuid.valid()) {
      classification.action = PageFilespaceAgentRequestRecoveryAction::fail_closed;
      classification.fail_closed = true;
      classification.stable_reason = "request UUID missing";
      classification.diagnostic_code = "page_filespace_agent_recovery_invalid_record";
    } else if (IsTerminalRequestState(record.request.state)) {
      if (HasQueueEvidence(record)) {
        classification.action = PageFilespaceAgentRequestRecoveryAction::no_action;
        classification.fail_closed = false;
        classification.stable_reason = "terminal request state has no restart mutation";
        classification.diagnostic_code = "ok";
      } else {
        classification.action = PageFilespaceAgentRequestRecoveryAction::fail_closed;
        classification.fail_closed = true;
        classification.stable_reason = "terminal request lacks explicit durable evidence";
        classification.diagnostic_code = "page_filespace_agent_recovery_evidence_required";
      }
    } else if (IsWaitingRequestState(record.request.state)) {
      if (HasQueueEvidence(record)) {
        classification.action = PageFilespaceAgentRequestRecoveryAction::resume_revalidate;
        classification.fail_closed = false;
        classification.stable_reason = "waiting request has explicit durable evidence and must be revalidated";
        classification.diagnostic_code = "ok";
      } else {
        classification.action = PageFilespaceAgentRequestRecoveryAction::fail_closed;
        classification.fail_closed = true;
        classification.stable_reason = "waiting request lacks explicit durable evidence";
        classification.diagnostic_code = "page_filespace_agent_recovery_evidence_required";
      }
    } else if (record.request.state == PageFilespaceAgentRequestState::approved ||
               record.request.state == PageFilespaceAgentRequestState::in_flight ||
               record.request.state == PageFilespaceAgentRequestState::recovery_required) {
      classification.action = PageFilespaceAgentRequestRecoveryAction::fail_closed;
      classification.fail_closed = true;
      classification.stable_reason = "approved or in-flight request may represent partial owner work";
      classification.diagnostic_code = "page_filespace_agent_recovery_partial_state";
    } else {
      classification.action = PageFilespaceAgentRequestRecoveryAction::fail_closed;
      classification.fail_closed = true;
      classification.stable_reason = "unsettled request state must be re-created by owner evidence";
      classification.diagnostic_code = "page_filespace_agent_recovery_unsettled_state";
    }

    any_fail_closed = any_fail_closed || classification.fail_closed;
    result.classifications.push_back(classification);
  }

  result.status = any_fail_closed ? HandoffErrorStatus() : HandoffOkStatus();
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(
      result.status,
      any_fail_closed ? "page_filespace_agent_queue_recovery_fail_closed" : "ok",
      any_fail_closed ? "storage.page.filespace_agent.queue_recovery_fail_closed"
                      : "storage.page.filespace_agent.queue_recovery_classified",
      any_fail_closed ? "one or more durable request records failed closed"
                      : "page/filespace agent request queue classified");
  return result;
}

PageFilespaceHandoffResult NotifyFilespaceLowReserve(PageFilespaceHandoffLedger* ledger,
                                                     const PageFilespaceLowReserveEvent& event) {
  if (ledger == nullptr) {
    return Refuse(nullptr,
                  PageFilespaceHandoffKind::low_reserve,
                  event.database_uuid,
                  event.filespace_uuid,
                  event.policy.policy_uuid,
                  event.page_family,
                  "page_filespace_handoff_missing_ledger",
                  "storage.page.filespace_handoff.missing_ledger",
                  "page/filespace handoff ledger is required");
  }
  if (!event.database_uuid.valid()) {
    return Refuse(ledger,
                  PageFilespaceHandoffKind::low_reserve,
                  event.database_uuid,
                  event.filespace_uuid,
                  event.policy.policy_uuid,
                  event.page_family,
                  "page_filespace_handoff_invalid_database_uuid",
                  "storage.page.filespace_handoff.invalid_database_uuid",
                  "database_uuid must be a valid engine UUID");
  }
  if (!event.filespace_uuid.valid()) {
    return Refuse(ledger,
                  PageFilespaceHandoffKind::low_reserve,
                  event.database_uuid,
                  event.filespace_uuid,
                  event.policy.policy_uuid,
                  event.page_family,
                  "page_filespace_handoff_invalid_filespace_uuid",
                  "storage.page.filespace_handoff.invalid_filespace_uuid",
                  "filespace_uuid must be a valid engine UUID");
  }
  if (!IsKnownInsertPageFamily(event.page_family)) {
    return Refuse(ledger,
                  PageFilespaceHandoffKind::low_reserve,
                  event.database_uuid,
                  event.filespace_uuid,
                  event.policy.policy_uuid,
                  event.page_family,
                  "page_filespace_handoff_unknown_page_family",
                  "storage.page.filespace_handoff.unknown_page_family",
                  event.page_family);
  }

  const bool notify = ShouldNotifyFilespaceLowReserve(event);
  PageFilespaceHandoffResult result;
  result.status = notify ? HandoffOkStatus() : HandoffErrorStatus();
  result.notified = notify;
  result.advisory = event.policy.advisory_events;
  result.evidence = LowReserveEvidence(ledger, event, notify);
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(
      result.status,
      notify ? "ok" : "page_filespace_low_reserve_threshold_not_met",
      notify ? "storage.page.filespace_handoff.low_reserve_recorded"
             : "storage.page.filespace_handoff.low_reserve_threshold_not_met",
      notify ? "low reserve notification recorded" : "released/free pages remain above half target");

  ledger->evidence.push_back(result.evidence);
  (void)scratchbird::core::metrics::PublishFilespaceAgentFreeReservePages(
      static_cast<double>(event.released_free_pages),
      UuidToString(event.filespace_uuid.value),
      notify ? "below_half_target" : "above_half_target");
  if (notify) {
    (void)scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(UuidToString(event.filespace_uuid.value),
                                                                          "low_reserve",
                                                                          "notified");
  }
  return result;
}

PageFilespaceHandoffResult NotifyFilespaceLowReserve(PageFilespaceAgentRequestQueue* queue,
                                                     const PageFilespaceLowReserveEvent& event,
                                                     const PageFilespaceAgentRequestPolicy& policy) {
  const TypedUuid policy_uuid = SelectPolicyUuid(event.policy.policy_uuid, policy);
  if (queue == nullptr) {
    return HandoffInputError(PageFilespaceHandoffKind::low_reserve,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_agent_queue_missing",
                             "storage.page.filespace_agent.queue_missing",
                             "page/filespace agent request queue is required");
  }
  if (!event.database_uuid.valid()) {
    return HandoffInputError(PageFilespaceHandoffKind::low_reserve,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_handoff_invalid_database_uuid",
                             "storage.page.filespace_handoff.invalid_database_uuid",
                             "database_uuid must be a valid engine UUID");
  }
  if (!event.filespace_uuid.valid()) {
    return HandoffInputError(PageFilespaceHandoffKind::low_reserve,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_handoff_invalid_filespace_uuid",
                             "storage.page.filespace_handoff.invalid_filespace_uuid",
                             "filespace_uuid must be a valid engine UUID");
  }
  if (!IsKnownInsertPageFamily(event.page_family)) {
    return HandoffInputError(PageFilespaceHandoffKind::low_reserve,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_handoff_unknown_page_family",
                             "storage.page.filespace_handoff.unknown_page_family",
                             event.page_family);
  }

  if (!ShouldNotifyFilespaceLowReserve(event)) {
    PageFilespaceHandoffResult result;
    result.status = HandoffErrorStatus();
    result.queue_backed = true;
    result.advisory = false;
    result.evidence.kind = PageFilespaceHandoffKind::low_reserve;
    result.evidence.previous_state = PageFilespaceHandoffState::absent;
    result.evidence.new_state = PageFilespaceHandoffState::absent;
    result.evidence.database_uuid = event.database_uuid;
    result.evidence.filespace_uuid = event.filespace_uuid;
    result.evidence.policy_uuid = policy_uuid;
    result.evidence.request_kind = PageFilespaceAgentRequestKind::extend_filespace;
    result.evidence.page_family = event.page_family;
    result.evidence.requested_pages = RequestedLowReservePages(event);
    result.evidence.released_free_pages = event.released_free_pages;
    result.evidence.target_reserve_pages = NormalizeFilespaceTargetReservePages(event.policy);
    result.evidence.threshold_pages = FilespaceLowReserveThresholdPages(event.policy);
    result.evidence.allocated_pages = event.allocated_pages;
    result.evidence.reserved_pages = event.reserved_pages;
    result.evidence.reason = event.reason;
    result.evidence.diagnostic_code = "page_filespace_low_reserve_threshold_not_met";
    result.evidence.advisory = false;
    result.evidence.durable_state_changed = false;
    result.diagnostic = MakePageFilespaceHandoffDiagnostic(
        result.status,
        "page_filespace_low_reserve_threshold_not_met",
        "storage.page.filespace_handoff.low_reserve_threshold_not_met",
        "released/free pages remain above half target");
    return result;
  }

  PageFilespaceAgentQueueResult queued =
      EnqueuePageFilespaceAgentRequest(queue, LowReserveCapacityRequest(event, policy), policy);
  if (queued.status.ok()) {
    OverrideQueuedHandoffEvidence(queue, &queued, "ok", "low_reserve_extend_requested");
  }

  const bool queued_ok = queued.status.ok();
  const std::string diagnostic_code =
      queued_ok ? "ok" : queued.record.diagnostic_code;
  auto result = QueueBackedHandoffResult(
      std::move(queued),
      PageFilespaceHandoffKind::low_reserve,
      true,
      false,
      diagnostic_code.empty() ? "page_filespace_agent_request_refused" : diagnostic_code,
      queued_ok ? "storage.page.filespace_handoff.low_reserve_queued"
                : "storage.page.filespace_handoff.low_reserve_refused",
      queued_ok ? "low reserve filespace capacity request queued"
                : "low reserve filespace capacity request refused");
  if (result.ok()) {
    (void)scratchbird::core::metrics::PublishFilespaceAgentFreeReservePages(
        static_cast<double>(event.released_free_pages),
        UuidToString(event.filespace_uuid.value),
        "below_half_target");
    (void)scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(UuidToString(event.filespace_uuid.value),
                                                                          "low_reserve",
                                                                          "queued");
  }
  return result;
}

PageFilespaceHandoffResult NotifyFilespaceShrinkReady(PageFilespaceHandoffLedger* ledger,
                                                      const PageFilespaceShrinkReadyEvent& event) {
  if (ledger == nullptr) {
    return Refuse(nullptr,
                  PageFilespaceHandoffKind::shrink_ready,
                  event.database_uuid,
                  event.filespace_uuid,
                  event.policy_uuid,
                  event.page_family,
                  "page_filespace_handoff_missing_ledger",
                  "storage.page.filespace_handoff.missing_ledger",
                  "page/filespace handoff ledger is required");
  }
  if (!event.database_uuid.valid() || !event.filespace_uuid.valid()) {
    return Refuse(ledger,
                  PageFilespaceHandoffKind::shrink_ready,
                  event.database_uuid,
                  event.filespace_uuid,
                  event.policy_uuid,
                  event.page_family,
                  "page_filespace_handoff_invalid_identity",
                  "storage.page.filespace_handoff.invalid_identity",
                  "database_uuid and filespace_uuid must be valid engine UUIDs");
  }
  if (!IsKnownInsertPageFamily(event.page_family)) {
    return Refuse(ledger,
                  PageFilespaceHandoffKind::shrink_ready,
                  event.database_uuid,
                  event.filespace_uuid,
                  event.policy_uuid,
                  event.page_family,
                  "page_filespace_handoff_unknown_page_family",
                  "storage.page.filespace_handoff.unknown_page_family",
                  event.page_family);
  }

  const bool blocked = event.pinned_pages > 0 || event.active_pages > 0 || event.reserved_pages > 0;
  PageFilespaceHandoffEvidenceRecord evidence;
  evidence.sequence = ledger->next_evidence_sequence++;
  evidence.kind = blocked ? PageFilespaceHandoffKind::shrink_blocked : PageFilespaceHandoffKind::shrink_ready;
  evidence.previous_state = PageFilespaceHandoffState::absent;
  evidence.new_state = PageFilespaceHandoffState::advisory_recorded;
  evidence.evidence_id = NewEvidenceId(ledger);
  evidence.database_uuid = event.database_uuid;
  evidence.filespace_uuid = event.filespace_uuid;
  evidence.policy_uuid = event.policy_uuid;
  evidence.request_kind = blocked ? PageFilespaceAgentRequestKind::deny_request
                                  : PageFilespaceAgentRequestKind::truncate_filespace;
  evidence.page_family = event.page_family;
  evidence.requested_pages = event.relocated_pages;
  evidence.relocated_pages = event.relocated_pages;
  evidence.pinned_pages = event.pinned_pages;
  evidence.active_pages = event.active_pages;
  evidence.reserved_pages = event.reserved_pages;
  evidence.reason = event.reason;
  evidence.diagnostic_code = blocked ? "page_filespace_shrink_blocked" : "ok";
  evidence.advisory = true;
  evidence.durable_state_changed = false;

  PageFilespaceHandoffResult result;
  result.status = blocked ? HandoffErrorStatus() : HandoffOkStatus();
  result.notified = !blocked;
  result.shrink_ready = !blocked;
  result.advisory = true;
  result.evidence = evidence;
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(
      result.status,
      blocked ? "page_filespace_shrink_blocked" : "ok",
      blocked ? "storage.page.filespace_handoff.shrink_blocked"
              : "storage.page.filespace_handoff.shrink_ready_recorded",
      blocked ? "pinned, active, or reserved pages remain" : "shrink-ready notification recorded");

  ledger->evidence.push_back(result.evidence);
  (void)scratchbird::core::metrics::RecordPageAllocationAgentRelocatedPages(
      static_cast<double>(event.relocated_pages),
      UuidToString(event.filespace_uuid.value),
      event.page_family,
      blocked ? "blocked" : "ready");
  return result;
}

PageFilespaceHandoffResult NotifyFilespaceShrinkReady(PageFilespaceAgentRequestQueue* queue,
                                                      const PageFilespaceShrinkReadyEvent& event,
                                                      const PageFilespaceAgentRequestPolicy& policy) {
  const TypedUuid policy_uuid = SelectPolicyUuid(event.policy_uuid, policy);
  if (ShrinkEventBlocked(event)) {
    return NotifyFilespaceShrinkBlocked(queue, event, policy);
  }
  if (queue == nullptr) {
    return HandoffInputError(PageFilespaceHandoffKind::shrink_ready,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_agent_queue_missing",
                             "storage.page.filespace_agent.queue_missing",
                             "page/filespace agent request queue is required");
  }
  if (!event.database_uuid.valid() || !event.filespace_uuid.valid()) {
    return HandoffInputError(PageFilespaceHandoffKind::shrink_ready,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_handoff_invalid_identity",
                             "storage.page.filespace_handoff.invalid_identity",
                             "database_uuid and filespace_uuid must be valid engine UUIDs");
  }
  if (!IsKnownInsertPageFamily(event.page_family)) {
    return HandoffInputError(PageFilespaceHandoffKind::shrink_ready,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_handoff_unknown_page_family",
                             "storage.page.filespace_handoff.unknown_page_family",
                             event.page_family);
  }

  PageFilespaceAgentQueueResult queued = EnqueuePageFilespaceAgentRequest(
      queue,
      ShrinkCapacityRequest(event, policy, PageFilespaceAgentRequestKind::truncate_filespace),
      policy);
  if (queued.status.ok()) {
    OverrideQueuedHandoffEvidence(queue, &queued, "ok", "shrink_ready_truncate_requested");
  }

  const bool queued_ok = queued.status.ok();
  const std::string diagnostic_code =
      queued_ok ? "ok" : queued.record.diagnostic_code;
  auto result = QueueBackedHandoffResult(
      std::move(queued),
      PageFilespaceHandoffKind::shrink_ready,
      true,
      true,
      diagnostic_code.empty() ? "page_filespace_agent_request_refused" : diagnostic_code,
      queued_ok ? "storage.page.filespace_handoff.shrink_ready_queued"
                : "storage.page.filespace_handoff.shrink_ready_refused",
      queued_ok ? "shrink-ready filespace capacity request queued"
                : "shrink-ready filespace capacity request refused");
  if (result.ok()) {
    (void)scratchbird::core::metrics::RecordPageAllocationAgentRelocatedPages(
        static_cast<double>(event.relocated_pages),
        UuidToString(event.filespace_uuid.value),
        event.page_family,
        "ready_queued");
  }
  return result;
}

PageFilespaceHandoffResult NotifyFilespaceShrinkBlocked(PageFilespaceAgentRequestQueue* queue,
                                                        const PageFilespaceShrinkReadyEvent& event,
                                                        const PageFilespaceAgentRequestPolicy& policy) {
  const TypedUuid policy_uuid = SelectPolicyUuid(event.policy_uuid, policy);
  if (queue == nullptr) {
    return HandoffInputError(PageFilespaceHandoffKind::shrink_blocked,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_agent_queue_missing",
                             "storage.page.filespace_agent.queue_missing",
                             "page/filespace agent request queue is required");
  }
  if (!event.database_uuid.valid() || !event.filespace_uuid.valid()) {
    return HandoffInputError(PageFilespaceHandoffKind::shrink_blocked,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_handoff_invalid_identity",
                             "storage.page.filespace_handoff.invalid_identity",
                             "database_uuid and filespace_uuid must be valid engine UUIDs");
  }
  if (!IsKnownInsertPageFamily(event.page_family)) {
    return HandoffInputError(PageFilespaceHandoffKind::shrink_blocked,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_handoff_unknown_page_family",
                             "storage.page.filespace_handoff.unknown_page_family",
                             event.page_family);
  }
  if (!ShrinkEventBlocked(event)) {
    return HandoffInputError(PageFilespaceHandoffKind::shrink_blocked,
                             event.database_uuid,
                             event.filespace_uuid,
                             policy_uuid,
                             event.page_family,
                             "page_filespace_shrink_blocker_evidence_missing",
                             "storage.page.filespace_handoff.shrink_blocker_evidence_missing",
                             "shrink-blocked handoff requires pinned, active, or reserved page blockers");
  }

  PageFilespaceAgentQueueResult queued = EnqueuePageFilespaceAgentRequest(
      queue,
      ShrinkCapacityRequest(event, policy, PageFilespaceAgentRequestKind::deny_request),
      policy);
  OverrideQueuedHandoffEvidence(queue,
                                &queued,
                                "page_filespace_shrink_blocked",
                                "shrink_blocked_refused");
  auto result = QueueBackedHandoffResult(
      std::move(queued),
      PageFilespaceHandoffKind::shrink_blocked,
      false,
      false,
      "page_filespace_shrink_blocked",
      "storage.page.filespace_handoff.shrink_blocked",
      "pinned, active, or reserved pages remain");
  (void)scratchbird::core::metrics::RecordPageAllocationAgentRelocatedPages(
      static_cast<double>(event.relocated_pages),
      UuidToString(event.filespace_uuid.value),
      event.page_family,
      "blocked_queued");
  return result;
}

PageFilespaceHandoffRecoveryResult ClassifyPageFilespaceHandoffLedgerForRecovery(
    const PageFilespaceHandoffLedger& ledger) {
  PageFilespaceHandoffRecoveryResult result;
  result.status = HandoffOkStatus();
  result.diagnostic = MakePageFilespaceHandoffDiagnostic(result.status,
                                                        "ok",
                                                        "storage.page.filespace_handoff.recovery_classified",
                                                        "page/filespace handoff ledger classified");
  result.classifications.reserve(ledger.evidence.size());
  for (const auto& evidence : ledger.evidence) {
    PageFilespaceHandoffRecoveryClassification classification;
    classification.evidence_id = evidence.evidence_id;
    classification.kind = evidence.kind;
    classification.observed_state = evidence.new_state;
    if (evidence.new_state == PageFilespaceHandoffState::durable_recorded) {
      classification.action = PageFilespaceHandoffRecoveryAction::reconcile_durable;
      classification.fail_closed = false;
      classification.stable_reason = "durable handoff evidence must be reconciled";
    } else if (evidence.new_state == PageFilespaceHandoffState::advisory_recorded) {
      classification.action = PageFilespaceHandoffRecoveryAction::replay_advisory;
      classification.fail_closed = false;
      classification.stable_reason = "advisory handoff may be recomputed from page/filespace state";
    } else if (evidence.new_state == PageFilespaceHandoffState::refused ||
               evidence.new_state == PageFilespaceHandoffState::absent) {
      classification.action = PageFilespaceHandoffRecoveryAction::no_action;
      classification.fail_closed = false;
      classification.stable_reason = "refused or absent handoff has no restart mutation";
    } else {
      classification.action = PageFilespaceHandoffRecoveryAction::fail_closed;
      classification.fail_closed = true;
      classification.stable_reason = "unknown handoff state fails closed";
    }
    result.classifications.push_back(classification);
  }
  return result;
}

DiagnosticRecord MakePageFilespaceHandoffDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(status.code,
                                                     status.severity,
                                                     status.subsystem,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(arguments),
                                                     {},
                                                     "storage.page.filespace_handoff",
                                                     status.ok() ? "" : "do not mutate filespace from page handoff; retry after page/filespace authority is available");
}

}  // namespace scratchbird::storage::page
