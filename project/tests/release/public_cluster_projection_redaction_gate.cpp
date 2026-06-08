// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_CLUSTER_PROJECTION_REDACTION_GATE

#include "observability/cluster_support_bundle_redaction_api.hpp"
#include "public_release_authz_fixture.hpp"
#include "security/visibility_api.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind) {
  return std::any_of(
      result.evidence.begin(),
      result.evidence.end(),
      [kind](const api::EngineEvidenceReference& evidence) {
        return evidence.evidence_kind == kind;
      });
}

api::EngineRequestContext Context(std::vector<std::string> rights) {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.database_uuid.canonical =
      "database:public-cluster-projection-redaction-pcr106";
  context.cluster_uuid.canonical =
      "cluster:public-cluster-projection-redaction-pcr106";
  context.principal_uuid.canonical =
      "principal:public-cluster-projection-redaction-pcr106";
  context.trace_tags.push_back("public_cluster_projection_redaction_gate");
  for (const auto& right : rights) {
    context.trace_tags.push_back("right:" + right);
    scratchbird::tests::release::GrantMaterializedRight(&context, right);
  }
  return context;
}

std::string ClearValue(api::ClusterProjectionRedactionSensitivity sensitivity) {
  switch (sensitivity) {
    case api::ClusterProjectionRedactionSensitivity::topology:
      return "node-private-address=10.9.8.7";
    case api::ClusterProjectionRedactionSensitivity::security:
      return "security-binding-secret=cluster-policy-material";
    case api::ClusterProjectionRedactionSensitivity::route:
      return "route-token=route-to-shard-secret";
    case api::ClusterProjectionRedactionSensitivity::metric:
      return "metric-label=node-health-secret";
  }
  return "unexpected-sensitive-value";
}

std::vector<api::ClusterProjectionRedactionSensitivity> Sensitivities() {
  return {api::ClusterProjectionRedactionSensitivity::topology,
          api::ClusterProjectionRedactionSensitivity::security,
          api::ClusterProjectionRedactionSensitivity::route,
          api::ClusterProjectionRedactionSensitivity::metric};
}

api::EngineEvaluateClusterProjectionRedactionRequest ProjectionRequest(
    api::ClusterProjectionRedactionSensitivity sensitivity,
    api::EngineRequestContext context) {
  api::EngineEvaluateClusterProjectionRedactionRequest request;
  request.context = std::move(context);
  request.operation_id = "public_cluster_projection_redaction_gate";
  request.sensitivity = sensitivity;
  request.projection_id = "projection:" +
                          api::ClusterProjectionRedactionSensitivityName(
                              sensitivity);
  request.projection_source = "cluster.sys.catalog." +
                              api::ClusterProjectionRedactionSensitivityName(
                                  sensitivity);
  request.target_uuid =
      "cluster-target:" + api::ClusterProjectionRedactionSensitivityName(
                              sensitivity);
  request.retention_policy_ref = "retention.cluster.projection.90d";
  request.support_bundle_policy_ref = "support.cluster.projection.redacted";
  request.retention_evidence_present = true;
  request.fields.push_back(
      {"sensitive_detail", ClearValue(sensitivity), true});
  request.fields.push_back({"projection_state", "active", false});
  return request;
}

api::ClusterSupportBundleProjectionSource BundleProjection(
    api::ClusterProjectionRedactionSensitivity sensitivity) {
  api::ClusterSupportBundleProjectionSource source;
  source.sensitivity = sensitivity;
  source.projection_id = "bundle-projection:" +
                         api::ClusterProjectionRedactionSensitivityName(
                             sensitivity);
  source.projection_source = "cluster.sys.catalog." +
                             api::ClusterProjectionRedactionSensitivityName(
                                 sensitivity);
  source.target_uuid =
      "bundle-cluster-target:" +
      api::ClusterProjectionRedactionSensitivityName(sensitivity);
  source.retention_policy_ref = "retention.cluster.projection.90d";
  source.support_bundle_policy_ref = "support.cluster.projection.redacted";
  source.retention_evidence_present = true;
  source.fields.push_back({"sensitive_detail", ClearValue(sensitivity), true});
  source.fields.push_back({"projection_state", "active", false});
  return source;
}

void TestUnauthorizedClusterProjectionRedaction() {
  for (const auto sensitivity : Sensitivities()) {
    auto request = ProjectionRequest(sensitivity, Context({}));
    const auto result = api::EngineEvaluateClusterProjectionRedaction(request);
    Require(result.ok, "unauthorized projection should return redacted output");
    Require(!result.visible, "unauthorized projection was visible");
    Require(result.redacted, "unauthorized projection was not redacted");
    Require(!result.failed_closed, "unauthorized projection failed closed");
    Require(result.retention_evidence_present,
            "unauthorized projection lost retention evidence");
    Require(!result.local_runtime_execution_enabled,
            "redaction enabled local runtime execution");
    Require(!result.local_projection_cluster_authority,
            "redaction claimed local cluster authority");
    Require(HasEvidence(result, "CLUSTER_PROJECTION_REDACTION"),
            "redaction evidence search key missing");
    Require(result.fields.size() == 2, "redacted field count mismatch");
    Require(result.fields[0].redacted, "sensitive field was not redacted");
    Require(result.fields[0].value != ClearValue(sensitivity),
            "sensitive clear value leaked");
    Require(result.fields[1].value == "active",
            "non-sensitive projection state was not preserved");
  }
}

