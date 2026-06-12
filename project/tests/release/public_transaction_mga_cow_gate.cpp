// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "copy_on_write.hpp"
#include "isolation.hpp"
#include "memory.hpp"
#include "row_data_page.hpp"
#include "row_version.hpp"
#include "transaction_manager.hpp"
#include "transaction_prepare.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace memory = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace txn = scratchbird::transaction::mga;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770000000000ull;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_transaction_mga_cow_gate";
  policy.hard_limit_bytes = 2 * 1024 * 1024;
  policy.soft_limit_bytes = 2 * 1024 * 1024;
  policy.per_context_limit_bytes = 2 * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 2 * 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

txn::RowIdentity MakeRequiredRowIdentity(TypedUuid row_uuid, bool* ok) {
  const auto row = txn::MakeRowIdentity(row_uuid);
  *ok = Expect(row.ok(), "row identity should validate") && *ok;
  return row.identity;
}

txn::RowVersionMetadata Version(txn::RowIdentity row,
                                const txn::TransactionInventoryEntry& creator,
                                u64 sequence,
                                txn::RowVersionState state,
                                txn::TransactionState creator_state,
                                bool payload_present) {
  txn::RowVersionMetadata metadata;
  metadata.identity.row = row;
  metadata.identity.creator_transaction = creator.identity;
  metadata.identity.version_sequence = sequence;
  metadata.state = state;
  metadata.creator_transaction_state = creator_state;
  metadata.payload_present = payload_present;
  return metadata;
}

bool ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(MemoryPolicy(),
                                                      "public_transaction_mga_cow_gate");
  return Expect(configured.ok(), "transaction gate memory fixture should configure") &&
         Expect(configured.fixture_mode, "transaction gate memory must be fixture mode");
}

bool IsolationLevelsAreExplicit() {
  bool ok = true;
  ok = Expect(txn::ValidateLocalIsolationLevel(txn::IsolationLevel::read_committed).ok(),
              "read committed should be supported") && ok;
  ok = Expect(txn::ValidateLocalIsolationLevel(txn::IsolationLevel::repeatable_read).ok(),
              "repeatable read should be supported") && ok;
  ok = Expect(txn::ValidateLocalIsolationLevel(txn::IsolationLevel::serializable).ok(),
              "serializable should be supported") && ok;
  ok = Expect(!txn::ValidateLocalIsolationLevel(txn::IsolationLevel::reference_compatibility).ok(),
              "reference compatibility isolation must not be local production authority") && ok;
  return ok;
}

