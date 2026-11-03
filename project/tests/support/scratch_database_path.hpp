// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "database_fixture_cleanup.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

namespace scratchbird::tests {

// A database path unique to this process under the temp directory, whose
// fixture files (the path and every "path.*" sidecar) are removed at exit.
// Use it instead of a fixed /tmp name, which collides between parallel runs
// and leaves the sidecars behind.
inline std::string ScratchAvailabilityDatabasePath(std::string_view tag) {
  const auto path =
      std::filesystem::temp_directory_path() /
      (std::string(tag) + "_" + std::to_string(::getpid()) + "_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  RemoveDatabaseFixtureArtifactsAtExit(path);
  return path.string();
}

}  // namespace scratchbird::tests
