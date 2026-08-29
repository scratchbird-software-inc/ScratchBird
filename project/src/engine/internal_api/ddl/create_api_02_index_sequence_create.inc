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
  for (const auto& extension : schema_extension_payload) {
    if (!record.payload.empty()) { record.payload.push_back(';'); }
    record.payload.append(extension);
  }
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
  for (const auto& extension : schema_extension_payload) {
    const auto pos = extension.find('=');
    if (pos == std::string::npos) { continue; }
    AddApiBehaviorEvidence(&result, extension.substr(0, pos), extension.substr(pos + 1));
  }
  AddApiBehaviorRow(&result, {{"object_uuid", record.schema_uuid},
                              {"object_kind", "schema"},
                              {"name", record.default_name},
                              {"state", "active"},
                              {"payload", record.payload}});
  AddDdlPublicationResult(&result,
                          "ddl.create_schema",
                          "schema",
                          record.schema_uuid,
                          result.catalog_row_uuid.canonical,
                          "schema_tree");
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
    AddDdlPublicationResult(&result,
                            kOperation,
                            "synonym",
                            result.primary_object.uuid.canonical,
                            result.catalog_row_uuid.canonical,
                            "catalog.synonym");
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
  // QOW-SOURCE-PACKET7-PERSISTED-TYPE-CATALOG-ENRICHMENT-V1: a table
  // descriptor must persist the engine-owned built-in type UUID. Parser and
  // donor spellings are resolution inputs only and never identity authority.
  const auto builtin_type_manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!builtin_type_manifest.ok()) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(
        request.context,
        "ddl.create_table",
        MakeInvalidRequestDiagnostic(
            "ddl.create_table", "builtin_type_catalog_manifest_invalid"));
  }
  for (const auto& column : request.table_columns) {
    std::string column_name = "column_" + std::to_string(column.ordinal);
    if (!column.names.empty() && !column.names.front().name.empty()) { column_name = column.names.front().name; }
    std::string engine_canonical_type_name =
        column.descriptor.canonical_type_name;
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
      const auto visible_domain = FindVisibleDomain(
          request.context, domain_uuid, request.context.local_transaction_id);
      if (!visible_domain) {
        return MakeCrudDiagnosticResult<EngineCreateTableResult>(
            request.context,
            "ddl.create_table",
            MakeInvalidRequestDiagnostic("ddl.create_table", "domain_column_descriptor_not_visible"));
      }
      engine_canonical_type_name =
          visible_domain->base_canonical_type_name;
      column_descriptor = column.descriptor.encoded_descriptor.empty()
                              ? std::string("domain_uuid=") + domain_uuid
                              : column.descriptor.encoded_descriptor;
      if (DomainUuidFromColumnDescriptor(column_descriptor).empty()) {
        column_descriptor += (column_descriptor.empty() ? "" : ";") + std::string("domain_uuid=") + domain_uuid;
      }
    }
    std::string pre_resource_type_name = engine_canonical_type_name;
    if (pre_resource_type_name.empty()) {
      pre_resource_type_name = DescriptorFieldLocal(
          DescriptorFieldsLocal(column_descriptor), {"canonical", "type"});
    }
    if (scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
            DescriptorBaseTypeLocal(pre_resource_type_name)) ==
            scratchbird::core::datatypes::CanonicalTypeId::character &&
        !StrictDescriptorFieldsLocal(column_descriptor).has_value()) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic(
              "ddl.create_table",
              "column_text_semantics_duplicate_or_malformed:" +
                  column_name));
    }
    const auto text_resource_diagnostic =
        ValidateAndCanonicalizeColumnTextResources(
            request.context, column, &column_descriptor);
    if (text_resource_diagnostic.error) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          text_resource_diagnostic);
    }
    if (engine_canonical_type_name.empty()) {
      const auto descriptor_fields = DescriptorFieldsLocal(column_descriptor);
      engine_canonical_type_name = DescriptorFieldLocal(
          descriptor_fields, {"canonical", "type"});
    }
    const auto canonical_type_id =
        scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
            DescriptorBaseTypeLocal(engine_canonical_type_name));
    if (canonical_type_id ==
        scratchbird::core::datatypes::CanonicalTypeId::unknown) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic(
              "ddl.create_table",
              "column_type_not_engine_resolved:" + column_name));
    }
    if (canonical_type_id ==
        scratchbird::core::datatypes::CanonicalTypeId::character) {
      static const std::set<std::string> kAllowedCanonicalTextFields{
          "type", "canonical", "nullable", "default", "column_uuid",
          "datatype_descriptor_uuid", "datatype_descriptor_generation",
          "type_uuid", "type_generation", "codec_uuid", "codec_id",
          "codec_version", "codec_generation", "null_encoding",
          "charset_uuid", "charset_generation", "collation_uuid",
          "collation_generation", "resource_epoch", "character_length",
          "primary_key", "pk", "generated", "identity", "domain_uuid"};
      const auto text_fields = StrictDescriptorFieldsLocal(column_descriptor);
      if (!text_fields.has_value()) {
        return MakeCrudDiagnosticResult<EngineCreateTableResult>(
            request.context,
            "ddl.create_table",
            MakeInvalidRequestDiagnostic(
                "ddl.create_table",
                "column_text_semantics_duplicate_or_malformed:" +
                    column_name));
      }
      for (const auto& [key, value] : *text_fields) {
        (void)value;
        if (!kAllowedCanonicalTextFields.contains(key)) {
          return MakeCrudDiagnosticResult<EngineCreateTableResult>(
              request.context,
              "ddl.create_table",
              MakeInvalidRequestDiagnostic(
                  "ddl.create_table",
                  "column_text_semantics_field_not_authoritative:" +
                      column_name + ":" + key));
        }
      }
      const auto type_field = text_fields->find("type");
      const auto canonical_field = text_fields->find("canonical");
      const auto nullable_field = text_fields->find("nullable");
      const bool has_exactly_one_text_spelling =
          (type_field == text_fields->end()) !=
          (canonical_field == text_fields->end());
      const std::string* text_spelling =
          type_field != text_fields->end()
              ? &type_field->second
              : (canonical_field != text_fields->end()
                     ? &canonical_field->second
                     : nullptr);
      const std::string expected_nullable =
          column.nullable ? "true" : "false";
      if (!has_exactly_one_text_spelling || text_spelling == nullptr ||
          *text_spelling != "text" || nullable_field == text_fields->end() ||
          nullable_field->second != expected_nullable) {
        return MakeCrudDiagnosticResult<EngineCreateTableResult>(
            request.context,
            "ddl.create_table",
            MakeInvalidRequestDiagnostic(
                "ddl.create_table",
                "column_text_semantics_authority_mismatch:" +
                    column_name));
      }
    }
    const auto type_row =
        scratchbird::core::datatypes::LookupDatatypeCatalogRow(
            builtin_type_manifest.manifest, canonical_type_id);
    if (!type_row.ok() || type_row.manifest.descriptor_rows.size() != 1 ||
        !type_row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic(
              "ddl.create_table",
              "column_type_catalog_row_invalid:" + column_name));
    }
    std::string engine_type_uuid =
        scratchbird::core::uuid::UuidToString(
            type_row.manifest.descriptor_rows.front().descriptor_uuid.value);
    const std::string engine_datatype_descriptor_uuid =
        scratchbird::core::uuid::UuidToString(
            type_row.manifest.descriptor_rows.front().descriptor_uuid.value);
    const bool persist_full_registry_tuple =
        canonical_type_id ==
            scratchbird::core::datatypes::CanonicalTypeId::boolean ||
        canonical_type_id ==
            scratchbird::core::datatypes::CanonicalTypeId::int32 ||
        canonical_type_id ==
            scratchbird::core::datatypes::CanonicalTypeId::int64 ||
        canonical_type_id ==
            scratchbird::core::datatypes::CanonicalTypeId::int128 ||
        canonical_type_id ==
            scratchbird::core::datatypes::CanonicalTypeId::decimal ||
        canonical_type_id ==
            scratchbird::core::datatypes::CanonicalTypeId::character;
    // Fresh descriptors for the six manifest-admitted scalar rows are issued
    // only from the exact datatype receipt carried by this DDL request. Other
    // canonical types retain their existing catalog projection and cannot be
    // inferred into this registry from a name, width, enum, or random UUID.
    const auto identity =
        scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
            request.context.datatype_catalog_snapshot_uuid.canonical,
            request.context.datatype_catalog_generation,
            request.context.datatype_registry_generation,
            engine_datatype_descriptor_uuid, 1);
    if (persist_full_registry_tuple) {
      if (!identity.ok ||
          identity.row.catalog_snapshot_uuid !=
              request.context.datatype_catalog_snapshot_uuid.canonical ||
          identity.row.catalog_generation !=
              request.context.datatype_catalog_generation ||
          identity.row.registry_generation !=
              request.context.datatype_registry_generation ||
          identity.row.descriptor_uuid != engine_datatype_descriptor_uuid ||
          identity.row.descriptor_generation != 1 ||
          identity.row.type_uuid.empty()) {
        return MakeCrudDiagnosticResult<EngineCreateTableResult>(
            request.context,
            "ddl.create_table",
            MakeInvalidRequestDiagnostic(
                "ddl.create_table",
                "column_type_codec_registry_row_invalid:" + column_name));
      }
      engine_type_uuid = identity.row.type_uuid;
    }
    std::optional<std::string> carried_type_uuid;
    bool type_uuid_carrier_invalid = false;
    std::size_t descriptor_offset = 0;
    while (descriptor_offset <= column_descriptor.size()) {
      const auto delimiter = column_descriptor.find(';', descriptor_offset);
      const auto end = delimiter == std::string::npos
                           ? column_descriptor.size()
                           : delimiter;
      const std::string_view field(
          column_descriptor.data() + descriptor_offset,
          end - descriptor_offset);
      constexpr std::string_view kTypeUuidPrefix = "type_uuid=";
      if (field.starts_with(kTypeUuidPrefix)) {
        if (carried_type_uuid.has_value() ||
            field.size() == kTypeUuidPrefix.size()) {
          type_uuid_carrier_invalid = true;
          break;
        }
        carried_type_uuid =
            std::string(field.substr(kTypeUuidPrefix.size()));
      }
      if (delimiter == std::string::npos) break;
      descriptor_offset = delimiter + 1;
    }
    if (carried_type_uuid.has_value()) {
      const auto parsed_type_uuid =
          scratchbird::core::uuid::ParseUuid(*carried_type_uuid);
      if (!parsed_type_uuid.ok() ||
          scratchbird::core::uuid::IsNilUuid(parsed_type_uuid.value) ||
          scratchbird::core::uuid::UuidToString(parsed_type_uuid.value) !=
              *carried_type_uuid ||
          *carried_type_uuid != engine_type_uuid) {
        type_uuid_carrier_invalid = true;
      }
    }
    if (type_uuid_carrier_invalid) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic(
              "ddl.create_table",
              "column_type_uuid_catalog_mismatch:" + column_name));
    }
    if (persist_full_registry_tuple) {
      std::optional<std::string> carried_datatype_descriptor_uuid;
      bool datatype_descriptor_uuid_carrier_invalid = false;
      descriptor_offset = 0;
      while (descriptor_offset <= column_descriptor.size()) {
        const auto delimiter = column_descriptor.find(';', descriptor_offset);
        const auto end = delimiter == std::string::npos
                             ? column_descriptor.size()
                             : delimiter;
        const std::string_view field(
            column_descriptor.data() + descriptor_offset,
            end - descriptor_offset);
        constexpr std::string_view kDatatypeDescriptorUuidPrefix =
            "datatype_descriptor_uuid=";
        if (field.starts_with(kDatatypeDescriptorUuidPrefix)) {
          if (carried_datatype_descriptor_uuid.has_value() ||
              field.size() == kDatatypeDescriptorUuidPrefix.size()) {
            datatype_descriptor_uuid_carrier_invalid = true;
            break;
          }
          carried_datatype_descriptor_uuid = std::string(
              field.substr(kDatatypeDescriptorUuidPrefix.size()));
        }
        if (delimiter == std::string::npos) break;
        descriptor_offset = delimiter + 1;
      }
      if (carried_datatype_descriptor_uuid.has_value()) {
        const auto parsed_descriptor_uuid = scratchbird::core::uuid::ParseUuid(
            *carried_datatype_descriptor_uuid);
        if (!parsed_descriptor_uuid.ok() ||
            scratchbird::core::uuid::IsNilUuid(parsed_descriptor_uuid.value) ||
            scratchbird::core::uuid::UuidToString(parsed_descriptor_uuid.value) !=
                *carried_datatype_descriptor_uuid ||
            *carried_datatype_descriptor_uuid !=
                engine_datatype_descriptor_uuid) {
          datatype_descriptor_uuid_carrier_invalid = true;
        }
      }
      if (datatype_descriptor_uuid_carrier_invalid) {
        return MakeCrudDiagnosticResult<EngineCreateTableResult>(
            request.context,
            "ddl.create_table",
            MakeInvalidRequestDiagnostic(
                "ddl.create_table",
                "column_datatype_descriptor_uuid_catalog_mismatch:" +
                    column_name));
      }
      if (!carried_datatype_descriptor_uuid.has_value()) {
        append_descriptor_field("datatype_descriptor_uuid",
                                engine_datatype_descriptor_uuid);
      }
      // Registry-backed descriptor/type/codec authority is persisted as one
      // exact tuple.  A caller may repeat an already-resolved field only when
      // its bytes equal the live registry row; duplicate or contradictory
      // fields refuse instead of being overwritten or inferred.
      const auto validate_or_append_identity_field =
          [&](const std::string_view key,
              const std::string& expected) -> bool {
        std::optional<std::string> carried;
        std::size_t offset = 0;
        const std::string prefix = std::string(key) + "=";
        while (offset <= column_descriptor.size()) {
          const auto delimiter = column_descriptor.find(';', offset);
          const auto end = delimiter == std::string::npos
                               ? column_descriptor.size()
                               : delimiter;
          const std::string_view field(
              column_descriptor.data() + offset, end - offset);
          if (field.starts_with(prefix)) {
            if (carried.has_value() || field.size() == prefix.size()) {
              return false;
            }
            carried = std::string(field.substr(prefix.size()));
          }
          if (delimiter == std::string::npos) break;
          offset = delimiter + 1;
        }
        if (carried.has_value()) return *carried == expected;
        append_descriptor_field(std::string(key), expected);
        return true;
      };
      const auto identity_field_absent =
          [&](const std::string_view key) -> bool {
        std::size_t offset = 0;
        const std::string prefix = std::string(key) + "=";
        while (offset <= column_descriptor.size()) {
          const auto delimiter = column_descriptor.find(';', offset);
          const auto end = delimiter == std::string::npos
                               ? column_descriptor.size()
                               : delimiter;
          const std::string_view field(
              column_descriptor.data() + offset, end - offset);
          if (field.starts_with(prefix)) return false;
          if (delimiter == std::string::npos) break;
          offset = delimiter + 1;
        }
        return true;
      };
      const bool exact_registry_tuple =
          validate_or_append_identity_field(
              "datatype_descriptor_generation",
              std::to_string(identity.row.descriptor_generation)) &&
          validate_or_append_identity_field("type_uuid",
                                            identity.row.type_uuid) &&
          validate_or_append_identity_field(
              "type_generation",
              std::to_string(identity.row.type_generation)) &&
          (identity.row.codec_uuid.empty()
               ? identity_field_absent("codec_uuid")
               : validate_or_append_identity_field("codec_uuid",
                                                   identity.row.codec_uuid)) &&
          validate_or_append_identity_field("codec_id",
                                            identity.row.codec_id) &&
          validate_or_append_identity_field(
              "codec_version", std::to_string(identity.row.codec_version)) &&
          validate_or_append_identity_field(
              "codec_generation",
              std::to_string(identity.row.codec_generation)) &&
          validate_or_append_identity_field(
              "null_encoding",
              std::to_string(identity.row.null_encoding_code));
      if (!exact_registry_tuple) {
        return MakeCrudDiagnosticResult<EngineCreateTableResult>(
            request.context,
            "ddl.create_table",
            MakeInvalidRequestDiagnostic(
                "ddl.create_table",
                "column_datatype_codec_registry_tuple_mismatch:" +
                    column_name));
      }
    } else if (!carried_type_uuid.has_value()) {
      append_descriptor_field("type_uuid", engine_type_uuid);
    }
    columns.push_back({column_name, column_descriptor});
  }
  bool primary_key_seen = false;
  for (const auto& [column_name, column_descriptor] : columns) {
    const auto descriptor_fields = DescriptorFieldsLocal(column_descriptor);
    if (!DescriptorBoolLocal(descriptor_fields, {"primary_key", "pk"})) {
      continue;
    }
    if (primary_key_seen) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic(
              "ddl.create_table", "multiple_primary_key_indexes_for_table"));
    }
    if (DescriptorBoolLocal(descriptor_fields, {"nullable"})) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic(
              "ddl.create_table", "primary_key_column_nullable:" + column_name));
    }
    primary_key_seen = true;
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
    const auto inline_key_columns = InlineIndexKeyColumns(inline_record);
    if (inline_key_columns.empty()) {
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
    const bool primary_key_index =
        std::find(inline_index.key_envelopes.begin(),
                  inline_index.key_envelopes.end(),
                  "primary_key") != inline_index.key_envelopes.end();
    const bool unique_constraint_index =
        std::find(inline_index.key_envelopes.begin(),
                  inline_index.key_envelopes.end(),
                  "unique") != inline_index.key_envelopes.end();
    if (primary_key_index || unique_constraint_index) {
      std::set<std::string> distinct_constraint_key_columns;
      for (const auto& key_envelope : inline_index.key_envelopes) {
        if (key_envelope == "primary_key" || key_envelope == "unique") {
          continue;
        }
        if (!DirectIndexKeyEnvelope(key_envelope)) {
          return MakeCrudDiagnosticResult<EngineCreateTableResult>(
              request.context,
              "ddl.create_table",
              UnsupportedCrudFeatureDiagnostic(
                  "ddl.create_table",
                  primary_key_index
                      ? "primary_key_requires_direct_columns"
                      : "unique_constraint_requires_direct_columns"));
        }
        if (!distinct_constraint_key_columns.insert(key_envelope).second) {
          return MakeCrudDiagnosticResult<EngineCreateTableResult>(
              request.context,
              "ddl.create_table",
              MakeInvalidRequestDiagnostic(
                  "ddl.create_table",
                  "inline_index_duplicate_key_column:" + key_envelope));
        }
      }
    }
    if (primary_key_index && primary_key_seen) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic(
              "ddl.create_table", "multiple_primary_key_indexes_for_table"));
    }
    primary_key_seen = primary_key_seen || primary_key_index;
    const bool composite_unique_index =
        unique_constraint_index && inline_key_columns.size() > 1;
    if (primary_key_index || composite_unique_index) {
      for (const auto& key_column : inline_key_columns) {
        const auto column = std::find_if(
            columns.begin(), columns.end(), [&key_column](const auto& candidate) {
              return candidate.first == key_column;
            });
        if (column == columns.end()) {
          return MakeCrudDiagnosticResult<EngineCreateTableResult>(
              request.context,
              "ddl.create_table",
              MakeInvalidRequestDiagnostic(
                  "ddl.create_table",
                  "inline_index_key_column_not_found:" + key_column));
        }
        const auto descriptor_fields = DescriptorFieldsLocal(column->second);
        if (DescriptorBoolLocal(descriptor_fields, {"nullable"})) {
          return MakeCrudDiagnosticResult<EngineCreateTableResult>(
              request.context,
              "ddl.create_table",
              MakeInvalidRequestDiagnostic(
                  "ddl.create_table",
                  std::string(primary_key_index
                                  ? "primary_key_column_nullable:"
                                  : "composite_unique_nullable_policy_unsupported:") +
                      key_column));
        }
      }
    }
  }
  const auto temporary_options = ResolveTemporaryTableOptions(request);
  if (temporary_options.diagnostic.error) {
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(request.context, "ddl.create_table", temporary_options.diagnostic);
  }
  // Column-level candidate keys receive neutral engine-owned identities at
  // table creation.  A later FK binds these exact persisted identities; it
  // must never synthesize a parent key descriptor from a support index.
  for (auto& [column_name, column_descriptor] : columns) {
    const auto descriptor_fields = DescriptorFieldsLocal(column_descriptor);
    const bool primary_key =
        DescriptorBoolLocal(descriptor_fields, {"primary_key", "pk"});
    const bool unique_key = primary_key ||
        DescriptorBoolLocal(descriptor_fields, {"unique", "unique_key"});
    if (!unique_key) continue;
    // Candidate-key, key-descriptor, and physical-support identities are
    // durable engine identities.  A parser/request describes semantics but
    // cannot choose or inject them.
    constexpr const char* kReservedCandidateIdentityFields[] = {
        "candidate_key_constraint_uuid",
        "candidate_key_descriptor_uuid",
        "key_descriptor_uuid",
        "support_uuid",
        "support_index_uuid",
        "index_uuid"};
    bool supplied_reserved_identity = false;
    for (const char* field : kReservedCandidateIdentityFields) {
      if (descriptor_fields.find(field) != descriptor_fields.end()) {
        supplied_reserved_identity = true;
        break;
      }
    }
    if (supplied_reserved_identity) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic(
              "ddl.create_table",
              "reserved_candidate_key_identity_supplied:" + column_name));
    }
    const std::string candidate_constraint_uuid =
        GenerateCrudEngineUuid("object");
    const std::string candidate_key_descriptor_uuid =
        GenerateCrudEngineUuid("object");
    const std::string support_uuid = GenerateCrudEngineUuid("object");
    const auto valid_object_identity = [](const std::string& value) {
      return !value.empty() &&
          scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
              scratchbird::core::platform::UuidKind::object, value)
              .ok();
    };
    if (!valid_object_identity(candidate_constraint_uuid) ||
        !valid_object_identity(candidate_key_descriptor_uuid) ||
        !valid_object_identity(support_uuid)) {
      return MakeCrudDiagnosticResult<EngineCreateTableResult>(
          request.context,
          "ddl.create_table",
          MakeInvalidRequestDiagnostic(
              "ddl.create_table",
              "candidate_key_identity_generation_failed:" + column_name));
    }
    column_descriptor = SetDescriptorFieldLocal(
        std::move(column_descriptor),
        "candidate_key_constraint_uuid",
        candidate_constraint_uuid);
    column_descriptor = SetDescriptorFieldLocal(
        std::move(column_descriptor),
        "candidate_key_descriptor_uuid",
        candidate_key_descriptor_uuid);
    column_descriptor = SetDescriptorFieldLocal(
        std::move(column_descriptor), "support_uuid", support_uuid);
    column_descriptor = SetDescriptorFieldLocal(
        std::move(column_descriptor), "support_family", "btree");
    column_descriptor = SetDescriptorFieldLocal(
        std::move(column_descriptor),
        "candidate_key_class",
        primary_key ? "primary_key" : "unique");
    (void)column_name;
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
  std::vector<EngineLocalizedName> table_names = request.table_names.empty() ? request.localized_names : request.table_names;
  if (table_names.empty()) { table_names.push_back({"en", "primary", "", table_name, true}); }
  EngineCatalogCreateObjectRequest catalog_request;
  static_cast<EngineApiRequest&>(catalog_request) = request;
  catalog_request.operation_id = "ddl.create_table";
  catalog_request.target_object.uuid.canonical = table_uuid;
  catalog_request.target_object.object_kind = "table";
  catalog_request.target_schema.uuid.canonical = target_schema_uuid;
  catalog_request.localized_names = table_names;
  if (created_table.temporary && created_table.temporary_scope != "global") {
    catalog_request.name_namespace.kind =
        EngineCatalogNameNamespaceKind::kSessionTemporary;
    catalog_request.name_namespace.owner_session_uuid.canonical =
        created_table.temporary_session_uuid;
  }
  const auto catalog_created = EngineCatalogCreateObject(catalog_request);
  if (!catalog_created.ok) {
    EngineApiDiagnostic diagnostic = catalog_created.diagnostics.empty()
                                         ? MakeInvalidRequestDiagnostic("ddl.create_table",
                                                                        "catalog_table_object_create_failed")
                                         : catalog_created.diagnostics.front();
    return MakeCrudDiagnosticResult<EngineCreateTableResult>(request.context,
                                                             "ddl.create_table",
                                                             diagnostic);
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
  std::vector<CrudIndexRecord> relation_descriptor_indexes =
      implicit_support_indexes;
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
