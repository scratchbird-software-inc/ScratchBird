// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <filesystem>
#include <string>

#include "listener_config.hpp"

namespace scratchbird::listener {

struct ListenerSocketIdentity {
  std::string listener_uuid;
  std::string profile;
  std::string endpoint_hash;
  std::string generation;
  std::filesystem::path control_socket;
  std::filesystem::path management_socket;
  std::filesystem::path owner_file;
  std::filesystem::path lifecycle_file;
};

ListenerSocketIdentity BuildSocketIdentity(const ListenerConfig& config);
bool WriteOwnerToken(const ListenerSocketIdentity& identity, std::string* error_message);
bool WriteLifecycleStateToken(const ListenerSocketIdentity& identity,
                              const std::string& effective_state,
                              const std::string& requested_state,
                              const std::string& identity_json,
                              const std::string& pool_json,
                              std::string* error_message);
std::string SocketIdentityJson(const ListenerSocketIdentity& identity);

} // namespace scratchbird::listener
