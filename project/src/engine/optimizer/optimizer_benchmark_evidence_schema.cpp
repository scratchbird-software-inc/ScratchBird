// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_benchmark_evidence_schema.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

bool Empty(std::string_view value) {
  return value.empty();
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool Positive(double value) {
  return value > 0.0;
}

bool SameMetric(double lhs, double rhs) {
  return std::abs(lhs - rhs) <= 0.0001;
}

double NearestRankPercentile(std::vector<double> samples,
                             double percentile) {
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  const auto raw_rank =
      static_cast<std::size_t>(std::ceil((percentile / 100.0) *
                                         static_cast<double>(samples.size())));
  const auto index = raw_rank == 0 ? 0 : raw_rank - 1;
  return samples[std::min(index, samples.size() - 1)];
}

void RequireField(PersistedOptimizerBenchmarkEvidenceValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

void AddDiagnostic(PersistedOptimizerBenchmarkEvidenceValidation* validation,
                   std::string diagnostic) {
  validation->diagnostics.push_back(std::move(diagnostic));
}

std::string RecordPrefix(const PersistedOptimizerBenchmarkEvidenceRecord& record) {
  if (!record.artifact_uuid.empty()) return record.artifact_uuid;
  if (!record.route_lane.empty()) return record.route_lane;
  return record.route_kind.empty() ? "unnamed_optimizer_benchmark_record"
                                   : record.route_kind;
}

std::string SamplePrefix(
    const OptimizerBenchmarkSampleGroupEvidence& sample) {
  if (!sample.sample_group_id.empty()) return sample.sample_group_id;
  return sample.cache_phase.empty() ? "unnamed_sample_group"
                                    : sample.cache_phase;
}

std::string ReferencePrefix(
    const OptimizerBenchmarkReferenceMethodologyEvidence& reference) {
  return reference.reference_engine.empty() ? "unnamed_reference" : reference.reference_engine;
}

bool HasAuthorityDrift(
    const OptimizerBenchmarkEvidenceAuthorityFlags& authority) {
  return authority.transaction_finality_authority ||
         authority.visibility_authority ||
         authority.authorization_security_authority ||
         authority.recovery_authority ||
         authority.parser_authority ||
         authority.reference_authority ||
         authority.wal_authority ||
         authority.benchmark_authority ||
         authority.optimizer_plan_authority ||
         authority.index_finality_authority ||
         authority.provider_finality_authority ||
         authority.local_cluster_authority ||
         authority.cluster_authority ||
         authority.agent_action_authority;
}

bool IsPlaceholderResultContract(std::string_view value) {
  return value.empty() || value == "result-contract-v1" ||
         value == "sha256:result-contract-v1";
}

bool IsHashLike(std::string_view value) {
  return StartsWith(value, "sha256:");
}

bool ContainsEmptyToggle(const std::vector<std::string>& toggles) {
  return std::any_of(toggles.begin(), toggles.end(), [](const auto& value) {
    return value.empty();
  });
}

void ValidateSampleGroup(
    PersistedOptimizerBenchmarkEvidenceValidation* validation,
    const OptimizerBenchmarkSampleGroupEvidence& sample) {
  const auto prefix = SamplePrefix(sample);
  if (sample.sample_group_id.empty()) {
    AddDiagnostic(validation, prefix + ":SB_OPT_BENCHMARK_SCHEMA.SAMPLE_ID_MISSING");
  }
  if (sample.cache_phase != "cold" && sample.cache_phase != "warm") {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.CACHE_PHASE_INVALID");
  }
  if (sample.cache_phase == "cold" && !sample.cold_cache_reset_proven) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.COLD_RESET_UNPROVEN");
  }
  if (sample.cache_phase == "warm" && !sample.warm_cache_prepared_proven) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.WARM_PREPARE_UNPROVEN");
  }
  if (sample.latency_us_samples.size() < 5) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.SAMPLES_INSUFFICIENT");
  }
  if (!Positive(sample.p50_us) || !Positive(sample.p95_us) ||
      !Positive(sample.p99_us) ||
      std::any_of(sample.latency_us_samples.begin(),
                  sample.latency_us_samples.end(),
                  [](double value) { return value <= 0.0; })) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.LATENCY_SAMPLE_INVALID");
  } else if (!sample.latency_us_samples.empty()) {
    if (!SameMetric(sample.p50_us,
                    NearestRankPercentile(sample.latency_us_samples, 50.0)) ||
        !SameMetric(sample.p95_us,
                    NearestRankPercentile(sample.latency_us_samples, 95.0)) ||
        !SameMetric(sample.p99_us,
                    NearestRankPercentile(sample.latency_us_samples, 99.0)) ||
        sample.p50_us > sample.p95_us || sample.p95_us > sample.p99_us) {
      AddDiagnostic(validation,
                    prefix + ":SB_OPT_BENCHMARK_SCHEMA.PERCENTILE_MISMATCH");
    }
  }
  if (sample.cpu_user_us + sample.cpu_system_us == 0) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.CPU_COUNTER_MISSING");
  }
  if (sample.memory_peak_bytes == 0 || sample.memory_reserved_bytes == 0 ||
      sample.memory_released_bytes == 0) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.MEMORY_COUNTER_MISSING");
  }
  if (sample.io_read_bytes + sample.io_write_bytes == 0 ||
      sample.io_read_ops + sample.io_write_ops == 0) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.IO_COUNTER_MISSING");
  }
  if (sample.cache_hits + sample.cache_misses == 0 ||
      sample.page_cache_hits + sample.page_cache_misses == 0) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.CACHE_COUNTER_MISSING");
  }
  if (!Positive(sample.estimated_rows) || !Positive(sample.actual_rows) ||
      !Positive(sample.estimate_actual_ratio)) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.CARDINALITY_MISSING");
  }
  if (sample.skew_profile.empty() || sample.cardinality_profile.empty()) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.SKEW_CARDINALITY_MISSING");
  }
  if (sample.metric_snapshot_digest.empty() || sample.profiler_digest.empty()) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.METRIC_DIGEST_MISSING");
  }
}

