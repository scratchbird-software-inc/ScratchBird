// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "deferred_secondary_index_runtime_policy.hpp"
#include "dynamic_cleanup_debt_scheduler.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace implemented_agents = scratchbird::core::agents::implemented_agents;
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

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, salt);
  Require(generated.ok(), "ODF-057 UUID generation failed");
  return generated.value;
}

mga::AuthoritativeCleanupHorizonResult AuthoritativeHorizon() {
  mga::AuthoritativeCleanupHorizonResult horizon;
  horizon.status = {platform::StatusCode::ok,
                    platform::Severity::info,
                    platform::Subsystem::transaction_mga};
  horizon.cleanup_horizon = mga::MakeLocalTransactionId(100);
  horizon.cleanup_horizon_authoritative = true;
  horizon.horizons.valid = true;
  horizon.horizons.oldest_interesting_transaction =
      mga::MakeLocalTransactionId(100);
  horizon.horizons.oldest_active_transaction = mga::MakeLocalTransactionId(100);
  horizon.horizons.oldest_snapshot_transaction =
      mga::MakeLocalTransactionId(100);
  horizon.horizons.next_transaction_id = mga::MakeLocalTransactionId(120);
  return horizon;
}

std::vector<std::string> RuntimeOptions() {
  return {
      idx::kDeferredSecondaryIndexRuntimeOption,
      idx::kSecondaryIndexDeltaLedgerFeatureOption,
      idx::kDeltaLedgerReaderOverlayOption,
      idx::kDeltaLedgerCleanupHorizonBoundOption,
      idx::kDeltaLedgerRecoveryClassifiableOption,
  };
}

idx::PageAwareSecondaryChangeBufferRequest ChangeBufferRequest() {
  idx::PageAwareSecondaryChangeBufferRequest request;
  request.option_envelopes = RuntimeOptions();
  request.index_kind = idx::SecondaryIndexKind::non_unique;
  request.target_page_cold = true;
  request.target_page_random_io_score = 92;
  request.cold_random_io_score_threshold = 70;
  request.pending_delta_count = 3;
  request.incoming_delta_count = 1;
  request.max_pending_delta_count = 12;
  request.pending_delta_bytes = 256;
  request.incoming_delta_bytes = 128;
  request.max_pending_delta_bytes = 8192;
  request.delta_overlay_available = true;
  request.delta_overlay_read_safe = true;
  request.persisted_recovery_proof_available = true;
  request.durable_transaction_inventory_authoritative = true;
  return request;
}

idx::PersistentSecondaryIndexDeltaLedger DeltaLedger() {
  idx::PersistentSecondaryIndexDeltaLedger ledger;
  idx::SecondaryIndexDeltaLedgerRecord record;
  record.delta.delta_id = NewUuid(platform::UuidKind::object, 57001);
  record.delta.index_uuid = NewUuid(platform::UuidKind::object, 57002);
  record.delta.table_uuid = NewUuid(platform::UuidKind::object, 57003);
  record.delta.row_uuid = NewUuid(platform::UuidKind::row, 57004);
  record.delta.version_uuid = NewUuid(platform::UuidKind::row, 57005);
  record.delta.transaction_uuid =
      NewUuid(platform::UuidKind::transaction, 57006);
  record.delta.local_transaction_id = 70;
  record.delta.delta_kind = idx::SecondaryIndexDeltaKind::insert;
  record.delta.key_payload = "city=oslo";
  record.delta.cleanup_horizon_token = "engine_mga_cleanup_horizon";
  record.delta.committed = true;
  record.commit_state =
      idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge;
  record.source_evidence_reference = "engine_mga_transaction_inventory";
  ledger.records.push_back(record);
  ledger.encoded_bytes = 256;
  return ledger;
}

idx::ExactIndexLeafPressureDecision ExactLeafDecision() {
  idx::ExactIndexLeafPressureDecision decision;
  decision.leaf_pressure_detected = true;
  decision.cleanup_attempted = true;
  decision.cleanup_refused = true;
  decision.split_selected = true;
  decision.required_reclaim_count = 3;
  decision.counters.retained_count = 3;
  decision.mga_authority_source = "durable_mga_transaction_inventory";
  decision.fail_open_reason = "cleanup_budget_exhausted_fail_open_to_split";
  decision.cleanup_result.fail_closed = false;
  return decision;
}

