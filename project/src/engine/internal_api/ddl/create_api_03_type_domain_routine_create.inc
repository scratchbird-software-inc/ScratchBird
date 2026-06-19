// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by create_api.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
      if (inline_index_name_appended.error) {
        return MakeCrudDiagnosticResult<EngineCreateTableResult>(request.context, "ddl.create_table", inline_index_name_appended);
      }
      result.created_storage_objects.push_back({{created_index.index_uuid}, "index"});
      result.evidence.push_back({"inline_index_create", created_index.index_uuid});
    }
  }
  return result;
}

EngineCreateIndexResult EngineCreateIndex(const EngineCreateIndexRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        MakeInvalidRequestDiagnostic("ddl.create_index", "local_transaction_id_required"));
  }
  if (request.target_object.uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        MakeInvalidRequestDiagnostic("ddl.create_index", "target_table_uuid_required"));
  }
  if (request.indexes.size() != 1) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        UnsupportedCrudFeatureDiagnostic("ddl.create_index", "exactly_one_index_definition_required"));
  }
  const auto& definition = request.indexes.front();
  const std::string requested_profile = !definition.index_kind.empty() ? definition.index_kind : definition.physical_profile;
  const std::string profile = NormalizeCrudIndexProfile(requested_profile);
  if (!IsSupportedCrudIndexProfile(profile)) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        UnsupportedCrudFeatureDiagnostic("ddl.create_index", "unsupported_index_profile"));
  }
  if (definition.key_envelopes.empty()) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        UnsupportedCrudFeatureDiagnostic("ddl.create_index", "at_least_one_key_envelope_required"));
  }
  const auto loaded_mga = LoadMgaRelationStoreState(request.context);
  if (!loaded_mga.ok) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(request.context, "ddl.create_index", loaded_mga.diagnostic);
  }
  CrudState loaded_state = BuildCrudCompatibilityStateFromMga(loaded_mga.state);
  const auto target_table = FindVisibleCrudTable(loaded_state, request.target_object.uuid.canonical, request.context.local_transaction_id);
  if (!target_table) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        MakeInvalidRequestDiagnostic("ddl.create_index", "target_table_not_visible"));
  }
  if (IndexTouchesOpaqueColumn(definition, target_table->columns)) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        UnsupportedCrudFeatureDiagnostic("ddl.create_index", "opaque_column_index_denied"));
  }
  const std::string index_uuid = UuidStringOrGenerated(definition.requested_index_uuid, "object");
  std::string index_name = "unnamed_index";
  if (!definition.names.empty() && !definition.names.front().name.empty()) { index_name = definition.names.front().name; }
  CrudIndexRecord created_index = BuildCrudIndexRecord(definition, request.target_object.uuid.canonical, index_uuid, index_name);
  created_index.creator_tx = request.context.local_transaction_id;
  if (created_index.key_envelopes.empty()) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        UnsupportedCrudFeatureDiagnostic("ddl.create_index", "direct_key_or_supported_expression_required"));
  }
  if (created_index.family == kCrudIndexFamilyPartial && created_index.predicate_kind.empty()) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        UnsupportedCrudFeatureDiagnostic("ddl.create_index", "partial_predicate_required"));
  }
  if (created_index.family == kCrudIndexFamilyExpression && !StartsWithLocal(created_index.key_envelopes.front(), "lower:") &&
      !SupportedExpressionEnvelope(created_index.key_envelopes.front())) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        UnsupportedCrudFeatureDiagnostic("ddl.create_index", "unsupported_expression_index_envelope"));
  }
  const auto index_build_rows =
      RowsVisibleForCreateIndexBuild(loaded_state, *target_table, request.context);
  if (created_index.unique &&
      ExistingRowsHaveDuplicateKeys(loaded_state,
                                    *target_table,
                                    created_index,
                                    request.context)) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
        request.context,
        "ddl.create_index",
        MakeInvalidRequestDiagnostic("ddl.create_index", "unique_index_duplicate"));
  }
  CreateIndexSortedBuildResult sorted_build;
  if (!target_table->temporary) {
    sorted_build = BuildCreateIndexSortedBulkArtifact(
        request.context,
        created_index,
        request.target_object.uuid.canonical,
        index_build_rows);
    if (!sorted_build.ok) {
      return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
          request.context,
          "ddl.create_index",
          sorted_build.diagnostic);
    }
  }
  const auto appended = AppendMgaIndexMetadata(request.context, created_index);
  if (appended.error) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(request.context, "ddl.create_index", appended);
  }
  const auto index_name_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                      "ddl.create_index",
                                                                      index_uuid,
                                                                      "index",
                                                                      request.target_object.uuid.canonical,
                                                                      definition.names,
                                                                      index_name);
  if (index_name_appended.error) {
    return MakeCrudDiagnosticResult<EngineCreateIndexResult>(request.context, "ddl.create_index", index_name_appended);
  }
  if (!target_table->temporary && sorted_build.selected && !sorted_build.batch.entries.empty()) {
    const auto entry = AppendMgaExactIndexEntryBatches(
        request.context,
        std::vector<MgaExactIndexEntryAppendBatch>{sorted_build.batch});
    if (entry.error) {
      return MakeCrudDiagnosticResult<EngineCreateIndexResult>(
          request.context,
          "ddl.create_index",
          entry);
    }
  } else {
    for (const auto& row : index_build_rows) {
      const auto entry = AppendMgaIndexEntriesForIndex(request.context,
                                                       created_index,
                                                       row.row_uuid,
                                                       row.version_uuid,
                                                       row.values);
      if (entry.error) {
        return MakeCrudDiagnosticResult<EngineCreateIndexResult>(request.context, "ddl.create_index", entry);
      }
    }
  }
  auto result = MakeCrudSuccessResult<EngineCreateIndexResult>(request.context, "ddl.create_index");
  result.primary_object.uuid.canonical = index_uuid;
  result.primary_object.object_kind = "index";
  result.catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
  result.evidence.push_back({"mga_relation_metadata", "index_create"});
  result.evidence.push_back({"index_family", created_index.family});
  AddApiBehaviorRow(&result, {{"object_uuid", index_uuid},
                              {"object_kind", "index"},
                              {"schema_uuid", request.target_object.uuid.canonical},
                              {"name", index_name},
                              {"state", "active"},
                              {"payload", "mga_relation_metadata=index_create"}});
  if (!created_index.predicate_kind.empty()) {
    result.evidence.push_back({"partial_index_filter", created_index.predicate_kind});
  }
  if (!target_table->temporary && sorted_build.selected) {
    result.evidence.insert(result.evidence.end(),
                           sorted_build.evidence.begin(),
                           sorted_build.evidence.end());
    result.evidence.push_back({"sorted_bulk_index_exact_append",
                               "mga_index_append_path"});
  } else if (target_table->temporary) {
    result.evidence.push_back({"temporary_index_entry_scope",
                               target_table->temporary_scope.empty()
                                   ? "private"
                                   : target_table->temporary_scope});
    result.evidence.push_back({"temporary_index_build_rows",
                               std::to_string(index_build_rows.size())});
  }
  return result;
}

