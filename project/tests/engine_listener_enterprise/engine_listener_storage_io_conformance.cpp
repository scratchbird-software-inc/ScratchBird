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
      !Require(!std::filesystem::exists(path.string() + ".sb.owner.lock"),
               "exclusive owner lock was not cleaned up on close")) {
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

int main() {
  bool ok = true;
  ok = CheckedPageOffsetArithmetic() && ok;
  ok = CheckedFileExtentArithmetic() && ok;
  ok = FileDeviceDurableCreateSyncCloseAndReadOnlyRefusal() && ok;
  ok = FileDeviceHugeOffsetRefusalPrecedesSeek() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
