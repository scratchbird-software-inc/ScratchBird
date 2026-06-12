// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::server {

enum class CompatibilityServerAuthorityAction {
  kSecurityDenial,
  kServerPolicyGate,
  kMigrationServiceRoute,
};

enum class CompatibilityMigrationRouteKind {
  kPhysicalOperation,
  kLiveReplicationEndpoint,
};

struct CompatibilityServerAuthorityDecision {
  std::string_view surface_key;
  CompatibilityServerAuthorityAction action;
  std::string_view server_action;
  std::string_view diagnostic_code;
  std::string_view route_contract_id;
  std::string_view required_execution_plan_lane;
  std::string_view sblr_action;
  std::string_view mga_rule;
  bool parser_classify_only;
  bool sblr_execution_surface;
  bool preserves_scratchbird_mga_authority;
  bool accepts_external_finality;
  bool requires_security_denial;
  bool requires_connector_authorization;
  bool requires_migration_service;
};

struct CompatibilityServerAuthorityRequest {
  std::string_view engine_id;
  std::string_view surface_key;
  std::string_view external_visible_surface;
  bool materialized_authorization_context = false;
  bool migration_service_available = false;
};

struct CompatibilityServerAuthorityRouteResult {
  bool recognized = false;
  bool routed = false;
  bool accepted = false;
  bool denied = true;
  bool policy_gate = false;
  bool migration_route = false;
  bool sblr_execution_attempted = false;
  bool sblr_execution_blocked = true;
  bool scratchbird_mga_authority_preserved = true;
  bool external_finality_accepted = false;
  CompatibilityServerAuthorityAction action = CompatibilityServerAuthorityAction::kSecurityDenial;
  std::string diagnostic_code;
  std::string route_contract_id;
  std::vector<std::pair<std::string, std::string>> evidence;
};

struct CompatibilityMigrationRouteContract {
  std::string_view surface_key;
  CompatibilityMigrationRouteKind route_kind;
  std::string_view route_contract_id;
  std::string_view route_diagnostic_code;
  std::string_view unavailable_diagnostic_code;
  std::string_view checkpoint_descriptor_kind;
  std::string_view resume_token_kind;
  std::string_view external_mechanism_class;
  std::string_view mga_rule;
  bool scratchbird_mga_authority_preserved;
  bool external_storage_authority_accepted;
  bool external_finality_accepted;
  bool sblr_execution_surface;
};

struct CompatibilityMigrationRouteRequest {
  std::string_view engine_id;
  std::string_view surface_key;
  bool migration_service_available = false;
};

struct CompatibilityMigrationRouteResult {
  bool recognized = false;
  bool routed = false;
  bool accepted = false;
  bool service_unavailable = false;
  bool sblr_execution_attempted = false;
  bool scratchbird_mga_authority_preserved = true;
  bool external_storage_authority_accepted = false;
  bool external_finality_accepted = false;
  CompatibilityMigrationRouteKind route_kind = CompatibilityMigrationRouteKind::kPhysicalOperation;
  std::string diagnostic_code;
  std::string route_contract_id;
  std::string checkpoint_descriptor_kind;
  std::string resume_token_kind;
  std::vector<std::pair<std::string, std::string>> evidence;
};

bool IsKnownCompatibilityEngineForServerAuthority(std::string_view engine_id);
std::optional<CompatibilityServerAuthorityDecision> ResolveCompatibilityServerAuthoritySurface(
    std::string_view engine_id,
    std::string_view surface_key);
CompatibilityServerAuthorityRouteResult EvaluateCompatibilityServerAuthorityRoute(
    const CompatibilityServerAuthorityRequest& request);
std::optional<CompatibilityMigrationRouteContract> ResolveCompatibilityMigrationRouteContract(
    std::string_view engine_id,
    std::string_view surface_key);
CompatibilityMigrationRouteResult EvaluateCompatibilityMigrationRoute(
    const CompatibilityMigrationRouteRequest& request);

}  // namespace scratchbird::server
