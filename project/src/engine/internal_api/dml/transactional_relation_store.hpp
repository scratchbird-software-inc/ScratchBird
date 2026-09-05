// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "dml/insert_physical_integration.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_CANONICAL_TRANSACTIONAL_RELATION_STORE_AUTHORITY_MAP
// This map names each storage artifact's role. It is deliberately code-owned
// so runtime routes and tests cannot accidentally promote a cache or the
// temporary CrudState projection into an equal source of truth.
struct TransactionalRelationStoreAuthorityRecord {
  std::string_view artifact;
  std::string_view authority;
  std::string_view classification;
};

std::span<const TransactionalRelationStoreAuthorityRecord>
TransactionalRelationStoreAuthorityMap();

enum class TransactionalRelationStoreRoute {
  diagnostic_full_state,
  insert_dependency_full_state,
  selectable_procedure_dependency_full_state,
  deferred_constraint_validation_full_state,
  insert_target,
  insert_target_metadata,
  insert_target_indexes,
  mutation_target,
  mutation_targets,
  mutation_target_rows,
  mutation_targets_rows,
  direct_physical_bulk_append,
};

std::string_view TransactionalRelationStoreRouteId(
    TransactionalRelationStoreRoute route);

// The single normal-DML entry seam over the existing MGA relation store and
// physical COW implementation. This facade owns routing, not transaction
// finality: finality remains authoritative only in the durable transaction
// inventory consulted by MGA visibility.
class TransactionalRelationStore {
 public:
  explicit TransactionalRelationStore(const EngineRequestContext& context);

  MgaRelationStoreResult LoadDiagnosticFullState() const;
  MgaRelationStoreResult LoadInsertDependencyFullState() const;
  MgaRelationStoreResult LoadSelectableProcedureDependencyFullState() const;
  MgaRelationStoreResult LoadDeferredConstraintValidationFullState() const;
  MgaRelationStoreResult LoadInsertTarget(const std::string& table_uuid) const;
  MgaRelationStoreResult LoadInsertTargetMetadata(
      const std::string& table_uuid) const;
  MgaRelationStoreResult LoadInsertTargetIndexes(
      const std::string& table_uuid) const;
  MgaRelationStoreResult LoadMutationTarget(
      const std::string& table_uuid) const;
  MgaRelationStoreResult LoadMutationTargets(
      const std::vector<std::string>& table_uuids) const;
  MgaRelationStoreResult LoadMutationTargetRows(
      const std::string& table_uuid) const;
  MgaRelationStoreResult LoadMutationTargetRows(
      const std::vector<std::string>& table_uuids) const;
  MgaRelationStorageDescriptorLoadResult LoadRelationDescriptor(
      const std::string& relation_uuid) const;

  // CrudState remains a transitional compatibility working projection of the
  // MGA state returned by this facade. Mutations must use the methods below
  // and never persist the projection as a second authority.
  CrudState BuildCompatibilityProjection(MgaRelationStoreResult* loaded) const;

  MgaRelationHotAppendContext OpenHotAppendContext() const;
  EngineApiDiagnostic AppendRowVersion(
      const CrudRowVersionRecord& row,
      std::uint64_t* written_event_sequence) const;
  EngineApiDiagnostic AppendIndexEntriesForRowsWithIndexes(
      const std::vector<CrudIndexRecord>& indexes,
      const std::string& table_uuid,
      const std::vector<MgaIndexEntryRowInput>& rows) const;
  EngineApiDiagnostic AppendSecondaryIndexDeltaLedgerEntries(
      const std::vector<MgaSecondaryIndexDeltaLedgerEntryInput>& entries,
      std::vector<EngineEvidenceReference>* evidence) const;

  dml::DirectPhysicalBulkAppendResult ExecuteDirectPhysicalBulkAppend(
      const dml::DirectPhysicalBulkAppendRequest& request) const;

  static void AppendRouteEvidence(
      TransactionalRelationStoreRoute route,
      std::vector<EngineEvidenceReference>* evidence);

 private:
  const EngineRequestContext& context_;
};

}  // namespace scratchbird::engine::internal_api
