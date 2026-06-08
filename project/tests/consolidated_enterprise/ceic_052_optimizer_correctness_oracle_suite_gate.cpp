// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-052 focused validation for optimizer correctness oracle coverage.
#include "optimizer_correctness_oracle.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace opt = scratchbird::engine::optimizer;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool HasDiagnostic(const std::vector<std::string>& diagnostics,
                   std::string_view token) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.find(token) != std::string::npos) return true;
  }
  return false;
}

std::string ClassName(opt::OptimizerCorrectnessClass value) {
  return opt::OptimizerCorrectnessClassName(value);
}

bool RequiresExactRecheck(opt::OptimizerCorrectnessClass value) {
  return value == opt::OptimizerCorrectnessClass::kDmlLocator ||
         value == opt::OptimizerCorrectnessClass::kDocumentPath ||
         value == opt::OptimizerCorrectnessClass::kVector ||
         value == opt::OptimizerCorrectnessClass::kTextSearch ||
         value == opt::OptimizerCorrectnessClass::kGraph ||
         value == opt::OptimizerCorrectnessClass::kMixedFusion;
}

bool RequiresExactRerank(opt::OptimizerCorrectnessClass value) {
  return value == opt::OptimizerCorrectnessClass::kVector ||
         value == opt::OptimizerCorrectnessClass::kTextSearch ||
         value == opt::OptimizerCorrectnessClass::kMixedFusion;
}

bool RequiresCorrelationContract(opt::OptimizerCorrectnessClass value) {
  return value == opt::OptimizerCorrectnessClass::kCorrelatedDependency ||
         value == opt::OptimizerCorrectnessClass::kMixedFusion;
}

opt::OptimizerResultOracleEvidence ResultSide(
    std::string side,
    std::string cls,
    bool exact_recheck,
    bool exact_rerank,
    bool dml_locator) {
  opt::OptimizerResultOracleEvidence result;
  result.producer_label = std::move(side);
  result.result_contract_hash = "sha256:ceic052-result-contract-" + cls;
  result.result_hash = "sha256:ceic052-result-" + cls;
  result.result_row_count = 100 + cls.size();
  result.result_row_count_observed = true;
  result.ordering_contract_hash = "sha256:ceic052-ordering-" + cls;
  result.null_semantics_hash = "sha256:ceic052-null-semantics-" + cls;
  result.error_diagnostic_code = "SB_OK";
  result.diagnostic_digest = "sha256:ceic052-diagnostics-" + cls;
  result.row_locator_contract_hash =
      dml_locator ? "sha256:ceic052-row-locator-" + cls : "";
  result.recheck_evidence_digest = "sha256:ceic052-recheck-" + cls;
  result.accepted = true;
  result.live_route_executed = true;
  result.synthetic_result = false;
  result.exact_recheck_proven = exact_recheck;
  result.exact_rerank_proven = exact_rerank;
  result.mga_recheck_proven = true;
  result.security_recheck_proven = true;
  result.row_locator_mga_snapshot_proven = dml_locator;
  return result;
}

opt::OptimizerCorrectnessOracleCase Case(
    opt::OptimizerCorrectnessClass correctness_class,
    std::string route) {
  const auto cls = ClassName(correctness_class);
  const bool exact_recheck = RequiresExactRecheck(correctness_class);
  const bool exact_rerank = RequiresExactRerank(correctness_class);
  const bool dml_locator =
      correctness_class == opt::OptimizerCorrectnessClass::kDmlLocator;

  opt::OptimizerCorrectnessOracleCase oracle_case;
  oracle_case.case_id = "ceic052-" + cls;
  oracle_case.correctness_class = correctness_class;
  oracle_case.route_kind = std::move(route);
  oracle_case.route_label = "ceic052/optimizer_correctness/" + cls;
  oracle_case.dataset_schema_digest = "sha256:ceic052-dataset-" + cls;
  oracle_case.sblr_digest = "sha256:ceic052-sblr-" + cls;
  oracle_case.logical_plan_hash = "sha256:ceic052-logical-plan-" + cls;
  oracle_case.baseline_plan_hash = "sha256:ceic052-baseline-plan-" + cls;
  oracle_case.optimized_plan_hash = "sha256:ceic052-optimized-plan-" + cls;
  oracle_case.equivalence_contract_hash =
      "sha256:ceic052-equivalence-" + cls;
  if (RequiresCorrelationContract(correctness_class)) {
    oracle_case.correlation_dependency_contract_hash =
        "sha256:ceic052-correlation-" + cls;
  }
  oracle_case.catalog_epoch = 5201;
  oracle_case.security_epoch = 5202;
  oracle_case.redaction_epoch = 5203;
  oracle_case.statistics_epoch = 5204;
  oracle_case.provider_generation = 5205;
  oracle_case.baseline =
      ResultSide("engine_baseline", cls, exact_recheck, exact_rerank,
                 dml_locator);
  oracle_case.optimized =
      ResultSide("optimized_engine_route", cls, exact_recheck, exact_rerank,
                 dml_locator);
  oracle_case.production_correctness_claim = true;
  oracle_case.evidence_only = true;
  oracle_case.baseline_is_engine_reference_route = true;
  oracle_case.optimized_route_consumed = true;
  oracle_case.exact_recheck_required = exact_recheck;
  oracle_case.exact_rerank_required = exact_rerank;
  oracle_case.mga_recheck_required = true;
  oracle_case.security_recheck_required = true;
  oracle_case.donor_reference_only = true;
  return oracle_case;
}

