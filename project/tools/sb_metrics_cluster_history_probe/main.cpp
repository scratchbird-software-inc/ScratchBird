// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_history.hpp"
#include "observability/metrics_api.hpp"
#include "sb_metrics_history_probe_support.hpp"

#include <iostream>

int main() {
  using namespace scratchbird::engine::internal_api;
  using namespace scratchbird::tools::metrics_history_probe;
  bool ok = true;
  EngineClusterSysMetricsHistoryRequest denied;
  denied.context = MetricsContext(false, false);
  auto fail = EngineClusterSysMetricsHistory(denied);
  ok &= Require(!fail.ok && fail.cluster_authority_required, "cluster history fails closed without authority");
  EngineClusterSysMetricsHistoryRequest allowed;
  allowed.context = MetricsContext(true, false);
  auto success = EngineClusterSysMetricsHistory(allowed);
  ok &= Require(success.ok, "cluster history contract succeeds with authority even without rows");
  if (!ok) return 1;
  std::cout << "metrics cluster history probe passed\n";
  return 0;
}
