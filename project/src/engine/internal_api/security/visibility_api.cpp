// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/visibility_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "security/security_model.hpp"

#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::string BoolString(bool value) {
  return value ? "true" : "false";
}

std::string OperationIdOr(const EngineApiRequest& request,
                          const std::string& fallback) {
  return request.operation_id.empty() ? fallback : request.operation_id;
}

std::string ProjectionTargetUuid(
    const EngineEvaluateClusterProjectionRedactionRequest& request) {
  if (!request.target_uuid.empty()) { return request.target_uuid; }
  return request.target_object.uuid.canonical;
}

std::string RedactionClassFor(ClusterProjectionRedactionSensitivity sensitivity) {
  return "cluster_projection_" +
         ClusterProjectionRedactionSensitivityName(sensitivity) +
         "_sensitive";
}

std::string RedactedValueFor(ClusterProjectionRedactionSensitivity sensitivity) {
  return "[redacted:" + ClusterProjectionRedactionSensitivityName(sensitivity) +
         "]";
}

void AddClusterProjectionRedactionEvidence(
    EngineEvaluateClusterProjectionRedactionResult* result,
    const EngineEvaluateClusterProjectionRedactionRequest& request,
    const std::string& state) {
  AddApiBehaviorEvidence(result, "CLUSTER_PROJECTION_REDACTION", state);
  AddApiBehaviorEvidence(result, "cluster_projection_id",
                         request.projection_id);
  AddApiBehaviorEvidence(result, "cluster_projection_retention_policy",
                         request.retention_policy_ref);
  AddApiBehaviorEvidence(result, "cluster_projection_support_bundle_policy",
                         request.support_bundle_policy_ref);
  AddApiBehaviorEvidence(result, "local_projection_cluster_authority", "false");
}

