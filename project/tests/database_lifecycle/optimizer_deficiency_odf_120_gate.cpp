// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-120 backup/restore/validate/repair closure gate for optimized persisted
// structures.

#include "backup_archive/backup_archive_api.hpp"
#include "index_backup_restore.hpp"
#include "index_validation_repair_tooling.hpp"
#include "management/support_bundle_api.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "OPTIMIZER_DEFICIENCY_ODF_120_BACKUP_REPAIR_GATE";

struct StructureContract {
  idx::OptimizedPersistedStructureKind structure;
  std::string feature_key;
  std::string artifact_kind;
  idx::IndexFamily movement_family = idx::IndexFamily::unknown;
  idx::OptimizedStructureLifecycleAction expected_restore_action =
      idx::OptimizedStructureLifecycleAction::validate_then_use;
  idx::OptimizedStructureLifecycleOperation maintenance_operation =
      idx::OptimizedStructureLifecycleOperation::validate;
  bool authoritative_base_required = false;
  bool preserve_published_generation = false;
  bool unpublished_generation_present = false;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool LooksLikeUuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (const std::size_t index : {8u, 13u, 18u, 23u}) {
    if (value[index] != '-') {
      return false;
    }
  }
  return true;
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

struct UuidFactory {
  platform::u64 base_millis = NowMillis();

  platform::TypedUuid Typed(platform::UuidKind kind, platform::u64 salt) const {
    const auto generated =
        uuid::GenerateEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "ODF-120 UUID generation failed");
    return generated.value;
  }