std::vector<opt::OptimizerCorrectnessOracleCase> FullSuite() {
  const std::vector<std::string> routes = {
      "embedded", "ipc", "inet", "cli", "driver"};
  std::vector<opt::OptimizerCorrectnessOracleCase> suite;
  const auto classes = opt::RequiredOptimizerCorrectnessClasses();
  for (std::size_t index = 0; index < classes.size(); ++index) {
    suite.push_back(Case(classes[index], routes[index % routes.size()]));
  }
  return suite;
}

void FullSuiteIsProven() {
  const auto validation =
      opt::ValidateOptimizerCorrectnessOracleSuite(
          FullSuite(), opt::RequiredOptimizerCorrectnessClasses(),
          {"embedded", "ipc", "inet", "cli", "driver"});
  Require(validation.ok, "CEIC-052 valid correctness oracle suite rejected");
  Require(validation.correctness_proven,
          "CEIC-052 valid correctness oracle suite not marked proven");
}

void ResultContractsMustMatch() {
  auto oracle_case = Case(opt::OptimizerCorrectnessClass::kOuterJoin,
                          "embedded");
  oracle_case.optimized.result_hash = "sha256:wrong-result";
  oracle_case.optimized.result_row_count += 1;
  oracle_case.optimized.ordering_contract_hash = "sha256:wrong-ordering";
  oracle_case.optimized.null_semantics_hash = "sha256:wrong-null";
  oracle_case.optimized.error_diagnostic_code = "SB_DIFFERENT_ERROR";
  const auto validation =
      opt::ValidateOptimizerCorrectnessOracleCase(oracle_case);
  Require(!validation.ok, "CEIC-052 mismatch result evidence was accepted");
  Require(HasDiagnostic(validation.diagnostics, "RESULT_HASH_MISMATCH"),
          "CEIC-052 result hash mismatch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "ROW_COUNT_MISMATCH"),
          "CEIC-052 row count mismatch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORDERING_CONTRACT_MISMATCH"),
          "CEIC-052 ordering mismatch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "NULL_SEMANTICS_MISMATCH"),
          "CEIC-052 null-semantics mismatch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ERROR_DIAGNOSTIC_MISMATCH"),
          "CEIC-052 error diagnostic mismatch diagnostic missing");
}

void EmptyResultsCanBeProven() {
  auto oracle_case =
      Case(opt::OptimizerCorrectnessClass::kAntiJoin, "embedded");
  oracle_case.baseline.result_row_count = 0;
  oracle_case.optimized.result_row_count = 0;
  oracle_case.baseline.result_hash = "sha256:ceic052-empty-result";
  oracle_case.optimized.result_hash = "sha256:ceic052-empty-result";
  const auto validation =
      opt::ValidateOptimizerCorrectnessOracleCase(oracle_case);
  Require(validation.ok, "CEIC-052 empty result evidence was rejected");

  oracle_case.optimized.result_row_count_observed = false;
  const auto missing_observation =
      opt::ValidateOptimizerCorrectnessOracleCase(oracle_case);
  Require(!missing_observation.ok,
          "CEIC-052 missing row-count observation was accepted");
  Require(HasDiagnostic(missing_observation.diagnostics,
                        "RESULT_FIELDS_MISSING"),
          "CEIC-052 missing row-count observation diagnostic missing");
}

void SpecializedProofsAreMandatory() {
  auto oracle_case = Case(opt::OptimizerCorrectnessClass::kVector, "driver");
  oracle_case.baseline.exact_recheck_proven = false;
  oracle_case.optimized.exact_rerank_proven = false;
  oracle_case.optimized.mga_recheck_proven = false;
  oracle_case.optimized.security_recheck_proven = false;
  const auto validation =
      opt::ValidateOptimizerCorrectnessOracleCase(oracle_case);
  Require(!validation.ok, "CEIC-052 missing specialized proofs accepted");
  Require(HasDiagnostic(validation.diagnostics, "EXACT_RECHECK_MISSING"),
          "CEIC-052 exact recheck diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "EXACT_RERANK_MISSING"),
          "CEIC-052 exact rerank diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "MGA_RECHECK_MISSING"),
          "CEIC-052 MGA recheck diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "SECURITY_RECHECK_MISSING"),
          "CEIC-052 security recheck diagnostic missing");
}

