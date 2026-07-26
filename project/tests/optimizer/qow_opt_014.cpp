// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_catalog_backed_planning.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-014-V1: " << detail << '\n';
  return condition;
}

std::string Uuid(const std::uint64_t suffix) {
  auto text = std::string("019f0000-0000-7400-8000-000000000000");
  const auto digits = std::to_string(suffix);
  text.replace(text.size() - digits.size(), digits.size(), digits);
  return text;
}

plan::CanonicalLogicalRelationalNode Node(
    const std::uint32_t id,
    const plan::CanonicalLogicalRelationalNodeKind kind,
    std::vector<std::uint32_t> inputs,
    const std::uint32_t descriptor,
    std::string semantic_variant) {
  plan::CanonicalLogicalRelationalNode node;
  node.logical_node_id = id;
  node.node_kind = kind;
  node.input_logical_node_ids = std::move(inputs);
  node.output_descriptor_ids = {descriptor};
  node.origin_relational_node_ids = {id};
  node.semantic_variant_id = std::move(semantic_variant);
  return node;
}

plan::CanonicalPhysicalAlternativeRecord Alternative(
    const std::uint64_t ordinal,
    const std::uint32_t node_id,
    std::string implementation_id,
    const std::uint32_t descriptor) {
  plan::CanonicalPhysicalAlternativeRecord alternative;
  alternative.alternative_uuid = Uuid(200 + ordinal);
  alternative.logical_node_id = node_id;
  alternative.implementation_id = std::move(implementation_id);
  alternative.capability_uuid = Uuid(300 + ordinal);
  alternative.output_descriptor_ids = {descriptor};
  alternative.available = true;
  return alternative;
}

opt::CanonicalOptimizerAdmissionRequest Request() {
  opt::CanonicalOptimizerAdmissionRequest request;
  auto& graph = request.logical_graph;
  graph.bound_sblr_tree_uuid = Uuid(1);
  graph.catalog_epoch_uuid = Uuid(2);
  graph.security_context_uuid = Uuid(3);
  graph.local_transaction_id = 701;
  graph.statement_snapshot_id = 699;
  graph.root_logical_node_id = 4;
  graph.result_descriptor_ids = {104};
  graph.nodes = {
      Node(1, plan::CanonicalLogicalRelationalNodeKind::kValues, {}, 101,
           "values.literal-table.v1"),
      Node(2, plan::CanonicalLogicalRelationalNodeKind::kValues, {}, 102,
           "values.literal-table.v1"),
      Node(3, plan::CanonicalLogicalRelationalNodeKind::kJoin, {1, 2}, 103,
           "join.inner.v1"),
      Node(4, plan::CanonicalLogicalRelationalNodeKind::kProject, {3}, 104,
           "project.bound-expressions.v1"),
  };

  auto& properties = request.logical_properties;
  properties.bound_sblr_tree_uuid = Uuid(1);
  properties.catalog_epoch_uuid = Uuid(2);
  properties.security_context_uuid = Uuid(3);
  properties.local_transaction_id = 701;
  properties.statement_snapshot_id = 699;

  request.catalog.snapshot_uuid = Uuid(2);
  request.catalog.catalog_epoch_uuid = Uuid(2);
  request.catalog.catalog_generation = 31;
  request.catalog.descriptor_ids = {101, 102, 103, 104};
  request.catalog.engine_owned = true;

  request.security.security_context_uuid = Uuid(3);
  request.security.security_epoch = 32;
  request.security.policy_epoch = 33;
  request.security.catalog_generation = 31;
  request.security.engine_owned = true;

  request.mga.local_transaction_id = 701;
  request.mga.statement_snapshot_id = 699;
  request.mga.metadata_snapshot_uuid = Uuid(2);
  request.mga.transaction_active = true;
  request.mga.statement_snapshot_fixed = true;
  request.mga.engine_owned = true;

  request.policy_capability.policy_snapshot_uuid = Uuid(3);
  request.policy_capability.policy_epoch = 33;
  request.policy_capability.capability_snapshot_uuid = Uuid(5);
  request.policy_capability.capability_abi_version = 1;
  request.policy_capability.supported_node_kinds = {
      plan::CanonicalLogicalRelationalNodeKind::kValues,
      plan::CanonicalLogicalRelationalNodeKind::kJoin,
      plan::CanonicalLogicalRelationalNodeKind::kProject,
  };
  request.policy_capability.engine_owned = true;

  request.resource.resource_snapshot_uuid = Uuid(6);
  request.resource.resource_epoch = 34;
  request.resource.memory_budget_bytes = 1'000'000;
  request.resource.maximum_candidate_count = 16;
  request.resource.maximum_memo_groups = 8;
  request.resource.maximum_search_steps = 1'000;
  request.resource.maximum_planning_time_ns = 1'000'000;
  request.resource.spill_allowed = true;
  request.resource.engine_owned = true;

  auto& statistics = request.statistics;
  statistics.statistics_snapshot_uuid = Uuid(7);
  statistics.catalog_epoch_uuid = Uuid(2);
  statistics.statistics_generation = 35;
  statistics.admitted_at_monotonic_ns = 700'000;
  statistics.captured_before_data_access = true;
  for (const auto& node : graph.nodes) {
    opt::CanonicalOptimizerNodeEstimate estimate;
    estimate.logical_node_id = node.logical_node_id;
    estimate.statistics_snapshot_uuid = Uuid(7);
    estimate.catalog_epoch_uuid = Uuid(2);
    estimate.statistics_generation = 35;
    estimate.admitted_at_monotonic_ns = 700'000;
    if (node.node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kValues) {
      estimate.state = opt::CanonicalOptimizerStatisticState::kNotApplicable;
      estimate.source = opt::CanonicalOptimizerStatisticSource::kUnavailable;
      estimate.confidence = opt::CostConfidence::kUnknown;
    } else {
      estimate.state = opt::CanonicalOptimizerStatisticState::kUnknown;
      estimate.source = opt::CanonicalOptimizerStatisticSource::kUnavailable;
      estimate.confidence = opt::CostConfidence::kUnknown;
    }
    statistics.node_estimates.push_back(std::move(estimate));
  }

  request.route.route_snapshot_uuid = Uuid(8);
  request.route.route_epoch = 36;
  request.route.route_generation = 37;
  request.route.operation_id = "query.execute";
  request.route.route_id = "native.sblr.query.execute.v2";
  request.route.native_local_route = true;
  request.route.engine_owned = true;
  request.populated_from_admitted_typed_sblr = true;
  return request;
}

