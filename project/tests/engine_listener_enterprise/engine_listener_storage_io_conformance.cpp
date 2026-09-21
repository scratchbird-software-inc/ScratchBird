// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "disk_device.hpp"
#include "page_manager.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <cerrno>
#include <chrono>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
extern char** environ;
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

std::filesystem::path TempRoot() {
  std::string scope = std::filesystem::current_path().filename().string();
  if (scope.empty()) {
    scope = "default";
  }
#ifdef _WIN32
  const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long long>(::getpid());
#endif
  auto root = std::filesystem::temp_directory_path() /
              ("scratchbird_engine_listener_storage_io_conformance_" + scope +
               "_" + std::to_string(pid));
  std::filesystem::create_directories(root);
  return root;
}

void RemoveDeviceArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.owner.lock", ignored);
  std::filesystem::remove(path.string() + ".sb.route.owner.lock", ignored);
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

#if defined(__linux__)
// Core SB-MGA-ATTACH-SINGLE-DATABASE-PROCESS-OWNERSHIP-V1 requires
// exclusive ownership even for inspection. File existence is not ownership.
// Exec gives each contender fresh process state, without inherited C++ owners.
constexpr disk::FileOpenMode kProbeModes[] = {
    disk::FileOpenMode::open_existing,
    disk::FileOpenMode::open_existing_read_only,
    disk::FileOpenMode::create_or_truncate,
    disk::FileOpenMode::create_new};

int OwnerProbe(const char* path, std::string_view mode, std::string_view outcome) {
  if (mode.size() != 1 || mode[0] < '0' || mode[0] > '3') return 10;
  disk::FileDevice contender;
  const auto opened = contender.Open(path, kProbeModes[mode[0] - '0']);
  if (outcome == "held") {
    return !opened.ok() &&
                   opened.diagnostic.diagnostic_code == "SB-STORAGE-DISK-OWNER-LOCK-HELD"
               ? 0 : 11;
  }
  if (!opened.ok()) return 12;
  // Exercise OS release without calling Close or any C++ destructor.
  if (outcome == "crash") ::_exit(0);
  if (outcome != "open") return 13;
  return contender.Close().ok() ? 0 : 14;
}

bool RunOwnerProbe(const std::string& executable, const std::filesystem::path& path,
                   int mode, const std::string& outcome) {
  std::string command = "--owner-probe";
  std::string target = path.string();
  std::string mode_arg = std::to_string(mode);
  char* args[] = {const_cast<char*>(executable.c_str()), command.data(),
                  target.data(), mode_arg.data(), const_cast<char*>(outcome.c_str()), nullptr};
  pid_t child = -1;
  if (!Require(::posix_spawn(&child, executable.c_str(), nullptr, nullptr, args, environ) == 0,
               "could not start independent ownership probe")) return false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  int status = 0;
  for (;;) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      return Require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                     "ownership probe failed: mode=" + mode_arg + " expected=" + outcome +
                         " wait_status=" + std::to_string(status));
    }
    if (waited < 0 && errno != EINTR) return Require(false, "ownership probe wait failed");
    if (std::chrono::steady_clock::now() >= deadline) {
      (void)::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      return Require(false, "ownership probe timed out");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool FileDeviceProcessOwnership(const std::string& executable) {
  const auto path = TempRoot() / "process_owner.sbdb";
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { RemoveDeviceArtifacts(path); }
  } cleanup{path};  // destroyed after every FileDevice below
  const std::string payload = "ownership-refusal-must-preserve-these-bytes";
  disk::FileDevice creator;
  if (!Require(creator.Open(path.string(), disk::FileOpenMode::create_new).ok(), "owner fixture create") ||
      !Require(creator.WriteAt(0, payload.data(), payload.size()).ok(), "owner fixture write") ||
      !Require(creator.Close().ok(), "owner fixture close")) return false;

  for (int owner_mode = 0; owner_mode != 2; ++owner_mode) {
    disk::FileDevice owner;
    if (!Require(owner.Open(path.string(), kProbeModes[owner_mode]).ok(), "owner acquisition")) return false;
    for (int contender_mode = 0; contender_mode != 4; ++contender_mode) {
      if (!RunOwnerProbe(executable, path, contender_mode, "held") ||
          !Require(ReadText(path) == payload, "refused contender modified owner data")) return false;
    }
    if (!Require(owner.Close().ok(), "owner release")) return false;
    for (int next_mode = 0; next_mode != 2; ++next_mode) {
      if (!RunOwnerProbe(executable, path, next_mode, "open") ||
          !RunOwnerProbe(executable, path, next_mode, "crash") ||
          !RunOwnerProbe(executable, path, next_mode, "open") ||
          !Require(ReadText(path) == payload, "ownership handoff modified data")) return false;
    }
  }
  return true;
}
#endif

