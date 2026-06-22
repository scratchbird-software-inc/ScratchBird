// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by crud_store.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  value.encoded_value = key_value;
  predicate.bound_values.push_back(value);
  return IndexedCrudRowsForPredicate(state, table_uuid, predicate, observer_tx, 0, index_uuid_used);
}

std::vector<CrudRowVersionRecord> IndexedCrudRowsForPredicate(const CrudState& state,
                                                              const std::string& table_uuid,
                                                              const EnginePredicateEnvelope& predicate,
                                                              std::uint64_t observer_tx,
                                                              std::uint64_t limit,
                                                              std::string* index_evidence_id) {
  EngineRequestContext context;
  context.local_transaction_id = observer_tx;
  context.transaction_isolation_level = "read_committed";
  return IndexedCrudRowsForPredicateForContext(state, table_uuid, predicate, context, limit, index_evidence_id);
}

std::vector<CrudRowVersionRecord> IndexedCrudRowsForPredicateForContext(const CrudState& state,
                                                                        const std::string& table_uuid,
                                                                        const EnginePredicateEnvelope& predicate,
                                                                        const EngineRequestContext& context,
                                                                        std::uint64_t limit,
                                                                        std::string* index_evidence_id) {
  const std::uint64_t observer_tx = context.local_transaction_id;
  std::optional<CrudIndexRecord> selected;
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (CrudIndexSupportsPredicate(index, predicate)) { selected = index; break; }
  }
  if (!selected) { return {}; }

  const bool vector_ordered =
      predicate.predicate_kind == "vector_exact_nearest" ||
      predicate.predicate_kind == "vector_approx_nearest";
  std::size_t candidate_count = 0;
  if (!vector_ordered) {
    std::unordered_map<std::string, const CrudRowVersionRecord*> newest_visible_by_uuid;
    for (const auto& row : state.row_versions) {
      if (row.table_uuid != table_uuid ||
          !CrudRowVersionVisibleToContext(state, row, context)) {
        continue;
      }
      const auto found = newest_visible_by_uuid.find(row.row_uuid);
      if (found == newest_visible_by_uuid.end() ||
          row.sequence > found->second->sequence) {
        newest_visible_by_uuid[row.row_uuid] = &row;
      }
    }
    std::unordered_set<std::string> in_list_key_values;
    bool use_in_list_key_values = false;
    if (predicate.predicate_kind == "column_in_list" &&
        !predicate.canonical_predicate_envelope.empty()) {
      const auto columns = CrudIndexKeyColumns(*selected);
      const std::string first_column =
          columns.empty() ? selected->column_name : columns.front();
      if (predicate.canonical_predicate_envelope == first_column) {
        in_list_key_values.reserve(predicate.bound_values.size());
        for (const auto& bound : predicate.bound_values) {
          in_list_key_values.insert(bound.encoded_value);
        }
        use_in_list_key_values = !in_list_key_values.empty();
      }
    }
    const auto entry_matches_predicate = [&](const CrudIndexEntryRecord& entry) {
      if (!use_in_list_key_values) {
        return EntryKeyMatchesPredicate(*selected, entry, predicate);
      }
      return in_list_key_values.find(FirstCompositeComponent(entry.key_value)) !=
             in_list_key_values.end();
    };
    const auto row_matches_predicate = [&](const CrudRowVersionRecord& row) {
      if (!use_in_list_key_values) {
        return CrudRowMatchesPredicate(row, predicate);
      }
      const std::string value =
          CrudFieldValue(row.values, predicate.canonical_predicate_envelope);
      return in_list_key_values.find(value) != in_list_key_values.end();
    };
    std::set<std::string> seen_row_uuids;
    std::vector<CrudRowVersionRecord> rows;
    for (const auto& entry : state.index_entries) {
      if (entry.index_uuid != selected->index_uuid ||
          entry.table_uuid != table_uuid ||
          !TxVisible(state, entry.creator_tx, observer_tx)) {
        continue;
      }
      if (!entry_matches_predicate(entry)) { continue; }
      ++candidate_count;
      if (!seen_row_uuids.insert(entry.row_uuid).second) { continue; }
      const auto row = newest_visible_by_uuid.find(entry.row_uuid);
      if (row != newest_visible_by_uuid.end() && row->second != nullptr &&
          !row->second->deleted && row_matches_predicate(*row->second)) {
        rows.push_back(*row->second);
        if (limit != 0 && rows.size() >= limit) { break; }
      }
    }
    if (index_evidence_id != nullptr) {
      *index_evidence_id = CrudIndexEvidenceId(*selected, predicate, candidate_count, rows.size());
    }
    return rows;
  }

  std::map<std::string, double> candidate_scores;
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != selected->index_uuid || entry.table_uuid != table_uuid || !TxVisible(state, entry.creator_tx, observer_tx)) {
      continue;
    }
    if (!EntryKeyMatchesPredicate(*selected, entry, predicate)) { continue; }
    ++candidate_count;
    double score = 0.0;
    if (!predicate.bound_values.empty()) {
      score = CrudVectorDistance(entry.key_value, predicate.bound_values.front().encoded_value);
    }
    const auto existing = candidate_scores.find(entry.row_uuid);
    if (existing == candidate_scores.end() || score < existing->second) { candidate_scores[entry.row_uuid] = score; }
  }

  std::vector<std::pair<CrudRowVersionRecord, double>> visible_scored;
  for (const auto& [row_uuid, score] : candidate_scores) {
    const auto row = FindVisibleCrudRowForContext(state, table_uuid, row_uuid, context);
    if (row && CrudRowMatchesPredicate(*row, predicate)) { visible_scored.push_back({*row, score}); }
  }
  std::sort(visible_scored.begin(), visible_scored.end(), [](const auto& left, const auto& right) {
    if (left.second != right.second) { return left.second < right.second; }
    return left.first.row_uuid < right.first.row_uuid;
  });
  std::vector<CrudRowVersionRecord> rows;
  for (const auto& [row, ignored_score] : visible_scored) {
    if (limit != 0 && rows.size() >= limit) { break; }
    rows.push_back(row);
  }
  if (index_evidence_id != nullptr) { *index_evidence_id = CrudIndexEvidenceId(*selected, predicate, candidate_count, rows.size()); }
  return rows;
}