bool CowPhaseAndVisibilityProof() {
  bool ok = true;
  txn::LocalTransactionManager manager;
  const auto tx1 = manager.Begin(MakeUuid(UuidKind::transaction, 10), kBaseMillis + 100);
  ok = Expect(tx1.ok(), "tx1 begin should succeed") && ok;
  const auto tx2 = manager.Begin(MakeUuid(UuidKind::transaction, 20), kBaseMillis + 110);
  ok = Expect(tx2.ok(), "tx2 begin should succeed") && ok;
  if (!ok) {
    return false;
  }

  const auto row = MakeRequiredRowIdentity(MakeUuid(UuidKind::row, 30), &ok);
  const auto insert = txn::PlanLocalCopyOnWriteMutationForTransaction(
      tx1.entry, row, txn::CopyOnWriteMutationKind::insert, 0, 1);
  ok = Expect(insert.ok(), "insert COW plan should succeed") && ok;
  ok = Expect(insert.mutation.resulting_row_state == txn::RowVersionState::uncommitted,
              "insert COW should create uncommitted row version") && ok;

  auto allocated = txn::AdvanceCopyOnWriteMutationPhase(
      insert.mutation, txn::CopyOnWriteMutationPhase::new_version_allocated);
  ok = Expect(allocated.ok(), "COW should advance to new_version_allocated") && ok;
  auto payload = txn::AdvanceCopyOnWriteMutationPhase(
      allocated.mutation, txn::CopyOnWriteMutationPhase::payload_written_unpublished);
  ok = Expect(payload.ok(), "COW should advance to payload_written_unpublished") && ok;
  auto publish_pending = txn::AdvanceCopyOnWriteMutationPhase(
      payload.mutation, txn::CopyOnWriteMutationPhase::publish_pending_transaction);
  ok = Expect(publish_pending.ok(), "COW should advance to publish_pending_transaction") && ok;
  auto published_without_evidence = publish_pending.mutation;
  published_without_evidence.phase = txn::CopyOnWriteMutationPhase::published;
  ok = Expect(!txn::ValidateCopyOnWriteMutationState(published_without_evidence).ok(),
              "published COW without evidence must be refused") && ok;
  published_without_evidence.evidence_record_written = true;
  ok = Expect(txn::ValidateCopyOnWriteMutationState(published_without_evidence).ok(),
              "published COW with evidence should validate") && ok;

  auto recovery_required = txn::AdvanceCopyOnWriteMutationPhase(
      payload.mutation, txn::CopyOnWriteMutationPhase::recovery_required);
  ok = Expect(recovery_required.ok(), "failed publish COW should advance to recovery_required") && ok;

  const auto delete_plan = txn::PlanLocalCopyOnWriteMutationForTransaction(
      tx2.entry, row, txn::CopyOnWriteMutationKind::delete_row, 1, 2);
  ok = Expect(delete_plan.ok(), "delete COW plan should succeed") && ok;
  ok = Expect(delete_plan.mutation.resulting_row_state == txn::RowVersionState::delete_marker,
              "delete COW should create delete marker") && ok;
  ok = Expect(!delete_plan.mutation.intent.payload_required,
              "delete marker COW should not require row payload") && ok;

  const auto read_only =
      txn::BeginLocalReadOnlyTransaction(manager.inventory(), MakeUuid(UuidKind::transaction, 40), kBaseMillis + 120);
  ok = Expect(read_only.ok(), "read-only transaction begin should succeed") && ok;
  const auto read_only_write = txn::PlanLocalCopyOnWriteMutationForTransaction(
      read_only.entry, row, txn::CopyOnWriteMutationKind::update, 1, 3);
  ok = Expect(!read_only_write.ok(), "read-only transaction must not plan COW mutation") && ok;

  const auto tx1_committed = manager.Commit(tx1.entry.identity.local_id, kBaseMillis + 200);
  ok = Expect(tx1_committed.ok(), "tx1 commit should succeed") && ok;
  const auto tx2_snapshot = manager.Snapshot(tx2.entry.identity.local_id);
  ok = Expect(tx2_snapshot.ok(), "tx2 snapshot should succeed") && ok;
  if (!ok) {
    return false;
  }

  auto committed = Version(row,
                           tx1_committed.entry,
                           1,
                           txn::RowVersionState::committed,
                           txn::TransactionState::committed,
                           true);
  auto own_uncommitted = Version(row,
                                 tx2.entry,
                                 2,
                                 txn::RowVersionState::uncommitted,
                                 txn::TransactionState::active,
                                 true);
  own_uncommitted.chain.previous_version_sequence = committed.identity.version_sequence;
  own_uncommitted.chain.previous_version_uuid = MakeUuid(UuidKind::row, 31);

  auto tx2_visibility = txn::SnapshotPolicyForIsolation(txn::IsolationLevel::read_committed,
                                                        tx2_snapshot.snapshot);
  ok = Expect(txn::EvaluateVisibility(committed, tx2_visibility).decision ==
                  txn::VisibilityDecision::visible,
              "committed tx1 row should be visible to tx2") && ok;
  ok = Expect(txn::EvaluateVisibility(own_uncommitted, tx2_visibility).decision ==
                  txn::VisibilityDecision::visible,
              "tx2 should see its own uncommitted row version") && ok;
  auto foreign_uncommitted = own_uncommitted;
  foreign_uncommitted.identity.creator_transaction = tx1.entry.identity;
  foreign_uncommitted.creator_transaction_state = txn::TransactionState::active;
  ok = Expect(txn::EvaluateVisibility(foreign_uncommitted, tx2_visibility).decision ==
                  txn::VisibilityDecision::wait_for_transaction,
              "foreign uncommitted row should wait for transaction finality") && ok;
  auto delete_marker = Version(row,
                               tx2.entry,
                               3,
                               txn::RowVersionState::delete_marker,
                               txn::TransactionState::active,
                               false);
  ok = Expect(txn::EvaluateVisibility(delete_marker, tx2_visibility).decision ==
                  txn::VisibilityDecision::invisible,
              "delete marker should not be visible as row payload") && ok;

  return ok;
}

