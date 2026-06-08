// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_resource_governance.hpp"

#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

SblrResult RefuseBudget(const SblrExecutionContext& context, std::string operation_id, std::string field) {
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_SBLR_RESOURCE_BUDGET_EXCEEDED", context,
                                              "SBLR resource budget exceeded");
  diagnostic.fields.push_back({"operation_id", operation_id});
  diagnostic.fields.push_back({"budget_field", std::move(field)});
  return MakeSblrFailure(SblrStatusCode::resource_exhausted, std::move(operation_id), std::move(diagnostic));
}

}  // namespace

SblrResult CheckSblrResourceBudget(const SblrResourceBudget& budget,
                                   const SblrResourceUsage& usage,
                                   const SblrExecutionContext& context,
                                   std::string operation_id) {
  if (usage.frame_depth > budget.max_frame_depth) return RefuseBudget(context, operation_id, "max_frame_depth");
  if (budget.max_rows != 0 && usage.rows > budget.max_rows) return RefuseBudget(context, operation_id, "max_rows");
  if (budget.max_steps != 0 && usage.steps > budget.max_steps) return RefuseBudget(context, operation_id, "max_steps");
  if (budget.max_memory_bytes != 0 && usage.memory_bytes > budget.max_memory_bytes) {
    return RefuseBudget(context, operation_id, "max_memory_bytes");
  }
  return MakeSblrSuccess(std::move(operation_id));
}

}  // namespace scratchbird::engine::sblr