  std::string Text(platform::UuidKind kind, platform::u64 salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

const std::vector<StructureContract>& Structures() {
  static const std::vector<StructureContract> structures = {
      {idx::OptimizedPersistedStructureKind::page_extent_summaries,
       "optimizer.persisted.page_extent_summaries",
       "dpc_page_extent_summary",
       idx::IndexFamily::brin_zone,
       idx::OptimizedStructureLifecycleAction::rebuild_from_authoritative_base,
       idx::OptimizedStructureLifecycleOperation::rebuild,
       true,
       false,
       false},
      {idx::OptimizedPersistedStructureKind::secondary_index_delta_ledgers,
       "optimizer.persisted.secondary_index_delta_ledgers",
       "dpc_secondary_index_delta_ledger",
       idx::IndexFamily::btree,
       idx::OptimizedStructureLifecycleAction::preserve_committed,
       idx::OptimizedStructureLifecycleOperation::repair,
       false,
       true,
       false},
      {idx::OptimizedPersistedStructureKind::deferred_index_merge_state,
       "optimizer.persisted.deferred_index_merge_state",
       "dpc_deferred_index_merge_state",
       idx::IndexFamily::btree,
       idx::OptimizedStructureLifecycleAction::rebuild_from_authoritative_base,
       idx::OptimizedStructureLifecycleOperation::rebuild,
       true,
       false,
       false},
      {idx::OptimizedPersistedStructureKind::cleanup_horizon_markers,
       "optimizer.persisted.cleanup_horizon_markers",
       "dpc_cleanup_horizon_marker",
       idx::IndexFamily::temporary_work,
       idx::OptimizedStructureLifecycleAction::recompute_from_mga_inventory,
       idx::OptimizedStructureLifecycleOperation::rebuild,
       true,
       false,
       false},
      {idx::OptimizedPersistedStructureKind::shadow_index_build_state,
       "optimizer.persisted.shadow_index_build_state",
       "dpc_shadow_index_build_state",
       idx::IndexFamily::btree,
       idx::OptimizedStructureLifecycleAction::discard_unpublished_then_fallback,
       idx::OptimizedStructureLifecycleOperation::repair,
       false,
       false,
       true},
      {idx::OptimizedPersistedStructureKind::search_inverted_segments,
       "optimizer.persisted.search_inverted_segments",
       "dpc_search_inverted_segment",
       idx::IndexFamily::full_text,
       idx::OptimizedStructureLifecycleAction::preserve_committed,
       idx::OptimizedStructureLifecycleOperation::repair,
       false,
       true,
       false},
      {idx::OptimizedPersistedStructureKind::vector_generations,
       "optimizer.persisted.vector_generations",
       "dpc_vector_generation",
       idx::IndexFamily::vector_ivf,
       idx::OptimizedStructureLifecycleAction::preserve_committed,
       idx::OptimizedStructureLifecycleOperation::repair,
       false,
       true,
       false},
      {idx::OptimizedPersistedStructureKind::optimization_management_metadata,
       "optimizer.persisted.management_metadata",
       "dpc_optimization_management_metadata",
       idx::IndexFamily::in_memory,
       idx::OptimizedStructureLifecycleAction::rebuild_management_projection,
       idx::OptimizedStructureLifecycleOperation::rebuild,
       true,
       false,
       false},
  };
  return structures;
}

idx::IndexMovementValidationRequest MovementRequest(const UuidFactory& uuids,
                                                    const StructureContract& contract,
                                                    platform::u64 salt,
                                                    idx::IndexMovementOperation operation) {
  idx::IndexMovementValidationRequest request;
  request.operation = operation;
  request.family = contract.movement_family;
  request.resource_available = true;
  request.transaction_finality_proven = true;
  request.destination_supports_family = true;
  request.page_authority.expected_index_uuid =
      uuids.Typed(platform::UuidKind::object, salt + 1);
  request.page_authority.observed_index_uuid =
      request.page_authority.expected_index_uuid;
  request.page_authority.expected_family = contract.movement_family;
  request.page_authority.observed_family = contract.movement_family;
  request.page_authority.expected_resource_epoch = 120;
  request.page_authority.observed_resource_epoch = 120;
  request.page_authority.checksum_valid = true;
  request.page_authority.page_type_supported = true;
  return request;
}

idx::OptimizedStructureLifecycleRequest LifecycleRequest(
    const UuidFactory& uuids,
    const StructureContract& contract,
    idx::OptimizedStructureLifecycleOperation operation,
    platform::u64 salt) {
  idx::OptimizedStructureLifecycleRequest request;
  request.structure = contract.structure;
  request.operation = operation;
  request.movement_validation_required =
      operation == idx::OptimizedStructureLifecycleOperation::backup ||
      operation == idx::OptimizedStructureLifecycleOperation::restore;
  request.movement = MovementRequest(
      uuids,
      contract,
      salt,
      operation == idx::OptimizedStructureLifecycleOperation::restore
          ? idx::IndexMovementOperation::restore
          : idx::IndexMovementOperation::backup);
  request.manifest_coverage_verified = true;
  request.restore_inspection_open =
      operation == idx::OptimizedStructureLifecycleOperation::restore;
  request.transaction_finality_proven_by_mga_inventory = true;
  request.authoritative_base_available = contract.authoritative_base_required;
  request.support_bundle_evidence_sink_available = true;
  request.repair_mutation_allowed = true;
  request.target_identity_resolved_to_generated_uuids = true;
  request.checksum_valid = true;
  request.structure_supported = true;
  request.published_or_committed_generation =
      contract.preserve_published_generation;
  request.unpublished_generation_present =
      contract.unpublished_generation_present;
  return request;
}

bool HasEvidence(const idx::OptimizedStructureLifecycleResult& result,
                 std::string_view key,
                 std::string_view value = {}) {
  for (const auto& evidence : result.support_evidence) {
    if (evidence.key == key && (value.empty() || evidence.value == value)) {
      return true;
    }
  }
  return false;
}

idx::OptimizedStructureLifecycleAction ExpectedActionForOperation(
    const StructureContract& contract,
    idx::OptimizedStructureLifecycleOperation operation) {
  if (operation == idx::OptimizedStructureLifecycleOperation::validate &&
      contract.expected_restore_action ==
          idx::OptimizedStructureLifecycleAction::preserve_committed) {
    return idx::OptimizedStructureLifecycleAction::validate_then_use;
  }
  return contract.expected_restore_action;
}

bool HasRepairEvidence(const idx::IndexValidationRepairResult& result,
                       std::string_view key,
                       std::string_view value = {}) {
  for (const auto& evidence : result.support_evidence) {
    if (evidence.key == key && (value.empty() || evidence.value == value)) {
      return true;
    }
  }
  return false;
}

void RequireNoForbiddenPayload(std::string_view payload) {
  for (const auto token : {"docs" "/execution-plans",
                           "docs" "/findings",
                           "public_audit_summary",
                           "public_release_evidence",
                           "docs/references",
                           "parser_finality_authority\":true",
                           "donor_finality_authority\":true",
                           "wal_recovery_authority\":true"}) {
    Require(!Contains(payload, token),
            "ODF-120 payload contains forbidden runtime token");
  }
}

void RequireLifecycleResult(const idx::OptimizedStructureLifecycleResult& result,
                            idx::OptimizedStructureLifecycleAction action,
                            std::string_view context) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << ':'
              << result.diagnostic.message_key << '\n';
  }
  Require(result.ok(), context);
  Require(result.action == action, context);
  Require(result.support_bundle_evidence_recorded, context);
  Require(result.engine_mga_authority_preserved, context);
  Require(HasEvidence(result, "finality_source",
                      "local_mga_transaction_inventory"),
          "ODF-120 lifecycle result omitted MGA finality evidence");
  Require(HasEvidence(result, "authoritative_wal", "false"),
          "ODF-120 lifecycle result omitted anti-WAL evidence");
  for (const auto& evidence : result.support_evidence) {
    RequireNoForbiddenPayload(evidence.key);
    RequireNoForbiddenPayload(evidence.value);
  }
}

