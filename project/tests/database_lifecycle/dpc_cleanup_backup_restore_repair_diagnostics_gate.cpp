// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle_test_memory.hpp"
#include "observability/cleanup_diagnostics_api.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_CLEANUP_BACKUP_RESTORE_REPAIR_DIAGNOSTICS_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::u64 NextMillis() {
  static platform::u64 next = 1779524000000ull;
  return ++next;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "DPC-034 generated UUID creation failed");
  return generated.value;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "DPC-034 transaction identity creation failed");
  return identity.identity;
}

mga::TransactionInventoryEntry Entry(platform::u64 local_id,
                                     mga::TransactionState state) {
  mga::TransactionInventoryEntry entry;
  entry.identity = NewIdentity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = NextMillis();
  if (mga::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = NextMillis();
    entry.evidence_record_written = true;
  }
  return entry;
}

mga::LocalTransactionInventory Inventory(
    std::vector<mga::TransactionInventoryEntry> entries,
    platform::u64 next_local_transaction_id) {
  mga::LocalTransactionInventory inventory;
  inventory.entries = std::move(entries);
  inventory.next_local_transaction_id = next_local_transaction_id;
  return inventory;
}

mga::AuthoritativeCleanupHorizonRequest HorizonRequest(
    mga::LocalTransactionInventory inventory) {
  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = std::move(inventory);
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  return request;
}

mga::RowIdentity Row() {
  mga::RowIdentity row;
  row.row_uuid = NewUuid(platform::UuidKind::row);
  return row;
}

mga::RowVersionMetadata Version(const mga::RowIdentity& row,
                                const mga::TransactionInventoryEntry& creator,
                                mga::RowVersionState state,
                                platform::u64 sequence,
                                platform::u64 next_sequence = 0,
                                platform::u64 successor_local_id = 0) {
  mga::RowVersionMetadata metadata;
  metadata.identity.row = row;
  metadata.identity.creator_transaction = creator.identity;
  metadata.identity.version_sequence = sequence;
  metadata.state = state;
  metadata.creator_transaction_state = creator.state;
  metadata.payload_present = state != mga::RowVersionState::rolled_back;
  if (next_sequence != 0) {
    metadata.chain.next_version_sequence = next_sequence;
  }
  if (successor_local_id != 0) {
    metadata.successor_transaction_local_id =
        mga::MakeLocalTransactionId(successor_local_id);
  }
  return metadata;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::database).value);
  context.node_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::object).value);
  context.session_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::object).value);
  context.principal_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::principal).value);
  context.transaction_uuid.canonical =
      uuid::UuidToString(NewUuid(platform::UuidKind::transaction).value);
  context.local_transaction_id = 42;
  context.trace_tags.push_back("right:MGA_CLEANUP_INSPECT");
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "dpc_cleanup_backup_restore_repair_diagnostics_gate",
      {"MGA_CLEANUP_INSPECT", "OBS_MANAGEMENT_INSPECT"});
  return context;
}

agents::StorageVersionCleanupAgentResult StorageCleanupSuccess() {
  const auto old = Entry(1, mga::TransactionState::committed);
  const auto successor = Entry(2, mga::TransactionState::committed);
  const auto rolled_back = Entry(3, mga::TransactionState::rolled_back);
  const auto row = Row();

  agents::StorageVersionCleanupAgentRequest request;
  request.horizon_request =
      HorizonRequest(Inventory({old, successor, rolled_back}, 4));
  request.row_versions = {
      Version(row, old, mga::RowVersionState::committed, 10, 20, 2),
      Version(row, successor, mga::RowVersionState::committed, 20),
      Version(Row(), rolled_back, mga::RowVersionState::rolled_back, 30)};
  request.max_candidate_row_versions = 32;
  request.engine_mga_authoritative = true;
  return agents::RunStorageVersionCleanupAgentBatch(request);
}

idx::SecondaryIndexDeltaLedgerRecord MergedCleanedRecord(
    const platform::TypedUuid& index_uuid,
    const platform::TypedUuid& table_uuid,
    platform::u64 local_transaction_id,
    std::string key_payload) {
  idx::SecondaryIndexDeltaLedgerRecord record;
  record.delta.delta_id = NewUuid(platform::UuidKind::object);
  record.delta.index_uuid = index_uuid;
  record.delta.table_uuid = table_uuid;
  record.delta.row_uuid = NewUuid(platform::UuidKind::row);
  record.delta.version_uuid = NewUuid(platform::UuidKind::row);
  record.delta.transaction_uuid = NewUuid(platform::UuidKind::transaction);
  record.delta.local_transaction_id = local_transaction_id;
  record.delta.delta_kind = idx::SecondaryIndexDeltaKind::insert;
  record.delta.key_payload = std::move(key_payload);
  record.delta.committed = true;
  record.commit_state = idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned;
  record.source_evidence_reference = "dpc034_cleanup_diagnostics";
  return record;
}

