// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_execution_metrics.hpp"

#include <utility>

namespace scratchbird::engine::sblr {

void RecordSblrOpcodeDispatchMetric(SblrExecutionMetrics* metrics, std::string, std::uint64_t latency_us) {
  if (!metrics) return;
  ++metrics->opcode_dispatch_count;
  metrics->latency_microseconds_total += latency_us;
}

void RecordSblrFunctionCallMetric(SblrExecutionMetrics* metrics, std::string, std::uint64_t latency_us) {
  if (!metrics) return;
  ++metrics->function_call_count;
  metrics->latency_microseconds_total += latency_us;
}

void RecordSblrRefusalMetric(SblrExecutionMetrics* metrics, std::string) {
  if (!metrics) return;
  ++metrics->refusal_count;
}

void RecordSblrErrorMetric(SblrExecutionMetrics* metrics, std::string) {
  if (!metrics) return;
  ++metrics->error_count;
}

std::vector<SblrExecutionMetricSample> SnapshotSblrExecutionMetrics(const SblrExecutionMetrics& metrics) {
  return {
      {"sys.metrics.executor.sblr.opcode_dispatch_count", "sblr", metrics.opcode_dispatch_count},
      {"sys.metrics.executor.sblr.function_call_count", "sblr", metrics.function_call_count},
      {"sys.metrics.executor.sblr.refusal_count", "sblr", metrics.refusal_count},
      {"sys.metrics.executor.sblr.error_count", "sblr", metrics.error_count},
      {"sys.metrics.executor.sblr.latency_microseconds_total", "sblr", metrics.latency_microseconds_total},
  };
}

}  // namespace scratchbird::engine::sblr