void ProveLifecyclePolicyForAllStructures() {
  const UuidFactory uuids;
  const auto& structures = Structures();
  Require(structures.size() == 8,
          "ODF-120 must cover all ODF-118 optimized persisted structures");
  std::set<std::string> features;
  std::set<std::string> artifacts;
  std::set<std::string> names;

  platform::u64 salt = 1000;
  for (const auto& structure : structures) {
    const std::string name =
        idx::OptimizedPersistedStructureKindName(structure.structure);
    Require(names.insert(name).second, "ODF-120 duplicate structure name");
    Require(features.insert(structure.feature_key).second,
            "ODF-120 duplicate feature key");
    Require(artifacts.insert(structure.artifact_kind).second,
            "ODF-120 duplicate artifact kind");
    Require(!LooksLikeUuid(structure.feature_key) &&
                !LooksLikeUuid(structure.artifact_kind) &&
                !LooksLikeUuid(name),
            "ODF-120 structure contract contains hard-coded UUID-looking key");

    const auto backup = idx::EvaluateOptimizedStructureLifecycle(
        LifecycleRequest(uuids,
                         structure,
                         idx::OptimizedStructureLifecycleOperation::backup,
                         salt));
    RequireLifecycleResult(backup,
                           structure.expected_restore_action,
                           "ODF-120 backup lifecycle refused structure");
    Require(backup.survived_backup_restore,
            "ODF-120 backup did not classify structure as survivable");

    const auto restore = idx::EvaluateOptimizedStructureLifecycle(
        LifecycleRequest(uuids,
                         structure,
                         idx::OptimizedStructureLifecycleOperation::restore,
                         salt + 10));
    RequireLifecycleResult(restore,
                           structure.expected_restore_action,
                           "ODF-120 restore lifecycle refused structure");
    Require(restore.survived_backup_restore,
            "ODF-120 restore did not classify structure as survivable");

    const auto validate = idx::EvaluateOptimizedStructureLifecycle(
        LifecycleRequest(uuids,
                         structure,
                         idx::OptimizedStructureLifecycleOperation::validate,
                         salt + 20));
    RequireLifecycleResult(
        validate,
        ExpectedActionForOperation(
            structure,
            idx::OptimizedStructureLifecycleOperation::validate),
        "ODF-120 validate lifecycle refused structure");

    const auto maintenance = idx::EvaluateOptimizedStructureLifecycle(
        LifecycleRequest(uuids,
                         structure,
                         structure.maintenance_operation,
                         salt + 30));
    RequireLifecycleResult(maintenance,
                           structure.expected_restore_action,
                           "ODF-120 repair/rebuild lifecycle refused structure");

    salt += 100;
  }
}

