// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "copy_on_write.hpp"
#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "savepoint.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_cleanup.hpp"
#include "transaction_prepare.hpp"
#include "transaction_recovery.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770200000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool ExpectDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                        std::string_view message) {
  if (diagnostic.error) {
    std::cerr << message << ": " << diagnostic.code << ':'
              << diagnostic.message_key << ':' << diagnostic.detail << '\n';
    return false;
  }
  return true;
}

bool ExpectApiOk(const api::EngineApiResult& result,
                 std::string_view message) {
  if (!result.ok) {
    std::cerr << message;
    if (!result.diagnostics.empty()) {
      std::cerr << ": " << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail;
    }
    std::cerr << '\n';
    return false;
  }
  return true;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_transaction_savepoint_limbo_cleanup_gate";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(), "public_transaction_savepoint_limbo_cleanup_gate");
  return Expect(configured.ok(),
                "transaction savepoint/cleanup gate memory should configure") &&
         Expect(configured.fixture_mode,
                "transaction savepoint/cleanup gate must use fixture memory");
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string MakeUuidText(UuidKind kind, u64 offset) {
  return uuid::UuidToString(MakeUuid(kind, offset).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_before_uuid;
  std::string table_after_uuid;
  std::string index_after_uuid;
};

Fixture MakeFixture(const std::filesystem::path& work_dir) {
  Fixture fixture;
  fixture.dir = work_dir / "pcr072_savepoint_marker";
  std::error_code ignored;
  std::filesystem::remove_all(fixture.dir, ignored);
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "pcr072.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = MakeUuid(UuidKind::database, 100);
  create.filespace_uuid = MakeUuid(UuidKind::filespace, 101);
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis + 100;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Expect(created.ok(), "PCR-072 fixture database should be created");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_before_uuid = MakeUuidText(UuidKind::object, 110);
  fixture.table_after_uuid = MakeUuidText(UuidKind::object, 111);
  fixture.index_after_uuid = MakeUuidText(UuidKind::object, 112);
  return fixture;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = MakeUuidText(UuidKind::principal, 120);
  context.session_uuid.canonical = MakeUuidText(UuidKind::object, 121);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  ExpectApiOk(begun, "PCR-072 begin transaction should succeed");

  api::EngineRequestContext context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

api::CrudTableRecord Table(std::string table_uuid,
                           std::string default_name) {
  api::CrudTableRecord table;
  table.table_uuid = std::move(table_uuid);
  table.default_name = std::move(default_name);
  table.columns.push_back({"id", "canonical=integer"});
  table.columns.push_back({"name", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_after_uuid;
  index.table_uuid = fixture.table_after_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "pcr072_idx_after_savepoint";
  index.key_envelopes.push_back("id");
  index.exact_fallback = true;
  return index;
}

api::CrudRowVersionRecord Row(const Fixture& fixture,
                              const api::EngineRequestContext& context) {
  api::CrudRowVersionRecord row;
  row.creator_tx = context.local_transaction_id;
  row.table_uuid = fixture.table_after_uuid;
  row.row_uuid = MakeUuidText(UuidKind::row, 130);
  row.version_uuid = MakeUuidText(UuidKind::row, 131);
  row.values = {{"id", "1"}, {"name", "after-savepoint"}};
  return row;
}

bool HasTable(const api::MgaRelationStoreState& state,
              const std::string& table_uuid) {
  for (const auto& table : state.crud_metadata.tables) {
    if (table.table_uuid == table_uuid) {
      return true;
    }
  }
  return false;
}

bool HasIndex(const api::MgaRelationStoreState& state,
              const std::string& index_uuid) {
  for (const auto& index : state.crud_metadata.indexes) {
    if (index.index_uuid == index_uuid) {
      return true;
    }
  }
  return false;
}

bool DurableSavepointMarkerProof(const std::filesystem::path& work_dir) {
  bool ok = true;
  const Fixture fixture = MakeFixture(work_dir);
  const auto context = Begin(fixture, "pcr072-savepoint");

  ok = Expect(context.local_transaction_id != 0,
              "PCR-072 transaction context should have local transaction id") && ok;
  ok = ExpectDiagnosticOk(
           api::AppendMgaTableMetadata(
               context,
               Table(fixture.table_before_uuid, "pcr072_before_savepoint")),
           "PCR-072 pre-savepoint table metadata should append") && ok;

  api::EngineCreateSavepointRequest create_savepoint;
  create_savepoint.context = context;
  create_savepoint.option_envelopes.push_back("savepoint_name:pcr072_sp");
  ok = ExpectApiOk(api::EngineCreateSavepoint(create_savepoint),
                   "PCR-072 engine savepoint create should succeed") && ok;

  const api::CrudIndexRecord index = Index(fixture, context);
  api::CrudRowVersionRecord row = Row(fixture, context);
  std::uint64_t row_event_sequence = 0;
  ok = ExpectDiagnosticOk(
           api::AppendMgaTableMetadata(
               context,
               Table(fixture.table_after_uuid, "pcr072_after_savepoint")),
           "PCR-072 post-savepoint table metadata should append") && ok;
  ok = ExpectDiagnosticOk(api::AppendMgaIndexMetadata(context, index),
                          "PCR-072 post-savepoint index metadata should append") && ok;
  ok = ExpectDiagnosticOk(api::AppendMgaRowVersion(context, row, &row_event_sequence),
                          "PCR-072 post-savepoint row version should append") && ok;
  ok = Expect(row_event_sequence != 0,
              "PCR-072 row event sequence should be assigned") && ok;
  ok = ExpectDiagnosticOk(
           api::AppendMgaIndexEntriesForIndex(
               context, index, row.row_uuid, row.version_uuid, row.values),
           "PCR-072 post-savepoint index entry should append") && ok;

  const auto before_rollback = api::LoadMgaRelationStoreState(context);
  ok = Expect(before_rollback.ok,
              "PCR-072 MGA relation store should load before rollback") && ok;
  if (before_rollback.ok) {
    ok = Expect(HasTable(before_rollback.state, fixture.table_before_uuid),
                "PCR-072 pre-savepoint table should be visible before rollback") && ok;
    ok = Expect(HasTable(before_rollback.state, fixture.table_after_uuid),
                "PCR-072 post-savepoint table should be visible before rollback") && ok;
    ok = Expect(HasIndex(before_rollback.state, fixture.index_after_uuid),
                "PCR-072 post-savepoint index should be visible before rollback") && ok;
    ok = Expect(before_rollback.state.row_versions.size() == 1,
                "PCR-072 post-savepoint row should be visible before rollback") && ok;
    ok = Expect(before_rollback.state.index_entries.size() == 1,
                "PCR-072 post-savepoint index entry should be visible before rollback") && ok;
  }

  api::EngineRollbackToSavepointRequest rollback_savepoint;
  rollback_savepoint.context = context;
  rollback_savepoint.option_envelopes.push_back("savepoint_name:pcr072_sp");
  ok = ExpectApiOk(api::EngineRollbackToSavepoint(rollback_savepoint),
                   "PCR-072 engine rollback-to-savepoint should succeed") && ok;
  ok = ExpectApiOk(api::EngineRollbackToSavepoint(rollback_savepoint),
                   "PCR-072 repeated rollback-to-savepoint should be idempotent") && ok;

  const auto after_rollback = api::LoadMgaRelationStoreState(context);
  ok = Expect(after_rollback.ok,
              "PCR-072 MGA relation store should load after rollback") && ok;
  if (after_rollback.ok) {
    ok = Expect(HasTable(after_rollback.state, fixture.table_before_uuid),
                "PCR-072 pre-savepoint table should remain visible") && ok;
    ok = Expect(!HasTable(after_rollback.state, fixture.table_after_uuid),
                "PCR-072 post-savepoint table should be hidden after rollback") && ok;
    ok = Expect(!HasIndex(after_rollback.state, fixture.index_after_uuid),
                "PCR-072 post-savepoint index should be hidden after rollback") && ok;
    ok = Expect(after_rollback.state.row_versions.empty(),
                "PCR-072 post-savepoint row should be hidden after rollback") && ok;
    ok = Expect(after_rollback.state.index_entries.empty(),
                "PCR-072 post-savepoint index entry should be hidden after rollback") && ok;
  }

  api::EngineReleaseSavepointRequest release_savepoint;
  release_savepoint.context = context;
  release_savepoint.option_envelopes.push_back("savepoint_name:pcr072_sp");
  ok = ExpectApiOk(api::EngineReleaseSavepoint(release_savepoint),
                   "PCR-072 savepoint release should succeed after rollback") && ok;
  ok = Expect(api::ActiveMgaSavepointNames(context).empty(),
              "PCR-072 released savepoint should not remain active") && ok;
  return ok;
}

txn::TransactionInventoryEntry InventoryEntry(
    u64 local_id,
    txn::TransactionState state,
    txn::TransactionScope scope = txn::TransactionScope::local_node) {
  txn::TransactionInventoryEntry entry;
  entry.identity.local_id = txn::MakeLocalTransactionId(local_id);
  entry.identity.transaction_uuid = MakeUuid(UuidKind::transaction, 1000 + local_id);
  entry.identity.scope = scope;
  entry.state = state;
  entry.begin_unix_epoch_millis = kBaseMillis + 1000 + local_id;
  if (txn::IsTerminalTransactionState(state) ||
      state == txn::TransactionState::prepared ||
      state == txn::TransactionState::limbo) {
    entry.evidence_record_required = true;
    entry.evidence_record_written = true;
  }
  if (txn::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = kBaseMillis + 2000 + local_id;
  }
  return entry;
}

txn::LocalTransactionInventory Inventory(
    std::vector<txn::TransactionInventoryEntry> entries,
    u64 next_local_transaction_id) {
  txn::LocalTransactionInventory inventory;
  inventory.entries = std::move(entries);
  inventory.next_local_transaction_id = next_local_transaction_id;
  return inventory;
}

txn::RowIdentity RowIdentity(u64 offset, bool* ok) {
  const auto row = txn::MakeRowIdentity(MakeUuid(UuidKind::row, offset));
  *ok = Expect(row.ok(), "PCR-072 row identity should validate") && *ok;
  return row.identity;
}

txn::RowVersionMetadata Version(const txn::TransactionInventoryEntry& entry,
                                txn::RowVersionState state,
                                u64 sequence,
                                bool payload_present,
                                bool has_next,
                                u64 successor_local_id,
                                bool* ok) {
  txn::RowVersionMetadata metadata;
  metadata.identity.row = RowIdentity(2000 + sequence, ok);
  metadata.identity.creator_transaction = entry.identity;
  metadata.identity.version_sequence = sequence;
  metadata.state = state;
  metadata.creator_transaction_state = entry.state;
  metadata.payload_present = payload_present;
  if (has_next) {
    metadata.chain.next_version_sequence = sequence + 1;
    metadata.chain.next_version_uuid = MakeUuid(UuidKind::row, 3000 + sequence);
  }
  if (successor_local_id != 0) {
    metadata.successor_transaction_local_id =
        txn::MakeLocalTransactionId(successor_local_id);
  }
  return metadata;
}

bool SavepointUndoStackProof() {
  bool ok = true;
  class ProofUndoExecutor final : public txn::SavepointPhysicalUndoExecutor {
   public:
    bool Supports(txn::SavepointMutationKind kind) const override {
      return kind == txn::SavepointMutationKind::data_page ||
             kind == txn::SavepointMutationKind::index;
    }

    txn::SavepointUndoResult ApplyUndo(const txn::SavepointMutationRecord& mutation) override {
      txn::SavepointUndoResult result;
      result.status = {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
      result.mutation = mutation;
      result.applied = true;
      result.executor_id = "pcr072_savepoint_stack_proof_executor";
      result.durable_evidence_id = "pcr072:" + mutation.stable_operation_id;
      return result;
    }
  };
  ProofUndoExecutor executor;
  txn::SavepointStack stack;
  const auto tx = txn::MakeLocalTransactionId(10);
  ok = Expect(stack.Create(tx, "outer", 10).ok(),
              "PCR-072 savepoint stack create should succeed") && ok;
  ok = Expect(stack.RecordMutation({tx,
                                    11,
                                    txn::SavepointMutationKind::data_page,
                                    "pcr072.data",
                                    true,
                                    true,
                                    false})
                  .ok(),
              "PCR-072 data mutation with undo evidence should record") && ok;
  ok = Expect(stack.RecordMutation({tx,
                                    12,
                                    txn::SavepointMutationKind::index,
                                    "pcr072.index",
                                    true,
                                    true,
                                    false})
                  .ok(),
              "PCR-072 index mutation with undo evidence should record") && ok;
  const auto plan = stack.PlanRollbackTo(tx, "outer");
  ok = Expect(plan.ok(),
              "PCR-072 savepoint rollback plan should be ready") && ok;
  ok = Expect(plan.rollback_actions.size() == 2 &&
                  plan.rollback_actions[0].mutation_sequence == 12 &&
                  plan.rollback_actions[1].mutation_sequence == 11,
              "PCR-072 rollback plan should be reverse mutation order") && ok;
  const auto applied = stack.ExecuteRollbackTo(tx, "outer", &executor);
  ok = Expect(applied.ok() && applied.affected_mutation_count == 2 &&
                  applied.undo_results.size() == 2,
              "PCR-072 rollback should execute two physical undo-backed mutations") && ok;
  const auto repeated = stack.ExecuteRollbackTo(tx, "outer", &executor);
  ok = Expect(repeated.ok() && repeated.affected_mutation_count == 0 &&
                  repeated.undo_results.empty(),
              "PCR-072 repeated rollback should be idempotent") && ok;

  txn::SavepointStack missing_undo;
  ok = Expect(missing_undo.Create(tx, "missing", 20).ok(),
              "PCR-072 missing-undo savepoint should create") && ok;
  ok = Expect(missing_undo.RecordMutation({tx,
                                           21,
                                           txn::SavepointMutationKind::catalog,
                                           "pcr072.catalog",
                                           true,
                                           false,
                                           false})
                  .ok(),
              "PCR-072 mutation without undo evidence should still record") && ok;
  const auto refused = missing_undo.PlanRollbackTo(tx, "missing");
  ok = Expect(!refused.ok() &&
                  refused.decision ==
                      txn::SavepointRollbackDecision::rollback_refused_missing_undo &&
                  refused.diagnostic.diagnostic_code ==
                      "SB-SNTXN-SAVEPOINT-ROLLBACK-MISSING-UNDO",
              "PCR-072 missing undo evidence should fail closed") && ok;
  return ok;
}

bool PreparedAndLimboResolutionProof() {
  bool ok = true;
  txn::LocalTransactionInventory inventory =
      txn::MakeEmptyLocalTransactionInventory();
  const auto begun = txn::BeginLocalTransaction(
      inventory, MakeUuid(UuidKind::transaction, 4000), kBaseMillis + 4000);
  ok = Expect(begun.ok(), "PCR-072 prepare transaction should begin") && ok;
  const auto prepared = txn::PrepareLocalTransactionDurable(
      begun.inventory, begun.entry.identity.local_id);
  ok = Expect(prepared.ok(),
              "PCR-072 durable prepare should succeed") && ok;
  ok = Expect(prepared.entry.evidence_record_written,
              "PCR-072 durable prepare should write evidence") && ok;
  const auto prepared_classification =
      txn::ClassifyLocalTransactionForRecovery(prepared.entry);
  ok = Expect(prepared_classification.action ==
                  txn::TransactionRecoveryAction::prepared_waiting_local_decision &&
                  prepared_classification.fail_closed,
              "PCR-072 prepared recovery should wait for local decision and fence writes") && ok;
  const auto prepared_rollback =
      txn::CompletePreparedLocalTransactionRollback(
          prepared.inventory, prepared.entry.identity.local_id,
          kBaseMillis + 4100);
  ok = Expect(prepared_rollback.ok() &&
                  prepared_rollback.entry.state ==
                      txn::TransactionState::rolled_back,
              "PCR-072 prepared rollback decision should publish rollback") && ok;

  auto local_limbo = Inventory(
      {InventoryEntry(1, txn::TransactionState::limbo)}, 2);
  const auto limbo_classification =
      txn::ClassifyLocalTransactionForRecovery(local_limbo.entries.front());
  ok = Expect(limbo_classification.action ==
                  txn::TransactionRecoveryAction::limbo_requires_operator &&
                  limbo_classification.fail_closed,
              "PCR-072 limbo recovery should require operator decision") && ok;
  const auto refused = txn::ResolveLimboLocalTransactionWithOperatorDecision(
      local_limbo,
      txn::MakeLocalTransactionId(1),
      txn::LimboOperatorDecision::rollback,
      kBaseMillis + 4200,
      {});
  ok = Expect(!refused.ok() &&
                  refused.diagnostic.diagnostic_code ==
                      "SB-SNTXN-LIMBO-OPERATOR-AUTHORITY-REQUIRED",
              "PCR-072 limbo resolution without authoritative evidence should fail closed") && ok;

  txn::LimboOperatorResolutionPolicy local_policy;
  local_policy.operator_decision_authoritative = true;
  local_policy.operator_evidence_reference =
      "operator://pcr072/local-limbo-rollback";
  const auto local_resolved =
      txn::ResolveLimboLocalTransactionWithOperatorDecision(
          std::move(local_limbo),
          txn::MakeLocalTransactionId(1),
          txn::LimboOperatorDecision::rollback,
          kBaseMillis + 4300,
          local_policy);
  ok = Expect(local_resolved.ok() &&
                  local_resolved.entry.state ==
                      txn::TransactionState::rolled_back &&
                  local_resolved.entry.evidence_record_written,
              "PCR-072 local limbo rollback should require and write operator evidence") && ok;

  auto cluster_limbo = Inventory(
      {InventoryEntry(7,
                      txn::TransactionState::limbo,
                      txn::TransactionScope::cluster_global)},
      8);
  const auto cluster_refused =
      txn::ResolveLimboLocalTransactionWithOperatorDecision(
          cluster_limbo,
          txn::MakeLocalTransactionId(7),
          txn::LimboOperatorDecision::commit,
          kBaseMillis + 4400,
          local_policy);
  ok = Expect(!cluster_refused.ok() &&
                  cluster_refused.diagnostic.diagnostic_code ==
                      "SB-SNTXN-LIMBO-EXTERNAL-PROVIDER-REQUIRED",
              "PCR-072 cluster limbo should require external provider evidence") && ok;

  txn::LimboOperatorResolutionPolicy external_policy = local_policy;
  external_policy.external_cluster_provider_decision_authoritative = true;
  external_policy.operator_evidence_reference =
      "external-cluster-provider://pcr072/cluster-limbo-commit";
  const auto cluster_resolved =
      txn::ResolveLimboLocalTransactionWithOperatorDecision(
          std::move(cluster_limbo),
          txn::MakeLocalTransactionId(7),
          txn::LimboOperatorDecision::commit,
          kBaseMillis + 4500,
          external_policy);
  ok = Expect(cluster_resolved.ok() &&
                  cluster_resolved.entry.state ==
                      txn::TransactionState::committed,
              "PCR-072 cluster limbo should resolve only with external provider evidence") && ok;
  return ok;
}

bool CleanupReclaimEvidenceProof() {
  bool ok = true;
  const auto committed_old =
      InventoryEntry(1, txn::TransactionState::committed);
  const auto rolled_back =
      InventoryEntry(2, txn::TransactionState::rolled_back);
  const auto committed_successor =
      InventoryEntry(3, txn::TransactionState::committed);
  const auto committed_current =
      InventoryEntry(4, txn::TransactionState::committed);
  const auto inventory = Inventory({committed_old,
                                    rolled_back,
                                    committed_successor,
                                    committed_current},
                                   5);

  auto old_version = Version(committed_old,
                             txn::RowVersionState::committed,
                             10,
                             true,
                             true,
                             committed_successor.identity.local_id.value,
                             &ok);
  txn::CleanupHorizonVector provisional_horizons;
  provisional_horizons.horizons.push_back(
      {txn::CleanupHoldKind::oldest_interesting_transaction,
       txn::MakeLocalTransactionId(5),
       true,
       "pcr072_provisional_oit"});
  const auto provisional =
      txn::EvaluateCleanupEligibility(old_version, provisional_horizons);
  ok = Expect(provisional.decision ==
                  txn::CleanupEligibilityDecision::eligible_requires_authority,
              "PCR-072 provisional COW cleanup should require authoritative inventory") && ok;

  txn::LocalCleanupWorksetRequest workset;
  workset.inventory = inventory;
  workset.inventory_authoritative = true;
  workset.inventory_complete = true;
  workset.emit_reclaim_evidence_records = true;
  workset.max_reclaim_evidence_records = 2;
  workset.row_versions = {
      old_version,
      Version(rolled_back,
              txn::RowVersionState::rolled_back,
              20,
              false,
              false,
              0,
              &ok),
      Version(committed_current,
              txn::RowVersionState::committed,
              30,
              true,
              false,
              0,
              &ok),
  };
  if (!ok) {
    return false;
  }

  const auto cleanup =
      txn::ApplyLocalCleanupWithAuthoritativeInventory(workset);
  ok = Expect(cleanup.ok(),
              "PCR-072 authoritative cleanup workset should succeed") && ok;
  ok = Expect(cleanup.reclaimed_row_version_count == 2,
              "PCR-072 cleanup should identify two reclaimable versions") && ok;
  ok = Expect(cleanup.retained_row_version_count == 1,
              "PCR-072 cleanup should retain current version") && ok;
  ok = Expect(cleanup.reclaim_evidence_records.size() == 2,
              "PCR-072 cleanup should emit bounded reclaim evidence") && ok;
  ok = Expect(!cleanup.physical_storage_mutated,
              "PCR-072 cleanup evidence must not claim physical storage mutation") && ok;
  if (cleanup.reclaim_evidence_records.size() == 2) {
    ok = Expect(cleanup.reclaim_evidence_records[0].stable_evidence_id.find(
                    "mga-cleanup-reclaim:") == 0,
                "PCR-072 reclaim evidence id should be stable") && ok;
    ok = Expect(cleanup.reclaim_evidence_records[0]
                        .authoritative_cleanup_horizon_local_transaction_id == 5,
                "PCR-072 reclaim evidence should bind cleanup horizon") && ok;
  }

  auto non_authoritative = workset;
  non_authoritative.inventory_authoritative = false;
  const auto refused =
      txn::ApplyLocalCleanupWithAuthoritativeInventory(non_authoritative);
  ok = Expect(!refused.ok() &&
                  refused.diagnostic.diagnostic_code ==
                      "SB-SNTXN-CLEANUP-HORIZON-NOT-AUTHORITATIVE",
              "PCR-072 non-authoritative cleanup should fail closed") && ok;

  auto over_limit = workset;
  over_limit.max_reclaim_evidence_records = 1;
  const auto evidence_limit =
      txn::ApplyLocalCleanupWithAuthoritativeInventory(over_limit);
  ok = Expect(!evidence_limit.ok() &&
                  evidence_limit.diagnostic.diagnostic_code ==
                      "SB-SNTXN-CLEANUP-RECLAIM-EVIDENCE-LIMIT-EXCEEDED",
              "PCR-072 reclaim evidence limit should fail closed") && ok;

  txn::LocalGarbageCollectionSweepRequest sweep;
  sweep.workset = workset;
  sweep.family = txn::LocalCleanupSweepFamily::maintenance;
  sweep.engine_mga_authoritative = true;
  sweep.max_candidate_row_versions = 8;
  sweep.max_retained_row_versions = 8;
  sweep.retain_row_versions_in_result = true;
  const auto sweep_result = txn::RunLocalGarbageCollectionSweep(sweep);
  ok = Expect(sweep_result.ok() &&
                  sweep_result.cleanup.reclaim_evidence_records.size() == 2,
              "PCR-072 bounded sweep should preserve reclaim evidence") && ok;

  sweep.engine_mga_authoritative = false;
  const auto external_sweep = txn::RunLocalGarbageCollectionSweep(sweep);
  ok = Expect(!external_sweep.ok() &&
                  external_sweep.diagnostic.diagnostic_code ==
                      "SB-SNTXN-CLEANUP-SWEEP-NOT-ENGINE-AUTHORITATIVE",
              "PCR-072 non-engine cleanup sweep should fail closed") && ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path work_dir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::path("public_transaction_savepoint_limbo_cleanup_gate_tmp");
  std::error_code ignored;
  std::filesystem::remove_all(work_dir, ignored);
  std::filesystem::create_directories(work_dir);

  bool ok = ConfigureMemoryFixture();
  ok = DurableSavepointMarkerProof(work_dir) && ok;
  ok = SavepointUndoStackProof() && ok;
  ok = PreparedAndLimboResolutionProof() && ok;
  ok = CleanupReclaimEvidenceProof() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
