// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_maintenance.hpp"

#include "index_access_method.hpp"
#include "index_route_capability.hpp"
#include "policy_blocked_index_admission.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

bool IsMutating(IndexMaintenanceOperation operation) {
  return operation != IndexMaintenanceOperation::verify && operation != IndexMaintenanceOperation::warm &&
         operation != IndexMaintenanceOperation::cool;
}

const char* BoolText(bool value) {
  return value ? "true" : "false";
}

bool OrderedBtreeFamily(IndexFamily family) {
  return family == IndexFamily::btree ||
         family == IndexFamily::unique_btree ||
         family == IndexFamily::expression ||
         family == IndexFamily::partial ||
         family == IndexFamily::covering;
}

bool TokenDeltaFamily(IndexFamily family) {
  return family == IndexFamily::full_text ||
         family == IndexFamily::gin ||
         family == IndexFamily::inverted ||
         family == IndexFamily::ngram ||
         family == IndexFamily::sparse_wand ||
         family == IndexFamily::document_path;
}

bool BufferedSummaryFamily(IndexFamily family) {
  return family == IndexFamily::bitmap ||
         family == IndexFamily::brin_zone ||
         family == IndexFamily::bloom ||
         family == IndexFamily::columnar_zone;
}

bool ProviderSpecificFamily(IndexFamily family) {
  return family == IndexFamily::hash ||
         family == IndexFamily::spatial ||
         family == IndexFamily::rtree ||
         family == IndexFamily::gist ||
         family == IndexFamily::spgist ||
         family == IndexFamily::vector_exact ||
         family == IndexFamily::vector_hnsw ||
         family == IndexFamily::vector_ivf ||
         family == IndexFamily::graph;
}

bool MemoryFamily(IndexFamily family) {
  return family == IndexFamily::temporary_work ||
         family == IndexFamily::in_memory;
}

bool NonBtreeExactRecheckRequired(IndexFamily family) {
  return family != IndexFamily::btree &&
         family != IndexFamily::unique_btree;
}

bool StrategyBindsExactRecheck(IndexFamily family,
                               IndexDmlMaintenanceStrategyKind strategy) {
  if (!NonBtreeExactRecheckRequired(family)) {
    return false;
  }
  switch (strategy) {
    case IndexDmlMaintenanceStrategyKind::synchronous_physical_rewrite:
    case IndexDmlMaintenanceStrategyKind::deferred_secondary_delta_ledger:
    case IndexDmlMaintenanceStrategyKind::page_aware_change_buffer:
    case IndexDmlMaintenanceStrategyKind::provider_specific:
    case IndexDmlMaintenanceStrategyKind::rebuild_from_authoritative_base:
      return true;
    case IndexDmlMaintenanceStrategyKind::refused:
      return false;
  }
  return false;
}

void AddEvidence(IndexDmlMaintenanceStrategy* strategy,
                 std::string key,
                 std::string value) {
  strategy->evidence.push_back(std::move(key) + "=" + std::move(value));
}

IndexDmlMaintenanceStrategy RefuseStrategy(IndexFamily family,
                                           Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail) {
  IndexDmlMaintenanceStrategy strategy;
  strategy.status = status;
  strategy.family = family;
  strategy.fail_closed = true;
  strategy.strategy_id = "refused";
  strategy.diagnostic = MakeIndexMaintenanceDiagnostic(
      strategy.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  AddEvidence(&strategy, "strategy_id", strategy.strategy_id);
  AddEvidence(&strategy, "family", IndexFamilyName(family));
  AddEvidence(&strategy, "admitted", "false");
  AddEvidence(&strategy, "fail_closed", "true");
  return strategy;
}
}  // namespace

const char* IndexDmlMaintenanceStrategyKindName(
    IndexDmlMaintenanceStrategyKind strategy) {
  switch (strategy) {
    case IndexDmlMaintenanceStrategyKind::synchronous_physical_rewrite:
      return "synchronous_physical_rewrite";
    case IndexDmlMaintenanceStrategyKind::deferred_secondary_delta_ledger:
      return "deferred_secondary_delta_ledger";
    case IndexDmlMaintenanceStrategyKind::page_aware_change_buffer:
      return "page_aware_change_buffer";
    case IndexDmlMaintenanceStrategyKind::provider_specific:
      return "provider_specific";
    case IndexDmlMaintenanceStrategyKind::rebuild_from_authoritative_base:
      return "rebuild_from_authoritative_base";
    case IndexDmlMaintenanceStrategyKind::refused:
      return "refused";
  }
  return "refused";
}