agents::IndexGarbageCleanupAgentRequest IndexCleanupRequest(
    mga::AuthoritativeCleanupHorizonRequest horizon,
    platform::u64 ledger_local_transaction_id) {
  const auto index_uuid = NewUuid(platform::UuidKind::object);
  const auto table_uuid = NewUuid(platform::UuidKind::object);

  agents::IndexGarbageCleanupAgentRequest request;
  request.horizon_request = std::move(horizon);
  request.index_uuid = index_uuid;
  request.table_uuid = table_uuid;
  request.ledger.records.push_back(MergedCleanedRecord(index_uuid,
                                                       table_uuid,
                                                       ledger_local_transaction_id,
                                                       "alpha"));
  request.index_kind = idx::SecondaryIndexKind::non_unique;
  request.max_records_to_scan = 32;
  request.max_records_to_clean = 32;
  request.engine_mga_authoritative = true;
  return request;
}

agents::IndexGarbageCleanupAgentResult IndexCleanupSuccess() {
  return agents::RunIndexGarbageCleanupAgentBatch(
      IndexCleanupRequest(HorizonRequest(Inventory({
                            Entry(1, mga::TransactionState::committed),
                          }, 2)),
                          1));
}

agents::IndexGarbageCleanupAgentResult IndexCleanupHorizonBlocked() {
  return agents::RunIndexGarbageCleanupAgentBatch(
      IndexCleanupRequest(HorizonRequest(Inventory({
                            Entry(1, mga::TransactionState::committed),
                            Entry(2, mga::TransactionState::active),
                            Entry(3, mga::TransactionState::committed),
                          }, 4)),
                          3));
}

agents::IndexGarbageCleanupAgentResult IndexCleanupValidationRefused() {
  const auto index_uuid = NewUuid(platform::UuidKind::object);
  const auto table_uuid = NewUuid(platform::UuidKind::object);
  const auto row_uuid = NewUuid(platform::UuidKind::row);
  const auto version_uuid = NewUuid(platform::UuidKind::row);

  agents::IndexGarbageCleanupAgentRequest request;
  request.horizon_request = HorizonRequest(Inventory({
      Entry(1, mga::TransactionState::committed),
  }, 2));
  request.index_uuid = index_uuid;
  request.table_uuid = table_uuid;
  idx::SecondaryIndexBaseEntry base;
  base.index_uuid = index_uuid;
  base.table_uuid = table_uuid;
  base.row_uuid = row_uuid;
  base.version_uuid = version_uuid;
  base.key_payload = "orphaned-base-entry";
  base.committed_local_transaction_id = 1;
  request.base_entries.push_back(base);
  request.index_kind = idx::SecondaryIndexKind::non_unique;
  request.max_records_to_scan = 32;
  request.max_records_to_clean = 32;
  request.engine_mga_authoritative = true;
  return agents::RunIndexGarbageCleanupAgentBatch(request);
}

agents::IndexGarbageCleanupAgentResult IndexCleanupNonAuthoritative() {
  auto request = IndexCleanupRequest(HorizonRequest(Inventory({
                                     Entry(1, mga::TransactionState::committed),
                                   }, 2)),
                                     1);
  request.horizon_request.inventory_authoritative = false;
  return agents::RunIndexGarbageCleanupAgentBatch(request);
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

std::string RowField(const api::EngineRowValue& row, std::string_view field_name) {
  for (const auto& [name, value] : row.fields) {
    if (name == field_name) {
      return value.encoded_value;
    }
  }
  return {};
}

bool HasRowField(const api::EngineApiResult& result,
                 std::string_view field_name,
                 std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    if (RowField(row, field_name) == value) {
      return true;
    }
  }
  return false;
}

