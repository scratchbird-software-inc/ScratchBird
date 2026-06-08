// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/extension_functions.hpp"

#include "common/function_result_helpers.hpp"

namespace scratchbird::engine::functions {

bool IsExtensionFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("extension.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.extension.", 0) == 0;
}

FunctionCallResult DispatchExtensionFunction(const FunctionCallRequest& request) {
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::policy_refused,
                                      "SB_DIAG_EXTENSION_SURFACE_NOT_ACTIVE",
                                      "extension lifecycle and UDR invocation require explicit package/policy activation");
}

}  // namespace scratchbird::engine::functions
