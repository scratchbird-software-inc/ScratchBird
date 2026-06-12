// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "policy_blocked_index_admission.hpp"

#include <cctype>
#include <string_view>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error,
          Subsystem::engine};
}

const char* BoolText(bool value) { return value ? "true" : "false"; }

std::string NormalizePolicyToken(std::string_view input) {
  std::string out;
  bool last_underscore = false;
  for (unsigned char ch : input) {
    if (std::isalnum(ch) != 0) {
      out.push_back(static_cast<char>(std::tolower(ch)));
      last_underscore = false;
      continue;
    }
    if (!last_underscore) {
      out.push_back('_');
      last_underscore = true;
    }
  }
  while (!out.empty() && out.front() == '_') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

std::string FamilyId(IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  return descriptor != nullptr ? descriptor->id : IndexFamilyName(family);
}

bool HasPolicyBlockedProfileText(std::string_view profile) {
  const auto normalized = NormalizePolicyToken(profile);
  return normalized == "policy_blocked" ||
         normalized == "advanced_vector_policy_blocked";
}

std::string ProfileFor(const PolicyBlockedIndexRouteRequest& request) {
  if (!request.requested_profile.empty()) {
    return NormalizePolicyToken(request.requested_profile);
  }
  const auto* descriptor = FindBuiltinIndexFamily(request.requested_family);
  return descriptor != nullptr ? descriptor->id : "policy_blocked";
}

std::string RequiredFeatureFor(
    const PolicyBlockedIndexRouteRequest& request) {
  if (!request.required_feature.empty()) {
    return request.required_feature;
  }
  const auto profile = ProfileFor(request);
  if (HasPolicyBlockedProfileText(profile)) {
    return profile;
  }
  const auto* descriptor = FindBuiltinIndexFamily(request.requested_family);
  return descriptor != nullptr ? descriptor->id : "policy_blocked_index";
}

std::string RequiredPolicyFor(const PolicyBlockedIndexRouteRequest& request) {
  if (!request.required_policy.empty()) {
    return request.required_policy;
  }
  if (ProfileFor(request) == "advanced_vector_policy_blocked") {
    return "SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA";
  }
  if (HasPolicyBlockedProfileText(ProfileFor(request))) {
    return "SB_POLICY_INDEX_NOT_ACCEPTED_ALPHA";
  }
  const auto* descriptor = FindBuiltinIndexFamily(request.requested_family);
  if (descriptor != nullptr && !descriptor->default_semantic_profile.empty()) {
    return descriptor->default_semantic_profile;
  }
  return "SB_POLICY_INDEX_NOT_ACCEPTED_ALPHA";
}

std::string RouteFor(const PolicyBlockedIndexRouteRequest& request) {
  return request.route.empty() ? "index_route" : request.route;
}

std::string FallbackPathFor(const PolicyBlockedIndexRouteRequest& request) {
  return request.fallback_path.empty() ? "missing" : request.fallback_path;
}

const char* FirstAuthorityClaim(
    const PolicyBlockedIndexAuthorityClaims& claims) {
  if (claims.parser) return "parser";
  if (claims.reference) return "reference";
  if (claims.provider) return "provider";
  if (claims.index) return "index";
  if (claims.security) return "security";
  if (claims.visibility) return "visibility";
  if (claims.transaction) return "transaction";
  if (claims.recovery) return "recovery";
  if (claims.log) return "log";
  return "";
}

bool HasAuthorityClaim(const PolicyBlockedIndexAuthorityClaims& claims) {
  return FirstAuthorityClaim(claims)[0] != '\0';
}

bool IsAcceptedPhysicalFamily(IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  return descriptor != nullptr &&
         descriptor->completion ==
             IndexCompletionStatus::accepted_requires_full_implementation &&
         descriptor->persistence != IndexPersistenceClass::policy_blocked;
}

std::vector<std::string> BaseEvidence(
    const PolicyBlockedIndexRouteRequest& request) {
  const auto family = FamilyId(request.requested_family);
  const auto profile = ProfileFor(request);
  const auto required_feature = RequiredFeatureFor(request);
  const auto required_policy = RequiredPolicyFor(request);
  const auto route = RouteFor(request);
  const auto fallback_path = FallbackPathFor(request);
  std::vector<std::string> evidence = {
      kPolicyBlockedIndexAdmissionKey,
      "policy_blocked.family=" + family,
      "policy_blocked.profile=" + profile,
      "policy_blocked.required_feature=" + required_feature,
      "policy_blocked.required_policy=" + required_policy,
      "policy_blocked.route=" + route,
      "policy_blocked.fallback_path=" + fallback_path,
      std::string("policy_blocked.fallback_available=") +
          BoolText(request.fallback_available),
      "policy_blocked.silent_downgrade_allowed=false",
      "policy_blocked.executable=false",
      "policy_blocked.physical=false",
      "policy_blocked.native_physical_provider=false",
      "policy_blocked.authoritative=false",
      "policy_blocked.parser_authority=false",
      "policy_blocked.reference_authority=false",
      "policy_blocked.provider_authority=false",
      "policy_blocked.index_authority=false",
      "policy_blocked.security_authority=false",
      "policy_blocked.visibility_authority=false",
      "policy_blocked.transaction_authority=false",
      "policy_blocked.recovery_authority=false",
      "policy_blocked.log_authority=false"};
  if (request.attempted_physical_family != IndexFamily::unknown) {
    evidence.push_back("policy_blocked.attempted_physical_family=" +
                       FamilyId(request.attempted_physical_family));
  }
  return evidence;
}

PolicyBlockedIndexAdmissionResult Refuse(
    const PolicyBlockedIndexRouteRequest& request,
    PolicyBlockedIndexRefusalReason reason,
    std::string diagnostic_code,
    std::string message_key,
    std::string authority_claim = {}) {
  PolicyBlockedIndexAdmissionResult result;
  result.status = RefuseStatus();
  result.applicable = true;
  result.fail_closed = true;
  result.policy_blocked = true;
  result.fallback_available = request.fallback_available;
  result.fallback_explicit =
      request.fallback_available && !request.fallback_path.empty() &&
      !request.silent_downgrade_attempted;
  result.executable = false;
  result.physical = false;
  result.native_physical_provider = false;
  result.authoritative = false;
  result.reason = reason;
  result.requested_family_id = FamilyId(request.requested_family);
  result.requested_profile = ProfileFor(request);
  result.required_feature = RequiredFeatureFor(request);
  result.required_policy = RequiredPolicyFor(request);
  result.route = RouteFor(request);
  result.fallback_path = FallbackPathFor(request);
  result.authority_claim = std::move(authority_claim);
  result.evidence = BaseEvidence(request);
  result.evidence.push_back("policy_blocked.fail_closed=true");
  result.evidence.push_back(
      std::string("policy_blocked.refusal_reason=") +
      PolicyBlockedIndexRefusalReasonName(reason));
  if (!result.authority_claim.empty()) {
    result.evidence.push_back("policy_blocked.authority_claim=" +
                              result.authority_claim);
  }
  result.refusal_reasons.push_back(
      PolicyBlockedIndexRefusalReasonName(reason));
  result.diagnostic = MakePolicyBlockedIndexDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      request, reason, result.authority_claim);
  return result;
}

}  // namespace

