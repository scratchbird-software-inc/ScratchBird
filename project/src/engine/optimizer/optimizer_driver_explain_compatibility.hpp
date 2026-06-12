// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_benchmark_evidence_schema.hpp"
#include "runtime_consumption_evidence.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: CEIC_057_DRIVER_VISIBLE_EXPLAIN_COMPATIBILITY
// Driver-visible explain compatibility records are observability evidence only.
// They prove stable redacted JSON explain parity across engine and driver
// routes. They are not transaction finality, visibility, authorization/security,
// recovery, parser, reference, WAL, benchmark, optimizer-plan, index-finality,
// provider-finality, cluster, or agent-action authority.
inline constexpr const char* kOptimizerDriverExplainCompatibilitySchemaId =
    "sb.optimizer.driver_visible_explain_compatibility.v1";
inline constexpr const char* kDriverVisibleExplainJsonSchemaId =
    "sb.optimizer.explain.driver_visible_json.v1";
inline constexpr std::uint32_t
    kOptimizerDriverExplainCompatibilitySchemaMajor = 1;
inline constexpr std::uint32_t
    kOptimizerDriverExplainCompatibilitySchemaMinor = 0;
inline constexpr std::uint32_t kDriverVisibleExplainJsonSchemaMajor = 1;
inline constexpr std::uint32_t kDriverVisibleExplainJsonSchemaMinor = 0;

struct OptimizerExplainCompatibilityAuthorityFlags {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

enum class OptimizerExplainCompatibilityClusterMode {
  kNoCluster,
  kExternalProviderDelegated,
  kLocalClusterEvidence,
};

struct OptimizerDriverVisibleExplainRouteRecord {
  std::string route_kind;
  std::string route_label;
  bool embedded_reference_route = false;
  bool claimed_driver_route = false;
  std::string claimed_driver_name;
  bool driver_visible_route = true;

  std::string explain_schema_id = kDriverVisibleExplainJsonSchemaId;
  std::uint32_t explain_schema_version_major =
      kDriverVisibleExplainJsonSchemaMajor;
  std::uint32_t explain_schema_version_minor =
      kDriverVisibleExplainJsonSchemaMinor;
  std::string explain_json;
  std::string json_canonicalization_digest;

  std::string plan_hash;
  std::string plan_evidence_digest;
  std::string explain_digest;
  std::string result_contract_hash;
  std::string result_hash;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;

  std::string redaction_digest;
  bool redaction_applied = false;
  bool no_sql_text_leak = false;
  bool no_raw_uuid_leak = false;
  bool no_protected_material_leak = false;
  std::vector<std::string> redaction_proofs;

  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t provider_generation = 0;
  std::uint64_t source_generation = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 0;

  std::string optimizer_profile;
  std::string source_provenance;
  std::string provenance_digest;
  std::string evidence_digest;
  bool trusted_provenance = false;
  bool fresh = false;
  bool evidence_only = true;

  bool placeholder_runtime_evidence = false;
  bool synthetic_statistics = false;
  bool local_default_statistics = false;
  bool policy_default_statistics = false;

  bool ceic_051_benchmark_evidence_attached = false;
  PersistedOptimizerBenchmarkEvidenceRecord benchmark_evidence;

  OptimizerExplainCompatibilityAuthorityFlags authority;
};

struct OptimizerDriverVisibleExplainCompatibilityReport {
  std::string schema_id = kOptimizerDriverExplainCompatibilitySchemaId;
  std::uint32_t schema_version_major =
      kOptimizerDriverExplainCompatibilitySchemaMajor;
  std::uint32_t schema_version_minor =
      kOptimizerDriverExplainCompatibilitySchemaMinor;

  std::string report_id;
  std::string dataset_schema_digest;
  std::string sblr_digest;
  std::string logical_plan_hash;
  std::string optimizer_profile;
  std::string evidence_digest;
  std::string provenance_digest;
  bool trusted_provenance = false;
  bool fresh = false;
  bool evidence_only = true;
  bool production_compatibility_claim = false;
  bool require_claimed_driver_route = true;
  std::vector<std::string> required_route_kinds;
  std::vector<std::string> claimed_driver_names;

  OptimizerExplainCompatibilityClusterMode cluster_mode =
      OptimizerExplainCompatibilityClusterMode::kNoCluster;
  std::string external_cluster_provider_id;
  bool cluster_claim_blocked = false;

  OptimizerExplainCompatibilityAuthorityFlags authority;
  std::vector<OptimizerDriverVisibleExplainRouteRecord> routes;
};

struct OptimizerDriverVisibleExplainCompatibilityValidation {
  bool ok = false;
  bool compatibility_proven = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

OptimizerDriverVisibleExplainCompatibilityValidation
ValidateOptimizerDriverVisibleExplainRouteRecord(
    const OptimizerDriverVisibleExplainRouteRecord& record);

OptimizerDriverVisibleExplainCompatibilityValidation
ValidateOptimizerDriverVisibleExplainCompatibilityReport(
    const OptimizerDriverVisibleExplainCompatibilityReport& report);

}  // namespace scratchbird::engine::optimizer
