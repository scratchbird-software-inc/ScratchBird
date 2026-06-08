// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/support_bundle_api.hpp"
#include "runtime_platform.hpp"
#include "savepoint.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770300000000ull;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

api::EngineRequestContext SupportContext() {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "pcr073-transaction-support-bundle";
  context.database_uuid.canonical =
      uuid::UuidToString(MakeUuid(UuidKind::database, 1).value);
  context.principal_uuid.canonical =
      uuid::UuidToString(MakeUuid(UuidKind::principal, 2).value);
  context.session_uuid.canonical =
      uuid::UuidToString(MakeUuid(UuidKind::object, 3).value);
  context.security_context_present = true;
  context.catalog_generation_id = 7;
  context.security_epoch = 11;
  context.resource_epoch = 13;
  context.name_resolution_epoch = 17;
  return context;
}

txn::TransactionInventoryEntry Entry(u64 local_id,
                                     txn::TransactionState state,
                                     txn::TransactionScope scope =
                                         txn::TransactionScope::local_node) {
  txn::TransactionInventoryEntry entry;
  entry.identity.local_id = txn::MakeLocalTransactionId(local_id);
  entry.identity.transaction_uuid = MakeUuid(UuidKind::transaction, 100 + local_id);
  entry.identity.scope = scope;
  entry.state = state;
  entry.begin_unix_epoch_millis = kBaseMillis + local_id;
  entry.final_unix_epoch_millis =
      txn::IsTerminalTransactionState(state) ? kBaseMillis + 1000 + local_id : 0;
  entry.begin_visible_through_local_transaction_id =
      local_id == 1 ? 0 : local_id - 1;
  entry.evidence_record_required = true;
  entry.evidence_record_written = true;
  return entry;
}

txn::SavepointRollbackPlan SavepointPlan() {
  txn::SavepointStack stack;
  const auto savepoint =
      stack.Create(txn::MakeLocalTransactionId(1), "pcr073_sp", 5);
  Expect(savepoint.ok(), "PCR-073 savepoint fixture should create");

  txn::SavepointMutationRecord mutation;
  mutation.local_id = txn::MakeLocalTransactionId(1);
  mutation.mutation_sequence = 7;
  mutation.kind = txn::SavepointMutationKind::data_page;
  mutation.stable_operation_id = "pcr073_savepoint_mutation";
  mutation.durable_evidence_written = true;
  mutation.undo_evidence_available = true;
  const auto recorded = stack.RecordMutation(mutation);
  Expect(recorded.ok(), "PCR-073 savepoint mutation should record");
  return stack.PlanRollbackTo(txn::MakeLocalTransactionId(1), "pcr073_sp");
}

api::EngineSupportBundleTransactionEvidenceSnapshot TransactionSnapshot() {
  api::EngineSupportBundleTransactionEvidenceSnapshot snapshot;
  snapshot.inventory_present = true;
  snapshot.inventory_authoritative = true;
  snapshot.inventory = txn::MakeEmptyLocalTransactionInventory();
  snapshot.inventory.next_local_transaction_id = 6;
  snapshot.inventory.entries.push_back(Entry(1, txn::TransactionState::active));
  snapshot.inventory.entries.push_back(Entry(2, txn::TransactionState::prepared));
  snapshot.inventory.entries.push_back(
      Entry(3, txn::TransactionState::limbo,
            txn::TransactionScope::cluster_global));
  snapshot.inventory.entries.push_back(
      Entry(4, txn::TransactionState::rolling_back));
  snapshot.inventory.entries.push_back(
      Entry(5, txn::TransactionState::committed));

  snapshot.horizons_present = true;
  snapshot.horizons_authoritative = true;
  snapshot.horizons.oldest_interesting_transaction =
      txn::MakeLocalTransactionId(1);
  snapshot.horizons.oldest_active_transaction = txn::MakeLocalTransactionId(1);
  snapshot.horizons.oldest_snapshot_transaction =
      txn::MakeLocalTransactionId(1);
  snapshot.horizons.next_transaction_id = txn::MakeLocalTransactionId(6);
  snapshot.horizons.valid = true;

  snapshot.current_row_decision_present = true;
  snapshot.current_row_decision.accepted = false;
  snapshot.current_row_decision.normal_mga_recheck_required = true;
  snapshot.current_row_decision.security_recheck_required = true;
  snapshot.current_row_decision.durable_mga_inventory_remains_authority = true;
  snapshot.current_row_decision.refusal_reason =
      "durable_mga_inventory_authority_required";
  snapshot.current_row_decision.counters.probes = 1;
  snapshot.current_row_decision.counters.refused = 1;
  snapshot.current_row_decision.counters.authority_refusals = 1;

  snapshot.page_finality_decision_present = true;
  snapshot.page_finality_decision.accepted = false;
  snapshot.page_finality_decision.normal_mga_recheck_required = true;
  snapshot.page_finality_decision.durable_mga_inventory_remains_authority = true;
  snapshot.page_finality_decision.refusal_reason =
      "page_finality_external_provenance_refused";
  snapshot.page_finality_decision.counters.evidence_examined = 1;
  snapshot.page_finality_decision.counters.refused = 1;
  snapshot.page_finality_decision.counters.provenance_refusals = 1;

  snapshot.cleanup_result_present = true;
  snapshot.cleanup_result.cleanup_horizon_authoritative = true;
  snapshot.cleanup_result.authoritative_cleanup_horizon_local_transaction_id = 1;
  snapshot.cleanup_result.reclaimed_row_version_count = 2;
  snapshot.cleanup_result.retained_row_version_count = 1;
  snapshot.cleanup_result.horizon_blocked_row_version_count = 1;
  snapshot.cleanup_result.limbo_or_unknown_outcome_blocked_row_version_count = 1;
  snapshot.cleanup_result.physical_storage_mutated = false;
  snapshot.cleanup_result.reclaim_evidence_records.resize(2);

  snapshot.lock_result_present = true;
  snapshot.lock_result.decision = txn::TransactionLockDecision::wait_required;
  snapshot.lock_result.blocking_transaction = txn::MakeLocalTransactionId(1);
  snapshot.lock_result.retry_after_millis = 250;
  snapshot.lock_result.wait_elapsed_millis = 50;
  snapshot.lock_result.diagnostic.diagnostic_code =
      "SB-SNTXN-LOCK-FAIRNESS-WAIT";

  snapshot.savepoint_plan_present = true;
  snapshot.savepoint_plan = SavepointPlan();
  return snapshot;
}

