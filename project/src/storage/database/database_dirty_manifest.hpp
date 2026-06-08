// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DIRTY-MANIFEST-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::database {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u32 kDirtyObjectManifestFormatVersion = 1;

enum class DirtyObjectKind : u16 {
  database_header,
  startup_state,
  transaction_inventory,
  catalog_page,
  allocation_map,
  row_data_page,
  index_page,
  filespace_header,
  metric_history,
  unknown
};

enum class DirtyManifestRecoveryAction : u16 {
  no_action,
  use_manifest,
  rebuild_by_scan,
  quarantine,
  fail_closed
};

struct DirtyObjectManifestEntry {
  DirtyObjectKind kind = DirtyObjectKind::unknown;
  TypedUuid object_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 object_checksum = 0;
  u64 local_transaction_id = 0;
  u64 operation_envelope_checksum = 0;
  u64 transaction_evidence_checksum = 0;
  bool dirty = true;
  bool authoritative = true;
};

struct DirtyObjectManifest {
  u32 format_version = kDirtyObjectManifestFormatVersion;
  u64 checkpoint_generation = 0;
  u64 manifest_checksum = 0;
  bool completed = false;
  bool classification_only = true;
  std::vector<DirtyObjectManifestEntry> entries;
};

struct DirtyObjectManifestResult {
  Status status;
  DirtyObjectManifest manifest;
  std::string serialized;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct DirtyManifestRecoveryClassification {
  DirtyObjectKind kind = DirtyObjectKind::unknown;
  TypedUuid object_uuid;
  u64 page_number = 0;
  DirtyManifestRecoveryAction action = DirtyManifestRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct DirtyManifestRecoveryResult {
  Status status;
  bool rebuild_by_scan_required = false;
  bool quarantine_required = false;
  std::vector<DirtyManifestRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct CheckpointRootCandidate {
  u64 checkpoint_generation = 0;
  u64 predecessor_generation = 0;
  TypedUuid root_object_uuid;
  u64 root_checksum = 0;
  bool completed = false;
  bool authoritative = true;
};

struct CheckpointRootSelectionResult {
  Status status;
  bool selected = false;
  CheckpointRootCandidate root;
  std::vector<u64> predecessor_chain;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct DirtyManifestRecoveryRunEvidence {
  std::string recovery_run_uuid;
  u64 checkpoint_generation = 0;
  u64 classification_count = 0;
  u64 classification_checksum = 0;
  std::string recovery_action;
  bool completed = false;
};

struct DirtyManifestRecoveryRunEvidenceResult {
  Status status;
  bool already_recorded = false;
  DirtyManifestRecoveryRunEvidence evidence;
  std::string serialized;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* DirtyObjectKindName(DirtyObjectKind kind);
const char* DirtyManifestRecoveryActionName(DirtyManifestRecoveryAction action);

DirtyObjectManifestResult BuildDirtyObjectManifest(const DirtyObjectManifest& manifest);
DirtyObjectManifestResult ParseDirtyObjectManifest(const std::string& serialized);
DirtyManifestRecoveryResult ClassifyDirtyObjectManifestForRecovery(const DirtyObjectManifest& manifest);
CheckpointRootSelectionResult SelectCheckpointRootSet(const std::vector<CheckpointRootCandidate>& candidates);
DirtyManifestRecoveryRunEvidenceResult PersistDirtyManifestRecoveryRunEvidence(
    const std::string& evidence_store_path,
    const DirtyObjectManifest& manifest,
    const DirtyManifestRecoveryResult& recovery,
    const std::string& recovery_run_uuid);
DiagnosticRecord MakeDirtyManifestDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

}  // namespace scratchbird::storage::database
