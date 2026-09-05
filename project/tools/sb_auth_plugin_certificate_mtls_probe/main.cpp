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
      Request<EngineAuthenticateProviderRequest>("certificate_mtls");
  unavailable.option_envelopes.push_back("dependency:tls_x509:available");
  unavailable.option_envelopes.push_back("channel_binding:verified");
  const auto unavailable_r = EngineAuthenticateProvider(unavailable);

  auto no_groups = unavailable;
  RemoveOption(&no_groups, "groups:materialized");
  EngineAuthenticateProviderResult no_groups_r;
  {
    ScopedTrustedProviderResult trusted_result;
    no_groups_r = EngineAuthenticateProvider(no_groups);
  }
  return Finish({
      {"mtls_unavailable",
       HasDiagnostic(unavailable_r, "SECURITY.CREDENTIAL_INVALID",
                     "certificate_evidence_required")},
      {"group_materialization_required",
       HasDiagnostic(no_groups_r, "SECURITY.GROUP.EXTERNAL_UNSYNCED",
                     "internal_group_materialization_required")},
  });
}
