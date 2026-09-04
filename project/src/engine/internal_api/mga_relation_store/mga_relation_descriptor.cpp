// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_descriptor.hpp"

#include "api_diagnostics.hpp"
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

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

std::string JoinDescriptorList(const std::vector<std::string>& values) {
  std::string joined;
  for (const auto& value : values) {
    if (!joined.empty()) joined.push_back(',');
    joined += value;
  }
  return joined;
}

std::string EncodeDescriptorList(const std::string& legacy_field,
                                 const std::vector<std::string>& values) {
  return EncodeCrudPairs({{legacy_field, JoinDescriptorList(values)}});
}

std::vector<std::string> DecodeDescriptorList(
    const std::string& encoded,
    const std::string& legacy_field) {
  std::vector<std::string> values;
  const auto pairs = DecodeCrudPairs(encoded);
  if (pairs.size() == 1 && pairs.front().first == legacy_field) {
    const std::string& joined = pairs.front().second;
    std::size_t offset = 0;
    while (offset <= joined.size()) {
      const auto delimiter = joined.find(',', offset);
      const auto length = delimiter == std::string::npos
                              ? joined.size() - offset
                              : delimiter - offset;
      if (length != 0) values.push_back(joined.substr(offset, length));
      if (delimiter == std::string::npos) break;
      offset = delimiter + 1;
    }
    return values;
  }
  for (const auto& pair : pairs) {
    values.push_back(pair.second);
  }
  return values;
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

constexpr std::string_view kCanonicalTextDescriptorUuid =
    "019d0000-0000-7000-8000-00000000d718";
constexpr std::string_view kCanonicalTextTypeUuid =
    "019d0000-0000-7000-8000-00000000d719";
constexpr std::string_view kCanonicalTextCodecUuid =
    "019d0000-0000-7000-8000-00000000d71a";
constexpr std::string_view kCanonicalTextCodecId =
    "datatype.text.utf8.v1";

bool BindFreshMinimalTextDescriptor(
    const std::string& presented,
    const std::string_view column_uuid,
    std::string* bound) {
  if (bound == nullptr || column_uuid.empty()) return false;
  std::map<std::string, std::string> fields;
  std::size_t offset = 0;
  while (offset <= presented.size()) {
    const auto delimiter = presented.find(';', offset);
    const auto length = delimiter == std::string::npos
                            ? presented.size() - offset
                            : delimiter - offset;
    const auto part = TrimDescriptorText(presented.substr(offset, length));
    const auto equals = part.find('=');
    if (part.empty() || equals == std::string::npos || equals == 0 ||
        equals + 1 == part.size()) {
      return false;
    }
    auto key = LowerDescriptorText(
        TrimDescriptorText(part.substr(0, equals)));
    auto value = TrimDescriptorText(part.substr(equals + 1));
    if (key.empty() || value.empty() ||
        !fields.emplace(std::move(key), std::move(value)).second) {
      return false;
    }
    if (delimiter == std::string::npos) break;
    offset = delimiter + 1;
  }
  if (fields.size() != 3 || fields.find("canonical") == fields.end() ||
      fields.find("type_uuid") == fields.end() ||
      fields.find("nullable") == fields.end() ||
      LowerDescriptorText(fields.at("canonical")) != "text" ||
      (fields.at("type_uuid") != kCanonicalTextDescriptorUuid &&
       fields.at("type_uuid") != kCanonicalTextTypeUuid) ||
      (fields.at("nullable") != "true" &&
       fields.at("nullable") != "false")) {
    return false;
  }

  // Some early internal callers projected the current datatype descriptor
  // UUID as though it were the type UUID.  This is a fresh engine-owned bind,
  // so resolve that exact current descriptor to its distinct type/codec
  // cohort once and persist the complete authority.  Existing rich or
  // partially specified descriptors are never inferred or repaired here.
  *bound = "canonical=text;type_uuid=";
  bound->append(kCanonicalTextTypeUuid);
  bound->append(";nullable=");
  bound->append(fields.at("nullable"));
  bound->append(";column_uuid=");
  bound->append(column_uuid);
  bound->append(";datatype_descriptor_uuid=");
  bound->append(kCanonicalTextDescriptorUuid);
  bound->append(";datatype_descriptor_generation=1;type_generation=1;");
  bound->append("codec_uuid=");
  bound->append(kCanonicalTextCodecUuid);
  bound->append(";codec_id=");
  bound->append(kCanonicalTextCodecId);
  bound->append(";codec_version=1;codec_generation=1;null_encoding=1");
  return true;
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
  if (descriptor.descriptor_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "descriptor_uuid_required");
  }
  if (descriptor.relation_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "relation_uuid_required");
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
    if (column.column_uuid.canonical.empty()) {
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
    if (!column.collation_uuid.empty() && column.charset_uuid.empty()) {
      return MakeInvalidRequestDiagnostic(
          "mga.relation_descriptor",
          "column_collation_requires_charset_uuid");
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
    if ((!column.charset_uuid.empty() || !column.collation_uuid.empty()) &&
        column.character_length == 0 && !large_object_text_resource) {
      return MakeInvalidRequestDiagnostic(
          "mga.relation_descriptor",
          "text_resource_descriptor_requires_character_length");
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
  PushU64(&fields, "relation_generation", descriptor.relation_generation);
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
    PushU64(&fields, prefix + "generation", column.column_generation);
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
    fields.push_back({prefix + "charset_uuid", column.charset_uuid});
    fields.push_back({prefix + "collation_uuid", column.collation_uuid});
    PushU64(&fields, prefix + "character_length", column.character_length);
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
    fields.push_back({prefix + "key_envelopes",
                      EncodeDescriptorList("keys", index.key_envelopes)});
    fields.push_back({prefix + "include_columns",
                      EncodeDescriptorList("columns", index.include_columns)});
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
  descriptor.relation_generation =
      FieldU64(fields, "relation_generation", descriptor.descriptor_generation);
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
    column.column_generation =
        FieldU64(fields, prefix + "generation",
                 descriptor.relation_generation);
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
    column.charset_uuid = FieldValue(fields, prefix + "charset_uuid");
    column.collation_uuid = FieldValue(fields, prefix + "collation_uuid");
    column.character_length =
        FieldU32(fields, prefix + "character_length", 0);
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
    index.key_envelopes = DecodeDescriptorList(
        FieldValue(fields, prefix + "key_envelopes"), "keys");
    index.include_columns = DecodeDescriptorList(
        FieldValue(fields, prefix + "include_columns"), "columns");
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
  descriptor.relation_generation =
      table.event_sequence == 0 ? 1 : table.event_sequence;
  descriptor.primary_filespace_uuid.canonical = context.default_root_uuid.canonical;
  descriptor.page_size = 0;
  descriptor.root_page_number = 0;
  descriptor.allocation_root_page_number = 0;
  descriptor.descriptor_status = "metadata_bridge_vetted_descriptor";
  descriptor.required_evidence_kinds = {"relation_descriptor", "row_version", "transaction_inventory", "dirty_manifest"};
  for (std::size_t i = 0; i < table.columns.size(); ++i) {
    MgaRelationColumnStorageDescriptor column;
    column.column_uuid.canonical = GeneratedIdentity("object");
    column.column_generation = descriptor.relation_generation;
    column.ordinal = static_cast<std::uint32_t>(i);
    column.canonical_name_key = table.columns[i].first;
    column.value_descriptor.canonical_type_name =
        EncodedDescriptorTypeName(table.columns[i].second);
    column.value_descriptor.encoded_descriptor = table.columns[i].second;
    if (column.value_descriptor.canonical_type_name == "text") {
      std::string bound_text_descriptor;
      if (BindFreshMinimalTextDescriptor(
              column.value_descriptor.encoded_descriptor,
              column.column_uuid.canonical, &bound_text_descriptor)) {
        column.value_descriptor.encoded_descriptor =
            std::move(bound_text_descriptor);
      } else if (column.value_descriptor.encoded_descriptor.find(
                     "column_uuid=") == std::string::npos) {
        if (!column.value_descriptor.encoded_descriptor.empty())
          column.value_descriptor.encoded_descriptor.push_back(';');
        column.value_descriptor.encoded_descriptor +=
            "column_uuid=" + column.column_uuid.canonical;
      }
    }
    // A rich datatype-bound column already has an exact column occurrence and
    // carries the distinct datatype-registry identity in its encoded
    // descriptor.  Keep the value descriptor anchored to that occurrence.
    // Legacy/minimal columns have no such datatype binding, so they still need
    // a separate engine-issued descriptor identity; reusing a shared type UUID
    // (or the column UUID) would collapse catalog identities.
    column.value_descriptor.descriptor_uuid.canonical =
        EncodedDescriptorField(column.value_descriptor.encoded_descriptor,
                               "datatype_descriptor_uuid")
                .empty()
            ? GeneratedIdentity("object")
            : column.column_uuid.canonical;
    column.value_descriptor.descriptor_kind = "canonical_type_descriptor";
    column.nullable =
        EncodedDescriptorBool(table.columns[i].second, "nullable", true);
    column.generated =
        EncodedDescriptorBool(table.columns[i].second, "generated", false);
    column.identity_column =
        EncodedDescriptorBool(table.columns[i].second, "identity", false);
    column.charset_uuid =
        EncodedDescriptorField(table.columns[i].second, "charset_uuid");
    column.collation_uuid =
        EncodedDescriptorField(table.columns[i].second, "collation_uuid");
    column.character_length =
        EncodedDescriptorU32(table.columns[i].second, "character_length");
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