void ProveExactRefusalDiagnostics() {
  const UuidFactory uuids;
  const auto& first = Structures().front();

  auto no_finality = LifecycleRequest(
      uuids, first, idx::OptimizedStructureLifecycleOperation::restore, 8000);
  no_finality.transaction_finality_proven_by_mga_inventory = false;
  const auto no_finality_result =
      idx::EvaluateOptimizedStructureLifecycle(no_finality);
  Require(!no_finality_result.ok() && no_finality_result.exact_refusal,
          "ODF-120 missing MGA finality did not refuse exactly");
  Require(no_finality_result.diagnostic.diagnostic_code ==
              "ODF.OPT_STRUCTURE.MGA_FINALITY_REQUIRED",
          "ODF-120 missing MGA finality diagnostic changed");
  Require(HasEvidence(no_finality_result, "support_bundle_evidence_recorded",
                      "true"),
          "ODF-120 refusal omitted support-bundle evidence");

  auto no_inspection = LifecycleRequest(
      uuids, first, idx::OptimizedStructureLifecycleOperation::restore, 8100);
  no_inspection.restore_inspection_open = false;
  const auto no_inspection_result =
      idx::EvaluateOptimizedStructureLifecycle(no_inspection);
  Require(!no_inspection_result.ok() && no_inspection_result.exact_refusal,
          "ODF-120 missing restore inspection did not refuse exactly");
  Require(no_inspection_result.diagnostic.diagnostic_code ==
              "ODF.OPT_STRUCTURE.RESTORE_INSPECTION_REQUIRED",
          "ODF-120 restore inspection diagnostic changed");

  auto no_support = LifecycleRequest(
      uuids, first, idx::OptimizedStructureLifecycleOperation::backup, 8200);
  no_support.support_bundle_evidence_sink_available = false;
  const auto no_support_result =
      idx::EvaluateOptimizedStructureLifecycle(no_support);
  Require(!no_support_result.ok() && no_support_result.exact_refusal,
          "ODF-120 missing support bundle did not refuse exactly");
  Require(no_support_result.diagnostic.diagnostic_code ==
              "ODF.OPT_STRUCTURE.SUPPORT_BUNDLE_REQUIRED",
          "ODF-120 support-bundle diagnostic changed");

  auto no_rebuild_base = LifecycleRequest(
      uuids, first, idx::OptimizedStructureLifecycleOperation::rebuild, 8300);
  no_rebuild_base.authoritative_base_available = false;
  const auto no_rebuild_base_result =
      idx::EvaluateOptimizedStructureLifecycle(no_rebuild_base);
  Require(!no_rebuild_base_result.ok() && no_rebuild_base_result.exact_refusal,
          "ODF-120 missing rebuild evidence did not refuse exactly");
  Require(no_rebuild_base_result.diagnostic.diagnostic_code ==
              "ODF.OPT_STRUCTURE.REBUILD_EVIDENCE_REQUIRED",
          "ODF-120 rebuild evidence diagnostic changed");

  auto unresolved = LifecycleRequest(
      uuids, first, idx::OptimizedStructureLifecycleOperation::validate, 8400);
  unresolved.target_identity_resolved_to_generated_uuids = false;
  const auto unresolved_result =
      idx::EvaluateOptimizedStructureLifecycle(unresolved);
  Require(!unresolved_result.ok() && unresolved_result.exact_refusal,
          "ODF-120 unresolved identity did not refuse exactly");
  Require(unresolved_result.diagnostic.diagnostic_code ==
              "ODF.OPT_STRUCTURE.IDENTITY_REFUSED",
          "ODF-120 unresolved identity diagnostic changed");
}

idx::IndexValidationRepairTarget RepairTarget(const UuidFactory& uuids,
                                              platform::u64 salt,
                                              idx::IndexFamily family) {
  idx::IndexValidationRepairTarget target;
  target.database_uuid = uuids.Typed(platform::UuidKind::database, salt + 1);
  target.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  target.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  target.generation_uuid = uuids.Typed(platform::UuidKind::object, salt + 4);
  target.physical_family = family;
  target.names_resolved_to_uuids = true;
  target.catalog_resolution_proven = true;
  target.contains_sql_text = false;
  return target;
}

idx::IndexValidationRepairRequest RepairRequest(
    const UuidFactory& uuids,
    platform::u64 salt,
    idx::IndexValidationRepairFamily family,
    idx::IndexValidationRepairOperation operation,
    idx::IndexFamily physical_family = idx::IndexFamily::btree) {
  idx::IndexValidationRepairRequest request;
  request.validation_family = family;
  request.operation = operation;
  request.target = RepairTarget(uuids, salt, physical_family);
  request.policy_allows_mutation =
      idx::IndexValidationRepairOperationMutates(operation);
  return request;
}

idx::PageExtentSummaryFormatCompatibility CurrentPageSummaryFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryFormatCompatibility format;
  format.observed = contract.current;
  format.open_class = idx::PageExtentSummaryFormatOpenClass::current;
  format.compatible = true;
  format.migration_required = false;
  format.diagnostic_code = "ODF-120.current_page_summary_format";
  return format;
}

