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
#include "observability/metrics_api.hpp"
#include "sb_metrics_history_probe_support.hpp"

#include <iostream>

int main() {
  using namespace scratchbird::core::metrics;
  using namespace scratchbird::engine::internal_api;
  using namespace scratchbird::tools::metrics_history_probe;
  const auto path = TempHistoryPath("sb_metrics_history_redaction_probe");
  RemoveTempHistory(path);
  bool ok = true;
  ok &= Require(ConfigureMetricHistoryPersistence(path).ok, "history persistence configured");
  ok &= Require(PublishIdentitySessionsActive(1.0,
                                              "local_password",
                                              "self",
                                              Labels({{"session_uuid", "secret-session"}, {"principal_uuid", "secret-principal"}})).ok,
                "sensitive identity metric emitted");
  EngineSysMetricsHistoryRequest request;
  request.context = MetricsContext(false, false, false);
  request.option_envelopes.push_back("history_path:" + path);
  auto result = EngineSysMetricsHistory(request);
  ok &= Require(result.ok, "history surface succeeds");
  ok &= Require(!ContainsValue(result, "secret-session") && ContainsValue(result, "<redacted>"),
                "history surface redacts sensitive labels");
  RemoveTempHistory(path);
  if (!ok) return 1;
  std::cout << "metrics history redaction probe passed\n";
  return 0;
}
