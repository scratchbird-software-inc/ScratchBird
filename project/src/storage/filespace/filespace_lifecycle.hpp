// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-FILESPACE-LIFECYCLE-ANCHOR
#include "database_format.hpp"
#include "runtime_platform.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace scratchbird::storage::filespace {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class FilespaceRole : u16 {
  unknown,
  active_primary,
  primary_shadow,
  primary_snapshot,
  primary_candidate,
  secondary_data,
  secondary_index,
  secondary_overflow,
  secondary_history,
  secondary_shard,
  archive_history,
  archive_log,
  archive_detached,
  temporary,
  import_candidate,
  drop_pending,
  forbidden
};

enum class FilespaceState : u16 {
  absent = 0,
  online = 1,
  attached = online,
  read_only = 2,
  detached = 3,
  archived = 4,
  deleted = 5,
  dropped = deleted,
  creating = 6,
  initializing = 7,
  maintenance = 8,
  moving = 9,
  relocating_objects = 10,
  promoting = 11,
  demoting = 12,
  detaching = 13,
  drop_pending = 14,
  quarantine = 15,
  forbidden = 16
};

enum class FilespaceOperation : u16 {
  create_filespace,
  attach_filespace,
  detach_filespace,
  promote_filespace,
  demote_filespace,
  set_read_only,
  set_read_write,
  assign_archive_owner,
  assign_history_owner,
  pin_filespace,
  unpin_filespace,
  verify_filespace,
  compact_filespace,
  truncate_filespace,
  drop_filespace,
  create_snapshot_filespace,
  create_shadow_filespace,
  refresh_snapshot_or_shadow,
  retire_snapshot_or_shadow,
  quarantine_filespace,
  move_filespace,
  delete_physical_filespace,
  merge_filespace,
  repair_filespace,
  rebuild_filespace,
  salvage_filespace
};

enum class FilespacePinKind : u16 {
  page_owner,
  transaction,
  backup,
  archive,
  catalog,
  external
};

enum class FilespaceObjectClass : u16 {
  unspecified,
  hot_row,
  cold_row,
  secondary_delta_ledger,
  exact_index,
  immutable_generation,
  large_blob,
  temp_spill,
  backup_stream
};

enum class FilespaceClass : u16 {
  unknown,
  hot_row,
  cold_row,
  secondary_delta_ledger,
  exact_index,
  immutable_generation,
  large_blob,
  temp_spill,
  backup_stream,
  forbidden
};

struct FilespacePin {
  FilespacePinKind kind = FilespacePinKind::external;
  std::string owner;
  u64 count = 0;
};

struct FilespaceDescriptor {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string path;
  FilespaceRole role = FilespaceRole::unknown;
  FilespaceState state = FilespaceState::absent;
  u32 page_size = static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  u64 generation = 0;
  bool read_only = false;
  bool archive_owner = false;
  bool history_owner = false;
  bool startup_authority = false;
  bool catalog_persistence_owner = false;
  bool filespace_manifest_owner = false;
  bool recovery_evidence_owner = false;
  bool first_filespace = false;
  bool active = false;
  u16 physical_filespace_id = 0;
  u64 total_pages = 0;
  u64 free_pages = 0;
  u64 preallocated_pages = 0;
  u64 allocation_root_page = 0;
  u64 header_generation = 0;
  TypedUuid writer_identity_uuid;
  std::vector<FilespacePin> pins;
};

struct FilespaceEvidenceRecord {
  u64 sequence = 0;
  FilespaceOperation operation = FilespaceOperation::verify_filespace;
  TypedUuid filespace_uuid;
  FilespaceState previous_state = FilespaceState::absent;
  FilespaceState new_state = FilespaceState::absent;
  FilespaceRole previous_role = FilespaceRole::unknown;
  FilespaceRole new_role = FilespaceRole::unknown;
  std::string reason;
  std::string diagnostic_code;
  bool durable_state_changed = false;
};

struct FilespaceLifecyclePolicy {
  bool allow_primary_detach = false;
  bool allow_primary_replacement = false;
  bool allow_promotion = true;
  bool allow_archive_owner_assignment = true;
  bool allow_history_owner_assignment = true;
  bool require_no_active_pins_for_detach = true;
  bool require_no_active_pins_for_promote = true;
  bool require_no_active_pins_for_drop = true;
  bool require_no_active_pins_for_quarantine = true;
  bool require_no_active_pins_for_move = true;
  bool require_no_active_pins_for_merge = true;
  bool require_no_active_pins_for_delete_physical = true;
  bool require_no_active_pins_for_repair = true;
  bool require_no_active_pins_for_rebuild = true;
  bool require_no_active_pins_for_salvage = true;
  bool allow_filespace_move = false;
  bool page_agent_relocation_complete_for_move = false;
  bool startup_open_safe_for_move = false;
  bool allow_filespace_merge = false;
  bool page_agent_merge_complete_for_merge = false;
  bool startup_open_safe_for_merge = false;
  bool allow_filespace_repair = false;
  bool repair_plan_authorized = false;
  bool repair_evidence_preserved = false;
  bool allow_filespace_rebuild = false;
  bool rebuild_source_verified = false;
  bool page_agent_rebuild_complete = false;
  bool startup_open_safe_for_rebuild = false;
  bool allow_filespace_salvage = false;
  bool salvage_review_authorized = false;
  bool salvage_output_quarantined = false;
  bool allow_physical_filespace_delete = false;
  bool physical_delete_retention_satisfied = false;
  bool physical_delete_legal_hold_clear = false;
  bool physical_delete_cleanup_horizon_authoritative = false;
  bool evidence_before_success = true;
  bool require_physical_header_for_attach = true;
  bool require_physical_header_for_promote = false;
};

