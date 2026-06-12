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

// SEARCH_KEY: CEIC_051_PERSISTED_OPTIMIZER_BENCHMARK_EVIDENCE_SCHEMA
// Versioned persisted benchmark evidence records are optimizer evidence only.
// They preserve benchmark route facts, result hashes, timing samples, counters,
// reference methodology, and provenance. They are not transaction finality,
// visibility, authorization/security, recovery, parser, reference, WAL,
// index-finality, provider-finality, cluster, or agent-action authority.
inline constexpr const char* kOptimizerBenchmarkEvidenceSchemaId =
    "sb.optimizer.benchmark_evidence.v1";
inline constexpr std::uint32_t kOptimizerBenchmarkEvidenceSchemaMajor = 1;
inline constexpr std::uint32_t kOptimizerBenchmarkEvidenceSchemaMinor = 0;

struct OptimizerBenchmarkEvidenceAuthorityFlags {
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

enum class OptimizerBenchmarkClusterMode {
  kNoCluster,
  kExternalProviderDelegated,
  kLocalClusterEvidence,
};

struct OptimizerBenchmarkSampleGroupEvidence {
  std::string sample_group_id;
  std::string cache_phase;
  std::vector<double> latency_us_samples;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;

  std::uint64_t cpu_user_us = 0;
  std::uint64_t cpu_system_us = 0;
  std::uint64_t memory_peak_bytes = 0;
  std::uint64_t memory_reserved_bytes = 0;
  std::uint64_t memory_released_bytes = 0;
  std::uint64_t io_read_bytes = 0;
  std::uint64_t io_write_bytes = 0;
  std::uint64_t io_read_ops = 0;
  std::uint64_t io_write_ops = 0;
  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  std::uint64_t page_cache_hits = 0;
  std::uint64_t page_cache_misses = 0;

  double estimated_rows = 0.0;
  double actual_rows = 0.0;
  double estimate_actual_ratio = 0.0;
  std::string skew_profile;
  std::string cardinality_profile;
  std::string metric_snapshot_digest;
  std::string profiler_digest;
  bool cold_cache_reset_proven = false;
  bool warm_cache_prepared_proven = false;
};

struct OptimizerBenchmarkReferenceMethodologyEvidence {
  std::string reference_engine;
  std::string reference_version;
  std::string reference_native_method;
  std::string comparable_status;
  std::string non_comparable_reason;
  std::string dataset_schema_mapping_digest;
  std::string workload_mapping_digest;
  std::string route_equivalence_contract_hash;
  std::string reference_result_hash;
  std::string reference_transaction_policy;
  std::string reference_timing_policy;
  bool reference_reference_only = true;
  bool reference_as_authority = false;
  bool uses_reference_storage_or_finality_for_scratchbird = false;
};

struct PersistedOptimizerBenchmarkEvidenceRecord {
  std::string schema_id = kOptimizerBenchmarkEvidenceSchemaId;
  std::uint32_t schema_version_major = kOptimizerBenchmarkEvidenceSchemaMajor;
  std::uint32_t schema_version_minor = kOptimizerBenchmarkEvidenceSchemaMinor;

  std::string artifact_uuid;
  std::string route_kind;
  std::string route_lane;
  std::string route_label;

  std::string dataset_schema_uuid;
  std::string dataset_schema_digest;
  std::string dataset_schema_version;
  std::string sblr_digest;
  std::string logical_plan_hash;
  std::string physical_plan_hash;
  std::string plan_cache_key_hash;
  std::string result_contract_hash;
  std::string result_hash;
  std::uint64_t result_row_count = 0;

  std::string optimizer_profile;
  std::vector<std::string> optimizer_toggles;
  std::string optimizer_toggles_digest;
  std::string benchmark_profile;
  std::string benchmark_run_id;
  std::string runner_id;

  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t feedback_generation = 0;
  std::uint64_t memory_feedback_generation = 0;
  std::uint64_t provider_generation = 0;

  std::string provenance_digest;
  std::string evidence_digest;
  std::string redaction_digest;
  std::string retention_class;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 0;
  bool trusted_provenance = false;
  bool fresh = false;
  bool redaction_applied = false;
  bool evidence_only = true;
  bool production_benchmark_clean_claim = false;

  bool placeholder_runtime_evidence = false;
  bool synthetic_statistics = false;
  bool local_default_statistics = false;
  bool policy_default_statistics = false;

  OptimizerBenchmarkClusterMode cluster_mode =
      OptimizerBenchmarkClusterMode::kNoCluster;
  std::string external_cluster_provider_id;
  bool cluster_claim_blocked = false;

  OptimizerBenchmarkEvidenceAuthorityFlags authority;
  std::vector<OptimizerBenchmarkSampleGroupEvidence> sample_groups;
  std::vector<OptimizerBenchmarkReferenceMethodologyEvidence> reference_methodology;
};

struct PersistedOptimizerBenchmarkEvidenceValidation {
  bool ok = false;
  bool benchmark_clean_admissible = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

PersistedOptimizerBenchmarkEvidenceValidation
ValidatePersistedOptimizerBenchmarkEvidenceRecord(
    const PersistedOptimizerBenchmarkEvidenceRecord& record);

PersistedOptimizerBenchmarkEvidenceValidation
ValidatePersistedOptimizerBenchmarkEvidenceSet(
    const std::vector<PersistedOptimizerBenchmarkEvidenceRecord>& records,
    const std::vector<std::string>& required_routes);

}  // namespace scratchbird::engine::optimizer
