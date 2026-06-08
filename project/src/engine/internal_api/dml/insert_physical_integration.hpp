// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INSERT-PHYSICAL-INTEGRATION-ANCHOR
#include "api_types.hpp"
#include "filespace_growth.hpp"
#include "overflow_persistence.hpp"
#include "page_reservation.hpp"
#include "page_selection.hpp"
#include "secondary_index_delta_merge.hpp"
#include "secondary_index_delta_overlay.hpp"
#include "strict_bulk_load_lifecycle.hpp"

#include <string>
#include <span>
#include <vector>

namespace scratchbird::engine::internal_api::dml {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

struct InsertPhysicalIntegrationContext {
  scratchbird::storage::page::PageReservationLedger* page_reservation_ledger = nullptr;
  scratchbird::storage::page::PageSelectionLedger* page_selection_ledger = nullptr;
  scratchbird::storage::filespace::FilespaceGrowthLedger* filespace_growth_ledger = nullptr;
  const scratchbird::storage::filespace::FilespaceRegistry* filespace_registry = nullptr;
  scratchbird::core::index::SecondaryIndexOverlayLedger* secondary_index_overlay_ledger = nullptr;
  scratchbird::core::index::SecondaryIndexDeltaMergeLedger* secondary_index_merge_ledger = nullptr;
  std::vector<scratchbird::core::index::SecondaryIndexBaseEntry>* secondary_index_base_entries = nullptr;
  scratchbird::core::index::SecondaryIndexDeltaLedger* secondary_index_delta_ledger = nullptr;
  scratchbird::storage::page::OverflowLedger* overflow_ledger = nullptr;
  scratchbird::core::bulk_load::StrictBulkLoadLedger* strict_bulk_load_ledger = nullptr;
};

struct InsertPhysicalIntegrationRequest {
  TypedUuid database_uuid;
  TypedUuid object_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid request_id;
  std::string page_family = "row_data";
  u64 estimated_row_count = 1;
  u64 estimated_payload_bytes = 0;
  u64 encoded_row_bytes = 0;
  u32 page_size = 16 * 1024;
  u64 time_authority_tick = 0;
  u64 reservation_lease_ticks = 60 * 1000;
  bool request_filespace_growth_on_missing_page = false;
  u64 growth_page_count = 0;
  u64 predicted_insert_pressure_pages = 0;
  scratchbird::storage::filespace::FilespaceRole growth_filespace_role =
      scratchbird::storage::filespace::FilespaceRole::secondary_data;
  scratchbird::storage::filespace::InsertFilespaceGrowthUrgency growth_urgency =
      scratchbird::storage::filespace::InsertFilespaceGrowthUrgency::normal;

  bool enable_deferred_secondary_index = false;
  bool deferred_index_overlay_gate = false;
  bool deferred_index_merge_gate = false;
  bool deferred_index_cleanup_gate = false;
  bool deferred_index_recovery_gate = false;
  TypedUuid secondary_index_uuid;
  u64 secondary_index_snapshot_high_water_local_transaction_id = 0;
  u64 secondary_index_cleanup_horizon_local_transaction_id = 0;

  bool persist_overflow_payload = false;
  TypedUuid row_uuid;
  std::string overflow_value_descriptor;
  std::vector<byte> overflow_payload;
  u32 overflow_chunk_size = 4096;

  bool run_strict_bulk_load = false;
  scratchbird::core::bulk_load::StrictBulkLoadPolicySnapshot strict_bulk_load_policy;
  std::vector<scratchbird::core::bulk_load::StrictBulkLoadRow> strict_bulk_load_rows;
  std::string strict_bulk_load_staging_target;
};

struct InsertPhysicalIntegrationResult {
  Status status;
  bool integrated = false;
  bool page_reserved = false;
  bool page_selected = false;
  bool filespace_growth_admitted = false;
  bool deferred_secondary_index_verified = false;
  bool overflow_persisted = false;
  bool strict_bulk_load_finalized = false;
  TypedUuid reservation_id;
  std::string selection_fence;
  TypedUuid growth_operation_id;
  TypedUuid overflow_value_uuid;
  TypedUuid strict_bulk_load_id;
  std::vector<std::string> evidence_refs;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && (integrated || filespace_growth_admitted); }
};

InsertPhysicalIntegrationResult ExecuteInsertPhysicalIntegration(
    InsertPhysicalIntegrationContext* context,
    const InsertPhysicalIntegrationRequest& request);
DiagnosticRecord MakeInsertPhysicalIntegrationDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail = {});

struct DirectPhysicalBulkAppendRequest {
  EngineRequestContext context;
  EngineObjectReference target_table;
  std::span<const EngineRowValue> borrowed_input_rows;
  std::vector<std::string> option_envelopes;
  std::vector<std::string> diagnostic_options;
  EngineApiU64 estimated_row_count = 0;
  std::string lane_operation = "copy_import";
  std::string duplicate_mode = "error";
  bool require_generated_row_uuid = true;
  bool strict_bulk_load_requested = false;
  bool direct_lane_enabled = true;
};

struct DirectPhysicalBulkAppendResult : EngineApiResult {
  EngineApiU64 accepted_rows = 0;
  EngineApiU64 inserted_rows = 0;
  EngineApiU64 rejected_rows = 0;
  std::vector<EngineUuid> row_uuids;
  bool direct_lane_selected = false;
};

DirectPhysicalBulkAppendResult ExecuteDirectPhysicalBulkAppend(
    const DirectPhysicalBulkAppendRequest& request);

}  // namespace scratchbird::engine::internal_api::dml
