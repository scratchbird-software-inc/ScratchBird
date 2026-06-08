// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_barrier.hpp"
#include "optimizer_safety_gates.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace {
void Expect(bool condition, const char* message, std::vector<std::string>* errors) { if (!condition) errors->push_back(message); }
int Finish(const std::vector<std::string>& errors) { std::cout << "{\"ok\":" << (errors.empty() ? "true" : "false") << ",\"failure_count\":" << errors.size() << "}\n"; return errors.empty() ? 0 : 1; }
}
int main() {
  std::vector<std::string> errors;
  opt::OptimizerBarrierInput barrier;
  barrier.security_context_present = true;
  barrier.grants_proven = true;
  auto decision = opt::EvaluateOptimizerBarriers(barrier);
  Expect(decision.may_fold && decision.may_pushdown && decision.may_reorder, "safe barrier allows rewrites", &errors);
  barrier.masking_policy_present = true;
  Expect(!opt::EvaluateOptimizerBarriers(barrier).may_pushdown, "masking blocks pushdown", &errors);
  opt::OptimizerSafetyGateInput gates;
  gates.grants_proven = true;
  gates.metadata_visible = true;
  gates.transaction_context_present = true;
  gates.isolation_preserved = true;
  gates.domain_rules_preserved = true;
  gates.parser_boundary_clean = true;
  Expect(opt::EvaluateAllOptimizerSafetyGates(gates).ok, "all gates pass", &errors);
  gates.parser_boundary_clean = false;
  Expect(!opt::EvaluateAllOptimizerSafetyGates(gates).ok, "parser boundary blocks", &errors);
  return Finish(errors);
}