IndexDmlMaintenanceStrategy ClassifyIndexDmlMaintenanceStrategy(
    IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  const auto* capability = FindBuiltinIndexFamilyPhysicalCapabilityState(family);
  if (descriptor == nullptr || capability == nullptr ||
      family == IndexFamily::unknown) {
    return RefuseStrategy(
        family,
        ErrorStatus(),
        "SB-INDEX-DML-MAINTENANCE-UNKNOWN-FAMILY",
        "index.dml_maintenance.unknown_family",
        "family must be declared before a DML maintenance strategy is selected");
  }
  if (descriptor->completion !=
      IndexCompletionStatus::accepted_requires_full_implementation) {
    return RefuseStrategy(
        family,
        ErrorStatus(),
        "SB-INDEX-DML-MAINTENANCE-FAMILY-NOT-ADMITTED",
        "index.dml_maintenance.family_not_admitted",
        "family is not accepted as a physical implementation");
  }
  if (descriptor->persistence == IndexPersistenceClass::reference_emulated ||
      descriptor->persistence == IndexPersistenceClass::policy_blocked ||
      capability->blocker != IndexFamilyPhysicalCapabilityBlocker::none) {
    return RefuseStrategy(
        family,
        ErrorStatus(),
        capability->blocker_diagnostic_code.empty()
            ? "SB-INDEX-DML-MAINTENANCE-FAMILY-REFUSED"
            : capability->blocker_diagnostic_code,
        capability->blocker_message_key.empty()
            ? "index.dml_maintenance.family_refused"
            : capability->blocker_message_key,
        capability->blocker_detail.empty()
            ? "family is not admitted for physical DML maintenance"
            : capability->blocker_detail);
  }

  IndexDmlMaintenanceStrategy strategy;
  strategy.status = OkStatus();
  strategy.family = family;
  strategy.runtime_available = capability->runtime_available;
  strategy.benchmark_clean = capability->benchmark_clean;
  const auto* dml_route =
      FindBuiltinIndexRouteCapabilityState(IndexRouteKind::dml_update, family);
  const auto* maintenance_route =
      FindBuiltinIndexRouteCapabilityState(IndexRouteKind::maintenance, family);
  strategy.dml_route_supported =
      dml_route != nullptr && dml_route->route_complete() &&
      dml_route->supports_write && dml_route->supports_mutation;
  strategy.maintenance_route_supported =
      maintenance_route != nullptr && maintenance_route->route_complete() &&
      maintenance_route->supports_mutation;
  strategy.exact_recheck_required = NonBtreeExactRecheckRequired(family);
  strategy.mga_recheck_required =
      maintenance_route == nullptr || maintenance_route->requires_mga_recheck;
  strategy.security_recheck_required =
      maintenance_route == nullptr ||
      maintenance_route->requires_security_recheck;

  if (OrderedBtreeFamily(family) || MemoryFamily(family)) {
    strategy.strategy =
        IndexDmlMaintenanceStrategyKind::synchronous_physical_rewrite;
    strategy.strategy_id = "synchronous_physical_rewrite";
    strategy.synchronous = true;
  } else if (TokenDeltaFamily(family)) {
    strategy.strategy =
        IndexDmlMaintenanceStrategyKind::deferred_secondary_delta_ledger;
    strategy.strategy_id = "deferred_secondary_delta_ledger";
    strategy.deferred_delta = true;
  } else if (BufferedSummaryFamily(family)) {
    strategy.strategy =
        IndexDmlMaintenanceStrategyKind::page_aware_change_buffer;
    strategy.strategy_id = "page_aware_change_buffer";
    strategy.buffered = true;
  } else if (ProviderSpecificFamily(family)) {
    strategy.strategy = IndexDmlMaintenanceStrategyKind::provider_specific;
    strategy.strategy_id = "provider_specific";
    strategy.provider_specific = true;
  } else {
    strategy.strategy =
        IndexDmlMaintenanceStrategyKind::rebuild_from_authoritative_base;
    strategy.strategy_id = "rebuild_from_authoritative_base";
    strategy.rebuild_required = true;
  }

  strategy.exact_recheck_strategy_bound =
      StrategyBindsExactRecheck(family, strategy.strategy);
  strategy.exact_recheck_gate_passed =
      !strategy.exact_recheck_required ||
      (maintenance_route != nullptr &&
       maintenance_route->requires_exact_recheck) ||
      strategy.exact_recheck_strategy_bound;
  strategy.admitted = capability->runtime_available &&
                      capability->benchmark_clean &&
                      capability->physically_complete() &&
                      strategy.maintenance_route_supported &&
                      strategy.exact_recheck_gate_passed &&
                      strategy.mga_recheck_required &&
                      strategy.security_recheck_required;
  strategy.fail_closed = !strategy.admitted;
  if (strategy.fail_closed) {
    strategy.status = ErrorStatus();
    strategy.diagnostic = MakeIndexMaintenanceDiagnostic(
        strategy.status,
        "SB-INDEX-DML-MAINTENANCE-STRATEGY-REFUSED",
        "index.dml_maintenance.strategy_refused",
        "runtime benchmark route exact MGA and security gates are required");
  } else {
    strategy.diagnostic = MakeIndexMaintenanceDiagnostic(
        strategy.status,
        "SB-INDEX-DML-MAINTENANCE-STRATEGY-ADMITTED",
        "index.dml_maintenance.strategy_admitted");
  }
  AddEvidence(&strategy, "strategy_id", strategy.strategy_id);
  AddEvidence(&strategy, "strategy_kind",
              IndexDmlMaintenanceStrategyKindName(strategy.strategy));
  AddEvidence(&strategy, "family", descriptor->id);
  AddEvidence(&strategy, "admitted", BoolText(strategy.admitted));
  AddEvidence(&strategy, "fail_closed", BoolText(strategy.fail_closed));
  AddEvidence(&strategy, "dml_route_supported",
              BoolText(strategy.dml_route_supported));
  AddEvidence(&strategy, "maintenance_route_supported",
              BoolText(strategy.maintenance_route_supported));
  AddEvidence(&strategy, "exact_recheck_required",
              BoolText(strategy.exact_recheck_required));
  AddEvidence(&strategy, "exact_recheck_strategy_bound",
              BoolText(strategy.exact_recheck_strategy_bound));
  AddEvidence(&strategy, "exact_recheck_gate_passed",
              BoolText(strategy.exact_recheck_gate_passed));
  AddEvidence(&strategy, "mga_recheck_required",
              BoolText(strategy.mga_recheck_required));
  AddEvidence(&strategy, "security_recheck_required",
              BoolText(strategy.security_recheck_required));
  AddEvidence(&strategy, "runtime_available",
              BoolText(strategy.runtime_available));
  AddEvidence(&strategy, "benchmark_clean",
              BoolText(strategy.benchmark_clean));
  AddEvidence(&strategy, "transaction_finality_authority", "false");
  AddEvidence(&strategy, "parser_or_reference_authority", "false");
  return strategy;
}