idx::PageExtentSummaryMetadata MissingPageSummary(const UuidFactory& uuids,
                                                  platform::u64 salt) {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryMetadata metadata;
  metadata.relation_uuid = uuids.Text(platform::UuidKind::object, salt + 1);
  metadata.summary_uuid = uuids.Text(platform::UuidKind::object, salt + 2);
  metadata.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  metadata.range.first_page_id = 10;
  metadata.range.page_count = 4;
  metadata.boundary.scalar_type_key = "int64_lex";
  metadata.boundary.encoded_min = "010";
  metadata.boundary.encoded_max = "030";
  metadata.boundary.min_present = true;
  metadata.boundary.max_present = true;
  metadata.status = idx::PageExtentSummaryStatus::missing;
  metadata.format_version = contract.current;
  metadata.checksum_valid = true;
  return metadata;
}

idx::PageExtentSummaryMaintenanceEvent RebuildPageSummaryEvent(
    const idx::PageExtentSummaryMetadata& metadata) {
  idx::PageExtentSummaryMaintenanceEvent event;
  event.kind = idx::PageExtentSummaryMaintenanceEventKind::rebuild;
  event.relation_uuid = metadata.relation_uuid;
  event.summary_uuid = metadata.summary_uuid;

  idx::PageExtentSummaryRowEvidence first;
  first.page_id = 11;
  first.extent_id = 0;
  first.scalar_type_key = "int64_lex";
  first.encoded_scalar = "011";
  first.engine_mga_visible = true;
  event.base_page_rows.push_back(std::move(first));

  idx::PageExtentSummaryRowEvidence second;
  second.page_id = 12;
  second.extent_id = 0;
  second.scalar_type_key = "int64_lex";
  second.encoded_scalar = "025";
  second.engine_mga_visible = true;
  event.base_page_rows.push_back(std::move(second));
  event.caller_allows_transient_rebuild = true;
  return event;
}

idx::SecondaryIndexDeltaLedgerRecord DeltaRecord(
    const UuidFactory& uuids,
    const idx::IndexValidationRepairTarget& target,
    platform::u64 salt) {
  idx::SecondaryIndexDeltaLedgerRecord record;
  record.delta.delta_id = uuids.Typed(platform::UuidKind::object, salt + 1);
  record.delta.index_uuid = target.index_uuid;
  record.delta.table_uuid = target.table_uuid;
  record.delta.row_uuid = uuids.Typed(platform::UuidKind::row, salt + 2);
  record.delta.version_uuid = uuids.Typed(platform::UuidKind::row, salt + 3);
  record.delta.transaction_uuid =
      uuids.Typed(platform::UuidKind::transaction, salt + 4);
  record.delta.local_transaction_id = salt + 20;
  record.delta.delta_kind = idx::SecondaryIndexDeltaKind::insert;
  record.delta.key_payload = "odf120:key:" + std::to_string(salt);
  record.delta.cleanup_horizon_token = "odf120:horizon:" + std::to_string(salt);
  record.delta.committed = true;
  record.commit_state =
      idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge;
  record.source_evidence_reference = "odf120:delta:" + std::to_string(salt);
  return record;
}

idx::ShadowIndexBuildRecord ShadowRecord(const UuidFactory& uuids,
                                         platform::u64 salt) {
  idx::ShadowIndexBuildRecord record;
  record.build_id = uuids.Typed(platform::UuidKind::object, salt + 1);
  record.shadow_index_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  record.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  record.validation_evidence_present = true;
  record.validation_evidence_ref = "odf120:validation:shadow";
  record.engine_mga_inventory_evidence_ref = "odf120:mga_inventory:shadow";
  record.engine_mga_horizon_evidence_ref = "odf120:mga_horizon:shadow";
  record.state = idx::ShadowIndexBuildState::validated;
  return record;
}

idx::InvertedSearchSegmentDescriptor SearchSegment(const UuidFactory& uuids,
                                                   platform::u64 salt) {
  idx::InvertedSearchSegmentDescriptor segment;
  segment.segment_uuid = uuids.Typed(platform::UuidKind::object, salt + 1);
  segment.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  segment.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  segment.generation = salt;
  segment.state = idx::InvertedSearchSegmentState::building;
  segment.visible = false;
  segment.persisted_record_present = false;
  segment.checksum_valid = true;
  segment.validation_evidence_ref = "odf120:validation:search";
  segment.engine_mga_inventory_evidence_ref = "odf120:mga_inventory:search";
  segment.engine_mga_horizon_evidence_ref = "odf120:mga_horizon:search";
  return segment;
}

