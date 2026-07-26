// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_contract.hpp"
#include "optimizer_catalog_backed_planning.hpp"

#ifndef SCRATCHBIRD_QOW_CANONICAL_CANDIDATE_LEGALITY_ONLY
#include "join_planner_full.hpp"
#include "relational_planner.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;

// QOW-SOURCE-OPT-005-V1
CanonicalOptimizerStatisticsAdmissionResult
AdmitCanonicalOptimizerStatisticsBeforeAccess(
    const planner::CanonicalLogicalRelationalGraph& graph,
    const CanonicalOptimizerStatisticsSnapshot& snapshot) {
  CanonicalOptimizerStatisticsAdmissionResult result;
  const auto refuse = [&](std::string diagnostic_id,
                          const std::uint32_t logical_node_id,
                          std::string field_id) {
    result.issues.push_back({std::move(diagnostic_id), logical_node_id,
                             std::move(field_id)});
    result.accepted = false;
    result.benchmark_clean_ready = false;
    result.data_access_allowed = false;
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
    return true;
  };

  const auto graph_validation =
      planner::ValidateCanonicalLogicalRelationalGraph(graph);
  if (!graph_validation.accepted) {
    const auto& issue = graph_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.logical_node_id, issue.field_id);
  }
  if (snapshot.abi_version != 1) {
    return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-VERSION-V1", 0,
                  "abi_version");
  }
  if (!canonical_uuid(snapshot.statistics_snapshot_uuid) ||
      snapshot.statistics_snapshot_uuid == graph.bound_sblr_tree_uuid ||
      snapshot.catalog_epoch_uuid != graph.catalog_epoch_uuid ||
      snapshot.statistics_generation == 0 ||
      snapshot.admitted_at_monotonic_ns == 0) {
    return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-SCOPE-V1", 0,
                  "statistics_snapshot_identity");
  }
  if (!snapshot.captured_before_data_access || snapshot.data_access_observed ||
      snapshot.runtime_actuals_present ||
      snapshot.parser_statistics_authority_claimed) {
    return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-PHASE-V1", 0,
                  "pre_access_statistics_boundary");
  }
  if (snapshot.node_estimates.size() != graph.nodes.size()) {
    return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-COVERAGE-V1", 0,
                  "node_estimate_count");
  }

  std::unordered_map<std::uint32_t,
                     const planner::CanonicalLogicalRelationalNode*>
      nodes;
  for (const auto& node : graph.nodes) nodes.emplace(node.logical_node_id, &node);
  std::unordered_set<std::uint32_t> seen_nodes;
  for (const auto& estimate : snapshot.node_estimates) {
    const auto node_it = nodes.find(estimate.logical_node_id);
    if (node_it == nodes.end() ||
        !seen_nodes.insert(estimate.logical_node_id).second) {
      return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-COVERAGE-V1",
                    estimate.logical_node_id, "logical_node_id");
    }
    const auto& node = *node_it->second;
    if (estimate.statistics_snapshot_uuid !=
            snapshot.statistics_snapshot_uuid ||
        estimate.catalog_epoch_uuid != snapshot.catalog_epoch_uuid ||
        estimate.statistics_generation != snapshot.statistics_generation ||
        estimate.admitted_at_monotonic_ns !=
            snapshot.admitted_at_monotonic_ns) {
      return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-SCOPE-V1",
                    estimate.logical_node_id, "estimate_snapshot_identity");
    }
    if (estimate.derived_from_runtime_actuals ||
        estimate.benchmark_clean_authority_claimed) {
      return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-AUTHORITY-V1",
                    estimate.logical_node_id,
                    "runtime_or_benchmark_authority_claim");
    }

    switch (estimate.state) {
      case CanonicalOptimizerStatisticState::kKnown: {
        const bool catalog_source =
            estimate.source ==
                CanonicalOptimizerStatisticSource::kCatalogExact ||
            estimate.source ==
                CanonicalOptimizerStatisticSource::kCatalogSample;
        const bool object_bound =
            !estimate.object_uuid.empty() &&
            std::ranges::find(node.required_object_uuids,
                              estimate.object_uuid) !=
                node.required_object_uuids.end();
        const bool fresh =
            estimate.collected_at_monotonic_ns != 0 &&
            estimate.collected_at_monotonic_ns <=
                estimate.admitted_at_monotonic_ns &&
            estimate.maximum_age_ns != 0 &&
            estimate.admitted_at_monotonic_ns -
                    estimate.collected_at_monotonic_ns <=
                estimate.maximum_age_ns;
        if (!catalog_source || !object_bound || !fresh ||
            estimate.confidence == CostConfidence::kUnknown ||
            estimate.confidence == CostConfidence::kRejected ||
            (!estimate.row_count_present && !estimate.page_count_present)) {
          return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-PROVENANCE-V1",
                        estimate.logical_node_id, "known_estimate");
        }
        ++result.known_estimate_count;
        break;
      }
      case CanonicalOptimizerStatisticState::kUnknown:
        if (estimate.source !=
                CanonicalOptimizerStatisticSource::kUnavailable ||
            estimate.confidence != CostConfidence::kUnknown ||
            estimate.row_count_present || estimate.page_count_present ||
            estimate.row_count != 0 || estimate.page_count != 0 ||
            estimate.collected_at_monotonic_ns != 0 ||
            estimate.maximum_age_ns != 0) {
          return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-UNKNOWN-V1",
                        estimate.logical_node_id, "unknown_estimate");
        }
        ++result.unknown_estimate_count;
        result.degraded_for_unknown_statistics = true;
        break;
      case CanonicalOptimizerStatisticState::kNotApplicable:
        if (node.node_kind !=
                planner::CanonicalLogicalRelationalNodeKind::kValues ||
            !estimate.object_uuid.empty() ||
            estimate.source !=
                CanonicalOptimizerStatisticSource::kUnavailable ||
            estimate.confidence != CostConfidence::kUnknown ||
            estimate.row_count_present || estimate.page_count_present ||
            estimate.collected_at_monotonic_ns != 0 ||
            estimate.maximum_age_ns != 0) {
          return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-NOT-APPLICABLE-V1",
                        estimate.logical_node_id,
                        "not_applicable_estimate");
        }
        ++result.not_applicable_estimate_count;
        break;
      default:
        return refuse("QOW-DIAG-OPTIMIZER-STATISTICS-STATE-V1",
                      estimate.logical_node_id, "statistic_state");
    }
  }

  result.accepted = true;
  result.benchmark_clean_ready =
      !result.degraded_for_unknown_statistics &&
      result.known_estimate_count != 0;
  result.data_access_allowed = false;
  return result;
}

// QOW-SOURCE-OPT-011-V1
CanonicalRelationalCandidateLegalityResult
EvaluateCanonicalRelationalCandidateLegality(
    const planner::CanonicalLogicalRelationalGraph& graph,
    const planner::CanonicalLogicalPropertyCatalog& properties,
    const planner::CanonicalPhysicalAlternativeCatalog& alternatives) {
  CanonicalRelationalCandidateLegalityResult result;
  const auto refuse = [&](std::string diagnostic_id,
                          const std::uint32_t node_id,
                          std::string alternative_uuid,
                          std::string field_id) {
    result.accepted = false;
    result.data_access_allowed = false;
    result.selectable_candidate_count = 0;
    result.candidates.clear();
    result.issues.push_back({std::move(diagnostic_id), node_id,
                             std::move(alternative_uuid),
                             std::move(field_id)});
    return result;
  };
  const auto property_validation =
      planner::ValidateCanonicalLogicalPropertyCatalog(graph, properties);
  if (!property_validation.accepted) {
    const auto& issue = property_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.logical_node_id, {},
                  issue.field_id);
  }
  const auto alternative_validation =
      planner::ValidateCanonicalLogicalPhysicalBoundary(graph, alternatives);
  if (!alternative_validation.accepted) {
    const auto& issue = alternative_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.logical_node_id, {},
                  issue.field_id);
  }

  std::unordered_map<std::uint32_t,
                     const planner::CanonicalLogicalRelationalNode*>
      nodes_by_id;
  for (const auto& node : graph.nodes) {
    nodes_by_id.emplace(node.logical_node_id, &node);
  }
  std::unordered_map<std::string,
                     const planner::CanonicalLogicalPropertyRecord*>
      properties_by_uuid;
  for (const auto& property : properties.properties) {
    properties_by_uuid.emplace(property.property_uuid, &property);
  }
  std::unordered_map<std::uint32_t, std::size_t> legal_candidates_by_node;

  const auto node_has_delivered_kind =
      [&](const planner::CanonicalLogicalRelationalNode& node,
          const planner::CanonicalLogicalPropertyKind kind) {
        return std::ranges::any_of(
            node.delivered_property_uuids,
            [&](const std::string& property_uuid) {
              return properties_by_uuid.at(property_uuid)->property_kind ==
                     kind;
            });
      };
  for (const auto& node : graph.nodes) {
    if (node.node_kind ==
            planner::CanonicalLogicalRelationalNodeKind::kSort &&
        !node_has_delivered_kind(
            node, planner::CanonicalLogicalPropertyKind::kOrdering)) {
      return refuse(
          "QOW-DIAG-CANDIDATE-LOGICAL-PROPERTY-UNAVAILABLE-V1",
          node.logical_node_id, {}, "sort_ordering_property");
    }
    if (node.node_kind ==
            planner::CanonicalLogicalRelationalNodeKind::kWindow &&
        !node_has_delivered_kind(
            node, planner::CanonicalLogicalPropertyKind::kWindow)) {
      return refuse(
          "QOW-DIAG-CANDIDATE-LOGICAL-PROPERTY-UNAVAILABLE-V1",
          node.logical_node_id, {}, "window_property");
    }
    if (node.node_kind ==
        planner::CanonicalLogicalRelationalNodeKind::kAggregate) {
      for (const auto& [property_uuid, property] : properties_by_uuid) {
        if (property->origin_logical_node_id == node.logical_node_id &&
            property->property_kind ==
                planner::CanonicalLogicalPropertyKind::kGrouping &&
            std::ranges::find(node.delivered_property_uuids, property_uuid) ==
                node.delivered_property_uuids.end()) {
          return refuse(
              "QOW-DIAG-CANDIDATE-LOGICAL-PROPERTY-UNAVAILABLE-V1",
              node.logical_node_id, {}, "aggregate_grouping_property");
        }
      }
    }
    if (node.node_kind ==
        planner::CanonicalLogicalRelationalNodeKind::kWindow) {
      for (const auto& delivered_uuid : node.delivered_property_uuids) {
        const auto* property = properties_by_uuid.at(delivered_uuid);
        if (property->property_kind !=
            planner::CanonicalLogicalPropertyKind::kWindow) {
          continue;
        }
        for (const auto& dependency_uuid :
             property->dependency_property_uuids) {
          if (std::ranges::find(node.required_property_uuids,
                                dependency_uuid) ==
              node.required_property_uuids.end()) {
            return refuse(
                "QOW-DIAG-CANDIDATE-LOGICAL-PROPERTY-UNAVAILABLE-V1",
                node.logical_node_id, {}, "window_required_property");
          }
        }
      }
    }
  }

  constexpr std::size_t kMaximumAlternativePropertyReferences = 1048576;
  std::size_t alternative_property_reference_count = 0;
  for (const auto& alternative : alternatives.alternatives) {
    if (alternative.required_property_uuids.size() >
            kMaximumAlternativePropertyReferences -
                alternative_property_reference_count) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    alternative.logical_node_id,
                    alternative.alternative_uuid,
                    "alternative_property_reference_count");
    }
    alternative_property_reference_count +=
        alternative.required_property_uuids.size();
    if (alternative.delivered_property_uuids.size() >
            kMaximumAlternativePropertyReferences -
                alternative_property_reference_count) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    alternative.logical_node_id,
                    alternative.alternative_uuid,
                    "alternative_property_reference_count");
    }
    alternative_property_reference_count +=
        alternative.delivered_property_uuids.size();
    const auto* node = nodes_by_id.at(alternative.logical_node_id);
    std::unordered_set<std::string> input_delivered;
    for (const auto input_id : node->input_logical_node_ids) {
      const auto* input = nodes_by_id.at(input_id);
      input_delivered.insert(input->delivered_property_uuids.begin(),
                             input->delivered_property_uuids.end());
    }
    std::unordered_set<std::string> candidate_required;
    std::unordered_set<std::string> candidate_delivered;
    for (const auto& property_uuid : alternative.required_property_uuids) {
      if (!properties_by_uuid.contains(property_uuid) ||
          !candidate_required.insert(property_uuid).second) {
        return refuse("QOW-DIAG-CANDIDATE-PROPERTY-REFERENCE-V1",
                      node->logical_node_id, alternative.alternative_uuid,
                      "required_property_uuids");
      }
    }
    for (const auto& property_uuid : alternative.delivered_property_uuids) {
      if (!properties_by_uuid.contains(property_uuid) ||
          !candidate_delivered.insert(property_uuid).second ||
          std::ranges::find(node->delivered_property_uuids, property_uuid) ==
              node->delivered_property_uuids.end()) {
        return refuse("QOW-DIAG-CANDIDATE-PROPERTY-REFERENCE-V1",
                      node->logical_node_id, alternative.alternative_uuid,
                      "delivered_property_uuids");
      }
    }

    CanonicalRelationalCandidateLegalityRecord candidate;
    candidate.alternative_uuid = alternative.alternative_uuid;
    candidate.logical_node_id = alternative.logical_node_id;
    for (const auto& required_uuid : candidate_required) {
      if (!input_delivered.contains(required_uuid)) {
        candidate.missing_property_uuids.push_back(required_uuid);
      }
    }
    for (const auto& required_uuid : node->required_property_uuids) {
      if (input_delivered.contains(required_uuid)) continue;
      if (candidate_delivered.contains(required_uuid)) {
        candidate.enforced_property_uuids.push_back(required_uuid);
      } else {
        candidate.missing_property_uuids.push_back(required_uuid);
      }
    }
    for (const auto& delivered_uuid : node->delivered_property_uuids) {
      if (!candidate_delivered.contains(delivered_uuid)) {
        candidate.missing_property_uuids.push_back(delivered_uuid);
      }
    }
    std::ranges::sort(candidate.missing_property_uuids);
    candidate.missing_property_uuids.erase(
        std::unique(candidate.missing_property_uuids.begin(),
                    candidate.missing_property_uuids.end()),
        candidate.missing_property_uuids.end());
    std::ranges::sort(candidate.enforced_property_uuids);
    candidate.property_enforcement_required =
        !candidate.enforced_property_uuids.empty();
    candidate.legal =
        alternative.available && candidate.missing_property_uuids.empty();
    if (!alternative.available) {
      candidate.refusal_diagnostic_id =
          alternative.refusal_diagnostic_id;
    } else if (!candidate.legal) {
      candidate.refusal_diagnostic_id =
          "QOW-DIAG-CANDIDATE-PROPERTY-UNAVAILABLE-V1";
    } else {
      ++result.selectable_candidate_count;
      ++legal_candidates_by_node[alternative.logical_node_id];
    }
    result.candidates.push_back(std::move(candidate));
  }
  result.accepted = true;
  result.data_access_allowed = false;
  result.complete_legal_coverage =
      std::ranges::all_of(graph.nodes, [&](const auto& node) {
        return legal_candidates_by_node[node.logical_node_id] != 0;
      });
  if (!result.complete_legal_coverage) {
    for (const auto& node : graph.nodes) {
      if (legal_candidates_by_node[node.logical_node_id] == 0) {
        result.issues.push_back(
            {"QOW-DIAG-NO-LEGAL-PROPERTY-CANDIDATE-V1",
             node.logical_node_id, {}, "property_coverage"});
      }
    }
  }
  return result;
}

