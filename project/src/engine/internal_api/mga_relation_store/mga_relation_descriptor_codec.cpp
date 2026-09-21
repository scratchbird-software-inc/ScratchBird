// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "mga_relation_store/mga_binary_fields.hpp"
#include <charconv>
#include <stdexcept>
#include <unordered_set>

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
  std::uint64_t decoded = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), decoded);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? decoded : fallback;
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

void PushIdentity(std::vector<std::pair<std::string, std::string>>* fields,
                  const std::string& key, const EngineUuid& identity, bool optional = false) {
  std::string encoded;
  if (!EncodeMgaIdentityField(identity, optional, &encoded))
    throw std::invalid_argument("MGA relation requires a bound binary identity: " + key);
  fields->emplace_back(key, std::move(encoded));
}

// Revision2 list values are count/length-framed binary bytes, not a comma
// or hex-text representation. Names/expressions here are not UUID authority.
std::string EncodeDescriptorList(const std::vector<std::string>& values) {
  if (values.size() > UINT32_MAX) throw std::length_error("MGA descriptor list too large");
  std::string bytes;
  const auto append = [&](std::uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes.push_back(static_cast<char>(value >> (8 * n)));
  };
  append(static_cast<std::uint32_t>(values.size()));
  for (const auto& value : values) {
    if (value.size() > UINT32_MAX) throw std::length_error("MGA descriptor list value too large");
    append(static_cast<std::uint32_t>(value.size())); bytes.append(value);
  }
  return bytes;
}

