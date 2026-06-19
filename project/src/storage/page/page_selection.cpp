// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_selection.hpp"

#include "metric_producer.hpp"
#include "page_registry.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::uuid::UuidToString;

Status SelectionOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status SelectionErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool IsKnownInsertPageFamily(const std::string& page_family) {
  return IsKnownPageFamilyName(page_family) || page_family == "overflow" || page_family == "toast";
}

bool StartupModeAllowsSelection(InsertReservationStartupMode mode) {
  return mode == InsertReservationStartupMode::normal;
}

std::string MakeFence(const InsertPageCandidate& candidate, u64 sequence) {
  return UuidToString(candidate.page_uuid.value) + ":" +
         std::to_string(candidate.page_generation) + ":" +
         std::to_string(sequence);
}

void EmitMetric(const std::string& family,
                const std::string& result,
                const std::string& reason,
                const std::string& page_family) {
  (void)scratchbird::core::metrics::IncrementCounter(
      family,
      scratchbird::core::metrics::Labels({{"component", "storage.page_selection"},
                                          {"result", result},
                                          {"reason", reason},
                                          {"page_family", page_family}}),
      1.0,
      "engine_insert");
}

InsertPageSelectionEvidenceRecord MakeEvidence(PageSelectionLedger* ledger,
                                               const InsertPageSelectionEntry& before,
                                               const InsertPageSelectionEntry& after,
                                               std::string action,
                                               std::string reason,
                                               std::string diagnostic_code) {
  InsertPageSelectionEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.action = std::move(action);
  evidence.selection_fence = after.selection_fence;
  evidence.database_uuid = after.database_uuid;
  evidence.transaction_uuid = after.transaction_uuid;
  evidence.local_transaction_id = after.local_transaction_id;
  evidence.object_uuid = after.object_uuid;
  evidence.filespace_uuid = after.filespace_uuid;
  evidence.page_uuid = after.page_uuid;
  evidence.reservation_id = after.reservation_id;
  evidence.page_family = after.page_family;
  evidence.previous_state = before.state;
  evidence.new_state = after.state;
  evidence.page_number = after.page_number;
  evidence.page_generation = after.page_generation;
  evidence.encoded_row_bytes = after.encoded_row_bytes;
  evidence.free_bytes_before = after.free_bytes_before;
  evidence.free_bytes_after = after.free_bytes_after;
  evidence.reason = std::move(reason);
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.durable_state_changed = true;
  if (ledger != nullptr) {
    ledger->evidence.push_back(evidence);
  }
  return evidence;
}

InsertPageSelectionResult SelectionError(PageSelectionLedger* ledger,
                                         const InsertPageSelectionRequest& request,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         bool retryable = false,
                                         std::string detail = {}) {
  if (retryable) {
    EmitMetric("sb_page_insert_page_full_retry_total", "retry", diagnostic_code, request.page_family);
  }
  InsertPageSelectionResult result;
  result.status = SelectionErrorStatus();
  result.retryable = retryable;
  result.diagnostic = MakePageSelectionDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  if (ledger != nullptr) {
    InsertPageSelectionEntry empty;
    empty.database_uuid = request.database_uuid;
    empty.transaction_uuid = request.transaction_uuid;
    empty.local_transaction_id = request.local_transaction_id;
    empty.object_uuid = request.object_uuid;
    empty.reservation_id = request.reservation_id;
    empty.page_family = request.page_family;
    empty.encoded_row_bytes = request.encoded_row_bytes;
    empty.state = retryable ? PageSelectionState::page_full_retry : PageSelectionState::stale_fence;
    result.evidence = MakeEvidence(ledger,
                                   InsertPageSelectionEntry{},
                                   empty,
                                   retryable ? "select_retry" : "select_refuse",
                                   result.diagnostic.diagnostic_code,
                                   result.diagnostic.diagnostic_code);
  }
  return result;
}

