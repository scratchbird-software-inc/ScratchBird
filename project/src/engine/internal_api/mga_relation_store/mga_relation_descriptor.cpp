// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_descriptor.hpp"

#include "api_diagnostics.hpp"

#include <cstdlib>
#include <string>

namespace scratchbird::engine::internal_api {
namespace {

std::string FieldValue(const std::vector<std::pair<std::string, std::string>>& fields,
                       const std::string& key,
                       const std::string& fallback = {}) {
  for (const auto& [field_key, field_value] : fields) {
    if (field_key == key) { return field_value; }
  }
  return fallback;
}

std::uint64_t FieldU64(const std::vector<std::pair<std::string, std::string>>& fields,
                       const std::string& key,
                       std::uint64_t fallback = 0) {
  const std::string value = FieldValue(fields, key);
  if (value.empty()) { return fallback; }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

std::uint32_t FieldU32(const std::vector<std::pair<std::string, std::string>>& fields,
                       const std::string& key,
                       std::uint32_t fallback = 0) {
  const auto value = FieldU64(fields, key, fallback);
  return value > 0xffffffffull ? fallback : static_cast<std::uint32_t>(value);
}

bool FieldBool(const std::vector<std::pair<std::string, std::string>>& fields,
               const std::string& key,
               bool fallback = false) {
  const std::string value = FieldValue(fields, key);
  if (value.empty()) { return fallback; }
  return value == "1" || value == "true";
}

void PushBool(std::vector<std::pair<std::string, std::string>>* fields,
              const std::string& key,
              bool value) {
  fields->push_back({key, value ? "1" : "0"});
}

void PushU64(std::vector<std::pair<std::string, std::string>>* fields,
             const std::string& key,
             std::uint64_t value) {
  fields->push_back({key, std::to_string(value)});
}

std::string GeneratedIdentity(const std::string& kind) {
  return GenerateCrudEngineUuid(kind);
}

}  // namespace

EngineApiDiagnostic ValidateMgaRelationStorageDescriptor(const MgaRelationStorageDescriptor& descriptor) {
  if (descriptor.descriptor_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "descriptor_uuid_required");
  }
  if (descriptor.relation_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "relation_uuid_required");
  }
  if (descriptor.row_identity_rule != "engine_uuid_v7_only" ||
      descriptor.version_identity_rule != "engine_uuid_v7_only") {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "engine_identity_must_be_uuid_v7");
  }
  if (descriptor.mutation_rule != "copy_on_write") {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "copy_on_write_required");
  }
  if (descriptor.recovery_rule.find("no_wal") == std::string::npos) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "wal_recovery_forbidden");
  }
  if (descriptor.columns.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "at_least_one_column_required");
  }
  for (const auto& column : descriptor.columns) {
    if (column.column_uuid.canonical.empty()) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "column_uuid_required");
    }
    if (column.canonical_name_key.empty()) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "column_name_key_required");
    }
    if (column.value_descriptor.encoded_descriptor.empty()) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "column_descriptor_required");
    }
  }
  for (const auto& index : descriptor.indexes) {
    if (index.index_uuid.canonical.empty()) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "index_uuid_required");
    }
    if (index.family.empty()) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "index_family_required");
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::vector<std::pair<std::string, std::string>> SerializeMgaRelationStorageDescriptor(
    const MgaRelationStorageDescriptor& descriptor) {
  std::vector<std::pair<std::string, std::string>> fields;
  fields.push_back({"descriptor_uuid", descriptor.descriptor_uuid.canonical});
  fields.push_back({"database_uuid", descriptor.database_uuid.canonical});
  fields.push_back({"schema_uuid", descriptor.schema_uuid.canonical});
  fields.push_back({"relation_uuid", descriptor.relation_uuid.canonical});
  fields.push_back({"primary_filespace_uuid", descriptor.primary_filespace_uuid.canonical});
  fields.push_back({"relation_kind", descriptor.relation_kind});
  fields.push_back({"storage_profile", descriptor.storage_profile});
  PushU64(&fields, "descriptor_generation", descriptor.descriptor_generation);
  PushU64(&fields, "page_size", descriptor.page_size);
  PushU64(&fields, "root_page_number", descriptor.root_page_number);
  PushU64(&fields, "allocation_root_page_number", descriptor.allocation_root_page_number);
  fields.push_back({"row_identity_rule", descriptor.row_identity_rule});
  fields.push_back({"version_identity_rule", descriptor.version_identity_rule});
  fields.push_back({"mutation_rule", descriptor.mutation_rule});
  fields.push_back({"visibility_rule", descriptor.visibility_rule});
  fields.push_back({"cleanup_rule", descriptor.cleanup_rule});
  fields.push_back({"recovery_rule", descriptor.recovery_rule});
  fields.push_back({"descriptor_status", descriptor.descriptor_status});
  PushU64(&fields, "column_count", descriptor.columns.size());
  for (std::size_t i = 0; i < descriptor.columns.size(); ++i) {
    const std::string prefix = "column." + std::to_string(i) + ".";
    const auto& column = descriptor.columns[i];
    fields.push_back({prefix + "uuid", column.column_uuid.canonical});
    PushU64(&fields, prefix + "ordinal", column.ordinal);
    fields.push_back({prefix + "name_key", column.canonical_name_key});
    fields.push_back({prefix + "descriptor_uuid", column.value_descriptor.descriptor_uuid.canonical});
    fields.push_back({prefix + "descriptor_kind", column.value_descriptor.descriptor_kind});
    fields.push_back({prefix + "type_name", column.value_descriptor.canonical_type_name});
    fields.push_back({prefix + "encoded_descriptor", column.value_descriptor.encoded_descriptor});
    PushBool(&fields, prefix + "nullable", column.nullable);
    PushBool(&fields, prefix + "generated", column.generated);
    PushBool(&fields, prefix + "identity", column.identity_column);
    fields.push_back({prefix + "storage_class", column.storage_class});
    fields.push_back({prefix + "collation_uuid", column.collation_uuid});
    PushU64(&fields, prefix + "max_inline_bytes", column.max_inline_bytes);
    fields.push_back({prefix + "overflow_policy", column.overflow_policy});
  }
  PushU64(&fields, "index_count", descriptor.indexes.size());
  for (std::size_t i = 0; i < descriptor.indexes.size(); ++i) {
    const std::string prefix = "index." + std::to_string(i) + ".";
    const auto& index = descriptor.indexes[i];
    fields.push_back({prefix + "uuid", index.index_uuid.canonical});
    fields.push_back({prefix + "family", index.family});
    fields.push_back({prefix + "profile", index.profile});
    PushBool(&fields, prefix + "unique", index.unique);
    PushBool(&fields, prefix + "approximate", index.approximate);
    fields.push_back({prefix + "key_envelopes", EncodeCrudPairs({{"keys", [&]() {
      std::string joined;
      for (std::size_t key_i = 0; key_i < index.key_envelopes.size(); ++key_i) {
        if (key_i != 0) { joined += ","; }
        joined += index.key_envelopes[key_i];
      }
      return joined;
    }()}})});
    fields.push_back({prefix + "predicate_kind", index.predicate_kind});
    fields.push_back({prefix + "predicate_column", index.predicate_column});
    fields.push_back({prefix + "predicate_value", index.predicate_value});
    fields.push_back({prefix + "residency_policy", index.residency_policy});
  }
  fields.push_back({"required_evidence_kinds", "relation_descriptor,row_version,transaction_inventory,dirty_manifest"});
  return fields;
}

