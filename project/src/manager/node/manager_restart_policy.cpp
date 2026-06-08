// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_RESTART_POLICY_MODULE

#include "manager_restart_policy.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace scratchbird::manager::node {

bool RestartArgumentsValid(const std::string& value) {
  for (char c : value) {
    if (c == '\'' || c == '"' || c == '\\' || c == '$' || c == '`' || c == '|' || c == '&' ||
        c == ';' || c == '<' || c == '>' || c == '(' || c == ')') {
      return false;
    }
  }
  return true;
}

bool RestartExecutableValid(const std::string& value) {
  if (value.empty()) return true;
  return std::filesystem::path(value).is_absolute() && RestartArgumentsValid(value);
}

std::vector<std::string> SplitRestartArguments(const std::string& value) {
  std::vector<std::string> out;
  std::istringstream in(value);
  std::string token;
  while (in >> token) out.push_back(token);
  return out;
}

std::uint64_t ComputeRestartBackoff(std::uint64_t attempt,
                                    std::uint64_t initial_backoff_ms,
                                    std::uint64_t max_backoff_ms) {
  std::uint64_t backoff = initial_backoff_ms;
  for (std::uint64_t i = 1; i < attempt && backoff < max_backoff_ms; ++i) {
    if (backoff > max_backoff_ms / 2u) {
      backoff = max_backoff_ms;
      break;
    }
    backoff *= 2u;
  }
  return std::min(backoff, max_backoff_ms);
}

} // namespace scratchbird::manager::node
