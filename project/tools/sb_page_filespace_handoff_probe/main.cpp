// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_filespace_handoff.hpp"
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
  scratchbird::core::platform::TypedUuid database_uuid = Id(scratchbird::core::platform::UuidKind::database, 5100);
  scratchbird::core::platform::TypedUuid filespace_uuid = Id(scratchbird::core::platform::UuidKind::filespace, 5101);
  scratchbird::core::platform::TypedUuid policy_uuid = Id(scratchbird::core::platform::UuidKind::object, 5102);
};

scratchbird::storage::page::PageFilespaceReservePolicy Policy(const Fixture& fixture) {
  scratchbird::storage::page::PageFilespaceReservePolicy policy;
  policy.target_reserve_pages = 8;
  policy.min_target_reserve_pages = 4;
  policy.max_target_reserve_pages = 8;
  policy.advisory_events = true;
  policy.policy_uuid = fixture.policy_uuid;
  return policy;
}

scratchbird::storage::page::PageFilespaceLowReserveEvent LowReserveEvent(
    const Fixture& fixture,
    scratchbird::core::platform::u64 released_free_pages) {
  scratchbird::storage::page::PageFilespaceLowReserveEvent event;
  event.database_uuid = fixture.database_uuid;
  event.filespace_uuid = fixture.filespace_uuid;
  event.page_family = "row_data";
  event.released_free_pages = released_free_pages;
  event.allocated_pages = 128;
  event.reserved_pages = 2;
  event.policy = Policy(fixture);
  event.reason = "probe_low_reserve";
  return event;
}

scratchbird::storage::page::PageFilespaceShrinkReadyEvent ShrinkEvent(
    const Fixture& fixture,
    scratchbird::core::platform::u64 pinned_pages,
    scratchbird::core::platform::u64 active_pages,
    scratchbird::core::platform::u64 reserved_pages) {
  scratchbird::storage::page::PageFilespaceShrinkReadyEvent event;
  event.database_uuid = fixture.database_uuid;
  event.filespace_uuid = fixture.filespace_uuid;
  event.page_family = "row_data";
  event.relocated_pages = 64;
  event.pinned_pages = pinned_pages;
  event.active_pages = active_pages;
  event.reserved_pages = reserved_pages;
  event.policy_uuid = fixture.policy_uuid;
  event.reason = "probe_shrink";
  return event;
}

}  // namespace

int main() {
  using namespace scratchbird::storage::page;

  bool ok = true;
  const Fixture fixture;

  ok &= Require(NormalizeFilespaceTargetReservePages(Policy(fixture)) == 8, "target reserve normalizes to 8");
  ok &= Require(FilespaceLowReserveThresholdPages(Policy(fixture)) == 4, "low reserve threshold is half target");
  ok &= Require(ShouldNotifyFilespaceLowReserve(LowReserveEvent(fixture, 4)), "half target triggers notification");
  ok &= Require(!ShouldNotifyFilespaceLowReserve(LowReserveEvent(fixture, 5)), "above half target does not trigger notification");

  PageFilespaceHandoffLedger ledger;
  const auto low = NotifyFilespaceLowReserve(&ledger, LowReserveEvent(fixture, 4));
  ok &= Require(low.ok(), "low reserve notification recorded");
  ok &= Require(low.evidence.kind == PageFilespaceHandoffKind::low_reserve, "low reserve kind tracked");
  ok &= Require(low.evidence.threshold_pages == 4, "low reserve threshold tracked");
  ok &= Require(low.evidence.target_reserve_pages == 8, "target reserve tracked");
  ok &= Require(low.evidence.advisory, "low reserve is advisory for restart recomputation");
  ok &= Require(ledger.evidence.size() == 1, "low reserve evidence appended");

  const auto no_low = NotifyFilespaceLowReserve(&ledger, LowReserveEvent(fixture, 5));
  ok &= Require(!no_low.ok(), "above threshold does not notify");
  ok &= Require(no_low.diagnostic.diagnostic_code == "page_filespace_low_reserve_threshold_not_met",
                "above threshold diagnostic");

  const auto shrink_ready = NotifyFilespaceShrinkReady(&ledger, ShrinkEvent(fixture, 0, 0, 0));
  ok &= Require(shrink_ready.ok(), "shrink-ready notification recorded");
  ok &= Require(shrink_ready.shrink_ready, "shrink-ready flag set");
  ok &= Require(shrink_ready.evidence.kind == PageFilespaceHandoffKind::shrink_ready, "shrink-ready kind tracked");

  const auto shrink_blocked = NotifyFilespaceShrinkReady(&ledger, ShrinkEvent(fixture, 1, 0, 0));
  ok &= Require(!shrink_blocked.ok(), "pinned page blocks shrink-ready");
  ok &= Require(!shrink_blocked.shrink_ready, "shrink-ready flag not set when blocked");
  ok &= Require(shrink_blocked.evidence.kind == PageFilespaceHandoffKind::shrink_blocked, "shrink-blocked kind tracked");
  ok &= Require(shrink_blocked.diagnostic.diagnostic_code == "page_filespace_shrink_blocked",
                "shrink-blocked diagnostic");

  PageFilespaceHandoffLedger unknown_family_ledger;
  auto unknown = LowReserveEvent(fixture, 4);
  unknown.page_family = "not_a_family";
  const auto unknown_family = NotifyFilespaceLowReserve(&unknown_family_ledger, unknown);
  ok &= Require(!unknown_family.ok(), "unknown page family refused");
  ok &= Require(unknown_family.diagnostic.diagnostic_code == "page_filespace_handoff_unknown_page_family",
                "unknown page family diagnostic");

  const auto recovery = ClassifyPageFilespaceHandoffLedgerForRecovery(ledger);
  ok &= Require(recovery.ok(), "handoff recovery classification succeeds");
  ok &= Require(!recovery.classifications.empty(), "handoff recovery rows produced");
  ok &= Require(recovery.classifications.front().action == PageFilespaceHandoffRecoveryAction::replay_advisory,
                "advisory handoff replays/recomputes on recovery");

  return ok ? 0 : 1;
}