MgaRelationStorageDescriptor DeserializeMgaRelationStorageDescriptor(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  MgaRelationStorageDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = FieldValue(fields, "descriptor_uuid");
  descriptor.database_uuid.canonical = FieldValue(fields, "database_uuid");
  descriptor.schema_uuid.canonical = FieldValue(fields, "schema_uuid");
  descriptor.relation_uuid.canonical = FieldValue(fields, "relation_uuid");
  descriptor.primary_filespace_uuid.canonical = FieldValue(fields, "primary_filespace_uuid");
  descriptor.relation_kind = FieldValue(fields, "relation_kind", descriptor.relation_kind);
  descriptor.storage_profile = FieldValue(fields, "storage_profile", descriptor.storage_profile);
  descriptor.descriptor_generation = FieldU64(fields, "descriptor_generation", descriptor.descriptor_generation);
  descriptor.page_size = FieldU32(fields, "page_size", descriptor.page_size);
  descriptor.root_page_number = FieldU64(fields, "root_page_number", descriptor.root_page_number);
  descriptor.allocation_root_page_number = FieldU64(fields, "allocation_root_page_number", descriptor.allocation_root_page_number);
  descriptor.row_identity_rule = FieldValue(fields, "row_identity_rule", descriptor.row_identity_rule);
  descriptor.version_identity_rule = FieldValue(fields, "version_identity_rule", descriptor.version_identity_rule);
  descriptor.mutation_rule = FieldValue(fields, "mutation_rule", descriptor.mutation_rule);
  descriptor.visibility_rule = FieldValue(fields, "visibility_rule", descriptor.visibility_rule);
  descriptor.cleanup_rule = FieldValue(fields, "cleanup_rule", descriptor.cleanup_rule);
  descriptor.recovery_rule = FieldValue(fields, "recovery_rule", descriptor.recovery_rule);
  descriptor.descriptor_status = FieldValue(fields, "descriptor_status", descriptor.descriptor_status);
  const auto column_count = FieldU64(fields, "column_count", 0);
  for (std::size_t i = 0; i < column_count; ++i) {
    const std::string prefix = "column." + std::to_string(i) + ".";
    MgaRelationColumnStorageDescriptor column;
    column.column_uuid.canonical = FieldValue(fields, prefix + "uuid");
    column.ordinal = FieldU32(fields, prefix + "ordinal", static_cast<std::uint32_t>(i));
    column.canonical_name_key = FieldValue(fields, prefix + "name_key");
    column.value_descriptor.descriptor_uuid.canonical = FieldValue(fields, prefix + "descriptor_uuid");
    column.value_descriptor.descriptor_kind = FieldValue(fields, prefix + "descriptor_kind");
    column.value_descriptor.canonical_type_name = FieldValue(fields, prefix + "type_name");
    column.value_descriptor.encoded_descriptor = FieldValue(fields, prefix + "encoded_descriptor");
    column.nullable = FieldBool(fields, prefix + "nullable", true);
    column.generated = FieldBool(fields, prefix + "generated", false);
    column.identity_column = FieldBool(fields, prefix + "identity", false);
    column.storage_class = FieldValue(fields, prefix + "storage_class", column.storage_class);
    column.collation_uuid = FieldValue(fields, prefix + "collation_uuid");
    column.max_inline_bytes = FieldU64(fields, prefix + "max_inline_bytes", column.max_inline_bytes);
    column.overflow_policy = FieldValue(fields, prefix + "overflow_policy", column.overflow_policy);
    descriptor.columns.push_back(std::move(column));
  }
  const auto index_count = FieldU64(fields, "index_count", 0);
  for (std::size_t i = 0; i < index_count; ++i) {
    const std::string prefix = "index." + std::to_string(i) + ".";
    MgaRelationIndexStorageDescriptor index;
    index.index_uuid.canonical = FieldValue(fields, prefix + "uuid");
    index.family = FieldValue(fields, prefix + "family");
    index.profile = FieldValue(fields, prefix + "profile");
    index.unique = FieldBool(fields, prefix + "unique", false);
    index.approximate = FieldBool(fields, prefix + "approximate", false);
    index.predicate_kind = FieldValue(fields, prefix + "predicate_kind");
    index.predicate_column = FieldValue(fields, prefix + "predicate_column");
    index.predicate_value = FieldValue(fields, prefix + "predicate_value");
    index.residency_policy = FieldValue(fields, prefix + "residency_policy", index.residency_policy);
    descriptor.indexes.push_back(std::move(index));
  }
  descriptor.required_evidence_kinds = {"relation_descriptor", "row_version", "transaction_inventory", "dirty_manifest"};
  return descriptor;
}

