// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metadata/function_hardening.hpp"

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

bool AnyResourceLimitPresent(const FunctionResourceLimits& limits) {
  return limits.max_input_bytes != 0 || limits.max_output_bytes != 0 ||
         limits.max_steps != 0 || limits.max_memory_bytes != 0 ||
         limits.max_recursion_depth != 0;
}

FunctionResourceLimits LimitsForBudgetClass(std::string_view budget_class) {
  FunctionResourceLimits limits;
  if (budget_class == "aggregate_state_budget" || budget_class == "window_frame_budget") {
    limits.max_input_bytes = 64 * 1024 * 1024;
    limits.max_output_bytes = 16 * 1024 * 1024;
    limits.max_steps = 50'000'000;
    limits.max_memory_bytes = 64 * 1024 * 1024;
    limits.max_recursion_depth = 0;
    return limits;
  }
  if (budget_class == "specialized_family_budget") {
    limits.max_input_bytes = 128 * 1024 * 1024;
    limits.max_output_bytes = 64 * 1024 * 1024;
    limits.max_steps = 100'000'000;
    limits.max_memory_bytes = 128 * 1024 * 1024;
    limits.max_recursion_depth = 128;
    return limits;
  }
  limits.max_input_bytes = 8 * 1024 * 1024;
  limits.max_output_bytes = 8 * 1024 * 1024;
  limits.max_steps = 1'000'000;
  limits.max_memory_bytes = 16 * 1024 * 1024;
  limits.max_recursion_depth = 32;
  return limits;
}

std::string DependencyGateFor(const FunctionRegistryEntry& entry) {
  if (Contains(entry.function_id, "crypto") || Contains(entry.function_id, "digest")) return "crypto_provider_required";
  if (entry.family == "search") return "search_runtime_available";
  if (entry.family == "spatial") return "spatial_runtime_available";
  if (entry.family == "vector") return "vector_runtime_available";
  if (entry.family == "timeseries") return "timeseries_runtime_available";
  if (StartsWith(entry.family, "nosql.")) return "nosql_runtime_available";
  if (Contains(entry.function_id, "llvm")) return "llvm_required";
  return "core_runtime";
}

std::string RedactionClassFor(const FunctionRegistryEntry& entry) {
  if (entry.optimizer_metadata.security_sensitive || entry.optimizer_metadata.audit_required) {
    return "sensitive_function_metadata";
  }
  if (entry.family == "metrics" || StartsWith(entry.family, "management")) return "operational_metadata";
  return "public_function_metadata";
}

void AddIssue(FunctionHardeningReport* report,
              std::string function_id,
              std::string severity,
              std::string detail) {
  report->ok = false;
  report->issues.push_back(FunctionHardeningIssue{std::move(function_id), std::move(severity), std::move(detail)});
}

}  // namespace

void PopulateFunctionHardeningDefaults(FunctionRegistryEntry* entry) {
  if (entry == nullptr) return;
  if (entry->execute_right.empty()) entry->execute_right = "EXECUTE_FUNCTION";
  if (entry->metadata_visibility_right.empty()) entry->metadata_visibility_right = "DISCOVER_FUNCTION_METADATA";
  if (entry->redaction_class.empty()) entry->redaction_class = RedactionClassFor(*entry);
  if (entry->dependency_gate.empty()) entry->dependency_gate = DependencyGateFor(*entry);
  if (entry->semantic_version.empty()) entry->semantic_version = "function_semantics_v1";
  if (!AnyResourceLimitPresent(entry->resource_limits)) {
    entry->resource_limits = LimitsForBudgetClass(entry->optimizer_metadata.resource_budget_class);
  }
}

FunctionCatalogExportRow BuildFunctionCatalogExportRow(const FunctionRegistryEntry& entry,
                                                       bool metadata_visible) {
  FunctionCatalogExportRow row;
  row.function_id = entry.function_id;
  row.function_uuid = metadata_visible ? entry.function_uuid : "";
  row.short_name = entry.short_name;
  row.family = entry.family;
  row.implementation_state = ToString(entry.implementation_state);
  row.descriptor_rule = metadata_visible ? entry.optimizer_metadata.descriptor_rule : "redacted";
  row.execute_right = metadata_visible ? entry.execute_right : "";
  row.visibility_right = entry.metadata_visibility_right;
  row.redaction_class = entry.redaction_class;
  row.metadata_redacted = !metadata_visible;
  return row;
}

FunctionVersionCompatibilityDecision ResolveFunctionVersionCompatibility(
    std::string stored_semantic_version,
    std::string current_semantic_version) {
  if (stored_semantic_version.empty()) {
    return FunctionVersionCompatibilityDecision{false, "refuse", "SB_DIAG_FUNCTION_VERSION_MISSING"};
  }
  if (current_semantic_version.empty()) current_semantic_version = "function_semantics_v1";
  if (stored_semantic_version == current_semantic_version) {
    return FunctionVersionCompatibilityDecision{true, "use_as_is", "SB_DIAG_OK"};
  }
  if (stored_semantic_version == "function_semantics_v1" && current_semantic_version == "function_semantics_v2") {
    return FunctionVersionCompatibilityDecision{true, "upgrade_metadata_only", "SB_DIAG_FUNCTION_VERSION_METADATA_UPGRADE"};
  }
  return FunctionVersionCompatibilityDecision{false, "refuse", "SB_DIAG_FUNCTION_VERSION_INCOMPATIBLE"};
}