struct FilespaceRegistry {
  std::vector<FilespaceDescriptor> filespaces;
  std::vector<FilespaceEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
};

struct FilespaceOperationRequest {
  FilespaceOperation operation = FilespaceOperation::verify_filespace;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid merge_target_filespace_uuid;
  std::string path;
  FilespaceRole role = FilespaceRole::secondary_data;
  u32 page_size = static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  FilespacePinKind pin_kind = FilespacePinKind::external;
  std::string pin_owner;
  u64 pin_count = 1;
  u16 physical_filespace_id = 0;
  u64 total_pages = 0;
  u64 free_pages = 0;
  u64 preallocated_pages = 0;
  u64 allocation_root_page = 0;
  u64 header_generation = 0;
  TypedUuid writer_identity_uuid;
  std::string reason;
  FilespaceLifecyclePolicy policy;
};

struct FilespaceOperationResult {
  Status status;
  FilespaceDescriptor descriptor;
  FilespaceEvidenceRecord evidence;
  DiagnosticRecord diagnostic;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;
  bool metrics_emitted = false;
  bool physical_file_removed = false;

  bool ok() const {
    return status.ok();
  }
};

struct FilespaceClassRequest {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid owner_object_uuid;
  FilespaceObjectClass object_class = FilespaceObjectClass::unspecified;
  std::string page_family;
  std::string reason;
  bool explicit_object_class = false;
};

struct FilespaceClassDecision {
  Status status;
  bool admitted = false;
  FilespaceObjectClass object_class = FilespaceObjectClass::unspecified;
  FilespaceClass filespace_class = FilespaceClass::unknown;
  FilespaceRole recommended_role = FilespaceRole::unknown;
  std::string page_family;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && admitted;
  }
};

struct FilespaceSerializeResult {
  Status status;
  std::string payload;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct FilespaceParseResult {
  Status status;
  FilespaceRegistry registry;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct FilespaceRegistryManifestWriteRequest {
  std::filesystem::path path;
  u64 generation = 0;
  TypedUuid writer_identity_uuid;
  bool remove_stale_temp = true;
};

struct FilespaceRegistryManifestResult {
  Status status;
  DiagnosticRecord diagnostic;
  u64 generation = 0;
  std::string checksum;
  bool stale_temp_removed = false;
  bool payload_written = false;
  bool file_synced = false;
  bool renamed = false;
  bool parent_synced = false;

  bool ok() const {
    return status.ok();
  }
};

struct FilespaceRegistryManifestLoadRequest {
  std::filesystem::path path;
  TypedUuid expected_writer_identity_uuid;
  bool remove_stale_temp = true;
};

struct FilespaceRegistryManifestLoadResult {
  Status status;
  DiagnosticRecord diagnostic;
  FilespaceRegistry registry;
  u64 generation = 0;
  std::string checksum;
  TypedUuid writer_identity_uuid;
  bool checksum_verified = false;
  bool writer_identity_verified = false;
  bool stale_temp_removed = false;

  bool ok() const {
    return status.ok();
  }
};

const char* FilespaceRoleName(FilespaceRole role);
const char* FilespaceStateName(FilespaceState state);
const char* FilespaceOperationName(FilespaceOperation operation);
const char* FilespacePinKindName(FilespacePinKind kind);
const char* FilespaceObjectClassName(FilespaceObjectClass object_class);
const char* FilespaceClassName(FilespaceClass filespace_class);
u64 ActivePinCount(const FilespaceDescriptor& descriptor);
FilespaceObjectClass DefaultFilespaceObjectClassForPageFamily(const std::string& page_family);
FilespaceClassDecision ResolveFilespaceClass(const FilespaceClassRequest& request);
FilespaceOperationResult ApplyFilespaceOperation(FilespaceRegistry* registry,
                                                 const FilespaceOperationRequest& request);
FilespaceSerializeResult SerializeFilespaceRegistry(const FilespaceRegistry& registry);
FilespaceParseResult ParseFilespaceRegistry(const std::string& payload);
FilespaceRegistryManifestResult PersistFilespaceRegistryManifest(
    const FilespaceRegistry& registry,
    const FilespaceRegistryManifestWriteRequest& request);
FilespaceRegistryManifestLoadResult LoadFilespaceRegistryManifest(
    const FilespaceRegistryManifestLoadRequest& request);
DiagnosticRecord MakeFilespaceDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {});

}  // namespace scratchbird::storage::filespace
