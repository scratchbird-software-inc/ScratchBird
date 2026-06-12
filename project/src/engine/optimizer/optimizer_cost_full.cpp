// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_cost_full.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

void Finish(CostVector* cost) {
  cost->total_cost = cost->startup_cost + cost->row_cost + cost->io_cost + cost->memory_cost + cost->uncertainty_cost;
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

std::uint64_t SaturatingMul(std::uint64_t left, std::uint64_t right) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left * right;
}

std::uint64_t CostUnits(double value) {
  if (value <= 0.0) return 0;
  if (value >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(std::ceil(value));
}

std::uint64_t ScaleCost(std::uint64_t value, double multiplier) {
  return CostUnits(static_cast<double>(value) * std::clamp(multiplier, 0.25, 4.0));
}

double ConfidenceUncertaintyRatio(CostConfidence confidence) {
  switch (confidence) {
    case CostConfidence::kExact: return 0.0;
    case CostConfidence::kHigh: return 0.05;
    case CostConfidence::kMedium: return 0.15;
    case CostConfidence::kLow: return 0.35;
    case CostConfidence::kUnknown: return 0.75;
    case CostConfidence::kRejected: return 1.0;
  }
  return 0.75;
}

std::uint64_t SortWorkRows(std::uint64_t rows, std::uint64_t keys) {
  if (rows <= 1) return rows;
  const double log2_rows = std::log2(static_cast<double>(rows));
  return CostUnits(static_cast<double>(rows) * log2_rows * static_cast<double>(std::max<std::uint64_t>(keys, 1)));
}

std::uint64_t MetricUnsigned(double value) {
  if (value <= 0.0) return 0;
  if (value >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(std::llround(value));
}

bool IsRuntimeFeedbackMetric(const std::string& name) {
  return name == "feedback.estimated_rows" ||
         name == "feedback.actual_rows" ||
         name == "feedback.estimated_pages" ||
         name == "feedback.actual_pages" ||
         name == "feedback.estimated_io_operations" ||
         name == "feedback.actual_io_operations" ||
         name == "feedback.estimated_visibility_recheck_rows" ||
         name == "feedback.actual_visibility_recheck_rows" ||
         name == "feedback.estimated_spill_bytes" ||
         name == "feedback.actual_spill_bytes" ||
         name == "feedback.memory_grant_bytes" ||
         name == "feedback.peak_memory_bytes" ||
         name == "feedback.estimated_latency_microseconds" ||
         name == "feedback.actual_latency_microseconds" ||
         name == "feedback.estimated_resource_units" ||
         name == "feedback.actual_resource_units";
}

void ApplyRuntimeFeedbackMetric(const OptimizerMetricCostInput& metric,
                                OptimizerRuntimeFeedback* feedback) {
  if (feedback == nullptr) return;
  if (feedback->operator_family.empty()) feedback->operator_family = metric.operator_family;
  if (feedback->plan_shape.empty()) feedback->plan_shape = metric.plan_shape;
  if (!metric.cost_profile_id.empty()) feedback->cost_profile_id = metric.cost_profile_id;
  feedback->freshness_microseconds = std::max(feedback->freshness_microseconds, metric.freshness_microseconds);
  feedback->policy_allowed = feedback->policy_allowed && metric.policy_allowed;
  feedback->advisory_only = feedback->advisory_only && metric.advisory_only;
  feedback->mga_visibility_recheck_preserved = feedback->mga_visibility_recheck_preserved &&
                                               metric.mga_visibility_recheck_preserved;
  feedback->parser_or_reference_authority = feedback->parser_or_reference_authority ||
                                         metric.parser_or_reference_authority;
  if (!metric.transaction_finality_authority.empty()) {
    feedback->transaction_finality_authority = metric.transaction_finality_authority;
  }

  const auto value = MetricUnsigned(metric.value);
  if (metric.metric_name == "feedback.estimated_rows") {
    feedback->estimated_rows = value;
  } else if (metric.metric_name == "feedback.actual_rows") {
    feedback->actual_rows = value;
  } else if (metric.metric_name == "feedback.estimated_pages") {
    feedback->estimated_pages = value;
  } else if (metric.metric_name == "feedback.actual_pages") {
    feedback->actual_pages = value;
  } else if (metric.metric_name == "feedback.estimated_io_operations") {
    feedback->estimated_io_operations = value;
  } else if (metric.metric_name == "feedback.actual_io_operations") {
    feedback->actual_io_operations = value;
  } else if (metric.metric_name == "feedback.estimated_visibility_recheck_rows") {
    feedback->estimated_visibility_recheck_rows = value;
  } else if (metric.metric_name == "feedback.actual_visibility_recheck_rows") {
    feedback->actual_visibility_recheck_rows = value;
  } else if (metric.metric_name == "feedback.estimated_spill_bytes") {
    feedback->estimated_spill_bytes = value;
  } else if (metric.metric_name == "feedback.actual_spill_bytes") {
    feedback->actual_spill_bytes = value;
  } else if (metric.metric_name == "feedback.memory_grant_bytes") {
    feedback->memory_grant_bytes = value;
  } else if (metric.metric_name == "feedback.peak_memory_bytes") {
    feedback->peak_memory_bytes = value;
  } else if (metric.metric_name == "feedback.estimated_latency_microseconds") {
    feedback->estimated_latency_microseconds = value;
  } else if (metric.metric_name == "feedback.actual_latency_microseconds") {
    feedback->actual_latency_microseconds = value;
  } else if (metric.metric_name == "feedback.estimated_resource_units") {
    feedback->estimated_resource_units = value;
  } else if (metric.metric_name == "feedback.actual_resource_units") {
    feedback->actual_resource_units = value;
  }
}

}  // namespace

CostVector EstimateBaseOperatorCost(const OptimizerCostEnvironment& environment,
                                    planner::PhysicalAccessKind access_kind,
                                    std::uint64_t rows,
                                    std::uint64_t row_width_bytes,
                                    std::uint64_t page_count) {
  CostFormulaInput input;
  input.access_kind = access_kind;
  input.estimated_rows = rows;
  input.row_width_bytes = row_width_bytes;
  input.sequential_pages = page_count;
  input.required_memory_bytes = SaturatingMul(rows, std::max<std::uint64_t>(row_width_bytes, 1));
  input.input_confidence = CostConfidence::kMedium;
  input.reason = environment.cost_profile_id;
  return EstimateCostVector(environment, input);
}

CostVector EstimateCostVector(const OptimizerCostEnvironment& environment,
                              const CostFormulaInput& input) {
  CostVector cost = EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead,
                                                                  input.access_kind,
                                                                  "query.operator",
                                                                  planner::PhysicalAccessKindName(input.access_kind)));
  cost.row_cost = 0;
  cost.io_cost = 0;
  cost.memory_cost = 0;
  cost.uncertainty_cost = 0;
  cost.reason = input.reason.empty() ? environment.cost_profile_id : input.reason;
  cost.confidence = input.input_confidence;

  const double cache_discount = std::clamp(1.0 - (std::clamp(input.cache_hit_ratio, 0.0, 1.0) * 0.85), 0.10, 1.0);
  const double prefetch_fraction = input.sequential_pages == 0
                                       ? 0.0
                                       : std::clamp(static_cast<double>(input.prefetch_pages) /
                                                        static_cast<double>(input.sequential_pages),
                                                    0.0,
                                                    1.0);
  const double prefetch_discount = std::clamp(1.0 - (prefetch_fraction * 0.25), 0.75, 1.0);
  const double ordered_heap_fraction = std::clamp(input.heap_correlation, 0.0, 1.0);
  const double random_discount = std::clamp(1.0 - (ordered_heap_fraction * 0.65), 0.35, 1.0);

  const double seq_io = static_cast<double>(input.sequential_pages) *
                        environment.sequential_page_cost *
                        cache_discount *
                        prefetch_discount;
  const double random_io = static_cast<double>(input.random_pages) *
                           environment.random_page_cost *
                           cache_discount *
                           random_discount;
  cost.io_cost = CostUnits((seq_io + random_io) * 1000.0);

  const auto tuple_visits = SaturatingAdd(input.estimated_rows,
                         SaturatingAdd(input.false_positive_rows, input.duplicate_rows));
  const auto index_work = SaturatingAdd(input.index_probe_count, input.index_tuple_visits);
  cost.row_cost = CostUnits((static_cast<double>(tuple_visits) * environment.cpu_tuple_cost +
                             static_cast<double>(index_work) * environment.cpu_index_tuple_cost +
                             static_cast<double>(SaturatingAdd(input.visibility_recheck_rows,
                                                               SaturatingAdd(input.false_positive_rows,
                                                                             input.duplicate_rows))) *
                                 environment.visibility_tuple_cost +
                             static_cast<double>(SaturatingAdd(input.estimated_rows, input.index_probe_count)) *
                                 environment.cpu_operator_cost) *
                            1000.0);

  cost.memory_cost = CostUnits(static_cast<double>(input.required_memory_bytes / 1024) *
                               environment.memory_kib_cost *
                               1000.0);

  if (input.sort_input_rows != 0 && !input.ordering_satisfied) {
    const auto sort_work = SortWorkRows(input.sort_input_rows, input.sort_key_count);
    cost.startup_cost = SaturatingAdd(cost.startup_cost, DefaultCostModelConstants().sort_startup);
    cost.row_cost = SaturatingAdd(cost.row_cost,
                                  CostUnits(static_cast<double>(sort_work) *
                                            environment.cpu_operator_cost *
                                            1000.0));
    const auto sort_memory = SaturatingMul(input.sort_input_rows,
                                           std::max<std::uint64_t>(input.row_width_bytes, 1));
    cost.memory_cost = SaturatingAdd(cost.memory_cost,
                                     CostUnits(static_cast<double>(sort_memory / 1024) *
                                               environment.memory_kib_cost *
                                               1000.0));
  } else if (input.sort_input_rows != 0 && input.ordering_satisfied) {
    cost.reason += ":sort_avoided";
  }

  std::uint64_t spill_bytes = input.spill_bytes;
  if (environment.memory_budget_bytes != 0 && input.required_memory_bytes > environment.memory_budget_bytes) {
    if (!input.spill_capable) {
      cost.selectable = false;
      cost.confidence = CostConfidence::kRejected;
      cost.rejection_reason = "memory_budget_exceeded_without_spill";
    } else {
      spill_bytes = SaturatingAdd(spill_bytes, input.required_memory_bytes - environment.memory_budget_bytes);
    }
  }
  if (spill_bytes != 0) {
    const auto spill_pages = (spill_bytes + 4095) / 4096;
    cost.io_cost = SaturatingAdd(cost.io_cost,
                                 CostUnits(static_cast<double>(spill_pages) *
                                           environment.spill_page_cost *
                                           1000.0));
    cost.uncertainty_cost = SaturatingAdd(cost.uncertainty_cost, spill_pages);
    cost.reason += ":spill_expected";
  }

  const auto known_cost = SaturatingAdd(SaturatingAdd(cost.startup_cost, cost.row_cost),
                                       SaturatingAdd(cost.io_cost, cost.memory_cost));
  cost.uncertainty_cost = SaturatingAdd(cost.uncertainty_cost,
                                        CostUnits(static_cast<double>(known_cost) *
                                                  ConfidenceUncertaintyRatio(cost.confidence)));
  Finish(&cost);
  return cost;
}