void ValidateReferenceMethodology(
    PersistedOptimizerBenchmarkEvidenceValidation* validation,
    const OptimizerBenchmarkReferenceMethodologyEvidence& reference) {
  const auto prefix = ReferencePrefix(reference);
  if (reference.reference_engine.empty() || reference.reference_version.empty() ||
      reference.reference_native_method.empty()) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.REFERENCE_IDENTITY_MISSING");
  }
  if (reference.dataset_schema_mapping_digest.empty() ||
      reference.workload_mapping_digest.empty() ||
      reference.route_equivalence_contract_hash.empty() ||
      reference.reference_result_hash.empty()) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.REFERENCE_MAPPING_MISSING");
  }
  if (reference.reference_transaction_policy.empty() ||
      reference.reference_timing_policy.empty()) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.REFERENCE_METHOD_MISSING");
  }
  if (reference.comparable_status != "comparable" &&
      reference.comparable_status != "non_comparable") {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.REFERENCE_COMPARABILITY_INVALID");
  }
  if (reference.comparable_status == "non_comparable" &&
      reference.non_comparable_reason.empty()) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.NON_COMPARABLE_REASON_MISSING");
  }
  if (!reference.reference_reference_only || reference.reference_as_authority ||
      reference.uses_reference_storage_or_finality_for_scratchbird) {
    AddDiagnostic(validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.REFERENCE_AUTHORITY_DRIFT");
  }
}

}  // namespace

