// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_STORAGE_VERSION_CLEANUP_AGENT
#include "agent_runtime.hpp"
#include "transaction_cleanup.hpp"
#include "transaction_cleanup_horizon_service.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonRequest;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonResult;
using scratchbird::transaction::mga::AuthoritativeCleanupHold;
using scratchbird::transaction::mga::HistoryRetentionHorizon;
using scratchbird::transaction::mga::LocalGarbageCollectionSweepResult;
using scratchbird::transaction::mga::RowVersionMetadata;

enum class StorageVersionCleanupDecisionKind : u32 {
  success,
  no_op,
  budget_exhausted,
  blocked_by_active_transactions,
  refused_non_authoritative,
  refused
};

struct StorageVersionCleanupPressureMetrics {
  u64 total_row_versions = 0;
  u64 cleanup_candidate_row_versions = 0;
  u64 current_visible_row_versions = 0;
  u64 blocked_row_versions = 0;
  u64 reclaimed_row_versions = 0;
  u64 retained_row_versions = 0;
  u64 active_cleanup_blockers = 0;
};

struct StorageVersionCleanupAgentRequest {
  AuthoritativeCleanupHorizonRequest horizon_request;
  std::vector<RowVersionMetadata> row_versions;
  std::vector<AuthoritativeCleanupHold> additional_holds;
  std::vector<HistoryRetentionHorizon> history_retention_horizons;
  u64 max_candidate_row_versions = 0;
  u64 max_retained_row_versions = 0;
  bool engine_mga_authoritative = false;
  bool retain_row_versions_in_result = false;
};

struct StorageVersionCleanupEvidenceField {
  std::string key;
  std::string value;
};

struct StorageVersionCleanupAgentResult {
  Status status;
  StorageVersionCleanupDecisionKind decision =
      StorageVersionCleanupDecisionKind::refused;
  DiagnosticRecord diagnostic;
  AuthoritativeCleanupHorizonResult horizon;
  LocalGarbageCollectionSweepResult sweep;
  StorageVersionCleanupPressureMetrics before;
  StorageVersionCleanupPressureMetrics after;
  std::vector<StorageVersionCleanupEvidenceField> evidence;
  bool fail_closed = false;
  bool bounded_batch = false;
  bool budget_exhausted = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* StorageVersionCleanupDecisionKindName(
    StorageVersionCleanupDecisionKind decision);
StorageVersionCleanupAgentResult RunStorageVersionCleanupAgentBatch(
    const StorageVersionCleanupAgentRequest& request);
DiagnosticRecord MakeStorageVersionCleanupAgentDiagnostic(Status status,
                                                          std::string diagnostic_code,
                                                          std::string message_key,
                                                          std::string detail = {});

const char* storage_version_cleanup_agent_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
