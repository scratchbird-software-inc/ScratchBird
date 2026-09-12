// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_header.hpp"
#include "server/database_ownership.hpp"
#include "storage/disk/disk_device.hpp"
#include "uuid.hpp"
#include <chrono>
#include <iostream>
#include <string>

namespace fs = scratchbird::storage::filespace;
namespace disk = scratchbird::storage::disk;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
namespace platform = scratchbird::core::platform;

bool ReceiveRelease() {
  std::string line;
  return std::getline(std::cin, line) && line == "release";
}

bool Valid(const fs::PhysicalFilespaceHeaderResult& result) {
  return result.ok() && result.file_size_matches_capacity &&
      result.file_size_bytes == 24576 && result.expected_capacity_bytes == 24576 &&
      result.header.page_size == 8192 && result.header.total_pages == 3 &&
      result.header.free_pages == 1 && result.header.preallocated_pages == 1 &&
      result.header.physical_filespace_id == 2 && result.header.header_generation == 7 &&
      uuid::IsEngineIdentityUuid(result.header.database_uuid.value) &&
      uuid::IsEngineIdentityUuid(result.header.filespace_uuid.value);
}

int Read(const std::string& mode, const std::string& path) {
  const auto result = mode == "offline" ? fs::ReadPhysicalFilespaceHeaderOffline(path)
                                         : fs::ReadPhysicalFilespaceHeader(path);
  if (Valid(result)) return 0;
  const auto& code = result.diagnostic.diagnostic_code;
  std::cerr << code << '\n';
  return code == "SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD" ||
      code == "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD" ||
      code == "SB-STORAGE-DISK-OWNER-LOCK-HELD" ? 2 : 3;
}

int main(int argc, char** argv) {
  if (argc != 4) return 64;
  const std::string mode = argv[1], path = argv[2], argument = argv[3];
  if (mode == "create") {
    const auto now = static_cast<platform::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto database = uuid::GenerateEngineIdentityV7(platform::UuidKind::database, now);
    const auto filespace = uuid::GenerateEngineIdentityV7(platform::UuidKind::filespace, now);
    const auto writer = uuid::GenerateEngineIdentityV7(platform::UuidKind::object, now);
    if (!database.ok() || !filespace.ok() || !writer.ok()) return 3;
    fs::PhysicalFilespaceHeader header;
    header.database_uuid = database.value;
    header.filespace_uuid = filespace.value;
    header.writer_identity_uuid = writer.value;
    header.state = fs::FilespaceState::online;
    header.page_size = 8192;
    header.physical_filespace_id = 2;
    header.total_pages = 3;
    header.free_pages = 1;
    header.preallocated_pages = 1;
    header.allocation_root_page = 1;
    header.header_generation = 7;
    const auto created = fs::CreatePhysicalFilespaceFile(path, header);
    const auto read = fs::ReadPhysicalFilespaceHeader(path);
    return created.ok() && Valid(read) &&
        read.header.database_uuid.value == header.database_uuid.value &&
        read.header.filespace_uuid.value == header.filespace_uuid.value &&
        read.header.writer_identity_uuid.value == header.writer_identity_uuid.value ? 0 : 3;
  }
  if (mode == "offline" || mode == "online") return Read(mode, path);
  if (mode == "server" || mode == "internal") {
    server::DatabaseOwnershipRequest request;
    request.database_path = path;
    auto owner = server::AcquireDatabaseOwnership(request);
    if (!owner.acquired) return 3;
    // Same-process maintenance borrows the genuine native owner, never PID text.
    if (mode == "internal") return Read("offline", argument);
    std::cout << "held" << std::endl;
    return ReceiveRelease() ? 0 : 64;
  }
  if (mode == "read" || mode == "write") {
    disk::FileDevice file;
    const auto opened = file.Open(path, mode == "read"
        ? disk::FileOpenMode::open_existing_read_only : disk::FileOpenMode::open_existing);
    if (!opened.ok()) return 3;
    std::cout << "held" << std::endl;
    return ReceiveRelease() && file.Close().ok() ? 0 : 3;
  }
  return 64;
}