const char* PolicyBlockedIndexRefusalReasonName(
    PolicyBlockedIndexRefusalReason reason) {
  switch (reason) {
    case PolicyBlockedIndexRefusalReason::none:
      return "none";
    case PolicyBlockedIndexRefusalReason::policy_not_accepted:
      return "policy_not_accepted";
    case PolicyBlockedIndexRefusalReason::required_feature_missing:
      return "required_feature_missing";
    case PolicyBlockedIndexRefusalReason::fallback_missing:
      return "fallback_missing";
    case PolicyBlockedIndexRefusalReason::silent_downgrade_attempted:
      return "silent_downgrade_attempted";
    case PolicyBlockedIndexRefusalReason::
        physical_accepted_family_route_attempted:
      return "physical_accepted_family_route_attempted";
    case PolicyBlockedIndexRefusalReason::authority_claim_refused:
      return "authority_claim_refused";
    case PolicyBlockedIndexRefusalReason::
        non_executable_policy_blocked_family:
      return "non_executable_policy_blocked_family";
    case PolicyBlockedIndexRefusalReason::not_policy_blocked_request:
      return "not_policy_blocked_request";
  }
  return "none";
}

bool IsPolicyBlockedIndexRequest(
    const PolicyBlockedIndexRouteRequest& request) {
  const auto* descriptor = FindBuiltinIndexFamily(request.requested_family);
  return request.policy_blocked_profile ||
         IsPolicyBlockedIndexFamily(request.requested_family) ||
         (descriptor != nullptr &&
          descriptor->persistence == IndexPersistenceClass::policy_blocked) ||
         HasPolicyBlockedProfileText(request.requested_profile);
}

