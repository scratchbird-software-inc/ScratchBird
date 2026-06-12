// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-014: portable runtime topology evidence for per-core memory caches.
#include "runtime_topology.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <dirent.h>
#include <sched.h>
#endif

namespace scratchbird::core::platform {
namespace {

constexpr const char* kAuthorityBoundary =
    "runtime_topology.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_authorization_optimizer_plan_index_finality_or_agent_action_authority";

std::string PlatformName() {
#if defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  return "bsd";
#else
  return "unsupported";
#endif
}

void AddEvidence(RuntimeTopologySnapshot* snapshot,
                 std::string key,
                 std::string value) {
  snapshot->evidence.push_back("runtime_topology." + std::move(key) + "=" +
                               std::move(value));
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

#if defined(__linux__)
bool ParseNonNegativeInt(const std::string& text, usize* value) {
  if (text.empty()) {
    return false;
  }
  usize parsed = 0;
  for (char ch : text) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
    const usize digit = static_cast<usize>(ch - '0');
    if (parsed > (std::numeric_limits<usize>::max() - digit) / 10u) {
      return false;
    }
    parsed = (parsed * 10u) + digit;
  }
  *value = parsed;
  return true;
}

bool ParseNodeRangeToken(const std::string& token, usize* count) {
  const auto dash = token.find('-');
  if (dash == std::string::npos) {
    usize node = 0;
    if (!ParseNonNegativeInt(token, &node)) {
      return false;
    }
    (void)node;
    *count += 1;
    return true;
  }
  usize first = 0;
  usize last = 0;
  if (!ParseNonNegativeInt(token.substr(0, dash), &first) ||
      !ParseNonNegativeInt(token.substr(dash + 1), &last) ||
      last < first) {
    return false;
  }
  if (*count > std::numeric_limits<usize>::max() - ((last - first) + 1u)) {
    return false;
  }
  *count += (last - first) + 1u;
  return true;
}

bool ReadLinuxNumaNodeCount(usize* node_count) {
  std::ifstream input("/sys/devices/system/node/online");
  if (!input.is_open()) {
    return false;
  }
  std::string text;
  std::getline(input, text);
  if (text.empty()) {
    return false;
  }
  std::replace(text.begin(), text.end(), ',', ' ');
  std::istringstream tokens(text);
  std::string token;
  usize count = 0;
  while (tokens >> token) {
    if (!ParseNodeRangeToken(token, &count)) {
      return false;
    }
  }
  if (count == 0) {
    return false;
  }
  *node_count = count;
  return true;
}

bool ReadLinuxCurrentNumaNode(usize core_id, int* numa_node) {
  const std::string cpu_path =
      "/sys/devices/system/cpu/cpu" + std::to_string(core_id);
  DIR* dir = ::opendir(cpu_path.c_str());
  if (dir == nullptr) {
    return false;
  }
  bool found = false;
  int node = -1;
  while (dirent* entry = ::readdir(dir)) {
    std::string name(entry->d_name);
    if (name.rfind("node", 0) != 0) {
      continue;
    }
    usize parsed = 0;
    if (ParseNonNegativeInt(name.substr(4), &parsed) &&
        parsed <= static_cast<usize>(std::numeric_limits<int>::max())) {
      node = static_cast<int>(parsed);
      found = true;
      break;
    }
  }
  ::closedir(dir);
  if (!found) {
    return false;
  }
  *numa_node = node;
  return true;
}
#endif

}  // namespace

RuntimeTopologySnapshot CurrentRuntimeTopology() {
  RuntimeTopologySnapshot snapshot;
  snapshot.platform_name = PlatformName();
  snapshot.evidence.push_back("CEIC-014_THREAD_LOCAL_PER_CORE_NUMA_CACHE");
  snapshot.evidence.push_back(kAuthorityBoundary);
  AddEvidence(&snapshot, "platform", snapshot.platform_name);

  const unsigned int logical_cores = std::thread::hardware_concurrency();
  if (logical_cores != 0) {
    snapshot.logical_core_count_available = true;
    snapshot.logical_core_count = static_cast<usize>(logical_cores);
    snapshot.logical_core_provider = "std_thread_hardware_concurrency";
  } else {
    snapshot.logical_core_count = 1;
    snapshot.logical_core_provider = "portable_single_core_fallback";
  }

#if defined(__linux__)
  const int current_cpu = ::sched_getcpu();
  if (current_cpu >= 0) {
    snapshot.current_core_available = true;
    snapshot.current_core_id = static_cast<usize>(current_cpu);
    snapshot.current_core_provider = "linux_sched_getcpu";
  } else {
    snapshot.current_core_provider = "portable_thread_hash_fallback";
  }

  usize numa_nodes = 0;
  if (ReadLinuxNumaNodeCount(&numa_nodes)) {
    snapshot.numa_node_count_available = true;
    snapshot.numa_node_count = numa_nodes;
    snapshot.numa_provider = "linux_sysfs_node_online";
  } else {
    snapshot.numa_provider = "portable_fallback_numa_unavailable";
  }

  int current_numa_node = -1;
  if (snapshot.current_core_available &&
      ReadLinuxCurrentNumaNode(snapshot.current_core_id, &current_numa_node)) {
    snapshot.current_numa_node_available = true;
    snapshot.current_numa_node = current_numa_node;
  }
#else
  snapshot.current_core_provider = "portable_thread_hash_fallback";
  snapshot.numa_provider = "portable_fallback_numa_unavailable";
#endif

  AddEvidence(&snapshot,
              "logical_core_count_available",
              BoolText(snapshot.logical_core_count_available));
  AddEvidence(&snapshot,
              "logical_core_count",
              std::to_string(snapshot.logical_core_count));
  AddEvidence(&snapshot,
              "logical_core_provider",
              snapshot.logical_core_provider);
  AddEvidence(&snapshot,
              "current_core_available",
              BoolText(snapshot.current_core_available));
  AddEvidence(&snapshot,
              "current_core_id",
              std::to_string(snapshot.current_core_id));
  AddEvidence(&snapshot,
              "current_core_provider",
              snapshot.current_core_provider);
  AddEvidence(&snapshot,
              "numa_node_count_available",
              BoolText(snapshot.numa_node_count_available));
  AddEvidence(&snapshot,
              "numa_node_count",
              std::to_string(snapshot.numa_node_count));
  AddEvidence(&snapshot, "numa_provider", snapshot.numa_provider);
  AddEvidence(&snapshot,
              "current_numa_node_available",
              BoolText(snapshot.current_numa_node_available));
  AddEvidence(&snapshot,
              "current_numa_node",
              std::to_string(snapshot.current_numa_node));
  AddEvidence(&snapshot,
              "numa_claim",
              snapshot.numa_node_count_available ? "discovered" : "not_claimed");
  return snapshot;
}

}  // namespace scratchbird::core::platform
