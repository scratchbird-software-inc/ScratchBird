// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"

int main() {
  using namespace sb_auth_probe;
  auto metrics = Request<EngineInspectAuthProviderMetricsRequest>("ldap_ad");
  const auto metrics_r = EngineInspectAuthProviderMetrics(metrics);
  auto denied = metrics;
  denied.context =
      ContextWithoutRights({"deny:OBS_METRICS_READ_FAMILY"});
  const auto denied_r = EngineInspectAuthProviderMetrics(denied);
  return Finish({
      {"metrics", metrics_r.ok && metrics_r.metrics_available},
      {"metrics_denied",
       HasDiagnostic(denied_r, "SECURITY.AUTHORIZATION.DENIED",
                     "OBS_METRICS_READ_FAMILY")},
  });
}
