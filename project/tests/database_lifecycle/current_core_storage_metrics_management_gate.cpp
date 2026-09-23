// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "storage_metrics_management.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace metrics = scratchbird::core::metrics;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::string& value, std::string_view needle) {
  return value.find(needle) != std::string::npos;
}

bool SawMetric(const metrics::StorageMetricsManagementResult& result,
               std::string_view family) {
  for (const auto& metric : result.visible_metrics) {
    if (metric.family == family) {
      return true;
    }
  }
  return false;
}

metrics::StorageMetricsManagementRequest AuthorizedRequest() {
  metrics::StorageMetricsManagementRequest request;
  request.metrics_read_authorized = true;
  request.support_bundle_requested = true;
  request.allow_sensitive_labels = false;
  request.observed_metric_generation = 7;
  request.current_metric_generation = 7;
  request.database_uuid = scratchbird::tests::FixtureUuid(1037, 1);
  request.filespace_uuid = scratchbird::tests::FixtureUuid(1037, 2);
  request.node_uuid = scratchbird::tests::FixtureUuid(1037, 3);
  request.local_path_sample = "/tmp/raw-storage-path";
  request.protected_payload_sample = "RAW_STORAGE_PROTECTED_PAYLOAD";
  return request;
}

void TestStorageMetricDescriptorsAndEmission() {
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_storage_pressure_total") != nullptr,
          "MDF-018 storage pressure descriptor missing");
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_temp_workspace_bytes") != nullptr,
          "MDF-018 temp workspace descriptor missing");
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_index_build_workspace_bytes") != nullptr,
          "MDF-018 index build workspace descriptor missing");
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_storage_support_redaction_total") != nullptr,
          "MDF-018 storage redaction descriptor missing");

  const auto result =
      metrics::PublishStorageMetricsManagementSurface(AuthorizedRequest());
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic << '\n';
    }
  }
  Require(result.ok, "MDF-018 storage metrics management surface failed");
  Require(SawMetric(result, "sb_filespace_total_bytes"),
          "MDF-018 filespace size metric missing");
  Require(SawMetric(result, "sb_filespace_reserved_bytes"),
          "MDF-018 filespace quota/safety metric missing");
  Require(SawMetric(result, "sb_filespace_device_read_latency_microseconds"),
          "MDF-018 device IO metric missing");
  Require(SawMetric(result, "sb_page_free_count"),
          "MDF-018 page allocation metric missing");
  Require(SawMetric(result, "sb_page_cache_resident_pages"),
          "MDF-018 page cache metric missing");
  Require(SawMetric(result, "sb_page_fragmentation_ratio"),
          "MDF-018 fragmentation metric missing");
  Require(SawMetric(result, "sb_archive_lag_bytes"),
          "MDF-018 archive metric missing");
  Require(SawMetric(result, "sb_backup_progress_percent"),
          "MDF-018 backup metric missing");
  Require(SawMetric(result, "sb_restore_drill_duration_microseconds"),
          "MDF-018 restore metric missing");
  Require(SawMetric(result, "sb_storage_pressure_total"),
          "MDF-018 storage pressure metric missing");
  Require(SawMetric(result, "sb_temp_workspace_bytes"),
          "MDF-018 temp workspace metric missing");
  Require(SawMetric(result, "sb_index_build_workspace_bytes"),
          "MDF-018 index build workspace metric missing");
}

void TestRedactionAuthorizationAndStaleInvalidation() {
  const auto result =
      metrics::PublishStorageMetricsManagementSurface(AuthorizedRequest());
  Require(result.redaction_applied, "MDF-018 redaction flag missing");
  Require(result.ok && !result.support_bundle_records.empty(),
          "MDF-018 binary support records missing");
  for (const auto& record : result.support_bundle_records) {
    Require(record.identities_redacted && record.database_uuid.is_nil() && record.filespace_uuid.is_nil(),
            "MDF-018 storage identities were not redacted");
    Require(record.local_path_redacted && record.protected_payload_redacted,
            "MDF-018 sensitive payload metadata missing");
    const auto& bytes = record.metric.encoded_value;
    Require(bytes.size() >= metrics::kMetricValueHeaderBytes,
            "MDF-018 binary metric frame missing");
    const std::string payload(bytes.begin(), bytes.end());
    Require(!Contains(payload, "RAW_STORAGE_PROTECTED_PAYLOAD") &&
            !Contains(payload, "/tmp/raw-storage-path"),
            "MDF-018 binary support records leaked sensitive payload");
  }
  auto permitted = AuthorizedRequest();
  permitted.allow_sensitive_labels = true;
  const auto native = metrics::PublishStorageMetricsManagementSurface(permitted);
  Require(native.ok && !native.support_bundle_records.empty(),
          "MDF-018 native support records missing");
  for (const auto& record : native.support_bundle_records) {
    Require(!record.identities_redacted && record.database_uuid == permitted.database_uuid &&
            record.filespace_uuid == permitted.filespace_uuid,
            "MDF-018 storage identities were not preserved as binary16");
  }
  auto missing_identity = AuthorizedRequest(); missing_identity.database_uuid = {};
  const auto invalid = metrics::PublishStorageMetricsManagementSurface(missing_identity);
  Require(!invalid.ok && invalid.visible_metrics.empty() && invalid.support_bundle_records.empty(),
          "MDF-018 missing identity was replaced by a fabricated UUID");

  auto unauthorized = AuthorizedRequest();
  unauthorized.metrics_read_authorized = false;
  const auto refused =
      metrics::PublishStorageMetricsManagementSurface(unauthorized);
  Require(!refused.ok, "MDF-018 unauthorized metrics read was accepted");
  Require(!refused.diagnostics.empty() &&
              refused.diagnostics.front() ==
                  "SB-STORAGE-METRICS-VISIBILITY-REFUSED",
          "MDF-018 unauthorized diagnostic mismatch");

  auto stale = AuthorizedRequest();
  stale.observed_metric_generation = 6;
  const auto stale_result =
      metrics::PublishStorageMetricsManagementSurface(stale);
  Require(!stale_result.ok, "MDF-018 stale metrics generation was accepted");
  Require(stale_result.stale_invalidated,
          "MDF-018 stale invalidation flag missing");
  Require(!stale_result.diagnostics.empty() &&
              stale_result.diagnostics.front() ==
                  "SB-STORAGE-METRICS-STALE-GENERATION-REFUSED",
          "MDF-018 stale diagnostic mismatch");
}

}  // namespace

int main() {
  // MDF-018-CURRENT-CORE-STORAGE-METRICS-MANAGEMENT
  // DEFER-SPM-*
  TestStorageMetricDescriptorsAndEmission();
  TestRedactionAuthorizationAndStaleInvalidation();
  std::cout << "current_core_storage_metrics_management_gate=passed\n";
  return EXIT_SUCCESS;
}
