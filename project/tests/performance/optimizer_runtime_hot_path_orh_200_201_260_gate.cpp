// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/performance_metric_event.hpp"
#include "runtime_consumption_evidence.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace opt = scratchbird::engine::optimizer;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ORH-200/201/260 gate failure: " << message << '\n';
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

void SetWholeRouteMeasurementProvenance(
    api::PerformanceMetricHotPathAttribution* hot) {
  hot->cpu_sample_measurement_source = "measured_by_perf_sample";
  hot->cpu_sample_measurement_quality = "measured";
  hot->allocator_counter_measurement_source = "measured_by_internal_counter";
  hot->allocator_counter_measurement_quality = "measured";
  hot->lock_latch_wait_measurement_source = "measured_by_internal_counter";
  hot->lock_latch_wait_measurement_quality = "actual_zero";
  hot->syscall_count_measurement_source = "measured_by_platform_api";
  hot->syscall_count_measurement_quality = "measured";
  hot->file_io_count_measurement_source = "measured_by_platform_api";
  hot->file_io_count_measurement_quality = "measured";
  hot->page_fault_count_measurement_source = "measured_by_platform_api";
  hot->page_fault_count_measurement_quality = "actual_zero";
  hot->context_switch_count_measurement_source = "measured_by_platform_api";
  hot->context_switch_count_measurement_quality = "measured";
  hot->evidence_rendering_measurement_source = "measured_by_internal_counter";
  hot->evidence_rendering_measurement_quality = "measured";
  hot->result_formatting_measurement_source = "measured_by_internal_counter";
  hot->result_formatting_measurement_quality = "actual_zero";
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
  hot->result_rendering_measurement_quality = "actual_zero";
  hot->evidence_construction_measurement_source = "measured_by_internal_counter";
  hot->evidence_construction_measurement_quality = "measured";
  hot->allocation_measurement_source = "estimated";
  hot->allocation_measurement_quality = "estimated";
  hot->syscall_measurement_source = "estimated";
  hot->syscall_measurement_quality = "estimated";
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

opt::ReferenceDominanceTargetEvidence ComparableTarget(
    std::string workload,
    std::string category,
    std::string reference_engine,
    double current_ms,
    double reference_ms,
    double prior_ms) {
  opt::ReferenceDominanceTargetEvidence evidence;
  evidence.workload = std::move(workload);
  evidence.category = std::move(category);
  evidence.comparable = true;
  evidence.comparable_status = "comparable";
  evidence.reference_best_engine = std::move(reference_engine);
  evidence.reference_best_duration_ms = reference_ms;
  evidence.scratchbird_current_duration_ms = current_ms;
  evidence.prior_scratchbird_duration_available = true;
  evidence.scratchbird_prior_duration_ms = prior_ms;
  evidence.dominance_target_duration_ms = reference_ms - 0.000001;
  evidence.dominance_target_rule = "strictly_less_than_reference_best_duration";
  evidence.exact_blocker_rule =
      "SB_ORH_REFERENCE_DOMINANCE_TARGET.UNRESOLVED_REFERENCE_GAP";
  evidence.diagnostic_code = "SB_ORH_REFERENCE_DOMINANCE_TARGET.COMPARABLE";
  return evidence;
}

opt::ReferenceDominanceTargetEvidence NonComparableMicroTarget(
    std::string workload,
    double current_ms) {
  opt::ReferenceDominanceTargetEvidence evidence;
  evidence.workload = std::move(workload);
  evidence.category = "micro";
  evidence.comparable = false;
  evidence.comparable_status = "non_comparable";
  evidence.scratchbird_current_duration_ms = current_ms;
  evidence.dominance_target_rule = "reference_equivalent_micro_required";
  evidence.exact_blocker_rule =
      "SB_ORH_REFERENCE_DOMINANCE_TARGET.MICRO_REFERENCE_EQUIVALENCE_REQUIRED";
  evidence.diagnostic_code = "SB_ORH_REFERENCE_DOMINANCE_TARGET.NON_COMPARABLE";
  return evidence;
}

std::vector<opt::ReferenceDominanceTargetEvidence> Execution_Plan10DominanceFixture() {
  return {
      NonComparableMicroTarget("single_insert", 516.030333),
      NonComparableMicroTarget("point_select", 515.888137),
      NonComparableMicroTarget("simple_aggregate", 516.365919),
      ComparableTarget("customers_load", "load", "postgresql", 1825.441837,
                       360.599518, 2615.075350),
      ComparableTarget("products_load", "load", "postgresql", 1072.378159,
                       114.174128, 758.158684),
      ComparableTarget("orders_load", "load", "postgresql", 10377.938032,
                       1299.834728, 8303.085566),
      ComparableTarget("order_items_load", "load", "postgresql",
                       65721.184731, 3009.978771, 14778.240681),
      ComparableTarget("inner_join_simple", "join", "postgresql",
                       5760.205984, 64.720154, 3410.299301),
      ComparableTarget("inner_join_large_result", "join", "postgresql",
                       7683.731318, 461.060047, 5435.934305),
      ComparableTarget("inner_join_multiple_conditions", "join_predicate",
                       "postgresql", 3834.690332, 3.732204, 429.449558),
      ComparableTarget("left_join_all_customers", "join_aggregate",
                       "postgresql", 5139.183760, 34.266233, 3382.730484),
      ComparableTarget("four_table_join", "join", "mysql", 6096.239090,
                       1.808167, 1089.739084),
      ComparableTarget("self_join_same_country", "join", "postgresql",
                       5263.990879, 11.940002, 90.133905),
      ComparableTarget("bulk_update_with_join", "dml_update", "mysql",
                       4228.485823, 170.458078, 134.031773),
  };
}

std::vector<std::string> RequiredExecution_Plan10Workloads() {
  return {"single_insert",
          "point_select",
          "simple_aggregate",
          "customers_load",
          "products_load",
          "orders_load",
          "order_items_load",
          "inner_join_simple",
          "inner_join_large_result",
          "inner_join_multiple_conditions",
          "left_join_all_customers",
          "four_table_join",
          "self_join_same_country",
          "bulk_update_with_join"};
}

api::PerformanceMetricEvent CompleteWholeRouteProfilerEvent() {
  api::PerformanceMetricEvent event;
  event.route = "embedded.sblr.bulk_load";
  event.operation = "optimizer_runtime_hot_path.orh_201";
  SetMetricFamilyProvenance(&event);
  event.phase_timings.parse_us = 0;
  event.phase_timings.bind_us = 3;
  event.phase_timings.lower_us = 4;
  event.phase_timings.plan_us = 7;
  event.phase_timings.execute_us = 19;
  event.storage_timings.append_us = 11;
  event.storage_timings.page_us = 5;
  event.storage_timings.index_us = 2;
  event.agent_counters.agent_thread_count = 1;
  event.agent_counters.agent_cpu_user_us = 31;
  event.agent_counters.agent_cpu_system_us = 2;
  event.agent_counters.agent_wait_us = 0;
  event.agent_counters.agent_io_read_bytes = 0;
  event.agent_counters.agent_io_write_bytes = 8192;
  event.cache_flags.plan_cache_hit = true;
  event.cache_flags.metadata_cache_hit = true;
  event.cache_flags.page_cache_hit = true;
  event.cache_flags.index_cache_hit = true;
  event.hot_path_attribution.cpu_sample_count = 100;
  event.hot_path_attribution.cpu_sample_attributed_count = 99;
  event.hot_path_attribution.cpu_sample_attribution = "whole_route_bucket";
  event.hot_path_attribution.allocator_allocation_count = 3;
  event.hot_path_attribution.allocator_allocation_bytes = 384;
  event.hot_path_attribution.lock_latch_wait_count = 0;
  event.hot_path_attribution.lock_latch_wait_us = 0;
  event.hot_path_attribution.syscall_count = 4;
  event.hot_path_attribution.file_open_count = 0;
  event.hot_path_attribution.file_flush_count = 1;
  event.hot_path_attribution.file_fsync_count = 0;
  event.hot_path_attribution.page_fault_count = 0;
  event.hot_path_attribution.context_switch_count = 1;
  event.hot_path_attribution.evidence_rendering_us = 1;
  event.hot_path_attribution.result_formatting_us = 0;
  event.hot_path_attribution.regression_budget_us = 1000;
  event.hot_path_attribution.regression_budget_margin_us = 947;
  event.hot_path_attribution.regression_budget_validated = true;
  event.hot_path_attribution.parser_lowering_us = 4;
  event.hot_path_attribution.sbps_listener_us = 2;
  event.hot_path_attribution.sblr_dispatch_us = 3;
  event.hot_path_attribution.internal_api_us = 5;
  event.hot_path_attribution.executor_us = 19;
  event.hot_path_attribution.storage_us = 16;
  event.hot_path_attribution.index_layer_us = 2;
  event.hot_path_attribution.transaction_us = 1;
  event.hot_path_attribution.result_rendering_us = 0;
  event.hot_path_attribution.evidence_construction_us = 1;
  event.hot_path_attribution.allocation_us = 1;
  event.hot_path_attribution.syscall_us = 2;
  event.hot_path_attribution.wait_us = 0;
  SetWholeRouteMeasurementProvenance(&event.hot_path_attribution);
  event.statistics_epoch = 7;
  event.resource_governor_state = "admitted";
  event.message_vector_present = false;
  event.result_hash = "orh201-result-hash";
  event.overhead_mode = api::InstrumentationOverheadMode::kBenchmarkClean;
  return event;
}

opt::BenchmarkMethodEvidence Method(std::string engine, std::string method) {
  opt::BenchmarkMethodEvidence evidence;
  evidence.engine = std::move(engine);
  evidence.logical_task = "bulk_load_order_items";
  evidence.workload_family = "bulk_load";
  evidence.method = std::move(method);
  evidence.best_normal_method = true;
  evidence.native_bulk_or_best_engine_path = true;
  evidence.prepared_or_warmed = true;
  evidence.output_suppressed = true;
  evidence.result_materialization_policy = "rows_affected_only";
  evidence.transaction_policy = "single_explicit_transaction";
  evidence.data_generator_id = "workload10_order_items_generator_v1";
  evidence.scale_profile = "execution_plan10_preserved_scale";
  evidence.skew_profile = "execution_plan10_preserved_skew";
  evidence.resource_budget_profile = "four_worker_2gb_memory_budget";
  evidence.constraint_policy = "same_constraints_enabled";
  evidence.reference_reference_only = evidence.engine != "scratchbird";
  evidence.uses_reference_storage_or_finality_for_scratchbird = false;
  evidence.diagnostic_code = "SB_ORH_BEST_METHOD_EQUIVALENCE.METHOD_READY";
  return evidence;
}

void ReferenceDominanceTargetContractPassesAndFailsClosed() {
  const auto fixture = Execution_Plan10DominanceFixture();
  for (const auto& item : fixture) {
    const auto validation = opt::ValidateReferenceDominanceTargetEvidence(item);
    Require(validation.ok, item.workload + " reference target rejected");
    Require(!item.diagnostic_code.empty(),
            item.workload + " diagnostic_code missing");
  }

  const auto set_validation = opt::ValidateReferenceDominanceTargetSet(
      fixture, RequiredExecution_Plan10Workloads());
  Require(set_validation.ok, "complete Execution_Plan 10 reference target set rejected");

  auto broken = ComparableTarget("broken_join", "join", "postgresql", 10.0,
                                 5.0, 9.0);
  broken.reference_best_engine.clear();
  broken.diagnostic_code.clear();
  const auto rejected = opt::ValidateReferenceDominanceTargetEvidence(broken);
  Require(!rejected.ok, "broken comparable reference target accepted");
  Require(rejected.diagnostic_code ==
              "SB_ORH_REFERENCE_DOMINANCE_TARGET.MISSING_REQUIRED_FIELD",
          "broken comparable reference target diagnostic mismatch");
  Require(Contains(rejected.missing_fields, "reference_best_engine"),
          "missing reference_best_engine not reported");
  Require(Contains(rejected.missing_fields, "diagnostic_code"),
          "missing diagnostic_code not reported");
}

void WholeRouteProfilerAttributionPassesAndFailsClosed() {
  const auto& schema = api::PerformanceMetricEventSchema();
  std::set<std::string> schema_names;
  for (const auto& field : schema) {
    Require(schema_names.insert(field.name).second,
            "duplicate performance metric schema field: " + field.name);
  }
  const auto storage_index_field =
      std::find_if(schema.begin(), schema.end(), [&](const auto& entry) {
        return entry.name == "index_us" && entry.required;
      });
  const auto profiler_index_field =
      std::find_if(schema.begin(), schema.end(), [&](const auto& entry) {
        return entry.name == "index_layer_us" && entry.required;
      });
  Require(storage_index_field != schema.end(),
          "storage timing index_us schema field missing");
  Require(profiler_index_field != schema.end(),
          "profiler index_layer_us schema field missing");

  for (const std::string field :
       {"parser_lowering_us",
        "sbps_listener_us",
        "sblr_dispatch_us",
        "internal_api_us",
        "executor_us",
        "storage_us",
        "index_layer_us",
        "transaction_us",
        "result_rendering_us",
        "evidence_construction_us",
        "allocation_us",
        "syscall_us",
        "wait_us",
        "cpu_sample_measurement_source",
        "cpu_sample_measurement_quality",
        "allocator_counter_measurement_source",
        "allocator_counter_measurement_quality",
        "lock_latch_wait_measurement_source",
        "lock_latch_wait_measurement_quality",
        "syscall_count_measurement_source",
        "syscall_count_measurement_quality",
        "file_io_count_measurement_source",
        "file_io_count_measurement_quality",
        "page_fault_count_measurement_source",
        "page_fault_count_measurement_quality",
        "context_switch_count_measurement_source",
        "context_switch_count_measurement_quality",
        "evidence_rendering_measurement_source",
        "evidence_rendering_measurement_quality",
        "result_formatting_measurement_source",
        "result_formatting_measurement_quality",
        "regression_budget_measurement_source",
        "regression_budget_measurement_quality",
        "parser_lowering_measurement_source",
        "parser_lowering_measurement_quality",
        "sbps_listener_measurement_source",
        "sbps_listener_measurement_quality",
        "sblr_dispatch_measurement_source",
        "sblr_dispatch_measurement_quality",
        "internal_api_measurement_source",
        "internal_api_measurement_quality",
        "executor_measurement_source",
        "executor_measurement_quality",
        "storage_measurement_source",
        "storage_measurement_quality",
        "index_layer_measurement_source",
        "index_layer_measurement_quality",
        "transaction_measurement_source",
        "transaction_measurement_quality",
        "result_rendering_measurement_source",
        "result_rendering_measurement_quality",
        "evidence_construction_measurement_source",
        "evidence_construction_measurement_quality",
        "allocation_measurement_source",
        "allocation_measurement_quality",
        "syscall_measurement_source",
        "syscall_measurement_quality",
        "wait_measurement_source",
        "wait_measurement_quality"}) {
    const auto found = std::find_if(schema.begin(), schema.end(),
                                    [&](const auto& entry) {
                                      return entry.name == field &&
                                             entry.required;
                                    });
    Require(found != schema.end(), "schema missing " + field);
  }

  const auto event = CompleteWholeRouteProfilerEvent();
  const auto validation = api::ValidatePerformanceMetricEvent(event);
  Require(validation.ok, "whole-route profiler event rejected");
  const auto json = api::SerializePerformanceMetricEventJson(event);
  Require(ContainsText(json, "\"index_us\":2"),
          "storage timing index_us not serialized");
  Require(ContainsText(json, "\"index_layer_us\":2"),
          "profiler index_layer_us not serialized");
  Require(ContainsText(json, "\"sblr_dispatch_us\":3"),
          "SBLR dispatch attribution not serialized");
  Require(ContainsText(json, "\"cpu_sample_measurement_source\":\"measured_by_perf_sample\""),
          "CPU sample measurement source not serialized");
  Require(ContainsText(json,
                       "\"result_formatting_measurement_quality\":\"actual_zero\""),
          "result formatting actual-zero quality not serialized");
  Require(ContainsText(json,
                       "\"regression_budget_measurement_source\":\"estimated\""),
          "regression budget measurement source not serialized");
  Require(ContainsText(json, "\"transaction_measurement_source\":\"estimated\""),
          "transaction measurement source not serialized");
  Require(ContainsText(json, "\"transaction_measurement_quality\":\"estimated\""),
          "transaction measurement quality not serialized");
  Require(ContainsText(json, "\"wait_measurement_source\":\"measured_by_internal_counter\""),
          "wait measurement source not serialized");
  Require(ContainsText(json, "\"wait_measurement_quality\":\"actual_zero\""),
          "actual-zero wait measurement quality not serialized");

  auto broken = event;
  broken.hot_path_attribution.transaction_us.reset();
  broken.hot_path_attribution.storage_measurement_source = "unknown";
  broken.hot_path_attribution.storage_measurement_quality = "unknown";
  const auto rejected = api::ValidatePerformanceMetricEvent(broken);
  Require(!rejected.ok, "incomplete whole-route profiler event accepted");
  Require(Contains(rejected.missing_fields, "transaction_us"),
          "missing transaction_us not reported");
  Require(Contains(rejected.missing_fields, "storage_measurement_source"),
          "invalid storage_measurement_source not reported");
  Require(Contains(rejected.missing_fields, "storage_measurement_quality"),
          "invalid storage_measurement_quality not reported");
}

void BestMethodBenchmarkEquivalencePassesAndFailsClosed() {
  const std::vector<opt::BenchmarkMethodEvidence> methods = {
      Method("scratchbird", "native_direct_bulk_ingest"),
      Method("firebird", "best_bulk_insert_or_external_table_path"),
      Method("mysql", "load_data_or_best_bulk_insert_path"),
      Method("postgresql", "copy_best_bulk_path"),
  };
  const std::vector<std::string> required_engines = {
      "scratchbird", "firebird", "mysql", "postgresql"};
  const auto validation =
      opt::ValidateBestMethodBenchmarkEquivalence(methods, required_engines);
  Require(validation.ok, "best-method benchmark equivalence rejected");
  Require(validation.diagnostic_code == "SB_ORH_BEST_METHOD_EQUIVALENCE.OK",
          "best-method success diagnostic mismatch");

  auto broken = methods;
  broken[2].result_materialization_policy = "client_printed_rows";
  broken[2].diagnostic_code.clear();
  broken[3].uses_reference_storage_or_finality_for_scratchbird = true;
  const auto rejected =
      opt::ValidateBestMethodBenchmarkEquivalence(broken, required_engines);
  Require(!rejected.ok, "broken benchmark equivalence accepted");
  Require(rejected.diagnostic_code ==
              "SB_ORH_BEST_METHOD_EQUIVALENCE.FAILED",
          "broken benchmark equivalence diagnostic mismatch");
  Require(Contains(rejected.diagnostics,
                   "mysql:SB_ORH_BEST_METHOD_EQUIVALENCE.DIAGNOSTIC_REQUIRED"),
          "missing method diagnostic not reported");
  Require(Contains(rejected.diagnostics,
                   "mysql:SB_ORH_BEST_METHOD_EQUIVALENCE.CONTROL_MISMATCH"),
          "materialization mismatch not reported");
  Require(Contains(rejected.diagnostics,
                   "postgresql:SB_ORH_BEST_METHOD_EQUIVALENCE.MGA_AUTHORITY_DRIFT"),
          "MGA authority drift not reported");
}

}  // namespace

int main() {
  ReferenceDominanceTargetContractPassesAndFailsClosed();
  WholeRouteProfilerAttributionPassesAndFailsClosed();
  BestMethodBenchmarkEquivalencePassesAndFailsClosed();
  return 0;
}
