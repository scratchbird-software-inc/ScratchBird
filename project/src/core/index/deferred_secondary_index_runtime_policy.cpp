// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_DEFERRED_INDEX_FEATURE_FLAG
#include "deferred_secondary_index_runtime_policy.hpp"

#include "unique_index_deferral_policy.hpp"

#include <utility>
#include <vector>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status BufferOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status BufferRefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::warning,
          Subsystem::engine};
}

bool OptionEquals(const std::string& actual, const std::string& expected) {
  if (actual == expected) {
    return true;
  }
  const auto equals = expected.find('=');
  if (equals == std::string::npos) {
    return false;
  }
  return actual == expected.substr(0, equals) + ":" + expected.substr(equals + 1);
}

const char* SecondaryIndexKindName(SecondaryIndexKind kind) {
  switch (kind) {
    case SecondaryIndexKind::non_unique:
      return "non_unique";
    case SecondaryIndexKind::unique:
      return "unique";
  }
  return "unknown";
}

bool RecoveryActionAllowsBufferedAppend(
    SecondaryIndexDeltaLedgerRecoveryAction action) {
  return action == SecondaryIndexDeltaLedgerRecoveryAction::no_action ||
         action ==
             SecondaryIndexDeltaLedgerRecoveryAction::retain_for_mga_transaction_finality ||
         action == SecondaryIndexDeltaLedgerRecoveryAction::apply_overlay_then_merge;
}

bool WouldExceedCap(u64 current, u64 incoming, u64 cap) {
  return current > cap || incoming > cap - current;
}

void AddEvidence(PageAwareSecondaryChangeBufferDecision* decision,
                 std::string name,
                 std::string value) {
  if (decision != nullptr) {
    decision->evidence.push_back({std::move(name), std::move(value)});
  }
}

void AddBaseEvidence(PageAwareSecondaryChangeBufferDecision* decision,
                     const PageAwareSecondaryChangeBufferRequest& request,
                     const DeferredSecondaryIndexRuntimeDecision& runtime) {
  AddEvidence(decision, "secondary_change_buffer_policy",
              "page_aware_secondary_change_buffer_v2");
  AddEvidence(decision, "index_kind", SecondaryIndexKindName(request.index_kind));
  AddEvidence(decision, "runtime_enabled",
              runtime.runtime_enabled ? "true" : "false");
  AddEvidence(decision, "feature_enabled",
              runtime.feature_enabled ? "true" : "false");
  AddEvidence(decision, "overlay_available",
              request.delta_overlay_available ? "true" : "false");
  AddEvidence(decision, "overlay_read_safe",
              request.delta_overlay_read_safe ? "true" : "false");
  AddEvidence(decision, "target_page_cold",
              request.target_page_cold ? "true" : "false");
  AddEvidence(decision, "target_page_random_io_score",
              std::to_string(request.target_page_random_io_score));
  AddEvidence(decision, "pending_delta_count",
              std::to_string(request.pending_delta_count));
  AddEvidence(decision, "incoming_delta_count",
              std::to_string(request.incoming_delta_count));
  AddEvidence(decision, "max_pending_delta_count",
              std::to_string(request.max_pending_delta_count));
  AddEvidence(decision, "pending_delta_bytes",
              std::to_string(request.pending_delta_bytes));
  AddEvidence(decision, "incoming_delta_bytes",
              std::to_string(request.incoming_delta_bytes));
  AddEvidence(decision, "max_pending_delta_bytes",
              std::to_string(request.max_pending_delta_bytes));
  AddEvidence(decision, "mga_finality_authority",
              "engine_transaction_inventory");
  AddEvidence(decision, "page_finality_advisory_only", "true");
}

DiagnosticRecord MakeChangeBufferDiagnostic(
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
                        "core.index.deferred_secondary_index_runtime_policy",
                        status.ok()
                            ? ""
                            : "fall back to synchronous secondary-index maintenance");
}

PageAwareSecondaryChangeBufferDecision FinishDecision(
    PageAwareSecondaryChangeBufferDecision decision,
    bool selected,
    std::string reason,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  decision.selected = selected;
  decision.synchronous_fallback_required = !selected;
  decision.reason = std::move(reason);
  decision.status = selected ? BufferOkStatus() : BufferRefuseStatus();
  decision.diagnostic = MakeChangeBufferDiagnostic(decision.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  AddEvidence(&decision, "secondary_change_buffer_selected",
              selected ? "true" : "false");
  AddEvidence(&decision, "secondary_change_buffer_reason", decision.reason);
  AddEvidence(&decision, "synchronous_fallback_required",
              decision.synchronous_fallback_required ? "true" : "false");
  AddEvidence(&decision, "decision_counter_selected",
              std::to_string(decision.counters.selected));
  AddEvidence(&decision, "decision_counter_refused",
              std::to_string(decision.counters.refused));
  return decision;
}

PageAwareSecondaryChangeBufferDecision Refuse(
    PageAwareSecondaryChangeBufferDecision decision,
    std::string reason,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  ++decision.counters.refused;
  return FinishDecision(std::move(decision),
                        false,
                        std::move(reason),
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(detail));
}

}  // namespace

