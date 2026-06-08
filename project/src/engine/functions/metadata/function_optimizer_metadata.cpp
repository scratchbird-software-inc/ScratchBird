// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metadata/function_optimizer_metadata.hpp"

#include "registry/function_registry.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::functions {
namespace {

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool IsVolatileId(std::string_view id) {
  return Contains(id, "random") || Contains(id, "uuid_generate") || Contains(id, "now") ||
         Contains(id, "current_timestamp") || Contains(id, "sequence") || Contains(id, "identity") ||
         id == "nextval" || id == "currval" || id == "gen_id";
}

bool IsSecuritySensitiveId(std::string_view id, std::string_view family) {
  return StartsWith(family, "management") || StartsWith(family, "metrics") ||
         StartsWith(family, "extension") || Contains(id, "domain") ||
         Contains(id, "current_user") || Contains(id, "current_role") ||
         Contains(id, "security") || Contains(id, "policy");
}

bool IsSpecializedFamily(std::string_view family) {
  return StartsWith(family, "nosql.") || family == "search" || family == "spatial" ||
         family == "timeseries" || family == "vector";
}

std::string DescriptorRuleFor(std::string_view id, std::string_view family) {
  if (Contains(id, "descriptor_accepts")) return "boolean descriptor compatibility";
  if (StartsWith(family, "data.aggregate")) return "aggregate descriptor from input and aggregate kind";
  if (StartsWith(family, "data.window")) return "window descriptor from function kind and frame input";
  if (StartsWith(family, "nosql.document")) return "json/document descriptor";
  if (StartsWith(family, "nosql.kv")) return "kv descriptor";
  if (StartsWith(family, "nosql.graph")) return "graph descriptor";
  if (family == "search") return "search descriptor";
  if (family == "spatial") return "spatial descriptor";
  if (family == "timeseries") return "time-series descriptor";
  if (family == "vector") return "vector descriptor";
  if (Contains(id, "sequence") || Contains(id, "identity") || id == "nextval" || id == "gen_id") {
    return "sequence descriptor";
  }
  if (Contains(id, "domain")) return "domain descriptor policy";
  return "scalar descriptor from canonical function signature";
}

FunctionCostClass CostClassFor(std::string_view id, std::string_view family) {
  if (StartsWith(family, "data.aggregate")) return FunctionCostClass::aggregate_stateful;
  if (StartsWith(family, "data.window")) return FunctionCostClass::aggregate_stateful;
  if (family == "vector" || family == "search" || family == "spatial") return FunctionCostClass::specialized_index;
  if (family == "nosql.document" || family == "nosql.graph" || family == "timeseries") {
    return FunctionCostClass::memory_heavy;
  }
  if (StartsWith(family, "management") || StartsWith(family, "metrics") || StartsWith(family, "extension")) {
    return FunctionCostClass::io_or_catalog;
  }
  if (Contains(id, "crypto") || Contains(id, "digest") || Contains(id, "hash")) {
    return FunctionCostClass::scalar_cpu_heavy;
  }
  return FunctionCostClass::scalar_cpu_light;
}

FunctionNullPolicy NullPolicyFor(std::string_view id, std::string_view family) {
  if (StartsWith(family, "data.aggregate")) return FunctionNullPolicy::ignores_nulls;
  if (Contains(id, "count")) return FunctionNullPolicy::custom;
  if (Contains(id, "coalesce") || Contains(id, "ifnull") || Contains(id, "nvl")) {
    return FunctionNullPolicy::custom;
  }
  return FunctionNullPolicy::null_in_null_out;
}

FunctionIndexability IndexabilityFor(std::string_view id, std::string_view family) {
  if (IsVolatileId(id)) return FunctionIndexability::not_indexable;
  if (family == "search" || family == "spatial" || family == "vector") {
    return FunctionIndexability::specialized_index_required;
  }
  if (Contains(id, "contains") || Contains(id, "starts_with") || Contains(id, "ends_with") ||
      Contains(id, "lower") || Contains(id, "upper")) {
    return FunctionIndexability::expression_indexable;
  }
  if (StartsWith(family, "data.aggregate") || StartsWith(family, "data.window")) {
    return FunctionIndexability::not_indexable;
  }
  return FunctionIndexability::expression_indexable;
}

FunctionPushdownEligibility PushdownFor(std::string_view id, std::string_view family) {
  if (IsVolatileId(id) || IsSecuritySensitiveId(id, family)) return FunctionPushdownEligibility::forbidden;
  if (IsSpecializedFamily(family)) return FunctionPushdownEligibility::remote_safe_with_policy;
  if (StartsWith(family, "data.aggregate")) return FunctionPushdownEligibility::remote_safe_with_policy;
  if (StartsWith(family, "data.window")) return FunctionPushdownEligibility::local_only;
  return FunctionPushdownEligibility::remote_safe;
}

FunctionLlvmEligibility LlvmFor(std::string_view id, std::string_view family) {
  if (IsVolatileId(id) || IsSecuritySensitiveId(id, family)) return FunctionLlvmEligibility::forbidden;
  if (StartsWith(family, "data.scalar") && (Contains(id, "abs") || Contains(id, "sqrt") ||
                                            Contains(id, "power") || Contains(id, "mod") ||
                                            Contains(id, "length"))) {
    return FunctionLlvmEligibility::llvm_safe;
  }
  if (StartsWith(family, "data.aggregate") || StartsWith(family, "data.window")) {
    return FunctionLlvmEligibility::interpreter_only;
  }
  return FunctionLlvmEligibility::interpreter_only;
}

FunctionDeterminism DeterminismFor(std::string_view id, std::string_view family) {
  if (Contains(id, "sequence") || Contains(id, "identity") || id == "nextval" || id == "gen_id") {
    return FunctionDeterminism::side_effecting;
  }
  if (IsVolatileId(id)) return FunctionDeterminism::volatile_value;
  if (IsSecuritySensitiveId(id, family)) return FunctionDeterminism::stable_statement;
  if (StartsWith(family, "management") || StartsWith(family, "metrics") || StartsWith(family, "extension")) {
    return FunctionDeterminism::stable_statement;
  }
  return FunctionDeterminism::deterministic;
}

}  // namespace

