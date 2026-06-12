// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "index_family_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kPolicyBlockedIndexAdmissionKey =
    "SB_POLICY_BLOCKED_INDEX_ADMISSION";

struct PolicyBlockedIndexAuthorityClaims {
  bool parser = false;
  bool reference = false;
  bool provider = false;
  bool index = false;
  bool security = false;
  bool visibility = false;
  bool transaction = false;
  bool recovery = false;
  bool log = false;
};

enum class PolicyBlockedIndexRefusalReason : u32 {
  none = 0,
  policy_not_accepted = 1,
  required_feature_missing = 2,
  fallback_missing = 3,
  silent_downgrade_attempted = 4,
  physical_accepted_family_route_attempted = 5,
  authority_claim_refused = 6,
  non_executable_policy_blocked_family = 7,
  not_policy_blocked_request = 8
};

struct PolicyBlockedIndexRouteRequest {
  IndexFamily requested_family = IndexFamily::unknown;
  std::string requested_profile;
  std::string required_feature;
  std::string required_policy;
  std::string route;
  std::string fallback_path;
  IndexFamily attempted_physical_family = IndexFamily::unknown;
  bool policy_blocked_profile = false;
  bool policy_accepted = false;
  bool required_feature_available = false;
  bool fallback_available = false;
  bool silent_downgrade_attempted = false;
  bool physical_accepted_family_route_attempted = false;
  PolicyBlockedIndexAuthorityClaims authority_claims;
};

struct PolicyBlockedIndexAdmissionResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool applicable = false;
  bool admitted = false;
  bool fail_closed = false;
  bool policy_blocked = false;
  bool fallback_available = false;
  bool fallback_explicit = false;
  bool executable = false;
  bool physical = false;
  bool native_physical_provider = false;
  bool authoritative = false;
  PolicyBlockedIndexRefusalReason reason =
      PolicyBlockedIndexRefusalReason::none;
  std::string requested_family_id;
  std::string requested_profile;
  std::string required_feature;
  std::string required_policy;
  std::string route;
  std::string fallback_path;
  std::string authority_claim;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

struct PolicyBlockedIndexMetadataProjection {
  Status status;
  DiagnosticRecord diagnostic;
  bool management_visible = false;
  bool blocked_state_visible = false;
  bool executable = false;
  bool physical = false;
  bool native_physical_provider = false;
  bool authoritative = false;
  std::string requested_family_id;
  std::string requested_profile;
  std::string required_feature;
  std::string required_policy;
  std::string route;
  std::string fallback_path;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && management_visible; }
};

const char* PolicyBlockedIndexRefusalReasonName(
    PolicyBlockedIndexRefusalReason reason);
bool IsPolicyBlockedIndexRequest(
    const PolicyBlockedIndexRouteRequest& request);
PolicyBlockedIndexRouteRequest MakePolicyBlockedIndexRouteRequest(
    IndexFamily family,
    std::string requested_profile,
    std::string route,
    std::string fallback_path,
    bool fallback_available);
PolicyBlockedIndexAdmissionResult EvaluatePolicyBlockedIndexAdmission(
    const PolicyBlockedIndexRouteRequest& request);
PolicyBlockedIndexMetadataProjection ProjectPolicyBlockedIndexMetadata(
    const PolicyBlockedIndexRouteRequest& request);
DiagnosticRecord MakePolicyBlockedIndexDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    const PolicyBlockedIndexRouteRequest& request,
    PolicyBlockedIndexRefusalReason reason,
    std::string authority_claim = {});

}  // namespace scratchbird::core::index