plan::CanonicalPhysicalAlternativeCatalog Alternatives() {
  plan::CanonicalPhysicalAlternativeCatalog catalog;
  catalog.bound_sblr_tree_uuid = Uuid(1);
  catalog.catalog_epoch_uuid = Uuid(2);
  catalog.security_context_uuid = Uuid(3);
  catalog.local_transaction_id = 701;
  catalog.statement_snapshot_id = 699;
  catalog.alternatives = {
      Alternative(1, 1, "values.materialize.v1", 101),
      Alternative(2, 2, "values.materialize.v1", 102),
      Alternative(3, 3, "join.hash.v1", 103),
      Alternative(4, 3, "join.merge.v1", 103),
      Alternative(5, 3, "join.nested-loop.v1", 103),
      Alternative(6, 4, "project.direct.v1", 104),
      Alternative(7, 4, "project.vector.v1", 104),
  };
  return catalog;
}

opt::CanonicalOptimizerSearchCandidateInput Candidate(
    const std::uint64_t ordinal, const std::uint64_t score) {
  opt::CanonicalOptimizerSearchCandidateInput candidate;
  candidate.alternative_uuid = Uuid(200 + ordinal);
  candidate.transformation_uuid = Uuid(400 + ordinal);
  candidate.transformation_rule_id =
      "canonical.transform." + std::to_string(ordinal) + ".v1";
  candidate.bound_sblr_tree_uuid = Uuid(1);
  candidate.statistics_snapshot_uuid = Uuid(7);
  candidate.statistics_generation = 35;
  candidate.model_family_id = "relational.local.v1";
  candidate.cost_terms.cost_vector_uuid = Uuid(500 + ordinal);
  candidate.cost_terms.calibration_profile_uuid = Uuid(600);
  candidate.cost_terms.cpu_units = score;
  candidate.cost_terms.page_read_sequential_units = ordinal;
  candidate.cost_terms.memory_bytes_required = 100 + ordinal;
  candidate.cost_terms.confidence = opt::CostConfidence::kExact;
  candidate.semantic_preserving = true;
  candidate.derived_from_admitted_statistics = true;
  candidate.engine_coster_owned = true;
  return candidate;
}