idx::VectorGenerationDescriptor VectorGeneration(const UuidFactory& uuids,
                                                 platform::u64 salt) {
  idx::VectorGenerationDescriptor generation;
  generation.generation_uuid = uuids.Typed(platform::UuidKind::object, salt + 1);
  generation.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  generation.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  generation.generation = salt;
  generation.state = idx::VectorGenerationState::building;
  generation.training_evidence_present = true;
  generation.training_evidence_ref = "odf120:training:vector";
  generation.engine_mga_inventory_evidence_ref = "odf120:mga_inventory:vector";
  generation.engine_mga_horizon_evidence_ref = "odf120:mga_horizon:vector";
  generation.resource_envelope.resource_governor_evidence_present = true;
  generation.resource_envelope.resource_governor_evidence_ref =
      "odf120:resource:vector";
  generation.resource_envelope.memory_limit_bytes = 1024;
  generation.resource_envelope.memory_observed_bytes = 256;
  generation.resource_envelope.temp_space_limit_bytes = 2048;
  generation.resource_envelope.temp_space_observed_bytes = 512;
  generation.resource_envelope.worker_limit = 2;
  generation.resource_envelope.workers_used = 1;
  return generation;
}

void ProveRepairToolingEvidence() {
  const UuidFactory uuids;

  auto page = RepairRequest(uuids,
                            10000,
                            idx::IndexValidationRepairFamily::page_extent_summary,
                            idx::IndexValidationRepairOperation::rebuild,
                            idx::IndexFamily::brin_zone);
  page.state.page_extent_summary = MissingPageSummary(uuids, 10010);
  page.state.page_extent_summary_format = CurrentPageSummaryFormat();
  page.state.page_extent_rebuild_event =
      RebuildPageSummaryEvent(page.state.page_extent_summary);
  const auto page_result = idx::ExecuteIndexValidationRepairOperation(page);
  Require(page_result.ok() &&
              page_result.diagnostic.diagnostic_code ==
                  "DPC.INDEX_REPAIR.PAGE_SUMMARY_REBUILT",
          "ODF-120 page summary rebuild proof failed");
  Require(HasRepairEvidence(page_result, "page_summary.rebuild_classification"),
          "ODF-120 page summary rebuild support evidence missing");

  auto delta =
      RepairRequest(uuids,
                    10100,
                    idx::IndexValidationRepairFamily::secondary_delta_ledger,
                    idx::IndexValidationRepairOperation::repair);
  delta.state.delta_ledger.records.push_back(
      DeltaRecord(uuids, delta.target, 10110));
  const auto delta_result = idx::ExecuteIndexValidationRepairOperation(delta);
  Require(delta_result.ok() &&
              delta_result.diagnostic.diagnostic_code ==
                  "DPC.INDEX_REPAIR.DELTA_LEDGER_REPAIRED",
          "ODF-120 delta ledger repair proof failed");
  Require(HasRepairEvidence(delta_result, "delta.recovery_action"),
          "ODF-120 delta ledger support evidence missing");

  auto shadow =
      RepairRequest(uuids,
                    10200,
                    idx::IndexValidationRepairFamily::shadow_index_build_state,
                    idx::IndexValidationRepairOperation::discard_unpublished);
  shadow.state.shadow_build = ShadowRecord(uuids, 10210);
  const auto shadow_result = idx::ExecuteIndexValidationRepairOperation(shadow);
  Require(shadow_result.ok() &&
              shadow_result.diagnostic.diagnostic_code ==
                  "DPC.INDEX_REPAIR.SHADOW_DISCARDED",
          "ODF-120 shadow discard proof failed");

  auto search = RepairRequest(
      uuids,
      10300,
      idx::IndexValidationRepairFamily::inverted_search_segment_state,
      idx::IndexValidationRepairOperation::discard_unpublished,
      idx::IndexFamily::full_text);
  search.state.inverted_segments.segments.push_back(
      SearchSegment(uuids, 10310));
  const auto search_result = idx::ExecuteIndexValidationRepairOperation(search);
  Require(search_result.ok() &&
              search_result.diagnostic.diagnostic_code ==
                  "DPC.INDEX_REPAIR.SEARCH_SEGMENT_DISCARDED_UNSAFE",
          "ODF-120 search segment discard proof failed");

  auto vector = RepairRequest(
      uuids,
      10400,
      idx::IndexValidationRepairFamily::vector_generation_state,
      idx::IndexValidationRepairOperation::discard_unpublished,
      idx::IndexFamily::vector_ivf);
  vector.state.vector_generations.generations.push_back(
      VectorGeneration(uuids, 10410));
  const auto vector_result = idx::ExecuteIndexValidationRepairOperation(vector);
  Require(vector_result.ok() &&
              vector_result.diagnostic.diagnostic_code ==
                  "DPC.INDEX_REPAIR.VECTOR_GENERATION_DISCARDED_UNSAFE",
          "ODF-120 vector generation discard proof failed");
}

