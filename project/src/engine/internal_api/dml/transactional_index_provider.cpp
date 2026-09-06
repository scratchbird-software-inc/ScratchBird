// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/transactional_index_provider.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "index_family_registry.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace core_hash = scratchbird::core::hash;
namespace core_index = scratchbird::core::index;
using scratchbird::core::platform::byte;

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic Refuse(std::string code,
                           std::string key,
                           std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

std::string ResolvedFamily(const CrudIndexRecord& index) {
  if (!index.family.empty()) {
    if (index.family == kCrudIndexFamilyBtree &&
        (index.unique ||
         std::find(index.key_envelopes.begin(), index.key_envelopes.end(),
                   "unique") != index.key_envelopes.end())) {
      return "unique_btree";
    }
    return index.family;
  }
  if (index.unique ||
      std::find(index.key_envelopes.begin(), index.key_envelopes.end(),
                "unique") != index.key_envelopes.end()) {
    return "unique_btree";
  }
  const std::string profile = CrudIndexFamilyForProfile(index.profile);
  return profile.empty() ? std::string(kCrudIndexFamilyBtree) : profile;
}

bool CommittedState(std::string_view state) {
  return state == "committed" || state == "archived";
}

bool RolledBackState(std::string_view state) {
  return state == "rolled_back" || state == "failed_terminal";
}

bool EntryKindIsMembership(std::string_view kind) {
  return kind == "exact" || kind == "insert" || kind == "rebuild";
}

bool EntryMatches(const CrudIndexEntryRecord& entry,
                  const CrudIndexRecord& index,
                  const std::string& row_uuid,
                  const std::string& version_uuid,
                  const std::string& key,
                  std::string_view kind,
                  std::uint64_t creator_tx = 0) {
  return entry.index_uuid == index.index_uuid &&
         entry.table_uuid == index.table_uuid && entry.row_uuid == row_uuid &&
         entry.version_uuid == version_uuid &&
         CrudIndexEntryMatchesLogicalKey(index, entry, key) &&
         (kind.empty() ? EntryKindIsMembership(entry.entry_kind)
                       : entry.entry_kind == kind) &&
         (creator_tx == 0 || entry.creator_tx == creator_tx);
}

const CrudRowVersionRecord* FindVersion(const RelationReadSnapshot& state,
                                        const std::string& table_uuid,
                                        const std::string& version_uuid) {
  for (const auto& row : state.row_versions) {
    if (row.table_uuid == table_uuid && row.version_uuid == version_uuid) {
      return &row;
    }
  }
  return nullptr;
}

bool HasEntry(const RelationReadSnapshot& state,
              const CrudIndexRecord& index,
              const std::string& row_uuid,
              const std::string& version_uuid,
              const std::string& key,
              std::string_view kind,
              std::uint64_t creator_tx = 0) {
  return std::any_of(
      state.index_entries.begin(), state.index_entries.end(),
      [&](const auto& entry) {
        return EntryMatches(entry, index, row_uuid, version_uuid, key, kind,
                            creator_tx);
      });
}

void AddProviderEvidence(
    const EngineRequestContext& context,
    const CrudIndexRecord* index,
    std::string_view operation,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr) return;
  evidence->push_back({"transactional_index_provider_contract",
                       "mga_native_index_family_v1"});
  evidence->push_back({"transactional_index_provider_operation",
                       std::string(operation)});
  evidence->push_back({"transactional_index_finality_authority",
                       "durable_transaction_inventory"});
  evidence->push_back({"transactional_index_candidate_recheck",
                       "mga_row_version_visibility"});
  evidence->push_back({"transactional_index_local_transaction_id",
                       std::to_string(context.local_transaction_id)});
  if (index != nullptr) {
    evidence->push_back({"transactional_index_uuid", index->index_uuid});
    evidence->push_back({"transactional_index_family",
                         ResolvedFamily(*index)});
    evidence->push_back({"transactional_index_generation",
                         std::to_string(index->event_sequence)});
    evidence->push_back({"transactional_index_physical_identity",
                         index->table_uuid + ".indexes"});
  }
}

