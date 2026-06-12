// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-FILESPACE-DISCOVERY-ANCHOR
#include "filespace_lifecycle.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::filespace {

enum class FilespaceDiscoveryClassification : u16 {
  ok,
  missing,
  wrong_database,
  wrong_filespace,
  duplicate_identity,
  stale_header,
  replaced_header,
  foreign_orphan,
  quarantined_candidate
};

struct FilespaceDiscoveryCandidate {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string path;
  FilespaceRole role = FilespaceRole::unknown;
  FilespaceState state = FilespaceState::absent;
  u32 page_size = static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  u16 physical_filespace_id = 0;
  u64 header_generation = 0;
  TypedUuid writer_identity_uuid;
  bool physical_header_present = true;
};

struct FilespaceDiscoveryRequest {
  TypedUuid database_uuid;
  std::vector<FilespaceDescriptor> expected;
  std::vector<FilespaceDiscoveryCandidate> observed;
  bool quarantine_unmatched_observed = true;
  bool release_requires_authority = true;
  bool require_operator_review_for_anomalies = true;
};

struct FilespaceDiscoveryRow {
  FilespaceDiscoveryClassification classification = FilespaceDiscoveryClassification::ok;
  DiagnosticRecord diagnostic;
  TypedUuid expected_database_uuid;
  TypedUuid expected_filespace_uuid;
  TypedUuid observed_database_uuid;
  TypedUuid observed_filespace_uuid;
  std::string expected_path;
  std::string observed_path;
  std::string recommended_action;
  bool normal_access_allowed = true;
  bool quarantine_required = false;
  bool release_requires_authority = false;
  bool operator_review_required = false;
  bool cache_invalidation_required = false;
};

struct FilespaceDiscoveryResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<FilespaceDiscoveryRow> rows;
  u64 anomaly_count = 0;
  bool quarantine_required = false;
  bool operator_review_required = false;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;

  bool ok() const { return status.ok(); }
};

struct FilespaceDiscoveryExecutionRequest {
  FilespaceDiscoveryRequest discovery;
  bool execute_quarantine_actions = false;
  bool execute_release_actions = false;
  bool execute_physical_cleanup_actions = false;
  bool physical_header_required_for_quarantine = true;
  bool header_inspection_passed = false;
  bool release_authorized = false;
  bool allow_physical_filespace_delete = false;
  bool physical_delete_retention_satisfied = false;
  bool physical_delete_legal_hold_clear = false;
  bool physical_delete_cleanup_horizon_authoritative = false;
  std::string operation_uuid;
  std::string inspector_uuid;
  std::string release_authority_uuid;
};

struct FilespaceDiscoveryExecutionResult {
  Status status;
  DiagnosticRecord diagnostic;
  FilespaceDiscoveryResult discovery;
  u64 quarantine_execution_count = 0;
  u64 release_execution_count = 0;
  u64 physical_cleanup_execution_count = 0;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;
  bool cleanup_or_quarantine_executed = false;
  bool release_executed = false;
  bool physical_cleanup_executed = false;
  bool physical_file_removed = false;

  bool ok() const { return status.ok(); }
};

struct FilespaceDiscoveryFilesystemScanRequest {
  TypedUuid database_uuid;
  std::vector<FilespaceDescriptor> expected;
  std::vector<std::string> observed_paths;
  bool quarantine_unmatched_observed = true;
  bool release_requires_authority = true;
  bool require_operator_review_for_anomalies = true;
};

struct FilespaceDiscoveryFilesystemScanResult {
  Status status;
  DiagnosticRecord diagnostic;
  FilespaceDiscoveryResult discovery;
  std::vector<FilespaceDiscoveryCandidate> observed;
  u64 scanned_path_count = 0;
  u64 observed_header_count = 0;
  u64 missing_path_count = 0;
  u64 unreadable_header_count = 0;
  bool runtime_filesystem_scan_executed = false;
  bool durable_state_changed = false;
  bool cleanup_or_quarantine_executed = false;

  bool ok() const { return status.ok(); }
};

const char* FilespaceDiscoveryClassificationName(FilespaceDiscoveryClassification classification);
FilespaceDiscoveryResult DiscoverFilespaceAnomalies(const FilespaceDiscoveryRequest& request);
FilespaceDiscoveryExecutionResult ExecuteFilespaceDiscoveryActions(
    FilespaceRegistry* registry,
    const FilespaceDiscoveryExecutionRequest& request);
FilespaceDiscoveryFilesystemScanResult DiscoverFilespaceAnomaliesFromFilesystem(
    const FilespaceDiscoveryFilesystemScanRequest& request);

}  // namespace scratchbird::storage::filespace
