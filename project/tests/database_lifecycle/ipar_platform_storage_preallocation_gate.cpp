// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "disk_device.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace disk = scratchbird::storage::disk;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

struct MatrixRow {
  std::string platform;
  std::string storage_class;
  std::string expected_strategy;
  bool fallback_allowed = false;
};

std::vector<MatrixRow> PlatformStorageMatrix() {
  return {
      {"linux", "nvme", "posix_fallocate", false},
      {"linux", "ssd", "posix_fallocate", false},
      {"linux", "hdd", "posix_fallocate", false},
      {"linux", "network_filesystem", "posix_fallocate_or_last_byte_extend", true},
      {"windows", "nvme", "set_file_allocation_info", false},
      {"windows", "ssd", "set_file_allocation_info", false},
      {"windows", "hdd", "set_file_allocation_info", false},
      {"windows", "network_filesystem", "set_file_allocation_info_or_last_byte_extend", true},
      {"macos", "nvme", "last_byte_extend", true},
      {"macos", "ssd", "last_byte_extend", true},
      {"macos", "hdd", "last_byte_extend", true},
      {"macos", "network_filesystem", "last_byte_extend", true},
  };
}

std::string CurrentPlatform() {
#if defined(__linux__)
  return "linux";
#elif defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "unknown";
#endif
}

void VerifyMatrixCompleteness() {
  const auto rows = PlatformStorageMatrix();
  const std::set<std::string> required_platforms = {"linux", "windows", "macos"};
  const std::set<std::string> required_storage_classes = {
      "nvme", "ssd", "hdd", "network_filesystem"};
  std::set<std::string> seen_platforms;
  std::set<std::string> seen_pairs;
  for (const auto& row : rows) {
    Require(!row.expected_strategy.empty(),
            "IPAR-P0-06 matrix row missing expected strategy");
    seen_platforms.insert(row.platform);
    seen_pairs.insert(row.platform + ":" + row.storage_class);
  }
  for (const auto& platform : required_platforms) {
    Require(seen_platforms.count(platform) == 1,
            "IPAR-P0-06 matrix missing required platform");
    for (const auto& storage_class : required_storage_classes) {
      Require(seen_pairs.count(platform + ":" + storage_class) == 1,
              "IPAR-P0-06 matrix missing required storage class");
    }
  }
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path path;

  Fixture() {
    dir = std::filesystem::temp_directory_path() /
          "scratchbird_ipar_platform_storage_preallocation";
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir);
    path = dir / "preallocate.sbfs";
  }

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

void VerifyLivePreallocation() {
  Fixture fixture;
  disk::FileDevice device;
  const auto opened =
      device.Open(fixture.path.string(), disk::FileOpenMode::create_or_truncate);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ':'
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "IPAR-P7-08 live preallocation file open failed");

  const auto capabilities = device.Capabilities();
  Require(capabilities.ok(), "IPAR-P7-08 capability query failed");
  Require(capabilities.capabilities.extent_preallocation,
          "IPAR-P7-08 writable file did not advertise extent preallocation");

  constexpr scratchbird::core::platform::u64 kOffset = 64 * 1024;
  constexpr scratchbird::core::platform::u64 kBytes = 256 * 1024;
  const auto preallocated = device.PreallocateExtent(kOffset, kBytes);
  if (!preallocated.ok()) {
    std::cerr << preallocated.diagnostic.diagnostic_code << ':'
              << preallocated.diagnostic.message_key << '\n';
  }
  Require(preallocated.ok(), "IPAR-P7-08 live preallocation failed");
  Require(preallocated.platform_preallocation_attempted,
          "IPAR-P7-08 platform preallocation was not attempted");
  Require(preallocated.logical_size_extended,
          "IPAR-P7-08 preallocation did not extend logical file size");

  const auto size = device.Size();
  Require(size.ok(), "IPAR-P7-08 size query after preallocation failed");
  Require(size.size_bytes >= kOffset + kBytes,
          "IPAR-P7-08 preallocation size proof failed");

  const std::string platform = CurrentPlatform();
  if (platform == "linux") {
    Require(preallocated.strategy == "posix_fallocate",
            "IPAR-P7-08 Linux preallocation did not use posix_fallocate");
    Require(preallocated.platform_preallocation_succeeded,
            "IPAR-P7-08 Linux preallocation did not succeed natively");
    Require(!preallocated.fallback_extension_used,
            "IPAR-P7-08 Linux preallocation unexpectedly used fallback");
  } else if (platform == "windows") {
    Require(preallocated.strategy == "set_file_allocation_info",
            "IPAR-P7-08 Windows preallocation did not use FileAllocationInfo");
    Require(preallocated.platform_preallocation_succeeded,
            "IPAR-P7-08 Windows preallocation did not succeed natively");
  } else {
    Require(preallocated.platform_preallocation_succeeded ||
                preallocated.fallback_extension_used,
            "IPAR-P7-08 unsupported platform did not use safe fallback");
  }

  Require(device.Close().ok(), "IPAR-P7-08 close after preallocation failed");
}

}  // namespace

int main() {
  VerifyMatrixCompleteness();
  VerifyLivePreallocation();
  std::cout << "ipar_platform_storage_preallocation_gate=passed platform="
            << CurrentPlatform() << '\n';
  return EXIT_SUCCESS;
}
