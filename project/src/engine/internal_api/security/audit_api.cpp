// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/audit_api.hpp"

#include "security/security_model.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_AUDIT_API_BEHAVIOR
EngineEmitAuditEventResult EngineEmitAuditEvent(const EngineEmitAuditEventRequest& request) {
  const std::string event_class = !request.event_class.empty() ? request.event_class : SecurityOptionValue(request, "event_class:");
  const std::string outcome = !request.outcome.empty() ? request.outcome : SecurityOptionValue(request, "outcome:");
  if (event_class.empty()) {
    return SecurityFailure<EngineEmitAuditEventResult>(
        request.context,
        "security.emit_audit_event",
        MakeSecurityDiagnostic("SECURITY.AUDIT.EVIDENCE_REQUIRED", "event_class_required"));
  }
  const auto evidence = AppendSecurityEvidenceEvent(request.context, "security.emit_audit_event", event_class, outcome);
  if (evidence.error) {
    return SecurityFailure<EngineEmitAuditEventResult>(request.context, "security.emit_audit_event", evidence);
  }
  auto result = SecuritySuccess<EngineEmitAuditEventResult>(request.context, "security.emit_audit_event");
  result.emitted = true;
  result.redacted = SecurityOptionBool(request, "redact:", false);
  AddSecurityEvidence(&result, "audit_event", event_class);
  if (result.redacted) {
    result.diagnostics.push_back(MakeSecurityDiagnostic("SECURITY.AUDIT.REDACTED", event_class));
    result.diagnostics.back().error = false;
  }
  AddSecurityRow(&result, {{"event_class", event_class}, {"outcome", outcome}, {"redacted", result.redacted ? "true" : "false"}});
  return result;
}

EngineEmitLifecycleAuditEventResult EngineEmitLifecycleAuditEvent(
    const EngineEmitLifecycleAuditEventRequest& request) {
  const std::string operation = request.operation_key.empty()
      ? SecurityOptionValue(request, "operation_key:")
      : request.operation_key;
  if (operation.empty()) {
    return SecurityFailure<EngineEmitLifecycleAuditEventResult>(
        request.context,
        "security.emit_lifecycle_audit_event",
        MakeSecurityDiagnostic("SECURITY.AUDIT.EVIDENCE_REQUIRED", "operation_key_required"));
  }
  const std::string outcome = request.outcome.empty()
      ? SecurityOptionValue(request, "outcome:")
      : request.outcome;
  const std::string diagnostic = request.diagnostic_code.empty()
      ? SecurityOptionValue(request, "diagnostic_code:")
      : request.diagnostic_code;
  const std::string correlation = request.correlation_uuid.empty()
      ? SecurityOptionValue(request, "correlation_uuid:")
      : request.correlation_uuid;
  const auto evidence = AppendSecurityEvidenceEvent(
      request.context,
      "security.emit_lifecycle_audit_event",
      "lifecycle_audit",
      "operation=" + operation + ";outcome=" + outcome +
          ";diagnostic_code=" + diagnostic + ";correlation_uuid=" + correlation);
  if (evidence.error) {
    return SecurityFailure<EngineEmitLifecycleAuditEventResult>(
        request.context,
        "security.emit_lifecycle_audit_event",
        evidence);
  }
  auto result = SecuritySuccess<EngineEmitLifecycleAuditEventResult>(
      request.context,
      "security.emit_lifecycle_audit_event");
  result.emitted = true;
  result.redacted = true;
  result.cache_marker_linked = request.cache_invalidation_recorded ||
                               SecurityOptionBool(request, "cache_invalidation_recorded:", false);
  AddSecurityEvidence(&result, "lifecycle_audit_event", operation + ":" + outcome);
  AddSecurityEvidence(&result, "message_vector_shape", "diag.server.lifecycle.v1");
  if (result.cache_marker_linked) {
    const auto marker = request.cache_marker_uuid.empty()
        ? SecurityOptionValue(request, "cache_marker_uuid:")
        : request.cache_marker_uuid;
    AddSecurityEvidence(&result, "lifecycle_cache_invalidation", marker.empty() ? operation : marker);
  }
  AddSecurityRow(&result,
                 {{"operation_key", operation},
                  {"outcome", outcome},
                  {"diagnostic_code", diagnostic},
                  {"correlation_uuid", correlation},
                  {"redacted", "true"},
                  {"public_private_shape_separated", "true"},
                  {"parser_finality_authority", "false"},
                  {"reference_finality_authority", "false"},
                  {"cache_marker_linked", result.cache_marker_linked ? "true" : "false"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
