// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/sys_information_projection.hpp"
#include "sblr_dispatch_server.hpp"
#include "server_observability.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace info = scratchbird::engine::internal_api;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

std::array<std::uint8_t, 16> Uuid(std::uint8_t seed) {
  std::array<std::uint8_t, 16> uuid{};
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<std::uint8_t>(seed + i);
  }
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

info::SysInformationProjectionContext Context() {
  info::SysInformationProjectionContext context;
  context.catalog_display_name = "IPARDB";
  context.session_language = "en";
  context.default_language = "en";
  context.visible_catalog_generation_id = 7;
  return context;
}

std::string Field(const info::SysInformationProjectionRow& row,
                  std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) {
      return value;
    }
  }
  return {};
}

bool HasRowValue(const info::SysInformationProjectionResult& result,
                 std::string_view field_name,
                 std::string_view expected_value) {
  for (const auto& row : result.rows) {
    if (Field(row, field_name) == expected_value) {
      return true;
    }
  }
  return false;
}

const info::SysInformationIparTelemetryControlSource* FindControl(
    const std::vector<info::SysInformationIparTelemetryControlSource>& controls,
    std::string_view control_name) {
  for (const auto& control : controls) {
    if (control.control_name == control_name) {
      return &control;
    }
  }
  return nullptr;
}

info::SysInformationProjectionResult BuildSlowPathProjection(
    std::vector<info::SysInformationIparSlowPathReasonSource> rows) {
  return info::BuildSysInformationProjection("sys.ipar.slow_path_reasons",
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
                                             {},
                                             {},
                                             {},
                                             std::move(rows));
}

void TestSlowPathRowsAreMetricBacked() {
  server::ServerObservabilityState state;
  state.metrics_enabled = true;

  server::SetServerMetric(
      &state,
      "sys.metrics.ipar.insert.slow_path_total",
      3,
      "counter",
      {{"metric_id", "IPAR-M035"},
       {"statement_id", "SBDFS-059:copy-batch-4"},
       {"result", "scan_fallback"},
       {"reason", "unique_physical_probe_cache_miss"},
       {"validation_stage", "unique_preflight"},
       {"diagnostic_code", "SB_IPAR_SLOW_PATH_REASON_RECORDED"},
       {"sample_count", "3"}});
  server::SetServerMetric(&state,
                          "sys.metrics.ipar.telemetry.metric_persist_stride",
                          128,
                          "gauge",
                          {{"budget_id", "IPAR-M031"}});

  const auto sources = server::BuildIparSlowPathReasonProjectionSources(state);
  Require(sources.size() == 1,
          "slow-path source adapter did not filter to metric-backed slow paths");
  Require(sources.front().statement_id == "SBDFS-059:copy-batch-4",
          "slow-path statement id not derived from metric labels");
  Require(sources.front().chosen_path == "scan_fallback",
          "slow-path chosen path not derived from metric result label");
  Require(sources.front().reason_code == "unique_physical_probe_cache_miss",
          "slow-path reason not derived from metric labels");
  Require(sources.front().fallback_count == 3,
          "slow-path fallback count not derived from metric value");
  Require(sources.front().sample_count == 3,
          "slow-path sample count not derived from metric labels");

  const auto projection = BuildSlowPathProjection(sources);
  Require(projection.ok, "metric-backed slow-path projection failed");
  Require(projection.rows.size() == 1,
          "metric-backed slow-path projection row count drifted");
  Require(HasRowValue(projection, "statement_id", "SBDFS-059:copy-batch-4"),
          "metric-backed slow-path projection missing statement id");
  Require(HasRowValue(projection, "driver_visible_message",
                      "Slow path selected during unique_preflight: unique_physical_probe_cache_miss"),
          "metric-backed slow-path projection message drifted");
  Require(HasRowValue(projection, "required_action",
                      "inspect_unique_preflight_reason_unique_physical_probe_cache_miss"),
          "metric-backed slow-path projection missing required action");
  Require(HasRowValue(projection, "authority_scope",
                      "diagnostic_evidence_only_not_finality_visibility_security_recovery_or_parser_authority"),
          "metric-backed slow-path projection became authoritative");
  Require(HasRowValue(projection, "finality_authority", "NO"),
          "metric-backed slow-path projection claimed finality authority");
}