std::vector<idx::PageExtentSummaryMetadata> SummaryDebt() {
  idx::PageExtentSummaryMetadata stale;
  stale.status = idx::PageExtentSummaryStatus::stale;
  stale.row_count = 64;
  stale.persisted_record_present = true;
  stale.checksum_valid = true;
  stale.generation = 2;
  idx::PageExtentSummaryMetadata missing;
  missing.status = idx::PageExtentSummaryStatus::missing;
  missing.row_count = 32;
  missing.generation = 0;
  return {stale, missing};
}

agents::DynamicCleanupDebtSchedulerPolicy Policy() {
  agents::DynamicCleanupDebtSchedulerPolicy policy;
  policy.max_total_work_units = 24;
  policy.max_scheduled_items = 6;
  policy.default_max_family_work_units = 4;
  policy.default_max_family_items = 1;
  policy.max_work_units_per_item = 4;
  policy.lease_duration_microseconds = 1000;
  return policy;
}

std::vector<agents::DynamicCleanupDebtSource> AllDebtSources() {
  implemented_agents::StorageVersionCleanupPressureMetrics version_metrics;
  version_metrics.cleanup_candidate_row_versions = 4;
  version_metrics.blocked_row_versions = 1;
  version_metrics.active_cleanup_blockers = 1;

  auto change_request = ChangeBufferRequest();
  const auto change_decision =
      idx::SelectPageAwareSecondaryChangeBufferV2(change_request);
  Require(change_decision.ok(), "ODF-057 ODF-054 setup was refused");

  auto hot_request = change_request;
  hot_request.target_page_cold = false;
  hot_request.target_page_random_io_score = 40;
  const auto hot_decision = idx::SelectPageAwareSecondaryChangeBufferV2(
      hot_request);
  Require(!hot_decision.ok() &&
              hot_decision.counters.hot_page_refusals == 1,
          "ODF-057 ODF-054 hot-leaf setup did not refuse hot page");

  return {
      agents::DynamicCleanupDebtSourceFromVersionChainMetrics(
          "relation-a/version-chain", version_metrics),
      agents::DynamicCleanupDebtSourceFromExactIndexLeafPressure(
          "index-a/exact-leaf", ExactLeafDecision()),
      agents::DynamicCleanupDebtSourceFromSecondaryDeltaLedger(
          "index-a/delta-ledger", DeltaLedger()),
      agents::DynamicCleanupDebtSourceFromPageRangeSummaries(
          "relation-a/page-summary", SummaryDebt()),
      agents::DynamicCleanupDebtSourceFromLargeValueDebt(
          "relation-a/large-values", 6, 65536, 2),
      agents::DynamicCleanupDebtSourceFromHotLeafPressure(
          "index-a/hot-leaf", hot_request, hot_decision),
  };
}

const agents::DynamicCleanupDebtAssignment* FindDecision(
    const agents::DynamicCleanupDebtSchedulerResult& result,
    agents::DynamicCleanupDebtFamily family) {
  for (const auto& decision : result.decisions) {
    if (decision.source.family == family) {
      return &decision;
    }
  }
  return nullptr;
}

bool EvidenceHas(const agents::DynamicCleanupDebtSchedulerResult& result,
                 std::string_view key,
                 std::string_view value) {
  for (const auto& field : result.evidence) {
    if (field.key == key && field.value == value) {
      return true;
    }
  }
  return false;
}

bool AssignmentEvidenceHas(const agents::DynamicCleanupDebtAssignment& decision,
                           std::string_view key,
                           std::string_view value) {
  for (const auto& field : decision.evidence) {
    if (field.key == key && field.value == value) {
      return true;
    }
  }
  return false;
}

void RequireNoRuntimeDocTokens(
    const agents::DynamicCleanupDebtSchedulerResult& result) {
  std::vector<std::string> values = {
      result.diagnostic.diagnostic_code,
      result.diagnostic.message_key,
      result.diagnostic.source_component,
      result.diagnostic.remediation_hint,
  };
  for (const auto& argument : result.diagnostic.arguments) {
    values.push_back(argument.key);
    values.push_back(argument.value);
  }
  for (const auto& field : result.evidence) {
    values.push_back(field.key);
    values.push_back(field.value);
  }
  for (const auto& decision : result.decisions) {
    values.push_back(decision.diagnostic_code);
    values.push_back(decision.detail);
    values.push_back(decision.lease_token);
    values.push_back(decision.source.stable_work_key);
    values.push_back(decision.source.source_detail);
    for (const auto& field : decision.evidence) {
      values.push_back(field.key);
      values.push_back(field.value);
    }
    for (const auto& field : decision.source.evidence) {
      values.push_back(field.key);
      values.push_back(field.value);
    }
  }
  for (const auto& value : values) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings",
                                 "contracts", "references"}) {
      Require(value.find(forbidden) == std::string::npos,
              "ODF-057 runtime evidence leaked documentation token");
    }
  }
}

