// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "page_header.hpp"
#include "runtime_platform.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace scratchbird::storage::disk {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::usize;

enum class FileOpenMode {
  open_existing,
  open_existing_read_only,
  create_new,
  create_or_truncate
};

enum class DiskAccessMode {
  read_write,
  read_only
};

enum class DiskFsyncPolicy {
  never,
  after_mutation,
  always
};

enum class DiskChecksumPolicy {
  accept_declared,
  require_supported,
  require_valid
};

enum class UnknownPagePolicy {
  reject_all,
  allow_unknown_safe_read_only,
  reject_cluster_pages_until_mapping_available,
  allow_encrypted_opaque_read_only
};

struct DeviceCapabilities {
  bool read_at = true;
  bool write_at = true;
  bool sync = true;
  bool size_query = true;
  bool extent_preallocation = false;
  bool sparse = false;
  bool direct_io = false;
  usize natural_alignment = 1;
};

struct DiskDevicePolicy {
  u32 page_size = 0;
  DiskAccessMode access_mode = DiskAccessMode::read_write;
  DiskFsyncPolicy fsync_policy = DiskFsyncPolicy::after_mutation;
  DiskChecksumPolicy checksum_policy = DiskChecksumPolicy::require_valid;
  UnknownPagePolicy unknown_page_policy = UnknownPagePolicy::reject_all;
  bool require_open_device = true;
  bool require_size_alignment = true;
};

struct DiskHealthSnapshot {
  bool opened = false;
  bool read_only = false;
  bool file_present = false;
  bool size_query_ok = false;
  bool size_aligned = false;
  bool can_read = false;
  bool can_write = false;
  bool can_sync = false;
  u64 size_bytes = 0;
  u32 page_size = 0;
  std::string health = "unknown";
};

struct IoResult {
  Status status;
  usize bytes_transferred = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct CheckedFileExtentResult {
  Status status;
  u64 offset = 0;
  usize bytes = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct SizeResult {
  Status status;
  u64 size_bytes = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct CapabilityResult {
  Status status;
  DeviceCapabilities capabilities;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct PreallocateExtentResult {
  Status status;
  u64 offset = 0;
  u64 bytes = 0;
  std::string strategy;
  std::string fallback_reason;
  bool platform_preallocation_attempted = false;
  bool platform_preallocation_succeeded = false;
  bool fallback_extension_used = false;
  bool logical_size_extended = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() &&
           (platform_preallocation_succeeded || fallback_extension_used || bytes == 0);
  }
};

struct DiskHealthResult {
  Status status;
  DiskHealthSnapshot snapshot;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DiskPageHeaderResult {
  Status status;
  SerializedPageHeader serialized{};
  PageClassification classification;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

class FileDevice {
 public:
  FileDevice();
  FileDevice(const FileDevice&) = delete;
  FileDevice& operator=(const FileDevice&) = delete;
  ~FileDevice();

  IoResult Open(std::string path, FileOpenMode mode);
  IoResult Close();
  IoResult ReadAt(u64 offset, void* buffer, usize bytes);
  IoResult WriteAt(u64 offset, const void* buffer, usize bytes);
  PreallocateExtentResult PreallocateExtent(u64 offset, u64 bytes);
  IoResult Sync();
  void SetMetricContext(std::string database_uuid,
                        std::string filespace_uuid,
                        std::string node_uuid,
                        std::string filespace_role,
                        std::string device_class);
  SizeResult Size() const;
  CapabilityResult Capabilities() const;

  bool is_open() const;
  bool read_only() const;
  const std::string& path() const;

 private:
  IoResult MakeIoError(std::string diagnostic_code,
                       std::string message_key,
                       std::string detail = {},
                       usize bytes_transferred = 0) const;

  std::string path_;
  std::string owner_lock_path_;
  std::string metric_database_uuid_;
  std::string metric_filespace_uuid_;
  std::string metric_node_uuid_;
  std::string metric_filespace_role_;
  std::string metric_device_class_;
  DeviceCapabilities capabilities_;
  bool read_only_ = false;
  bool owner_lock_held_ = false;
  bool owner_lock_exclusive_ = false;
  std::unique_lock<std::recursive_mutex> route_owner_storage_guard_;
#ifdef _WIN32
  void* file_handle_ = nullptr;
  void* owner_lock_handle_ = nullptr;
#else
  int file_fd_ = -1;
  int owner_lock_fd_ = -1;
#endif
};

const char* DiskAccessModeName(DiskAccessMode mode);
const char* DiskFsyncPolicyName(DiskFsyncPolicy policy);
const char* DiskChecksumPolicyName(DiskChecksumPolicy policy);
const char* UnknownPagePolicyName(UnknownPagePolicy policy);
CheckedFileExtentResult CheckFileDeviceExtent(u64 offset, usize bytes);
CheckedFileExtentResult CheckDevicePageOffset(u32 page_size,
                                              u64 page_number,
                                              u64 in_page_offset = 0);
DiskHealthResult CheckDiskDeviceHealth(const FileDevice& device,
                                       const DiskDevicePolicy& policy);
IoResult SyncFileDeviceWithPolicy(FileDevice* device,
                                  const DiskDevicePolicy& policy);
IoResult SyncFilesystemPath(const std::string& path, bool writable);
IoResult SyncParentDirectoryPath(const std::string& path);
DiskPageHeaderResult ReadDevicePageHeader(FileDevice* device,
                                          u32 page_size,
                                          u64 page_number,
                                          const DiskDevicePolicy& policy);
DiagnosticRecord MakeDiskDiagnostic(Status status,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string path,
                                    std::string detail = {});

}  // namespace scratchbird::storage::disk