void TestAuthorizedClusterProjectionVisibility() {
  for (const auto sensitivity : Sensitivities()) {
    const auto right =
        api::ClusterProjectionRedactionRequiredRight(sensitivity);
    auto request = ProjectionRequest(sensitivity, Context({right}));
    const auto result = api::EngineEvaluateClusterProjectionRedaction(request);
    Require(result.ok, "authorized projection was refused");
    Require(result.visible, "authorized projection was not visible");
    Require(!result.redacted, "authorized projection was redacted");
    Require(result.fields.size() == 2, "authorized field count mismatch");
    Require(result.fields[0].value == ClearValue(sensitivity),
            "authorized clear value was not returned");
    Require(result.required_right == right,
            "projection required-right mapping changed");
  }
}

void TestClusterProjectionFailClosedInputs() {
  auto missing_security =
      ProjectionRequest(api::ClusterProjectionRedactionSensitivity::topology,
                        Context({"OBS_CLUSTER_HEALTH_INSPECT"}));
  missing_security.context.security_context_present = false;
  auto result =
      api::EngineEvaluateClusterProjectionRedaction(missing_security);
  Require(!result.ok && result.failed_closed,
          "missing security context did not fail closed");

  auto missing_cluster_authority =
      ProjectionRequest(api::ClusterProjectionRedactionSensitivity::route,
                        Context({"OBS_DATA_MOVEMENT_INSPECT"}));
  missing_cluster_authority.context.cluster_authority_available = false;
  result = api::EngineEvaluateClusterProjectionRedaction(
      missing_cluster_authority);
  Require(!result.ok && result.failed_closed,
          "missing cluster authority did not fail closed");

  auto missing_retention =
      ProjectionRequest(api::ClusterProjectionRedactionSensitivity::metric,
                        Context({"OBS_METRICS_READ_CLUSTER"}));
  missing_retention.retention_evidence_present = false;
  result = api::EngineEvaluateClusterProjectionRedaction(missing_retention);
  Require(!result.ok && result.failed_closed,
          "missing retention evidence did not fail closed");
}

void TestSupportBundleProjectionRedaction() {
  api::EngineBuildClusterProjectionSupportBundleRedactionRequest request;
  request.context = Context({"SUPPORT_EXPORT"});
  request.operation_id = "public_cluster_projection_redaction_gate";
  request.support_bundle_id = "support-bundle:pcr106";
  request.capture_generation = "generation:1";
  for (const auto sensitivity : Sensitivities()) {
    request.projections.push_back(BundleProjection(sensitivity));
  }

  const auto result =
      api::EngineBuildClusterProjectionSupportBundleRedaction(request);
  Require(result.ok, "support bundle redaction was refused");
  Require(result.bundle_allowed, "support bundle was not allowed");
  Require(result.sensitive_values_redacted,
          "support bundle did not redact sensitive values");
  Require(result.retention_evidence_present,
          "support bundle lost retention evidence");
  Require(!result.failed_closed, "support bundle failed closed unexpectedly");
  Require(!result.local_runtime_execution_enabled,
          "support bundle enabled local runtime execution");
  Require(!result.local_projection_cluster_authority,
          "support bundle claimed local cluster authority");
  Require(HasEvidence(result, "CLUSTER_SUPPORT_BUNDLE_REDACTION"),
          "support bundle redaction evidence search key missing");
  Require(result.projection_decisions.size() == Sensitivities().size(),
          "support bundle projection count mismatch");
  for (const auto sensitivity : Sensitivities()) {
    Require(!Contains(result.support_bundle_json, ClearValue(sensitivity)),
            "support bundle leaked a sensitive clear value");
    Require(Contains(result.support_bundle_json,
                     "[redacted:" +
                         api::ClusterProjectionRedactionSensitivityName(
                             sensitivity) +
                         "]"),
            "support bundle missing redaction marker");
  }
}

void TestSupportBundleRequiresSupportExport() {
  api::EngineBuildClusterProjectionSupportBundleRedactionRequest request;
  request.context = Context({});
  request.operation_id = "public_cluster_projection_redaction_gate";
  request.support_bundle_id = "support-bundle:pcr106-denied";
  request.capture_generation = "generation:1";
  request.projections.push_back(
      BundleProjection(api::ClusterProjectionRedactionSensitivity::security));

  const auto result =
      api::EngineBuildClusterProjectionSupportBundleRedaction(request);
  Require(!result.ok && result.failed_closed,
          "support bundle without SUPPORT_EXPORT did not fail closed");
  Require(!result.bundle_allowed,
          "support bundle without SUPPORT_EXPORT was allowed");
}

}  // namespace

int main() {
  TestUnauthorizedClusterProjectionRedaction();
  TestAuthorizedClusterProjectionVisibility();
  TestClusterProjectionFailClosedInputs();
  TestSupportBundleProjectionRedaction();
  TestSupportBundleRequiresSupportExport();
  return EXIT_SUCCESS;
}
