// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_METRIC_FEEDBACK_CONTRACTS
// Runtime feedback is optimizer-advisory only. It may calibrate relative cost
// and memory-grant estimates, but it is never transaction finality, visibility,
// parser execution, or reference authority.
struct OptimizerRuntimeFeedback {
  std::string operator_family;
  std::string plan_shape;
  std::string cost_profile_id = "local_default_v1";
  std::uint64_t estimated_rows = 0;
  std::uint64_t actual_rows = 0;
  std::uint64_t actual_rows_examined = 0;
  std::uint64_t actual_rows_filtered = 0;
  std::uint64_t loop_count = 0;
  std::uint64_t estimated_pages = 0;
  std::uint64_t actual_pages = 0;
  std::uint64_t estimated_io_operations = 0;
  std::uint64_t actual_io_operations = 0;
  std::uint64_t estimated_visibility_recheck_rows = 0;
  std::uint64_t actual_visibility_recheck_rows = 0;
  std::uint64_t estimated_spill_bytes = 0;
  std::uint64_t actual_spill_bytes = 0;
  std::uint64_t memory_grant_bytes = 0;
  std::uint64_t peak_memory_bytes = 0;
  std::uint64_t estimated_latency_microseconds = 0;
  std::uint64_t actual_latency_microseconds = 0;
  std::uint64_t estimated_resource_units = 0;
  std::uint64_t actual_resource_units = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 60000000;
  bool policy_allowed = true;
  bool advisory_only = true;
  bool mga_visibility_recheck_preserved = true;
  bool parser_or_reference_authority = false;
  std::string transaction_finality_authority = "engine_transaction_inventory";
};

struct OptimizerCalibratedCostProfile {
  bool apply = false;
  double row_cost_multiplier = 1.0;
  double page_cost_multiplier = 1.0;
  double io_cost_multiplier = 1.0;
  double visibility_cost_multiplier = 1.0;
  double memory_cost_multiplier = 1.0;
  double latency_cost_multiplier = 1.0;
  std::uint64_t spill_penalty_pages = 0;
  std::uint64_t uncertainty_penalty = 0;
  std::string profile_id = "feedback_disabled";
};

struct OptimizerMemoryGrantFeedback {
  bool apply = false;
  std::uint64_t observed_grant_bytes = 0;
  std::uint64_t observed_peak_bytes = 0;
  std::uint64_t recommended_grant_bytes = 0;
  double grant_multiplier = 1.0;
  std::string diagnostic_code = "SB_OPTIMIZER_FEEDBACK.MEMORY_GRANT_OK";
};

struct OptimizerFeedbackStatus {
  bool ok = false;
  bool applied = false;
  double estimate_error_ratio = 0.0;
  double page_error_ratio = 0.0;
  double io_error_ratio = 0.0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  OptimizerCalibratedCostProfile cost_profile;
  OptimizerMemoryGrantFeedback memory_grant;
};

// SEARCH_KEY: OPCH_ADAPTIVE_FEEDBACK_ACTUALS_PERSISTENCE
// Scoped runtime actuals and feedback persistence is optimizer-advisory only.
// Records carry policy/scope generations and can be invalidated; they never
// become transaction finality, visibility, security, recovery, parser, or reference
// authority.
struct OptimizerRuntimeFeedbackRecord {
  std::string feedback_uuid;
  std::string scope_uuid;
  std::string route_label;
  std::uint64_t feedback_generation = 0;
  std::uint64_t policy_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  OptimizerRuntimeFeedback feedback;
  OptimizerFeedbackStatus status;
  bool valid = true;
  std::string invalidation_reason;
  std::vector<std::string> evidence;
};

struct OptimizerRuntimeFeedbackInvalidation {
  std::string scope_uuid;
  std::uint64_t policy_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::string reason;
};

struct OptimizerRuntimeFeedbackSnapshot {
  std::uint64_t total_records = 0;
  std::uint64_t valid_records = 0;
  std::uint64_t invalidated_records = 0;
  std::vector<OptimizerRuntimeFeedbackRecord> records;
};

