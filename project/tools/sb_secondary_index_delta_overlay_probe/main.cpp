// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "secondary_index_delta_overlay.hpp"
#include "uuid.hpp"

#include <iostream>
#include <string>
#include <vector>

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
  scratchbird::core::platform::TypedUuid index_uuid = Id(scratchbird::core::platform::UuidKind::object, 6100);
  scratchbird::core::platform::TypedUuid table_uuid = Id(scratchbird::core::platform::UuidKind::object, 6101);
  scratchbird::core::platform::TypedUuid transaction_uuid = Id(scratchbird::core::platform::UuidKind::transaction, 6102);
  scratchbird::core::platform::TypedUuid other_transaction_uuid = Id(scratchbird::core::platform::UuidKind::transaction, 6103);
};

scratchbird::core::index::SecondaryIndexBaseEntry Base(const Fixture& fixture,
                                                       scratchbird::core::platform::u64 row_seed,
                                                       std::string key,
                                                       scratchbird::core::platform::u64 commit_id) {
  scratchbird::core::index::SecondaryIndexBaseEntry entry;
  entry.index_uuid = fixture.index_uuid;
  entry.table_uuid = fixture.table_uuid;
  entry.row_uuid = Id(scratchbird::core::platform::UuidKind::row, row_seed);
  entry.version_uuid = Id(scratchbird::core::platform::UuidKind::row, row_seed + 1000);
  entry.key_payload = std::move(key);
  entry.committed_local_transaction_id = commit_id;
  return entry;
}

scratchbird::core::index::SecondaryIndexDeltaEntry Delta(
    const Fixture& fixture,
    scratchbird::core::index::SecondaryIndexDeltaKind kind,
    scratchbird::core::platform::TypedUuid row_uuid,
    std::string key,
    bool committed,
    scratchbird::core::platform::u64 local_transaction_id,
    bool own_transaction) {
  scratchbird::core::index::SecondaryIndexDeltaEntry entry;
  entry.delta_id = Id(scratchbird::core::platform::UuidKind::object, 7000 + local_transaction_id);
  entry.index_uuid = fixture.index_uuid;
  entry.table_uuid = fixture.table_uuid;
  entry.row_uuid = row_uuid;
  entry.version_uuid = Id(scratchbird::core::platform::UuidKind::row, 8000 + local_transaction_id);
  entry.transaction_uuid = own_transaction ? fixture.transaction_uuid : fixture.other_transaction_uuid;
  entry.local_transaction_id = local_transaction_id;
  entry.delta_kind = kind;
  entry.key_payload = std::move(key);
  entry.cleanup_horizon_token = "horizon-token";
  entry.committed = committed;
  return entry;
}

scratchbird::core::index::SecondaryIndexOverlayRequest Request(const Fixture& fixture) {
  scratchbird::core::index::SecondaryIndexOverlayRequest request;
  request.index_uuid = fixture.index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.transaction_uuid = fixture.transaction_uuid;
  request.local_transaction_id = 42;
  request.snapshot_high_water_local_transaction_id = 50;
  request.index_kind = scratchbird::core::index::SecondaryIndexKind::non_unique;
  request.include_own_transaction = true;
  return request;
}

