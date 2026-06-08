// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path.hpp"

#include "cluster_candidate.hpp"
#include "optimizer_cost_full.hpp"
#include "selectivity_model.hpp"
#include "specialized_planner.hpp"

#include <algorithm>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

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

PlanCandidate MakeCandidate(std::string id,
                            planner::PhysicalAccessKind access_kind,
                            std::vector<std::string> required_facts,
                            CostVector cost,
                            std::uint64_t estimated_rows,
                            std::vector<std::string> missing_facts = {}) {
  PlanCandidate candidate;
  candidate.candidate_id = std::move(id);
  candidate.access_kind = access_kind;
  candidate.required_facts = std::move(required_facts);
  candidate.cost = std::move(cost);
  candidate.estimated_rows = estimated_rows;
  candidate.missing_facts = std::move(missing_facts);
  if (!candidate.missing_facts.empty()) {
    candidate.refusal_reasons = candidate.missing_facts;
    candidate.cost.selectable = false;
    candidate.cost.confidence = CostConfidence::kRejected;
    candidate.cost.rejection_reason = candidate.missing_facts.front();
  } else {
    candidate.acceptance_reasons = candidate.required_facts;
  }
  return candidate;
}

std::string RequiredObjectUuid(const planner::LogicalPlanNode& node) {
  return node.required_object_uuids.empty() ? "local.default" : node.required_object_uuids.front();
}

bool HasDescriptor(const planner::LogicalPlanNode& node, std::string_view descriptor) {
  return std::find(node.required_descriptors.begin(), node.required_descriptors.end(), descriptor) !=
         node.required_descriptors.end();
}

bool IsShapeReadNode(const planner::LogicalPlanNode& node) {
  return node.kind == planner::LogicalPlanNodeKind::kDmlRead ||
         node.kind == planner::LogicalPlanNodeKind::kNoSqlOperation;
}

void AttachStatisticDiagnostics(PlanCandidate* candidate,
                                const OptimizerStatisticsCatalog& statistics,
                                const std::string& object_uuid,
                                const std::vector<std::string>& statistic_names) {
  if (candidate == nullptr) return;
  for (const auto& statistic_name : statistic_names) {
    const auto exact = statistics.Find(statistic_name, object_uuid);
    if (exact && exact->available && exact->source == StatisticSource::kPolicyDefault) {
      candidate->uses_policy_default_statistics = true;
      candidate->statistics_diagnostics.push_back("policy-default:" + statistic_name + ":" + object_uuid);
      continue;
    }
    if (exact && exact->available) {
      continue;
    }
    const auto local_default = statistics.Find(statistic_name, "local.default");
    if (local_default && local_default->available) {
      candidate->uses_local_default_statistics = true;
      candidate->statistics_diagnostics.push_back("local.default:" + statistic_name);
      if (local_default->source == StatisticSource::kPolicyDefault) {
        candidate->uses_policy_default_statistics = true;
        candidate->statistics_diagnostics.push_back("policy-default:" + statistic_name + ":local.default");
      }
    }
  }
}

std::uint64_t EstimateUnsignedForObject(const OptimizerStatisticsCatalog& statistics,
                                        const std::string& statistic_name,
                                        const std::string& object_uuid,
                                        std::uint64_t fallback) {
  auto value = statistics.EstimateUnsigned(statistic_name, object_uuid, 0);
  if (value != 0) { return value; }
  value = statistics.EstimateUnsigned(statistic_name, "local.default", 0);
  return value == 0 ? fallback : value;
}

double EstimateDoubleForObject(const OptimizerStatisticsCatalog& statistics,
                               const std::string& statistic_name,
                               const std::string& object_uuid,
                               double fallback) {
  if (const auto value = statistics.Find(statistic_name, object_uuid); value && value->available) {
    return value->value;
  }
  if (const auto value = statistics.Find(statistic_name, "local.default"); value && value->available) {
    return value->value;
  }
  return fallback;
}

