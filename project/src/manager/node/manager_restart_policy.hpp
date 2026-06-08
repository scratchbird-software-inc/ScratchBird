// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_RESTART_POLICY_MODULE

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::manager::node {

bool RestartArgumentsValid(const std::string& value);
bool RestartExecutableValid(const std::string& value);
std::vector<std::string> SplitRestartArguments(const std::string& value);
std::uint64_t ComputeRestartBackoff(std::uint64_t attempt,
                                    std::uint64_t initial_backoff_ms,
                                    std::uint64_t max_backoff_ms);

} // namespace scratchbird::manager::node
