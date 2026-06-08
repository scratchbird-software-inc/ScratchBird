// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_CLEANUP_BACKUP_RESTORE_REPAIR_DIAGNOSTICS
#include "api_types.hpp"
#include "agents/index_garbage_cleanup_agent.hpp"
#include "agents/storage_version_cleanup_agent.hpp"
#include "transaction_cleanup_horizon_service.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct EngineCleanupDiagnosticsRequest : EngineApiRequest {
  bool cleanup_horizon_present = false;
  scratchbird::transaction::mga::AuthoritativeCleanupHorizonResult cleanup_horizon;

  bool storage_cleanup_present = false;
  scratchbird::core::agents::implemented_agents::StorageVersionCleanupAgentResult
      storage_cleanup;

  bool index_cleanup_present = false;
  scratchbird::core::agents::implemented_agents::IndexGarbageCleanupAgentResult
      index_cleanup;

  // Empty means report all DPC-034 contexts: backup, restore, restricted_open,
  // and repair.
  std::vector<std::string> context_kinds;
  bool support_bundle_requested = true;
};

struct EngineCleanupDiagnosticsResult : EngineApiResult {
  bool cleanup_diagnostics_ready = false;
  bool support_bundle_ready = false;

  std::string cleanup_horizon_identity;
  std::string cleanup_horizon_authority_status;
  bool cleanup_horizon_authoritative = false;
  EngineApiU64 cleanup_horizon_local_transaction_id = 0;

  EngineApiU64 storage_row_version_backlog_count = 0;
  EngineApiU64 storage_row_version_retained_count = 0;
  EngineApiU64 storage_row_version_reclaimed_count = 0;
  EngineApiU64 storage_row_version_blocked_count = 0;

  EngineApiU64 index_garbage_backlog_count = 0;
  EngineApiU64 index_garbage_cleaned_count = 0;
  EngineApiU64 index_garbage_retained_count = 0;
  EngineApiU64 index_garbage_horizon_blocked_count = 0;
  EngineApiU64 index_validation_refused_count = 0;
  EngineApiU64 index_non_authoritative_refused_count = 0;

  bool parser_finality_authority = false;
  bool client_finality_authority = false;
  bool timestamp_finality_authority = false;
  bool uuid_ordering_finality_authority = false;
  bool event_stream_finality_authority = false;

  std::string support_bundle_json;
};

EngineCleanupDiagnosticsResult EngineInspectCleanupDiagnostics(
    const EngineCleanupDiagnosticsRequest& request);

const char* CleanupDiagnosticsSurfaceSchemaId();
EngineApiU64 CleanupDiagnosticsSurfaceSchemaVersion();

}  // namespace scratchbird::engine::internal_api
