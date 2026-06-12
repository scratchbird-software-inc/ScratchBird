// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr/sblr_runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::functions {

enum class FunctionImplementationState {
  implement_now,
  implemented_behavior,
  implemented_alias_to_canonical_behavior,
  implemented_domain_emulation_behavior,
  implemented_policy_security_or_dependency_runtime_refusal,
  refuse_until_classified,
  optional_package_dependency_gated,
  reference_compat_package,
  udr_only,
  connector_agent,
  future_gated_package,
  policy_blocked,
  unsupported,
};

enum class FunctionPackageState {
  core,
  optional,
  future_or_refusal,
};

enum class FunctionDeterminism {
  deterministic,
  deterministic_given_catalog_snapshot,
  stable_transaction,
  stable_statement,
  volatile_value,
  side_effecting,
};

enum class FunctionCostClass {
  trivial,
  scalar_cpu_light,
  scalar_cpu_heavy,
  memory_heavy,
  io_or_catalog,
  aggregate_stateful,
  specialized_index,
};

enum class FunctionNullPolicy {
  null_in_null_out,
  ignores_nulls,
  nulls_participate,
  custom,
};

enum class FunctionIndexability {
  not_indexable,
  expression_indexable,
  predicate_indexable,
  specialized_index_required,
};

enum class FunctionPushdownEligibility {
  local_only,
  remote_safe,
  remote_safe_with_policy,
  forbidden,
};

enum class FunctionLlvmEligibility {
  interpreter_only,
  llvm_safe,
  llvm_required_when_enabled,
  forbidden,
};

struct FunctionOptimizerMetadata {
  FunctionDeterminism determinism = FunctionDeterminism::volatile_value;
  FunctionCostClass cost_class = FunctionCostClass::scalar_cpu_light;
  FunctionNullPolicy null_policy = FunctionNullPolicy::custom;
  FunctionIndexability indexability = FunctionIndexability::not_indexable;
  FunctionPushdownEligibility pushdown = FunctionPushdownEligibility::local_only;
  FunctionLlvmEligibility llvm = FunctionLlvmEligibility::interpreter_only;
  std::string descriptor_rule;
  std::string collation_charset_timezone_rule;
  std::string resource_budget_class;
  bool security_sensitive = false;
  bool audit_required = false;
};

struct FunctionResourceLimits {
  std::uint64_t max_input_bytes = 0;
  std::uint64_t max_output_bytes = 0;
  std::uint64_t max_steps = 0;
  std::uint64_t max_memory_bytes = 0;
  std::uint32_t max_recursion_depth = 0;
};

struct FunctionArgument {
  std::string name;
  scratchbird::engine::sblr::SblrValue value;
};

struct FunctionCallContext {
  scratchbird::engine::sblr::SblrExecutionContext sblr_context;
  std::string function_id;
  std::string function_uuid;
  std::string package_name;
  FunctionImplementationState implementation_state = FunctionImplementationState::refuse_until_classified;
  FunctionPackageState package_state = FunctionPackageState::future_or_refusal;
  bool security_allowed = false;
  bool policy_allowed = false;
  bool dependency_available = true;
};

struct FunctionCallRequest {
  FunctionCallContext context;
  std::vector<FunctionArgument> arguments;
};

struct FunctionCallResult {
  scratchbird::engine::sblr::SblrResult result;
};

std::string ToString(FunctionImplementationState state);
std::string ToString(FunctionPackageState state);
std::string RefusalDiagnosticForState(FunctionImplementationState state);
bool FunctionMayExecute(const FunctionCallContext& context);
FunctionCallResult RefuseFunctionCall(const FunctionCallRequest& request, std::string detail = {});
FunctionCallResult MakeFunctionSuccess(const FunctionCallRequest& request,
                                       std::vector<scratchbird::engine::sblr::SblrValue> values = {});

}  // namespace scratchbird::engine::functions
