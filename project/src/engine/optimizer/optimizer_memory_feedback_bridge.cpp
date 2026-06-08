// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_memory_feedback_bridge.hpp"

#include "runtime_platform.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>

namespace scratchbird::engine::optimizer {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityBoundary =
    "optimizer_memory_feedback.authority_scope=advisory_only_not_transaction_finality_visibility_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_provider_cluster_or_agent_authority";
constexpr const char* kSchemaId = "sb.optimizer.memory_feedback_evidence.v1";
constexpr const char* kLedgerSource = "resource_governance_reservation_ledger";
constexpr const char* kSupportSnapshotSource =
    "memory_support_bundle_metric_snapshot";
constexpr const char* kOperationMetricSource = "real_operation_memory_metrics";
constexpr u64 kMaxFreshnessBudgetTicks = 24ull * 60ull * 60ull * 1000000ull;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status BlockStatus() {
  return {StatusCode::memory_invalid_request, Severity::error,
          Subsystem::engine};
}

OptimizerMemoryFeedbackBridgeResult Block(
    const OptimizerMemoryFeedbackEvidence& feedback,
    const char* code,
    const char* reason) {
  OptimizerMemoryFeedbackBridgeResult result;
  result.status = BlockStatus();
  result.accepted = false;
  result.fail_closed = true;
  result.ceic_059_contract_accepted = false;
  result.authority_boundaries_clean = false;
  result.diagnostic_code = code;
  result.evidence.push_back("MMCH_OPTIMIZER_MEMORY_FEEDBACK_EVIDENCE_BRIDGE");
  result.evidence.push_back("CEIC_059_OPTIMIZER_MEMORY_FEEDBACK_GOVERNED_LEDGER");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("optimizer_memory_feedback.accepted=false");
  result.evidence.push_back(std::string("optimizer_memory_feedback.reason=") +
                            reason);
  result.evidence.push_back("optimizer_memory_feedback.redacted=true");
  result.diagnostic = MakeDiagnostic(
      result.status.code, result.status.severity, result.status.subsystem,
      result.diagnostic_code, "optimizer.memory_feedback.fail_closed",
      {DiagnosticArgument{"query_uuid", feedback.query_uuid},
       DiagnosticArgument{"scope_uuid", feedback.scope_uuid},
       DiagnosticArgument{"reason", reason},
       DiagnosticArgument{"authority_scope", kAuthorityBoundary}},
      {}, "optimizer_memory_feedback_bridge",
      "Use only trusted governed runtime memory feedback; never use parser, donor, WAL, recovery, or benchmark evidence as optimizer authority.");
  return result;
}

std::string Lower(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

bool AllZeroLike(std::string_view value) {
  bool saw_zero = false;
  for (const char ch : value) {
    if (ch == '0') {
      saw_zero = true;
      continue;
    }
    if (ch == '-' || ch == ':' || ch == '_' || ch == '.') {
      continue;
    }
    return false;
  }
  return saw_zero;
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool IsPlaceholderToken(std::string_view value) {
  if (value.empty()) return true;
  const auto lower = Lower(value);
  return lower == "0" || lower == "na" || lower == "n/a" ||
         lower == "none" || lower == "unknown" ||
         lower == "result-contract-v1" || lower == "digest" ||
         lower == "memory-snapshot:digest" || AllZeroLike(lower) ||
         Contains(lower, "placeholder") || Contains(lower, "local_default") ||
         Contains(lower, "local-default") || Contains(lower, "policy_default") ||
         Contains(lower, "policy-default") || Contains(lower, "synthetic") ||
         Contains(lower, "test-only") || Contains(lower, "test_only") ||
         Contains(lower, "fixture");
}

bool ValidDigest(std::string_view value) {
  if (IsPlaceholderToken(value)) return false;
  if (value.size() <= 12) return false;
  return value.substr(0, 7) == "sha256:";
}

bool ValidIdentity(std::string_view value) {
  return !IsPlaceholderToken(value);
}

bool PlaceholderGeneration(u64 value) {
  return value <= 1;
}

bool TrustedSourceKind(std::string_view source_kind) {
  return source_kind == kLedgerSource ||
         source_kind == kSupportSnapshotSource ||
         source_kind == kOperationMetricSource;
}

bool ReservationProofRequired(std::string_view source_kind) {
  return source_kind == kLedgerSource || source_kind == kOperationMetricSource;
}

bool TrustMatchesSource(const OptimizerMemoryFeedbackEvidence& feedback) {
  if (feedback.source_kind == feedback.trust_provenance) return true;
  return feedback.source_kind == kSupportSnapshotSource &&
         feedback.trust_provenance == "memory_support_bundle_redacted_snapshot";
}

bool AuthorityBoundaryDrift(const OptimizerMemoryFeedbackEvidence& feedback) {
  return !feedback.advisory_only ||
         !feedback.mga_visibility_recheck_preserved ||
         !feedback.security_recheck_preserved ||
         feedback.transaction_finality_authority ||
         feedback.visibility_authority ||
         feedback.authorization_security_authority ||
         feedback.recovery_authority ||
         feedback.parser_authority ||
         feedback.client_authority ||
         feedback.donor_authority ||
         feedback.wal_authority ||
         feedback.parser_client_or_donor_authority ||
         feedback.recovery_or_wal_authority ||
         feedback.benchmark_authority ||
         feedback.optimizer_plan_authority ||
         feedback.index_finality_authority ||
         feedback.provider_finality_authority ||
         feedback.local_cluster_authority ||
         feedback.cluster_authority ||
         feedback.agent_action_authority;
}

u64 AgeTicks(const OptimizerMemoryFeedbackEvidence& feedback) {
  if (feedback.received_timestamp_ticks < feedback.observed_timestamp_ticks) {
    return feedback.max_age_ticks + 1;
  }
  return feedback.received_timestamp_ticks - feedback.observed_timestamp_ticks;
}

}  // namespace

OptimizerMemoryFeedbackBridgeResult BuildOptimizerMemoryFeedbackForPlanner(
    const OptimizerMemoryFeedbackEvidence& feedback) {
  if (feedback.schema_id != kSchemaId || feedback.schema_version != 1) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.BAD_SCHEMA",
                 "ceic_059_schema_version_required");
  }
  if (!ValidIdentity(feedback.query_uuid) ||
      !ValidIdentity(feedback.scope_uuid) ||
      !ValidIdentity(feedback.route_kind) ||
      !ValidIdentity(feedback.operator_family) ||
      !ValidIdentity(feedback.plan_shape)) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.MISSING_SCOPE",
                 "query_scope_route_operator_and_plan_shape_required");
  }
  if (PlaceholderGeneration(feedback.policy_generation) ||
      PlaceholderGeneration(feedback.feedback_generation) ||
      PlaceholderGeneration(feedback.catalog_epoch) ||
      PlaceholderGeneration(feedback.security_epoch) ||
      PlaceholderGeneration(feedback.redaction_epoch) ||
      PlaceholderGeneration(feedback.statistics_epoch)) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.MISSING_GENERATION",
                 "policy_feedback_catalog_security_redaction_and_statistics_generations_required");
  }
  if (!TrustedSourceKind(feedback.source_kind) ||
      feedback.source_kind == feedback.source_quality ||
      feedback.source_kind == "observed_runtime") {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNTRUSTED_SOURCE",
                 "trusted_ceic_059_source_kind_required");
  }
  if (IsPlaceholderToken(feedback.source_quality) ||
      !feedback.trusted_provenance ||
      IsPlaceholderToken(feedback.trust_provenance) ||
      !TrustMatchesSource(feedback) ||
      !ValidDigest(feedback.provenance_digest)) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNTRUSTED_PROVENANCE",
                 "trusted_source_specific_provenance_required");
  }
  if (ReservationProofRequired(feedback.source_kind) &&
      (!feedback.governed_reservation ||
       !feedback.reservation_token_bound ||
       !ValidIdentity(feedback.reservation_id) ||
       !ValidIdentity(feedback.reservation_token) ||
       PlaceholderGeneration(feedback.reservation_generation))) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNGOVERNED",
                 "governed_reservation_id_token_and_generation_required");
  }
  if (feedback.source_kind == kLedgerSource &&
      !feedback.resource_governance_ledger_recorded) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNGOVERNED",
                 "resource_governance_ledger_record_required");
  }
  if (feedback.source_kind == kOperationMetricSource &&
      (!feedback.real_operation_metric ||
       !feedback.operation_metric_runtime_path)) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNTRUSTED_SOURCE",
                 "real_operation_metric_runtime_path_required");
  }
  if (feedback.source_kind == kSupportSnapshotSource &&
      (!feedback.bounded_support_bundle ||
       !feedback.support_bundle_redacted ||
       !feedback.support_bundle_fresh ||
       !ValidDigest(feedback.support_snapshot_digest))) {
    return Block(feedback,
                 "SB_OPTIMIZER_MEMORY_FEEDBACK.SUPPORT_BUNDLE_PROOF_REQUIRED",
                 "redacted_bounded_fresh_support_bundle_snapshot_required");
  }
  if (!ValidDigest(feedback.redaction_digest) ||
      !ValidDigest(feedback.metric_snapshot_digest) ||
      IsPlaceholderToken(feedback.redaction_class) ||
      feedback.redaction_class == "protected_material") {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.PLACEHOLDER_DIGEST",
                 "redaction_and_metric_snapshot_digests_required");
  }
  if (feedback.synthetic || feedback.test_evidence ||
      feedback.local_default_evidence || feedback.policy_default_evidence) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.SYNTHETIC",
                 "synthetic_test_local_or_policy_default_feedback_is_forbidden");
  }
  if (feedback.observed_timestamp_ticks == 0 ||
      feedback.received_timestamp_ticks == 0 ||
      feedback.max_age_ticks == 0 ||
      feedback.max_age_ticks > kMaxFreshnessBudgetTicks ||
      feedback.max_age_ticks == std::numeric_limits<u64>::max()) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNBOUNDED_FRESHNESS",
                 "bounded_freshness_window_required");
  }
  if (feedback.received_timestamp_ticks < feedback.observed_timestamp_ticks) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.FUTURE",
                 "future_observation_timestamp_rejected");
  }
  if (AgeTicks(feedback) > feedback.max_age_ticks) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.STALE",
                 "feedback_exceeds_freshness_budget");
  }
  if (!feedback.protected_material_redacted ||
      feedback.protected_material_exposed) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.REDACTION_REQUIRED",
                 "protected_material_must_be_redacted");
  }
  if (AuthorityBoundaryDrift(feedback)) {
    return Block(feedback, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNSAFE_AUTHORITY",
                 "feedback_must_be_advisory_and_preserve_all_authority_boundaries");
  }

  OptimizerMemoryFeedbackBridgeResult result;
  result.status = OkStatus();
  result.accepted = true;
  result.fail_closed = false;
  result.ceic_059_contract_accepted = true;
  result.authority_boundaries_clean = true;
  result.diagnostic_code = "SB_OPTIMIZER_MEMORY_FEEDBACK.ACCEPTED";
  result.evidence.push_back("MMCH_OPTIMIZER_MEMORY_FEEDBACK_EVIDENCE_BRIDGE");
  result.evidence.push_back("CEIC_059_OPTIMIZER_MEMORY_FEEDBACK_GOVERNED_LEDGER");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("optimizer_memory_feedback.accepted=true");
  result.evidence.push_back("optimizer_memory_feedback.schema=sb.optimizer.memory_feedback_evidence.v1");
  result.evidence.push_back("optimizer_memory_feedback.source_kind=" +
                            feedback.source_kind);
  result.evidence.push_back("optimizer_memory_feedback.trusted_provenance=true");
  result.evidence.push_back("optimizer_memory_feedback.provenance_digest=" +
                            feedback.provenance_digest);
  result.evidence.push_back("optimizer_memory_feedback.redaction_digest=" +
                            feedback.redaction_digest);
  result.evidence.push_back("optimizer_memory_feedback.metric_snapshot_digest=" +
                            feedback.metric_snapshot_digest);
  if (!feedback.support_snapshot_digest.empty()) {
    result.evidence.push_back(
        "optimizer_memory_feedback.support_snapshot_digest=" +
        feedback.support_snapshot_digest);
  }
  result.evidence.push_back(
      std::string("optimizer_memory_feedback.governed_reservation=") +
      (feedback.governed_reservation ? "true" : "false"));
  result.evidence.push_back(
      std::string("optimizer_memory_feedback.reservation_id_present=") +
      (!feedback.reservation_id.empty() ? "true" : "false"));
  result.evidence.push_back("optimizer_memory_feedback.protected_material_redacted=true");
  result.evidence.push_back("optimizer_memory_feedback.mga_visibility_recheck=preserved");
  result.evidence.push_back("optimizer_memory_feedback.security_recheck=preserved");
  result.evidence.push_back("optimizer_memory_feedback.benchmark_authority=false");
  result.evidence.push_back("optimizer_memory_feedback.optimizer_plan_authority=false");
  result.evidence.push_back("optimizer_memory_feedback.index_provider_cluster_agent_authority=false");
  result.evidence.push_back("optimizer_memory_feedback.spill_passes=" +
                            std::to_string(feedback.spill_passes));

  result.runtime_feedback.operator_family = feedback.operator_family;
  result.runtime_feedback.plan_shape = feedback.plan_shape;
  result.runtime_feedback.cost_profile_id =
      "memory-feedback:" + feedback.source_kind + ":" + feedback.route_kind + ":" +
      std::to_string(feedback.policy_generation) + ":" +
      std::to_string(feedback.feedback_generation);
  result.runtime_feedback.estimated_spill_bytes = 0;
  result.runtime_feedback.actual_spill_bytes = feedback.spill_bytes;
  result.runtime_feedback.memory_grant_bytes = feedback.memory_grant_bytes;
  result.runtime_feedback.peak_memory_bytes = feedback.peak_memory_bytes;
  result.runtime_feedback.estimated_resource_units = 1;
  result.runtime_feedback.actual_resource_units =
      1 + feedback.allocation_failure_count + feedback.spill_passes;
  result.runtime_feedback.freshness_microseconds = AgeTicks(feedback);
  result.runtime_feedback.max_freshness_microseconds = feedback.max_age_ticks;
  result.runtime_feedback.policy_allowed = true;
  result.runtime_feedback.advisory_only = true;
  result.runtime_feedback.mga_visibility_recheck_preserved = true;
  result.runtime_feedback.parser_or_donor_authority = false;
  result.runtime_feedback.transaction_finality_authority =
      "engine_transaction_inventory";
  result.diagnostic = MakeDiagnostic(
      result.status.code, result.status.severity, result.status.subsystem,
      result.diagnostic_code, "optimizer.memory_feedback.accepted",
      {DiagnosticArgument{"query_uuid", feedback.query_uuid},
       DiagnosticArgument{"scope_uuid", feedback.scope_uuid},
       DiagnosticArgument{"authority_scope", kAuthorityBoundary}},
      {}, "optimizer_memory_feedback_bridge", {});
  return result;
}

}  // namespace scratchbird::engine::optimizer
