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
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrErrorHandlerFrame {
  std::string handler_uuid;
  std::string match_code;
  bool when_any = false;
};

SblrResult EnterSblrErrorHandler(SblrFrameStack* stack, SblrErrorHandlerFrame handler);
SblrResult LeaveSblrErrorHandler(SblrFrameStack* stack);
SblrResult RaiseSblrError(std::string_view operation_id,
                          const SblrExecutionContext& context,
                          std::string diagnostic_id,
                          std::string detail,
                          std::string sqlstate = {});
bool SblrErrorHandlerMatches(const SblrErrorHandlerFrame& handler, const SblrResult& error);
SblrResult SelectSblrErrorHandler(std::string_view operation_id,
                                  const std::vector<SblrErrorHandlerFrame>& handlers,
                                  const SblrResult& error,
                                  const SblrExecutionContext& context);
SblrResult UnwindSblrFramesForErrorHandler(std::string_view operation_id,
                                           SblrFrameStack* stack,
                                           std::string_view handler_uuid,
                                           const SblrExecutionContext& context);
SblrResult RefuseUnhandledSblrError(const SblrExecutionContext& context, std::string detail);

}  // namespace scratchbird::engine::sblr
