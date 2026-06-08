// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "logical_plan.hpp"
#include "sblr_engine_envelope.hpp"

namespace scratchbird::engine::planner {

// SEARCH_KEY: SB_SBLR_TO_OPTIMIZER_BINDING
struct RulePlannerInput {
  scratchbird::engine::sblr::SblrOperationEnvelope envelope;
  scratchbird::engine::internal_api::EngineApiRequest api_request;
};

LogicalPlan BuildDeterministicLogicalPlan(const RulePlannerInput& input);

}  // namespace scratchbird::engine::planner
