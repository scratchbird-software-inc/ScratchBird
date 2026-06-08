// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// ELER-024 physical cleanup reclamation coordinator.
// The durable MGA cleanup sweep is the only cleanup authority. All maps,
// indexes, allocation records, and page-finality records are evidence surfaces.

#include "api_types.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "allocation_map_page.hpp"
#include "current_row_map.hpp"
#include "overflow_persistence.hpp"
#include "page_allocation_lifecycle.hpp"
#include "page_finality_evidence.hpp"
#include "row_data_physical_sweep.hpp"
#include "secondary_index_garbage_cleanup.hpp"
#include "transaction_cleanup.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct MgaIntegratedPhysicalCleanupRequest {
  scratchbird::transaction::mga::LocalGarbageCollectionSweepResult sweep;
  bool engine_mga_authoritative = false;

  scratchbird::storage::page::RowDataPageBody row_page;
  scratchbird::core::platform::u32 row_page_size = 0;
  scratchbird::core::platform::u64 max_reclaim_rows = 0;

  MgaRelationStoreState relation_state;
  scratchbird::core::platform::u64 max_row_versions_to_scan = 0;
  scratchbird::core::platform::u64 max_index_entries_to_scan = 0;

  scratchbird::core::index::SecondaryIndexGarbageCleanupRequest
      secondary_index_cleanup;

  scratchbird::storage::page::OverflowLedger overflow_ledger;
  std::vector<scratchbird::storage::page::OverflowBlobPageReclaimRequest>
      overflow_blob_reclaims;

  scratchbird::transaction::mga::CurrentRowMapRebuildRequest
      current_row_map_rebuild;
  scratchbird::transaction::mga::PageFinalityMapEntry page_finality_entry;
  scratchbird::transaction::mga::PageFinalityObservedFacts page_finality_observed;

  scratchbird::storage::page::PageAllocationLedger allocation_ledger;
  std::vector<scratchbird::core::platform::TypedUuid> allocation_reclaim_uuids;
  scratchbird::storage::page::AllocationMapPageBody allocation_map;
  scratchbird::core::platform::u32 allocation_map_page_size = 0;
};

struct MgaIntegratedPhysicalCleanupResult {
  bool ok = false;
  bool fail_closed = false;
  EngineApiDiagnostic diagnostic;

  scratchbird::storage::page::RowDataPhysicalSweepResult row_page_sweep;
  MgaRelationPhysicalSweepResult relation_sweep;
  scratchbird::core::index::SecondaryIndexGarbageCleanupResult
      secondary_index_cleanup;
  scratchbird::storage::page::OverflowCleanupResult overflow_cleanup;
  std::vector<scratchbird::storage::page::OverflowBlobPageResult>
      overflow_blob_reclaims;
  scratchbird::transaction::mga::CurrentRowMapRebuildResult
      current_row_map_rebuild;
  scratchbird::transaction::mga::PageFinalityEvidenceDecision
      page_finality_cleanup;
  scratchbird::transaction::mga::ExactIndexCleanupAuthorityDecision
      exact_index_cleanup_authority;
  std::vector<scratchbird::storage::page::PageAllocationMutationResult>
      allocation_reclaims;
  scratchbird::storage::page::AllocationMapPageBodyResult allocation_map;

  scratchbird::core::platform::u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  bool cleanup_horizon_authoritative = false;
  bool row_page_mutated = false;
  bool relation_state_mutated = false;
  bool secondary_index_cleaned = false;
  bool overflow_cleaned = false;
  bool overflow_pages_reclaimed = false;
  bool current_row_map_rebuilt = false;
  bool page_finality_accepted = false;
  bool allocation_reclaimed = false;
  bool allocation_map_mutated = false;
  std::vector<EngineEvidenceReference> evidence;
};

MgaIntegratedPhysicalCleanupResult ApplyMgaIntegratedPhysicalCleanup(
    const MgaIntegratedPhysicalCleanupRequest& request);

}  // namespace scratchbird::engine::internal_api
