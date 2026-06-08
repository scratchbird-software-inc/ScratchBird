// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_block_runtime.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

std::string DiagnosticFieldValue(const SblrRuntimeDiagnostic& diagnostic, std::string_view key) {
  const auto it = std::find_if(diagnostic.fields.begin(), diagnostic.fields.end(), [key](const SblrDiagnosticField& field) {
    return field.key == key;
  });
  return it == diagnostic.fields.end() ? std::string() : it->value;
}

bool PatternMatches(std::string_view pattern, std::string_view value) {
  if (pattern.empty() || value.empty()) return false;
  if (pattern == value) return true;
  if (pattern.back() == '*') {
    const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
    return value.substr(0, prefix.size()) == prefix;
  }
  return false;
}

SblrValue HandlerUuidValue(std::string handler_uuid) {
  SblrValue value;
  value.descriptor_id = "uuid";
  value.payload_kind = SblrValuePayloadKind::uuid_text;
  value.is_null = false;
  value.text_value = std::move(handler_uuid);
  value.encoded_value = value.text_value;
  return value;
}

SblrResult HandlerSelectionResult(std::string_view operation_id, std::string handler_uuid) {
  SblrResult out = MakeSblrSuccess(std::string(operation_id));
  out.scalar_values.push_back(HandlerUuidValue(std::move(handler_uuid)));
  return out;
}

}  // namespace

SblrResult EnterSblrErrorHandler(SblrFrameStack* stack, SblrErrorHandlerFrame handler) {
  SblrResult failure;
  SblrFrame frame;
  frame.frame_uuid = handler.handler_uuid;
  frame.depth = stack ? stack->frames.size() : 0;
  frame.rollback_region_open = true;
  if (!PushSblrFrame(stack, std::move(frame), &failure)) return failure;
  return MakeSblrSuccess("sblr.error_handler.enter");
}

SblrResult LeaveSblrErrorHandler(SblrFrameStack* stack) {
  SblrResult failure;
  if (stack != nullptr && !stack->frames.empty()) {
    stack->frames.back().rollback_region_open = false;
  }
  if (!PopSblrFrame(stack, &failure)) return failure;
  return MakeSblrSuccess("sblr.error_handler.leave");
}

SblrResult RaiseSblrError(std::string_view operation_id,
                          const SblrExecutionContext& context,
                          std::string diagnostic_id,
                          std::string detail,
                          std::string sqlstate) {
  auto diagnostic = MakeSblrRefusalDiagnostic(std::move(diagnostic_id), context, std::move(detail));
  diagnostic.fields.push_back({"operation_id", std::string(operation_id)});
  if (!sqlstate.empty()) diagnostic.fields.push_back({"sqlstate", std::move(sqlstate)});
  return MakeSblrFailure(SblrStatusCode::execution_failed, std::string(operation_id), std::move(diagnostic));
}

bool SblrErrorHandlerMatches(const SblrErrorHandlerFrame& handler, const SblrResult& error) {
  if (error.ok()) return false;
  if (handler.when_any) return true;
  if (handler.match_code.empty()) return false;
  if (PatternMatches(handler.match_code, ToString(error.status))) return true;
  for (const auto& diagnostic : error.diagnostics) {
    if (PatternMatches(handler.match_code, diagnostic.diagnostic_id)) return true;
    if (PatternMatches(handler.match_code, DiagnosticFieldValue(diagnostic, "sqlstate"))) return true;
  }
  return false;
}

SblrResult SelectSblrErrorHandler(std::string_view operation_id,
                                  const std::vector<SblrErrorHandlerFrame>& handlers,
                                  const SblrResult& error,
                                  const SblrExecutionContext& context) {
  for (const auto& handler : handlers) {
    if (SblrErrorHandlerMatches(handler, error)) {
      return HandlerSelectionResult(operation_id, handler.handler_uuid);
    }
  }
  return RefuseUnhandledSblrError(context, "no WHEN handler matched the raised error");
}

SblrResult UnwindSblrFramesForErrorHandler(std::string_view operation_id,
                                           SblrFrameStack* stack,
                                           std::string_view handler_uuid,
                                           const SblrExecutionContext& context) {
  if (stack == nullptr) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           std::string(operation_id),
                           MakeSblrRefusalDiagnostic("SB_DIAG_SBLR_FRAME_STACK_REQUIRED", context, "frame stack is required for error unwind"));
  }
  while (!stack->frames.empty()) {
    SblrFrame& frame = stack->frames.back();
    frame.rollback_region_open = false;
    if (frame.frame_uuid == handler_uuid) {
      return MakeSblrSuccess(std::string(operation_id));
    }
    SblrResult failure;
    if (!PopSblrFrame(stack, &failure)) return failure;
  }
  return MakeSblrFailure(SblrStatusCode::execution_failed,
                         std::string(operation_id),
                         MakeSblrRefusalDiagnostic("SB_DIAG_SBLR_ERROR_HANDLER_FRAME_NOT_FOUND",
                                                   context,
                                                   "matched error handler frame was not present during unwind"));
}

SblrResult RefuseUnhandledSblrError(const SblrExecutionContext& context, std::string detail) {
  return MakeSblrFailure(SblrStatusCode::execution_failed, "sblr.error.unhandled",
                         MakeSblrRefusalDiagnostic("SB_DIAG_SBLR_ERROR_UNHANDLED", context, std::move(detail)));
}

}  // namespace scratchbird::engine::sblr