// SEARCH_KEY: ValidatePersistedOptimizerBenchmarkEvidenceRecord
PersistedOptimizerBenchmarkEvidenceValidation
ValidatePersistedOptimizerBenchmarkEvidenceRecord(
    const PersistedOptimizerBenchmarkEvidenceRecord& record) {
  PersistedOptimizerBenchmarkEvidenceValidation validation;
  const auto prefix = RecordPrefix(record);

  RequireField(&validation,
               record.schema_id == kOptimizerBenchmarkEvidenceSchemaId,
               "schema_id");
  RequireField(&validation,
               record.schema_version_major ==
                   kOptimizerBenchmarkEvidenceSchemaMajor,
               "schema_version_major");
  RequireField(&validation,
               record.schema_version_minor ==
                   kOptimizerBenchmarkEvidenceSchemaMinor,
               "schema_version_minor");
  RequireField(&validation, !Empty(record.artifact_uuid), "artifact_uuid");
  RequireField(&validation, !Empty(record.route_kind), "route_kind");
  RequireField(&validation, !Empty(record.route_lane), "route_lane");
  RequireField(&validation, !Empty(record.route_label), "route_label");
  RequireField(&validation,
               !Empty(record.dataset_schema_uuid),
               "dataset_schema_uuid");
  RequireField(&validation,
               IsHashLike(record.dataset_schema_digest),
               "dataset_schema_digest");
  RequireField(&validation,
               !Empty(record.dataset_schema_version),
               "dataset_schema_version");
  RequireField(&validation, IsHashLike(record.sblr_digest), "sblr_digest");
  RequireField(&validation,
               IsHashLike(record.logical_plan_hash),
               "logical_plan_hash");
  RequireField(&validation,
               IsHashLike(record.physical_plan_hash),
               "physical_plan_hash");
  RequireField(&validation,
               IsHashLike(record.plan_cache_key_hash),
               "plan_cache_key_hash");
  RequireField(&validation,
               !IsPlaceholderResultContract(record.result_contract_hash) &&
                   IsHashLike(record.result_contract_hash),
               "result_contract_hash");
  RequireField(&validation, IsHashLike(record.result_hash), "result_hash");
  RequireField(&validation, record.result_row_count > 0, "result_row_count");
  RequireField(&validation,
               !Empty(record.optimizer_profile),
               "optimizer_profile");
  RequireField(&validation,
               !record.optimizer_toggles.empty() &&
                   !ContainsEmptyToggle(record.optimizer_toggles),
               "optimizer_toggles");
  RequireField(&validation,
               IsHashLike(record.optimizer_toggles_digest),
               "optimizer_toggles_digest");
  RequireField(&validation,
               !Empty(record.benchmark_profile),
               "benchmark_profile");
  RequireField(&validation, !Empty(record.benchmark_run_id), "benchmark_run_id");
  RequireField(&validation, !Empty(record.runner_id), "runner_id");
  RequireField(&validation,
               IsHashLike(record.provenance_digest),
               "provenance_digest");
  RequireField(&validation, IsHashLike(record.evidence_digest), "evidence_digest");
  RequireField(&validation, IsHashLike(record.redaction_digest), "redaction_digest");
  RequireField(&validation, !Empty(record.retention_class), "retention_class");

  if (record.catalog_epoch <= 1 || record.security_epoch <= 1 ||
      record.redaction_epoch <= 1 || record.statistics_epoch <= 1 ||
      record.feedback_generation <= 1 ||
      record.memory_feedback_generation <= 1 ||
      record.provider_generation <= 1) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.PLACEHOLDER_EPOCH");
  }
  if (!record.trusted_provenance || !record.fresh ||
      !record.redaction_applied || !record.evidence_only ||
      record.freshness_microseconds == 0 ||
      record.max_freshness_microseconds == 0 ||
      record.freshness_microseconds > record.max_freshness_microseconds) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.PROVENANCE_FRESHNESS_INVALID");
  }
  if (record.placeholder_runtime_evidence || record.synthetic_statistics ||
      record.local_default_statistics || record.policy_default_statistics) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.SYNTHETIC_OR_PLACEHOLDER");
  }
  if (HasAuthorityDrift(record.authority)) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.FORBIDDEN_AUTHORITY");
  }

  if (record.cluster_mode == OptimizerBenchmarkClusterMode::kLocalClusterEvidence ||
      record.route_kind == "cluster") {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.LOCAL_CLUSTER_FORBIDDEN");
  } else if (record.cluster_mode ==
             OptimizerBenchmarkClusterMode::kExternalProviderDelegated) {
    if (record.external_cluster_provider_id.empty() ||
        !record.cluster_claim_blocked ||
        record.production_benchmark_clean_claim) {
      AddDiagnostic(
          &validation,
          prefix + ":SB_OPT_BENCHMARK_SCHEMA.EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED");
    }
  }

  bool saw_cold = false;
  bool saw_warm = false;
  if (record.sample_groups.empty()) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.SAMPLE_GROUPS_MISSING");
  }
  for (const auto& sample : record.sample_groups) {
    if (sample.cache_phase == "cold") saw_cold = true;
    if (sample.cache_phase == "warm") saw_warm = true;
    ValidateSampleGroup(&validation, sample);
  }
  if (!saw_cold || !saw_warm) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.COLD_WARM_REQUIRED");
  }

  if (record.reference_methodology.empty()) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_BENCHMARK_SCHEMA.REFERENCE_METHODOLOGY_MISSING");
  }
  for (const auto& reference : record.reference_methodology) {
    ValidateReferenceMethodology(&validation, reference);
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.benchmark_clean_admissible =
      validation.ok && record.production_benchmark_clean_claim &&
      record.cluster_mode == OptimizerBenchmarkClusterMode::kNoCluster;
  if (validation.ok) {
    validation.diagnostic_code =
        "SB_OPT_BENCHMARK_SCHEMA.OK";
  } else if (!validation.missing_fields.empty()) {
    validation.diagnostic_code =
        "SB_OPT_BENCHMARK_SCHEMA.MISSING_REQUIRED_FIELD";
  } else {
    validation.diagnostic_code =
        "SB_OPT_BENCHMARK_SCHEMA.INVALID_CONTRACT";
  }
  return validation;
}

