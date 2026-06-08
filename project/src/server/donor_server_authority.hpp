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

enum class DonorServerAuthorityAction {
  kSecurityDenial,
  kServerPolicyGate,
  kMigrationServiceRoute,
};

enum class DonorMigrationRouteKind {
  kPhysicalOperation,
  kLiveReplicationEndpoint,
};

struct DonorServerAuthorityDecision {
  std::string_view surface_key;
  DonorServerAuthorityAction action;
  std::string_view server_action;
  std::string_view diagnostic_code;
  std::string_view route_contract_id;
  std::string_view required_execution_plan_lane;
  std::string_view sblr_action;
  std::string_view mga_rule;
  bool parser_classify_only;
  bool sblr_execution_surface;
  bool preserves_scratchbird_mga_authority;
  bool accepts_donor_finality;
  bool requires_security_denial;
  bool requires_connector_authorization;
  bool requires_migration_service;
};

struct DonorServerAuthorityRequest {
  std::string_view engine_id;
  std::string_view surface_key;
  std::string_view donor_visible_surface;
  bool materialized_authorization_context = false;
  bool migration_service_available = false;
};

struct DonorServerAuthorityRouteResult {
  bool recognized = false;
  bool routed = false;
  bool accepted = false;
  bool denied = true;
  bool policy_gate = false;
  bool migration_route = false;
  bool sblr_execution_attempted = false;
  bool sblr_execution_blocked = true;
  bool scratchbird_mga_authority_preserved = true;
  bool donor_finality_accepted = false;
  DonorServerAuthorityAction action = DonorServerAuthorityAction::kSecurityDenial;
  std::string diagnostic_code;
  std::string route_contract_id;
  std::vector<std::pair<std::string, std::string>> evidence;
};

struct DonorMigrationRouteContract {
  std::string_view surface_key;
  DonorMigrationRouteKind route_kind;
  std::string_view route_contract_id;
  std::string_view route_diagnostic_code;
  std::string_view unavailable_diagnostic_code;
  std::string_view checkpoint_descriptor_kind;
  std::string_view resume_token_kind;
  std::string_view donor_mechanism_class;
  std::string_view mga_rule;
  bool scratchbird_mga_authority_preserved;
  bool donor_storage_authority_accepted;
  bool donor_finality_accepted;
  bool sblr_execution_surface;
};

struct DonorMigrationRouteRequest {
  std::string_view engine_id;
  std::string_view surface_key;
  bool migration_service_available = false;
};

struct DonorMigrationRouteResult {
  bool recognized = false;
  bool routed = false;
  bool accepted = false;
  bool service_unavailable = false;
  bool sblr_execution_attempted = false;
  bool scratchbird_mga_authority_preserved = true;
  bool donor_storage_authority_accepted = false;
  bool donor_finality_accepted = false;
  DonorMigrationRouteKind route_kind = DonorMigrationRouteKind::kPhysicalOperation;
  std::string diagnostic_code;
  std::string route_contract_id;
  std::string checkpoint_descriptor_kind;
  std::string resume_token_kind;
  std::vector<std::pair<std::string, std::string>> evidence;
};

bool IsKnownDonorEngineForServerAuthority(std::string_view engine_id);
std::optional<DonorServerAuthorityDecision> ResolveDonorServerAuthoritySurface(
    std::string_view engine_id,
    std::string_view surface_key);
DonorServerAuthorityRouteResult EvaluateDonorServerAuthorityRoute(
    const DonorServerAuthorityRequest& request);
std::optional<DonorMigrationRouteContract> ResolveDonorMigrationRouteContract(
    std::string_view engine_id,
    std::string_view surface_key);
DonorMigrationRouteResult EvaluateDonorMigrationRoute(
    const DonorMigrationRouteRequest& request);

}  // namespace scratchbird::server