void SchedulesAcrossAllCleanupDebtFamiliesWithFairCaps() {
  agents::DynamicCleanupDebtSchedulerRequest request;
  request.policy = Policy();
  request.cleanup_horizon = AuthoritativeHorizon();
  request.engine_mga_authoritative = true;
  request.now_microseconds = 100000;
  request.sources = AllDebtSources();

  const auto result = agents::PlanDynamicCleanupDebt(request);
  Require(result.ok(), "ODF-057 scheduler refused authoritative debt");
  Require(result.scheduled_count == 6,
          "ODF-057 scheduler did not select one item per debt family");
  Require(result.scheduled_work_units <= request.policy.max_total_work_units,
          "ODF-057 scheduler exceeded total work budget");
  Require(EvidenceHas(result, "authority_source",
                      "durable_mga_transaction_inventory"),
          "ODF-057 MGA authority evidence missing");
  Require(EvidenceHas(result, "parser_finality_authority", "false"),
          "ODF-057 parser finality guard evidence missing");

  std::set<std::string> scheduled_families;
  for (const auto& decision : result.decisions) {
    if (decision.scheduled()) {
      scheduled_families.insert(
          agents::DynamicCleanupDebtFamilyName(decision.source.family));
      Require(decision.scheduled_work_units <=
                  request.policy.default_max_family_work_units,
              "ODF-057 family cap was exceeded");
      Require(!decision.lease_token.empty(),
              "ODF-057 scheduled work lacks lease evidence");
      Require(!AssignmentEvidenceHas(decision,
                                     "cleanup_debt_decision",
                                     "no_op"),
              "ODF-057 final scheduled evidence retained stale no-op decision");
    }
  }
  Require(scheduled_families.size() == 6,
          "ODF-057 scheduled families were not fair across sources");
  RequireNoRuntimeDocTokens(result);
}

void MissingMgaAuthorityKeepsFinalityWithInventoryAndUsesFailureModes() {
  auto sources = AllDebtSources();
  sources.resize(3);

  agents::DynamicCleanupDebtSchedulerRequest request;
  request.policy = Policy();
  request.engine_mga_authoritative = false;
  request.now_microseconds = 200000;
  request.sources = sources;

  const auto result = agents::PlanDynamicCleanupDebt(request);
  Require(!result.ok() && result.fail_closed,
          "ODF-057 missing MGA authority did not fail closed for retained debt");
  Require(result.scheduled_count == 0,
          "ODF-057 scheduled destructive work without MGA authority");
  Require(result.fail_open_deferred_count == 1,
          "ODF-057 exact leaf fail-open count mismatch");
  Require(result.fail_closed_refusal_count == 2,
          "ODF-057 fail-closed refusal count mismatch");

  const auto* version =
      FindDecision(result, agents::DynamicCleanupDebtFamily::version_chain);
  Require(version != nullptr &&
              version->decision ==
                  agents::DynamicCleanupDebtDecisionKind::refused_authority &&
              version->failure_mode ==
                  agents::DynamicCleanupDebtFailureMode::fail_closed_retain_debt,
          "ODF-057 version-chain authority refusal mismatch");

  const auto* exact =
      FindDecision(result, agents::DynamicCleanupDebtFamily::exact_index_leaf);
  Require(exact != nullptr &&
              exact->decision ==
                  agents::DynamicCleanupDebtDecisionKind::refused_authority &&
              exact->failure_mode ==
                  agents::DynamicCleanupDebtFailureMode::fail_open_to_foreground,
          "ODF-057 exact leaf fail-open authority mismatch");
}

