// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "exact_index_leaf_cleanup.hpp"
#include "index_maintenance.hpp"
#include "page_finality_evidence.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "ODF-052 UUID generation failed");
  return generated.value;
}

struct Fixture {
  platform::TypedUuid index_uuid =
      NewUuid(platform::UuidKind::object, 52001);
  platform::TypedUuid table_uuid =
      NewUuid(platform::UuidKind::object, 52002);
  std::string relation_uuid =
      uuid::UuidToString(NewUuid(platform::UuidKind::object, 52003).value);
};

mga::PageFinalityObservedFacts Observed(const Fixture& fixture) {
  mga::PageFinalityObservedFacts observed;
  observed.requested_scope = mga::PageFinalityScope::extent;
  observed.relation_uuid = fixture.relation_uuid;
  observed.page_number = 0;
  observed.page_generation = 9;
  observed.extent_id = 7;
  observed.extent_epoch = 11;
  observed.relation_epoch = 13;
  observed.catalog_epoch = 17;
  observed.reader_visible_through_local_transaction_id =
      mga::MakeLocalTransactionId(90);
  observed.oldest_active_local_transaction_id =
      mga::MakeLocalTransactionId(100);
  observed.transaction_horizon_authoritative = true;
  observed.transaction_inventory_authoritative = true;
  observed.normal_mga_visibility_authority_available = true;
  return observed;
}

mga::PageFinalityMapEntry ExtentFinality(const Fixture& fixture) {
  mga::PageFinalityMapEntry entry;
  entry.scope = mga::PageFinalityScope::extent;
  entry.status = mga::PageFinalityMapStatus::current;
  entry.provenance =
      mga::PageFinalityProvenance::engine_mga_transaction_inventory;
  entry.relation_uuid = fixture.relation_uuid;
  entry.page_generation = 9;
  entry.extent_id = 7;
  entry.extent_epoch = 11;
  entry.relation_epoch = 13;
  entry.catalog_epoch = 17;
  entry.final_through_local_transaction_id = mga::MakeLocalTransactionId(80);
  entry.map_generation = 5;
  entry.persisted_record_present = true;
  entry.checksum_valid = true;
  entry.all_visible = true;
  entry.all_final = true;
  return entry;
}

mga::ExactIndexCleanupAuthorityDecision AcceptedAuthority(
    const Fixture& fixture) {
  const auto page = mga::EvaluatePageFinalityEvidence(
      ExtentFinality(fixture), Observed(fixture), mga::PageFinalityConsumer::cleanup);
  Require(page.accepted, "ODF-052 setup finality evidence was refused");
  auto authority = mga::EvaluateExactIndexCleanupAuthority(
      page, mga::MakeLocalTransactionId(90), true, true);
  Require(authority.accepted, "ODF-052 setup cleanup authority was refused");
  return authority;
}

mga::ExactIndexCleanupAuthorityDecision RefusedAuthority(
    const Fixture& fixture) {
  auto entry = ExtentFinality(fixture);
  entry.provenance = mga::PageFinalityProvenance::uuid_order_claim;
  const auto page = mga::EvaluatePageFinalityEvidence(
      entry, Observed(fixture), mga::PageFinalityConsumer::cleanup);
  Require(!page.accepted, "ODF-052 setup external finality was accepted");
  return mga::EvaluateExactIndexCleanupAuthority(
      page, mga::MakeLocalTransactionId(90), false, true);
}

idx::SecondaryIndexBaseEntry Base(const Fixture& fixture,
                                  platform::u64 salt,
                                  std::string key) {
  idx::SecondaryIndexBaseEntry entry;
  entry.index_uuid = fixture.index_uuid;
  entry.table_uuid = fixture.table_uuid;
  entry.row_uuid = NewUuid(platform::UuidKind::row, salt);
  entry.version_uuid = NewUuid(platform::UuidKind::row, salt + 100);
  entry.key_payload = std::move(key);
  entry.committed_local_transaction_id = 40;
  return entry;
}