EngineEvaluateClusterProjectionRedactionResult ClusterProjectionRedactionFailure(
    const EngineEvaluateClusterProjectionRedactionRequest& request,
    std::string code,
    std::string detail) {
  auto result = MakeApiBehaviorSuccess<
      EngineEvaluateClusterProjectionRedactionResult>(
      request.context,
      OperationIdOr(request, "security.cluster_projection_redaction"));
  result.ok = false;
  result.visible = false;
  result.redacted = true;
  result.failed_closed = true;
  result.retention_evidence_present = request.retention_evidence_present;
  result.support_bundle_allowed = false;
  result.local_runtime_execution_enabled = false;
  result.local_projection_cluster_authority = false;
  result.sensitivity =
      ClusterProjectionRedactionSensitivityName(request.sensitivity);
  result.required_right =
      ClusterProjectionRedactionRequiredRight(request.sensitivity);
  result.redaction_class = RedactionClassFor(request.sensitivity);
  result.retention_policy_ref = request.retention_policy_ref;
  result.support_bundle_policy_ref = request.support_bundle_policy_ref;
  result.diagnostics.push_back(
      MakeSecurityDiagnostic(std::move(code), std::move(detail)));
  AddClusterProjectionRedactionEvidence(&result, request, "fail_closed");
  AddApiBehaviorRow(
      &result,
      {{"projection_id", request.projection_id},
       {"sensitivity", result.sensitivity},
       {"target_uuid", ProjectionTargetUuid(request)},
       {"visible", "false"},
       {"redacted", "true"},
       {"failed_closed", "true"},
       {"support_bundle_export", BoolString(request.support_bundle_export)},
       {"support_bundle_allowed", "false"},
       {"retention_evidence_present",
        BoolString(request.retention_evidence_present)},
       {"retention_policy_ref", request.retention_policy_ref},
       {"local_runtime_execution_enabled", "false"},
       {"local_projection_cluster_authority", "false"}});
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_VISIBILITY_API_BEHAVIOR
EngineEvaluateVisibilityResult EngineEvaluateVisibility(const EngineEvaluateVisibilityRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineEvaluateVisibilityResult>(request.context, "security.evaluate_visibility");
  const std::string right = SecurityOptionValue(request, "visibility_right:").empty()
      ? "VISIBLE"
      : SecurityOptionValue(request, "visibility_right:");
  const bool visible = request.target_object.uuid.canonical.empty() ||
                       SecurityContextHasRight(request.context, right, request.target_object.uuid.canonical);
  AddApiBehaviorEvidence(&result, "visibility_decision", visible ? "allow" : "deny");
  AddApiBehaviorRow(&result, {{"decision", visible ? "allow" : "deny"},
                              {"right", right},
                              {"target_uuid", request.target_object.uuid.canonical},
                              {"target_kind", request.target_object.object_kind}});
  if (!visible) {
    result.ok = false;
    result.diagnostics.push_back(MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", right));
  }
  return result;
}

std::string ClusterProjectionRedactionSensitivityName(
    ClusterProjectionRedactionSensitivity sensitivity) {
  switch (sensitivity) {
    case ClusterProjectionRedactionSensitivity::topology:
      return "topology";
    case ClusterProjectionRedactionSensitivity::security:
      return "security";
    case ClusterProjectionRedactionSensitivity::route:
      return "route";
    case ClusterProjectionRedactionSensitivity::metric:
      return "metric";
  }
  return "unknown";
}

std::string ClusterProjectionRedactionRequiredRight(
    ClusterProjectionRedactionSensitivity sensitivity) {
  switch (sensitivity) {
    case ClusterProjectionRedactionSensitivity::topology:
      return "OBS_CLUSTER_HEALTH_INSPECT";
    case ClusterProjectionRedactionSensitivity::security:
      return "OBS_POLICY_READ";
    case ClusterProjectionRedactionSensitivity::route:
      return "OBS_DATA_MOVEMENT_INSPECT";
    case ClusterProjectionRedactionSensitivity::metric:
      return "OBS_METRICS_READ_CLUSTER";
  }
  return {};
}

// SEARCH_KEY: CLUSTER_PROJECTION_REDACTION
EngineEvaluateClusterProjectionRedactionResult
EngineEvaluateClusterProjectionRedaction(
    const EngineEvaluateClusterProjectionRedactionRequest& request) {
  if (request.projection_id.empty()) {
    return ClusterProjectionRedactionFailure(
        request,
        "SECURITY.CLUSTER_PROJECTION.PROJECTION_ID_REQUIRED",
        "projection_id_required");
  }
  if (request.retention_policy_ref.empty() ||
      !request.retention_evidence_present) {
    return ClusterProjectionRedactionFailure(
        request,
        "SECURITY.CLUSTER_PROJECTION.RETENTION_EVIDENCE_REQUIRED",
        "retention_policy_and_evidence_required");
  }
  if (!request.context.security_context_present) {
    return ClusterProjectionRedactionFailure(
        request,
        "SECURITY.CLUSTER_PROJECTION.SECURITY_CONTEXT_REQUIRED",
        "security_context_required");
  }
  if (!request.context.cluster_authority_available) {
    return ClusterProjectionRedactionFailure(
        request,
        "SECURITY.CLUSTER_PROJECTION.CLUSTER_AUTHORITY_REQUIRED",
        "cluster_authority_required");
  }
  const bool support_bundle_allowed =
      !request.support_bundle_export ||
      SecurityContextHasRight(request.context, "SUPPORT_EXPORT",
                              ProjectionTargetUuid(request));
  if (!support_bundle_allowed) {
    return ClusterProjectionRedactionFailure(
        request,
        "SECURITY.CLUSTER_PROJECTION.SUPPORT_EXPORT_DENIED",
        "support_export_right_required");
  }

  const std::string target_uuid = ProjectionTargetUuid(request);
  const std::string required_right =
      ClusterProjectionRedactionRequiredRight(request.sensitivity);
  const bool visible = SecurityContextHasRight(request.context,
                                              required_right,
                                              target_uuid);
  const bool redacted = !visible;
  auto result =
      MakeApiBehaviorSuccess<EngineEvaluateClusterProjectionRedactionResult>(
          request.context,
          OperationIdOr(request, "security.cluster_projection_redaction"));
  result.visible = visible;
  result.redacted = redacted;
  result.failed_closed = false;
  result.retention_evidence_present = true;
  result.support_bundle_allowed = support_bundle_allowed;
  result.local_runtime_execution_enabled = false;
  result.local_projection_cluster_authority = false;
  result.sensitivity =
      ClusterProjectionRedactionSensitivityName(request.sensitivity);
  result.required_right = required_right;
  result.redaction_class = RedactionClassFor(request.sensitivity);
  result.retention_policy_ref = request.retention_policy_ref;
  result.support_bundle_policy_ref = request.support_bundle_policy_ref;

  for (const auto& field : request.fields) {
    ClusterProjectionRedactedField redacted_field;
    redacted_field.field_name = field.field_name;
    redacted_field.redacted = redacted && field.sensitive;
    redacted_field.redaction_class =
        redacted_field.redacted ? result.redaction_class : "clear";
    redacted_field.value = redacted_field.redacted
                               ? RedactedValueFor(request.sensitivity)
                               : field.clear_value;
    result.fields.push_back(std::move(redacted_field));
  }

  AddClusterProjectionRedactionEvidence(
      &result, request, redacted ? "redacted" : "visible");
  AddApiBehaviorRow(
      &result,
      {{"projection_id", request.projection_id},
       {"projection_source", request.projection_source},
       {"sensitivity", result.sensitivity},
       {"required_right", result.required_right},
       {"target_uuid", target_uuid},
       {"visible", BoolString(result.visible)},
       {"redacted", BoolString(result.redacted)},
       {"failed_closed", "false"},
       {"support_bundle_export", BoolString(request.support_bundle_export)},
       {"support_bundle_allowed", BoolString(result.support_bundle_allowed)},
       {"retention_evidence_present", "true"},
       {"retention_policy_ref", result.retention_policy_ref},
       {"support_bundle_policy_ref", result.support_bundle_policy_ref},
       {"local_runtime_execution_enabled", "false"},
       {"local_projection_cluster_authority", "false"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