// QOW-SOURCE-OPT-014-V1
CanonicalOptimizerSearchResult SearchCanonicalRelationalMemo(
    const CanonicalOptimizerAdmissionRequest& admission_request,
    const CanonicalOptimizerAdmissionResult& admission,
    const planner::CanonicalPhysicalAlternativeCatalog& alternatives,
    const std::vector<CanonicalOptimizerSearchCandidateInput>& candidates,
    const CanonicalOptimizerSearchPolicy& policy) {
  CanonicalOptimizerSearchResult result;
  const auto refuse = [&](std::string diagnostic_id,
                          const std::uint32_t logical_node_id,
                          std::string alternative_uuid,
                          std::string field_id) {
    result = {};
    result.issues.push_back({std::move(diagnostic_id), logical_node_id,
                             std::move(alternative_uuid),
                             std::move(field_id)});
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
    return true;
  };
  const auto valid_rule_id = [](const std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::ranges::all_of(value, [](const unsigned char ch) {
             return (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                    ch == '-';
           });
  };
  const auto checked_add = [](const std::uint64_t left,
                              const std::uint64_t right,
                              std::uint64_t* out) {
    if (out == nullptr ||
        std::numeric_limits<std::uint64_t>::max() - left < right) {
      return false;
    }
    *out = left + right;
    return true;
  };

  if (!admission.admitted || !admission.planning_allowed ||
      admission.data_access_allowed || !admission.issues.empty() ||
      admission.evidence.size() != 8) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-ADMISSION-V1", 0, {},
                  "optimizer_admission");
  }
  if (admission.bound_sblr_tree_uuid !=
          admission_request.logical_graph.bound_sblr_tree_uuid ||
      admission.catalog_epoch_uuid !=
          admission_request.logical_graph.catalog_epoch_uuid ||
      admission.security_context_uuid !=
          admission_request.logical_graph.security_context_uuid ||
      admission.capability_snapshot_uuid !=
          admission_request.policy_capability.capability_snapshot_uuid ||
      admission.resource_snapshot_uuid !=
          admission_request.resource.resource_snapshot_uuid ||
      admission.statistics_snapshot_uuid !=
          admission_request.statistics.statistics_snapshot_uuid ||
      admission.route_snapshot_uuid !=
          admission_request.route.route_snapshot_uuid ||
      admission.local_transaction_id !=
          admission_request.logical_graph.local_transaction_id ||
      admission.statement_snapshot_id !=
          admission_request.logical_graph.statement_snapshot_id ||
      admission.catalog_generation !=
          admission_request.catalog.catalog_generation ||
      admission.security_epoch != admission_request.security.security_epoch ||
      admission.policy_epoch != admission_request.security.policy_epoch ||
      admission.resource_epoch != admission_request.resource.resource_epoch ||
      admission.statistics_generation !=
          admission_request.statistics.statistics_generation ||
      admission.route_epoch != admission_request.route.route_epoch ||
      admission.route_generation !=
          admission_request.route.route_generation) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-ADMISSION-V1", 0, {},
                  "optimizer_admission_scope");
  }
  for (std::size_t index = 0; index < admission.evidence.size(); ++index) {
    if (admission.evidence[index].stage !=
        static_cast<CanonicalOptimizerAdmissionStage>(index + 1)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-ADMISSION-V1", 0, {},
                    "optimizer_admission_order");
    }
  }
  if (policy.abi_version != 1 || !policy.engine_owned ||
      policy.maximum_exhaustive_plan_count == 0 ||
      policy.bounded_beam_width == 0 ||
      policy.deterministic_step_cost_ns == 0 ||
      policy.allow_cross_model_cost_comparison ||
      policy.parser_search_authority_claimed ||
      policy.transaction_finality_claimed) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-POLICY-V1", 0, {},
                  "deterministic_search_policy");
  }
  const auto& resource = admission_request.resource;
  if (!resource.engine_owned || resource.memory_budget_bytes == 0 ||
      resource.maximum_candidate_count == 0 ||
      resource.maximum_memo_groups == 0 ||
      resource.maximum_search_steps == 0 ||
      resource.maximum_planning_time_ns == 0 ||
      policy.bounded_beam_width > resource.maximum_candidate_count ||
      admission_request.logical_graph.nodes.size() >
          resource.maximum_memo_groups ||
      alternatives.alternatives.size() >
          resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-RESOURCE-V1", 0, {},
                  "memo_candidate_resource_budget");
  }
  const auto time_step_budget =
      resource.maximum_planning_time_ns / policy.deterministic_step_cost_ns;
  const auto search_step_budget =
      std::min(resource.maximum_search_steps, time_step_budget);
  if (search_step_budget == 0) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-RESOURCE-V1", 0, {},
                  "deterministic_step_budget");
  }

  const auto legality = EvaluateCanonicalRelationalCandidateLegality(
      admission_request.logical_graph, admission_request.logical_properties,
      alternatives);
  if (!legality.accepted || !legality.complete_legal_coverage) {
    if (!legality.issues.empty()) {
      const auto& issue = legality.issues.front();
      return refuse(issue.diagnostic_id, issue.logical_node_id,
                    issue.alternative_uuid, issue.field_id);
    }
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-LEGALITY-V1", 0, {},
                  "complete_legal_coverage");
  }
  if (legality.selectable_candidate_count >
          resource.maximum_candidate_count ||
      candidates.size() != legality.selectable_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-COVERAGE-V1", 0, {},
                  "legal_candidate_cost_count");
  }

  std::unordered_map<std::string,
                     const planner::CanonicalPhysicalAlternativeRecord*>
      alternatives_by_uuid;
  for (const auto& alternative : alternatives.alternatives) {
    alternatives_by_uuid.emplace(alternative.alternative_uuid, &alternative);
  }
  std::unordered_set<std::string> legal_alternative_uuids;
  for (const auto& record : legality.candidates) {
    if (record.legal) legal_alternative_uuids.insert(record.alternative_uuid);
  }
  std::unordered_map<std::string,
                     const CanonicalOptimizerSearchCandidateInput*>
      candidate_inputs;
  std::unordered_set<std::string> transformation_uuids;
  std::unordered_set<std::string> cost_vector_uuids;
  std::optional<std::string> calibration_profile_uuid;
  for (const auto& candidate : candidates) {
    const auto alternative_it =
        alternatives_by_uuid.find(candidate.alternative_uuid);
    if (alternative_it == alternatives_by_uuid.end() ||
        !legal_alternative_uuids.contains(candidate.alternative_uuid) ||
        !candidate_inputs.emplace(candidate.alternative_uuid, &candidate)
             .second) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-COVERAGE-V1", 0,
                    candidate.alternative_uuid, "alternative_uuid");
    }
    const auto node_id = alternative_it->second->logical_node_id;
    if (!canonical_uuid(candidate.transformation_uuid) ||
        !transformation_uuids.insert(candidate.transformation_uuid).second ||
        !valid_rule_id(candidate.transformation_rule_id) ||
        !candidate.semantic_preserving) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-TRANSFORMATION-V1", node_id,
                    candidate.alternative_uuid,
                    "semantic_transformation_authority");
    }
    if (candidate.bound_sblr_tree_uuid !=
            admission.bound_sblr_tree_uuid ||
        candidate.statistics_snapshot_uuid !=
            admission.statistics_snapshot_uuid ||
        candidate.statistics_generation != admission.statistics_generation ||
        candidate.model_family_id != "relational.local.v1" ||
        !candidate.derived_from_admitted_statistics ||
        !candidate.engine_coster_owned ||
        candidate.parser_or_reference_cost_authority_claimed ||
        candidate.benchmark_authority_claimed) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-PROVENANCE-V1", node_id,
                    candidate.alternative_uuid,
                    "admitted_cost_provenance");
    }
    const auto& terms = candidate.cost_terms;
    if (!canonical_uuid(terms.cost_vector_uuid) ||
        !cost_vector_uuids.insert(terms.cost_vector_uuid).second ||
        !canonical_uuid(terms.calibration_profile_uuid) ||
        (calibration_profile_uuid.has_value() &&
         terms.calibration_profile_uuid != *calibration_profile_uuid) ||
        terms.confidence == CostConfidence::kUnknown ||
        terms.confidence == CostConfidence::kRejected ||
        ((terms.confidence == CostConfidence::kMedium ||
          terms.confidence == CostConfidence::kLow) &&
         terms.uncertainty_penalty == 0) ||
        terms.memory_bytes_required > resource.memory_budget_bytes ||
        (!resource.spill_allowed && terms.spill_bytes_expected != 0) ||
        terms.network_bytes_expected != 0 ||
        terms.archive_fetches_expected != 0) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1", node_id,
                    candidate.alternative_uuid, "cost_vector_terms");
    }
    if (!calibration_profile_uuid.has_value()) {
      calibration_profile_uuid = terms.calibration_profile_uuid;
    }
    const auto node_it = std::ranges::find_if(
        admission_request.logical_graph.nodes, [&](const auto& node) {
          return node.logical_node_id == node_id;
        });
    if (node_it == admission_request.logical_graph.nodes.end() ||
        (node_it->node_kind ==
             planner::CanonicalLogicalRelationalNodeKind::kRelationSource &&
         terms.mga_visibility_checks_expected == 0)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1", node_id,
                    candidate.alternative_uuid,
                    "mga_visibility_cost_terms");
    }
  }

  std::vector<std::uint32_t> node_ids;
  node_ids.reserve(admission_request.logical_graph.nodes.size());
  for (const auto& node : admission_request.logical_graph.nodes) {
    node_ids.push_back(node.logical_node_id);
  }
  std::ranges::sort(node_ids);
  result.memo_groups.reserve(node_ids.size());
  std::uint64_t total_legal_candidates = 0;
  std::uint64_t plan_space_count = 1;
  bool plan_space_saturated = false;
  for (const auto node_id : node_ids) {
    CanonicalOptimizerMemoGroup group;
    group.logical_node_id = node_id;
    for (const auto& legality_record : legality.candidates) {
      if (!legality_record.legal ||
          legality_record.logical_node_id != node_id) {
        continue;
      }
      const auto candidate_it =
          candidate_inputs.find(legality_record.alternative_uuid);
      if (candidate_it == candidate_inputs.end()) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-COVERAGE-V1", node_id,
                      legality_record.alternative_uuid,
                      "legal_candidate_cost");
      }
      const auto& candidate = *candidate_it->second;
      std::uint64_t scalar_score = 0;
      const auto add_term = [&](const std::uint64_t term) {
        return checked_add(scalar_score, term, &scalar_score);
      };
      const auto& terms = candidate.cost_terms;
      if (!add_term(terms.cpu_units) ||
          !add_term(terms.page_read_sequential_units) ||
          !add_term(terms.page_read_random_units) ||
          !add_term(terms.page_write_units) ||
          !add_term(terms.memory_bytes_required) ||
          !add_term(terms.spill_bytes_expected) ||
          !add_term(terms.network_bytes_expected) ||
          !add_term(terms.mga_visibility_checks_expected) ||
          !add_term(terms.archive_fetches_expected) ||
          !add_term(terms.uncertainty_penalty) ||
          !add_term(terms.risk_penalty)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1", node_id,
                      candidate.alternative_uuid, "scalar_score");
      }
      group.candidates.push_back(
          {candidate.alternative_uuid, candidate.transformation_uuid,
           candidate.transformation_rule_id, {terms, scalar_score}});
    }
    std::ranges::sort(group.candidates, {},
                      &CanonicalOptimizerMemoCandidate::alternative_uuid);
    if (group.candidates.empty()) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-LEGALITY-V1", node_id, {},
                    "memo_group_candidates");
    }
    if (!checked_add(total_legal_candidates, group.candidates.size(),
                     &total_legal_candidates)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-RESOURCE-V1", node_id, {},
                    "legal_candidate_count");
    }
    if (group.candidates.size() != 0 &&
        plan_space_count > std::numeric_limits<std::uint64_t>::max() /
                               group.candidates.size()) {
      plan_space_count = std::numeric_limits<std::uint64_t>::max();
      plan_space_saturated = true;
    } else if (!plan_space_saturated) {
      plan_space_count *= group.candidates.size();
    }
    result.memo_groups.push_back(std::move(group));
  }
  result.memo_group_count = result.memo_groups.size();
  result.legal_candidate_count = total_legal_candidates;
  result.complete_plan_space_count = plan_space_count;
  result.complete_plan_space_count_saturated = plan_space_saturated;

  struct FrontierPlan {
    std::uint64_t scalar_score{0};
    std::uint64_t io_units{0};
    std::uint64_t memory_bytes{0};
    std::string signature;
    std::vector<const CanonicalOptimizerMemoCandidate*> selected;
  };
  const auto plan_less = [](const FrontierPlan& left,
                            const FrontierPlan& right) {
    if (left.scalar_score != right.scalar_score) {
      return left.scalar_score < right.scalar_score;
    }
    if (left.io_units != right.io_units) {
      return left.io_units < right.io_units;
    }
    if (left.memory_bytes != right.memory_bytes) {
      return left.memory_bytes < right.memory_bytes;
    }
    return left.signature < right.signature;
  };

  const bool exhaustive = !plan_space_saturated &&
                          plan_space_count <=
                              policy.maximum_exhaustive_plan_count;
  result.mode = exhaustive
                    ? CanonicalOptimizerSearchMode::kExhaustiveSmall
                    : CanonicalOptimizerSearchMode::kDeterministicBounded;
  std::vector<FrontierPlan> frontier(1);
  for (const auto& group : result.memo_groups) {
    std::vector<FrontierPlan> expanded;
    const auto maximum_expanded =
        exhaustive ? std::numeric_limits<std::uint64_t>::max()
                   : policy.bounded_beam_width;
    for (const auto& prior : frontier) {
      for (const auto& candidate : group.candidates) {
        if (result.search_step_count == search_step_budget) {
          return refuse("QOW-DIAG-OPTIMIZER-SEARCH-RESOURCE-V1",
                        group.logical_node_id, candidate.alternative_uuid,
                        "search_step_budget");
        }
        ++result.search_step_count;
        FrontierPlan plan = prior;
        std::uint64_t io_units = 0;
        const auto& terms = candidate.cost.terms;
        if (!checked_add(terms.page_read_sequential_units,
                         terms.page_read_random_units, &io_units) ||
            !checked_add(io_units, terms.page_write_units, &io_units) ||
            !checked_add(plan.scalar_score, candidate.cost.scalar_score,
                         &plan.scalar_score) ||
            !checked_add(plan.io_units, io_units, &plan.io_units) ||
            !checked_add(plan.memory_bytes, terms.memory_bytes_required,
                         &plan.memory_bytes)) {
          return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                        group.logical_node_id, candidate.alternative_uuid,
                        "plan_cost_vector");
        }
        plan.signature += std::to_string(group.logical_node_id) + "=" +
                          candidate.alternative_uuid + ";";
        plan.selected.push_back(&candidate);
        expanded.push_back(std::move(plan));
      }
    }
    std::ranges::sort(expanded, plan_less);
    std::uint64_t pruned = 0;
    if (!exhaustive && expanded.size() > maximum_expanded) {
      pruned = expanded.size() - maximum_expanded;
      expanded.resize(static_cast<std::size_t>(maximum_expanded));
      if (!checked_add(result.pruned_plan_count, pruned,
                       &result.pruned_plan_count)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-RESOURCE-V1",
                      group.logical_node_id, {}, "pruned_plan_count");
      }
    }
    frontier = std::move(expanded);
    result.trace.push_back(
        {result.search_step_count,
         exhaustive ? "memo.exhaustive.expand.v1" : "memo.bounded.expand.v1",
         group.logical_node_id, frontier.size(), pruned,
         "deterministic_candidate_uuid_order"});
  }
  if (frontier.empty()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1", 0, {},
                  "search_frontier");
  }
  std::ranges::sort(frontier, plan_less);
  const auto selected = frontier.front();

  if (exhaustive) {
    result.exhaustive_oracle_executed = true;
    std::optional<FrontierPlan> oracle_best;
    FrontierPlan current;
    bool oracle_budget_exhausted = false;
    bool oracle_cost_overflow = false;
    std::function<void(std::size_t)> enumerate = [&](const std::size_t index) {
      if (oracle_budget_exhausted || oracle_cost_overflow) return;
      if (index == result.memo_groups.size()) {
        if (result.search_step_count == search_step_budget) {
          oracle_budget_exhausted = true;
          return;
        }
        ++result.search_step_count;
        if (!oracle_best || plan_less(current, *oracle_best)) {
          oracle_best = current;
        }
        return;
      }
      const auto& group = result.memo_groups[index];
      for (const auto& candidate : group.candidates) {
        const auto prior = current;
        std::uint64_t io_units = 0;
        const auto& terms = candidate.cost.terms;
        if (!checked_add(terms.page_read_sequential_units,
                         terms.page_read_random_units, &io_units) ||
            !checked_add(io_units, terms.page_write_units, &io_units) ||
            !checked_add(current.scalar_score, candidate.cost.scalar_score,
                         &current.scalar_score) ||
            !checked_add(current.io_units, io_units, &current.io_units) ||
            !checked_add(current.memory_bytes, terms.memory_bytes_required,
                         &current.memory_bytes)) {
          oracle_cost_overflow = true;
          current = prior;
          return;
        }
        current.signature += std::to_string(group.logical_node_id) + "=" +
                             candidate.alternative_uuid + ";";
        current.selected.push_back(&candidate);
        enumerate(index + 1);
        current = prior;
      }
    };
    enumerate(0);
    if (oracle_budget_exhausted) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-RESOURCE-V1", 0, {},
                    "exhaustive_oracle_step_budget");
    }
    if (oracle_cost_overflow || !oracle_best) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1", 0, {},
                    "exhaustive_oracle_cost");
    }
    result.exhaustive_oracle_agreed =
        oracle_best->signature == selected.signature &&
        oracle_best->scalar_score == selected.scalar_score;
    if (!result.exhaustive_oracle_agreed) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-ORACLE-V1", 0, {},
                    "exhaustive_oracle_disagreement");
    }
    result.trace.push_back(
        {result.search_step_count, "memo.exhaustive.oracle-agreed.v1", 0,
         frontier.size(), 0, oracle_best->signature});
  }

  result.selected_alternatives.reserve(selected.selected.size());
  for (std::size_t index = 0; index < selected.selected.size(); ++index) {
    const auto* candidate = selected.selected[index];
    result.selected_alternatives.push_back(
        {result.memo_groups[index].logical_node_id,
         candidate->alternative_uuid, candidate->transformation_uuid,
         candidate->transformation_rule_id, candidate->cost});
  }
  result.accepted = true;
  result.selected = true;
  result.resource_bounded = true;
  result.deterministic = true;
  result.physical_dag_published = false;
  result.data_access_allowed = false;
  result.selected_scalar_score = selected.scalar_score;
  result.selected_plan_signature = selected.signature;
  result.trace.push_back(
      {result.search_step_count, "memo.plan.selected.v1", 0,
       selected.selected.size(), result.pruned_plan_count,
       selected.signature});
  return result;
}

