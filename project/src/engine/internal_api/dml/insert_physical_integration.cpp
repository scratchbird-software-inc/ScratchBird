// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-INSERT-PHYSICAL-INTEGRATION-ANCHOR
#include "insert_physical_integration.hpp"

#include "api_diagnostics.hpp"
#include "bulk_constraint_proof.hpp"
#include "crud_support/crud_store.hpp"
#include "dml/constraint_enforcement.hpp"
#include "dml/dml_ingestion_pipeline.hpp"
#include "dml/index_apply_locality_bridge.hpp"
#include "dml/insert_batch.hpp"
#include "dml/page_allocation_runtime_bridge.hpp"
#include "dml/transactional_index_provider.hpp"
#include "dml/transactional_relation_store.hpp"
#include "dml/write_result_policy.hpp"
#include "bulk_placement_order.hpp"
#include "datatype_operations.hpp"
#include "domain_support/domain_store.hpp"
#include "ipar_fault_injection.hpp"
#include "metric_contracts.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "ordered_ingest.hpp"
#include "observability/dml_summary_counters.hpp"
#include "whole_store_crash_injection.hpp"
#include "security/deep_enforcement_api.hpp"
#include "index_bulk_publish_recovery.hpp"
#include "index_key_encoding.hpp"
#include "index_root_generation_publish.hpp"
#include "physical_mga_cow_store.hpp"
#include "sorted_bulk_index_build.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <numeric>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

namespace scratchbird::engine::internal_api::dml {

// SEARCH_KEY: SB_ENGINE_INSERT_PHYSICAL_AUTHORITY_COORDINATOR
// Owns page reservation/selection, filespace admission, overflow coordination,
// and strict-load integration. It consumes engine-owned transaction identity;
// it does not decide transaction finality or row-version visibility.
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
Status IntegrationOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status IntegrationErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

InsertPhysicalIntegrationResult Refuse(std::string diagnostic_code,
                                       std::string message_key,
                                       std::string detail) {
  InsertPhysicalIntegrationResult result;
  result.status = IntegrationErrorStatus();
  result.diagnostic = MakeInsertPhysicalIntegrationDiagnostic(result.status,
                                                             std::move(diagnostic_code),
                                                             std::move(message_key),
                                                             std::move(detail));
  return result;
}

std::string EvidenceRef(const std::string& kind, const TypedUuid& uuid) {
  std::ostringstream out;
  out << kind << ":" << scratchbird::core::uuid::UuidToString(uuid.value);
  return out.str();
}

}  // namespace