api::EnginePrepareSupportBundleRequest SupportRequest() {
  api::EnginePrepareSupportBundleRequest request;
  request.context = SupportContext();
  request.option_envelopes.push_back("engine_authorized_support_export");
  request.transaction_evidence_snapshot_present = true;
  request.transaction_evidence_snapshot = TransactionSnapshot();
  return request;
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) {
      return value.encoded_value;
    }
  }
  return {};
}

const api::EngineRowValue* RowByKind(const api::EngineApiResult& result,
                                     std::string_view kind) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "bundle_record_kind") == kind) {
      return &row;
    }
  }
  return nullptr;
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

bool ExpectRowField(const api::EngineApiResult& result,
                    std::string_view kind,
                    std::string_view field,
                    std::string_view expected) {
  const auto* row = RowByKind(result, kind);
  if (row == nullptr) {
    std::cerr << "missing row kind: " << kind << '\n';
    return false;
  }
  const auto actual = FieldValue(*row, field);
  if (actual != expected) {
    std::cerr << "unexpected " << kind << '.' << field << ": " << actual
              << " != " << expected << '\n';
    return false;
  }
  return true;
}

bool PositiveSupportBundleProof() {
  const auto result = api::EnginePrepareSupportBundle(SupportRequest());
  bool ok = true;
  ok = Expect(result.ok, "PCR-073 support bundle should prepare") && ok;
  ok = Expect(result.redaction_applied,
              "PCR-073 support bundle should apply redaction") &&
       ok;
  ok = Expect(result.forbidden_fields_absent,
              "PCR-073 support bundle should suppress forbidden fields") &&
       ok;
  ok = Expect(result.flush_required_before_export,
              "PCR-073 support bundle should require pre-export flush") &&
       ok;
  ok = Expect(result.transaction_evidence_collected,
              "PCR-073 support bundle should collect transaction evidence") &&
       ok;
  ok = Expect(Contains(result.support_bundle_json, "transaction_evidence"),
              "PCR-073 support JSON should report transaction evidence") &&
       ok;
  ok = Expect(Contains(result.support_bundle_json,
                       "\"support_bundle_is_authority\":false"),
              "PCR-073 support JSON must not claim authority") &&
       ok;
  ok = Expect(Contains(result.support_bundle_json,
                       "\"cleanup_physical_storage_mutated\":false"),
              "PCR-073 support JSON must report non-mutating cleanup") &&
       ok;
  ok = Expect(!Contains(result.support_bundle_json, "docs" "/execution-plans"),
              "PCR-073 support JSON must not reference private execution-plans") &&
       ok;
  ok = Expect(!Contains(result.support_bundle_json, "/home/"),
              "PCR-073 support JSON must not leak local paths") &&
       ok;

  ok = ExpectRowField(result,
                      "transaction_support_bundle_summary",
                      "authority_source",
                      "durable_mga_transaction_inventory") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_support_bundle_summary",
                      "support_bundle_is_authority",
                      "false") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_support_bundle_summary",
                      "active_transaction_count",
                      "1") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_support_bundle_summary",
                      "prepared_transaction_count",
                      "1") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_support_bundle_summary",
                      "limbo_transaction_count",
                      "1") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_support_bundle_summary",
                      "recovery_required_transaction_count",
                      "2") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_support_bundle_summary",
                      "wal_recovery_authority",
                      "false") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_current_row_refusal",
                      "refusal_reason",
                      "durable_mga_inventory_authority_required") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_current_row_refusal",
                      "map_is_transaction_finality_authority",
                      "false") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_page_finality_refusal",
                      "refusal_reason",
                      "page_finality_external_provenance_refused") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_cleanup_evidence",
                      "reclaim_evidence_record_count",
                      "2") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_cleanup_evidence",
                      "physical_storage_mutated",
                      "false") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_lock_evidence",
                      "lock_decision",
                      "wait_required") &&
       ok;
  ok = ExpectRowField(result,
                      "transaction_savepoint_evidence",
                      "savepoint_decision",
                      "rollback_actions_ready") &&
       ok;
  ok = Expect(HasEvidence(result,
                          "support_bundle_transaction_evidence",
                          "scratchbird.transaction_support_bundle.v1"),
              "PCR-073 support bundle should carry transaction evidence marker") &&
       ok;
  ok = Expect(HasEvidence(result,
                          "transaction_support_bundle_authority",
                          "false"),
              "PCR-073 support bundle should explicitly deny authority") &&
       ok;
  return ok;
}

