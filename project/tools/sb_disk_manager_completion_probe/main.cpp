// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-DISK-MANAGER-COMPLETION-PROBE-ANCHOR
#include "disk_device.hpp"
#include "page_header.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::storage::disk::CheckDiskDeviceHealth;
using scratchbird::storage::disk::DiskAccessMode;
using scratchbird::storage::disk::DiskDevicePolicy;
using scratchbird::storage::disk::DiskFsyncPolicy;
using scratchbird::storage::disk::FileDevice;
using scratchbird::storage::disk::FileOpenMode;
using scratchbird::storage::disk::PageClassificationKind;
using scratchbird::storage::disk::PageHeader;
using scratchbird::storage::disk::PageSizeProfile;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::disk::ReadDevicePageHeader;
using scratchbird::storage::disk::SerializePageHeader;
using scratchbird::storage::disk::SyncFileDeviceWithPolicy;
using scratchbird::storage::disk::UnknownPagePolicy;

TypedUuid Generate(UuidKind kind, u64 millis) {
  const auto generated = GenerateEngineIdentityV7(kind, millis);
  if (!generated.ok()) {
    return {};
  }
  return generated.value;
}

PageHeader Header(PageType page_type,
                  u64 page_number,
                  u64 flags,
                  const TypedUuid& database_uuid,
                  const TypedUuid& filespace_uuid,
                  const TypedUuid& page_uuid,
                  u32 page_size) {
  PageHeader header;
  header.page_size = page_size;
  header.page_type = page_type;
  header.database_uuid = database_uuid.value;
  header.filespace_uuid = filespace_uuid.value;
  header.page_uuid = page_uuid.value;
  header.page_number = page_number;
  header.page_generation = 1;
  header.flags = flags;
  return header;
}

bool WriteWholePage(FileDevice* device, const PageHeader& header, u32 page_size) {
  const auto serialized = SerializePageHeader(header);
  if (!serialized.ok()) {
    std::cerr << serialized.diagnostic.diagnostic_code << "\n";
    return false;
  }
  std::vector<byte> page(page_size, 0);
  std::memcpy(page.data(), serialized.serialized.data(), serialized.serialized.size());
  const auto write = device->WriteAt(static_cast<u64>(page_size) * header.page_number,
                                    page.data(),
                                    page.size());
  if (!write.ok()) {
    std::cerr << write.diagnostic.diagnostic_code << "\n";
    return false;
  }
  return true;
}

void PrintCheck(const char* name, bool passed) {
  std::cout << name << "=" << (passed ? "true" : "false") << "\n";
}

}  // namespace

