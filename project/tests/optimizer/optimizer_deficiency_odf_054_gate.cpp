// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "deferred_secondary_index_runtime_policy.hpp"
#include "index_apply_planner.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
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

mga::PageFinalityEvidenceDecision AdvisoryPageFinality() {
  mga::PageFinalityEvidenceDecision decision;
  decision.accepted = true;
  decision.all_visible = true;
  decision.all_final = false;
  decision.map_is_transaction_finality_authority = false;
  decision.durable_mga_inventory_remains_authority = true;
  decision.normal_mga_recheck_required = false;
  decision.evidence_name = "mga_page_finality.page_all_visible.accepted";
  decision.refusal_reason = "none";
  decision.counters.evidence_examined = 1;
  decision.counters.accepted = 1;
  return decision;
}

idx::PageAwareSecondaryChangeBufferRequest BaseRequest() {
  idx::PageAwareSecondaryChangeBufferRequest request;
  request.option_envelopes = RuntimeOptions();
  request.index_kind = idx::SecondaryIndexKind::non_unique;
  request.target_page_cold = true;
  request.target_page_random_io_score = 90;
  request.cold_random_io_score_threshold = 70;
  request.pending_delta_count = 3;
  request.incoming_delta_count = 1;
  request.max_pending_delta_count = 8;
  request.pending_delta_bytes = 256;
  request.incoming_delta_bytes = 64;
  request.max_pending_delta_bytes = 4096;
  request.delta_overlay_available = true;
  request.delta_overlay_read_safe = true;
  request.persisted_recovery_proof_available = true;
  request.durable_transaction_inventory_authoritative = true;
  request.page_finality_evidence_present = true;
  request.page_finality = AdvisoryPageFinality();
  return request;
}

bool EvidenceHas(const idx::PageAwareSecondaryChangeBufferDecision& decision,
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
    const idx::PageAwareSecondaryChangeBufferDecision& decision) {
  std::vector<std::string> values = {
      decision.reason,
      decision.recovery_class,
      decision.recovery_action,
      decision.diagnostic.diagnostic_code,
      decision.diagnostic.message_key,
      decision.diagnostic.source_component,
      decision.diagnostic.remediation_hint,
  };
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
              "ODF-054 runtime evidence leaked documentation token");
    }
  }
}

