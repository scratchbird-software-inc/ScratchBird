// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/function_runtime.hpp"
#include "registry/function_registry.hpp"

#include <string>

namespace scratchbird::engine::functions {

enum class FunctionGateKind {
  package,
  dependency,
  security,
  policy,
};

struct FunctionGateDecision {
  bool allowed = true;
  FunctionGateKind kind = FunctionGateKind::package;
  scratchbird::engine::sblr::SblrStatusCode status = scratchbird::engine::sblr::SblrStatusCode::ok;
  std::string diagnostic_id;
  std::string detail;
};

FunctionGateDecision EvaluateFunctionPackageGate(const FunctionRegistryEntry& entry,
                                                 const FunctionCallContext& context);
FunctionGateDecision EvaluateFunctionDependencyGate(const FunctionRegistryEntry& entry,
                                                    const FunctionCallContext& context);
FunctionGateDecision EvaluateFunctionSecurityGate(const FunctionRegistryEntry& entry,
                                                  const FunctionCallContext& context);
FunctionGateDecision EvaluateFunctionPolicyGate(const FunctionRegistryEntry& entry,
                                                const FunctionCallContext& context);

}  // namespace scratchbird::engine::functions
