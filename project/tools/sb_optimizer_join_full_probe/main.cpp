// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "join_planner_full.hpp"

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
  auto graph = opt::BuildJoinGraph({{"b", 1000, false}, {"a", 10, false}}, {{"a", "b", "scalar_eq", true, false, false, 0.01}}, false, false);
  auto plan = opt::EnumerateDeterministicJoinOrder(graph, 1024 * 1024);
  Expect(plan.ok, "join plan ok", &errors);
  Expect(plan.ordered_relation_uuids.front() == "a", "smallest relation first", &errors);
  Expect(plan.method == planner::PhysicalAccessKind::kJoinHash || plan.method == planner::PhysicalAccessKind::kJoinNestedLoop || plan.method == planner::PhysicalAccessKind::kJoinMerge, "join method chosen", &errors);
  auto blocked = opt::BuildJoinGraph({{"a", 10, false}, {"b", 1000, false}}, {{"a", "b", "scalar_eq", true, true, true, 0.01}}, true, false);
  Expect(!opt::JoinReorderAllowed(blocked), "outer/nullable join blocks reorder", &errors);
  return Finish(errors);
}
