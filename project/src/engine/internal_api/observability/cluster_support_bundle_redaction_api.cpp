// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/cluster_support_bundle_redaction_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "security/security_model.hpp"

#include <sstream>
#include <string_view>
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

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      default:
        out << ch;
    }
  }
  return out.str();
}

void AddClusterSupportBundleRedactionEvidence(
    EngineBuildClusterProjectionSupportBundleRedactionResult* result,
    const EngineBuildClusterProjectionSupportBundleRedactionRequest& request,
    std::string_view state) {
  AddApiBehaviorEvidence(result, "CLUSTER_SUPPORT_BUNDLE_REDACTION",
                         std::string(state));
  AddApiBehaviorEvidence(result, "cluster_support_bundle_id",
                         request.support_bundle_id);
  AddApiBehaviorEvidence(result, "cluster_projection_support_bundle_policy",
                         "retention_bound_redaction_only");
  AddApiBehaviorEvidence(result, "local_projection_cluster_authority", "false");
}

EngineBuildClusterProjectionSupportBundleRedactionResult
ClusterSupportBundleRedactionFailure(
    const EngineBuildClusterProjectionSupportBundleRedactionRequest& request,
    std::string code,
    std::string detail) {
  auto result = MakeApiBehaviorSuccess<
      EngineBuildClusterProjectionSupportBundleRedactionResult>(
      request.context,
      OperationIdOr(request,
                    "observability.cluster_projection_support_bundle"));
  result.ok = false;
  result.bundle_allowed = false;
  result.sensitive_values_redacted = true;
  result.retention_evidence_present = false;
  result.failed_closed = true;
  result.local_runtime_execution_enabled = false;
  result.local_projection_cluster_authority = false;
  result.diagnostics.push_back(
      MakeSecurityDiagnostic(std::move(code), std::move(detail)));
  AddClusterSupportBundleRedactionEvidence(&result, request, "fail_closed");
  AddApiBehaviorRow(
      &result,
      {{"support_bundle_id", request.support_bundle_id},
       {"capture_generation", request.capture_generation},
       {"bundle_allowed", "false"},
       {"failed_closed", "true"},
       {"sensitive_values_redacted", "true"},
       {"retention_evidence_present", "false"},
       {"projection_count", std::to_string(request.projections.size())},
       {"local_runtime_execution_enabled", "false"},
       {"local_projection_cluster_authority", "false"}});
  return result;
}

std::string RenderClusterProjectionSupportBundleJson(
    const EngineBuildClusterProjectionSupportBundleRedactionRequest& request,
    const EngineBuildClusterProjectionSupportBundleRedactionResult& result) {
  std::ostringstream out;
  out << "{\"support_bundle\":{\"section\":\"cluster_projections\","
      << "\"support_bundle_id\":\"" << JsonEscape(request.support_bundle_id)
      << "\",\"capture_generation\":\""
      << JsonEscape(request.capture_generation)
      << "\",\"redaction_state\":\"retention_bound_redaction_only\","
      << "\"local_projection_cluster_authority\":false,"
      << "\"projection_count\":" << result.projection_decisions.size()
      << ",\"projections\":[";
  for (std::size_t i = 0; i < result.projection_decisions.size(); ++i) {
    const auto& decision = result.projection_decisions[i];
    if (i != 0) { out << ','; }
    out << "{\"sensitivity\":\"" << JsonEscape(decision.sensitivity)
        << "\",\"required_right\":\"" << JsonEscape(decision.required_right)
        << "\",\"visible\":" << BoolString(decision.visible)
        << ",\"redacted\":" << BoolString(decision.redacted)
        << ",\"retention_policy_ref\":\""
        << JsonEscape(decision.retention_policy_ref)
        << "\",\"support_bundle_policy_ref\":\""
        << JsonEscape(decision.support_bundle_policy_ref)
        << "\",\"fields\":[";
    for (std::size_t j = 0; j < decision.fields.size(); ++j) {
      const auto& field = decision.fields[j];
      if (j != 0) { out << ','; }
      out << "{\"name\":\"" << JsonEscape(field.field_name)
          << "\",\"value\":\"" << JsonEscape(field.value)
          << "\",\"redacted\":" << BoolString(field.redacted)
          << ",\"redaction_class\":\""
          << JsonEscape(field.redaction_class) << "\"}";
    }
    out << "]}";
  }
  out << "]}}";
  return out.str();
}

}  // namespace