FunctionHardeningReport ReviewFunctionDeterminismMetadata(const FunctionRegistry& registry) {
  FunctionHardeningReport report;
  for (const auto& entry : registry.Entries()) {
    const bool side_effect_name = Contains(entry.function_id, "sequence") || Contains(entry.function_id, "identity") ||
                                  Contains(entry.function_id, "nextval") || Contains(entry.function_id, "gen_id");
    if (side_effect_name && entry.optimizer_metadata.determinism != FunctionDeterminism::side_effecting) {
      AddIssue(&report, entry.function_id, "blocker", "side-effecting function is not marked side_effecting");
    }
    if (entry.optimizer_metadata.security_sensitive &&
        entry.optimizer_metadata.pushdown != FunctionPushdownEligibility::forbidden &&
        entry.optimizer_metadata.pushdown != FunctionPushdownEligibility::local_only) {
      AddIssue(&report, entry.function_id, "blocker", "security-sensitive function allows unsafe pushdown");
    }
    if (entry.optimizer_metadata.determinism == FunctionDeterminism::volatile_value &&
        entry.optimizer_metadata.indexability != FunctionIndexability::not_indexable) {
      AddIssue(&report, entry.function_id, "major", "volatile function is marked indexable");
    }
  }
  return report;
}

FunctionHardeningReport ValidateFunctionCrossPlatformGate(const FunctionRegistry& registry) {
  FunctionHardeningReport report;
  for (const auto& entry : registry.Entries()) {
    if (entry.optimizer_metadata.descriptor_rule.empty()) {
      AddIssue(&report, entry.function_id, "blocker", "descriptor rule is required for cross-platform behavior");
    }
    if (entry.optimizer_metadata.collation_charset_timezone_rule.empty()) {
      AddIssue(&report, entry.function_id, "blocker", "charset/collation/timezone rule is required");
    }
    if (!AnyResourceLimitPresent(entry.resource_limits)) {
      AddIssue(&report, entry.function_id, "major", "resource limits are required for deterministic cross-platform execution");
    }
  }
  return report;
}

std::vector<GoldenDonorBehaviorFixture> GoldenDonorBehaviorFixtures() {
  return {
      {"postgresql", "substring", "data.scalar.substring", "('abcdef',2,3)", "text:'bcd'", "canonical_then_donor_rendered"},
      {"mysql", "JSON_EXTRACT", "nosql.document.get", "('{\"a\":1}','$.a')", "json_document:'1'", "canonical_then_donor_rendered"},
      {"firebird", "GEN_ID", "data.sequence.next", "('seq_uuid',5)", "int64", "side_effect_evidence_required"},
      {"sqlite", "last_insert_rowid", "data.identity.current", "()", "int64_or_null", "context_value"},
      {"postgis", "ST_Distance", "spatial.distance", "(POINT(0 0),POINT(3 4))", "real64:5", "plugin_alias"},
      {"pgvector", "vector_l2_distance", "vector.distance", "([1,2],[4,6])", "real64:5", "plugin_alias"},
  };
}

std::vector<FunctionPerformanceBudget> FunctionPerformanceBudgets() {
  return {
      {"scalar_default_budget", 8 * 1024 * 1024, 8 * 1024 * 1024, 1'000'000, 16 * 1024 * 1024, "warn_then_ticket"},
      {"aggregate_state_budget", 64 * 1024 * 1024, 16 * 1024 * 1024, 50'000'000, 64 * 1024 * 1024, "block_regression"},
      {"window_frame_budget", 64 * 1024 * 1024, 16 * 1024 * 1024, 50'000'000, 64 * 1024 * 1024, "block_regression"},
      {"specialized_family_budget", 128 * 1024 * 1024, 64 * 1024 * 1024, 100'000'000, 128 * 1024 * 1024, "warn_then_profile"},
  };
}

std::vector<FunctionAuditEvidenceRequirement> FunctionAuditEvidenceRequirements(
    const FunctionRegistry& registry) {
  std::vector<FunctionAuditEvidenceRequirement> rows;
  for (const auto& entry : registry.Entries()) {
    if (entry.optimizer_metadata.audit_required) {
      rows.push_back(FunctionAuditEvidenceRequirement{
          entry.function_id,
          "function_execution_policy_evidence",
          "security_sensitive_or_side_effecting_or_runtime_refusal",
          true,
      });
    }
  }
  return rows;
}

}  // namespace scratchbird::engine::functions
