// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_candidate.hpp"
#include "../../tests/support/binary_uuid_fixture.hpp"
#include "optimizer_explain.hpp"
#include "optimizer_request.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;

namespace {
void Expect(bool condition, const char* message, std::vector<std::string>* errors) {
  if (!condition) errors->push_back(message);
}
int Finish(const std::vector<std::string>& errors) {
  for (const auto& error : errors) std::cerr << error << '\n';
  std::cout << "{\"ok\":" << (errors.empty() ? "true" : "false") << ",\"failure_count\":" << errors.size() << "}\n";
  return errors.empty() ? 0 : 1;
}
opt::BoundOptimizerRequest Request(bool valid) {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = scratchbird::tests::FixtureUuid(2209, 14);
  request.context.operation_id = "dml.select_rows";
  request.context.sblr_digest = "sblr:select:v1";
  request.context.descriptor_set_digest = "desc:v1";
  request.context.statistics_snapshot_id = "stats:v1";
  request.context.executor_capability_set_id = "exec:v1";
  request.context.catalog_epoch = 1;
  request.context.security_epoch = 1;
  request.context.policy_epoch = 1;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.context.name_authority_present = !valid;
  request.logical_plan.ok = true;
  request.logical_plan.plan_id = "plan:contract";
  request.logical_plan.nodes.push_back(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kTableScan, "dml.select_rows", "scan"));
  request.statistics = opt::DefaultLocalStatisticsCatalog();
  return request;
}
}

int main() {
  std::vector<std::string> errors;
  auto valid = Request(true);
  auto invalid = Request(false);
  Expect(opt::ValidateBoundOptimizerRequest(valid).ok, "valid bound request should validate", &errors);
  Expect(!opt::ValidateBoundOptimizerRequest(invalid).ok, "name authority should be rejected", &errors);
  const auto result = opt::OptimizeBoundRequest(valid);
  Expect(result.ok, "valid request should optimize", &errors);
  for (const auto& candidate : result.candidates) {
    Expect(!candidate.cluster_candidate,
           "cluster-disabled enumeration should not invent cluster candidates", &errors);
  }
  // Exercise cluster candidate enumeration explicitly. The default facts do
  // not supply a provider, so those alternatives must still be refused while
  // the valid local plan remains available.
  auto cluster_routes = valid;
  cluster_routes.context.cluster_build_enabled = true;
  const auto cluster_result = opt::OptimizeBoundRequest(cluster_routes);
  Expect(cluster_result.ok, "cluster alternatives must not discard the valid local plan", &errors);
  bool cluster_refused = false;
  for (const auto& candidate : cluster_result.candidates) {
    if (candidate.cluster_candidate && !candidate.cost.selectable) cluster_refused = true;
    Expect(!candidate.cluster_candidate || (!candidate.cost.selectable && !candidate.selected),
           "cluster candidate without provider facts must not be selectable", &errors);
  }
  Expect(cluster_refused, "cluster enumeration without provider facts must retain refused alternatives", &errors);
  const auto explain = opt::BuildOptimizerExplainDocument(valid, result);
  Expect(opt::RenderOptimizerExplainJson(explain).find("optimizer_explain_v1") != std::string::npos, "explain document should render", &errors);
  return Finish(errors);
}
