// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "mga_relation_store/mga_binary_fields.hpp"
#include "mga_relation_store/mga_contextual_text_sidecar_set_v2.hpp"
#include "core/catalog/catalog_column_ordinals.hpp"
#include <array>
#include <charconv>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace scratchbird::engine::internal_api {
namespace {
constexpr std::string_view kRelationFields[]{
    "identity_encoding", "descriptor_uuid", "database_uuid", "schema_uuid", "relation_uuid",
    "relation_generation", "primary_filespace_uuid", "relation_kind", "storage_profile",
    "descriptor_generation", "page_size", "root_page_number", "allocation_root_page_number",
    "row_identity_rule", "version_identity_rule", "mutation_rule", "visibility_rule",
    "cleanup_rule", "recovery_rule", "descriptor_status", "column_count", "index_count",
    "required_evidence_kinds"};
constexpr std::string_view kColumnFields[]{
    "uuid", "generation", "ordinal", "name_key", "descriptor_uuid", "descriptor_kind",
    "type_name", "encoded_descriptor", "datatype_binding_v1", "nullable", "generated",
    "identity", "storage_class", "charset_uuid", "collation_uuid", "character_length",
    "max_inline_bytes", "overflow_policy"};
constexpr std::string_view kIndexFields[]{
    "uuid", "family", "profile", "unique", "approximate", "key_envelopes", "include_columns",
    "predicate_kind", "predicate_column", "predicate_value", "residency_policy"};
constexpr std::string_view kRequiredEvidence = "relation_descriptor,row_version,transaction_inventory,dirty_manifest";
bool HasFields(const std::unordered_set<std::string_view>& keys, const std::string& prefix,
               std::span<const std::string_view> fields) {
  for (const auto name : fields) {
    if (prefix.empty() ? !keys.contains(name) : !keys.contains(prefix + std::string(name))) return false;
  }
  return true;
}
using ColumnOrdinalMap = std::map<decltype(EngineUuid{}.bytes), std::uint32_t>;
bool BuildColumnOrdinalMap(const MgaRelationStorageDescriptor& descriptor,
                           ColumnOrdinalMap& output) {
  if (descriptor.columns.empty()) return false;
  std::set<std::uint32_t> ordinals;
  for (const auto& column : descriptor.columns) {
    if (!core::uuid::IsEngineIdentityUuid(column.column_uuid) ||
        !ordinals.insert(column.ordinal).second ||
        !output.emplace(column.column_uuid.bytes, column.ordinal).second) return false;
  }
  return true;
}
bool ValidVectorFieldPosition(std::string_view key, std::string_view prefix,
                              std::uint64_t count) {
  if (!key.starts_with(prefix)) return true;
  key.remove_prefix(prefix.size());
  const auto dot = key.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == key.size()) return false;
  const auto position = key.substr(0, dot);
  if (position.size() > 1 && position.front() == '0') return false;
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(position.data(), position.data() + position.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == position.data() + position.size() && value < count;
}
// The base projection is also read from complete sealed TEXT field vectors.
// Recognizing these keys is NOT SBTLTD02/hash/seal admission: the owning
// sidecar-set validator must still validate the untouched complete vector.
bool IsContextualSidecarKey(std::string_view key) {
  if (key == kMgaContextualTextSidecarSetSealKeyV2) return true;
  if (!key.starts_with("column.")) return false;
  const auto dot = key.find('.', 7);
  if (dot == std::string_view::npos) return false;
  const auto suffix = key.substr(dot);
  return (suffix == kMgaContextualTextSidecarBlobSuffixV2 ||
          suffix == kMgaContextualTextSidecarHashSuffixV2) &&
      ValidVectorFieldPosition(key, "column.", std::uint64_t{UINT32_MAX} + 1);
}
std::string FieldValue(const std::vector<std::pair<std::string, std::string>>& fields,
                       const std::string& key,
                       const std::string& fallback = {}) {
  for (const auto& [field_key, field_value] : fields) {
    if (field_key == key) { return field_value; }
  }
  return fallback;
}

template<class Integer>
bool ReadRequiredUnsigned(const std::vector<std::pair<std::string, std::string>>& fields,
                          const std::string& key, Integer* output) {
  const std::string value = FieldValue(fields, key);
  if (value.empty()) return false;
  Integer decoded = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), decoded);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
  *output = decoded;
  return true;
}
std::uint64_t FieldU64(const std::vector<std::pair<std::string, std::string>>& fields,
                       const std::string& key,
                       std::uint64_t fallback = 0) {
  std::uint64_t result = 0;
  return ReadRequiredUnsigned(fields, key, &result) ? result : fallback;
}

