// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "rule_planner.hpp"

#include <iostream>

int main() {
  namespace planner = scratchbird::engine::planner;
  namespace sblr = scratchbird::engine::sblr;
  planner::RulePlannerInput input;
  input.envelope = sblr::MakeSblrEnvelope("dml.select_rows", "dml.select_rows", "TRACE-LOGICAL-PLAN");
  input.api_request.predicate.predicate_kind = "row_uuid_eq";
  const auto row_uuid = planner::BuildDeterministicLogicalPlan(input);
  input.api_request.predicate.predicate_kind = "scalar_range";
  const auto range = planner::BuildDeterministicLogicalPlan(input);
  const bool ok = row_uuid.ok && range.ok && !row_uuid.nodes.empty() && !range.nodes.empty() &&
                  row_uuid.nodes.front().access_kind == planner::PhysicalAccessKind::kRowUuidLookup &&
                  range.nodes.front().access_kind == planner::PhysicalAccessKind::kScalarBtreeRange;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"row_uuid_access\":\"" << planner::PhysicalAccessKindName(row_uuid.nodes.front().access_kind)
            << "\",\"range_access\":\"" << planner::PhysicalAccessKindName(range.nodes.front().access_kind) << "\"}\n";
  return ok ? 0 : 1;
}
