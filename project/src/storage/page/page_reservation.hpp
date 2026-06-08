// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INSERT-PAGE-RESERVATION-ANCHOR
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

enum class InsertReservationStartupMode : u32 {
  normal,
  read_only,
  restricted_open,
  recovery_unsafe,
  maintenance,
  shutdown
};

enum class PageReservationState : u32 {
  absent,
  durable_unconsumed,
  partially_consumed,
  consumed,
  released,
  expired,
  quarantine
};

enum class PageReservationRecoveryAction : u32 {
  no_action,
  release,
  retain,
  quarantine,
  fail_closed
};

struct InsertPageReservationRequest {
  TypedUuid database_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TypedUuid object_uuid;
  std::string page_family;
  u64 estimated_row_count = 0;
  u64 estimated_payload_bytes = 0;
  TypedUuid preferred_filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid request_id;
  scratchbird::storage::filespace::FilespaceObjectClass object_class =
      scratchbird::storage::filespace::FilespaceObjectClass::unspecified;
  InsertReservationStartupMode startup_mode = InsertReservationStartupMode::normal;
  u32 page_size = 16 * 1024;
  u64 current_time_authority_tick = 0;
  u64 lease_duration_ticks = 60 * 1000;
};

struct PageReservationEvidenceRecord {
  u64 sequence = 0;
  std::string action;
  TypedUuid reservation_id;
  TypedUuid database_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TypedUuid object_uuid;
  TypedUuid filespace_uuid;
  std::string page_family;
  std::string filespace_class;
  std::string filespace_class_reason;
  PageReservationState previous_state = PageReservationState::absent;
  PageReservationState new_state = PageReservationState::absent;
  u64 reserved_page_count = 0;
  u64 consumed_page_count = 0;
  u64 released_page_count = 0;
  u64 expires_at_time_authority_tick = 0;
  std::string reason;
  std::string diagnostic_code;
  bool durable_state_changed = false;
};

struct PageReservationEntry {
  TypedUuid reservation_id;
  TypedUuid database_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TypedUuid object_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  std::string page_family;
  std::string filespace_class;
  std::string filespace_class_reason;
  PageReservationState state = PageReservationState::absent;
  u64 reserved_page_count = 0;
  u64 consumed_page_count = 0;
  u64 released_page_count = 0;
  u64 usable_free_bytes_estimate = 0;
  u64 expires_at_time_authority_tick = 0;
};

struct PageReservationLedger {
  std::vector<PageReservationEntry> reservations;
  std::vector<PageReservationEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
};

struct InsertPageReservationResult {
  Status status;
  bool admitted = false;
  PageReservationEntry reservation;
  PageReservationEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

struct ConsumePageReservationRequest {
  TypedUuid reservation_id;
  u64 pages_to_consume = 1;
  std::string reason;
};

struct ReleasePageReservationRequest {
  TypedUuid reservation_id;
  std::string reason;
};

struct ExpirePageReservationsRequest {
  u64 current_time_authority_tick = 0;
  std::string reason;
};

struct PageReservationMutationResult {
  Status status;
  bool changed = false;
  PageReservationEntry reservation;
  PageReservationEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct PageReservationRecoveryClassification {
  TypedUuid reservation_id;
  PageReservationState observed_state = PageReservationState::absent;
  PageReservationRecoveryAction action = PageReservationRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct PageReservationRecoveryResult {
  Status status;
  std::vector<PageReservationRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* InsertReservationStartupModeName(InsertReservationStartupMode mode);
const char* PageReservationStateName(PageReservationState state);
const char* PageReservationRecoveryActionName(PageReservationRecoveryAction action);

InsertPageReservationResult ReserveInsertPagesDurable(PageReservationLedger* ledger,
                                                      const InsertPageReservationRequest& request);
PageReservationMutationResult ConsumeInsertPageReservation(PageReservationLedger* ledger,
                                                           const ConsumePageReservationRequest& request);
PageReservationMutationResult ReleaseInsertPageReservation(PageReservationLedger* ledger,
                                                           const ReleasePageReservationRequest& request);
std::vector<PageReservationMutationResult> ExpireInsertPageReservations(PageReservationLedger* ledger,
                                                                        const ExpirePageReservationsRequest& request);
PageReservationRecoveryClassification ClassifyPageReservationForRecovery(const PageReservationEntry& reservation);
PageReservationRecoveryResult ClassifyPageReservationLedgerForRecovery(const PageReservationLedger& ledger);
const PageReservationEntry* FindPageReservation(const PageReservationLedger& ledger,
                                                const TypedUuid& reservation_id);
DiagnosticRecord MakePageReservationDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});

}  // namespace scratchbird::storage::page
