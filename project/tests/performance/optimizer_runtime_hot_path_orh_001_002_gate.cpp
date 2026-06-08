// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_consumption_evidence.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ORH-001/002 gate failure: " << message << '\n';
    std::exit(1);
  }
}

opt::RuntimeOptimizedPathEvidence CompleteSelectionOnlyEvidence(
    std::string route_kind) {
  auto evidence = opt::MakeSelectionOnlyRuntimeEvidence(
      "catalog_backed_access_path_v1",
      std::move(route_kind),
      "SB_ORH_TEST.SELECTION_ONLY",
      "executor has not consumed selected physical path");
  evidence.catalog_epoch = 17;
  evidence.security_epoch = 23;
  evidence.redaction_epoch = 29;
  evidence.provider_generation = 31;
  evidence.transaction_snapshot_class = "snapshot";
  evidence.result_contract_hash = "hash:orh001-result-contract";
  return evidence;
}

void SelectionOnlyEvidenceIsDistinctFromRuntimeConsumption() {
  const auto evidence = CompleteSelectionOnlyEvidence("embedded");
  const auto validation = opt::ValidateRuntimeOptimizedPathEvidence(evidence);
  Require(validation.ok, "selection-only evidence rejected");
  Require(validation.state == opt::RuntimeConsumptionState::kSelectionOnly,
          "selection-only evidence state mismatch");
  Require(!evidence.runtime_consumed,
          "selection-only evidence marked runtime_consumed");
  Require(evidence.selected_path == "catalog_backed_access_path_v1",
          "selected_path not preserved");
  Require(evidence.consumed_module.empty(),
          "selection-only evidence should not name consumed_module");
  Require(evidence.route_kind == "embedded", "route_kind not preserved");
  Require(!evidence.live_execution, "selection-only should not be live");
  Require(!evidence.contract_only, "selection-only should not be contract-only");
  Require(evidence.transaction_snapshot_class == "snapshot",
          "transaction_snapshot_class missing");
  Require(evidence.catalog_epoch == 17, "catalog_epoch missing");
  Require(evidence.security_epoch == 23, "security_epoch missing");
  Require(evidence.redaction_epoch == 29, "redaction_epoch missing");
  Require(evidence.provider_generation == 31, "provider_generation missing");
  Require(evidence.result_contract_hash == "hash:orh001-result-contract",
          "result_contract_hash missing");
  Require(evidence.fallback_reason ==
              "executor has not consumed selected physical path",
          "fallback_reason missing");
  Require(evidence.diagnostic_code == "SB_ORH_TEST.SELECTION_ONLY",
          "diagnostic_code missing");
}

void RuntimeConsumedEvidenceNamesConsumingModule() {
  auto evidence =
      opt::MarkRuntimeEvidenceConsumed(CompleteSelectionOnlyEvidence("ipc"),
                                       "engine.executor.physical_scan");
  const auto validation = opt::ValidateRuntimeOptimizedPathEvidence(evidence);
  Require(validation.ok, "runtime-consumed evidence rejected");
  Require(validation.state == opt::RuntimeConsumptionState::kRuntimeConsumed,
          "runtime-consumed state mismatch");
  Require(evidence.runtime_consumed, "runtime_consumed was not set");
  Require(evidence.live_execution, "live_execution was not set");
  Require(!evidence.contract_only,
          "runtime-consumed evidence cannot be contract-only");
  Require(evidence.consumed_module == "engine.executor.physical_scan",
          "consumed_module missing");
  Require(evidence.fallback_reason.empty(),
          "runtime-consumed evidence should clear fallback_reason");
  Require(evidence.diagnostic_code ==
              "SB_ORH_RUNTIME_CONSUMPTION_EVIDENCE.CONSUMED",
          "runtime-consumed diagnostic mismatch");
}

void MissingRequiredFieldsAreRejected() {
  auto evidence = CompleteSelectionOnlyEvidence("inet");
  evidence.result_contract_hash.clear();
  const auto validation = opt::ValidateRuntimeOptimizedPathEvidence(evidence);
  Require(!validation.ok, "missing result_contract_hash accepted");
  Require(validation.diagnostic_code ==
              "SB_ORH_RUNTIME_CONSUMPTION_EVIDENCE.MISSING_REQUIRED_FIELD",
          "missing field diagnostic mismatch");
  Require(!validation.missing_fields.empty(), "missing fields not reported");
}