api::EngineRequestContext SupportContext(const UuidFactory& uuids) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "odf120-support";
  context.database_path = "/tmp/scratchbird_odf120_support_bundle.sbdb";
  context.database_uuid.canonical = uuids.Text(platform::UuidKind::database, 20000);
  context.principal_uuid.canonical =
      uuids.Text(platform::UuidKind::principal, 20001);
  context.session_uuid.canonical = uuids.Text(platform::UuidKind::object, 20002);
  context.security_context_present = true;
  context.catalog_generation_id = 120;
  context.security_epoch = 120;
  context.resource_epoch = 120;
  context.name_resolution_epoch = 120;
  context.trace_tags = {"optimizer_deficiency_odf_120_gate",
                        "mga_transaction_regression"};
  return context;
}

api::PerformanceOptimizationSurfaceSnapshot SupportSnapshot() {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "odf120_backup_restore_validate_repair";
  snapshot.catalog_generation_id = 120;
  snapshot.name_resolution_epoch = 120;
  snapshot.security_epoch = 120;
  snapshot.resource_epoch = 120;
  snapshot.optimization_state_epoch = 120;
  snapshot.summary_prune_status = "backup_restore_rebuild_validated";
  snapshot.summary_prune_last_reason = "backup_restore_validate_repair";
  snapshot.summary_prune_fallback_reason = "exact_refusal_when_evidence_missing";
  snapshot.cleanup_horizon_authority_status = "authoritative";
  snapshot.cleanup_horizon_authoritative = true;
  snapshot.cleanup_horizon_local_transaction_id = 1;
  snapshot.oldest_interesting_transaction_id = 1;
  snapshot.oldest_active_transaction_id = 1;
  snapshot.oldest_snapshot_transaction_id = 1;
  snapshot.oldest_cleanup_transaction_id = 1;
  snapshot.secondary_index_state = "backup_restore_validate_repair";
  snapshot.shadow_index_state = "discard_unpublished_then_fallback";
  snapshot.summary_index_state = "rebuild_from_authoritative_base";
  snapshot.specialized_index_state = "search_vector_preserve_or_discard";
  snapshot.index_state_authority_source =
      "local_mga_transaction_inventory_and_uuid_resolved_catalog";
  snapshot.exact_refusal_diagnostic_code =
      "ODF.OPT_STRUCTURE.MGA_FINALITY_REQUIRED";
  snapshot.exact_refusal_message_vector =
      "ODF.OPT_STRUCTURE.MGA_FINALITY_REQUIRED|"
      "ODF.OPT_STRUCTURE.RESTORE_INSPECTION_REQUIRED|"
      "ODF.OPT_STRUCTURE.REBUILD_EVIDENCE_REQUIRED|"
      "ODF.OPT_STRUCTURE.SUPPORT_BUNDLE_REQUIRED|"
      "ODF.OPT_STRUCTURE.IDENTITY_REFUSED";
  snapshot.exact_refusal_source = "odf120.backup_restore_validate_repair";
  snapshot.message_vector_ready = true;
  snapshot.metric_family = "sys.metrics.odf120.backup_restore_repair";
  snapshot.metric_sample_count = 8;
  snapshot.audit_event_family = "engine.audit.odf120.backup_restore_repair";
  snapshot.audit_event_count = 8;
  snapshot.audit_last_decision = "survive_rebuild_repair_or_exact_refusal";
  snapshot.support_bundle_correlation_id = "odf120-support-bundle";
  snapshot.support_bundle_redaction_state = "public_safe_summary";
  snapshot.support_bundle_completeness_state = "complete";
  snapshot.support_bundle_forbidden_fields_absent = true;

  snapshot.odf108_runtime_compatibility.clear();
  snapshot.odf108_rebuild_states.clear();
  snapshot.odf108_exact_refusals.clear();
  for (const auto& structure : Structures()) {
    const std::string structure_name =
        idx::OptimizedPersistedStructureKindName(structure.structure);
    snapshot.odf108_runtime_compatibility.push_back(
        {"odf120." + structure_name,
         "compatible",
         idx::OptimizedStructureLifecycleActionName(
             structure.expected_restore_action),
         120,
         "manifest_coverage,mga_finality,restore_inspection,support_bundle",
         "supported",
         "none",
         "none"});
    snapshot.odf108_rebuild_states.push_back(
        {structure_name,
         "completed",
         idx::OptimizedStructureLifecycleActionName(
             structure.expected_restore_action),
         120,
         1,
         1,
         "none"});
  }
  snapshot.odf108_exact_refusals.push_back(
      {"optimized_structure_lifecycle",
       snapshot.exact_refusal_diagnostic_code,
       snapshot.exact_refusal_message_vector,
       "refused",
       "public_safe_summary",
       true});
  return snapshot;
}

