// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/function_runtime.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions {

class FunctionRegistry;
struct FunctionRegistryEntry;

struct FunctionMetadataDecision {
  bool allowed = false;
  std::string reason;
};

FunctionOptimizerMetadata BuildFunctionOptimizerMetadata(std::string_view function_id,
                                                         std::string_view family,
                                                         FunctionImplementationState state);
FunctionOptimizerMetadata BuildOperatorOptimizerMetadata(std::string_view operator_id);
void PopulateDefaultFunctionOptimizerMetadata(FunctionRegistryEntry* entry);
std::optional<FunctionOptimizerMetadata> LookupFunctionOptimizerMetadata(const FunctionRegistry& registry,
                                                                         std::string_view function_id);
FunctionMetadataDecision PlannerMayFoldFunction(const FunctionOptimizerMetadata& metadata);
FunctionMetadataDecision OptimizerMayPushDownFunction(const FunctionOptimizerMetadata& metadata,
                                                      bool cluster_execution_available);
FunctionMetadataDecision LlvmMayCompileFunction(const FunctionOptimizerMetadata& metadata,
                                                bool llvm_available);
std::vector<std::string> ValidateFunctionOptimizerMetadataComplete(const FunctionRegistry& registry);

}  // namespace scratchbird::engine::functions