#ifndef SCRATCHBIRD_QOW_CANONICAL_CANDIDATE_LEGALITY_ONLY
namespace {

constexpr std::uint64_t kMaxCost = std::numeric_limits<std::uint64_t>::max();
// SEARCH_KEY: ORH_CATALOG_BACKED_PUBLIC_SQL_ROUTE
constexpr std::string_view kCatalogBackedProfile = "catalog_backed_access_path_v1";
// SEARCH_KEY: ORH_STATISTICS_ONLY_OPTIMIZER_QUARANTINE
constexpr std::string_view kStatisticsOnlyNotBenchmarkClean =
    "SB_ORH_CATALOG_BACKED_PUBLIC_SQL_ROUTE.STATISTICS_ONLY_NOT_BENCHMARK_CLEAN";
constexpr std::string_view kCatalogFactsRequired =
    "SB_ORH_CATALOG_BACKED_PUBLIC_SQL_ROUTE.CATALOG_FACTS_REQUIRED";

std::uint64_t SaturatingAdd(std::uint64_t lhs, std::uint64_t rhs) {
  if (kMaxCost - lhs < rhs) return kMaxCost;
  return lhs + rhs;
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      default: out << ch;
    }
  }
  return out.str();
}

bool HasDescriptor(const planner::LogicalPlanNode& node, std::string_view descriptor) {
  return std::find(node.required_descriptors.begin(), node.required_descriptors.end(), descriptor) !=
         node.required_descriptors.end();
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsJoinAccessKind(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kJoinNestedLoop ||
         access_kind == planner::PhysicalAccessKind::kJoinHash ||
         access_kind == planner::PhysicalAccessKind::kJoinMerge;
}

bool IsAggregateAccessKind(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kAggregateGeneric ||
         access_kind == planner::PhysicalAccessKind::kAggregateHash;
}

bool IsWindowAccessKind(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kSortThenWindow;
}

bool IsSortLimitAccessKind(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kSort ||
         access_kind == planner::PhysicalAccessKind::kTopN;
}

bool IsUpperOperatorAccessKind(planner::PhysicalAccessKind access_kind) {
  return IsAggregateAccessKind(access_kind) ||
         IsWindowAccessKind(access_kind) ||
         IsSortLimitAccessKind(access_kind) ||
         access_kind == planner::PhysicalAccessKind::kCteInline ||
         access_kind == planner::PhysicalAccessKind::kCteMaterialize ||
         access_kind == planner::PhysicalAccessKind::kSetOperation;
}

bool IsSetOperationAccessKind(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kSetOperation;
}

bool IsScanAccessKind(planner::PhysicalAccessKind access_kind) {
  switch (access_kind) {
    case planner::PhysicalAccessKind::kCatalogUuidLookup:
    case planner::PhysicalAccessKind::kTableScan:
    case planner::PhysicalAccessKind::kRowUuidLookup:
    case planner::PhysicalAccessKind::kScalarBtreeLookup:
    case planner::PhysicalAccessKind::kScalarHashLookup:
    case planner::PhysicalAccessKind::kScalarBtreeRange:
    case planner::PhysicalAccessKind::kCoveringIndexScan:
    case planner::PhysicalAccessKind::kBitmapSummaryScan:
    case planner::PhysicalAccessKind::kFullTextProbe:
    case planner::PhysicalAccessKind::kVectorExactSearch:
    case planner::PhysicalAccessKind::kVectorApproximateWithFallback:
    case planner::PhysicalAccessKind::kDocumentPathProbe:
    case planner::PhysicalAccessKind::kGraphTraversalSeed:
    case planner::PhysicalAccessKind::kTimeSeriesAppendPath:
    case planner::PhysicalAccessKind::kClusterFragmentScan:
    case planner::PhysicalAccessKind::kRemoteNodePushdown:
      return true;
    case planner::PhysicalAccessKind::kNone:
    case planner::PhysicalAccessKind::kJoinNestedLoop:
    case planner::PhysicalAccessKind::kJoinHash:
    case planner::PhysicalAccessKind::kJoinMerge:
    case planner::PhysicalAccessKind::kAggregateGeneric:
    case planner::PhysicalAccessKind::kAggregateHash:
    case planner::PhysicalAccessKind::kSort:
    case planner::PhysicalAccessKind::kTopN:
    case planner::PhysicalAccessKind::kSortThenWindow:
    case planner::PhysicalAccessKind::kCteInline:
    case planner::PhysicalAccessKind::kCteMaterialize:
    case planner::PhysicalAccessKind::kSetOperation:
      return false;
  }
  return false;
}

bool IsBaseLogicalNode(const planner::LogicalPlanNode& node) {
  return (node.kind == planner::LogicalPlanNodeKind::kDmlRead ||
          node.kind == planner::LogicalPlanNodeKind::kNoSqlOperation ||
          node.kind == planner::LogicalPlanNodeKind::kCatalogLookup) &&
         !IsJoinAccessKind(node.access_kind) &&
         !IsUpperOperatorAccessKind(node.access_kind);
}

bool PlanHasBaseForObject(const planner::LogicalPlan& plan, const std::string& object_uuid) {
  return std::any_of(plan.nodes.begin(), plan.nodes.end(), [&](const planner::LogicalPlanNode& node) {
    return IsBaseLogicalNode(node) &&
           std::find(node.required_object_uuids.begin(), node.required_object_uuids.end(), object_uuid) !=
               node.required_object_uuids.end();
  });
}

std::string RequiredObjectUuid(const planner::LogicalPlanNode& node) {
  return node.required_object_uuids.empty() ? "local.default" : node.required_object_uuids.front();
}

std::string RelationKeyForNode(const planner::LogicalPlanNode& node) {
  if (!node.required_object_uuids.empty()) return node.required_object_uuids.front();
  if (!node.operation_id.empty()) return node.operation_id;
  return node.stable_name.empty() ? "local.default" : node.stable_name;
}

std::string DescriptorDigestForNode(const planner::LogicalPlanNode& node) {
  for (const auto& descriptor : node.required_descriptors) {
    if (StartsWith(descriptor, "desc.") || StartsWith(descriptor, "descriptor.")) return descriptor;
  }
  if (!node.required_descriptors.empty()) return node.required_descriptors.front();
  if (!node.stable_name.empty()) return node.stable_name;
  return node.operation_id.empty() ? "logical_node" : node.operation_id;
}

std::uint64_t EstimateRowsForNode(const OptimizerStatisticsCatalog& statistics,
                                  const planner::LogicalPlanNode& node,
                                  std::uint64_t fallback) {
  const std::string object_uuid = RequiredObjectUuid(node);
  auto rows = statistics.EstimateUnsigned("visible_row_count", object_uuid, 0);
  if (rows == 0) rows = statistics.EstimateUnsigned("row_count", object_uuid, 0);
  if (rows == 0) rows = statistics.EstimateUnsigned("visible_row_count", "local.default", 0);
  if (rows == 0) rows = statistics.EstimateUnsigned("row_count", "local.default", 0);
  return rows == 0 ? fallback : rows;
}

void FinishCost(CostVector* cost) {
  cost->total_cost = SaturatingAdd(
      SaturatingAdd(SaturatingAdd(cost->startup_cost, cost->row_cost),
                    SaturatingAdd(cost->io_cost, cost->memory_cost)),
      cost->uncertainty_cost);
}

void AddCost(CostVector* destination, const CostVector& source) {
  destination->startup_cost = SaturatingAdd(destination->startup_cost, source.startup_cost);
  destination->row_cost = SaturatingAdd(destination->row_cost, source.row_cost);
  destination->io_cost = SaturatingAdd(destination->io_cost, source.io_cost);
  destination->memory_cost = SaturatingAdd(destination->memory_cost, source.memory_cost);
  destination->uncertainty_cost = SaturatingAdd(destination->uncertainty_cost, source.uncertainty_cost);
  destination->selectable = destination->selectable && source.selectable;
  if (!source.rejection_reason.empty() && destination->rejection_reason.empty()) {
    destination->rejection_reason = source.rejection_reason;
  }
  if (source.confidence > destination->confidence) {
    destination->confidence = source.confidence;
  }
  FinishCost(destination);
}

PlanCandidate MakeDecisionCandidate(std::string candidate_id,
                                    planner::PhysicalAccessKind access_kind,
                                    CostVector cost,
                                    std::uint64_t estimated_rows) {
  PlanCandidate candidate;
  candidate.candidate_id = std::move(candidate_id);
  candidate.access_kind = access_kind;
  candidate.scope = "local";
  candidate.cost = std::move(cost);
  candidate.estimated_rows = estimated_rows;
  if (!candidate.cost.selectable && !candidate.cost.rejection_reason.empty()) {
    candidate.refusal_reasons.push_back(candidate.cost.rejection_reason);
  }
  return candidate;
}

void AppendOptimizerCandidate(OptimizedPlan* optimized,
                              const planner::LogicalPlanNode& node,
                              PlanCandidate plan_candidate,
                              std::string statistics_version) {
  OptimizerCandidate candidate;
  candidate.node = node;
  plan_candidate.selected = false;
  candidate.plan_candidate = std::move(plan_candidate);
  candidate.cost = candidate.plan_candidate.cost;
  candidate.rejected = !candidate.plan_candidate.cost.selectable;
  candidate.rejection_reason = candidate.plan_candidate.cost.rejection_reason;
  candidate.statistics_version = std::move(statistics_version);
  optimized->candidates.push_back(std::move(candidate));
}

std::string StatisticsVersionForCandidate(const PlanCandidate& candidate) {
  return candidate.uses_local_default_statistics ? "local.default:epoch1" : "catalog-scoped:epoch1";
}

bool HasAnyDescriptor(const planner::LogicalPlanNode& node, std::initializer_list<std::string_view> descriptors) {
  return std::any_of(descriptors.begin(), descriptors.end(), [&](std::string_view descriptor) {
    return HasDescriptor(node, descriptor);
  });
}

bool JoinNodeHasSemanticBarrier(const planner::LogicalPlanNode& node) {
  return HasAnyDescriptor(node,
                          {"join.preserve_order",
                           "join.outer",
                           "join.left_outer",
                           "join.right_outer",
                           "join.full_outer",
                           "join.semi",
                           "join.anti",
                           "join.nullable",
                           "join.correlated",
                           "join.correlation",
                           "join.lateral",
                           "join.volatile",
                           "join.barrier"});
}

JoinSemanticKind SemanticKindForJoinNode(const planner::LogicalPlanNode& node) {
  if (HasDescriptor(node, "join.left_outer") || HasDescriptor(node, "join.outer")) {
    return JoinSemanticKind::kLeftOuter;
  }
  if (HasDescriptor(node, "join.right_outer")) return JoinSemanticKind::kRightOuter;
  if (HasDescriptor(node, "join.full_outer")) return JoinSemanticKind::kFullOuter;
  if (HasDescriptor(node, "join.semi")) return JoinSemanticKind::kSemi;
  if (HasDescriptor(node, "join.anti")) return JoinSemanticKind::kAnti;
  return JoinSemanticKind::kInner;
}

std::uint64_t CardinalityForRelationUuid(const OptimizerStatisticsCatalog& statistics,
                                         const std::string& relation_uuid) {
  auto value = statistics.EstimateUnsigned("row_count", relation_uuid, 0);
  if (value == 0) value = statistics.EstimateUnsigned("visible_row_count", relation_uuid, 0);
  if (value == 0) value = statistics.EstimateUnsigned("row_count", "local.default", 0);
  if (value == 0) value = statistics.EstimateUnsigned("visible_row_count", "local.default", 0);
  return std::max<std::uint64_t>(1, value);
}

std::vector<std::string> JoinRelationKeysForNode(const planner::LogicalPlanNode& node) {
  if (!node.required_object_uuids.empty()) return node.required_object_uuids;
  return {"local.default", "local.default"};
}

double JoinSelectivityForNode(const planner::LogicalPlanNode& node,
                              const OptimizerStatisticsCatalog& statistics) {
  const auto statistic = statistics.Find("join_selectivity", node.operation_id);
  if (statistic && statistic->available) return std::clamp(statistic->value, 0.000001, 1.0);
  const auto fallback = statistics.Find("join_selectivity", "local.default");
  if (fallback && fallback->available) return std::clamp(fallback->value, 0.000001, 1.0);
  return HasDescriptor(node, "join.non_equi") ? 0.25 : 0.10;
}

JoinGraph JoinGraphForNode(const planner::LogicalPlanNode& node,
                           const OptimizerStatisticsCatalog& statistics) {
  const auto relation_keys = JoinRelationKeysForNode(node);
  std::vector<JoinRelationNode> relations;
  relations.reserve(relation_keys.size());
  for (const auto& relation_key : relation_keys) {
    JoinRelationNode relation;
    relation.relation_uuid = relation_key;
    relation.estimated_rows = CardinalityForRelationUuid(statistics, relation_key);
    relation.order_preserving_required = HasDescriptor(node, "join.inputs_ordered");
    relation.semantic_order_barrier = HasDescriptor(node, "join.barrier") ||
                                      HasDescriptor(node, "join.preserve_order");
    relation.correlated_dependency = HasDescriptor(node, "join.correlated") ||
                                     HasDescriptor(node, "join.correlation");
    relation.lateral_dependency = HasDescriptor(node, "join.lateral");
    relation.volatile_dependency = HasDescriptor(node, "join.volatile");
    relations.push_back(std::move(relation));
  }

  std::vector<JoinPredicateEdge> predicates;
  const bool equality = !HasDescriptor(node, "join.non_equi");
  const auto semantic_kind = SemanticKindForJoinNode(node);
  const auto selectivity = JoinSelectivityForNode(node, statistics);
  for (std::size_t i = 1; i < relation_keys.size(); ++i) {
    JoinPredicateEdge edge;
    edge.left_relation_uuid = relation_keys[i - 1];
    edge.right_relation_uuid = relation_keys[i];
    edge.predicate_kind = equality ? "join.equi" : "join.non_equi";
    edge.semantic_kind = semantic_kind;
    edge.equality = equality;
    edge.nullable = HasDescriptor(node, "join.nullable");
    edge.outer_join_sensitive = HasAnyDescriptor(node, {"join.outer",
                                                        "join.left_outer",
                                                        "join.right_outer",
                                                        "join.full_outer"});
    edge.correlated = HasDescriptor(node, "join.correlated") ||
                      HasDescriptor(node, "join.correlation");
    edge.lateral = HasDescriptor(node, "join.lateral");
    edge.volatile_predicate = HasDescriptor(node, "join.volatile");
    edge.explicit_order_barrier = HasDescriptor(node, "join.barrier") ||
                                  HasDescriptor(node, "join.preserve_order");
    edge.selectivity = selectivity;
    predicates.push_back(std::move(edge));
  }

  return BuildJoinGraph(std::move(relations),
                        std::move(predicates),
                        HasAnyDescriptor(node, {"join.outer",
                                                "join.left_outer",
                                                "join.right_outer",
                                                "join.full_outer"}),
                        HasAnyDescriptor(node, {"join.semi", "join.anti"}));
}

JoinPlanningInput JoinInputForNode(const planner::LogicalPlanNode& node,
                                  const OptimizerStatisticsCatalog& statistics) {
  JoinPlanningInput join_input;
  const std::string left_uuid = node.required_object_uuids.empty()
                                    ? "local.default"
                                    : node.required_object_uuids.front();
  const std::string right_uuid = node.required_object_uuids.size() < 2
                                     ? "local.default"
                                     : node.required_object_uuids[1];
  join_input.left_cardinality = statistics.EstimateUnsigned("row_count", left_uuid, 0);
  join_input.right_cardinality = statistics.EstimateUnsigned("row_count", right_uuid, 0);
  join_input.equi_join = !HasDescriptor(node, "join.non_equi");
  join_input.reorder_safe = !JoinNodeHasSemanticBarrier(node);
  join_input.ordered_inputs = node.access_kind == planner::PhysicalAccessKind::kJoinMerge ||
                              HasDescriptor(node, "join.inputs_ordered");
  join_input.memory_budget_bytes = statistics.EstimateUnsigned("memory_grant_available_bytes",
                                                               "local.default",
                                                               1048576);
  if (join_input.left_cardinality == 0 || join_input.right_cardinality == 0 ||
      statistics.ConfidenceFor("row_count", left_uuid) == CostConfidence::kUnknown ||
      statistics.ConfidenceFor("row_count", right_uuid) == CostConfidence::kUnknown) {
    join_input.reorder_safe = false;
    join_input.hash_join_executor_available = false;
    join_input.merge_join_executor_available = false;
  }
  return join_input;
}

void AppendJoinCandidates(OptimizedPlan* optimized,
                          const planner::LogicalPlanNode& node,
                          const OptimizerStatisticsCatalog& statistics) {
  const auto decision = PlanLocalJoin(JoinInputForNode(node, statistics));
  const auto graph = JoinGraphForNode(node, statistics);
  const auto memory_budget = statistics.EstimateUnsigned("memory_grant_available_bytes",
                                                         "local.default",
                                                         1048576);
  const auto order_plan = EnumerateDeterministicJoinOrder(graph, memory_budget);
  for (auto plan_candidate : decision.candidates) {
    plan_candidate.statistics_diagnostics.insert(plan_candidate.statistics_diagnostics.end(),
                                                 order_plan.diagnostics.begin(),
                                                 order_plan.diagnostics.end());
    if (order_plan.ok && plan_candidate.cost.selectable &&
        plan_candidate.access_kind == order_plan.method) {
      plan_candidate.statistics_diagnostics.push_back("SB_OPT_JOIN_DP_METHOD_SELECTED");
      plan_candidate.cost = order_plan.cost;
      plan_candidate.estimated_rows = order_plan.estimated_rows;
    }
    AppendOptimizerCandidate(optimized, node, std::move(plan_candidate), "join-local:epoch1");
  }
  optimized->diagnostics.insert(optimized->diagnostics.end(),
                                decision.diagnostics.begin(),
                                decision.diagnostics.end());
  optimized->diagnostics.insert(optimized->diagnostics.end(),
                                order_plan.diagnostics.begin(),
                                order_plan.diagnostics.end());
}

void AppendRelationalCandidate(OptimizedPlan* optimized,
                               const planner::LogicalPlanNode& node,
                               const OptimizerStatisticsCatalog& statistics) {
  const std::uint64_t input_rows = EstimateRowsForNode(statistics, node, 1000);
  const std::uint64_t row_width = statistics.EstimateUnsigned("average_row_bytes",
                                                             RequiredObjectUuid(node),
                                                             64);
  const std::uint64_t memory_budget = statistics.EstimateUnsigned("memory_grant_available_bytes",
                                                                 "local.default",
                                                                 1048576);
  if (IsAggregateAccessKind(node.access_kind)) {
    AggregatePlanningInput input;
    input.input_rows = input_rows;
    input.group_count = statistics.EstimateUnsigned("group_count",
                                                    RequiredObjectUuid(node),
                                                    std::max<std::uint64_t>(1, input_rows / 10));
    input.row_width_bytes = row_width;
    input.memory_budget_bytes = memory_budget;
    input.grouping_present = node.access_kind == planner::PhysicalAccessKind::kAggregateHash ||
                             HasDescriptor(node, "aggregate.grouping");
    input.distinct_present = HasDescriptor(node, "aggregate.distinct");
    input.input_ordered_by_group = HasDescriptor(node, "aggregate.input_ordered_by_group");
    const auto decision = PlanAggregate(input);
    auto candidate = MakeDecisionCandidate("CAND-ODF-017-AGGREGATE",
                                           decision.access_kind,
                                           decision.cost,
                                           input.grouping_present || input.distinct_present
                                               ? std::max<std::uint64_t>(1, input.group_count)
                                               : 1);
    candidate.statistics_diagnostics = decision.diagnostics;
    AppendOptimizerCandidate(optimized, node, std::move(candidate), "relational-upper:epoch1");
    return;
  }

  if (IsWindowAccessKind(node.access_kind)) {
    WindowPlanningInput input;
    input.input_rows = input_rows;
    input.partition_count = statistics.EstimateUnsigned("window_partition_count",
                                                        RequiredObjectUuid(node),
                                                        std::max<std::uint64_t>(1, input_rows / 100));
    input.input_ordered = HasDescriptor(node, "window.input_ordered");
    input.frame_requires_materialization = node.access_kind == planner::PhysicalAccessKind::kSortThenWindow ||
                                           HasDescriptor(node, "window.frame_materialization");
    const auto decision = PlanWindow(input);
    auto candidate = MakeDecisionCandidate("CAND-ODF-017-WINDOW",
                                           decision.access_kind,
                                           decision.cost,
                                           input_rows);
    candidate.statistics_diagnostics = decision.diagnostics;
    AppendOptimizerCandidate(optimized, node, std::move(candidate), "relational-upper:epoch1");
    return;
  }

  if (IsSortLimitAccessKind(node.access_kind)) {
    SortPlanningInput input;
    input.input_rows = input_rows;
    input.row_width_bytes = row_width;
    input.memory_budget_bytes = memory_budget;
    input.input_already_ordered = node.access_kind == planner::PhysicalAccessKind::kTopN ||
                                  HasDescriptor(node, "sort.input_ordered");
    input.limit_present = node.access_kind == planner::PhysicalAccessKind::kTopN ||
                          HasDescriptor(node, "limit.present");
    input.limit_count = statistics.EstimateUnsigned("limit_count",
                                                   RequiredObjectUuid(node),
                                                   10);
    const auto decision = PlanSortLimit(input);
    auto candidate = MakeDecisionCandidate(node.access_kind == planner::PhysicalAccessKind::kTopN
                                               ? "CAND-ODF-017-LIMIT"
                                               : "CAND-ODF-017-SORT",
                                           decision.access_kind,
                                           decision.cost,
                                           input.limit_present ? std::min(input.input_rows, input.limit_count)
                                                               : input.input_rows);
    candidate.statistics_diagnostics = decision.diagnostics;
    AppendOptimizerCandidate(optimized, node, std::move(candidate), "relational-upper:epoch1");
    return;
  }

  auto cost = EstimateNodeCost(node);
  AppendOptimizerCandidate(optimized,
                           node,
                           MakeDecisionCandidate("CAND-ODF-017-UPPER",
                                                 node.access_kind,
                                                 cost,
                                                 input_rows),
                           "relational-upper:epoch1");
}

struct LeafSelection {
  std::string key;
  std::size_t candidate_index = 0;
  PhysicalPlanNode node;
};

PhysicalPlanNode PhysicalNodeForCandidate(const OptimizerCandidate& candidate,
                                          std::string node_id_suffix = {}) {
  auto node = PhysicalPlanNodeFromCandidate(candidate.plan_candidate,
                                           RequiredExecutorCapabilityForAccessKind(candidate.plan_candidate.access_kind),
                                           DescriptorDigestForNode(candidate.node));
  if (!node_id_suffix.empty()) {
    node.node_id += ":" + node_id_suffix;
  }
  node.runtime_evidence.push_back("logical_operation_id=" + candidate.node.operation_id);
  node.runtime_evidence.push_back("logical_stable_name=" + candidate.node.stable_name);
  node.runtime_evidence.push_back("statistics_version=" + candidate.statistics_version);
  node.runtime_evidence.push_back("mga_visibility_authority=engine_transaction_inventory");
  node.runtime_evidence.push_back("visibility_recheck_preserved=true");
  if (!candidate.node.required_object_uuids.empty()) {
    node.runtime_evidence.push_back("base_relation_uuid=" + candidate.node.required_object_uuids.front());
  }
  return node;
}

std::vector<LeafSelection> SelectBaseLeaves(OptimizedPlan* optimized) {
  std::vector<LeafSelection> leaves;
  for (std::size_t i = 0; i < optimized->candidates.size(); ++i) {
    const auto& candidate = optimized->candidates[i];
    if (!candidate.cost.selectable || !IsScanAccessKind(candidate.plan_candidate.access_kind)) continue;
    const auto key = RelationKeyForNode(candidate.node);
    auto existing = std::find_if(leaves.begin(), leaves.end(), [&](const LeafSelection& leaf) {
      return leaf.key == key;
    });
    if (existing == leaves.end()) {
      LeafSelection leaf;
      leaf.key = key;
      leaf.candidate_index = i;
      leaves.push_back(std::move(leaf));
      continue;
    }
    if (IsBetterCost(candidate.cost, optimized->candidates[existing->candidate_index].cost)) {
      existing->candidate_index = i;
    }
  }

  for (auto& leaf : leaves) {
    auto& candidate = optimized->candidates[leaf.candidate_index];
    candidate.selected_in_physical_tree = true;
    leaf.node = PhysicalNodeForCandidate(candidate, leaf.key);
    leaf.node.runtime_evidence.push_back("physical_role=base_scan");
    leaf.node.runtime_evidence.push_back("relation_key=" + leaf.key);
  }
  return leaves;
}

const LeafSelection* FindLeaf(const std::vector<LeafSelection>& leaves, const std::string& key) {
  const auto found = std::find_if(leaves.begin(), leaves.end(), [&](const LeafSelection& leaf) {
    return leaf.key == key;
  });
  return found == leaves.end() ? nullptr : &*found;
}

std::optional<std::size_t> FindBestCandidateIndex(const OptimizedPlan& optimized,
                                                  const planner::LogicalPlanNode& node,
                                                  bool (*predicate)(planner::PhysicalAccessKind)) {
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < optimized.candidates.size(); ++i) {
    const auto& candidate = optimized.candidates[i];
    if (candidate.node.operation_id != node.operation_id ||
        candidate.node.stable_name != node.stable_name ||
        !predicate(candidate.plan_candidate.access_kind) ||
        !candidate.cost.selectable) {
      continue;
    }
    if (!best || IsBetterCost(candidate.cost, optimized.candidates[*best].cost)) {
      best = i;
    }
  }
  return best;
}

