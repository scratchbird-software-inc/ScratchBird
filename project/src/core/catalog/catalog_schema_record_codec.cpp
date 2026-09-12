// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_schema_record_codec.hpp"
#include <array>
#include <map>
#include <set>

namespace scratchbird::core::catalog {
namespace {
bool Valid(const CatalogSchemaRecord& r) {
  return !r.path_cache.empty() && !r.name_cache.empty() &&
      r.local_single_node_scope && r.recursive_schema_tree &&
      r.creator_transaction_number == 1 &&
      r.schema_object_uuid.value != r.parent_object_uuid.value;
}
}
const CatalogValueSchema& CatalogSchemaRecordSchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65539,1,{
      {1,T::engine_identity,true,16,UuidKind::object},
      {2,T::engine_identity,true,16,UuidKind::object},
      {3,T::utf8_text,true,130844},{4,T::utf8_text,true,130844},
      {5,T::boolean,true,1},{6,T::boolean,true,1},{7,T::boolean,true,1},
      {8,T::unsigned_integer,true,8}}};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogSchemaRecord(const CatalogSchemaRecord& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value,{}};
  if (r.path_cache.size()>130844 || r.name_cache.size()>130844 ||
      r.path_cache.size()+r.name_cache.size()>130845)
    return {CatalogValueError::size_limit,{}};
  return EncodeCatalogValueBlock(CatalogSchemaRecordSchema(),{
      {1,r.schema_object_uuid},{2,r.parent_object_uuid},{3,r.path_cache},
      {4,r.name_cache},{5,r.root_schema},{6,r.local_single_node_scope},
      {7,r.recursive_schema_tree},{8,r.creator_transaction_number}});
}
CatalogSchemaRecordDecodeResult DecodeCatalogSchemaRecord(std::string_view bytes) {
  if (bytes.size()<133 || bytes.size()>130976) return {CatalogValueError::invalid_framing,{}};
  const auto d=DecodeCatalogValueBlock(CatalogSchemaRecordSchema(),std::vector<byte>(bytes.begin(),bytes.end()));
  if (!d.ok()) return {d.error,{}};
  CatalogSchemaRecord r;
  r.schema_object_uuid=std::get<TypedUuid>(d.fields[0].value);
  r.parent_object_uuid=std::get<TypedUuid>(d.fields[1].value);
  r.path_cache=std::get<std::string>(d.fields[2].value);
  r.name_cache=std::get<std::string>(d.fields[3].value);
  r.root_schema=std::get<bool>(d.fields[4].value);
  r.local_single_node_scope=std::get<bool>(d.fields[5].value);
  r.recursive_schema_tree=std::get<bool>(d.fields[6].value);
  r.creator_transaction_number=std::get<u64>(d.fields[7].value);
  if (!Valid(r)) return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,std::move(r)};
}
bool CatalogSchemaPayloadMatchesHeader(const CatalogTypedRecord& r) {
  if (r.header.kind!=CatalogRecordKind::schema || r.header.deleted) return false;
  const auto d=DecodeCatalogSchemaRecord(r.payload);
  return d.ok() &&
      d.record->schema_object_uuid.kind==r.header.object_uuid.kind &&
      d.record->schema_object_uuid.value==r.header.object_uuid.value &&
      d.record->parent_object_uuid.kind==r.header.parent_uuid.kind &&
      d.record->parent_object_uuid.value==r.header.parent_uuid.value;
}
bool ValidateCatalogSchemaGraph(const std::vector<CatalogTypedRecord>& records) {
  using Key=std::array<byte,16>;
  std::map<Key,const CatalogTypedRecord*> objects;
  std::map<Key,CatalogSchemaRecord> schemas;
  unsigned database_count=0;
  for (const auto& r:records) {
    if (r.header.kind!=CatalogRecordKind::database && r.header.kind!=CatalogRecordKind::schema) continue;
    if (!objects.emplace(r.header.object_uuid.value.bytes,&r).second || r.header.deleted) return false;
    if (r.header.kind==CatalogRecordKind::database) ++database_count;
    if (r.header.kind==CatalogRecordKind::schema) {
      if (!CatalogSchemaPayloadMatchesHeader(r)) return false;
      schemas.emplace(r.header.object_uuid.value.bytes,*DecodeCatalogSchemaRecord(r.payload).record);
    }
  }
  if (schemas.empty() || database_count!=1) return false;
  // Mark completed chains once: bounded linear traversal, no recursive stack.
  std::set<Key> complete;
  for (const auto& [id,schema]:schemas) {
    std::set<Key> visiting;
    Key cursor=id;
    while (!complete.count(cursor)) {
      if (!visiting.insert(cursor).second) return false;
      const auto current=schemas.find(cursor);
      if (current==schemas.end()) return false;
      const auto& s=current->second;
      const auto parent=objects.find(s.parent_object_uuid.value.bytes);
      if (parent==objects.end()) return false;
      if (s.root_schema) {
        if (parent->second->header.kind!=CatalogRecordKind::database) return false;
        break;
      }
      if (parent->second->header.kind!=CatalogRecordKind::schema) return false;
      cursor=parent->first;
    }
    complete.insert(visiting.begin(),visiting.end());
  }
  return true;
}
}  // namespace scratchbird::core::catalog
