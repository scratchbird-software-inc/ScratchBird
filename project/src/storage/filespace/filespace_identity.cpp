// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-FILESPACE-IDENTITY-ANCHOR
#include "filespace_identity.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::storage::filespace {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status IdentityOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status IdentityErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

FilespacePageIdResult PageIdError(std::string code, std::string key, std::string detail = {}) {
  FilespacePageIdResult result;
  result.status = IdentityErrorStatus();
  result.diagnostic = MakeFilespaceIdentityDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

FilespaceDirectoryResult DirectoryError(std::string code, std::string key, std::string detail = {}) {
  FilespaceDirectoryResult result;
  result.status = IdentityErrorStatus();
  result.diagnostic = MakeFilespaceIdentityDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.valid() && right.valid() && left.kind == right.kind && left.value.bytes == right.value.bytes;
}

}  // namespace

bool IsReservedPhysicalFilespaceId(u16 physical_filespace_id) {
  return physical_filespace_id == kReservedPhysicalFilespaceId;
}

bool IsValidPhysicalFilespaceId(u16 physical_filespace_id) {
  return !IsReservedPhysicalFilespaceId(physical_filespace_id);
}

bool IsValidFilespacePageNumber(u64 page_number) {
  return page_number <= kMaxFilespacePageNumber;
}

u64 EncodeFilespacePageIdUnchecked(FilespacePageId page_id) {
  return (static_cast<u64>(page_id.physical_filespace_id) << kFilespacePageNumberBits) |
         (page_id.page_number & kMaxFilespacePageNumber);
}

FilespacePageIdResult MakeFilespacePageId(u16 physical_filespace_id, u64 page_number) {
  if (!IsValidPhysicalFilespaceId(physical_filespace_id)) {
    return PageIdError("SB-FILESPACE-PAGE-ID-PHYSICAL-ID-RESERVED",
                       "storage.filespace.identity.physical_id_reserved",
                       "physical filespace id 1 is reserved and cannot identify a live filespace");
  }
  if (!IsValidFilespacePageNumber(page_number)) {
    return PageIdError("SB-FILESPACE-PAGE-ID-PAGE-NUMBER-OUT-OF-RANGE",
                       "storage.filespace.identity.page_number_out_of_range",
                       "filespace page number exceeds 48-bit page-number field");
  }
  FilespacePageIdResult result;
  result.status = IdentityOkStatus();
  result.page_id = FilespacePageId{physical_filespace_id, page_number};
  result.encoded = EncodeFilespacePageIdUnchecked(result.page_id);
  return result;
}

FilespacePageIdResult DecodeFilespacePageId(u64 encoded_page_id) {
  const auto physical_id = static_cast<u16>(encoded_page_id >> kFilespacePageNumberBits);
  const auto page_number = encoded_page_id & kMaxFilespacePageNumber;
  auto result = MakeFilespacePageId(physical_id, page_number);
  if (result.ok()) {
    result.encoded = encoded_page_id;
  }
  return result;
}

FilespacePageIdResult MakePrimaryFilespacePageId(u64 page_number) {
  return MakeFilespacePageId(kActivePrimaryPhysicalFilespaceId, page_number);
}

std::string FilespacePageIdToString(FilespacePageId page_id) {
  std::ostringstream stream;
  stream << page_id.physical_filespace_id << ':' << page_id.page_number;
  return stream.str();
}

FilespaceDirectoryResult AddFilespaceDirectoryEntry(FilespaceDirectory* directory,
                                                    FilespaceDirectoryEntry entry) {
  if (directory == nullptr) {
    return DirectoryError("SB-FILESPACE-DIRECTORY-MISSING",
                          "storage.filespace.identity.directory_missing",
                          "filespace directory is required");
  }
  if (!entry.database_uuid.valid() || !entry.filespace_uuid.valid() || entry.path.empty() || entry.page_size == 0) {
    return DirectoryError("SB-FILESPACE-DIRECTORY-ENTRY-INVALID",
                          "storage.filespace.identity.directory_entry_invalid",
                          "directory entry requires database UUID, filespace UUID, path, and page size");
  }
  if (!IsValidPhysicalFilespaceId(entry.physical_filespace_id)) {
    return DirectoryError("SB-FILESPACE-DIRECTORY-PHYSICAL-ID-RESERVED",
                          "storage.filespace.identity.directory_physical_id_reserved",
                          "physical filespace id is reserved");
  }
  for (const auto& existing : directory->entries) {
    if (existing.physical_filespace_id == entry.physical_filespace_id ||
        SameUuid(existing.filespace_uuid, entry.filespace_uuid)) {
      return DirectoryError("SB-FILESPACE-DIRECTORY-DUPLICATE-IDENTITY",
                            "storage.filespace.identity.directory_duplicate_identity",
                            "physical filespace id and filespace UUID must be unique");
    }
  }
  directory->entries.push_back(std::move(entry));
  ++directory->generation;
  FilespaceDirectoryResult result;
  result.status = IdentityOkStatus();
  result.entry = directory->entries.back();
  result.physical_filespace_id = result.entry.physical_filespace_id;
  return result;
}

