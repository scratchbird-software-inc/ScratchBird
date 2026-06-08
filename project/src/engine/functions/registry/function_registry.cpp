// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "registry/function_registry.hpp"

#include "metadata/function_hardening.hpp"
#include "metadata/function_optimizer_metadata.hpp"

#include <utility>

namespace scratchbird::engine::functions {

bool FunctionRegistry::Register(FunctionRegistryEntry entry, std::string* error) {
  if (entry.function_id.empty()) {
    if (error) *error = "function_id is required";
    return false;
  }
  if (entry.function_uuid.empty()) {
    if (error) *error = "function_uuid is required for " + entry.function_id;
    return false;
  }
  if (entry.semantic_version.empty()) entry.semantic_version = "function_semantics_v1";
  PopulateDefaultFunctionOptimizerMetadata(&entry);
  PopulateFunctionHardeningDefaults(&entry);
  entry.refusal_diagnostic = RefusalDiagnosticForState(entry.implementation_state);
  const auto [_, inserted] = entries_.emplace(entry.function_id, std::move(entry));
  if (!inserted && error) *error = "duplicate function_id";
  return inserted;
}

const FunctionRegistryEntry* FunctionRegistry::Lookup(std::string_view function_id) const {
  const auto it = entries_.find(std::string(function_id));
  return it == entries_.end() ? nullptr : &it->second;
}

std::vector<FunctionRegistryEntry> FunctionRegistry::Entries() const {
  std::vector<FunctionRegistryEntry> out;
  out.reserve(entries_.size());
  for (const auto& [_, entry] : entries_) out.push_back(entry);
  return out;
}

FunctionRegistry MakeEmptyFunctionRegistry() { return FunctionRegistry{}; }

FunctionRegistryEntry MakeRefusalOnlyFunction(std::string function_id,
                                              std::string function_uuid,
                                              std::string family,
                                              std::string short_name,
                                              FunctionImplementationState state) {
  FunctionRegistryEntry entry;
  entry.function_id = std::move(function_id);
  entry.function_uuid = std::move(function_uuid);
  entry.family = std::move(family);
  entry.short_name = std::move(short_name);
  entry.implementation_state = state;
  entry.package_state = state == FunctionImplementationState::implement_now
                            ? FunctionPackageState::core
                            : FunctionPackageState::future_or_refusal;
  entry.refusal_diagnostic = RefusalDiagnosticForState(entry.implementation_state);
  return entry;
}

bool IsFinalFunctionImplementationState(FunctionImplementationState state) {
  switch (state) {
    case FunctionImplementationState::implemented_behavior:
    case FunctionImplementationState::implemented_alias_to_canonical_behavior:
    case FunctionImplementationState::implemented_domain_emulation_behavior:
    case FunctionImplementationState::implemented_policy_security_or_dependency_runtime_refusal:
      return true;
    default:
      return false;
  }
}

bool IsForbiddenClosureState(FunctionImplementationState state) {
  switch (state) {
    case FunctionImplementationState::refuse_until_classified:
    case FunctionImplementationState::future_gated_package:
      return true;
    default:
      return false;
  }
}

std::vector<std::string> ValidateFunctionRegistryForClosure(const FunctionRegistry& registry) {
  std::vector<std::string> errors;
  for (const auto& entry : registry.Entries()) {
    if (entry.function_id.empty()) errors.push_back("function_id is required");
    if (entry.function_uuid.empty()) errors.push_back(entry.function_id + ": function_uuid is required");
    if (entry.family.empty()) errors.push_back(entry.function_id + ": family is required");
    if (entry.short_name.empty()) errors.push_back(entry.function_id + ": short_name is required");
    if (entry.owner_source.empty()) errors.push_back(entry.function_id + ": owner_source is required");
    if (entry.owner_test.empty()) errors.push_back(entry.function_id + ": owner_test is required");
    if (entry.optimizer_metadata.descriptor_rule.empty()) errors.push_back(entry.function_id + ": descriptor_rule metadata is required");
    if (entry.execute_right.empty()) errors.push_back(entry.function_id + ": execute_right is required");
    if (entry.metadata_visibility_right.empty()) errors.push_back(entry.function_id + ": metadata_visibility_right is required");
    if (IsForbiddenClosureState(entry.implementation_state)) {
      errors.push_back(entry.function_id + ": forbidden closure state " + ToString(entry.implementation_state));
    }
    if (!IsFinalFunctionImplementationState(entry.implementation_state)) {
      errors.push_back(entry.function_id + ": non-final implementation state " + ToString(entry.implementation_state));
    }
  }
  return errors;
}

}  // namespace scratchbird::engine::functions
