// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "mga_relation_store/mga_binary_identity_codec.hpp"

#include "api_diagnostics.hpp"
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {


EngineUuid GeneratedIdentity(const std::string& kind) {
  return GenerateCrudEngineUuid(kind);
}


std::string TrimDescriptorText(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string LowerDescriptorText(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string EncodedDescriptorField(const std::string& descriptor,
                                   const std::string& requested_field) {
  const std::string normalized_field = LowerDescriptorText(requested_field);
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto delimiter = descriptor.find(';', offset);
    const auto length = delimiter == std::string::npos
                            ? descriptor.size() - offset
                            : delimiter - offset;
    const std::string part =
        TrimDescriptorText(descriptor.substr(offset, length));
    const auto equals = part.find('=');
    if (equals != std::string::npos &&
        LowerDescriptorText(TrimDescriptorText(part.substr(0, equals))) ==
            normalized_field) {
      return TrimDescriptorText(part.substr(equals + 1));
    }
    if (delimiter == std::string::npos) break;
    offset = delimiter + 1;
  }
  return {};
}

std::uint32_t EncodedDescriptorU32(const std::string& descriptor,
                                   const std::string& field) {
  const std::string value = EncodedDescriptorField(descriptor, field);
  if (value.empty()) return 0;
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size() || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
      return 0;
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (...) {
    return 0;
  }
}

bool EncodedDescriptorBool(const std::string& descriptor,
                           const std::string& field,
                           bool fallback) {
  const std::string value = LowerDescriptorText(
      EncodedDescriptorField(descriptor, field));
  if (value == "1" || value == "true") return true;
  if (value == "0" || value == "false") return false;
  return fallback;
}

std::string EncodedDescriptorTypeName(const std::string& descriptor) {
  for (const std::string_view field :
       {"type", "canonical", "canonical_type", "descriptor"}) {
    auto value = EncodedDescriptorField(descriptor, std::string(field));
    if (!value.empty()) return value;
  }
  return descriptor;
}

std::string EncodedDescriptorBaseType(const std::string& descriptor) {
  std::string type = LowerDescriptorText(
      TrimDescriptorText(EncodedDescriptorTypeName(descriptor)));
  const auto open = type.find('(');
  if (open != std::string::npos) type = type.substr(0, open);
  std::string collapsed;
  bool prior_space = false;
  for (const char ch : type) {
    const bool space = std::isspace(static_cast<unsigned char>(ch)) != 0;
    if (space) {
      if (!collapsed.empty() && !prior_space) collapsed.push_back(' ');
    } else {
      collapsed.push_back(ch);
    }
    prior_space = space;
  }
  return TrimDescriptorText(std::move(collapsed));
}

bool EncodedDescriptorSupportsLargeObjectTextResources(
    const std::string& descriptor) {
  const std::string base = EncodedDescriptorBaseType(descriptor);
  return base == "blob" || base == "clob" ||
         base == "character large object";
}

}  // namespace

EngineApiDiagnostic ValidateMgaRelationStorageDescriptor(const MgaRelationStorageDescriptor& descriptor) {
  if (!core::uuid::IsEngineIdentityUuid(descriptor.descriptor_uuid)) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "descriptor_uuid_required");
  }
  if (!core::uuid::IsEngineIdentityUuid(descriptor.relation_uuid)) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "relation_uuid_required");
  }
  if (!core::uuid::IsEngineIdentityUuid(descriptor.database_uuid) ||
      !core::uuid::IsEngineIdentityUuid(descriptor.schema_uuid) ||
      !core::uuid::IsEngineIdentityUuid(descriptor.primary_filespace_uuid)) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "binary_owner_identity_required");
  }
  if (descriptor.relation_generation == 0 ||
      descriptor.descriptor_generation == 0) {
    return MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor", "relation_generation_required");
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
    if (!core::uuid::IsEngineIdentityUuid(column.column_uuid) ||
        !core::uuid::IsEngineIdentityUuid(column.value_descriptor.descriptor_uuid)) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "column_uuid_required");
    }
    if (column.column_generation == 0) {
      return MakeInvalidRequestDiagnostic(
          "mga.relation_descriptor", "column_generation_required");
    }
    if (column.canonical_name_key.empty()) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "column_name_key_required");
    }
    if (column.value_descriptor.encoded_descriptor.empty()) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "column_descriptor_required");
    }
    if (!HasMgaColumnDatatypeBinding(column.value_descriptor)) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "binary_datatype_binding_required");
    }
    if (!HasMgaColumnResourceIdentities(column.value_descriptor) ||
        column.charset_uuid != column.value_descriptor.charset_uuid ||
        column.collation_uuid != column.value_descriptor.collation_uuid) {
      return MakeInvalidRequestDiagnostic(
          "mga.relation_descriptor",
          "column_bound_resource_identity_mismatch");
    }
    const std::string text_resource_storage = LowerDescriptorText(
        EncodedDescriptorField(column.value_descriptor.encoded_descriptor,
                               "text_resource_storage"));
    const bool large_object_text_resource =
        text_resource_storage == "large_object";
    if (!text_resource_storage.empty() && !large_object_text_resource) {
      return MakeInvalidRequestDiagnostic(
          "mga.relation_descriptor", "text_resource_storage_invalid");
    }
    if (large_object_text_resource &&
        !EncodedDescriptorSupportsLargeObjectTextResources(
            column.value_descriptor.encoded_descriptor)) {
      return MakeInvalidRequestDiagnostic(
          "mga.relation_descriptor",
          "text_resource_modifier_on_incompatible_type");
    }
    if (large_object_text_resource && column.character_length != 0) {
      return MakeInvalidRequestDiagnostic(
          "mga.relation_descriptor",
          "large_object_text_resource_forbids_character_length");
    }
    if ((!column.charset_uuid.is_nil() || !column.collation_uuid.is_nil()) &&
        column.character_length == 0 && !large_object_text_resource) {
      return MakeInvalidRequestDiagnostic(
          "mga.relation_descriptor",
          "text_resource_descriptor_requires_character_length");
    }
  }
  for (const auto& index : descriptor.indexes) {
    if (!core::uuid::IsEngineIdentityUuid(index.index_uuid)) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "index_uuid_required");
    }
    if (index.family.empty()) {
      return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "index_family_required");
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
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
  if (table.bound_columns.size() != table.columns.size() ||
      !HasMgaBoundColumnIdentities(table.bound_columns) ||
      !table.bound_relation_generation || !table.bound_column_generation) {
    throw std::invalid_argument("MGA relation construction requires the published bound column cohort");
  }
  MgaRelationStorageDescriptor descriptor;
  descriptor.descriptor_uuid = GeneratedIdentity("object");
  descriptor.database_uuid = context.database_uuid;
  descriptor.schema_uuid = context.current_schema_uuid;
  descriptor.relation_uuid = table.table_uuid;
  descriptor.relation_generation = table.bound_relation_generation;
  descriptor.primary_filespace_uuid = context.default_root_uuid;
  descriptor.page_size = 0;
  descriptor.root_page_number = 0;
  descriptor.allocation_root_page_number = 0;
  descriptor.descriptor_status = "metadata_bridge_vetted_descriptor";
  descriptor.required_evidence_kinds = {"relation_descriptor", "row_version", "transaction_inventory", "dirty_manifest"};
  for (std::size_t i = 0; i < table.columns.size(); ++i) {
    const auto& bound = table.bound_columns[i];
    MgaRelationColumnStorageDescriptor column;
    if (!BindMgaColumnStorageIdentity(bound, table.bound_column_generation, &column))
      throw std::invalid_argument("MGA column construction requires published binary identities");
    column.canonical_name_key = table.columns[i].first;
    // Preserve existing shape/default metadata while its typed replacement is
    // completed. This string is never the source of the binary datatype binding.
    column.value_descriptor.encoded_descriptor = table.columns[i].second;
    column.generated =
        EncodedDescriptorBool(table.columns[i].second, "generated", false);
    column.identity_column =
        EncodedDescriptorBool(table.columns[i].second, "identity", false);
    column.character_length =
        EncodedDescriptorU32(table.columns[i].second, "character_length");
    descriptor.columns.push_back(std::move(column));
  }
  for (const auto& crud_index : indexes) {
    MgaRelationIndexStorageDescriptor index;
    index.index_uuid = crud_index.index_uuid;
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
