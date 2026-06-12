// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "registry/function_registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::functions {

struct FunctionHardeningIssue {
  std::string function_id;
  std::string severity;
  std::string detail;
};

struct FunctionHardeningReport {
  bool ok = true;
  std::vector<FunctionHardeningIssue> issues;
};

struct FunctionCatalogExportRow {
  std::string function_id;
  std::string function_uuid;
  std::string short_name;
  std::string family;
  std::string implementation_state;
  std::string descriptor_rule;
  std::string execute_right;
  std::string visibility_right;
  std::string redaction_class;
  bool metadata_redacted = false;
};

struct FunctionVersionCompatibilityDecision {
  bool compatible = false;
  std::string action;
  std::string diagnostic_id;
};

struct GoldenReferenceBehaviorFixture {
  std::string reference_or_plugin;
  std::string alias_name;
  std::string canonical_function_id;
  std::string sample_input;
  std::string expected_shape;
  std::string diagnostic_policy;
};

struct FunctionPerformanceBudget {
  std::string budget_class;
  std::uint64_t max_input_bytes = 0;
  std::uint64_t max_output_bytes = 0;
  std::uint64_t max_steps = 0;
  std::uint64_t max_memory_bytes = 0;
  std::string regression_policy;
};

struct FunctionAuditEvidenceRequirement {
  std::string function_id;
  std::string evidence_kind;
  std::string trigger;
  bool required = false;
};

void PopulateFunctionHardeningDefaults(FunctionRegistryEntry* entry);
FunctionCatalogExportRow BuildFunctionCatalogExportRow(const FunctionRegistryEntry& entry,
                                                       bool metadata_visible);
FunctionVersionCompatibilityDecision ResolveFunctionVersionCompatibility(
    std::string stored_semantic_version,
    std::string current_semantic_version);
FunctionHardeningReport ReviewFunctionDeterminismMetadata(const FunctionRegistry& registry);
FunctionHardeningReport ValidateFunctionCrossPlatformGate(const FunctionRegistry& registry);
std::vector<GoldenReferenceBehaviorFixture> GoldenReferenceBehaviorFixtures();
std::vector<FunctionPerformanceBudget> FunctionPerformanceBudgets();
std::vector<FunctionAuditEvidenceRequirement> FunctionAuditEvidenceRequirements(
    const FunctionRegistry& registry);

}  // namespace scratchbird::engine::functions
