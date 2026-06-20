// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/sys_information_projection.hpp"
#include "memory_observability_overhead.hpp"
#include "observability/show_api.hpp"
#include "query/plan_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace info = scratchbird::engine::internal_api;
namespace mem = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

info::SysInformationProjectionContext Context() {
  info::SysInformationProjectionContext context;
  context.catalog_display_name = "IPARDB";
  context.session_language = "en";
  context.default_language = "en";
  context.visible_catalog_generation_id = 7;
  context.cluster_authority_available = false;
  return context;
}

info::EngineRequestContext EngineContext() {
  info::EngineRequestContext context;
  context.database_path = "/tmp/ipar_observability_projection_gate.sbdb";
  context.database_uuid.canonical = "database:ipar-observability";
  context.principal_uuid.canonical = "principal:sysarch";
  context.session_uuid.canonical = "session:ipar-observability";
  context.catalog_generation_id = 7;
  context.security_epoch = 7;
  context.resource_epoch = 7;
  context.name_resolution_epoch = 7;
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

std::string Field(const info::SysInformationProjectionRow& row,
                  std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) { return value; }
  }
  return {};
}

std::string Field(const info::EngineRowValue& row,
                  std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) { return value.encoded_value; }
  }
  return {};
}

bool HasEvidence(const info::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

bool HasColumn(const info::SysInformationProjectionDefinition& definition,
               std::string_view name,
               std::string_view logical_type = {}) {
  for (const auto& column : definition.columns) {
    if (column.column_name == name &&
        (logical_type.empty() || column.logical_type == logical_type)) {
      return true;
    }
  }
  return false;
}

bool HasRowValue(const info::SysInformationProjectionResult& result,
                 std::string_view field_name,
                 std::string_view value) {
  for (const auto& row : result.rows) {
    if (Field(row, field_name) == value) { return true; }
  }
  return false;
}

bool HasRowValue(const info::EngineApiResult& result,
                 std::string_view field_name,
                 std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    if (Field(row, field_name) == value) { return true; }
  }
  return false;
}

void RequireOk(const info::SysInformationProjectionResult& result,
               const std::string& message) {
  if (!result.ok) {
    std::cerr << result.diagnostic_code << " " << result.diagnostic_detail << '\n';
  }
  Require(result.ok, message);
}

void RequireOk(const info::EngineApiResult& result,
               const std::string& message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << " " << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, message);
}

void RequireNoPrivateLeak(const info::SysInformationProjectionResult& result) {
  for (const auto& row : result.rows) {
    for (const auto& [field_name, value] : row.fields) {
      Require(value.find("/tmp/private") == std::string::npos,
              "private path leaked in " + field_name);
      Require(value.find("secret=") == std::string::npos,
              "secret payload leaked in " + field_name);
      Require(value.find("raw-principal") == std::string::npos,
              "raw principal leaked in " + field_name);
    }
  }
}

std::vector<info::SysInformationIparAgentLifecycleSource> LifecycleRows() {
  return {
      {.runtime_id = "server_agent_runtime",
       .source_kind = "server_agent_runtime",
       .lifecycle_state = "running",
       .idle_state = "idle_resident",
       .worker_wake_policy = "staggered_worker_per_scheduler_tick",
       .worker_thread_count = 5,
       .background_worker_slots = 5,
       .foreground_reserved_capacity = 1,
       .scheduler_ticks = 42,
       .total_worker_ticks = 42,
       .total_actions_accepted = 2,
       .total_actions_refused = 1,
       .durable_lease_count = 5,
       .durable_action_backlog_count = 0,
       .durable_replay_pending_action_count = 0,
       .process_rss_kb = 64000,
       .process_vsize_kb = 128000,
       .source_state = "observed",
       .catalog_generation_id = 2,
       .started = true},
      {.runtime_id = "stale_epoch_agent_runtime",
       .catalog_generation_id = 99},
      {.runtime_id = "hidden_agent_runtime",
       .catalog_generation_id = 2,
       .hidden = true},
  };
}

