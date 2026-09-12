// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_name_record_codec.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace scratchbird::core::catalog {
namespace {
using Type = CatalogValueType;
using Error = CatalogValueError;
bool ValidLifecycle(CatalogNameLifecycle state) {
  const auto raw = static_cast<u64>(state);
  return raw >= 1 && raw <= 7;
}
bool Valid(const CatalogNameVector& r) {
  return !r.object_class.empty() && !r.default_language_tag.empty() &&
      r.catalog_generation_id != 0 && ValidLifecycle(r.lifecycle_state);
}
bool Valid(const CatalogNameEntry& r) {
  return !r.object_class.empty() && !r.language_tag.empty() &&
      r.catalog_generation_id != 0 && r.resource_epoch != 0 &&
      r.name_resolution_epoch != 0 && ValidLifecycle(r.lifecycle_state) &&
      static_cast<u64>(r.name_class) >= 1 && static_cast<u64>(r.name_class) <= 5 &&
      static_cast<u64>(r.quote_style) <= 5 &&
      (r.full_path_lookup_key.empty() == (r.path_component_count == 0));
}
template <typename Record, typename Visitor>
void VisitVector(Record& r, Visitor visit) {
  visit(1, r.name_vector_uuid);
  visit(2, r.object_uuid);
  visit(3, r.object_class);
  visit(4, r.owning_schema_uuid);
  visit(5, r.default_language_tag);
  visit(6, r.default_name_entry_uuid);
  visit(7, r.name_collision_policy_uuid);
  visit(8, r.catalog_generation_id);
  visit(9, r.security_policy_uuid);
  visit(10, r.lifecycle_state);
}
template <typename Record, typename Visitor>
void VisitEntry(Record& r, Visitor visit) {
  visit(1, r.name_entry_uuid);
  visit(2, r.name_vector_uuid);
  visit(3, r.object_uuid);
  visit(4, r.object_class);
  visit(5, r.scope_uuid);
  visit(6, r.parent_object_uuid);
  visit(7, r.parent_schema_uuid);
  visit(8, r.language_tag);
  visit(9, r.name_class);
  visit(10, r.donor_id);
  visit(11, r.dialect_profile_uuid);
  visit(12, r.identifier_profile_uuid);
  visit(13, r.case_fold_profile_uuid);
  visit(14, r.quoted_identifier_profile_uuid);
  visit(15, r.raw_name_text);
  visit(16, r.display_name);
  visit(17, r.was_quoted);
  visit(18, r.quote_style);
  visit(19, r.requires_exact_match);
  visit(20, r.normalized_lookup_key);
  visit(21, r.exact_lookup_key);
  visit(22, r.full_path_lookup_key);
  visit(23, r.path_component_count);
  visit(24, r.search_path_eligible);
  visit(25, r.default_for_language);
  visit(26, r.default_for_object);
  visit(27, r.catalog_generation_id);
  visit(28, r.created_transaction_uuid);
  visit(29, r.dropped_transaction_uuid);
  visit(30, r.security_policy_uuid);
  visit(31, r.resource_epoch);
  visit(32, r.name_resolution_epoch);
  visit(33, r.lifecycle_state);
}

template <typename Value>
void Add(std::vector<CatalogValueField>& fields, u16 id, const Value& value) {
  if constexpr (std::is_enum_v<Value>) fields.push_back({id, static_cast<u64>(value)});
  else fields.push_back({id, value});
}
void Add(std::vector<CatalogValueField>& fields, u16 id,
         const std::optional<TypedUuid>& value) {
  if (value) fields.push_back({id, *value});
}
const CatalogValue* Find(const std::vector<CatalogValueField>& fields, u16 id) {
  const auto found = std::lower_bound(fields.begin(), fields.end(), id,
      [](const CatalogValueField& field, u16 key) { return field.id < key; });
  return found != fields.end() && found->id == id ? &found->value : nullptr;
}
template <typename Value>
bool Read(const std::vector<CatalogValueField>& fields, u16 id, Value* value) {
  const auto* found = Find(fields, id);
  if (!found) return false;
  if constexpr (std::is_enum_v<Value>) {
    const auto* typed = std::get_if<u64>(found);
    if (!typed) return false;
    *value = static_cast<Value>(*typed);
  } else {
    const auto* typed = std::get_if<Value>(found);
    if (!typed) return false;
    *value = *typed;
  }
  return true;
}
bool Read(const std::vector<CatalogValueField>& fields, u16 id,
          std::optional<TypedUuid>* value) {
  const auto* found = Find(fields, id);
  if (!found) { value->reset(); return true; }
  const auto* typed = std::get_if<TypedUuid>(found);
  if (!typed) return false;
  *value = *typed;
  return true;
}
}  // namespace