idx::SecondaryIndexTableSnapshotEntry Snapshot(
    const idx::SecondaryIndexBaseEntry& base) {
  idx::SecondaryIndexTableSnapshotEntry entry;
  entry.index_uuid = base.index_uuid;
  entry.table_uuid = base.table_uuid;
  entry.row_uuid = base.row_uuid;
  entry.version_uuid = base.version_uuid;
  entry.key_payload = base.key_payload;
  return entry;
}

idx::SecondaryIndexDeltaLedgerRecord Garbage(const Fixture& fixture,
                                             platform::u64 salt,
                                             platform::u64 tx,
                                             std::string key) {
  idx::SecondaryIndexDeltaLedgerRecord record;
  record.delta.delta_id = NewUuid(platform::UuidKind::object, salt);
  record.delta.index_uuid = fixture.index_uuid;
  record.delta.table_uuid = fixture.table_uuid;
  record.delta.row_uuid = NewUuid(platform::UuidKind::row, salt + 10);
  record.delta.version_uuid = NewUuid(platform::UuidKind::row, salt + 20);
  record.delta.transaction_uuid =
      NewUuid(platform::UuidKind::transaction, salt + 30);
  record.delta.local_transaction_id = tx;
  record.delta.delta_kind = idx::SecondaryIndexDeltaKind::insert;
  record.delta.key_payload = std::move(key);
  record.delta.cleanup_horizon_token = "engine_mga_cleanup_horizon";
  record.delta.committed = true;
  record.commit_state =
      idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned;
  record.source_evidence_reference = "engine_mga_transaction_inventory";
  return record;
}

idx::ExactIndexLeafPressureRequest RequestWithGarbage(
    const Fixture& fixture,
    std::size_t garbage_count) {
  idx::ExactIndexLeafPressureRequest request;
  request.index_uuid = fixture.index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.family = idx::IndexFamily::btree;
  request.index_kind = idx::SecondaryIndexKind::non_unique;
  request.current_leaf_entry_count = 3 + garbage_count;
  request.pending_insert_entry_count = 1;
  request.leaf_entry_capacity = 4;
  request.max_cleanup_steps = 0;
  request.mga_cleanup_authority = AcceptedAuthority(fixture);
  request.cleanup.index_uuid = fixture.index_uuid;
  request.cleanup.table_uuid = fixture.table_uuid;
  request.cleanup.cleanup_horizon_authoritative = true;
  request.cleanup.authoritative_cleanup_horizon_local_transaction_id = 90;
  request.cleanup.index_kind = idx::SecondaryIndexKind::non_unique;
  request.cleanup.max_records_to_scan = 16;
  request.cleanup.max_records_to_clean = 16;
  for (platform::u64 i = 0; i < 3; ++i) {
    auto base = Base(fixture, 52100 + i, "key-" + std::to_string(i));
    request.cleanup.table_snapshot.push_back(Snapshot(base));
    request.cleanup.base_entries.push_back(std::move(base));
  }
  for (std::size_t i = 0; i < garbage_count; ++i) {
    request.cleanup.ledger.records.push_back(
        Garbage(fixture, 52200 + static_cast<platform::u64>(i * 10), 70,
                "garbage-" + std::to_string(i)));
  }
  return request;
}

bool EvidenceHas(const idx::ExactIndexLeafPressureDecision& decision,
                 std::string_view name,
                 std::string_view value) {
  for (const auto& field : decision.evidence) {
    if (field.name == name && field.value == value) {
      return true;
    }
  }
  return false;
}

void RequireNoRuntimeDocTokens(
    const idx::ExactIndexLeafPressureDecision& decision) {
  std::vector<std::string> values = {
      decision.diagnostic.diagnostic_code,
      decision.diagnostic.message_key,
      decision.diagnostic.source_component,
      decision.diagnostic.remediation_hint,
      decision.fail_open_reason,
      decision.mga_authority_source};
  for (const auto& argument : decision.diagnostic.arguments) {
    values.push_back(argument.key);
    values.push_back(argument.value);
  }
  for (const auto& field : decision.evidence) {
    values.push_back(field.name);
    values.push_back(field.value);
  }
  for (const auto& value : values) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings",
                                 "contracts", "references"}) {
      Require(value.find(forbidden) == std::string::npos,
              "ODF-052 runtime evidence leaked documentation token");
    }
  }
}

