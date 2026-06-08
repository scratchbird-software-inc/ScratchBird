// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-014: portable runtime topology evidence for per-core memory caches.
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::platform {

struct RuntimeTopologySnapshot {
  std::string platform_name;
  bool logical_core_count_available = false;
  usize logical_core_count = 1;
  bool current_core_available = false;
  usize current_core_id = 0;
  bool numa_node_count_available = false;
  usize numa_node_count = 0;
  bool current_numa_node_available = false;
  int current_numa_node = -1;
  std::string logical_core_provider;
  std::string current_core_provider;
  std::string numa_provider;
  std::vector<std::string> evidence;
};

RuntimeTopologySnapshot CurrentRuntimeTopology();

}  // namespace scratchbird::core::platform