std::string MakeCrudRowVersionEvent(std::uint64_t creator_tx,
                                    const std::string& table_uuid,
                                    const std::string& row_uuid,
                                    const std::string& version_uuid,
                                    bool deleted,
                                    const std::string& previous_version_uuid,
                                    std::uint64_t previous_sequence,
                                    const std::vector<std::pair<std::string, std::string>>& values) {
  (void)creator_tx;
  (void)table_uuid;
  (void)row_uuid;
  (void)version_uuid;
  (void)deleted;
  (void)previous_version_uuid;
  (void)previous_sequence;
  (void)values;
  return "SBCRUD_RETIRED_ROW_VERSION_USE_SBMGA1";
}

std::string CrudIndexEvidenceId(const CrudIndexRecord& index,
                                const EnginePredicateEnvelope& predicate,
                                std::size_t candidate_count,
                                std::size_t visible_count) {
  const std::string family = index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  const std::string exactness = IsApproximateCrudIndexFamily(family) ? "approximate" : "exact";
  const std::string fallback = IsApproximateCrudIndexFamily(family) ? "exact_scan_compatibility_fallback" : "none";
  return index.index_uuid + "|index_family=" + family + "|index_profile=" + index.profile +
         "|predicate_kind=" + predicate.predicate_kind + "|candidate_count=" + std::to_string(candidate_count) +
         "|visible_count=" + std::to_string(visible_count) + "|row_recheck_applied=true|exactness=" + exactness +
         "|fallback_mode=" + fallback;
}

std::string MakeCrudIndexCreateEvent(std::uint64_t creator_tx,
                                     const std::string& index_uuid,
                                     const std::string& table_uuid,
                                     const std::string& column_name,
                                     const std::string& profile,
                                     const std::string& default_name) {
  return std::string("SBCRUD1\tINDEX_CREATE\t") + std::to_string(creator_tx) + "\t" + index_uuid + "\t" + table_uuid +
         "\t" + EncodeCrudText(column_name) + "\t" + profile + "\t" + EncodeCrudText(default_name);
}

std::string MakeCrudIndexCreateEventV2(std::uint64_t creator_tx, const CrudIndexRecord& index) {
  return std::string("SBCRUD1\tINDEX_CREATE_V2\t") + std::to_string(creator_tx) + "\t" + index.index_uuid + "\t" +
         index.table_uuid + "\t" + index.profile + "\t" + index.family + "\t" + EncodeCrudText(index.default_name) +
         "\t" + EncodeCrudText(index.column_name) + "\t" + EncodeCrudList(index.key_envelopes) + "\t" +
         EncodeCrudList(index.include_columns) + "\t" + index.predicate_kind + "\t" + EncodeCrudText(index.predicate_column) +
         "\t" + EncodeCrudText(index.predicate_value) + "\t" + (index.unique ? "1" : "0");
}

