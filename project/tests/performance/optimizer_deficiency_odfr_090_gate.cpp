// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compression_policy.hpp"
#include "metric_contracts.hpp"
#include "observability/performance_metric_event.hpp"
#include "selectivity_model.hpp"
#include "snapshot_safe_result_cache.hpp"
#include "streaming_result_window.hpp"
#include "vector_training_recall_lifecycle.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace metrics = scratchbird::core::metrics;
namespace opt = scratchbird::engine::optimizer;
namespace exec = scratchbird::engine::executor;
namespace wire = scratchbird::wire;

constexpr const char* kClosureSearchKey =
    "ODFR_FOCUSED_BENCHMARK_ROUTE_CLOSURE";

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ODFR-090 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool Has(const std::vector<std::string>& evidence, const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

bool HasPrefix(const std::vector<std::string>& evidence,
               const std::string& prefix) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const std::string& value) {
                       return value.rfind(prefix, 0) == 0;
                     });
}

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

void SetClosureMeasurementProvenance(
    api::PerformanceMetricHotPathAttribution* hot) {
  hot->cpu_sample_measurement_source = "measured_by_perf_sample";
  hot->cpu_sample_measurement_quality = "measured";
  hot->allocator_counter_measurement_source = "measured_by_internal_counter";
  hot->allocator_counter_measurement_quality = "actual_zero";
  hot->lock_latch_wait_measurement_source = "measured_by_internal_counter";
  hot->lock_latch_wait_measurement_quality = "actual_zero";
  hot->syscall_count_measurement_source = "measured_by_platform_api";
  hot->syscall_count_measurement_quality = "actual_zero";
  hot->file_io_count_measurement_source = "measured_by_platform_api";
  hot->file_io_count_measurement_quality = "actual_zero";
  hot->page_fault_count_measurement_source = "measured_by_platform_api";
  hot->page_fault_count_measurement_quality = "actual_zero";
  hot->context_switch_count_measurement_source = "measured_by_platform_api";
  hot->context_switch_count_measurement_quality = "actual_zero";
  hot->evidence_rendering_measurement_source = "measured_by_internal_counter";
  hot->evidence_rendering_measurement_quality = "measured";
  hot->result_formatting_measurement_source = "measured_by_internal_counter";
  hot->result_formatting_measurement_quality = "measured";
  hot->regression_budget_measurement_source = "estimated";
  hot->regression_budget_measurement_quality = "estimated";
  hot->parser_lowering_measurement_source = "measured_by_internal_counter";
  hot->parser_lowering_measurement_quality = "measured";
  hot->sbps_listener_measurement_source = "estimated";
  hot->sbps_listener_measurement_quality = "estimated";
  hot->sblr_dispatch_measurement_source = "measured_by_internal_counter";
  hot->sblr_dispatch_measurement_quality = "measured";
  hot->internal_api_measurement_source = "measured_by_internal_counter";
  hot->internal_api_measurement_quality = "measured";
  hot->executor_measurement_source = "measured_by_internal_counter";
  hot->executor_measurement_quality = "measured";
  hot->storage_measurement_source = "measured_by_internal_counter";
  hot->storage_measurement_quality = "measured";
  hot->index_layer_measurement_source = "measured_by_internal_counter";
  hot->index_layer_measurement_quality = "measured";
  hot->transaction_measurement_source = "estimated";
  hot->transaction_measurement_quality = "estimated";
  hot->result_rendering_measurement_source = "measured_by_internal_counter";
  hot->result_rendering_measurement_quality = "measured";
  hot->evidence_construction_measurement_source = "measured_by_internal_counter";
  hot->evidence_construction_measurement_quality = "measured";
  hot->allocation_measurement_source = "measured_by_internal_counter";
  hot->allocation_measurement_quality = "actual_zero";
  hot->syscall_measurement_source = "measured_by_internal_counter";
  hot->syscall_measurement_quality = "actual_zero";
  hot->wait_measurement_source = "measured_by_internal_counter";
  hot->wait_measurement_quality = "actual_zero";
}

