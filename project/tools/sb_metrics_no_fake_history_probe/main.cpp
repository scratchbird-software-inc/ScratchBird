// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_history.hpp"
#include "metric_contracts.hpp"
#include "metric_producer.hpp"
#include "sb_metrics_history_probe_support.hpp"

#include <iostream>

int main() {
  using namespace scratchbird::core::metrics;
  using namespace scratchbird::tools::metrics_history_probe;
  const auto path = TempHistoryPath("sb_metrics_no_fake_history_probe");
  RemoveTempHistory(path);
  bool ok = true;
  ok &= Require(ConfigureMetricHistoryPersistence(path).ok, "history persistence configured");
  auto store = LoadMetricHistoryStore(path);
  for (const auto& sample : store.raw_samples) {
    ok &= Require(sample.metric_family != "sb_archive_lag_bytes", "history does not fabricate archive samples");
  }
  ok &= Require(PublishArchiveLagBytes(42.0, "primary", "probe").ok,
                "archive producer emits explicit sample");
  store = LoadMetricHistoryStore(path);
  bool found_archive_sample = false;
  for (const auto& sample : store.raw_samples) {
    found_archive_sample = found_archive_sample || sample.metric_family == "sb_archive_lag_bytes";
  }
  ok &= Require(found_archive_sample, "explicit archive sample is persisted");
  RemoveTempHistory(path);
  if (!ok) return 1;
  std::cout << "metrics no fake history probe passed\n";
  return 0;
}
