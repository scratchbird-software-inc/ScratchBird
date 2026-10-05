// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_table_definition.hpp"
#include "catalog_column_ordinals.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>

namespace scratchbird::core::catalog {
namespace {
using T = CatalogValueType;
constexpr CatalogValueFieldSchema Id(u16 id, UuidKind kind = UuidKind::object, bool required = true) {
  return {id, T::engine_identity, required, 16, kind};
}
constexpr CatalogValueFieldSchema Number(u16 id, bool required = true) {
  return {id, T::unsigned_integer, required, 8};
}
constexpr CatalogValueFieldSchema table_fields[]{
    Id(1), Id(2,UuidKind::database), Id(3,UuidKind::transaction), Number(4), Number(5),
    Id(6), Number(7), Number(8), Number(9), Id(10,UuidKind::transaction),
    Id(11,UuidKind::object,false), Number(12,false)};
constexpr CatalogValueFieldSchema column_fields[]{
    Id(1), Id(2), Id(3,UuidKind::transaction), Number(4), Number(5),
    Id(6), Number(7), Id(8), Number(9), Id(10), {11,T::boolean,true,1},
    Id(12,UuidKind::object,false), Number(13,false),
    Id(14,UuidKind::object,false), Number(15,false),
    Id(16,UuidKind::object,false), Number(17,false), Number(18),
    Id(19,UuidKind::object,false), Number(20,false), Number(21),
    Id(22,UuidKind::object,false), Number(23,false),
    Id(24,UuidKind::object,false), Number(25,false),
    Id(26,UuidKind::object,false), Number(27,false)};
bool Identity(const TypedUuid& id, UuidKind kind) {
  return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
}
bool Same(const TypedUuid& a, const TypedUuid& b) { return a.kind == b.kind && a.value == b.value; }
bool Absent(const CatalogDefinitionReference& r) {
  return r.uuid.kind == UuidKind::unknown && r.uuid.value.is_nil() && r.generation == 0;
}
bool Reference(const CatalogDefinitionReference& r) { return Identity(r.uuid,UuidKind::object) && r.generation != 0; }
bool Optional(const CatalogDefinitionReference& r) { return Absent(r) || Reference(r); }
bool Valid(const CatalogTableDefinition& d) {
  return Identity(d.table_uuid,UuidKind::object) && Identity(d.database_uuid,UuidKind::database) &&
      Identity(d.origin_transaction_uuid,UuidKind::transaction) && d.origin_local_transaction_id &&
      ValidCatalogColumnOrdinalHighWater(d.next_column_ordinal) && Reference(d.storage_descriptor) &&
      d.table_uuid.value != d.storage_descriptor.uuid.value && d.type_enforcement_generation &&
      Identity(d.type_enforcement_transaction_uuid,UuidKind::transaction) && Optional(d.coercion_profile) &&
      ((d.type_enforcement == CatalogTableTypeEnforcement::strict && Absent(d.coercion_profile)) ||
       (d.type_enforcement == CatalogTableTypeEnforcement::coercing && Reference(d.coercion_profile)));
}
bool Valid(const CatalogColumnDefinition& d) {
  if (!Identity(d.column_uuid,UuidKind::object) || !Identity(d.table_uuid,UuidKind::object) ||
      d.column_uuid.value == d.table_uuid.value || !Identity(d.origin_transaction_uuid,UuidKind::transaction) ||
      !d.origin_local_transaction_id || d.ordinal > UINT32_MAX || !Reference(d.value_descriptor) ||
      !Reference(d.datatype_descriptor) || !Identity(d.type_uuid,UuidKind::object) ||
      d.value_descriptor.uuid.value == d.column_uuid.value ||
      d.value_descriptor.uuid.value == d.datatype_descriptor.uuid.value ||
      d.value_descriptor.uuid.value == d.type_uuid.value) return false;
  for (const auto* r : {&d.domain,&d.default_expression,&d.generated_expression,&d.identity_sequence,
                         &d.charset,&d.collation,&d.storage_profile})
    if (!Optional(*r)) return false;
  const bool generated = Reference(d.generated_expression);
  const bool identity = Reference(d.identity_sequence);
  if (generated != (d.generated_storage != CatalogGeneratedColumnStorage::none) ||
      static_cast<u64>(d.generated_storage) > 2 ||
      identity != (d.identity_mode != CatalogIdentityColumnMode::none) ||
      static_cast<u64>(d.identity_mode) > 2 ||
      (generated && identity) ||
      ((generated || identity) && Reference(d.default_expression)) ||
      (Reference(d.collation) && !Reference(d.charset))) return false;
  return true;
}
void Append(std::vector<CatalogValueField>& fields, u16 id, const CatalogDefinitionReference& r) {
  if (!Absent(r)) { fields.push_back({id,r.uuid}); fields.push_back({static_cast<u16>(id+1),r.generation}); }
}
template<std::size_t N> auto Decode(CatalogValueSchemaView schema, std::string_view bytes,
                                  std::array<CatalogValueFieldView,N>& fields) {
  return DecodeCatalogValueBlockInto(schema,
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()}, fields);
}
CatalogValueSchema Owning(CatalogValueSchemaView view) {
  return {view.id,view.version,{view.fields.begin(),view.fields.end()}};
}
bool Common(const CatalogMetadataVersionView& m, const TypedUuid& origin, u64 number) {
  return Identity(m.default_name_uuid,UuidKind::object) && Identity(m.name_vector_uuid,UuidKind::object) &&
      m.default_name_uuid.value != m.record.header.object_uuid.value &&
      m.name_vector_uuid.value != m.record.header.object_uuid.value &&
      m.default_name_uuid.value != m.name_vector_uuid.value &&
      Identity(m.owning_schema_uuid,UuidKind::schema) && m.definition_version != 0 &&
      number <= m.creator_local_transaction_id &&
      (m.definition_version != 1 || (Same(origin,m.creator_transaction_uuid) && number == m.creator_local_transaction_id));
}
}  // namespace