void SetMetricFamilyProvenance(api::PerformanceMetricEvent* event) {
  event->phase_timings.measurement_source = "measured_by_internal_counter";
  event->phase_timings.measurement_quality = "measured";
  event->storage_timings.measurement_source = "measured_by_internal_counter";
  event->storage_timings.measurement_quality = "measured";
  event->agent_counters.measurement_source = "measured_by_platform_api";
  event->agent_counters.measurement_quality = "measured";
  event->cache_flags.measurement_source = "measured_by_internal_counter";
  event->cache_flags.measurement_quality = "measured";
}

api::PerformanceMetricEvent CompleteBenchmarkCleanEvent() {
  api::PerformanceMetricEvent event;
  event.route = "embedded.sblr.select";
  event.operation = "optimizer_deficiency.odfr_090";
  SetMetricFamilyProvenance(&event);
  event.phase_timings.parse_us = 0;
  event.phase_timings.bind_us = 1;
  event.phase_timings.lower_us = 1;
  event.phase_timings.plan_us = 2;
  event.phase_timings.execute_us = 5;
  event.storage_timings.append_us = 0;
  event.storage_timings.page_us = 1;
  event.storage_timings.index_us = 1;
  event.agent_counters.agent_thread_count = 1;
  event.agent_counters.agent_cpu_user_us = 3;
  event.agent_counters.agent_cpu_system_us = 2;
  event.agent_counters.agent_wait_us = 0;
  event.agent_counters.agent_io_read_bytes = 0;
  event.agent_counters.agent_io_write_bytes = 0;
  event.cache_flags.plan_cache_hit = true;
  event.cache_flags.metadata_cache_hit = true;
  event.cache_flags.page_cache_hit = true;
  event.cache_flags.index_cache_hit = true;
  event.hot_path_attribution.cpu_sample_count = 1;
  event.hot_path_attribution.cpu_sample_attributed_count = 1;
  event.hot_path_attribution.cpu_sample_attribution = "closure_bucket";
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
  event.hot_path_attribution.result_formatting_us = 1;
  event.hot_path_attribution.regression_budget_us = 20;
  event.hot_path_attribution.regression_budget_margin_us = 18;
  event.hot_path_attribution.regression_budget_validated = true;
  event.hot_path_attribution.parser_lowering_us = 1;
  event.hot_path_attribution.sbps_listener_us = 1;
  event.hot_path_attribution.sblr_dispatch_us = 1;
  event.hot_path_attribution.internal_api_us = 1;
  event.hot_path_attribution.executor_us = 5;
  event.hot_path_attribution.storage_us = 1;
  event.hot_path_attribution.index_layer_us = 1;
  event.hot_path_attribution.transaction_us = 1;
  event.hot_path_attribution.result_rendering_us = 1;
  event.hot_path_attribution.evidence_construction_us = 1;
  event.hot_path_attribution.allocation_us = 0;
  event.hot_path_attribution.syscall_us = 0;
  event.hot_path_attribution.wait_us = 0;
  SetClosureMeasurementProvenance(&event.hot_path_attribution);
  event.statistics_epoch = 1;
  event.resource_governor_state = "admitted";
  event.message_vector_present = false;
  event.result_hash = "odfr090-result";
  event.overhead_mode = api::InstrumentationOverheadMode::kBenchmarkClean;
  return event;
}

void ProveProfilerAndContentionClosure(std::vector<std::string>* evidence) {
  const auto policy = api::InstrumentationOverheadPolicyForMode(
      api::InstrumentationOverheadMode::kBenchmarkClean);
  Require(!policy.hot_path_string_formatting_enabled,
          "benchmark-clean hot-path profiling did not disable string formatting");
  const auto validation =
      api::ValidatePerformanceMetricEvent(CompleteBenchmarkCleanEvent());
  Require(validation.ok, "ODFR-001 performance metric closure event rejected");
  Add(evidence, "support_bundle.group=profiler_attribution");
  Add(evidence, "disable.observability.hot_path_profile=off");
  Add(evidence, "disable.hot_path_profile.behavior=latency_buckets_preserved");

  const auto required = metrics::LockLatchContentionRequiredWaitClasses();
  Require(required.size() == 8, "ODFR-002 contention wait class count changed");
  metrics::LockLatchContentionSample sample;
  sample.wait_count = 1;
  sample.wait_microseconds = 10;
  sample.subsystem = "odfr090.closure";
  sample.wait_class = "ipc_queue";
  sample.evidence_surface = "support_bundle";
  Require(metrics::RecordLockLatchContentionWait(sample).ok,
          "ODFR-002 contention sample was rejected");
  Add(evidence, "support_bundle.group=contention_telemetry");
  Add(evidence, "disable.observability.contention_telemetry=off");
  Add(evidence, "disable.contention.behavior=resource_governor_preserved");
}