void ProveSupportBundleEvidence() {
  const UuidFactory uuids;
  api::EnginePrepareSupportBundleRequest request;
  request.context = SupportContext(uuids);
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  request.performance_optimization_snapshot = SupportSnapshot();
  request.performance_optimization_snapshot_present = true;

  const auto result = api::EnginePrepareSupportBundle(request);
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, "ODF-120 support bundle refused optimized evidence");
  Require(result.redaction_applied && result.forbidden_fields_absent,
          "ODF-120 support bundle redaction state incomplete");
  Require(result.performance_optimization_surface_collected,
          "ODF-120 support bundle omitted optimization surface");
  Require(Contains(result.support_bundle_json,
                   "odf120_backup_restore_validate_repair"),
          "ODF-120 support bundle omitted profile");
  Require(Contains(result.support_bundle_json,
                   "ODF.OPT_STRUCTURE.MGA_FINALITY_REQUIRED"),
          "ODF-120 support bundle omitted finality refusal");
  Require(Contains(result.support_bundle_json,
                   "rebuild_from_authoritative_base"),
          "ODF-120 support bundle omitted rebuild evidence");
  Require(Contains(result.support_bundle_json,
                   "discard_unpublished_then_fallback"),
          "ODF-120 support bundle omitted repair/discard evidence");
  for (const auto& structure : Structures()) {
    Require(Contains(result.support_bundle_json,
                     idx::OptimizedPersistedStructureKindName(
                         structure.structure)),
            "ODF-120 support bundle omitted structure evidence");
  }
  RequireNoForbiddenPayload(result.support_bundle_json);
}

void ProveBackupArchiveAdmissionStillFailsClosed() {
  const UuidFactory uuids;
  api::EngineApiRequest request;
  request.context = SupportContext(uuids);
  request.option_envelopes.push_back("filespace_uuid:" +
                                     uuids.Text(platform::UuidKind::filespace,
                                                21000));
  request.option_envelopes.push_back("restore_inspection_open:true");
  request.option_envelopes.push_back("recovery_classification:restore_inspection");
  request.option_envelopes.push_back("target_database_open:false");
  request.option_envelopes.push_back("authoritative_wal:true");
  const auto refused = api::EvaluateBackupArchiveLifecycleAdmission(
      request, api::BackupArchiveLifecycleOperation::physical_restore);
  Require(!refused.admitted &&
              Contains(refused.diagnostic.detail,
                       "BACKUP_AUTHORITATIVE_WAL_FORBIDDEN"),
          "ODF-120 backup/archive admission accepted authoritative WAL");
}

}  // namespace

int main() {
  std::cout << kGateSearchKey << '\n';
  ProveLifecyclePolicyForAllStructures();
  ProveExactRefusalDiagnostics();
  ProveRepairToolingEvidence();
  ProveSupportBundleEvidence();
  ProveBackupArchiveAdmissionStillFailsClosed();
  return EXIT_SUCCESS;
}
