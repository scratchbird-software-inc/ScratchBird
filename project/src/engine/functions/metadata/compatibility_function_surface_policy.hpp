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

namespace scratchbird::engine::functions {

enum class CompatibilityFunctionSurfaceDecision {
  kCatalogProjectionOnly,
  kConnectorOperation,
  kPolicyBlocked,
  kTrustedUdrRegistration,
  kUnsupported,
};

struct CompatibilityFunctionSurfacePolicy {
  CompatibilityFunctionSurfaceDecision decision;
  std::string_view implementation_decision;
  std::string_view required_execution_plan_lane;
  std::string_view diagnostic_code;
  std::string_view route_contract_id;
  std::string_view result_shape;
  bool accepted_without_external_authority;
  bool requires_catalog_projection;
  bool requires_connector_authorization;
  bool requires_trusted_udr_policy;
  bool always_denied;
  bool unsupported_refusal;
  bool parser_shortcut_allowed;
  bool external_execution_authority_accepted;
  bool sblr_execution_authority;
};

struct CompatibilityFunctionSurfaceRequest {
  std::string_view engine_id;
  std::string_view inventory_id;
  std::string_view item_name;
  std::string_view implementation_decision;
  std::string_view capability_family;
  std::string_view sb_normalized_target;
  bool connector_authorized = false;
  bool trusted_udr_policy_available = false;
};

struct CompatibilityFunctionSurfaceResult {
  bool recognized = false;
  bool accepted = false;
  bool denied = true;
  bool parser_shortcut_used = false;
  bool external_execution_authority_accepted = false;
  bool sblr_execution_authority = false;
  bool catalog_projection = false;
  bool connector_route = false;
  bool trusted_udr_registration_route = false;
  bool unsupported_refusal = false;
  CompatibilityFunctionSurfaceDecision decision = CompatibilityFunctionSurfaceDecision::kUnsupported;
  std::string diagnostic_code;
  std::string route_contract_id;
  std::string result_shape;
  std::vector<std::pair<std::string, std::string>> evidence;
};

std::optional<CompatibilityFunctionSurfacePolicy> ResolveCompatibilityFunctionSurfacePolicy(
    std::string_view implementation_decision);
CompatibilityFunctionSurfaceResult EvaluateCompatibilityFunctionSurface(
    const CompatibilityFunctionSurfaceRequest& request);

}  // namespace scratchbird::engine::functions
