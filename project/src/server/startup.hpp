// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_CONFIG_LIFECYCLE_STARTUP

#pragma once

#include "cli.hpp"
#include "config.hpp"
#include "diagnostics.hpp"
#include "lifecycle.hpp"

#include <string>
#include <vector>

namespace scratchbird::server {

struct ServerStartupResult {
  int exit_code = 0;
  std::string stdout_text;
  std::vector<ServerDiagnostic> diagnostics;
  ServerBootstrapConfig effective_config;
  ServerLifecycleArtifacts lifecycle_artifacts;
  bool serving_requested = false;
};

ServerStartupResult RunServerStartup(const ServerCliOptions& cli);

}  // namespace scratchbird::server
