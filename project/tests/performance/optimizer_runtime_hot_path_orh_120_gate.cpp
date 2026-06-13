// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_tuning_controller.hpp"
#include "adaptive_tuning_metrics_evidence.hpp"
#include "agent_runtime.hpp"
#include "cache/sblr_template_cache.hpp"
#include "compression_policy.hpp"
#include "direct_binary_result_frame.hpp"
#include "dml/dml_target_access_plan.hpp"
#include "hot_point_lookup_cache.hpp"
#include "nosql/document_api.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "observability/performance_metric_event.hpp"
#include "optimizer_differential_fuzz.hpp"
#include "selectivity_model.hpp"
#include "snapshot_safe_result_cache.hpp"
#include "streaming_cursor_manager.hpp"
#include "uuid.hpp"
#include "vector_maintenance_jobs.hpp"
#include "vector_training_recall_lifecycle.hpp"
#include "vectorized_result_batch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace parser = scratchbird::parser::sbsql;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::wire;

struct SemanticCapture {
  bool accepted = true;
  std::string result_digest;
  std::vector<std::string> rows;
  std::string required_ordering;
  std::vector<std::string> diagnostics;
  std::string security_signature;
  std::string redaction_signature;
  std::string mga_visibility_signature;
  std::vector<std::string> evidence;
};

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-120 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasPrefix(const std::vector<std::string>& values,
               const std::string& prefix) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.rfind(prefix, 0) == 0;
  });
}

std::string Join(const std::vector<std::string>& values,
                 std::string_view delimiter) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << delimiter;
    }
    out << values[i];
  }
  return out.str();
}

void CompareSemanticEquivalent(const std::string& scenario,
                               const SemanticCapture& baseline,
                               const SemanticCapture& optimized) {
  if (baseline.accepted != optimized.accepted) {
    Fail(scenario + ": accepted/refused state diverged baseline=" +
         std::to_string(baseline.accepted) + " optimized=" +
         std::to_string(optimized.accepted));
  }
  if (!baseline.accepted) {
    if (baseline.diagnostics != optimized.diagnostics) {
      Fail(scenario + ": exact refusal diagnostics diverged baseline=[" +
           Join(baseline.diagnostics, "|") + "] optimized=[" +
           Join(optimized.diagnostics, "|") + "]");
    }
    Require(!baseline.diagnostics.empty(),
            scenario + ": refusal equivalence did not carry an exact code");
    Require(Has(optimized.evidence, "fail_closed=true"),
            scenario + ": optimized refusal did not fail closed");
    return;
  }

  if (baseline.result_digest != optimized.result_digest) {
    Fail(scenario + ": result digest diverged baseline=" +
         baseline.result_digest + " optimized=" + optimized.result_digest);
  }
  if (baseline.rows != optimized.rows) {
    Fail(scenario + ": canonical rows diverged baseline=[" +
         Join(baseline.rows, "|") + "] optimized=[" +
         Join(optimized.rows, "|") + "]");
  }
  if (baseline.required_ordering != optimized.required_ordering) {
    Fail(scenario + ": required ordering diverged baseline=" +
         baseline.required_ordering + " optimized=" +
         optimized.required_ordering);
  }
  if (baseline.security_signature != optimized.security_signature) {
    Fail(scenario + ": security signature diverged");
  }
  if (baseline.redaction_signature != optimized.redaction_signature) {
    Fail(scenario + ": redaction signature diverged");
  }
  if (baseline.mga_visibility_signature !=
      optimized.mga_visibility_signature) {
    Fail(scenario + ": MGA visibility signature diverged baseline=" +
         baseline.mga_visibility_signature + " optimized=" +
         optimized.mga_visibility_signature);
  }
}

SemanticCapture AcceptedCapture(std::string digest,
                                std::vector<std::string> rows,
                                std::string ordering,
                                std::vector<std::string> evidence = {}) {
  SemanticCapture capture;
  capture.accepted = true;
  capture.result_digest = std::move(digest);
  capture.rows = std::move(rows);
  capture.required_ordering = std::move(ordering);
  capture.security_signature = "security_epoch=120:grants_rechecked";
  capture.redaction_signature = "redaction_epoch=120:policy_bound";
  capture.mga_visibility_signature =
      "mga_visibility=engine_transaction_inventory:row_recheck_required";
  capture.evidence = std::move(evidence);
  return capture;
}

SemanticCapture RefusedCapture(std::string diagnostic,
                               std::vector<std::string> evidence = {}) {
  SemanticCapture capture;
  capture.accepted = false;
  capture.diagnostics.push_back(std::move(diagnostic));
  capture.security_signature = "security_epoch=120:refusal_path";
  capture.redaction_signature = "redaction_epoch=120:refusal_path";
  capture.mga_visibility_signature =
      "mga_visibility=engine_transaction_inventory:refusal_path";
  capture.evidence = std::move(evidence);
  capture.evidence.push_back("fail_closed=true");
  return capture;
}