OptimizerCostEnvironment EnvironmentForStatistics(const OptimizerStatisticsCatalog& statistics,
                                                  const std::string& object_uuid) {
  OptimizerCostEnvironment environment;
  environment.cost_profile_id = "runtime_metric_local_v1";
  environment.memory_budget_bytes = EstimateUnsignedForObject(statistics,
                                                              "memory_grant_available_bytes",
                                                              object_uuid,
                                                              1048576);
  const double read_latency = EstimateDoubleForObject(statistics, "page_family_read_latency_microseconds", object_uuid, 1000.0);
  environment.sequential_page_cost = std::clamp(read_latency / 1000.0, 0.25, 10.0);
  environment.random_page_cost = std::clamp(environment.sequential_page_cost * 4.0, 1.0, 40.0);
  return environment;
}

std::vector<OptimizerMetricCostInput> MetricFeedbackForStatistics(const OptimizerStatisticsCatalog& statistics,
                                                                  const std::string& object_uuid) {
  std::vector<OptimizerMetricCostInput> feedback;
  const auto add = [&](const std::string& statistic_name, const std::string& metric_name) {
    if (const auto value = statistics.Find(statistic_name, object_uuid); value && value->available) {
      feedback.push_back({metric_name, value->value, value->freshness_microseconds, true});
      return;
    }
    if (const auto value = statistics.Find(statistic_name, "local.default"); value && value->available) {
      feedback.push_back({metric_name, value->value, value->freshness_microseconds, true});
    }
  };
  add("operator_latency_multiplier", "operator_latency_multiplier");
  add("io_latency_multiplier", "io_latency_multiplier");
  add("estimate_uncertainty", "estimate_uncertainty");
  const auto add_runtime_feedback = [&](const std::string& statistic_name, const std::string& metric_name) {
    const auto push = [&](const auto& value) {
      OptimizerMetricCostInput metric;
      metric.metric_name = metric_name;
      metric.value = value.value;
      metric.freshness_microseconds = value.freshness_microseconds;
      metric.policy_allowed = true;
      metric.operator_family = "access_path";
      metric.plan_shape = object_uuid;
      metric.cost_profile_id = "runtime_metric_local_v1";
      feedback.push_back(std::move(metric));
    };
    if (const auto value = statistics.Find(statistic_name, object_uuid); value && value->available) {
      push(*value);
      return;
    }
    if (const auto value = statistics.Find(statistic_name, "local.default"); value && value->available) {
      push(*value);
    }
  };
  add_runtime_feedback("feedback_estimated_rows", "feedback.estimated_rows");
  add_runtime_feedback("feedback_actual_rows", "feedback.actual_rows");
  add_runtime_feedback("feedback_estimated_pages", "feedback.estimated_pages");
  add_runtime_feedback("feedback_actual_pages", "feedback.actual_pages");
  add_runtime_feedback("feedback_estimated_io_operations", "feedback.estimated_io_operations");
  add_runtime_feedback("feedback_actual_io_operations", "feedback.actual_io_operations");
  add_runtime_feedback("feedback_estimated_visibility_recheck_rows", "feedback.estimated_visibility_recheck_rows");
  add_runtime_feedback("feedback_actual_visibility_recheck_rows", "feedback.actual_visibility_recheck_rows");
  add_runtime_feedback("feedback_estimated_spill_bytes", "feedback.estimated_spill_bytes");
  add_runtime_feedback("feedback_actual_spill_bytes", "feedback.actual_spill_bytes");
  add_runtime_feedback("feedback_memory_grant_bytes", "feedback.memory_grant_bytes");
  add_runtime_feedback("feedback_peak_memory_bytes", "feedback.peak_memory_bytes");
  add_runtime_feedback("feedback_estimated_latency_microseconds", "feedback.estimated_latency_microseconds");
  add_runtime_feedback("feedback_actual_latency_microseconds", "feedback.actual_latency_microseconds");
  add_runtime_feedback("feedback_estimated_resource_units", "feedback.estimated_resource_units");
  add_runtime_feedback("feedback_actual_resource_units", "feedback.actual_resource_units");
  return feedback;
}

