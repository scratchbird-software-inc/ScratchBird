// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_reservation.hpp"
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

scratchbird::storage::page::InsertPageReservationRequest BaseRequest() {
  using scratchbird::core::platform::UuidKind;
  scratchbird::storage::page::InsertPageReservationRequest request;
  request.database_uuid = Id(UuidKind::database, 1000);
  request.transaction_uuid = Id(UuidKind::transaction, 1001);
  request.local_transaction_id = 42;
  request.object_uuid = Id(UuidKind::object, 1002);
  request.page_family = "row_data";
  request.estimated_row_count = 256;
  request.estimated_payload_bytes = 8192;
  request.preferred_filespace_uuid = Id(UuidKind::filespace, 1003);
  request.policy_uuid = Id(UuidKind::object, 1004);
  request.request_id = Id(UuidKind::object, 1005);
  request.current_time_authority_tick = 10;
  request.lease_duration_ticks = 5;
  return request;
}

}  // namespace

int main() {
  using namespace scratchbird::storage::page;

  bool ok = true;

  PageReservationLedger ledger;
  const auto admitted = ReserveInsertPagesDurable(&ledger, BaseRequest());
  ok &= Require(admitted.ok(), "normal reservation admitted");
  ok &= Require(admitted.reservation.state == PageReservationState::durable_unconsumed, "reservation state durable_unconsumed");
  ok &= Require(admitted.reservation.reserved_page_count >= 1, "reserved page count populated");
  ok &= Require(admitted.evidence.durable_state_changed, "admit evidence durable state changed");
  ok &= Require(ledger.reservations.size() == 1, "ledger has reservation");
  ok &= Require(ledger.evidence.size() == 1, "ledger has admit evidence");

  const auto consume = ConsumeInsertPageReservation(
      &ledger,
      ConsumePageReservationRequest{admitted.reservation.reservation_id, 1, "probe_consume"});
  ok &= Require(consume.ok() && consume.changed, "consume reservation changed state");
  ok &= Require(consume.reservation.consumed_page_count == 1, "consume count tracked");
  ok &= Require(consume.reservation.state == PageReservationState::partially_consumed ||
                    consume.reservation.state == PageReservationState::consumed,
                "consume state is partial or consumed");

  const auto release = ReleaseInsertPageReservation(
      &ledger,
      ReleasePageReservationRequest{admitted.reservation.reservation_id, "rollback"});
  ok &= Require(release.ok() && release.changed, "release on rollback succeeds");
  ok &= Require(release.reservation.state == PageReservationState::released, "release state tracked");
  ok &= Require(release.reservation.released_page_count + release.reservation.consumed_page_count ==
                    release.reservation.reserved_page_count,
                "release accounts for all reserved pages");

  PageReservationLedger unknown_family_ledger;
  auto unknown_family = BaseRequest();
  unknown_family.request_id = Id(scratchbird::core::platform::UuidKind::object, 1105);
  unknown_family.page_family = "not_a_page_family";
  const auto unknown = ReserveInsertPagesDurable(&unknown_family_ledger, unknown_family);
  ok &= Require(!unknown.ok(), "unknown page family refused");
  ok &= Require(unknown.diagnostic.diagnostic_code == "insert_page_reservation_unknown_page_family",
                "unknown page family diagnostic");

  PageReservationLedger unsafe_ledger;
  auto unsafe_request = BaseRequest();
  unsafe_request.request_id = Id(scratchbird::core::platform::UuidKind::object, 1205);
  unsafe_request.startup_mode = InsertReservationStartupMode::recovery_unsafe;
  const auto unsafe = ReserveInsertPagesDurable(&unsafe_ledger, unsafe_request);
  ok &= Require(!unsafe.ok(), "unsafe startup refused");
  ok &= Require(unsafe.diagnostic.diagnostic_code == "insert_page_reservation_unsafe_startup_mode",
                "unsafe startup diagnostic");

  PageReservationLedger expiry_ledger;
  auto expiry_request = BaseRequest();
  expiry_request.request_id = Id(scratchbird::core::platform::UuidKind::object, 1305);
  expiry_request.current_time_authority_tick = 100;
  expiry_request.lease_duration_ticks = 1;
  const auto expiring = ReserveInsertPagesDurable(&expiry_ledger, expiry_request);
  ok &= Require(expiring.ok(), "expiring reservation admitted");
  const auto expired = ExpireInsertPageReservations(&expiry_ledger, ExpirePageReservationsRequest{101, "probe_expiry"});
  ok &= Require(expired.size() == 1, "one reservation expired");
  ok &= Require(expired.front().ok() && expired.front().changed, "expiry changed state");
  ok &= Require(expired.front().reservation.state == PageReservationState::expired, "expired state tracked");

  const auto recovery = ClassifyPageReservationLedgerForRecovery(expiry_ledger);
  ok &= Require(recovery.ok(), "recovery classification succeeds");
  ok &= Require(!recovery.classifications.empty(), "recovery classification produced rows");
  ok &= Require(recovery.classifications.front().action == PageReservationRecoveryAction::no_action,
                "expired reservation has no recovery action");

  PageReservationLedger recover_release_ledger;
  auto recover_request = BaseRequest();
  recover_request.request_id = Id(scratchbird::core::platform::UuidKind::object, 1405);
  const auto recover_reservation = ReserveInsertPagesDurable(&recover_release_ledger, recover_request);
  ok &= Require(recover_reservation.ok(), "recoverable reservation admitted");
  const auto recover_release = ClassifyPageReservationLedgerForRecovery(recover_release_ledger);
  ok &= Require(recover_release.ok(), "recoverable classification ok");
  ok &= Require(recover_release.classifications.front().action == PageReservationRecoveryAction::release,
                "unconsumed durable reservation releases on recovery");

  return ok ? 0 : 1;
}
