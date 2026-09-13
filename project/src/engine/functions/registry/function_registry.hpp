// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/function_runtime.hpp"
#include "uuid.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scratchbird::engine::functions {

static_assert(sizeof(FunctionUuid) == 16);

struct FunctionRegistryEntry {
  std::string function_id;
  FunctionUuid function_uuid;
  std::string family;
  std::string short_name;
  FunctionImplementationState implementation_state = FunctionImplementationState::refuse_until_classified;
  FunctionPackageState package_state = FunctionPackageState::future_or_refusal;
  std::string owner_source;
  std::string owner_header;
  std::string owner_test;
  std::string refusal_diagnostic;
  FunctionOptimizerMetadata optimizer_metadata;
  FunctionResourceLimits resource_limits;
  std::string canonical_target_id;
  std::string semantic_equivalence_class;
  std::string semantic_version;
  std::string execute_right;
  std::string metadata_visibility_right;
  std::string redaction_class;
  std::string dependency_gate;
  bool generated_row = false;
  bool catalog_visible = true;
};

class FunctionRegistry {
 public:
  // Registration is builder-owned, before publication to readers. A rejected
  // registration or allocation exception preserves both lookup routes; after
  // publication, concurrent immutable lookups borrow stable entry addresses.
  bool Register(FunctionRegistryEntry entry, std::string* error = nullptr);
  [[nodiscard]] const FunctionRegistryEntry* Lookup(std::string_view function_id) const;
  [[nodiscard]] const FunctionRegistryEntry* LookupByUuid(
      const FunctionUuid& function_uuid) const;
  // Binds only the supplied binary identity. Copies engine-owned dispatch
  // metadata atomically; failure never grants gates or changes the context.
  [[nodiscard]] const FunctionRegistryEntry* BindCallContext(FunctionCallContext& context) const;
  [[nodiscard]] std::vector<FunctionRegistryEntry> Entries() const;
  [[nodiscard]] bool empty() const { return entries_by_uuid_.empty(); }

 private:
  std::map<FunctionUuid, FunctionRegistryEntry> entries_by_uuid_;
  // Internal symbol projection only; executable lookup uses the binary key.
  std::unordered_map<std::string, FunctionUuid> uuid_by_function_id_;
};

FunctionRegistry MakeEmptyFunctionRegistry();
FunctionRegistryEntry MakeRefusalOnlyFunction(std::string function_id,
                                              FunctionUuid function_uuid,
                                              std::string family,
                                              std::string short_name,
                                              FunctionImplementationState state);
bool IsFinalFunctionImplementationState(FunctionImplementationState state);
bool IsForbiddenClosureState(FunctionImplementationState state);
std::vector<std::string> ValidateFunctionRegistryForClosure(const FunctionRegistry& registry);

}  // namespace scratchbird::engine::functions
