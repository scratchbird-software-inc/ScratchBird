// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metadata/compatibility_function_surface_policy.hpp"

namespace scratchbird::engine::functions {
namespace {

void AddEvidence(CompatibilityFunctionSurfaceResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.emplace_back(std::move(key), std::move(value));
}

std::optional<CompatibilityFunctionSurfaceDecision> ParseDecision(std::string_view decision) {
  if (decision == "catalog_projection_only") {
    return CompatibilityFunctionSurfaceDecision::kCatalogProjectionOnly;
  }
  if (decision == "connector_operation") {
    return CompatibilityFunctionSurfaceDecision::kConnectorOperation;
  }
  if (decision == "policy_blocked") {
    return CompatibilityFunctionSurfaceDecision::kPolicyBlocked;
  }
  if (decision == "trusted_udr_registration") {
    return CompatibilityFunctionSurfaceDecision::kTrustedUdrRegistration;
  }
  if (decision == "unsupported") {
    return CompatibilityFunctionSurfaceDecision::kUnsupported;
  }
  return std::nullopt;
}

std::string_view DecisionName(CompatibilityFunctionSurfaceDecision decision) {
  switch (decision) {
    case CompatibilityFunctionSurfaceDecision::kCatalogProjectionOnly:
      return "catalog_projection_only";
    case CompatibilityFunctionSurfaceDecision::kConnectorOperation:
      return "connector_operation";
    case CompatibilityFunctionSurfaceDecision::kPolicyBlocked:
      return "policy_blocked";
    case CompatibilityFunctionSurfaceDecision::kTrustedUdrRegistration:
      return "trusted_udr_registration";
    case CompatibilityFunctionSurfaceDecision::kUnsupported:
      return "unsupported";
  }
  return "unsupported";
}

std::string_view Execution_PlanLane(CompatibilityFunctionSurfaceDecision decision) {
  switch (decision) {
    case CompatibilityFunctionSurfaceDecision::kCatalogProjectionOnly:
      return "catalog_projection_and_seed_rowsets";
    case CompatibilityFunctionSurfaceDecision::kConnectorOperation:
      return "connector_external_operation_route";
    case CompatibilityFunctionSurfaceDecision::kPolicyBlocked:
      return "server_policy_and_refusal_vectors";
    case CompatibilityFunctionSurfaceDecision::kTrustedUdrRegistration:
      return "trusted_udr_package_registration";
    case CompatibilityFunctionSurfaceDecision::kUnsupported:
      return "exact_unsupported_refusal";
  }
  return "exact_unsupported_refusal";
}

std::string_view DiagnosticCode(CompatibilityFunctionSurfaceDecision decision) {
  switch (decision) {
    case CompatibilityFunctionSurfaceDecision::kCatalogProjectionOnly:
      return "SB.COMPATIBILITY_FUNCTION.CATALOG_PROJECTION_READY";
    case CompatibilityFunctionSurfaceDecision::kConnectorOperation:
      return "SB.COMPATIBILITY_FUNCTION.CONNECTOR_AUTHORIZATION_REQUIRED";
    case CompatibilityFunctionSurfaceDecision::kPolicyBlocked:
      return "SB.COMPATIBILITY_FUNCTION.POLICY_BLOCKED";
    case CompatibilityFunctionSurfaceDecision::kTrustedUdrRegistration:
      return "SB.COMPATIBILITY_FUNCTION.TRUSTED_UDR_POLICY_REQUIRED";
    case CompatibilityFunctionSurfaceDecision::kUnsupported:
      return "SB.COMPATIBILITY_FUNCTION.UNSUPPORTED";
  }
  return "SB.COMPATIBILITY_FUNCTION.UNSUPPORTED";
}

std::string_view RouteContractId(CompatibilityFunctionSurfaceDecision decision) {
  switch (decision) {
    case CompatibilityFunctionSurfaceDecision::kCatalogProjectionOnly:
      return "compatibility_function.catalog_projection.seed_rowset.v1";
    case CompatibilityFunctionSurfaceDecision::kConnectorOperation:
      return "compatibility_function.connector.external_operation.v1";
    case CompatibilityFunctionSurfaceDecision::kPolicyBlocked:
      return "compatibility_function.policy_blocked.refusal.v1";
    case CompatibilityFunctionSurfaceDecision::kTrustedUdrRegistration:
      return "compatibility_function.trusted_udr.registration_policy.v1";
    case CompatibilityFunctionSurfaceDecision::kUnsupported:
      return "compatibility_function.unsupported.exact_refusal.v1";
  }
  return "compatibility_function.unsupported.exact_refusal.v1";
}

std::string_view ResultShape(CompatibilityFunctionSurfaceDecision decision) {
  switch (decision) {
    case CompatibilityFunctionSurfaceDecision::kCatalogProjectionOnly:
      return "compatibility.function.catalog_projection.v1";
    case CompatibilityFunctionSurfaceDecision::kConnectorOperation:
      return "compatibility.function.connector_route.v1";
    case CompatibilityFunctionSurfaceDecision::kPolicyBlocked:
      return "compatibility.function.policy_refusal.v1";
    case CompatibilityFunctionSurfaceDecision::kTrustedUdrRegistration:
      return "compatibility.function.trusted_udr_registration.v1";
    case CompatibilityFunctionSurfaceDecision::kUnsupported:
      return "compatibility.function.unsupported_refusal.v1";
  }
  return "compatibility.function.unsupported_refusal.v1";
}

}  // namespace

std::optional<CompatibilityFunctionSurfacePolicy> ResolveCompatibilityFunctionSurfacePolicy(
    std::string_view implementation_decision) {
  const auto decision = ParseDecision(implementation_decision);
  if (!decision.has_value()) return std::nullopt;
  const bool catalog = *decision == CompatibilityFunctionSurfaceDecision::kCatalogProjectionOnly;
  const bool connector = *decision == CompatibilityFunctionSurfaceDecision::kConnectorOperation;
  const bool trusted_udr =
      *decision == CompatibilityFunctionSurfaceDecision::kTrustedUdrRegistration;
  const bool unsupported = *decision == CompatibilityFunctionSurfaceDecision::kUnsupported;
  const bool policy_blocked = *decision == CompatibilityFunctionSurfaceDecision::kPolicyBlocked;
  return CompatibilityFunctionSurfacePolicy{
      *decision,
      DecisionName(*decision),
      Execution_PlanLane(*decision),
      DiagnosticCode(*decision),
      RouteContractId(*decision),
      ResultShape(*decision),
      catalog,
      catalog,
      connector,
      trusted_udr,
      policy_blocked || unsupported,
      unsupported,
      false,
      false,
      false,
  };
}

CompatibilityFunctionSurfaceResult EvaluateCompatibilityFunctionSurface(
    const CompatibilityFunctionSurfaceRequest& request) {
  CompatibilityFunctionSurfaceResult result;
  const auto policy =
      ResolveCompatibilityFunctionSurfacePolicy(request.implementation_decision);
  if (!policy.has_value()) {
    result.recognized = false;
    result.accepted = false;
    result.denied = true;
    result.diagnostic_code = "SB.COMPATIBILITY_FUNCTION.UNKNOWN_SURFACE";
    result.route_contract_id = "compatibility_function.unknown.fail_closed.v1";
    result.result_shape = "compatibility.function.unknown_refusal.v1";
    AddEvidence(&result, "compatibility_function_policy_known", "false");
    AddEvidence(&result, "parser_shortcut_used", "false");
    AddEvidence(&result, "external_execution_authority_accepted", "false");
    AddEvidence(&result, "sblr_execution_authority", "false");
    return result;
  }

  result.recognized = true;
  result.decision = policy->decision;
  result.parser_shortcut_used = false;
  result.external_execution_authority_accepted =
      policy->external_execution_authority_accepted;
  result.sblr_execution_authority = policy->sblr_execution_authority;
  result.catalog_projection = policy->requires_catalog_projection;
  result.connector_route = policy->requires_connector_authorization;
  result.trusted_udr_registration_route = policy->requires_trusted_udr_policy;
  result.unsupported_refusal = policy->unsupported_refusal;
  result.diagnostic_code = std::string(policy->diagnostic_code);
  result.route_contract_id = std::string(policy->route_contract_id);
  result.result_shape = std::string(policy->result_shape);

  if (policy->requires_catalog_projection) {
    result.accepted = true;
    result.denied = false;
  } else if (policy->requires_connector_authorization) {
    result.accepted = request.connector_authorized;
    result.denied = !request.connector_authorized;
  } else if (policy->requires_trusted_udr_policy) {
    result.accepted = request.trusted_udr_policy_available;
    result.denied = !request.trusted_udr_policy_available;
  } else {
    result.accepted = false;
    result.denied = true;
  }

  AddEvidence(&result, "compatibility_function_policy_known", "true");
  AddEvidence(&result, "engine_id", std::string(request.engine_id));
  AddEvidence(&result, "inventory_id", std::string(request.inventory_id));
  AddEvidence(&result, "item_name", std::string(request.item_name));
  AddEvidence(&result,
              "implementation_decision",
              std::string(policy->implementation_decision));
  AddEvidence(&result, "required_execution_plan_lane", std::string(policy->required_execution_plan_lane));
  AddEvidence(&result, "capability_family", std::string(request.capability_family));
  AddEvidence(&result, "sb_normalized_target", std::string(request.sb_normalized_target));
  AddEvidence(&result, "route_contract_id", result.route_contract_id);
  AddEvidence(&result, "diagnostic_code", result.diagnostic_code);
  AddEvidence(&result, "parser_shortcut_used", "false");
  AddEvidence(&result, "external_execution_authority_accepted", "false");
  AddEvidence(&result, "sblr_execution_authority", "false");
  return result;
}

}  // namespace scratchbird::engine::functions
