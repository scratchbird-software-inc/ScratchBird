// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authority_api.hpp"

#include <iostream>

using namespace scratchbird::engine::internal_api;

namespace {
EngineRequestContext Context(bool cluster = false) {
  EngineRequestContext context;
  context.security_context_present = true;
  context.database_path = "/tmp/sb_security_authority_probe.sbdb";
  context.database_uuid.canonical = "018f0000-0000-7000-8000-00000000a001";
  context.cluster_uuid.canonical = "018f0000-0000-7000-8000-00000000a002";
  context.cluster_authority_available = cluster;
  return context;
}
}

int main() {
  EngineResolveSecurityAuthorityRequest local;
  local.context = Context();
  local.candidate.authority_class = "database_local";
  const auto local_result = EngineResolveSecurityAuthority(local);

  EngineResolveSecurityAuthorityRequest cluster;
  cluster.context = Context(false);
  cluster.candidate.authority_class = "cluster_security";
  const auto cluster_result = EngineResolveSecurityAuthority(cluster);

  EngineResolveSecurityAuthorityRequest invalid;
  invalid.context = Context();
  invalid.candidate.authority_class = "magic";
  const auto invalid_result = EngineResolveSecurityAuthority(invalid);

  const bool ok = local_result.ok && local_result.admitted && !cluster_result.ok && cluster_result.cluster_authority_required && !invalid_result.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"local_admitted\":" << (local_result.admitted ? "true" : "false")
            << ",\"cluster_required\":" << (cluster_result.cluster_authority_required ? "true" : "false")
            << ",\"invalid_rejected\":" << (!invalid_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}