DmlTransactionalIndexProviderResult Failure(const EngineRequestContext& context,
                                             const CrudIndexRecord* index,
                                             std::string operation,
                                             EngineApiDiagnostic diagnostic) {
  DmlTransactionalIndexProviderResult result;
  result.diagnostic = std::move(diagnostic);
  result.lifecycle_state = "refused";
  AddProviderEvidence(context, index, operation, &result.evidence);
  return result;
}

bool TransactionEntryVisible(const RelationReadSnapshot& state,
                             const CrudIndexEntryRecord& entry,
                             const EngineRequestContext& context) {
  return CrudCreatorVisible(state, entry.creator_tx, entry.event_sequence,
                            context.local_transaction_id);
}

}  // namespace

bool IsReleasedOrderedBtreeTransactionalFamily(const CrudIndexRecord& index) {
  const std::string family = ResolvedFamily(index);
  return family == kCrudIndexFamilyBtree || family == "unique_btree" ||
         family == kCrudIndexFamilyExpression ||
         family == kCrudIndexFamilyPartial ||
         family == kCrudIndexFamilyCovering;
}

bool IsAdmittedMgaTransactionalIndexFamily(const CrudIndexRecord& index) {
  std::string family = ResolvedFamily(index);
  if (family == kCrudIndexFamilyGraphAdjacency) family = "graph";
  const auto lookup = core_index::FindBuiltinIndexFamilyById(family);
  if (!lookup.ok()) return false;
  const auto* capability =
      core_index::FindBuiltinIndexFamilyPhysicalCapabilityState(
          lookup.descriptor->family);
  return capability != nullptr && capability->runtime_available &&
         lookup.descriptor->persistence !=
             core_index::IndexPersistenceClass::reference_emulated &&
         lookup.descriptor->persistence !=
             core_index::IndexPersistenceClass::policy_blocked;
}

std::string DmlTransactionalIndexMutationIdentity(
    const EngineRequestContext& context,
    const DmlTransactionalIndexEntryRequest& request,
    std::string_view mutation_kind) {
  const std::string material =
      "SB_DML_TRANSACTIONAL_INDEX_MUTATION_V1\t" +
      context.transaction_uuid.canonical + "\t" +
      std::to_string(context.local_transaction_id) + "\t" +
      request.index.index_uuid + "\t" +
      std::to_string(request.index.event_sequence) + "\t" +
      request.table_uuid + "\t" + request.row_uuid + "\t" +
      request.version_uuid + "\t" + request.predecessor_version_uuid + "\t" +
      std::string(mutation_kind) + "\t" + request.key_value + "\t" +
      request.payload_value;
  const auto digest = core_hash::ComputeSha256Digest(
      reinterpret_cast<const byte*>(material.data()), material.size());
  return digest.ok() ? core_hash::HexLower(digest.digest) : std::string{};
}