bool DecodeDescriptorList(const std::string& encoded, std::vector<std::string>* output) {
  const std::span<const std::uint8_t> bytes{
      reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()};
  std::size_t cursor = 0; std::uint32_t count = 0;
  if (!ReadBinaryU32(bytes, &cursor, &count) || count > (bytes.size() - cursor) / 4) return false;
  std::vector<std::string> staged;
  staged.reserve(count);
  for (std::uint32_t n = 0; n < count; ++n) {
    std::string value;
    if (!ReadBinaryString(bytes, &cursor, &value)) return false;
    staged.push_back(std::move(value));
  }
  if (cursor != bytes.size()) return false;
  output->swap(staged); return true;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> SerializeMgaRelationStorageDescriptor(
    const MgaRelationStorageDescriptor& descriptor) {
  std::vector<std::pair<std::string, std::string>> fields;
  if (!descriptor.relation_generation || !descriptor.descriptor_generation)
    throw std::invalid_argument("MGA relation requires explicit definition generations");
  fields.emplace_back("identity_encoding", std::string("\x02\x00", 2));
  PushIdentity(&fields, "descriptor_uuid", descriptor.descriptor_uuid);
  PushIdentity(&fields, "database_uuid", descriptor.database_uuid);
  PushIdentity(&fields, "schema_uuid", descriptor.schema_uuid);
  PushIdentity(&fields, "relation_uuid", descriptor.relation_uuid);
  PushU64(&fields, "relation_generation", descriptor.relation_generation);
  PushIdentity(&fields, "primary_filespace_uuid", descriptor.primary_filespace_uuid);
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
    if (!column.column_generation || !HasMgaColumnResourceIdentities(column.value_descriptor) ||
        column.charset_uuid != column.value_descriptor.charset_uuid ||
        column.collation_uuid != column.value_descriptor.collation_uuid)
      throw std::invalid_argument("MGA column requires exact generation and bound resources");
    PushIdentity(&fields, prefix + "uuid", column.column_uuid);
    PushU64(&fields, prefix + "generation", column.column_generation);
    PushU64(&fields, prefix + "ordinal", column.ordinal);
    fields.push_back({prefix + "name_key", column.canonical_name_key});
    PushIdentity(&fields, prefix + "descriptor_uuid", column.value_descriptor.descriptor_uuid);
    fields.push_back({prefix + "descriptor_kind", column.value_descriptor.descriptor_kind});
    fields.push_back({prefix + "type_name", column.value_descriptor.canonical_type_name});
    fields.push_back({prefix + "encoded_descriptor", column.value_descriptor.encoded_descriptor});
    std::string datatype_binding;
    if (!EncodeMgaColumnDatatypeBinding(column.value_descriptor, &datatype_binding))
      throw std::invalid_argument("MGA relation serialization requires a binary datatype binding");
    fields.emplace_back(prefix + "datatype_binding_v1", std::move(datatype_binding));
    PushBool(&fields, prefix + "nullable", column.nullable);
    PushBool(&fields, prefix + "generated", column.generated);
    PushBool(&fields, prefix + "identity", column.identity_column);
    fields.push_back({prefix + "storage_class", column.storage_class});
    PushIdentity(&fields, prefix + "charset_uuid", column.charset_uuid, true);
    PushIdentity(&fields, prefix + "collation_uuid", column.collation_uuid, true);
    PushU64(&fields, prefix + "character_length", column.character_length);
    PushU64(&fields, prefix + "max_inline_bytes", column.max_inline_bytes);
    fields.push_back({prefix + "overflow_policy", column.overflow_policy});
  }
  PushU64(&fields, "index_count", descriptor.indexes.size());
  for (std::size_t i = 0; i < descriptor.indexes.size(); ++i) {
    const std::string prefix = "index." + std::to_string(i) + ".";
    const auto& index = descriptor.indexes[i];
    PushIdentity(&fields, prefix + "uuid", index.index_uuid);
    fields.push_back({prefix + "family", index.family});
    fields.push_back({prefix + "profile", index.profile});
    PushBool(&fields, prefix + "unique", index.unique);
    PushBool(&fields, prefix + "approximate", index.approximate);
    fields.push_back({prefix + "key_envelopes",
                      EncodeDescriptorList(index.key_envelopes)});
    fields.push_back({prefix + "include_columns",
                      EncodeDescriptorList(index.include_columns)});
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
  const auto invalid = [] {
    MgaRelationStorageDescriptor result;
    result.descriptor_status = "invalid_binary_relation_fields";
    return result;
  };
  std::unordered_set<std::string_view> keys;
  for (const auto& field : fields) if (!keys.insert(field.first).second) return invalid();
  if (FieldValue(fields, "identity_encoding") != std::string("\x02\x00", 2) ||
      !ReadMgaIdentityField(fields, "descriptor_uuid", false, &descriptor.descriptor_uuid) ||
      !ReadMgaIdentityField(fields, "database_uuid", false, &descriptor.database_uuid) ||
      !ReadMgaIdentityField(fields, "schema_uuid", false, &descriptor.schema_uuid) ||
      !ReadMgaIdentityField(fields, "relation_uuid", false, &descriptor.relation_uuid) ||
      !ReadMgaIdentityField(fields, "primary_filespace_uuid", false, &descriptor.primary_filespace_uuid)) return invalid();
  descriptor.relation_generation = FieldU64(fields, "relation_generation");
  descriptor.relation_kind = FieldValue(fields, "relation_kind", descriptor.relation_kind);
  descriptor.storage_profile = FieldValue(fields, "storage_profile", descriptor.storage_profile);
  descriptor.descriptor_generation = FieldU64(fields, "descriptor_generation");
  if (!descriptor.relation_generation || !descriptor.descriptor_generation) return invalid();
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
  const auto index_count = FieldU64(fields, "index_count", UINT64_MAX);
  if (!column_count || column_count > fields.size() || index_count > fields.size()) return invalid();
  for (std::size_t i = 0; i < column_count; ++i) {
    const std::string prefix = "column." + std::to_string(i) + ".";
    MgaRelationColumnStorageDescriptor column;
    if (!ReadMgaIdentityField(fields, prefix + "uuid", false, &column.column_uuid) ||
        !ReadMgaIdentityField(fields, prefix + "descriptor_uuid", false, &column.value_descriptor.descriptor_uuid) ||
        !ReadMgaIdentityField(fields, prefix + "charset_uuid", true, &column.charset_uuid) ||
        !ReadMgaIdentityField(fields, prefix + "collation_uuid", true, &column.collation_uuid)) return invalid();
    column.value_descriptor.charset_uuid = column.charset_uuid;
    column.value_descriptor.collation_uuid = column.collation_uuid;
    if (!HasMgaColumnResourceIdentities(column.value_descriptor)) return invalid();
    column.column_generation = FieldU64(fields, prefix + "generation");
    if (!column.column_generation) return invalid();
    column.ordinal = FieldU32(fields, prefix + "ordinal", static_cast<std::uint32_t>(i));
    column.canonical_name_key = FieldValue(fields, prefix + "name_key");
    column.value_descriptor.descriptor_kind = FieldValue(fields, prefix + "descriptor_kind");
    column.value_descriptor.canonical_type_name = FieldValue(fields, prefix + "type_name");
    column.value_descriptor.encoded_descriptor = FieldValue(fields, prefix + "encoded_descriptor");
    if (!ReadMgaColumnDatatypeBindingField(fields, prefix, &column.value_descriptor)) {
      return invalid();
    }
    column.nullable = FieldBool(fields, prefix + "nullable", true);
    column.generated = FieldBool(fields, prefix + "generated", false);
    column.identity_column = FieldBool(fields, prefix + "identity", false);
    column.storage_class = FieldValue(fields, prefix + "storage_class", column.storage_class);
    column.character_length =
        FieldU32(fields, prefix + "character_length", 0);
    column.max_inline_bytes = FieldU64(fields, prefix + "max_inline_bytes", column.max_inline_bytes);
    column.overflow_policy = FieldValue(fields, prefix + "overflow_policy", column.overflow_policy);
    descriptor.columns.push_back(std::move(column));
  }
  for (std::size_t i = 0; i < index_count; ++i) {
    const std::string prefix = "index." + std::to_string(i) + ".";
    MgaRelationIndexStorageDescriptor index;
    if (!ReadMgaIdentityField(fields, prefix + "uuid", false, &index.index_uuid)) return invalid();
    index.family = FieldValue(fields, prefix + "family");
    index.profile = FieldValue(fields, prefix + "profile");
    index.unique = FieldBool(fields, prefix + "unique", false);
    index.approximate = FieldBool(fields, prefix + "approximate", false);
    if (!DecodeDescriptorList(FieldValue(fields, prefix + "key_envelopes"), &index.key_envelopes) ||
        !DecodeDescriptorList(FieldValue(fields, prefix + "include_columns"), &index.include_columns)) return invalid();
    index.predicate_kind = FieldValue(fields, prefix + "predicate_kind");
    index.predicate_column = FieldValue(fields, prefix + "predicate_column");
    index.predicate_value = FieldValue(fields, prefix + "predicate_value");
    index.residency_policy = FieldValue(fields, prefix + "residency_policy", index.residency_policy);
    descriptor.indexes.push_back(std::move(index));
  }
  descriptor.required_evidence_kinds = {"relation_descriptor", "row_version", "transaction_inventory", "dirty_manifest"};
  return descriptor;
}

}  // namespace scratchbird::engine::internal_api