FilespaceDirectoryResult FindFilespaceByPhysicalId(const FilespaceDirectory& directory,
                                                   u16 physical_filespace_id) {
  for (const auto& entry : directory.entries) {
    if (entry.physical_filespace_id == physical_filespace_id) {
      FilespaceDirectoryResult result;
      result.status = IdentityOkStatus();
      result.entry = entry;
      result.physical_filespace_id = physical_filespace_id;
      return result;
    }
  }
  return DirectoryError("SB-FILESPACE-DIRECTORY-PHYSICAL-ID-NOT-FOUND",
                        "storage.filespace.identity.directory_physical_id_not_found",
                        "physical filespace id is not present in the startup directory");
}

FilespaceDirectoryResult FindFilespaceByUuid(const FilespaceDirectory& directory,
                                             const TypedUuid& filespace_uuid) {
  for (const auto& entry : directory.entries) {
    if (SameUuid(entry.filespace_uuid, filespace_uuid)) {
      FilespaceDirectoryResult result;
      result.status = IdentityOkStatus();
      result.entry = entry;
      result.physical_filespace_id = entry.physical_filespace_id;
      return result;
    }
  }
  return DirectoryError("SB-FILESPACE-DIRECTORY-UUID-NOT-FOUND",
                        "storage.filespace.identity.directory_uuid_not_found",
                        "filespace UUID is not present in the startup directory");
}

FilespaceDirectoryResult AllocateNextSecondaryPhysicalFilespaceId(const FilespaceDirectory& directory) {
  for (u16 candidate = 2; candidate != 0; ++candidate) {
    if (std::none_of(directory.entries.begin(), directory.entries.end(), [candidate](const auto& entry) {
          return entry.physical_filespace_id == candidate;
        })) {
      FilespaceDirectoryResult result;
      result.status = IdentityOkStatus();
      result.physical_filespace_id = candidate;
      return result;
    }
  }
  return DirectoryError("SB-FILESPACE-DIRECTORY-PHYSICAL-ID-EXHAUSTED",
                        "storage.filespace.identity.directory_physical_id_exhausted",
                        "no secondary physical filespace id is available");
}

FilespaceDirectoryResult ValidateStartupFilespaceDirectory(const FilespaceDirectory& directory) {
  bool active_primary_seen = false;
  for (const auto& entry : directory.entries) {
    if (!entry.database_uuid.valid() || !entry.filespace_uuid.valid() || entry.path.empty() || entry.page_size == 0) {
      return DirectoryError("SB-FILESPACE-DIRECTORY-ENTRY-INVALID",
                            "storage.filespace.identity.directory_entry_invalid",
                            "startup directory contains an invalid filespace entry");
    }
    if (!IsValidPhysicalFilespaceId(entry.physical_filespace_id)) {
      return DirectoryError("SB-FILESPACE-DIRECTORY-PHYSICAL-ID-RESERVED",
                            "storage.filespace.identity.directory_physical_id_reserved",
                            "startup directory contains reserved physical filespace id");
    }
    if (entry.physical_filespace_id == kActivePrimaryPhysicalFilespaceId && entry.active) {
      active_primary_seen = true;
    }
  }
  for (std::size_t left = 0; left < directory.entries.size(); ++left) {
    for (std::size_t right = left + 1; right < directory.entries.size(); ++right) {
      if (directory.entries[left].physical_filespace_id == directory.entries[right].physical_filespace_id ||
          SameUuid(directory.entries[left].filespace_uuid, directory.entries[right].filespace_uuid)) {
        return DirectoryError("SB-FILESPACE-DIRECTORY-DUPLICATE-IDENTITY",
                              "storage.filespace.identity.directory_duplicate_identity",
                              "startup directory has duplicate filespace identity");
      }
    }
  }
  if (!active_primary_seen) {
    return DirectoryError("SB-FILESPACE-DIRECTORY-ACTIVE-PRIMARY-MISSING",
                          "storage.filespace.identity.directory_active_primary_missing",
                          "startup directory requires active physical filespace id 0");
  }
  FilespaceDirectoryResult result;
  result.status = IdentityOkStatus();
  return result;
}

DiagnosticRecord MakeFilespaceIdentityDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> args;
  args.push_back({"detail", std::move(detail)});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(args),
                        {},
                        "storage.filespace.identity");
}

}  // namespace scratchbird::storage::filespace
