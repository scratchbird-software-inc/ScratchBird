// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_gate.hpp"

namespace scratchbird::engine::functions {

FunctionGateDecision EvaluateFunctionPackageGate(const FunctionRegistryEntry& entry,
                                                 const FunctionCallContext&) {
  if (entry.package_state == FunctionPackageState::core) return {};
  if (entry.implementation_state == FunctionImplementationState::policy_blocked) {
    return {false, FunctionGateKind::package, scratchbird::engine::sblr::SblrStatusCode::policy_refused,
            "SB_DIAG_FUNCTION_PACKAGE_POLICY_BLOCKED", "function package is blocked by policy"};
  }
  if (entry.package_state == FunctionPackageState::optional) {
    return {false, FunctionGateKind::package, scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
            "SB_DIAG_FUNCTION_PACKAGE_NOT_ACTIVE", "optional function package is not active"};
  }
  return {false, FunctionGateKind::package, scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
          "SB_DIAG_FUNCTION_PACKAGE_FUTURE_GATED", "function package is future-gated"};
}

FunctionGateDecision EvaluateFunctionDependencyGate(const FunctionRegistryEntry& entry,
                                                    const FunctionCallContext& context) {
  if (context.dependency_available) return {};
  return {false, FunctionGateKind::dependency, scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
          entry.refusal_diagnostic.empty() ? "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE" : entry.refusal_diagnostic,
          "function dependency is unavailable"};
}

FunctionGateDecision EvaluateFunctionSecurityGate(const FunctionRegistryEntry&,
                                                  const FunctionCallContext& context) {
  if (context.security_allowed) return {};
  return {false, FunctionGateKind::security, scratchbird::engine::sblr::SblrStatusCode::security_refused,
          "SB_DIAG_EXECUTE_FUNCTION_REFUSED", "EXECUTE_FUNCTION or function-family right is missing"};
}

FunctionGateDecision EvaluateFunctionPolicyGate(const FunctionRegistryEntry& entry,
                                                const FunctionCallContext& context) {
  if (context.policy_allowed) return {};
  return {false, FunctionGateKind::policy, scratchbird::engine::sblr::SblrStatusCode::policy_refused,
          entry.implementation_state == FunctionImplementationState::policy_blocked
              ? "SB_DIAG_FUNCTION_POLICY_BLOCKED"
              : "SB_DIAG_FUNCTION_POLICY_REFUSED",
          "function execution refused by policy"};
}

}  // namespace scratchbird::engine::functions