int main() {
  constexpr u32 page_size = static_cast<u32>(PageSizeProfile::profile_4k);
  const std::string path = "/tmp/sb_disk_manager_completion_probe.sbdb";
  const std::string misaligned_path = "/tmp/sb_disk_manager_completion_probe_misaligned.sbdb";
  const u64 base_millis = 1770000000000ull;

  const TypedUuid database_uuid = Generate(UuidKind::database, base_millis);
  const TypedUuid filespace_uuid = Generate(UuidKind::filespace, base_millis + 1);
  const TypedUuid page0_uuid = Generate(UuidKind::page, base_millis + 2);
  const TypedUuid page1_uuid = Generate(UuidKind::page, base_millis + 3);
  const TypedUuid page2_uuid = Generate(UuidKind::page, base_millis + 4);
  if (!database_uuid.valid() || !filespace_uuid.valid() || !page0_uuid.valid() ||
      !page1_uuid.valid() || !page2_uuid.valid()) {
    std::cerr << "uuid_generation_failed\n";
    return 2;
  }

  DiskDevicePolicy read_write_policy;
  read_write_policy.page_size = page_size;
  read_write_policy.access_mode = DiskAccessMode::read_write;
  read_write_policy.fsync_policy = DiskFsyncPolicy::after_mutation;
  read_write_policy.unknown_page_policy = UnknownPagePolicy::reject_all;

  FileDevice device;
  bool create_open_ok = device.Open(path, FileOpenMode::create_or_truncate).ok();
  bool write_local_page_ok = create_open_ok &&
      WriteWholePage(&device,
                     Header(PageType::row_data, 0, 0, database_uuid, filespace_uuid, page0_uuid, page_size),
                     page_size);
  bool write_unknown_page_ok = write_local_page_ok &&
      WriteWholePage(&device,
                     Header(static_cast<PageType>(424242), 1, 0, database_uuid, filespace_uuid, page1_uuid, page_size),
                     page_size);
  bool write_unknown_safe_page_ok = write_unknown_page_ok &&
      WriteWholePage(&device,
                     Header(static_cast<PageType>(424243),
                            2,
                            scratchbird::storage::disk::PageHeaderFlag::unknown_safe_read_only,
                            database_uuid,
                            filespace_uuid,
                            page2_uuid,
                            page_size),
                     page_size);
  const auto sync = SyncFileDeviceWithPolicy(&device, read_write_policy);
  const auto health = CheckDiskDeviceHealth(device, read_write_policy);
  const auto local_page = ReadDevicePageHeader(&device, page_size, 0, read_write_policy);
  const auto rejected_unknown = ReadDevicePageHeader(&device, page_size, 1, read_write_policy);

  DiskDevicePolicy safe_unknown_policy = read_write_policy;
  safe_unknown_policy.access_mode = DiskAccessMode::read_only;
  safe_unknown_policy.unknown_page_policy = UnknownPagePolicy::allow_unknown_safe_read_only;
  const auto allowed_unknown_safe = ReadDevicePageHeader(&device, page_size, 2, safe_unknown_policy);
  const bool close_ok = device.Close().ok();

  FileDevice read_only_device;
  const bool read_only_open_ok = read_only_device.Open(path, FileOpenMode::open_existing_read_only).ok();
  const std::vector<byte> one_byte(1, 0);
  const auto read_only_write = read_only_device.WriteAt(0, one_byte.data(), one_byte.size());
  const bool read_only_close_ok = read_only_device.Close().ok();

  FileDevice misaligned;
  const bool misaligned_open_ok = misaligned.Open(misaligned_path, FileOpenMode::create_or_truncate).ok();
  const auto misaligned_write = misaligned.WriteAt(0, one_byte.data(), one_byte.size());
  const auto misaligned_sync = misaligned.Sync();
  const auto misaligned_health = CheckDiskDeviceHealth(misaligned, read_write_policy);
  const bool misaligned_close_ok = misaligned.Close().ok();

  const bool disk_health_ok = health.ok() && health.snapshot.size_aligned && health.snapshot.can_read && health.snapshot.can_write;
  const bool local_page_ok = local_page.ok() && local_page.classification.kind == PageClassificationKind::supported_local;
  const bool unknown_rejected_ok = !rejected_unknown.ok() &&
                                   rejected_unknown.diagnostic.diagnostic_code == "SB-STORAGE-DISK-PAGE-POLICY-REJECTED";
  const bool unknown_safe_ok = allowed_unknown_safe.ok() &&
                               allowed_unknown_safe.classification.kind == PageClassificationKind::unknown_safe;
  const bool read_only_reject_ok = read_only_open_ok && !read_only_write.ok() &&
                                   read_only_write.diagnostic.diagnostic_code == "SB-STORAGE-DISK-WRITE-READ-ONLY";
  const bool size_alignment_ok = misaligned_open_ok && misaligned_write.ok() && misaligned_sync.ok() && !misaligned_health.ok() &&
                                 misaligned_health.diagnostic.diagnostic_code == "SB-STORAGE-DISK-HEALTH-SIZE-UNALIGNED";

  PrintCheck("create_open_ok", create_open_ok);
  PrintCheck("write_local_page_ok", write_local_page_ok);
  PrintCheck("write_unknown_page_ok", write_unknown_page_ok);
  PrintCheck("write_unknown_safe_page_ok", write_unknown_safe_page_ok);
  PrintCheck("sync_ok", sync.ok());
  PrintCheck("disk_health_ok", disk_health_ok);
  PrintCheck("local_page_ok", local_page_ok);
  PrintCheck("unknown_rejected_ok", unknown_rejected_ok);
  PrintCheck("unknown_safe_ok", unknown_safe_ok);
  PrintCheck("close_ok", close_ok);
  PrintCheck("read_only_reject_ok", read_only_reject_ok);
  PrintCheck("read_only_close_ok", read_only_close_ok);
  PrintCheck("misaligned_sync_ok", misaligned_sync.ok());
  PrintCheck("size_alignment_ok", size_alignment_ok);
  PrintCheck("misaligned_close_ok", misaligned_close_ok);

  return create_open_ok && write_local_page_ok && write_unknown_page_ok &&
                 write_unknown_safe_page_ok && sync.ok() && disk_health_ok &&
                 local_page_ok && unknown_rejected_ok && unknown_safe_ok && close_ok &&
                 read_only_reject_ok && read_only_close_ok && size_alignment_ok &&
                 misaligned_sync.ok() && misaligned_close_ok
             ? 0
             : 1;
}
