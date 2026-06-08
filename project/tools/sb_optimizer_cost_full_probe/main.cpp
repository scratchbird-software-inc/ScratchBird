// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_cost_full.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;
namespace {
void Expect(bool condition, const char* message, std::vector<std::string>* errors) { if (!condition) errors->push_back(message); }
int Finish(const std::vector<std::string>& errors) { std::cout << "{\"ok\":" << (errors.empty() ? "true" : "false") << ",\"failure_count\":" << errors.size() << "}\n"; return errors.empty() ? 0 : 1; }
}
int main() {
  std::vector<std::string> errors;
  opt::OptimizerCostEnvironment env;
  env.memory_budget_bytes = 1024;
  auto cost = opt::EstimateBaseOperatorCost(env, planner::PhysicalAccessKind::kTableScan, 1000, 64, 16);
  Expect(cost.total_cost > 0, "base cost positive", &errors);
  cost = opt::ApplyMemoryAndSpillCost(cost, env, 8192, true);
  Expect(cost.uncertainty_cost > 0, "spill uncertainty added", &errors);
  opt::OptimizerMetricCostInput metric{"operator_latency_multiplier", 2.0, 10, true};
  auto metric_cost = opt::ApplyMetricFeedbackCost(cost, {metric});
  Expect(metric_cost.total_cost >= cost.total_cost, "metric cost applied", &errors);
  opt::AgentCostRecommendation rec{"agent:index", "avoid", 2.0, true};
  auto agent_cost = opt::ApplyAgentRecommendationCost(cost, {rec});
  Expect(agent_cost.uncertainty_cost > cost.uncertainty_cost, "agent recommendation applied", &errors);
  return Finish(errors);
}
