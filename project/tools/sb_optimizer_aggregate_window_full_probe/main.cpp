// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "relational_planner.hpp"

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
  auto agg = opt::PlanAggregate({1000, 100, 64, 4096, true, false, false});
  Expect(agg.ok && agg.access_kind == planner::PhysicalAccessKind::kAggregateHash, "hash aggregate planned", &errors);
  auto window = opt::PlanWindow({1000, 10, false, true});
  Expect(window.ok && window.access_kind == planner::PhysicalAccessKind::kSortThenWindow, "window sort planned", &errors);
  auto sort = opt::PlanSortLimit({1000, 64, 4096, false, true, 10});
  Expect(sort.ok && sort.access_kind == planner::PhysicalAccessKind::kTopN, "top-n planned", &errors);
  auto setop = opt::PlanSetOperation(10, 20, true);
  Expect(setop.ok && setop.access_kind == planner::PhysicalAccessKind::kSetOperation, "setop planned", &errors);
  Expect(!opt::PlanLocalMutation("update_rows", false, true).ok, "mutation requires transaction context", &errors);
  return Finish(errors);
}