MgaRelationStorageDescriptor BuildMgaRelationStorageDescriptorFromCrudMetadata(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes,
    const std::vector<std::pair<std::string, std::string>>& persisted_fields) {
  if (!persisted_fields.empty()) {
    return DeserializeMgaRelationStorageDescriptor(persisted_fields);
  }
  return DeserializeMgaRelationStorageDescriptor(
      BuildPersistedMgaRelationDescriptorFields(context, table, indexes));
}

std::vector<std::pair<std::string, std::string>> BuildPersistedMgaRelationDescriptorFields(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes) {
  MgaRelationStorageDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = GeneratedIdentity("object");
  descriptor.database_uuid = context.database_uuid;
  descriptor.schema_uuid = context.current_schema_uuid;
  descriptor.relation_uuid.canonical = table.table_uuid;
  descriptor.primary_filespace_uuid.canonical = context.default_root_uuid.canonical;
  descriptor.page_size = 0;
  descriptor.root_page_number = 0;
  descriptor.allocation_root_page_number = 0;
  descriptor.descriptor_status = "metadata_bridge_vetted_descriptor";
  descriptor.required_evidence_kinds = {"relation_descriptor", "row_version", "transaction_inventory", "dirty_manifest"};
  for (std::size_t i = 0; i < table.columns.size(); ++i) {
    MgaRelationColumnStorageDescriptor column;
    column.column_uuid.canonical = GeneratedIdentity("object");
    column.ordinal = static_cast<std::uint32_t>(i);
    column.canonical_name_key = table.columns[i].first;
    column.value_descriptor.descriptor_uuid.canonical = GeneratedIdentity("object");
    column.value_descriptor.descriptor_kind = "canonical_type_descriptor";
    column.value_descriptor.canonical_type_name = table.columns[i].second;
    column.value_descriptor.encoded_descriptor = table.columns[i].second;
    descriptor.columns.push_back(std::move(column));
  }
  for (const auto& crud_index : indexes) {
    MgaRelationIndexStorageDescriptor index;
    index.index_uuid.canonical = crud_index.index_uuid;
    index.family = crud_index.family.empty() ? CrudIndexFamilyForProfile(crud_index.profile) : crud_index.family;
    index.profile = crud_index.profile;
    index.unique = crud_index.unique;
    index.approximate = crud_index.approximate;
    index.key_envelopes = crud_index.key_envelopes;
    index.include_columns = crud_index.include_columns;
    index.predicate_kind = crud_index.predicate_kind;
    index.predicate_column = crud_index.predicate_column;
    index.predicate_value = crud_index.predicate_value;
    descriptor.indexes.push_back(std::move(index));
  }
  return SerializeMgaRelationStorageDescriptor(descriptor);
}

}  // namespace scratchbird::engine::internal_api
