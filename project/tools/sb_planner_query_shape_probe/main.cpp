// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
namespace planner = scratchbird::engine::planner;
static std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}
static void Expect(bool condition, std::string message, std::vector<std::string>* errors) {
  if (!condition) errors->push_back(std::move(message));
}
static int Finish(const std::vector<std::string>& errors) {
  std::cout << "{\n  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
int main() {
  std::vector<std::string> errors;
  planner::QueryShapeEvidence point{.shape = planner::QueryShapeKind::kPointLookup, .has_usable_index = true};
  planner::QueryShapeEvidence range{.shape = planner::QueryShapeKind::kRangeQuery, .has_usable_index = true};
  planner::QueryShapeEvidence fallback{.shape = planner::QueryShapeKind::kRangeQuery, .has_usable_index = false};
  planner::QueryShapeEvidence window{.shape = planner::QueryShapeKind::kWindowQuery, .has_ordered_access = false};
  planner::QueryShapeEvidence cte{.shape = planner::QueryShapeKind::kCteSubquery, .cte_reused = true};
  planner::QueryShapeEvidence setop{.shape = planner::QueryShapeKind::kSetOperation};
  Expect(planner::BuildQueryShapePlan(point).nodes.front().access_kind == planner::PhysicalAccessKind::kScalarBtreeLookup, "point lookup should use scalar lookup", &errors);
  Expect(planner::BuildQueryShapePlan(range).nodes.front().access_kind == planner::PhysicalAccessKind::kScalarBtreeRange, "range query should use scalar range", &errors);
  const auto fallback_plan = planner::BuildQueryShapePlan(fallback);
  Expect(fallback_plan.nodes.front().access_kind == planner::PhysicalAccessKind::kTableScan, "range fallback should table scan", &errors);
  Expect(!fallback_plan.diagnostics.empty(), "range fallback should report deterministic fallback", &errors);
  Expect(planner::BuildQueryShapePlan(window).nodes.front().access_kind == planner::PhysicalAccessKind::kSortThenWindow, "window should sort when ordered access is missing", &errors);
  Expect(planner::BuildQueryShapePlan(cte).nodes.front().access_kind == planner::PhysicalAccessKind::kCteMaterialize, "reused CTE should materialize", &errors);
  Expect(planner::BuildQueryShapePlan(setop).nodes.front().access_kind == planner::PhysicalAccessKind::kSetOperation, "set operation shape mismatch", &errors);
  return Finish(errors);
}
