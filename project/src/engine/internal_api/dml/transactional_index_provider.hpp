// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_DML_TRANSACTIONAL_INDEX_PROVIDER_V1
//
// This is the common transaction-version contract for the first released
// physical provider family: ordered B-tree and its unique/expression/partial/
// covering profiles.  Index bytes are candidate-access state.  The durable MGA
// transaction inventory remains the sole authority for finality and visibility.
struct DmlTransactionalIndexEntryRequest {
  CrudIndexRecord index;
  std::string table_uuid;
  std::string row_uuid;
  std::string version_uuid;
  std::string predecessor_version_uuid;
  std::string key_value;
  std::string payload_value;
};

struct DmlTransactionalIndexProviderResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::string lifecycle_state;
  std::uint64_t prepared_insert_count = 0;
  std::uint64_t prepared_retire_count = 0;
  std::uint64_t visible_entry_count = 0;
  std::uint64_t rebuilt_entry_count = 0;
  std::vector<CrudRowVersionRecord> rows;
  std::vector<EngineEvidenceReference> evidence;
};

bool IsReleasedOrderedBtreeTransactionalFamily(const CrudIndexRecord& index);

std::string DmlTransactionalIndexMutationIdentity(
    const EngineRequestContext& context,
    const DmlTransactionalIndexEntryRequest& request,
    std::string_view mutation_kind);

class DmlTransactionalIndexProvider {
 public:
  virtual ~DmlTransactionalIndexProvider() = default;

  virtual DmlTransactionalIndexProviderResult PrepareInsertEntry(
      const DmlTransactionalIndexEntryRequest& request) = 0;
  virtual DmlTransactionalIndexProviderResult PrepareRetireEntry(
      const DmlTransactionalIndexEntryRequest& request) = 0;
  virtual DmlTransactionalIndexProviderResult PublishTransaction(
      const RelationReadSnapshot& state) const = 0;
  virtual DmlTransactionalIndexProviderResult AbortTransaction(
      const RelationReadSnapshot& state) const = 0;
  virtual DmlTransactionalIndexProviderResult ResolveVisibleEntry(
      const RelationReadSnapshot& state,
      const CrudIndexRecord& index,
      const EnginePredicateEnvelope& predicate,
      std::uint64_t limit = 0) const = 0;
  virtual DmlTransactionalIndexProviderResult RecoverInterruptedMutation(
      const RelationReadSnapshot& state) const = 0;
  virtual DmlTransactionalIndexProviderResult ValidateAgainstRelation(
      const RelationReadSnapshot& state,
      const CrudIndexRecord& index) const = 0;
  virtual DmlTransactionalIndexProviderResult RebuildFromRelation(
      const RelationReadSnapshot& state,
      const CrudIndexRecord& index,
      bool cleanup_horizon_excludes_old_snapshots) = 0;
};

class MgaOrderedBtreeTransactionalIndexProvider final
    : public DmlTransactionalIndexProvider {
 public:
  MgaOrderedBtreeTransactionalIndexProvider(
      const EngineRequestContext& context,
      MgaRelationHotAppendContext* append_context);

  DmlTransactionalIndexProviderResult PrepareInsertEntry(
      const DmlTransactionalIndexEntryRequest& request) override;
  DmlTransactionalIndexProviderResult PrepareRetireEntry(
      const DmlTransactionalIndexEntryRequest& request) override;
  DmlTransactionalIndexProviderResult PrepareInsertEntries(
      const std::vector<DmlTransactionalIndexEntryRequest>& requests);
  DmlTransactionalIndexProviderResult PrepareRetireEntries(
      const std::vector<DmlTransactionalIndexEntryRequest>& requests);
  DmlTransactionalIndexProviderResult PublishTransaction(
      const RelationReadSnapshot& state) const override;
  DmlTransactionalIndexProviderResult AbortTransaction(
      const RelationReadSnapshot& state) const override;
  DmlTransactionalIndexProviderResult ResolveVisibleEntry(
      const RelationReadSnapshot& state,
      const CrudIndexRecord& index,
      const EnginePredicateEnvelope& predicate,
      std::uint64_t limit = 0) const override;
  DmlTransactionalIndexProviderResult RecoverInterruptedMutation(
      const RelationReadSnapshot& state) const override;
  DmlTransactionalIndexProviderResult ValidateAgainstRelation(
      const RelationReadSnapshot& state,
      const CrudIndexRecord& index) const override;
  DmlTransactionalIndexProviderResult RebuildFromRelation(
      const RelationReadSnapshot& state,
      const CrudIndexRecord& index,
      bool cleanup_horizon_excludes_old_snapshots) override;

 private:
  DmlTransactionalIndexProviderResult PrepareEntry(
      const DmlTransactionalIndexEntryRequest& request,
      std::string entry_kind);
  DmlTransactionalIndexProviderResult PrepareEntries(
      const std::vector<DmlTransactionalIndexEntryRequest>& requests,
      std::string entry_kind);

  const EngineRequestContext& context_;
  MgaRelationHotAppendContext* append_context_ = nullptr;
};

// Commit-barrier proof for all ordered B-tree mutations owned by the active
// transaction.  A missing insert or retire record blocks inventory finality.
DmlTransactionalIndexProviderResult
ValidateOrderedBtreeTransactionalIndexMutationSetForCommit(
    const EngineRequestContext& context,
    const RelationReadSnapshot& state);

}  // namespace scratchbird::engine::internal_api