bool CheckedPageOffsetArithmetic() {
  constexpr std::uint32_t kSupportedPageSize = 8192;
  const auto positive = page::CheckedPageOffset(kSupportedPageSize, 7);
  if (!Require(positive.ok(), "checked page offset rejected a valid offset") ||
      !Require(positive.offset == 57344, "checked page offset value mismatch")) {
    return false;
  }

  const auto body =
      page::CheckedPageBodyOffset(kSupportedPageSize, 7, disk::kPageHeaderSerializedBytes);
  if (!Require(body.ok(), "checked page body offset rejected a valid offset") ||
      !Require(body.offset == 57344 + disk::kPageHeaderSerializedBytes,
               "checked page body offset value mismatch")) {
    return false;
  }

  const auto invalid_page_size = page::CheckedPageOffset(12345, 1);
  if (!Require(!invalid_page_size.ok(), "invalid page size accepted") ||
      !Require(invalid_page_size.diagnostic.diagnostic_code ==
                   "SB-PAGE-MANAGER-PAGE-SIZE-INVALID",
               "invalid page size diagnostic mismatch")) {
    return false;
  }

  const auto overflow =
      page::CheckedPageOffset(65536, std::numeric_limits<std::uint64_t>::max() / 65536 + 1);
  if (!Require(!overflow.ok(), "overflowing page offset accepted") ||
      !Require(overflow.diagnostic.diagnostic_code ==
                   "SB-PAGE-MANAGER-PAGE-OFFSET-OVERFLOW",
               "page offset overflow diagnostic mismatch")) {
    return false;
  }

  const auto invalid_body =
      page::CheckedPageBodyOffset(kSupportedPageSize, 1, kSupportedPageSize);
  return Require(!invalid_body.ok(), "invalid in-page offset accepted") &&
         Require(invalid_body.diagnostic.diagnostic_code ==
                     "SB-PAGE-MANAGER-IN-PAGE-OFFSET-INVALID",
                 "invalid in-page offset diagnostic mismatch");
}

bool CheckedFileExtentArithmetic() {
  const auto positive = disk::CheckFileDeviceExtent(128, 512);
  if (!Require(positive.ok(), "valid file extent rejected") ||
      !Require(positive.offset == 128 && positive.bytes == 512,
               "valid file extent values changed")) {
    return false;
  }

  const auto offset_conversion =
      disk::CheckFileDeviceExtent(std::numeric_limits<std::uint64_t>::max(), 1);
  if (!Require(!offset_conversion.ok(), "unrepresentable stream offset accepted") ||
      !Require(offset_conversion.diagnostic.diagnostic_code ==
                   "SB-STORAGE-DISK-OFFSET-CONVERSION-OVERFLOW",
               "stream offset conversion diagnostic mismatch")) {
    return false;
  }

  const auto max_streamoff =
      static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
  const auto extent_overflow = disk::CheckFileDeviceExtent(max_streamoff, 1);
  if (!Require(!extent_overflow.ok(), "file extent overflow accepted") ||
      !Require(extent_overflow.diagnostic.diagnostic_code ==
                   "SB-STORAGE-DISK-EXTENT-OVERFLOW",
               "file extent overflow diagnostic mismatch")) {
    return false;
  }

  const auto page_overflow =
      disk::CheckDevicePageOffset(65536, std::numeric_limits<std::uint64_t>::max() / 65536 + 1);
  return Require(!page_overflow.ok(), "disk page offset overflow accepted") &&
         Require(page_overflow.diagnostic.diagnostic_code ==
                     "SB-STORAGE-DISK-PAGE-OFFSET-OVERFLOW",
                 "disk page offset overflow diagnostic mismatch");
}

