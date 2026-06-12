// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/performance_metric_event.hpp"
#include "snapshot_safe_result_cache.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ORH-100/110/111 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool Contains(const std::vector<std::string>& values,
              const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool ContainsText(const std::string& text, const std::string& expected) {
  return text.find(expected) != std::string::npos;
}

bool SchemaHasRequiredField(const std::string& name) {
  const auto& schema = api::PerformanceMetricEventSchema();
  return std::any_of(schema.begin(), schema.end(), [&](const auto& field) {
    return field.name == name && field.required;
  });
}

void SetProvenance(api::PerformanceMetricHotPathAttribution* hot) {
  hot->cpu_sample_measurement_source = "measured_by_perf_sample";
  hot->cpu_sample_measurement_quality = "measured";
  hot->allocator_counter_measurement_source = "measured_by_internal_counter";
  hot->allocator_counter_measurement_quality = "actual_zero";
  hot->lock_latch_wait_measurement_source = "not_available_zeroed";
  hot->lock_latch_wait_measurement_quality = "not_available_zeroed";
  hot->syscall_count_measurement_source = "measured_by_platform_api";
  hot->syscall_count_measurement_quality = "actual_zero";
  hot->file_io_count_measurement_source = "disabled";
  hot->file_io_count_measurement_quality = "disabled";
  hot->page_fault_count_measurement_source = "measured_by_platform_api";
  hot->page_fault_count_measurement_quality = "actual_zero";
  hot->context_switch_count_measurement_source = "unsupported";
  hot->context_switch_count_measurement_quality = "unsupported";
  hot->evidence_rendering_measurement_source = "measured_by_internal_counter";
  hot->evidence_rendering_measurement_quality = "measured";
  hot->result_formatting_measurement_source = "measured_by_internal_counter";
  hot->result_formatting_measurement_quality = "actual_zero";
  hot->regression_budget_measurement_source = "estimated";
  hot->regression_budget_measurement_quality = "estimated";
  hot->parser_lowering_measurement_source = "measured_by_internal_counter";
  hot->parser_lowering_measurement_quality = "measured";
  hot->sbps_listener_measurement_source = "measured_by_perf_sample";
  hot->sbps_listener_measurement_quality = "measured";
  hot->sblr_dispatch_measurement_source = "measured_by_platform_api";
  hot->sblr_dispatch_measurement_quality = "measured";
  hot->internal_api_measurement_source = "estimated";
  hot->internal_api_measurement_quality = "estimated";
  hot->executor_measurement_source = "measured_by_internal_counter";
  hot->executor_measurement_quality = "measured";
  hot->storage_measurement_source = "not_available_zeroed";
  hot->storage_measurement_quality = "not_available_zeroed";
  hot->index_layer_measurement_source = "disabled";
  hot->index_layer_measurement_quality = "disabled";
  hot->transaction_measurement_source = "unsupported";
  hot->transaction_measurement_quality = "unsupported";
  hot->result_rendering_measurement_source = "measured_by_internal_counter";
  hot->result_rendering_measurement_quality = "actual_zero";
  hot->evidence_construction_measurement_source = "measured_by_internal_counter";
  hot->evidence_construction_measurement_quality = "measured";
  hot->allocation_measurement_source = "estimated";
  hot->allocation_measurement_quality = "estimated";
  hot->syscall_measurement_source = "measured_by_platform_api";
  hot->syscall_measurement_quality = "actual_zero";
  hot->wait_measurement_source = "measured_by_perf_sample";
  hot->wait_measurement_quality = "actual_zero";
}

void SetMetricFamilyProvenance(api::PerformanceMetricEvent* event) {
  event->phase_timings.measurement_source = "measured_by_internal_counter";
  event->phase_timings.measurement_quality = "measured";
  event->storage_timings.measurement_source = "not_available_zeroed";
  event->storage_timings.measurement_quality = "not_available_zeroed";
  event->agent_counters.measurement_source = "measured_by_platform_api";
  event->agent_counters.measurement_quality = "measured";
  event->cache_flags.measurement_source = "measured_by_internal_counter";
  event->cache_flags.measurement_quality = "measured";
}