std::vector<info::SysInformationIparMetricCounterSource> CounterRows() {
  return {
      {.metric_id = "IPAR-M001",
       .metric_path = "sys.metrics.ipar.script.prepared_descriptor_hits",
       .metric_type = "counter",
       .metric_unit = "count",
       .label_summary = "script_id=SBDFS-020",
       .producer = "engine_insert",
       .source_state = "observed",
       .value = 1048576,
       .sample_count = 64,
       .catalog_generation_id = 2},
      {.metric_id = "IPAR-M035",
       .metric_path = "sys.metrics.ipar.script.slow_path_reason_count",
       .metric_type = "counter",
       .metric_unit = "count",
       .label_summary = "chosen_path=degraded_path",
       .producer = "engine_insert",
       .source_state = "observed",
       .value = 1,
       .sample_count = 1,
       .catalog_generation_id = 2},
  };
}

std::vector<info::SysInformationIparTelemetryControlSource> TelemetryRows() {
  return {
      {.budget_id = "IPAR-M031",
       .control_name = "metric_persist_stride",
       .metric_path = "sys.metrics.ipar.telemetry.metric_persist_stride",
       .source_state = "observed",
       .overhead_budget_percent = "5.0",
       .configured_value = 128,
       .observed_value = 128,
       .sample_rate_per_mille = 7,
       .persist_stride = 128,
       .skipped_count = 4096,
       .dropped_metric_count = 0,
       .catalog_generation_id = 2},
      {.budget_id = "IPAR-M031",
       .control_name = "audit_persist_stride",
       .metric_path = "sys.metrics.ipar.telemetry.audit_persist_stride",
       .source_state = "observed",
       .overhead_budget_percent = "5.0",
       .configured_value = 128,
       .observed_value = 128,
       .sample_rate_per_mille = 7,
       .persist_stride = 128,
       .skipped_count = 2048,
       .dropped_metric_count = 0,
       .catalog_generation_id = 2},
      {.budget_id = "IPAR-M031",
       .control_name = "dropped_metric_count",
       .metric_path = "sys.metrics.ipar.telemetry.dropped_metric_count",
       .source_state = "observed",
       .overhead_budget_percent = "5.0",
       .configured_value = 0,
       .observed_value = 0,
       .sample_rate_per_mille = 7,
       .persist_stride = 128,
       .skipped_count = 0,
       .dropped_metric_count = 0,
       .catalog_generation_id = 2},
  };
}

std::vector<info::SysInformationIparSlowPathReasonSource> SlowPathRows() {
  return {
      {.metric_id = "IPAR-M035",
       .statement_id = "SBDFS-059:copy-batch-4",
       .chosen_path = "degraded_path",
       .reason_code = "relation_state_full_load_required",
       .validation_stage = "pre_execution_validation",
       .driver_visible_message = "COPY used full validation because descriptor epochs were stale.",
       .diagnostic_code = "SB_IPAR_SLOW_PATH_REASON_RECORDED",
       .source_state = "observed",
       .fallback_count = 1,
       .sample_count = 1,
       .catalog_generation_id = 2},
  };
}

info::SysInformationProjectionResult Build(
    std::string_view view_path,
    std::vector<info::SysInformationIparAgentLifecycleSource> lifecycle = {},
    std::vector<info::SysInformationIparMetricCounterSource> counters = {},
    std::vector<info::SysInformationIparTelemetryControlSource> telemetry = {},
    std::vector<info::SysInformationIparSlowPathReasonSource> slow_paths = {}) {
  return info::BuildSysInformationProjection(
      view_path,
      Context(),
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      std::move(lifecycle),
      std::move(counters),
      std::move(telemetry),
      std::move(slow_paths));
}