bool ReadRequiredBool(const std::vector<std::pair<std::string, std::string>>& fields,
                      const std::string& key, bool* output) {
  const std::string value = FieldValue(fields, key);
  if (value != "0" && value != "1") return false;
  *output = value == "1";
  return true;
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

namespace {
MgaColumnOrdinalStateError OrdinalState(
    const MgaRelationStorageDescriptor& descriptor, std::uint64_t high_water,
    ColumnOrdinalMap& columns) {
  using E = MgaColumnOrdinalStateError;
  if (!core::uuid::IsEngineIdentityUuid(descriptor.relation_uuid))
    return E::invalid_table_identity;
  if (!core::catalog::ValidCatalogColumnOrdinalHighWater(high_water))
    return E::invalid_high_water;
  if (!BuildColumnOrdinalMap(descriptor, columns)) return E::invalid_column_cohort;
  for (const auto& [identity, ordinal] : columns)
    if (!core::catalog::CatalogColumnOrdinalBelowHighWater(ordinal, high_water))
      return E::column_not_below_high_water;
  return E::none;
}
}  // namespace

MgaColumnOrdinalStateError ValidateMgaRelationColumnOrdinalState(
    const MgaRelationStorageDescriptor& descriptor, std::uint64_t high_water) {
  ColumnOrdinalMap columns;
  return OrdinalState(descriptor, high_water, columns);
}

MgaColumnOrdinalStateError ValidateMgaRelationColumnOrdinalEvolution(
    const MgaRelationStorageDescriptor& previous, std::uint64_t previous_high_water,
    const MgaRelationStorageDescriptor& successor, std::uint64_t successor_high_water) {
  using E = MgaColumnOrdinalStateError;
  ColumnOrdinalMap old_columns, new_columns;
  auto error = OrdinalState(previous, previous_high_water, old_columns);
  if (error != E::none) return error;
  error = OrdinalState(successor, successor_high_water, new_columns);
  if (error != E::none) return error;
  if (previous.relation_uuid != successor.relation_uuid) return E::table_identity_mismatch;
  if (successor_high_water < previous_high_water) return E::high_water_decreased;
  for (const auto& [identity, ordinal] : new_columns) {
    const auto found = old_columns.find(identity);
    if (found != old_columns.end() && found->second != ordinal)
      return E::surviving_ordinal_changed;
  }
  for (const auto& [identity, ordinal] : new_columns)
    if (old_columns.find(identity) == old_columns.end() && ordinal < previous_high_water)
      return E::retired_ordinal_reused;
  return E::none;
}

bool MgaRelationStoragePreservesSurvivingColumnOrdinals(
    const MgaRelationStorageDescriptor& previous,
    const MgaRelationStorageDescriptor& successor) {
  if (!core::uuid::IsEngineIdentityUuid(previous.relation_uuid) ||
      previous.relation_uuid != successor.relation_uuid) return false;
  ColumnOrdinalMap old_columns, new_columns;
  if (!BuildColumnOrdinalMap(previous, old_columns) ||
      !BuildColumnOrdinalMap(successor, new_columns)) return false;
  for (const auto& [identity, ordinal] : new_columns) {
    const auto found = old_columns.find(identity);
    if (found != old_columns.end() && found->second != ordinal) return false;
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> SerializeMgaRelationStorageDescriptor(
    const MgaRelationStorageDescriptor& descriptor) {
  std::vector<std::pair<std::string, std::string>> fields;
  if (!descriptor.relation_generation || !descriptor.descriptor_generation)
    throw std::invalid_argument("MGA relation requires explicit definition generations");
  ColumnOrdinalMap column_ordinals;
  if (!BuildColumnOrdinalMap(descriptor, column_ordinals))
    throw std::invalid_argument("MGA relation requires a nonempty unique column cohort");
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
  std::size_t sidecar_fields = 0;
  for (const auto& field : fields) {
    if (!keys.insert(field.first).second) return invalid();
    if (IsContextualSidecarKey(field.first)) ++sidecar_fields;
  }
  if (!HasFields(keys, {}, kRelationFields) ||
      FieldValue(fields, "required_evidence_kinds") != kRequiredEvidence) return invalid();
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
  if (!ReadRequiredUnsigned(fields, "page_size", &descriptor.page_size) ||
      !ReadRequiredUnsigned(fields, "root_page_number", &descriptor.root_page_number) ||
      !ReadRequiredUnsigned(fields, "allocation_root_page_number", &descriptor.allocation_root_page_number)) return invalid();
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
  auto remaining = fields.size() - sidecar_fields - std::size(kRelationFields);
  if (column_count > remaining / std::size(kColumnFields)) return invalid();
  remaining -= column_count * std::size(kColumnFields);
  if (index_count > remaining / std::size(kIndexFields) ||
      index_count * std::size(kIndexFields) != remaining) return invalid();
  for (const auto& field : fields) {
    if (IsContextualSidecarKey(field.first)) continue;
    if (!ValidVectorFieldPosition(field.first, "column.", column_count) ||
        !ValidVectorFieldPosition(field.first, "index.", index_count)) return invalid();
  }
  std::set<decltype(EngineUuid{}.bytes)> column_identities;
  std::set<std::uint32_t> column_ordinals;
  for (std::size_t i = 0; i < column_count; ++i) {
    const std::string prefix = "column." + std::to_string(i) + ".";
    if (!HasFields(keys, prefix, kColumnFields)) return invalid();
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
    const auto ordinal = FieldU64(fields, prefix + "ordinal", UINT64_MAX);
    if (ordinal > UINT32_MAX ||
        !column_ordinals.insert(static_cast<std::uint32_t>(ordinal)).second ||
        !column_identities.insert(column.column_uuid.bytes).second) return invalid();
    column.ordinal = static_cast<std::uint32_t>(ordinal);
    column.canonical_name_key = FieldValue(fields, prefix + "name_key");
    column.value_descriptor.descriptor_kind = FieldValue(fields, prefix + "descriptor_kind");
    column.value_descriptor.canonical_type_name = FieldValue(fields, prefix + "type_name");
    column.value_descriptor.encoded_descriptor = FieldValue(fields, prefix + "encoded_descriptor");
    if (!ReadMgaColumnDatatypeBindingField(fields, prefix, &column.value_descriptor)) {
      return invalid();
    }
    if (!ReadRequiredBool(fields, prefix + "nullable", &column.nullable) ||
        !ReadRequiredBool(fields, prefix + "generated", &column.generated) ||
        !ReadRequiredBool(fields, prefix + "identity", &column.identity_column)) return invalid();
    column.storage_class = FieldValue(fields, prefix + "storage_class", column.storage_class);
    if (!ReadRequiredUnsigned(fields, prefix + "character_length", &column.character_length) ||
        !ReadRequiredUnsigned(fields, prefix + "max_inline_bytes", &column.max_inline_bytes)) return invalid();
    column.overflow_policy = FieldValue(fields, prefix + "overflow_policy", column.overflow_policy);
    descriptor.columns.push_back(std::move(column));
  }
  for (std::size_t i = 0; i < index_count; ++i) {
    const std::string prefix = "index." + std::to_string(i) + ".";
    if (!HasFields(keys, prefix, kIndexFields)) return invalid();
    MgaRelationIndexStorageDescriptor index;
    if (!ReadMgaIdentityField(fields, prefix + "uuid", false, &index.index_uuid)) return invalid();
    index.family = FieldValue(fields, prefix + "family");
    index.profile = FieldValue(fields, prefix + "profile");
    if (!ReadRequiredBool(fields, prefix + "unique", &index.unique) ||
        !ReadRequiredBool(fields, prefix + "approximate", &index.approximate)) return invalid();
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
