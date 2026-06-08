// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/metrics_functions.hpp"
#include "families/inactive_family_dispatch.hpp"
namespace scratchbird::engine::functions {
bool IsMetricsFunction(const FunctionCallRequest& request) { return request.context.function_id.rfind("metrics.", 0) == 0; }
FunctionCallResult DispatchMetricsFunction(const FunctionCallRequest& request) {
  return RefuseInactiveFunctionFamily(request, "SB_DIAG_METRICS_FUNCTION_SURFACE_INACTIVE", "metrics");
}
}
