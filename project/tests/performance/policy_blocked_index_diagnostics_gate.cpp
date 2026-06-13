// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_management.hpp"
#include "index_maintenance.hpp"
#include "index_optimizer_integration.hpp"
#include "index_spatial_vector_graph_access.hpp"
#include "policy_blocked_index_admission.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace idx = scratchbird::core::index;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "policy_blocked_index_diagnostics_gate: " << message << '\n';
    std::exit(1);
  }
}

scratchbird::core::platform::TypedUuid TestObjectUuid(unsigned char seed) {
  scratchbird::core::platform::TypedUuid uuid;
  uuid.kind = scratchbird::core::platform::UuidKind::object;
  for (std::size_t i = 0; i < uuid.value.bytes.size(); ++i) {
    uuid.value.bytes[i] = static_cast<scratchbird::core::platform::byte>(
        seed + static_cast<unsigned char>(i + 1));
  }
  uuid.value.bytes[6] =
      static_cast<scratchbird::core::platform::byte>((uuid.value.bytes[6] & 0x0f) | 0x70);
  uuid.value.bytes[8] =
      static_cast<scratchbird::core::platform::byte>((uuid.value.bytes[8] & 0x3f) | 0x80);
  return uuid;
}

idx::PolicyBlockedIndexRouteRequest BaseRequest() {
  auto request = idx::MakePolicyBlockedIndexRouteRequest(
      idx::IndexFamily::policy_blocked,
      "advanced_vector_policy_blocked",
      "optimizer.index_path",
      "full_scan",
      true);
  request.required_feature = "advanced_vector_index_provider";
  request.required_policy = "SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA";
  return request;
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

bool HasArgument(const scratchbird::core::platform::DiagnosticRecord& diagnostic,
                 const std::string& key,
                 const std::string& value) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == key && argument.value == value) {
      return true;
    }
  }
  return false;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

void AppendDiagnosticValues(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    std::vector<std::string>* values) {
  values->push_back(diagnostic.diagnostic_code);
  values->push_back(diagnostic.message_key);
  values->push_back(diagnostic.source_component);
  values->push_back(diagnostic.remediation_hint);
  for (const auto& argument : diagnostic.arguments) {
    values->push_back(argument.key);
    values->push_back(argument.value);
  }
}

void RequireNoRuntimeDocLeak(const std::vector<std::string>& values) {
  for (const auto& value : values) {
    const auto lower = Lower(value);
    for (const auto marker :
         {"todo", "stub", "defer", "execution_plan", "spec", "docs/"}) {
      Require(lower.find(marker) == std::string::npos,
              std::string("runtime value leaked marker ") + marker + ": " +
                  value);
    }
  }
}

void RequireExactCommonFields(
    const idx::PolicyBlockedIndexAdmissionResult& result,
    const std::string& diagnostic_code,
    idx::PolicyBlockedIndexRefusalReason reason,
    const std::string& fallback_path) {
  Require(result.fail_closed, "policy-blocked route did not fail closed");
  Require(!result.ok(), "policy-blocked route reported executable success");
  Require(!result.executable && !result.physical &&
              !result.native_physical_provider && !result.authoritative,
          "policy-blocked result exposed executable or authoritative surface");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          "diagnostic code mismatch");
  Require(result.reason == reason, "refusal reason enum mismatch");
  Require(HasArgument(result.diagnostic,
                      "reason",
                      idx::PolicyBlockedIndexRefusalReasonName(reason)),
          "diagnostic reason argument missing");
  Require(HasArgument(result.diagnostic,
                      "required_feature",
                      "advanced_vector_index_provider"),
          "diagnostic required feature missing");
  Require(HasArgument(result.diagnostic,
                      "required_policy",
                      "SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA"),
          "diagnostic required policy missing");
  Require(HasArgument(result.diagnostic, "fallback_path", fallback_path),
          "diagnostic fallback path missing");
  Require(HasArgument(result.diagnostic, "route", "optimizer.index_path"),
          "diagnostic route missing");
  Require(HasEvidence(result.evidence,
                      "policy_blocked.required_feature=advanced_vector_index_provider"),
          "evidence required feature missing");
  Require(HasEvidence(result.evidence,
                      "policy_blocked.required_policy=SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA"),
          "evidence required policy missing");
  Require(HasEvidence(result.evidence,
                      "policy_blocked.fallback_path=" + fallback_path),
          "evidence fallback path missing");
  Require(HasEvidence(result.evidence,
                      "policy_blocked.silent_downgrade_allowed=false"),
          "no-silent-downgrade evidence missing");
}

