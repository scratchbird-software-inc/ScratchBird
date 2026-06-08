// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_METRICS_HISTORY_PAGE_BODY
// Metric history page/body descriptor. Persistent metric history records use
// the metrics page family and are classified safely when unknown by older code.

#include "page_header.hpp"
#include "runtime_platform.hpp"

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class MetricHistoryRecordKind : u16 {
  series = 1,
  raw_sample = 2,
  rollup = 3,
  retention_policy = 4,
  retention_evidence = 5,
  unknown = 0xffff
};

struct MetricHistoryPageBodyHeader {
  u32 body_header_bytes = 64;
  u16 format_version = 1;
  MetricHistoryRecordKind record_kind = MetricHistoryRecordKind::unknown;
  u16 reserved = 0;
  u64 page_generation = 0;
  u64 first_record_sequence = 0;
  u64 last_record_sequence = 0;
  u32 record_count = 0;
  u32 free_bytes = 0;
  u64 body_checksum = 0;
};

struct MetricHistoryPageCapacity {
  u32 page_size = 0;
  u32 body_bytes = 0;
  u32 usable_payload_bytes = 0;
  u32 minimum_record_slots = 0;
};

struct MetricHistoryPageValidationResult {
  Status status;
  DiagnosticRecord diagnostic;
  MetricHistoryPageBodyHeader header;
  MetricHistoryPageCapacity capacity;

  bool ok() const { return status.ok(); }
};

const char* MetricHistoryRecordKindName(MetricHistoryRecordKind kind);
MetricHistoryPageCapacity ComputeMetricHistoryPageCapacity(u32 page_size);
MetricHistoryPageValidationResult ValidateMetricHistoryPageBodyHeader(const MetricHistoryPageBodyHeader& header,
                                                                      u32 page_size);
DiagnosticRecord MakeMetricHistoryPageDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::storage::page
