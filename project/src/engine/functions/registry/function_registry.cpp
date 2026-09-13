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
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(entry.function_uuid)) {
    if (error) *error = "function_uuid must be a binary system UUIDv7 for " + entry.function_id;
    return false;
  }
  if (entries_by_uuid_.contains(entry.function_uuid)) {
    if (error) *error = "duplicate function_uuid";
    return false;
  }
  if (uuid_by_function_id_.contains(entry.function_id)) {
    if (error) *error = "duplicate function_id";
    return false;
  }
  if (entry.semantic_version.empty()) entry.semantic_version = "function_semantics_v1";
  PopulateDefaultFunctionOptimizerMetadata(&entry);
  PopulateFunctionHardeningDefaults(&entry);
  entry.refusal_diagnostic = RefusalDiagnosticForState(entry.implementation_state);
  const auto function_id = entry.function_id;
  const auto function_uuid = entry.function_uuid;
  const auto [position, inserted] = entries_by_uuid_.emplace(function_uuid, std::move(entry));
  if (!inserted && error) *error = "duplicate function_uuid";
  if (inserted) {
    try {
      uuid_by_function_id_.emplace(function_id, function_uuid);
    } catch (...) {
      // Both lookup routes publish together; a failed secondary allocation
      // must not leave a partially registered function behind.
      entries_by_uuid_.erase(position);
      throw;
    }
  }
  return inserted;
}

const FunctionRegistryEntry* FunctionRegistry::Lookup(std::string_view function_id) const {
  const auto it = uuid_by_function_id_.find(std::string(function_id));
  return it == uuid_by_function_id_.end() ? nullptr : LookupByUuid(it->second);
}

const FunctionRegistryEntry* FunctionRegistry::LookupByUuid(
    const FunctionUuid& function_uuid) const {
  const auto entry = entries_by_uuid_.find(function_uuid);
  return entry == entries_by_uuid_.end() ? nullptr : &entry->second;
}

std::vector<FunctionRegistryEntry> FunctionRegistry::Entries() const {
  std::vector<FunctionRegistryEntry> out;
  out.reserve(entries_by_uuid_.size());
  for (const auto& [_, entry] : entries_by_uuid_) out.push_back(entry);
  return out;
}

const FunctionRegistryEntry* FunctionRegistry::BindCallContext(FunctionCallContext& context) const {
  const auto* entry = LookupByUuid(context.function_uuid);
  if (entry == nullptr) return nullptr;
  auto symbol = entry->function_id;
  auto package = entry->family;
  // Complete all fallible copies before publishing either metadata field.
  context.function_id.swap(symbol);
  context.package_name.swap(package);
  context.implementation_state = entry->implementation_state;
  context.package_state = entry->package_state;
  return entry;
}

FunctionRegistry MakeEmptyFunctionRegistry() { return FunctionRegistry{}; }

FunctionRegistryEntry MakeRefusalOnlyFunction(std::string function_id,
                                              FunctionUuid function_uuid,
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
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(entry.function_uuid))
      errors.push_back(entry.function_id + ": binary system UUIDv7 is required");
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