void TestDefinitions() {
  const auto validation = info::ValidateBuiltinSysInformationProjectionDefinitions();
  if (!validation.ok) {
    for (const auto& code : validation.diagnostic_codes) {
      std::cerr << code << '\n';
    }
  }
  Require(validation.ok, "builtin sys projection definitions failed validation");

  const std::vector<std::string> views = {
      "sys.ipar.agent_lifecycle",
      "sys.ipar.metric_counters",
      "sys.ipar.telemetry_controls",
      "sys.ipar.slow_path_reasons",
  };
  for (const auto& view : views) {
    const auto* definition = info::FindSysInformationProjectionDefinition(view);
    Require(definition != nullptr, view + " definition missing");
    Require(definition->view_scope == "local", view + " must remain local");
    Require(definition->authorization_filter_required,
            view + " must require authorization filtering");
    Require(definition->redaction_required, view + " must require redaction");
    Require(definition->mga_snapshot_visibility_required,
            view + " must remain snapshot-visible metadata");
    Require(definition->cluster_path_fail_closed,
            view + " must fail closed on cluster paths");
  }

  const auto* lifecycle =
      info::FindSysInformationProjectionDefinition("sys.ipar.agent_lifecycle");
  Require(HasColumn(*lifecycle, "worker_thread_count", "uint64"),
          "agent lifecycle worker count column missing");
  Require(HasColumn(*lifecycle, "idle_state", "text"),
          "agent lifecycle idle state column missing");

  const auto* telemetry =
      info::FindSysInformationProjectionDefinition("sys.ipar.telemetry_controls");
  Require(HasColumn(*telemetry, "sample_rate_per_mille", "uint64"),
          "telemetry sample rate column missing");

  const auto* slow =
      info::FindSysInformationProjectionDefinition("sys.ipar.slow_path_reasons");
  Require(HasColumn(*slow, "driver_visible_message", "text"),
          "slow-path message column missing");
  Require(HasColumn(*slow, "required_action", "text"),
          "slow-path required action column missing");
  Require(HasColumn(*slow, "authority_scope", "text"),
          "slow-path authority scope column missing");
  Require(HasColumn(*slow, "finality_authority", "yes_no"),
          "slow-path finality authority column missing");
}

void TestEmptySourcesDoNotFabricateRows() {
  for (const auto& view : {"sys.ipar.agent_lifecycle",
                           "sys.ipar.metric_counters",
                           "sys.ipar.telemetry_controls",
                           "sys.ipar.slow_path_reasons"}) {
    const auto result = Build(view);
    RequireOk(result, std::string(view) + " projection failed");
    Require(result.rows.empty(), std::string(view) + " fabricated rows without sources");
  }
}

