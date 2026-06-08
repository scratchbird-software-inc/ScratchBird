// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_MCP_PAYLOAD_MODULE

#pragma once

#include "manager_protocol.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::manager::node {
namespace proto = scratchbird::manager::protocol;

bool DecodeManagerCommandPayload(const proto::Bytes& payload,
                                 std::string* operation,
                                 std::string* idempotency_key,
                                 std::vector<std::pair<std::string, std::string>>* args);

} // namespace scratchbird::manager::node