std::vector<opt::CanonicalOptimizerSearchCandidateInput> Candidates() {
  return {
      Candidate(1, 1), Candidate(2, 2), Candidate(3, 20), Candidate(4, 10),
      Candidate(5, 30), Candidate(6, 8), Candidate(7, 3),
  };
}

opt::CanonicalOptimizerSearchPolicy Policy() {
  opt::CanonicalOptimizerSearchPolicy policy;
  policy.maximum_exhaustive_plan_count = 64;
  policy.bounded_beam_width = 2;
  policy.deterministic_step_cost_ns = 10;
  policy.engine_owned = true;
  return policy;
}

bool SameTrace(const std::vector<opt::CanonicalOptimizerSearchTraceRecord>& a,
               const std::vector<opt::CanonicalOptimizerSearchTraceRecord>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t index = 0; index < a.size(); ++index) {
    const auto& left = a[index];
    const auto& right = b[index];
    if (left.step_ordinal != right.step_ordinal ||
        left.event_id != right.event_id ||
        left.logical_node_id != right.logical_node_id ||
        left.frontier_size != right.frontier_size ||
        left.pruned_count != right.pruned_count ||
        left.detail != right.detail) {
      return false;
    }
  }
  return true;
}

bool ValidateExhaustiveSearch() {
  const auto request = Request();
  const auto admission = opt::AdmitCanonicalOptimizerPlanningRequest(request);
  auto alternatives = Alternatives();
  auto candidates = Candidates();
  const auto result = opt::SearchCanonicalRelationalMemo(
      request, admission, alternatives, candidates, Policy());
  bool passed = true;
  passed &= Require(admission.admitted && admission.planning_allowed,
                    "canonical optimizer admission fixture was refused");
  passed &= Require(
      result.accepted && result.selected && result.issues.empty() &&
          result.mode == opt::CanonicalOptimizerSearchMode::kExhaustiveSmall &&
          result.memo_group_count == 4 && result.legal_candidate_count == 7 &&
          result.complete_plan_space_count == 6 &&
          !result.complete_plan_space_count_saturated &&
          result.exhaustive_oracle_executed &&
          result.exhaustive_oracle_agreed && result.resource_bounded &&
          result.deterministic && !result.physical_dag_published &&
          !result.data_access_allowed && result.selected_alternatives.size() == 4,
      "small legal plan space was not exhaustively and independently proved");
  passed &= Require(
      result.selected_plan_signature ==
          "1=" + Uuid(201) + ";2=" + Uuid(202) + ";3=" + Uuid(204) +
              ";4=" + Uuid(207) + ";",
      "exhaustive search selected the wrong deterministic plan");

  std::reverse(alternatives.alternatives.begin(),
               alternatives.alternatives.end());
  std::reverse(candidates.begin(), candidates.end());
  const auto reordered = opt::SearchCanonicalRelationalMemo(
      request, admission, alternatives, candidates, Policy());
  passed &= Require(reordered.accepted &&
                        reordered.selected_plan_signature ==
                            result.selected_plan_signature &&
                        reordered.selected_scalar_score ==
                            result.selected_scalar_score &&
                        SameTrace(reordered.trace, result.trace),
                    "input order changed deterministic exhaustive search");
  return passed;
}

bool ValidateBoundedSearch() {
  const auto request = Request();
  const auto admission = opt::AdmitCanonicalOptimizerPlanningRequest(request);
  auto policy = Policy();
  policy.maximum_exhaustive_plan_count = 1;
  const auto first = opt::SearchCanonicalRelationalMemo(
      request, admission, Alternatives(), Candidates(), policy);
  const auto second = opt::SearchCanonicalRelationalMemo(
      request, admission, Alternatives(), Candidates(), policy);
  bool passed = true;
  passed &= Require(
      first.accepted && first.selected &&
          first.mode ==
              opt::CanonicalOptimizerSearchMode::kDeterministicBounded &&
          !first.exhaustive_oracle_executed &&
          !first.exhaustive_oracle_agreed && first.pruned_plan_count != 0 &&
          first.resource_bounded && first.deterministic &&
          !first.physical_dag_published && !first.data_access_allowed,
      "large plan-space policy did not use bounded deterministic search");
  passed &= Require(first.selected_plan_signature ==
                        second.selected_plan_signature &&
                        first.selected_scalar_score ==
                            second.selected_scalar_score &&
                        SameTrace(first.trace, second.trace),
                    "bounded search was not repeatable");
  return passed;
}

