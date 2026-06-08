// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: CEIC_052_OPTIMIZER_CORRECTNESS_ORACLE
// Optimizer correctness-oracle evidence compares an engine baseline result with
// an optimized engine result. It is evidence only. It is not transaction
// finality, visibility, authorization/security, recovery, parser, donor, WAL,
// benchmark, optimizer-plan, index-finality, provider-finality, cluster, or
// agent-action authority.
inline constexpr const char* kOptimizerCorrectnessOracleSchemaId =
    "sb.optimizer.correctness_oracle.v1";
inline constexpr std::uint32_t kOptimizerCorrectnessOracleSchemaMajor = 1;
inline constexpr std::uint32_t kOptimizerCorrectnessOracleSchemaMinor = 0;

enum class OptimizerCorrectnessClass {
  kInnerJoin,
  kOuterJoin,
  kSemiJoin,
  kAntiJoin,
  kCorrelatedDependency,
  kAggregation,
  kDistinct,
  kWindow,
  kTopN,
  kDmlLocator,
  kDocumentPath,
  kVector,
  kTextSearch,
  kGraph,
  kMixedFusion,
};

enum class OptimizerCorrectnessClusterMode {
  kNoCluster,
  kExternalProviderDelegated,
  kLocalClusterEvidence,
};

struct OptimizerCorrectnessAuthorityFlags {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct OptimizerResultOracleEvidence {
  std::string producer_label;
  std::string result_contract_hash;
  std::string result_hash;
  std::uint64_t result_row_count = 0;
  bool result_row_count_observed = false;
  std::string ordering_contract_hash;
  std::string null_semantics_hash;
  std::string error_diagnostic_code;
  std::string diagnostic_digest;
  std::string row_locator_contract_hash;
  std::string recheck_evidence_digest;
  bool accepted = true;
  bool live_route_executed = false;
  bool synthetic_result = false;
  bool exact_recheck_proven = false;
  bool exact_rerank_proven = false;
  bool mga_recheck_proven = false;
  bool security_recheck_proven = false;
  bool row_locator_mga_snapshot_proven = false;
};

struct OptimizerCorrectnessOracleCase {
  std::string schema_id = kOptimizerCorrectnessOracleSchemaId;
  std::uint32_t schema_version_major =
      kOptimizerCorrectnessOracleSchemaMajor;
  std::uint32_t schema_version_minor =
      kOptimizerCorrectnessOracleSchemaMinor;

  std::string case_id;
  OptimizerCorrectnessClass correctness_class =
      OptimizerCorrectnessClass::kInnerJoin;
  std::string route_kind;
  std::string route_label;
  std::string dataset_schema_digest;
  std::string sblr_digest;
  std::string logical_plan_hash;
  std::string baseline_plan_hash;
  std::string optimized_plan_hash;
  std::string equivalence_contract_hash;
  std::string correlation_dependency_contract_hash;

  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t provider_generation = 0;

  OptimizerResultOracleEvidence baseline;
  OptimizerResultOracleEvidence optimized;

  bool production_correctness_claim = false;
  bool evidence_only = true;
  bool baseline_is_engine_reference_route = false;
  bool optimized_route_consumed = false;
  bool exact_recheck_required = false;
  bool exact_rerank_required = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool donor_reference_only = true;
  bool donor_as_authority = false;
  bool uses_donor_storage_or_finality_for_scratchbird = false;

  OptimizerCorrectnessClusterMode cluster_mode =
      OptimizerCorrectnessClusterMode::kNoCluster;
  std::string external_cluster_provider_id;
  bool cluster_claim_blocked = false;

  OptimizerCorrectnessAuthorityFlags authority;
};

struct OptimizerCorrectnessOracleValidation {
  bool ok = false;
  bool correctness_proven = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

const char* OptimizerCorrectnessClassName(OptimizerCorrectnessClass value);

std::vector<OptimizerCorrectnessClass> RequiredOptimizerCorrectnessClasses();

OptimizerCorrectnessOracleValidation ValidateOptimizerCorrectnessOracleCase(
    const OptimizerCorrectnessOracleCase& oracle_case);

OptimizerCorrectnessOracleValidation ValidateOptimizerCorrectnessOracleSuite(
    const std::vector<OptimizerCorrectnessOracleCase>& oracle_cases,
    const std::vector<OptimizerCorrectnessClass>& required_classes,
    const std::vector<std::string>& required_routes);

}  // namespace scratchbird::engine::optimizer
