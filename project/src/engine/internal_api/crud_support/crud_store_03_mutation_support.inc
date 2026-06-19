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
  if (predicate.predicate_kind == "vector_exact_nearest" || predicate.predicate_kind == "vector_approx_nearest") {
    return predicate.canonical_predicate_envelope == first_column && !predicate.bound_values.empty() &&
           !ParseCrudVector(entry.key_value).empty();
  }
  if (predicate.predicate_kind == "expression_equals") {
    return !predicate.bound_values.empty() && columns.size() == 1 && predicate.canonical_predicate_envelope == columns.front() &&
           entry.key_value == predicate.bound_values.front().encoded_value;
  }
  if (predicate.predicate_kind == "partial_index_probe") { return true; }
  return false;
}

}  // namespace

bool CrudRowVersionVisibleToContext(const CrudState& state,
                                    const CrudRowVersionRecord& row,
                                    const EngineRequestContext& context) {
  return CrudRowVersionVisibleToContextInternal(state, row, context);
}

std::string GenerateCrudEngineUuid(std::string kind, std::uint64_t unix_epoch_millis) {
  using scratchbird::core::platform::UuidKind;
  using scratchbird::core::uuid::GenerateEngineIdentityV7;
  using scratchbird::core::uuid::UuidToString;
  if (unix_epoch_millis == 0) { unix_epoch_millis = CurrentUnixMillis(); }
  UuidKind uuid_kind = UuidKind::object;
  if (kind == "row") { uuid_kind = UuidKind::row; }
  else if (kind == "transaction") { uuid_kind = UuidKind::transaction; }
  else if (kind == "schema") { uuid_kind = UuidKind::schema; }
  else if (kind == "database") { uuid_kind = UuidKind::database; }
  const auto generated = GenerateEngineIdentityV7(uuid_kind, unix_epoch_millis);
  if (!generated.ok()) { return {}; }
  return UuidToString(generated.value.value);
}

bool IsEmptyUuid(const EngineUuid& uuid) { return uuid.canonical.empty(); }

bool CrudCreatorVisible(const CrudState& state, std::uint64_t creator_tx, std::uint64_t, std::uint64_t observer_tx) {
  if (creator_tx == 0) { return true; }
  const auto it = state.transactions.find(creator_tx);
  if (it == state.transactions.end()) { return false; }
  if (it->second == "committed" || it->second == "archived") { return true; }
  return creator_tx == observer_tx && (it->second == "active" || it->second == "prepared");
}

std::string UuidStringOrGenerated(const EngineUuid& uuid, std::string kind) {
  if (!IsEmptyUuid(uuid)) { return uuid.canonical; }
  return GenerateCrudEngineUuid(std::move(kind));
}

EngineApiDiagnostic ValidateCrudDatabasePath(const EngineRequestContext& context, const std::string& operation_id) {
  if (context.database_path.empty()) { return MakeInvalidRequestDiagnostic(operation_id, "database_path_required"); }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic UnsupportedCrudFeatureDiagnostic(const std::string& operation_id, const std::string& feature) {
  return MakeUnsupportedProfileDiagnostic(operation_id, feature);
}

std::string EncodeCrudText(const std::string& value) { return HexEncode(value); }

std::string EncodeCrudPairs(const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::string joined;
  for (const auto& [key, value] : pairs) {
    if (!joined.empty()) { joined.push_back('|'); }
    joined += HexEncode(key);
    joined.push_back('=');
    joined += HexEncode(value);
  }
  return joined;
}

std::vector<std::pair<std::string, std::string>> DecodeCrudPairs(const std::string& encoded) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (const auto& part : Split(encoded, '|')) {
    const auto pos = part.find('=');
    if (pos == std::string::npos) { continue; }
    pairs.push_back({HexDecode(part.substr(0, pos)), HexDecode(part.substr(pos + 1))});
  }
  return pairs;
}

