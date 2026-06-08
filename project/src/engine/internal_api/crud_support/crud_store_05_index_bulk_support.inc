// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by crud_store.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  }
  FilterTemporaryObjectsForSession(context, &result.state);

  result.ok = true;
  return result;
}

EngineApiDiagnostic AppendCrudEvent(const EngineRequestContext& context, const std::string& event) {
  const auto path_status = ValidateCrudDatabasePath(context, "crud.append_event");
  if (path_status.error) { return path_status; }
  const auto authority_status = ValidateCrudMutatingTransactionAuthority(context);
  if (authority_status.error) { return authority_status; }
  std::ofstream out(CrudEventPath(context), std::ios::binary | std::ios::app);
  if (!out) { return MakeInvalidRequestDiagnostic("crud.append_event", "database_path_unwritable"); }
  out << '\n' << event << '\n';
  out.flush();
  if (!out) { return MakeInvalidRequestDiagnostic("crud.append_event", "database_write_failed"); }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ApplyCrudTemporaryOnCommitActions(const EngineRequestContext& context,
                                                      std::uint64_t local_transaction_id,
                                                      std::uint64_t* deleted_row_count) {
  if (deleted_row_count != nullptr) { *deleted_row_count = 0; }
  (void)context;
  (void)local_transaction_id;
  return MakeInvalidRequestDiagnostic("crud.temporary", "crud_temporary_on_commit_retired_use_mga_relation_store");
}

std::optional<CrudTableRecord> FindVisibleCrudTable(const CrudState& state,
                                                    const std::string& table_uuid,
                                                    std::uint64_t observer_tx) {
  std::optional<CrudTableRecord> visible;
  for (const auto& table : state.tables) {
    if (table.table_uuid == table_uuid && TxVisible(state, table.creator_tx, observer_tx)) { visible = table; }
  }
  return visible;
}

std::vector<CrudRowVersionRecord> VisibleCrudRows(const CrudState& state,
                                                  const std::string& table_uuid,
                                                  std::uint64_t observer_tx) {
  EngineRequestContext context;
  context.local_transaction_id = observer_tx;
  context.transaction_isolation_level = "read_committed";
  return VisibleCrudRowsForContext(state, table_uuid, context);
}

std::vector<CrudRowVersionRecord> VisibleCrudRowsForContext(const CrudState& state,
                                                            const std::string& table_uuid,
                                                            const EngineRequestContext& context) {
  std::map<std::string, std::vector<CrudRowVersionRecord>> chains;
  for (const auto& row : state.row_versions) {
    if (row.table_uuid == table_uuid) { chains[row.row_uuid].push_back(row); }
  }
  std::vector<CrudRowVersionRecord> visible;
  for (auto& [row_uuid, versions] : chains) {
    std::sort(versions.begin(), versions.end(), [](const auto& left, const auto& right) {
      return left.sequence > right.sequence;
    });
    for (const auto& row : versions) {
      if (!CrudRowVersionVisibleToContext(state, row, context)) { continue; }
      if (!row.deleted) { visible.push_back(row); }
      break;
    }
  }
  return visible;
}

std::optional<CrudRowVersionRecord> FindVisibleCrudRow(const CrudState& state,
                                                       const std::string& table_uuid,
                                                       const std::string& row_uuid,
                                                       std::uint64_t observer_tx) {
  EngineRequestContext context;
  context.local_transaction_id = observer_tx;
  context.transaction_isolation_level = "read_committed";
  return FindVisibleCrudRowForContext(state, table_uuid, row_uuid, context);
}

std::optional<CrudRowVersionRecord> FindVisibleCrudRowForContext(const CrudState& state,
                                                                 const std::string& table_uuid,
                                                                 const std::string& row_uuid,
                                                                 const EngineRequestContext& context) {
  for (const auto& row : VisibleCrudRowsForContext(state, table_uuid, context)) {
    if (row.row_uuid == row_uuid) { return row; }
  }
  return std::nullopt;
}

std::vector<CrudIndexRecord> VisibleCrudIndexesForTable(const CrudState& state,
                                                        const std::string& table_uuid,
                                                        std::uint64_t observer_tx) {
  std::vector<CrudIndexRecord> indexes;
  for (const auto& index : state.indexes) {
    if (index.table_uuid == table_uuid && TxVisible(state, index.creator_tx, observer_tx)) { indexes.push_back(index); }
  }
  return indexes;
}

std::vector<CrudIndexRecord> VisibleCrudIndexesForTableColumn(const CrudState& state,
                                                              const std::string& table_uuid,
                                                              const std::string& column_name,
                                                              std::uint64_t observer_tx) {
  std::vector<CrudIndexRecord> indexes;
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    const auto columns = CrudIndexKeyColumns(index);
    if (!columns.empty() && columns.front() == column_name) { indexes.push_back(index); }
  }
  return indexes;
}