std::string MakeCrudIndexEntryEvent(std::uint64_t creator_tx,
                                    const std::string& index_uuid,
                                    const std::string& table_uuid,
                                    const std::string& column_name,
                                    const std::string& key_value,
                                    const std::string& row_uuid,
                                    const std::string& version_uuid) {
  (void)creator_tx;
  (void)index_uuid;
  (void)table_uuid;
  (void)column_name;
  (void)key_value;
  (void)row_uuid;
  (void)version_uuid;
  return "SBCRUD_RETIRED_INDEX_ENTRY_USE_SBMGA1";
}

std::string MakeCrudIndexEntryEventV2(std::uint64_t creator_tx,
                                      const CrudIndexRecord& index,
                                      const std::string& key_value,
                                      const std::string& payload_value,
                                      const std::string& row_uuid,
                                      const std::string& version_uuid) {
  (void)creator_tx;
  (void)index;
  (void)key_value;
  (void)payload_value;
  (void)row_uuid;
  (void)version_uuid;
  return "SBCRUD_RETIRED_INDEX_ENTRY_V2_USE_SBMGA1";
}

EngineApiDiagnostic ValidateCrudUniqueIndexesForRow(const CrudState& state,
                                                    const std::string& table_uuid,
                                                    const std::string& row_uuid,
                                                    const std::vector<std::pair<std::string, std::string>>& values,
                                                    const EngineRequestContext& context) {
  const std::uint64_t observer_tx = context.local_transaction_id;
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (!index.unique) { continue; }
    const auto keys = CrudIndexEntryKeysForValues(index, values);
    for (const auto& key : keys) {
      for (const auto& entry : state.index_entries) {
        if (entry.index_uuid != index.index_uuid || entry.table_uuid != table_uuid || entry.row_uuid == row_uuid ||
            entry.key_value != key || !TxVisible(state, entry.creator_tx, observer_tx)) {
          continue;
        }
        const auto visible_row =
            FindVisibleCrudRowForContext(state, table_uuid, entry.row_uuid, context);
        if (visible_row) { return MakeInvalidRequestDiagnostic("crud.unique_index", "unique_index_duplicate"); }
      }
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic AppendCrudIndexEntriesForIndex(const EngineRequestContext& context,
                                                   const CrudIndexRecord& index,
                                                   const std::string& row_uuid,
                                                   const std::string& version_uuid,
                                                   const std::vector<std::pair<std::string, std::string>>& values) {
  (void)context;
  (void)index;
  (void)row_uuid;
  (void)version_uuid;
  (void)values;
  return MakeInvalidRequestDiagnostic("crud.index_entry", "crud_index_entry_store_retired_use_mga_relation_store");
}

EngineApiDiagnostic AppendCrudIndexEntriesForRow(const EngineRequestContext& context,
                                                 const CrudState& state,
                                                 const std::string& table_uuid,
                                                 const std::string& row_uuid,
                                                 const std::string& version_uuid,
                                                 const std::vector<std::pair<std::string, std::string>>& values) {
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, context.local_transaction_id)) {
    const auto appended = AppendCrudIndexEntriesForIndex(context, index, row_uuid, version_uuid, values);
    if (appended.error) { return appended; }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineResultShape CrudRowsToResultShape(const std::vector<CrudRowVersionRecord>& rows) {
  EngineResultShape shape;
  shape.result_kind = "crud_rows";
  for (const auto& row : rows) {
    EngineRowValue out_row;
    out_row.requested_row_uuid.canonical = row.row_uuid;
    for (const auto& [field, value] : row.values) {
      EngineTypedValue typed;
      typed.encoded_value = value;
      typed.is_null = value == "<NULL>";
      out_row.fields.push_back({field, typed});
    }
    shape.rows.push_back(std::move(out_row));
  }
  return shape;
}

void AddEmbeddedTrustModeEvidence(const EngineRequestContext& context, EngineApiResult* result) {
  if (context.trust_mode == EngineTrustMode::embedded_in_process) {
    result->embedded_trust_mode_observed = true;
    result->diagnostics.push_back(MakeEmbeddedTrustModeDiagnostic(result->operation_id));
  }
}

}  // namespace scratchbird::engine::internal_api
