// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_selection.hpp"
#include "uuid.hpp"

#include <iostream>
#include <string>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

scratchbird::core::platform::TypedUuid Id(scratchbird::core::platform::UuidKind kind,
                                          scratchbird::core::platform::u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : scratchbird::core::platform::TypedUuid{};
}

struct Fixture {
  scratchbird::core::platform::TypedUuid database_uuid = Id(scratchbird::core::platform::UuidKind::database, 2100);
  scratchbird::core::platform::TypedUuid transaction_uuid = Id(scratchbird::core::platform::UuidKind::transaction, 2101);
  scratchbird::core::platform::TypedUuid object_uuid = Id(scratchbird::core::platform::UuidKind::object, 2102);
  scratchbird::core::platform::TypedUuid filespace_uuid = Id(scratchbird::core::platform::UuidKind::filespace, 2103);
  scratchbird::core::platform::TypedUuid policy_uuid = Id(scratchbird::core::platform::UuidKind::object, 2104);
};

scratchbird::storage::page::InsertPageReservationRequest ReservationRequest(const Fixture& fixture,
                                                                            scratchbird::core::platform::u64 request_seed) {
  scratchbird::storage::page::InsertPageReservationRequest request;
  request.database_uuid = fixture.database_uuid;
  request.transaction_uuid = fixture.transaction_uuid;
  request.local_transaction_id = 77;
  request.object_uuid = fixture.object_uuid;
  request.page_family = "row_data";
  request.estimated_row_count = 10;
  request.estimated_payload_bytes = 4096;
  request.preferred_filespace_uuid = fixture.filespace_uuid;
  request.policy_uuid = fixture.policy_uuid;
  request.request_id = Id(scratchbird::core::platform::UuidKind::object, request_seed);
  request.current_time_authority_tick = 1;
  request.lease_duration_ticks = 100;
  return request;
}

scratchbird::storage::page::InsertPageCandidate Candidate(const Fixture& fixture,
                                                          scratchbird::core::platform::u64 page_seed,
                                                          scratchbird::core::platform::u64 free_bytes) {
  scratchbird::storage::page::InsertPageCandidate candidate;
  candidate.database_uuid = fixture.database_uuid;
  candidate.filespace_uuid = fixture.filespace_uuid;
  candidate.object_uuid = fixture.object_uuid;
  candidate.page_uuid = Id(scratchbird::core::platform::UuidKind::page, page_seed);
  candidate.page_family = "row_data";
  candidate.page_number = page_seed;
  candidate.page_generation = 1;
  candidate.free_bytes = free_bytes;
  return candidate;
}

scratchbird::storage::page::InsertPageSelectionRequest SelectionRequest(
    const Fixture& fixture,
    const scratchbird::core::platform::TypedUuid& reservation_id,
    scratchbird::core::platform::u64 row_bytes) {
  scratchbird::storage::page::InsertPageSelectionRequest request;
  request.database_uuid = fixture.database_uuid;
  request.transaction_uuid = fixture.transaction_uuid;
  request.local_transaction_id = 77;
  request.object_uuid = fixture.object_uuid;
  request.reservation_id = reservation_id;
  request.page_family = "row_data";
  request.encoded_row_bytes = row_bytes;
  return request;
}

}  // namespace

