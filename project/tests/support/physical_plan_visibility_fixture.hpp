// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../src/engine/optimizer/physical_plan.hpp"
#include <algorithm>

namespace scratchbird::tests {
// A planning test proves preservation requirements, never that a transaction
// inventory was read. Mutate every node separately and require recursive plan
// validation to reject the missing requirement with its exact diagnostic.
inline bool PhysicalPlanRequiresVisibilityAtEveryNode(
    const engine::optimizer::PhysicalPlanNode& source) {
  namespace opt = engine::optimizer;
  if (!opt::ValidatePhysicalPlanNode(source).ok) return false;
  auto root = source;
  const auto check = [&](auto&& self, opt::PhysicalPlanNode& node) -> bool {
    if (!node.preserves_visibility ||
        std::find(node.runtime_evidence.begin(), node.runtime_evidence.end(),
                  "mga_visibility_authority=engine_transaction_inventory") !=
            node.runtime_evidence.end()) return false;
    node.preserves_visibility = false;
    const auto rejected = opt::ValidatePhysicalPlanNode(root);
    node.preserves_visibility = true;
    if (rejected.ok ||
        std::find(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                  "SB_OPT_VISIBILITY_PRESERVATION_REQUIRED") ==
            rejected.diagnostics.end()) return false;
    for (auto& child : node.children)
      if (!self(self, child)) return false;
    return true;
  };
  return check(check, root);
}
}  // namespace scratchbird::tests
