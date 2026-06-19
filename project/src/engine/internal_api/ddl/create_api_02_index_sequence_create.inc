// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by create_api.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
          "ddl.create_schema",
          UnsupportedCrudFeatureDiagnostic("ddl.create_schema", "unsupported_schema_option"));
    }
  }
  record.payload = SchemaTreePayload(record.parent_schema_uuid, record.localized_names, record.localized_comments);
  const auto appended = PersistSchemaTreeRecord(request.context, record, "ddl.create_schema");
  if (appended.error) {
    return MakeApiBehaviorDiagnostic<EngineCreateSchemaResult>(request.context, "ddl.create_schema", appended);
  }
  const auto name_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                "ddl.create_schema",
                                                                record.schema_uuid,
                                                                "schema",
                                                                record.parent_schema_uuid,
                                                                record.localized_names,
                                                                record.default_name);
  if (name_appended.error) {
    return MakeApiBehaviorDiagnostic<EngineCreateSchemaResult>(request.context, "ddl.create_schema", name_appended);
  }
  auto result = MakeApiBehaviorSuccess<EngineCreateSchemaResult>(request.context, "ddl.create_schema");
  result.primary_object.uuid.canonical = record.schema_uuid;
  result.primary_object.object_kind = "schema";
  result.catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
  AddApiBehaviorEvidence(&result, "api_behavior_event", "ddl.create_schema");
  AddApiBehaviorEvidence(&result, "schema", record.schema_uuid);
  AddApiBehaviorRow(&result, {{"object_uuid", record.schema_uuid},
                              {"object_kind", "schema"},
                              {"name", record.default_name},
                              {"state", "active"},
                              {"payload", record.payload}});
  return result;
}

EngineCreateSynonymResult EngineCreateSynonym(const EngineCreateSynonymRequest& request) {
  constexpr const char* kOperation = "ddl.create_synonym";
  EngineCatalogCreateObjectRequest catalog_request;
  static_cast<EngineApiRequest&>(catalog_request) = request;
  catalog_request.operation_id = kOperation;
  catalog_request.target_object.object_kind = "synonym";

  const auto created = EngineCatalogCreateObject(catalog_request);
  EngineCreateSynonymResult result;
  result.ok = created.ok;
  result.operation_id = kOperation;
  result.diagnostics = created.diagnostics;
  result.unsupported_features = created.unsupported_features;
  result.evidence = created.evidence;
  result.result_shape = created.result_shape;
  result.primary_object = created.primary_object;
  result.catalog_row_uuid = created.catalog_row_uuid;
  result.transaction_uuid = created.transaction_uuid;
  result.local_transaction_id = created.local_transaction_id;
  result.embedded_trust_mode_observed = created.embedded_trust_mode_observed;
  result.cluster_authority_required = created.cluster_authority_required;
  result.bound_object_identity = created.bound_object_identity;
  result.metadata_cache_epoch = created.metadata_cache_epoch;
  if (result.ok) {
    result.evidence.push_back({"ddl_catalog_route", "sys.catalog.synonym"});
  }
  return result;
}

