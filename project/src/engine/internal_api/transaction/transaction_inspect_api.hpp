// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_MGA_TRANSACTION_INSPECT_API
struct EngineInspectTransactionLineageRequest : EngineApiRequest {};
struct EngineInspectTransactionLineageResult : EngineApiResult {};
EngineInspectTransactionLineageResult EngineInspectTransactionLineage(
    const EngineInspectTransactionLineageRequest& request);

struct EngineClassifyTransactionRestoreRequest : EngineApiRequest {};
struct EngineClassifyTransactionRestoreResult : EngineApiResult {
  bool restore_allowed = false;
  bool wal_required = false;
};
EngineClassifyTransactionRestoreResult EngineClassifyTransactionRestore(
    const EngineClassifyTransactionRestoreRequest& request);

struct EngineInspectTransactionForensicsRequest : EngineApiRequest {};
struct EngineInspectTransactionForensicsResult : EngineApiResult {};
EngineInspectTransactionForensicsResult EngineInspectTransactionForensics(
    const EngineInspectTransactionForensicsRequest& request);

struct EngineLocateTransactionRequest : EngineApiRequest {
  EngineApiU64 target_local_transaction_id = 0;
  EngineUuid target_transaction_uuid;
  std::string requested_location_class;
  bool remote_location_requested = false;
  bool retired_history_evidence_present = false;
};

struct EngineLocateTransactionResult : EngineApiResult {
  EngineApiU64 target_local_transaction_id = 0;
  EngineUuid target_transaction_uuid;
  std::string location_class = "unknown";
  std::string transaction_state = "unknown";
  bool queryable = false;
  bool writes_refused = true;
  bool fail_closed = true;
  bool local_inventory_authoritative = false;
  bool archive_authoritative = false;
  bool external_cluster_provider_required = false;
  std::string location_diagnostic_code;
  std::string location_diagnostic_detail;
};
EngineLocateTransactionResult EngineLocateTransaction(
    const EngineLocateTransactionRequest& request);

struct EngineBeginAuditReadTransactionRequest : EngineLocateTransactionRequest {
  std::string isolation_level = "audit_as_of";
};

struct EngineBeginAuditReadTransactionResult : EngineLocateTransactionResult {
  EngineUuid audit_transaction_uuid;
  EngineApiU64 audit_local_transaction_id = 0;
  EngineApiU64 snapshot_visible_through_local_transaction_id = 0;
  bool audit_transaction_distinct = false;
  bool read_only = true;
};
EngineBeginAuditReadTransactionResult EngineBeginAuditReadTransaction(
    const EngineBeginAuditReadTransactionRequest& request);

}  // namespace scratchbird::engine::internal_api