class OptimizerRuntimeFeedbackStore {
 public:
  OptimizerFeedbackStatus Record(OptimizerRuntimeFeedbackRecord record);
  std::uint64_t Invalidate(const OptimizerRuntimeFeedbackInvalidation& event);
  OptimizerRuntimeFeedbackSnapshot Snapshot() const;
  std::optional<OptimizerRuntimeFeedbackRecord> Find(const std::string& feedback_uuid) const;

 private:
  mutable std::mutex mutex_;
  std::vector<OptimizerRuntimeFeedbackRecord> records_;
};

OptimizerFeedbackStatus EvaluateOptimizerRuntimeFeedback(const OptimizerRuntimeFeedback& feedback);
OptimizerCalibratedCostProfile BuildOptimizerCalibratedCostProfile(const OptimizerRuntimeFeedback& feedback,
                                                                    const OptimizerFeedbackStatus& status);

// QOW-SOURCE-OPT-013-V1
// This ABI admits only a caller-supplied, already frozen engine observation.
// It does not observe execution itself and it cannot authorize or mutate a
// plan, result, actual, cache entry, transaction, or execution outcome.
enum class OptimizerGovernedFeedbackMetricState : std::uint8_t {
  kUnavailable = 0,
  kObserved = 1,
  kNotApplicable = 2,
};

struct OptimizerGovernedFeedbackMetric {
  OptimizerGovernedFeedbackMetricState state{
      OptimizerGovernedFeedbackMetricState::kUnavailable};
  std::uint64_t value{0};

  bool operator==(const OptimizerGovernedFeedbackMetric&) const = default;
};

enum class OptimizerGovernedFeedbackOutcome : std::uint8_t {
  kSucceeded = 1,
  kFailed,
  kCrashed,
  kCancelled,
  kRolledBack,
  kUnauthorized,
  kRedacted,
  kCorrupt,
  kPoisoned,
  kPolicyQuarantined,
};

enum class OptimizerGovernedFeedbackDisposition : std::uint8_t {
  kIgnored = 1,
  kPending,
  kAdmitted,
  kDiagnosticOnly,
  kInvalidated,
  kQuarantined,
};

struct OptimizerGovernedFeedbackIdentity {
  std::uint16_t abi_version{1};
  std::string observation_uuid;
  std::string selected_plan_uuid;
  std::string selected_plan_signature;
  std::uint64_t physical_node_id{0};
  std::uint32_t logical_node_id{0};
  std::uint64_t causal_counter_id{0};
  std::string implementation_id;
  std::string selected_alternative_uuid;
  std::string output_descriptor_digest;
  std::string result_identity_digest;
  std::string dependency_signature;
};

struct OptimizerGovernedFeedbackGenerations {
  std::uint64_t catalog{0};
  std::uint64_t security{0};
  std::uint64_t policy{0};
  std::uint64_t statistics{0};
  std::uint64_t capability{0};
  std::uint64_t route{0};
  std::uint64_t resource{0};
  std::uint64_t feedback{0};

  bool operator==(const OptimizerGovernedFeedbackGenerations&) const = default;
};

struct OptimizerGovernedFeedbackAuthorityReceipt {
  bool engine_execution_observation{false};
  bool producer_receipt_complete{false};
  bool counters_frozen_after_finish{false};
  bool estimates_frozen_before_access{false};
  bool plan_identity_verified{false};
  bool node_causal_identity_verified{false};
  bool implementation_alternative_identity_verified{false};
  bool descriptor_identity_verified{false};
  bool result_identity_verified{false};
  bool dependency_identity_verified{false};
  bool mga_statement_context_verified{false};
  bool security_recheck_preserved{false};
  bool donor_semantics_preserved{false};
  bool owns_execution{false};
  bool owns_mga_visibility{false};
  bool owns_transaction_finality{false};
  bool owns_security{false};
  bool owns_parser{false};
  bool owns_reference{false};
  bool owns_donor{false};
  bool owns_recovery{false};
  bool owns_wal{false};
  bool owns_benchmark{false};
  bool owns_release{false};
  bool owns_cluster{false};
};