MgaOrderedBtreeTransactionalIndexProvider::
    MgaOrderedBtreeTransactionalIndexProvider(
        const EngineRequestContext& context,
        MgaRelationHotAppendContext* append_context)
    : context_(context), append_context_(append_context) {}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::PrepareEntry(
    const DmlTransactionalIndexEntryRequest& request,
    std::string entry_kind) {
  if (!IsAdmittedMgaTransactionalIndexFamily(request.index)) {
    return Failure(
        context_, &request.index, entry_kind,
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.FAMILY_NOT_ADMITTED",
               "index.transactional_provider.family_not_admitted",
               "family=" + ResolvedFamily(request.index)));
  }
  if (append_context_ == nullptr || context_.local_transaction_id == 0 ||
      context_.transaction_uuid.canonical.empty() ||
      request.index.index_uuid.empty() || request.index.event_sequence == 0 ||
      request.table_uuid.empty() || request.row_uuid.empty() ||
      request.version_uuid.empty()) {
    return Failure(
        context_, &request.index, entry_kind,
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.IDENTITY_INCOMPLETE",
               "index.transactional_provider.identity_incomplete",
               "transaction, index generation, row/version, physical relation, and key identity are required"));
  }

  MgaExactIndexEntryAppendBatch batch;
  batch.index = request.index;
  batch.table_uuid = request.table_uuid;
  batch.entry_kind = entry_kind;
  batch.entries.push_back({request.key_value,
                           request.payload_value,
                           request.row_uuid,
                           request.version_uuid});
  const auto appended = append_context_->AppendExactIndexEntryBatches({batch});
  if (appended.error) {
    return Failure(context_, &request.index, entry_kind, appended);
  }

  DmlTransactionalIndexProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.lifecycle_state = "commit_publish_ready";
  result.prepared_insert_count = entry_kind == "insert" ? 1 : 0;
  result.prepared_retire_count = entry_kind == "retire" ? 1 : 0;
  AddProviderEvidence(context_, &request.index,
                      entry_kind == "insert" ? "PrepareInsertEntry"
                                             : "PrepareRetireEntry",
                      &result.evidence);
  result.evidence.push_back({"transactional_index_mutation_identity",
                             DmlTransactionalIndexMutationIdentity(
                                 context_, request, entry_kind)});
  result.evidence.push_back({"transactional_index_entry_kind", entry_kind});
  result.evidence.push_back({"transactional_index_row_uuid", request.row_uuid});
  result.evidence.push_back({"transactional_index_version_uuid",
                             request.version_uuid});
  result.evidence.push_back({"transactional_index_predecessor_version_uuid",
                             request.predecessor_version_uuid});
  return result;
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::PrepareInsertEntry(
    const DmlTransactionalIndexEntryRequest& request) {
  return PrepareEntry(request, "insert");
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::PrepareRetireEntry(
    const DmlTransactionalIndexEntryRequest& request) {
  if (request.predecessor_version_uuid.empty()) {
    return Failure(
        context_, &request.index, "PrepareRetireEntry",
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.PREDECESSOR_REQUIRED",
               "index.transactional_provider.predecessor_required",
               "retire mutation must name the predecessor row version"));
  }
  return PrepareEntry(request, "retire");
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::PrepareEntries(
    const std::vector<DmlTransactionalIndexEntryRequest>& requests,
    std::string entry_kind) {
  DmlTransactionalIndexProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.lifecycle_state = "commit_publish_ready";
  AddProviderEvidence(context_, nullptr,
                      entry_kind == "insert" ? "PrepareInsertEntry"
                                             : "PrepareRetireEntry",
                      &result.evidence);
  for (const auto& request : requests) {
    auto prepared = entry_kind == "insert" ? PrepareInsertEntry(request)
                                            : PrepareRetireEntry(request);
    if (!prepared.ok) return prepared;
    result.prepared_insert_count += prepared.prepared_insert_count;
    result.prepared_retire_count += prepared.prepared_retire_count;
  }
  result.evidence.push_back({"transactional_index_prepared_insert_count",
                             std::to_string(result.prepared_insert_count)});
  result.evidence.push_back({"transactional_index_prepared_retire_count",
                             std::to_string(result.prepared_retire_count)});
  return result;
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::PrepareInsertEntries(
    const std::vector<DmlTransactionalIndexEntryRequest>& requests) {
  return PrepareEntries(requests, "insert");
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::PrepareRetireEntries(
    const std::vector<DmlTransactionalIndexEntryRequest>& requests) {
  return PrepareEntries(requests, "retire");
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::PublishTransaction(
    const RelationReadSnapshot& state) const {
  const auto found = state.transactions.find(context_.local_transaction_id);
  if (found == state.transactions.end() || !CommittedState(found->second)) {
    return Failure(
        context_, nullptr, "PublishTransaction",
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.INVENTORY_COMMIT_REQUIRED",
               "index.transactional_provider.inventory_commit_required",
               "index publication cannot lead durable MGA inventory finality"));
  }
  DmlTransactionalIndexProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.lifecycle_state = "published_by_inventory";
  AddProviderEvidence(context_, nullptr, "PublishTransaction", &result.evidence);
  return result;
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::AbortTransaction(
    const RelationReadSnapshot& state) const {
  const auto found = state.transactions.find(context_.local_transaction_id);
  if (found == state.transactions.end() || !RolledBackState(found->second)) {
    return Failure(
        context_, nullptr, "AbortTransaction",
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.INVENTORY_ROLLBACK_REQUIRED",
               "index.transactional_provider.inventory_rollback_required",
               "index abort cannot lead durable MGA inventory finality"));
  }
  DmlTransactionalIndexProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.lifecycle_state = "invisible_by_inventory";
  AddProviderEvidence(context_, nullptr, "AbortTransaction", &result.evidence);
  return result;
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::ResolveVisibleEntry(
    const RelationReadSnapshot& state,
    const CrudIndexRecord& index,
    const EnginePredicateEnvelope& predicate,
    std::uint64_t limit) const {
  if (!IsAdmittedMgaTransactionalIndexFamily(index)) {
    return Failure(
        context_, &index, "ResolveVisibleEntry",
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.FAMILY_NOT_ADMITTED",
               "index.transactional_provider.family_not_admitted",
               "family=" + ResolvedFamily(index)));
  }
  RelationReadSnapshot selected = state;
  selected.indexes.erase(
      std::remove_if(selected.indexes.begin(), selected.indexes.end(),
                     [&](const auto& candidate) {
                       return candidate.index_uuid != index.index_uuid;
                     }),
      selected.indexes.end());
  std::string evidence_id;
  auto rows = IndexedCrudRowsForPredicateForContext(
      selected, index.table_uuid, predicate, context_, limit, &evidence_id);
  DmlTransactionalIndexProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.lifecycle_state = "resolved_with_mga_recheck";
  result.visible_entry_count = rows.size();
  result.rows = std::move(rows);
  AddProviderEvidence(context_, &index, "ResolveVisibleEntry", &result.evidence);
  result.evidence.push_back({"transactional_index_resolution_evidence",
                             std::move(evidence_id)});
  return result;
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::RecoverInterruptedMutation(
    const RelationReadSnapshot& state) const {
  const auto found = state.transactions.find(context_.local_transaction_id);
  if (found == state.transactions.end()) {
    return Failure(
        context_, nullptr, "RecoverInterruptedMutation",
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.RECOVERY_TX_UNKNOWN",
               "index.transactional_provider.recovery_transaction_unknown",
               "index entry creator has no durable transaction inventory classification"));
  }
  DmlTransactionalIndexProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  if (CommittedState(found->second)) {
    result.lifecycle_state = "committed_by_inventory";
  } else if (RolledBackState(found->second)) {
    result.lifecycle_state = "abandoned_by_inventory";
  } else {
    result.lifecycle_state = "retryable_unpublished";
  }
  AddProviderEvidence(context_, nullptr, "RecoverInterruptedMutation",
                      &result.evidence);
  return result;
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::ValidateAgainstRelation(
    const RelationReadSnapshot& state,
    const CrudIndexRecord& index) const {
  if (!IsAdmittedMgaTransactionalIndexFamily(index) ||
      index.event_sequence == 0) {
    return Failure(
        context_, &index, "ValidateAgainstRelation",
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.STALE_OR_INCOMPLETE",
               "index.transactional_provider.stale_or_incomplete",
               "admitted provider requires a durable index generation"));
  }

  std::map<std::string, std::string> unique_rows_by_key;
  std::uint64_t expected = 0;
  for (const auto& row :
       VisibleCrudRowsForContext(state, index.table_uuid, context_)) {
    if (row.deleted) continue;
    for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
      ++expected;
      bool candidate = false;
      for (const auto& entry : state.index_entries) {
        if (entry.index_uuid != index.index_uuid ||
            entry.table_uuid != index.table_uuid ||
            entry.row_uuid != row.row_uuid ||
            !CrudIndexEntryMatchesLogicalKey(index, entry, key) ||
            !EntryKindIsMembership(entry.entry_kind) ||
            !TransactionEntryVisible(state, entry, context_)) {
          continue;
        }
        candidate = true;
        break;
      }
      if (!candidate) {
        return Failure(
            context_, &index, "ValidateAgainstRelation",
            Refuse("INDEX.TRANSACTIONAL_PROVIDER.MISSING_VISIBLE_ENTRY",
                   "index.transactional_provider.missing_visible_entry",
                   "index_uuid=" + index.index_uuid + ";row_uuid=" +
                       row.row_uuid + ";key=" + key));
      }
      if (index.unique || ResolvedFamily(index) == "unique_btree") {
        const auto inserted = unique_rows_by_key.emplace(key, row.row_uuid);
        if (!inserted.second && inserted.first->second != row.row_uuid) {
          return Failure(
              context_, &index, "ValidateAgainstRelation",
              Refuse("INDEX.TRANSACTIONAL_PROVIDER.UNIQUE_CONFLICT",
                     "index.transactional_provider.unique_conflict",
                     "index_uuid=" + index.index_uuid + ";key=" + key));
        }
      }
    }
  }

  DmlTransactionalIndexProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.lifecycle_state = "validated_against_relation";
  result.visible_entry_count = expected;
  AddProviderEvidence(context_, &index, "ValidateAgainstRelation",
                      &result.evidence);
  result.evidence.push_back({"transactional_index_validated_memberships",
                             std::to_string(expected)});
  return result;
}

DmlTransactionalIndexProviderResult
MgaOrderedBtreeTransactionalIndexProvider::RebuildFromRelation(
    const RelationReadSnapshot& state,
    const CrudIndexRecord& index,
    bool cleanup_horizon_excludes_old_snapshots) {
  if (!cleanup_horizon_excludes_old_snapshots) {
    return Failure(
        context_, &index, "RebuildFromRelation",
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.REBUILD_HORIZON_REQUIRED",
               "index.transactional_provider.rebuild_horizon_required",
               "in-place current-generation rebuild is refused while an older snapshot may require the prior generation"));
  }
  if (!IsAdmittedMgaTransactionalIndexFamily(index)) {
    return Failure(
        context_, &index, "RebuildFromRelation",
        Refuse("INDEX.TRANSACTIONAL_PROVIDER.FAMILY_NOT_ADMITTED",
               "index.transactional_provider.family_not_admitted",
               "family=" + ResolvedFamily(index)));
  }

  DmlTransactionalIndexProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.lifecycle_state = "rebuild_commit_publish_ready";
  AddProviderEvidence(context_, &index, "RebuildFromRelation", &result.evidence);
  for (const auto& row :
       VisibleCrudRowsForContext(state, index.table_uuid, context_)) {
    if (row.deleted) continue;
    for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
      DmlTransactionalIndexEntryRequest request;
      request.index = index;
      request.table_uuid = index.table_uuid;
      request.row_uuid = row.row_uuid;
      request.version_uuid = row.version_uuid;
      request.predecessor_version_uuid = row.previous_version_uuid;
      request.key_value = key;
      request.payload_value = CrudFieldValue(row.values, index.column_name);
      auto prepared = PrepareEntry(request, "rebuild");
      if (!prepared.ok) return prepared;
      ++result.rebuilt_entry_count;
      result.evidence.insert(result.evidence.end(),
                             prepared.evidence.begin(),
                             prepared.evidence.end());
    }
  }
  result.evidence.push_back({"transactional_index_rebuilt_entry_count",
                             std::to_string(result.rebuilt_entry_count)});
  return result;
}

