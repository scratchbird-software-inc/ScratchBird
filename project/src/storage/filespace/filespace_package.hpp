// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-FILESPACE-PACKAGE-ANCHOR
#include "filespace_lifecycle.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace scratchbird::storage::filespace {

enum class FilespacePackageAction : u16 {
  export_manifest,
  inspect_manifest,
  import_to_quarantine,
  admit,
  reject
};

struct FilespacePackageMember {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string path;
  FilespaceRole role = FilespaceRole::unknown;
  FilespaceState state = FilespaceState::absent;
  u32 page_size = static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  u16 physical_filespace_id = 0;
  u64 header_generation = 0;
  TypedUuid writer_identity_uuid;
  std::string member_checksum;
};

struct FilespacePackageManifest {
  TypedUuid package_uuid;
  TypedUuid source_database_uuid;
  std::string package_name;
  u32 format_version = 1;
  std::vector<FilespacePackageMember> members;
  std::string manifest_checksum;
  bool root_authority_present = false;
  bool encrypted_material_included = false;
};

struct FilespacePackageRequest {
  TypedUuid package_uuid;
  TypedUuid database_uuid;
  TypedUuid target_database_uuid;
  std::string package_name;
  std::vector<FilespaceDescriptor> descriptors;
  FilespacePackageManifest manifest;
  std::string operator_identity;
  bool inspection_passed = false;
  bool admission_authorized = false;
  bool reject_authorized = false;
};

struct FilespacePackageEvent {
  u64 sequence = 0;
  FilespacePackageAction action = FilespacePackageAction::inspect_manifest;
  TypedUuid package_uuid;
  TypedUuid filespace_uuid;
  FilespaceState previous_state = FilespaceState::absent;
  FilespaceState new_state = FilespaceState::absent;
  std::string diagnostic_code;
  bool durable_state_changed = false;
};

struct FilespacePackageResult {
  Status status;
  DiagnosticRecord diagnostic;
  FilespacePackageManifest manifest;
  std::vector<FilespacePackageEvent> events;
  u64 staged_count = 0;
  u64 admitted_count = 0;
  u64 rejected_count = 0;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;

  bool ok() const { return status.ok(); }
};

struct FilespacePackageFileWriteRequest {
  std::filesystem::path path;
  FilespacePackageManifest manifest;
  bool allow_overwrite = false;
  bool execute_physical_package_transfer = false;
  bool allow_physical_package_transfer = false;
};

struct FilespacePackageFileReadRequest {
  std::filesystem::path path;
  std::filesystem::path physical_output_directory;
  bool execute_physical_package_transfer = false;
  bool allow_physical_package_transfer = false;
};

struct FilespacePackageFileResult {
  Status status;
  DiagnosticRecord diagnostic;
  FilespacePackageManifest manifest;
  u64 byte_count = 0;
  u64 physical_member_count = 0;
  u64 physical_byte_count = 0;
  bool runtime_package_file_io_executed = false;
  bool physical_package_transfer_executed = false;
  bool encrypted_material_included = false;
  bool durable_state_changed = false;
  bool checksum_verified = false;
  bool file_flushed = false;
  bool filesystem_sync_executed = false;

  bool ok() const { return status.ok(); }
};

const char* FilespacePackageActionName(FilespacePackageAction action);
FilespacePackageResult ExportFilespacePackageManifest(const FilespacePackageRequest& request);
FilespacePackageResult InspectFilespacePackageManifest(const FilespacePackageRequest& request);
FilespacePackageFileResult WriteFilespacePackageFile(
    const FilespacePackageFileWriteRequest& request);
FilespacePackageFileResult ReadFilespacePackageFile(
    const FilespacePackageFileReadRequest& request);
FilespacePackageResult ImportFilespacePackageToQuarantine(
    FilespaceRegistry* registry,
    const FilespacePackageRequest& request);
FilespacePackageResult AdmitFilespacePackage(FilespaceRegistry* registry,
                                             const FilespacePackageRequest& request);
FilespacePackageResult RejectFilespacePackage(FilespaceRegistry* registry,
                                              const FilespacePackageRequest& request);

}  // namespace scratchbird::storage::filespace
