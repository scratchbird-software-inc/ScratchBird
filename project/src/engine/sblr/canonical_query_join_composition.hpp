// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_execute.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_relational_expression.hpp"

#include "engine/executor/executor_foundation.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

struct PreparedJoinRoot {
  bool ok{false};
  std::uint32_t predicate_expression_id{0};
  CanonicalRelationalExpressionRowBinding predicate_row_binding;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

// Exposes only the already-bound object-free JOIN preparation shared with
// larger compositions. It cannot select a plan, access storage, or create MGA
// transaction authority.
PreparedJoinRoot PrepareJoinRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& left_node,
    const plan::CanonicalLogicalRelationalNode& right_node,
    MaterializedValues& left,
    MaterializedValues& right,
    exec::CanonicalAcceptedJoinKind join_kind);

CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalObjectFreeJoinQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request);

}  // namespace scratchbird::engine::sblr
