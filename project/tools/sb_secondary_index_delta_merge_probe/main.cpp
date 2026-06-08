// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "secondary_index_delta_merge.hpp"
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
  scratchbird::core::platform::TypedUuid index_uuid = Id(scratchbird::core::platform::UuidKind::object, 9100);
  scratchbird::core::platform::TypedUuid table_uuid = Id(scratchbird::core::platform::UuidKind::object, 9101);
  scratchbird::core::platform::TypedUuid transaction_uuid = Id(scratchbird::core::platform::UuidKind::transaction, 9102);
  scratchbird::core::platform::TypedUuid merge_id = Id(scratchbird::core::platform::UuidKind::object, 9103);
};

scratchbird::core::index::SecondaryIndexBaseEntry Base(const Fixture& fixture,
                                                       scratchbird::core::platform::u64 row_seed,
                                                       std::string key) {
  scratchbird::core::index::SecondaryIndexBaseEntry entry;
  entry.index_uuid = fixture.index_uuid;
  entry.table_uuid = fixture.table_uuid;
  entry.row_uuid = Id(scratchbird::core::platform::UuidKind::row, row_seed);
  entry.version_uuid = Id(scratchbird::core::platform::UuidKind::row, row_seed + 1000);
  entry.key_payload = std::move(key);
  entry.committed_local_transaction_id = 10;
  return entry;
}

scratchbird::core::index::SecondaryIndexDeltaEntry Delta(
    const Fixture& fixture,
    scratchbird::core::index::SecondaryIndexDeltaKind kind,
    scratchbird::core::platform::TypedUuid row_uuid,
    std::string key,
    scratchbird::core::platform::u64 local_transaction_id,
    bool committed) {
  scratchbird::core::index::SecondaryIndexDeltaEntry entry;
  entry.delta_id = Id(scratchbird::core::platform::UuidKind::object, 9200 + local_transaction_id);
  entry.index_uuid = fixture.index_uuid;
  entry.table_uuid = fixture.table_uuid;
  entry.row_uuid = row_uuid;
  entry.version_uuid = Id(scratchbird::core::platform::UuidKind::row, 9300 + local_transaction_id);
  entry.transaction_uuid = fixture.transaction_uuid;
  entry.local_transaction_id = local_transaction_id;
  entry.delta_kind = kind;
  entry.key_payload = std::move(key);
  entry.cleanup_horizon_token = "horizon-token";
  entry.committed = committed;
  return entry;
}

scratchbird::core::index::SecondaryIndexMergeRequest MergeRequest(const Fixture& fixture,
                                                                  scratchbird::core::platform::u64 horizon) {
  scratchbird::core::index::SecondaryIndexMergeRequest request;
  request.index_uuid = fixture.index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.merge_id = fixture.merge_id;
  request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  request.cleanup_horizon_authoritative = true;
  return request;
}

scratchbird::core::index::SecondaryIndexCleanupRequest CleanupRequest(const Fixture& fixture,
                                                                      scratchbird::core::platform::u64 horizon,
                                                                      bool authoritative = true) {
  scratchbird::core::index::SecondaryIndexCleanupRequest request;
  request.index_uuid = fixture.index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  request.cleanup_horizon_authoritative = authoritative;
  return request;
}