InsertPhysicalIntegrationResult ExecuteInsertPhysicalIntegration(
    InsertPhysicalIntegrationContext* context,
    const InsertPhysicalIntegrationRequest& request) {
  if (context == nullptr ||
      context->page_reservation_ledger == nullptr ||
      context->page_selection_ledger == nullptr) {
    return Refuse("insert_physical_integration_missing_page_authority",
                  "engine.insert.physical.missing_page_authority",
                  "page reservation and selection ledgers are required");
  }
  if (!request.database_uuid.valid() ||
      !request.object_uuid.valid() ||
      !request.transaction_uuid.valid() ||
      request.local_transaction_id == 0) {
    return Refuse("insert_physical_integration_invalid_identity",
                  "engine.insert.physical.invalid_identity",
                  "database, object, transaction UUIDs and local transaction ID are required");
  }

  InsertPhysicalIntegrationResult result;
  TypedUuid resolved_filespace_uuid = request.filespace_uuid;
  auto resolved_object_class = request.placement_object_class;
  auto resolved_growth_role = request.growth_filespace_role;

  const bool placement_resolution_required =
      request.require_placement_policy ||
      request.placement_policy.present ||
      request.placement_object_class !=
          scratchbird::storage::filespace::FilespaceObjectClass::unspecified;
  if (placement_resolution_required) {
    if (context->filespace_registry == nullptr) {
      return Refuse("insert_physical_integration_missing_placement_registry",
                    "engine.insert.physical.missing_placement_registry",
                    "filespace registry is required for placement policy resolution");
    }
    scratchbird::storage::filespace::FilespacePlacementRequest placement_request;
    placement_request.database_uuid = request.database_uuid;
    placement_request.preferred_filespace_uuid = request.filespace_uuid;
    placement_request.owner_object_uuid = request.object_uuid;
    placement_request.policy_uuid = request.policy_uuid;
    placement_request.object_class = request.placement_object_class;
    placement_request.page_family = request.page_family;
    placement_request.page_size = request.page_size;
    placement_request.require_preallocation =
        request.require_placement_preallocation;
    placement_request.requested_preallocation_pages =
        request.placement_preallocation_pages;
    placement_request.reason = "insert_physical_integration";
    placement_request.policy = request.placement_policy;
    const auto placement = scratchbird::storage::filespace::ResolveFilespacePlacement(
        *context->filespace_registry,
        placement_request);
    if (!placement.ok()) {
      return Refuse(placement.diagnostic.diagnostic_code,
                    placement.diagnostic.message_key,
                    "filespace placement refused");
    }
    resolved_filespace_uuid = placement.descriptor.filespace_uuid;
    resolved_object_class = placement.object_class;
    resolved_growth_role = placement.descriptor.role;
    result.filespace_placement_resolved = true;
    result.resolved_filespace_uuid = resolved_filespace_uuid;
    result.resolved_filespace_class =
        scratchbird::storage::filespace::FilespaceClassName(
            placement.filespace_class);
    result.resolved_filespace_role =
        scratchbird::storage::filespace::FilespaceRoleName(
            placement.descriptor.role);
    result.evidence_refs.push_back(EvidenceRef("filespace_placement",
                                               resolved_filespace_uuid));
    for (const auto& evidence : placement.evidence) {
      result.evidence_refs.push_back("filespace_placement:" + evidence);
    }

    if (placement.preallocation_required) {
      if (context->filespace_growth_ledger == nullptr) {
        return Refuse("insert_physical_integration_missing_preallocation_ledger",
                      "engine.insert.physical.missing_preallocation_ledger",
                      "filespace growth ledger is required for placement preallocation");
      }
      scratchbird::storage::filespace::FilespacePreallocationRequest preallocate;
      preallocate.request_uuid = request.request_id.valid()
                                     ? request.request_id
                                     : GeneratedId(UuidKind::object, 300010);
      preallocate.database_uuid = request.database_uuid;
      preallocate.filespace_uuid = resolved_filespace_uuid;
      preallocate.policy_uuid = request.policy_uuid;
      preallocate.storage_profile_uuid =
          placement.descriptor.writer_identity_uuid.valid()
              ? placement.descriptor.writer_identity_uuid
              : GeneratedId(UuidKind::object, 300011);
      preallocate.requested_page_count = placement.preallocation_page_count;
      preallocate.page_size_bytes = request.page_size;
      preallocate.policy_generation = 1;
      preallocate.observed_policy_generation = 1;
      preallocate.catalog_generation =
          placement.descriptor.generation == 0 ? 1 : placement.descriptor.generation;
      preallocate.observed_catalog_generation = preallocate.catalog_generation;
      preallocate.member_capacity.present = true;
      preallocate.member_capacity.explicit_capacity_context = true;
      preallocate.member_capacity.file_member_uuid =
          placement.descriptor.writer_identity_uuid.valid()
              ? placement.descriptor.writer_identity_uuid
              : GeneratedId(UuidKind::object, 300012);
      preallocate.member_capacity.start_page_number = 0;
      preallocate.member_capacity.current_page_count =
          placement.descriptor.total_pages;
      preallocate.member_capacity.preallocated_page_count =
          placement.descriptor.preallocated_pages;
      preallocate.member_capacity.maximum_page_count =
          placement.descriptor.total_pages +
          placement.descriptor.preallocated_pages +
          placement.preallocation_page_count + 1024;
      preallocate.member_capacity.physical_path = placement.descriptor.path;
      preallocate.member_capacity.online = true;
      preallocate.member_capacity.writable = !placement.descriptor.read_only;
      preallocate.transaction_context.present = true;
      preallocate.transaction_context.transaction_uuid = request.transaction_uuid;
      preallocate.transaction_context.transaction_number =
          request.local_transaction_id;
      preallocate.transaction_context.durable_inventory_admitted = true;
      preallocate.transaction_context.write_intent = true;
      preallocate.transaction_context.durability_fence_satisfied = true;
      preallocate.evidence_store_present = true;
      preallocate.evidence_before_success = true;
      preallocate.require_mga_transaction_context = true;
      preallocate.reason = "insert_physical_integration.placement_preallocation";
      const auto preallocated =
          scratchbird::storage::filespace::PreallocateFilespace(
              context->filespace_growth_ledger,
              *context->filespace_registry,
              preallocate);
      if (!preallocated.ok()) {
        return Refuse(preallocated.diagnostic.diagnostic_code,
                      preallocated.diagnostic.message_key,
                      "filespace placement preallocation refused");
      }
      result.filespace_preallocation_admitted = true;
      result.preallocation_operation_id =
          preallocated.operation.preallocation_operation_id;
      result.evidence_refs.push_back(EvidenceRef("filespace_preallocation",
                                                 result.preallocation_operation_id));
    }
  }

  scratchbird::storage::page::InsertPageReservationRequest reservation_request;
  reservation_request.database_uuid = request.database_uuid;
  reservation_request.transaction_uuid = request.transaction_uuid;
  reservation_request.local_transaction_id = request.local_transaction_id;
  reservation_request.object_uuid = request.object_uuid;
  reservation_request.page_family = request.page_family;
  reservation_request.estimated_row_count = request.estimated_row_count;
  reservation_request.estimated_payload_bytes = request.estimated_payload_bytes;
  reservation_request.preferred_filespace_uuid = resolved_filespace_uuid;
  reservation_request.policy_uuid = request.policy_uuid;
  reservation_request.request_id = request.request_id.valid() ? request.request_id : GeneratedId(UuidKind::object, 300000);
  reservation_request.object_class = resolved_object_class;
  reservation_request.page_size = request.page_size;
  reservation_request.current_time_authority_tick = request.time_authority_tick;
  reservation_request.lease_duration_ticks = request.reservation_lease_ticks;

  const auto reservation = scratchbird::storage::page::ReserveInsertPagesDurable(
      context->page_reservation_ledger,
      reservation_request);
  if (!reservation.ok()) {
    return Refuse(reservation.diagnostic.diagnostic_code,
                  reservation.diagnostic.message_key,
                  "page reservation refused");
  }
  result.page_reserved = true;
  result.reservation_id = reservation.reservation.reservation_id;
  result.evidence_refs.push_back(EvidenceRef("page_reservation", reservation.reservation.reservation_id));

  auto refuse_after_reservation = [&](std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail) {
    (void)scratchbird::storage::page::ReleaseInsertPageReservation(
        context->page_reservation_ledger,
        scratchbird::storage::page::ReleasePageReservationRequest{
            result.reservation_id,
            "insert_physical_integration_failure:" + diagnostic_code});
    return Refuse(std::move(diagnostic_code), std::move(message_key), std::move(detail));
  };

  scratchbird::storage::page::InsertPageSelectionRequest selection_request;
  selection_request.database_uuid = request.database_uuid;
  selection_request.transaction_uuid = request.transaction_uuid;
  selection_request.local_transaction_id = request.local_transaction_id;
  selection_request.object_uuid = request.object_uuid;
  selection_request.reservation_id = reservation.reservation.reservation_id;
  selection_request.page_family = request.page_family;
  selection_request.encoded_row_bytes = request.encoded_row_bytes;

  auto selection = scratchbird::storage::page::SelectInsertTargetPage(
      context->page_selection_ledger,
      context->page_reservation_ledger,
      selection_request);
  if (!selection.ok() && request.request_filespace_growth_on_missing_page) {
    if (context->filespace_growth_ledger == nullptr || context->filespace_registry == nullptr) {
      return refuse_after_reservation("insert_physical_integration_missing_filespace_authority",
                                      "engine.insert.physical.missing_filespace_authority",
                                      "filespace growth ledger and registry are required for growth admission");
    }
    scratchbird::storage::filespace::InsertFilespaceGrowthRequest growth_request;
    growth_request.database_uuid = request.database_uuid;
    growth_request.filespace_uuid = resolved_filespace_uuid;
    growth_request.filespace_role = resolved_growth_role;
    growth_request.page_family = request.page_family;
    growth_request.requested_page_count = request.growth_page_count == 0 ? 1 : request.growth_page_count;
    growth_request.urgency_class = request.growth_urgency;
    growth_request.predicted_insert_pressure_pages = request.predicted_insert_pressure_pages;
    growth_request.policy_uuid = request.policy_uuid;
    const auto growth = scratchbird::storage::filespace::RequestInsertFilespaceGrowth(
        context->filespace_growth_ledger,
        *context->filespace_registry,
        growth_request);
    if (!growth.ok()) {
      return refuse_after_reservation(growth.diagnostic.diagnostic_code,
                                      growth.diagnostic.message_key,
                                      "filespace growth admission refused");
    }
    result.filespace_growth_admitted = true;
    result.growth_operation_id = growth.growth_operation_id;
    result.evidence_refs.push_back(EvidenceRef("filespace_growth", growth.growth_operation_id));
    (void)scratchbird::storage::page::ReleaseInsertPageReservation(
        context->page_reservation_ledger,
        scratchbird::storage::page::ReleasePageReservationRequest{
            result.reservation_id,
            "insert_physical_integration_growth_admitted_without_append"});
    result.status = IntegrationOkStatus();
    result.integrated = false;
    result.diagnostic = MakeInsertPhysicalIntegrationDiagnostic(result.status,
                                                               "ok",
                                                               "engine.insert.physical.growth_admitted",
                                                               "filespace growth admitted; row was not physically integrated");
    return result;
  } else if (!selection.ok()) {
    return refuse_after_reservation(selection.diagnostic.diagnostic_code,
                                    selection.diagnostic.message_key,
                                    "page selection refused");
  } else {
    result.page_selected = true;
    result.selection_fence = selection.selection.selection_fence;
    result.evidence_refs.push_back("page_selection:" + selection.selection.selection_fence);
  }

  if (request.enable_deferred_secondary_index) {
    if (!(request.deferred_index_overlay_gate &&
          request.deferred_index_merge_gate &&
          request.deferred_index_cleanup_gate &&
          request.deferred_index_recovery_gate)) {
      return refuse_after_reservation("insert_physical_integration_deferred_index_gates_unproven",
                                      "engine.insert.physical.deferred_index_gates_unproven",
                                      "deferred secondary-index maintenance requires overlay, merge, cleanup, and recovery gates");
    }
    if (context->secondary_index_overlay_ledger == nullptr ||
        context->secondary_index_merge_ledger == nullptr ||
        context->secondary_index_base_entries == nullptr ||
        context->secondary_index_delta_ledger == nullptr ||
        !request.secondary_index_uuid.valid()) {
      return refuse_after_reservation("insert_physical_integration_missing_index_authority",
                                      "engine.insert.physical.missing_index_authority",
                                      "secondary-index overlay, merge, base index, and delta ledgers are required");
    }
    scratchbird::core::index::SecondaryIndexOverlayRequest overlay_request;
    overlay_request.index_uuid = request.secondary_index_uuid;
    overlay_request.table_uuid = request.object_uuid;
    overlay_request.transaction_uuid = request.transaction_uuid;
    overlay_request.local_transaction_id = request.local_transaction_id;
    overlay_request.snapshot_high_water_local_transaction_id =
        request.secondary_index_snapshot_high_water_local_transaction_id;
    const auto overlay = scratchbird::core::index::BuildSecondaryIndexDeltaOverlay(
        context->secondary_index_overlay_ledger,
        *context->secondary_index_base_entries,
        *context->secondary_index_delta_ledger,
        overlay_request);
    if (!overlay.ok()) {
      return refuse_after_reservation(overlay.diagnostic.diagnostic_code,
                                      overlay.diagnostic.message_key,
                                      "secondary-index overlay refused");
    }
    scratchbird::core::index::SecondaryIndexMergeRequest merge_request;
    merge_request.index_uuid = request.secondary_index_uuid;
    merge_request.table_uuid = request.object_uuid;
    merge_request.merge_id = GeneratedId(UuidKind::object, 300001);
    merge_request.authoritative_cleanup_horizon_local_transaction_id =
        request.secondary_index_cleanup_horizon_local_transaction_id;
    merge_request.cleanup_horizon_authoritative = true;
    const auto merge = scratchbird::core::index::MergeSecondaryIndexDeltas(
        context->secondary_index_merge_ledger,
        context->secondary_index_base_entries,
        context->secondary_index_delta_ledger,
        merge_request);
    if (!merge.ok()) {
      return refuse_after_reservation(merge.diagnostic.diagnostic_code,
                                      merge.diagnostic.message_key,
                                      "secondary-index merge refused");
    }
    const auto recovery = scratchbird::core::index::ClassifySecondaryIndexMergeLedgerForRecovery(
        *context->secondary_index_merge_ledger);
    if (!recovery.ok()) {
      return refuse_after_reservation(recovery.diagnostic.diagnostic_code,
                                      recovery.diagnostic.message_key,
                                      "secondary-index recovery classification refused");
    }
    result.deferred_secondary_index_verified = true;
    result.evidence_refs.push_back(EvidenceRef("secondary_index_merge", merge.evidence.evidence_id));
  }

  if (request.persist_overflow_payload) {
    if (context->overflow_ledger == nullptr) {
      return refuse_after_reservation("insert_physical_integration_missing_overflow_authority",
                                      "engine.insert.physical.missing_overflow_authority",
                                      "overflow ledger is required");
    }
    scratchbird::storage::page::OverflowPersistRequest overflow_request;
    overflow_request.row_uuid = request.row_uuid;
    overflow_request.object_uuid = request.object_uuid;
    overflow_request.transaction_uuid = request.transaction_uuid;
    overflow_request.local_transaction_id = request.local_transaction_id;
    overflow_request.value_descriptor = request.overflow_value_descriptor;
    overflow_request.payload_bytes = request.overflow_payload;
    overflow_request.chunk_policy_uuid = request.policy_uuid;
    overflow_request.chunk_size = request.overflow_chunk_size;
    const auto overflow = scratchbird::storage::page::PersistOverflowValue(context->overflow_ledger, overflow_request);
    if (!overflow.ok()) {
      return refuse_after_reservation(overflow.diagnostic.diagnostic_code,
                                      overflow.diagnostic.message_key,
                                      "overflow persistence refused");
    }
    const auto commit = scratchbird::storage::page::CommitOverflowValue(
        context->overflow_ledger,
        scratchbird::storage::page::OverflowCommitRequest{
            overflow.overflow_value_uuid,
            request.transaction_uuid,
            request.local_transaction_id,
            "insert_physical_integration"});
    if (!commit.ok()) {
      return refuse_after_reservation(commit.diagnostic.diagnostic_code,
                                      commit.diagnostic.message_key,
                                      "overflow commit refused");
    }
    result.overflow_persisted = true;
    result.overflow_value_uuid = overflow.overflow_value_uuid;
    result.evidence_refs.push_back(EvidenceRef("overflow", overflow.overflow_value_uuid));
  }

  if (request.run_strict_bulk_load) {
    if (context->strict_bulk_load_ledger == nullptr) {
      return refuse_after_reservation("insert_physical_integration_missing_bulk_load_authority",
                                      "engine.insert.physical.missing_bulk_load_authority",
                                      "strict bulk-load ledger is required");
    }
    scratchbird::core::bulk_load::StrictBulkLoadBeginRequest begin_request;
    begin_request.database_uuid = request.database_uuid;
    begin_request.object_uuid = request.object_uuid;
    begin_request.transaction_uuid = request.transaction_uuid;
    begin_request.local_transaction_id = request.local_transaction_id;
    begin_request.policy = request.strict_bulk_load_policy;
    begin_request.staging_target = request.strict_bulk_load_staging_target;
    const auto begin = scratchbird::core::bulk_load::BeginStrictBulkLoad(
        context->strict_bulk_load_ledger,
        begin_request);
    if (!begin.ok()) {
      return refuse_after_reservation(begin.diagnostic.diagnostic_code,
                                      begin.diagnostic.message_key,
                                      "strict bulk-load begin refused");
    }
    const auto append = scratchbird::core::bulk_load::AppendStrictBulkLoadRows(
        context->strict_bulk_load_ledger,
        scratchbird::core::bulk_load::StrictBulkLoadAppendRequest{
            begin.operation.bulk_load_id,
            request.transaction_uuid,
            request.local_transaction_id,
            request.strict_bulk_load_rows});
    if (!append.ok()) {
      return refuse_after_reservation(append.diagnostic.diagnostic_code,
                                      append.diagnostic.message_key,
                                      "strict bulk-load append refused");
    }
    const auto finalize = scratchbird::core::bulk_load::FinalizeStrictBulkLoad(
        context->strict_bulk_load_ledger,
        scratchbird::core::bulk_load::StrictBulkLoadFinalizeRequest{
            begin.operation.bulk_load_id,
            request.transaction_uuid,
            request.local_transaction_id,
            false,
            "insert-physical-strict-bulk-load-fence"});
    if (!finalize.ok()) {
      return refuse_after_reservation(finalize.diagnostic.diagnostic_code,
                                      finalize.diagnostic.message_key,
                                      "strict bulk-load finalize refused");
    }
    result.strict_bulk_load_finalized = true;
    result.strict_bulk_load_id = begin.operation.bulk_load_id;
    result.evidence_refs.push_back(EvidenceRef("strict_bulk_load", begin.operation.bulk_load_id));
  }

  if (result.page_selected) {
    const auto append = scratchbird::storage::page::AppendRowToSelectedPageWithReservationLedger(
        context->page_selection_ledger,
        context->page_reservation_ledger,
        scratchbird::storage::page::InsertPageAppendRequest{
            result.selection_fence,
            request.encoded_row_bytes,
            "insert_physical_integration"});
    if (!append.ok()) {
      return refuse_after_reservation(append.diagnostic.diagnostic_code,
                                      append.diagnostic.message_key,
                                      "selected page append refused");
    }
  }

  result.status = IntegrationOkStatus();
  result.integrated = result.page_selected;
  result.diagnostic = MakeInsertPhysicalIntegrationDiagnostic(result.status,
                                                            "ok",
                                                            "engine.insert.physical.integrated",
                                                            "insert physical integration completed");
  return result;
}

DiagnosticRecord MakeInsertPhysicalIntegrationDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(status.code,
                                                     status.severity,
                                                     status.subsystem,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(arguments),
                                                     {},
                                                     "engine.insert.physical_integration",
                                                     status.ok() ? "" : "fall back to existing safe insert path and do not claim physical integration success");
}

}  // namespace scratchbird::engine::internal_api::dml
