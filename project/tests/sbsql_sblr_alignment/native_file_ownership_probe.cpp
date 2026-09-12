// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/database_ownership.hpp"
#include "storage/disk/disk_device.hpp"
#include <iostream>
#include <string>
#include <vector>

namespace server = scratchbird::server;
namespace disk = scratchbird::storage::disk;

bool Receive(const char* expected) {
  std::string line;
  return std::getline(std::cin, line) && line == expected;
}

server::DatabaseOwnershipResult Own(const std::string& path) {
  server::DatabaseOwnershipRequest request;
  request.database_path = path;
  return server::AcquireDatabaseOwnership(request);
}

int Refusal(const disk::IoResult& result) {
  const auto& code = result.diagnostic.diagnostic_code;
  std::cerr << code << '\n';
  return code == "SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD" ||
      code == "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD" ||
      code == "SB-STORAGE-DISK-OWNER-LOCK-HELD" ? 2 : 3;
}

bool ReadRealByte(disk::FileDevice& file) {
  char byte = 0;
  const auto result = file.ReadAt(0, &byte, 1);
  return result.ok() && result.bytes_transferred == 1 && byte == 'Z';
}

int main(int argc, char** argv) {
  if (argc != 4) return 64;
  const std::string mode = argv[1], path = argv[2], argument = argv[3];
  if (argument == "race") {
    std::cout << "ready" << std::endl;
    if (!Receive("go")) return 64;
  }
  const bool hold = argument == "hold" || argument == "race";
  if (mode == "server") {
    auto owner = Own(path);
    if (!owner.acquired) {
      std::cerr << owner.diagnostic_code << ':' << owner.diagnostic_detail << '\n';
      std::cout << "refused" << std::endl;
      return owner.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER" ? 2 : 3;
    }
    if (hold) {
      std::cout << "held" << std::endl;
      if (!Receive("release")) return 64;
    }
    return 0;
  }
  if (mode == "borrow" || mode == "reserve-create" || mode == "retarget" ||
      mode == "truncate-owned") {
    auto owner = Own(path);
    if (!owner.acquired) return 3;
    if (mode == "retarget") {
      std::cout << "held" << std::endl;
      if (!Receive("swapped")) return 64;
      for (auto open_mode : {disk::FileOpenMode::open_existing_read_only,
                             disk::FileOpenMode::create_or_truncate}) {
        disk::FileDevice replacement;
        const auto result = replacement.Open(path, open_mode);
        if (result.ok() || result.diagnostic.diagnostic_code !=
                              "SB-STORAGE-DISK-DATA-OWNER-LOCK-FAILED") return 3;
      }
      std::cout << "checked" << std::endl;
      return Receive("release") ? 0 : 64;
    }
    if (mode == "truncate-owned") {
      disk::FileDevice file;
      if (!file.Open(path, disk::FileOpenMode::create_or_truncate).ok()) return 3;
      const auto size = file.Size();
      return size.ok() && size.size_bytes == 0 && file.Close().ok() ? 0 : 3;
    }
    if (mode == "reserve-create") {
      disk::FileDevice creator;
      const std::vector<char> bytes(4096, 'Z');
      if (!creator.Open(path, disk::FileOpenMode::create_new).ok()) return 3;
      const auto written = creator.WriteAt(0, bytes.data(), bytes.size());
      if (!written.ok() || written.bytes_transferred != bytes.size() ||
          !creator.Sync().ok() || !creator.Close().ok()) return 3;
      std::filesystem::create_hard_link(path, argument);
    }
    disk::FileDevice reader;
    if (!reader.Open(argument, disk::FileOpenMode::open_existing_read_only).ok()) return 3;
    owner.lock->release();
    if (!ReadRealByte(reader)) return 3;
    disk::FileDevice late;
    const auto late_open = late.Open(path, disk::FileOpenMode::open_existing_read_only);
    if (late_open.ok() || Refusal(late_open) != 2) return 3;
    std::cout << "held" << std::endl;
    if (!Receive("release") || !ReadRealByte(reader)) return 3;
    return reader.Close().ok() ? 0 : 3;
  }
  disk::FileOpenMode open_mode;
  if (mode == "read") open_mode = disk::FileOpenMode::open_existing_read_only;
  else if (mode == "write") open_mode = disk::FileOpenMode::open_existing;
  else if (mode == "truncate") open_mode = disk::FileOpenMode::create_or_truncate;
  else return 64;
  disk::FileDevice file;
  const auto opened = file.Open(path, open_mode);
  if (!opened.ok()) {
    std::cout << "refused" << std::endl;
    return Refusal(opened);
  }
  if (mode == "truncate") {
    const auto size = file.Size();
    if (!size.ok() || size.size_bytes != 0) return 3;
  } else if (!ReadRealByte(file)) return 3;
  if (hold) {
    std::cout << "held" << std::endl;
    if (!Receive("release")) return 64;
  }
  return file.Close().ok() ? 0 : 3;
}
