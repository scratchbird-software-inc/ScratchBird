// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "datatype_descriptor.hpp"
#include "metric_support_projection.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

inline constexpr const char* kDatatypeMetricsRedactionKey =
    "MDF-016-CURRENT-CORE-DATATYPE-METRICS-REDACTION";

struct DatatypeMetricsManagementRequest {
  bool metrics_read_authorized = false;
  bool allow_sensitive_labels = false;
  bool support_bundle_requested = false;
  metrics::MetricUuid principal_uuid;
  CanonicalTypeId canonical_type = CanonicalTypeId::unknown;
  CanonicalTypeId source_type = CanonicalTypeId::unknown;
  CanonicalTypeId target_type = CanonicalTypeId::unknown;
  std::string operation;
  std::string result;
  std::string reason;
  std::string protected_payload_sample;
};

struct DatatypeMetricSupportRecord {
  metrics::MetricSupportProjection metric;
  metrics::MetricUuid principal_uuid;
  bool principal_redacted = false;
  bool protected_payload_redacted = false;
};

struct DatatypeMetricsManagementResult {
  bool ok = false;
  std::vector<std::string> diagnostics;
  std::vector<scratchbird::core::metrics::MetricValue> visible_metrics;
  std::vector<DatatypeMetricSupportRecord> support_bundle_records;
  bool redaction_applied = false;
};

DatatypeMetricsManagementResult PublishDatatypeMetricsManagementSurface(
    const DatatypeMetricsManagementRequest& request);

}  // namespace scratchbird::core::datatypes
