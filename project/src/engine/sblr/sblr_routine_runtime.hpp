// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_assignment_runtime.hpp"
#include "sblr_runtime.hpp"

#include <string>
#include <string_view>

namespace scratchbird::engine::sblr {

enum class SblrRoutineControlKind {
  normal,
  return_from_routine,
  leave_block,
  exit_loop,
  continue_loop,
};

struct SblrRoutineControlSignal {
  SblrRoutineControlKind kind = SblrRoutineControlKind::normal;
  std::string target_label;
  SblrValue value;
  bool has_value = false;
};

SblrResult EnterSblrRoutineFrame(SblrFrameStack* stack, SblrFrame frame);
SblrResult LeaveSblrRoutineFrame(SblrFrameStack* stack);
SblrRoutineControlSignal MakeSblrRoutineReturnSignal(SblrValue value);
SblrRoutineControlSignal MakeSblrRoutineLeaveSignal(std::string target_label);
SblrRoutineControlSignal MakeSblrRoutineExitLoopSignal(std::string target_label);
SblrRoutineControlSignal MakeSblrRoutineContinueLoopSignal(std::string target_label);
SblrResult ApplySblrRoutineReturnSignal(std::string_view operation_id,
                                        SblrAssignmentFrame* assignment_frame,
                                        std::string_view result_slot_id,
                                        const SblrRoutineControlSignal& signal,
                                        const SblrExecutionContext& context,
                                        const SblrAssignmentDomainValidator& domain_validator = {});
SblrResult ValidateSblrRoutineControlSignal(std::string_view operation_id,
                                            const SblrRoutineControlSignal& signal,
                                            const SblrExecutionContext& context);
const char* ToString(SblrRoutineControlKind kind);

}  // namespace scratchbird::engine::sblr
