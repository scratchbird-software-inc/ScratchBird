// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_candidate_legality.hpp"
#include "cost_model.hpp"
#include "logical_plan.hpp"
#include "../executor/physical_node_abi.hpp"
#ifndef SCRATCHBIRD_QOW_CANONICAL_CANDIDATE_LEGALITY_ONLY
#include "access_path.hpp"
#include "access_path_full.hpp"
#include "join_planner.hpp"
#include "optimizer_feedback.hpp"
#include "physical_plan.hpp"
#include "selectivity_model.hpp"
#include "statistics_catalog.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct CanonicalOptimizerAdmissionRequest;
struct CanonicalOptimizerAdmissionResult;

enum class CanonicalOptimizerStatisticState : std::uint8_t {
  kKnown = 1,
  kUnknown,
  kNotApplicable,
};

enum class CanonicalOptimizerStatisticSource : std::uint8_t {
  kCatalogExact = 1,
  kCatalogSample,
  kUnavailable,
};

struct CanonicalOptimizerNodeEstimate {
  std::uint32_t logical_node_id{0};
  std::string object_uuid;
  CanonicalOptimizerStatisticState state{
      CanonicalOptimizerStatisticState::kUnknown};
  CanonicalOptimizerStatisticSource source{
      CanonicalOptimizerStatisticSource::kUnavailable};
  std::string catalog_epoch_uuid;
  std::string statistics_snapshot_uuid;
  std::uint64_t statistics_generation{0};
  std::uint64_t collected_at_monotonic_ns{0};
  std::uint64_t admitted_at_monotonic_ns{0};
  std::uint64_t maximum_age_ns{0};
  CostConfidence confidence{CostConfidence::kUnknown};
  bool row_count_present{false};
  std::uint64_t row_count{0};
  bool page_count_present{false};
  std::uint64_t page_count{0};
  bool derived_from_runtime_actuals{false};
  bool benchmark_clean_authority_claimed{false};
};

struct CanonicalOptimizerStatisticsSnapshot {
  std::uint16_t abi_version{1};
  std::string statistics_snapshot_uuid;
  std::string catalog_epoch_uuid;
  std::uint64_t statistics_generation{0};
  std::uint64_t admitted_at_monotonic_ns{0};
  std::vector<CanonicalOptimizerNodeEstimate> node_estimates;
  bool captured_before_data_access{false};
  bool data_access_observed{false};
  bool runtime_actuals_present{false};
  bool parser_statistics_authority_claimed{false};
};

struct CanonicalOptimizerStatisticsIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string field_id;
};

struct CanonicalOptimizerStatisticsAdmissionResult {
  bool accepted{false};
  bool degraded_for_unknown_statistics{false};
  bool benchmark_clean_ready{false};
  bool data_access_allowed{false};
  std::size_t known_estimate_count{0};
  std::size_t unknown_estimate_count{0};
  std::size_t not_applicable_estimate_count{0};
  std::vector<CanonicalOptimizerStatisticsIssue> issues;
};

// SEARCH_KEY: QOW-SOURCE-OPT-005-V1
CanonicalOptimizerStatisticsAdmissionResult
AdmitCanonicalOptimizerStatisticsBeforeAccess(
    const scratchbird::engine::planner::CanonicalLogicalRelationalGraph& graph,
    const CanonicalOptimizerStatisticsSnapshot& snapshot);

struct CanonicalOptimizerCostTerms {
  std::string cost_vector_uuid;
  std::string calibration_profile_uuid;
  std::uint64_t cpu_units{0};
  std::uint64_t page_read_sequential_units{0};
  std::uint64_t page_read_random_units{0};
  std::uint64_t page_write_units{0};
  std::uint64_t memory_bytes_required{0};
  std::uint64_t spill_bytes_expected{0};
  std::uint64_t network_bytes_expected{0};
  std::uint64_t mga_visibility_checks_expected{0};
  std::uint64_t archive_fetches_expected{0};
  std::uint64_t uncertainty_penalty{0};
  std::uint64_t risk_penalty{0};
  CostConfidence confidence{CostConfidence::kUnknown};
};

struct CanonicalOptimizerSearchCandidateInput {
  std::string alternative_uuid;
  std::string transformation_uuid;
  std::string transformation_rule_id;
  std::string bound_sblr_tree_uuid;
  std::string statistics_snapshot_uuid;
  std::uint64_t statistics_generation{0};
  std::string model_family_id;
  CanonicalOptimizerCostTerms cost_terms;
  bool semantic_preserving{false};
  bool derived_from_admitted_statistics{false};
  bool engine_coster_owned{false};
  bool parser_or_reference_cost_authority_claimed{false};
  bool benchmark_authority_claimed{false};
};

