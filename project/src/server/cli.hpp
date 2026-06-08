// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_SKELETON_CLI

#pragma once

#include "diagnostics.hpp"

#include <string>
#include <vector>

namespace scratchbird::server {

struct ServerManagementRequest;

struct ServerCliOptions {
  bool help = false;
  bool version = false;
  bool foreground = false;
  bool service = false;
  bool validate_config = false;
  bool validate_endpoints = false;
  bool read_only = false;
  bool maintenance = false;
  bool restricted_open = false;
  bool no_listeners = false;
  bool create_if_missing = false;
  bool lifecycle_request = false;
  std::string config_path;
  std::string control_dir;
  std::string runtime_dir;
  std::string database_ref;
  std::string sbps_endpoint;
  std::string log_path;
  std::string log_level = "info";
  std::string lifecycle_operation;
  std::string lifecycle_mode;
  std::string lifecycle_target_uuid;
  std::string lifecycle_audit_reason = "server_cli_lifecycle_request";
};

struct ServerCliParseResult {
  ServerCliOptions options;
  std::vector<ServerDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

ServerCliParseResult ParseServerCli(int argc, char** argv);
std::string ServerHelpText();
bool HasServerCliLifecycleRequest(const ServerCliOptions& options);
ServerManagementRequest BuildServerCliLifecycleRequest(const ServerCliOptions& options);

}  // namespace scratchbird::server
