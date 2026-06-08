// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_contract.hpp"
#include "rule_planner.hpp"

#include <iostream>

int main() {
  namespace opt = scratchbird::engine::optimizer;
  namespace planner = scratchbird::engine::planner;
  namespace sblr = scratchbird::engine::sblr;
  planner::RulePlannerInput input;
  input.envelope = sblr::MakeSblrEnvelope("dml.select_rows", "dml.select_rows", "TRACE-OPT-CONTRACT");
  input.api_request.predicate.predicate_kind = "scalar_eq";
  const auto logical = planner::BuildDeterministicLogicalPlan(input);
  const auto optimized = opt::OptimizeLogicalPlan(logical);
  bool selected = false;
  for (const auto& candidate : optimized.candidates) {
    selected = selected || candidate.selected;
  }
  const bool ok = optimized.ok && optimized.optimizer_profile == "deterministic_first_cost_v1" &&
                  !optimized.candidates.empty() && selected;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"candidates\":" << optimized.candidates.size()
            << ",\"selected\":" << (selected ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}