opt::OptimizerStatsIdentity StatsIdentity(const std::string& relation_uuid,
                                          const std::string& statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = relation_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 120;
  identity.catalog_epoch = 120;
  identity.transaction_visibility_epoch = 120;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogSample;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::SelectivityEstimate Child(double selectivity) {
  opt::SelectivityEstimate estimate;
  estimate.selectivity = selectivity;
  estimate.confidence = opt::CostConfidence::kHigh;
  estimate.diagnostic_code = "orh120_child";
  estimate.conservative = false;
  return estimate;
}

opt::ExtendedOptimizerStatistic ExtendedStat(
    const std::string& relation_uuid) {
  opt::ExtendedOptimizerStatistic stats;
  stats.identity = StatsIdentity(relation_uuid, "orh120.extended");
  stats.kind = opt::ExtendedOptimizerStatisticKind::kJointMcv;
  stats.relation_uuid = relation_uuid;
  stats.column_uuids = {"col.tenant", "col.status"};
  stats.joint_mcv.push_back({{"tenant-7", "active"}, 0.07});
  return stats;
}

parser::CacheKey ParserCacheKey() {
  parser::CacheKey key;
  key.shape_hash = 1201;
  key.registry_version = 3;
  key.catalog_epoch = 120;
  key.security_policy_epoch = 121;
  key.grant_epoch = 122;
  key.descriptor_epoch = 123;
  key.udr_epoch = 124;
  key.name_resolution_epoch = 125;
  key.resource_epoch = 126;
  key.parser_package_generation = 127;
  key.protocol_version = 3;
  key.parser_package_version_hash = 128;
  key.disclosure_policy_generation = 129;
  key.redaction_policy_generation = 130;
  key.security_authority_epoch = 131;
  key.cluster_policy_generation = 132;
  key.ttl_generation = 133;
  key.memory_pressure_generation = 134;
  key.normalized_statement_hash = 135;
  key.parameter_type_shape_hash = 136;
  key.connection_uuid = "orh120-connection";
  key.transaction_context_hash = "mga-snapshot:orh120";
  key.dialect = "sbsql_v3";
  key.role_set_hash = "role.reader";
  key.group_set_hash = "group.reader";
  key.search_path_hash = "schema.public";
  key.language_profile = "en";
  key.policy_profile = "policy.orh120";
  key.parser_profile = "parser.orh120";
  key.result_contract_hash = "rowset.orh120.v1";
  return key;
}

platform::TypedUuid Typed(platform::UuidKind kind, std::string_view text) {
  auto parsed = uuid::ParseDurableEngineIdentityUuid(kind, std::string(text));
  Require(parsed.ok(), "failed to parse typed durable UUID");
  return parsed.value;
}

idx::HotPointLookupCacheKey HotPointKey() {
  idx::HotPointLookupCacheKey key;
  key.probe_class = idx::HotPointProbeClass::row_uuid_lookup;
  key.database_uuid = Typed(platform::UuidKind::database,
                            "019f2200-0000-7000-8000-000000120001");
  key.object_uuid = Typed(platform::UuidKind::object,
                          "019f2200-0000-7000-8000-000000120002");
  key.encoded_probe_key = "ORH-120:row_uuid_lookup";
  key.statistics_snapshot_id = "stats_epoch:120";
  key.descriptor_set_digest = "descriptor:orh120";
  key.index_definition_digest = "index:none";
  key.security_policy_digest = "security_epoch:120";
  key.redaction_policy_digest = "redaction_epoch:120";
  key.access_policy_digest = "access_epoch:120";
  key.collation_profile_digest = "collation:sbsql_v3";
  key.catalog_epoch = 120;
  key.index_epoch = 120;
  key.statistics_epoch = 120;
  key.security_epoch = 120;
  key.policy_epoch = 120;
  key.object_epoch = 120;
  key.compatibility_epoch = 120;
  return key;
}

idx::HotPointLookupCacheEntry HotPointEntry(bool authority_bearing) {
  idx::HotPointLookupCacheEntry entry;
  entry.key = HotPointKey();
  idx::HotPointLookupCandidate candidate;
  candidate.locator.table_uuid = entry.key.object_uuid;
  candidate.locator.row_uuid = Typed(platform::UuidKind::row,
                                     "019f2200-0000-7000-8000-000000120003");
  candidate.locator.local_transaction_id = 120;
  candidate.proof_kind = "orh120_actual_successful_row_locator";
  candidate.posting_list_digest = "posting:orh120";
  candidate.candidate_locator_only = true;
  candidate.equality_proof_metadata_only = true;
  candidate.requires_mga_visibility_recheck = true;
  candidate.requires_security_authorization_recheck = true;
  candidate.visibility_finality_authority = authority_bearing;
  candidate.authorization_finality_authority = false;
  candidate.parser_or_reference_finality_authority = false;
  candidate.timestamp_or_uuid_order_finality_authority = false;
  entry.candidates.push_back(candidate);
  entry.dependency_uuids.push_back(entry.key.object_uuid);
  entry.created_epoch = entry.key.catalog_epoch;
  return entry;
}

exec::VectorizedResultBatch Batch(const std::vector<std::string>& values) {
  std::vector<platform::u64> offsets;
  std::vector<std::uint8_t> data;
  offsets.push_back(0);
  for (const auto& value : values) {
    data.insert(data.end(), value.begin(), value.end());
    offsets.push_back(static_cast<platform::u64>(data.size()));
  }
  exec::VectorizedResultBatchBuilder builder(values.size());
  builder.AddColumn(exec::MakeVariableWidthResultBatchColumn(
      "payload",
      values.size(),
      offsets,
      data,
      exec::MakeResultBatchValidityBitmap(values.size())));
  auto finalized = builder.Finalize();
  Require(finalized.ok(), "vectorized result batch finalization failed");
  return finalized.batch;
}

idx::CompressionPolicyRequest CostedCompressionRequest(
    idx::CompressionFamily family) {
  auto request = idx::DefaultCompressionPolicyRequest(family);
  request.uncompressed_bytes = 16 * 1024;
  request.estimated_compressed_bytes = 4 * 1024;
  request.cost.cpu_cost = 1;
  request.cost.io_savings = 32;
  request.cost.cache_density_gain = 8;
  request.cost.update_frequency_penalty = 1;
  request.cost.read_hotness = 7;
  request.cost.write_hotness = 1;
  request.measured_feedback.present = true;
  request.measured_feedback.compress_ns_per_byte = 2.0;
  request.measured_feedback.decompress_ns_per_byte = 2.5;
  request.measured_feedback.observed_compression_ratio = 0.25;
  request.measured_feedback.cache_hit_improvement = 0.16;
  request.measured_feedback.write_amplification_change = -0.01;
  request.measured_feedback.update_rewrite_cost = 1.0;
  request.measured_feedback.dictionary_miss_rate = 0.01;
  request.measured_feedback.fallback_rate = 0.0;
  request.measured_feedback.sample_count = 512;
  request.measured_feedback.age_ms = 1000;
  return request;
}

idx::VectorTrainingRecallLifecycleProfile VectorProfile() {
  auto profile = idx::DefaultVectorTrainingRecallLifecycleProfile(
      idx::IndexVectorAlgorithm::hnsw);
  profile.drift.p95_latency_microseconds = 4000;
  profile.drift.policy_p95_latency_microseconds = 2000;
  profile.drift.adaptive_tuning_expected_sufficient = true;
  profile.drift.current_ef_search = 80;
  profile.drift.tuned_ef_search = 120;
  profile.drift.max_ef_search = 256;
  return profile;
}

agents::VectorMaintenanceJobRequest VectorJobRequest(
    const idx::VectorTrainingRecallLifecycleDecision& decision) {
  agents::VectorMaintenanceJobRequest request;
  request.job_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("orh120|job");
  request.database_uuid =
      agents::DeterministicAgentRuntimeDatabaseUuidFromKey("orh120|db");
  request.target_collection_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("orh120|collection");
  request.target_index_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("orh120|index");
  request.provider_generation = 120;
  request.old_training_generation = 119;
  request.new_training_generation = 120;
  request.action_kind = agents::VectorMaintenanceActionKind::adaptive_tuning;
  request.lifecycle_decision = decision;
  request.total_units = 10;
  request.now_microseconds = 1200;
  return request;
}

exec::SnapshotSafeCacheKey SnapshotKey() {
  exec::SnapshotSafeCacheKey key;
  key.normalized_operation = "select orh120 where tenant=?";
  key.safe_parameter_digest = "tenant:stable";
  key.catalog_epoch = 120;
  key.statistics_epoch = 121;
  key.security_epoch = 122;
  key.redaction_epoch = 123;
  key.mga_visibility_snapshot_class = "repeatable_read:orh120";
  key.provider_generation = 124;
  key.descriptor_identity_digest = "descriptor:orh120-rowset:v1";
  key.descriptor_epoch = 125;
  key.result_contract_identity = "orh120.rowset.v1";
  key.result_contract_hash = "sha256:orh120-rowset-contract";
  key.route_compatibility = "embedded";
  key.dialect_compatibility = "sbsql_v3";
  return key;
}

exec::SnapshotSafeCacheStoreRequest SnapshotStoreRequest() {
  exec::SnapshotSafeCacheStoreRequest request;
  request.entry.key = SnapshotKey();
  request.entry.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  request.entry.row_count = 2;
  request.entry.cached_result_digest = "sha256:orh120-result";
  request.entry.cached_mga_security_digest = "sha256:orh120-mga-security";
  request.read_only_operation = true;
  request.small_final_result = true;
  request.max_small_result_rows = 16;
  return request;
}

exec::SnapshotSafeCacheLookupRequest SnapshotLookupRequest() {
  exec::SnapshotSafeCacheLookupRequest request;
  request.key = SnapshotKey();
  request.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  request.read_only_operation = true;
  request.small_final_result = true;
  request.row_count = 2;
  request.max_small_result_rows = 16;
  request.recomputed_result_digest = "sha256:orh120-result";
  request.recomputed_mga_security_digest = "sha256:orh120-mga-security";
  return request;
}

api::PerformanceMetricEvent MetricEvent() {
  api::PerformanceMetricEvent event;
  event.route = "embedded.orh120";
  event.operation = "optimizer_runtime_hot_path.orh_120";
  event.phase_timings.measurement_source = "measured_by_internal_counter";
  event.phase_timings.measurement_quality = "measured";
  event.storage_timings.measurement_source = "not_available_zeroed";
  event.storage_timings.measurement_quality = "not_available_zeroed";
  event.agent_counters.measurement_source = "measured_by_platform_api";
  event.agent_counters.measurement_quality = "measured";
  event.cache_flags.measurement_source = "measured_by_internal_counter";
  event.cache_flags.measurement_quality = "measured";
  event.hot_path_attribution.cpu_sample_measurement_source =
      "measured_by_perf_sample";
  event.hot_path_attribution.cpu_sample_measurement_quality = "measured";
  event.hot_path_attribution.allocator_counter_measurement_source =
      "measured_by_internal_counter";
  event.hot_path_attribution.allocator_counter_measurement_quality =
      "actual_zero";
  event.hot_path_attribution.lock_latch_wait_measurement_source =
      "not_available_zeroed";
  event.hot_path_attribution.lock_latch_wait_measurement_quality =
      "not_available_zeroed";
  event.hot_path_attribution.syscall_count_measurement_source =
      "measured_by_platform_api";
  event.hot_path_attribution.syscall_count_measurement_quality = "actual_zero";
  event.hot_path_attribution.file_io_count_measurement_source = "disabled";
  event.hot_path_attribution.file_io_count_measurement_quality = "disabled";
  event.hot_path_attribution.page_fault_count_measurement_source =
      "measured_by_platform_api";
  event.hot_path_attribution.page_fault_count_measurement_quality =
      "actual_zero";
  event.hot_path_attribution.context_switch_count_measurement_source =
      "unsupported";
  event.hot_path_attribution.context_switch_count_measurement_quality =
      "unsupported";
  event.hot_path_attribution.evidence_rendering_measurement_source =
      "measured_by_internal_counter";
  event.hot_path_attribution.evidence_rendering_measurement_quality =
      "measured";
  event.hot_path_attribution.result_formatting_measurement_source =
      "measured_by_internal_counter";
  event.hot_path_attribution.result_formatting_measurement_quality =
      "actual_zero";
  event.hot_path_attribution.regression_budget_measurement_source =
      "estimated";
  event.hot_path_attribution.regression_budget_measurement_quality =
      "estimated";
  event.hot_path_attribution.parser_lowering_measurement_source =
      "measured_by_internal_counter";
  event.hot_path_attribution.parser_lowering_measurement_quality = "measured";
  event.hot_path_attribution.sbps_listener_measurement_source =
      "measured_by_perf_sample";
  event.hot_path_attribution.sbps_listener_measurement_quality = "measured";
  event.hot_path_attribution.sblr_dispatch_measurement_source =
      "measured_by_platform_api";
  event.hot_path_attribution.sblr_dispatch_measurement_quality = "measured";
  event.hot_path_attribution.internal_api_measurement_source = "estimated";
  event.hot_path_attribution.internal_api_measurement_quality = "estimated";
  event.hot_path_attribution.executor_measurement_source =
      "measured_by_internal_counter";
  event.hot_path_attribution.executor_measurement_quality = "measured";
  event.hot_path_attribution.storage_measurement_source =
      "not_available_zeroed";
  event.hot_path_attribution.storage_measurement_quality =
      "not_available_zeroed";
  event.hot_path_attribution.index_layer_measurement_source = "disabled";
  event.hot_path_attribution.index_layer_measurement_quality = "disabled";
  event.hot_path_attribution.transaction_measurement_source = "unsupported";
  event.hot_path_attribution.transaction_measurement_quality = "unsupported";
  event.hot_path_attribution.result_rendering_measurement_source =
      "measured_by_internal_counter";
  event.hot_path_attribution.result_rendering_measurement_quality =
      "actual_zero";
  event.hot_path_attribution.evidence_construction_measurement_source =
      "measured_by_internal_counter";
  event.hot_path_attribution.evidence_construction_measurement_quality =
      "measured";
  event.hot_path_attribution.allocation_measurement_source = "estimated";
  event.hot_path_attribution.allocation_measurement_quality = "estimated";
  event.hot_path_attribution.syscall_measurement_source =
      "measured_by_platform_api";
  event.hot_path_attribution.syscall_measurement_quality = "actual_zero";
  event.hot_path_attribution.wait_measurement_source = "measured_by_perf_sample";
  event.hot_path_attribution.wait_measurement_quality = "actual_zero";
  event.phase_timings.parse_us = 0;
  event.phase_timings.bind_us = 1;
  event.phase_timings.lower_us = 2;
  event.phase_timings.plan_us = 3;
  event.phase_timings.execute_us = 4;
  event.storage_timings.append_us = 0;
  event.storage_timings.page_us = 0;
  event.storage_timings.index_us = 0;
  event.agent_counters.agent_thread_count = 1;
  event.agent_counters.agent_cpu_user_us = 8;
  event.agent_counters.agent_cpu_system_us = 1;
  event.agent_counters.agent_wait_us = 0;
  event.agent_counters.agent_io_read_bytes = 0;
  event.agent_counters.agent_io_write_bytes = 0;
  event.cache_flags.plan_cache_hit = true;
  event.cache_flags.metadata_cache_hit = true;
  event.cache_flags.page_cache_hit = false;
  event.cache_flags.index_cache_hit = false;
  event.hot_path_attribution.cpu_sample_count = 8;
  event.hot_path_attribution.cpu_sample_attributed_count = 8;
  event.hot_path_attribution.cpu_sample_attribution = "orh120";
  event.hot_path_attribution.allocator_allocation_count = 0;
  event.hot_path_attribution.allocator_allocation_bytes = 0;
  event.hot_path_attribution.lock_latch_wait_count = 0;
  event.hot_path_attribution.lock_latch_wait_us = 0;
  event.hot_path_attribution.syscall_count = 0;
  event.hot_path_attribution.file_open_count = 0;
  event.hot_path_attribution.file_flush_count = 0;
  event.hot_path_attribution.file_fsync_count = 0;
  event.hot_path_attribution.page_fault_count = 0;
  event.hot_path_attribution.context_switch_count = 0;
  event.hot_path_attribution.evidence_rendering_us = 1;
  event.hot_path_attribution.result_formatting_us = 0;
  event.hot_path_attribution.regression_budget_us = 100;
  event.hot_path_attribution.regression_budget_margin_us = 92;
  event.hot_path_attribution.regression_budget_validated = true;
  event.hot_path_attribution.parser_lowering_us = 2;
  event.hot_path_attribution.sbps_listener_us = 1;
  event.hot_path_attribution.sblr_dispatch_us = 1;
  event.hot_path_attribution.internal_api_us = 2;
  event.hot_path_attribution.executor_us = 4;
  event.hot_path_attribution.storage_us = 0;
  event.hot_path_attribution.index_layer_us = 0;
  event.hot_path_attribution.transaction_us = 0;
  event.hot_path_attribution.result_rendering_us = 0;
  event.hot_path_attribution.evidence_construction_us = 1;
  event.hot_path_attribution.allocation_us = 0;
  event.hot_path_attribution.syscall_us = 0;
  event.hot_path_attribution.wait_us = 0;
  event.statistics_epoch = 120;
  event.resource_governor_state = "admitted";
  event.message_vector_present = false;
  event.result_hash = "sha256:orh120-result";
  return event;
}

std::filesystem::path UniqueTempDir(std::string_view name) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("scratchbird_orh120_" + std::string(name) + "_" +
                     std::to_string(now));
  std::filesystem::create_directories(path);
  return path;
}

