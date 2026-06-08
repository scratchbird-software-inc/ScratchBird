// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-FILESPACE-IDENTITY-ANCHOR
#include "filespace_lifecycle.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::filespace {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u16 kActivePrimaryPhysicalFilespaceId = 0;
inline constexpr u16 kReservedPhysicalFilespaceId = 1;
inline constexpr u64 kFilespacePageNumberBits = 48;
inline constexpr u64 kMaxFilespacePageNumber = (1ull << kFilespacePageNumberBits) - 1ull;

struct FilespacePageId {
  u16 physical_filespace_id = kActivePrimaryPhysicalFilespaceId;
  u64 page_number = 0;
};

struct FilespacePageIdResult {
  Status status;
  FilespacePageId page_id;
  u64 encoded = 0;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct FilespaceDirectoryEntry {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  u16 physical_filespace_id = kActivePrimaryPhysicalFilespaceId;
  FilespaceRole role = FilespaceRole::secondary_data;
  FilespaceState state = FilespaceState::absent;
  std::string path;
  u32 page_size = 0;
  u64 generation = 0;
  bool startup_authority = false;
  bool active = false;
  bool read_only = false;
};

struct FilespaceDirectory {
  std::vector<FilespaceDirectoryEntry> entries;
  u64 generation = 1;
};

struct FilespaceDirectoryResult {
  Status status;
  DiagnosticRecord diagnostic;
  u16 physical_filespace_id = kActivePrimaryPhysicalFilespaceId;
  FilespaceDirectoryEntry entry;

  bool ok() const { return status.ok(); }
};

bool IsReservedPhysicalFilespaceId(u16 physical_filespace_id);
bool IsValidPhysicalFilespaceId(u16 physical_filespace_id);
bool IsValidFilespacePageNumber(u64 page_number);
u64 EncodeFilespacePageIdUnchecked(FilespacePageId page_id);
FilespacePageIdResult MakeFilespacePageId(u16 physical_filespace_id, u64 page_number);
FilespacePageIdResult DecodeFilespacePageId(u64 encoded_page_id);
FilespacePageIdResult MakePrimaryFilespacePageId(u64 page_number);
std::string FilespacePageIdToString(FilespacePageId page_id);

FilespaceDirectoryResult AddFilespaceDirectoryEntry(FilespaceDirectory* directory,
                                                    FilespaceDirectoryEntry entry);
FilespaceDirectoryResult FindFilespaceByPhysicalId(const FilespaceDirectory& directory,
                                                   u16 physical_filespace_id);
FilespaceDirectoryResult FindFilespaceByUuid(const FilespaceDirectory& directory,
                                             const TypedUuid& filespace_uuid);
FilespaceDirectoryResult AllocateNextSecondaryPhysicalFilespaceId(const FilespaceDirectory& directory);
FilespaceDirectoryResult ValidateStartupFilespaceDirectory(const FilespaceDirectory& directory);
DiagnosticRecord MakeFilespaceIdentityDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::storage::filespace