void ExactRefusalDiagnostics() {
  auto policy = BaseRequest();
  const auto policy_result = idx::EvaluatePolicyBlockedIndexAdmission(policy);
  RequireExactCommonFields(
      policy_result,
      "INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
      idx::PolicyBlockedIndexRefusalReason::policy_not_accepted,
      "full_scan");

  auto feature = BaseRequest();
  feature.policy_accepted = true;
  feature.required_feature_available = false;
  const auto feature_result = idx::EvaluatePolicyBlockedIndexAdmission(feature);
  RequireExactCommonFields(
      feature_result,
      "INDEX.POLICY_BLOCKED.REQUIRED_FEATURE_MISSING",
      idx::PolicyBlockedIndexRefusalReason::required_feature_missing,
      "full_scan");

  auto fallback = BaseRequest();
  fallback.policy_accepted = true;
  fallback.required_feature_available = true;
  fallback.fallback_available = false;
  fallback.fallback_path = "missing";
  const auto fallback_result =
      idx::EvaluatePolicyBlockedIndexAdmission(fallback);
  RequireExactCommonFields(
      fallback_result,
      "INDEX.POLICY_BLOCKED.FALLBACK_MISSING",
      idx::PolicyBlockedIndexRefusalReason::fallback_missing,
      "missing");
  Require(HasEvidence(fallback_result.evidence,
                      "policy_blocked.fallback_available=false"),
          "fallback availability evidence missing");

  auto downgrade = BaseRequest();
  downgrade.policy_accepted = true;
  downgrade.required_feature_available = true;
  downgrade.silent_downgrade_attempted = true;
  const auto downgrade_result =
      idx::EvaluatePolicyBlockedIndexAdmission(downgrade);
  RequireExactCommonFields(
      downgrade_result,
      "INDEX.POLICY_BLOCKED.SILENT_DOWNGRADE_REFUSED",
      idx::PolicyBlockedIndexRefusalReason::silent_downgrade_attempted,
      "full_scan");
}

void PhysicalRouteRefusal() {
  auto request = BaseRequest();
  request.policy_accepted = true;
  request.required_feature_available = true;
  request.physical_accepted_family_route_attempted = true;
  request.attempted_physical_family = idx::IndexFamily::btree;
  const auto result = idx::EvaluatePolicyBlockedIndexAdmission(request);
  RequireExactCommonFields(
      result,
      "INDEX.POLICY_BLOCKED.PHYSICAL_ACCEPTED_ROUTE_REFUSED",
      idx::PolicyBlockedIndexRefusalReason::
          physical_accepted_family_route_attempted,
      "full_scan");
  Require(HasArgument(result.diagnostic,
                      "attempted_physical_family",
                      "btree"),
          "attempted physical family diagnostic missing");
  Require(HasEvidence(result.evidence,
                      "policy_blocked.attempted_physical_family=btree"),
          "attempted physical family evidence missing");
}

std::vector<std::pair<std::string, idx::PolicyBlockedIndexAuthorityClaims>>
AuthorityCases() {
  std::vector<std::pair<std::string, idx::PolicyBlockedIndexAuthorityClaims>>
      cases;
  idx::PolicyBlockedIndexAuthorityClaims claims;
  claims.parser = true;
  cases.push_back({"parser", claims});
  claims = {};
  claims.reference = true;
  cases.push_back({"reference", claims});
  claims = {};
  claims.provider = true;
  cases.push_back({"provider", claims});
  claims = {};
  claims.index = true;
  cases.push_back({"index", claims});
  claims = {};
  claims.security = true;
  cases.push_back({"security", claims});
  claims = {};
  claims.visibility = true;
  cases.push_back({"visibility", claims});
  claims = {};
  claims.transaction = true;
  cases.push_back({"transaction", claims});
  claims = {};
  claims.recovery = true;
  cases.push_back({"recovery", claims});
  claims = {};
  claims.log = true;
  cases.push_back({"log", claims});
  return cases;
}

void AuthorityClaimsRefused() {
  for (const auto& item : AuthorityCases()) {
    auto request = BaseRequest();
    request.policy_accepted = true;
    request.required_feature_available = true;
    request.authority_claims = item.second;
    const auto result = idx::EvaluatePolicyBlockedIndexAdmission(request);
    RequireExactCommonFields(
        result,
        "INDEX.POLICY_BLOCKED.AUTHORITY_CLAIM_REFUSED",
        idx::PolicyBlockedIndexRefusalReason::authority_claim_refused,
        "full_scan");
    Require(HasArgument(result.diagnostic, "authority_claim", item.first),
            "authority claim diagnostic missing: " + item.first);
    Require(HasEvidence(result.evidence,
                        "policy_blocked.authority_claim=" + item.first),
            "authority claim evidence missing: " + item.first);
  }
}

