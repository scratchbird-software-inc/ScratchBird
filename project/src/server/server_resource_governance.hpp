// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_SERVER_RESOURCE_GOVERNANCE_ODF_106
// Server-side admission API for ODF-106 runtime quota descriptors. The server
// may present runtime policy descriptors to the engine-owned governance route;
// it does not own finality, visibility, parser execution, donor behavior, or
// recovery authority.

#include "resource_governance_admission.hpp"

#include <string>
#include <vector>

namespace scratchbird::server {

struct ServerResourceGovernanceContext {
  bool engine_runtime_bound = false;
  bool security_context_present = false;
  bool parser_or_client_authority = false;
  std::string principal_redaction_class = "operational_redacted";
};

struct ServerResourceGovernanceResult {
  bool ok = false;
  bool fail_closed = true;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  scratchbird::core::agents::ResourceGovernanceAdmissionResult admission;
};

ServerResourceGovernanceResult AdmitServerResourceGovernance(
    const ServerResourceGovernanceContext& context,
    scratchbird::core::agents::ResourceGovernanceAdmissionRequest request);

}  // namespace scratchbird::server