bool HasKey(const std::vector<scratchbird::core::index::SecondaryIndexBaseEntry>& entries,
            const std::string& key) {
  for (const auto& entry : entries) {
    if (entry.key_payload == key && !entry.deleted) {
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
  base_entries.push_back(Base(fixture, 9400, "alpha"));
  base_entries.push_back(Base(fixture, 9401, "remove_me"));
  base_entries.push_back(Base(fixture, 9402, "old_value"));

  SecondaryIndexDeltaLedger delta_ledger;
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::insert,
                                      Id(scratchbird::core::platform::UuidKind::row, 9500),
                                      "inserted",
                                      20,
                                      true));
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::delete_row,
                                      base_entries[1].row_uuid,
                                      "remove_me",
                                      21,
                                      true));
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::update_before,
                                      base_entries[2].row_uuid,
                                      "old_value",
                                      22,
                                      true));
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::update_after,
                                      base_entries[2].row_uuid,
                                      "new_value",
                                      22,
                                      true));
  delta_ledger.deltas.push_back(Delta(fixture,
                                      SecondaryIndexDeltaKind::insert,
                                      Id(scratchbird::core::platform::UuidKind::row, 9501),
                                      "future_unmerged",
                                      80,
                                      true));

  SecondaryIndexDeltaMergeLedger merge_ledger;
  const auto merged = MergeSecondaryIndexDeltas(&merge_ledger,
                                                &base_entries,
                                                &delta_ledger,
                                                MergeRequest(fixture, 50));
  ok &= Require(merged.ok(), "committed deltas merge");
  ok &= Require(HasKey(base_entries, "alpha"), "base key remains");
  ok &= Require(HasKey(base_entries, "inserted"), "insert delta merged into base");
  ok &= Require(!HasKey(base_entries, "remove_me"), "delete delta removed base key");
  ok &= Require(!HasKey(base_entries, "old_value"), "update_before removed old key");
  ok &= Require(HasKey(base_entries, "new_value"), "update_after merged new key");
  ok &= Require(!HasKey(base_entries, "future_unmerged"), "future delta not merged");
  ok &= Require(merged.evidence.merged_count == 4, "merged count tracked");
  ok &= Require(merged.evidence.retained_delta_count == 1, "retained count tracked");

  const auto retry = MergeSecondaryIndexDeltas(&merge_ledger,
                                               &base_entries,
                                               &delta_ledger,
                                               MergeRequest(fixture, 50));
  ok &= Require(retry.ok(), "merge retry succeeds");
  ok &= Require(retry.evidence.merged_count == 2, "idempotent retry avoids duplicate add entries but delete/update-before remain safe");
  int inserted_count = 0;
  for (const auto& entry : base_entries) {
    if (entry.key_payload == "inserted") {
      ++inserted_count;
    }
  }
  ok &= Require(inserted_count == 1, "merge retry does not duplicate inserted base entry");

  const auto blocked_cleanup = CleanupSecondaryIndexDeltas(&merge_ledger,
                                                           &delta_ledger,
                                                           CleanupRequest(fixture, 50));
  ok &= Require(!blocked_cleanup.ok(), "cleanup blocked by retained future delta");
  ok &= Require(blocked_cleanup.horizon_blocked, "cleanup reports horizon blocked");
  ok &= Require(blocked_cleanup.diagnostic.diagnostic_code == "secondary_index_delta_cleanup_horizon_blocked",
                "horizon blocked diagnostic");
  ok &= Require(delta_ledger.deltas.size() == 5, "blocked cleanup retains all deltas");

  const auto cleanup = CleanupSecondaryIndexDeltas(&merge_ledger,
                                                   &delta_ledger,
                                                   CleanupRequest(fixture, 100));
  ok &= Require(cleanup.ok(), "cleanup succeeds after horizon advances");
  ok &= Require(cleanup.cleaned_count == 5, "all matching deltas cleaned");
  ok &= Require(delta_ledger.deltas.empty(), "delta ledger empty after cleanup");

  SecondaryIndexDeltaLedger no_authority_delta_ledger;
  no_authority_delta_ledger.deltas.push_back(Delta(fixture,
                                                   SecondaryIndexDeltaKind::insert,
                                                   Id(scratchbird::core::platform::UuidKind::row, 9600),
                                                   "no_authority",
                                                   10,
                                                   true));
  const auto no_authority_cleanup = CleanupSecondaryIndexDeltas(&merge_ledger,
                                                                &no_authority_delta_ledger,
                                                                CleanupRequest(fixture, 100, false));
  ok &= Require(!no_authority_cleanup.ok(), "cleanup without authoritative horizon refused");
  ok &= Require(no_authority_cleanup.diagnostic.diagnostic_code ==
                    "secondary_index_delta_cleanup_horizon_not_authoritative",
                "non-authoritative cleanup diagnostic");

  const auto recovery = ClassifySecondaryIndexMergeLedgerForRecovery(merge_ledger);
  ok &= Require(recovery.ok(), "merge recovery classification succeeds");
  ok &= Require(!recovery.classifications.empty(), "merge recovery rows produced");
  ok &= Require(recovery.classifications.front().action ==
                    SecondaryIndexMergeRecoveryAction::complete_idempotent_merge,
                "merged state recovers through idempotent completion");

  return ok ? 0 : 1;
}