FunctionOptimizerMetadata BuildFunctionOptimizerMetadata(std::string_view function_id,
                                                         std::string_view family,
                                                         FunctionImplementationState state) {
  FunctionOptimizerMetadata metadata;
  metadata.determinism = DeterminismFor(function_id, family);
  metadata.cost_class = CostClassFor(function_id, family);
  metadata.null_policy = NullPolicyFor(function_id, family);
  metadata.indexability = IndexabilityFor(function_id, family);
  metadata.pushdown = PushdownFor(function_id, family);
  metadata.llvm = LlvmFor(function_id, family);
  metadata.descriptor_rule = DescriptorRuleFor(function_id, family);
  metadata.collation_charset_timezone_rule = StartsWith(family, "data.scalar") || family == "search"
                                                 ? "charset/collation sensitive when text descriptor participates"
                                                 : "not text-locale sensitive unless descriptor says so";
  metadata.resource_budget_class = IsSpecializedFamily(family) ? "specialized_family_budget" :
                                   StartsWith(family, "data.aggregate") ? "aggregate_state_budget" :
                                   StartsWith(family, "data.window") ? "window_frame_budget" :
                                   "scalar_default_budget";
  metadata.security_sensitive = IsSecuritySensitiveId(function_id, family);
  metadata.audit_required = metadata.security_sensitive ||
                            state == FunctionImplementationState::implemented_policy_security_or_dependency_runtime_refusal ||
                            metadata.determinism == FunctionDeterminism::side_effecting;
  return metadata;
}

