// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_runtime.hpp"

#include <utility>

namespace scratchbird::engine::functions {

std::string ToString(FunctionImplementationState state) {
  switch (state) {
    case FunctionImplementationState::implement_now: return "implement_now";
    case FunctionImplementationState::implemented_behavior: return "implemented_behavior";
    case FunctionImplementationState::implemented_alias_to_canonical_behavior: return "implemented_alias_to_canonical_behavior";
    case FunctionImplementationState::implemented_domain_emulation_behavior: return "implemented_domain_emulation_behavior";
    case FunctionImplementationState::implemented_policy_security_or_dependency_runtime_refusal: return "implemented_policy_security_or_dependency_runtime_refusal";
    case FunctionImplementationState::refuse_until_classified: return "refuse_until_classified";
    case FunctionImplementationState::optional_package_dependency_gated: return "optional_package_dependency_gated";
    case FunctionImplementationState::reference_compat_package: return "reference_compat_package";
    case FunctionImplementationState::udr_only: return "udr_only";
    case FunctionImplementationState::connector_agent: return "connector_agent";
    case FunctionImplementationState::future_gated_package: return "future_gated_package";
    case FunctionImplementationState::policy_blocked: return "policy_blocked";
    case FunctionImplementationState::unsupported: return "unsupported";
  }
  return "unsupported";
}

std::string ToString(FunctionPackageState state) {
  switch (state) {
    case FunctionPackageState::core: return "core";
    case FunctionPackageState::optional: return "optional";
    case FunctionPackageState::future_or_refusal: return "future_or_refusal";
  }
  return "future_or_refusal";
}

std::string RefusalDiagnosticForState(FunctionImplementationState state) {
  switch (state) {
    case FunctionImplementationState::implemented_behavior:
    case FunctionImplementationState::implemented_alias_to_canonical_behavior:
    case FunctionImplementationState::implemented_domain_emulation_behavior:
    case FunctionImplementationState::implement_now: return "";
    case FunctionImplementationState::implemented_policy_security_or_dependency_runtime_refusal:
      return "SB_DIAG_FUNCTION_RUNTIME_REFUSAL";
    case FunctionImplementationState::optional_package_dependency_gated: return "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE";
    case FunctionImplementationState::future_gated_package: return "SB_DIAG_FUNCTION_FUTURE_GATED";
    case FunctionImplementationState::policy_blocked: return "SB_DIAG_FUNCTION_POLICY_BLOCKED";
    case FunctionImplementationState::unsupported: return "SB_DIAG_FUNCTION_UNSUPPORTED";
    case FunctionImplementationState::udr_only: return "SB_DIAG_FUNCTION_UDR_UNAVAILABLE";
    case FunctionImplementationState::connector_agent: return "SB_DIAG_FUNCTION_CONNECTOR_UNAVAILABLE";
    case FunctionImplementationState::reference_compat_package: return "SB_DIAG_FUNCTION_REFERENCE_PACKAGE_UNAVAILABLE";
    case FunctionImplementationState::refuse_until_classified: return "SB_DIAG_FUNCTION_CLASSIFICATION_REQUIRED";
  }
  return "SB_DIAG_FUNCTION_UNSUPPORTED";
}

bool FunctionMayExecute(const FunctionCallContext& context) {
  const bool executable_state =
      context.implementation_state == FunctionImplementationState::implement_now ||
      context.implementation_state == FunctionImplementationState::implemented_behavior ||
      context.implementation_state == FunctionImplementationState::implemented_alias_to_canonical_behavior ||
      context.implementation_state == FunctionImplementationState::implemented_domain_emulation_behavior ||
      context.implementation_state ==
          FunctionImplementationState::implemented_policy_security_or_dependency_runtime_refusal;
  return executable_state &&
         context.security_allowed && context.policy_allowed && context.dependency_available;
}

FunctionCallResult RefuseFunctionCall(const FunctionCallRequest& request, std::string detail) {
  auto diagnostic = scratchbird::engine::sblr::MakeSblrRefusalDiagnostic(
      RefusalDiagnosticForState(request.context.implementation_state),
      request.context.sblr_context,
      std::move(detail));
  diagnostic.fields.push_back({"function_id", request.context.function_id});
  diagnostic.fields.push_back({"function_uuid", request.context.function_uuid});
  diagnostic.fields.push_back({"implementation_state", ToString(request.context.implementation_state)});
  diagnostic.fields.push_back({"package_state", ToString(request.context.package_state)});
  FunctionCallResult out;
  out.result = scratchbird::engine::sblr::MakeSblrFailure(
      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
      request.context.function_id,
      std::move(diagnostic));
  return out;
}

FunctionCallResult MakeFunctionSuccess(const FunctionCallRequest& request,
                                       std::vector<scratchbird::engine::sblr::SblrValue> values) {
  FunctionCallResult out;
  out.result = scratchbird::engine::sblr::MakeSblrSuccess(request.context.function_id);
  out.result.scalar_values = std::move(values);
  return out;
}

}  // namespace scratchbird::engine::functions
