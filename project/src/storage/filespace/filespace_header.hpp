// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PHYSICAL-FILESPACE-HEADER-ANCHOR
#include "filespace_lifecycle.hpp"

namespace scratchbird::storage::filespace {

struct PhysicalFilespaceHeader {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  FilespaceRole role = FilespaceRole::secondary_data;
  FilespaceState state = FilespaceState::initializing;
  u32 page_size = static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  u32 format_version = 1;
  u32 checksum_profile = 1;
  u32 encryption_profile = 0;
  u64 compatibility_flags = 0;
  u64 attach_policy_flags = 0;
  u16 physical_filespace_id = 0;
  u64 total_pages = 0;
  u64 free_pages = 0;
  u64 preallocated_pages = 0;
  u64 allocation_root_page = 0;
  u64 header_generation = 1;
  TypedUuid writer_identity_uuid;
  std::string creation_operation_uuid;
};

struct PhysicalFilespaceHeaderResult {
  Status status;
  PhysicalFilespaceHeader header;
  u64 file_size_bytes = 0;
  u64 expected_capacity_bytes = 0;
  bool file_size_matches_capacity = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct PhysicalFilespaceWriteResult {
  Status status;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct PhysicalFilespaceCapacityGrowthResult {
  Status status;
  PhysicalFilespaceHeader header_before;
  PhysicalFilespaceHeader header_after;
  u64 file_size_before_bytes = 0;
  u64 file_size_after_bytes = 0;
  u64 expected_capacity_after_bytes = 0;
  u64 extent_preallocation_offset_bytes = 0;
  u64 extent_preallocation_bytes = 0;
  std::string extent_preallocation_strategy;
  std::string extent_preallocation_fallback_reason;
  bool extent_preallocation_attempted = false;
  bool extent_preallocation_succeeded = false;
  bool extent_preallocation_fallback_used = false;
  bool physical_extension_completed = false;
  bool physical_extension_synced = false;
  bool header_updated = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

PhysicalFilespaceWriteResult CreatePhysicalFilespaceFile(const std::string& path,
                                                         const PhysicalFilespaceHeader& header,
                                                         bool allow_overwrite = false);
PhysicalFilespaceHeaderResult ReadPhysicalFilespaceHeader(const std::string& path);
PhysicalFilespaceHeaderResult ReadPhysicalFilespaceHeaderOffline(const std::string& path);
PhysicalFilespaceHeaderResult ValidatePhysicalFilespaceHeader(const PhysicalFilespaceHeader& expected,
                                                              const PhysicalFilespaceHeader& actual);
PhysicalFilespaceHeaderResult ValidatePhysicalFilespaceHeader(const PhysicalFilespaceHeader& expected,
                                                              const PhysicalFilespaceHeader& actual,
                                                              u64 file_size_bytes);
PhysicalFilespaceWriteResult WritePhysicalFilespaceHeader(const std::string& path,
                                                          const PhysicalFilespaceHeader& header,
                                                          bool allow_overwrite = false);
PhysicalFilespaceCapacityGrowthResult ExtendPhysicalFilespaceCapacity(
    const std::string& path,
    u64 expected_total_pages_before,
    u64 expected_preallocated_pages_before,
    u64 growth_pages,
    bool reserve_growth_as_preallocated,
    u64 header_generation_after = 0);
DiagnosticRecord MakePhysicalFilespaceHeaderDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail = {});

}  // namespace scratchbird::storage::filespace