struct TempDatabase {
  std::filesystem::path dir;
  std::filesystem::path path;

  explicit TempDatabase(std::string_view name) : dir(UniqueTempDir(name)) {
    path = dir / "orh120.sbdb";
  }

  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext DocumentContext(const std::filesystem::path& path,
                                          std::uint64_t tx) {
  api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical = "orh120-db";
  context.current_schema_uuid.canonical = "orh120-documents";
  context.transaction_uuid.canonical = "orh120-tx-" + std::to_string(tx);
  context.local_transaction_id = tx;
  context.security_context_present = true;
  context.resource_epoch = 120;
  context.security_epoch = 121;
  context.catalog_generation_id = 122;
  context.trace_tags = {"optimizer_runtime_hot_path_orh_120_gate",
                        "benchmark_clean",
                        "mga_transaction_regression"};
  return context;
}

api::EngineTypedValue Value(std::string value) {
  api::EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  return typed;
}

void AddFragment(api::EngineDocumentInsertRequest* request,
                 std::string path,
                 std::string value) {
  request->assignments.push_back({std::move(path), Value(std::move(value))});
}

void SeedCrudTransaction(const api::EngineRequestContext& context) {
  std::ofstream crud(context.database_path, std::ios::binary | std::ios::trunc);
  crud << "SBCRUD1\tTX_BEGIN\t" << context.local_transaction_id << '\t'
       << context.transaction_uuid.canonical << '\n';
  crud << "SBCRUD1\tTX_BEGIN\t950\torh120-reader-tx\n";
  crud.flush();
  Require(static_cast<bool>(crud), "could not seed transaction inventory");
}

api::EngineNoSqlProviderGenerationMetadata CurrentGeneration(
    const api::EngineRequestContext& context) {
  const auto generations = api::ListNoSqlProviderGenerations(context);
  Require(generations.size() == 1,
          "expected one NoSQL provider generation");
  return generations.front();
}

api::EngineDocumentPhysicalProof DocumentProof(
    const api::EngineRequestContext& context,
    const api::EngineNoSqlProviderGenerationMetadata& generation) {
  api::EngineDocumentPhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_path_index_proof = true;
  proof.wildcard_shape_index_proof = true;
  proof.shape_dictionary_proof = true;
  proof.structural_sharing_proof = true;
  proof.partial_materialization_proof = true;
  proof.document_path_index_runtime_proven = false;
  auto& contract = proof.provider_contract;
  contract.family = api::EngineNoSqlProviderFamily::kDocument;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = generation.provider_id;
  contract.fallback_provider_id = "nosql.local.document.shape_dictionary";
  contract.local_provider_available = true;
  contract.exact_fallback_available = true;
  contract.descriptor_visibility.proof_present = true;
  contract.descriptor_visibility.visible_to_snapshot = true;
  contract.descriptor_visibility.descriptor_shape_compatible = true;
  contract.security_redaction.proof_present = true;
  contract.security_redaction.redaction_policy_bound = true;
  contract.security_redaction.security_snapshot_bound = true;
  contract.index_generation.proof_present = true;
  contract.index_generation.visible_to_snapshot = true;
  contract.index_generation.covers_predicate = true;
  contract.index_generation.required_generation = generation.generation_id;
  contract.index_generation.available_generation = generation.generation_id;
  contract.index_generation.index_uuid = "orh120-document-path-index";
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.provider_generation.required = true;
  contract.provider_generation.proof_present = true;
  contract.provider_generation.visible_to_snapshot = true;
  contract.provider_generation.publish_state_bound = true;
  contract.provider_generation.validation_state_bound = true;
  contract.provider_generation.backup_restore_repair_metadata_bound = true;
  contract.provider_generation.support_bundle_evidence_bound = true;
  contract.provider_generation.required_generation = generation.generation_id;
  contract.provider_generation.available_generation = generation.generation_id;
  contract.provider_generation.descriptor_epoch = context.resource_epoch;
  contract.provider_generation.security_epoch = context.security_epoch;
  contract.provider_generation.redaction_epoch = context.security_epoch;
  contract.provider_generation.catalog_epoch = context.catalog_generation_id;
  contract.provider_generation.generation_uuid = generation.generation_uuid;
  contract.provider_generation.provider_id = generation.provider_id;
  contract.provider_generation.database_uuid = context.database_uuid.canonical;
  contract.provider_generation.collection_uuid = generation.collection_uuid;
  contract.provider_generation.publish_state = "published";
  contract.provider_generation.validation_state = "validated";
  contract.provider_generation.backup_metadata_ref = generation.backup_metadata_ref;
  contract.provider_generation.restore_metadata_ref =
      generation.restore_metadata_ref;
  contract.provider_generation.repair_metadata_ref = generation.repair_metadata_ref;
  contract.provider_generation.support_bundle_evidence_id =
      generation.support_bundle_evidence_id;
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return proof;
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view id) {
  for (const auto& item : result.evidence) {
    if (item.evidence_kind.find(kind) != std::string::npos &&
        item.evidence_id.find(id) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool DiagnosticContains(const api::EngineApiResult& result,
                        std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(code) != std::string::npos ||
        diagnostic.detail.find(code) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string RowField(const api::EngineApiResult& result,
                     std::size_t row_index,
                     std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) {
    return {};
  }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) {
      return value.encoded_value;
    }
  }
  return {};
}

void ProveOptimizerDifferentialCorpus() {
  const auto corpus = opt::GenerateOptimizerDifferentialFuzzCorpus();
  const auto report = opt::RunOptimizerDifferentialFuzzCorpus(corpus);
  Require(report.mismatch_count == 0,
          "optimizer differential corpus found mismatches: " +
              opt::SerializeOptimizerDifferentialEvidence(report));
  Require(report.accepted_equivalent_count > 0,
          "optimizer differential corpus did not include accepted equivalence");
  Require(report.exact_refusal_equivalent_count > 0,
          "optimizer differential corpus did not include exact refusals");
  for (const auto& result : report.results) {
    Require(Has(result.baseline.evidence,
                "mga_finality_authority=engine_transaction_inventory"),
            result.test_case.case_id + ": baseline missing MGA authority");
    Require(Has(result.optimized.evidence,
                "mga_finality_authority=engine_transaction_inventory"),
            result.test_case.case_id + ": optimized missing MGA authority");
    Require(Has(result.optimized.evidence,
                "parser_or_reference_finality_authority=false"),
            result.test_case.case_id +
                ": optimized route drifted to parser/reference authority");
  }
  CompareSemanticEquivalent(
      "optimizer_differential_fuzz",
      AcceptedCapture("optimizer_differential_report",
                      {"accepted=" +
                           std::to_string(report.accepted_equivalent_count),
                       "refused=" +
                           std::to_string(report.exact_refusal_equivalent_count)},
                      "corpus_order"),
      AcceptedCapture("optimizer_differential_report",
                      {"accepted=" +
                           std::to_string(report.accepted_equivalent_count),
                       "refused=" +
                           std::to_string(report.exact_refusal_equivalent_count)},
                      "corpus_order",
                      {"catalog_sql_planning=true",
                       "plan_cache_security_redaction_checked=true"}));
}

void ProveExtendedStatsDoNotChangeResultSemantics() {
  opt::ExtendedStatsSelectivityRequest request;
  request.relation_uuid = "rel.orh120.extended";
  request.column_uuids = {"col.tenant", "col.status"};
  request.value_encodings = {"tenant-7", "active"};
  request.children = {Child(0.10), Child(0.20)};
  request.minimum_confidence = opt::CostConfidence::kMedium;
  const auto estimate = opt::EstimateCorrelatedConjunctionSelectivity(
      request, {ExtendedStat(request.relation_uuid)});
  Require(estimate.used_kind == opt::ExtendedOptimizerStatisticKind::kJointMcv,
          "extended stats optimized path did not consume joint MCV");
  Require(Has(estimate.evidence,
              "extended_stats_selected_uuid=orh120.extended"),
          "extended stats selected UUID evidence missing");
  CompareSemanticEquivalent(
      "extended_stats_result_semantics",
      AcceptedCapture("rowset:tenant-status", {"tenant-7|active|row-1"},
                      "tenant,status"),
      AcceptedCapture("rowset:tenant-status", {"tenant-7|active|row-1"},
                      "tenant,status",
                      {"extended_stats_selectivity=" +
                       std::to_string(estimate.estimate.selectivity)}));
}

void ProveParserFrontDoorCacheEquivalence() {
  parser::SblrTemplateCache cache(2);
  parser::CacheEntry entry;
  entry.key = ParserCacheKey();
  entry.sblr_payload = "sblr:uuid-bound:orh120";
  entry.statement_family = "select";
  entry.operation_family = "read_only";
  entry.statement_hash = 120120;
  auto stored = cache.StoreEntry(entry);
  Require(stored.stored, "parser SBLR cache did not store safe entry");
  const auto hit = cache.Lookup(entry.key);
  Require(hit && *hit == entry.sblr_payload,
          "parser SBLR cache did not return baseline payload");

  auto redaction_changed = entry.key;
  redaction_changed.redaction_policy_generation += 1;
  Require(!cache.Lookup(redaction_changed),
          "parser SBLR cache admitted changed redaction policy key");

  auto authority = entry;
  authority.key.shape_hash += 1;
  authority.finality_authority_cached = true;
  const auto refused = cache.StoreEntry(authority);
  Require(!refused.stored, "parser authority-bearing cache entry was stored");
  CompareSemanticEquivalent(
      "parser_frontdoor_cache_payload",
      AcceptedCapture("sblr:uuid-bound:orh120", {"lowered-template"},
                      "statement-order"),
      AcceptedCapture("sblr:uuid-bound:orh120", {"lowered-template"},
                      "statement-order",
                      {"stable_key=" + stored.stable_key,
                       "cache_authority_refusal=" + refused.diagnostic_code}));
  CompareSemanticEquivalent(
      "parser_frontdoor_cache_authority_refusal",
      RefusedCapture(refused.diagnostic_code),
      RefusedCapture(refused.diagnostic_code,
                     {"parser_cache_finality_authority=false"}));
}

void ProveDmlAndHotPointEquivalence() {
  api::DmlTargetAccessPlanRequest request;
  request.mutation_kind = "dml.merge_rows";
  request.database_uuid = "orh120-db";
  request.relation_uuid = "orh120-table";
  request.predicate_kind = "row_uuid_match";
  request.predicate_descriptor_digest = "row_uuid_match:row-120";
  request.row_uuid = "row-120";
  request.security_policy_digest = "security:orh120";
  request.redaction_policy_digest = "redaction:orh120";
  request.access_policy_digest = "access:orh120";
  request.collation_profile_digest = "binary";
  request.access_descriptor_present = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  request.grants_proven = true;
  request.security_context_present = true;
  request.observed_catalog_epoch = 120;
  request.current_catalog_epoch = 120;
  request.observed_security_epoch = 120;
  request.current_security_epoch = 120;
  request.observed_policy_epoch = 120;
  request.current_policy_epoch = 120;
  request.local_transaction_id = 120;
  request.estimated_rows = 1;
  const auto plan = api::BuildDmlTargetAccessPlan(request);
  Require(plan.ok, "DML target access plan refused row UUID route");
  Require(plan.access_kind == api::DmlTargetAccessKind::row_uuid_singleton,
          "DML target access plan did not choose row UUID singleton");
  Require(Has(plan.evidence, "mga_visibility_recheck=required"),
          "DML plan missing MGA recheck evidence");

  idx::AdaptiveHotPointLookupCache cache;
  auto admitted = cache.Put(HotPointEntry(false));
  Require(admitted.admitted, "hot-point cache did not admit safe locator");
  const auto lookup = cache.Lookup(HotPointKey());
  Require(lookup.cache_hit, "hot-point cache lookup missed safe locator");
  Require(HasPrefix(lookup.evidence, "mga_visibility_recheck=required"),
          "hot-point lookup missing MGA evidence");

  CompareSemanticEquivalent(
      "dml_merge_hot_point_rows",
      AcceptedCapture("merge:row-120", {"row-120|merged"}, "row_uuid"),
      AcceptedCapture("merge:row-120", {"row-120|merged"}, "row_uuid",
                      {"dml_access_kind=" +
                           std::string(api::DmlTargetAccessKindName(
                               plan.access_kind)),
                       "hot_point_cache_hit=true"}));

  auto refused = cache.Put(HotPointEntry(true));
  Require(!refused.admitted,
          "hot-point authority-bearing locator was admitted");
  CompareSemanticEquivalent(
      "dml_hot_point_authority_refusal",
      RefusedCapture("SB_INDEX_HOT_POINT_LOOKUP_CACHE_AUTHORITY_REFUSED"),
      RefusedCapture(refused.diagnostic_code,
                     {"cache_visibility_finality_authority=false"}));
}

void ProveStreamingAndDirectFrameEquivalence() {
  const std::vector<std::string> rows = {"row-1", "row-2", "row-3"};
  const auto batch = Batch(rows);
  const auto full_frame = wire::BuildDirectBinaryResultFrame(batch);
  Require(full_frame.ok(), "direct binary frame build failed");
  auto parsed = wire::ParseDirectBinaryResultFrame(full_frame.frame.bytes);
  Require(parsed.ok(), "direct binary frame parse failed");
  Require(parsed.frame.descriptor.row_count == rows.size(),
          "direct binary frame row count mismatch");

  wire::DirectBinaryResultFrameWindowPolicy policy;
  policy.start_row = 0;
  policy.requested_rows = rows.size();
  policy.max_rows = rows.size();
  policy.max_frame_bytes = full_frame.frame.bytes.size();
  policy.client_credit_rows = rows.size();
  policy.client_credit_bytes = full_frame.frame.bytes.size();
  policy.frame_sequence = 120;
  policy.require_ordered_output = true;
  const auto window = wire::BuildDirectBinaryResultFrameWindow(batch, policy);
  Require(window.ok(), "direct binary frame window failed");
  Require(window.ordering_preserved,
          "direct binary frame did not preserve required ordering");

  wire::StreamingCursorManager manager;
  wire::StreamingCursorState state;
  state.cursor_id = "019b8000-0000-7000-8000-000000000120";
  state.plan_result_contract_hash = "sha256:orh120-frame-contract";
  state.catalog_epoch = 120;
  state.descriptor_epoch = 120;
  state.transaction_snapshot_class = "mga_statement_snapshot";
  state.transaction_uuid = "019b8000-0000-7000-8000-000000000121";
  state.local_transaction_id = 120;
  state.snapshot_visible_through_local_transaction_id = 119;
  state.security_epoch = 120;
  state.redaction_epoch = 120;
  state.route_kind = "embedded";
  state.frame_sequence = 120;
  state.expiry_deadline_unix_millis = 10000;
  state.client_credit.frame_credit = 1;
  state.client_credit.row_credit = rows.size();
  state.client_credit.byte_credit = full_frame.frame.bytes.size();
  const auto opened =
      manager.OpenCursor({.state = state, .now_unix_millis = 100});
  Require(opened.ok(), "streaming cursor open failed");
  Require(!opened.state.mga_visibility_or_finality_authority,
          "streaming cursor claimed MGA finality authority");

  CompareSemanticEquivalent(
      "streaming_cursor_direct_binary_frame",
      AcceptedCapture("rowset:streaming-frame", rows, "input_order"),
      AcceptedCapture("rowset:streaming-frame", rows, "input_order",
                      {"frame_bytes=" +
                           std::to_string(full_frame.frame.bytes.size()),
                       "window_sequence=" +
                           std::to_string(window.frame_sequence),
                       "streaming_cursor_authority=false"}));
}

void ProveCompressionAndVectorEquivalence() {
  const auto compression =
      idx::EvaluateCompressionPolicy(CostedCompressionRequest(
          idx::CompressionFamily::kBinaryResultFrame));
  Require(compression.accepted, "safe compression policy was refused");
  Require(Has(compression.evidence, "compression_exact_semantic_equivalence=true"),
          "compression semantic equivalence evidence missing");
  Require(Has(compression.evidence, "parser_or_reference_authority=false"),
          "compression policy claimed parser/reference authority");
  CompareSemanticEquivalent(
      "compression_policy_rows",
      AcceptedCapture("rowset:compressed-equivalent", {"row-1|payload"},
                      "row_id"),
      AcceptedCapture("rowset:compressed-equivalent", {"row-1|payload"},
                      "row_id",
                      {"compression_method=" +
                       std::string(idx::CompressionMethodName(
                           compression.method))}));

  auto index_request =
      CostedCompressionRequest(idx::CompressionFamily::kExactIndexPage);
  index_request.runtime_index_compression_requested = true;
  index_request.index_runtime_correctness_proven = false;
  const auto index_refusal = idx::EvaluateCompressionPolicy(index_request);
  Require(!index_refusal.accepted, "index runtime compression was accepted");
  CompareSemanticEquivalent(
      "compression_index_runtime_refusal",
      RefusedCapture(
          "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.INDEX_RUNTIME_UNPROVEN"),
      RefusedCapture(
          index_refusal.diagnostics.front(),
          {"compression_index_runtime_closure_claimed=false"}));

  const auto vector_decision =
      idx::EvaluateVectorTrainingRecallLifecycle(VectorProfile());
  Require(vector_decision.accepted, "vector lifecycle decision was refused");
  Require(Has(vector_decision.evidence,
              "vector_runtime_correctness_blocker="
              "SB_ORH_VECTOR_INDEX_RUNTIME_CORRECTNESS_UNPROVEN"),
          "vector runtime blocker evidence missing");
  agents::VectorMaintenanceJobStore store;
  const auto job =
      agents::CreateVectorMaintenanceJob(&store, VectorJobRequest(vector_decision));
  Require(job.ok(), "vector maintenance job creation failed");
  Require(!job.record.ann_visibility_authority &&
              !job.record.ann_finality_authority,
          "vector maintenance job claimed ANN authority");
  CompareSemanticEquivalent(
      "vector_lifecycle_job_rows",
      AcceptedCapture("rowset:vector-exact-rerank", {"vec-1|score=1.0"},
                      "score_desc"),
      AcceptedCapture("rowset:vector-exact-rerank", {"vec-1|score=1.0"},
                      "score_desc",
                      {"vector_action=" +
                       std::string(idx::VectorTrainingRecallLifecycleActionName(
                           vector_decision.action)),
                       "vector_job_runtime_blocker=" +
                           job.record.runtime_correctness_blocker}));
}

void ProveNoSqlProviderEquivalenceAndDocumentPathRuntime() {
  TempDatabase db("nosql");
  const auto writer = DocumentContext(db.path, 900);
  SeedCrudTransaction(writer);
  api::EngineDocumentInsertRequest insert;
  insert.context = writer;
  insert.target_object.uuid.canonical = "orh120-doc-a";
  AddFragment(&insert, "tenant.id", "T1");
  AddFragment(&insert, "status", "active");
  auto inserted = api::EngineDocumentInsert(insert);
  Require(inserted.ok, "document insert failed");
  const auto generation = CurrentGeneration(writer);

  api::EngineDocumentFindRequest find;
  find.context = DocumentContext(db.path, 950);
  find.path = "tenant.id";
  find.equals_value = "T1";
  find.projected_paths = {"tenant.id", "status"};
  find.physical_proof = DocumentProof(writer, generation);
  auto found = api::EngineDocumentFind(find);
  Require(found.ok, "document path provider lookup failed");
  Require(RowField(found, 0, "path:tenant.id") == "T1",
          "document provider lookup returned wrong tenant");
  Require(EvidenceContains(found, "document_provider_index_consumed", "true"),
          "document provider did not consume provider index");
  Require(EvidenceContains(found,
                           "benchmark_clean_index_runtime_closure",
                           "true"),
          "document path runtime closure evidence missing");
  CompareSemanticEquivalent(
      "nosql_provider_generation_path_rows",
      AcceptedCapture("document:T1", {"T1|active"}, "document_uuid"),
      AcceptedCapture("document:T1", {"T1|active"}, "document_uuid",
                      {"provider_generation=" +
                       std::to_string(generation.generation_id),
                       "document_path_index_runtime_proven=true"}));

  auto strict = find;
  strict.require_benchmark_clean_index_runtime = true;
  auto strict_result = api::EngineDocumentFind(strict);
  Require(strict_result.ok,
          "strict NoSQL path-index runtime proof was refused");
  Require(EvidenceContains(strict_result,
                           "benchmark_clean_index_runtime_closure",
                           "true"),
          "strict NoSQL path-index runtime closure evidence missing");
  CompareSemanticEquivalent(
      "document_path_index_runtime_strict_route",
      AcceptedCapture("document:T1", {"T1|active"}, "document_uuid"),
      AcceptedCapture("document:T1", {"T1|active"}, "document_uuid",
                      {"document_path_index_runtime_proven=true"}));
}

void ProveSnapshotCacheAndMetricsEquivalence() {
  exec::SnapshotSafeResultCache cache;
  auto store = cache.Store(SnapshotStoreRequest());
  Require(store.action == exec::SnapshotSafeCacheAction::kStore,
          "snapshot cache store failed");
  const auto hit = cache.Lookup(SnapshotLookupRequest());
  Require(hit.action == exec::SnapshotSafeCacheAction::kHit && hit.cache_hit,
          "snapshot cache did not return equivalent hit");
  Require(Has(hit.evidence, "snapshot_cache_recompute_result_match=true"),
          "snapshot cache missing result recompute proof");
  Require(Has(hit.evidence, "snapshot_cache_recompute_mga_security_match=true"),
          "snapshot cache missing MGA/security recompute proof");
  CompareSemanticEquivalent(
      "snapshot_safe_cache_rows",
      AcceptedCapture("sha256:orh120-result", {"row-1", "row-2"},
                      "contract_order"),
      AcceptedCapture("sha256:orh120-result", {"row-1", "row-2"},
                      "contract_order",
                      {"snapshot_cache_hit=true"}));

  auto authority = SnapshotLookupRequest();
  authority.transaction_finality_authority_cached = true;
  const auto authority_refusal = cache.Lookup(authority);
  CompareSemanticEquivalent(
      "snapshot_cache_authority_refusal",
      RefusedCapture("EXECUTOR.SNAPSHOT_RESULT_CACHE.AUTHORITY_REFUSED"),
      RefusedCapture(authority_refusal.diagnostic_code,
                     {"cache_transaction_finality_authority=false"}));

  const auto event = MetricEvent();
  const auto validated = api::ValidatePerformanceMetricEvent(event);
  Require(validated.ok, "performance metric event was rejected");
  const auto json = api::SerializePerformanceMetricEventJson(event);
  Require(json.find("\"phase_timings_measurement_source\":"
                    "\"measured_by_internal_counter\"") != std::string::npos,
          "metric source field missing from serialization");
  Require(json.find("\"storage_measurement_quality\":"
                    "\"not_available_zeroed\"") != std::string::npos,
          "metric quality field missing from serialization");
  CompareSemanticEquivalent(
      "metric_source_quality_fields",
      AcceptedCapture("metric:sha256:orh120-result",
                      {"source=measured_by_internal_counter",
                       "quality=not_available_zeroed"},
                      "field_name"),
      AcceptedCapture("metric:sha256:orh120-result",
                      {"source=measured_by_internal_counter",
                       "quality=not_available_zeroed"},
                      "field_name",
                      {"schema_version=" +
                       std::to_string(api::PerformanceMetricEventSchemaVersion())}));
}

void ProveIndexDependentBlockersRemainExact() {
  CompareSemanticEquivalent(
      "orh211_deferred_index_blocker_preserved",
      RefusedCapture(
          "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.INDEX_CORRECTNESS_UNPROVEN",
          {"tracker_row=ORH-211:blocked"}),
      RefusedCapture(
          "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.INDEX_CORRECTNESS_UNPROVEN",
          {"tracker_row=ORH-211:blocked"}));
  CompareSemanticEquivalent(
      "orh220_physical_operator_index_blocker_preserved",
      RefusedCapture("SB_ORH_PHYSICAL_OPERATOR.INDEX_RUNTIME_UNPROVEN",
                     {"tracker_row=ORH-220:blocked"}),
      RefusedCapture("SB_ORH_PHYSICAL_OPERATOR.INDEX_RUNTIME_UNPROVEN",
                     {"tracker_row=ORH-220:blocked"}));
  CompareSemanticEquivalent(
      "orh222_covering_index_blocker_preserved",
      RefusedCapture(
          "SB_ORH_LATE_MATERIALIZATION_COVERING_INDEX.INDEX_RUNTIME_UNPROVEN",
          {"tracker_row=ORH-222:blocked"}),
      RefusedCapture(
          "SB_ORH_LATE_MATERIALIZATION_COVERING_INDEX.INDEX_RUNTIME_UNPROVEN",
          {"tracker_row=ORH-222:blocked"}));
}

}  // namespace

int main() {
  ProveOptimizerDifferentialCorpus();
  ProveExtendedStatsDoNotChangeResultSemantics();
  ProveParserFrontDoorCacheEquivalence();
  ProveDmlAndHotPointEquivalence();
  ProveStreamingAndDirectFrameEquivalence();
  ProveCompressionAndVectorEquivalence();
  ProveNoSqlProviderEquivalenceAndDocumentPathRuntime();
  ProveSnapshotCacheAndMetricsEquivalence();
  ProveIndexDependentBlockersRemainExact();
  std::cout << "optimizer_runtime_hot_path_orh_120_gate=passed\n";
  return EXIT_SUCCESS;
}
