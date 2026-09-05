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
  auto unavailable =
      Request<EngineAuthenticateProviderRequest>("workload_identity");
  unavailable.option_envelopes.push_back(
      "dependency:spiffe_svid_or_workload_oidc:available");
  const auto unavailable_r = EngineAuthenticateProvider(unavailable);
  auto no_mapping = unavailable;
  RemoveOption(&no_mapping, "groups:materialized");
  EngineAuthenticateProviderResult no_mapping_r;
  {
    ScopedTrustedProviderResult trusted_result;
    no_mapping_r = EngineAuthenticateProvider(no_mapping);
  }
  return Finish({
      {"workload_unavailable",
       HasDiagnostic(unavailable_r, "SECURITY.AUTH_SOURCE_UNAVAILABLE",
                     "workload_identity_validator_required")},
      {"service_mapping_required",
       HasDiagnostic(no_mapping_r, "SECURITY.GROUP.EXTERNAL_UNSYNCED",
                     "internal_group_materialization_required")},
  });
}