void DmlLocatorProofIsMandatory() {
  auto oracle_case =
      Case(opt::OptimizerCorrectnessClass::kDmlLocator, "ipc");
  oracle_case.optimized.row_locator_contract_hash.clear();
  oracle_case.optimized.row_locator_mga_snapshot_proven = false;
  const auto validation =
      opt::ValidateOptimizerCorrectnessOracleCase(oracle_case);
  Require(!validation.ok, "CEIC-052 missing DML locator proof accepted");
  Require(HasDiagnostic(validation.diagnostics, "DML_LOCATOR_PROOF_MISSING"),
          "CEIC-052 DML locator proof diagnostic missing");
}

void UnsafeAuthorityAndSyntheticEvidenceFailClosed() {
  auto oracle_case =
      Case(opt::OptimizerCorrectnessClass::kInnerJoin, "embedded");
  oracle_case.baseline.synthetic_result = true;
  oracle_case.optimized.live_route_executed = false;
  oracle_case.baseline.result_contract_hash = "result-contract-v1";
  oracle_case.authority.transaction_finality_authority = true;
  oracle_case.authority.visibility_authority = true;
  oracle_case.authority.parser_authority = true;
  oracle_case.donor_reference_only = false;
  oracle_case.donor_as_authority = true;
  oracle_case.uses_donor_storage_or_finality_for_scratchbird = true;
  const auto validation =
      opt::ValidateOptimizerCorrectnessOracleCase(oracle_case);
  Require(!validation.ok,
          "CEIC-052 authority/synthetic/placeholder evidence accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "SYNTHETIC_PRODUCTION_RESULT"),
          "CEIC-052 synthetic production diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "RESULT_FIELDS_MISSING"),
          "CEIC-052 placeholder result-contract diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "DONOR_AUTHORITY_DRIFT"),
          "CEIC-052 donor authority diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-052 forbidden authority diagnostic missing");
}

void ClusterAndRouteGapsFailClosed() {
  auto local_cluster =
      Case(opt::OptimizerCorrectnessClass::kAggregation, "cluster");
  local_cluster.cluster_mode =
      opt::OptimizerCorrectnessClusterMode::kLocalClusterEvidence;
  const auto local_validation =
      opt::ValidateOptimizerCorrectnessOracleCase(local_cluster);
  Require(!local_validation.ok,
          "CEIC-052 local cluster correctness claim accepted");
  Require(HasDiagnostic(local_validation.diagnostics,
                        "LOCAL_CLUSTER_FORBIDDEN"),
          "CEIC-052 local cluster diagnostic missing");

  auto external_cluster =
      Case(opt::OptimizerCorrectnessClass::kAggregation, "embedded");
  external_cluster.cluster_mode =
      opt::OptimizerCorrectnessClusterMode::kExternalProviderDelegated;
  external_cluster.external_cluster_provider_id = "external-cluster-provider";
  external_cluster.cluster_claim_blocked = true;
  external_cluster.production_correctness_claim = false;
  const auto external_validation =
      opt::ValidateOptimizerCorrectnessOracleCase(external_cluster);
  Require(!external_validation.ok,
          "CEIC-052 external cluster correctness evidence must not close production correctness");
  Require(HasDiagnostic(external_validation.diagnostics,
                        "PRODUCTION_ROUTE_PROOF_MISSING"),
          "CEIC-052 external cluster claim-block diagnostic missing");

  auto suite = FullSuite();
  suite.pop_back();
  const auto gap_validation =
      opt::ValidateOptimizerCorrectnessOracleSuite(
          suite, opt::RequiredOptimizerCorrectnessClasses(),
          {"embedded", "ipc", "inet", "cli", "driver", "missing_route"});
  Require(!gap_validation.ok, "CEIC-052 route/class gap accepted");
  Require(HasDiagnostic(gap_validation.diagnostics, "MISSING_CLASS"),
          "CEIC-052 missing class diagnostic missing");
  Require(HasDiagnostic(gap_validation.diagnostics, "MISSING_ROUTE"),
          "CEIC-052 missing route diagnostic missing");
}

}  // namespace

int main() {
  FullSuiteIsProven();
  ResultContractsMustMatch();
  EmptyResultsCanBeProven();
  SpecializedProofsAreMandatory();
  DmlLocatorProofIsMandatory();
  UnsafeAuthorityAndSyntheticEvidenceFailClosed();
  ClusterAndRouteGapsFailClosed();
  std::cout << "ceic_052_optimizer_correctness_oracle_suite_gate=pass\n";
  return EXIT_SUCCESS;
}