EngineCreateTableResult EngineCreateTable(const EngineCreateTableRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(
        request.context,
        "ddl.create_table",
        MakeInvalidRequestDiagnostic("ddl.create_table", "local_transaction_id_required"));
  }
  if (!SecurityOptionValue(request, "unresolved_schema_parent_path:").empty()) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(
        request.context,
        "ddl.create_table",
        MakeInvalidRequestDiagnostic("ddl.create_table", "parent_schema_not_visible"));
  }
  std::string target_schema_uuid = request.target_schema.uuid.canonical;
  if (target_schema_uuid.empty()) {
    target_schema_uuid = request.context.current_schema_uuid.canonical;
  }
  if (target_schema_uuid.empty()) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(
        request.context,
        "ddl.create_table",
        MakeInvalidRequestDiagnostic("ddl.create_table", "target_schema_uuid_required"));
  }
  if (!FindVisibleSchemaTreeRecord(request.context,
                                   target_schema_uuid,
                                   request.context.local_transaction_id)) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(
        request.context,
        "ddl.create_table",
        MakeInvalidRequestDiagnostic("ddl.create_table", "target_schema_not_visible"));
  }
  if (request.table_columns.empty()) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(
        request.context,
        "ddl.create_table",
        MakeInvalidRequestDiagnostic("ddl.create_table", "at_least_one_column_required"));
  }
  if (!request.table_constraints.empty()) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(
        request.context,
        "ddl.create_table",
        UnsupportedCrudFeatureDiagnostic("ddl.create_table", "constraints"));
  }
  const auto loaded = LoadCrudState(request.context);
  if (!loaded.ok) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(request.context, "ddl.create_table", loaded.diagnostic);
  }
  const std::string table_uuid = UuidStringOrGenerated(request.requested_table_uuid, "object");
  std::string table_name = "unnamed_table";
  if (!request.table_names.empty() && !request.table_names.front().name.empty()) {
    table_name = request.table_names.front().name;
  } else if (!request.localized_names.empty() && !request.localized_names.front().name.empty()) {
    table_name = request.localized_names.front().name;
  }
  std::vector<std::pair<std::string, std::string>> columns;
  for (const auto& column : request.table_columns) {
    std::string column_name = "column_" + std::to_string(column.ordinal);
    if (!column.names.empty() && !column.names.front().name.empty()) { column_name = column.names.front().name; }
    std::string column_descriptor = column.descriptor.encoded_descriptor.empty()
                                        ? column.descriptor.canonical_type_name
                                        : column.descriptor.encoded_descriptor;
    auto descriptor_has_field = [&column_descriptor](const std::string& field) {
      const std::string prefix = field + "=";
      return StartsWithLocal(column_descriptor, prefix) ||
             column_descriptor.find(";" + prefix) != std::string::npos;
    };
    auto append_descriptor_field = [&column_descriptor, &descriptor_has_field](
                                       const std::string& field,
                                       const std::string& value) {
      if (value.empty() || descriptor_has_field(field)) return;
      if (!column_descriptor.empty()) column_descriptor.push_back(';');
      column_descriptor += field;
      column_descriptor.push_back('=');
      column_descriptor += value;
    };
    if (column_descriptor.empty()) {
      column_descriptor = "type=unknown";
    } else if (!descriptor_has_field("type") && !descriptor_has_field("canonical") &&
               !column.descriptor.canonical_type_name.empty()) {
      column_descriptor = "type=" + column.descriptor.canonical_type_name + ";" +
                          column_descriptor;
    }
    append_descriptor_field("nullable", column.nullable ? "true" : "false");
    append_descriptor_field("default", column.default_expression_envelope);
    bool qualified_domain_lookup_required = false;
    std::string requested_domain_type_name;
    std::string domain_uuid = DomainUuidFromDescriptor(column.descriptor);
    if (domain_uuid.empty()) {
      domain_uuid = ResolveDomainTypeUuidFromColumnDescriptor(request,
                                                             column_descriptor,
                                                             &qualified_domain_lookup_required,
                                                             &requested_domain_type_name);
    }
    if (domain_uuid.empty() && qualified_domain_lookup_required) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic("ddl.create_table",
                                       "domain_column_descriptor_not_visible:" +
                                           requested_domain_type_name));
    }
    if (!domain_uuid.empty()) {
      if (!FindVisibleDomain(request.context, domain_uuid, request.context.local_transaction_id)) {
        return MakeCrudDiagnosticResult<EngineCreateTableResult>(
            request.context,
            "ddl.create_table",
            MakeInvalidRequestDiagnostic("ddl.create_table", "domain_column_descriptor_not_visible"));
      }
      column_descriptor = column.descriptor.encoded_descriptor.empty()
                              ? std::string("domain_uuid=") + domain_uuid
                              : column.descriptor.encoded_descriptor;
      if (DomainUuidFromColumnDescriptor(column_descriptor).empty()) {
        column_descriptor += (column_descriptor.empty() ? "" : ";") + std::string("domain_uuid=") + domain_uuid;
      }
    }
    columns.push_back({column_name, column_descriptor});
  }
  for (const auto& inline_index : request.table_indexes) {
    const std::string requested_profile = !inline_index.index_kind.empty() ? inline_index.index_kind : inline_index.physical_profile;
    const std::string profile = NormalizeCrudIndexProfile(requested_profile);
    if (!IsSupportedCrudIndexProfile(profile)) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          UnsupportedCrudFeatureDiagnostic("ddl.create_table", "unsupported_inline_index_profile"));
    }
    if (inline_index.key_envelopes.empty()) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          UnsupportedCrudFeatureDiagnostic("ddl.create_table", "inline_index_requires_key_envelope"));
    }
    if (IndexTouchesOpaqueColumn(inline_index, columns)) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          UnsupportedCrudFeatureDiagnostic("ddl.create_table", "opaque_column_index_denied"));
    }
    const CrudIndexRecord inline_record = BuildCrudIndexRecord(inline_index, table_uuid, "", "");
    if (inline_record.family == kCrudIndexFamilyPartial && inline_record.predicate_kind.empty()) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          UnsupportedCrudFeatureDiagnostic("ddl.create_table", "inline_partial_predicate_required"));
    }
    if (inline_record.key_envelopes.empty()) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          UnsupportedCrudFeatureDiagnostic("ddl.create_table", "inline_direct_key_or_supported_expression_required"));
    }
    if (inline_record.family == kCrudIndexFamilyExpression &&
        !SupportedExpressionEnvelope(inline_record.key_envelopes.front())) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          UnsupportedCrudFeatureDiagnostic("ddl.create_table", "unsupported_inline_expression_index_envelope"));
    }
  }
  const auto temporary_options = ResolveTemporaryTableOptions(request);
  if (temporary_options.diagnostic.error) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(request.context, "ddl.create_table", temporary_options.diagnostic);
  }
  CrudTableRecord created_table;
  created_table.creator_tx = request.context.local_transaction_id;
  created_table.table_uuid = table_uuid;
  created_table.default_name = table_name;
  created_table.columns = columns;
  created_table.temporary = temporary_options.temporary;
  created_table.temporary_scope = temporary_options.temporary ? temporary_options.scope : "";
  created_table.temporary_session_uuid =
      temporary_options.temporary && temporary_options.scope != "global"
          ? request.context.session_uuid.canonical
          : "";
  created_table.on_commit_action = temporary_options.temporary ? temporary_options.on_commit_action : "";
  const auto appended = AppendMgaTableMetadata(request.context, created_table);
  if (appended.error) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(request.context, "ddl.create_table", appended);
  }
  std::vector<EngineLocalizedName> table_names = request.table_names.empty() ? request.localized_names : request.table_names;
  if (table_names.empty()) { table_names.push_back({"en", "primary", "", table_name, true}); }
  const auto table_name_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                      "ddl.create_table",
                                                                      table_uuid,
                                                                      "table",
                                                                      target_schema_uuid,
                                                                      table_names,
                                                                      table_name);
  if (table_name_appended.error) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(request.context, "ddl.create_table", table_name_appended);
  }
  std::vector<CrudIndexRecord> implicit_support_indexes;
  for (const auto& [column_name, column_descriptor] : columns) {
    const auto descriptor_fields = DescriptorFieldsLocal(column_descriptor);
    const bool primary_key = DescriptorBoolLocal(descriptor_fields, {"primary_key", "pk"});
    const bool unique_key =
        primary_key || DescriptorBoolLocal(descriptor_fields, {"unique", "unique_key"});
    if (!unique_key) { continue; }
    CrudIndexRecord support_index;
    support_index.creator_tx = request.context.local_transaction_id;
    support_index.index_uuid = DescriptorFieldLocal(
        descriptor_fields,
        {"support_uuid", "support_index_uuid", "index_uuid"});
    if (support_index.index_uuid.empty()) {
      support_index.index_uuid = GenerateCrudEngineUuid("object");
    }
    support_index.table_uuid = table_uuid;
    support_index.column_name = column_name;
    support_index.profile = kCrudIndexProfileRowStoreScalarBtreeV1;
    support_index.family = kCrudIndexFamilyBtree;
    support_index.default_name =
        std::string(primary_key ? "pk_" : "uq_") + table_name + "_" + column_name;
    support_index.key_envelopes.push_back(column_name);
    support_index.key_envelopes.push_back(primary_key ? "primary_key" : "unique");
    support_index.unique = true;
    const auto index_appended = AppendMgaIndexMetadata(request.context, support_index);
    if (index_appended.error) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          index_appended);
    }
    std::vector<EngineLocalizedName> support_names;
    support_names.push_back({"en", "primary", "", support_index.default_name, true});
    const auto support_name_appended = PersistNameRegistryEntriesForObject(
        request.context,
        "ddl.create_table",
        support_index.index_uuid,
        "index",
        table_uuid,
        support_names,
        support_index.default_name);
    if (support_name_appended.error) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          support_name_appended);
    }
    implicit_support_indexes.push_back(std::move(support_index));
  }
  auto result = MakeCrudSuccessResult<EngineCreateTableResult>(request.context, "ddl.create_table");
  result.primary_object.uuid.canonical = table_uuid;
  result.primary_object.object_kind = "table";
  result.table_object = result.primary_object;
  result.table_catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
  result.catalog_row_uuid = result.table_catalog_row_uuid;
  result.created_catalog_records.push_back(result.primary_object);
  result.evidence.push_back({"mga_relation_metadata", "table_create"});
  AddApiBehaviorRow(&result, {{"object_uuid", table_uuid},
                              {"object_kind", "table"},
                              {"schema_uuid", target_schema_uuid},
                              {"name", table_name},
                              {"state", "active"},
                              {"payload", "mga_relation_metadata=table_create"}});
  for (const auto& support_index : implicit_support_indexes) {
    result.evidence.push_back({"constraint_support_index", support_index.index_uuid});
    result.evidence.push_back({"constraint_support_column", support_index.column_name});
  }
  if (temporary_options.temporary) {
    result.evidence.push_back({"temporary_object_scope", temporary_options.scope});
    result.evidence.push_back({"temporary_on_commit", temporary_options.on_commit_action});
  }
  for (const auto& inline_index : request.table_indexes) {
    CrudIndexRecord created_index = BuildCrudIndexRecord(inline_index,
                                                         table_uuid,
                                                         UuidStringOrGenerated(inline_index.requested_index_uuid, "object"),
                                                         inline_index.names.empty() ? "inline_index" : inline_index.names.front().name);
    created_index.creator_tx = request.context.local_transaction_id;
    if (!created_index.key_envelopes.empty() && IsSupportedCrudIndexProfile(created_index.profile)) {
      const auto index_appended = AppendMgaIndexMetadata(request.context, created_index);
      if (index_appended.error) { return MakeCrudDiagnosticResult<EngineCreateTableResult>(request.context, "ddl.create_table", index_appended); }
      const auto inline_index_name_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                                 "ddl.create_table",
                                                                                 created_index.index_uuid,
                                                                                 "index",
                                                                                 table_uuid,
                                                                                 inline_index.names,
                                                                                 created_index.default_name);