bool IsolationHighWaterAndRollbackProof() {
  bool ok = true;

  txn::LocalTransactionManager zero_boundary_manager;
  const auto first_reader =
      zero_boundary_manager.Begin(MakeUuid(UuidKind::transaction, 87),
                                  kBaseMillis + 540);
  ok = Expect(first_reader.ok(),
              "first reader transaction should begin before any commit") && ok;
  const auto first_later_writer =
      zero_boundary_manager.Begin(MakeUuid(UuidKind::transaction, 88),
                                  kBaseMillis + 550);
  ok = Expect(first_later_writer.ok(),
              "first later writer should begin") && ok;
  const auto first_later_commit =
      zero_boundary_manager.Commit(first_later_writer.entry.identity.local_id,
                                   kBaseMillis + 560);
  ok = Expect(first_later_commit.ok(),
              "first later writer should commit") && ok;
  const auto first_snapshot =
      zero_boundary_manager.Snapshot(first_reader.entry.identity.local_id);
  ok = Expect(first_snapshot.ok(),
              "first reader snapshot should be created") && ok;
  if (!ok) {
    return false;
  }
  ok = Expect(!first_snapshot.snapshot.transaction_start_visible_through_local_transaction.valid(),
              "first reader transaction-start high-water should be zero boundary") && ok;
  ok = Expect(first_snapshot.snapshot.visible_through_local_transaction.value ==
                  first_later_commit.entry.identity.local_id.value,
              "first reader statement high-water should include later commit") && ok;
  const auto first_row =
      MakeRequiredRowIdentity(MakeUuid(UuidKind::row, 89), &ok);
  auto first_later_version = Version(first_row,
                                     first_later_commit.entry,
                                     1,
                                     txn::RowVersionState::committed,
                                     txn::TransactionState::committed,
                                     true);
  const auto first_read_committed =
      txn::SnapshotPolicyForIsolation(txn::IsolationLevel::read_committed,
                                      first_snapshot.snapshot);
  const auto first_repeatable =
      txn::SnapshotPolicyForIsolation(txn::IsolationLevel::repeatable_read,
                                      first_snapshot.snapshot);
  ok = Expect(txn::EvaluateVisibility(first_later_version,
                                      first_read_committed)
                      .decision == txn::VisibilityDecision::visible,
              "read committed should use zero-boundary reader statement high-water") && ok;
  ok = Expect(txn::EvaluateVisibility(first_later_version,
                                      first_repeatable)
                      .decision == txn::VisibilityDecision::invisible,
              "repeatable read should treat zero high-water as a real boundary") && ok;

  txn::LocalTransactionManager manager;

  const auto committed_before_reader =
      manager.Begin(MakeUuid(UuidKind::transaction, 90), kBaseMillis + 600);
  ok = Expect(committed_before_reader.ok(),
              "pre-reader transaction should begin") && ok;
  const auto committed_before_reader_done =
      manager.Commit(committed_before_reader.entry.identity.local_id,
                     kBaseMillis + 610);
  ok = Expect(committed_before_reader_done.ok(),
              "pre-reader transaction should commit") && ok;

  const auto reader =
      manager.Begin(MakeUuid(UuidKind::transaction, 91), kBaseMillis + 620);
  ok = Expect(reader.ok(), "reader transaction should begin") && ok;

  const auto committed_after_reader =
      manager.Begin(MakeUuid(UuidKind::transaction, 92), kBaseMillis + 630);
  ok = Expect(committed_after_reader.ok(),
              "post-reader transaction should begin") && ok;
  const auto committed_after_reader_done =
      manager.Commit(committed_after_reader.entry.identity.local_id,
                     kBaseMillis + 640);
  ok = Expect(committed_after_reader_done.ok(),
              "post-reader transaction should commit") && ok;

  const auto snapshot = manager.Snapshot(reader.entry.identity.local_id);
  ok = Expect(snapshot.ok(), "reader snapshot should be created") && ok;
  if (!ok) {
    return false;
  }
  ok = Expect(snapshot.snapshot.visible_through_local_transaction.value ==
                  committed_after_reader_done.entry.identity.local_id.value,
              "statement high-water should include later committed transaction") && ok;
  ok = Expect(snapshot.snapshot.transaction_start_visible_through_local_transaction.value ==
                  committed_before_reader_done.entry.identity.local_id.value,
              "transaction high-water should remain fixed at begin") && ok;

  const auto row_before = MakeRequiredRowIdentity(MakeUuid(UuidKind::row, 93), &ok);
  const auto row_after = MakeRequiredRowIdentity(MakeUuid(UuidKind::row, 94), &ok);
  auto before_version = Version(row_before,
                                committed_before_reader_done.entry,
                                1,
                                txn::RowVersionState::committed,
                                txn::TransactionState::committed,
                                true);
  auto after_version = Version(row_after,
                               committed_after_reader_done.entry,
                               1,
                               txn::RowVersionState::committed,
                               txn::TransactionState::committed,
                               true);

  const auto read_committed =
      txn::SnapshotPolicyForIsolation(txn::IsolationLevel::read_committed,
                                      snapshot.snapshot);
  const auto repeatable =
      txn::SnapshotPolicyForIsolation(txn::IsolationLevel::repeatable_read,
                                      snapshot.snapshot);
  const auto serializable =
      txn::SnapshotPolicyForIsolation(txn::IsolationLevel::serializable,
                                      snapshot.snapshot);

  ok = Expect(txn::EvaluateVisibility(before_version, read_committed).decision ==
                  txn::VisibilityDecision::visible,
              "read committed should see pre-reader commit") && ok;
  ok = Expect(txn::EvaluateVisibility(after_version, read_committed).decision ==
                  txn::VisibilityDecision::visible,
              "read committed should see statement-time committed row") && ok;
  ok = Expect(txn::EvaluateVisibility(after_version, repeatable).decision ==
                  txn::VisibilityDecision::invisible,
              "repeatable read should not see post-begin commit") && ok;
  ok = Expect(txn::EvaluateVisibility(after_version, serializable).decision ==
                  txn::VisibilityDecision::invisible,
              "serializable should not see post-begin commit") && ok;

  const auto rollback_tx =
      manager.Begin(MakeUuid(UuidKind::transaction, 95), kBaseMillis + 650);
  ok = Expect(rollback_tx.ok(), "rollback transaction should begin") && ok;
  const auto rolled_back =
      manager.Rollback(rollback_tx.entry.identity.local_id, kBaseMillis + 660);
  ok = Expect(rolled_back.ok(), "rollback transaction should roll back") && ok;
  ok = Expect(rolled_back.entry.evidence_record_written,
              "rolled-back transaction should write durable inventory evidence") && ok;
  ok = Expect(!manager.Commit(rollback_tx.entry.identity.local_id,
                              kBaseMillis + 670).ok(),
              "committing a rolled-back transaction should fail closed") && ok;

  auto rolled_back_version = Version(row_after,
                                     rolled_back.entry,
                                     2,
                                     txn::RowVersionState::rolled_back,
                                     txn::TransactionState::rolled_back,
                                     false);
  ok = Expect(txn::EvaluateVisibility(rolled_back_version, read_committed).decision ==
                  txn::VisibilityDecision::invisible,
              "rolled-back row version should be invisible") && ok;

  const auto read_only =
      txn::BeginLocalReadOnlyTransaction(manager.inventory(),
                                         MakeUuid(UuidKind::transaction, 96),
                                         kBaseMillis + 680);
  ok = Expect(read_only.ok(), "read-only inventory transaction should begin") && ok;
  const auto read_only_commit =
      txn::CommitLocalTransaction(read_only.inventory,
                                  read_only.entry.identity.local_id,
                                  kBaseMillis + 690);
  ok = Expect(read_only_commit.ok(),
              "read-only transaction should commit through inventory authority") && ok;
  ok = Expect(read_only_commit.entry.evidence_record_written,
              "read-only commit should write durable inventory evidence") && ok;
  return ok;
}