FunctionOptimizerMetadata BuildOperatorOptimizerMetadata(std::string_view operator_id) {
  FunctionOptimizerMetadata metadata = BuildFunctionOptimizerMetadata(operator_id, "operator", FunctionImplementationState::implemented_behavior);
  metadata.descriptor_rule = "operator descriptor from operand descriptors";
  metadata.resource_budget_class = "operator_default_budget";
  metadata.pushdown = IsVolatileId(operator_id) ? FunctionPushdownEligibility::forbidden
                                                : FunctionPushdownEligibility::remote_safe;
  metadata.llvm = FunctionLlvmEligibility::llvm_safe;
  if (Contains(operator_id, "json") || Contains(operator_id, "document") || Contains(operator_id, "spatial") ||
      Contains(operator_id, "vector")) {
    metadata.llvm = FunctionLlvmEligibility::interpreter_only;
  }
  return metadata;
}

void PopulateDefaultFunctionOptimizerMetadata(FunctionRegistryEntry* entry) {
  if (entry == nullptr) return;
  if (entry->optimizer_metadata.descriptor_rule.empty()) {
    entry->optimizer_metadata = BuildFunctionOptimizerMetadata(entry->function_id,
                                                               entry->family,
                                                               entry->implementation_state);
  }
}

std::optional<FunctionOptimizerMetadata> LookupFunctionOptimizerMetadata(const FunctionRegistry& registry,
                                                                         std::string_view function_id) {
  const auto* entry = registry.Lookup(function_id);
  if (entry == nullptr) return std::nullopt;
  return entry->optimizer_metadata;
}

FunctionMetadataDecision PlannerMayFoldFunction(const FunctionOptimizerMetadata& metadata) {
  const bool allowed = metadata.determinism == FunctionDeterminism::deterministic &&
                       !metadata.security_sensitive &&
                       metadata.cost_class != FunctionCostClass::aggregate_stateful;
  return FunctionMetadataDecision{allowed, allowed ? "deterministic_fold_allowed" : "not_safe_to_fold"};
}

FunctionMetadataDecision OptimizerMayPushDownFunction(const FunctionOptimizerMetadata& metadata,
                                                      bool cluster_execution_available) {
  if (metadata.pushdown == FunctionPushdownEligibility::forbidden) {
    return FunctionMetadataDecision{false, "pushdown_forbidden_by_metadata"};
  }
  if (metadata.pushdown == FunctionPushdownEligibility::local_only) {
    return FunctionMetadataDecision{false, "local_only_function"};
  }
  if (metadata.pushdown == FunctionPushdownEligibility::remote_safe_with_policy && !cluster_execution_available) {
    return FunctionMetadataDecision{false, "cluster_or_policy_execution_unavailable"};
  }
  return FunctionMetadataDecision{true, "pushdown_allowed"};
}

FunctionMetadataDecision LlvmMayCompileFunction(const FunctionOptimizerMetadata& metadata,
                                                bool llvm_available) {
  if (metadata.llvm == FunctionLlvmEligibility::forbidden) {
    return FunctionMetadataDecision{false, "llvm_forbidden_by_metadata"};
  }
  if (!llvm_available && metadata.llvm == FunctionLlvmEligibility::llvm_required_when_enabled) {
    return FunctionMetadataDecision{false, "llvm_required_but_unavailable"};
  }
  if (!llvm_available) return FunctionMetadataDecision{false, "llvm_unavailable"};
  return FunctionMetadataDecision{metadata.llvm == FunctionLlvmEligibility::llvm_safe ||
                                      metadata.llvm == FunctionLlvmEligibility::llvm_required_when_enabled,
                                  metadata.llvm == FunctionLlvmEligibility::interpreter_only
                                      ? "interpreter_only"
                                      : "llvm_allowed"};
}

std::vector<std::string> ValidateFunctionOptimizerMetadataComplete(const FunctionRegistry& registry) {
  std::vector<std::string> errors;
  for (const auto& entry : registry.Entries()) {
    if (entry.optimizer_metadata.descriptor_rule.empty()) {
      errors.push_back(entry.function_id + ": descriptor_rule missing");
    }
    if (entry.optimizer_metadata.resource_budget_class.empty()) {
      errors.push_back(entry.function_id + ": resource_budget_class missing");
    }
    if (entry.optimizer_metadata.collation_charset_timezone_rule.empty()) {
      errors.push_back(entry.function_id + ": collation_charset_timezone_rule missing");
    }
  }
  return errors;
}

}  // namespace scratchbird::engine::functions
