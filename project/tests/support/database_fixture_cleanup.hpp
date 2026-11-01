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
    // X.sbdb.sb.*, X.sbdb.dirty.manifest, X.sbdb.bulk_import_streams.v1, ...
    if (candidate == name || candidate.starts_with(name + "."))
      owned.push_back(entry.path());
  }
  for (const auto& entry : owned) std::filesystem::remove_all(entry);
}
// Removes the database's fixture namespace at process exit. Static
// destructors run on return from main and on std::exit, so a failing check
// that exits still cleans up (not on abort or a fatal signal).
inline void RemoveDatabaseFixtureArtifactsAtExit(std::filesystem::path path) {
  struct Registry {
    std::vector<std::filesystem::path> paths;
    ~Registry() {
      for (const auto& registered : paths) {
        try { RemoveDatabaseFixtureArtifacts(registered); } catch (...) {}
      }
    }
  };
  static Registry registry;
  registry.paths.push_back(std::move(path));
}

// Removes the database's fixture namespace when the scope ends. A failing
// check that calls std::exit skips this, as it skips every destructor.
struct DatabaseFixtureArtifactsGuard {
  std::filesystem::path path;
  ~DatabaseFixtureArtifactsGuard() {
    try { RemoveDatabaseFixtureArtifacts(path); } catch (...) {}
  }
};
}  // namespace scratchbird::tests
