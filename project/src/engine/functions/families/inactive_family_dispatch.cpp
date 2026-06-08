// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/inactive_family_dispatch.hpp"

#include "common/function_result_helpers.hpp"

#include <utility>

namespace scratchbird::engine::functions {

FunctionCallResult RefuseInactiveFunctionFamily(const FunctionCallRequest& request,
                                                std::string diagnostic_id,
                                                std::string family_name) {
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      std::move(diagnostic_id),
                                      family_name + " function family is specified but not active in this build slice");
}

}  // namespace scratchbird::engine::functions
