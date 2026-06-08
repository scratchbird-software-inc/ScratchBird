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
  const auto path = TempHistoryPath("sb_metrics_history_storage_probe");
  RemoveTempHistory(path);
  bool ok = true;
  ok &= Require(ConfigureMetricHistoryPersistence(path).ok, "history persistence configured");
  ok &= Require(SetGauge("sb_memory_allocated_bytes", Labels({{"component", "probe"}}), 64.0, "core_memory").ok,
                "persistent gauge sample accepted");
  ok &= Require(ObserveHistogram("sb_storage_device_read_latency_microseconds",
                                 Labels({{"component", "probe"}, {"operation", "read_at"}, {"result", "ok"}}),
                                 11.0,
                                 "storage_disk").ok,
                "persistent histogram sample accepted");
  DisableMetricHistoryPersistence();
  auto store = LoadMetricHistoryStore(path);
  ok &= Require(!store.raw_samples.empty(), "raw samples persisted across disable/reload");
  ok &= Require(!store.series.empty(), "series rows persisted across disable/reload");
  ok &= Require(GenerateMetricRollups(path, MetricRollupGrain::one_minute).ok, "rollups generated");
  store = LoadMetricHistoryStore(path);
  ok &= Require(!store.rollups.empty(), "rollups persisted across reload");
  RemoveTempHistory(path);
  if (!ok) return 1;
  std::cout << "metrics history storage probe passed\n";
  return 0;
}