bool DeferredIndexOptionEnabled(const std::vector<std::string>& option_envelopes,
                                const std::string& option) {
  for (const auto& candidate : option_envelopes) {
    if (OptionEquals(candidate, option)) {
      return true;
    }
  }
  return false;
}

DeferredSecondaryIndexRuntimeDecision ResolveDeferredSecondaryIndexRuntimePolicy(
    const std::vector<std::string>& option_envelopes) {
  // DPC_DEFERRED_INDEX_FEATURE_FLAG_GATE
  DeferredSecondaryIndexRuntimeDecision decision;
  decision.runtime_enabled =
      DeferredIndexOptionEnabled(option_envelopes, kDeferredSecondaryIndexRuntimeOption);
  decision.feature_enabled =
      DeferredIndexOptionEnabled(option_envelopes, kSecondaryIndexDeltaLedgerFeatureOption);
  decision.readers_overlay_committed_deltas =
      DeferredIndexOptionEnabled(option_envelopes, kDeltaLedgerReaderOverlayOption);
  decision.cleanup_horizon_bound =
      DeferredIndexOptionEnabled(option_envelopes, kDeltaLedgerCleanupHorizonBoundOption);
  decision.recovery_classifiable =
      DeferredIndexOptionEnabled(option_envelopes, kDeltaLedgerRecoveryClassifiableOption);

  if (!decision.runtime_enabled) {
    decision.fallback_reason = "runtime_deferred_secondary_index_disabled";
    return decision;
  }
  if (!decision.feature_enabled) {
    decision.fallback_reason = "secondary_index_delta_ledger_feature_disabled";
    return decision;
  }
  if (!decision.readers_overlay_committed_deltas ||
      !decision.cleanup_horizon_bound ||
      !decision.recovery_classifiable) {
    decision.fallback_reason = "secondary_index_delta_ledger_safety_proofs_incomplete";
    return decision;
  }

  decision.enabled = true;
  decision.synchronous_fallback_required = false;
  decision.fallback_reason.clear();
  return decision;
}

