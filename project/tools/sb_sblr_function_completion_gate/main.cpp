// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "generated/function_completion_manifest.hpp"
#include "metadata/function_hardening.hpp"
#include "metadata/function_optimizer_metadata.hpp"
#include "registry/function_seed_registry.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fn = scratchbird::engine::functions;
namespace gen = scratchbird::engine::functions::generated;

namespace {

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}

void Expect(bool condition, std::string message, std::vector<std::string>* errors) {
  if (!condition) errors->push_back(std::move(message));
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  Expect(!gen::AuthoritativeFunctionInputs().empty(), "authoritative function inputs are empty", &errors);
  Expect(gen::IsClosureAllowedState("implemented_behavior"), "implemented_behavior must be closure-allowed", &errors);
  Expect(!gen::IsClosureAllowedState("stub_refuse_until_classified"), "stub_refuse_until_classified must not be closure-allowed", &errors);
  Expect(!gen::IsClosureAllowedState("future_gated_package"), "future_gated_package must not be closure-allowed", &errors);
  Expect(gen::IsRuntimeRefusalOnlyState("implemented_policy_security_or_dependency_runtime_refusal"),
         "runtime refusal state must be recognized", &errors);
  const auto seeds = fn::BuildStandardFunctionSeedPackage();
  Expect(!seeds.registry.empty(), "standard function seed registry is empty", &errors);
  Expect(!seeds.name_rows.empty(), "standard function name rows are empty", &errors);
  const auto closure_errors = fn::ValidateFunctionRegistryForClosure(seeds.registry);
  for (const auto& error : closure_errors) errors.push_back("closure:" + error);
  const auto metadata_errors = fn::ValidateFunctionOptimizerMetadataComplete(seeds.registry);
  for (const auto& error : metadata_errors) errors.push_back("metadata:" + error);
  const auto determinism = fn::ReviewFunctionDeterminismMetadata(seeds.registry);
  for (const auto& issue : determinism.issues) {
    errors.push_back("determinism:" + issue.function_id + ":" + issue.detail);
  }
  const auto cross_platform = fn::ValidateFunctionCrossPlatformGate(seeds.registry);
  for (const auto& issue : cross_platform.issues) {
    errors.push_back("cross_platform:" + issue.function_id + ":" + issue.detail);
  }
  for (const auto& row : seeds.name_rows) {
    bool matched = false;
    for (const auto& entry : seeds.registry.Entries()) {
      if (entry.function_uuid == row.function_uuid) {
        matched = true;
        break;
      }
    }
    Expect(matched, "name row has no matching registry UUID: " + row.function_uuid, &errors);
  }

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"authoritative_inputs\": " << gen::AuthoritativeFunctionInputs().size() << ",\n";
  std::cout << "  \"seed_functions\": " << seeds.registry.Entries().size() << ",\n";
  std::cout << "  \"checksum\": \"" << JsonEscape(gen::FunctionCompletionGeneratedRegistrySourceChecksum()) << "\",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