void MetadataProjectionIsVisibleAndNonExecutable() {
  const auto metadata = idx::ProjectPolicyBlockedIndexMetadata(BaseRequest());
  Require(metadata.ok(), "metadata projection was refused");
  Require(metadata.management_visible && metadata.blocked_state_visible,
          "blocked metadata is not management visible");
  Require(!metadata.executable && !metadata.physical &&
              !metadata.native_physical_provider && !metadata.authoritative,
          "metadata projection became executable, physical, or authoritative");
  Require(metadata.diagnostic.diagnostic_code ==
              "INDEX.POLICY_BLOCKED.METADATA_VISIBLE",
          "metadata diagnostic mismatch");
  Require(HasEvidence(metadata.evidence,
                      "policy_blocked.metadata_visible=true"),
          "metadata visibility evidence missing");
  Require(HasEvidence(metadata.evidence,
                      "policy_blocked.blocked_state_visible=true"),
          "blocked-state visibility evidence missing");
}

void EquivalentPolicyBlockedProfileIsNormalized() {
  auto request = idx::MakePolicyBlockedIndexRouteRequest(
      idx::IndexFamily::btree,
      "Advanced Vector Policy Blocked",
      "optimizer.index_path",
      "full_scan",
      true);
  const auto result = idx::EvaluatePolicyBlockedIndexAdmission(request);
  Require(result.fail_closed && !result.ok(),
          "equivalent policy-blocked profile did not fail closed");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
          "equivalent policy-blocked profile diagnostic drift");
  Require(result.reason ==
              idx::PolicyBlockedIndexRefusalReason::policy_not_accepted,
          "equivalent policy-blocked profile reason drift");
  Require(HasArgument(result.diagnostic,
                      "required_policy",
                      "SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA"),
          "equivalent policy-blocked profile required policy missing");
  Require(HasArgument(result.diagnostic, "fallback_path", "full_scan"),
          "equivalent policy-blocked profile fallback path missing");
  Require(result.requested_profile == "advanced_vector_policy_blocked",
          "policy-blocked profile was not normalized");
  Require(result.required_feature == "advanced_vector_policy_blocked",
          "equivalent policy-blocked profile kept native family feature");
  Require(HasArgument(result.diagnostic,
                      "required_feature",
                      "advanced_vector_policy_blocked"),
          "equivalent policy-blocked profile required feature missing");
  Require(HasEvidence(result.evidence,
                      "policy_blocked.profile=advanced_vector_policy_blocked"),
          "normalized policy-blocked profile evidence missing");

  auto metadata_request = idx::MakePolicyBlockedIndexRouteRequest(
      idx::IndexFamily::btree,
      "btree",
      "index.management",
      "full_scan",
      true);
  const auto metadata =
      idx::ProjectPolicyBlockedIndexMetadata(metadata_request);
  Require(!metadata.ok() &&
              metadata.diagnostic.diagnostic_code ==
                  "INDEX.POLICY_BLOCKED.NOT_POLICY_BLOCKED_REQUEST",
          "metadata projection accepted non-policy-blocked request");
}

void RouteHelpersFailBeforePhysicalPlanning() {
  idx::IndexOptimizerRequest optimizer;
  optimizer.index_uuid = TestObjectUuid(0x81);
  optimizer.family = idx::IndexFamily::policy_blocked;
  const auto optimized = idx::PlanIndexOptimizerPath(optimizer);
  Require(!optimized.ok() && optimized.fallback_full_scan,
          "optimizer admitted policy-blocked route");
  Require(optimized.diagnostic.diagnostic_code ==
              "INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
          "optimizer did not emit policy-blocked diagnostic");

  idx::IndexMaintenanceRequest maintenance;
  maintenance.index_uuid = TestObjectUuid(0x82);
  maintenance.family = idx::IndexFamily::policy_blocked;
  maintenance.operation = idx::IndexMaintenanceOperation::verify;
  maintenance.page_budget = 1;
  const auto maintained = idx::PlanIndexMaintenance(maintenance);
  Require(!maintained.ok(),
          "maintenance admitted policy-blocked physical route");
  Require(maintained.diagnostic.diagnostic_code ==
              "INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
          "maintenance did not emit policy-blocked diagnostic");

  idx::IndexManagementRequest inspect;
  inspect.index_uuid = TestObjectUuid(0x83);
  inspect.family = idx::IndexFamily::policy_blocked;
  inspect.operation = idx::IndexManagementOperation::inspect;
  const auto inspected = idx::PlanIndexManagementOperation(inspect);
  Require(inspected.ok(), "management inspect did not expose blocked metadata");
  Require(HasEvidence(inspected.steps,
                      "policy_blocked.metadata_visible=true"),
          "management inspect metadata evidence missing");

  idx::IndexManagementRequest create = inspect;
  create.operation = idx::IndexManagementOperation::create;
  const auto created = idx::PlanIndexManagementOperation(create);
  Require(!created.ok(), "management mutation admitted policy-blocked route");
  Require(created.diagnostic.diagnostic_code ==
              "INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
          "management mutation did not emit policy-blocked diagnostic");
}

