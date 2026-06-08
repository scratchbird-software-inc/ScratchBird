// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/function_runtime.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions::generated {

struct AuthoritativeFunctionInput {
  std::string input_kind;
  std::string authoritative_path;
  std::string status;
};

struct FunctionCompletionPolicyRow {
  std::string state_name;
  bool closure_allowed = false;
  bool executable_behavior = false;
  bool runtime_refusal_only = false;
};

const std::vector<AuthoritativeFunctionInput>& AuthoritativeFunctionInputs();
const std::vector<FunctionCompletionPolicyRow>& FunctionCompletionPolicyRows();
bool IsClosureAllowedState(std::string_view state_name);
bool IsRuntimeRefusalOnlyState(std::string_view state_name);
std::string FunctionCompletionGeneratedRegistrySourceChecksum();

}  // namespace scratchbird::engine::functions::generated