std::optional<std::size_t> FindBestJoinCandidateIndex(const OptimizedPlan& optimized,
                                                      const planner::LogicalPlanNode& node,
                                                      planner::PhysicalAccessKind preferred_access_kind) {
  std::optional<std::size_t> preferred;
  std::optional<std::size_t> fallback;
  for (std::size_t i = 0; i < optimized.candidates.size(); ++i) {
    const auto& candidate = optimized.candidates[i];
    if (candidate.node.operation_id != node.operation_id ||
        candidate.node.stable_name != node.stable_name ||
        !IsJoinAccessKind(candidate.plan_candidate.access_kind) ||
        !candidate.cost.selectable) {
      continue;
    }
    if (!fallback || IsBetterCost(candidate.cost, optimized.candidates[*fallback].cost)) {
      fallback = i;
    }
    if (candidate.plan_candidate.access_kind == preferred_access_kind &&
        (!preferred || IsBetterCost(candidate.cost, optimized.candidates[*preferred].cost))) {
      preferred = i;
    }
  }
  return preferred ? preferred : fallback;
}

bool ComposeJoinNode(OptimizedPlan* optimized,
                     const planner::LogicalPlanNode& logical_node,
                     const OptimizerStatisticsCatalog& statistics,
                     const std::vector<LeafSelection>& leaves,
                     std::optional<PhysicalPlanNode>* current) {
  const auto graph = JoinGraphForNode(logical_node, statistics);
  const auto memory_budget = statistics.EstimateUnsigned("memory_grant_available_bytes",
                                                         "local.default",
                                                         1048576);
  const auto order_plan = EnumerateDeterministicJoinOrder(graph, memory_budget);
  const auto candidate_index = FindBestJoinCandidateIndex(*optimized,
                                                         logical_node,
                                                         order_plan.ok ? order_plan.method
                                                                       : planner::PhysicalAccessKind::kJoinNestedLoop);
  if (!candidate_index) {
    optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_JOIN_CANDIDATE_MISSING");
    return false;
  }
  if (leaves.size() < 2 && logical_node.required_object_uuids.size() < 2) {
    optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_JOIN_INPUTS_MISSING");
    return false;
  }

  auto ordered_relation_uuids = order_plan.ordered_relation_uuids;
  if (ordered_relation_uuids.empty()) ordered_relation_uuids = JoinRelationKeysForNode(logical_node);
  std::vector<const LeafSelection*> ordered_leaves;
  ordered_leaves.reserve(ordered_relation_uuids.size());
  for (const auto& relation_uuid : ordered_relation_uuids) {
    const auto* leaf = FindLeaf(leaves, relation_uuid);
    if (leaf == nullptr) {
      optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_JOIN_LEAF_MISSING:" + relation_uuid);
      return false;
    }
    ordered_leaves.push_back(leaf);
  }
  if (ordered_leaves.size() < 2) {
    optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_JOIN_INPUTS_MISSING");
    return false;
  }

  auto& selected = optimized->candidates[*candidate_index];
  selected.selected_in_physical_tree = true;
  const bool reorder_safe = JoinReorderAllowed(graph);
  const auto join_order = [&]() {
    std::ostringstream out;
    for (std::size_t i = 0; i < ordered_relation_uuids.size(); ++i) {
      if (i != 0) out << ",";
      out << ordered_relation_uuids[i];
    }
    return out.str();
  }();

  auto build_join = [&](PhysicalPlanNode left,
                        PhysicalPlanNode right,
                        std::size_t step,
                        std::size_t total_steps) {
    auto join = PhysicalNodeForCandidate(selected,
                                         logical_node.operation_id + "." + std::to_string(step));
    join.children.push_back(std::move(left));
    join.children.push_back(std::move(right));
    join.storage_backed = false;
    join.preserves_visibility = join.children[0].preserves_visibility && join.children[1].preserves_visibility;
    join.preserves_order = selected.plan_candidate.access_kind == planner::PhysicalAccessKind::kJoinMerge &&
                           join.children[0].preserves_order &&
                           join.children[1].preserves_order;
    if (step == total_steps && order_plan.estimated_rows != 0) {
      join.estimated_rows = order_plan.estimated_rows;
    }
    AddCost(&join.cost, join.children[0].cost);
    AddCost(&join.cost, join.children[1].cost);
    join.runtime_evidence.push_back("physical_role=join");
    join.runtime_evidence.push_back(std::string("join_method=") +
                                    planner::PhysicalAccessKindName(selected.plan_candidate.access_kind));
    join.runtime_evidence.push_back("join_order=" + join_order);
    join.runtime_evidence.push_back(std::string("join_reorder_safe=") + (reorder_safe ? "true" : "false"));
    join.runtime_evidence.push_back(std::string("join_order_strategy=") +
                                    (order_plan.semantic_order_preserved ? "semantic_input_order" : "bounded_dp"));
    join.runtime_evidence.push_back(std::string("join_semantic_order_preserved=") +
                                    (order_plan.semantic_order_preserved ? "true" : "false"));
    join.runtime_evidence.push_back(std::string("join_dp_bounded=") +
                                    (order_plan.bounded_enumeration_applied ? "true" : "false"));
    join.runtime_evidence.push_back(std::string("join_dp_pruned=") +
                                    (order_plan.pruning_applied ? "true" : "false"));
    join.runtime_evidence.push_back("join_dp_enumerated_subsets=" + std::to_string(order_plan.enumerated_subsets));
    join.runtime_evidence.push_back("join_dp_transitions_considered=" + std::to_string(order_plan.transitions_considered));
    join.runtime_evidence.push_back("join_dp_pruned_alternatives=" + std::to_string(order_plan.pruned_alternatives));
    join.runtime_evidence.push_back("left_cardinality=" + std::to_string(join.children[0].estimated_rows));
    join.runtime_evidence.push_back("right_cardinality=" + std::to_string(join.children[1].estimated_rows));
    for (const auto& diagnostic : order_plan.diagnostics) {
      join.runtime_evidence.push_back("join_diagnostic=" + diagnostic);
      join.diagnostics.push_back(diagnostic);
    }
    return join;
  };

  PhysicalPlanNode joined = ordered_leaves.front()->node;
  const auto total_steps = ordered_leaves.size() - 1;
  for (std::size_t i = 1; i < ordered_leaves.size(); ++i) {
    joined = build_join(std::move(joined), ordered_leaves[i]->node, i, total_steps);
  }
  *current = std::move(joined);
  return true;
}