struct OptimizerGovernedFeedbackMetrics {
  OptimizerGovernedFeedbackMetric estimated_input_rows;
  OptimizerGovernedFeedbackMetric estimated_output_rows;
  OptimizerGovernedFeedbackMetric actual_input_rows;
  OptimizerGovernedFeedbackMetric actual_output_rows;
  OptimizerGovernedFeedbackMetric actual_rows_examined;
  OptimizerGovernedFeedbackMetric elapsed_ns;
  OptimizerGovernedFeedbackMetric operator_wait_ns;
  OptimizerGovernedFeedbackMetric current_memory_bytes;
  OptimizerGovernedFeedbackMetric peak_memory_bytes;
  OptimizerGovernedFeedbackMetric estimated_pages;
  OptimizerGovernedFeedbackMetric decoded_bytes;
  OptimizerGovernedFeedbackMetric bytes_read;
  OptimizerGovernedFeedbackMetric bytes_written;
  OptimizerGovernedFeedbackMetric pages_read;
  OptimizerGovernedFeedbackMetric pages_written;
  OptimizerGovernedFeedbackMetric spill_bytes_read;
  OptimizerGovernedFeedbackMetric spill_bytes_written;
  OptimizerGovernedFeedbackMetric visibility_rechecks;
  OptimizerGovernedFeedbackMetric security_rechecks;
  OptimizerGovernedFeedbackMetric storage_rechecks;
  OptimizerGovernedFeedbackMetric index_rechecks;
  OptimizerGovernedFeedbackMetric residual_rechecks;
  OptimizerGovernedFeedbackMetric compatibility_rechecks;
  OptimizerGovernedFeedbackMetric archive_bytes_read;
  OptimizerGovernedFeedbackMetric cluster_bytes_sent;
  OptimizerGovernedFeedbackMetric cluster_bytes_received;
  OptimizerGovernedFeedbackMetric estimated_resource_units;
  OptimizerGovernedFeedbackMetric actual_resource_units;
};

struct OptimizerGovernedFeedbackObservation {
  OptimizerGovernedFeedbackIdentity identity;
  OptimizerGovernedFeedbackGenerations generations;
  OptimizerGovernedFeedbackAuthorityReceipt authority;
  OptimizerGovernedFeedbackMetrics metrics;
  OptimizerGovernedFeedbackOutcome outcome{
      OptimizerGovernedFeedbackOutcome::kFailed};
  std::uint64_t observation_sequence{0};
  bool policy_allowed{false};
  bool quality_improvement_observed{false};
  bool regression_observed{false};
  bool correctness_constraint_conflict{false};
};