PolicyBlockedIndexRouteRequest MakePolicyBlockedIndexRouteRequest(
    IndexFamily family,
    std::string requested_profile,
    std::string route,
    std::string fallback_path,
    bool fallback_available) {
  PolicyBlockedIndexRouteRequest request;
  request.requested_family = family;
  request.requested_profile = std::move(requested_profile);
  request.route = std::move(route);
  request.fallback_path = std::move(fallback_path);
  request.fallback_available = fallback_available;
  request.policy_blocked_profile =
      IsPolicyBlockedIndexFamily(family) ||
      HasPolicyBlockedProfileText(request.requested_profile);
  const auto* descriptor = FindBuiltinIndexFamily(family);
  if (descriptor != nullptr) {
    if (request.requested_profile.empty()) {
      request.requested_profile = descriptor->id;
    }
    request.required_feature = descriptor->id;
    request.required_policy = descriptor->default_semantic_profile.empty()
                                  ? "SB_POLICY_INDEX_NOT_ACCEPTED_ALPHA"
                                  : descriptor->default_semantic_profile;
  }
  if (request.policy_blocked_profile &&
      !IsPolicyBlockedIndexFamily(request.requested_family)) {
    const auto profile = ProfileFor(request);
    request.required_feature = profile;
    request.required_policy =
        profile == "advanced_vector_policy_blocked"
            ? "SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA"
            : "SB_POLICY_INDEX_NOT_ACCEPTED_ALPHA";
  }
  request.policy_accepted = !request.policy_blocked_profile;
  request.required_feature_available = !request.policy_blocked_profile;
  return request;
}

PolicyBlockedIndexAdmissionResult EvaluatePolicyBlockedIndexAdmission(
    const PolicyBlockedIndexRouteRequest& request) {
  const bool applicable = IsPolicyBlockedIndexRequest(request);
  if (!applicable && !HasAuthorityClaim(request.authority_claims) &&
      !request.silent_downgrade_attempted &&
      !request.physical_accepted_family_route_attempted) {
    PolicyBlockedIndexAdmissionResult result;
    result.status = OkStatus();
    result.admitted = true;
    result.applicable = false;
    result.requested_family_id = FamilyId(request.requested_family);
    result.requested_profile = ProfileFor(request);
    result.required_feature = RequiredFeatureFor(request);
    result.required_policy = RequiredPolicyFor(request);
    result.route = RouteFor(request);
    result.fallback_path = FallbackPathFor(request);
    return result;
  }

  if (HasAuthorityClaim(request.authority_claims)) {
    return Refuse(
        request, PolicyBlockedIndexRefusalReason::authority_claim_refused,
        "INDEX.POLICY_BLOCKED.AUTHORITY_CLAIM_REFUSED",
        "index.policy_blocked.authority_claim_refused",
        FirstAuthorityClaim(request.authority_claims));
  }
  if (request.silent_downgrade_attempted) {
    return Refuse(
        request,
        PolicyBlockedIndexRefusalReason::silent_downgrade_attempted,
        "INDEX.POLICY_BLOCKED.SILENT_DOWNGRADE_REFUSED",
        "index.policy_blocked.silent_downgrade_refused");
  }
  if (request.physical_accepted_family_route_attempted ||
      IsAcceptedPhysicalFamily(request.attempted_physical_family)) {
    return Refuse(
        request,
        PolicyBlockedIndexRefusalReason::
            physical_accepted_family_route_attempted,
        "INDEX.POLICY_BLOCKED.PHYSICAL_ACCEPTED_ROUTE_REFUSED",
        "index.policy_blocked.physical_accepted_route_refused");
  }
  if (!request.policy_accepted) {
    return Refuse(request,
                  PolicyBlockedIndexRefusalReason::policy_not_accepted,
                  "INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
                  "index.policy_blocked.policy_not_accepted");
  }
  if (!request.required_feature_available) {
    return Refuse(request,
                  PolicyBlockedIndexRefusalReason::required_feature_missing,
                  "INDEX.POLICY_BLOCKED.REQUIRED_FEATURE_MISSING",
                  "index.policy_blocked.required_feature_missing");
  }
  if (request.fallback_path.empty() || !request.fallback_available) {
    return Refuse(request, PolicyBlockedIndexRefusalReason::fallback_missing,
                  "INDEX.POLICY_BLOCKED.FALLBACK_MISSING",
                  "index.policy_blocked.fallback_missing");
  }
  return Refuse(
      request,
      PolicyBlockedIndexRefusalReason::non_executable_policy_blocked_family,
      "INDEX.POLICY_BLOCKED.NON_EXECUTABLE",
      "index.policy_blocked.non_executable");
}