void TestPopulatedRows() {
  const auto lifecycle = Build("sys.ipar.agent_lifecycle", LifecycleRows());
  RequireOk(lifecycle, "sys.ipar.agent_lifecycle projection failed");
  Require(lifecycle.rows.size() == 1,
          "lifecycle projection did not filter hidden/stale-epoch rows");
  Require(HasRowValue(lifecycle, "runtime_id", "server_agent_runtime"),
          "lifecycle runtime id missing");
  Require(HasRowValue(lifecycle, "started", "YES"),
          "lifecycle started flag missing");
  Require(HasRowValue(lifecycle, "idle_state", "idle_resident"),
          "lifecycle idle state missing");
  Require(HasRowValue(lifecycle, "worker_thread_count", "5"),
          "lifecycle worker count missing");
  RequireNoPrivateLeak(lifecycle);

  const auto counters = Build("sys.ipar.metric_counters", {}, CounterRows());
  RequireOk(counters, "sys.ipar.metric_counters projection failed");
  Require(HasRowValue(counters, "metric_id", "IPAR-M001"),
          "IPAR counter metric id missing");
  Require(HasRowValue(counters,
                      "metric_path",
                      "sys.metrics.ipar.script.prepared_descriptor_hits"),
          "IPAR counter metric path missing");
  Require(HasRowValue(counters, "value", "1048576"),
          "IPAR counter value missing");
  RequireNoPrivateLeak(counters);

  const auto telemetry = Build("sys.ipar.telemetry_controls", {}, {}, TelemetryRows());
  RequireOk(telemetry, "sys.ipar.telemetry_controls projection failed");
  Require(telemetry.rows.size() == 3,
          "telemetry projection did not expose supplied control rows");
  Require(HasRowValue(telemetry,
                      "metric_path",
                      "sys.metrics.ipar.telemetry.metric_persist_stride"),
          "metric persist stride telemetry path missing");
  Require(HasRowValue(telemetry, "sample_rate_per_mille", "7"),
          "telemetry sample rate missing");
  RequireNoPrivateLeak(telemetry);

  const auto slow_paths =
      Build("sys.ipar.slow_path_reasons", {}, {}, {}, SlowPathRows());
  RequireOk(slow_paths, "sys.ipar.slow_path_reasons projection failed");
  Require(HasRowValue(slow_paths, "statement_id", "SBDFS-059:copy-batch-4"),
          "slow-path statement id missing");
  Require(HasRowValue(slow_paths, "chosen_path", "degraded_path"),
          "slow-path chosen path missing");
  Require(HasRowValue(slow_paths, "fallback_count", "1"),
          "slow-path fallback count missing");
  Require(HasRowValue(slow_paths,
                      "driver_visible_message",
                      "COPY used full validation because descriptor epochs were stale."),
          "slow-path driver-visible message missing");
  Require(HasRowValue(slow_paths,
                      "required_action",
                      "inspect_pre_execution_validation_reason_relation_state_full_load_required"),
          "slow-path required action missing");
  Require(HasRowValue(slow_paths,
                      "authority_scope",
                      "diagnostic_evidence_only_not_finality_visibility_security_recovery_or_parser_authority"),
          "slow-path authority scope did not remain diagnostic-only");
  Require(HasRowValue(slow_paths, "finality_authority", "NO"),
          "slow-path diagnostic became finality authority");
  RequireNoPrivateLeak(slow_paths);
}

void TestMemoryOverheadSamplingBounds() {
  mem::MemorySupportBundleRequest request;
  request.snapshot.current_bytes = 16384;
  request.snapshot.peak_bytes = 32768;
  request.snapshot.allocation_count = 64;
  request.snapshot.deallocation_count = 32;
  request.snapshot.failure_count = 1;
  request.snapshot.contexts.push_back(
      {"query", "query-ipar-overhead-a", 8192, 16384, 8, 4, 1, 2});
  request.snapshot.contexts.push_back(
      {"query", "query-ipar-overhead-b", 4096, 8192, 4, 2, 1, 2});
  request.snapshot.categories.push_back(
      {mem::MemoryCategory::executor_query_reserved, 8192, 16384, 8, 4, 1, 2});
  request.snapshot.categories.push_back(
      {mem::MemoryCategory::page_buffer, 4096, 8192, 4, 2, 1, 2});
  request.diagnostics.push_back(platform::MakeDiagnostic(
      platform::StatusCode::memory_limit_exceeded,
      platform::Severity::error,
      platform::Subsystem::memory,
      "SB_MEMORY.IPAR_OVERHEAD_PROOF",
      "memory.ipar.overhead_proof",
      {{"reason", "sampled_budget_proof"}},
      {},
      "ipar_observability_projection_gate",
      {}));

  mem::MemoryObservabilityOverheadPolicy policy;
  policy.sample_count = 8;
  policy.p50_budget_microseconds = 100000;
  policy.p95_budget_microseconds = 250000;
  policy.p99_budget_microseconds = 500000;
  policy.sampled_max_top_contexts = 1;
  policy.sampled_max_top_categories = 1;

  const auto result = mem::MeasureMemoryObservabilityOverhead(request, policy);
  Require(result.ok(), "IPAR telemetry overhead sampling result failed");
  Require(result.within_budget, "IPAR telemetry overhead exceeded configured budget");
  Require(result.sampling_bounds_enforced,
          "IPAR telemetry sampling bounds were not enforced");
  Require(result.sampled_top_context_count <= 1,
          "IPAR sampled top-context count exceeded policy");
  Require(result.sampled_top_category_count <= 1,
          "IPAR sampled top-category count exceeded policy");
  Require(result.sampled_row_count < result.observed_row_count,
          "IPAR sampled mode did not reduce support-bundle rows");
  Require(result.failure_evidence_preserved_under_sampling,
          "IPAR sampled mode lost failure diagnostic evidence");
}

