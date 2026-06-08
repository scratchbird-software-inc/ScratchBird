// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-052 bottom-up exact-index cleanup before leaf split selection.

#include "exact_index_leaf_cleanup.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool ExactBtreeFamily(IndexFamily family) {
  switch (family) {
    case IndexFamily::btree:
    case IndexFamily::unique_btree:
    case IndexFamily::expression:
    case IndexFamily::partial:
    case IndexFamily::covering:
      return true;
    default:
      return false;
  }
}

void AddEvidence(ExactIndexLeafPressureDecision* decision,
                 std::string name,
                 std::string value) {
  decision->evidence.push_back({std::move(name), std::move(value)});
}

u64 RelevantRetainedCount(const SecondaryIndexGarbageCleanupResult& cleanup) {
  return cleanup.after.relevant_delta_records;
}

void PopulateCommonEvidence(ExactIndexLeafPressureDecision* decision) {
  AddEvidence(decision,
              "exact_leaf_cleanup_attempted",
              decision->cleanup_attempted ? "true" : "false");
  AddEvidence(decision,
              "exact_leaf_cleanup_accepted",
              decision->cleanup_accepted ? "true" : "false");
  AddEvidence(decision,
              "exact_leaf_cleanup_refused",
              decision->cleanup_refused ? "true" : "false");
  AddEvidence(decision,
              "exact_leaf_cleanup_budget_exhausted",
              decision->budget_exhausted ? "true" : "false");
  AddEvidence(decision,
              "exact_leaf_cleanup_cleaned_count",
              std::to_string(decision->counters.cleaned_count));
  AddEvidence(decision,
              "exact_leaf_cleanup_retained_count",
              std::to_string(decision->counters.retained_count));
  AddEvidence(decision,
              "exact_leaf_split_avoided",
              decision->split_avoided ? "true" : "false");
  AddEvidence(decision,
              "exact_leaf_split_selected",
              decision->split_selected ? "true" : "false");
  AddEvidence(decision,
              "exact_leaf_pressure_selected_action",
              ExactIndexLeafPressureActionName(decision->action));
  AddEvidence(decision,
              "exact_leaf_cleanup_mga_authority_source",
              decision->mga_authority_source);
  AddEvidence(decision,
              "exact_leaf_cleanup_fail_open_reason",
              decision->fail_open_reason.empty() ? "none"
                                                 : decision->fail_open_reason);
}

ExactIndexLeafPressureDecision Finish(
    ExactIndexLeafPressureDecision decision,
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  decision.status = status;
  PopulateCommonEvidence(&decision);
  decision.diagnostic = MakeExactIndexLeafCleanupDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return decision;
}

ExactIndexLeafPressureDecision SelectSplit(
    ExactIndexLeafPressureDecision decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    bool cleanup_refused) {
  decision.action = ExactIndexLeafPressureAction::split_selected;
  decision.split_selected = true;
  decision.split_avoided = false;
  decision.cleanup_refused = cleanup_refused;
  decision.fail_open_reason = reason;
  decision.counters.cleanup_refused += cleanup_refused ? 1 : 0;
  decision.counters.split_selected = 1;
  return Finish(std::move(decision),
                OkStatus(),
                std::move(diagnostic_code),
                std::move(message_key),
                std::move(reason));
}

}  // namespace

const char* ExactIndexLeafPressureActionName(
    ExactIndexLeafPressureAction action) {
  switch (action) {
    case ExactIndexLeafPressureAction::insert_without_split:
      return "insert_without_split";
    case ExactIndexLeafPressureAction::cleanup_avoided_split:
      return "cleanup_avoided_split";
    case ExactIndexLeafPressureAction::split_selected:
      return "split_selected";
  }
  return "split_selected";
}

