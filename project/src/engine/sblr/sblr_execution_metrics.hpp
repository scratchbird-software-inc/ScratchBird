// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrExecutionMetricSample {
  std::string metric_id;
  std::string operation_id;
  std::uint64_t value = 0;
};

struct SblrExecutionMetrics {
  std::uint64_t opcode_dispatch_count = 0;
  std::uint64_t function_call_count = 0;
  std::uint64_t refusal_count = 0;
  std::uint64_t error_count = 0;
  std::uint64_t latency_microseconds_total = 0;
};

void RecordSblrOpcodeDispatchMetric(SblrExecutionMetrics* metrics, std::string operation_id, std::uint64_t latency_us);
void RecordSblrFunctionCallMetric(SblrExecutionMetrics* metrics, std::string function_id, std::uint64_t latency_us);
void RecordSblrRefusalMetric(SblrExecutionMetrics* metrics, std::string operation_id);
void RecordSblrErrorMetric(SblrExecutionMetrics* metrics, std::string operation_id);
std::vector<SblrExecutionMetricSample> SnapshotSblrExecutionMetrics(const SblrExecutionMetrics& metrics);

}  // namespace scratchbird::engine::sblr
