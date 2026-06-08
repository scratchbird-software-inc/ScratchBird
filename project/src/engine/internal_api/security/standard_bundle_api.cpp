// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/standard_bundle_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "security/security_model.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_STANDARD_BUNDLE_API_BEHAVIOR
EngineSeedStandardSecurityBundlesResult EngineSeedStandardSecurityBundles(
    const EngineSeedStandardSecurityBundlesRequest& request) {
  if (!SecurityContextHasRight(request.context, "SEC_GRANT_ADMIN") &&
      !SecurityContextHasTag(request.context, "security.bootstrap")) {
    return SecurityFailure<EngineSeedStandardSecurityBundlesResult>(
        request.context,
        "security.seed_standard_bundles",
        MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "SEC_GRANT_ADMIN"));
  }
  static constexpr const char* kGroups[] = {"PUBLIC", "APP", "DEV", "ANL", "ETL", "SCH", "DBA", "AUD", "SUP", "OPS", "SEC", "ROOT"};
  static constexpr const char* kRoles[] = {"ROLE_APP_RUNTIME", "ROLE_DBA", "ROLE_SECURITY_ADMIN", "ROLE_AUDIT_READER", "ROLE_OPERATOR"};
  static constexpr const char* kPolicies[] = {"revoke_all_default", "bootstrap_handoff", "external_group_sync",
                                              "stale_security_context", "observability_control_baseline",
                                              "audit_evidence_required", "protected_material_purpose_bound",
                                              "udr_trust", "manager_admission", "cluster_security_fail_closed"};
  auto result = SecuritySuccess<EngineSeedStandardSecurityBundlesResult>(request.context, "security.seed_standard_bundles");
  for (const auto* group : kGroups) {
    EngineApiRequest record_request = request;
    record_request.localized_names.push_back({"en", "default", group, group, true});
    record_request.option_envelopes.push_back(std::string("bundle_group:") + group);
    const auto persisted = PersistApiBehaviorRecord(record_request, "security.seed_standard_bundles", "security_group", true, "active");
    if (!persisted.ok) { return SecurityFailure<EngineSeedStandardSecurityBundlesResult>(request.context, "security.seed_standard_bundles", persisted.diagnostic); }
    ++result.groups_seeded;
  }
  for (const auto* role : kRoles) {
    EngineApiRequest record_request = request;
    record_request.localized_names.push_back({"en", "default", role, role, true});
    record_request.option_envelopes.push_back(std::string("bundle_role:") + role);
    const auto persisted = PersistApiBehaviorRecord(record_request, "security.seed_standard_bundles", "security_role", true, "active");
    if (!persisted.ok) { return SecurityFailure<EngineSeedStandardSecurityBundlesResult>(request.context, "security.seed_standard_bundles", persisted.diagnostic); }
    ++result.roles_seeded;
  }
  result.policies_seeded = sizeof(kPolicies) / sizeof(kPolicies[0]);
  AddSecurityEvidence(&result, "standard_security_bundles", "seeded");
  AddSecurityRow(&result, {{"groups_seeded", std::to_string(result.groups_seeded)},
                           {"roles_seeded", std::to_string(result.roles_seeded)},
                           {"policies_seeded", std::to_string(result.policies_seeded)}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