void VectorRouteReportsExplicitFallback() {
  idx::IndexVectorAdmissionRequest request;
  request.algorithm = idx::IndexVectorAlgorithm::annoy;
  request.descriptor.metric_resource_uuid = TestObjectUuid(0x84);
  request.descriptor.dimensions = 8;
  request.exact_fallback_allowed = true;
  const auto decision = idx::AdmitVectorIndex(request);
  Require(decision.ok(), "explicit vector fallback was not admitted");
  Require(decision.family == idx::IndexFamily::vector_exact,
          "explicit fallback did not route to vector_exact");
  Require(decision.requested_algorithm_policy_blocked &&
              decision.exact_fallback,
          "explicit policy-blocked fallback flags missing");
  Require(decision.diagnostic.diagnostic_code ==
              "INDEX.POLICY_BLOCKED.EXPLICIT_FALLBACK_SELECTED",
          "vector route did not emit explicit fallback diagnostic");
  Require(HasArgument(decision.diagnostic,
                      "fallback_path",
                      "vector_exact"),
          "vector fallback path diagnostic missing");
}

void PolicyBlockedCapabilityDoesNotPromote() {
  const auto* descriptor =
      idx::FindBuiltinIndexFamily(idx::IndexFamily::policy_blocked);
  Require(descriptor != nullptr, "policy-blocked descriptor missing");
  Require(descriptor->persistence == idx::IndexPersistenceClass::policy_blocked,
          "policy-blocked descriptor persistence promoted");
  Require(descriptor->completion ==
              idx::IndexCompletionStatus::policy_blocked_alpha,
          "policy-blocked descriptor completion promoted");
  Require(!descriptor->persistent,
          "policy-blocked descriptor became persistent physical storage");

  const auto* state = idx::FindBuiltinIndexFamilyPhysicalCapabilityState(
      idx::IndexFamily::policy_blocked);
  Require(state != nullptr, "policy-blocked capability state missing");
  Require(!state->runtime_available && !state->benchmark_clean,
          "policy-blocked capability advertised runtime or benchmark-clean");
  Require(!state->physically_complete(),
          "policy-blocked capability became physically complete");
  Require(state->blocker ==
              idx::IndexFamilyPhysicalCapabilityBlocker::policy_blocked,
          "policy-blocked capability blocker drifted");
}

void NoRuntimeDocLeakage() {
  std::vector<std::string> values;
  const auto refused = idx::EvaluatePolicyBlockedIndexAdmission(BaseRequest());
  values.insert(values.end(), refused.evidence.begin(), refused.evidence.end());
  AppendDiagnosticValues(refused.diagnostic, &values);

  const auto metadata = idx::ProjectPolicyBlockedIndexMetadata(BaseRequest());
  values.insert(values.end(), metadata.evidence.begin(), metadata.evidence.end());
  AppendDiagnosticValues(metadata.diagnostic, &values);

  auto fallback_request = BaseRequest();
  fallback_request.fallback_path = "vector_exact";
  fallback_request.fallback_available = true;
  values.push_back(fallback_request.fallback_path);

  RequireNoRuntimeDocLeak(values);
}

}  // namespace

int main() {
  ExactRefusalDiagnostics();
  PhysicalRouteRefusal();
  AuthorityClaimsRefused();
  MetadataProjectionIsVisibleAndNonExecutable();
  EquivalentPolicyBlockedProfileIsNormalized();
  RouteHelpersFailBeforePhysicalPlanning();
  VectorRouteReportsExplicitFallback();
  PolicyBlockedCapabilityDoesNotPromote();
  NoRuntimeDocLeakage();
  std::cout << "policy_blocked_index_diagnostics_gate=passed\n";
  return 0;
}