struct CanonicalOptimizerSearchPolicy {
  std::uint16_t abi_version{1};
  std::uint64_t maximum_exhaustive_plan_count{0};
  std::uint64_t bounded_beam_width{0};
  std::uint64_t deterministic_step_cost_ns{0};
  bool engine_owned{false};
  bool allow_cross_model_cost_comparison{false};
  bool parser_search_authority_claimed{false};
  bool transaction_finality_claimed{false};
};

enum class CanonicalOptimizerSearchMode : std::uint8_t {
  kExhaustiveSmall = 1,
  kDeterministicBounded,
};

struct CanonicalOptimizerRankedCostVector {
  CanonicalOptimizerCostTerms terms;
  std::uint64_t scalar_score{0};
};

struct CanonicalOptimizerMemoCandidate {
  std::string alternative_uuid;
  std::string transformation_uuid;
  std::string transformation_rule_id;
  CanonicalOptimizerRankedCostVector cost;
};

struct CanonicalOptimizerMemoGroup {
  std::uint32_t logical_node_id{0};
  std::vector<CanonicalOptimizerMemoCandidate> candidates;
};

struct CanonicalOptimizerSearchTraceRecord {
  std::uint64_t step_ordinal{0};
  std::string event_id;
  std::uint32_t logical_node_id{0};
  std::uint64_t frontier_size{0};
  std::uint64_t pruned_count{0};
  std::string detail;
};

struct CanonicalOptimizerSelectedAlternative {
  std::uint32_t logical_node_id{0};
  std::string alternative_uuid;
  std::string transformation_uuid;
  std::string transformation_rule_id;
  CanonicalOptimizerRankedCostVector cost;
};

struct CanonicalOptimizerSearchIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string alternative_uuid;
  std::string field_id;
};

struct CanonicalOptimizerSearchResult {
  bool accepted{false};
  bool selected{false};
  bool exhaustive_oracle_executed{false};
  bool exhaustive_oracle_agreed{false};
  bool resource_bounded{false};
  bool deterministic{false};
  bool physical_dag_published{false};
  bool data_access_allowed{false};
  CanonicalOptimizerSearchMode mode{
      CanonicalOptimizerSearchMode::kExhaustiveSmall};
  std::uint64_t memo_group_count{0};
  std::uint64_t legal_candidate_count{0};
  std::uint64_t complete_plan_space_count{0};
  bool complete_plan_space_count_saturated{false};
  std::uint64_t search_step_count{0};
  std::uint64_t pruned_plan_count{0};
  std::uint64_t selected_scalar_score{0};
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  scratchbird::engine::planner::CanonicalMgaStatementContext
      mga_statement_context;
  std::string statistics_snapshot_uuid;
  std::uint64_t statistics_generation{0};
  std::string model_family_id;
  std::string calibration_profile_uuid;
  std::string selected_plan_signature;
  std::vector<CanonicalOptimizerMemoGroup> memo_groups;
  std::vector<CanonicalOptimizerSelectedAlternative> selected_alternatives;
  std::vector<CanonicalOptimizerSearchTraceRecord> trace;
  std::vector<CanonicalOptimizerSearchIssue> issues;
};

// QOW-SOURCE-OPT-014-V1
CanonicalOptimizerSearchResult SearchCanonicalRelationalMemo(
    const CanonicalOptimizerAdmissionRequest& admission_request,
    const CanonicalOptimizerAdmissionResult& admission,
    const scratchbird::engine::planner::CanonicalPhysicalAlternativeCatalog&
        alternatives,
    const std::vector<CanonicalOptimizerSearchCandidateInput>& candidates,
    const CanonicalOptimizerSearchPolicy& policy);