InsertPageAppendResult AppendError(std::string diagnostic_code,
                                   std::string message_key,
                                   bool retryable = false,
                                   std::string detail = {}) {
  InsertPageAppendResult result;
  result.status = SelectionErrorStatus();
  result.retryable = retryable;
  result.diagnostic = MakePageSelectionDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

InsertPageCandidate* FindMutableCandidate(PageSelectionLedger* ledger, const TypedUuid& page_uuid) {
  if (ledger == nullptr) {
    return nullptr;
  }
  for (auto& candidate : ledger->candidates) {
    if (candidate.page_uuid.value == page_uuid.value) {
      return &candidate;
    }
  }
  return nullptr;
}

InsertPageSelectionEntry* FindMutableSelection(PageSelectionLedger* ledger, const std::string& fence) {
  if (ledger == nullptr) {
    return nullptr;
  }
  for (auto& selection : ledger->selections) {
    if (selection.selection_fence == fence) {
      return &selection;
    }
  }
  return nullptr;
}

bool CandidateMatches(const InsertPageCandidate& candidate, const InsertPageSelectionRequest& request) {
  return candidate.active &&
         !candidate.quarantined &&
         candidate.database_uuid.value == request.database_uuid.value &&
         candidate.object_uuid.value == request.object_uuid.value &&
         candidate.page_family == request.page_family;
}

bool CandidateMatchesReservationFilespace(const InsertPageCandidate& candidate,
                                          const PageReservationEntry* reservation) {
  return reservation == nullptr ||
         candidate.filespace_uuid.value == reservation->filespace_uuid.value;
}

}  // namespace

const char* PageSelectionStateName(PageSelectionState state) {
  switch (state) {
    case PageSelectionState::planned: return "planned";
    case PageSelectionState::selected: return "selected";
    case PageSelectionState::appended: return "appended";
    case PageSelectionState::page_full_retry: return "page_full_retry";
    case PageSelectionState::stale_fence: return "stale_fence";
    case PageSelectionState::released: return "released";
    case PageSelectionState::quarantine: return "quarantine";
  }
  return "unknown";
}

Status RegisterInsertPageCandidate(PageSelectionLedger* ledger, const InsertPageCandidate& candidate) {
  if (ledger == nullptr) {
    return SelectionErrorStatus();
  }
  if (!IsTypedEngineIdentity(candidate.database_uuid, UuidKind::database) ||
      !IsTypedEngineIdentity(candidate.filespace_uuid, UuidKind::filespace) ||
      !IsTypedEngineIdentity(candidate.object_uuid, UuidKind::object) ||
      !IsTypedEngineIdentity(candidate.page_uuid, UuidKind::page) ||
      !IsKnownInsertPageFamily(candidate.page_family)) {
    return SelectionErrorStatus();
  }
  ledger->candidates.push_back(candidate);
  return SelectionOkStatus();
}

