// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_runtime.hpp"

#include <utility>

namespace scratchbird::engine::sblr {

SblrRuntimeDiagnostic MakeSblrDiagnostic(std::string diagnostic_id,
                                         std::string message_key,
                                         std::string detail,
                                         SblrDiagnosticSeverity severity) {
  SblrRuntimeDiagnostic diagnostic;
  diagnostic.diagnostic_id = std::move(diagnostic_id);
  diagnostic.message_key = std::move(message_key);
  diagnostic.detail = std::move(detail);
  diagnostic.severity = severity;
  return diagnostic;
}

SblrRuntimeDiagnostic MakeSblrRefusalDiagnostic(std::string diagnostic_id,
                                                const SblrExecutionContext& context,
                                                std::string detail) {
  auto diagnostic = MakeSblrDiagnostic(std::move(diagnostic_id), "engine.sblr.refusal", std::move(detail));
  diagnostic.fields.push_back({"cluster_uuid", context.cluster_uuid});
  diagnostic.fields.push_back({"node_uuid", context.node_uuid});
  diagnostic.fields.push_back({"database_uuid", context.database_uuid});
  diagnostic.fields.push_back({"transaction_uuid", context.transaction_uuid});
  diagnostic.fields.push_back({"local_transaction_id", std::to_string(context.local_transaction_id)});
  diagnostic.fields.push_back({"statement_uuid", context.statement_uuid});
  diagnostic.fields.push_back({"user_uuid", context.user_uuid});
  diagnostic.fields.push_back({"parser_profile_uuid", context.parser_profile_uuid});
  diagnostic.fields.push_back({"security_snapshot_uuid", context.security_snapshot_uuid});
  return diagnostic;
}

SblrResult MakeSblrSuccess(std::string operation_id) {
  SblrResult result;
  result.operation_id = std::move(operation_id);
  return result;
}

SblrResult MakeSblrFailure(SblrStatusCode status,
                           std::string operation_id,
                           SblrRuntimeDiagnostic diagnostic) {
  SblrResult result;
  result.status = status;
  result.operation_id = std::move(operation_id);
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

bool ValidateDiagnosticCompleteness(const SblrRuntimeDiagnostic& diagnostic,
                                    std::vector<std::string>* missing_fields) {
  const auto before = missing_fields ? missing_fields->size() : 0;
  if (diagnostic.diagnostic_id.empty() && missing_fields) missing_fields->push_back("diagnostic_id");
  if (diagnostic.message_key.empty() && missing_fields) missing_fields->push_back("message_key");
  auto has_field = [&](std::string_view key) {
    for (const auto& field : diagnostic.fields) {
      if (field.key == key) return true;
    }
    return false;
  };
  for (std::string_view key : {"database_uuid", "statement_uuid", "user_uuid", "security_snapshot_uuid"}) {
    if (!has_field(key) && missing_fields) missing_fields->push_back(std::string(key));
  }
  return !missing_fields || missing_fields->size() == before;
}

bool PushSblrFrame(SblrFrameStack* stack, SblrFrame frame, SblrResult* failure) {
  if (stack == nullptr) {
    if (failure) *failure = MakeSblrFailure(SblrStatusCode::internal_error, {}, MakeSblrDiagnostic("SB_DIAG_SBLR_FRAME_STACK_MISSING", "engine.sblr.frame.stack_missing"));
    return false;
  }
  if (stack->frames.size() >= stack->max_depth) {
    if (failure) *failure = MakeSblrFailure(SblrStatusCode::resource_exhausted, {}, MakeSblrDiagnostic("SB_DIAG_SBLR_FRAME_DEPTH_EXCEEDED", "engine.sblr.frame.depth_exceeded"));
    return false;
  }
  frame.depth = stack->frames.size() + 1;
  stack->frames.push_back(std::move(frame));
  return true;
}

bool PopSblrFrame(SblrFrameStack* stack, SblrResult* failure) {
  if (stack == nullptr || stack->frames.empty()) {
    if (failure) *failure = MakeSblrFailure(SblrStatusCode::internal_error, {}, MakeSblrDiagnostic("SB_DIAG_SBLR_FRAME_UNDERFLOW", "engine.sblr.frame.underflow"));
    return false;
  }
  if (stack->frames.back().rollback_region_open) {
    if (failure) *failure = MakeSblrFailure(SblrStatusCode::execution_failed, {}, MakeSblrDiagnostic("SB_DIAG_SBLR_FRAME_ROLLBACK_REGION_OPEN", "engine.sblr.frame.rollback_region_open"));
    return false;
  }
  stack->frames.pop_back();
  return true;
}

std::string ToString(SblrStatusCode status) {
  switch (status) {
    case SblrStatusCode::ok: return "ok";
    case SblrStatusCode::invalid_envelope: return "invalid_envelope";
    case SblrStatusCode::unsupported_feature: return "unsupported_feature";
    case SblrStatusCode::security_refused: return "security_refused";
    case SblrStatusCode::policy_refused: return "policy_refused";
    case SblrStatusCode::dependency_unavailable: return "dependency_unavailable";
    case SblrStatusCode::execution_failed: return "execution_failed";
    case SblrStatusCode::resource_exhausted: return "resource_exhausted";
    case SblrStatusCode::internal_error: return "internal_error";
  }
  return "internal_error";
}

std::string ToString(SblrDiagnosticSeverity severity) {
  switch (severity) {
    case SblrDiagnosticSeverity::info: return "info";
    case SblrDiagnosticSeverity::warning: return "warning";
    case SblrDiagnosticSeverity::error: return "error";
  }
  return "error";
}

}  // namespace scratchbird::engine::sblr