bool PreparedAndHotKeyChangeProof() {
  bool ok = true;
  txn::LocalTransactionInventory inventory = txn::MakeEmptyLocalTransactionInventory();
  const auto begin = txn::BeginLocalTransaction(inventory, MakeUuid(UuidKind::transaction, 50), kBaseMillis + 300);
  ok = Expect(begin.ok(), "prepare begin should succeed") && ok;
  const auto prepared =
      txn::PrepareLocalTransactionDurable(begin.inventory, begin.entry.identity.local_id);
  ok = Expect(prepared.ok(), "durable prepare should succeed") && ok;
  ok = Expect(prepared.entry.state == txn::TransactionState::prepared,
              "prepared transaction state should be authoritative") && ok;
  ok = Expect(prepared.entry.evidence_record_written,
              "prepared transaction should carry durable evidence") && ok;
  const auto prepared_commit =
      txn::CompletePreparedLocalTransactionCommit(prepared.inventory,
                                                  prepared.entry.identity.local_id,
                                                  kBaseMillis + 400);
  ok = Expect(prepared_commit.ok(), "prepared commit should succeed") && ok;
  ok = Expect(prepared_commit.entry.state == txn::TransactionState::committed,
              "prepared commit should publish committed state") && ok;

  const auto row = MakeRequiredRowIdentity(MakeUuid(UuidKind::row, 60), &ok);
  auto old_version = Version(row,
                             prepared_commit.entry,
                             1,
                             txn::RowVersionState::committed,
                             txn::TransactionState::committed,
                             true);
  auto active = txn::BeginLocalTransaction(prepared_commit.inventory,
                                           MakeUuid(UuidKind::transaction, 70),
                                           kBaseMillis + 500);
  ok = Expect(active.ok(), "active transaction for HOT should begin") && ok;
  auto new_version = Version(row,
                             active.entry,
                             2,
                             txn::RowVersionState::uncommitted,
                             txn::TransactionState::active,
                             true);
  const TypedUuid old_version_uuid = MakeUuid(UuidKind::row, 61);
  new_version.chain.previous_version_uuid = old_version_uuid;
  new_version.chain.previous_version_sequence = old_version.identity.version_sequence;

  txn::VisibilitySnapshot visibility;
  visibility.reader_transaction = active.entry.identity.local_id;
  visibility.visible_through_local_transaction_id = active.entry.identity.local_id.value;
  visibility.allow_reader_own_uncommitted = true;

  txn::HotStableRowHeadProofInput hot;
  hot.old_visible_version = old_version;
  hot.new_version = new_version;
  hot.old_version_uuid = old_version_uuid;
  hot.new_previous_version_uuid = old_version_uuid;
  hot.visibility_snapshot = visibility;
  hot.exact_index_keys_unchanged = true;
  hot.same_page_budget_available = true;
  const auto hot_decision = txn::EvaluateHotStableRowHeadDecision(hot);
  ok = Expect(hot_decision.ok(), "HOT decision should validate with MGA authority") && ok;
  ok = Expect(hot_decision.decision == txn::HotStableRowHeadDecisionKind::page_local_hot,
              "unchanged keys with page budget should use page-local HOT") && ok;

  hot.exact_index_keys_unchanged = false;
  const auto key_change = txn::EvaluateHotStableRowHeadDecision(hot);
  ok = Expect(key_change.ok(), "key-change decision should validate") && ok;
  ok = Expect(key_change.decision == txn::HotStableRowHeadDecisionKind::ordinary_index_rewrite,
              "changed keys should require ordinary index rewrite") && ok;

  hot.exact_index_keys_unchanged = true;
  hot.parser_or_reference_authority = true;
  ok = Expect(!txn::EvaluateHotStableRowHeadDecision(hot).ok(),
              "parser or reference HOT authority must be refused") && ok;
  return ok;
}