CostVector ApplyFilespaceCost(CostVector cost, const PageFilespaceStats& filespace) {
  if (!OptimizerStatsIdentityIsUsable(filespace.identity)) return ApplyMissingStatsFallbackCost(std::move(cost), CostConfidence::kLow, "filespace_stats_unusable");
  const double health_multiplier = filespace.degraded ? 10.0 : std::clamp(2.0 - filespace.health_score, 0.25, 10.0);
  cost.io_cost = static_cast<std::uint64_t>(static_cast<double>(cost.io_cost) * filespace.sequential_latency_score * health_multiplier);
  if (filespace.degraded) cost.uncertainty_cost += 10000;
  Finish(&cost);
  return cost;
}

CostVector ApplyMemoryAndSpillCost(CostVector cost,
                                   const OptimizerCostEnvironment& environment,
                                   std::uint64_t required_memory_bytes,
                                   bool spill_capable) {
  cost.memory_cost += required_memory_bytes / 1024;
  if (environment.memory_budget_bytes != 0 && required_memory_bytes > environment.memory_budget_bytes) {
    if (!spill_capable) {
      cost.selectable = false;
      cost.confidence = CostConfidence::kRejected;
      cost.rejection_reason = "memory_budget_exceeded_without_spill";
    } else {
      const auto excess_pages = (required_memory_bytes - environment.memory_budget_bytes + 4095) / 4096;
      cost.io_cost += static_cast<std::uint64_t>(static_cast<double>(excess_pages) * environment.spill_page_cost);
      cost.uncertainty_cost += excess_pages;
      cost.reason = "spill_expected";
    }
  }
  Finish(&cost);
  return cost;
}

