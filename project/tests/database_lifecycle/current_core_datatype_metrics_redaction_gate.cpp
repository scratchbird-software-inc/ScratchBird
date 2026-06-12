// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_metrics_redaction.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace dt = scratchbird::core::datatypes;
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

bool SawMetric(const dt::DatatypeMetricsManagementResult& result,
               std::string_view family) {
  for (const auto& metric : result.visible_metrics) {
    if (metric.family == family) {
      return true;
    }
  }
  return false;
}

std::string BundleText(const dt::DatatypeMetricsManagementResult& result) {
  std::string out;
  for (const auto& line : result.support_bundle_lines) {
    out += line;
    out += '\n';
  }
  return out;
}

dt::DatatypeMetricsManagementRequest AuthorizedRequest() {
  dt::DatatypeMetricsManagementRequest request;
  request.metrics_read_authorized = true;
  request.support_bundle_requested = true;
  request.allow_sensitive_labels = false;
  request.principal_uuid = "principal-secret-0001";
  request.canonical_type = dt::CanonicalTypeId::decimal;
  request.source_type = dt::CanonicalTypeId::character;
  request.target_type = dt::CanonicalTypeId::decimal;
  request.operation = "cast";
  request.result = "ok";
  request.reason = "none";
  request.protected_payload_sample = "RAW_PROTECTED_DATATYPE_PAYLOAD";
  return request;
}

void TestDatatypeMetricDescriptorsAndEmission() {
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_datatype_physical_encoding_total") != nullptr,
          "MDF-016 physical encoding metric descriptor missing");
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_datatype_chunk_event_total") != nullptr,
          "MDF-016 chunk metric descriptor missing");
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_datatype_comparison_total") != nullptr,
          "MDF-016 comparison metric descriptor missing");
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_datatype_locator_event_total") != nullptr,
          "MDF-016 locator metric descriptor missing");
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_datatype_redaction_total") != nullptr,
          "MDF-016 redaction metric descriptor missing");

  const auto result =
      dt::PublishDatatypeMetricsManagementSurface(AuthorizedRequest());
  Require(result.ok, "MDF-016 datatype metrics management surface failed");
  Require(SawMetric(result, "sb_datatype_operation_total"),
          "MDF-016 operation metric missing");
  Require(SawMetric(result, "sb_datatype_cast_total"),
          "MDF-016 cast metric missing");
  Require(SawMetric(result, "sb_datatype_physical_encoding_total"),
          "MDF-016 physical encoding metric missing");
  Require(SawMetric(result, "sb_datatype_chunk_event_total"),
          "MDF-016 chunk metric missing");
  Require(SawMetric(result, "sb_datatype_comparison_total"),
          "MDF-016 comparison metric missing");
  Require(SawMetric(result, "sb_datatype_locator_event_total"),
          "MDF-016 locator metric missing");
  Require(SawMetric(result, "sb_datatype_redaction_total"),
          "MDF-016 redaction metric missing");
}

void TestSupportBundleRedactionAndVisibilityRefusal() {
  const auto result =
      dt::PublishDatatypeMetricsManagementSurface(AuthorizedRequest());
  Require(result.redaction_applied, "MDF-016 support bundle redaction flag missing");
  const std::string bundle = BundleText(result);
  Require(Contains(bundle, "namespace=sys.metrics.datatypes"),
          "MDF-016 support bundle namespace missing");
  Require(Contains(bundle, "principal_uuid=[redacted]"),
          "MDF-016 principal UUID was not redacted");
  Require(Contains(bundle, "protected_payload=[redacted]"),
          "MDF-016 protected payload marker missing");
  Require(!Contains(bundle, "RAW_PROTECTED_DATATYPE_PAYLOAD"),
          "MDF-016 support bundle leaked raw protected payload");
  Require(!Contains(bundle, "principal-secret-0001"),
          "MDF-016 support bundle leaked principal UUID");

  auto unauthorized = AuthorizedRequest();
  unauthorized.metrics_read_authorized = false;
  const auto refused =
      dt::PublishDatatypeMetricsManagementSurface(unauthorized);
  Require(!refused.ok, "MDF-016 unauthorized metrics read was accepted");
  Require(!refused.diagnostics.empty() &&
              refused.diagnostics.front() ==
                  "SB-DATATYPE-METRICS-VISIBILITY-REFUSED",
          "MDF-016 unauthorized visibility diagnostic mismatch");
}

}  // namespace

int main() {
  // MDF-016-CURRENT-CORE-DATATYPE-METRICS-REDACTION
  // DEFER-DTM-*
  TestDatatypeMetricDescriptorsAndEmission();
  TestSupportBundleRedactionAndVisibilityRefusal();
  std::cout << "current_core_datatype_metrics_redaction_gate=passed\n";
  return EXIT_SUCCESS;
}
