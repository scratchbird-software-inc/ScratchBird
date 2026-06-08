// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-FAMILY-MANAGEMENT-SURFACE-CLOSURE-ANCHOR

#include "index_family_registry.hpp"
#include "metric_registry.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::index {

struct IndexFamilySurfaceLastError {
  IndexFamily family = IndexFamily::unknown;
  DiagnosticRecord diagnostic;
};

struct IndexFamilyManagementSurfaceRequest {
  bool redact_diagnostic_details = true;
  bool include_support_bundle_rows = true;
  std::vector<IndexFamilySurfaceLastError> last_errors;
};

struct IndexFamilyManagementSurfaceRow {
  std::string family_id;
  std::string family_name;
  std::string persistence;
  std::string key_model;
  std::string native_physical_family;
  std::string semantic_profile;
  bool declared_capability = false;
  bool planner_contract = false;
  bool implemented = false;
  bool physical_reader = false;
  bool physical_writer = false;
  bool maintenance = false;
  bool validate = false;
  bool repair = false;
  bool recovery_reopen = false;
  bool rebuild = false;
  bool runtime_available = false;
  bool benchmark_clean = false;
  std::string blocker;
  std::string blocker_diagnostic_code;
  std::string blocker_message_key;
  std::string blocker_detail;
  std::string validation_state;
  std::string repair_state;
  bool last_error_present = false;
  std::string last_diagnostic_code;
  std::string last_message_key;
  std::string last_error_detail;
  bool diagnostic_detail_redacted = false;
  bool observational_only = true;
  bool catalog_authority = false;
  bool execution_authority = false;
  bool visibility_authority = false;
  bool security_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool provider_authority = false;
};

struct IndexFamilySupportBundleField {
  std::string key;
  std::string value;
};

struct IndexFamilySupportBundleRow {
  std::string family_id;
  std::vector<IndexFamilySupportBundleField> fields;
};

struct IndexFamilyMetricLabel {
  std::string key;
  std::string value;
};

struct IndexFamilyMetricRecord {
  std::string family;
  scratchbird::core::metrics::MetricType type =
      scratchbird::core::metrics::MetricType::gauge;
  scratchbird::core::metrics::MetricUnit unit =
      scratchbird::core::metrics::MetricUnit::count;
  double value = 0.0;
  std::vector<IndexFamilyMetricLabel> labels;
};

struct IndexFamilyManagementSurface {
  std::string surface_name = "sys.index_family_runtime_state";
  std::string surface_version = "irc170.v1";
  bool redaction_applied = true;
  bool observational_only = true;
  std::vector<IndexFamilyManagementSurfaceRow> rows;
  std::vector<IndexFamilySupportBundleRow> support_bundle_rows;
};

IndexFamilyManagementSurface BuildIndexFamilyManagementSurface(
    const IndexFamilyManagementSurfaceRequest& request = {});

std::vector<IndexFamilyMetricRecord> ExportIndexFamilyManagementMetricRecords(
    const std::vector<IndexFamilyManagementSurfaceRow>& rows);

std::vector<IndexFamilyMetricRecord> ExportIndexFamilyManagementMetricRecords(
    const IndexFamilyManagementSurface& surface);

}  // namespace scratchbird::core::index
