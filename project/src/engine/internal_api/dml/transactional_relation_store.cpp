// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/transactional_relation_store.hpp"

#include <array>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::array<TransactionalRelationStoreAuthorityRecord, 8>
    kAuthorityMap{{
        {"row_versions", "physical_mga_cow_relation_segments",
         "canonical_durable"},
        {"row_directory", "physical_mga_cow_relation_directory",
         "canonical_durable"},
        {"relation_descriptors", "immutable_mga_relation_descriptors",
         "canonical_durable"},
        {"secondary_index_membership",
         "mga_index_base_plus_transaction_delta", "canonical_durable"},
        {"transaction_finality", "durable_transaction_inventory",
         "canonical_durable"},
        {"publication_generations",
         "relation_descriptor_and_transaction_inventory", "canonical_durable"},
        {"relation_caches", "canonical_store_derived_cache",
         "derived_non_authoritative"},
        {"crud_compatibility_state", "mga_state_projection",
         "read_only_subordinate_projection"},
    }};

MgaRelationStoreResult WithRouteEvidence(
    MgaRelationStoreResult result,
    TransactionalRelationStoreRoute route) {
  TransactionalRelationStore::AppendRouteEvidence(route, &result.evidence);
  return result;
}

}  // namespace

std::span<const TransactionalRelationStoreAuthorityRecord>
TransactionalRelationStoreAuthorityMap() {
  return kAuthorityMap;
}

MgaRelationReadView BuildMgaRelationReadView(
    const MgaRelationStoreState& state) {
  MgaRelationReadView view;
  view.transactions = state.relation_metadata.transactions;
  view.tables = state.relation_metadata.tables;
  view.row_versions = state.row_versions;
  view.indexes = state.relation_metadata.indexes;
  view.index_entries = state.index_entries;
  view.large_values = state.relation_metadata.large_values;
  view.sealed_relation_descriptor_snapshots =
      state.relation_metadata.sealed_relation_descriptor_snapshots;
  view.max_transaction_id = state.relation_metadata.max_transaction_id;
  view.max_sequence = state.max_row_event_sequence;
  view.max_index_sequence = state.max_index_event_sequence;
  view.max_event_sequence = state.relation_metadata.max_event_sequence;
  view.savepoints = state.relation_metadata.savepoints;
  return view;
}

MgaRelationReadView BuildMgaRelationReadView(MgaRelationStoreState&& state) {
  MgaRelationReadView view;
  view.transactions = std::move(state.relation_metadata.transactions);
  view.tables = std::move(state.relation_metadata.tables);
  view.row_versions = std::move(state.row_versions);
  view.indexes = std::move(state.relation_metadata.indexes);
  view.index_entries = std::move(state.index_entries);
  view.large_values = std::move(state.relation_metadata.large_values);
  view.sealed_relation_descriptor_snapshots =
      std::move(state.relation_metadata.sealed_relation_descriptor_snapshots);
  view.max_transaction_id = state.relation_metadata.max_transaction_id;
  view.max_sequence = state.max_row_event_sequence;
  view.max_index_sequence = state.max_index_event_sequence;
  view.max_event_sequence = state.relation_metadata.max_event_sequence;
  view.savepoints = std::move(state.relation_metadata.savepoints);
  return view;
}

bool MgaCreatorVisible(const MgaRelationReadView& view,
                       std::uint64_t creator_tx,
                       std::uint64_t event_sequence,
                       std::uint64_t observer_tx) {
  return CrudCreatorVisible(view, creator_tx, event_sequence, observer_tx);
}

bool MgaRowVersionVisibleToContext(const MgaRelationReadView& view,
                                   const CrudRowVersionRecord& row,
                                   const EngineRequestContext& context) {
  return CrudRowVersionVisibleToContext(view, row, context);
}

std::optional<CrudTableRecord> FindVisibleMgaTable(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    std::uint64_t observer_tx) {
  return FindVisibleCrudTable(view, table_uuid, observer_tx);
}

std::optional<CrudRowVersionRecord> FindVisibleMgaRowForContext(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    const std::string& row_uuid,
    const EngineRequestContext& context) {
  return FindVisibleCrudRowForContext(view, table_uuid, row_uuid, context);
}

