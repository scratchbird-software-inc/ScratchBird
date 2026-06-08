// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_physical_cleanup_coordinator.hpp"

#include "api_diagnostics.hpp"

#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;
namespace page = scratchbird::storage::page;
using scratchbird::core::platform::u64;

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

MgaIntegratedPhysicalCleanupResult Refuse(std::string detail) {
  MgaIntegratedPhysicalCleanupResult result;
  result.fail_closed = true;
  result.diagnostic =
      MakeInvalidRequestDiagnostic("mga.integrated_physical_cleanup",
                                   std::move(detail));
  return result;
}

void AddEvidence(MgaIntegratedPhysicalCleanupResult* result,
                 std::string kind,
                 std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

bool NeedsAllocationMapMutation(const page::AllocationMapExtent& extent,
                                u64 horizon) {
  return extent.state == page::PageAllocationLifecycleState::reusable_pending_mga &&
         extent.reusable_after_local_transaction_id != 0 &&
         extent.reusable_after_local_transaction_id < horizon;
}

}  // namespace

MgaIntegratedPhysicalCleanupResult ApplyMgaIntegratedPhysicalCleanup(
    const MgaIntegratedPhysicalCleanupRequest& request) {
  if (!request.engine_mga_authoritative) {
    return Refuse("engine_mga_authority_required");
  }
  if (!request.sweep.ok() ||
      !request.sweep.cleanup.cleanup_horizon_authoritative ||
      request.sweep.cleanup.authoritative_cleanup_horizon_local_transaction_id == 0) {
    return Refuse("authoritative_mga_cleanup_sweep_required");
  }
  if (request.sweep.cleanup.physical_storage_mutated) {
    return Refuse("cleanup_decision_must_not_be_pre_mutated");
  }

  const u64 horizon =
      request.sweep.cleanup.authoritative_cleanup_horizon_local_transaction_id;
  if (request.current_row_map_rebuild.map_self_authoritative ||
      !request.current_row_map_rebuild.authoritative_base_rows_proof ||
      !request.current_row_map_rebuild.durable_mga_inventory_proof) {
    return Refuse("current_row_map_authoritative_base_rows_required");
  }
  if (!request.page_finality_observed.transaction_horizon_authoritative ||
      !request.page_finality_observed.transaction_inventory_authoritative ||
      !request.page_finality_observed.normal_mga_visibility_authority_available) {
    return Refuse("page_finality_observed_authority_required");
  }

  MgaIntegratedPhysicalCleanupResult result;
  result.authoritative_cleanup_horizon_local_transaction_id = horizon;
  result.cleanup_horizon_authoritative = true;
  AddEvidence(&result, "mga_cleanup_authority", "durable_mga_transaction_inventory");
  AddEvidence(&result, "mga_cleanup_horizon", std::to_string(horizon));

  page::RowDataPhysicalSweepRequest row_request;
  row_request.page = request.row_page;
  row_request.sweep = request.sweep;
  row_request.page_size = request.row_page_size;
  row_request.engine_mga_authoritative = true;
  row_request.max_reclaim_rows = request.max_reclaim_rows;
  result.row_page_sweep = page::ApplyRowDataPhysicalSweep(row_request);
  if (!result.row_page_sweep.ok()) {
    result.fail_closed = true;
    result.diagnostic = MakeEngineApiDiagnostic(
        result.row_page_sweep.diagnostic.diagnostic_code,
        result.row_page_sweep.diagnostic.message_key,
        "row_data_physical_sweep",
        true);
    return result;
  }
  result.row_page_mutated = result.row_page_sweep.physical_storage_mutated;

  MgaRelationPhysicalSweepRequest relation_request;
  relation_request.state = request.relation_state;
  relation_request.reclaim_evidence_records =
      request.sweep.cleanup.reclaim_evidence_records;
  relation_request.engine_mga_authoritative = true;
  relation_request.cleanup_horizon_authoritative = true;
  relation_request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  relation_request.max_row_versions_to_scan = request.max_row_versions_to_scan;
  relation_request.max_index_entries_to_scan = request.max_index_entries_to_scan;
  result.relation_sweep = ApplyMgaRelationPhysicalSweepToState(relation_request);
  if (!result.relation_sweep.ok) {
    result.fail_closed = true;
    result.diagnostic = result.relation_sweep.diagnostic;
    return result;
  }
  result.relation_state_mutated = result.relation_sweep.physical_state_mutated;

  idx::SecondaryIndexGarbageCleanupRequest index_request =
      request.secondary_index_cleanup;
  index_request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  index_request.cleanup_horizon_authoritative = true;
  result.secondary_index_cleanup =
      idx::RunSecondaryIndexGarbageCleanupBatch(index_request);
  if (!result.secondary_index_cleanup.ok()) {
    result.fail_closed = result.secondary_index_cleanup.fail_closed;
    result.diagnostic = MakeEngineApiDiagnostic(
        result.secondary_index_cleanup.diagnostic.diagnostic_code,
        result.secondary_index_cleanup.diagnostic.message_key,
        "secondary_index_garbage_cleanup",
        result.secondary_index_cleanup.fail_closed);
    return result;
  }
  result.secondary_index_cleaned =
      result.secondary_index_cleanup.after.cleaned_garbage_records != 0;

  page::OverflowLedger overflow_ledger = request.overflow_ledger;
  page::OverflowCleanupRequest overflow_request;
  overflow_request.authoritative_cleanup_horizon_local_transaction_id = horizon;
  overflow_request.cleanup_horizon_authoritative = true;
  overflow_request.reason = "mga_integrated_physical_cleanup";
  result.overflow_cleanup =
      page::CleanupOverflowValues(&overflow_ledger, overflow_request);
  if (!result.overflow_cleanup.status.ok()) {
    result.fail_closed = true;
    result.diagnostic = MakeEngineApiDiagnostic(
        result.overflow_cleanup.diagnostic.diagnostic_code,
        result.overflow_cleanup.diagnostic.message_key,
        "overflow_cleanup",
        true);
    return result;
  }
  result.overflow_cleaned = result.overflow_cleanup.cleaned;

  auto map_request = request.current_row_map_rebuild;
  result.current_row_map_rebuild =
      mga::RebuildCurrentRowMapFromAuthoritativeBaseRows(map_request);
  if (!result.current_row_map_rebuild.ok) {
    result.fail_closed = true;
    result.diagnostic = MakeEngineApiDiagnostic(
        result.current_row_map_rebuild.diagnostic_code,
        "mga.current_row_map.rebuild_refused",
        result.current_row_map_rebuild.refusal_reason,
        true);
    return result;
  }
  result.current_row_map_rebuilt =
      result.current_row_map_rebuild.rebuilt_entry_count != 0;

  auto observed = request.page_finality_observed;
  observed.transaction_horizon_authoritative = true;
  observed.transaction_inventory_authoritative = true;
  observed.normal_mga_visibility_authority_available = true;
  result.page_finality_cleanup = mga::EvaluatePageFinalityEvidence(
      request.page_finality_entry, observed, mga::PageFinalityConsumer::cleanup);
  result.exact_index_cleanup_authority =
      mga::EvaluateExactIndexCleanupAuthority(
          result.page_finality_cleanup,
          mga::MakeLocalTransactionId(horizon),
          true,
          true);
  if (!result.page_finality_cleanup.accepted ||
      !result.exact_index_cleanup_authority.accepted) {
    result.fail_closed = true;
    result.diagnostic = MakeEngineApiDiagnostic(
        "MGA.INTEGRATED_PHYSICAL_CLEANUP.PAGE_FINALITY_REFUSED",
        "mga.integrated_physical_cleanup.page_finality_refused",
        result.exact_index_cleanup_authority.refusal_reason,
        true);
    return result;
  }
  result.page_finality_accepted = true;

  page::PageAllocationLedger allocation_ledger = request.allocation_ledger;
  for (const auto& allocation_uuid : request.allocation_reclaim_uuids) {
    page::PageReleaseRequest release;
    release.allocation_uuid = allocation_uuid;
    release.cleanup_horizon_local_transaction_id = horizon;
    release.engine_mga_authoritative = true;
    release.reason = "mga_integrated_physical_cleanup";
    auto reclaimed =
        page::ReclaimReusablePageAllocation(&allocation_ledger, release);
    if (!reclaimed.ok() || !reclaimed.changed) {
      result.fail_closed = true;
      result.allocation_reclaims.push_back(reclaimed);
      result.diagnostic = MakeEngineApiDiagnostic(
          reclaimed.diagnostic.diagnostic_code,
          reclaimed.diagnostic.message_key,
          "page_allocation_reclaim",
          true);
      return result;
    }
    result.allocation_reclaimed = true;
    result.allocation_reclaims.push_back(std::move(reclaimed));
  }

  page::AllocationMapPageBody allocation_map = request.allocation_map;
  bool mutated_map = false;
  for (const auto& extent : request.allocation_map.extents) {
    if (!NeedsAllocationMapMutation(extent, horizon)) {
      continue;
    }
    page::AllocationMapPageBodyMutation mutation;
    mutation.kind = page::AllocationMapPageBodyMutationKind::replace_extent_state;
    mutation.extent = extent;
    mutation.extent.state = page::PageAllocationLifecycleState::reusable_free;
    const auto mutated = page::ApplyAllocationMapPageBodyMutation(
        allocation_map, mutation, request.allocation_map_page_size);
    if (!mutated.ok()) {
      result.fail_closed = true;
      result.allocation_map = mutated;
      result.diagnostic = MakeEngineApiDiagnostic(
          mutated.diagnostic.diagnostic_code,
          mutated.diagnostic.message_key,
          "allocation_map_extent_mutation",
          true);
      return result;
    }
    allocation_map = mutated.body;
    result.allocation_map = mutated;
    mutated_map = true;
  }
  if (!mutated_map) {
    result.fail_closed = true;
    result.diagnostic =
        MakeInvalidRequestDiagnostic("mga.integrated_physical_cleanup",
                                     "allocation_map_reclaimable_extent_required");
    return result;
  }
  result.allocation_map_mutated = true;

  for (auto reclaim_request : request.overflow_blob_reclaims) {
    reclaim_request.authoritative_cleanup_horizon_local_transaction_id = horizon;
    reclaim_request.cleanup_horizon_authoritative = true;
    const auto reclaim = page::ReclaimOverflowValueBlobPages(reclaim_request);
    if (!reclaim.ok()) {
      result.fail_closed = true;
      result.overflow_blob_reclaims.push_back(reclaim);
      result.diagnostic = MakeEngineApiDiagnostic(
          reclaim.diagnostic.diagnostic_code,
          reclaim.diagnostic.message_key,
          "overflow_blob_page_reclaim",
          true);
      return result;
    }
    result.overflow_pages_reclaimed = true;
    result.overflow_blob_reclaims.push_back(std::move(reclaim));
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  AddEvidence(&result, "row_data_physical_sweep", "applied");
  AddEvidence(&result, "mga_relation_physical_sweep", "applied");
  AddEvidence(&result, "secondary_index_garbage_cleanup", "applied");
  AddEvidence(&result, "overflow_cleanup", "applied");
  AddEvidence(&result, "current_row_map_rebuild", "applied");
  AddEvidence(&result, "page_finality_cleanup_evidence", "accepted");
  AddEvidence(&result, "allocation_map_extent_mutation", "applied");
  return result;
}

}  // namespace scratchbird::engine::internal_api