struct OptimizerGovernedFeedbackPolicy {
  std::uint32_t deviation_threshold_basis_points{1000};
  std::uint32_t required_compatible_samples{2};
  std::uint32_t maximum_samples_per_identity{8};
  std::uint32_t maximum_retained_identities{64};
  std::uint32_t maximum_quarantine_records{64};
  std::uint32_t minimum_multiplier_basis_points{2500};
  std::uint32_t maximum_multiplier_basis_points{40000};
  std::uint64_t maximum_advisory_rows{
      std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t maximum_advisory_memory_bytes{
      std::numeric_limits<std::uint64_t>::max()};

  bool operator==(const OptimizerGovernedFeedbackPolicy&) const = default;
};

struct OptimizerGovernedFeedbackPublicationResult {
  bool accepted{false};
  OptimizerGovernedFeedbackDisposition disposition{
      OptimizerGovernedFeedbackDisposition::kQuarantined};
  std::string diagnostic_code;
  std::string identity_key;
  std::uint64_t feedback_generation{0};
  std::uint32_t compatible_sample_count{0};
  std::uint32_t cardinality_multiplier_basis_points{10000};
  std::uint64_t advisory_output_rows{0};
  std::uint64_t advisory_memory_bytes{0};
  std::vector<std::string> evidence;
};

struct OptimizerGovernedFeedbackAlternativeProof {
  std::string candidate_alternative_uuid;
  std::string exact_fallback_alternative_uuid;
  bool identical_logical_semantics{false};
  bool identical_output_descriptors{false};
  bool identical_required_properties{false};
  bool identical_delivered_properties{false};
  bool identical_security_behavior{false};
  bool identical_mga_behavior{false};
  bool identical_donor_compatibility{false};
  bool compatible_capability{false};
  bool exact_fallback_available{false};
};

struct OptimizerGovernedFeedbackConsumptionRequest {
  std::uint16_t abi_version{1};
  OptimizerGovernedFeedbackIdentity source_identity;
  OptimizerGovernedFeedbackGenerations current_generations;
  OptimizerGovernedFeedbackAlternativeProof alternative;
  std::uint64_t planning_attempt_sequence{0};
  bool later_planning_attempt{false};
  bool policy_allowed{false};
  bool mutate_executed_plan_requested{false};
  bool mutate_runtime_actual_requested{false};
  bool mutate_result_requested{false};
  bool mutate_transaction_requested{false};
  bool force_execution_requested{false};
  bool retry_requested{false};
  bool reprepare_requested{false};
  bool statistics_refresh_requested{false};
  bool operator_replacement_requested{false};
  bool feedback_authority_claimed{false};
};

struct OptimizerGovernedFeedbackConsumptionResult {
  bool consumed{false};
  bool conservative_fallback_required{true};
  std::string diagnostic_code;
  std::uint64_t feedback_generation{0};
  std::uint32_t cardinality_multiplier_basis_points{10000};
  std::uint64_t advisory_output_rows{0};
  std::uint64_t advisory_memory_bytes{0};
  std::vector<std::string> evidence;
};

struct OptimizerGovernedFeedbackInvalidation {
  std::string selected_plan_uuid;
  OptimizerGovernedFeedbackGenerations current_generations;
  std::string reason;
};

struct OptimizerGovernedFeedbackSample {
  std::string observation_uuid;
  std::uint64_t observation_sequence{0};
  std::uint64_t actual_output_rows{0};
  std::uint64_t peak_memory_bytes{0};
};

struct OptimizerGovernedFeedbackRecord {
  OptimizerGovernedFeedbackIdentity identity;
  OptimizerGovernedFeedbackGenerations generations;
  OptimizerGovernedFeedbackDisposition disposition{
      OptimizerGovernedFeedbackDisposition::kPending};
  std::uint64_t first_observation_sequence{0};
  std::uint64_t last_observation_sequence{0};
  std::uint32_t compatible_sample_count{0};
  std::uint32_t direction{0};
  std::uint64_t frozen_estimated_output_rows{0};
  std::uint64_t accumulated_actual_output_rows{0};
  std::uint64_t accumulated_peak_memory_bytes{0};
  std::uint32_t cardinality_multiplier_basis_points{10000};
  std::uint64_t advisory_output_rows{0};
  std::uint64_t advisory_memory_bytes{0};
  bool valid{true};
  std::string invalidation_reason;
  std::vector<OptimizerGovernedFeedbackSample> retained_samples;
};

struct OptimizerGovernedFeedbackQuarantineRecord {
  std::string identity_key;
  std::uint64_t observation_sequence{0};
  std::string reason;
  OptimizerGovernedFeedbackDisposition disposition{
      OptimizerGovernedFeedbackDisposition::kQuarantined};
};

struct OptimizerGovernedFeedbackSnapshot {
  std::uint64_t retained_identity_count{0};
  std::uint64_t admitted_identity_count{0};
  std::uint64_t invalidated_identity_count{0};
  std::uint64_t quarantined_identity_count{0};
  std::uint64_t quarantine_record_count{0};
  std::vector<OptimizerGovernedFeedbackRecord> records;
  std::vector<OptimizerGovernedFeedbackQuarantineRecord> quarantine;
};

class OptimizerGovernedFeedbackStore {
 public:
  OptimizerGovernedFeedbackPublicationResult Publish(
      const OptimizerGovernedFeedbackObservation& observation,
      const OptimizerGovernedFeedbackPolicy& policy);
  OptimizerGovernedFeedbackConsumptionResult Consume(
      const OptimizerGovernedFeedbackConsumptionRequest& request) const;
  std::uint64_t Invalidate(
      const OptimizerGovernedFeedbackInvalidation& event);
  OptimizerGovernedFeedbackSnapshot Snapshot() const;

 private:
  mutable std::mutex governed_mutex_;
  std::optional<OptimizerGovernedFeedbackPolicy> governed_policy_;
  std::vector<OptimizerGovernedFeedbackRecord> governed_records_;
  std::vector<OptimizerGovernedFeedbackQuarantineRecord>
      governed_quarantine_;
};

}  // namespace scratchbird::engine::optimizer