void AdjustUpperEstimatedRows(PhysicalPlanNode* node,
                              const PhysicalPlanNode& child,
                              const OptimizerStatisticsCatalog& statistics,
                              const planner::LogicalPlanNode& logical_node) {
  if (IsAggregateAccessKind(node->access_kind)) {
    node->estimated_rows = statistics.EstimateUnsigned("group_count",
                                                       RequiredObjectUuid(logical_node),
                                                       std::max<std::uint64_t>(1, child.estimated_rows / 10));
  } else if (node->access_kind == planner::PhysicalAccessKind::kTopN) {
    const auto limit = statistics.EstimateUnsigned("limit_count", RequiredObjectUuid(logical_node), 10);
    node->estimated_rows = std::min(child.estimated_rows, std::max<std::uint64_t>(1, limit));
  } else {
    node->estimated_rows = child.estimated_rows;
  }
}

bool ComposeUpperNode(OptimizedPlan* optimized,
                      const planner::LogicalPlanNode& logical_node,
                      const OptimizerStatisticsCatalog& statistics,
                      std::optional<PhysicalPlanNode>* current) {
  if (!current->has_value()) {
    optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_UPPER_INPUT_MISSING");
    return false;
  }
  const auto candidate_index = FindBestCandidateIndex(*optimized, logical_node, IsUpperOperatorAccessKind);
  if (!candidate_index) {
    optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_UPPER_CANDIDATE_MISSING");
    return false;
  }
  auto& selected = optimized->candidates[*candidate_index];
  selected.selected_in_physical_tree = true;
  auto child = std::move(**current);
  auto upper = PhysicalNodeForCandidate(selected, logical_node.operation_id);
  AdjustUpperEstimatedRows(&upper, child, statistics, logical_node);
  AddCost(&upper.cost, child.cost);
  upper.storage_backed = false;
  upper.preserves_visibility = child.preserves_visibility;
  if (upper.access_kind == planner::PhysicalAccessKind::kTopN ||
      upper.access_kind == planner::PhysicalAccessKind::kSort ||
      upper.access_kind == planner::PhysicalAccessKind::kSortThenWindow) {
    upper.preserves_order = true;
  }
  upper.runtime_evidence.push_back("physical_role=upper_operator");
  upper.runtime_evidence.push_back("input_rows=" + std::to_string(child.estimated_rows));
  upper.runtime_evidence.push_back("output_rows=" + std::to_string(upper.estimated_rows));
  upper.children.push_back(std::move(child));
  *current = std::move(upper);
  return true;
}