ExactIndexLeafPressureDecision PlanExactIndexLeafPressureAction(
    const ExactIndexLeafPressureRequest& request) {
  ExactIndexLeafPressureDecision decision;
  decision.status = OkStatus();
  decision.selected_ledger = request.cleanup.ledger;
  decision.mga_authority_source =
      request.mga_cleanup_authority.authority_source.empty()
          ? "none"
          : request.mga_cleanup_authority.authority_source;
  decision.projected_leaf_entry_count =
      request.current_leaf_entry_count + request.pending_insert_entry_count;

  if (!request.index_uuid.valid() || !request.table_uuid.valid() ||
      !SameUuid(request.index_uuid, request.cleanup.index_uuid) ||
      !SameUuid(request.table_uuid, request.cleanup.table_uuid) ||
      !ExactBtreeFamily(request.family) || request.leaf_entry_capacity == 0) {
    decision.cleanup_refused = true;
    decision.counters.cleanup_refused = 1;
    decision.fail_open_reason = "invalid_exact_leaf_cleanup_request";
    return Finish(std::move(decision),
                  ErrorStatus(),
                  "EXACT_INDEX_LEAF_CLEANUP.INVALID_REQUEST",
                  "core.index.exact_leaf_cleanup.invalid_request",
                  "valid exact index identity, family, cleanup request, and leaf capacity are required");
  }

  if (decision.projected_leaf_entry_count <= request.leaf_entry_capacity) {
    decision.action = ExactIndexLeafPressureAction::insert_without_split;
    decision.split_selected = false;
    decision.split_avoided = false;
    decision.fail_open_reason = "none";
    return Finish(std::move(decision),
                  OkStatus(),
                  "EXACT_INDEX_LEAF_CLEANUP.NO_PRESSURE",
                  "core.index.exact_leaf_cleanup.no_pressure");
  }

  decision.leaf_pressure_detected = true;
  decision.cleanup_attempted = true;
  decision.counters.cleanup_attempted = 1;
  decision.required_reclaim_count =
      decision.projected_leaf_entry_count - request.leaf_entry_capacity;

  if (request.index_kind == SecondaryIndexKind::unique ||
      request.family == IndexFamily::unique_btree) {
    decision.unique_exact_recheck_required = true;
    AddEvidence(&decision, "exact_leaf_unique_recheck_required", "true");
    return SelectSplit(
        std::move(decision),
        "EXACT_INDEX_LEAF_CLEANUP.UNIQUE_SPLIT_SELECTED",
        "core.index.exact_leaf_cleanup.unique_split_selected",
        "unique_exact_index_requires_synchronous_recheck",
        true);
  }

  if (!request.mga_cleanup_authority.accepted ||
      request.mga_cleanup_authority.cleanup_horizon_local_transaction_id == 0) {
    return SelectSplit(
        std::move(decision),
        "EXACT_INDEX_LEAF_CLEANUP.NON_AUTHORITATIVE_SPLIT_SELECTED",
        "core.index.exact_leaf_cleanup.non_authoritative_split_selected",
        request.mga_cleanup_authority.refusal_reason.empty()
            ? "mga_cleanup_authority_refused"
            : request.mga_cleanup_authority.refusal_reason,
        true);
  }

  if (request.cleanup.max_records_to_scan == 0 ||
      request.cleanup.max_records_to_clean == 0) {
    return SelectSplit(
        std::move(decision),
        "EXACT_INDEX_LEAF_CLEANUP.BUDGET_REQUIRED",
        "core.index.exact_leaf_cleanup.budget_required",
        "bounded_scan_and_clean_budgets_required",
        true);
  }

  auto cleanup_request = request.cleanup;
  cleanup_request.cleanup_horizon_authoritative = true;
  cleanup_request.authoritative_cleanup_horizon_local_transaction_id =
      request.mga_cleanup_authority.cleanup_horizon_local_transaction_id;
  cleanup_request.index_kind = request.index_kind;
  if (request.max_cleanup_steps != 0) {
    cleanup_request.max_records_to_scan =
        std::min(cleanup_request.max_records_to_scan,
                 request.max_cleanup_steps);
  }

  decision.cleanup_result =
      RunSecondaryIndexGarbageCleanupBatch(cleanup_request);
  decision.budget_exhausted = decision.cleanup_result.budget_exhausted ||
                              decision.cleanup_result.decision ==
                                  SecondaryIndexGarbageCleanupDecisionKind::
                                      budget_exhausted;
  decision.counters.budget_exhausted = decision.budget_exhausted ? 1 : 0;
  decision.counters.cleaned_count =
      decision.cleanup_result.after.cleaned_garbage_records;
  decision.counters.retained_count =
      RelevantRetainedCount(decision.cleanup_result);
  decision.selected_ledger = decision.cleanup_result.cleaned_ledger;

  if (decision.budget_exhausted) {
    return SelectSplit(std::move(decision),
                       "EXACT_INDEX_LEAF_CLEANUP.BUDGET_EXHAUSTED_SPLIT",
                       "core.index.exact_leaf_cleanup.budget_exhausted_split",
                       "cleanup_budget_exhausted_fail_open_to_split",
                       true);
  }

  if (!decision.cleanup_result.validation_before_ok ||
      !decision.cleanup_result.validation_after_ok ||
      decision.cleanup_result.fail_closed) {
    decision.selected_ledger = request.cleanup.ledger;
    return SelectSplit(std::move(decision),
                       "EXACT_INDEX_LEAF_CLEANUP.VALIDATION_SPLIT",
                       "core.index.exact_leaf_cleanup.validation_split",
                       "effective_exact_index_validation_mismatch",
                       true);
  }

  if (decision.counters.cleaned_count < decision.required_reclaim_count) {
    return SelectSplit(std::move(decision),
                       "EXACT_INDEX_LEAF_CLEANUP.INSUFFICIENT_RECLAIM_SPLIT",
                       "core.index.exact_leaf_cleanup.insufficient_reclaim_split",
                       "insufficient_reclaimable_entries",
                       true);
  }

  decision.cleanup_accepted = true;
  decision.cleanup_refused = false;
  decision.split_selected = false;
  decision.split_avoided = true;
  decision.action = ExactIndexLeafPressureAction::cleanup_avoided_split;
  decision.counters.cleanup_accepted = 1;
  decision.counters.split_avoided = 1;
  decision.projected_leaf_entry_count =
      request.current_leaf_entry_count + request.pending_insert_entry_count -
      decision.counters.cleaned_count;
  decision.fail_open_reason = "none";
  return Finish(std::move(decision),
                OkStatus(),
                "EXACT_INDEX_LEAF_CLEANUP.SPLIT_AVOIDED",
                "core.index.exact_leaf_cleanup.split_avoided",
                std::to_string(decision.counters.cleaned_count));
}

DiagnosticRecord MakeExactIndexLeafCleanupDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.exact_leaf_cleanup",
                        status.ok() ? "" : "fail open to normal exact leaf split");
}

}  // namespace scratchbird::core::index