struct CanonicalExecutorCapabilityRecord {
  std::string capability_uuid;
  std::uint32_t capability_abi_version{0};
  std::string implementation_id;
  scratchbird::engine::planner::CanonicalLogicalRelationalNodeKind
      logical_node_kind{
          scratchbird::engine::planner::CanonicalLogicalRelationalNodeKind::
              kValues};
  scratchbird::engine::executor::PhysicalNodeKind physical_node_kind{
      scratchbird::engine::executor::PhysicalNodeKind::kValues};
  std::size_t minimum_input_count{0};
  std::size_t maximum_input_count{0};
  std::vector<scratchbird::engine::planner::CanonicalLogicalPropertyKind>
      supported_property_kinds;
  std::uint64_t maximum_memory_bytes{0};
  bool spill_supported{false};
  bool storage_read_capable{false};
  bool mga_visibility_capable{false};
  bool available{false};
  std::string refusal_diagnostic_id;
  bool engine_owned{false};
  bool cluster_capability_claimed{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct CanonicalExecutorCapabilityCatalog {
  std::uint16_t abi_version{1};
  std::string capability_snapshot_uuid;
  std::uint64_t policy_epoch{0};
  std::vector<CanonicalExecutorCapabilityRecord> capabilities;
  bool engine_owned{false};
  bool cluster_catalog_claimed{false};
  bool parser_capability_authority_claimed{false};
};

struct CanonicalOptimizerPhysicalPublicationIdentity {
  std::string selected_plan_uuid;
  std::uint64_t first_causal_counter_id{0};
  bool engine_owned{false};
  bool data_access_observed{false};
  bool parser_publication_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct CanonicalOptimizerPhysicalPublicationIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string alternative_uuid;
  std::string capability_uuid;
  std::string field_id;
};

struct CanonicalOptimizerPhysicalPublicationResult {
  bool accepted{false};
  bool published{false};
  bool immutable_node_identity_validated{false};
  bool capability_validated_before_access{false};
  bool data_access_allowed{false};
  scratchbird::engine::executor::TypedPhysicalNodeDag physical_dag;
  std::vector<CanonicalOptimizerPhysicalPublicationIssue> issues;
};

// QOW-SOURCE-OPT-016-V1
CanonicalOptimizerPhysicalPublicationResult PublishCanonicalPhysicalDag(
    const CanonicalOptimizerAdmissionRequest& admission_request,
    const CanonicalOptimizerAdmissionResult& admission,
    const scratchbird::engine::planner::CanonicalPhysicalAlternativeCatalog&
        alternatives,
    const CanonicalOptimizerSearchResult& search,
    const CanonicalExecutorCapabilityCatalog& capability_catalog,
    const CanonicalOptimizerPhysicalPublicationIdentity& identity);

#ifndef SCRATCHBIRD_QOW_CANONICAL_CANDIDATE_LEGALITY_ONLY
// SEARCH_KEY: SB_OPTIMIZER_CONTRACT
struct OptimizerEvidence {
  bool has_usable_index = false;
  bool point_predicate = false;
  bool range_predicate = false;
  bool reorder_safe_join = false;
  std::uint64_t left_cardinality = 0;
  std::uint64_t right_cardinality = 0;
  bool grouping_present = false;
  bool ordered_input = false;
  std::string specialized_kind;
  bool exact_fallback_available = false;
};

struct OptimizerDecision {
  bool ok = false;
  scratchbird::engine::planner::PhysicalAccessKind access_kind = scratchbird::engine::planner::PhysicalAccessKind::kNone;
  std::string rule;
  std::string diagnostic_code;
  bool llvm_eligible = false;
  bool gpu_eligible = false;
};

struct OptimizerCandidate {
  scratchbird::engine::planner::LogicalPlanNode node;
  PlanCandidate plan_candidate;
  CostVector cost;
  bool selected = false;
  bool selected_in_physical_tree = false;
  bool rejected = false;
  std::string rejection_reason;
  std::string statistics_version;
};

struct OptimizedPlan {
  bool ok = false;
  std::string optimizer_profile = "deterministic_first_cost_v1";
  std::vector<OptimizerCandidate> candidates;
  bool has_physical_plan = false;
  PhysicalPlanNode physical_root;
  std::string selected_primary_candidate_id;
  std::string selected_primary_operation_id;
  std::vector<std::string> diagnostics;
};

OptimizedPlan OptimizeLogicalPlan(const scratchbird::engine::planner::LogicalPlan& plan);
OptimizedPlan OptimizeLogicalPlanWithStatistics(const scratchbird::engine::planner::LogicalPlan& plan,
                                                const OptimizerStatisticsCatalog& statistics);
OptimizedPlan OptimizeLogicalPlanWithAccessPathRequest(
    const scratchbird::engine::planner::LogicalPlan& plan,
    const AccessPathPlanningRequest& access_request);
StatisticsContractStatus ValidateBenchmarkCleanOptimizedPlan(const OptimizedPlan& plan);
std::string SerializeOptimizedPlanToJson(const OptimizedPlan& plan);
OptimizerDecision ChooseIndexAccess(const OptimizerEvidence& evidence);
OptimizerDecision ChooseJoinOrder(const OptimizerEvidence& evidence);
OptimizerDecision ChooseAggregateStrategy(const OptimizerEvidence& evidence);
OptimizerDecision ChooseSpecializedWorkloadAccess(const OptimizerEvidence& evidence);
#endif

}  // namespace scratchbird::engine::optimizer
