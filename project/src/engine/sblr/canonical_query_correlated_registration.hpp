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
#include <string>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

struct LiveCorrelatedSubqueryRegistrationProfile {
  std::size_t outer_binding_column{0};
  std::size_t inner_reference_column{0};
  std::uint32_t outer_binding_descriptor_id{0};
  std::uint32_t inner_reference_descriptor_id{0};
  std::size_t outer_row_count{0};
  std::size_t inner_row_count{0};
  std::size_t pair_count{0};
  std::size_t output_row_bound{0};
  bool comparison_authority_required{false};
  std::uint64_t comparison_authority_memory_bytes{0};
  std::string implementation_id;
  std::string transformation_id;
};

struct LiveLateralSubqueryProfile {
  bool matched{false};
  exec::CanonicalLateralJoinForm form =
      exec::CanonicalLateralJoinForm::kInnerLateral;
  std::string required_operand_type;
  std::string implementation_id;
  std::string transformation_id;
};

// Registers bounded correlated and LATERAL/APPLY execution over two
// already-materialized typed inputs. These callbacks consume engine-selected
// MGA authority and cannot construct snapshots or finalize transactions.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveCorrelatedSubqueryRegistration(
    LiveCorrelatedSubqueryRegistrationProfile prepared,
    std::string capability_uuid,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

exec::CanonicalPhysicalExecutorRegistration MakeLiveLateralSubqueryRegistration(
    LiveCorrelatedSubqueryRegistrationProfile prepared,
    LiveLateralSubqueryProfile profile,
    std::string capability_uuid,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    bool runtime_bounded_inputs = false,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr);

}  // namespace scratchbird::engine::sblr