void CleanupAvoidsSplitWhenEnoughReclaimableEntriesExist() {
  const Fixture fixture;
  auto request = RequestWithGarbage(fixture, 2);
  const auto decision = idx::PlanExactIndexLeafPressureAction(request);
  Require(decision.ok(), "ODF-052 cleanup avoid split status failed");
  Require(decision.action ==
              idx::ExactIndexLeafPressureAction::cleanup_avoided_split,
          "ODF-052 cleanup did not avoid split");
  Require(decision.cleanup_attempted && decision.cleanup_accepted &&
              !decision.cleanup_refused,
          "ODF-052 cleanup accepted/refused counters drifted");
  Require(decision.counters.cleaned_count == 2 &&
              decision.counters.retained_count == 0,
          "ODF-052 cleanup counts mismatched");
  Require(decision.selected_ledger.records.empty(),
          "ODF-052 selected ledger retained cleaned garbage");
  Require(EvidenceHas(decision,
                      "exact_leaf_cleanup_mga_authority_source",
                      "durable_mga_transaction_inventory"),
          "ODF-052 MGA authority source evidence missing");
  Require(EvidenceHas(decision,
                      "exact_leaf_pressure_selected_action",
                      "cleanup_avoided_split"),
          "ODF-052 selected action evidence missing");
}

void MaintenanceDecisionPathCarriesSelectedActionEvidence() {
  const Fixture fixture;
  auto leaf_request = RequestWithGarbage(fixture, 2);
  idx::IndexMaintenanceRequest maintenance;
  maintenance.index_uuid = fixture.index_uuid;
  maintenance.family = idx::IndexFamily::btree;
  maintenance.operation = idx::IndexMaintenanceOperation::rebalance;
  maintenance.page_budget = 1;
  maintenance.policy_allows_mutation = true;
  maintenance.evaluate_exact_leaf_pressure = true;
  maintenance.exact_leaf_pressure = leaf_request;

  const auto plan = idx::PlanIndexMaintenance(maintenance);
  Require(plan.ok(), "ODF-052 maintenance route refused leaf cleanup plan");
  Require(plan.exact_leaf_pressure_evaluated,
          "ODF-052 maintenance route did not evaluate leaf pressure");
  Require(plan.exact_leaf_pressure_decision.action ==
              idx::ExactIndexLeafPressureAction::cleanup_avoided_split,
          "ODF-052 maintenance route selected wrong leaf action");
  bool saw_step = false;
  for (const auto& step : plan.steps) {
    if (step == "exact_leaf_cleanup_avoided_split") {
      saw_step = true;
    }
  }
  Require(saw_step, "ODF-052 maintenance route omitted cleanup step");
  bool saw_evidence = false;
  for (const auto& field : plan.selected_action_evidence) {
    if (field.name == "exact_leaf_pressure_selected_action" &&
        field.value == "cleanup_avoided_split") {
      saw_evidence = true;
    }
  }
  Require(saw_evidence,
          "ODF-052 maintenance route omitted selected action evidence");
}

void BudgetExhaustionFailsOpenToSplit() {
  const Fixture fixture;
  auto request = RequestWithGarbage(fixture, 4);
  request.current_leaf_entry_count = 7;
  request.cleanup.max_records_to_scan = 2;
  request.cleanup.max_records_to_clean = 16;
  const auto decision = idx::PlanExactIndexLeafPressureAction(request);
  Require(decision.action == idx::ExactIndexLeafPressureAction::split_selected,
          "ODF-052 budget exhaustion did not select split");
  Require(decision.budget_exhausted && decision.cleanup_refused &&
              decision.split_selected,
          "ODF-052 budget fail-open flags missing");
  Require(decision.fail_open_reason ==
              "cleanup_budget_exhausted_fail_open_to_split",
          "ODF-052 budget fail-open reason mismatch");
  Require(decision.selected_ledger.records.size() == 2,
          "ODF-052 budget exhaustion did not stop cleanup within bound");
  Require(decision.counters.retained_count == 2,
          "ODF-052 budget exhaustion retained-count evidence mismatch");
}