std::string NormalizeCrudIndexProfile(const std::string& profile) {
  if (profile.empty() || profile == kCrudIndexProfileRowStoreScalarBtreeV1 ||
      profile == "rowstore_btree_v1" || profile == "native_btree") {
    return kCrudIndexProfileRowStoreScalarBtreeV1;
  }
  if (profile == "btree" || profile == "btree_unique" ||
      profile == "native_unique_btree") {
    return profile == "native_unique_btree" ? "btree_unique" : profile;
  }
  if (profile == "rtree" || profile == "native_rtree") { return "rtree"; }
  if (profile == "geohash" || profile == "native_spatial") { return "spatial"; }
  if (profile == "postgresql_gist_profile") { return "gist"; }
  if (profile == "postgresql_spgist_profile") { return "spgist"; }
  if (profile == "inverted" || profile == "native_inverted") { return "inverted"; }
  if (profile == "fulltext" || profile == "native_full_text") { return "full_text"; }
  if (profile == "postgresql_gin_profile") { return "gin"; }
  if (profile == "native_ngram") { return "ngram"; }
  if (profile == "native_sparse_wand") { return "sparse_wand"; }
  if (profile == "native_document_path") { return "document_path"; }
  if (profile == "hnsw" || profile == "native_hnsw") { return "vector_hnsw"; }
  if (profile == "ivfflat" || profile == "ivf_flat" || profile == "ivf" || profile == "native_ivf") { return "vector_ivf"; }
  if (profile == "native_vector_exact") { return "vector_exact"; }
  if (profile == "adjacency" || profile == "graph_adjacency" || profile == "native_graph_lookup") { return "graph_adjacency"; }
  if (profile == "time_partition" || profile == "time-partition" || profile == "native_columnar_zone") { return "columnar_zone"; }
  if (profile == "native_zone_summary") { return "brin_zone"; }
  if (profile == "native_bloom_summary") { return "bloom"; }
  if (profile == "native_expression_index") { return "expression"; }
  if (profile == "native_partial_index") { return "partial"; }
  if (profile == "native_covering_index") { return "covering"; }
  if (profile == "native_hash") { return "hash"; }
  if (profile == "native_bitmap") { return "bitmap"; }
  if (profile == "native_temporary_work") { return "temporary_work"; }
  if (profile == "native_in_memory") { return "in_memory"; }
  if (profile == "native_reference_emulated") { return "reference_emulated"; }
  if (profile == "hash" || profile == "bitmap" || profile == "brin_zone" || profile == "bloom" ||
      profile == "full_text" || profile == "gin" || profile == "inverted" || profile == "ngram" ||
      profile == "sparse_wand" || profile == "spatial" || profile == "rtree" || profile == "gist" ||
      profile == "spgist" || profile == "vector_exact" || profile == "vector_hnsw" ||
      profile == "vector_ivf" || profile == "columnar_zone" || profile == "document_path" ||
      profile == "graph_adjacency" || profile == "expression" || profile == "partial" ||
      profile == "covering" || profile == "temporary_work" || profile == "in_memory" ||
      profile == "reference_emulated") {
    return profile;
  }
  if (StartsWith(profile, "reference:") || StartsWith(profile, "postgres_") || StartsWith(profile, "mysql_") ||
      StartsWith(profile, "firebird_")) {
    return kCrudIndexFamilyReferenceEmulated;
  }
  return profile;
}

std::string CrudIndexFamilyForProfile(const std::string& profile) {
  const std::string normalized = NormalizeCrudIndexProfile(profile);
  if (normalized == kCrudIndexProfileRowStoreScalarBtreeV1 || normalized == "btree" || normalized == "btree_unique") {
    return kCrudIndexFamilyBtree;
  }
  if (normalized == "hash") { return kCrudIndexFamilyHash; }
  if (normalized == "bitmap") { return kCrudIndexFamilyBitmap; }
  if (normalized == "brin_zone") { return kCrudIndexFamilyBrinZone; }
  if (normalized == "bloom") { return kCrudIndexFamilyBloom; }
  if (normalized == "full_text") { return kCrudIndexFamilyFullText; }
  if (normalized == "gin") { return kCrudIndexFamilyGin; }
  if (normalized == "inverted") { return kCrudIndexFamilyInverted; }
  if (normalized == "ngram") { return kCrudIndexFamilyNgram; }
  if (normalized == "sparse_wand") { return kCrudIndexFamilySparseWand; }
  if (normalized == "spatial") { return kCrudIndexFamilySpatial; }
  if (normalized == "rtree") { return kCrudIndexFamilyRtree; }
  if (normalized == "gist") { return kCrudIndexFamilyGist; }
  if (normalized == "spgist") { return kCrudIndexFamilySpgist; }
  if (normalized == "vector_exact") { return kCrudIndexFamilyVectorExact; }
  if (normalized == "vector_hnsw") { return kCrudIndexFamilyVectorHnsw; }
  if (normalized == "vector_ivf") { return kCrudIndexFamilyVectorIvf; }
  if (normalized == "columnar_zone") { return kCrudIndexFamilyColumnarZone; }
  if (normalized == "document_path") { return kCrudIndexFamilyDocumentPath; }
  if (normalized == "graph_adjacency") { return kCrudIndexFamilyGraphAdjacency; }
  if (normalized == "expression") { return kCrudIndexFamilyExpression; }
  if (normalized == "partial") { return kCrudIndexFamilyPartial; }
  if (normalized == "covering") { return kCrudIndexFamilyCovering; }
  if (normalized == "temporary_work") { return kCrudIndexFamilyTemporaryWork; }
  if (normalized == "in_memory") { return kCrudIndexFamilyInMemory; }
  if (normalized == "reference_emulated") { return kCrudIndexFamilyReferenceEmulated; }
  return {};
}

bool IsSupportedCrudIndexProfile(const std::string& profile) { return !CrudIndexFamilyForProfile(profile).empty(); }

