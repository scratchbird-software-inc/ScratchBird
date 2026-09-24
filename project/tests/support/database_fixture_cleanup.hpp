// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace scratchbird::tests {
// Call only after the test has released every handle to this database.
// Enumerate the exact fixture namespace before removing it; preserve siblings.
inline void RemoveDatabaseFixtureArtifacts(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  if (name.empty() || name == "." || name == "..")
    throw std::invalid_argument("database fixture filename required");
  const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
  if (!std::filesystem::exists(parent)) return;
  std::vector<std::filesystem::path> owned;
  for (const auto& entry : std::filesystem::directory_iterator(parent)) {
    const auto candidate = entry.path().filename().string();
    if (candidate == name || candidate.starts_with(name + ".sb.") ||
        candidate == name + ".dirty.manifest" || candidate == name + ".recovery.evidence")
      owned.push_back(entry.path());
  }
  for (const auto& entry : owned) std::filesystem::remove_all(entry);
}
}  // namespace scratchbird::tests
