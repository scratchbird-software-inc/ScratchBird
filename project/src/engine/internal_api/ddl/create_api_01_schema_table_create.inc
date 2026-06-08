// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by create_api.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
}

bool IndexTouchesOpaqueColumn(const EngineIndexDefinition& definition,
                              const std::vector<std::pair<std::string, std::string>>& columns) {
  for (const auto& envelope : definition.key_envelopes) {
    if (StartsWithLocal(envelope, "include:")) {
      if (DescriptorIsOpaqueRenderOnly(ColumnDescriptorForName(columns, envelope.substr(8)))) { return true; }
      continue;
    }
    if (StartsWithLocal(envelope, "where_eq:")) {
      const std::string condition = envelope.substr(9);
      const auto pos = condition.find('=');
      if (pos != std::string::npos &&
          DescriptorIsOpaqueRenderOnly(ColumnDescriptorForName(columns, condition.substr(0, pos)))) {
        return true;
      }
      continue;
    }
    const std::string column_name = ColumnNameFromIndexEnvelope(envelope);
    if (!column_name.empty() && DescriptorIsOpaqueRenderOnly(ColumnDescriptorForName(columns, column_name))) { return true; }
  }
  return false;
}

CrudIndexRecord BuildCrudIndexRecord(const EngineIndexDefinition& definition,
                                     const std::string& table_uuid,
                                     const std::string& index_uuid,
                                     const std::string& index_name) {
  CrudIndexRecord index;
  const std::string requested_profile = !definition.index_kind.empty() ? definition.index_kind : definition.physical_profile;
  index.profile = NormalizeCrudIndexProfile(requested_profile);
  index.family = CrudIndexFamilyForProfile(index.profile);
  index.index_uuid = index_uuid;
  index.table_uuid = table_uuid;
  index.default_name = index_name;
  index.unique = index.profile == "btree_unique";
  index.approximate = IsApproximateCrudIndexFamily(index.family);
  index.exact_fallback = index.approximate;
  for (const auto& envelope : definition.key_envelopes) {
    if (envelope == "unique") {
      index.unique = true;
    } else if (StartsWithLocal(envelope, "include:")) {
      index.include_columns.push_back(envelope.substr(8));
    } else if (StartsWithLocal(envelope, "where_eq:")) {
      const std::string condition = envelope.substr(9);
      const auto pos = condition.find('=');
      if (pos != std::string::npos) {
        index.predicate_kind = "where_eq";
        index.predicate_column = condition.substr(0, pos);
        index.predicate_value = condition.substr(pos + 1);
      }
    } else if (!envelope.empty()) {
      index.key_envelopes.push_back(envelope);
    }
  }
  if (!index.key_envelopes.empty()) { index.column_name = index.key_envelopes.front(); }
  return index;
}

bool ExistingRowsHaveDuplicateKeys(const CrudState& state,
                                   const CrudIndexRecord& index,
                                   const std::string& table_uuid,
                                   std::uint64_t observer_tx) {
  std::set<std::string> seen;
  for (const auto& row : VisibleCrudRows(state, table_uuid, observer_tx)) {
    for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
      if (!seen.insert(key).second) { return true; }
    }
  }
  return false;
}

template <typename TResult, typename TRequest>
TResult PersistedCreateWithNameRegistry(const TRequest& request,
                                        const std::string& operation_id,
                                        const std::string& object_kind,
                                        bool require_transaction,
                                        const std::string& state) {
  auto result = PersistedRecordResult<TResult>(request, operation_id, object_kind, require_transaction, state);
  if (!result.ok) { return result; }
  const std::string fallback_name = ApiBehaviorPrimaryName(request, object_kind);
  const std::string scope_uuid = request.target_schema.uuid.canonical.empty()
                                     ? request.target_object.uuid.canonical
                                     : request.target_schema.uuid.canonical;
  const auto name_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                operation_id,
                                                                result.primary_object.uuid.canonical,
                                                                object_kind,
                                                                scope_uuid,
                                                                request.localized_names,
                                                                fallback_name);
  if (name_appended.error) {
    return MakeApiBehaviorDiagnostic<TResult>(request.context, operation_id, name_appended);
  }
  AddApiBehaviorEvidence(&result, "name_registry", result.primary_object.uuid.canonical);
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DDL_CREATE_API_STUBS

EngineCreateDatabaseResult EngineCreateDatabase(const EngineCreateDatabaseRequest& request) {
  return PersistedCreateWithNameRegistry<EngineCreateDatabaseResult>(request, "ddl.create_database", "database", false, "created");
}

EngineCreateSchemaResult EngineCreateSchema(const EngineCreateSchemaRequest& request) {
  const auto context_status = ValidateApiBehaviorContext(request.context, "ddl.create_schema", true, true);
  if (context_status.error) {
    return MakeApiBehaviorDiagnostic<EngineCreateSchemaResult>(request.context, "ddl.create_schema", context_status);
  }
  const std::string schema_uuid = ApiBehaviorObjectUuid(request, "schema");
  if (FindVisibleSchemaTreeRecord(request.context, schema_uuid, request.context.local_transaction_id)) {
    return MakeApiBehaviorDiagnostic<EngineCreateSchemaResult>(
        request.context,
        "ddl.create_schema",
        MakeInvalidRequestDiagnostic("ddl.create_schema", "schema_uuid_already_exists"));
  }
  const std::string parent_schema_uuid = request.target_schema.uuid.canonical;
  if (!parent_schema_uuid.empty() &&
      !FindVisibleSchemaTreeRecord(request.context, parent_schema_uuid, request.context.local_transaction_id)) {
    return MakeApiBehaviorDiagnostic<EngineCreateSchemaResult>(
        request.context,
        "ddl.create_schema",
        MakeInvalidRequestDiagnostic("ddl.create_schema", "parent_schema_not_visible"));
  }
  std::vector<EngineLocalizedName> names = request.localized_names;
  if (names.empty()) {
    const std::string fallback = schema_uuid.empty() ? "unnamed_schema" : schema_uuid;
    names.push_back({"en", "default", fallback, fallback, true});
  }
  if (const auto conflict = SchemaTreePathConflict(request.context,
                                                  schema_uuid,
                                                  parent_schema_uuid,
                                                  names,
                                                  request.context.local_transaction_id)) {
    return MakeApiBehaviorDiagnostic<EngineCreateSchemaResult>(
        request.context,
        "ddl.create_schema",
        MakeInvalidRequestDiagnostic("ddl.create_schema", "schema_path_ambiguous:" + *conflict));
  }
  EngineSchemaTreeRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.schema_uuid = schema_uuid;
  record.parent_schema_uuid = parent_schema_uuid;
  record.localized_names = names;
  record.default_name = SchemaTreeDefaultName(names, "unnamed_schema");
  for (const auto& option : request.option_envelopes) {
    if (StartsWithLocal(option, "comment:")) {
      const std::string text = option.substr(8);
      record.localized_comments.push_back({"und", text});
    } else if (StartsWithLocal(option, "localized_comment:")) {
      const std::string rest = option.substr(18);
      const auto pos = rest.find(':');
      if (pos == std::string::npos) {
        return MakeApiBehaviorDiagnostic<EngineCreateSchemaResult>(
            request.context,
            "ddl.create_schema",
            MakeInvalidRequestDiagnostic("ddl.create_schema", "localized_comment_requires_language_and_text"));
      }
      record.localized_comments.push_back({rest.substr(0, pos), rest.substr(pos + 1)});
    } else {
      return MakeApiBehaviorDiagnostic<EngineCreateSchemaResult>(
          request.context,
