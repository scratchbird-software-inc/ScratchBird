// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "optimizer_contract.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <tuple>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
using scratchbird::tests::FixtureUuid;
using Uuid = scratchbird::core::platform::Uuid;
using Target = opt::OptimizerStatisticTarget;
using Kind = plan::PhysicalAccessKind;

namespace {
void Require(bool value, const char* message) {
  if (!value) {
    std::cerr << "native cost statistics: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

auto Fields(const opt::OptimizerStatistic& s) {
  return std::tie(s.statistic_name, s.scope, s.target, s.value, s.source,
      s.stats_epoch, s.freshness_microseconds, s.confidence, s.available,
      s.cluster_only, s.value_domain, s.exact_unsigned_value);
}

opt::OptimizerStatistic Count(const char* name, Target target, std::uint64_t value,
                               std::uint64_t epoch = 41) {
  return opt::MakeUnsignedStatistic(name, "native-cost-fixture", target, value,
      target.kind == opt::OptimizerStatisticTargetKind::kLocalDefault
          ? opt::StatisticSource::kPolicyDefault : opt::StatisticSource::kCatalogExact,
      epoch, 0, target.kind == opt::OptimizerStatisticTargetKind::kLocalDefault
          ? opt::CostConfidence::kLow : opt::CostConfidence::kHigh);
}

opt::OptimizerStatisticsCatalog Catalog(const std::vector<opt::OptimizerStatistic>& inputs) {
  opt::OptimizerStatisticsCatalog catalog;
  for (const auto& input : inputs) Require(catalog.Add(input), "fixture admission");
  return catalog;
}

plan::LogicalPlan Logical(Kind kind, std::vector<Uuid> objects,
                          std::vector<std::string> descriptors = {}) {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "native-cost-statistics";
  if (kind != Kind::kJoinHash) {
    auto base = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
        Kind::kNone, "native-cost-base", "native_cost_base");
    base.required_object_uuids = objects;
    base.required_descriptors = {"desc.native-cost"};
    logical.nodes.push_back(std::move(base));
  }
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
      kind, "native-cost-subject", "native_cost_subject");
  node.required_object_uuids = std::move(objects);
  node.required_descriptors = std::move(descriptors);
  node.required_descriptors.push_back("desc.native-cost");
  logical.nodes.push_back(std::move(node));
  return logical;
}

const opt::OptimizerCandidate& Subject(const opt::OptimizedPlan& result,
                                       Kind kind = Kind::kNone) {
  const auto found = std::find_if(result.candidates.begin(), result.candidates.end(),
      [&](const auto& candidate) {
        return candidate.node.operation_id == "native-cost-subject" &&
            (kind == Kind::kNone || candidate.plan_candidate.access_kind == kind);
      });
  Require(found != result.candidates.end(), "subject candidate missing");
  return *found;
}

void ExactInputs(const opt::OptimizerCandidate& candidate,
                 const std::vector<opt::OptimizerStatistic>& expected) {
  const auto& actual = candidate.plan_candidate.statistic_inputs;
  Require(actual.size() == expected.size(), "consumed input count");
  for (const auto& statistic : expected)
    Require(std::count_if(actual.begin(), actual.end(), [&](const auto& found) {
      return Fields(statistic) == Fields(found);
    }) == 1, "exact native identity/value/source/epoch record missing or duplicated");
}

bool IndependentV7(const Uuid& uuid) {
  return (uuid.bytes[6] & 0xf0) == 0x70 && (uuid.bytes[8] & 0xc0) == 0x80;
}

void EveryIdentityByte() {
  const auto object = FixtureUuid(2205, 1);
  const auto logical = Logical(Kind::kAggregateGeneric, {object});
  for (std::size_t position = 0; position != 16; ++position) {
    for (unsigned octet = 0; octet != 256; ++octet) {
      auto changed = object;
      changed.bytes[position] = static_cast<scratchbird::core::platform::byte>(octet);
      const auto record = Count("row_count", Target::Object(changed), 9007199254740993ULL);
      opt::OptimizerStatisticsCatalog catalog;
      const bool accepted = catalog.Add(record);
      Require(accepted == IndependentV7(changed), "independent version/variant admission");
      const auto result = opt::OptimizeLogicalPlanWithStatistics(logical, catalog);
      const auto& candidate = Subject(result);
      if (changed == object) {
        ExactInputs(candidate, {record});
        Require(!candidate.plan_candidate.uses_policy_default_statistics,
                "exact native match called a fallback");
      } else {
        ExactInputs(candidate, {});
        Require(candidate.plan_candidate.uses_policy_default_statistics,
                "different UUID borrowed another object's row count");
        Require(candidate.statistics_version == "statistics-unbound", "invented fallback epoch");
      }
      if (accepted) {
        const auto own = opt::OptimizeLogicalPlanWithStatistics(
            Logical(Kind::kAggregateGeneric, {changed}), catalog);
        ExactInputs(Subject(own), {record});
        Require(Subject(own).cost.row_cost >= 9007199254740993ULL,
                "unsigned row cardinality lost precision during costing");
      }
    }
  }
  Require(!Target::Object({}).Valid(), "nil target became wildcard");
}

void JoinInputsAndFallbacks() {
  const auto left = FixtureUuid(2205, 2), right = FixtureUuid(2205, 3);
  const auto logical = Logical(Kind::kJoinHash, {left, right});
  const auto left_rows = Count("row_count", Target::Object(left), 1000, 41);
  const auto right_rows = Count("row_count", Target::Object(right), 500, 43);
  const auto memory = Count("memory_grant_available_bytes", Target::LocalDefault(), 1048576, 11);
  const auto selectivity = opt::MakeStatistic("join_selectivity", "native-cost-fixture",
      Target::LocalDefault(), .10, opt::StatisticSource::kPolicyDefault, 29, 10,
      opt::CostConfidence::kLow);
  const std::vector<opt::OptimizerStatistic> expected{left_rows, right_rows, memory, selectivity};
  const auto result = opt::OptimizeLogicalPlanWithStatistics(logical, Catalog(expected));
  Require(result.ok, "join plan must remain executable-shaped");
  const auto& joined = Subject(result, Kind::kJoinHash);
  ExactInputs(joined, expected);
  Require(joined.statistics_version == "statistics-epochs:11:29:41:43", "real join epochs");
  Require(joined.plan_candidate.uses_local_default_statistics &&
          joined.plan_candidate.uses_policy_default_statistics &&
          joined.cost.confidence >= opt::CostConfidence::kLow, "join default source flags");
  ExactInputs(Subject(result, Kind::kJoinNestedLoop), {left_rows, right_rows, memory});
  auto changed = expected;
  changed[0] = Count("row_count", Target::Object(left), 100000, 47);
  const auto larger = opt::OptimizeLogicalPlanWithStatistics(logical, Catalog(changed));
  Require(Subject(larger, Kind::kJoinHash).cost.total_cost > joined.cost.total_cost,
          "retained rows do not actually affect join cost");

  const auto missing_selectivity = opt::OptimizeLogicalPlanWithStatistics(logical,
      Catalog({left_rows, right_rows, memory}));
  const auto& fallback = Subject(missing_selectivity, Kind::kJoinHash);
  ExactInputs(fallback, {left_rows, right_rows, memory});
  Require(fallback.plan_candidate.uses_policy_default_statistics &&
          fallback.cost.confidence == opt::CostConfidence::kUnknown &&
          fallback.cost.uncertainty_cost >= joined.cost.uncertainty_cost &&
          std::find(fallback.plan_candidate.statistics_diagnostics.begin(),
                    fallback.plan_candidate.statistics_diagnostics.end(),
                    "statistics-missing:join_selectivity") !=
              fallback.plan_candidate.statistics_diagnostics.end(),
          "unmeasured selectivity must carry uncertainty without fake statistics");

  auto stale = left_rows;
  stale.freshness_microseconds = 60000001;
  const auto local_rows = Count("visible_row_count", Target::LocalDefault(), 7, 53);
  const auto stale_plan = opt::OptimizeLogicalPlanWithStatistics(logical,
      Catalog({stale, right_rows, local_rows, memory, selectivity}));
  for (const auto& candidate : stale_plan.candidates) {
    if (candidate.node.operation_id != "native-cost-subject") continue;
    const auto& inputs = candidate.plan_candidate.statistic_inputs;
    Require(std::none_of(inputs.begin(), inputs.end(), [&](const auto& s) {
      return s.target == Target::Object(left);
    }), "stale object statistics claimed as consumed");
    Require(candidate.plan_candidate.uses_local_default_statistics,
            "local substitute hidden as exact object data");
  }
  Require(Subject(stale_plan, Kind::kJoinHash).rejected,
          "default cardinality must not invent exact-row join eligibility");
  const auto cross = opt::OptimizeLogicalPlanWithStatistics(
      Logical(Kind::kJoinHash, {left, right}, {"join.cross"}), Catalog({left_rows, right_rows, memory}));
  for (const auto& candidate : cross.candidates)
    if (candidate.node.operation_id == "native-cost-subject")
      Require(std::find(candidate.plan_candidate.statistics_diagnostics.begin(),
                        candidate.plan_candidate.statistics_diagnostics.end(),
                        "statistics-missing:join_selectivity") ==
                  candidate.plan_candidate.statistics_diagnostics.end(),
              "semantic cross selectivity treated as missing measured data");
}

void UpperInputsAndAbsence() {
  const auto object = FixtureUuid(2205, 4);
  const auto target = Target::Object(object);
  const auto rows = Count("row_count", target, 2000, 71);
  const auto visible_zero = Count("visible_row_count", target, 0, 73);
  const auto width = Count("average_row_bytes", target, 64, 79);
  const auto groups = Count("group_count", target, 12, 83);
  const auto partitions = Count("window_partition_count", target, 5, 89);
  const auto memory = Count("memory_grant_available_bytes", Target::LocalDefault(), 1048576, 97);
  const auto limit = Count("limit_count", target, 3, 101);
  const auto catalog = Catalog({rows, width, groups, partitions, memory, limit});
  for (const auto kind : {Kind::kAggregateGeneric, Kind::kAggregateHash, Kind::kSortThenWindow,
                          Kind::kSort, Kind::kTopN}) {
    const auto result = opt::OptimizeLogicalPlanWithStatistics(Logical(kind, {object}), catalog);
    const auto& candidate = Subject(result);
    std::vector<opt::OptimizerStatistic> expected{rows};
    if (kind == Kind::kAggregateHash) expected.insert(expected.end(), {width, groups, memory});
    if (kind == Kind::kSortThenWindow) expected.push_back(partitions);
    if (kind == Kind::kSort || kind == Kind::kTopN) expected.insert(expected.end(), {width, memory});
    if (kind == Kind::kTopN) expected.push_back(limit);
    ExactInputs(candidate, expected);
    Require(candidate.plan_candidate.uses_policy_default_statistics ==
                (kind == Kind::kAggregateHash || kind == Kind::kSort || kind == Kind::kTopN),
            "unconsumed catalog names introduce fallback");
    Require(!opt::ValidateBenchmarkCleanOptimizedPlan(result).ok,
            "statistics-only route falsely promoted to benchmark clean");
  }
  const auto zero = opt::OptimizeLogicalPlanWithStatistics(Logical(Kind::kAggregateGeneric, {object}),
      Catalog({rows, visible_zero}));
  ExactInputs(Subject(zero), {visible_zero});
  Require(!Subject(zero).plan_candidate.uses_policy_default_statistics, "known zero became missing");
  auto stale_visible = visible_zero;
  stale_visible.freshness_microseconds = 60000001;
  const auto valid_rows = opt::OptimizeLogicalPlanWithStatistics(Logical(Kind::kAggregateGeneric, {object}),
      Catalog({rows, stale_visible}));
  ExactInputs(Subject(valid_rows), {rows});
  Require(!Subject(valid_rows).plan_candidate.uses_policy_default_statistics,
          "unusable visible-row probe hid usable exact row count");
  const auto absent = opt::OptimizeLogicalPlanWithStatistics(Logical(Kind::kAggregateGeneric, {object}), {});
  Require(Subject(absent).cost.row_cost > Subject(zero).cost.row_cost &&
          Subject(absent).cost.uncertainty_cost > Subject(zero).cost.uncertainty_cost,
          "missing rows became exact zero or optimistic cost");

  const auto full = Count("row_count", target, std::numeric_limits<std::uint64_t>::max() - 4096, 107);
  const auto huge = opt::OptimizeLogicalPlanWithStatistics(Logical(Kind::kAggregateGeneric, {object}), Catalog({full}));
  ExactInputs(Subject(huge), {full});
  Require(Subject(huge).cost.row_cost >= *full.exact_unsigned_value, "full-range unsigned count narrowed");

  const auto streaming = opt::OptimizeLogicalPlanWithStatistics(
      Logical(Kind::kAggregateHash, {object}, {"aggregate.input_ordered_by_group"}), catalog);
  ExactInputs(Subject(streaming), {rows, groups});
  Require(!Subject(streaming).plan_candidate.uses_policy_default_statistics,
          "streaming aggregate claimed unused hash-memory inputs");

  opt::AccessPathPlanningRequest request;
  request.relation_uuid = object;
  request.descriptor_digest = "desc.native-cost";
  request.ordered_limit.present = true;
  request.ordered_limit.limit_count = 0;
  const auto literal_limit = opt::OptimizeLogicalPlanWithAccessPathRequest(
      Logical(Kind::kTopN, {object}), request);
  const auto& limited = Subject(literal_limit);
  Require(limited.plan_candidate.estimated_rows == 0, "bound zero limit replaced by fallback");
  Require(std::none_of(limited.plan_candidate.statistic_inputs.begin(),
                      limited.plan_candidate.statistic_inputs.end(), [](const auto& input) {
                        return input.statistic_name == "limit_count";
                      }), "bound literal limit invented a catalog observation");
}

void EverySelectedNodeIsChecked() {
  // Validator unit contract: no selected upper node may hide fallback merely
  // because the primary/base candidate has clean object-bound statistics.
  opt::OptimizedPlan plan;
  plan.ok = true;
  plan.optimizer_profile = "catalog_backed_access_path_v1";
  opt::OptimizerCandidate base;
  base.selected = true;
  base.selected_in_physical_tree = true;
  base.plan_candidate.candidate_id = "base";
  base.plan_candidate.statistic_inputs = {Count("row_count", Target::Object(FixtureUuid(2205, 8)), 20)};
  auto upper = base;
  upper.selected = false;
  upper.plan_candidate.candidate_id = "upper";
  plan.candidates = {base, upper};
  Require(opt::ValidateBenchmarkCleanOptimizedPlan(plan).ok, "clean validator control");
  plan.candidates[1].plan_candidate.uses_policy_default_statistics = true;
  Require(!opt::ValidateBenchmarkCleanOptimizedPlan(plan).ok, "non-primary policy default admitted");
  plan.candidates[1].plan_candidate.uses_policy_default_statistics = false;
  plan.candidates[1].plan_candidate.uses_local_default_statistics = true;
  Require(!opt::ValidateBenchmarkCleanOptimizedPlan(plan).ok, "non-primary local default admitted");
}
}  // namespace

int main() {
  EveryIdentityByte();
  JoinInputsAndFallbacks();
  UpperInputsAndAbsence();
  EverySelectedNodeIsChecked();
  std::cout << "native cost statistics: all-byte identity, exact provenance and conservative fallback PASS\n";
}