bool HasKey(const std::vector<scratchbird::core::index::SecondaryIndexOverlayEntry>& entries,
            const std::string& key) {
  for (const auto& entry : entries) {
    if (entry.key_payload == key) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  using namespace scratchbird::core::index;

  bool ok = true;
  const Fixture fixture;

  std::vector<SecondaryIndexBaseEntry> base_entries;
  base_entries.push_back(Base(fixture, 6200, "alpha", 10));
  base_entries.push_back(Base(fixture, 6201, "remove_me", 10));
  base_entries.push_back(Base(fixture, 6202, "old_value", 10));
  base_entries.push_back(Base(fixture, 6203, "future_base", 99));

  SecondaryIndexDeltaLedger delta_ledger;
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::insert,
                                      Id(scratchbird::core::platform::UuidKind::row, 6300),
                                      "own_insert",
                                      false,
                                      42,
                                      true));
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::delete_row,
                                      base_entries[1].row_uuid,
                                      "remove_me",
                                      false,
                                      42,
                                      true));
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::update_before,
                                      base_entries[2].row_uuid,
                                      "old_value",
                                      false,
                                      42,
                                      true));
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::update_after,
                                      base_entries[2].row_uuid,
                                      "new_value",
                                      false,
                                      42,
                                      true));
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::insert,
                                      Id(scratchbird::core::platform::UuidKind::row, 6301),
                                      "committed_visible",
                                      true,
                                      40,
                                      false));
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::insert,
                                      Id(scratchbird::core::platform::UuidKind::row, 6302),
                                      "future_delta",
                                      true,
                                      90,
                                      false));

  SecondaryIndexOverlayLedger overlay_ledger;
  const auto overlay = BuildSecondaryIndexDeltaOverlay(&overlay_ledger, base_entries, delta_ledger, Request(fixture));
  ok &= Require(overlay.ok(), "secondary-index overlay succeeds");
  ok &= Require(HasKey(overlay.entries, "alpha"), "base entry remains visible");
  ok &= Require(!HasKey(overlay.entries, "remove_me"), "delete delta removes base entry");
  ok &= Require(!HasKey(overlay.entries, "old_value"), "update_before removes old key");
  ok &= Require(HasKey(overlay.entries, "new_value"), "update_after adds new key");
  ok &= Require(HasKey(overlay.entries, "own_insert"), "own insert delta visible");
  ok &= Require(HasKey(overlay.entries, "committed_visible"), "committed visible delta visible");
  ok &= Require(!HasKey(overlay.entries, "future_delta"), "future committed delta invisible");
  ok &= Require(!HasKey(overlay.entries, "future_base"), "future base entry invisible");
  ok &= Require(overlay.evidence.visible_delta_entries == 5, "visible delta count tracked");
  ok &= Require(overlay.evidence.durable_state_changed == false, "overlay does not mutate durable index state");

  auto hidden_own_request = Request(fixture);
  hidden_own_request.include_own_transaction = false;
  const auto hidden_own = BuildSecondaryIndexDeltaOverlay(&overlay_ledger, base_entries, delta_ledger, hidden_own_request);
  ok &= Require(hidden_own.ok(), "overlay without own transaction succeeds");
  ok &= Require(!HasKey(hidden_own.entries, "own_insert"), "own transaction can be excluded");

  auto unique_request = Request(fixture);
  unique_request.index_kind = SecondaryIndexKind::unique;
  const auto unique_refusal = BuildSecondaryIndexDeltaOverlay(&overlay_ledger, base_entries, delta_ledger, unique_request);
  ok &= Require(!unique_refusal.ok(), "unique deferred overlay refused");
  ok &= Require(unique_refusal.diagnostic.diagnostic_code == "secondary_index_overlay_unique_deferred_forbidden",
                "unique deferred diagnostic");

  SecondaryIndexDeltaLedger bad_delta_ledger;
  bad_delta_ledger.deltas.push_back(Delta(fixture,
                                          SecondaryIndexDeltaKind::insert,
                                          Id(scratchbird::core::platform::UuidKind::row, 6400),
                                          "bad_cleanup",
                                          false,
                                          42,
                                          true));
  bad_delta_ledger.deltas.front().cleanup_horizon_token.clear();
  const auto bad_cleanup = BuildSecondaryIndexDeltaOverlay(&overlay_ledger, base_entries, bad_delta_ledger, Request(fixture));
  ok &= Require(!bad_cleanup.ok(), "missing cleanup horizon refused");
  ok &= Require(bad_cleanup.diagnostic.diagnostic_code == "secondary_index_overlay_missing_cleanup_horizon",
                "missing cleanup horizon diagnostic");

  return ok ? 0 : 1;
}
