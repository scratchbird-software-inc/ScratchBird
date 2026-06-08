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
  const auto path = TempHistoryPath("sb_metrics_rollup_probe");
  RemoveTempHistory(path);
  bool ok = true;
  ok &= Require(ConfigureMetricHistoryPersistence(path).ok, "history persistence configured");
  const auto* descriptor = DefaultMetricRegistry().FindDescriptorOrAlias("sb_memory_allocated_bytes");
  MetricValue value;
  value.family = "sb_memory_allocated_bytes";
  value.type = MetricType::gauge;
  value.value = 10.0;
  value.labels = Labels({{"component", "rollup"}});
  ok &= Require(AppendMetricRawSample(path, *descriptor, value, 60000000).ok, "first raw sample appended");
  value.value = 20.0;
  ok &= Require(AppendMetricRawSample(path, *descriptor, value, 120000000).ok, "second raw sample appended");
  ok &= Require(GenerateMetricRollups(path, MetricRollupGrain::one_minute).ok, "minute rollup generated");
  ok &= Require(GenerateMetricRollups(path, MetricRollupGrain::one_minute).ok, "minute rollup idempotent rerun");
  auto store = LoadMetricHistoryStore(path);
  ok &= Require(store.rollups.size() == 2, "one rollup per minute window without duplicates");
  RemoveTempHistory(path);
  if (!ok) return 1;
  std::cout << "metrics rollup probe passed\n";
  return 0;
}