InsertPageSelectionResult SelectInsertTargetPage(PageSelectionLedger* selection_ledger,
                                                 PageReservationLedger* reservation_ledger,
                                                 const InsertPageSelectionRequest& request) {
  if (selection_ledger == nullptr) {
    return SelectionError(nullptr,
                          request,
                          "insert_page_selection_ledger_required",
                          "storage.page_selection.ledger_required");
  }
  if (!StartupModeAllowsSelection(request.startup_mode)) {
    return SelectionError(selection_ledger,
                          request,
                          "insert_page_selection_unsafe_startup_mode",
                          "storage.page_selection.unsafe_startup_mode",
                          false,
                          InsertReservationStartupModeName(request.startup_mode));
  }
  if (!IsTypedEngineIdentity(request.database_uuid, UuidKind::database) ||
      !IsTypedEngineIdentity(request.transaction_uuid, UuidKind::transaction) ||
      !IsTypedEngineIdentity(request.object_uuid, UuidKind::object)) {
    return SelectionError(selection_ledger,
                          request,
                          "insert_page_selection_uuid_invalid",
                          "storage.page_selection.uuid_invalid");
  }
  if (request.local_transaction_id == 0) {
    return SelectionError(selection_ledger,
                          request,
                          "insert_page_selection_local_tx_required",
                          "storage.page_selection.local_tx_required");
  }
  if (request.encoded_row_bytes == 0) {
    return SelectionError(selection_ledger,
                          request,
                          "insert_page_selection_encoded_row_required",
                          "storage.page_selection.encoded_row_required");
  }
  if (!IsKnownInsertPageFamily(request.page_family)) {
    return SelectionError(selection_ledger,
                          request,
                          "insert_page_selection_unknown_page_family",
                          "storage.page_selection.unknown_page_family",
                          false,
                          request.page_family);
  }

  const PageReservationEntry* reservation = nullptr;
  if (request.reservation_id.valid()) {
    if (reservation_ledger == nullptr) {
      return SelectionError(selection_ledger,
                            request,
                            "insert_page_selection_reservation_ledger_required",
                            "storage.page_selection.reservation_ledger_required");
    }
    reservation = FindPageReservation(*reservation_ledger, request.reservation_id);
    if (reservation == nullptr) {
      return SelectionError(selection_ledger,
                            request,
                            "insert_page_selection_reservation_not_found",
                            "storage.page_selection.reservation_not_found");
    }
    if (reservation->state != PageReservationState::durable_unconsumed &&
        reservation->state != PageReservationState::partially_consumed) {
      return SelectionError(selection_ledger,
                            request,
                            "insert_page_selection_reservation_not_selectable",
                            "storage.page_selection.reservation_not_selectable");
    }
  }

  bool matching_page_seen = false;
  for (const auto& candidate : selection_ledger->candidates) {
    if (!CandidateMatches(candidate, request)) {
      continue;
    }
    if (!CandidateMatchesReservationFilespace(candidate, reservation)) {
      continue;
    }
    matching_page_seen = true;
    if (candidate.free_bytes < request.encoded_row_bytes) {
      continue;
    }
    InsertPageSelectionEntry before;
    InsertPageSelectionEntry selection;
    selection.database_uuid = request.database_uuid;
    selection.transaction_uuid = request.transaction_uuid;
    selection.local_transaction_id = request.local_transaction_id;
    selection.object_uuid = request.object_uuid;
    selection.filespace_uuid = candidate.filespace_uuid;
    selection.page_uuid = candidate.page_uuid;
    selection.reservation_id = request.reservation_id;
    selection.page_family = request.page_family;
    selection.state = PageSelectionState::selected;
    selection.page_number = candidate.page_number;
    selection.page_generation = candidate.page_generation;
    selection.encoded_row_bytes = request.encoded_row_bytes;
    selection.free_bytes_before = candidate.free_bytes;
    selection.free_bytes_after = candidate.free_bytes;
    selection.selection_fence = MakeFence(candidate, selection_ledger->next_evidence_sequence);
    const auto evidence = MakeEvidence(selection_ledger,
                                       before,
                                       selection,
                                       "select_page",
                                       "selected",
                                       "insert_page_selection_selected");
    selection_ledger->selections.push_back(selection);
    InsertPageSelectionResult result;
    result.status = SelectionOkStatus();
    result.selected = true;
    result.selection = selection;
    result.evidence = evidence;
    return result;
  }

  return SelectionError(selection_ledger,
                        request,
                        matching_page_seen ? "insert_page_selection_page_full_retry" : "insert_page_selection_no_candidate_page",
                        matching_page_seen ? "storage.page_selection.page_full_retry" : "storage.page_selection.no_candidate_page",
                        matching_page_seen);
}

