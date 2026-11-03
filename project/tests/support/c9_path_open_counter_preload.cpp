// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_FILE_DEVICE_PATH_OPEN_OBSERVATION (register C9)
// LD_PRELOAD counter for a live server process. SB_C9_WATCH_PATH names the
// database file; at process exit the counts are written to SB_C9_COUNT_FILE
// as "file_opens=<n> sidecar_opens=<n> agent_file_opens=<n>
// agent_sidecar_opens=<n>". Test support only; never shipped.
#include "path_open_counter.hpp"

#include <fstream>

namespace {

struct PreloadCounter {
  PreloadCounter() {
    if (const char* path = std::getenv("SB_C9_WATCH_PATH")) {
      scratchbird::tests::path_open_counter::Watch(path);
    }
  }
  ~PreloadCounter() {
    const char* out = std::getenv("SB_C9_COUNT_FILE");
    if (out == nullptr) return;
    const auto file_opens = scratchbird::tests::path_open_counter::FileOpens();
    const auto sidecar_opens = scratchbird::tests::path_open_counter::SidecarOpens();
    const auto agent_file_opens = scratchbird::tests::path_open_counter::AgentFileOpens();
    const auto agent_sidecar_opens = scratchbird::tests::path_open_counter::AgentSidecarOpens();
    scratchbird::tests::path_open_counter::Watch({});  // the count file is not counted
    std::ofstream(out) << "file_opens=" << file_opens << " sidecar_opens=" << sidecar_opens
                       << " agent_file_opens=" << agent_file_opens
                       << " agent_sidecar_opens=" << agent_sidecar_opens << '\n';
  }
};

PreloadCounter preload_counter;

}  // namespace
