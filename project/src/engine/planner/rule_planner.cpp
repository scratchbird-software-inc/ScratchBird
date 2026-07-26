// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "rule_planner.hpp"

#include "logical_plan.hpp"

#include <concepts>

namespace scratchbird::engine::planner {
namespace {

template <typename T>
concept HasPhysicalAccessSelection = requires(T node) {
  node.access_kind;
};

static_assert(kOperationPrefixRulePlannerRemoved);
static_assert(!HasPhysicalAccessSelection<CanonicalLogicalRelationalNode>);

}  // namespace

// QOW-SOURCE-QRY-030-V1
// QOW-SOURCE-IAS-015-V1
// SEARCH_KEY: SB_RULE_PLANNER_OPTIMIZER_INTEGRATION_REMOVED
// No operation-prefix or predicate-string planner remains.  Relational plans
// enter through the canonical logical graph contract and physical alternatives
// remain separate until capability validation and selection.

}  // namespace scratchbird::engine::planner