void RuntimeConsumedEvidenceRequiresDiagnosticCode() {
  auto evidence =
      opt::MarkRuntimeEvidenceConsumed(CompleteSelectionOnlyEvidence("ipc"),
                                       "engine.executor.physical_scan");
  evidence.diagnostic_code.clear();
  const auto validation = opt::ValidateRuntimeOptimizedPathEvidence(evidence);
  Require(!validation.ok,
          "runtime-consumed evidence with empty diagnostic_code accepted");
  Require(validation.diagnostic_code ==
              "SB_ORH_RUNTIME_CONSUMPTION_EVIDENCE.MISSING_REQUIRED_FIELD",
          "runtime-consumed missing diagnostic_code mismatch");
  bool reported = false;
  for (const auto& field : validation.missing_fields) {
    if (field == "diagnostic_code") reported = true;
  }
  Require(reported, "runtime-consumed missing diagnostic_code not reported");
}

void ContractOnlyEvidenceIsExactBlockerDiagnostic() {
  auto evidence = CompleteSelectionOnlyEvidence("embedded");
  evidence.contract_only = true;
  evidence.diagnostic_code = "SB_ORH_TEST.CONTRACT_ONLY_READINESS";
  evidence.fallback_reason = "live embedded route is not running";
  const auto validation = opt::ValidateRuntimeOptimizedPathEvidence(evidence);
  Require(validation.ok, "contract-only blocker evidence rejected");
  Require(validation.state == opt::RuntimeConsumptionState::kContractOnlyBlocker,
          "contract-only blocker state mismatch");

  const opt::RouteCompletionClaim claim{
      .route_kind = "embedded",
      .benchmark_clean = true,
      .live_route = true,
      .mark_complete = true,
  };
  const auto guard = opt::EvaluateRouteCompletionClaim(claim, {evidence});
  Require(!guard.can_mark_complete,
          "contract-only embedded route was allowed to close");
  Require(guard.exact_blocker,
          "contract-only live route did not produce exact blocker");
  Require(guard.diagnostic_code ==
              "SB_ORH_ROUTE_CONTRACT_ONLY_LIVE_CLOSURE_BLOCKED",
          "contract-only live route blocker diagnostic mismatch");
}

void LiveBenchmarkCleanRoutesRequireRuntimeConsumption() {
  for (const std::string route : {"embedded", "ipc", "inet"}) {
    const opt::RouteCompletionClaim claim{
        .route_kind = route,
        .benchmark_clean = true,
        .live_route = true,
        .mark_complete = true,
    };
    auto selection_only = CompleteSelectionOnlyEvidence(route);
    const auto blocked =
        opt::EvaluateRouteCompletionClaim(claim, {selection_only});
    Require(!blocked.can_mark_complete,
            route + " selection-only route was allowed to close");
    Require(blocked.diagnostic_code ==
                "SB_ORH_ROUTE_RUNTIME_CONSUMPTION_MISSING",
            route + " missing runtime diagnostic mismatch");

    auto consumed = opt::MarkRuntimeEvidenceConsumed(
        CompleteSelectionOnlyEvidence(route),
        "engine.executor." + route + ".optimized_path");
    const auto allowed = opt::EvaluateRouteCompletionClaim(claim, {consumed});
    Require(allowed.can_mark_complete,
            route + " live runtime-consumed route was blocked");
    Require(allowed.diagnostic_code ==
                "SB_ORH_ROUTE_CLAIM.LIVE_CONSUMPTION_OK",
            route + " live route success diagnostic mismatch");
  }
}

}  // namespace

int main() {
  SelectionOnlyEvidenceIsDistinctFromRuntimeConsumption();
  RuntimeConsumedEvidenceNamesConsumingModule();
  MissingRequiredFieldsAreRejected();
  RuntimeConsumedEvidenceRequiresDiagnosticCode();
  ContractOnlyEvidenceIsExactBlockerDiagnostic();
  LiveBenchmarkCleanRoutesRequireRuntimeConsumption();
  return 0;
}
