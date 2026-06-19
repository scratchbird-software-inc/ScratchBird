// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/sys_information_projection.hpp"
#include "server_observability.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace info = scratchbird::engine::internal_api;
namespace server = scratchbird::server;

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

std::string Field(const info::SysInformationProjectionRow& row,
                  std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) { return value; }
  }
  return {};
}

const info::SysInformationProjectionRow* FindRow(
    const info::SysInformationProjectionResult& result,
    std::string_view category,
    std::string_view subject) {
  for (const auto& row : result.rows) {
    if (Field(row, "category") == category && Field(row, "subject") == subject) {
      return &row;
    }
  }
  return nullptr;
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

void RequireOk(const info::SysInformationProjectionResult& result,
               const std::string& message) {
  if (!result.ok) {
    std::cerr << result.diagnostic_code << " " << result.diagnostic_detail << '\n';
  }
  Require(result.ok, message);
}

info::SysInformationProjectionResult BuildContentionQuotaProjection(
    const std::vector<info::SysInformationIparContentionQuotaSource>& rows) {
  return info::BuildSysInformationProjection(
      "sys.ipar.contention_quota",
      Context(),
      std::vector<info::SysInformationCatalogObjectSource>{},
      std::vector<info::SysInformationResolverNameSource>{},
      std::vector<info::SysInformationCommentSource>{},
      std::vector<info::SysInformationDatatypeDescriptorSource>{},
      std::vector<info::SysInformationColumnSource>{},
      std::vector<info::SysInformationSettingSource>{},
      std::vector<info::SysInformationFrontendAgentSource>{},
      std::vector<info::SysInformationProtectedMaterialSource>{},
      std::vector<info::SysInformationProtectedMaterialVersionSource>{},
      std::vector<info::SysInformationAgentSource>{},
      std::vector<info::SysInformationAgentMetricDependencySource>{},
      std::vector<info::SysInformationAgentPolicySource>{},
      std::vector<info::SysInformationAgentActionSource>{},
      std::vector<info::SysInformationAgentOverrideSource>{},
      std::vector<info::SysInformationAgentEvidenceSource>{},
      std::vector<info::SysInformationAgentAuditSource>{},
      std::vector<info::SysInformationFilespaceCapacityAgentStateSource>{},
      std::vector<info::SysInformationPageAllocationAgentStateSource>{},
      std::vector<info::SysInformationFilespaceShrinkReadinessSource>{},
      std::vector<info::SysInformationDomainSource>{},
      std::vector<info::SysInformationIparAgentLifecycleSource>{},
      std::vector<info::SysInformationIparMetricCounterSource>{},
      std::vector<info::SysInformationIparTelemetryControlSource>{},
      std::vector<info::SysInformationIparSlowPathReasonSource>{},
      rows);
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

void TestDefinitionAndEmptyProjection() {
  const auto validation = info::ValidateBuiltinSysInformationProjectionDefinitions();
  if (!validation.ok) {
    for (const auto& code : validation.diagnostic_codes) {
      std::cerr << code << '\n';
    }
  }
  Require(validation.ok, "builtin sys projection definitions failed validation");

  const auto* definition =
      info::FindSysInformationProjectionDefinition("sys.ipar.contention_quota");
  Require(definition != nullptr, "sys.ipar.contention_quota definition missing");
  Require(definition->view_scope == "local", "IPAR contention/quota view must be local");
  Require(definition->authorization_filter_required,
          "IPAR contention/quota view must require authorization filtering");
  Require(definition->redaction_required,
          "IPAR contention/quota view must require redaction");
  Require(definition->mga_snapshot_visibility_required,
          "IPAR contention/quota view must stay snapshot-visible metadata");
  Require(definition->cluster_path_fail_closed,
          "IPAR contention/quota view must fail closed on cluster paths");
  Require(HasColumn(*definition, "source_kind", "text"),
          "source_kind provenance column missing");
  Require(HasColumn(*definition, "source_ref", "text"),
          "source_ref provenance column missing");
  Require(HasColumn(*definition, "provenance", "text"),
          "provenance column missing");
  Require(HasColumn(*definition, "queue_depth", "uint64"),
          "queue_depth column missing");

  const auto empty = BuildContentionQuotaProjection({});
  RequireOk(empty, "empty contention/quota projection failed");
  Require(empty.rows.empty(), "empty contention/quota projection fabricated rows");
}

void TestServerMetricHookBacksRows() {
  server::ServerObservabilityState state;
  state.metrics_enabled = true;

  server::SetServerMetric(&state,
                          "sys.metrics.ipar.telemetry.metric_persist_stride",
                          128,
                          "gauge",
                          {{"budget_id", "IPAR-M031"}});
  server::RecordIparContentionQuotaObservation(
      &state,
      {.category = "cache",
       .subject = "descriptor_cache",
       .metric_unit = "entries",
       .source_ref = "catalog.descriptor_cache",
       .diagnostic_code = "SB_IPAR_DESCRIPTOR_CACHE_OBSERVED",
       .observed_value = 31,
       .limit_value = 64,
       .sample_count = 2});
  server::RecordIparContentionQuotaObservation(
      &state,
      {.category = "cache",
       .subject = "prepared_cache",
       .metric_unit = "entries",
       .source_ref = "server.prepared_handle_cache",
       .diagnostic_code = "SB_IPAR_PREPARED_CACHE_OBSERVED",
       .observed_value = 17,
       .limit_value = 32,
       .sample_count = 2});
  server::RecordIparContentionQuotaObservation(
      &state,
      {.category = "contention",
       .subject = "page_allocation",
       .metric_unit = "waits",
       .source_kind = "page_allocation_metric",
       .source_ref = "page_allocation.agent_queue",
       .diagnostic_code = "SB_IPAR_PAGE_ALLOCATION_CONTENTION",
       .observed_value = 6,
       .wait_count = 6,
       .queue_depth = 2,
       .sample_count = 3});
  server::RecordIparContentionQuotaObservation(
      &state,
      {.category = "quota",
       .subject = "backpressure_agent_queue",
       .metric_unit = "requests",
       .source_kind = "agent_queue_metric",
       .source_ref = "agent_runtime.backpressure_queue",
       .diagnostic_code = "SB_IPAR_QUOTA_BACKPRESSURE",
       .observed_value = 3,
       .limit_value = 8,
       .refusal_count = 1,
       .queue_depth = 3,
       .sample_count = 4});

  const auto sources = server::BuildIparContentionQuotaProjectionSources(state);
  Require(sources.size() == 4,
          "contention/quota adapter included unrelated rows or dropped source rows");

  const auto projection = BuildContentionQuotaProjection(sources);
  RequireOk(projection, "contention/quota projection failed");
  Require(projection.rows.size() == 4,
          "contention/quota projection row count drifted");
  RequireNoPrivateLeak(projection);

  const auto* descriptor_cache = FindRow(projection, "cache", "descriptor_cache");
  Require(descriptor_cache != nullptr, "descriptor cache row missing");
  Require(Field(*descriptor_cache, "source_kind") == "server_observability_hook",
          "descriptor cache row missing source kind");
  Require(Field(*descriptor_cache, "source_ref") == "catalog.descriptor_cache",
          "descriptor cache row missing source ref");
  Require(Field(*descriptor_cache, "provenance") ==
              "server_observability.record_ipar_contention_quota",
          "descriptor cache row missing provenance");
  Require(Field(*descriptor_cache, "observed_value") == "31",
          "descriptor cache observed value missing");

  const auto* prepared_cache = FindRow(projection, "cache", "prepared_cache");
  Require(prepared_cache != nullptr, "prepared cache row missing");
  Require(Field(*prepared_cache, "diagnostic_code") ==
              "SB_IPAR_PREPARED_CACHE_OBSERVED",
          "prepared cache diagnostic code missing");
  Require(Field(*prepared_cache, "limit_value") == "32",
          "prepared cache limit value missing");

  const auto* page_contention = FindRow(projection, "contention", "page_allocation");
  Require(page_contention != nullptr, "page allocation contention row missing");
  Require(Field(*page_contention, "source_kind") == "page_allocation_metric",
          "page allocation contention source kind missing");
  Require(Field(*page_contention, "wait_count") == "6",
          "page allocation contention wait count missing");
  Require(Field(*page_contention, "queue_depth") == "2",
          "page allocation contention queue depth missing");

  const auto* quota = FindRow(projection, "quota", "backpressure_agent_queue");
  Require(quota != nullptr, "quota/backpressure/agent queue row missing");
  Require(Field(*quota, "source_kind") == "agent_queue_metric",
          "quota row source kind missing");
  Require(Field(*quota, "refusal_count") == "1",
          "quota row refusal count missing");
  Require(Field(*quota, "queue_depth") == "3",
          "quota row agent queue depth missing");
  Require(Field(*quota, "source_state") == "observed",
          "quota row source state missing");
}

void TestClusterPathFailsClosed() {
  const auto cluster = info::BuildSysInformationProjection(
      "cluster.sys.ipar.contention_quota",
      Context(),
      {},
      {});
  Require(!cluster.ok, "cluster-scoped contention/quota projection succeeded");
  Require(cluster.diagnostic_code ==
              info::kSysInformationDiagnosticClusterScopeForbidden,
          "cluster-scoped contention/quota diagnostic drifted");
}

}  // namespace

int main() {
  TestDefinitionAndEmptyProjection();
  TestServerMetricHookBacksRows();
  TestClusterPathFailsClosed();
  return EXIT_SUCCESS;
}
