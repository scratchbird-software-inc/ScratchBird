// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "overflow_persistence.hpp"
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

std::vector<scratchbird::core::platform::byte> Payload(std::size_t size) {
  std::vector<scratchbird::core::platform::byte> payload;
  payload.reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    payload.push_back(static_cast<scratchbird::core::platform::byte>((i * 37) % 251));
  }
  return payload;
}

struct Fixture {
  scratchbird::core::platform::TypedUuid row_uuid = Id(scratchbird::core::platform::UuidKind::row, 10100);
  scratchbird::core::platform::TypedUuid object_uuid = Id(scratchbird::core::platform::UuidKind::object, 10101);
  scratchbird::core::platform::TypedUuid transaction_uuid = Id(scratchbird::core::platform::UuidKind::transaction, 10102);
  scratchbird::core::platform::TypedUuid policy_uuid = Id(scratchbird::core::platform::UuidKind::object, 10103);
};

scratchbird::storage::page::OverflowPersistRequest Request(const Fixture& fixture,
                                                           const std::vector<scratchbird::core::platform::byte>& payload) {
  scratchbird::storage::page::OverflowPersistRequest request;
  request.row_uuid = fixture.row_uuid;
  request.object_uuid = fixture.object_uuid;
  request.transaction_uuid = fixture.transaction_uuid;
  request.local_transaction_id = 55;
  request.value_descriptor = "descriptor:blob.binary";
  request.payload_bytes = payload;
  request.chunk_policy_uuid = fixture.policy_uuid;
  request.chunk_size = 1024;
  return request;
}

}  // namespace

int main() {
  using namespace scratchbird::storage::page;

  bool ok = true;
  const Fixture fixture;
  const auto payload = Payload(5000);

  OverflowLedger ledger;
  const auto persisted = PersistOverflowValue(&ledger, Request(fixture, payload));
  ok &= Require(persisted.ok(), "overflow value persisted");
  ok &= Require(persisted.chunk_count == 5, "payload split into five chunks");
  ok &= Require(persisted.first_page_uuid.valid(), "first overflow page populated");
  ok &= Require(!persisted.content_hash.empty(), "content hash populated");
  ok &= Require(persisted.evidence.durable_state_changed, "persist evidence durable state changed");
  ok &= Require(ledger.values.size() == 1, "ledger has overflow value");
  ok &= Require(ledger.evidence.size() == 1, "ledger has persist evidence");

  OverflowReadRequest own_read;
  own_read.overflow_value_uuid = persisted.overflow_value_uuid;
  own_read.include_uncommitted_own_transaction = true;
  own_read.transaction_uuid = fixture.transaction_uuid;
  own_read.local_transaction_id = 55;
  const auto own_visible = ReadOverflowValue(ledger, own_read);
  ok &= Require(own_visible.ok(), "own uncommitted overflow value visible");
  ok &= Require(own_visible.payload_bytes == payload, "own uncommitted payload reconstructs byte-identically");

  OverflowReadRequest committed_read;
  committed_read.overflow_value_uuid = persisted.overflow_value_uuid;
  const auto not_yet_visible = ReadOverflowValue(ledger, committed_read);
  ok &= Require(!not_yet_visible.ok(), "uncommitted overflow hidden from ordinary read");

  const auto committed = CommitOverflowValue(
      &ledger,
      OverflowCommitRequest{persisted.overflow_value_uuid, fixture.transaction_uuid, 55, "probe_commit"});
  ok &= Require(committed.ok(), "overflow commit succeeds");
  const auto visible = ReadOverflowValue(ledger, committed_read);
  ok &= Require(visible.ok(), "committed overflow visible");
  ok &= Require(visible.payload_bytes == payload, "committed payload reconstructs byte-identically");

  const Fixture rollback_fixture{Id(scratchbird::core::platform::UuidKind::row, 10200),
                                 fixture.object_uuid,
                                 Id(scratchbird::core::platform::UuidKind::transaction, 10202),
                                 fixture.policy_uuid};
  OverflowLedger rollback_ledger;
  const auto rollback_persisted = PersistOverflowValue(&rollback_ledger, Request(rollback_fixture, payload));
  ok &= Require(rollback_persisted.ok(), "rollback fixture persisted");
  const auto rolled_back = RollbackOverflowValue(
      &rollback_ledger,
      OverflowRollbackRequest{rollback_persisted.overflow_value_uuid,
                              rollback_fixture.transaction_uuid,
                              55,
                              "probe_rollback"});
  ok &= Require(rolled_back.ok(), "overflow rollback succeeds");
  OverflowReadRequest rollback_read;
  rollback_read.overflow_value_uuid = rollback_persisted.overflow_value_uuid;
  const auto hidden_after_rollback = ReadOverflowValue(rollback_ledger, rollback_read);
  ok &= Require(!hidden_after_rollback.ok(), "rolled-back overflow not visible");
  ok &= Require(hidden_after_rollback.diagnostic.diagnostic_code == "overflow_read_not_visible",
                "rolled-back read diagnostic");

  const auto cleanup_refused = CleanupOverflowValues(&rollback_ledger, OverflowCleanupRequest{100, false, "probe_cleanup"});
  ok &= Require(!cleanup_refused.ok(), "cleanup without authoritative horizon refused");
  ok &= Require(cleanup_refused.diagnostic.diagnostic_code == "overflow_cleanup_horizon_not_authoritative",
                "cleanup horizon diagnostic");

  const auto cleanup = CleanupOverflowValues(&rollback_ledger, OverflowCleanupRequest{100, true, "probe_cleanup"});
  ok &= Require(cleanup.ok(), "cleanup with authoritative horizon succeeds");
  ok &= Require(cleanup.cleaned_count == 1, "cleanup reclaimed one value");
  const auto* cleaned = FindOverflowValue(rollback_ledger, rollback_persisted.overflow_value_uuid);
  ok &= Require(cleaned != nullptr && cleaned->state == OverflowValueState::cleanup_reclaimed, "cleaned state tracked");
  ok &= Require(cleaned != nullptr && cleaned->chunks.empty(), "cleaned chunks reclaimed");

  const auto recovery = ClassifyOverflowLedgerForRecovery(ledger);
  ok &= Require(recovery.ok(), "overflow recovery classification succeeds");
  ok &= Require(!recovery.classifications.empty(), "overflow recovery rows produced");
  ok &= Require(recovery.classifications.front().action == OverflowRecoveryAction::retain,
                "committed overflow retained during recovery");

  OverflowLedger invalid_ledger;
  auto invalid_request = Request(fixture, payload);
  invalid_request.value_descriptor.clear();
  const auto invalid = PersistOverflowValue(&invalid_ledger, invalid_request);
  ok &= Require(!invalid.ok(), "missing descriptor refused");
  ok &= Require(invalid.diagnostic.diagnostic_code == "overflow_persist_missing_descriptor",
                "missing descriptor diagnostic");

  return ok ? 0 : 1;
}
