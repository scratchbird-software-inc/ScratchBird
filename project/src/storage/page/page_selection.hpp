// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INSERT-PAGE-SELECTION-ANCHOR
#include "page_reservation.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class PageSelectionState : u32 {
  planned,
  selected,
  appended,
  page_full_retry,
  stale_fence,
  released,
  quarantine
};

struct InsertPageCandidate {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid object_uuid;
  TypedUuid page_uuid;
  std::string page_family;
  u64 page_number = 0;
  u64 page_generation = 1;
  u64 free_bytes = 0;
  bool active = true;
  bool quarantined = false;
};

struct InsertPageSelectionEvidenceRecord {
  u64 sequence = 0;
  std::string action;
  std::string selection_fence;
  TypedUuid database_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TypedUuid object_uuid;
  TypedUuid filespace_uuid;
  TypedUuid page_uuid;
  TypedUuid reservation_id;
  std::string page_family;
  PageSelectionState previous_state = PageSelectionState::planned;
  PageSelectionState new_state = PageSelectionState::planned;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 encoded_row_bytes = 0;
  u64 free_bytes_before = 0;
  u64 free_bytes_after = 0;
  std::string reason;
  std::string diagnostic_code;
  bool durable_state_changed = false;
};

struct InsertPageSelectionEntry {
  std::string selection_fence;
  TypedUuid database_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TypedUuid object_uuid;
  TypedUuid filespace_uuid;
  TypedUuid page_uuid;
  TypedUuid reservation_id;
  std::string page_family;
  PageSelectionState state = PageSelectionState::planned;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 encoded_row_bytes = 0;
  u64 free_bytes_before = 0;
  u64 free_bytes_after = 0;
};

struct PageSelectionLedger {
  std::vector<InsertPageCandidate> candidates;
  std::vector<InsertPageSelectionEntry> selections;
  std::vector<InsertPageSelectionEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
};

struct InsertPageSelectionRequest {
  TypedUuid database_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TypedUuid object_uuid;
  TypedUuid reservation_id;
  std::string page_family;
  u64 encoded_row_bytes = 0;
  u32 required_alignment = 8;
  InsertReservationStartupMode startup_mode = InsertReservationStartupMode::normal;
};

struct InsertPageSelectionResult {
  Status status;
  bool selected = false;
  bool retryable = false;
  InsertPageSelectionEntry selection;
  InsertPageSelectionEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && selected; }
};

struct InsertPageAppendRequest {
  std::string selection_fence;
  u64 encoded_row_bytes = 0;
  std::string reason;
};

struct InsertPageAppendResult {
  Status status;
  bool appended = false;
  bool retryable = false;
  InsertPageSelectionEntry selection;
  InsertPageSelectionEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && appended; }
};

struct InsertPageFullRetryRequest {
  std::string selection_fence;
  std::string reason;
};

const char* PageSelectionStateName(PageSelectionState state);

Status RegisterInsertPageCandidate(PageSelectionLedger* ledger, const InsertPageCandidate& candidate);
InsertPageSelectionResult SelectInsertTargetPage(PageSelectionLedger* selection_ledger,
                                                 PageReservationLedger* reservation_ledger,
                                                 const InsertPageSelectionRequest& request);
InsertPageAppendResult AppendRowToSelectedPageWithReservationLedger(PageSelectionLedger* ledger,
                                                                    PageReservationLedger* reservation_ledger,
                                                                    const InsertPageAppendRequest& request);
// Compatibility wrapper for unreserved selections only. Reservation-backed
// selections must use AppendRowToSelectedPageWithReservationLedger so append
// success can consume the durable reservation atomically with the page append.
InsertPageAppendResult AppendRowToSelectedPage(PageSelectionLedger* ledger,
                                               const InsertPageAppendRequest& request);
InsertPageAppendResult ReportInsertPageFullRetry(PageSelectionLedger* ledger,
                                                 const InsertPageFullRetryRequest& request);
const InsertPageCandidate* FindInsertPageCandidate(const PageSelectionLedger& ledger,
                                                   const TypedUuid& page_uuid);
DiagnosticRecord MakePageSelectionDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

}  // namespace scratchbird::storage::page