bool ComposeSetOperationNode(OptimizedPlan* optimized,
                             const planner::LogicalPlanNode& logical_node,
                             const std::vector<LeafSelection>& leaves,
                             std::optional<PhysicalPlanNode>* current) {
  std::vector<std::string> ordered_relation_uuids = logical_node.required_object_uuids;
  if (ordered_relation_uuids.empty()) {
    ordered_relation_uuids.reserve(leaves.size());
    for (const auto& leaf : leaves) ordered_relation_uuids.push_back(leaf.key);
  }
  std::vector<const LeafSelection*> ordered_leaves;
  ordered_leaves.reserve(ordered_relation_uuids.size());
  for (const auto& relation_uuid : ordered_relation_uuids) {
    const auto* leaf = FindLeaf(leaves, relation_uuid);
    if (leaf == nullptr) {
      optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_SET_OPERATION_LEAF_MISSING:" + relation_uuid);
      return false;
    }
    ordered_leaves.push_back(leaf);
  }
  if (ordered_leaves.size() < 2) {
    optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_SET_OPERATION_INPUTS_MISSING");
    return false;
  }
  const auto candidate_index = FindBestCandidateIndex(*optimized,
                                                      logical_node,
                                                      IsSetOperationAccessKind);
  if (!candidate_index) {
    optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_SET_OPERATION_CANDIDATE_MISSING");
    return false;
  }
  auto& selected = optimized->candidates[*candidate_index];
  selected.selected_in_physical_tree = true;
  auto set_node = PhysicalNodeForCandidate(selected, logical_node.operation_id);
  set_node.storage_backed = false;
  set_node.preserves_order = false;
  set_node.preserves_visibility = true;
  set_node.runtime_evidence.push_back("physical_role=set_operation");
  set_node.runtime_evidence.push_back("input_count=" + std::to_string(ordered_leaves.size()));
  for (const auto* leaf : ordered_leaves) {
    set_node.preserves_visibility = set_node.preserves_visibility &&
                                    leaf->node.preserves_visibility;
    AddCost(&set_node.cost, leaf->node.cost);
    set_node.children.push_back(leaf->node);
  }
  *current = std::move(set_node);
  return true;
}

