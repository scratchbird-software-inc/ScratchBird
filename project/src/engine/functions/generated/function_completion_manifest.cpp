// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "generated/function_completion_manifest.hpp"

namespace scratchbird::engine::functions::generated {

const std::vector<AuthoritativeFunctionInput>& AuthoritativeFunctionInputs() {
  static const std::vector<AuthoritativeFunctionInput> inputs = {
      {"canonical_function", "public_input_snapshot", "authoritative_for_this_execution_plan"},
      {"reference_alias", "public_input_snapshot", "authoritative_for_this_execution_plan"},
      {"plugin_alias", "public_input_snapshot", "authoritative_for_this_execution_plan"},
      {"operator", "public_input_snapshot", "authoritative_for_this_execution_plan"},
      {"context_variable", "public_input_snapshot", "authoritative_for_this_execution_plan"},
      {"special_form", "public_input_snapshot", "authoritative_for_this_execution_plan"},
  };
  return inputs;
}

const std::vector<FunctionCompletionPolicyRow>& FunctionCompletionPolicyRows() {
  static const std::vector<FunctionCompletionPolicyRow> rows = {
      {"implemented_behavior", true, true, false},
      {"implemented_alias_to_canonical_behavior", true, true, false},
      {"implemented_domain_emulation_behavior", true, true, false},
      {"implemented_policy_security_or_dependency_runtime_refusal", true, false, true},
      {"implement_now", false, true, false},
      {"refuse_until_classified", false, false, false},
      {"optional_package_dependency_gated", false, false, true},
      {"reference_compat_package", false, false, true},
      {"udr_only", false, false, true},
      {"connector_agent", false, false, true},
      {"future_gated_package", false, false, false},
      {"policy_blocked", false, false, true},
      {"unsupported", false, false, false},
  };
  return rows;
}

bool IsClosureAllowedState(std::string_view state_name) {
  for (const auto& row : FunctionCompletionPolicyRows()) {
    if (row.state_name == state_name) return row.closure_allowed;
  }
  return false;
}

bool IsRuntimeRefusalOnlyState(std::string_view state_name) {
  for (const auto& row : FunctionCompletionPolicyRows()) {
    if (row.state_name == state_name) return row.runtime_refusal_only;
  }
  return false;
}

std::string FunctionCompletionGeneratedRegistrySourceChecksum() {
  return "sffc-pre001-manual-freeze-v1";
}

}  // namespace scratchbird::engine::functions::generated