bool CrudIndexSupportsPredicate(const CrudIndexRecord& index, const EnginePredicateEnvelope& predicate) {
  const auto columns = CrudIndexKeyColumns(index);
  const std::string first_column = columns.empty() ? index.column_name : columns.front();
  const std::string family = index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  if (predicate.predicate_kind == "column_equals") {
    return predicate.canonical_predicate_envelope == first_column &&
           (family == kCrudIndexFamilyBtree || family == kCrudIndexFamilyHash || family == kCrudIndexFamilyBitmap ||
            family == kCrudIndexFamilyCovering || family == kCrudIndexFamilyColumnarZone ||
            family == kCrudIndexFamilyInMemory || family == kCrudIndexFamilyDonorEmulated);
  }
  if (predicate.predicate_kind == "column_in_list") {
    return predicate.canonical_predicate_envelope == first_column &&
           (family == kCrudIndexFamilyBtree || family == kCrudIndexFamilyBitmap);
  }
  if (predicate.predicate_kind == "column_range") {
    return predicate.canonical_predicate_envelope == first_column &&
           (family == kCrudIndexFamilyBtree || family == kCrudIndexFamilyColumnarZone);
  }
  if (predicate.predicate_kind == "text_term_contains" || predicate.predicate_kind == "text_all_terms") {
    return family == kCrudIndexFamilyFullText && predicate.canonical_predicate_envelope == first_column;
  }
  if (predicate.predicate_kind == "spatial_bbox_intersects" || predicate.predicate_kind == "spatial_bbox_contains") {
    return family == kCrudIndexFamilySpatial && predicate.canonical_predicate_envelope == first_column;
  }
  if (predicate.predicate_kind == "vector_exact_nearest") {
    return family == kCrudIndexFamilyVectorExact && predicate.canonical_predicate_envelope == first_column;
  }
  if (predicate.predicate_kind == "vector_approx_nearest") {
    return (family == kCrudIndexFamilyVectorHnsw || family == kCrudIndexFamilyVectorIvf) &&
           predicate.canonical_predicate_envelope == first_column;
  }
  if (predicate.predicate_kind == "expression_equals") {
    return family == kCrudIndexFamilyExpression && columns.size() == 1 && predicate.canonical_predicate_envelope == columns.front();
  }
  if (predicate.predicate_kind == "partial_index_probe") { return family == kCrudIndexFamilyPartial; }
  return false;
}