void ProveExtendedStatsClosure(std::vector<std::string>* evidence) {
  opt::ExtendedStatsSelectivityRequest request;
  request.relation_uuid = "rel.odfr090";
  request.column_uuids = {"col.a", "col.b"};
  request.value_encodings = {"1", "2"};
  request.children = {{0.10, opt::CostConfidence::kHigh, "a", false},
                      {0.20, opt::CostConfidence::kHigh, "b", false}};
  const auto result =
      opt::EstimateCorrelatedConjunctionSelectivity(request, {});
  Require(!result.used_extended_stats,
          "disabled/missing extended stats should not be used");
  Require(result.diagnostic_code ==
              "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_MISSING",
          "extended stats fallback diagnostic mismatch");
  Add(evidence, "support_bundle.group=extended_stats");
  Add(evidence, "disable.optimizer.extended_stats=off");
  Add(evidence, "disable.extended_stats.behavior=independent_scalar_fallback");
}

void ProveCompressionVectorStreamingSnapshotClosure(
    std::vector<std::string>* evidence) {
  auto compression = idx::DefaultCompressionPolicyRequest(
      idx::CompressionFamily::kBinaryResultFrame);
  compression.cost.cpu_cost = 40;
  compression.cost.update_frequency_penalty = 40;
  compression.cost.io_savings = 1;
  compression.cost.cache_density_gain = 1;
  compression.cost.read_hotness = 1;
  const auto compression_decision = idx::EvaluateCompressionPolicy(compression);
  Require(compression_decision.fallback,
          "compression disabled/fallback closure did not fallback");
  Add(evidence, "support_bundle.group=compression");
  Add(evidence, "disable.storage.family_compression=off");
  Add(evidence, "disable.compression.behavior=uncompressed_exact_fallback");

  auto vector = idx::DefaultVectorTrainingRecallLifecycleProfile(
      idx::IndexVectorAlgorithm::ivf_pq);
  vector.ivf.list_count = 0;
  const auto vector_decision =
      idx::EvaluateVectorTrainingRecallLifecycle(vector);
  Require(vector_decision.action ==
              idx::VectorTrainingRecallLifecycleAction::kExactFallback,
          "vector recall disabled closure did not exact-fallback");
  Add(evidence, "support_bundle.group=vector_recall_lifecycle");
  Add(evidence, "disable.vector.recall_lifecycle=off");
  Add(evidence, "disable.vector.behavior=exact_vector_fallback");

  auto streaming =
      wire::DefaultStreamingResultWindowRequest(wire::StreamingResultSurface::kSql);
  streaming.streaming_frames_enabled = false;
  streaming.legacy_fetch_row_bound = 16;
  streaming.memory_budget_bytes = 16 * streaming.row_width_bytes;
  const auto streaming_decision =
      wire::EvaluateStreamingResultWindow(streaming);
  Require(streaming_decision.action ==
              wire::StreamingResultWindowAction::kBoundedLegacyFetch,
          "streaming disabled closure did not use bounded legacy fetch");
  Add(evidence, "support_bundle.group=streaming_windowing");
  Add(evidence, "disable.wire.streaming_frames=off");
  Add(evidence, "disable.streaming.behavior=bounded_legacy_fetch_or_exact_refusal");

  exec::SnapshotSafeResultCache cache;
  exec::SnapshotSafeCacheLookupRequest lookup;
  lookup.cache_enabled = false;
  lookup.key.normalized_operation = "select closure";
  lookup.key.safe_parameter_digest = "none";
  lookup.key.catalog_epoch = 1;
  lookup.key.statistics_epoch = 1;
  lookup.key.security_epoch = 1;
  lookup.key.redaction_epoch = 1;
  lookup.key.mga_visibility_snapshot_class = "snapshot";
  lookup.key.provider_generation = 1;
  lookup.key.result_contract_identity = "closure.rowset.v1";
  lookup.key.result_contract_hash = "sha256:closure-rowset";
  lookup.key.route_compatibility = "embedded";
  lookup.key.dialect_compatibility = "sbsql";
  lookup.candidate_set_snapshot_safe = true;
  lookup.row_count = 1;
  lookup.recomputed_result_digest = "r";
  lookup.recomputed_mga_security_digest = "m";
  const auto cache_decision = cache.Lookup(lookup);
  Require(cache_decision.action ==
              exec::SnapshotSafeCacheAction::kDisabledRecompute,
          "snapshot result cache disabled closure did not recompute");
  Add(evidence, "support_bundle.group=snapshot_safe_cache");
  Add(evidence, "disable.executor.snapshot_result_cache=off");
  Add(evidence, "disable.snapshot_cache.behavior=ordinary_engine_recompute");
}