api::PerformanceMetricEvent CompleteMetricEvent() {
  api::PerformanceMetricEvent event;
  event.route = "embedded.orh100";
  event.operation = "optimizer_runtime_hot_path.orh_100";
  SetMetricFamilyProvenance(&event);
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
  event.hot_path_attribution.cpu_sample_count = 10;
  event.hot_path_attribution.cpu_sample_attributed_count = 9;
  event.hot_path_attribution.cpu_sample_attribution = "orh100_source_quality";
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
  event.hot_path_attribution.regression_budget_margin_us = 91;
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
  SetProvenance(&event.hot_path_attribution);
  event.statistics_epoch = 100;
  event.resource_governor_state = "admitted";
  event.message_vector_present = false;
  event.result_hash = "sha256:orh100-result";
  return event;
}

exec::SnapshotSafeCacheKey BaseKey() {
  exec::SnapshotSafeCacheKey key;
  key.normalized_operation = "select orh110 where tenant=?";
  key.safe_parameter_digest = "tenant:stable";
  key.catalog_epoch = 101;
  key.statistics_epoch = 102;
  key.security_epoch = 103;
  key.redaction_epoch = 104;
  key.mga_visibility_snapshot_class = "repeatable_read:orh110";
  key.provider_generation = 105;
  key.descriptor_identity_digest = "descriptor:orh110-rowset:v1";
  key.descriptor_epoch = 106;
  key.result_contract_identity = "orh110.rowset.v1";
  key.result_contract_hash = "sha256:orh110-rowset-contract";
  key.route_compatibility = "embedded_ipc_v1";
  key.dialect_compatibility = "sbsql_v1";
  return key;
}

exec::SnapshotSafeCacheEntry Entry() {
  exec::SnapshotSafeCacheEntry entry;
  entry.key = BaseKey();
  entry.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  entry.row_count = 3;
  entry.cached_result_digest = "sha256:result-3";
  entry.cached_mga_security_digest = "sha256:mga-security-3";
  return entry;
}

exec::SnapshotSafeCacheStoreRequest StoreRequest() {
  exec::SnapshotSafeCacheStoreRequest request;
  request.entry = Entry();
  request.read_only_operation = true;
  request.small_final_result = true;
  request.max_small_result_rows = 16;
  return request;
}

exec::SnapshotSafeCacheLookupRequest LookupRequest() {
  exec::SnapshotSafeCacheLookupRequest request;
  request.key = BaseKey();
  request.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  request.read_only_operation = true;
  request.small_final_result = true;
  request.row_count = 3;
  request.max_small_result_rows = 16;
  request.recomputed_result_digest = "sha256:result-3";
  request.recomputed_mga_security_digest = "sha256:mga-security-3";
  return request;
}

