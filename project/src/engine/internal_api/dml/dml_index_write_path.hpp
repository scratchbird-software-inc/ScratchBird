// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "crud_support/crud_store.hpp"
#include "deferred_secondary_index_runtime_policy.hpp"
#include "index_btree_page.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class DmlIndexWriteOperation {
  insert,
  update,
  delete_row,
  merge_insert,
  merge_update,
  merge_delete
};

struct DmlIndexWriteRowImage {
  std::string row_uuid;
  std::string version_uuid;
  std::vector<std::pair<std::string, std::string>> values;
};

struct DmlIndexWriteEvent {
  DmlIndexWriteOperation operation = DmlIndexWriteOperation::insert;
  std::size_t source_ordinal = 0;
  std::size_t action_ordinal = 0;
  CrudIndexRecord index;
  std::string table_uuid;
  DmlIndexWriteRowImage old_row;
  DmlIndexWriteRowImage new_row;
  bool has_old_row = false;
  bool has_new_row = false;
  std::string transaction_uuid;
  EngineApiU64 local_transaction_id = 0;
  bool mga_transaction_identity_proof = false;
  bool mga_transaction_finality_authority_proof = false;
  std::string rollback_evidence_token;
  bool rollback_safe_structural_evidence = false;
  bool index_descriptor_capability_proof = false;
  bool key_extraction_proof = false;
  bool partial_predicate_proof = false;
  bool covering_payload_proof = false;
  bool unique_preflight_proof = false;
  bool unique_reservation_preflight_proof = false;
};

struct DmlPhysicalIndexTreeRef {
  std::string index_uuid;
  scratchbird::storage::page::IndexBtreePhysicalTree* tree = nullptr;
};

enum class DmlUpdateIndexMaintenanceMode {
  synchronous_physical_rewrite,
  hot_like_version_append,
  deferred_secondary_delta_ledger
};

struct DmlUpdateIndexMaintenanceRequest {
  bool indexed_keys_changed = true;
  bool unique = false;
  std::string family;
  std::vector<std::string> option_envelopes;
  bool cleanup_horizon_present = false;
  bool durable_mga_inventory_proof = false;
  bool delta_overlay_read_proof = false;
  bool recovery_classification_proof = false;
  bool unique_reservation_protocol_proof = false;
  bool unique_deferred_route_closure_proof = false;
};

struct DmlUpdateIndexMaintenanceDecision {
  bool ok = false;
  DmlUpdateIndexMaintenanceMode mode =
      DmlUpdateIndexMaintenanceMode::synchronous_physical_rewrite;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
  bool synchronous_fallback_required = true;
  std::string reason;
};

struct DmlSecondaryDeltaLedgerRef {
  std::string index_uuid;
  scratchbird::core::index::PersistentSecondaryIndexDeltaLedger* ledger = nullptr;
  scratchbird::core::index::SecondaryIndexDeltaLedgerLimits limits;
};

struct DmlIndexWritePathRequest {
  std::vector<DmlIndexWriteEvent> events;
  std::vector<DmlPhysicalIndexTreeRef> physical_trees;
  std::vector<DmlSecondaryDeltaLedgerRef> secondary_delta_ledgers;
  std::vector<std::string> deferred_secondary_index_options;
  std::string cleanup_horizon_token;
  bool durable_mga_inventory_proof = false;
  bool delta_overlay_read_proof = false;
  bool recovery_classification_proof = false;
  bool unique_reservation_protocol_proof = false;
  bool unique_deferred_route_closure_proof = false;
};

struct DmlIndexWritePathResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
  EngineApiU64 physical_inserts = 0;
  EngineApiU64 physical_deletes = 0;
  EngineApiU64 unchanged_key_noops = 0;
  EngineApiU64 merge_events = 0;
  EngineApiU64 hot_like_version_appends = 0;
  EngineApiU64 secondary_delta_ledger_appends = 0;
  EngineApiU64 secondary_delta_overlay_reads = 0;
};

const char* DmlIndexWriteOperationName(DmlIndexWriteOperation operation);

DmlUpdateIndexMaintenanceDecision DecideDmlUpdateIndexMaintenance(
    const DmlUpdateIndexMaintenanceRequest& request);

DmlIndexWritePathResult ApplyDmlIndexWritePath(
    const DmlIndexWritePathRequest& request);

}  // namespace scratchbird::engine::internal_api
