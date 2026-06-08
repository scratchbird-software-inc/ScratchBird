// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-TXN-HISTORICAL-SNAPSHOT-ANCHOR
#include "transaction_snapshot.hpp"

#include <string>

namespace scratchbird::transaction::mga {

enum class HistoricalAuditLocationClass : u16 {
  local_hot,
  local_archive,
  retired,
  remote,
  unknown
};

struct HistoricalAuditSnapshotEvidence {
  bool transaction_inventory_authoritative = false;
  bool archive_manifest_authoritative = false;
  bool security_policy_authoritative = false;
  bool catalog_epoch_authoritative = false;
  bool cluster_epoch_authoritative = false;
  bool external_cluster_provider_available = false;
  u64 security_epoch = 0;
  u64 catalog_generation_id = 0;
  u64 cluster_epoch = 0;
};

struct HistoricalAuditSnapshotRequest {
  LocalTransactionInventory inventory;
  LocalTransactionId audit_reader_transaction;
  LocalTransactionId target_local_transaction_id;
  HistoricalAuditLocationClass requested_location_class =
      HistoricalAuditLocationClass::unknown;
  HistoricalAuditSnapshotEvidence evidence;
  bool retired_history_evidence_present = false;
  bool write_intent = false;
};

struct HistoricalAuditSnapshotResult {
  Status status;
  LocalTransactionSnapshot snapshot;
  VisibilitySnapshot visibility_snapshot;
  LocalTransactionId target_local_transaction_id;
  HistoricalAuditLocationClass location_class =
      HistoricalAuditLocationClass::unknown;
  std::string target_transaction_state = "unknown";
  bool queryable = false;
  bool writes_refused = true;
  bool fail_closed = true;
  bool retired_history_exact = false;
  bool no_cluster_remote_fail_closed = false;
  bool parser_finality_authority = false;
  bool donor_finality_authority = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && queryable && snapshot.reader_transaction.valid();
  }
};

const char* HistoricalAuditLocationClassName(HistoricalAuditLocationClass value);
HistoricalAuditSnapshotResult CreateHistoricalAuditSnapshot(
    const HistoricalAuditSnapshotRequest& request);
DiagnosticRecord MakeHistoricalAuditSnapshotDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail = {});

}  // namespace scratchbird::transaction::mga