void AcceptedNonUniqueColdRandomIoPath() {
  const auto decision = idx::SelectPageAwareSecondaryChangeBufferV2(BaseRequest());
  Require(decision.ok(), "ODF-054 cold non-unique path was refused");
  Require(decision.selected && !decision.synchronous_fallback_required,
          "ODF-054 accepted path did not select buffering");
  Require(decision.recovery_class == "empty_clean" &&
              decision.recovery_action == "no_action",
          "ODF-054 accepted path did not classify recovery proof");
  Require(decision.page_finality_used_as_advisory_acceleration,
          "ODF-054 page finality did not act as advisory acceleration");
  Require(decision.effective_random_io_threshold == 60,
          "ODF-054 advisory finality did not adjust random-IO threshold");
  Require(EvidenceHas(decision, "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-054 MGA authority evidence missing");
  Require(EvidenceHas(decision, "page_finality_advisory_only", "true"),
          "ODF-054 advisory-only evidence missing");
  RequireNoRuntimeDocTokens(decision);
}

void RefusedUniqueIndexPath() {
  auto request = BaseRequest();
  request.index_kind = idx::SecondaryIndexKind::unique;
  const auto decision = idx::SelectPageAwareSecondaryChangeBufferV2(request);
  Require(!decision.ok() && !decision.selected,
          "ODF-054 unique index buffering was accepted");
  Require(decision.synchronous_fallback_required,
          "ODF-054 unique refusal did not require synchronous fallback");
  Require(decision.counters.unique_refusals == 1,
          "ODF-054 unique refusal counter mismatch");
}

void RefusedHotLowRandomIoPath() {
  auto request = BaseRequest();
  request.page_finality_evidence_present = false;
  request.target_page_cold = false;
  request.target_page_random_io_score = 40;
  const auto decision = idx::SelectPageAwareSecondaryChangeBufferV2(request);
  Require(!decision.ok() &&
              decision.reason == "target_page_random_io_score_too_low",
          "ODF-054 hot page random-IO refusal mismatch");
  Require(decision.counters.hot_page_refusals == 1,
          "ODF-054 hot page refusal counter mismatch");
}

void RefusedBacklogCapPath() {
  auto request = BaseRequest();
  request.pending_delta_count = 8;
  request.incoming_delta_count = 1;
  const auto decision = idx::SelectPageAwareSecondaryChangeBufferV2(request);
  Require(!decision.ok() &&
              decision.reason ==
                  "secondary_change_buffer_backlog_cap_exceeded",
          "ODF-054 backlog cap refusal mismatch");
  Require(decision.counters.backlog_refusals == 1,
          "ODF-054 backlog refusal counter mismatch");

  auto overflow = BaseRequest();
  overflow.pending_delta_count = std::numeric_limits<platform::u64>::max();
  overflow.incoming_delta_count = 1;
  overflow.max_pending_delta_count = std::numeric_limits<platform::u64>::max();
  const auto overflow_decision =
      idx::SelectPageAwareSecondaryChangeBufferV2(overflow);
  Require(!overflow_decision.ok() &&
              overflow_decision.reason ==
                  "secondary_change_buffer_backlog_cap_exceeded",
          "ODF-054 backlog overflow was not refused");
}

void RefusedMissingOverlayAndRecoveryProofPath() {
  auto no_overlay = BaseRequest();
  no_overlay.delta_overlay_available = false;
  const auto overlay_decision =
      idx::SelectPageAwareSecondaryChangeBufferV2(no_overlay);
  Require(!overlay_decision.ok() &&
              overlay_decision.reason ==
                  "secondary_change_buffer_overlay_unavailable",
          "ODF-054 missing overlay refusal mismatch");
  Require(overlay_decision.counters.overlay_refusals == 1,
          "ODF-054 missing overlay counter mismatch");

  auto no_recovery = BaseRequest();
  no_recovery.persisted_recovery_proof_available = false;
  const auto recovery_decision =
      idx::SelectPageAwareSecondaryChangeBufferV2(no_recovery);
  Require(!recovery_decision.ok() &&
              recovery_decision.reason ==
                  "secondary_change_buffer_recovery_proof_missing",
          "ODF-054 missing recovery proof refusal mismatch");
  Require(recovery_decision.counters.recovery_refusals == 1,
          "ODF-054 missing recovery proof counter mismatch");
}

void MgaFinalityAuthorityGuard() {
  auto request = BaseRequest();
  request.page_finality.map_is_transaction_finality_authority = true;
  const auto map_authority =
      idx::SelectPageAwareSecondaryChangeBufferV2(request);
  Require(!map_authority.ok() &&
              map_authority.reason ==
                  "durable_mga_inventory_authority_required",
          "ODF-054 finality-map authority claim was not refused");
  Require(map_authority.counters.finality_authority_refusals == 1,
          "ODF-054 finality authority refusal counter mismatch");
  Require(EvidenceHas(map_authority, "page_finality_advisory_only", "true"),
          "ODF-054 finality guard lost advisory-only evidence");

  auto no_inventory = BaseRequest();
  no_inventory.durable_transaction_inventory_authoritative = false;
  const auto inventory_guard =
      idx::SelectPageAwareSecondaryChangeBufferV2(no_inventory);
  Require(!inventory_guard.ok() &&
              inventory_guard.reason ==
                  "durable_mga_inventory_authority_required",
          "ODF-054 missing durable inventory authority was not refused");
}

void CommitGroupPlannerRoutesChangeBufferDecision() {
  idx::CommitGroupLocalityIndexApplyItem item;
  item.source_batch_ordinal = 0;
  item.source_row_ordinal = 0;
  item.index_uuid = "idx-city";
  item.family = "btree";
  item.profile = "rowstore_scalar_btree_v1";
  item.unique = false;
  item.target_keys.push_back("city=oslo");
  const auto plan = idx::PlanCommitGroupLocalityIndexApply({item});
  Require(plan.accepted, "ODF-054 locality planner setup refused item");
  Require(plan.secondary_change_buffer_decisions.size() == 1,
          "ODF-054 locality planner did not route change-buffer decision");
  Require(!plan.secondary_change_buffer_decisions.front().selected &&
              plan.secondary_change_buffer_decisions.front().reason ==
                  "runtime_deferred_secondary_index_disabled",
          "ODF-054 locality planner did not fail closed without proofs");

  item.secondary_change_buffer_request_present = true;
  item.secondary_change_buffer_request = BaseRequest();
  const auto selected_plan = idx::PlanCommitGroupLocalityIndexApply({item});
  Require(selected_plan.accepted,
          "ODF-054 locality planner refused item with proofs");
  Require(selected_plan.secondary_change_buffer_decisions.size() == 1,
          "ODF-054 locality planner lost proof-backed decision");
  Require(selected_plan.secondary_change_buffer_decisions.front().selected &&
              !selected_plan.secondary_change_buffer_decisions.front()
                   .synchronous_fallback_required,
          "ODF-054 locality planner did not select proof-backed buffering");

  item.unique = true;
  item.secondary_change_buffer_request.index_kind = idx::SecondaryIndexKind::non_unique;
  const auto unique_plan = idx::PlanCommitGroupLocalityIndexApply({item});
  Require(unique_plan.accepted,
          "ODF-054 locality planner refused unique setup before decision");
  Require(!unique_plan.secondary_change_buffer_decisions.front().selected &&
              unique_plan.secondary_change_buffer_decisions.front()
                      .counters.unique_refusals == 1,
          "ODF-054 locality planner did not force unique items to fail closed");
}

}  // namespace

int main() {
  AcceptedNonUniqueColdRandomIoPath();
  RefusedUniqueIndexPath();
  RefusedHotLowRandomIoPath();
  RefusedBacklogCapPath();
  RefusedMissingOverlayAndRecoveryProofPath();
  MgaFinalityAuthorityGuard();
  CommitGroupPlannerRoutesChangeBufferDecision();
  return EXIT_SUCCESS;
}