const CatalogValueSchema& CatalogNameVectorSchema() {
  static const CatalogValueSchema schema{327681, 1, {
    {1, Type::engine_identity, true, 16, UuidKind::object},  // name_vector_uuid
    {2, Type::engine_identity, true, 16, UuidKind::object},  // object_uuid
    {3, Type::utf8_text, true, 131040},  // object_class
    {4, Type::engine_identity, false, 16, UuidKind::schema},  // owning_schema_uuid
    {5, Type::utf8_text, true, 131040},  // default_language_tag
    {6, Type::engine_identity, true, 16, UuidKind::object},  // default_name_entry_uuid
    {7, Type::engine_identity, true, 16, UuidKind::object},  // name_collision_policy_uuid
    {8, Type::unsigned_integer, true, 8},  // catalog_generation_id
    {9, Type::engine_identity, true, 16, UuidKind::object},  // security_policy_uuid
    {10, Type::unsigned_integer, true, 8},  // lifecycle_state
  }};
  return schema;
}
const CatalogValueSchema& CatalogNameEntrySchema() {
  static const CatalogValueSchema schema{327682, 1, {
    {1, Type::engine_identity, true, 16, UuidKind::object},  // name_entry_uuid
    {2, Type::engine_identity, true, 16, UuidKind::object},  // name_vector_uuid
    {3, Type::engine_identity, true, 16, UuidKind::object},  // object_uuid
    {4, Type::utf8_text, true, 131040},  // object_class
    {5, Type::engine_identity, true, 16, UuidKind::object},  // scope_uuid
    {6, Type::engine_identity, false, 16, UuidKind::object},  // parent_object_uuid
    {7, Type::engine_identity, false, 16, UuidKind::schema},  // parent_schema_uuid
    {8, Type::utf8_text, true, 131040},  // language_tag
    {9, Type::unsigned_integer, true, 8},  // name_class
    {10, Type::utf8_text, true, 131040},  // donor_id
    {11, Type::engine_identity, true, 16, UuidKind::object},  // dialect_profile_uuid
    {12, Type::engine_identity, true, 16, UuidKind::object},  // identifier_profile_uuid
    {13, Type::engine_identity, false, 16, UuidKind::object},  // case_fold_profile_uuid
    {14, Type::engine_identity, false, 16, UuidKind::object},  // quoted_identifier_profile_uuid
    {15, Type::utf8_text, true, 131040},  // raw_name_text
    {16, Type::utf8_text, true, 131040},  // display_name
    {17, Type::boolean, true, 1},  // was_quoted
    {18, Type::unsigned_integer, true, 8},  // quote_style
    {19, Type::boolean, true, 1},  // requires_exact_match
    {20, Type::opaque_bytes, true, 131040},  // normalized_lookup_key
    {21, Type::opaque_bytes, true, 131040},  // exact_lookup_key
    {22, Type::opaque_bytes, true, 131040},  // full_path_lookup_key
    {23, Type::unsigned_integer, true, 8},  // path_component_count
    {24, Type::boolean, true, 1},  // search_path_eligible
    {25, Type::boolean, true, 1},  // default_for_language
    {26, Type::boolean, true, 1},  // default_for_object
    {27, Type::unsigned_integer, true, 8},  // catalog_generation_id
    {28, Type::engine_identity, true, 16, UuidKind::transaction},  // created_transaction_uuid
    {29, Type::engine_identity, false, 16, UuidKind::transaction},  // dropped_transaction_uuid
    {30, Type::engine_identity, true, 16, UuidKind::object},  // security_policy_uuid
    {31, Type::unsigned_integer, true, 8},  // resource_epoch
    {32, Type::unsigned_integer, true, 8},  // name_resolution_epoch
    {33, Type::unsigned_integer, true, 8},  // lifecycle_state
  }};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogNameVector(const CatalogNameVector& record) {
  if (!Valid(record)) return {Error::invalid_value, {}};
  std::vector<CatalogValueField> fields;
  VisitVector(record, [&](u16 id, const auto& value) { Add(fields, id, value); });
  return EncodeCatalogValueBlock(CatalogNameVectorSchema(), fields);
}
CatalogValueEncodeResult EncodeCatalogNameEntry(const CatalogNameEntry& record) {
  if (!Valid(record)) return {Error::invalid_value, {}};
  std::vector<CatalogValueField> fields;
  VisitEntry(record, [&](u16 id, const auto& value) { Add(fields, id, value); });
  return EncodeCatalogValueBlock(CatalogNameEntrySchema(), fields);
}
CatalogNameRecordDecodeResult<CatalogNameVector> DecodeCatalogNameVector(const std::vector<byte>& bytes) {
  const auto decoded = DecodeCatalogValueBlock(CatalogNameVectorSchema(), bytes);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogNameVector record;
  bool complete = true;
  VisitVector(record, [&](u16 id, auto& value) { complete = Read(decoded.fields, id, &value) && complete; });
  if (!complete) return {Error::type_mismatch, {}};
  if (!Valid(record)) return {Error::invalid_value, {}};
  return {Error::none, std::move(record)};
}
CatalogNameRecordDecodeResult<CatalogNameEntry> DecodeCatalogNameEntry(const std::vector<byte>& bytes) {
  const auto decoded = DecodeCatalogValueBlock(CatalogNameEntrySchema(), bytes);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogNameEntry record;
  bool complete = true;
  VisitEntry(record, [&](u16 id, auto& value) { complete = Read(decoded.fields, id, &value) && complete; });
  if (!complete) return {Error::type_mismatch, {}};
  if (!Valid(record)) return {Error::invalid_value, {}};
  return {Error::none, std::move(record)};
}
}  // namespace scratchbird::core::catalog
