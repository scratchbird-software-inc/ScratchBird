// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../executor/descriptor_value_runtime.hpp"
#include "../executor/physical_node_abi.hpp"
#include "optimizer_request.hpp"
#include "optimizer_planning_context.hpp"
#include "result_cursor_plan_memory_governance.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::optimizer {

namespace memory = scratchbird::core::memory;

// QOW-SOURCE-OPT-010-PREPARE-V1
// PREPARE retains the complete immutable physical structure selected by the
// optimizer, but never the PREPARE statement's MGA context, parameter values,
// current-authority resolver, or any execution/finality authority. A later
// EXECUTE must bind a fresh engine-owned statement context and revalidate all
// identities; this artifact alone cannot read a row or dispatch a node.
enum class CanonicalPreparedPlanDependencyKind : std::uint8_t {
  kObject = 1,
  kFunction,
  kIndex,
  kFilespace,
  kDescriptor,
  kDatatype,
  kDomain,
  kCollation,
  kMetricSnapshot,
  kContinuationContext,
};

struct CanonicalPreparedPlanParameterDescriptor {
  std::uint32_t ordinal{0};
  std::uint32_t descriptor_id{0};
  std::string descriptor_uuid;
  std::string type_uuid;
  std::string domain_uuid;
  std::string collation_uuid;
  std::string timezone_uuid;
  std::string type_modifier_digest;
  std::string encoded_descriptor;
  bool nullable{false};

  bool operator==(
      const CanonicalPreparedPlanParameterDescriptor&) const = default;
};

struct CanonicalPreparedPlanResultDescriptor {
  std::uint32_t ordinal{0};
  std::uint32_t descriptor_id{0};
  std::string name_utf8;
  std::string descriptor_uuid;
  std::string type_uuid;
  std::string domain_uuid;
  std::string collation_uuid;
  std::string timezone_uuid;
  std::string type_modifier_digest;
  std::string encoded_descriptor;
  bool nullable{false};

  bool operator==(
      const CanonicalPreparedPlanResultDescriptor&) const = default;
};

struct CanonicalPreparedPlanDependency {
  CanonicalPreparedPlanDependencyKind dependency_kind{
      CanonicalPreparedPlanDependencyKind::kObject};
  std::string dependency_uuid;
  std::uint64_t generation{0};
  std::string definition_digest;

  bool operator==(const CanonicalPreparedPlanDependency&) const = default;
};

struct CanonicalPreparedPhysicalNode {
  std::uint64_t physical_node_id{0};
  std::uint32_t relational_node_id{0};
  executor::PhysicalNodeKind node_kind{executor::PhysicalNodeKind::kValues};
  std::string logical_semantic_variant_id;
  std::string implementation_id;
  std::vector<std::uint64_t> input_physical_node_ids;
  std::vector<std::uint32_t> output_descriptor_ids;
  bool shareable{false};
  std::uint64_t publication_ordinal{0};
  std::uint64_t causal_counter_id{0};
  std::string selected_alternative_uuid;
  std::string transformation_uuid;
  std::string transformation_rule_id;
  std::string executor_capability_uuid;
  std::uint32_t executor_capability_abi_version{0};
  std::string cost_vector_uuid;
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
  std::vector<std::string> enforced_property_uuids;
  executor::PhysicalCostVectorReceipt retained_cost;
  std::uint64_t memory_bytes_required{0};
  std::uint64_t spill_bytes_expected{0};
};

// QOW-SOURCE-OPT-017-V1
// EXPLAIN consumes this immutable planning receipt. It records what the
// optimizer actually considered and selected; rendering is never allowed to
// rerun search, costing, planning, execution, or MGA visibility work.
enum class CanonicalPreparedExplainCandidateDisposition : std::uint8_t {
  kSelected = 1,
  kRejected,
  kPruned,
};

enum class CanonicalPreparedExplainStatisticState : std::uint8_t {
  kUsed = 1,
  kMissing,
  kNotApplicable,
};

struct CanonicalPreparedExplainStageRecord {
  std::uint64_t ordinal{0};
  std::string stage_id;
  std::string outcome_id;
  std::string detail_id;
  bool protected_detail{false};

  bool operator==(const CanonicalPreparedExplainStageRecord&) const = default;
};

struct CanonicalPreparedExplainNodeEstimate {
  std::uint64_t physical_node_id{0};
  std::uint32_t logical_node_id{0};
  std::uint64_t estimated_input_rows{0};
  std::uint64_t estimated_output_rows{0};
  std::string confidence_id;

  bool operator==(const CanonicalPreparedExplainNodeEstimate&) const = default;
};

struct CanonicalPreparedExplainCandidateRecord {
  std::string candidate_family_id;
  std::string alternative_uuid;
  std::uint32_t logical_node_id{0};
  CanonicalPreparedExplainCandidateDisposition disposition{
      CanonicalPreparedExplainCandidateDisposition::kRejected};
  std::string reason_id;
  executor::PhysicalCostVectorReceipt retained_cost;
  std::uint64_t estimated_rows{0};
  std::string confidence_id;
  bool protected_detail{false};
};

struct CanonicalPreparedExplainBarrierRecord {
  std::uint32_t logical_node_id{0};
  std::string barrier_kind_id;
  std::string reason_id;
  bool protected_detail{false};

  bool operator==(const CanonicalPreparedExplainBarrierRecord&) const = default;
};

struct CanonicalPreparedExplainStatisticRecord {
  std::string statistic_uuid;
  CanonicalPreparedExplainStatisticState state{
      CanonicalPreparedExplainStatisticState::kMissing};
  std::string confidence_id;
  bool protected_detail{false};

  bool operator==(const CanonicalPreparedExplainStatisticRecord&) const =
      default;
};

struct CanonicalPreparedExplainAssumptionRecord {
  std::string category_id;
  std::string assumption_id;
  bool protected_detail{false};

  bool operator==(const CanonicalPreparedExplainAssumptionRecord&) const =
      default;
};

struct CanonicalPreparedExplainEvidence {
  std::uint16_t abi_version{1};
  std::string selected_plan_uuid;
  std::string selected_plan_signature;
  std::string bound_sblr_tree_uuid;
  std::string statistics_snapshot_uuid;
  std::uint64_t statistics_generation{0};
  std::string search_strategy_id;
  std::vector<CanonicalPreparedExplainStageRecord> stages;
  std::vector<CanonicalPreparedExplainNodeEstimate> node_estimates;
  std::vector<CanonicalPreparedExplainCandidateRecord> candidates;
  std::vector<CanonicalPreparedExplainBarrierRecord> barriers;
  std::vector<CanonicalPreparedExplainStatisticRecord> statistics;
  std::vector<CanonicalPreparedExplainAssumptionRecord> assumptions;
  bool complete{false};
  bool engine_planning_evidence{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
  bool feedback_authority_claimed{false};
  bool benchmark_authority_claimed{false};
};

// Immutable PREPARE-time evidence for the bounded model-family metric
// collector. Metrics are advisory planning inputs only: none of these records
// carries a statement snapshot, transaction handle, visibility decision, or
// execution/finality authority.
struct CanonicalPreparedMetricValue {
  std::string metric_id;
  std::string unit_id;
  std::uint64_t unsigned_value{0};
  std::string source_snapshot_uuid;
  std::uint64_t source_generation{0};