void ProveMetricSourceQualityHardening() {
  Require(api::PerformanceMetricEventSchemaVersion() == 3,
          "metric schema version was not advanced for provenance split");
  for (const std::string field :
       {"phase_timings", "storage_timings", "agent_counters", "cache_flags"}) {
    Require(SchemaHasRequiredField(field + "_measurement_source"),
            "schema missing family measurement source: " + field);
    Require(SchemaHasRequiredField(field + "_measurement_quality"),
            "schema missing family measurement quality: " + field);
  }
  for (const std::string field :
       {"cpu_sample",
        "allocator_counter",
        "lock_latch_wait",
        "syscall_count",
        "file_io_count",
        "page_fault_count",
        "context_switch_count",
        "evidence_rendering",
        "result_formatting",
        "regression_budget"}) {
    Require(SchemaHasRequiredField(field + "_measurement_source"),
            "schema missing low-level measurement source: " + field);
    Require(SchemaHasRequiredField(field + "_measurement_quality"),
            "schema missing low-level measurement quality: " + field);
  }
  for (const std::string field :
       {"parser_lowering",
        "sbps_listener",
        "sblr_dispatch",
        "internal_api",
        "executor",
        "storage",
        "index_layer",
        "transaction",
        "result_rendering",
        "evidence_construction",
        "allocation",
        "syscall",
        "wait"}) {
    Require(SchemaHasRequiredField(field + "_measurement_source"),
            "schema missing measurement source: " + field);
    Require(SchemaHasRequiredField(field + "_measurement_quality"),
            "schema missing measurement quality: " + field);
  }

  const auto event = CompleteMetricEvent();
  const auto valid = api::ValidatePerformanceMetricEvent(event);
  Require(valid.ok, "complete source-qualified metric event rejected");
  const auto json = api::SerializePerformanceMetricEventJson(event);
  Require(ContainsText(json, "\"schema_id\":\"scratchbird.performance_metric_event.v3\""),
          "schema v3 id missing from serialization");
  Require(ContainsText(json,
                       "\"phase_timings_measurement_source\":\"measured_by_internal_counter\""),
          "phase timings measurement source missing");
  Require(ContainsText(json,
                       "\"storage_timings_measurement_quality\":\"not_available_zeroed\""),
          "storage timings measurement quality missing");
  Require(ContainsText(json,
                       "\"agent_counters_measurement_source\":\"measured_by_platform_api\""),
          "agent counter measurement source missing");
  Require(ContainsText(json, "\"cache_flags_measurement_quality\":\"measured\""),
          "cache flag measurement quality missing");
  Require(ContainsText(json,
                       "\"sbps_listener_measurement_source\":\"measured_by_perf_sample\""),
          "perf sample measurement source missing");
  Require(ContainsText(json, "\"allocator_counter_measurement_quality\":\"actual_zero\""),
          "low-level actual-zero measurement quality missing");
  Require(ContainsText(json,
                       "\"lock_latch_wait_measurement_quality\":\"not_available_zeroed\""),
          "low-level not-available-zeroed measurement quality missing");
  Require(ContainsText(json, "\"file_io_count_measurement_source\":\"disabled\""),
          "low-level disabled measurement source missing");
  Require(ContainsText(json,
                       "\"context_switch_count_measurement_quality\":\"unsupported\""),
          "low-level unsupported measurement quality missing");
  Require(ContainsText(json,
                       "\"regression_budget_measurement_source\":\"estimated\""),
          "low-level estimated measurement source missing");
  Require(ContainsText(json,
                       "\"sblr_dispatch_measurement_source\":\"measured_by_platform_api\""),
          "platform API measurement source missing");
  Require(ContainsText(json,
                       "\"storage_measurement_quality\":\"not_available_zeroed\""),
          "not-available zeroed quality missing");
  Require(ContainsText(json, "\"index_layer_measurement_quality\":\"disabled\""),
          "disabled quality missing");
  Require(ContainsText(json, "\"transaction_measurement_source\":\"unsupported\""),
          "unsupported source missing");
  Require(ContainsText(json, "\"result_rendering_measurement_quality\":\"actual_zero\""),
          "actual zero quality missing");

  auto missing = event;
  missing.phase_timings.measurement_source.clear();
  missing.hot_path_attribution.cpu_sample_measurement_source.clear();
  missing.hot_path_attribution.executor_measurement_source.clear();
  missing.hot_path_attribution.executor_measurement_quality.clear();
  const auto missing_result = api::ValidatePerformanceMetricEvent(missing);
  Require(!missing_result.ok, "missing measurement provenance was accepted");
  Require(Contains(missing_result.missing_fields,
                   "phase_timings_measurement_source"),
          "missing family measurement source not reported");
  Require(Contains(missing_result.missing_fields,
                   "cpu_sample_measurement_source"),
          "missing low-level measurement source not reported");
  Require(Contains(missing_result.missing_fields,
                   "executor_measurement_source"),
          "missing measurement source not reported");
  Require(Contains(missing_result.missing_fields,
                   "executor_measurement_quality"),
          "missing measurement quality not reported");

  auto unknown = event;
  unknown.hot_path_attribution.storage_measurement_source = "measured";
  unknown.hot_path_attribution.storage_measurement_quality = "unavailable";
  const auto unknown_result = api::ValidatePerformanceMetricEvent(unknown);
  Require(!unknown_result.ok, "unknown measurement provenance was accepted");
  Require(Contains(unknown_result.missing_fields,
                   "storage_measurement_source"),
          "unknown measurement source not reported");
  Require(Contains(unknown_result.missing_fields,
                   "storage_measurement_quality"),
          "unknown measurement quality not reported");
}

