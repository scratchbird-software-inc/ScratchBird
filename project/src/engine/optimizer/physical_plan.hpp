// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "access_path.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_PHYSICAL_PLAN_NODE_TAXONOMY
struct PhysicalPlanNode {
  std::string node_id;
  scratchbird::engine::planner::PhysicalAccessKind access_kind = scratchbird::engine::planner::PhysicalAccessKind::kNone;
  std::string executor_capability_id;
  std::string descriptor_digest;
  CostVector cost;
  std::uint64_t estimated_rows = 0;
  std::vector<PhysicalPlanNode> children;
  std::vector<std::string> runtime_evidence;
  std::vector<std::string> diagnostics;
  bool storage_backed = false;
  bool materializes = false;
  bool preserves_order = false;
  bool preserves_visibility = true;
  // SEARCH_KEY: OPCH_PHYSICAL_PLAN_VALIDATION_EXPANSION
  bool memory_evidence_required = false;
  bool memory_evidence_present = false;
  bool memory_evidence_trusted = false;
  bool agent_evidence_required = false;
  bool agent_evidence_present = false;
  bool agent_evidence_trusted = false;
  bool parser_or_donor_evidence_authority = false;
};

struct PhysicalPlanValidation {
  bool ok = false;
  std::vector<std::string> diagnostics;
};

PhysicalPlanNode PhysicalPlanNodeFromCandidate(const PlanCandidate& candidate,
                                               std::string executor_capability_id,
                                               std::string descriptor_digest);
PhysicalPlanValidation ValidatePhysicalPlanNode(const PhysicalPlanNode& node);
const char* RequiredExecutorCapabilityForAccessKind(scratchbird::engine::planner::PhysicalAccessKind access_kind);
std::string SerializePhysicalPlanNodeToJson(const PhysicalPlanNode& node, std::size_t indent = 0);

}  // namespace scratchbird::engine::optimizer
