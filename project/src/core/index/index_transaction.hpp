// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-TRANSACTION-CLOSURE-ANCHOR

#include "index_management.hpp"
#include "index_posting.hpp"

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class IndexTransactionalOperation : u32 {
  create_index = 1,
  drop_index = 2,
  build_index = 3,
  insert_entry = 4,
  update_entry = 5,
  delete_entry = 6,
  savepoint_open = 7,
  savepoint_rollback = 8,
  cleanup_dead_entries = 9,
  recovery_classify = 10,
  local_prepare = 11,
  cluster_prepare_todo = 12
};

enum class IndexTransactionalState : u32 {
  active = 1,
  pending_catalog = 2,
  building = 3,
  catchup = 4,
  ready_to_publish = 5,
  committed = 6,
  rolled_back = 7,
  stale = 8,
  suspect = 9,
  quarantine = 10,
  dropping = 11,
  refused = 12
};

struct IndexTransactionalRequest {
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::unknown;
  IndexTransactionalOperation operation = IndexTransactionalOperation::insert_entry;
  u64 local_transaction_id = 0;
  u64 savepoint_id = 0;
  u64 oldest_active_transaction_id = 0;
  bool catalog_evidence_written = false;
  bool mutation_evidence_written = false;
  bool online_build = false;
  bool snapshot_captured = false;
  bool catchup_complete = false;
  bool validation_complete = false;
  bool publish_requested = false;
  bool cluster_authority_active = false;
  bool local_prepare_supported = false;
  bool interrupted_recovery = false;
  bool page_authority_valid = true;
};

struct IndexTransactionalPlan {
  Status status;
  bool admitted = false;
  IndexTransactionalState next_state = IndexTransactionalState::refused;
  bool durable_evidence_required = true;
  bool catalog_update_required = false;
  bool row_mutation_required = false;
  bool horizon_guard_required = true;
  bool cluster_todo_only = false;
  std::vector<std::string> steps;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

IndexTransactionalPlan PlanIndexTransactionalOperation(const IndexTransactionalRequest& request);
DiagnosticRecord MakeIndexTransactionDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {});

}  // namespace scratchbird::core::index