bool PhysicalTreeContainsCandidateEvidence(const PhysicalPlanNode& node,
                                           const std::string& candidate_id) {
  const auto token = "selected_candidate_id=" + candidate_id;
  if (std::find(node.runtime_evidence.begin(), node.runtime_evidence.end(), token) != node.runtime_evidence.end()) {
    return true;
  }
  return std::any_of(node.children.begin(), node.children.end(), [&](const PhysicalPlanNode& child) {
    return PhysicalTreeContainsCandidateEvidence(child, candidate_id);
  });
}

void MarkPrimaryFlatSelection(OptimizedPlan* optimized,
                              const std::vector<LeafSelection>& leaves) {
  if (!leaves.empty()) {
    const auto primary = leaves.front().candidate_index;
    optimized->candidates[primary].selected = true;
    optimized->candidates[primary].plan_candidate.selected = true;
    optimized->selected_primary_candidate_id = optimized->candidates[primary].plan_candidate.candidate_id;
    optimized->selected_primary_operation_id = optimized->candidates[primary].node.operation_id;
    return;
  }
  for (auto& candidate : optimized->candidates) {
    if (!candidate.cost.selectable) continue;
    candidate.selected = true;
    candidate.plan_candidate.selected = true;
    optimized->selected_primary_candidate_id = candidate.plan_candidate.candidate_id;
    optimized->selected_primary_operation_id = candidate.node.operation_id;
    return;
  }
}

void BuildPhysicalPlanTree(OptimizedPlan* optimized,
                           const planner::LogicalPlan& plan,
                           const OptimizerStatisticsCatalog& statistics) {
  for (auto& candidate : optimized->candidates) {
    candidate.selected = false;
    candidate.selected_in_physical_tree = false;
    candidate.plan_candidate.selected = false;
  }

  auto leaves = SelectBaseLeaves(optimized);
  std::optional<PhysicalPlanNode> current;
  if (leaves.size() == 1) current = leaves.front().node;
  if (leaves.empty()) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < optimized->candidates.size(); ++i) {
      const auto& candidate = optimized->candidates[i];
      if (!candidate.cost.selectable ||
          IsJoinAccessKind(candidate.plan_candidate.access_kind) ||
          IsUpperOperatorAccessKind(candidate.plan_candidate.access_kind)) {
        continue;
      }
      if (!best || IsBetterCost(candidate.cost, optimized->candidates[*best].cost)) {
        best = i;
      }
    }
    if (best) {
      optimized->candidates[*best].selected_in_physical_tree = true;
      current = PhysicalNodeForCandidate(optimized->candidates[*best], RelationKeyForNode(optimized->candidates[*best].node));
    }
  }

  for (const auto& node : plan.nodes) {
    if (IsJoinAccessKind(node.access_kind)) {
      if (!ComposeJoinNode(optimized, node, statistics, leaves, &current)) return;
      continue;
    }
    if (node.access_kind == planner::PhysicalAccessKind::kSetOperation) {
      if (!ComposeSetOperationNode(optimized, node, leaves, &current)) return;
      continue;
    }
    if (IsUpperOperatorAccessKind(node.access_kind)) {
      if (leaves.size() == 1 && !current.has_value()) current = leaves.front().node;
      if (!ComposeUpperNode(optimized, node, statistics, &current)) return;
      continue;
    }
  }

  if (!current.has_value()) {
    if (leaves.empty()) {
      optimized->diagnostics.push_back("no_selectable_optimizer_candidate");
      for (const auto& candidate : optimized->candidates) {
        if (!candidate.rejection_reason.empty()) {
          optimized->diagnostics.push_back(candidate.rejection_reason);
        }
      }
      return;
    }
    if (leaves.size() > 1) {
      optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_JOIN_REQUIRED_FOR_MULTIPLE_BASE_RELATIONS");
      return;
    }
    current = leaves.front().node;
  }

  optimized->physical_root = std::move(*current);
  optimized->has_physical_plan = true;
  MarkPrimaryFlatSelection(optimized, leaves);
  if (optimized->selected_primary_candidate_id.empty() ||
      !PhysicalTreeContainsCandidateEvidence(optimized->physical_root, optimized->selected_primary_candidate_id)) {
    optimized->diagnostics.push_back("SB_OPT_PHYSICAL_TREE_PRIMARY_SELECTION_NOT_COMPATIBLE");
    optimized->has_physical_plan = false;
    return;
  }

  const auto validation = ValidatePhysicalPlanNode(optimized->physical_root);
  if (!validation.ok) {
    optimized->diagnostics.insert(optimized->diagnostics.end(),
                                  validation.diagnostics.begin(),
                                  validation.diagnostics.end());
    optimized->has_physical_plan = false;
    return;
  }

  const auto primary = std::find_if(optimized->candidates.begin(), optimized->candidates.end(), [](const OptimizerCandidate& candidate) {
    return candidate.selected;
  });
  if (primary != optimized->candidates.end() && primary->plan_candidate.uses_local_default_statistics) {
    optimized->diagnostics.push_back("SB_OPTIMIZER_BENCHMARK_CLEAN.LOCAL_DEFAULT_STATS");
  }
  if (primary != optimized->candidates.end() && primary->plan_candidate.uses_policy_default_statistics) {
    optimized->diagnostics.push_back("SB_OPTIMIZER_BENCHMARK_CLEAN.POLICY_DEFAULT_STATS");
  }
  optimized->ok = true;
}

}  // namespace

OptimizedPlan OptimizeLogicalPlan(const planner::LogicalPlan& plan) {
  return OptimizeLogicalPlanWithStatistics(plan, DefaultLocalStatisticsCatalog());
}

OptimizedPlan OptimizeLogicalPlanWithStatistics(const planner::LogicalPlan& plan,
                                                const OptimizerStatisticsCatalog& statistics) {
  OptimizedPlan optimized;
  if (!plan.ok) {
    optimized.diagnostics.push_back("logical_plan_not_ok");
    return optimized;
  }
  if (plan.nodes.empty()) {
    optimized.diagnostics.push_back("logical_plan_empty");
    return optimized;
  }
  optimized.diagnostics.push_back(std::string(kStatisticsOnlyNotBenchmarkClean));

  for (const auto& node : plan.nodes) {
    if (IsJoinAccessKind(node.access_kind)) {
      const auto append_join_operand = [&](const std::string& object_uuid, const char* suffix) {
        if (PlanHasBaseForObject(plan, object_uuid)) return;
        auto operand = planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead,
                                                    planner::PhysicalAccessKind::kNone,
                                                    node.operation_id + "." + suffix,
                                                    std::string("join_operand_") + suffix);
        operand.required_object_uuids.push_back(object_uuid);
        operand.required_descriptors = node.required_descriptors;
        for (auto plan_candidate : GenerateLocalAccessPathCandidates(operand, statistics)) {
          const auto statistics_version = StatisticsVersionForCandidate(plan_candidate);
          AppendOptimizerCandidate(&optimized,
                                   operand,
                                   std::move(plan_candidate),
                                   statistics_version);
        }
      };
      const auto relation_keys = JoinRelationKeysForNode(node);
      for (std::size_t i = 0; i < relation_keys.size(); ++i) {
        const auto suffix = "input" + std::to_string(i);
        append_join_operand(relation_keys[i], suffix.c_str());
      }
      AppendJoinCandidates(&optimized, node, statistics);
      continue;
    }
    if (IsUpperOperatorAccessKind(node.access_kind)) {
      AppendRelationalCandidate(&optimized, node, statistics);
      continue;
    }
    for (auto plan_candidate : GenerateLocalAccessPathCandidates(node, statistics)) {
      const auto statistics_version = StatisticsVersionForCandidate(plan_candidate);
      AppendOptimizerCandidate(&optimized,
                               node,
                               std::move(plan_candidate),
                               statistics_version);
    }
  }
  BuildPhysicalPlanTree(&optimized, plan, statistics);
  return optimized;
}

