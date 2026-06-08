// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT
#include "secondary_index_garbage_cleanup.hpp"
#include "transaction_cleanup_horizon_service.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::index::PersistentSecondaryIndexDeltaLedger;
using scratchbird::core::index::SecondaryIndexBaseEntry;
using scratchbird::core::index::SecondaryIndexGarbageCleanupDecisionKind;
using scratchbird::core::index::SecondaryIndexGarbageCleanupMetrics;
using scratchbird::core::index::SecondaryIndexGarbageCleanupResult;
using scratchbird::core::index::SecondaryIndexKind;
using scratchbird::core::index::SecondaryIndexTableSnapshotEntry;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonRequest;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonResult;

struct IndexGarbageCleanupAgentRequest {
  AuthoritativeCleanupHorizonRequest horizon_request;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  PersistentSecondaryIndexDeltaLedger ledger;
  std::vector<SecondaryIndexBaseEntry> base_entries;
  std::vector<SecondaryIndexTableSnapshotEntry> table_snapshot;
  SecondaryIndexKind index_kind = SecondaryIndexKind::non_unique;
  u64 max_records_to_scan = 1024;
  u64 max_records_to_clean = 256;
  bool engine_mga_authoritative = false;
};

struct IndexGarbageCleanupEvidenceField {
  std::string key;
  std::string value;
};

struct IndexGarbageCleanupAgentResult {
  Status status;
  SecondaryIndexGarbageCleanupDecisionKind decision =
      SecondaryIndexGarbageCleanupDecisionKind::refused;
  DiagnosticRecord diagnostic;
  AuthoritativeCleanupHorizonResult horizon;
  SecondaryIndexGarbageCleanupResult cleanup;
  PersistentSecondaryIndexDeltaLedger cleaned_ledger;
  SecondaryIndexGarbageCleanupMetrics before;
  SecondaryIndexGarbageCleanupMetrics after;
  std::vector<IndexGarbageCleanupEvidenceField> evidence;
  bool fail_closed = true;
  bool bounded_batch = false;
  bool budget_exhausted = false;
  bool horizon_blocked = false;
  bool validation_before_ok = false;
  bool validation_after_ok = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

IndexGarbageCleanupAgentResult RunIndexGarbageCleanupAgentBatch(
    const IndexGarbageCleanupAgentRequest& request);

DiagnosticRecord MakeIndexGarbageCleanupAgentDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

const char* index_garbage_cleanup_agent_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
