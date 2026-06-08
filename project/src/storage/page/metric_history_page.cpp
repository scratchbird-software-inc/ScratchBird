// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_history_page.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

}  // namespace

const char* MetricHistoryRecordKindName(MetricHistoryRecordKind kind) {
  switch (kind) {
    case MetricHistoryRecordKind::series: return "series";
    case MetricHistoryRecordKind::raw_sample: return "raw_sample";
    case MetricHistoryRecordKind::rollup: return "rollup";
    case MetricHistoryRecordKind::retention_policy: return "retention_policy";
    case MetricHistoryRecordKind::retention_evidence: return "retention_evidence";
    case MetricHistoryRecordKind::unknown: return "unknown";
  }
  return "unknown";
}

MetricHistoryPageCapacity ComputeMetricHistoryPageCapacity(u32 page_size) {
  MetricHistoryPageCapacity capacity;
  capacity.page_size = page_size;
  if (page_size <= kPageHeaderSerializedBytes + 64) {
    return capacity;
  }
  capacity.body_bytes = page_size - kPageHeaderSerializedBytes;
  capacity.usable_payload_bytes = capacity.body_bytes - 64;
  capacity.minimum_record_slots = capacity.usable_payload_bytes / 64;
  return capacity;
}

MetricHistoryPageValidationResult ValidateMetricHistoryPageBodyHeader(const MetricHistoryPageBodyHeader& header,
                                                                      u32 page_size) {
  MetricHistoryPageValidationResult result;
  result.status = OkStatus();
  result.header = header;
  result.capacity = ComputeMetricHistoryPageCapacity(page_size);
  if (result.capacity.usable_payload_bytes == 0) {
    result.status = ErrorStatus();
    result.diagnostic = MakeMetricHistoryPageDiagnostic(result.status,
                                                        "SB-METRICS-HISTORY-PAGE-SIZE-TOO-SMALL",
                                                        "metrics.history.page_size_too_small",
                                                        std::to_string(page_size));
    return result;
  }
  if (header.body_header_bytes != 64 || header.format_version != 1) {
    result.status = ErrorStatus();
    result.diagnostic = MakeMetricHistoryPageDiagnostic(result.status,
                                                        "SB-METRICS-HISTORY-PAGE-BODY-HEADER-INVALID",
                                                        "metrics.history.page_body_header_invalid");
    return result;
  }
  if (header.record_kind == MetricHistoryRecordKind::unknown) {
    result.status = ErrorStatus();
    result.diagnostic = MakeMetricHistoryPageDiagnostic(result.status,
                                                        "SB-METRICS-HISTORY-PAGE-RECORD-KIND-UNKNOWN",
                                                        "metrics.history.page_record_kind_unknown");
    return result;
  }
  if (header.free_bytes > result.capacity.usable_payload_bytes) {
    result.status = ErrorStatus();
    result.diagnostic = MakeMetricHistoryPageDiagnostic(result.status,
                                                        "SB-METRICS-HISTORY-PAGE-FREE-BYTES-INVALID",
                                                        "metrics.history.page_free_bytes_invalid");
    return result;
  }
  return result;
}

DiagnosticRecord MakeMetricHistoryPageDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.metric_history_page");
}

}  // namespace scratchbird::storage::page