PolicyBlockedIndexMetadataProjection ProjectPolicyBlockedIndexMetadata(
    const PolicyBlockedIndexRouteRequest& request) {
  PolicyBlockedIndexMetadataProjection projection;
  if (!IsPolicyBlockedIndexRequest(request)) {
    projection.status = RefuseStatus();
    projection.management_visible = false;
    projection.requested_family_id = FamilyId(request.requested_family);
    projection.requested_profile = ProfileFor(request);
    projection.required_feature = RequiredFeatureFor(request);
    projection.required_policy = RequiredPolicyFor(request);
    projection.route = RouteFor(request);
    projection.fallback_path = FallbackPathFor(request);
    projection.evidence = BaseEvidence(request);
    projection.evidence.push_back("policy_blocked.metadata_visible=false");
    projection.diagnostic = MakePolicyBlockedIndexDiagnostic(
        projection.status,
        "INDEX.POLICY_BLOCKED.NOT_POLICY_BLOCKED_REQUEST",
        "index.policy_blocked.not_policy_blocked_request",
        request,
        PolicyBlockedIndexRefusalReason::not_policy_blocked_request);
    return projection;
  }
  if (HasAuthorityClaim(request.authority_claims)) {
    const auto refused = EvaluatePolicyBlockedIndexAdmission(request);
    projection.status = refused.status;
    projection.diagnostic = refused.diagnostic;
    projection.requested_family_id = refused.requested_family_id;
    projection.requested_profile = refused.requested_profile;
    projection.required_feature = refused.required_feature;
    projection.required_policy = refused.required_policy;
    projection.route = refused.route;
    projection.fallback_path = refused.fallback_path;
    projection.evidence = refused.evidence;
    return projection;
  }

  projection.status = OkStatus();
  projection.management_visible = true;
  projection.blocked_state_visible = true;
  projection.executable = false;
  projection.physical = false;
  projection.native_physical_provider = false;
  projection.authoritative = false;
  projection.requested_family_id = FamilyId(request.requested_family);
  projection.requested_profile = ProfileFor(request);
  projection.required_feature = RequiredFeatureFor(request);
  projection.required_policy = RequiredPolicyFor(request);
  projection.route = RouteFor(request);
  projection.fallback_path = FallbackPathFor(request);
  projection.evidence = BaseEvidence(request);
  projection.evidence.push_back("policy_blocked.metadata_visible=true");
  projection.evidence.push_back("policy_blocked.blocked_state_visible=true");
  projection.evidence.push_back("policy_blocked.management_visible=true");
  projection.evidence.push_back("policy_blocked.admission_required=true");
  projection.diagnostic = MakePolicyBlockedIndexDiagnostic(
      projection.status, "INDEX.POLICY_BLOCKED.METADATA_VISIBLE",
      "index.policy_blocked.metadata_visible", request,
      PolicyBlockedIndexRefusalReason::none);
  return projection;
}

DiagnosticRecord MakePolicyBlockedIndexDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    const PolicyBlockedIndexRouteRequest& request,
    PolicyBlockedIndexRefusalReason reason,
    std::string authority_claim) {
  std::vector<DiagnosticArgument> arguments = {
      {"reason", PolicyBlockedIndexRefusalReasonName(reason)},
      {"requested_family", FamilyId(request.requested_family)},
      {"requested_profile", ProfileFor(request)},
      {"required_feature", RequiredFeatureFor(request)},
      {"required_policy", RequiredPolicyFor(request)},
      {"route", RouteFor(request)},
      {"fallback_path", FallbackPathFor(request)},
      {"fallback_available", BoolText(request.fallback_available)},
      {"silent_downgrade_allowed", "false"},
      {"executable", "false"},
      {"physical", "false"},
      {"native_physical_provider", "false"},
      {"authoritative", "false"}};
  if (request.attempted_physical_family != IndexFamily::unknown) {
    arguments.push_back({"attempted_physical_family",
                         FamilyId(request.attempted_physical_family)});
  }
  if (!authority_claim.empty()) {
    arguments.push_back({"authority_claim", std::move(authority_claim)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {},
                        "core.index.policy_blocked_admission");
}

}  // namespace scratchbird::core::index
