// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/standard_bundle_api.hpp"

#include <filesystem>
#include <iostream>

using namespace scratchbird::engine::internal_api;

int main() {
  const std::string path = "/tmp/sb_security_standard_bundle_probe.sbdb";
  std::filesystem::remove(path);
  EngineSeedStandardSecurityBundlesRequest request;
  request.context.database_path = path;
  request.context.local_transaction_id = 9;
  request.context.security_context_present = true;
  request.context.trace_tags.push_back("security.bootstrap");
  const auto seeded = EngineSeedStandardSecurityBundles(request);

  EngineSeedStandardSecurityBundlesRequest denied;
  denied.context.database_path = path;
  denied.context.local_transaction_id = 10;
  denied.context.security_context_present = true;
  const auto denied_result = EngineSeedStandardSecurityBundles(denied);

  const bool ok = seeded.ok && seeded.groups_seeded == 12 && seeded.roles_seeded == 5 && seeded.policies_seeded == 10 && !denied_result.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"groups\":" << seeded.groups_seeded
            << ",\"roles\":" << seeded.roles_seeded
            << ",\"policies\":" << seeded.policies_seeded
            << ",\"denied\":" << (!denied_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}
