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
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace disk = scratchbird::storage::disk;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

std::filesystem::path TempRoot() {
  auto root = std::filesystem::temp_directory_path() / "scratchbird_public_disk_file_device_gate";
  std::filesystem::create_directories(root);
  return root;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool CreateNewDoesNotTruncateExistingFile() {
  const auto path = TempRoot() / "existing.sbdb";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "keep-existing-content";
  }

  disk::FileDevice device;
  const auto opened = device.Open(path.string(), disk::FileOpenMode::create_new);
  return Expect(!opened.ok(), "create_new should reject an existing file") &&
         Expect(ReadText(path) == "keep-existing-content",
                "create_new must not truncate or rewrite an existing file");
}

bool CreateNewSyncsAndPreservesWrittenBytes() {
  const auto path = TempRoot() / "created.sbdb";
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".sb.owner.lock");

  disk::FileDevice device;
  auto opened = device.Open(path.string(), disk::FileOpenMode::create_new);
  if (!Expect(opened.ok(), "create_new should atomically create a missing file")) {
    return false;
  }
  const std::string payload = "scratchbird-durable-sync";
  auto written = device.WriteAt(0, payload.data(), payload.size());
  auto synced = device.Sync();
  auto closed = device.Close();
  return Expect(written.ok(), "write_at should succeed for create_new file") &&
         Expect(synced.ok(), "sync should perform durable writable-file sync") &&
         Expect(closed.ok(), "close should succeed after sync") &&
         Expect(ReadText(path) == payload, "synced file content mismatch");
}

bool ConcurrentCreateNewHasSingleWinner() {
  const auto path = TempRoot() / "concurrent.sbdb";
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".sb.owner.lock");

  std::vector<bool> opened(2, false);
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < opened.size(); ++i) {
    threads.emplace_back([&, i]() {
      disk::FileDevice device;
      auto result = device.Open(path.string(), disk::FileOpenMode::create_new);
      opened[i] = result.ok();
      if (result.ok()) {
        const std::string payload = "winner-" + std::to_string(i);
        (void)device.WriteAt(0, payload.data(), payload.size());
        (void)device.Sync();
        (void)device.Close();
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const int winners = static_cast<int>(opened[0]) + static_cast<int>(opened[1]);
  return Expect(winners == 1, "concurrent create_new must have exactly one winner") &&
         Expect(std::filesystem::exists(path), "concurrent create_new should leave one file");
}

}  // namespace

int main() {
  bool ok = true;
  ok = CreateNewDoesNotTruncateExistingFile() && ok;
  ok = CreateNewSyncsAndPreservesWrittenBytes() && ok;
  ok = ConcurrentCreateNewHasSingleWinner() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