void TestClusterAndUnsupportedPaths() {
  const auto cluster =
      Build("cluster.sys.ipar.metric_counters", {}, CounterRows(), {}, {});
  Require(!cluster.ok, "cluster IPAR sys view did not fail closed");
  Require(cluster.diagnostic_code ==
              info::kSysInformationDiagnosticClusterScopeForbidden,
          "cluster IPAR sys view diagnostic drifted");

  const auto unsupported = Build("sys.ipar.missing_view");
  Require(!unsupported.ok, "unsupported IPAR sys view succeeded");
  Require(unsupported.diagnostic_code ==
              info::kSysInformationDiagnosticViewUnsupported,
          "unsupported IPAR sys view diagnostic drifted");
}

void TestShowCatalogRequestCarriesIparSources() {
  info::EngineShowCatalogRequest request;
  request.context = EngineContext();
  request.option_envelopes.push_back("projection:sys.ipar.metric_counters");
  request.ipar_metric_counters = CounterRows();

  const auto result = info::EngineShowCatalog(request);
  RequireOk(result, "EngineShowCatalog did not expose supplied IPAR metric sources");
  Require(result.result_shape.rows.size() == 2,
          "EngineShowCatalog lost IPAR metric source rows");
  Require(HasRowValue(result, "metric_id", "IPAR-M001"),
          "EngineShowCatalog IPAR metric id missing");
  Require(HasRowValue(result,
                      "metric_path",
                      "sys.metrics.ipar.script.prepared_descriptor_hits"),
          "EngineShowCatalog IPAR metric path missing");
  Require(HasEvidence(result, "catalog_projection", "sys.ipar.metric_counters"),
          "EngineShowCatalog catalog projection evidence missing");
  Require(HasEvidence(result, "catalog_filtered_rows", "2"),
          "EngineShowCatalog filtered-row evidence missing");
}

void TestPlanOperationRequestCarriesIparSources() {
  info::EnginePlanOperationRequest request;
  request.context = EngineContext();
  request.execute = true;
  request.query_operation = "values";
  request.option_envelopes.push_back("catalog_projection:sys.ipar.telemetry_controls");
  request.ipar_telemetry_controls = TelemetryRows();

  const auto result = info::EnginePlanOperation(request);
  RequireOk(result, "EnginePlanOperation did not expose supplied IPAR telemetry sources");
  Require(result.output_row_count == 3,
          "EnginePlanOperation output row count did not match IPAR telemetry rows");
  Require(HasRowValue(result, "control_name", "metric_persist_stride"),
          "EnginePlanOperation telemetry control name missing");
  Require(HasRowValue(result,
                      "metric_path",
                      "sys.metrics.ipar.telemetry.metric_persist_stride"),
          "EnginePlanOperation telemetry metric path missing");
  Require(HasEvidence(result, "query_execution", "values"),
          "EnginePlanOperation values execution evidence missing");
}

}  // namespace

int main() {
  TestDefinitions();
  TestEmptySourcesDoNotFabricateRows();
  TestPopulatedRows();
  TestMemoryOverheadSamplingBounds();
  TestClusterAndUnsupportedPaths();
  TestShowCatalogRequestCarriesIparSources();
  TestPlanOperationRequestCarriesIparSources();
  return EXIT_SUCCESS;
}