// SEARCH_KEY: CLUSTER_SUPPORT_BUNDLE_REDACTION
EngineBuildClusterProjectionSupportBundleRedactionResult
EngineBuildClusterProjectionSupportBundleRedaction(
    const EngineBuildClusterProjectionSupportBundleRedactionRequest& request) {
  if (request.support_bundle_id.empty()) {
    return ClusterSupportBundleRedactionFailure(
        request,
        "OBSERVABILITY.CLUSTER_SUPPORT_BUNDLE.ID_REQUIRED",
        "support_bundle_id_required");
  }
  if (request.capture_generation.empty()) {
    return ClusterSupportBundleRedactionFailure(
        request,
        "OBSERVABILITY.CLUSTER_SUPPORT_BUNDLE.GENERATION_REQUIRED",
        "capture_generation_required");
  }
  if (request.projections.empty()) {
    return ClusterSupportBundleRedactionFailure(
        request,
        "OBSERVABILITY.CLUSTER_SUPPORT_BUNDLE.PROJECTION_REQUIRED",
        "cluster_projection_required");
  }

  auto result = MakeApiBehaviorSuccess<
      EngineBuildClusterProjectionSupportBundleRedactionResult>(
      request.context,
      OperationIdOr(request,
                    "observability.cluster_projection_support_bundle"));
  result.bundle_allowed = true;
  result.sensitive_values_redacted = false;
  result.retention_evidence_present = true;
  result.failed_closed = false;
  result.local_runtime_execution_enabled = false;
  result.local_projection_cluster_authority = false;

  for (const auto& source : request.projections) {
    EngineEvaluateClusterProjectionRedactionRequest redaction_request;
    redaction_request.context = request.context;
    redaction_request.operation_id =
        "security.cluster_projection_redaction.support_bundle";
    redaction_request.sensitivity = source.sensitivity;
    redaction_request.projection_id = source.projection_id;
    redaction_request.projection_source = source.projection_source;
    redaction_request.target_uuid = source.target_uuid;
    redaction_request.retention_policy_ref = source.retention_policy_ref;
    redaction_request.support_bundle_policy_ref =
        source.support_bundle_policy_ref;
    redaction_request.retention_evidence_present =
        source.retention_evidence_present;
    redaction_request.support_bundle_export = true;
    redaction_request.fields = source.fields;

    auto decision =
        EngineEvaluateClusterProjectionRedaction(redaction_request);
    if (!decision.ok) {
      auto failure = ClusterSupportBundleRedactionFailure(
          request,
          "OBSERVABILITY.CLUSTER_SUPPORT_BUNDLE.PROJECTION_REDACTION_FAILED",
          decision.diagnostics.empty()
              ? "projection_redaction_failed"
              : decision.diagnostics.front().detail);
      failure.diagnostics.insert(failure.diagnostics.end(),
                                 decision.diagnostics.begin(),
                                 decision.diagnostics.end());
      failure.projection_decisions.push_back(std::move(decision));
      return failure;
    }
    result.bundle_allowed = result.bundle_allowed &&
                            decision.support_bundle_allowed;
    result.retention_evidence_present =
        result.retention_evidence_present &&
        decision.retention_evidence_present;
    for (const auto& field : decision.fields) {
      if (field.redacted) { result.sensitive_values_redacted = true; }
    }
    result.projection_decisions.push_back(std::move(decision));
  }

  AddClusterSupportBundleRedactionEvidence(&result, request, "redacted");
  AddApiBehaviorRow(
      &result,
      {{"support_bundle_id", request.support_bundle_id},
       {"capture_generation", request.capture_generation},
       {"bundle_allowed", BoolString(result.bundle_allowed)},
       {"failed_closed", "false"},
       {"sensitive_values_redacted",
        BoolString(result.sensitive_values_redacted)},
       {"retention_evidence_present",
        BoolString(result.retention_evidence_present)},
       {"projection_count",
        std::to_string(result.projection_decisions.size())},
       {"local_runtime_execution_enabled", "false"},
       {"local_projection_cluster_authority", "false"}});
  result.support_bundle_json =
      RenderClusterProjectionSupportBundleJson(request, result);
  return result;
}

}  // namespace scratchbird::engine::internal_api