bool CrudRowMatchesPredicate(const CrudRowVersionRecord& row, const EnginePredicateEnvelope& predicate) {
  if (predicate.predicate_kind.empty()) { return true; }
  if (predicate.predicate_kind == "row_uuid_match") { return row.row_uuid == predicate.canonical_predicate_envelope; }
  if (predicate.predicate_kind == "row_uuid_in_list") {
    for (const auto& bound : predicate.bound_values) {
      if (row.row_uuid == bound.encoded_value) { return true; }
    }
    return false;
  }
  if (predicate.predicate_kind == "column_equals") {
    return !predicate.bound_values.empty() &&
           CrudFieldValue(row.values, predicate.canonical_predicate_envelope) == predicate.bound_values.front().encoded_value;
  }
  if (predicate.predicate_kind == "column_in_list") {
    const std::string value = CrudFieldValue(row.values, predicate.canonical_predicate_envelope);
    for (const auto& bound : predicate.bound_values) {
      if (value == bound.encoded_value) { return true; }
    }
    return false;
  }
  if (predicate.predicate_kind == "column_range") {
    const std::string value = CrudFieldValue(row.values, predicate.canonical_predicate_envelope);
    const bool lower_ok = predicate.bound_values.empty() || CompareCrudScalar(value, predicate.bound_values[0].encoded_value) >= 0;
    const bool upper_ok = predicate.bound_values.size() < 2 || CompareCrudScalar(value, predicate.bound_values[1].encoded_value) <= 0;
    return lower_ok && upper_ok;
  }
  if (predicate.predicate_kind == "text_term_contains") {
    const std::string term = LowerAscii(!predicate.bound_values.empty() ? predicate.bound_values.front().encoded_value : predicate.canonical_predicate_envelope);
    const auto tokens = TokenizeCrudText(CrudFieldValue(row.values, predicate.canonical_predicate_envelope));
    return std::find(tokens.begin(), tokens.end(), term) != tokens.end();
  }
  if (predicate.predicate_kind == "text_all_terms") {
    const auto tokens = TokenizeCrudText(CrudFieldValue(row.values, predicate.canonical_predicate_envelope));
    for (const auto& bound : predicate.bound_values) {
      if (std::find(tokens.begin(), tokens.end(), LowerAscii(bound.encoded_value)) == tokens.end()) { return false; }
    }
    return !predicate.bound_values.empty();
  }
  if (predicate.predicate_kind == "spatial_bbox_intersects" || predicate.predicate_kind == "spatial_bbox_contains") {
    if (predicate.bound_values.empty()) { return false; }
    const auto row_box = ParseCrudBBox(CrudFieldValue(row.values, predicate.canonical_predicate_envelope));
    const auto query_box = ParseCrudBBox(predicate.bound_values.front().encoded_value);
    if (predicate.predicate_kind == "spatial_bbox_intersects") { return BBoxIntersects(row_box, query_box); }
    return BBoxContains(row_box, query_box);
  }
  if (predicate.predicate_kind == "vector_exact_nearest" || predicate.predicate_kind == "vector_approx_nearest") {
    return !predicate.bound_values.empty() &&
           std::isfinite(CrudVectorDistance(CrudFieldValue(row.values, predicate.canonical_predicate_envelope),
                                            predicate.bound_values.front().encoded_value));
  }
  if (predicate.predicate_kind == "expression_equals") {
    return !predicate.bound_values.empty() &&
           ExpressionKeyForValues(predicate.canonical_predicate_envelope, row.values) == predicate.bound_values.front().encoded_value;
  }
  if (predicate.predicate_kind == "partial_index_probe") {
    const auto pos = predicate.canonical_predicate_envelope.find('=');
    if (pos == std::string::npos) { return true; }
    return CrudFieldValue(row.values, predicate.canonical_predicate_envelope.substr(0, pos)) ==
           predicate.canonical_predicate_envelope.substr(pos + 1);
  }
  return false;
}

std::vector<std::string> CrudIndexKeysForValues(const CrudIndexRecord& index,
                                                const std::vector<std::pair<std::string, std::string>>& values) {
  return CrudIndexEntryKeysForValues(index, values);
}

std::vector<CrudRowVersionRecord> IndexedCrudRows(const CrudState& state,
                                                  const std::string& table_uuid,
                                                  const std::string& column_name,
                                                  const std::string& key_value,
                                                  std::uint64_t observer_tx,
                                                  std::string* index_uuid_used) {
  EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = column_name;
  EngineTypedValue value;