int main() {
  using namespace scratchbird::storage::page;

  bool ok = true;
  const Fixture fixture;

  PageReservationLedger reservation_ledger;
  PageSelectionLedger selection_ledger;
  const auto reservation = ReserveInsertPagesDurable(&reservation_ledger, ReservationRequest(fixture, 2200));
  ok &= Require(reservation.ok(), "reservation admitted for selection");
  ok &= Require(RegisterInsertPageCandidate(&selection_ledger, Candidate(fixture, 3000, 8192)).ok(), "candidate registered");

  const auto selected = SelectInsertTargetPage(&selection_ledger,
                                               &reservation_ledger,
                                               SelectionRequest(fixture, reservation.reservation.reservation_id, 1024));
  ok &= Require(selected.ok(), "target page selected");
  ok &= Require(!selected.selection.selection_fence.empty(), "selection fence populated");
  ok &= Require(selected.selection.state == PageSelectionState::selected, "selection state selected");
  ok &= Require(!reservation_ledger.reservations.empty() &&
                    reservation_ledger.reservations.front().consumed_page_count == 0,
                "selection does not consume reserved page before append");

  const auto appended = AppendRowToSelectedPageWithReservationLedger(
      &selection_ledger,
      &reservation_ledger,
      InsertPageAppendRequest{selected.selection.selection_fence, 1024, "probe_append"});
  ok &= Require(appended.ok(), "append to selected page succeeds");
  ok &= Require(appended.selection.state == PageSelectionState::appended, "append state tracked");
  ok &= Require(!reservation_ledger.reservations.empty() &&
                    reservation_ledger.reservations.front().consumed_page_count == 1,
                "append consumes one reserved page");
  const auto* candidate_after_append = FindInsertPageCandidate(selection_ledger, selected.selection.page_uuid);
  ok &= Require(candidate_after_append != nullptr, "candidate remains findable");
  ok &= Require(candidate_after_append != nullptr && candidate_after_append->free_bytes == 7168, "candidate free bytes decremented");

  const auto stale = AppendRowToSelectedPage(&selection_ledger,
                                             InsertPageAppendRequest{selected.selection.selection_fence, 1024, "probe_stale"});
  ok &= Require(!stale.ok(), "stale fence rejected after append");
  ok &= Require(stale.diagnostic.diagnostic_code == "insert_page_selection_stale_fence", "stale fence diagnostic");

  PageReservationLedger failing_append_reservation_ledger;
  PageSelectionLedger failing_append_selection_ledger;
  const auto failing_append_reservation =
      ReserveInsertPagesDurable(&failing_append_reservation_ledger, ReservationRequest(fixture, 2250));
  ok &= Require(failing_append_reservation.ok(), "failing append reservation admitted");
  ok &= Require(RegisterInsertPageCandidate(&failing_append_selection_ledger, Candidate(fixture, 3050, 2048)).ok(),
                "failing append candidate registered");
  const auto failing_append_selection = SelectInsertTargetPage(
      &failing_append_selection_ledger,
      &failing_append_reservation_ledger,
      SelectionRequest(fixture, failing_append_reservation.reservation.reservation_id, 1024));
  ok &= Require(failing_append_selection.ok(), "failing append selection admitted");
  failing_append_selection_ledger.candidates.front().free_bytes = 8;
  const auto failing_append = AppendRowToSelectedPageWithReservationLedger(
      &failing_append_selection_ledger,
      &failing_append_reservation_ledger,
      InsertPageAppendRequest{failing_append_selection.selection.selection_fence, 1024, "probe_page_full"});
  ok &= Require(!failing_append.ok() && failing_append.retryable, "append page-full retry is retryable");
  ok &= Require(!failing_append_reservation_ledger.reservations.empty() &&
                    failing_append_reservation_ledger.reservations.front().consumed_page_count == 0,
                "failed append does not consume reserved page");

  PageReservationLedger small_reservation_ledger;
  PageSelectionLedger small_selection_ledger;
  const auto small_reservation = ReserveInsertPagesDurable(&small_reservation_ledger, ReservationRequest(fixture, 2300));
  ok &= Require(small_reservation.ok(), "small reservation admitted");
  ok &= Require(RegisterInsertPageCandidate(&small_selection_ledger, Candidate(fixture, 3100, 128)).ok(), "small candidate registered");
  const auto full_retry = SelectInsertTargetPage(&small_selection_ledger,
                                                 &small_reservation_ledger,
                                                 SelectionRequest(fixture, small_reservation.reservation.reservation_id, 1024));
  ok &= Require(!full_retry.ok() && full_retry.retryable, "page-full retry is retryable");
  ok &= Require(full_retry.diagnostic.diagnostic_code == "insert_page_selection_page_full_retry", "page-full diagnostic");

  PageSelectionLedger unknown_family_ledger;
  auto unknown_request = SelectionRequest(fixture, scratchbird::core::platform::TypedUuid{}, 512);
  unknown_request.page_family = "not_a_family";
  const auto unknown_family = SelectInsertTargetPage(&unknown_family_ledger, nullptr, unknown_request);
  ok &= Require(!unknown_family.ok(), "unknown page family refused");
  ok &= Require(unknown_family.diagnostic.diagnostic_code == "insert_page_selection_unknown_page_family", "unknown family diagnostic");

  PageSelectionLedger unsafe_ledger;
  auto unsafe_request = SelectionRequest(fixture, scratchbird::core::platform::TypedUuid{}, 512);
  unsafe_request.startup_mode = InsertReservationStartupMode::recovery_unsafe;
  const auto unsafe = SelectInsertTargetPage(&unsafe_ledger, nullptr, unsafe_request);
  ok &= Require(!unsafe.ok(), "unsafe startup refused");
  ok &= Require(unsafe.diagnostic.diagnostic_code == "insert_page_selection_unsafe_startup_mode", "unsafe startup diagnostic");

  PageReservationLedger no_candidate_reservation_ledger;
  PageSelectionLedger no_candidate_ledger;
  const auto no_candidate_reservation = ReserveInsertPagesDurable(&no_candidate_reservation_ledger, ReservationRequest(fixture, 2400));
  ok &= Require(no_candidate_reservation.ok(), "no-candidate reservation admitted");
  const auto no_candidate = SelectInsertTargetPage(&no_candidate_ledger,
                                                   &no_candidate_reservation_ledger,
                                                   SelectionRequest(fixture, no_candidate_reservation.reservation.reservation_id, 512));
  ok &= Require(!no_candidate.ok() && !no_candidate.retryable, "missing candidate refused non-retryable");
  ok &= Require(no_candidate.diagnostic.diagnostic_code == "insert_page_selection_no_candidate_page", "missing candidate diagnostic");

  return ok ? 0 : 1;
}