void TestEmptyMetricsDoNotFabricateRows() {
  server::ServerObservabilityState state;
  state.metrics_enabled = true;
  const auto sources = server::BuildIparSlowPathReasonProjectionSources(state);
  Require(sources.empty(), "empty server metrics fabricated slow-path rows");

  const auto projection = BuildSlowPathProjection(sources);
  Require(projection.ok, "empty slow-path projection failed");
  Require(projection.rows.empty(), "empty slow-path projection fabricated rows");
}

void TestTelemetryControlsExposeBoundedBudget() {
  server::ServerObservabilityState state;
  state.metrics_enabled = true;
  state.metric_persist_stride = 128;
  state.audit_persist_stride = 64;
  state.ipar_hot_path_budget_percent_x100 = 500;
  state.ipar_detail_trace_enabled = false;
  state.ipar_detail_trace_sample_stride = 512;
  state.ipar_slow_path_reason_series_limit = 2;

  const auto controls = server::BuildIparTelemetryControlProjectionSources(state);
  const auto* metric_stride = FindControl(controls, "metric_persist_stride");
  const auto* audit_stride = FindControl(controls, "audit_persist_stride");
  const auto* hot_budget = FindControl(controls, "hot_path_budget_percent_x100");
  const auto* detail_enabled = FindControl(controls, "detail_trace_enabled");
  const auto* detail_stride = FindControl(controls, "detail_trace_sample_stride");
  const auto* series_limit = FindControl(controls, "diagnostic_reason_series_limit");

  Require(metric_stride != nullptr && metric_stride->sample_rate_per_mille == 7,
          "metric persist stride control missing bounded sample rate");
  Require(audit_stride != nullptr && audit_stride->sample_rate_per_mille == 15,
          "audit persist stride control missing bounded sample rate");
  Require(hot_budget != nullptr && hot_budget->observed_value == 500 &&
              hot_budget->overhead_budget_percent == "5.0",
          "hot-path overhead budget control missing");
  Require(detail_enabled != nullptr && detail_enabled->observed_value == 0 &&
              detail_enabled->sample_rate_per_mille == 0,
          "detail trace must be disabled by default on hot path");
  Require(detail_stride != nullptr && detail_stride->persist_stride == 512,
          "detail trace sample stride control missing");
  Require(series_limit != nullptr && series_limit->configured_value == 2,
          "diagnostic reason series limit control missing");
}

void TestSlowPathSeriesLimitBoundsHotPathCardinality() {
  server::ServerObservabilityState state;
  state.metrics_enabled = true;
  state.ipar_slow_path_reason_series_limit = 2;

  for (int i = 0; i < 3; ++i) {
    server::SetServerMetric(
        &state,
        "sys.metrics.ipar.insert.slow_path_total",
        1,
        "counter",
        {{"metric_id", "IPAR-M035"},
         {"statement_id", "SBDFS-059:copy-batch-" + std::to_string(i)},
         {"result", "scan_fallback"},
         {"reason", "bounded_diagnostic_series"},
         {"validation_stage", "unique_preflight"},
         {"diagnostic_code", "SB_IPAR_SLOW_PATH_REASON_RECORDED"},
         {"sample_count", "1"}});
  }

  const auto sources = server::BuildIparSlowPathReasonProjectionSources(state);
  Require(sources.size() == 2,
          "slow-path diagnostic source adapter ignored configured series limit");
  Require(state.ipar_slow_path_reason_series_count == 2,
          "slow-path diagnostic retained series count drifted");
  Require(state.ipar_slow_path_reason_series_dropped == 1,
          "slow-path diagnostic dropped count drifted");

  const auto controls = server::BuildIparTelemetryControlProjectionSources(state);
  const auto* dropped = FindControl(controls, "diagnostic_reason_series_dropped");
  const auto* dropped_total = FindControl(controls, "dropped_metric_count");
  Require(dropped != nullptr && dropped->observed_value == 1,
          "slow-path dropped diagnostic control missing");
  Require(dropped_total != nullptr && dropped_total->dropped_metric_count == 1,
          "aggregate dropped metric count did not include bounded slow-path drop");
}