  bool operator==(const CanonicalPreparedMetricValue&) const = default;
};

struct CanonicalPreparedMetricCollectionReceipt {
  std::uint16_t abi_version{1};
  std::uint16_t stable_leg_ordinal{0};
  std::uint32_t dependency_wave{0};
  std::string leg_uuid;
  std::string family_id;
  std::vector<std::string> dependency_leg_uuids;
  std::vector<std::string> required_metric_ids;
  std::string metric_snapshot_uuid;
  std::uint64_t metric_snapshot_generation{0};
  std::uint64_t started_at_monotonic_ns{0};
  std::uint64_t completed_at_monotonic_ns{0};
  std::vector<CanonicalPreparedMetricValue> metrics;
  std::string collection_receipt_uuid;
  std::string dependency_definition_digest;
  bool collected{false};
  bool cancelled{false};
  bool timed_out{false};
  bool cleanup_complete{false};
  bool advisory_only{true};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalPreparedLegPlanReceipt {
  std::uint16_t abi_version{1};
  std::uint16_t stable_leg_ordinal{0};
  std::string leg_uuid;
  std::string family_id;
  std::vector<std::string> dependency_leg_uuids;
  std::string metric_collection_receipt_uuid;
  std::string selected_leg_plan_uuid;
  std::string selected_alternative_uuid;
  std::string family_local_cost_vector_uuid;
  std::vector<std::string> retained_alternative_uuids;
  std::uint64_t estimated_output_rows{0};
  std::string planning_receipt_uuid;
  bool planned{false};
  bool family_local_selection{true};
  bool cross_family_cost_comparison_performed{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalPreparedMetricCoordinatorReceipt {
  std::uint16_t abi_version{1};
  std::string coordinator_policy_uuid;
  std::uint64_t coordinator_policy_generation{0};
  std::string bound_sblr_tree_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::string cluster_scope_id;
  std::uint16_t metric_thread_budget{0};
  std::uint16_t maximum_observed_concurrency{0};
  std::uint64_t timeout_ns{0};
  std::string dependency_chain_receipt_uuid;
  bool all_workers_joined{false};
  bool transient_state_cleaned{false};
  bool dependency_chain_acyclic{false};
  bool no_cross_family_cost_comparison{true};
  bool execute_metric_recollection_forbidden{true};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalPreparedPhysicalPlan {
  std::uint16_t abi_version{1};
  std::string prepared_plan_uuid;
  std::uint64_t prepare_generation{0};
  std::string parameter_shape_uuid;
  std::string result_schema_uuid;
  std::string selected_plan_uuid;
  std::string selected_plan_signature;
  std::uint64_t selected_scalar_score{0};
  std::uint64_t root_physical_node_id{0};
  std::uint64_t published_node_count{0};
  std::uint64_t first_causal_counter_id{0};
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string capability_snapshot_uuid;
  std::string resource_snapshot_uuid;
  std::string statistics_snapshot_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t resource_epoch{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::uint64_t memory_budget_bytes{0};
  bool spill_allowed{false};
  std::vector<CanonicalPreparedPlanParameterDescriptor> parameters;
  std::vector<CanonicalPreparedPlanResultDescriptor> result_descriptors;
  std::vector<CanonicalPreparedPlanDependency> dependencies;
  std::vector<CanonicalPreparedPhysicalNode> nodes;
  std::optional<CanonicalPreparedExplainEvidence> explain_evidence;
  std::optional<CanonicalPreparedMetricCoordinatorReceipt>
      prepare_metric_coordinator_receipt;
  std::vector<CanonicalPreparedMetricCollectionReceipt>
      prepare_metric_collection_receipts;
  std::vector<CanonicalPreparedLegPlanReceipt> prepare_leg_plan_receipts;
  std::optional<CanonicalPlannerContinuationReceipt> continuation_receipt;
  bool immutable_physical_identity_retained{false};
  bool complete_cost_vectors_retained{false};
  bool prepare_metric_receipts_retained{false};
  bool continuation_context_retained{false};
  bool parameter_values_retained{false};
  bool prepare_statement_authority_retained{false};
  bool execution_authority_granted{false};
};

struct CanonicalPreparePhysicalPlanRequest {
  std::string prepared_plan_uuid;
  std::uint64_t prepare_generation{0};
  std::string parameter_shape_uuid;
  std::string result_schema_uuid;
  executor::TypedPhysicalNodeDag selected_physical_dag;
  std::vector<CanonicalPreparedPlanParameterDescriptor> parameters;
  std::vector<CanonicalPreparedPlanResultDescriptor> result_descriptors;
  std::vector<CanonicalPreparedPlanDependency> dependencies;
  std::optional<CanonicalPreparedExplainEvidence> explain_evidence;
  std::optional<CanonicalPreparedMetricCoordinatorReceipt>
      prepare_metric_coordinator_receipt;
  std::vector<CanonicalPreparedMetricCollectionReceipt>
      prepare_metric_collection_receipts;
  std::vector<CanonicalPreparedLegPlanReceipt> prepare_leg_plan_receipts;
  std::optional<CanonicalPlannerContinuationReceipt> continuation_receipt;
  bool engine_prepare_authorized{false};
  bool parameter_values_supplied{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalPreparePhysicalPlanIssue {
  std::string diagnostic_id;
  std::string field_id;
};

struct CanonicalPreparePhysicalPlanResult {
  bool accepted{false};
  bool prepared{false};
  bool persisted{false};
  bool immutable_physical_identity_retained{false};
  bool complete_parameter_typing_retained{false};
  bool complete_dependency_generations_retained{false};
  bool result_schema_retained{false};
  bool prepare_metric_receipts_retained{false};
  bool continuation_context_retained{false};
  bool parameter_values_retained{false};
  bool prepare_statement_authority_retained{false};
  bool execution_authority_granted{false};
  std::shared_ptr<const CanonicalPreparedPhysicalPlan> prepared_plan;
  std::vector<CanonicalPreparePhysicalPlanIssue> issues;
};

class CanonicalPreparedPlanStore {
 public:
  std::shared_ptr<const CanonicalPreparedPhysicalPlan> Find(
      const std::string& prepared_plan_uuid) const {
    std::lock_guard lock(mutex_);
    const auto found = plans_.find(prepared_plan_uuid);
    return found == plans_.end() ? nullptr : found->second;
  }

  std::size_t Size() const {
    std::lock_guard lock(mutex_);
    return plans_.size();
  }

 private:
  friend CanonicalPreparePhysicalPlanResult PrepareCanonicalPhysicalPlan(
      const CanonicalPreparePhysicalPlanRequest& request,
      CanonicalPreparedPlanStore* prepared_plan_store);

  bool PersistValidated(
      std::shared_ptr<const CanonicalPreparedPhysicalPlan> prepared_plan) {
    if (!prepared_plan) return false;
    std::lock_guard lock(mutex_);
    return plans_
        .emplace(prepared_plan->prepared_plan_uuid, std::move(prepared_plan))
        .second;
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<const CanonicalPreparedPhysicalPlan>>
      plans_;
};

inline CanonicalPreparePhysicalPlanResult PrepareCanonicalPhysicalPlan(
    const CanonicalPreparePhysicalPlanRequest& request,
    CanonicalPreparedPlanStore* prepared_plan_store) {
  CanonicalPreparePhysicalPlanResult result;
  const auto refuse = [&](std::string field_id) {
    result = {};
    result.issues.push_back(
        {"QOW-DIAG-OPT-010-PREPARE-REFUSAL-V1", std::move(field_id)});
    return result;
  };
  const auto canonical_uuid = [](const std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
      return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (index == 8 || index == 13 || index == 18 || index == 23) continue;
      const auto ch = static_cast<unsigned char>(value[index]);
      if (!std::isxdigit(ch) || std::isupper(ch)) return false;
    }
    return value != "00000000-0000-0000-0000-000000000000";
  };
  const auto optional_uuid = [&](const std::string& value) {
    return value.empty() || canonical_uuid(value);
  };
  const auto digest = [](const std::string_view value) {
    return value.size() == 64 &&
           std::ranges::all_of(value, [](const unsigned char ch) {
             return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
  };
  const auto known_dependency_kind = [](const auto kind) {
    return kind >= CanonicalPreparedPlanDependencyKind::kObject &&
           kind <= CanonicalPreparedPlanDependencyKind::kContinuationContext;
  };

  if (!request.engine_prepare_authorized || request.parameter_values_supplied ||
      request.parser_execution_authority_claimed ||
      request.transaction_visibility_authority_claimed ||
      request.transaction_finality_authority_claimed ||
      request.recovery_authority_claimed) {
    return refuse("prepare_authority_scope");
  }
  if (!canonical_uuid(request.prepared_plan_uuid) ||
      request.prepare_generation == 0 ||
      !canonical_uuid(request.parameter_shape_uuid) ||
      !canonical_uuid(request.result_schema_uuid)) {
    return refuse("prepared_plan_identity");
  }
  const auto dag_validation =
      executor::ValidateTypedPhysicalNodeDag(request.selected_physical_dag);
  const auto& dag = request.selected_physical_dag;
  if (!dag_validation.accepted || dag.abi_version != 2 ||
      dag.publication_contract_version != 1 || !dag.optimizer_published ||
      !dag.immutable_node_identity_validated ||
      !dag.capability_validated_before_access || dag.data_access_observed ||
      dag.parser_execution_authority_claimed ||
      dag.transaction_finality_authority_claimed ||
      dag.nodes.size() != dag.published_node_count ||
      !dag.complete_cost_vectors_retained ||
      !dag.descriptor_contract_validated || !dag.property_contract_validated ||
      !dag.dependency_contract_validated || !dag.resource_contract_validated ||
      !dag.mga_contract_validated || !dag.causal_identity_validated) {
    return refuse(dag_validation.accepted
                      ? "complete_immutable_physical_publication"
                      : dag_validation.issues.front().field_id);
  }

  if (request.explain_evidence.has_value()) {
    const auto& explain = *request.explain_evidence;
    if (explain.abi_version != 1 || !explain.complete ||
        !explain.engine_planning_evidence ||
        explain.parser_execution_authority_claimed ||
        explain.transaction_visibility_authority_claimed ||
        explain.transaction_finality_authority_claimed ||
        explain.recovery_authority_claimed ||
        explain.feedback_authority_claimed ||
        explain.benchmark_authority_claimed ||
        explain.selected_plan_uuid != dag.selected_plan_uuid ||
        explain.selected_plan_signature != dag.selected_plan_signature ||
        explain.bound_sblr_tree_uuid != dag.bound_sblr_tree_uuid ||
        explain.statistics_snapshot_uuid != dag.statistics_snapshot_uuid ||
        explain.statistics_generation != dag.statistics_generation ||
        explain.search_strategy_id.empty() || explain.stages.empty() ||
        explain.node_estimates.size() != dag.nodes.size() ||
        explain.candidates.empty()) {
      return refuse("explain_planning_evidence");
    }
    for (std::size_t index = 0; index < explain.stages.size(); ++index) {
      const auto& stage = explain.stages[index];
      if (stage.ordinal != index + 1 || stage.stage_id.empty() ||
          stage.outcome_id.empty()) {
        return refuse("explain_stage_order");
      }
    }
    std::uint64_t previous_physical_node_id = 0;
    std::unordered_set<std::uint32_t> estimated_logical_nodes;
    for (const auto& estimate : explain.node_estimates) {
      const auto physical = std::ranges::find_if(
          dag.nodes, [&](const auto& node) {
            return node.physical_node_id == estimate.physical_node_id;
          });
      if (estimate.physical_node_id <= previous_physical_node_id ||
          physical == dag.nodes.end() ||
          physical->relational_node_id != estimate.logical_node_id ||
          estimate.confidence_id.empty() ||
          !estimated_logical_nodes.insert(estimate.logical_node_id).second) {
        return refuse("explain_node_estimate_coverage");
      }
      previous_physical_node_id = estimate.physical_node_id;
    }
    std::unordered_set<std::uint32_t> selected_logical_nodes;
    const auto same_cost = [](const executor::PhysicalCostVectorReceipt& left,
                              const executor::PhysicalCostVectorReceipt& right) {
      return left.cost_vector_uuid == right.cost_vector_uuid &&
             left.calibration_profile_uuid == right.calibration_profile_uuid &&
             left.scalar_score == right.scalar_score &&
             left.cpu_units == right.cpu_units &&
             left.page_read_sequential_units ==
                 right.page_read_sequential_units &&
             left.page_read_random_units == right.page_read_random_units &&
             left.page_write_units == right.page_write_units &&
             left.memory_bytes_required == right.memory_bytes_required &&
             left.spill_bytes_expected == right.spill_bytes_expected &&
             left.network_bytes_expected == right.network_bytes_expected &&
             left.mga_visibility_checks_expected ==
                 right.mga_visibility_checks_expected &&
             left.archive_fetches_expected == right.archive_fetches_expected &&
             left.uncertainty_penalty == right.uncertainty_penalty &&
             left.risk_penalty == right.risk_penalty &&
             left.cache_units == right.cache_units &&
             left.memory_grant_units == right.memory_grant_units &&
             left.spill_units == right.spill_units &&
             left.network_units == right.network_units &&
             left.compression_units == right.compression_units &&
             left.encryption_units == right.encryption_units &&
             left.predicate_evaluation_units ==
                 right.predicate_evaluation_units &&
             left.vector_distance_units == right.vector_distance_units &&
             left.text_scoring_units == right.text_scoring_units &&
             left.spatial_evaluation_units ==
                 right.spatial_evaluation_units &&
             left.udr_invocation_units == right.udr_invocation_units &&
             left.mga_units == right.mga_units &&
             left.index_maintenance_units ==
                 right.index_maintenance_units &&
             left.confidence == right.confidence;
    };
    std::string previous_candidate_key;
    for (const auto& candidate : explain.candidates) {
      const auto disposition = candidate.disposition;
      const auto known_disposition =
          disposition == CanonicalPreparedExplainCandidateDisposition::kSelected ||
          disposition == CanonicalPreparedExplainCandidateDisposition::kRejected ||
          disposition == CanonicalPreparedExplainCandidateDisposition::kPruned;
      const auto key = std::to_string(candidate.logical_node_id) + ":" +
                       candidate.candidate_family_id + ":" +
                       candidate.alternative_uuid + ":" +
                       std::to_string(static_cast<std::uint8_t>(disposition));
      const auto candidate_node = std::ranges::find_if(
          dag.nodes, [&](const auto& node) {
            return node.relational_node_id == candidate.logical_node_id;
          });
      if (!known_disposition || candidate.logical_node_id == 0 ||
          candidate_node == dag.nodes.end() ||
          candidate.candidate_family_id.empty() ||
          !canonical_uuid(candidate.alternative_uuid) ||
          candidate.confidence_id.empty() ||
          (!previous_candidate_key.empty() && key <= previous_candidate_key) ||
          (disposition !=
               CanonicalPreparedExplainCandidateDisposition::kSelected &&
           candidate.reason_id.empty())) {
        return refuse("explain_candidate_evidence");
      }
      previous_candidate_key = key;
      if (disposition ==
          CanonicalPreparedExplainCandidateDisposition::kSelected) {
        const auto physical = std::ranges::find_if(
            dag.nodes, [&](const auto& node) {
              return node.relational_node_id == candidate.logical_node_id &&
                     node.selected_alternative_uuid ==
                         candidate.alternative_uuid;
            });
        const auto estimate = std::ranges::find_if(
            explain.node_estimates, [&](const auto& item) {
              return physical != dag.nodes.end() &&
                     item.physical_node_id == physical->physical_node_id &&
                     item.logical_node_id == candidate.logical_node_id;
            });
        if (physical == dag.nodes.end() ||
            estimate == explain.node_estimates.end() ||
            candidate.estimated_rows != estimate->estimated_output_rows ||
            candidate.confidence_id != estimate->confidence_id ||
            candidate.retained_cost.cost_vector_uuid !=
                physical->cost_vector_uuid ||
            !same_cost(candidate.retained_cost, physical->retained_cost) ||
            !selected_logical_nodes.insert(candidate.logical_node_id).second) {
          return refuse("explain_selected_candidate_coverage");
        }
      }
    }
    if (selected_logical_nodes.size() != dag.nodes.size()) {
      return refuse("explain_selected_candidate_coverage");
    }
    std::string previous_barrier_key;
    for (const auto& barrier : explain.barriers) {
      const auto key = std::to_string(barrier.logical_node_id) + ":" +
                       barrier.barrier_kind_id + ":" + barrier.reason_id;
      const auto barrier_node = std::ranges::find_if(
          dag.nodes, [&](const auto& node) {
            return node.relational_node_id == barrier.logical_node_id;
          });
      if (barrier.logical_node_id == 0 || barrier.barrier_kind_id.empty() ||
          barrier_node == dag.nodes.end() ||
          barrier.reason_id.empty() ||
          (!previous_barrier_key.empty() && key <= previous_barrier_key)) {
        return refuse("explain_barrier_evidence");
      }
      previous_barrier_key = key;
    }
    std::string previous_statistic_uuid;
    for (const auto& statistic : explain.statistics) {
      const auto known_state =
          statistic.state == CanonicalPreparedExplainStatisticState::kUsed ||
          statistic.state == CanonicalPreparedExplainStatisticState::kMissing ||
          statistic.state ==
              CanonicalPreparedExplainStatisticState::kNotApplicable;
      if (!canonical_uuid(statistic.statistic_uuid) || !known_state ||
          statistic.confidence_id.empty() ||
          (!previous_statistic_uuid.empty() &&
           statistic.statistic_uuid <= previous_statistic_uuid)) {
        return refuse("explain_statistic_evidence");
      }
      previous_statistic_uuid = statistic.statistic_uuid;
    }
    std::string previous_assumption_key;
    for (const auto& assumption : explain.assumptions) {
      const auto key = assumption.category_id + ":" + assumption.assumption_id;
      if (assumption.category_id.empty() || assumption.assumption_id.empty() ||
          (!previous_assumption_key.empty() && key <= previous_assumption_key)) {
        return refuse("explain_assumption_evidence");
      }
      previous_assumption_key = key;
    }
  }

  std::unordered_set<std::uint32_t> parameter_descriptor_ids;
  std::unordered_set<std::string> parameter_descriptor_uuids;
  for (std::size_t index = 0; index < request.parameters.size(); ++index) {
    const auto& parameter = request.parameters[index];
    if (parameter.ordinal != index + 1 || parameter.descriptor_id == 0 ||
        !parameter_descriptor_ids.insert(parameter.descriptor_id).second ||
        !canonical_uuid(parameter.descriptor_uuid) ||
        !parameter_descriptor_uuids.insert(parameter.descriptor_uuid).second ||
        !canonical_uuid(parameter.type_uuid) ||
        !optional_uuid(parameter.domain_uuid) ||
        !optional_uuid(parameter.collation_uuid) ||
        !optional_uuid(parameter.timezone_uuid) ||
        !digest(parameter.type_modifier_digest)) {
      return refuse("typed_parameter_descriptor");
    }
  }

  const auto root = std::ranges::find_if(
      dag.nodes, [&](const auto& node) {
        return node.physical_node_id == dag.root_physical_node_id;
      });
  if (root == dag.nodes.end() ||
      request.result_descriptors.size() !=
          root->output_descriptor_ids.size()) {
    return refuse("result_descriptor_coverage");
  }
  std::unordered_set<std::string> result_descriptor_uuids;
  for (std::size_t index = 0; index < request.result_descriptors.size();
       ++index) {
    const auto& descriptor = request.result_descriptors[index];
    if (descriptor.ordinal != index + 1 || descriptor.descriptor_id == 0 ||
        descriptor.descriptor_id != root->output_descriptor_ids[index] ||
        !canonical_uuid(descriptor.descriptor_uuid) ||
        !result_descriptor_uuids.insert(descriptor.descriptor_uuid).second ||
        !canonical_uuid(descriptor.type_uuid) ||
        !optional_uuid(descriptor.domain_uuid) ||
        !optional_uuid(descriptor.collation_uuid) ||
        !optional_uuid(descriptor.timezone_uuid) ||
        !digest(descriptor.type_modifier_digest)) {
      return refuse("typed_result_descriptor");
    }
  }

  if (request.dependencies.empty()) {
    return refuse("generation_qualified_dependencies");
  }
  std::uint8_t previous_dependency_kind{0};
  std::string previous_dependency_uuid;
  for (const auto& dependency : request.dependencies) {
    const auto kind =
        static_cast<std::uint8_t>(dependency.dependency_kind);
    if (!known_dependency_kind(dependency.dependency_kind) ||
        !canonical_uuid(dependency.dependency_uuid) ||
        dependency.generation == 0 ||
        !digest(dependency.definition_digest) ||
        (previous_dependency_kind != 0 &&
         (kind < previous_dependency_kind ||
          (kind == previous_dependency_kind &&
           dependency.dependency_uuid <= previous_dependency_uuid)))) {
      return refuse("generation_qualified_dependencies");
    }
    previous_dependency_kind = kind;
    previous_dependency_uuid = dependency.dependency_uuid;
  }

  std::optional<CanonicalPlannerContinuationReceipt>
      verified_continuation_receipt;
  if (request.continuation_receipt.has_value()) {
    verified_continuation_receipt = request.continuation_receipt;
    auto& continuation = *verified_continuation_receipt;
    const auto dependency_count = std::ranges::count_if(
        request.dependencies, [](const auto& dependency) {
          return dependency.dependency_kind ==
                 CanonicalPreparedPlanDependencyKind::kContinuationContext;
        });
    const auto dependency = std::ranges::find_if(
        request.dependencies, [&](const auto& item) {
          return item.dependency_kind ==
                     CanonicalPreparedPlanDependencyKind::kContinuationContext &&
                 item.dependency_uuid ==
                     continuation.context.authority.context_uuid &&
                 item.generation ==
                     continuation.context.authority.generation &&
                 item.definition_digest ==
                     continuation.context.authority.dependency_signature;
        });
    if (!CanonicalPlannerContinuationReceiptValid(continuation) ||
        continuation.context.result_schema_uuid != request.result_schema_uuid ||
        continuation.bound_sblr_tree_uuid != dag.bound_sblr_tree_uuid ||
        dependency_count != 1 || dependency == request.dependencies.end() ||
        !ValidateCanonicalContinuationPhysicalRoot(&continuation, dag)) {
      return refuse("continuation_resumable_plan_receipt");
    }
  } else if (std::ranges::any_of(
                 request.dependencies, [](const auto& dependency) {
                   return dependency.dependency_kind ==
                          CanonicalPreparedPlanDependencyKind::
                              kContinuationContext;
                 })) {
    return refuse("orphan_continuation_dependency");
  }

  const bool has_prepare_metric_evidence =
      request.prepare_metric_coordinator_receipt.has_value() ||
      !request.prepare_metric_collection_receipts.empty() ||
      !request.prepare_leg_plan_receipts.empty();
  if (has_prepare_metric_evidence) {
    if (!request.prepare_metric_coordinator_receipt.has_value() ||
        request.prepare_metric_collection_receipts.empty() ||
        request.prepare_metric_collection_receipts.size() !=
            request.prepare_leg_plan_receipts.size() ||
        request.prepare_metric_collection_receipts.size() > 8) {
      return refuse("prepare_metric_receipt_coverage");
    }
    const auto& coordinator =
        *request.prepare_metric_coordinator_receipt;
    if (coordinator.abi_version != 1 ||
        !canonical_uuid(coordinator.coordinator_policy_uuid) ||
        coordinator.coordinator_policy_generation == 0 ||
        coordinator.bound_sblr_tree_uuid != dag.bound_sblr_tree_uuid ||
        coordinator.route_snapshot_uuid != dag.route_snapshot_uuid ||
        coordinator.route_epoch != dag.route_epoch ||
        coordinator.route_generation != dag.route_generation ||
        coordinator.cluster_scope_id.empty() ||
        coordinator.metric_thread_budget == 0 ||
        coordinator.metric_thread_budget > 64 ||
        coordinator.maximum_observed_concurrency == 0 ||
        coordinator.maximum_observed_concurrency >
            coordinator.metric_thread_budget ||
        coordinator.maximum_observed_concurrency >
            request.prepare_metric_collection_receipts.size() ||
        coordinator.timeout_ns == 0 ||
        !canonical_uuid(coordinator.dependency_chain_receipt_uuid) ||
        !coordinator.all_workers_joined ||
        !coordinator.transient_state_cleaned ||
        !coordinator.dependency_chain_acyclic ||
        !coordinator.no_cross_family_cost_comparison ||
        !coordinator.execute_metric_recollection_forbidden ||
        coordinator.parser_execution_authority_claimed ||
        coordinator.transaction_visibility_authority_claimed ||
        coordinator.transaction_finality_authority_claimed ||
        coordinator.recovery_authority_claimed) {
      return refuse("prepare_metric_coordinator_receipt");
    }

    static constexpr std::array<std::string_view, 8> kKnownFamilies = {
        "relational", "document", "graph", "time_series",
        "text_search", "vector", "spatial", "key_value"};
    const auto required_metric_ids = [](const std::string_view family_id) {
      if (family_id == "relational") {
        return std::vector<std::string>{
            "available_memory_bytes", "filespace_available_bytes",
            "index_coverage_basis_points", "page_density_basis_points",
            "row_count", "version_chain_depth"};
      }
      if (family_id == "document") {
        return std::vector<std::string>{
            "average_document_bytes", "document_count",
            "document_index_coverage_basis_points"};
      }
      if (family_id == "graph") {
        return std::vector<std::string>{
            "adjacency_edge_count", "connectivity_density_basis_points",
            "vertex_count"};
      }
      if (family_id == "time_series") {
        return std::vector<std::string>{"retention_horizon_ns",
                                        "sample_count", "series_count"};
      }
      if (family_id == "text_search") {
        return std::vector<std::string>{"document_frequency_count",
                                        "search_index_generation",
                                        "term_count"};
      }
      if (family_id == "vector") {
        return std::vector<std::string>{
            "approximate_neighbor_count", "precision_basis_points",
            "recall_basis_points", "vector_index_state"};
      }
      if (family_id == "spatial") {
        return std::vector<std::string>{
            "geometry_count", "spatial_index_coverage_basis_points",
            "spatial_partition_count"};
      }
      if (family_id == "key_value") {
        return std::vector<std::string>{"key_count", "tombstone_count",
                                        "value_bytes"};
      }
      return std::vector<std::string>{};
    };
    std::unordered_set<std::string> completed_leg_uuids;
    std::unordered_set<std::string> family_ids;
    std::unordered_set<std::string> metric_snapshot_uuids;
    std::map<std::string, std::uint32_t> completed_dependency_waves;
    for (std::size_t index = 0;
         index < request.prepare_metric_collection_receipts.size(); ++index) {
      const auto& metric = request.prepare_metric_collection_receipts[index];
      const auto& leg = request.prepare_leg_plan_receipts[index];
      if (metric.abi_version != 1 || leg.abi_version != 1 ||
          metric.stable_leg_ordinal != index + 1 ||
          leg.stable_leg_ordinal != metric.stable_leg_ordinal ||
          metric.leg_uuid != leg.leg_uuid ||
          metric.family_id != leg.family_id ||
          metric.dependency_leg_uuids != leg.dependency_leg_uuids ||
          !canonical_uuid(metric.leg_uuid) ||
          !std::ranges::contains(kKnownFamilies,
                                std::string_view(metric.family_id)) ||
          !family_ids.insert(metric.family_id).second ||
          metric.required_metric_ids != required_metric_ids(metric.family_id) ||
          metric.required_metric_ids.empty() || metric.metrics.empty() ||
          !canonical_uuid(metric.metric_snapshot_uuid) ||
          !metric_snapshot_uuids.insert(metric.metric_snapshot_uuid).second ||
          metric.metric_snapshot_generation == 0 ||
          metric.started_at_monotonic_ns == 0 ||
          metric.completed_at_monotonic_ns <
              metric.started_at_monotonic_ns ||
          !canonical_uuid(metric.collection_receipt_uuid) ||
          !digest(metric.dependency_definition_digest) ||
          !metric.collected || metric.cancelled || metric.timed_out ||
          !metric.cleanup_complete || !metric.advisory_only ||
          metric.parser_execution_authority_claimed ||
          metric.transaction_visibility_authority_claimed ||
          metric.transaction_finality_authority_claimed ||
          metric.recovery_authority_claimed ||
          leg.metric_collection_receipt_uuid !=
              metric.collection_receipt_uuid ||
          !canonical_uuid(leg.selected_leg_plan_uuid) ||
          !canonical_uuid(leg.selected_alternative_uuid) ||
          !canonical_uuid(leg.family_local_cost_vector_uuid) ||
          leg.retained_alternative_uuids.empty() ||
          !std::ranges::all_of(leg.retained_alternative_uuids,
                               [&](const auto& alternative_uuid) {
                                 return canonical_uuid(alternative_uuid);
                               }) ||
          !std::ranges::contains(leg.retained_alternative_uuids,
                                leg.selected_alternative_uuid) ||
          !canonical_uuid(leg.planning_receipt_uuid) || !leg.planned ||
          !leg.family_local_selection ||
          leg.cross_family_cost_comparison_performed ||
          leg.parser_execution_authority_claimed ||
          leg.transaction_visibility_authority_claimed ||
          leg.transaction_finality_authority_claimed ||
          leg.recovery_authority_claimed) {
        return refuse("prepare_metric_leg_receipt");
      }
      if (!std::ranges::is_sorted(metric.dependency_leg_uuids) ||
          std::ranges::adjacent_find(metric.dependency_leg_uuids) !=
              metric.dependency_leg_uuids.end() ||
          std::ranges::any_of(metric.dependency_leg_uuids,
                              [&](const auto& dependency_uuid) {
                                const auto dependency_wave =
                                    completed_dependency_waves.find(
                                        dependency_uuid);
                                return !completed_leg_uuids.contains(
                                           dependency_uuid) ||
                                       dependency_wave ==
                                           completed_dependency_waves.end() ||
                                       dependency_wave->second >=
                                           metric.dependency_wave;
                              }) ||
          !std::ranges::is_sorted(metric.required_metric_ids) ||
          std::ranges::adjacent_find(metric.required_metric_ids) !=
              metric.required_metric_ids.end() ||
          !std::ranges::is_sorted(
              metric.metrics, {}, &CanonicalPreparedMetricValue::metric_id) ||
          std::ranges::adjacent_find(
              metric.metrics, {},
              &CanonicalPreparedMetricValue::metric_id) !=
              metric.metrics.end() ||
          !std::ranges::is_sorted(leg.retained_alternative_uuids) ||
          std::ranges::adjacent_find(leg.retained_alternative_uuids) !=
              leg.retained_alternative_uuids.end()) {
        return refuse("prepare_metric_leg_order");
      }
      for (const auto& required : metric.required_metric_ids) {
        if (required.empty() ||
            std::ranges::none_of(metric.metrics, [&](const auto& value) {
              return value.metric_id == required;
            })) {
          return refuse("prepare_metric_required_coverage");
        }
      }
      for (const auto& value : metric.metrics) {
        if (value.metric_id.empty() || value.unit_id.empty() ||
            !canonical_uuid(value.source_snapshot_uuid) ||
            value.source_generation == 0) {
          return refuse("prepare_metric_value");
        }
      }
      completed_leg_uuids.insert(metric.leg_uuid);
      completed_dependency_waves.emplace(metric.leg_uuid,
                                         metric.dependency_wave);
    }
    const auto metric_dependencies =
        std::ranges::count_if(request.dependencies, [](const auto& dependency) {
          return dependency.dependency_kind ==
                 CanonicalPreparedPlanDependencyKind::kMetricSnapshot;
        });
    if (metric_dependencies !=
        request.prepare_metric_collection_receipts.size()) {
      return refuse("prepare_metric_dependency_signature");
    }
    for (const auto& metric : request.prepare_metric_collection_receipts) {
      const auto dependency = std::ranges::find_if(
          request.dependencies, [&](const auto& item) {
            return item.dependency_kind ==
                       CanonicalPreparedPlanDependencyKind::kMetricSnapshot &&
                   item.dependency_uuid == metric.metric_snapshot_uuid &&
                   item.generation == metric.metric_snapshot_generation &&
                   item.definition_digest ==
                       metric.dependency_definition_digest;
          });
      if (dependency == request.dependencies.end()) {
        return refuse("prepare_metric_dependency_signature");
      }
    }
  }

  auto prepared = std::make_shared<CanonicalPreparedPhysicalPlan>();
  prepared->prepared_plan_uuid = request.prepared_plan_uuid;
  prepared->prepare_generation = request.prepare_generation;
  prepared->parameter_shape_uuid = request.parameter_shape_uuid;
  prepared->result_schema_uuid = request.result_schema_uuid;
  prepared->selected_plan_uuid = dag.selected_plan_uuid;
  prepared->selected_plan_signature = dag.selected_plan_signature;
  prepared->selected_scalar_score = dag.selected_scalar_score;
  prepared->root_physical_node_id = dag.root_physical_node_id;
  prepared->published_node_count = dag.published_node_count;
  prepared->first_causal_counter_id = dag.first_causal_counter_id;
  prepared->bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  prepared->catalog_epoch_uuid = dag.catalog_epoch_uuid;
  prepared->security_context_uuid = dag.security_context_uuid;
  prepared->capability_snapshot_uuid = dag.capability_snapshot_uuid;
  prepared->resource_snapshot_uuid = dag.resource_snapshot_uuid;
  prepared->statistics_snapshot_uuid = dag.statistics_snapshot_uuid;
  prepared->route_snapshot_uuid = dag.route_snapshot_uuid;
  prepared->catalog_generation = dag.catalog_generation;
  prepared->security_epoch = dag.security_epoch;
  prepared->policy_epoch = dag.policy_epoch;
  prepared->resource_epoch = dag.resource_epoch;
  prepared->statistics_generation = dag.statistics_generation;
  prepared->route_epoch = dag.route_epoch;
  prepared->route_generation = dag.route_generation;
  prepared->memory_budget_bytes = dag.memory_budget_bytes;
  prepared->spill_allowed = dag.spill_allowed;
  prepared->parameters = request.parameters;
  prepared->result_descriptors = request.result_descriptors;
  prepared->dependencies = request.dependencies;
  prepared->explain_evidence = request.explain_evidence;
  prepared->prepare_metric_coordinator_receipt =
      request.prepare_metric_coordinator_receipt;
  prepared->prepare_metric_collection_receipts =
      request.prepare_metric_collection_receipts;
  prepared->prepare_leg_plan_receipts = request.prepare_leg_plan_receipts;
  prepared->continuation_receipt = verified_continuation_receipt;
  prepared->nodes.reserve(dag.nodes.size());
  for (const auto& node : dag.nodes) {
    prepared->nodes.push_back(
        {node.physical_node_id,
         node.relational_node_id,
         node.node_kind,
         node.logical_semantic_variant_id,
         node.implementation_id,
         node.input_physical_node_ids,
         node.output_descriptor_ids,
         node.shareable,
         node.publication_ordinal,
         node.causal_counter_id,
         node.selected_alternative_uuid,
         node.transformation_uuid,
         node.transformation_rule_id,
         node.executor_capability_uuid,
         node.executor_capability_abi_version,
         node.cost_vector_uuid,
         node.required_property_uuids,
         node.delivered_property_uuids,
         node.enforced_property_uuids,
         node.retained_cost,
         node.memory_bytes_required,
         node.spill_bytes_expected});
  }
  prepared->immutable_physical_identity_retained = true;
  prepared->complete_cost_vectors_retained = true;
  prepared->prepare_metric_receipts_retained = has_prepare_metric_evidence;
  prepared->continuation_context_retained =
      verified_continuation_receipt.has_value();
  prepared->parameter_values_retained = false;
  prepared->prepare_statement_authority_retained = false;
  prepared->execution_authority_granted = false;

  if (prepared_plan_store == nullptr ||
      !prepared_plan_store->PersistValidated(prepared)) {
    return refuse("prepared_plan_store");
  }

  result.accepted = true;
  result.prepared = true;
  result.persisted = true;
  result.immutable_physical_identity_retained = true;
  result.complete_parameter_typing_retained = true;
  result.complete_dependency_generations_retained = true;
  result.result_schema_retained = true;
  result.prepare_metric_receipts_retained = has_prepare_metric_evidence;
  result.continuation_context_retained =
      verified_continuation_receipt.has_value();
  result.parameter_values_retained = false;
  result.prepare_statement_authority_retained = false;
  result.execution_authority_granted = false;
  result.prepared_plan = std::move(prepared);
  return result;
}

// QOW-SOURCE-OPT-009-V1
// This is the executable cache authority. It is intentionally distinct from
// CachedOptimizerPlan below, which remains optimizer metadata only. A cache
// entry owns stable plan/key identity, never a PREPARE or EXECUTE statement
// snapshot, parameter value, current-authority resolver, or transaction
// finality capability.
enum class CanonicalExecutablePlanSnapshotClass : std::uint8_t {
  kReadCommitted = 1,
  kSnapshot,
  kSerializable,
  kHistorical,
  kClusterFinal,
  kBranchLocal,
  kDonorProfiled,
};

enum class CanonicalExecutablePlanStatus : std::uint8_t {
  kValid = 1,
  kRequiresRevalidation,
  kInvalid,
  kRetired,
  kBlocked,
};

struct CanonicalExecutablePlanGeneration {
  std::string identity_uuid;
  std::uint64_t generation{0};
  std::string definition_digest;

  bool operator==(const CanonicalExecutablePlanGeneration&) const = default;
};

struct CanonicalExecutablePlanCapabilityGeneration {
  std::string capability_uuid;
  std::uint32_t abi_version{0};
  std::uint64_t generation{0};
  std::string definition_digest;

  bool operator==(
      const CanonicalExecutablePlanCapabilityGeneration&) const = default;
};

struct CanonicalExecutablePlanCacheKey {
  std::string cache_plan_uuid;
  std::string compiled_at_uuidv7;
  CanonicalExecutablePlanStatus plan_status{
      CanonicalExecutablePlanStatus::kValid};
  std::string plan_key_digest;
  std::string database_uuid;
  std::uint64_t engine_format_generation{0};
  std::string sblr_unit_uuid;
  std::string internal_procedure_uuid;
  std::string bound_sblr_tree_uuid;
  std::string parser_compatibility_profile_uuid;
  std::uint64_t parser_compatibility_generation{0};
  std::string donor_compatibility_profile_uuid;
  std::uint64_t donor_compatibility_generation{0};
  std::string plan_policy_profile_uuid;
  std::uint64_t optimizer_configuration_generation{0};
  std::string bound_object_set_digest;
  std::string security_policy_digest;
  std::string redaction_policy_digest;
  std::string resource_policy_digest;
  std::uint64_t filespace_placement_generation{0};
  CanonicalExecutablePlanSnapshotClass snapshot_class{
      CanonicalExecutablePlanSnapshotClass::kReadCommitted};
  bool standalone_database{true};
  std::string cluster_uuid;
  std::uint64_t cluster_epoch{0};

  std::string prepared_plan_uuid;
  std::uint64_t prepare_generation{0};
  std::string parameter_shape_uuid;
  std::string result_schema_uuid;
  std::string selected_plan_uuid;
  std::string selected_plan_signature;
  std::uint64_t selected_scalar_score{0};
  std::uint64_t root_physical_node_id{0};
  std::uint64_t published_node_count{0};
  std::uint64_t first_causal_counter_id{0};

  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string capability_snapshot_uuid;
  std::string resource_snapshot_uuid;
  std::string statistics_snapshot_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t resource_epoch{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::uint64_t memory_budget_bytes{0};
  bool spill_allowed{false};
  std::optional<CanonicalPlannerContinuationReceipt> continuation_receipt;

  std::vector<CanonicalPreparedPlanParameterDescriptor> parameters;
  std::vector<CanonicalPreparedPlanResultDescriptor> result_descriptors;
  std::vector<CanonicalPreparedPlanDependency> physical_dependencies;
  std::vector<CanonicalExecutablePlanGeneration> object_generations;
  std::vector<CanonicalExecutablePlanGeneration> function_generations;
  std::vector<CanonicalExecutablePlanGeneration> metadata_generations;
  std::vector<CanonicalExecutablePlanGeneration> datatype_generations;
  std::vector<CanonicalExecutablePlanGeneration> collation_generations;
  std::vector<CanonicalExecutablePlanGeneration> statistics_generations;
  std::vector<CanonicalExecutablePlanGeneration> index_generations;
  std::vector<CanonicalExecutablePlanGeneration> filespace_generations;
  std::vector<CanonicalExecutablePlanGeneration> route_generations;
  std::vector<CanonicalExecutablePlanGeneration> metric_generations;
  std::vector<CanonicalExecutablePlanGeneration> continuation_generations;
  std::vector<CanonicalExecutablePlanCapabilityGeneration>
      capability_generations;

  bool operator==(const CanonicalExecutablePlanCacheKey&) const = default;
};

struct CanonicalExecutablePlanParameterBinding {
  CanonicalPreparedPlanParameterDescriptor descriptor;
  const scratchbird::engine::internal_api::EngineTypedValue* typed_value{
      nullptr};
};

struct CanonicalExecutablePlanCacheIssue {
  std::string diagnostic_id;
  std::string field_id;
  std::string upstream_diagnostic_id;
  std::string upstream_field_id;
  std::string stable_code;
  std::string severity;
  std::string sqlstate;
  std::string message_key;
  std::vector<std::string> argument_values;
  std::string phase;
  std::string record_path;
  std::optional<std::uint64_t> physical_node_id;
  std::string transaction_effect;
  std::string retryability;
};

inline CanonicalExecutablePlanCacheIssue CanonicalExecutablePlanIssue(
    std::string diagnostic_id, std::string field_id,
    std::string severity = "error", std::string phase = "execute",
    std::string retryability = "not_retryable") {
  CanonicalExecutablePlanCacheIssue issue;
  issue.diagnostic_id = std::move(diagnostic_id);
  issue.field_id = std::move(field_id);
  issue.stable_code = issue.diagnostic_id;
  issue.severity = std::move(severity);
  issue.message_key = issue.diagnostic_id;
  issue.phase = std::move(phase);
  issue.record_path = "prepared_plan_cache";
  issue.transaction_effect = "statement_failed_transaction_usable";
  issue.retryability = std::move(retryability);
  return issue;
}

struct CanonicalExecutablePlanCacheEntry {
  CanonicalExecutablePlanCacheKey key;
  std::shared_ptr<const CanonicalPreparedPhysicalPlan> prepared_plan;
  bool executable{false};
  bool metadata_only{true};
  bool parameter_values_retained{false};
  bool prepare_statement_authority_retained{false};
  bool execution_statement_authority_retained{false};
  bool transaction_finality_authority_granted{false};
};

struct CanonicalExecutablePlanCacheAdmissionRequest {
  CanonicalExecutablePlanCacheKey key;
  bool engine_cache_admission_authorized{false};
  bool metadata_only_entry{false};
  bool parameter_values_supplied{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalExecutablePlanCacheAdmissionResult {
  bool accepted{false};
  bool cached{false};
  std::shared_ptr<const CanonicalExecutablePlanCacheEntry> entry;
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

struct CanonicalExecutablePlanCacheLookupRequest {
  CanonicalExecutablePlanCacheKey current_key;
  std::vector<CanonicalExecutablePlanParameterBinding> parameter_bindings;
  std::optional<CanonicalPlannerContinuationReplayRequest>
      continuation_replay;
  executor::CanonicalExecutionMgaAuthority mga_authority;
  bool engine_lookup_authorized{false};
  bool engine_security_revalidated{false};
  bool engine_policy_revalidated{false};
  bool engine_authorization_revalidated{false};
  std::string authorization_revalidation_receipt_uuid;
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalExecutablePlanCacheHitReceipt {
  std::string plan_key_digest;
  std::string prepared_plan_uuid;
  std::string selected_plan_uuid;
  std::uint64_t root_physical_node_id{0};
  std::vector<std::uint64_t> physical_node_ids;
  std::vector<std::uint64_t> causal_counter_ids;
  std::string parameter_shape_uuid;
  std::string result_schema_uuid;
  std::size_t transient_parameter_value_count{0};
  bool immutable_stored_plan_unchanged{false};
  bool fresh_engine_mga_statement_bound{false};
  bool parameter_values_retained{false};
  bool prepare_metric_receipts_retained{false};
  bool metric_recollection_performed{false};
  bool continuation_replay_performed{false};
  bool structural_no_optimizer_search_planner_or_fallback_route{false};
  std::uint64_t metric_collector_invocation_count{0};
  std::uint64_t optimizer_invocation_count{0};
  std::uint64_t search_invocation_count{0};
  std::uint64_t planner_invocation_count{0};
  std::uint64_t uncached_fallback_invocation_count{0};
  std::optional<CanonicalPlannerContinuationReplayReceipt>
      continuation_replay_receipt;
  executor::PhysicalMgaStatementContext mga_statement_context;
};

struct CanonicalExecutablePlanCacheLookupResult {
  bool accepted{false};
  bool hit{false};
  bool reprepare_required{false};
  executor::TypedPhysicalNodeDag execution_physical_dag;
  std::shared_ptr<const CanonicalExecutablePlanCacheEntry> entry;
  CanonicalExecutablePlanCacheHitReceipt receipt;
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

inline bool CanonicalExecutablePlanUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return value != "00000000-0000-0000-0000-000000000000";
}

inline bool CanonicalExecutablePlanDigest(const std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](const unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

inline bool CanonicalExecutablePlanUuidV7(const std::string_view value) {
  if (!CanonicalExecutablePlanUuid(value) || value[14] != '7') return false;
  return value[19] == '8' || value[19] == '9' || value[19] == 'a' ||
         value[19] == 'b';
}

inline bool CanonicalExecutablePlanGenerationVectorValid(
    const std::vector<CanonicalExecutablePlanGeneration>& values,
    const bool empty_allowed) {
  if (!empty_allowed && values.empty()) return false;
  std::string previous;
  for (const auto& value : values) {
    if (!CanonicalExecutablePlanUuid(value.identity_uuid) ||
        value.generation == 0 ||
        !CanonicalExecutablePlanDigest(value.definition_digest) ||
        (!previous.empty() && value.identity_uuid <= previous)) {
      return false;
    }
    previous = value.identity_uuid;
  }
  return true;
}

inline std::vector<CanonicalExecutablePlanGeneration>
CanonicalExecutablePlanDependencyProjection(
    const std::vector<CanonicalPreparedPlanDependency>& dependencies,
    const std::initializer_list<CanonicalPreparedPlanDependencyKind> kinds) {
  std::vector<CanonicalExecutablePlanGeneration> projected;
  for (const auto& dependency : dependencies) {
    if (std::ranges::find(kinds, dependency.dependency_kind) == kinds.end()) {
      continue;
    }
    projected.push_back({dependency.dependency_uuid, dependency.generation,
                         dependency.definition_digest});
  }
  std::ranges::sort(projected, {},
                    &CanonicalExecutablePlanGeneration::identity_uuid);
  return projected;
}

inline bool CanonicalExecutablePlanCapabilityVectorValid(
    const std::vector<CanonicalExecutablePlanCapabilityGeneration>& values,
    const CanonicalPreparedPhysicalPlan& plan) {
  if (values.empty()) return false;
  std::string previous;
  for (const auto& value : values) {
    if (!CanonicalExecutablePlanUuid(value.capability_uuid) ||
        value.abi_version == 0 || value.generation == 0 ||
        !CanonicalExecutablePlanDigest(value.definition_digest) ||
        (!previous.empty() && value.capability_uuid <= previous)) {
      return false;
    }
    previous = value.capability_uuid;
  }
  std::vector<std::pair<std::string, std::uint32_t>> expected;
  for (const auto& node : plan.nodes) {
    expected.emplace_back(node.executor_capability_uuid,
                          node.executor_capability_abi_version);
  }
  std::ranges::sort(expected);
  expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
  if (expected.size() != values.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (values[index].capability_uuid != expected[index].first ||
        values[index].abi_version != expected[index].second) {
      return false;
    }
  }
  return true;
}

inline bool CanonicalExecutablePlanKeyMatchesPreparedPlan(
    const CanonicalExecutablePlanCacheKey& key,
    const CanonicalPreparedPhysicalPlan& plan) {
  const auto known_snapshot_class =
      key.snapshot_class >= CanonicalExecutablePlanSnapshotClass::kReadCommitted &&
      key.snapshot_class <= CanonicalExecutablePlanSnapshotClass::kDonorProfiled;
  const bool statement_source_valid =
      CanonicalExecutablePlanUuid(key.sblr_unit_uuid) !=
      CanonicalExecutablePlanUuid(key.internal_procedure_uuid);
  const bool donor_identity_valid =
      (key.donor_compatibility_profile_uuid.empty() &&
       key.donor_compatibility_generation == 0) ||
      (CanonicalExecutablePlanUuid(key.donor_compatibility_profile_uuid) &&
       key.donor_compatibility_generation != 0);
  const bool cluster_identity_valid =
      (key.standalone_database && key.cluster_uuid.empty() &&
       key.cluster_epoch == 0) ||
      (!key.standalone_database &&
       CanonicalExecutablePlanUuid(key.cluster_uuid) &&
       key.cluster_epoch != 0);
  if (!CanonicalExecutablePlanUuidV7(key.cache_plan_uuid) ||
      !CanonicalExecutablePlanUuidV7(key.compiled_at_uuidv7) ||
      key.plan_status != CanonicalExecutablePlanStatus::kValid ||
      !CanonicalExecutablePlanDigest(key.plan_key_digest) ||
      !CanonicalExecutablePlanUuid(key.database_uuid) ||
      key.engine_format_generation == 0 || !statement_source_valid ||
      !CanonicalExecutablePlanUuid(key.bound_sblr_tree_uuid) ||
      !CanonicalExecutablePlanUuid(key.parser_compatibility_profile_uuid) ||
      key.parser_compatibility_generation == 0 || !donor_identity_valid ||
      !CanonicalExecutablePlanUuid(key.plan_policy_profile_uuid) ||
      key.optimizer_configuration_generation == 0 ||
      !CanonicalExecutablePlanDigest(key.bound_object_set_digest) ||
      !CanonicalExecutablePlanDigest(key.security_policy_digest) ||
      !CanonicalExecutablePlanDigest(key.redaction_policy_digest) ||
      !CanonicalExecutablePlanDigest(key.resource_policy_digest) ||
      key.filespace_placement_generation == 0 ||
      !known_snapshot_class || !cluster_identity_valid ||
      key.prepared_plan_uuid != plan.prepared_plan_uuid ||
      key.prepare_generation != plan.prepare_generation ||
      key.parameter_shape_uuid != plan.parameter_shape_uuid ||
      key.result_schema_uuid != plan.result_schema_uuid ||
      key.selected_plan_uuid != plan.selected_plan_uuid ||
      key.selected_plan_signature != plan.selected_plan_signature ||
      key.selected_scalar_score != plan.selected_scalar_score ||
      key.root_physical_node_id != plan.root_physical_node_id ||
      key.published_node_count != plan.published_node_count ||
      key.first_causal_counter_id != plan.first_causal_counter_id ||
      key.bound_sblr_tree_uuid != plan.bound_sblr_tree_uuid ||
      key.catalog_epoch_uuid != plan.catalog_epoch_uuid ||
      key.security_context_uuid != plan.security_context_uuid ||
      key.capability_snapshot_uuid != plan.capability_snapshot_uuid ||
      key.resource_snapshot_uuid != plan.resource_snapshot_uuid ||
      key.statistics_snapshot_uuid != plan.statistics_snapshot_uuid ||
      key.route_snapshot_uuid != plan.route_snapshot_uuid ||
      key.catalog_generation != plan.catalog_generation ||
      key.security_epoch != plan.security_epoch ||
      key.policy_epoch != plan.policy_epoch ||
      key.resource_epoch != plan.resource_epoch ||
      key.statistics_generation != plan.statistics_generation ||
      key.route_epoch != plan.route_epoch ||
      key.route_generation != plan.route_generation ||
      key.memory_budget_bytes != plan.memory_budget_bytes ||
      key.spill_allowed != plan.spill_allowed ||
      key.continuation_receipt != plan.continuation_receipt ||
      key.parameters != plan.parameters ||
      key.result_descriptors != plan.result_descriptors ||
      key.physical_dependencies != plan.dependencies ||
      plan.abi_version != 1 ||
      !std::ranges::all_of(plan.parameters, [](const auto& descriptor) {
        return !descriptor.encoded_descriptor.empty();
      }) ||
      !std::ranges::all_of(plan.result_descriptors,
                           [](const auto& descriptor) {
                             return !descriptor.name_utf8.empty() &&
                                    !descriptor.encoded_descriptor.empty();
                           }) ||
      !plan.immutable_physical_identity_retained ||
      !plan.complete_cost_vectors_retained || plan.parameter_values_retained ||
      plan.continuation_context_retained !=
          plan.continuation_receipt.has_value() ||
      (plan.continuation_receipt.has_value() &&
       (!CanonicalPlannerContinuationReceiptValid(
            *plan.continuation_receipt) ||
        !plan.continuation_receipt->physical_root_delivery_validated)) ||
      plan.prepare_statement_authority_retained ||
      plan.execution_authority_granted) {
    return false;
  }

  if (!CanonicalExecutablePlanGenerationVectorValid(key.object_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.function_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.metadata_generations,
                                                     false) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.datatype_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.collation_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.statistics_generations,
                                                     false) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.index_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.filespace_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.route_generations,
                                                     false) ||
      !CanonicalExecutablePlanGenerationVectorValid(
          key.metric_generations,
          !plan.prepare_metric_receipts_retained) ||
      !CanonicalExecutablePlanGenerationVectorValid(
          key.continuation_generations,
          !plan.continuation_context_retained) ||
      !CanonicalExecutablePlanCapabilityVectorValid(key.capability_generations,
                                                     plan)) {
    return false;
  }
  if (key.object_generations != CanonicalExecutablePlanDependencyProjection(
                                    plan.dependencies,
                                    {CanonicalPreparedPlanDependencyKind::kObject}) ||
      key.function_generations != CanonicalExecutablePlanDependencyProjection(
                                      plan.dependencies,
                                      {CanonicalPreparedPlanDependencyKind::kFunction}) ||
      key.datatype_generations != CanonicalExecutablePlanDependencyProjection(
                                      plan.dependencies,
                                      {CanonicalPreparedPlanDependencyKind::kDatatype,
                                       CanonicalPreparedPlanDependencyKind::kDomain}) ||
      key.collation_generations != CanonicalExecutablePlanDependencyProjection(
                                       plan.dependencies,
                                       {CanonicalPreparedPlanDependencyKind::kCollation}) ||
      key.index_generations != CanonicalExecutablePlanDependencyProjection(
                                   plan.dependencies,
                                   {CanonicalPreparedPlanDependencyKind::kIndex}) ||
      key.filespace_generations != CanonicalExecutablePlanDependencyProjection(
                                       plan.dependencies,
                                       {CanonicalPreparedPlanDependencyKind::kFilespace}) ||
      key.metric_generations != CanonicalExecutablePlanDependencyProjection(
                                    plan.dependencies,
                                    {CanonicalPreparedPlanDependencyKind::kMetricSnapshot})) {
    return false;
  }
  if (key.continuation_generations !=
      CanonicalExecutablePlanDependencyProjection(
          plan.dependencies,
          {CanonicalPreparedPlanDependencyKind::kContinuationContext})) {
    return false;
  }
  const auto descriptor_dependencies =
      CanonicalExecutablePlanDependencyProjection(
          plan.dependencies,
          {CanonicalPreparedPlanDependencyKind::kDescriptor});
  if (key.metadata_generations.size() != descriptor_dependencies.size() + 1) {
    return false;
  }
  const auto schema_generation = std::ranges::find_if(
      key.metadata_generations, [&](const auto& value) {
        return value.identity_uuid == plan.result_schema_uuid &&
               value.generation == plan.catalog_generation;
      });
  if (schema_generation == key.metadata_generations.end()) return false;
  for (const auto& dependency : descriptor_dependencies) {
    if (std::ranges::find(key.metadata_generations, dependency) ==
        key.metadata_generations.end()) {
      return false;
    }
  }
  return key.statistics_generations.size() == 1 &&
         key.statistics_generations.front().identity_uuid ==
             plan.statistics_snapshot_uuid &&
         key.statistics_generations.front().generation ==
             plan.statistics_generation &&
         key.route_generations.size() == 1 &&
         key.route_generations.front().identity_uuid ==
             plan.route_snapshot_uuid &&
         key.route_generations.front().generation == plan.route_generation;
}

// QOW-SOURCE-OPT-010-PARAMETER-V1
struct CanonicalExecutablePlanParameterBindResult {
  bool accepted{false};
  std::size_t transient_value_count{0};
  bool parameter_values_retained{false};
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

inline CanonicalExecutablePlanParameterBindResult
ValidateCanonicalExecutablePlanParameterBindings(
    const std::vector<CanonicalExecutablePlanParameterBinding>& bindings,
    const CanonicalPreparedPhysicalPlan& plan) {
  CanonicalExecutablePlanParameterBindResult result;
  const auto refuse = [&](std::string field_id) {
    result = {};
    result.issues.push_back(CanonicalExecutablePlanIssue(
        "QOW-DIAG-OPT-010-PARAMETER-REFUSAL-V1", std::move(field_id)));
    return result;
  };

  // Decision precedence is intentionally independent of caller order:
  // missing > extra > conflicting repeated > non-nullable null > wrong type.
  for (const auto& declared : plan.parameters) {
    if (std::ranges::none_of(bindings, [&](const auto& binding) {
          return binding.descriptor.ordinal == declared.ordinal;
        })) {
      return refuse("missing");
    }
  }
  if (std::ranges::any_of(bindings, [&](const auto& binding) {
        return std::ranges::none_of(plan.parameters, [&](const auto& declared) {
          return binding.descriptor.ordinal == declared.ordinal;
        });
      })) {
    return refuse("extra");
  }
  for (const auto& declared : plan.parameters) {
    if (std::ranges::count_if(bindings, [&](const auto& binding) {
          return binding.descriptor.ordinal == declared.ordinal;
        }) != 1) {
      return refuse("conflicting_repeated");
    }
  }
  for (const auto& binding : bindings) {
    const auto declared = std::ranges::find_if(
        plan.parameters, [&](const auto& parameter) {
          return parameter.ordinal == binding.descriptor.ordinal;
        });
    if (declared != plan.parameters.end() && binding.typed_value != nullptr &&
        binding.typed_value->isSqlNull() && !declared->nullable) {
      return refuse("non_nullable_null");
    }
  }
  for (const auto& binding : bindings) {
    const auto declared = std::ranges::find_if(
        plan.parameters, [&](const auto& parameter) {
          return parameter.ordinal == binding.descriptor.ordinal;
        });
    const auto* value = binding.typed_value;
    if (declared == plan.parameters.end() || binding.descriptor != *declared ||
        value == nullptr || declared->encoded_descriptor.empty() ||
        value->descriptor.descriptor_uuid.canonical !=
            declared->descriptor_uuid ||
        value->descriptor.encoded_descriptor != declared->encoded_descriptor ||
        value->descriptor.encoded_descriptor.find("type_uuid=" +
                                                  declared->type_uuid) ==
            std::string::npos ||
        (value->state !=
             scratchbird::engine::internal_api::EngineValueState::value &&
         value->state != scratchbird::engine::internal_api::
                             EngineValueState::sql_null) ||
        (value->isSqlNull() &&
         (!value->encoded_value.empty() || !value->binary_value.empty())) ||
        (!value->isSqlNull() && !value->hasPayload()) ||
        (!value->isSqlNull() &&
         (value->encoded_value.empty() == value->binary_value.empty()))) {
      return refuse("wrong_type");
    }
  }
  result.accepted = true;
  result.transient_value_count = bindings.size();
  result.parameter_values_retained = false;
  return result;
}

inline bool CanonicalExecutablePlanParameterBindingsValid(
    const std::vector<CanonicalExecutablePlanParameterBinding>& bindings,
    const CanonicalPreparedPhysicalPlan& plan) {
  return ValidateCanonicalExecutablePlanParameterBindings(bindings, plan)
      .accepted;
}

inline executor::TypedPhysicalNodeDag
BindCanonicalExecutablePlanToCurrentStatement(
    const CanonicalPreparedPhysicalPlan& plan,
    const executor::PhysicalMgaStatementContext& statement_context) {
  executor::TypedPhysicalNodeDag dag;
  dag.abi_version = plan.abi_version + 1;
  dag.selected_plan_uuid = plan.selected_plan_uuid;
  dag.root_physical_node_id = plan.root_physical_node_id;
  dag.local_transaction_id = statement_context.owning_local_transaction_id;
  dag.statement_snapshot_id =
      statement_context.visible_committed_high_watermark;
  dag.mga_statement_context = statement_context;
  dag.admission_evidence = {
      {executor::PhysicalAdmissionStage::kBoundRequest,
       plan.bound_sblr_tree_uuid},
      {executor::PhysicalAdmissionStage::kCatalogEpoch,
       plan.catalog_epoch_uuid},
      {executor::PhysicalAdmissionStage::kSecurity,
       plan.security_context_uuid},
      {executor::PhysicalAdmissionStage::kMgaStatementBoundary,
       statement_context.statement_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kPolicyCapability,
       plan.capability_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kResource,
       plan.resource_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kStatisticsProvenance,
       plan.statistics_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kCanonicalRoute,
       plan.route_snapshot_uuid},
  };
  dag.bound_sblr_tree_uuid = plan.bound_sblr_tree_uuid;
  dag.catalog_epoch_uuid = plan.catalog_epoch_uuid;
  dag.security_context_uuid = plan.security_context_uuid;
  dag.capability_snapshot_uuid = plan.capability_snapshot_uuid;
  dag.resource_snapshot_uuid = plan.resource_snapshot_uuid;
  dag.statistics_snapshot_uuid = plan.statistics_snapshot_uuid;
  dag.route_snapshot_uuid = plan.route_snapshot_uuid;
  dag.catalog_generation = plan.catalog_generation;
  dag.security_epoch = plan.security_epoch;
  dag.policy_epoch = plan.policy_epoch;
  dag.resource_epoch = plan.resource_epoch;
  dag.statistics_generation = plan.statistics_generation;
  dag.route_epoch = plan.route_epoch;
  dag.route_generation = plan.route_generation;
  dag.memory_budget_bytes = plan.memory_budget_bytes;
  dag.spill_allowed = plan.spill_allowed;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  dag.data_access_observed = false;
  dag.parser_execution_authority_claimed = false;
  dag.transaction_finality_authority_claimed = false;
  dag.publication_contract_version = 1;
  dag.selected_plan_signature = plan.selected_plan_signature;
  dag.selected_scalar_score = plan.selected_scalar_score;
  dag.published_node_count = plan.published_node_count;
  dag.first_causal_counter_id = plan.first_causal_counter_id;
  dag.complete_cost_vectors_retained = plan.complete_cost_vectors_retained;
  dag.descriptor_contract_validated = true;
  dag.property_contract_validated = true;
  dag.dependency_contract_validated = true;
  dag.resource_contract_validated = true;
  dag.mga_contract_validated = true;
  dag.causal_identity_validated = true;
  dag.nodes.reserve(plan.nodes.size());
  for (const auto& prepared : plan.nodes) {
    executor::PhysicalNodeRecord node;
    node.physical_node_id = prepared.physical_node_id;
    node.relational_node_id = prepared.relational_node_id;
    node.node_kind = prepared.node_kind;
    node.implementation_id = prepared.implementation_id;
    node.input_physical_node_ids = prepared.input_physical_node_ids;
    node.output_descriptor_ids = prepared.output_descriptor_ids;
    node.shareable = prepared.shareable;
    node.causal_counter_id = prepared.causal_counter_id;
    node.selected_alternative_uuid = prepared.selected_alternative_uuid;
    node.executor_capability_uuid = prepared.executor_capability_uuid;
    node.executor_capability_abi_version =
        prepared.executor_capability_abi_version;
    node.cost_vector_uuid = prepared.cost_vector_uuid;
    node.required_property_uuids = prepared.required_property_uuids;
    node.delivered_property_uuids = prepared.delivered_property_uuids;
    node.memory_bytes_required = prepared.memory_bytes_required;
    node.spill_bytes_expected = prepared.spill_bytes_expected;
    node.engine_capability_validated = true;
    node.mga_statement_context = statement_context;
    node.logical_semantic_variant_id = prepared.logical_semantic_variant_id;
    node.publication_ordinal = prepared.publication_ordinal;
    node.transformation_uuid = prepared.transformation_uuid;
    node.transformation_rule_id = prepared.transformation_rule_id;
    node.enforced_property_uuids = prepared.enforced_property_uuids;
    node.retained_cost = prepared.retained_cost;
    dag.nodes.push_back(std::move(node));
  }
  return dag;
}

// QOW-SOURCE-OPT-010-DEPENDENCY-V1
struct CanonicalExecutablePlanDependencyMismatch {
  std::string field_id;
  bool protected_detail{false};
};

inline std::optional<CanonicalExecutablePlanDependencyMismatch>
CanonicalExecutablePlanFirstDependencyMismatch(
    const CanonicalExecutablePlanCacheKey& stored,
    const CanonicalExecutablePlanCacheKey& current) {
  const auto mismatch = [](std::string field_id,
                           const bool protected_detail = false) {
    return std::optional<CanonicalExecutablePlanDependencyMismatch>(
        CanonicalExecutablePlanDependencyMismatch{std::move(field_id),
                                                   protected_detail});
  };
  // Security-safe external precedence: protected security/redaction/policy
  // changes dominate object or route details that could disclose identity.
  if (stored.security_context_uuid != current.security_context_uuid ||
      stored.security_epoch != current.security_epoch ||
      stored.security_policy_digest != current.security_policy_digest) {
    return mismatch("security_generation", true);
  }
  if (stored.redaction_policy_digest != current.redaction_policy_digest) {
    return mismatch("redaction_generation", true);
  }
  if (stored.policy_epoch != current.policy_epoch ||
      stored.plan_policy_profile_uuid != current.plan_policy_profile_uuid) {
    return mismatch("policy_generation", true);
  }
  if (stored.continuation_receipt != current.continuation_receipt) {
    return mismatch("continuation_identity", true);
  }
  if (stored.database_uuid != current.database_uuid)
    return mismatch("database_uuid");
  if (stored.engine_format_generation != current.engine_format_generation)
    return mismatch("engine_format_generation");
  if (stored.catalog_epoch_uuid != current.catalog_epoch_uuid ||
      stored.catalog_generation != current.catalog_generation)
    return mismatch("catalog_generation");
  if (stored.bound_object_set_digest != current.bound_object_set_digest ||
      stored.object_generations != current.object_generations)
    return mismatch("object_generations", true);
  if (stored.metadata_generations != current.metadata_generations)
    return mismatch("metadata_generations", true);
  if (stored.datatype_generations != current.datatype_generations)
    return mismatch("datatype_generations");
  if (stored.collation_generations != current.collation_generations)
    return mismatch("collation_generations");
  if (stored.function_generations != current.function_generations)
    return mismatch("function_generations", true);
  if (stored.index_generations != current.index_generations)
    return mismatch("index_generations", true);
  if (stored.statistics_snapshot_uuid != current.statistics_snapshot_uuid ||
      stored.statistics_generation != current.statistics_generation ||
      stored.statistics_generations != current.statistics_generations)
    return mismatch("statistics_generations");
  if (stored.metric_generations != current.metric_generations)
    return mismatch("prepare_metric_generations");
  if (stored.continuation_generations != current.continuation_generations)
    return mismatch("continuation_generation", true);
  if (stored.capability_snapshot_uuid != current.capability_snapshot_uuid ||
      stored.capability_generations != current.capability_generations)
    return mismatch("capability_generations");
  if (stored.optimizer_configuration_generation !=
      current.optimizer_configuration_generation)
    return mismatch("optimizer_configuration_generation");
  if (stored.resource_snapshot_uuid != current.resource_snapshot_uuid ||
      stored.resource_epoch != current.resource_epoch ||
      stored.resource_policy_digest != current.resource_policy_digest ||
      stored.memory_budget_bytes != current.memory_budget_bytes ||
      stored.spill_allowed != current.spill_allowed)
    return mismatch("resource_generation");
  if (stored.filespace_placement_generation !=
          current.filespace_placement_generation ||
      stored.filespace_generations != current.filespace_generations)
    return mismatch("filespace_generations", true);
  if (stored.standalone_database != current.standalone_database ||
      stored.cluster_uuid != current.cluster_uuid ||
      stored.cluster_epoch != current.cluster_epoch)
    return mismatch("cluster_generation", true);
  if (stored.route_snapshot_uuid != current.route_snapshot_uuid ||
      stored.route_epoch != current.route_epoch ||
      stored.route_generation != current.route_generation ||
      stored.route_generations != current.route_generations)
    return mismatch("route_generations", true);
  if (stored.parser_compatibility_profile_uuid !=
          current.parser_compatibility_profile_uuid ||
      stored.parser_compatibility_generation !=
          current.parser_compatibility_generation)
    return mismatch("parser_compatibility_generation");
  if (stored.donor_compatibility_profile_uuid !=
          current.donor_compatibility_profile_uuid ||
      stored.donor_compatibility_generation !=
          current.donor_compatibility_generation)
    return mismatch("donor_compatibility_generation", true);
  if (stored.physical_dependencies != current.physical_dependencies)
    return mismatch("physical_dependencies", true);
  if (stored.result_schema_uuid != current.result_schema_uuid ||
      stored.result_descriptors != current.result_descriptors)
    return mismatch("result_schema");
  if (stored.parameter_shape_uuid != current.parameter_shape_uuid ||
      stored.parameters != current.parameters)
    return mismatch("parameter_shape");
  if (stored.sblr_unit_uuid != current.sblr_unit_uuid ||
      stored.internal_procedure_uuid != current.internal_procedure_uuid ||
      stored.bound_sblr_tree_uuid != current.bound_sblr_tree_uuid)
    return mismatch("bound_request_identity");
  if (stored.prepared_plan_uuid != current.prepared_plan_uuid ||
      stored.prepare_generation != current.prepare_generation ||
      stored.cache_plan_uuid != current.cache_plan_uuid ||
      stored.compiled_at_uuidv7 != current.compiled_at_uuidv7 ||
      stored.plan_key_digest != current.plan_key_digest ||
      stored.plan_status != current.plan_status ||
      stored.selected_plan_uuid != current.selected_plan_uuid ||
      stored.selected_plan_signature != current.selected_plan_signature ||
      stored.selected_scalar_score != current.selected_scalar_score ||
      stored.root_physical_node_id != current.root_physical_node_id ||
      stored.published_node_count != current.published_node_count ||
      stored.first_causal_counter_id != current.first_causal_counter_id ||
      stored.snapshot_class != current.snapshot_class) {
    return mismatch("physical_plan_identity");
  }
  return std::nullopt;
}

inline bool CanonicalExecutablePlanReplacementMatchesCurrentDependencies(
    const CanonicalExecutablePlanCacheKey& current,
    const CanonicalExecutablePlanCacheKey& replacement) {
  auto normalized = replacement;
  normalized.cache_plan_uuid = current.cache_plan_uuid;
  normalized.compiled_at_uuidv7 = current.compiled_at_uuidv7;
  normalized.plan_status = current.plan_status;
  normalized.plan_key_digest = current.plan_key_digest;
  normalized.prepared_plan_uuid = current.prepared_plan_uuid;
  normalized.prepare_generation = current.prepare_generation;
  normalized.selected_plan_uuid = current.selected_plan_uuid;
  normalized.selected_plan_signature = current.selected_plan_signature;
  normalized.selected_scalar_score = current.selected_scalar_score;
  normalized.root_physical_node_id = current.root_physical_node_id;
  normalized.published_node_count = current.published_node_count;
  normalized.first_causal_counter_id = current.first_causal_counter_id;
  return !CanonicalExecutablePlanFirstDependencyMismatch(current, normalized)
              .has_value();
}

struct CanonicalExecutablePlanInvalidationReceipt {
  bool invalidated{false};
  bool duplicate_invalidation{false};
  std::uint64_t invalidation_generation{0};
  std::string prepared_plan_uuid;
  std::string field_id;
  bool protected_detail{false};
  bool stale_execution_observed{false};
};

struct CanonicalExecutablePlanReprepareCandidate {
  CanonicalPreparePhysicalPlanRequest prepare_request;
  CanonicalExecutablePlanCacheAdmissionRequest admission_request;
  bool engine_candidate_authorized{false};
  bool parser_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
  std::uint64_t optimizer_invocation_count{0};
  std::uint64_t search_invocation_count{0};
  std::uint64_t planner_invocation_count{0};
  std::uint64_t uncached_fallback_invocation_count{0};
};

struct CanonicalExecutablePlanReprepareRequest {
  std::string invalidated_prepared_plan_uuid;
  CanonicalExecutablePlanCacheKey current_key;
  CanonicalPreparedPlanStore* prepared_plan_store{nullptr};
  bool engine_invalidation_authorized{false};
  bool engine_reprepare_authorized{false};
  bool engine_security_revalidated{false};
  bool engine_policy_revalidated{false};
  bool parser_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
  std::function<CanonicalExecutablePlanReprepareCandidate(
      const CanonicalExecutablePlanInvalidationReceipt&)>
      reprepare_once;
};

struct CanonicalExecutablePlanReprepareResult {
  bool accepted{false};
  bool invalidated{false};
  bool reprepared{false};
  bool replacement_admitted{false};
  bool caller_was_single_flight_leader{false};
  std::uint64_t governed_attempt_count{0};
  std::uint64_t total_attempt_count{0};
  std::uint64_t stale_execution_count{0};
  std::string old_prepared_plan_uuid;
  std::string replacement_prepared_plan_uuid;
  CanonicalExecutablePlanInvalidationReceipt invalidation;
  std::shared_ptr<const CanonicalExecutablePlanCacheEntry> replacement_entry;
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

class CanonicalExecutablePlanCache {
 public:
  CanonicalExecutablePlanCacheAdmissionResult Admit(
      const CanonicalPreparedPlanStore& prepared_plan_store,
      const CanonicalExecutablePlanCacheAdmissionRequest& request) {
    CanonicalExecutablePlanCacheAdmissionResult result;
    const auto refuse = [&](std::string field_id) {
      result = {};
      result.issues.push_back(
          {"QOW-DIAG-OPT-009-REFUSAL-V1", std::move(field_id)});
      return result;
    };
    if (!request.engine_cache_admission_authorized ||
        request.metadata_only_entry || request.parameter_values_supplied ||
        request.parser_execution_authority_claimed ||
        request.transaction_visibility_authority_claimed ||
        request.transaction_finality_authority_claimed ||
        request.recovery_authority_claimed) {
      return refuse("cache_admission_authority");
    }
    const auto prepared =
        prepared_plan_store.Find(request.key.prepared_plan_uuid);
    if (!prepared) return refuse("prepared_plan_not_found");
    if (!CanonicalExecutablePlanKeyMatchesPreparedPlan(request.key,
                                                       *prepared)) {
      return refuse("complete_executable_cache_key");
    }
    auto entry = std::make_shared<CanonicalExecutablePlanCacheEntry>();
    entry->key = request.key;
    entry->prepared_plan = prepared;
    entry->executable = true;
    entry->metadata_only = false;
    entry->parameter_values_retained = false;
    entry->prepare_statement_authority_retained = false;
    entry->execution_statement_authority_retained = false;
    entry->transaction_finality_authority_granted = false;
    {
      std::lock_guard lock(mutex_);
      if (states_.contains(entry->key.prepared_plan_uuid) ||
          cache_plan_uuids_.contains(entry->key.cache_plan_uuid)) {
        return refuse("duplicate_executable_cache_entry");
      }
      auto state = std::make_shared<LifecycleState>();
      state->entry = entry;
      state->status = CanonicalExecutablePlanStatus::kValid;
      states_.emplace(entry->key.prepared_plan_uuid, std::move(state));
      cache_plan_uuids_.insert(entry->key.cache_plan_uuid);
    }
    result.accepted = true;
    result.cached = true;
    result.entry = std::move(entry);
    return result;
  }

  CanonicalExecutablePlanCacheLookupResult LookupAndBind(
      const CanonicalExecutablePlanCacheLookupRequest& request) {
    CanonicalExecutablePlanCacheLookupResult result;
    const auto refuse = [&](std::string field_id,
                            const bool reprepare_required = true,
                            std::string diagnostic_id =
                                "QOW-DIAG-OPT-009-REFUSAL-V1") {
      result = {};
      result.reprepare_required = reprepare_required;
      result.issues.push_back(CanonicalExecutablePlanIssue(
          std::move(diagnostic_id), std::move(field_id)));
      return result;
    };
    if (!request.engine_lookup_authorized ||
        !request.engine_security_revalidated ||
        !request.engine_policy_revalidated ||
        !request.engine_authorization_revalidated ||
        !CanonicalExecutablePlanUuid(
            request.authorization_revalidation_receipt_uuid) ||
        request.parser_execution_authority_claimed ||
        request.transaction_finality_authority_claimed ||
        request.recovery_authority_claimed ||
        request.mga_authority.origin !=
            executor::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory ||
        !request.mga_authority.resolve_current ||
        !executor::PhysicalMgaStatementContextValid(
            request.mga_authority.statement_context)) {
      return refuse("fresh_engine_mga_statement_authority", false);
    }
    std::shared_ptr<const CanonicalExecutablePlanCacheEntry> entry;
    std::optional<CanonicalPlannerContinuationReplayReceipt>
        pending_continuation_replay;
    {
      std::lock_guard lock(mutex_);
      const auto found = states_.find(request.current_key.prepared_plan_uuid);
      if (found == states_.end()) return refuse("executable_cache_miss");
      const auto& state = found->second;
      if (!state || !state->entry ||
          state->status != CanonicalExecutablePlanStatus::kValid) {
        return refuse("invalidated_dependency", true,
                      "QOW-DIAG-OPT-010-DEPENDENCY-REFUSAL-V1");
      }
      const auto mismatch = CanonicalExecutablePlanFirstDependencyMismatch(
          state->entry->key, request.current_key);
      if (mismatch.has_value()) {
        state->status = CanonicalExecutablePlanStatus::kInvalid;
        state->invalidation_generation = ++invalidation_generation_;
        state->invalidation_field_id = mismatch->field_id;
        state->protected_detail = mismatch->protected_detail;
        return refuse(mismatch->protected_detail ? "protected_generation"
                                                 : mismatch->field_id,
                      true,
                      "QOW-DIAG-OPT-010-DEPENDENCY-REFUSAL-V1");
      }
      const auto& expected_continuation =
          state->entry->prepared_plan->continuation_receipt;
      if (expected_continuation.has_value() !=
          request.continuation_replay.has_value()) {
        return refuse(expected_continuation.has_value()
                          ? "continuation_replay_required"
                          : "unexpected_continuation_replay",
                      false,
                      "QOW-DIAG-OPT-CONTINUATION-REPLAY-REFUSAL-V1");
      }
      if (expected_continuation.has_value()) {
        CanonicalPlannerContinuationReplayReceipt replay_receipt;
        if (!CanonicalPlannerContinuationReplayMatches(
                *expected_continuation, *request.continuation_replay,
                &replay_receipt)) {
          return refuse("continuation_replay_identity", false,
                        "QOW-DIAG-OPT-CONTINUATION-REPLAY-REFUSAL-V1");
        }
        pending_continuation_replay = std::move(replay_receipt);
      }
      entry = state->entry;
    }
    if (!entry || !entry->executable || entry->metadata_only ||
        !entry->prepared_plan || entry->parameter_values_retained ||
        entry->prepare_statement_authority_retained ||
        entry->execution_statement_authority_retained ||
        entry->transaction_finality_authority_granted) {
      return refuse("metadata_or_authority_bearing_entry");
    }
    const auto parameter_validation =
        ValidateCanonicalExecutablePlanParameterBindings(
            request.parameter_bindings, *entry->prepared_plan);
    if (!parameter_validation.accepted) {
      return refuse(parameter_validation.issues.front().field_id, false,
                    "QOW-DIAG-OPT-010-PARAMETER-REFUSAL-V1");
    }
    auto current = request.mga_authority.resolve_current();
    if (!current.diagnostic.ok ||
        !executor::PhysicalMgaStatementContextValid(current.statement_context) ||
        !executor::PhysicalMgaStatementContextEqual(
            current.statement_context,
            request.mga_authority.statement_context)) {
      return refuse("fresh_engine_mga_statement_revalidation", false);
    }
    auto execution_dag = BindCanonicalExecutablePlanToCurrentStatement(
        *entry->prepared_plan, current.statement_context);
    const auto validation = executor::ValidateTypedPhysicalNodeDag(execution_dag);
    if (!validation.accepted) {
      return refuse(validation.issues.empty()
                        ? "reconstructed_physical_dag"
                        : validation.issues.front().field_id,
                    false);
    }
    const auto authority_validation =
        executor::RevalidateCanonicalExecutionMgaAuthority(
            request.mga_authority, execution_dag);
    if (!authority_validation.ok) {
      return refuse("canonical_execution_mga_revalidation:" +
                        authority_validation.diagnostic_code,
                    false);
    }
    if (pending_continuation_replay.has_value()) {
      std::lock_guard lock(mutex_);
      const auto found = states_.find(request.current_key.prepared_plan_uuid);
      if (found == states_.end() || !found->second ||
          !found->second->entry ||
          found->second->status != CanonicalExecutablePlanStatus::kValid ||
          found->second->entry != entry ||
          CanonicalExecutablePlanFirstDependencyMismatch(
              found->second->entry->key, request.current_key)
              .has_value()) {
        return refuse("continuation_replay_dependency_changed", true,
                      "QOW-DIAG-OPT-CONTINUATION-REPLAY-REFUSAL-V1");
      }
      if (!found->second->consumed_continuation_replay_identities
               .insert(pending_continuation_replay->replay_identity)
               .second) {
        return refuse("continuation_replay_consumed", false,
                      "QOW-DIAG-OPT-CONTINUATION-REPLAY-REFUSAL-V1");
      }
      pending_continuation_replay->consumed_once = true;
    }
    result.accepted = true;
    result.hit = true;
    result.entry = entry;
    result.execution_physical_dag = std::move(execution_dag);
    result.receipt.plan_key_digest = entry->key.plan_key_digest;
    result.receipt.prepared_plan_uuid = entry->key.prepared_plan_uuid;
    result.receipt.selected_plan_uuid = entry->key.selected_plan_uuid;
    result.receipt.root_physical_node_id =
        entry->key.root_physical_node_id;
    result.receipt.parameter_shape_uuid = entry->key.parameter_shape_uuid;
    result.receipt.result_schema_uuid = entry->key.result_schema_uuid;
    result.receipt.transient_parameter_value_count =
        request.parameter_bindings.size();
    result.receipt.immutable_stored_plan_unchanged = true;
    result.receipt.fresh_engine_mga_statement_bound = true;
    result.receipt.parameter_values_retained = false;
    result.receipt.prepare_metric_receipts_retained =
        entry->prepared_plan->prepare_metric_receipts_retained;
    result.receipt.metric_recollection_performed = false;
    result.receipt.continuation_replay_performed =
        pending_continuation_replay.has_value();
    result.receipt.continuation_replay_receipt =
        std::move(pending_continuation_replay);
    result.receipt.metric_collector_invocation_count = 0;
    result.receipt
        .structural_no_optimizer_search_planner_or_fallback_route = true;
    result.receipt.mga_statement_context = current.statement_context;
    for (const auto& node : entry->prepared_plan->nodes) {
      result.receipt.physical_node_ids.push_back(node.physical_node_id);
      result.receipt.causal_counter_ids.push_back(node.causal_counter_id);
    }
    return result;
  }

  std::size_t Size() const {
    std::lock_guard lock(mutex_);
    return std::ranges::count_if(states_, [](const auto& item) {
      return item.second &&
             item.second->status == CanonicalExecutablePlanStatus::kValid;
    });
  }

  CanonicalExecutablePlanInvalidationReceipt InvalidateIfStale(
      const std::string& prepared_plan_uuid,
      const CanonicalExecutablePlanCacheKey& current_key,
      const bool engine_invalidation_authorized) {
    CanonicalExecutablePlanInvalidationReceipt receipt;
    receipt.prepared_plan_uuid = prepared_plan_uuid;
    if (!engine_invalidation_authorized) return receipt;
    std::lock_guard lock(mutex_);
    const auto found = states_.find(prepared_plan_uuid);
    if (found == states_.end() || !found->second || !found->second->entry) {
      return receipt;
    }
    const auto& state = found->second;
    if (state->status != CanonicalExecutablePlanStatus::kValid) {
      receipt.invalidated = true;
      receipt.duplicate_invalidation = true;
      receipt.invalidation_generation = state->invalidation_generation;
      receipt.field_id = state->protected_detail
                             ? "protected_generation"
                             : state->invalidation_field_id;
      receipt.protected_detail = state->protected_detail;
      return receipt;
    }
    const auto mismatch = CanonicalExecutablePlanFirstDependencyMismatch(
        state->entry->key, current_key);
    if (!mismatch.has_value()) return receipt;
    state->status = CanonicalExecutablePlanStatus::kInvalid;
    state->invalidation_generation = ++invalidation_generation_;
    state->invalidation_field_id = mismatch->field_id;
    state->protected_detail = mismatch->protected_detail;
    receipt.invalidated = true;
    receipt.invalidation_generation = state->invalidation_generation;
    receipt.field_id = mismatch->protected_detail ? "protected_generation"
                                                  : mismatch->field_id;
    receipt.protected_detail = mismatch->protected_detail;
    return receipt;
  }

  // QOW-SOURCE-OPT-010-REPREPARE-V1
  // QOW-SOURCE-OPT-010-CONCURRENCY-V1
  CanonicalExecutablePlanReprepareResult InvalidateAndReprepareOnce(
      const CanonicalExecutablePlanReprepareRequest& request) {
    CanonicalExecutablePlanReprepareResult result;
    result.old_prepared_plan_uuid = request.invalidated_prepared_plan_uuid;
    const auto refuse = [&](std::string field_id) {
      result.accepted = false;
      result.issues.clear();
      result.issues.push_back(CanonicalExecutablePlanIssue(
          "QOW-DIAG-OPT-010-REPREPARE-REFUSAL-V1", std::move(field_id),
          "error", "plan", "not_retryable"));
      return result;
    };
    if (!request.engine_invalidation_authorized ||
        !request.engine_reprepare_authorized ||
        !request.engine_security_revalidated ||
        !request.engine_policy_revalidated ||
        request.parser_authority_claimed ||
        request.transaction_finality_authority_claimed ||
        request.recovery_authority_claimed ||
        request.prepared_plan_store == nullptr || !request.reprepare_once) {
      return refuse("engine_governed_reprepare_authority");
    }

    std::shared_ptr<LifecycleState> old_state;
    {
      std::unique_lock lock(mutex_);
      const auto found = states_.find(request.invalidated_prepared_plan_uuid);
      if (found == states_.end() || !found->second || !found->second->entry) {
        return refuse("invalidated_plan_not_found");
      }
      old_state = found->second;
      if (old_state->status == CanonicalExecutablePlanStatus::kValid) {
        const auto mismatch = CanonicalExecutablePlanFirstDependencyMismatch(
            old_state->entry->key, request.current_key);
        if (!mismatch.has_value()) return refuse("invalidation_required");
        old_state->status = CanonicalExecutablePlanStatus::kInvalid;
        old_state->invalidation_generation = ++invalidation_generation_;
        old_state->invalidation_field_id = mismatch->field_id;
        old_state->protected_detail = mismatch->protected_detail;
      }
      result.invalidation.invalidated = true;
      result.invalidation.invalidation_generation =
          old_state->invalidation_generation;
      result.invalidation.prepared_plan_uuid =
          request.invalidated_prepared_plan_uuid;
      result.invalidation.field_id = old_state->protected_detail
                                         ? "protected_generation"
                                         : old_state->invalidation_field_id;
      result.invalidation.protected_detail = old_state->protected_detail;
      result.invalidated = true;

      while (old_state->reprepare_in_progress) {
        old_state->condition.wait(lock);
      }
      if (old_state->reprepare_attempted) {
        result.total_attempt_count = 1;
        result.reprepared = old_state->replacement_admitted;
        result.replacement_admitted = old_state->replacement_admitted;
        result.replacement_prepared_plan_uuid =
            old_state->replacement_prepared_plan_uuid;
        result.replacement_entry = old_state->replacement_entry;
        if (!old_state->replacement_admitted) {
          return refuse(old_state->reprepare_failure_field.empty()
                            ? "governed_reprepare_failed"
                            : old_state->reprepare_failure_field);
        }
        if (!old_state->replacement_entry ||
            !CanonicalExecutablePlanReplacementMatchesCurrentDependencies(
                request.current_key, old_state->replacement_entry->key)) {
          return refuse("divergent_current_dependency_state");
        }
        result.accepted = true;
        return result;
      }
      old_state->reprepare_attempted = true;
      old_state->reprepare_in_progress = true;
      result.caller_was_single_flight_leader = true;
      result.governed_attempt_count = 1;
      result.total_attempt_count = 1;
    }

    std::string failure_field;
    CanonicalExecutablePlanReprepareCandidate candidate;
    try {
      candidate = request.reprepare_once(result.invalidation);
    } catch (...) {
      failure_field = "governed_reprepare_callback_exception";
    }
    std::shared_ptr<const CanonicalExecutablePlanCacheEntry> replacement;
    if (failure_field.empty() &&
        (!candidate.engine_candidate_authorized ||
        candidate.parser_authority_claimed ||
        candidate.transaction_finality_authority_claimed ||
        candidate.recovery_authority_claimed ||
        candidate.optimizer_invocation_count > 1 ||
        candidate.search_invocation_count > 1 ||
        candidate.planner_invocation_count != 1 ||
        candidate.uncached_fallback_invocation_count != 0 ||
        candidate.prepare_request.prepared_plan_uuid ==
            request.invalidated_prepared_plan_uuid ||
        candidate.prepare_request.prepare_generation !=
            old_state->entry->key.prepare_generation + 1 ||
        candidate.admission_request.key.cache_plan_uuid ==
            old_state->entry->key.cache_plan_uuid ||
        candidate.admission_request.key.compiled_at_uuidv7 ==
            old_state->entry->key.compiled_at_uuidv7 ||
        candidate.admission_request.key.selected_plan_uuid ==
            old_state->entry->key.selected_plan_uuid)) {
      failure_field = "governed_reprepare_candidate";
    }

    if (failure_field.empty()) {
      auto normalized = candidate.admission_request.key;
      normalized.cache_plan_uuid = request.current_key.cache_plan_uuid;
      normalized.compiled_at_uuidv7 = request.current_key.compiled_at_uuidv7;
      normalized.plan_status = request.current_key.plan_status;
      normalized.plan_key_digest = request.current_key.plan_key_digest;
      normalized.prepared_plan_uuid = request.current_key.prepared_plan_uuid;
      normalized.prepare_generation = request.current_key.prepare_generation;
      normalized.selected_plan_uuid = request.current_key.selected_plan_uuid;
      normalized.selected_plan_signature =
          request.current_key.selected_plan_signature;
      normalized.selected_scalar_score = request.current_key.selected_scalar_score;
      normalized.root_physical_node_id =
          request.current_key.root_physical_node_id;
      normalized.published_node_count = request.current_key.published_node_count;
      normalized.first_causal_counter_id =
          request.current_key.first_causal_counter_id;
      const auto replacement_mismatch =
          CanonicalExecutablePlanFirstDependencyMismatch(request.current_key,
                                                         normalized);
      if (replacement_mismatch.has_value()) {
        failure_field = "replacement_current_dependency_state:" +
                        replacement_mismatch->field_id;
      }
    }

    if (failure_field.empty()) {
      const auto prepared = PrepareCanonicalPhysicalPlan(
          candidate.prepare_request, request.prepared_plan_store);
      if (!prepared.accepted || !prepared.prepared || !prepared.persisted) {
        failure_field = prepared.issues.empty()
                            ? "replacement_prepare"
                            : "replacement_prepare:" +
                                  prepared.issues.front().field_id;
      }
    }
    if (failure_field.empty()) {
      const auto admitted =
          Admit(*request.prepared_plan_store, candidate.admission_request);
      if (!admitted.accepted || !admitted.cached || !admitted.entry) {
        failure_field = admitted.issues.empty()
                            ? "replacement_cache_admission"
                            : "replacement_cache_admission:" +
                                  admitted.issues.front().field_id;
      } else {
        replacement = admitted.entry;
      }
    }

    {
      std::lock_guard lock(mutex_);
      old_state->reprepare_in_progress = false;
      old_state->replacement_admitted = replacement != nullptr;
      old_state->replacement_entry = replacement;
      old_state->replacement_prepared_plan_uuid =
          replacement ? replacement->key.prepared_plan_uuid : std::string{};
      old_state->reprepare_failure_field = failure_field;
      old_state->status = replacement ? CanonicalExecutablePlanStatus::kRetired
                                      : CanonicalExecutablePlanStatus::kInvalid;
      old_state->condition.notify_all();
    }
    if (!replacement) return refuse(failure_field);
    result.accepted = true;
    result.reprepared = true;
    result.replacement_admitted = true;
    result.replacement_prepared_plan_uuid = replacement->key.prepared_plan_uuid;
    result.replacement_entry = std::move(replacement);
    return result;
  }

 private:
  struct LifecycleState {
    std::shared_ptr<const CanonicalExecutablePlanCacheEntry> entry;
    CanonicalExecutablePlanStatus status{CanonicalExecutablePlanStatus::kValid};
    std::uint64_t invalidation_generation{0};
    std::string invalidation_field_id;
    bool protected_detail{false};
    bool reprepare_attempted{false};
    bool reprepare_in_progress{false};
    bool replacement_admitted{false};
    std::string replacement_prepared_plan_uuid;
    std::shared_ptr<const CanonicalExecutablePlanCacheEntry> replacement_entry;
    std::string reprepare_failure_field;
    std::unordered_set<std::string>
        consumed_continuation_replay_identities;
    std::condition_variable condition;
  };

  mutable std::mutex mutex_;
  std::uint64_t invalidation_generation_{0};
  std::map<std::string,
           std::shared_ptr<LifecycleState>>
      states_;
  std::unordered_set<std::string> cache_plan_uuids_;
};

struct CanonicalExecutablePlanHitExecutionRequest {
  CanonicalExecutablePlanCache* executable_plan_cache{nullptr};
  CanonicalExecutablePlanCacheLookupRequest lookup;
  executor::PhysicalNodeAbiLimits limits;
  executor::CanonicalPhysicalDagRuntimeLimits runtime_limits;
  std::function<bool()> cancellation_requested;
  std::vector<executor::CanonicalPhysicalExecutorRegistration>
      available_executors;
  executor::CanonicalResultPublicationRequest result_publication_request;
  bool engine_execution_authorized{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalExecutablePlanExecutedNodeReceipt {
  std::uint64_t physical_node_id{0};
  std::uint64_t causal_counter_id{0};
  std::size_t execution_ordinal{0};
};

struct CanonicalExecutablePlanHitExecutionResult {
  bool accepted{false};
  bool cache_hit{false};
  bool exact_selected_nodes_executed{false};
  bool canonical_result_published{false};
  bool data_access_observed{false};
  bool reprepare_required{false};
  bool automatic_replan_attempted{false};
  bool parameter_values_retained{false};
  bool prepare_metric_receipts_retained{false};
  bool metric_recollection_performed{false};
  bool continuation_replay_performed{false};
  bool structural_no_optimizer_search_planner_or_fallback_route{false};
  std::uint64_t metric_collector_invocation_count{0};
  std::uint64_t optimizer_invocation_count{0};
  std::uint64_t search_invocation_count{0};
  std::uint64_t planner_invocation_count{0};
  std::uint64_t uncached_fallback_invocation_count{0};
  std::optional<CanonicalPlannerContinuationReplayReceipt>
      continuation_replay_receipt;
  std::string selected_plan_uuid;
  std::uint64_t executed_root_physical_node_id{0};
  std::string result_schema_uuid;
  std::vector<CanonicalExecutablePlanExecutedNodeReceipt> executed_nodes;
  CanonicalExecutablePlanCacheLookupResult checkout;
  executor::CanonicalPhysicalDagDispatchResult dispatch;
  executor::CanonicalResultPublicationResult result_publication;
  executor::PhysicalMgaStatementContext mga_statement_context;
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

struct CanonicalExecutablePlanGovernedExecutionRequest {
  CanonicalExecutablePlanCache* executable_plan_cache{nullptr};
  CanonicalExecutablePlanReprepareRequest reprepare;
  std::function<CanonicalExecutablePlanHitExecutionRequest(
      const std::shared_ptr<const CanonicalExecutablePlanCacheEntry>&)>
      build_replacement_execution;
  bool engine_execution_authorized{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalExecutablePlanGovernedExecutionResult {
  bool accepted{false};
  bool replacement_executed{false};
  bool stale_plan_executed{false};
  std::uint64_t governed_reprepare_attempt_count{0};
  CanonicalExecutablePlanReprepareResult reprepare;
  CanonicalExecutablePlanHitExecutionResult execution;
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

enum class CanonicalExplainMode : std::uint8_t {
  kPlain = 1,
  kAnalyze,
};

enum class CanonicalExplainFieldState : std::uint8_t {
  kVisible = 1,
  kRedacted,
  kBucketed,
  kAbsent,
};

struct CanonicalExplainDisclosurePolicy {
  std::uint16_t abi_version{1};
  std::string request_uuid;
  std::string subject_uuid;
  std::string redaction_policy_uuid;
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  bool engine_plan_read_authorized{false};
  bool plan_shape_authorized{false};
  bool object_existence_authorized{false};
  bool object_names_authorized{false};
  bool cardinality_cost_authorized{false};
  bool actual_profile_authorized{false};
  bool security_detail_authorized{false};
  bool route_detail_authorized{false};
  bool cluster_detail_authorized{false};
  bool archive_detail_authorized{false};
  bool donor_detail_authorized{false};
  bool invalidation_detail_authorized{false};
  bool bucket_hidden_cardinality{false};
  bool parser_policy_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalExplainCandidateRecord {
  std::uint32_t logical_node_id{0};
  CanonicalPreparedExplainCandidateDisposition disposition{
      CanonicalPreparedExplainCandidateDisposition::kRejected};
  std::string candidate_family_id;
  std::string alternative_uuid;
  std::string reason_id;
  std::uint64_t estimated_rows{0};
  executor::PhysicalCostVectorReceipt retained_cost;
  CanonicalExplainFieldState identity_state{
      CanonicalExplainFieldState::kVisible};
  CanonicalExplainFieldState estimate_state{
      CanonicalExplainFieldState::kVisible};
};

struct CanonicalExplainNodeRecord {
  std::uint64_t physical_node_id{0};
  std::uint32_t logical_node_id{0};
  std::uint64_t causal_counter_id{0};
  std::size_t execution_ordinal{0};
  std::string implementation_id;
  std::string logical_semantic_variant_id;
  std::string selected_alternative_uuid;
  std::string transformation_uuid;
  std::string transformation_rule_id;
  std::string executor_capability_uuid;
  std::uint32_t executor_capability_abi_version{0};
  std::vector<std::uint64_t> input_physical_node_ids;
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
  std::vector<std::string> enforced_property_uuids;
  executor::PhysicalCostVectorReceipt estimated_cost;
  std::uint64_t memory_bytes_required{0};
  std::uint64_t spill_bytes_expected{0};
  std::uint64_t estimated_input_rows{0};
  std::uint64_t estimated_output_rows{0};
  std::uint64_t actual_input_rows{0};
  std::uint64_t actual_output_rows{0};
  std::uint64_t actual_rows_examined{0};
  bool data_access_observation_known{false};
  bool data_access_observed{false};
  CanonicalExplainFieldState data_access_state{
      CanonicalExplainFieldState::kAbsent};
  executor::CanonicalPhysicalNodeRuntimeObservation runtime_observation;
  CanonicalExplainFieldState runtime_route_state{
      CanonicalExplainFieldState::kAbsent};
  CanonicalExplainFieldState runtime_security_state{
      CanonicalExplainFieldState::kAbsent};
  CanonicalExplainFieldState runtime_archive_state{
      CanonicalExplainFieldState::kAbsent};
  CanonicalExplainFieldState runtime_cluster_state{
      CanonicalExplainFieldState::kAbsent};
  CanonicalExplainFieldState runtime_donor_state{
      CanonicalExplainFieldState::kAbsent};
  CanonicalExplainFieldState identity_state{
      CanonicalExplainFieldState::kVisible};
  CanonicalExplainFieldState estimate_state{
      CanonicalExplainFieldState::kVisible};
  CanonicalExplainFieldState actual_state{
      CanonicalExplainFieldState::kAbsent};
};

struct CanonicalExplainDocument {
  std::uint16_t abi_version{1};
  CanonicalExplainMode mode{CanonicalExplainMode::kPlain};
  std::string prepared_plan_uuid;
  std::uint64_t prepare_generation{0};
  std::string selected_plan_uuid;
  std::string selected_plan_signature;
  std::string bound_sblr_tree_uuid;
  std::string result_schema_uuid;
  std::uint64_t root_physical_node_id{0};
  std::uint64_t published_node_count{0};
  std::string search_strategy_id;
  std::vector<CanonicalPreparedExplainStageRecord> stages;
  std::vector<CanonicalExplainCandidateRecord> candidates;
  std::vector<CanonicalPreparedExplainBarrierRecord> barriers;
  std::vector<CanonicalPreparedExplainStatisticRecord> statistics;
  std::vector<CanonicalPreparedExplainAssumptionRecord> assumptions;
  std::vector<CanonicalPreparedPlanResultDescriptor> result_descriptors;
  std::vector<CanonicalPreparedPlanDependency> dependencies;
  std::vector<CanonicalExplainNodeRecord> nodes;
  CanonicalExecutablePlanStatus lifecycle_status{
      CanonicalExecutablePlanStatus::kValid};
  std::optional<CanonicalExecutablePlanInvalidationReceipt> invalidation;
  bool cache_entry_present{false};
  bool cache_hit{false};
  bool reprepare_attempted{false};
  bool reprepare_succeeded{false};
  std::uint64_t reprepare_attempt_count{0};
  std::string replacement_prepared_plan_uuid;
  bool data_access_observation_known{false};
  bool data_access_observed{false};
  CanonicalExplainFieldState data_access_state{
      CanonicalExplainFieldState::kAbsent};
  bool analyzed_mga_statement_context_present{false};
  executor::PhysicalMgaStatementContext analyzed_mga_statement_context;
  CanonicalExplainFieldState analyzed_mga_statement_context_state{
      CanonicalExplainFieldState::kAbsent};
  bool analyzed{false};
  bool redacted{false};
  bool immutable_stored_plan_rendered{false};
  bool completed_engine_execution_consumed{false};
};

struct CanonicalExplainRequest {
  std::uint16_t abi_version{1};
  CanonicalExplainMode mode{CanonicalExplainMode::kPlain};
  std::shared_ptr<const CanonicalPreparedPhysicalPlan> prepared_plan;
  CanonicalExecutablePlanStatus lifecycle_status{
      CanonicalExecutablePlanStatus::kValid};
  std::optional<CanonicalExecutablePlanInvalidationReceipt> invalidation;
  bool cache_entry_present{false};
  bool cache_hit{false};
  bool reprepare_attempted{false};
  bool reprepare_succeeded{false};
  std::uint64_t reprepare_attempt_count{0};
  std::string replacement_prepared_plan_uuid;
  const executor::CanonicalPhysicalDagDispatchResult* completed_dispatch{
      nullptr};
  std::string completed_result_schema_uuid;
  bool engine_result_schema_evidence{false};
  executor::CanonicalExecutionMgaAuthority mga_authority;
  CanonicalExplainDisclosurePolicy disclosure;
  bool renderer_execution_authority_claimed{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
  bool feedback_authority_claimed{false};
  bool benchmark_authority_claimed{false};
};

struct CanonicalExplainResult {
  bool accepted{false};
  bool analyzed{false};
  CanonicalExplainDocument document;
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

// SEARCH_KEY: SB_OPTIMIZER_PLAN_CACHE_KEY
struct OptimizerPlanCacheKeyInput {
  std::string operation_id;
  std::string sblr_digest;
  std::string descriptor_set_digest;
  std::string statistics_snapshot_id;
  std::string catalog_stats_digest;
  std::string cost_profile_id;
  std::string executor_capability_set_id;
  std::string route_capability_digest;
  std::string security_policy_digest;
  std::string redaction_route_digest;
  std::string normalized_optimizer_controls_digest;
  std::string parameter_shape_digest;
  std::string memory_grant_class;
  std::string memory_grant_digest;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t memory_policy_epoch = 0;
  std::uint64_t memory_feedback_generation = 0;
  std::uint64_t compatibility_epoch = 0;
  std::uint64_t format_compatibility_epoch = 0;
  std::uint64_t route_epoch = 0;
  std::vector<std::string> object_uuids;
  std::vector<std::string> function_uuids;
  std::vector<std::string> index_uuids;
  std::vector<std::string> filespace_uuids;
  std::vector<std::string> dependency_digests;
};

struct CachedOptimizerPlan {
  std::string cache_key;
  OptimizerPlanCacheKeyInput key_input;
  BoundOptimizerResult result;
  std::uint64_t created_epoch = 0;
  bool valid = true;
  bool invalidated_by_dependency = false;
  std::string invalidation_diagnostic_code;
  std::string invalidation_event_kind;
  std::string invalidation_dependency_uuid;
  bool metadata_only = true;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool parser_or_reference_finality_authority = false;
  bool memory_governed = false;
  std::uint64_t memory_reserved_bytes = 0;
  std::string memory_lease_id;
  memory::ResultCursorPlanMemoryScope memory_scope;
  std::vector<std::string> memory_governance_evidence;
};

struct OptimizerInvalidationEvent {
  std::string event_kind;
  std::string dependency_uuid;
  std::uint64_t event_epoch = 0;
};

struct OptimizerPlanCacheStats {
  std::uint64_t puts = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t invalidations = 0;
};

struct OptimizerPlanCacheLookupResult {
  bool hit = false;
  std::string cache_key;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::optional<CachedOptimizerPlan> plan;
};

struct OptimizerPlanCacheInvalidationResult {
  std::uint64_t invalidated_count = 0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

// SEARCH_KEY: OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE
struct OptimizerPlanCacheEnterpriseValidation {
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

struct OptimizerPlanCacheMemoryGovernanceRequest {
  memory::ResultCursorPlanMemoryGovernor* governor = nullptr;
  memory::HierarchicalMemoryBudgetLedger* ledger = nullptr;
  memory::ResultCursorPlanMemoryPolicy policy;
  memory::ResultCursorPlanMemoryScope scope;
  memory::ResultCursorPlanMemoryEpochs epochs;
  memory::HierarchicalMemoryBudgetProvenance provenance;
  std::uint64_t estimated_plan_bytes = 0;
  bool cluster_route_requested = false;
};

struct OptimizerPlanCachePersistenceRequest {
  std::string storage_scope_uuid;
  std::string persisted_by_principal_uuid;
  std::uint64_t persisted_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t memory_policy_epoch = 0;
  std::uint64_t memory_feedback_generation = 0;
  bool durable_catalog_persistence = true;
  bool mga_transaction_committed = true;
  bool security_redaction_evidence_present = true;
  bool fixture_or_test_only = false;
  bool cluster_route_projection_present = false;
};

struct OptimizerPlanCachePersistenceEnvelope {
  std::uint32_t schema_version = 1;
  std::string persistence_source = "engine_optimizer_plan_cache_catalog";
  OptimizerPlanCachePersistenceRequest request;
  std::vector<CachedOptimizerPlan> plans;
  std::string envelope_digest_algorithm = "sha256-v1";
  std::string envelope_digest;
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

// SEARCH_KEY: SB_OPTIMIZER_PRODUCTION_PLAN_CACHE_KEY_BUILDER
// Production cache keys require caller-supplied route, redaction, parameter,
// memory, dependency, and cost-profile digests. The compatibility builder is
// intentionally rejected by enterprise validation when it falls back to local
// defaults or unbound parameter placeholders.
struct OptimizerProductionPlanCacheKeyRequest {
  BoundOptimizerRequest bound_request;
  std::string catalog_stats_digest;
  std::string cost_profile_id;
  std::string route_capability_digest;
  std::string security_policy_digest;
  std::string redaction_route_digest;
  std::string parameter_shape_digest;
  std::string memory_grant_class;
  std::string memory_grant_digest;
  std::uint64_t compatibility_epoch = 0;
  std::uint64_t format_compatibility_epoch = 0;
  std::vector<std::string> object_uuids;
  std::vector<std::string> function_uuids;
  std::vector<std::string> index_uuids;
  std::vector<std::string> filespace_uuids;
  std::vector<std::string> dependency_digests;
  bool cluster_route_requested = false;
  bool parser_or_reference_authority_claimed = false;
};

struct OptimizerProductionPlanCacheKeyResult {
  bool ok = false;
  std::string diagnostic_code;
  OptimizerPlanCacheKeyInput input;
  std::vector<std::string> evidence;
};

class OptimizerPlanCache {
 public:
  void Put(CachedOptimizerPlan plan);
  OptimizerPlanCacheEnterpriseValidation PutEnterprise(CachedOptimizerPlan plan);
  OptimizerPlanCacheEnterpriseValidation PutEnterpriseGoverned(
      CachedOptimizerPlan plan,
      OptimizerPlanCacheMemoryGovernanceRequest governance);
  std::optional<CachedOptimizerPlan> Get(const std::string& cache_key);
  OptimizerPlanCacheLookupResult Lookup(const OptimizerPlanCacheKeyInput& input);
  OptimizerPlanCacheLookupResult LookupEnterprise(const OptimizerPlanCacheKeyInput& input);
  std::uint64_t Invalidate(const OptimizerInvalidationEvent& event);
  OptimizerPlanCacheInvalidationResult InvalidateWithEvidence(const OptimizerInvalidationEvent& event);
  OptimizerPlanCacheInvalidationResult InvalidateWithGovernedMemory(
      const OptimizerInvalidationEvent& event,
      memory::ResultCursorPlanMemoryGovernor* governor);
  OptimizerPlanCacheInvalidationResult ShrinkGovernedMemory(
      const std::string& database_id,
      std::uint64_t target_bytes,
      memory::ResultCursorPlanMemoryGovernor* governor);
  OptimizerPlanCachePersistenceEnvelope ExportPersistenceEnvelope(
      const OptimizerPlanCachePersistenceRequest& request) const;
  OptimizerPlanCacheEnterpriseValidation ImportPersistenceEnvelope(
      const OptimizerPlanCachePersistenceEnvelope& envelope);
  void Clear();
  OptimizerPlanCacheStats Stats() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, CachedOptimizerPlan> plans_;
  OptimizerPlanCacheStats stats_;
};

std::string BuildOptimizerPlanCacheKey(const OptimizerPlanCacheKeyInput& input);
std::string BuildNormalizedOptimizerPolicyControlDigest(
    const scratchbird::engine::planner::OptimizerPolicyMetadata& policy);
OptimizerPlanCacheKeyInput BuildOptimizerPlanCacheKeyInput(const BoundOptimizerRequest& request,
                                                           std::string cost_profile_id,
                                                           std::vector<std::string> object_uuids = {},
                                                           std::vector<std::string> function_uuids = {},
                                                           std::vector<std::string> index_uuids = {},
                                                           std::vector<std::string> filespace_uuids = {});
OptimizerProductionPlanCacheKeyResult BuildProductionOptimizerPlanCacheKeyInput(
    const OptimizerProductionPlanCacheKeyRequest& request);
bool OptimizerPlanDependsOnEvent(const CachedOptimizerPlan& plan, const OptimizerInvalidationEvent& event);
bool OptimizerInvalidationEventKindRecognized(const std::string& event_kind);
std::string OptimizerInvalidationDiagnosticCode(const OptimizerInvalidationEvent& event);
OptimizerInvalidationEvent OptimizerInvalidationEventForMutation(std::string mutation_source,
                                                                 std::string dependency_uuid,
                                                                 std::uint64_t event_epoch);
OptimizerPlanCacheEnterpriseValidation ValidateEnterpriseOptimizerPlanCacheKeyInput(
    const OptimizerPlanCacheKeyInput& input);
OptimizerPlanCacheEnterpriseValidation ValidateEnterpriseCachedOptimizerPlan(
    const CachedOptimizerPlan& plan);
std::string BuildOptimizerPlanCachePersistenceDigest(
    const OptimizerPlanCachePersistenceEnvelope& envelope);

}  // namespace scratchbird::engine::optimizer

namespace scratchbird::engine::internal_api {

// Implemented by the canonical query-plan API owner. The optimizer cache
// contract intentionally exposes only optimizer/executor types here, so the
// optimizer layer does not depend on an internal-API header.
scratchbird::engine::optimizer::CanonicalExecutablePlanHitExecutionResult
ExecuteCanonicalExecutablePlanCacheHit(
    const scratchbird::engine::optimizer::
        CanonicalExecutablePlanHitExecutionRequest& request);

scratchbird::engine::optimizer::
    CanonicalExecutablePlanGovernedExecutionResult
ExecuteCanonicalExecutablePlanAfterSingleReprepare(
    const scratchbird::engine::optimizer::
        CanonicalExecutablePlanGovernedExecutionRequest& request);

scratchbird::engine::optimizer::CanonicalExplainResult
RenderCanonicalStoredPlanExplain(
    const scratchbird::engine::optimizer::CanonicalExplainRequest& request);

}  // namespace scratchbird::engine::internal_api
