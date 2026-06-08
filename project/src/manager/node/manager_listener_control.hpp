// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_LISTENER_CONTROL_MODULE

#pragma once

#include "manager_runtime.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::manager::node {

struct ListenerControlMapping {
  bool supported = false;
  bool mutating = false;
  std::string listener_command;
  std::string diagnostic_code;
};

struct ListenerManagementCallResult {
  bool ok = false;
  std::string response_text;
  std::vector<proto::Diagnostic> diagnostics;
};

std::string ListenerControlSocketPath(const ManagerConfig& config);
ListenerControlMapping MapListenerControlOperation(
    const std::string& operation,
    const std::vector<std::pair<std::string, std::string>>& args);
proto::Bytes EncodeListenerManagementCommand(const std::string& command,
                                             std::uint64_t request_id);
std::string DecodeListenerManagementResponseText(const proto::Bytes& encoded,
                                                 std::vector<proto::Diagnostic>* diagnostics);
ListenerManagementCallResult SendListenerManagementCommand(const std::string& socket_path,
                                                           const std::string& command,
                                                           std::uint64_t request_id,
                                                           std::uint32_t timeout_ms = 5000);

} // namespace scratchbird::manager::node