void NonAuthoritativeEvidenceRefusesCleanupAndSelectsSplit() {
  const Fixture fixture;
  auto request = RequestWithGarbage(fixture, 2);
  request.mga_cleanup_authority = RefusedAuthority(fixture);
  const auto decision = idx::PlanExactIndexLeafPressureAction(request);
  Require(decision.action == idx::ExactIndexLeafPressureAction::split_selected,
          "ODF-052 non-authoritative evidence did not select split");
  Require(decision.cleanup_attempted && decision.cleanup_refused &&
              !decision.cleanup_accepted,
          "ODF-052 non-authoritative cleanup flags mismatch");
  Require(decision.selected_ledger.records.size() ==
              request.cleanup.ledger.records.size(),
          "ODF-052 non-authoritative cleanup mutated ledger");
  Require(decision.fail_open_reason ==
              "cleanup_horizon_or_transaction_inventory_uncertain",
          "ODF-052 non-authoritative fail-open reason mismatch");
}

void ValidationMismatchRefusesCleanupAndKeepsOriginalEntries() {
  const Fixture fixture;
  auto request = RequestWithGarbage(fixture, 2);
  request.cleanup.table_snapshot.push_back(
      Snapshot(Base(fixture, 52300, "missing-from-base")));
  const auto decision = idx::PlanExactIndexLeafPressureAction(request);
  Require(decision.action == idx::ExactIndexLeafPressureAction::split_selected,
          "ODF-052 validation mismatch did not select split");
  Require(decision.cleanup_refused && !decision.cleanup_accepted,
          "ODF-052 validation mismatch cleanup flags mismatch");
  Require(decision.selected_ledger.records.size() ==
              request.cleanup.ledger.records.size(),
          "ODF-052 validation mismatch did not keep original ledger");
  Require(decision.fail_open_reason ==
              "effective_exact_index_validation_mismatch",
          "ODF-052 validation fail-open reason mismatch");
}

void UniqueIndexSafetyIsPreserved() {
  const Fixture fixture;
  auto request = RequestWithGarbage(fixture, 2);
  request.family = idx::IndexFamily::unique_btree;
  request.index_kind = idx::SecondaryIndexKind::unique;
  request.cleanup.index_kind = idx::SecondaryIndexKind::unique;
  const auto decision = idx::PlanExactIndexLeafPressureAction(request);
  Require(decision.action == idx::ExactIndexLeafPressureAction::split_selected,
          "ODF-052 unique exact index did not select synchronous split path");
  Require(decision.unique_exact_recheck_required && decision.cleanup_refused,
          "ODF-052 unique exact recheck evidence missing");
  Require(decision.selected_ledger.records.size() ==
              request.cleanup.ledger.records.size(),
          "ODF-052 unique exact path mutated cleanup ledger");
  Require(EvidenceHas(decision, "exact_leaf_unique_recheck_required", "true"),
          "ODF-052 unique exact recheck runtime evidence missing");
}

void RuntimeEvidenceHasNoDocumentationPathsOrTokens() {
  const Fixture fixture;
  const auto decision =
      idx::PlanExactIndexLeafPressureAction(RequestWithGarbage(fixture, 2));
  RequireNoRuntimeDocTokens(decision);
}

}  // namespace

int main() {
  CleanupAvoidsSplitWhenEnoughReclaimableEntriesExist();
  MaintenanceDecisionPathCarriesSelectedActionEvidence();
  BudgetExhaustionFailsOpenToSplit();
  NonAuthoritativeEvidenceRefusesCleanupAndSelectsSplit();
  ValidationMismatchRefusesCleanupAndKeepsOriginalEntries();
  UniqueIndexSafetyIsPreserved();
  RuntimeEvidenceHasNoDocumentationPathsOrTokens();
  return 0;
}
