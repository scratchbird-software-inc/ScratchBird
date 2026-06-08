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
  bool ok = true;
  const auto* descriptor = DefaultMetricRegistry().FindDescriptorOrAlias("sb_memory_allocated_bytes");
  ok &= Require(descriptor != nullptr, "descriptor exists");
  const auto& policy = DefaultMetricRetentionPolicyForDescriptor(*descriptor);
  auto a = MakeMetricSeriesIdentity(*descriptor, Labels({{"component", "alpha"}}), policy);
  auto b = MakeMetricSeriesIdentity(*descriptor, Labels({{"component", "alpha"}}), policy);
  auto c = MakeMetricSeriesIdentity(*descriptor, Labels({{"component", "beta"}}), policy);
  ok &= Require(a.series_uuid == b.series_uuid, "same labels produce stable series uuid");
  ok &= Require(a.series_uuid != c.series_uuid, "changed labels produce different series uuid");
  ok &= Require(a.series_uuid.size() == 36 && a.series_uuid[14] == '7', "series uuid is v7-shaped identity");
  if (!ok) return 1;
  std::cout << "metrics series identity probe passed\n";
  return 0;
}
