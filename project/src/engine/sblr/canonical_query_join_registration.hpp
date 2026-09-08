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
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

struct LiveJoinRuntimeNodeConfiguration {
  std::uint32_t relational_node_id{0};
  exec::CanonicalAcceptedJoinKind join_kind{
      exec::CanonicalAcceptedJoinKind::kCross};
  std::string operation_name;
  std::uint32_t predicate_expression_id{0};
  CanonicalRelationalExpressionRowBinding predicate_binding;
};

// Registers bounded JOIN execution over two optimizer-published typed inputs.
// Runtime callbacks only revalidate the engine-selected MGA authority.
exec::CanonicalPhysicalExecutorRegistration MakeLiveJoinRegistration(
    std::string implementation_id,
    std::string capability_uuid,
    std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>
        predicate_truth_values,
    std::size_t pair_count,
    std::size_t output_row_bound,
    exec::CanonicalAcceptedJoinKind join_kind,
    std::string operation_name,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    bool runtime_bounded_inputs = false,
    std::uint32_t runtime_predicate_expression_id = 0,
    CanonicalRelationalExpressionRowBinding runtime_predicate_binding = {},
    scratchbird::engine::internal_api::TypedRelationalDag
        runtime_relational_dag = {},
    CanonicalRelationalExpressionRuntimeServices runtime_expression_services = {},
    std::vector<LiveJoinRuntimeNodeConfiguration>
        runtime_node_configurations = {},
    std::optional<exec::CanonicalExecutionMgaAuthority>
        runtime_mga_authority = std::nullopt,
    const scratchbird::engine::internal_api::TypedRelationalDag*
        borrowed_runtime_relational_dag = nullptr,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority*
        borrowed_runtime_mga_authority = nullptr);

}  // namespace scratchbird::engine::sblr
