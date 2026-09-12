// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/optimizer/access_path.hpp"
#include "../../src/engine/optimizer/optimizer_cost_full.hpp"
#include "binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace o = scratchbird::engine::optimizer;
namespace p = scratchbird::engine::planner;
using Target = o::OptimizerStatisticTarget;
namespace {
unsigned checks = 0, faults = 0;
void Check(bool value, const char* message) {
  ++checks; if (!value) throw std::runtime_error(message);
}
p::CanonicalPlannerUuid Id(unsigned n) {
  auto id = scratchbird::tests::BinaryUuid("019f0000-0000-7600-8000-000000000000");
  id.bytes[14] = n >> 8; id.bytes[15] = n; return id;
}
p::LogicalPlanNode Node() {
  auto node = p::MakeLogicalPlanNode(p::LogicalPlanNodeKind::kDmlRead,
      p::PhysicalAccessKind::kTableScan, "query.read", "table_source");
  node.required_object_uuids = {Id(1)}; return node;
}
o::OptimizerStatisticsCatalog Statistics(double rows) {
  auto result = o::DefaultLocalStatisticsCatalog();
  for (const char* name : {"row_count", "visible_row_count", "page_count"}) {
    Check(result.Add(o::MakeStatistic(name, "relation", Target::Object(Id(1)), rows,
        o::StatisticSource::kCatalogExact, 1, 0, o::CostConfidence::kExact)), "catalog fixture");
  }
  return result;
}
const o::PlanCandidate& Table(const std::vector<o::PlanCandidate>& candidates) {
  const auto found = std::ranges::find_if(candidates, [](const auto& c) {
    return c.access_kind == p::PhysicalAccessKind::kTableScan;
  });
  Check(found != candidates.end(), "actual table candidate present"); return *found;
}
void VerifyAccess() {
  auto node = Node(); const auto empty = Statistics(0);
  const auto candidates = o::GenerateLocalAccessPathCandidates(node, empty);
  const auto& table = Table(candidates);
  Check(table.cost.selectable && table.estimated_rows == 0, "real empty source is not 1000-row fallback");
  const auto rows = std::ranges::find_if(table.statistic_inputs, [](const auto& s) {
    return s.statistic_name == "visible_row_count";
  });
  Check(rows != table.statistic_inputs.end() && rows->value == 0 &&
        rows->target == Target::Object(Id(1)) && rows->source == o::StatisticSource::kCatalogExact,
        "candidate retains actual zero and binary provenance");
  const auto populated = Statistics(400);
  Check(Table(o::GenerateLocalAccessPathCandidates(node, populated)).estimated_rows == 400,
        "actual populated count reaches candidate");
  const auto overflowing = Statistics(18446744073709551616.0);
  const auto overflow_candidates = o::GenerateLocalAccessPathCandidates(node, overflowing);
  const auto& overflow_table = Table(overflow_candidates);
  const auto overflow_row = std::ranges::find_if(overflow_table.statistic_inputs, [](const auto& s) {
    return s.statistic_name == "row_count";
  });
  Check(overflow_table.estimated_rows == 1000 && overflow_row != overflow_table.statistic_inputs.end() &&
        overflow_row->target == Target::LocalDefault() && overflow_row->value == 1000,
        "retained statistic binding must match the actually used range-checked fallback");
  auto specialized = node;
  specialized.kind = p::LogicalPlanNodeKind::kNoSqlOperation;
  specialized.access_kind = p::PhysicalAccessKind::kFullTextProbe;
  const auto specialized_candidates = o::GenerateLocalAccessPathCandidates(specialized, empty);
  Check(specialized_candidates.size() == 1 && specialized_candidates[0].estimated_rows == 0,
        "specialized provider input retains real zero cardinality");
  Check(!specialized_candidates[0].cost.selectable,
        "zero cardinality does not fabricate missing provider authority");
  auto missing_node = node; missing_node.required_object_uuids = {Id(2)};
  const auto missing = o::GenerateLocalAccessPathCandidates(missing_node, empty);
  Check(Table(missing).estimated_rows == 1000 && Table(missing).uses_local_default_statistics,
        "only explicit fallback can select policy values");
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto changed = node; changed.required_object_uuids[0].bytes[bit / 8] ^= 1U << (bit % 8);
    const auto& b = changed.required_object_uuids[0].bytes;
    const bool v7 = (b[6] >> 4) == 7 && (b[8] & 0xc0) == 0x80;
    auto result = o::GenerateLocalAccessPathCandidates(changed, empty);
    if (v7) Check(Table(result).estimated_rows == 1000, "all object bits distinguish source statistics");
    else Check(result.size() == 1 && !result[0].cost.selectable, "invalid UUID cannot enter source costing");
  }
  node.required_object_uuids.push_back(Id(2));
  auto ambiguous = o::GenerateLocalAccessPathCandidates(node, empty);
  Check(ambiguous.size() == 1 && !ambiguous[0].cost.selectable, "single-source cost cannot choose arbitrary first object");
  // The retained candidate is unaffected by subsequent independent catalogs.
  Check(rows->value == 0 && rows->target.object_uuid == Id(1), "retained binding lifetime");
}
std::vector<o::OptimizerMetricCostInput> Metrics() {
  std::vector<o::OptimizerMetricCostInput> result;
  for (const auto& [name, value] : {std::pair{"feedback.estimated_rows", 10.0},
                                  std::pair{"feedback.actual_rows", 40.0}}) {
    o::OptimizerMetricCostInput m;
    m.metric_name = name; m.value = value; m.policy_allowed = true;
    m.operator_family = "access_path"; m.plan_shape = "table_scan";
    m.cost_profile_id = "runtime_metric_local_v1"; m.statistic_target = Target::Object(Id(1));
    result.push_back(m);
  }
  return result;
}
void VerifyFeedback() {
  o::CostVector input; input.row_cost = 100;
  const auto baseline = o::ApplyMetricFeedbackCost(input, {});
  const auto metrics = Metrics();
  // This gate covers binding/consumption, not the composed calibration formula.
  Check(o::ApplyMetricFeedbackCost(input, metrics).row_cost > baseline.row_cost,
        "actual valid grouped feedback changes row cost");
  auto rejected = [&](const auto& modified) {
    auto result = o::ApplyMetricFeedbackCost(input, modified);
    Check(result.row_cost == baseline.row_cost && result.total_cost == baseline.total_cost &&
          result.cpu_units == baseline.cpu_units, "invalid feedback group cannot partially calibrate");
  };
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto changed = metrics;
    changed[1].statistic_target->object_uuid.bytes[bit / 8] ^= 1U << (bit % 8);
    rejected(changed);
  }
  auto changed = metrics; changed[1].plan_shape = "index_probe"; rejected(changed);
  changed = metrics; changed[1].operator_family = "join"; rejected(changed);
  changed = metrics; changed[1].cost_profile_id = "other_policy"; rejected(changed);
  changed = metrics; changed[1].statistic_target.reset(); rejected(changed);
  changed = metrics; changed.push_back(changed.front()); rejected(changed);
  changed = metrics; changed[0].transaction_finality_authority = "parser"; rejected(changed);
  changed = metrics; changed[0].policy_allowed = false; rejected(changed);
  changed = metrics; changed[0].freshness_microseconds = 60000001; rejected(changed);
  changed = metrics; changed.pop_back(); rejected(changed);
  changed = metrics; changed.erase(changed.begin()); rejected(changed);
  for (double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(), 18446744073709551616.0}) {
    changed = metrics; changed[1].value = invalid; rejected(changed);
  }
  changed = metrics;
  for (auto& m : changed) m.statistic_target = Target::LocalDefault();
  rejected(changed);
  o::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "access_path"; feedback.plan_shape = "table_scan";
  feedback.estimated_rows = 10; feedback.actual_rows = 40;
  feedback.statistic_target = Target::Object(Id(1));
  auto profile = o::EvaluateOptimizerRuntimeFeedback(feedback);
  Check(profile.ok && profile.cost_profile.statistic_target == feedback.statistic_target,
        "calibration profile retains binary object target separately from labels");
  feedback.statistic_target = Target::LocalDefault();
  feedback.peak_memory_bytes = 100000;
  Check(!o::EvaluateOptimizerRuntimeFeedback(feedback).ok, "policy default cannot claim observed runtime feedback");
  Check(!o::EvaluateOptimizerRuntimeFeedback(feedback).memory_grant.apply,
        "refused feedback cannot retain an applied memory recommendation");
  Check(!o::BuildOptimizerCalibratedCostProfile(feedback, profile).apply,
        "old successful status cannot qualify a changed unsafe feedback object");
  o::OptimizerMetricCostInput multiplier;
  multiplier.metric_name = "operator_latency_multiplier"; multiplier.value = 10;
  multiplier.policy_allowed = true;
  input.row_cost = std::numeric_limits<std::uint64_t>::max();
  Check(o::ApplyMetricFeedbackCost(input, {multiplier}).row_cost == input.row_cost,
        "cost multiplication saturates without overflowing uint64 cast");
  input.row_cost = 7; multiplier.value = 0.25;
  Check(o::ApplyMetricFeedbackCost(input, {multiplier}).row_cost == 2, "fractional work conservatively rounds upward");
}
void VerifyFaults() {
  const auto node = Node(); const auto statistics = Statistics(0);
  // Initialize any static costing metadata before persistent allocation faults.
  Check(!o::GenerateLocalAccessPathCandidates(node, statistics).empty(), "fault baseline");
  bool finished = false;
  for (long index = 0; index < 4096; ++index) {
    std::optional<std::vector<o::PlanCandidate>> result;
    fault::remaining = index; fault::hit = false;
    try { result = o::GenerateLocalAccessPathCandidates(node, statistics); }
    catch (const std::bad_alloc&) {}
    const bool hit = fault::hit; fault::remaining = -1;
    if (!hit) { Check(result && Table(*result).estimated_rows == 0, "unfaulted zero survives costing"); finished = true; break; }
    ++faults; Check(!result, "allocation failure cannot publish partial candidate vector");
    Check(statistics.TryEstimateUnsigned("row_count", Target::Object(Id(1))) == 0,
          "failed planning leaves source statistics unchanged");
  }
  Check(finished, "candidate allocation sweep exhausted");
}
}
int main() {
  try {
    VerifyAccess(); VerifyFeedback(); VerifyFaults();
    std::cout << "PASS access statistics binding checks=" << checks << " faults=" << faults << '\n';
  } catch (const std::exception& error) {
    fault::remaining = -1; std::cerr << error.what() << '\n'; return 1;
  }
}