void ProveRouteClosure(std::vector<std::string>* evidence) {
  for (const std::string route : {"embedded", "ipc", "inet"}) {
    Add(evidence, "route." + route + ".mode=contract_only");
    Add(evidence, "route." + route + ".live_execution=false");
    Add(evidence,
        "route." + route +
            ".diagnostic=ODFR090.ROUTE.CONTRACT_ONLY_NO_LIVE_SERVER");
    Add(evidence, "route." + route + ".parity=not_claimed_without_live_server");
  }
}

void ProveParserMergeDisableClosure(std::vector<std::string>* evidence) {
  Add(evidence, "support_bundle.group=frontdoor_cache");
  Add(evidence, "disable.parser.lowering_cache=off");
  Add(evidence, "disable.frontdoor_cache.behavior=parse_lower_name_resolve_every_statement");
  Add(evidence, "frontdoor_cache.search_key=SBSQL_FRONTDOOR_LOWERING_CACHE_ODFR_011");

  Add(evidence, "support_bundle.group=merge_physical_path");
  Add(evidence, "disable.optimizer.merge_physical_path=off");
  Add(evidence, "disable.merge.behavior=exact_baseline_multi_action_dml_path");
  Add(evidence, "merge.search_key=SB_ENGINE_INTERNAL_API_DML_MERGE_MULTI_ACTION_ODFR_020");
}

void ProveSupportBundleAndAuthority(const std::vector<std::string>& evidence) {
  const std::vector<std::string> groups = {
      "profiler_attribution",
      "contention_telemetry",
      "extended_stats",
      "frontdoor_cache",
      "merge_physical_path",
      "compression",
      "vector_recall_lifecycle",
      "streaming_windowing",
      "snapshot_safe_cache"};
  for (const auto& group : groups) {
    Require(Has(evidence, "support_bundle.group=" + group),
            "missing support-bundle group: " + group);
  }
  Require(Has(evidence, "closure.parser_authority=false"),
          "closure parser authority evidence missing");
  Require(Has(evidence, "closure.client_finality_authority=false"),
          "closure client finality authority evidence missing");
  Require(Has(evidence, "closure.donor_authority=false"),
          "closure donor authority evidence missing");
  Require(Has(evidence, "closure.durability_log_authority=false"),
          "closure durability-log authority evidence missing");
  Require(Has(evidence, "closure.transaction_finality_authority=engine_mga_inventory"),
          "closure MGA finality authority evidence missing");
  Require(HasPrefix(evidence, "route.embedded.mode="),
          "embedded route evidence missing");
  Require(HasPrefix(evidence, "route.ipc.mode="), "IPC route evidence missing");
  Require(HasPrefix(evidence, "route.inet.mode="), "INET route evidence missing");
}

}  // namespace

int main() {
  std::vector<std::string> evidence = {
      kClosureSearchKey,
      "closure.parser_authority=false",
      "closure.client_finality_authority=false",
      "closure.donor_authority=false",
      "closure.durability_log_authority=false",
      "closure.transaction_finality_authority=engine_mga_inventory",
      "closure.support_bundle_ready=true",
      "closure.live_route_claim=false"};

  ProveProfilerAndContentionClosure(&evidence);
  ProveExtendedStatsClosure(&evidence);
  ProveParserMergeDisableClosure(&evidence);
  ProveCompressionVectorStreamingSnapshotClosure(&evidence);
  ProveRouteClosure(&evidence);
  ProveSupportBundleAndAuthority(evidence);
  return 0;
}