CostVector ApplyMetricFeedbackCost(CostVector cost, const std::vector<OptimizerMetricCostInput>& metrics) {
  std::optional<OptimizerRuntimeFeedback> runtime_feedback;
  for (const auto& metric : metrics) {
    if (IsRuntimeFeedbackMetric(metric.metric_name)) {
      if (!runtime_feedback) runtime_feedback.emplace();
      ApplyRuntimeFeedbackMetric(metric, &*runtime_feedback);
      continue;
    }
    if (!metric.policy_allowed || metric.freshness_microseconds > 60000000) continue;
    if (metric.metric_name == "operator_latency_multiplier") {
      const auto multiplier = std::clamp(metric.value, 0.25, 10.0);
      cost.row_cost = static_cast<std::uint64_t>(static_cast<double>(cost.row_cost) * multiplier);
    } else if (metric.metric_name == "io_latency_multiplier") {
      const auto multiplier = std::clamp(metric.value, 0.25, 10.0);
      cost.io_cost = static_cast<std::uint64_t>(static_cast<double>(cost.io_cost) * multiplier);
    } else if (metric.metric_name == "estimate_uncertainty") {
      cost.uncertainty_cost += static_cast<std::uint64_t>(std::clamp(metric.value, 0.0, 1000000.0));
    }
  }
  if (runtime_feedback) {
    cost = ApplyOptimizerRuntimeFeedbackCost(std::move(cost), *runtime_feedback);
  }
  Finish(&cost);
  return cost;
}