std::vector<CrudRowVersionRecord> VisibleMgaRowsForContext(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    const EngineRequestContext& context) {
  return VisibleCrudRowsForContext(view, table_uuid, context);
}

std::vector<CrudRowVersionRecord> VisibleMgaRows(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    std::uint64_t observer_tx) {
  return VisibleCrudRows(view, table_uuid, observer_tx);
}

std::vector<CrudIndexRecord> VisibleMgaIndexesForTable(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    std::uint64_t observer_tx) {
  return VisibleCrudIndexesForTable(view, table_uuid, observer_tx);
}

EngineApiDiagnostic ValidateMgaUniqueIndexesForRow(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    const EngineRequestContext& context) {
  return ValidateCrudUniqueIndexesForRow(view, table_uuid, row_uuid, values,
                                         context);
}

std::string_view TransactionalRelationStoreRouteId(
    TransactionalRelationStoreRoute route) {
  switch (route) {
    case TransactionalRelationStoreRoute::diagnostic_full_state:
      return "normal_dml.diagnostic_full_state.v1";
    case TransactionalRelationStoreRoute::insert_dependency_full_state:
      return "normal_dml.insert_dependency_full_state.v1";
    case TransactionalRelationStoreRoute::
        selectable_procedure_dependency_full_state:
      return "normal_dml.selectable_procedure_dependency_full_state.v1";
    case TransactionalRelationStoreRoute::
        deferred_constraint_validation_full_state:
      return "normal_dml.deferred_constraint_validation_full_state.v1";
    case TransactionalRelationStoreRoute::insert_target:
      return "normal_dml.insert_target.v1";
    case TransactionalRelationStoreRoute::insert_target_metadata:
      return "normal_dml.insert_target_metadata.v1";
    case TransactionalRelationStoreRoute::insert_target_indexes:
      return "normal_dml.insert_target_indexes.v1";
    case TransactionalRelationStoreRoute::mutation_target:
      return "normal_dml.mutation_target.v1";
    case TransactionalRelationStoreRoute::mutation_targets:
      return "normal_dml.mutation_targets.v1";
    case TransactionalRelationStoreRoute::mutation_target_rows:
      return "normal_dml.mutation_target_rows.v1";
    case TransactionalRelationStoreRoute::mutation_targets_rows:
      return "normal_dml.mutation_targets_rows.v1";
    case TransactionalRelationStoreRoute::direct_physical_bulk_append:
      return "normal_dml.direct_physical_bulk_append.v1";
  }
  return "normal_dml.unknown.v1";
}

TransactionalRelationStore::TransactionalRelationStore(
    const EngineRequestContext& context)
    : context_(context) {}

MgaRelationStoreResult TransactionalRelationStore::LoadDiagnosticFullState()
    const {
  return WithRouteEvidence(
      LoadMgaRelationStoreState(context_),
      TransactionalRelationStoreRoute::diagnostic_full_state);
}

MgaRelationStoreResult
TransactionalRelationStore::LoadInsertDependencyFullState() const {
  return WithRouteEvidence(
      LoadMgaRelationStoreState(context_),
      TransactionalRelationStoreRoute::insert_dependency_full_state);
}

MgaRelationStoreResult
TransactionalRelationStore::LoadSelectableProcedureDependencyFullState() const {
  return WithRouteEvidence(
      LoadMgaRelationStoreState(context_),
      TransactionalRelationStoreRoute::
          selectable_procedure_dependency_full_state);
}

MgaRelationStoreResult
TransactionalRelationStore::LoadDeferredConstraintValidationFullState() const {
  return WithRouteEvidence(
      LoadMgaRelationStoreState(context_),
      TransactionalRelationStoreRoute::
          deferred_constraint_validation_full_state);
}

MgaRelationStoreResult TransactionalRelationStore::LoadInsertTarget(
    const std::string& table_uuid) const {
  return WithRouteEvidence(
      LoadMgaRelationStoreStateForInsertTarget(context_, table_uuid),
      TransactionalRelationStoreRoute::insert_target);
}

MgaRelationStoreResult TransactionalRelationStore::LoadInsertTargetMetadata(
    const std::string& table_uuid) const {
  return WithRouteEvidence(
      LoadMgaRelationStoreMetadataOnlyForInsertTarget(context_, table_uuid),
      TransactionalRelationStoreRoute::insert_target_metadata);
}

