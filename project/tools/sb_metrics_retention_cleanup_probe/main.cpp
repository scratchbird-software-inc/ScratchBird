// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_history.hpp"
#include "metric_producer.hpp"
#include "sb_metrics_history_probe_support.hpp"

#include <iostream>

int main() {
  using namespace scratchbird::core::metrics;
  using namespace scratchbird::tools::metrics_history_probe;
  const auto path = TempHistoryPath("sb_metrics_retention_cleanup_probe");
  RemoveTempHistory(path);
  bool ok = true;
  ok &= Require(ConfigureMetricHistoryPersistence(path).ok, "history persistence configured");
  const auto* descriptor = DefaultMetricRegistry().FindDescriptorOrAlias("sb_memory_allocated_bytes");
  MetricValue value;
  value.family = "sb_memory_allocated_bytes";
  value.type = MetricType::gauge;
  value.value = 10.0;
  value.labels = Labels({{"component", "cleanup"}});
  ok &= Require(AppendMetricRawSample(path, *descriptor, value, 1).ok, "old raw sample appended");
  ok &= Require(ApplyMetricRetentionCleanup(path,
                                            30ull * 24ull * 60ull * 60ull * 1000000ull,
                                            "018f0000-0000-7000-8000-000000000004",
                                            "018f0000-0000-7000-8000-000000000005").ok,
                "retention cleanup succeeded");
  auto store = LoadMetricHistoryStore(path);
  ok &= Require(store.raw_samples.empty(), "expired sample purged");
  ok &= Require(!store.evidence.empty(), "cleanup evidence written");
  RemoveTempHistory(path);
  if (!ok) return 1;
  std::cout << "metrics retention cleanup probe passed\n";
  return 0;
}