bool ValidateAtomicRefusals() {
  const auto expect_refusal = [](auto request, auto admission,
                                 auto alternatives, auto candidates,
                                 auto policy, const std::string_view diagnostic,
                                 const std::string_view detail) {
    const auto result = opt::SearchCanonicalRelationalMemo(
        request, admission, alternatives, candidates, policy);
    return Require(!result.accepted && !result.selected &&
                       !result.physical_dag_published &&
                       !result.data_access_allowed && result.memo_groups.empty() &&
                       result.selected_alternatives.empty() &&
                       result.trace.empty() && result.issues.size() == 1 &&
                       result.issues.front().diagnostic_id == diagnostic,
                   detail);
  };

  bool passed = true;
  auto request = Request();
  auto admission = opt::AdmitCanonicalOptimizerPlanningRequest(request);
  auto alternatives = Alternatives();
  auto candidates = Candidates();
  auto policy = Policy();

  auto missing = candidates;
  missing.pop_back();
  passed &= expect_refusal(
      request, admission, alternatives, missing, policy,
      "QOW-DIAG-OPTIMIZER-SEARCH-COST-COVERAGE-V1",
      "missing cost record was admitted");

  auto unsafe = candidates;
  unsafe[2].semantic_preserving = false;
  passed &= expect_refusal(
      request, admission, alternatives, unsafe, policy,
      "QOW-DIAG-OPTIMIZER-SEARCH-TRANSFORMATION-V1",
      "non-semantic transformation was admitted");

  auto nonlocal = candidates;
  nonlocal[2].cost_terms.network_bytes_expected = 1;
  passed &= expect_refusal(
      request, admission, alternatives, nonlocal, policy,
      "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
      "unsupported cross-model network cost was admitted");

  auto stale_cost = candidates;
  stale_cost[2].statistics_generation -= 1;
  passed &= expect_refusal(
      request, admission, alternatives, stale_cost, policy,
      "QOW-DIAG-OPTIMIZER-SEARCH-COST-PROVENANCE-V1",
      "cost derived from a stale statistics generation was admitted");

  auto incomparable = candidates;
  incomparable[2].cost_terms.calibration_profile_uuid = Uuid(601);
  passed &= expect_refusal(
      request, admission, alternatives, incomparable, policy,
      "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
      "incomparable calibration profiles were ranked together");

  auto unbounded = request;
  unbounded.resource.maximum_search_steps = 4;
  const auto bounded_admission =
      opt::AdmitCanonicalOptimizerPlanningRequest(unbounded);
  passed &= Require(bounded_admission.admitted,
                    "resource refusal fixture did not pass admission");
  passed &= expect_refusal(
      unbounded, bounded_admission, alternatives, candidates, policy,
      "QOW-DIAG-OPTIMIZER-SEARCH-RESOURCE-V1",
      "search emitted a partial plan after exhausting its step budget");

  admission.admitted = false;
  admission.planning_allowed = false;
  passed &= expect_refusal(
      request, admission, alternatives, candidates, policy,
      "QOW-DIAG-OPTIMIZER-SEARCH-ADMISSION-V1",
      "unadmitted optimizer request reached search");

  auto admitted_request = Request();
  auto stale_admission =
      opt::AdmitCanonicalOptimizerPlanningRequest(admitted_request);
  admitted_request.resource.resource_epoch += 1;
  passed &= expect_refusal(
      admitted_request, stale_admission, alternatives, candidates, policy,
      "QOW-DIAG-OPTIMIZER-SEARCH-ADMISSION-V1",
      "stale admission evidence was reused for a changed request");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-014-V1
int main() {
  bool passed = true;
  passed &= ValidateExhaustiveSearch();
  passed &= ValidateBoundedSearch();
  passed &= ValidateAtomicRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