bool FileDeviceDurableCreateSyncCloseAndReadOnlyRefusal() {
  const auto path = TempRoot() / "io_conformance.sbdb";
  RemoveDeviceArtifacts(path);

  disk::FileDevice device;
  const auto opened = device.Open(path.string(), disk::FileOpenMode::create_new);
  if (!Require(opened.ok(), "create_new open failed")) {
    RemoveDeviceArtifacts(path);
    return false;
  }

  const std::string payload = "engine-listener-storage-io-proof";
  const auto write = device.WriteAt(0, payload.data(), payload.size());
  const auto sync = device.Sync();
  const auto close = device.Close();
  if (!Require(write.ok(), "valid write_at failed") ||
      !Require(write.bytes_transferred == payload.size(), "write_at byte count mismatch") ||
      !Require(sync.ok(), "durable sync failed") ||
      !Require(close.ok(), "close failed") ||
      !Require(ReadText(path) == payload, "synced payload was not preserved") ||
      !Require(!device.is_open(), "closed device still reports an open handle")) {
    RemoveDeviceArtifacts(path);
    return false;
  }

  disk::FileDevice read_only;
  const auto read_opened = read_only.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
  char byte = '\0';
  const auto refused_write = read_only.WriteAt(0, &byte, 1);
  const auto read_only_sync = read_only.Sync();
  const auto read_only_close = read_only.Close();
  const bool ok =
      Require(read_opened.ok(), "read-only open failed") &&
      Require(!refused_write.ok(), "read-only write was accepted") &&
      Require(refused_write.diagnostic.diagnostic_code ==
                  "SB-STORAGE-DISK-WRITE-READ-ONLY",
              "read-only write diagnostic mismatch") &&
      Require(read_only_sync.ok(), "read-only sync should be a safe no-op") &&
      Require(read_only_close.ok(), "read-only close failed");
  RemoveDeviceArtifacts(path);
  return ok;
}

bool FileDeviceHugeOffsetRefusalPrecedesSeek() {
  const auto path = TempRoot() / "huge_offset.sbdb";
  RemoveDeviceArtifacts(path);

  disk::FileDevice device;
  const auto opened = device.Open(path.string(), disk::FileOpenMode::create_new);
  if (!Require(opened.ok(), "huge-offset fixture open failed")) {
    RemoveDeviceArtifacts(path);
    return false;
  }

  char byte = '\0';
  const auto max_streamoff =
      static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
  const auto refused = device.WriteAt(max_streamoff, &byte, 1);
  const auto header_overflow = disk::ReadDevicePageHeader(
      &device,
      65536,
      std::numeric_limits<std::uint64_t>::max() / 65536 + 1,
      disk::DiskDevicePolicy{65536,
                             disk::DiskAccessMode::read_write,
                             disk::DiskFsyncPolicy::after_mutation,
                             disk::DiskChecksumPolicy::require_valid,
                             disk::UnknownPagePolicy::reject_all,
                             true,
                             false});
  const auto close = device.Close();
  const bool ok =
      Require(!refused.ok(), "huge offset write was accepted") &&
      Require(refused.diagnostic.diagnostic_code ==
                  "SB-STORAGE-DISK-EXTENT-OVERFLOW",
              "huge offset write diagnostic mismatch") &&
      Require(!header_overflow.ok(), "overflowing page header read was accepted") &&
      Require(header_overflow.diagnostic.diagnostic_code ==
                  "SB-STORAGE-DISK-PAGE-OFFSET-OVERFLOW",
              "page header overflow diagnostic mismatch") &&
      Require(close.ok(), "huge-offset fixture close failed");
  RemoveDeviceArtifacts(path);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(__linux__)
  if (argc == 5 && std::string_view(argv[1]) == "--owner-probe") {
    return OwnerProbe(argv[2], argv[3], argv[4]);
  }
#endif
  if (argc != 1) return EXIT_FAILURE;
  bool ok = true;
  ok = CheckedPageOffsetArithmetic() && ok;
  ok = CheckedFileExtentArithmetic() && ok;
  ok = FileDeviceDurableCreateSyncCloseAndReadOnlyRefusal() && ok;
  ok = FileDeviceHugeOffsetRefusalPrecedesSeek() && ok;
#if defined(__linux__)
  ok = FileDeviceProcessOwnership(std::filesystem::canonical(argv[0]).string()) && ok;
#endif
  std::error_code cleanup_error;
  const bool removed = std::filesystem::remove(TempRoot(), cleanup_error);
  ok = Require(removed && !cleanup_error, "test temporary directory cleanup failed") && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