CatalogValueSchemaView CatalogTableDefinitionSchemaView() { return {65700,1,table_fields}; }
CatalogValueSchemaView CatalogColumnDefinitionSchemaView() { return {65701,1,column_fields}; }
namespace {
bool SchemaPayload(std::string_view bytes, u32 schema) {
  if (bytes.size() < 20 || bytes.substr(0,4) != "SBCV") return false;
  u32 actual = 0;
  for (unsigned i = 0; i != 4; ++i) actual |= u32(static_cast<unsigned char>(bytes[16+i])) << (8*i);
  return actual == schema;
}
}
bool IsCatalogTableDefinitionPayload(std::string_view bytes) { return SchemaPayload(bytes,65700); }
bool IsCatalogColumnDefinitionPayload(std::string_view bytes) { return SchemaPayload(bytes,65701); }
CatalogValueEncodeResult EncodeCatalogTableDefinition(const CatalogTableDefinition& d) {
  if (!Valid(d)) return {CatalogValueError::invalid_value,{}};
  std::vector<CatalogValueField> f{{1,d.table_uuid},{2,d.database_uuid},{3,d.origin_transaction_uuid},
      {4,d.origin_local_transaction_id},{5,d.next_column_ordinal}};
  Append(f,6,d.storage_descriptor);
  f.push_back({8,static_cast<u64>(d.type_enforcement)});
  f.push_back({9,d.type_enforcement_generation}); f.push_back({10,d.type_enforcement_transaction_uuid});
  Append(f,11,d.coercion_profile);
  return EncodeCatalogValueBlock(Owning(CatalogTableDefinitionSchemaView()),f);
}
CatalogValueEncodeResult EncodeCatalogColumnDefinition(const CatalogColumnDefinition& d) {
  if (!Valid(d)) return {CatalogValueError::invalid_value,{}};
  std::vector<CatalogValueField> f{{1,d.column_uuid},{2,d.table_uuid},{3,d.origin_transaction_uuid},
      {4,d.origin_local_transaction_id},{5,d.ordinal}};
  Append(f,6,d.value_descriptor); Append(f,8,d.datatype_descriptor);
  f.push_back({10,d.type_uuid}); f.push_back({11,d.nullable});
  Append(f,12,d.domain); Append(f,14,d.default_expression); Append(f,16,d.generated_expression);
  f.push_back({18,static_cast<u64>(d.generated_storage)}); Append(f,19,d.identity_sequence);
  f.push_back({21,static_cast<u64>(d.identity_mode)}); Append(f,22,d.charset);
  Append(f,24,d.collation); Append(f,26,d.storage_profile);
  return EncodeCatalogValueBlock(Owning(CatalogColumnDefinitionSchemaView()),f);
}
CatalogDefinitionResult<CatalogTableDefinition> DecodeCatalogTableDefinition(std::string_view bytes) {
  std::array<CatalogValueFieldView,12> fields;
  const auto decoded = Decode(CatalogTableDefinitionSchemaView(),bytes,fields);
  if (!decoded.ok()) return {decoded.error,{}};
  CatalogTableDefinition d;
  for (const auto& f : decoded.fields) switch(f.id) {
    case 1: d.table_uuid=*f.identity(); break;
    case 2: d.database_uuid=*f.identity(); break;
    case 3: d.origin_transaction_uuid=*f.identity(); break;
    case 4: d.origin_local_transaction_id=*f.unsigned_value(); break;
    case 5: d.next_column_ordinal=*f.unsigned_value(); break;
    case 6: d.storage_descriptor.uuid=*f.identity(); break;
    case 7: d.storage_descriptor.generation=*f.unsigned_value(); break;
    case 8: d.type_enforcement=static_cast<CatalogTableTypeEnforcement>(*f.unsigned_value()); break;
    case 9: d.type_enforcement_generation=*f.unsigned_value(); break;
    case 10: d.type_enforcement_transaction_uuid=*f.identity(); break;
    case 11: d.coercion_profile.uuid=*f.identity(); break;
    case 12: d.coercion_profile.generation=*f.unsigned_value(); break;
    default: return {CatalogValueError::unknown_field,{}};
  }
  if (!Valid(d)) return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,d};
}
CatalogDefinitionResult<CatalogColumnDefinition> DecodeCatalogColumnDefinition(std::string_view bytes) {
  std::array<CatalogValueFieldView,27> fields;
  const auto decoded = Decode(CatalogColumnDefinitionSchemaView(),bytes,fields);
  if (!decoded.ok()) return {decoded.error,{}};
  CatalogColumnDefinition d;
  for (const auto& f : decoded.fields) switch(f.id) {
    case 1: d.column_uuid=*f.identity(); break;
    case 2: d.table_uuid=*f.identity(); break;
    case 3: d.origin_transaction_uuid=*f.identity(); break;
    case 4: d.origin_local_transaction_id=*f.unsigned_value(); break;
    case 5: d.ordinal=*f.unsigned_value(); break;
    case 6: d.value_descriptor.uuid=*f.identity(); break;
    case 7: d.value_descriptor.generation=*f.unsigned_value(); break;
    case 8: d.datatype_descriptor.uuid=*f.identity(); break;
    case 9: d.datatype_descriptor.generation=*f.unsigned_value(); break;
    case 10: d.type_uuid=*f.identity(); break;
    case 11: d.nullable=f.bytes[0]!=0; break;
    case 12: d.domain.uuid=*f.identity(); break;
    case 13: d.domain.generation=*f.unsigned_value(); break;
    case 14: d.default_expression.uuid=*f.identity(); break;
    case 15: d.default_expression.generation=*f.unsigned_value(); break;
    case 16: d.generated_expression.uuid=*f.identity(); break;
    case 17: d.generated_expression.generation=*f.unsigned_value(); break;
    case 18: d.generated_storage=static_cast<CatalogGeneratedColumnStorage>(*f.unsigned_value()); break;
    case 19: d.identity_sequence.uuid=*f.identity(); break;
    case 20: d.identity_sequence.generation=*f.unsigned_value(); break;
    case 21: d.identity_mode=static_cast<CatalogIdentityColumnMode>(*f.unsigned_value()); break;
    case 22: d.charset.uuid=*f.identity(); break;
    case 23: d.charset.generation=*f.unsigned_value(); break;
    case 24: d.collation.uuid=*f.identity(); break;
    case 25: d.collation.generation=*f.unsigned_value(); break;
    case 26: d.storage_profile.uuid=*f.identity(); break;
    case 27: d.storage_profile.generation=*f.unsigned_value(); break;
    default: return {CatalogValueError::unknown_field,{}};
  }
  if (!Valid(d)) return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,d};
}
bool CatalogTableDefinitionMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind != CatalogRecordKind::table_descriptor) return false;
  const auto d = DecodeCatalogTableDefinition(r.payload);
  return d.ok() && Same(d.definition->table_uuid,r.header.object_uuid) && Identity(r.header.parent_uuid,UuidKind::object);
}
bool CatalogColumnDefinitionMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind != CatalogRecordKind::column_descriptor) return false;
  const auto d = DecodeCatalogColumnDefinition(r.payload);
  return d.ok() && Same(d.definition->column_uuid,r.header.object_uuid) && Same(d.definition->table_uuid,r.header.parent_uuid);
}
bool CatalogTableDefinitionMatchesMetadata(const CatalogMetadataVersionView& m) {
  if (!CatalogTableDefinitionMatchesHeader(m.record)) return false;
  const auto decoded = DecodeCatalogTableDefinition(m.record.payload);
  const auto& d = *decoded.definition;
  return Common(m,d.origin_transaction_uuid,d.origin_local_transaction_id) &&
      m.object_subtype == "ordinary_persistent_table" && m.authority_scope == CatalogAuthorityScope::local &&
      m.record.header.parent_uuid.value == m.owning_schema_uuid.value && Same(d.storage_descriptor.uuid,m.storage_binding_uuid);
}
bool CatalogColumnDefinitionMatchesMetadata(const CatalogMetadataVersionView& m) {
  if (!CatalogColumnDefinitionMatchesHeader(m.record)) return false;
  const auto decoded = DecodeCatalogColumnDefinition(m.record.payload);
  const auto& d = *decoded.definition;
  return Common(m,d.origin_transaction_uuid,d.origin_local_transaction_id) &&
      m.object_subtype == "persistent_table_column" && m.authority_scope == CatalogAuthorityScope::local;
}
bool CatalogTableDefinitionPreservesOrigin(const CatalogMetadataVersionView& a, const CatalogMetadataVersionView& b) {
  if (a.object_subtype != "ordinary_persistent_table" && b.object_subtype != "ordinary_persistent_table" &&
      !IsCatalogTableDefinitionPayload(a.record.payload) && !IsCatalogTableDefinitionPayload(b.record.payload)) return true;
  if (!CatalogTableDefinitionMatchesMetadata(a) || !CatalogTableDefinitionMatchesMetadata(b)) return false;
  const auto before=DecodeCatalogTableDefinition(a.record.payload), after=DecodeCatalogTableDefinition(b.record.payload);
  const auto& x=*before.definition; const auto& y=*after.definition;
  return Same(x.table_uuid,y.table_uuid) && Same(x.database_uuid,y.database_uuid) &&
      Same(x.origin_transaction_uuid,y.origin_transaction_uuid) && x.origin_local_transaction_id == y.origin_local_transaction_id &&
      CatalogColumnOrdinalHighWaterAdvances(x.next_column_ordinal,y.next_column_ordinal);
}
bool CatalogColumnDefinitionPreservesOrigin(const CatalogMetadataVersionView& a, const CatalogMetadataVersionView& b) {
  if (a.object_subtype != "persistent_table_column" && b.object_subtype != "persistent_table_column" &&
      !IsCatalogColumnDefinitionPayload(a.record.payload) && !IsCatalogColumnDefinitionPayload(b.record.payload)) return true;
  if (!CatalogColumnDefinitionMatchesMetadata(a) || !CatalogColumnDefinitionMatchesMetadata(b)) return false;
  const auto before=DecodeCatalogColumnDefinition(a.record.payload), after=DecodeCatalogColumnDefinition(b.record.payload);
  const auto& x=*before.definition; const auto& y=*after.definition;
  return Same(x.column_uuid,y.column_uuid) && Same(x.table_uuid,y.table_uuid) && x.ordinal == y.ordinal &&
      Same(x.origin_transaction_uuid,y.origin_transaction_uuid) && x.origin_local_transaction_id == y.origin_local_transaction_id;
}
namespace {
template<class Remember>
bool VisitTableColumnCohort(const CatalogMetadataVersionView& table,
    std::span<const CatalogMetadataVersionView> columns, bool fresh_creation, Remember remember) {
  if (!CatalogTableDefinitionMatchesMetadata(table) || columns.empty()) return false;
  const auto definition = DecodeCatalogTableDefinition(table.record.payload);
  const auto& d = *definition.definition;
  if (fresh_creation && (table.definition_version != 1 || d.next_column_ordinal != columns.size())) return false;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const auto& column = columns[i];
    if (!CatalogColumnDefinitionMatchesMetadata(column) ||
        !Same(column.owning_schema_uuid,table.owning_schema_uuid) ||
        !Same(column.record.header.parent_uuid,table.record.header.object_uuid)) return false;
    const auto decoded = DecodeCatalogColumnDefinition(column.record.payload);
    const auto& c = *decoded.definition;
    if (c.ordinal >= d.next_column_ordinal || !remember(i,c)) return false;
    if (fresh_creation && (column.definition_version != 1 || c.ordinal != i ||
        !Same(c.origin_transaction_uuid,d.origin_transaction_uuid) ||
        c.origin_local_transaction_id != d.origin_local_transaction_id)) return false;
  }
  return true;
}
}  // namespace
bool CatalogTableColumnCohortMatches(const CatalogMetadataVersionView& table,
    std::span<const CatalogMetadataVersionView> columns, bool fresh_creation) {
  std::set<Uuid> identities;
  std::set<u64> ordinals;
  return VisitTableColumnCohort(table,columns,fresh_creation,[&](std::size_t,const auto& c) {
    return identities.insert(c.column_uuid.value).second && ordinals.insert(c.ordinal).second;
  });
}
bool CatalogTableColumnCohortMatchesWithScratch(const CatalogMetadataVersionView& table,
    std::span<const CatalogMetadataVersionView> columns, bool fresh_creation,
    std::span<CatalogColumnCohortScratch> scratch) {
  if (scratch.size() < columns.size()) return false;
  auto used = scratch.first(columns.size());
  if (!VisitTableColumnCohort(table,columns,fresh_creation,[&](std::size_t i,const auto& c) {
      used[i]={c.column_uuid.value,c.ordinal};return true;
    })) return false;
  std::sort(used.begin(),used.end(),[](const auto& a,const auto& b){return a.column_uuid < b.column_uuid;});
  for (std::size_t i=1;i<used.size();++i)
    if (used[i-1].column_uuid==used[i].column_uuid) return false;
  std::sort(used.begin(),used.end(),[](const auto& a,const auto& b){return a.ordinal < b.ordinal;});
  for (std::size_t i=1;i<used.size();++i)
    if (used[i-1].ordinal==used[i].ordinal) return false;
  return true;
}
bool CatalogTableColumnCohortPreservesHistory(const CatalogMetadataVersionView& previous_table,
    std::span<const CatalogMetadataVersionView> previous_columns,
    const CatalogMetadataVersionView& successor_table,
    std::span<const CatalogMetadataVersionView> successor_columns) {
  if (!CatalogTableColumnCohortMatches(previous_table,previous_columns,false) ||
      !CatalogTableColumnCohortMatches(successor_table,successor_columns,false) ||
      !CatalogTableDefinitionPreservesOrigin(previous_table,successor_table)) return false;
  const auto previous = DecodeCatalogTableDefinition(previous_table.record.payload);
  std::map<Uuid,const CatalogMetadataVersionView*> old;
  for (const auto& column : previous_columns) old.emplace(column.record.header.object_uuid.value,&column);
  for (const auto& column : successor_columns) {
    const auto found = old.find(column.record.header.object_uuid.value);
    if (found != old.end()) {
      if (!CatalogColumnDefinitionPreservesOrigin(*found->second,column)) return false;
    } else {
      const auto decoded = DecodeCatalogColumnDefinition(column.record.payload);
      if (decoded.definition->ordinal < previous.definition->next_column_ordinal) return false;
    }
  }
  return true;
}
bool CatalogTableColumnCohortPreservesHistoryWithScratch(
    const CatalogMetadataVersionView& previous_table,
    std::span<const CatalogMetadataVersionView> previous_columns,
    const CatalogMetadataVersionView& successor_table,
    std::span<const CatalogMetadataVersionView> successor_columns,
    CatalogColumnHistoryScratch scratch) {
  if (scratch.previous.size() < previous_columns.size() ||
      scratch.successor.size() < successor_columns.size() ||
      scratch.previous_order.size() < previous_columns.size()) return false;
  if (!CatalogTableColumnCohortMatchesWithScratch(previous_table,previous_columns,false,scratch.previous) ||
      !CatalogTableColumnCohortMatchesWithScratch(successor_table,successor_columns,false,scratch.successor) ||
      !CatalogTableDefinitionPreservesOrigin(previous_table,successor_table)) return false;
  const auto previous = DecodeCatalogTableDefinition(previous_table.record.payload);
  auto order = scratch.previous_order.first(previous_columns.size());
  for (std::size_t i=0;i<order.size();++i) order[i]=i;
  std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b) {
    return previous_columns[a].record.header.object_uuid.value < previous_columns[b].record.header.object_uuid.value;
  });
  for (const auto& column : successor_columns) {
    const auto& id = column.record.header.object_uuid.value;
    const auto found = std::lower_bound(order.begin(),order.end(),id,[&](std::size_t index,const Uuid& key) {
      return previous_columns[index].record.header.object_uuid.value < key;
    });
    if (found != order.end() && previous_columns[*found].record.header.object_uuid.value == id) {
      if (!CatalogColumnDefinitionPreservesOrigin(previous_columns[*found],column)) return false;
    } else {
      const auto decoded = DecodeCatalogColumnDefinition(column.record.payload);
      if (decoded.definition->ordinal < previous.definition->next_column_ordinal) return false;
    }
  }
  return true;
}
}  // namespace scratchbird::core::catalog
