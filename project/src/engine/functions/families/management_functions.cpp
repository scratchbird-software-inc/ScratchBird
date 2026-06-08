// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/management_functions.hpp"
#include "families/inactive_family_dispatch.hpp"
namespace scratchbird::engine::functions {
bool IsManagementFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("management.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.management.", 0) == 0;
}
FunctionCallResult DispatchManagementFunction(const FunctionCallRequest& request) {
  return RefuseInactiveFunctionFamily(request, "SB_DIAG_MANAGEMENT_FUNCTION_SURFACE_INACTIVE", "management");
}
}