MgaRelationStoreResult TransactionalRelationStore::LoadInsertTargetIndexes(
    const std::string& table_uuid) const {
  return WithRouteEvidence(
      LoadMgaRelationStoreIndexesOnlyForInsertTarget(context_, table_uuid),
      TransactionalRelationStoreRoute::insert_target_indexes);
}

MgaRelationStoreResult TransactionalRelationStore::LoadMutationTarget(
    const std::string& table_uuid) const {
  return WithRouteEvidence(
      LoadMgaRelationStoreStateForMutationTarget(context_, table_uuid),
      TransactionalRelationStoreRoute::mutation_target);
}

MgaRelationStoreResult TransactionalRelationStore::LoadMutationTargets(
    const std::vector<std::string>& table_uuids) const {
  return WithRouteEvidence(
      LoadMgaRelationStoreStateForMutationTargets(context_, table_uuids),
      TransactionalRelationStoreRoute::mutation_targets);
}

MgaRelationStoreResult TransactionalRelationStore::LoadMutationTargetRows(
    const std::string& table_uuid) const {
  return WithRouteEvidence(
      LoadMgaRelationStoreRowsOnlyForMutationTarget(context_, table_uuid),
      TransactionalRelationStoreRoute::mutation_target_rows);
}

MgaRelationStoreResult TransactionalRelationStore::LoadMutationTargetRows(
    const std::vector<std::string>& table_uuids) const {
  return WithRouteEvidence(
      LoadMgaRelationStoreRowsOnlyForMutationTargets(context_, table_uuids),
      TransactionalRelationStoreRoute::mutation_targets_rows);
}

MgaRelationStorageDescriptorLoadResult
TransactionalRelationStore::LoadRelationDescriptor(
    const std::string& relation_uuid) const {
  return LoadMgaRelationStorageDescriptor(context_, relation_uuid);
}

MgaRelationReadView TransactionalRelationStore::BuildReadView(
    MgaRelationStoreResult* loaded) const {
  if (loaded == nullptr) {
    return {};
  }
  loaded->evidence.push_back(
      {"transactional_relation_store_read_model", "mga_scoped_read_view_v1"});
  return BuildMgaRelationReadView(std::move(loaded->state));
}

MgaRelationHotAppendContext
TransactionalRelationStore::OpenHotAppendContext() const {
  return MgaRelationHotAppendContext(context_);
}

EngineApiDiagnostic TransactionalRelationStore::AppendRowVersion(
    const CrudRowVersionRecord& row,
    std::uint64_t* written_event_sequence) const {
  return AppendMgaRowVersion(context_, row, written_event_sequence);
}

EngineApiDiagnostic
TransactionalRelationStore::AppendIndexEntriesForRowsWithIndexes(
    const std::vector<CrudIndexRecord>& indexes,
    const std::string& table_uuid,
    const std::vector<MgaIndexEntryRowInput>& rows) const {
  return AppendMgaIndexEntriesForRowsWithIndexes(
      context_, indexes, table_uuid, rows);
}

EngineApiDiagnostic
TransactionalRelationStore::AppendSecondaryIndexDeltaLedgerEntries(
    const std::vector<MgaSecondaryIndexDeltaLedgerEntryInput>& entries,
    std::vector<EngineEvidenceReference>* evidence) const {
  return AppendMgaSecondaryIndexDeltaLedgerEntries(context_, entries, evidence);
}

dml::DirectPhysicalBulkAppendResult
TransactionalRelationStore::ExecuteDirectPhysicalBulkAppend(
    const dml::DirectPhysicalBulkAppendRequest& request) const {
  auto result = dml::ExecuteDirectPhysicalBulkAppend(request);
  AppendRouteEvidence(
      TransactionalRelationStoreRoute::direct_physical_bulk_append,
      &result.evidence);
  return result;
}

void TransactionalRelationStore::AppendRouteEvidence(
    TransactionalRelationStoreRoute route,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr) {
    return;
  }
  evidence->push_back(
      {"transactional_relation_store", "canonical_normal_dml_v1"});
  evidence->push_back(
      {"transactional_relation_store_route",
       std::string(TransactionalRelationStoreRouteId(route))});
  evidence->push_back(
      {"transactional_relation_store_finality_authority",
       "durable_transaction_inventory"});
}

}  // namespace scratchbird::engine::internal_api