void Finish(CostVector* cost) {
  cost->total_cost = cost->startup_cost + cost->row_cost + cost->io_cost + cost->memory_cost + cost->uncertainty_cost;
}

}  // namespace

std::vector<PlanCandidate> GenerateLocalAccessPathCandidates(const planner::LogicalPlanNode& node,
                                                            const OptimizerStatisticsCatalog& statistics) {
  std::vector<PlanCandidate> candidates;
  if (node.access_kind == planner::PhysicalAccessKind::kNone) {
    if (!IsShapeReadNode(node)) {
      candidates.push_back(MakeCandidate("CAND-OPT-COMMAND", planner::PhysicalAccessKind::kNone, {"bound_operation"}, EstimateNodeCost(node), 0));
      return candidates;
    }
  }
  if (node.access_kind == planner::PhysicalAccessKind::kCatalogUuidLookup) {
    candidates.push_back(MakeCandidate("CAND-OPT-002", planner::PhysicalAccessKind::kCatalogUuidLookup, {"catalog_object_uuid", "catalog_visibility"}, EstimateNodeCost(node), 1));
    return candidates;
  }
  if (node.kind == planner::LogicalPlanNodeKind::kNoSqlOperation) {
    candidates.push_back(PlanNoSqlLogicalNodeCandidate(node, statistics));
    return candidates;
  }
  if (node.access_kind == planner::PhysicalAccessKind::kClusterFragmentScan) {
    candidates.push_back(BuildClusterFragmentCandidate({}));
    return candidates;
  }
  if (node.access_kind == planner::PhysicalAccessKind::kRemoteNodePushdown) {
    candidates.push_back(BuildRemoteNodePushdownCandidate({}));
    return candidates;
  }
  const std::string object_uuid = RequiredObjectUuid(node);
  const std::uint64_t row_count = EstimateUnsignedForObject(statistics, "row_count", object_uuid, 1000);
  const std::uint64_t visible_count = EstimateUnsignedForObject(statistics, "visible_row_count", object_uuid, row_count);
  const std::uint64_t retained_versions = EstimateUnsignedForObject(statistics, "relation_retained_version_count", object_uuid, visible_count);
  const std::uint64_t page_count = EstimateUnsignedForObject(statistics, "page_count", object_uuid, 64);
  const std::uint64_t row_width_bytes = EstimateUnsignedForObject(statistics, "average_row_bytes", object_uuid, 32);
  const std::uint64_t filespace_available_pages = EstimateUnsignedForObject(statistics, "filespace_available_pages", object_uuid, page_count + 1);
  const double cache_hit_ratio = std::clamp(EstimateDoubleForObject(statistics, "page_cache_hit_ratio", object_uuid, 0.0), 0.0, 1.0);
  const double cache_pressure = std::clamp(EstimateDoubleForObject(statistics, "page_cache_pressure_level", object_uuid, 0.0), 0.0, 10.0);
  const auto environment = EnvironmentForStatistics(statistics, object_uuid);
  const auto metric_feedback = MetricFeedbackForStatistics(statistics, object_uuid);

  auto table_scan = EstimateBaseOperatorCost(environment,
                                             planner::PhysicalAccessKind::kTableScan,
                                             visible_count,
                                             row_width_bytes,
                                             page_count);
  table_scan.row_cost += retained_versions > visible_count ? (retained_versions - visible_count) / 4 : 0;
  table_scan.io_cost = static_cast<std::uint64_t>(static_cast<double>(table_scan.io_cost) * std::clamp(1.0 - (cache_hit_ratio * 0.50), 0.25, 1.0));
  table_scan.uncertainty_cost += static_cast<std::uint64_t>(cache_pressure * 100.0);
  if (filespace_available_pages < page_count) {
    table_scan.uncertainty_cost += (page_count - filespace_available_pages) * 100;
    table_scan.reason = "filespace_page_pressure";
  }
  Finish(&table_scan);
  table_scan = ApplyMetricFeedbackCost(std::move(table_scan), metric_feedback);
  auto table_scan_candidate = MakeCandidate("CAND-OPT-001", planner::PhysicalAccessKind::kTableScan, {"relation_uuid", "visibility_rules", "row_count", "page_count", "page_cache", "filespace_available_pages"}, table_scan, visible_count);
  AttachStatisticDiagnostics(&table_scan_candidate, statistics, object_uuid, {"row_count", "visible_row_count", "page_count", "average_row_bytes", "memory_grant_available_bytes", "filespace_available_pages"});
  candidates.push_back(std::move(table_scan_candidate));

  const bool wants_row_uuid = node.access_kind == planner::PhysicalAccessKind::kRowUuidLookup ||
                              HasDescriptor(node, "predicate.row_uuid_eq");
  const bool wants_equality = node.access_kind == planner::PhysicalAccessKind::kScalarBtreeLookup ||
                              node.access_kind == planner::PhysicalAccessKind::kScalarHashLookup ||
                              HasDescriptor(node, "predicate.scalar_eq") ||
                              HasDescriptor(node, "predicate.unique_eq") ||
                              node.stable_name == "point_lookup";
  const bool wants_range = node.access_kind == planner::PhysicalAccessKind::kScalarBtreeRange ||
                           HasDescriptor(node, "predicate.scalar_range") ||
                           node.stable_name == "range_query";
  const bool wants_covering = node.access_kind == planner::PhysicalAccessKind::kCoveringIndexScan ||
                              HasDescriptor(node, "projection.covered");

  if (wants_row_uuid) {
    auto row_uuid = ApplyMetricFeedbackCost(EstimateBaseOperatorCost(environment, planner::PhysicalAccessKind::kRowUuidLookup, 1, row_width_bytes, 1), metric_feedback);
    candidates.push_back(MakeCandidate("CAND-OPT-003", planner::PhysicalAccessKind::kRowUuidLookup, {"row_uuid_predicate", "relation_uuid"}, row_uuid, 1));
  }
  if (wants_equality) {
    const auto index_depth = EstimateUnsignedForObject(statistics, "index_depth", object_uuid, 3);
    const auto index_leaf_pages = EstimateUnsignedForObject(statistics, "index_leaf_pages", object_uuid, 1);
    const auto index_distinct_keys = EstimateUnsignedForObject(statistics, "index_distinct_keys", object_uuid, 0);
    const auto fragmentation = EstimateDoubleForObject(statistics, "index_fragmentation_ratio", object_uuid, 0.0);
    PredicateSelectivityInput equality_selectivity;
    equality_selectivity.predicate_kind = HasDescriptor(node, "predicate.unique_eq") ? "unique_eq" : "scalar_eq";
    equality_selectivity.input_rows = visible_count;
    equality_selectivity.distinct_values = index_distinct_keys;
    equality_selectivity.has_mcv_frequency = statistics.Find("mcv_frequency", object_uuid).has_value();
    equality_selectivity.mcv_frequency = EstimateDoubleForObject(statistics, "mcv_frequency", object_uuid, 0.0);
    equality_selectivity.input_confidence = statistics.ConfidenceFor(index_distinct_keys == 0 ? "mcv_frequency" : "index_distinct_keys", object_uuid);
    const auto equality_estimate = EstimatePredicateSelectivity(equality_selectivity);
    const auto equality_rows = EstimateRowsAfterSelectivity(visible_count, equality_estimate);
    auto index_lookup = EstimateBaseOperatorCost(environment, planner::PhysicalAccessKind::kScalarBtreeLookup, equality_rows, row_width_bytes, index_depth);
    index_lookup.io_cost += index_depth + index_leaf_pages / 128;
    index_lookup.uncertainty_cost += static_cast<std::uint64_t>(std::clamp(fragmentation, 0.0, 10.0) * 100.0);
    Finish(&index_lookup);
    index_lookup = ApplyMetricFeedbackCost(std::move(index_lookup), metric_feedback);
    auto btree_candidate = MakeCandidate("CAND-OPT-004", planner::PhysicalAccessKind::kScalarBtreeLookup, {"index_uuid", "exact_key_descriptor", "index_exactness", "index_depth", "index_fragmentation_ratio"}, index_lookup, equality_rows);
    btree_candidate.statistics_diagnostics.push_back(equality_estimate.diagnostic_code);
    AttachStatisticDiagnostics(&btree_candidate, statistics, object_uuid, {"row_count", "visible_row_count", "page_count", "average_row_bytes", "memory_grant_available_bytes", "index_depth", "index_leaf_pages", "index_fragmentation_ratio"});
    candidates.push_back(std::move(btree_candidate));

    auto hash_lookup = EstimateBaseOperatorCost(environment, planner::PhysicalAccessKind::kScalarHashLookup, equality_rows, row_width_bytes, 1);
    if (!HasDescriptor(node, "index.hash_candidate")) {
      hash_lookup.uncertainty_cost += 1000;
    }
    Finish(&hash_lookup);
    hash_lookup = ApplyMetricFeedbackCost(std::move(hash_lookup), metric_feedback);
    auto hash_candidate = MakeCandidate("CAND-OPT-HASH", planner::PhysicalAccessKind::kScalarHashLookup, {"index_uuid", "hash_key_descriptor", "hash_bucket_directory"}, hash_lookup, equality_rows);
    hash_candidate.statistics_diagnostics.push_back(equality_estimate.diagnostic_code);
    AttachStatisticDiagnostics(&hash_candidate, statistics, object_uuid, {"row_count", "visible_row_count", "page_count", "average_row_bytes", "memory_grant_available_bytes", "index_distinct_keys"});
    candidates.push_back(std::move(hash_candidate));

    if (wants_covering || EstimateDoubleForObject(statistics, "index_visibility_coverage", object_uuid, 0.0) > 0.0) {
      auto covering = EstimateBaseOperatorCost(environment, planner::PhysicalAccessKind::kCoveringIndexScan, equality_rows, row_width_bytes, index_depth);
      covering.io_cost += index_depth;
      Finish(&covering);
      covering = ApplyMetricFeedbackCost(std::move(covering), metric_feedback);
      auto covering_candidate = MakeCandidate("CAND-OPT-006", planner::PhysicalAccessKind::kCoveringIndexScan, {"index_uuid", "projection_covered", "visibility_rules", "index_visibility_coverage"}, covering, equality_rows);
      covering_candidate.statistics_diagnostics.push_back(equality_estimate.diagnostic_code);
      AttachStatisticDiagnostics(&covering_candidate, statistics, object_uuid, {"row_count", "visible_row_count", "page_count", "average_row_bytes", "memory_grant_available_bytes", "index_visibility_coverage", "index_depth"});
      candidates.push_back(std::move(covering_candidate));
    }
  }
  if (wants_covering && !wants_equality) {
    const auto index_depth = EstimateUnsignedForObject(statistics, "index_depth", object_uuid, 3);
    auto covering = EstimateBaseOperatorCost(environment, planner::PhysicalAccessKind::kCoveringIndexScan, 1, row_width_bytes, index_depth);
    covering.io_cost += index_depth;
    Finish(&covering);
    covering = ApplyMetricFeedbackCost(std::move(covering), metric_feedback);
    auto covering_candidate = MakeCandidate("CAND-OPT-006", planner::PhysicalAccessKind::kCoveringIndexScan, {"index_uuid", "projection_covered", "visibility_rules", "index_visibility_coverage"}, covering, 1);
    AttachStatisticDiagnostics(&covering_candidate, statistics, object_uuid, {"row_count", "visible_row_count", "page_count", "average_row_bytes", "memory_grant_available_bytes", "index_visibility_coverage", "index_depth"});
    candidates.push_back(std::move(covering_candidate));
  }
  if (wants_range) {
    PredicateSelectivityInput range_selectivity;
    range_selectivity.predicate_kind = "scalar_range";
    range_selectivity.input_rows = visible_count;
    range_selectivity.has_histogram = statistics.Find("index_selectivity", object_uuid).has_value();
    range_selectivity.range_fraction = EstimateDoubleForObject(statistics, "index_selectivity", object_uuid, 0.33);
    range_selectivity.input_confidence = statistics.ConfidenceFor("index_selectivity", object_uuid);
    const auto range_estimate = EstimatePredicateSelectivity(range_selectivity);
    const auto estimated_rows = EstimateRowsAfterSelectivity(visible_count, range_estimate);
    auto range = EstimateBaseOperatorCost(environment, planner::PhysicalAccessKind::kScalarBtreeRange, estimated_rows, row_width_bytes, std::max<std::uint64_t>(1, estimated_rows / 64));
    range = ApplyMetricFeedbackCost(std::move(range), metric_feedback);
    auto range_candidate = MakeCandidate("CAND-OPT-005", planner::PhysicalAccessKind::kScalarBtreeRange, {"index_uuid", "compatible_ordering", "range_predicate", "index_selectivity"}, range, estimated_rows);
    range_candidate.statistics_diagnostics.push_back(range_estimate.diagnostic_code);
    AttachStatisticDiagnostics(&range_candidate, statistics, object_uuid, {"row_count", "visible_row_count", "page_count", "average_row_bytes", "memory_grant_available_bytes", "index_selectivity", "index_depth", "index_leaf_pages"});
    candidates.push_back(std::move(range_candidate));
  }
  planner::PhysicalAccessKind specialized_kind = node.access_kind;
  if (HasDescriptor(node, "specialized.search")) specialized_kind = planner::PhysicalAccessKind::kFullTextProbe;
  if (HasDescriptor(node, "specialized.vector")) specialized_kind = planner::PhysicalAccessKind::kVectorApproximateWithFallback;
  if (HasDescriptor(node, "specialized.document")) specialized_kind = planner::PhysicalAccessKind::kDocumentPathProbe;
  if (HasDescriptor(node, "specialized.graph")) specialized_kind = planner::PhysicalAccessKind::kGraphTraversalSeed;
  if (specialized_kind == planner::PhysicalAccessKind::kFullTextProbe ||
      specialized_kind == planner::PhysicalAccessKind::kVectorExactSearch ||
      specialized_kind == planner::PhysicalAccessKind::kVectorApproximateWithFallback ||
      specialized_kind == planner::PhysicalAccessKind::kDocumentPathProbe ||
      specialized_kind == planner::PhysicalAccessKind::kGraphTraversalSeed) {
    auto specialized_node = node;
    specialized_node.kind = planner::LogicalPlanNodeKind::kNoSqlOperation;
    specialized_node.access_kind = specialized_kind;
    candidates.push_back(PlanNoSqlLogicalNodeCandidate(specialized_node, statistics));
  }
  const auto bitmap = EstimateNodeCost(planner::MakeLogicalPlanNode(node.kind, planner::PhysicalAccessKind::kBitmapSummaryScan, node.operation_id, "bitmap_summary_deferred"));
  candidates.push_back(MakeCandidate("CAND-OPT-007", planner::PhysicalAccessKind::kBitmapSummaryScan, {"index_family_supported", "predicate_supported"}, bitmap, visible_count / 4, {"index_family_not_supported"}));
  return candidates;
}