PageAwareSecondaryChangeBufferDecision SelectPageAwareSecondaryChangeBufferV2(
    const PageAwareSecondaryChangeBufferRequest& request) {
  PageAwareSecondaryChangeBufferDecision decision;
  decision.counters.decisions = 1;
  decision.effective_random_io_threshold =
      request.cold_random_io_score_threshold;
  decision.finality_map_transaction_authority =
      request.page_finality_evidence_present &&
      request.page_finality.map_is_transaction_finality_authority;
  decision.durable_mga_inventory_remains_authority =
      request.durable_transaction_inventory_authoritative &&
      (!request.page_finality_evidence_present ||
       request.page_finality.durable_mga_inventory_remains_authority);

  const auto runtime =
      ResolveDeferredSecondaryIndexRuntimePolicy(request.option_envelopes);
  AddBaseEvidence(&decision, request, runtime);
  AddEvidence(&decision, "runtime_safety_proofs_complete",
              runtime.enabled ? "true" : "false");

  if (!runtime.enabled) {
    return Refuse(std::move(decision),
                  runtime.fallback_reason,
                  "secondary_change_buffer_runtime_refused",
                  "core.index.secondary_change_buffer.runtime_refused",
                  runtime.fallback_reason);
  }

  if (request.index_kind == SecondaryIndexKind::unique) {
    UniqueIndexDeferralPolicyRequest unique_request;
    unique_request.uniqueness = IndexUniquenessClass::unique_secondary;
    unique_request.request_kind =
        IndexDeferralRequestKind::unique_deferred_with_reservation;
    unique_request.reservation_protocol_present =
        request.unique_reservation_protocol_present;
    unique_request.reservation_protocol_proven =
        request.unique_reservation_protocol_proven;
    unique_request.reservation_protocol_enabled =
        request.unique_reservation_protocol_enabled;
    unique_request.reservation_protocol_gate_token =
        request.unique_reservation_protocol_gate_token;
    const auto unique_decision =
        EvaluateUniqueIndexDeferralPolicy(unique_request);
    ++decision.counters.unique_refusals;
    if (unique_decision.ok()) {
      return Refuse(std::move(decision),
                    "unique_dml_route_incomplete",
                    "secondary_change_buffer_unique_dml_route_incomplete",
                    "core.index.secondary_change_buffer.unique_dml_route_incomplete",
                    "unique secondary buffering still requires DML route closure to consume reservation ledger proofs");
    }
    return Refuse(std::move(decision),
                  unique_decision.diagnostic.diagnostic_code,
                  unique_decision.diagnostic.diagnostic_code,
                  unique_decision.diagnostic.message_key,
                  "unique secondary index buffering remains synchronous unless a proven reservation protocol admits it");
  }

  if (!decision.durable_mga_inventory_remains_authority ||
      decision.finality_map_transaction_authority) {
    ++decision.counters.finality_authority_refusals;
    return Refuse(std::move(decision),
                  "durable_mga_inventory_authority_required",
                  "secondary_change_buffer_finality_authority_refused",
                  "core.index.secondary_change_buffer.finality_authority_refused",
                  "page-finality evidence is advisory only; durable transaction inventory remains required");
  }

  if (request.page_finality_evidence_present && request.page_finality.accepted) {
    decision.page_finality_used_as_advisory_acceleration = true;
    if (request.page_finality_advisory_score_credit >=
        decision.effective_random_io_threshold) {
      decision.effective_random_io_threshold = 1;
    } else {
      decision.effective_random_io_threshold -=
          request.page_finality_advisory_score_credit;
    }
  }
  AddEvidence(&decision, "page_finality_advisory_acceleration",
              decision.page_finality_used_as_advisory_acceleration ? "true"
                                                                   : "false");
  AddEvidence(&decision, "effective_random_io_threshold",
              std::to_string(decision.effective_random_io_threshold));
  AddEvidence(&decision, "finality_map_transaction_authority",
              decision.finality_map_transaction_authority ? "true" : "false");
  AddEvidence(&decision, "durable_mga_inventory_remains_authority",
              decision.durable_mga_inventory_remains_authority ? "true"
                                                               : "false");

  if (!request.target_page_cold ||
      request.target_page_random_io_score <
          decision.effective_random_io_threshold) {
    ++decision.counters.hot_page_refusals;
    return Refuse(std::move(decision),
                  "target_page_random_io_score_too_low",
                  "secondary_change_buffer_hot_page_refused",
                  "core.index.secondary_change_buffer.hot_page_refused",
                  "target page is hot or random-IO score is below threshold");
  }

  if (WouldExceedCap(request.pending_delta_count,
                     request.incoming_delta_count,
                     request.max_pending_delta_count) ||
      WouldExceedCap(request.pending_delta_bytes,
                     request.incoming_delta_bytes,
                     request.max_pending_delta_bytes)) {
    ++decision.counters.backlog_refusals;
    return Refuse(std::move(decision),
                  "secondary_change_buffer_backlog_cap_exceeded",
                  "secondary_change_buffer_backlog_cap_exceeded",
                  "core.index.secondary_change_buffer.backlog_cap_exceeded",
                  "delta backlog cap would be exceeded");
  }

  if (!request.delta_overlay_available || !request.delta_overlay_read_safe) {
    ++decision.counters.overlay_refusals;
    return Refuse(std::move(decision),
                  "secondary_change_buffer_overlay_unavailable",
                  "secondary_change_buffer_overlay_unavailable",
                  "core.index.secondary_change_buffer.overlay_unavailable",
                  "read-safe delta overlay is required before buffering");
  }

  if (!request.persisted_recovery_proof_available) {
    ++decision.counters.recovery_refusals;
    return Refuse(std::move(decision),
                  "secondary_change_buffer_recovery_proof_missing",
                  "secondary_change_buffer_recovery_proof_missing",
                  "core.index.secondary_change_buffer.recovery_proof_missing",
                  "persistent delta ledger recovery classification is required");
  }

  const auto recovery =
      ClassifySecondaryIndexDeltaLedgerForRecovery(
          request.persistent_delta_ledger);
  decision.recovery_class =
      SecondaryIndexDeltaLedgerRecoveryClassName(recovery.recovery_class);
  decision.recovery_action =
      SecondaryIndexDeltaLedgerRecoveryActionName(recovery.action);
  AddEvidence(&decision, "delta_ledger_recovery_class",
              decision.recovery_class);
  AddEvidence(&decision, "delta_ledger_recovery_action",
              decision.recovery_action);
  if (!recovery.ok() || !RecoveryActionAllowsBufferedAppend(recovery.action)) {
    ++decision.counters.recovery_refusals;
    return Refuse(std::move(decision),
                  "secondary_change_buffer_recovery_class_refused",
                  "secondary_change_buffer_recovery_class_refused",
                  "core.index.secondary_change_buffer.recovery_class_refused",
                  recovery.stable_reason);
  }

  ++decision.counters.selected;
  return FinishDecision(std::move(decision),
                        true,
                        "secondary_change_buffer_selected",
                        "secondary_change_buffer_selected",
                        "core.index.secondary_change_buffer.selected",
                        "non-unique cold-page secondary delta buffering selected");
}

}  // namespace scratchbird::core::index