CostVector ApplyOptimizerCalibratedCostProfile(CostVector cost, const OptimizerCalibratedCostProfile& profile) {
  if (!profile.apply) {
    Finish(&cost);
    return cost;
  }
  cost.row_cost = ScaleCost(cost.row_cost, (profile.row_cost_multiplier + profile.visibility_cost_multiplier) / 2.0);
  cost.io_cost = ScaleCost(cost.io_cost, (profile.page_cost_multiplier + profile.io_cost_multiplier) / 2.0);
  cost.memory_cost = ScaleCost(cost.memory_cost, profile.memory_cost_multiplier);
  cost.startup_cost = ScaleCost(cost.startup_cost, profile.latency_cost_multiplier);
  if (profile.spill_penalty_pages != 0) {
    cost.io_cost = SaturatingAdd(cost.io_cost, CostUnits(static_cast<double>(profile.spill_penalty_pages) * 1500.0));
    cost.uncertainty_cost = SaturatingAdd(cost.uncertainty_cost, profile.spill_penalty_pages);
  }
  cost.uncertainty_cost = SaturatingAdd(cost.uncertainty_cost, profile.uncertainty_penalty);
  if (!profile.profile_id.empty()) {
    cost.reason += ":feedback=" + profile.profile_id;
  }
  Finish(&cost);
  return cost;
}