bool RefusalProofs() {
  bool ok = true;

  auto authority_claim = SupportRequest();
  authority_claim.transaction_evidence_snapshot.support_bundle_is_authority = true;
  const auto authority_refused =
      api::EnginePrepareSupportBundle(authority_claim);
  ok = Expect(!authority_refused.ok,
              "PCR-073 authority-claiming support bundle should fail closed") &&
       ok;
  ok = Expect(!authority_refused.diagnostics.empty() &&
                  Contains(authority_refused.diagnostics.front().detail,
                           "OPS.SUPPORT_BUNDLE.TRANSACTION_AUTHORITY_CLAIM_REFUSED"),
              "PCR-073 authority refusal should use stable diagnostic detail") &&
       ok;

  auto current_row_claim = SupportRequest();
  current_row_claim.transaction_evidence_snapshot.current_row_decision
      .map_is_transaction_finality_authority = true;
  const auto current_row_refused =
      api::EnginePrepareSupportBundle(current_row_claim);
  ok = Expect(!current_row_refused.ok,
              "PCR-073 current-row authority claim should fail closed") &&
       ok;
  ok = Expect(!current_row_refused.diagnostics.empty() &&
                  Contains(current_row_refused.diagnostics.front().detail,
                           "OPS.SUPPORT_BUNDLE.CURRENT_ROW_AUTHORITY_CLAIM_REFUSED"),
              "PCR-073 current-row refusal should use stable diagnostic detail") &&
       ok;

  auto cleanup_mutation = SupportRequest();
  cleanup_mutation.transaction_evidence_snapshot.cleanup_result
      .physical_storage_mutated = true;
  const auto cleanup_refused = api::EnginePrepareSupportBundle(cleanup_mutation);
  ok = Expect(!cleanup_refused.ok,
              "PCR-073 mutating cleanup evidence should fail closed") &&
       ok;
  ok = Expect(!cleanup_refused.diagnostics.empty() &&
                  Contains(cleanup_refused.diagnostics.front().detail,
                           "OPS.SUPPORT_BUNDLE.TRANSACTION_CLEANUP_MUTATION_REFUSED"),
              "PCR-073 cleanup mutation refusal should use stable diagnostic detail") &&
       ok;

  auto non_authoritative = SupportRequest();
  non_authoritative.transaction_evidence_snapshot.inventory_authoritative = false;
  const auto non_authoritative_refused =
      api::EnginePrepareSupportBundle(non_authoritative);
  ok = Expect(!non_authoritative_refused.ok,
              "PCR-073 non-authoritative inventory should fail closed") &&
       ok;
  ok = Expect(!non_authoritative_refused.diagnostics.empty() &&
                  Contains(non_authoritative_refused.diagnostics.front().detail,
                           "OPS.SUPPORT_BUNDLE.TRANSACTION_INVENTORY_AUTHORITY_REQUIRED"),
              "PCR-073 non-authoritative inventory refusal should be exact") &&
       ok;

  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = PositiveSupportBundleProof() && ok;
  ok = RefusalProofs() && ok;
  return ok ? 0 : 1;
}