bool HasContextDecision(const api::EngineApiResult& result,
                        std::string_view context,
                        std::string_view decision,
                        std::string_view diagnostic_code) {
  for (const auto& row : result.result_shape.rows) {
    if (RowField(row, "record_kind") == "cleanup_context_decision" &&
        RowField(row, "context_kind") == context &&
        RowField(row, "exact_refusal_decision") == decision &&
        RowField(row, "exact_refusal_diagnostic") == diagnostic_code) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

void TestSupportBundleSummarizesCleanupBacklog() {
  const auto storage = StorageCleanupSuccess();
  Require(storage.ok(), "DPC-034 storage cleanup fixture did not succeed");
  const auto index = IndexCleanupSuccess();
  Require(index.ok(), "DPC-034 index cleanup fixture did not succeed");

  api::EngineCleanupDiagnosticsRequest request;
  request.context = Context();
  request.storage_cleanup_present = true;
  request.storage_cleanup = storage;
  request.index_cleanup_present = true;
  request.index_cleanup = index;

  const auto result = api::EngineInspectCleanupDiagnostics(request);
  Require(result.ok, "DPC-034 cleanup diagnostics refused valid cleanup evidence");
  Require(result.cleanup_diagnostics_ready && result.support_bundle_ready,
          "DPC-034 diagnostics/support-bundle readiness missing");
  Require(result.cleanup_horizon_authority_status == "authoritative",
          "DPC-034 horizon authority status mismatch");
  Require(result.storage_row_version_backlog_count == 2 &&
              result.storage_row_version_reclaimed_count == 2,
          "DPC-034 storage cleanup counts mismatch");
  Require(result.index_garbage_backlog_count == 1 &&
              result.index_garbage_cleaned_count == 1,
          "DPC-034 index cleanup counts mismatch");
  Require(HasRowField(result, "cleanup_horizon_identity",
                      "dpc030_authoritative_cleanup_horizon_v1"),
          "DPC-034 horizon identity row missing");
  Require(HasRowField(result, "storage_row_version_backlog_count", "2"),
          "DPC-034 storage backlog row missing");
  Require(HasRowField(result, "index_garbage_cleaned_count", "1"),
          "DPC-034 index cleaned row missing");
  Require(HasContextDecision(result,
                             "backup",
                             "backup_allowed_cleanup_backlog_documented",
                             "CLEANUP_DIAGNOSTICS.BACKUP_ALLOWED"),
          "DPC-034 backup allowed context decision missing");
  Require(HasContextDecision(result,
                             "restore",
                             "restore_allowed_cleanup_backlog_documented",
                             "CLEANUP_DIAGNOSTICS.RESTORE_ALLOWED"),
          "DPC-034 restore allowed context decision missing");
  Require(HasEvidence(result,
                      "support_bundle_surface",
                      "cleanup_diagnostics"),
          "DPC-034 support bundle evidence missing");
  Require(result.support_bundle_json.find("\"cleanup_diagnostics\"") !=
              std::string::npos,
          "DPC-034 support bundle JSON missing cleanup section");
}

void TestHorizonBlockedAndNonAuthorityEvidence() {
  const auto index = IndexCleanupHorizonBlocked();
  Require(index.ok(), "DPC-034 horizon-blocked index fixture failed");
  Require(index.horizon_blocked,
          "DPC-034 horizon-blocked fixture did not expose blocker");

  api::EngineCleanupDiagnosticsRequest request;
  request.context = Context();
  request.index_cleanup_present = true;
  request.index_cleanup = index;
  request.context_kinds = {"restricted_open", "repair"};

  const auto result = api::EngineInspectCleanupDiagnostics(request);
  Require(result.ok, "DPC-034 horizon-blocked diagnostics failed");
  Require(result.index_garbage_horizon_blocked_count == 1,
          "DPC-034 horizon-blocked count mismatch");
  Require(HasContextDecision(result,
                             "restricted_open",
                             "restricted_open_allowed_support_only",
                             "CLEANUP_DIAGNOSTICS.RESTRICTED_OPEN_ALLOWED"),
          "DPC-034 restricted-open classification missing");
  Require(HasContextDecision(result,
                             "repair",
                             "repair_allowed_cleanup_backlog_documented",
                             "CLEANUP_DIAGNOSTICS.REPAIR_ALLOWED"),
          "DPC-034 repair classification missing");
  Require(HasRowField(result, "parser_finality_authority", "false"),
          "DPC-034 parser non-authority evidence missing");
  Require(HasRowField(result, "client_finality_authority", "false"),
          "DPC-034 client non-authority evidence missing");
  Require(HasRowField(result, "timestamp_finality_authority", "false"),
          "DPC-034 timestamp non-authority evidence missing");
  Require(HasRowField(result, "uuid_ordering_finality_authority", "false"),
          "DPC-034 UUID ordering non-authority evidence missing");
  Require(HasRowField(result, "event_stream_finality_authority", "false"),
          "DPC-034 event stream non-authority evidence missing");
}

void TestExactRefusalsForValidationAndNonAuthoritativeInputs() {
  {
    api::EngineCleanupDiagnosticsRequest request;
    request.context = Context();
    request.index_cleanup_present = true;
    request.index_cleanup = IndexCleanupValidationRefused();

    const auto result = api::EngineInspectCleanupDiagnostics(request);
    Require(result.ok, "DPC-034 validation-refused diagnostics should render");
    Require(result.index_validation_refused_count == 1,
            "DPC-034 validation-refused count mismatch");
    Require(HasContextDecision(
                result,
                "backup",
                "backup_refused_index_cleanup_validation_failed",
                "CLEANUP_DIAGNOSTICS.BACKUP_REFUSED_INDEX_VALIDATION"),
            "DPC-034 backup validation refusal missing");
    Require(HasContextDecision(
                result,
                "restore",
                "restore_refused_index_cleanup_validation_failed",
                "CLEANUP_DIAGNOSTICS.RESTORE_REFUSED_INDEX_VALIDATION"),
            "DPC-034 restore validation refusal missing");
    Require(HasContextDecision(
                result,
                "restricted_open",
                "restricted_open_refused_index_cleanup_validation_failed",
                "CLEANUP_DIAGNOSTICS.RESTRICTED_OPEN_REFUSED_INDEX_VALIDATION"),
            "DPC-034 restricted-open validation refusal missing");
    Require(HasContextDecision(
                result,
                "repair",
                "repair_refused_index_cleanup_validation_failed",
                "CLEANUP_DIAGNOSTICS.REPAIR_REFUSED_INDEX_VALIDATION"),
            "DPC-034 repair validation refusal missing");
  }

  {
    api::EngineCleanupDiagnosticsRequest request;
    request.context = Context();
    request.index_cleanup_present = true;
    request.index_cleanup = IndexCleanupNonAuthoritative();

    const auto result = api::EngineInspectCleanupDiagnostics(request);
    Require(result.ok, "DPC-034 non-authoritative diagnostics should render");
    Require(result.cleanup_horizon_authority_status ==
                "refused_non_authoritative",
            "DPC-034 non-authoritative horizon status mismatch");
    Require(result.index_non_authoritative_refused_count == 1,
            "DPC-034 index non-authoritative count mismatch");
    Require(HasContextDecision(
                result,
                "backup",
                "backup_refused_cleanup_horizon_non_authoritative",
                "CLEANUP_DIAGNOSTICS.BACKUP_REFUSED_NON_AUTHORITATIVE_HORIZON"),
            "DPC-034 backup non-authoritative refusal missing");
    Require(HasContextDecision(
                result,
                "repair",
                "repair_refused_cleanup_horizon_non_authoritative",
                "CLEANUP_DIAGNOSTICS.REPAIR_REFUSED_NON_AUTHORITATIVE_HORIZON"),
            "DPC-034 repair non-authoritative refusal missing");
  }
}

void TestInputValidation() {
  api::EngineCleanupDiagnosticsRequest missing_security;
  missing_security.index_cleanup_present = true;
  missing_security.index_cleanup = IndexCleanupSuccess();
  const auto denied = api::EngineInspectCleanupDiagnostics(missing_security);
  Require(!denied.ok, "DPC-034 accepted missing security context");
  Require(HasDiagnostic(denied, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "DPC-034 missing security diagnostic mismatch");

  api::EngineCleanupDiagnosticsRequest unknown_context;
  unknown_context.context = Context();
  unknown_context.index_cleanup_present = true;
  unknown_context.index_cleanup = IndexCleanupSuccess();
  unknown_context.context_kinds = {"backup", "parser_replay"};
  const auto refused = api::EngineInspectCleanupDiagnostics(unknown_context);
  Require(!refused.ok, "DPC-034 accepted unsupported context kind");
  Require(HasDiagnostic(refused, "SB_ENGINE_API_INVALID_REQUEST"),
          "DPC-034 unsupported context diagnostic mismatch");

  api::EngineCleanupDiagnosticsRequest missing_input;
  missing_input.context = Context();
  const auto missing = api::EngineInspectCleanupDiagnostics(missing_input);
  Require(!missing.ok, "DPC-034 accepted missing cleanup inputs");
  Require(HasDiagnostic(missing, "SB_ENGINE_API_INVALID_REQUEST"),
          "DPC-034 missing input diagnostic mismatch");
}

}  // namespace

int main() {
  std::cout << kGateSearchKey << '\n';
  TestSupportBundleSummarizesCleanupBacklog();
  TestHorizonBlockedAndNonAuthorityEvidence();
  TestExactRefusalsForValidationAndNonAuthoritativeInputs();
  TestInputValidation();
  return EXIT_SUCCESS;
}