CostVector ApplyOptimizerRuntimeFeedbackCost(CostVector cost, const OptimizerRuntimeFeedback& feedback) {
  const auto status = EvaluateOptimizerRuntimeFeedback(feedback);
  if (!status.ok || !status.applied) {
    cost.reason += ":feedback_rejected=" + status.diagnostic_code;
    Finish(&cost);
    return cost;
  }
  return ApplyOptimizerCalibratedCostProfile(std::move(cost), status.cost_profile);
}

CostVector ApplyAgentRecommendationCost(CostVector cost, const std::vector<AgentCostRecommendation>& recommendations) {
  for (const auto& recommendation : recommendations) {
    if (!recommendation.policy_accepted) continue;
    const auto multiplier = std::clamp(recommendation.cost_multiplier, 0.25, 10.0);
    if (recommendation.recommendation_kind == "prefer") {
      cost.total_cost = static_cast<std::uint64_t>(static_cast<double>(cost.total_cost) * multiplier);
    } else if (recommendation.recommendation_kind == "avoid") {
      cost.uncertainty_cost += static_cast<std::uint64_t>(1000.0 * multiplier);
    }
  }
  Finish(&cost);
  return cost;
}

CostVector ApplyMgaPressureCost(CostVector cost,
                                const OptimizerMgaPressureEvidence& pressure) {
  if (!pressure.observed_runtime_metric || !pressure.trusted_metric_digest ||
      !pressure.fresh || pressure.parser_or_reference_authority ||
      pressure.transaction_finality_authority || pressure.visibility_authority ||
      pressure.recovery_authority) {
    cost.selectable = false;
    cost.confidence = CostConfidence::kRejected;
    cost.rejection_reason = "mga_pressure_untrusted_or_unsafe_authority";
    cost.reason += ":mga_pressure_rejected";
    Finish(&cost);
    return cost;
  }
  if (!pressure.exact_recheck_required ||
      !pressure.mga_visibility_recheck_required ||
      !pressure.security_recheck_required) {
    cost.selectable = false;
    cost.confidence = CostConfidence::kRejected;
    cost.rejection_reason = "mga_pressure_recheck_proof_required";
    cost.reason += ":mga_pressure_recheck_required";
    Finish(&cost);
    return cost;
  }

  const auto cleanup_pages = (pressure.cleanup_debt_bytes + 4095) / 4096;
  const auto retained_pages = (pressure.retained_dead_bytes + 4095) / 4096;
  cost.io_cost = SaturatingAdd(cost.io_cost, SaturatingAdd(cleanup_pages, retained_pages));
  cost.row_cost = SaturatingAdd(cost.row_cost,
                                SaturatingAdd(pressure.index_backlog_entries,
                                              pressure.commit_fence_backlog));
  cost.uncertainty_cost = SaturatingAdd(
      cost.uncertainty_cost,
      SaturatingAdd(SaturatingAdd(pressure.chain_depth_bucket * 250,
                                  pressure.chain_scatter_bucket * 100),
                    CostUnits(std::clamp(pressure.same_page_update_ratio,
                                         0.0,
                                         1.0) *
                              1000.0)));
  if (pressure.cleanup_debt_bytes != 0 || pressure.retained_dead_bytes != 0 ||
      pressure.index_backlog_entries != 0 || pressure.chain_depth_bucket != 0 ||
      pressure.chain_scatter_bucket != 0 ||
      pressure.same_page_update_ratio != 0.0 ||
      pressure.commit_fence_backlog != 0) {
    cost.reason += ":mga_pressure_costed";
  }
  Finish(&cost);
  return cost;
}

CostVector ApplyMissingStatsFallbackCost(CostVector cost, CostConfidence fallback_confidence, std::string diagnostic_reason) {
  cost.confidence = fallback_confidence;
  cost.uncertainty_cost += DefaultCostModelConstants().unknown_stats_uncertainty;
  cost.reason = std::move(diagnostic_reason);
  Finish(&cost);
  return cost;
}

}  // namespace scratchbird::engine::optimizer