server::HostedEngineState MakeEngineState() {
  server::HostedEngineState state;
  state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/ipar_server_observability_projection_gate.sbdb";
  database.database_uuid = "database-ipar-server-observability";
  database.read_only = false;
  database.write_admission_fenced = false;
  state.databases.push_back(std::move(database));
  return state;
}

server::ServerSessionRecord MakeSession() {
  server::ServerSessionRecord session;
  session.connection_uuid = Uuid(0x10);
  session.session_uuid = Uuid(0x20);
  session.auth_context_uuid = Uuid(0x30);
  session.principal_uuid = Uuid(0x40);
  session.effective_user_uuid = session.principal_uuid;
  session.principal_claim = "ipar-server-observability-user";
  session.database_path = "/tmp/ipar_server_observability_projection_gate.sbdb";
  session.database_uuid = "database-ipar-server-observability";
  session.local_transaction_id = 7;
  session.catalog_generation = 1;
  session.security_epoch = 1;
  session.descriptor_epoch = 1;
  session.grant_epoch = 1;
  session.policy_generation = 1;
  session.role_set_hash = "roles/ipar-server-observability";
  session.group_set_hash = "groups/ipar-server-observability";
  session.search_path_hash = "search/ipar-server-observability";
  return session;
}

server::ServerIparProjectionSources BuildTestIparSources(void* context) {
  auto* state = static_cast<server::ServerObservabilityState*>(context);
  server::ServerIparProjectionSources sources;
  if (state == nullptr) {
    return sources;
  }
  sources.metric_counters = server::BuildIparMetricCounterProjectionSources(*state);
  sources.telemetry_controls = server::BuildIparTelemetryControlProjectionSources(*state);
  sources.slow_path_reasons = server::BuildIparSlowPathReasonProjectionSources(*state);
  return sources;
}

std::string SelectTelemetryControlsEnvelope() {
  return "operation_id=dml.select_rows\n"
         "opcode=SBLR_DML_SELECT_ROWS\n"
         "sblr_operation_family=sblr.query.relational.v3\n"
         "result_shape=engine.api.result.v1\n"
         "diagnostic_shape=engine.diagnostic.v1\n"
         "trace_key=ipar.server.live_projection.select\n"
         "contains_sql_text=false\n"
         "parser_resolved_names_to_uuids=true\n"
         "requires_security_context=true\n"
         "requires_transaction_context=true\n"
         "requires_cluster_authority=false\n"
         "target_name=sys.ipar.telemetry_controls\n";
}

void TestSelectRowsUsesLiveIparProjectionSources() {
  server::ServerObservabilityState observability;
  observability.metrics_enabled = true;
  observability.metric_persist_stride = 16;
  observability.audit_persist_stride = 16;

  server::ServerSessionRegistry registry;
  const auto session = MakeSession();
  registry.sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] = session;

  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session.session_uuid;
  frame.payload = server::EncodeExecuteSblrPayloadForTest(
      session.session_uuid, {}, SelectTelemetryControlsEnvelope());

  server::ServerIparProjectionSourceFactory factory;
  factory.context = &observability;
  factory.build = &BuildTestIparSources;

  const auto result = server::HandleExecuteSblr(
      &registry, MakeEngineState(), frame, &factory);
  Require(result.accepted,
          "dml.select_rows sys.ipar.telemetry_controls route was rejected");
  const std::string payload(result.payload.begin(), result.payload.end());
  Require(payload.find("observability.show_catalog") != std::string::npos,
          "sys.ipar select did not dispatch through catalog observability");
  Require(payload.find("sys.metrics.ipar.telemetry.metrics_enabled") != std::string::npos,
          "sys.ipar select did not include live telemetry metric path");
  Require(payload.find("metric_persist_stride") != std::string::npos,
          "sys.ipar select did not include live telemetry control row");
}

}  // namespace

int main() {
  TestSlowPathRowsAreMetricBacked();
  TestEmptyMetricsDoNotFabricateRows();
  TestTelemetryControlsExposeBoundedBudget();
  TestSlowPathSeriesLimitBoundsHotPathCardinality();
  TestSelectRowsUsesLiveIparProjectionSources();
  return EXIT_SUCCESS;
}