PersistedOptimizerBenchmarkEvidenceValidation
ValidatePersistedOptimizerBenchmarkEvidenceSet(
    const std::vector<PersistedOptimizerBenchmarkEvidenceRecord>& records,
    const std::vector<std::string>& required_routes) {
  PersistedOptimizerBenchmarkEvidenceValidation validation;
  if (records.empty()) {
    validation.diagnostic_code = "SB_OPT_BENCHMARK_SCHEMA.EMPTY_RECORD_SET";
    validation.diagnostics.push_back(
        "SB_OPT_BENCHMARK_SCHEMA.EMPTY_RECORD_SET");
    return validation;
  }

  std::set<std::string> seen_artifacts;
  std::set<std::string> seen_routes;
  bool all_admissible = true;
  for (const auto& record : records) {
    const auto record_validation =
        ValidatePersistedOptimizerBenchmarkEvidenceRecord(record);
    if (!record.artifact_uuid.empty() &&
        !seen_artifacts.insert(record.artifact_uuid).second) {
      AddDiagnostic(&validation,
                    record.artifact_uuid +
                        ":SB_OPT_BENCHMARK_SCHEMA.DUPLICATE_ARTIFACT");
    }
    if (!record.route_kind.empty()) {
      seen_routes.insert(record.route_kind);
    }
    if (!record_validation.ok) {
      AddDiagnostic(&validation,
                    RecordPrefix(record) + ":" +
                        record_validation.diagnostic_code);
      validation.diagnostics.insert(validation.diagnostics.end(),
                                    record_validation.diagnostics.begin(),
                                    record_validation.diagnostics.end());
      validation.missing_fields.insert(
          validation.missing_fields.end(),
          record_validation.missing_fields.begin(),
          record_validation.missing_fields.end());
    }
    all_admissible = all_admissible &&
                     record_validation.benchmark_clean_admissible;
  }

  for (const auto& route : required_routes) {
    if (seen_routes.find(route) == seen_routes.end()) {
      AddDiagnostic(&validation,
                    route + ":SB_OPT_BENCHMARK_SCHEMA.MISSING_ROUTE");
    }
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.benchmark_clean_admissible = validation.ok && all_admissible;
  validation.diagnostic_code =
      validation.ok ? "SB_OPT_BENCHMARK_SCHEMA.SET_OK"
                    : "SB_OPT_BENCHMARK_SCHEMA.SET_INVALID";
  return validation;
}

}  // namespace scratchbird::engine::optimizer
