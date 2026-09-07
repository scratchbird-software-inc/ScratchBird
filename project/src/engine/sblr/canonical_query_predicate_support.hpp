// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_relational_expression.hpp"

#include "engine/executor/executor_foundation.hpp"
#include "engine/internal_api/api_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace scratchbird::engine::sblr {

// Conservative payload and structural scratch bounds for one admitted
// row-predicate expression. This contract analyzes immutable relational
// descriptors and does not evaluate values or create transaction authority.
struct CanonicalPredicateScratchBound {
  bool ok = false;
  bool cancelled = false;
  std::uint64_t maximum_payload_bytes = 0;
  std::uint64_t maximum_structural_bytes = 0;
  std::string detail;
};

CanonicalPredicateScratchBound BoundCanonicalPredicateScratchBytes(
    const scratchbird::engine::internal_api::TypedRelationalDag& dag,
    std::uint32_t root_expression_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    std::uint64_t pair_payload_bytes,
    const std::function<bool()>& abort_requested);

// Evaluates one bound FILTER/HAVING predicate, issues its private executor
// receipt, and executes against borrowed physical/MGA carriers. This does not
// create, refresh, or finalize transaction authority.
scratchbird::engine::executor::CanonicalDescriptorFilterResult
IssueAndExecuteCanonicalFilterPredicateBorrowed(
    const scratchbird::engine::internal_api::TypedRelationalDag& relational_dag,
    std::uint32_t predicate_expression_id,
    const CanonicalRelationalExpressionRowBinding& predicate_row_binding,
    const scratchbird::engine::executor::DescriptorBatch& input_batch,
    const CanonicalRelationalExpressionRuntimeServices& expression_services,
    scratchbird::engine::internal_api::EngineCanonicalExpressionConsumer
        expression_consumer,
    scratchbird::engine::internal_api::EnginePredicateConsumer
        predicate_consumer,
    const scratchbird::engine::executor::TypedPhysicalNodeDag& physical_dag,
    std::uint64_t selected_physical_node_id,
    std::uint64_t scoped_root_physical_node_id,
    std::size_t maximum_input_row_count,
    const scratchbird::engine::executor::CanonicalExecutionMgaAuthority&
        mga_authority,
    bool borrow_mga_authority = false);

}  // namespace scratchbird::engine::sblr