void ProveSnapshotCacheResultContractHardening() {
  exec::SnapshotSafeResultCache cache;
  const auto store = cache.Store(StoreRequest());
  Require(store.action == exec::SnapshotSafeCacheAction::kStore,
          "snapshot cache store was not accepted");
  Require(Contains(store.evidence,
                   "result_contract_identity=orh110.rowset.v1"),
          "result contract identity evidence missing");
  Require(Contains(store.evidence,
                   "result_contract_hash=sha256:orh110-rowset-contract"),
          "result contract hash evidence missing");

  const auto hit = cache.Lookup(LookupRequest());
  Require(hit.action == exec::SnapshotSafeCacheAction::kHit && hit.cache_hit,
          "snapshot cache hit was not accepted after recompute proof");
  Require(Contains(hit.evidence, "snapshot_cache_recompute_result_match=true"),
          "result recompute proof missing");
  Require(Contains(hit.evidence,
                   "snapshot_cache_recompute_mga_security_match=true"),
          "MGA/security recompute proof missing");

  auto missing_contract = StoreRequest();
  missing_contract.entry.key.result_contract_hash.clear();
  const auto incomplete = cache.Store(missing_contract);
  Require(incomplete.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.KEY_INCOMPLETE",
          "incomplete result contract did not fail key completeness");

  auto uncertain = StoreRequest();
  uncertain.result_contract_uncertain = true;
  const auto uncertain_decision = cache.Store(uncertain);
  Require(uncertain_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.RESULT_CONTRACT_UNCERTAIN",
          "result-contract uncertainty diagnostic mismatch");

  auto mutable_provider = StoreRequest();
  mutable_provider.provider_generation_mutable = true;
  const auto mutable_decision = cache.Store(mutable_provider);
  Require(mutable_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.PROVIDER_GENERATION_MUTABLE",
          "mutable provider generation diagnostic mismatch");

  auto route = LookupRequest();
  route.route_mismatch = true;
  const auto route_decision = cache.Lookup(route);
  Require(route_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.ROUTE_MISMATCH",
          "route mismatch diagnostic mismatch");

  auto dialect = LookupRequest();
  dialect.dialect_mismatch = true;
  const auto dialect_decision = cache.Lookup(dialect);
  Require(dialect_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.DIALECT_MISMATCH",
          "dialect mismatch diagnostic mismatch");

  auto mismatch = LookupRequest();
  mismatch.recomputed_result_digest = "sha256:changed";
  const auto invalidated = cache.Lookup(mismatch);
  Require(invalidated.action ==
              exec::SnapshotSafeCacheAction::kInvalidateRecompute,
          "result digest mismatch did not invalidate before hit");
  Require(!invalidated.cache_hit, "mismatched digest produced a hit");
}

void ProveSnapshotCacheVolatilityAuthorityAndNegativeRefusals() {
  exec::SnapshotSafeResultCache cache;
  cache.Store(StoreRequest());

  auto volatile_lookup = LookupRequest();
  volatile_lookup.volatile_function_dependency = true;
  const auto volatile_decision = cache.Lookup(volatile_lookup);
  Require(volatile_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.VOLATILE_FUNCTION_REFUSED",
          "volatile function diagnostic mismatch");

  auto own_txn = LookupRequest();
  own_txn.uncommitted_own_transaction_visibility_dependency = true;
  const auto own_txn_decision = cache.Lookup(own_txn);
  Require(own_txn_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.OWN_TRANSACTION_VISIBILITY_REFUSED",
          "own-transaction visibility diagnostic mismatch");

  auto negative = LookupRequest();
  negative.negative_cache_entry = true;
  negative.negative_cache_snapshot_safe_proven = true;
  const auto negative_decision = cache.Lookup(negative);
  Require(negative_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.NEGATIVE_CACHE_REFUSED",
          "negative-cache diagnostic mismatch");

  auto authority = LookupRequest();
  authority.storage_authority_cached = true;
  authority.authorization_authority_cached = true;
  authority.visibility_authority_cached = true;
  authority.transaction_finality_authority_cached = true;
  authority.recovery_authority_cached = true;
  authority.parser_execution_authority_cached = true;
  authority.reference_behavior_authority_cached = true;
  authority.durability_log_authority_cached = true;
  const auto authority_decision = cache.Lookup(authority);
  Require(authority_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.AUTHORITY_REFUSED",
          "authority-bearing cache entry diagnostic mismatch");
  Require(Contains(authority_decision.evidence,
                   "cache_transaction_finality_authority=false"),
          "cache must still declare no finality authority");

  auto missing_recompute = LookupRequest();
  missing_recompute.ordinary_recompute_available = false;
  const auto recompute_decision = cache.Lookup(missing_recompute);
  Require(recompute_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.RECOMPUTE_PROOF_REQUIRED",
          "ordinary recompute proof diagnostic mismatch");
}

}  // namespace

int main() {
  ProveMetricSourceQualityHardening();
  ProveSnapshotCacheResultContractHardening();
  ProveSnapshotCacheVolatilityAuthorityAndNegativeRefusals();
  return 0;
}