EngineCreateStatisticsResult EngineCreateStatistics(const EngineCreateStatisticsRequest& request) {
  constexpr const char* kOperation = "ddl.create_statistics";
  const auto context_status = ValidateApiBehaviorContext(request.context, kOperation, true, true);
  if (context_status.error) {
    return MakeApiBehaviorDiagnostic<EngineCreateStatisticsResult>(request.context, kOperation, context_status);
  }

  EngineObjectReference target_table = request.target_table;
  if (target_table.uuid.canonical.empty() && request.target_object.object_kind == "table") {
    target_table = request.target_object;
  }
  if (target_table.uuid.canonical.empty()) {
    for (const auto& related : request.related_objects) {
      if (related.object_kind == "table" && !related.uuid.canonical.empty()) {
        target_table = related;
        break;
      }
    }
  }
  if (target_table.uuid.canonical.empty()) {
    target_table.uuid.canonical = SecurityOptionValue(request, "statistics_target_uuid:");
    target_table.object_kind = SecurityOptionValue(request, "statistics_target_kind:");
  }
  if (target_table.uuid.canonical.empty()) {
    target_table.uuid.canonical = SecurityOptionValue(request, "target_table_uuid:");
  }
  if (target_table.object_kind.empty()) { target_table.object_kind = "table"; }
  if (target_table.uuid.canonical.empty()) {
    return MakeApiBehaviorDiagnostic<EngineCreateStatisticsResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "statistics_target_uuid_required"));
  }
  if (target_table.object_kind != "table") {
    return MakeApiBehaviorDiagnostic<EngineCreateStatisticsResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "statistics_target_table_required"));
  }

  const auto loaded_mga = LoadMgaRelationStoreState(request.context);
  if (!loaded_mga.ok) {
    return MakeApiBehaviorDiagnostic<EngineCreateStatisticsResult>(request.context, kOperation, loaded_mga.diagnostic);
  }
  const CrudState loaded_state = BuildCrudCompatibilityStateFromMga(loaded_mga.state);
  const auto visible_table =
      FindVisibleCrudTable(loaded_state, target_table.uuid.canonical, request.context.local_transaction_id);
  if (!visible_table) {
    return MakeApiBehaviorDiagnostic<EngineCreateStatisticsResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "statistics_target_table_not_visible"));
  }

  std::string statistics_kind = request.statistics_kinds.empty()
                                    ? SecurityOptionValue(request, "statistics_kind:")
                                    : request.statistics_kinds.front();
  if (statistics_kind.empty()) { statistics_kind = "auto"; }
  statistics_kind = SecurityLower(statistics_kind);

  std::uint64_t expression_count = request.expression_envelopes.size();
  if (expression_count == 0) {
    const std::string count_text = SecurityOptionValue(request, "statistics_expression_count:");
    if (!count_text.empty()) {
      try {
        expression_count = static_cast<std::uint64_t>(std::stoull(count_text));
      } catch (...) {
        expression_count = 0;
      }
    }
  }
  if (expression_count == 0) {
    return MakeApiBehaviorDiagnostic<EngineCreateStatisticsResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "statistics_expression_required"));
  }

  const std::string statistics_uuid =
      !request.requested_statistics_uuid.canonical.empty()
          ? request.requested_statistics_uuid.canonical
          : (request.target_object.object_kind == "statistics" && !request.target_object.uuid.canonical.empty()
                 ? request.target_object.uuid.canonical
                 : GenerateCrudEngineUuid("object"));
  std::vector<EngineLocalizedName> names = request.statistics_names.empty()
                                               ? request.localized_names
                                               : request.statistics_names;
  const std::string statistics_name = DefaultLocalizedName(names, "unnamed_statistics");

  EngineApiRequest record_request = request;
  record_request.target_object.uuid.canonical = statistics_uuid;
  record_request.target_object.object_kind = "statistics";
  record_request.localized_names = names;
  record_request.related_objects.push_back(target_table);
  record_request.option_envelopes.push_back("statistics_catalog_table:sys.optimizer.statistics_descriptor");
  record_request.option_envelopes.push_back("statistics_target_uuid:" + target_table.uuid.canonical);
  record_request.option_envelopes.push_back("statistics_target_kind:table");
  record_request.option_envelopes.push_back("statistics_kind:" + statistics_kind);
  record_request.option_envelopes.push_back("statistics_expression_count:" + std::to_string(expression_count));
  for (const auto& expression : request.expression_envelopes) {
    record_request.option_envelopes.push_back("statistics_expression:" + expression);
  }

  const auto persisted = PersistApiBehaviorRecord(record_request, kOperation, "statistics", true, "active");
  if (!persisted.ok) {
    return MakeApiBehaviorDiagnostic<EngineCreateStatisticsResult>(request.context, kOperation, persisted.diagnostic);
  }
  const auto name_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                kOperation,
                                                                statistics_uuid,
                                                                "statistics",
                                                                target_table.uuid.canonical,
                                                                names,
                                                                statistics_name);
  if (name_appended.error) {
    return MakeApiBehaviorDiagnostic<EngineCreateStatisticsResult>(request.context, kOperation, name_appended);
  }

  auto result = MakeApiBehaviorSuccess<EngineCreateStatisticsResult>(request.context, kOperation);
  result.primary_object.uuid.canonical = statistics_uuid;
  result.primary_object.object_kind = "statistics";
  result.catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
  result.bound_object_identity.object_uuid.canonical = statistics_uuid;
  result.bound_object_identity.resolved_object_type = "statistics";
  result.bound_object_identity.parent_object_uuid.canonical = target_table.uuid.canonical;
  result.bound_object_identity.catalog_generation_id = request.context.catalog_generation_id + 1;
  result.bound_object_identity.security_epoch = request.context.security_epoch;
  result.bound_object_identity.resource_epoch = request.context.resource_epoch;
  result.metadata_cache_epoch = request.context.name_resolution_epoch + 1;
  AddApiBehaviorEvidence(&result, "api_behavior_event", kOperation);
  AddApiBehaviorEvidence(&result, "statistics_descriptor_created", statistics_uuid);
  AddApiBehaviorEvidence(&result, "statistics_target_table", target_table.uuid.canonical);
  AddApiBehaviorEvidence(&result, "statistics_kind", statistics_kind);
  AddApiBehaviorEvidence(&result, "statistics_expression_count", std::to_string(expression_count));
  AddApiBehaviorEvidence(&result, "mga_table_visibility_checked", target_table.uuid.canonical);
  AddApiBehaviorEvidence(&result, "ddl_catalog_route", "sys.optimizer.statistics_descriptor");
  AddApiBehaviorEvidence(&result, "name_registry", statistics_uuid);
  AddApiBehaviorRow(&result, {{"object_uuid", statistics_uuid},
                              {"object_kind", "statistics"},
                              {"name", statistics_name},
                              {"state", "active"},
                              {"statistics_target_uuid", target_table.uuid.canonical},
                              {"statistics_target_kind", "table"},
                              {"statistics_kind", statistics_kind},
                              {"statistics_expression_count", std::to_string(expression_count)},
                              {"payload", persisted.record.payload}});
  return result;
}