bool PhysicalRowPageProof() {
  bool ok = true;
  page::RowDataPageBody body;
  body.relation_uuid = MakeUuid(UuidKind::object, 80);
  body.segment_id = 1;
  body.segment_generation = 1;
  body.page_number = 42;
  body.page_generation = 7;

  page::RowDataRecord live;
  live.row_uuid = MakeUuid(UuidKind::row, 81);
  live.transaction_uuid = MakeUuid(UuidKind::transaction, 82);
  live.local_transaction_id = 1;
  live.row_version = 1;
  body.rows.push_back(live);

  page::RowDataRecord deleted;
  deleted.row_uuid = MakeUuid(UuidKind::row, 83);
  deleted.transaction_uuid = MakeUuid(UuidKind::transaction, 84);
  deleted.local_transaction_id = 2;
  deleted.row_version = 2;
  deleted.deleted = true;
  body.rows.push_back(deleted);

  const auto built = page::BuildRowDataPageBody(body, 16384);
  ok = Expect(built.ok(), "row-data page body should build") && ok;
  const auto parsed = page::ParseRowDataPageBody(built.serialized, body.page_number);
  ok = Expect(parsed.ok(), "row-data page body should parse") && ok;
  ok = Expect(parsed.body.rows.size() == 2, "row-data page should preserve row count") && ok;
  if (!parsed.ok() || parsed.body.rows.size() != 2) {
    return false;
  }
  ok = Expect(parsed.body.rows[0].internal_row_ordinal == 1,
              "row-data page should assign first dense ordinal") && ok;
  ok = Expect(parsed.body.rows[1].internal_row_ordinal == 2,
              "row-data page should assign second dense ordinal") && ok;
  ok = Expect(parsed.body.rows[1].deleted,
              "row-data page should preserve physical delete marker") && ok;

  const auto scope = page::MakeDenseRowOrdinalScope(parsed.body);
  const auto locator = page::MakeDenseRowOrdinalLocator(scope,
                                                        parsed.body.rows[0],
                                                        true,
                                                        true);
  const auto accepted = page::ValidateDenseRowOrdinalLocator(parsed.body, locator);
  ok = Expect(accepted.accepted, "dense row ordinal should validate only with MGA authority") && ok;
  ok = Expect(!accepted.ordinal_is_visibility_or_finality_authority,
              "dense row ordinal must not become visibility/finality authority") && ok;

  const auto refused_locator = page::MakeDenseRowOrdinalLocator(scope,
                                                                parsed.body.rows[0],
                                                                false,
                                                                true);
  const auto refused = page::ValidateDenseRowOrdinalLocator(parsed.body, refused_locator);
  ok = Expect(!refused.accepted, "dense row ordinal should fail closed without durable MGA authority") && ok;
  ok = Expect(refused.durable_mga_inventory_remains_authority,
              "refused dense ordinal should preserve MGA inventory authority") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = ConfigureMemoryFixture() && ok;
  ok = IsolationLevelsAreExplicit() && ok;
  ok = CowPhaseAndVisibilityProof() && ok;
  ok = IsolationHighWaterAndRollbackProof() && ok;
  ok = PreparedAndHotKeyChangeProof() && ok;
  ok = PhysicalRowPageProof() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