std::optional<PlanCandidate> ChooseBestSelectableCandidate(const std::vector<PlanCandidate>& candidates) {
  std::optional<PlanCandidate> best;
  for (const auto& candidate : candidates) {
    if (!candidate.cost.selectable) continue;
    if (!best || IsBetterCost(candidate.cost, best->cost)) best = candidate;
  }
  if (best) best->selected = true;
  return best;
}

std::string SerializePlanCandidateToJson(const PlanCandidate& candidate) {
  std::ostringstream out;
  out << "{";
  out << "\"candidate_id\":\"" << JsonEscape(candidate.candidate_id) << "\",";
  out << "\"access_kind\":\"" << planner::PhysicalAccessKindName(candidate.access_kind) << "\",";
  out << "\"scope\":\"" << JsonEscape(candidate.scope) << "\",";
  out << "\"estimated_rows\":" << candidate.estimated_rows << ",";
  out << "\"selected\":" << (candidate.selected ? "true" : "false") << ",";
  out << "\"selectable\":" << (candidate.cost.selectable ? "true" : "false") << ",";
  out << "\"rejection_reason\":\"" << JsonEscape(candidate.cost.rejection_reason) << "\",";
  out << "\"total_cost\":" << candidate.cost.total_cost;
  out << ",\"acceptance_reasons\":[";
  for (std::size_t i = 0; i < candidate.acceptance_reasons.size(); ++i) {
    out << "\"" << JsonEscape(candidate.acceptance_reasons[i]) << "\"";
    if (i + 1 != candidate.acceptance_reasons.size()) out << ",";
  }
  out << "]";
  out << ",\"refusal_reasons\":[";
  for (std::size_t i = 0; i < candidate.refusal_reasons.size(); ++i) {
    out << "\"" << JsonEscape(candidate.refusal_reasons[i]) << "\"";
    if (i + 1 != candidate.refusal_reasons.size()) out << ",";
  }
  out << "]";
  out << ",\"runtime_evidence\":[";
  for (std::size_t i = 0; i < candidate.runtime_evidence.size(); ++i) {
    out << "\"" << JsonEscape(candidate.runtime_evidence[i]) << "\"";
    if (i + 1 != candidate.runtime_evidence.size()) out << ",";
  }
  out << "]";
  if (candidate.ordered_limit_evidence.present) {
    const auto& evidence = candidate.ordered_limit_evidence;
    out << ",\"ordered_limit\":{";
    out << "\"index_uuid\":\"" << JsonEscape(evidence.index_uuid) << "\",";
    out << "\"limit_count\":" << evidence.limit_count << ",";
    out << "\"index_order_satisfied\":" << (evidence.index_order_satisfied ? "true" : "false") << ",";
    out << "\"sort_avoided\":" << (evidence.sort_avoided ? "true" : "false");
    out << "}";
  }
  if (candidate.summary_prune_evidence.present) {
    const auto& evidence = candidate.summary_prune_evidence;
    out << ",\"summary_prune\":{";
    out << "\"selected_access\":\"" << JsonEscape(evidence.selected_access) << "\",";
    out << "\"prune_reason\":\"" << JsonEscape(evidence.prune_reason) << "\",";
    out << "\"fallback_reason\":\"" << JsonEscape(evidence.fallback_reason) << "\",";
    out << "\"summary_status\":\"" << JsonEscape(evidence.summary_status) << "\",";
    out << "\"summary_generation\":" << evidence.summary_generation << ",";
    out << "\"candidate_ranges\":" << evidence.candidate_ranges << ",";
    out << "\"ranges_pruned\":" << evidence.ranges_pruned << ",";
    out << "\"ranges_scanned\":" << evidence.ranges_scanned << ",";
    out << "\"pages_considered\":" << evidence.pages_considered << ",";
    out << "\"pages_pruned\":" << evidence.pages_pruned << ",";
    out << "\"pages_scanned\":" << evidence.pages_scanned << ",";
    out << "\"authority_source\":\"" << JsonEscape(evidence.authority_source) << "\",";
    out << "\"base_row_mga_recheck_required\":"
        << (evidence.base_row_mga_recheck_required ? "true" : "false") << ",";
    out << "\"base_row_security_recheck_required\":"
        << (evidence.base_row_security_recheck_required ? "true" : "false") << ",";
    out << "\"redaction_state\":\"" << JsonEscape(evidence.redaction_state) << "\"";
    out << "}";
  }
  if (candidate.partition_segment_prune_evidence.present) {
    const auto& evidence = candidate.partition_segment_prune_evidence;
    out << ",\"partition_segment_prune\":{";
    out << "\"selected_access\":\"" << JsonEscape(evidence.selected_access) << "\",";
    out << "\"fallback_reason\":\"" << JsonEscape(evidence.fallback_reason) << "\",";
    out << "\"partitions_considered\":" << evidence.partitions_considered << ",";
    out << "\"partitions_pruned\":" << evidence.partitions_pruned << ",";
    out << "\"partitions_scanned\":" << evidence.partitions_scanned << ",";
    out << "\"segments_considered\":" << evidence.segments_considered << ",";
    out << "\"segments_pruned\":" << evidence.segments_pruned << ",";
    out << "\"segments_scanned\":" << evidence.segments_scanned << ",";
    out << "\"placements_considered\":" << evidence.placements_considered << ",";
    out << "\"placements_pruned\":" << evidence.placements_pruned << ",";
    out << "\"placements_scanned\":" << evidence.placements_scanned << ",";
    out << "\"candidate_ranges\":" << evidence.candidate_ranges << ",";
    out << "\"ranges_pruned\":" << evidence.ranges_pruned << ",";
    out << "\"ranges_scanned\":" << evidence.ranges_scanned << ",";
    out << "\"pages_considered\":" << evidence.pages_considered << ",";
    out << "\"pages_pruned\":" << evidence.pages_pruned << ",";
    out << "\"pages_scanned\":" << evidence.pages_scanned << ",";
    out << "\"authority_source\":\"" << JsonEscape(evidence.authority_source) << "\",";
    out << "\"base_row_mga_recheck_required\":"
        << (evidence.base_row_mga_recheck_required ? "true" : "false") << ",";
    out << "\"base_row_security_recheck_required\":"
        << (evidence.base_row_security_recheck_required ? "true" : "false") << ",";
    out << "\"pruning_metadata_visibility_authority\":"
        << (evidence.pruning_metadata_visibility_authority ? "true" : "false") << ",";
    out << "\"pruning_metadata_finality_authority\":"
        << (evidence.pruning_metadata_finality_authority ? "true" : "false") << ",";
    out << "\"decisions\":[";
    for (std::size_t i = 0; i < evidence.decisions.size(); ++i) {
      const auto& decision = evidence.decisions[i];
      out << "{\"object_type\":\"" << JsonEscape(decision.object_type)
          << "\",\"object_uuid\":\"" << JsonEscape(decision.object_uuid)
          << "\",\"parent_uuid\":\"" << JsonEscape(decision.parent_uuid)
          << "\",\"filespace_uuid\":\"" << JsonEscape(decision.filespace_uuid)
          << "\",\"decision\":\"" << JsonEscape(decision.decision)
          << "\",\"reason\":\"" << JsonEscape(decision.reason)
          << "\",\"pages\":" << decision.pages << "}";
      if (i + 1 != evidence.decisions.size()) out << ",";
    }
    out << "]}";
  }
  if (candidate.mga_page_finality_evidence.present) {
    const auto& evidence = candidate.mga_page_finality_evidence;
    out << ",\"mga_page_finality\":{";
    out << "\"evidence_name\":\"" << JsonEscape(evidence.evidence_name) << "\",";
    out << "\"accepted\":" << (evidence.accepted ? "true" : "false") << ",";
    out << "\"all_visible\":" << (evidence.all_visible ? "true" : "false") << ",";
    out << "\"all_final\":" << (evidence.all_final ? "true" : "false") << ",";
    out << "\"normal_mga_recheck_required\":"
        << (evidence.normal_mga_recheck_required ? "true" : "false") << ",";
    out << "\"finality_map_transaction_authority\":"
        << (evidence.finality_map_transaction_authority ? "true" : "false") << ",";
    out << "\"authority_source\":\"" << JsonEscape(evidence.authority_source) << "\",";
    out << "\"refusal_reason\":\"" << JsonEscape(evidence.refusal_reason) << "\",";
    out << "\"evidence_examined\":" << evidence.evidence_examined << ",";
    out << "\"accepted_count\":" << evidence.accepted_count << ",";
    out << "\"refused_count\":" << evidence.refused_count << ",";
    out << "\"stale_refusals\":" << evidence.stale_refusals << ",";
    out << "\"epoch_refusals\":" << evidence.epoch_refusals << ",";
    out << "\"horizon_refusals\":" << evidence.horizon_refusals << ",";
    out << "\"provenance_refusals\":" << evidence.provenance_refusals;
    out << "}";
  }
  out << "}";
  return out.str();
}

}  // namespace scratchbird::engine::optimizer