OptimizedPlan OptimizeLogicalPlanWithAccessPathRequest(const planner::LogicalPlan& plan,
                                                       const AccessPathPlanningRequest& access_request) {
  OptimizedPlan optimized;
  optimized.optimizer_profile = std::string(kCatalogBackedProfile);
  if (!access_request.table_stats || !access_request.visibility_proven ||
      !access_request.grants_proven) {
    optimized.diagnostics.push_back(std::string(kCatalogFactsRequired));
  }
  if (!plan.ok) {
    optimized.diagnostics.push_back("logical_plan_not_ok");
    return optimized;
  }
  if (plan.nodes.empty()) {
    optimized.diagnostics.push_back("logical_plan_empty");
    return optimized;
  }

  OptimizerStatisticsCatalog tree_statistics;
  if (access_request.table_stats) {
    const auto& stats = *access_request.table_stats;
    tree_statistics.Add(MakeStatistic("row_count", "relation", access_request.relation_uuid,
                                      static_cast<double>(stats.row_count),
                                      StatisticSource::kCatalogExact,
                                      stats.identity.stats_epoch,
                                      0,
                                      stats.identity.confidence));
    tree_statistics.Add(MakeStatistic("visible_row_count", "relation", access_request.relation_uuid,
                                      static_cast<double>(stats.visible_row_count),
                                      StatisticSource::kCatalogExact,
                                      stats.identity.stats_epoch,
                                      0,
                                      stats.identity.confidence));
    tree_statistics.Add(MakeStatistic("page_count", "relation", access_request.relation_uuid,
                                      static_cast<double>(stats.page_count),
                                      StatisticSource::kCatalogExact,
                                      stats.identity.stats_epoch,
                                      0,
                                      stats.identity.confidence));
    tree_statistics.Add(MakeStatistic("average_row_bytes", "relation", access_request.relation_uuid,
                                      static_cast<double>(stats.average_row_bytes),
                                      StatisticSource::kCatalogExact,
                                      stats.identity.stats_epoch,
                                      0,
                                      stats.identity.confidence));
  }
  tree_statistics.Add(MakeStatistic("memory_grant_available_bytes", "session", "local.default",
                                    1048576.0,
                                    StatisticSource::kCatalogExact,
                                    access_request.table_stats ? access_request.table_stats->identity.stats_epoch : 1,
                                    0,
                                    CostConfidence::kHigh));
  if (access_request.ordered_limit.present && access_request.ordered_limit.limit_count != 0) {
    tree_statistics.Add(MakeStatistic("limit_count", "relation", access_request.relation_uuid,
                                      static_cast<double>(access_request.ordered_limit.limit_count),
                                      StatisticSource::kCatalogExact,
                                      access_request.table_stats ? access_request.table_stats->identity.stats_epoch : 1,
                                      0,
                                      CostConfidence::kHigh));
  }

  for (const auto& node : plan.nodes) {
    auto bound_node = node;
    if (bound_node.required_object_uuids.empty() && !access_request.relation_uuid.empty()) {
      bound_node.required_object_uuids.push_back(access_request.relation_uuid);
    }
    if (bound_node.required_descriptors.empty() && !access_request.descriptor_digest.empty()) {
      bound_node.required_descriptors.push_back(access_request.descriptor_digest);
    }
    if (IsUpperOperatorAccessKind(bound_node.access_kind)) {
      AppendRelationalCandidate(&optimized, bound_node, tree_statistics);
      continue;
    }
    const auto plan_candidates = GenerateFullAccessPathCandidates(access_request);
    for (auto plan_candidate : plan_candidates) {
      AppendOptimizerCandidate(&optimized,
                               bound_node,
                               std::move(plan_candidate),
                               access_request.table_stats
                                   ? ("catalog:" + std::to_string(access_request.table_stats->identity.stats_epoch))
                                   : "catalog-missing:epoch0");
    }
  }
  BuildPhysicalPlanTree(&optimized, plan, tree_statistics);
  return optimized;
}

StatisticsContractStatus ValidateBenchmarkCleanOptimizedPlan(const OptimizedPlan& plan) {
  if (!plan.ok) {
    return {false, "SB_OPTIMIZER_BENCHMARK_CLEAN.PLAN_NOT_OK", "optimized_plan"};
  }
  if (std::find(plan.diagnostics.begin(),
                plan.diagnostics.end(),
                kStatisticsOnlyNotBenchmarkClean) != plan.diagnostics.end()) {
    return {false, std::string(kStatisticsOnlyNotBenchmarkClean), "optimized_plan"};
  }
  if (plan.optimizer_profile != kCatalogBackedProfile) {
    return {false, std::string(kStatisticsOnlyNotBenchmarkClean), plan.optimizer_profile};
  }
  if (std::find(plan.diagnostics.begin(),
                plan.diagnostics.end(),
                kCatalogFactsRequired) != plan.diagnostics.end()) {
    return {false, std::string(kCatalogFactsRequired), "optimized_plan"};
  }
  const auto selected = std::find_if(plan.candidates.begin(), plan.candidates.end(), [](const OptimizerCandidate& candidate) {
    return candidate.selected;
  });
  if (selected == plan.candidates.end()) {
    return {false, "SB_OPTIMIZER_BENCHMARK_CLEAN.NO_SELECTED_PLAN", "optimized_plan"};
  }
  if (selected->plan_candidate.uses_local_default_statistics) {
    return {false, "SB_OPTIMIZER_BENCHMARK_CLEAN.LOCAL_DEFAULT_STATS", selected->plan_candidate.candidate_id};
  }
  if (selected->plan_candidate.uses_policy_default_statistics) {
    return {false, "SB_OPTIMIZER_BENCHMARK_CLEAN.POLICY_DEFAULT_STATS", selected->plan_candidate.candidate_id};
  }
  if (selected->statistics_version.find("local.default") != std::string::npos) {
    return {false, "SB_OPTIMIZER_BENCHMARK_CLEAN.LOCAL_DEFAULT_STATS", selected->statistics_version};
  }
  return {true, "SB_OPTIMIZER_BENCHMARK_CLEAN.OK", selected->plan_candidate.candidate_id};
}

std::string SerializeOptimizedPlanToJson(const OptimizedPlan& plan) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"ok\": " << (plan.ok ? "true" : "false") << ",\n";
  out << "  \"optimizer_profile\": \"" << JsonEscape(plan.optimizer_profile) << "\",\n";
  out << "  \"has_physical_plan\": " << (plan.has_physical_plan ? "true" : "false") << ",\n";
  out << "  \"selected_primary_candidate_id\": \"" << JsonEscape(plan.selected_primary_candidate_id) << "\",\n";
  out << "  \"selected_primary_operation_id\": \"" << JsonEscape(plan.selected_primary_operation_id) << "\",\n";
  out << "  \"candidates\": [\n";
  for (std::size_t i = 0; i < plan.candidates.size(); ++i) {
    const auto& candidate = plan.candidates[i];
    out << "    {\"operation_id\": \"" << JsonEscape(candidate.node.operation_id) << "\", \"access_kind\": \""
        << planner::PhysicalAccessKindName(candidate.plan_candidate.access_kind) << "\", \"total_cost\": "
        << candidate.cost.total_cost << ", \"selected\": " << (candidate.selected ? "true" : "false")
        << ", \"selected_in_physical_tree\": " << (candidate.selected_in_physical_tree ? "true" : "false")
        << ", \"rejected\": " << (candidate.rejected ? "true" : "false")
        << ", \"rejection_reason\": \"" << JsonEscape(candidate.rejection_reason)
        << "\", \"statistics_version\": \"" << JsonEscape(candidate.statistics_version) << "\"}";
    if (i + 1 != plan.candidates.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < plan.diagnostics.size(); ++i) {
    out << "\"" << JsonEscape(plan.diagnostics[i]) << "\"";
    if (i + 1 != plan.diagnostics.size()) out << ", ";
  }
  out << "],\n";
  out << "  \"physical_root\": ";
  if (plan.has_physical_plan) {
    out << SerializePhysicalPlanNodeToJson(plan.physical_root, 2) << "\n";
  } else {
    out << "null\n";
  }
  out << "}\n";
  return out.str();
}

OptimizerDecision ChooseIndexAccess(const OptimizerEvidence& evidence) {
  OptimizerDecision decision;
  decision.rule = "index_choice";
  decision.ok = true;
  if (evidence.has_usable_index && evidence.point_predicate) decision.access_kind = planner::PhysicalAccessKind::kScalarBtreeLookup;
  else if (evidence.has_usable_index && evidence.range_predicate) decision.access_kind = planner::PhysicalAccessKind::kScalarBtreeRange;
  else { decision.access_kind = planner::PhysicalAccessKind::kTableScan; decision.diagnostic_code = "SBSQL_V3_OPTIMIZER_DETERMINISTIC_FALLBACK"; }
  return decision;
}

OptimizerDecision ChooseJoinOrder(const OptimizerEvidence& evidence) {
  OptimizerDecision decision;
  decision.rule = "join_reorder";
  decision.ok = true;
  if (evidence.reorder_safe_join && evidence.left_cardinality != 0 && evidence.right_cardinality != 0) {
    decision.access_kind = planner::PhysicalAccessKind::kJoinHash;
  } else {
    decision.access_kind = planner::PhysicalAccessKind::kJoinNestedLoop;
    decision.diagnostic_code = "SBSQL_V3_OPTIMIZER_PRESERVE_JOIN_ORDER";
  }
  return decision;
}

OptimizerDecision ChooseAggregateStrategy(const OptimizerEvidence& evidence) {
  OptimizerDecision decision;
  decision.rule = "aggregate_strategy";
  decision.ok = true;
  decision.access_kind = evidence.grouping_present ? planner::PhysicalAccessKind::kAggregateHash : planner::PhysicalAccessKind::kAggregateGeneric;
  return decision;
}

OptimizerDecision ChooseSpecializedWorkloadAccess(const OptimizerEvidence& evidence) {
  OptimizerDecision decision;
  decision.rule = "specialized_workload";
  decision.ok = true;
  if (evidence.specialized_kind == "vector") { decision.access_kind = evidence.exact_fallback_available ? planner::PhysicalAccessKind::kVectorApproximateWithFallback : planner::PhysicalAccessKind::kVectorExactSearch; decision.llvm_eligible = true; decision.gpu_eligible = true; }
  else if (evidence.specialized_kind == "search") decision.access_kind = planner::PhysicalAccessKind::kFullTextProbe;
  else if (evidence.specialized_kind == "document") decision.access_kind = planner::PhysicalAccessKind::kDocumentPathProbe;
  else if (evidence.specialized_kind == "graph") decision.access_kind = planner::PhysicalAccessKind::kGraphTraversalSeed;
  else decision.access_kind = planner::PhysicalAccessKind::kTimeSeriesAppendPath;
  return decision;
}

#endif

}  // namespace scratchbird::engine::optimizer
