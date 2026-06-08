// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cost_model.hpp"

#include <iostream>

int main() {
  namespace opt = scratchbird::engine::optimizer;
  namespace planner = scratchbird::engine::planner;
  const auto lookup_node = planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead,
                                                        planner::PhysicalAccessKind::kRowUuidLookup,
                                                        "dml.select_rows",
                                                        "row_lookup");
  const auto scan_node = planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead,
                                                      planner::PhysicalAccessKind::kTableScan,
                                                      "dml.select_rows",
                                                      "table_scan");
  const auto lookup_cost = opt::EstimateNodeCost(lookup_node);
  const auto scan_cost = opt::EstimateNodeCost(scan_node);
  const bool ok = lookup_cost.total_cost > 0 && scan_cost.total_cost > lookup_cost.total_cost &&
                  opt::IsBetterCost(lookup_cost, scan_cost);
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"lookup_cost\":" << lookup_cost.total_cost
            << ",\"scan_cost\":" << scan_cost.total_cost << "}\n";
  return ok ? 0 : 1;
}