EngineCreateDomainResult EngineCreateDomain(const EngineCreateDomainRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
        request.context,
        "ddl.create_domain",
        MakeInvalidRequestDiagnostic("ddl.create_domain", "local_transaction_id_required"));
  }
  if (request.descriptors.empty()) {
    return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
        request.context,
        "ddl.create_domain",
        MakeInvalidRequestDiagnostic("ddl.create_domain", "base_descriptor_required"));
  }
  if (!request.rows.empty() || !request.columns.empty() || !request.indexes.empty()) {
    return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
        request.context,
        "ddl.create_domain",
        UnsupportedCrudFeatureDiagnostic("ddl.create_domain", "domain_request_contains_non_domain_payload"));
  }
  const auto loaded = LoadCrudState(request.context);
  if (!loaded.ok) {
    return MakeCrudDiagnosticResult<EngineCreateDomainResult>(request.context, "ddl.create_domain", loaded.diagnostic);
  }
  DomainRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.domain_uuid = UuidStringOrGenerated(request.target_object.uuid, "object");
  if (FindVisibleDomain(request.context, record.domain_uuid, request.context.local_transaction_id)) {
    return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
        request.context,
        "ddl.create_domain",
        MakeInvalidRequestDiagnostic("ddl.create_domain", "domain_uuid_already_exists"));
  }
  record.catalog_row_uuid = GenerateCrudEngineUuid("row");
  record.schema_uuid = request.target_schema.uuid.canonical;
  record.default_name = DefaultLocalizedName(request.localized_names, "unnamed_domain");
  record.localized_names_envelope = LocalizedNamesEnvelope(request.localized_names);
  const auto& descriptor = request.descriptors.front();
  record.base_descriptor_uuid = descriptor.descriptor_uuid.canonical.empty() ? GenerateCrudEngineUuid("object") : descriptor.descriptor_uuid.canonical;