DmlTransactionalIndexProviderResult
ValidateTransactionalIndexMutationSetForCommit(
    const EngineRequestContext& context,
    const RelationReadSnapshot& state) {
  MgaOrderedBtreeTransactionalIndexProvider provider(context, nullptr);
  DmlTransactionalIndexProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.lifecycle_state = "commit_publish_ready";
  AddProviderEvidence(context, nullptr, "ValidateTransactionMutationSet",
                      &result.evidence);

  std::map<std::string, std::vector<CrudIndexRecord>> indexes_by_table;
  for (const auto& index : state.indexes) {
    if (IsAdmittedMgaTransactionalIndexFamily(index) &&
        CrudCreatorVisible(state, index.creator_tx, index.event_sequence,
                           context.local_transaction_id)) {
      indexes_by_table[index.table_uuid].push_back(index);
    }
  }

  for (const auto& row : state.row_versions) {
    if (row.creator_tx != context.local_transaction_id) continue;
    const auto found_indexes = indexes_by_table.find(row.table_uuid);
    if (found_indexes == indexes_by_table.end()) continue;
    const CrudRowVersionRecord* predecessor =
        row.previous_version_uuid.empty()
            ? nullptr
            : FindVersion(state, row.table_uuid, row.previous_version_uuid);
    if (!row.previous_version_uuid.empty() && predecessor == nullptr) {
      return Failure(
          context, nullptr, "ValidateTransactionMutationSet",
          Refuse("INDEX.TRANSACTIONAL_PROVIDER.PREDECESSOR_MISSING",
                 "index.transactional_provider.predecessor_missing",
                 "row_uuid=" + row.row_uuid + ";version_uuid=" +
                     row.version_uuid));
    }
    for (const auto& index : found_indexes->second) {
      if (index.event_sequence == 0) {
        return Failure(
            context, &index, "ValidateTransactionMutationSet",
            Refuse("INDEX.TRANSACTIONAL_PROVIDER.STALE_OR_INCOMPLETE",
                   "index.transactional_provider.stale_or_incomplete",
                   "durable index generation is missing"));
      }
      const auto new_keys = row.deleted
                                ? std::vector<std::string>{}
                                : CrudIndexKeysForValues(index, row.values);
      const auto old_keys = predecessor == nullptr
                                ? std::vector<std::string>{}
                                : CrudIndexKeysForValues(index,
                                                         predecessor->values);
      const bool keys_changed = predecessor != nullptr && old_keys != new_keys;
      if (predecessor != nullptr && (row.deleted || keys_changed)) {
        for (const auto& key : old_keys) {
          if (!HasEntry(state, index, row.row_uuid, row.version_uuid, key,
                        "retire", context.local_transaction_id)) {
            return Failure(
                context, &index, "ValidateTransactionMutationSet",
                Refuse("INDEX.TRANSACTIONAL_PROVIDER.RETIRE_ENTRY_MISSING",
                       "index.transactional_provider.retire_entry_missing",
                       "row_uuid=" + row.row_uuid + ";version_uuid=" +
                           row.version_uuid + ";key=" + key));
          }
          ++result.prepared_retire_count;
        }
      }
      if (predecessor == nullptr || keys_changed) {
        for (const auto& key : new_keys) {
          if (!HasEntry(state, index, row.row_uuid, row.version_uuid, key, {},
                        context.local_transaction_id)) {
            return Failure(
                context, &index, "ValidateTransactionMutationSet",
                Refuse("INDEX.TRANSACTIONAL_PROVIDER.INSERT_ENTRY_MISSING",
                       "index.transactional_provider.insert_entry_missing",
                       "row_uuid=" + row.row_uuid + ";version_uuid=" +
                           row.version_uuid + ";key=" + key));
          }
          ++result.prepared_insert_count;
        }
      }
    }
  }

  for (const auto& [_, indexes] : indexes_by_table) {
    for (const auto& index : indexes) {
      const auto validated = provider.ValidateAgainstRelation(state, index);
      if (!validated.ok) return validated;
      result.visible_entry_count += validated.visible_entry_count;
    }
  }
  result.evidence.push_back({"transactional_index_commit_insert_count",
                             std::to_string(result.prepared_insert_count)});
  result.evidence.push_back({"transactional_index_commit_retire_count",
                             std::to_string(result.prepared_retire_count)});
  return result;
}

DmlTransactionalIndexProviderResult
ValidateOrderedBtreeTransactionalIndexMutationSetForCommit(
    const EngineRequestContext& context,
    const RelationReadSnapshot& state) {
  return ValidateTransactionalIndexMutationSetForCommit(context, state);
}

}  // namespace scratchbird::engine::internal_api
