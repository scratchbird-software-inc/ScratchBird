// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "catalog_database_record_codec.hpp"
#include "catalog_schema_definition.hpp"
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeCatalogNamespaceError {
  none,source_required,workspace_required,duplicate_identity,provisional_row,
  missing_database,duplicate_database,database_binding,invalid_schema,
  missing_schema,database_reference,cyclic_parent,unsupported_scope
};
struct NativeCatalogNamespaceVerification {
  NativeCatalogNamespaceError error=NativeCatalogNamespaceError::source_required;
  core::platform::Uuid object_uuid,referenced_uuid,database_object_uuid;
  std::size_t schemas=0,scoped_rows=0;
  bool ok() const noexcept{return error==NativeCatalogNamespaceError::none;}
};
struct NativeCatalogNamespaceWorkspace {
  std::span<std::size_t> order,parents;
  std::span<core::platform::byte> colors;
};
// Local namespace reference closure over the complete captured selected rows.
// This checks the database identity, typed schema definitions, namespace
// membership and an acyclic parent forest. It does not infer canonical schema
// placement from names or admit resources, security, cluster authority or MGA.
inline NativeCatalogNamespaceVerification VerifyNativeCatalogNamespaces(
    const storage::database::NativeCatalogCapture& capture,NativeCatalogNamespaceWorkspace workspace) noexcept {
  namespace c=core::catalog;namespace ranges=storage::disk::detail;
  using E=NativeCatalogNamespaceError;using K=c::CatalogRecordKind;
  NativeCatalogNamespaceVerification out;
  const auto fail=[&](E error,core::platform::Uuid object={},core::platform::Uuid reference={}){
    out.error=error;out.object_uuid=object;out.referenced_uuid=reference;
    out.database_object_uuid={};out.schemas=out.scoped_rows=0;return out;};
  if(!capture.ok())return out;
  const auto rows=capture.rows();const auto count=rows.size();
  if(workspace.order.size()<count||workspace.parents.size()<count||workspace.colors.size()<count||
      !ranges::DisjointNativeDecodeRegions(workspace.order,workspace.parents,workspace.colors)||
      !capture.DisjointWorkspace(std::as_bytes(workspace.order))||
      !capture.DisjointWorkspace(std::as_bytes(workspace.parents))||
      !capture.DisjointWorkspace(std::as_bytes(workspace.colors)))return fail(E::workspace_required);
  auto order=workspace.order.first(count);auto parents=workspace.parents.first(count);
  auto colors=workspace.colors.first(count);
  std::iota(order.begin(),order.end(),std::size_t{0});
  std::fill(parents.begin(),parents.end(),SIZE_MAX);std::fill(colors.begin(),colors.end(),0);
  const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  for(std::size_t i=1;i<count;++i)if(identity(order[i-1])==identity(order[i]))
    return fail(E::duplicate_identity,identity(order[i]));
  const auto find=[&](const core::platform::Uuid& id){
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    return at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted?SIZE_MAX:*at;
  };
  std::size_t database=SIZE_MAX;
  for(std::size_t i=0;i<count;++i){
    const auto& row=rows[i].row;const auto& m=row.metadata;
    if(row.provisional)return fail(E::provisional_row,identity(i));
    if(m.record.header.deleted||m.record.header.kind!=K::database)continue;
    if(database!=SIZE_MAX)return fail(E::duplicate_database,identity(i),identity(database));
    const auto decoded=c::DecodeCatalogDatabaseRecord(m.record.payload);
    if(rows[i].role!=1||!decoded.ok()||
        decoded.record->database_uuid.value!=capture.selection()->header.database_uuid||
        decoded.record->page_size!=capture.selection()->header.page_size_bytes||
        !m.record.header.parent_uuid.value.is_nil()||!m.owning_schema_uuid.value.is_nil())
      return fail(E::database_binding,identity(i));
    database=i;
  }
  if(database==SIZE_MAX)return fail(E::missing_database);
  for(std::size_t i=0;i<count;++i){
    const auto& m=rows[i].row.metadata;
    if(m.record.header.deleted)continue;
    if(!m.owning_schema_uuid.value.is_nil()){
      const auto scope=find(m.owning_schema_uuid.value);
      if(m.owning_schema_uuid.kind!=core::platform::UuidKind::schema||scope==SIZE_MAX||
          rows[scope].role!=1||rows[scope].row.metadata.record.header.kind!=K::schema)
        return fail(E::missing_schema,identity(i),m.owning_schema_uuid.value);
      ++out.scoped_rows;
    }
    if(m.record.header.kind!=K::schema)continue;
    if(rows[i].role!=1||!c::CatalogSchemaDefinitionMatchesMetadata(m))return fail(E::invalid_schema,identity(i));
    const auto decoded=c::DecodeCatalogSchemaDefinition(m.record.payload);
    if(!decoded.ok())return fail(E::invalid_schema,identity(i));
    const auto& schema=*decoded.definition;
    // The cluster schema is a local object in every database
    // (SB_DRIFT_CLUSTER_SCHEMA_IN_EVERY_DATABASE, owner 2026-11-04); cluster
    // authority itself is the cluster library's and never local.
    if(m.authority_scope!=c::CatalogAuthorityScope::local)
      return fail(E::unsupported_scope,identity(i));
    if(schema.database_catalog_object_uuid.value!=identity(database))
      return fail(E::database_reference,identity(i),schema.database_catalog_object_uuid.value);
    if(!schema.parent_schema_uuid.value.is_nil()){
      const auto parent=find(schema.parent_schema_uuid.value);
      if(parent==SIZE_MAX||rows[parent].role!=1||rows[parent].row.metadata.record.header.kind!=K::schema)
        return fail(E::missing_schema,identity(i),schema.parent_schema_uuid.value);
      parents[i]=parent;
    }
    ++out.schemas;
  }
  // Each node changes color at most twice. Deep valid trees do not recurse,
  // and a cycle is refused before any success counters escape.
  for(std::size_t i=0;i<count;++i){
    if(rows[i].row.metadata.record.header.deleted||rows[i].row.metadata.record.header.kind!=K::schema||colors[i])continue;
    auto current=i;
    while(current!=SIZE_MAX&&!colors[current]){colors[current]=1;current=parents[current];}
    if(current!=SIZE_MAX&&colors[current]==1)return fail(E::cyclic_parent,identity(current));
    current=i;
    while(current!=SIZE_MAX&&colors[current]==1){colors[current]=2;current=parents[current];}
  }
  out.database_object_uuid=identity(database);out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
