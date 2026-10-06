// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "sb_test_temp_compat.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace scratchbird::tests {

// Declare before every device using this directory, so devices close before
// exception cleanup. Never removes or repairs a pre-existing shared /tmp path.
class OwnedTempDirectory final {
 public:
  OwnedTempDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "sb-datatype-fixture-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr)
      throw std::runtime_error("cannot create private datatype fixture directory");
    // Remove the newly created empty directory if recording its path fails.
    try {
      path_ = pattern;
    } catch (...) {
      std::error_code ignored;
      std::filesystem::remove(pattern, ignored);
      throw;
    }
  }
  OwnedTempDirectory(const OwnedTempDirectory&) = delete;
  OwnedTempDirectory& operator=(const OwnedTempDirectory&) = delete;
  ~OwnedTempDirectory() noexcept {
    if (path_.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error) std::cerr << "fixture cleanup failed: " << path_ << ": "
                         << error.message() << '\n';
  }
  const std::filesystem::path& path() const noexcept { return path_; }
  // Call only after all device handles are closed. Make cleanup failures fail
  // the success path instead of silently accumulating databases and sidecars.
  void Cleanup() {
    std::filesystem::remove_all(path_);
    path_.clear();
  }

 private:
  std::filesystem::path path_;
};

}  // namespace scratchbird::tests