void LeaseCooldownAndContentionAvoidWorkerSpin() {
  auto sources = AllDebtSources();
  sources.resize(3);
  sources[0].lease_active = true;
  sources[0].lease_until_microseconds = 600000;
  sources[1].last_attempt_microseconds = 500000;
  sources[1].failure_count = 2;
  sources[2].worker_contention_observed = true;

  agents::DynamicCleanupDebtSchedulerRequest request;
  request.policy = Policy();
  request.cleanup_horizon = AuthoritativeHorizon();
  request.engine_mga_authoritative = true;
  request.now_microseconds = 501000;
  request.sources = sources;

  const auto result = agents::PlanDynamicCleanupDebt(request);
  Require(result.scheduled_count == 0,
          "ODF-057 scheduled work that should have been backed off");
  Require(!result.fail_closed,
          "ODF-057 lease/cooldown/contention should defer without finality failure");

  bool saw_lease = false;
  bool saw_cooldown = false;
  bool saw_contention = false;
  for (const auto& decision : result.decisions) {
    saw_lease = saw_lease ||
                decision.decision ==
                    agents::DynamicCleanupDebtDecisionKind::deferred_lease;
    saw_cooldown =
        saw_cooldown ||
        decision.decision ==
            agents::DynamicCleanupDebtDecisionKind::deferred_cooldown;
    saw_contention =
        saw_contention ||
        decision.decision ==
            agents::DynamicCleanupDebtDecisionKind::deferred_contention;
    if (decision.decision !=
        agents::DynamicCleanupDebtDecisionKind::deferred_lease) {
      Require(decision.next_eligible_microseconds > request.now_microseconds,
              "ODF-057 backoff decision lacked future eligibility evidence");
    }
  }
  Require(saw_lease && saw_cooldown && saw_contention,
          "ODF-057 did not emit all no-spin deferral decisions");
}

void SourceAdaptersCoverChangeBufferAndTimeRangeDebt() {
  auto request = ChangeBufferRequest();
  request.pending_delta_count = 7;
  request.incoming_delta_count = 2;
  request.pending_delta_bytes = 4096;
  request.incoming_delta_bytes = 1024;
  const auto decision = idx::SelectPageAwareSecondaryChangeBufferV2(request);
  Require(decision.ok(), "ODF-057 secondary change-buffer setup failed");

  auto source = agents::DynamicCleanupDebtSourceFromSecondaryChangeBuffer(
      "index-a/change-buffer", request, decision);
  Require(source.family ==
              agents::DynamicCleanupDebtFamily::secondary_delta_ledger &&
              source.debt_units == 9 &&
              source.debt_bytes == 5120 &&
              source.recovery_proof_available &&
              source.bounded_cleanup_available,
          "ODF-057 secondary change-buffer debt adapter mismatch");

  idx::TimeRangeSummaryPrunePlan plan;
  plan.exact_fallback_required = true;
  plan.summary_metadata_finality_authority = false;
  plan.summary_metadata_visibility_authority = false;
  plan.fallback_reason = "stale_summary_exact_fallback";
  plan.counters.ranges_scanned = 3;
  plan.counters.pages_scanned = 11;
  source = agents::DynamicCleanupDebtSourceFromTimeRangePrunePlan(
      "relation-a/time-summary", plan);
  Require(source.family ==
              agents::DynamicCleanupDebtFamily::summary_page_range &&
              source.debt_units == 3 &&
              source.debt_bytes == 45056 &&
              !source.requires_mga_cleanup_horizon &&
              !source.destructive_cleanup,
          "ODF-057 time-range summary debt adapter mismatch");
}

void ForegroundProtectionBoundsBudget() {
  agents::DynamicCleanupDebtSchedulerRequest request;
  request.policy = Policy();
  request.policy.max_total_work_units = 16;
  request.cleanup_horizon = AuthoritativeHorizon();
  request.engine_mga_authoritative = true;
  request.foreground_work_active = true;
  request.now_microseconds = 700000;
  request.sources = AllDebtSources();

  const auto result = agents::PlanDynamicCleanupDebt(request);
  Require(result.foreground_protected,
          "ODF-057 foreground protection evidence missing");
  Require(result.effective_total_work_units == 4,
          "ODF-057 foreground budget was not reduced");
  Require(result.scheduled_work_units <= 4,
          "ODF-057 foreground-protected budget was exceeded");
}

}  // namespace

int main() {
  SchedulesAcrossAllCleanupDebtFamiliesWithFairCaps();
  MissingMgaAuthorityKeepsFinalityWithInventoryAndUsesFailureModes();
  LeaseCooldownAndContentionAvoidWorkerSpin();
  SourceAdaptersCoverChangeBufferAndTimeRangeDebt();
  ForegroundProtectionBoundsBudget();
  return EXIT_SUCCESS;
}
