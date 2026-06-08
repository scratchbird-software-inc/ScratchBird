// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <string>
#include <string_view>

namespace scratchbird::engine::sblr {

SblrStatusCode StatusForRefusalDiagnostic(std::string_view diagnostic_id);
SblrResult RefuseSblrOperation(const SblrExecutionContext& context,
                               std::string operation_id,
                               std::string diagnostic_id,
                               std::string detail = {});
SblrResult RefuseClusterTransactionHook(const SblrExecutionContext& context,
                                        std::string hook_name,
                                        std::string feature_gate);
SblrResult RefuseParserAuthority(const SblrExecutionContext& context,
                                 std::string operation_id,
                                 std::string detail = {});

}  // namespace scratchbird::engine::sblr