IndexMaintenancePlan PlanIndexMaintenance(const IndexMaintenanceRequest& request) {
  IndexMaintenancePlan plan;
  if (!request.index_uuid.valid() || request.family == IndexFamily::unknown) {
    plan.status = ErrorStatus();
    plan.diagnostic = MakeIndexMaintenanceDiagnostic(plan.status,
                                                     "SB-INDEX-MAINTENANCE-INVALID-REQUEST",
                                                     "index.maintenance.invalid_request");
    return plan;
  }
  const auto policy_blocked = EvaluatePolicyBlockedIndexAdmission(
      MakePolicyBlockedIndexRouteRequest(request.family,
                                         "",
                                         "index.maintenance",
                                         "management_policy_blocked_metadata",
                                         true));
  if (policy_blocked.fail_closed) {
    plan.status = policy_blocked.status;
    plan.diagnostic = policy_blocked.diagnostic;
    plan.steps.push_back("policy_blocked_fail_closed_before_physical_maintenance");
    return plan;
  }
  const auto* descriptor = FindBuiltinIndexFamily(request.family);
  const auto* capability =
      FindBuiltinIndexFamilyPhysicalCapabilityState(request.family);
  if (!descriptor || descriptor->completion != IndexCompletionStatus::accepted_requires_full_implementation) {
    plan.status = ErrorStatus();
    plan.diagnostic = MakeIndexMaintenanceDiagnostic(plan.status,
                                                     "SB-INDEX-MAINTENANCE-FAMILY-NOT-ADMITTED",
                                                     "index.maintenance.family_not_admitted");
    return plan;
  }
  if (capability == nullptr || !capability->declared_capability) {
    plan.status = ErrorStatus();
    plan.diagnostic = MakeIndexFamilyCapabilityDiagnostic(
        plan.status,
        "INDEX.CAPABILITY.UNKNOWN_FAMILY",
        "index.capability.unknown_family",
        IndexFamilyName(request.family),
        "family has no physical capability state",
        IndexFamilyPhysicalCapabilityBlocker::unknown_family);
    return plan;
  }
  plan.mutation_required = IsMutating(request.operation);
  const auto caps = CapabilitiesForFamily(request.family);
  const auto* maintenance_route =
      FindBuiltinIndexRouteCapabilityState(IndexRouteKind::maintenance,
                                           request.family);
  const bool generic_operation_available =
      (request.operation == IndexMaintenanceOperation::verify &&
       caps.supports_verify) ||
      (request.operation == IndexMaintenanceOperation::rebuild &&
       capability->rebuild &&
       maintenance_route != nullptr &&
       maintenance_route->route_complete() &&
       maintenance_route->supports_mutation) ||
      (request.operation != IndexMaintenanceOperation::verify &&
       request.operation != IndexMaintenanceOperation::rebuild &&
       request.family != IndexFamily::bitmap &&
       capability->maintenance);
  if (!generic_operation_available) {
    plan.status = ErrorStatus();
    plan.diagnostic = MakeIndexMaintenanceDiagnostic(
        plan.status,
        "SB-INDEX-MAINTENANCE-GENERIC-FAMILY-UNSUPPORTED",
        "index.maintenance.generic_family_unsupported",
        "family is specialized-route capable but is not admitted through the generic index maintenance surface");
    return plan;
  }
  const bool operation_available =
      (request.operation == IndexMaintenanceOperation::verify &&
       capability->validate) ||
      (request.operation == IndexMaintenanceOperation::rebuild &&
       capability->rebuild) ||
      (request.operation != IndexMaintenanceOperation::verify &&
       request.operation != IndexMaintenanceOperation::rebuild &&
       capability->maintenance);
  if (!capability->runtime_available || !operation_available) {
    plan.status = ErrorStatus();
    plan.diagnostic =
        MakeIndexFamilyCapabilityBlockerDiagnostic(plan.status, *capability);
    return plan;
  }
  if (plan.mutation_required && (request.read_only_database || !request.policy_allows_mutation)) {
    plan.status = ErrorStatus();
    plan.diagnostic = MakeIndexMaintenanceDiagnostic(plan.status,
                                                     "SB-INDEX-MAINTENANCE-MUTATION-REFUSED",
                                                     "index.maintenance.mutation_refused");
    return plan;
  }
  if (request.page_budget == 0 && request.byte_budget == 0 && request.time_budget_microseconds == 0) {
    plan.status = ErrorStatus();
    plan.diagnostic = MakeIndexMaintenanceDiagnostic(plan.status,
                                                     "SB-INDEX-MAINTENANCE-BUDGET-MISSING",
                                                     "index.maintenance.budget_missing");
    return plan;
  }
  plan.status = OkStatus();
  plan.admitted = true;
  plan.requires_exclusive_access = !request.online && plan.mutation_required;
  plan.steps.push_back("capture_authoritative_index_descriptor");
  plan.steps.push_back("validate_resource_epoch_and_family");
  if (request.operation == IndexMaintenanceOperation::verify) {
    plan.steps.push_back("scan_table_snapshot_and_index_candidates");
    plan.steps.push_back("compare_candidate_sets");
  } else {
    plan.steps.push_back("reserve_budgeted_maintenance_window");
    if (request.evaluate_exact_leaf_pressure) {
      plan.exact_leaf_pressure_decision =
          PlanExactIndexLeafPressureAction(request.exact_leaf_pressure);
      plan.exact_leaf_pressure_evaluated = true;
      plan.selected_action_evidence =
          plan.exact_leaf_pressure_decision.evidence;
      if (!plan.exact_leaf_pressure_decision.ok()) {
        plan.status = plan.exact_leaf_pressure_decision.status;
        plan.admitted = false;
        plan.diagnostic = plan.exact_leaf_pressure_decision.diagnostic;
        return plan;
      }
      switch (plan.exact_leaf_pressure_decision.action) {
        case ExactIndexLeafPressureAction::insert_without_split:
          plan.steps.push_back("exact_leaf_insert_without_split");
          break;
        case ExactIndexLeafPressureAction::cleanup_avoided_split:
          plan.steps.push_back("exact_leaf_cleanup_avoided_split");
          break;
        case ExactIndexLeafPressureAction::split_selected:
          plan.steps.push_back("exact_leaf_split_selected_after_cleanup_fail_open");
          break;
      }
    }
    plan.steps.push_back("write_required_evidence_before_success");
    plan.steps.push_back("publish_new_resource_epoch");
  }
  return plan;
}

DiagnosticRecord MakeIndexMaintenanceDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.maintenance");
}

}  // namespace scratchbird::core::index