InsertPageAppendResult AppendRowToSelectedPageWithReservationLedger(PageSelectionLedger* ledger,
                                                                    PageReservationLedger* reservation_ledger,
                                                                    const InsertPageAppendRequest& request) {
  if (ledger == nullptr) {
    return AppendError("insert_page_append_ledger_required", "storage.page_selection.ledger_required");
  }
  auto* selection = FindMutableSelection(ledger, request.selection_fence);
  if (selection == nullptr || selection->state != PageSelectionState::selected) {
    return AppendError("insert_page_selection_stale_fence", "storage.page_selection.stale_fence");
  }
  auto* candidate = FindMutableCandidate(ledger, selection->page_uuid);
  if (candidate == nullptr || candidate->page_generation != selection->page_generation || candidate->quarantined) {
    const InsertPageSelectionEntry before = *selection;
    selection->state = PageSelectionState::stale_fence;
    selection->free_bytes_after = selection->free_bytes_before;
    const auto evidence = MakeEvidence(ledger,
                                       before,
                                       *selection,
                                       "append_stale_fence",
                                       "stale_fence",
                                       "insert_page_selection_stale_fence");
    InsertPageAppendResult result = AppendError("insert_page_selection_stale_fence",
                                                "storage.page_selection.stale_fence");
    result.selection = *selection;
    result.evidence = evidence;
    return result;
  }
  const u64 bytes = request.encoded_row_bytes == 0 ? selection->encoded_row_bytes : request.encoded_row_bytes;
  if (candidate->free_bytes < bytes) {
    const InsertPageSelectionEntry before = *selection;
    selection->state = PageSelectionState::page_full_retry;
    selection->free_bytes_after = candidate->free_bytes;
    const auto evidence = MakeEvidence(ledger,
                                       before,
                                       *selection,
                                       "append_page_full_retry",
                                       request.reason.empty() ? "page_full_retry" : request.reason,
                                       "insert_page_selection_page_full_retry");
    EmitMetric("sb_page_insert_page_full_retry_total", "retry", "page_full_retry", selection->page_family);
    InsertPageAppendResult result = AppendError("insert_page_selection_page_full_retry",
                                                "storage.page_selection.page_full_retry",
                                                true);
    result.selection = *selection;
    result.evidence = evidence;
    return result;
  }

  const InsertPageSelectionEntry before = *selection;
  if (selection->reservation_id.valid()) {
    if (reservation_ledger == nullptr) {
      return AppendError("insert_page_selection_reservation_ledger_required",
                         "storage.page_selection.reservation_ledger_required");
    }
    const auto consumed = ConsumeInsertPageReservation(
        reservation_ledger,
        ConsumePageReservationRequest{selection->reservation_id, 1, "page_append"});
    if (!consumed.ok()) {
      return AppendError("insert_page_selection_reservation_consume_failed",
                         "storage.page_selection.reservation_consume_failed");
    }
  }
  candidate->free_bytes -= bytes;
  selection->state = PageSelectionState::appended;
  selection->encoded_row_bytes = bytes;
  selection->free_bytes_after = candidate->free_bytes;
  const auto evidence = MakeEvidence(ledger,
                                     before,
                                     *selection,
                                     "append_row",
                                     request.reason.empty() ? "append" : request.reason,
                                     "insert_page_selection_appended");
  EmitMetric("sb_page_insert_page_reuse_total", "ok", "append", selection->page_family);
  InsertPageAppendResult result;
  result.status = SelectionOkStatus();
  result.appended = true;
  result.selection = *selection;
  result.evidence = evidence;
  return result;
}

InsertPageAppendResult AppendRowToSelectedPage(PageSelectionLedger* ledger,
                                               const InsertPageAppendRequest& request) {
  return AppendRowToSelectedPageWithReservationLedger(ledger, nullptr, request);
}

InsertPageAppendResult ReportInsertPageFullRetry(PageSelectionLedger* ledger,
                                                 const InsertPageFullRetryRequest& request) {
  if (ledger == nullptr) {
    return AppendError("insert_page_append_ledger_required", "storage.page_selection.ledger_required");
  }
  auto* selection = FindMutableSelection(ledger, request.selection_fence);
  if (selection == nullptr || selection->state != PageSelectionState::selected) {
    return AppendError("insert_page_selection_stale_fence", "storage.page_selection.stale_fence");
  }
  const InsertPageSelectionEntry before = *selection;
  selection->state = PageSelectionState::page_full_retry;
  const auto evidence = MakeEvidence(ledger,
                                     before,
                                     *selection,
                                     "report_page_full_retry",
                                     request.reason.empty() ? "page_full_retry" : request.reason,
                                     "insert_page_selection_page_full_retry");
  EmitMetric("sb_page_insert_page_full_retry_total", "retry", "page_full_retry", selection->page_family);
  InsertPageAppendResult result = AppendError("insert_page_selection_page_full_retry",
                                              "storage.page_selection.page_full_retry",
                                              true);
  result.selection = *selection;
  result.evidence = evidence;
  return result;
}

const InsertPageCandidate* FindInsertPageCandidate(const PageSelectionLedger& ledger,
                                                   const TypedUuid& page_uuid) {
  for (const auto& candidate : ledger.candidates) {
    if (candidate.page_uuid.value == page_uuid.value) {
      return &candidate;
    }
  }
  return nullptr;
}

DiagnosticRecord MakePageSelectionDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  std::vector<DiagnosticArgument> arguments;
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
                        "storage.page.selection");
}

}  // namespace scratchbird::storage::page