bool IsApproximateCrudIndexFamily(const std::string& family) {
  return family == kCrudIndexFamilyVectorHnsw || family == kCrudIndexFamilyVectorIvf;
}

std::vector<std::pair<std::string, std::string>> RowValuePairs(const EngineRowValue& row) {
  std::vector<std::pair<std::string, std::string>> values;
  for (const auto& [field, typed] : row.fields) { values.push_back({field, typed.is_null ? "<NULL>" : typed.encoded_value}); }
  return values;
}

std::size_t EncodedValueBytes(const std::vector<std::pair<std::string, std::string>>& values) {
  std::size_t total = 0;
  for (const auto& [key, value] : values) { total += key.size() + value.size(); }
  return total;
}

bool CrudValueIsLargeValueLocator(const std::string& value) {
  return StartsWith(value, kCrudLargeValueLocatorPrefix);
}

EngineApiDiagnostic PersistCrudLargeValuesForRow(const EngineRequestContext& context,
                                                 const std::string& table_uuid,
                                                 const std::string& row_uuid,
                                                 const std::string& version_uuid,
                                                 bool force_large_value,
                                                 std::vector<std::pair<std::string, std::string>>* values,
                                                 std::vector<EngineEvidenceReference>* evidence) {
  (void)context;
  (void)table_uuid;
  (void)row_uuid;
  (void)version_uuid;
  (void)force_large_value;
  (void)values;
  (void)evidence;
  return MakeInvalidRequestDiagnostic("crud.large_value", "crud_large_value_store_retired_use_mga_relation_store");
}

std::string CrudFieldValue(const std::vector<std::pair<std::string, std::string>>& values, const std::string& field) {
  for (const auto& [key, value] : values) {
    if (key == field) { return value; }
  }
  return {};
}

std::string CrudColumnDescriptorForName(const std::vector<std::pair<std::string, std::string>>& columns,
                                        const std::string& column_name) {
  for (const auto& [name, descriptor] : columns) {
    if (name == column_name) { return descriptor; }
  }
  return {};
}

bool CrudColumnDescriptorIsOpaqueRenderOnly(const std::string& descriptor) {
  return descriptor == "opaque_extension" || StartsWith(descriptor, "opaque_extension;") ||
         descriptor.find("canonical=opaque_extension") != std::string::npos ||
         descriptor.find("base_type=opaque_extension") != std::string::npos;
}

bool CrudTypedValueIsOpaqueRenderOnly(const EngineTypedValue& value) {
  return CrudColumnDescriptorIsOpaqueRenderOnly(value.descriptor.canonical_type_name) ||
         CrudColumnDescriptorIsOpaqueRenderOnly(value.descriptor.encoded_descriptor);
}

bool CrudExpressionTouchesColumn(const std::string& expression, const std::string& column_name) {
  if (expression == column_name) { return true; }
  if ((StartsWith(expression, "lower:") || StartsWith(expression, "upper:")) && expression.substr(6) == column_name) {
    return true;
  }
  if (StartsWith(expression, "length:") && expression.substr(7) == column_name) { return true; }
  if (StartsWith(expression, "identity:") && expression.substr(9) == column_name) { return true; }
  return ExtractFunctionArgument(expression, "lower") == column_name ||
         ExtractFunctionArgument(expression, "upper") == column_name ||
         ExtractFunctionArgument(expression, "length") == column_name;
}

bool CrudPredicateTouchesOpaqueColumn(const CrudTableRecord& table, const EnginePredicateEnvelope& predicate) {
  if (predicate.predicate_kind.empty() || predicate.predicate_kind == "row_uuid_match") { return false; }
  for (const auto& bound : predicate.bound_values) {
    if (CrudTypedValueIsOpaqueRenderOnly(bound)) { return true; }
  }
  for (const auto& [column_name, descriptor] : table.columns) {
    if (!CrudColumnDescriptorIsOpaqueRenderOnly(descriptor)) { continue; }
    if (CrudExpressionTouchesColumn(predicate.canonical_predicate_envelope, column_name)) { return true; }
    if (predicate.predicate_kind == "partial_index_probe") {
      const auto pos = predicate.canonical_predicate_envelope.find('=');
      if (pos != std::string::npos && predicate.canonical_predicate_envelope.substr(0, pos) == column_name) {
        return true;
      }
    }
  }
  return false;
}

bool CrudRowsTouchOpaqueColumn(const CrudTableRecord& table, std::span<const EngineRowValue> rows) {
  for (const auto& row : rows) {
    for (const auto& [field, typed] : row.fields) {
      if (CrudTypedValueIsOpaqueRenderOnly(typed) ||
          CrudColumnDescriptorIsOpaqueRenderOnly(CrudColumnDescriptorForName(table.columns, field))) {
        return true;
      }
    }
  }
  return false;
}
