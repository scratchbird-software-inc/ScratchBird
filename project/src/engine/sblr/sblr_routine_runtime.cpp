// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_routine_runtime.hpp"

#include <utility>

namespace scratchbird::engine::sblr {

SblrResult EnterSblrRoutineFrame(SblrFrameStack* stack, SblrFrame frame) {
  SblrResult failure;
  if (!PushSblrFrame(stack, std::move(frame), &failure)) return failure;
  return MakeSblrSuccess("sblr.routine.enter");
}

SblrResult LeaveSblrRoutineFrame(SblrFrameStack* stack) {
  SblrResult failure;
  if (!PopSblrFrame(stack, &failure)) return failure;
  return MakeSblrSuccess("sblr.routine.leave");
}

const char* ToString(SblrRoutineControlKind kind) {
  switch (kind) {
    case SblrRoutineControlKind::normal:
      return "normal";
    case SblrRoutineControlKind::return_from_routine:
      return "return_from_routine";
    case SblrRoutineControlKind::leave_block:
      return "leave_block";
    case SblrRoutineControlKind::exit_loop:
      return "exit_loop";
    case SblrRoutineControlKind::continue_loop:
      return "continue_loop";
  }
  return "unknown";
}

SblrRoutineControlSignal MakeSblrRoutineReturnSignal(SblrValue value) {
  SblrRoutineControlSignal signal;
  signal.kind = SblrRoutineControlKind::return_from_routine;
  signal.value = std::move(value);
  signal.has_value = true;
  return signal;
}

SblrRoutineControlSignal MakeSblrRoutineLeaveSignal(std::string target_label) {
  SblrRoutineControlSignal signal;
  signal.kind = SblrRoutineControlKind::leave_block;
  signal.target_label = std::move(target_label);
  return signal;
}

SblrRoutineControlSignal MakeSblrRoutineExitLoopSignal(std::string target_label) {
  SblrRoutineControlSignal signal;
  signal.kind = SblrRoutineControlKind::exit_loop;
  signal.target_label = std::move(target_label);
  return signal;
}

SblrRoutineControlSignal MakeSblrRoutineContinueLoopSignal(std::string target_label) {
  SblrRoutineControlSignal signal;
  signal.kind = SblrRoutineControlKind::continue_loop;
  signal.target_label = std::move(target_label);
  return signal;
}

SblrResult ValidateSblrRoutineControlSignal(std::string_view operation_id,
                                            const SblrRoutineControlSignal& signal,
                                            const SblrExecutionContext& context) {
  if (signal.kind == SblrRoutineControlKind::normal) {
    return MakeSblrSuccess(std::string(operation_id));
  }
  if ((signal.kind == SblrRoutineControlKind::leave_block ||
       signal.kind == SblrRoutineControlKind::exit_loop ||
       signal.kind == SblrRoutineControlKind::continue_loop) &&
      signal.target_label.empty()) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           std::string(operation_id),
                           MakeSblrRefusalDiagnostic("SB_DIAG_SBLR_CONTROL_TARGET_REQUIRED",
                                                     context,
                                                     "leave/exit/continue control signal requires a target label"));
  }
  if (signal.kind == SblrRoutineControlKind::return_from_routine && !signal.has_value) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           std::string(operation_id),
                           MakeSblrRefusalDiagnostic("SB_DIAG_SBLR_RETURN_VALUE_REQUIRED",
                                                     context,
                                                     "routine return signal requires an explicit return value"));
  }
  return MakeSblrSuccess(std::string(operation_id));
}

SblrResult ApplySblrRoutineReturnSignal(std::string_view operation_id,
                                        SblrAssignmentFrame* assignment_frame,
                                        std::string_view result_slot_id,
                                        const SblrRoutineControlSignal& signal,
                                        const SblrExecutionContext& context,
                                        const SblrAssignmentDomainValidator& domain_validator) {
  const auto validation = ValidateSblrRoutineControlSignal(operation_id, signal, context);
  if (!validation.ok()) return validation;
  if (signal.kind != SblrRoutineControlKind::return_from_routine) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           std::string(operation_id),
                           MakeSblrRefusalDiagnostic("SB_DIAG_SBLR_RETURN_SIGNAL_REQUIRED",
                                                     context,
                                                     "routine result assignment requires a return control signal"));
  }
  if (result_slot_id.empty()) {
    SblrResult out = MakeSblrSuccess(std::string(operation_id));
    out.scalar_values.push_back(signal.value);
    return out;
  }
  return AssignSblrSlot(operation_id, assignment_frame, result_slot_id, signal.value, context, domain_validator);
}

}  // namespace scratchbird::engine::sblr
