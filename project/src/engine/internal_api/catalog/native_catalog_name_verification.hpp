// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "catalog_construction_intent.hpp"
#include "catalog_metadata_change.hpp"
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeCatalogNameVerificationError {
  none,source_required,workspace_required,duplicate_identity,invalid_role,
  provisional_row,missing_object,missing_vector,missing_default_name,
  object_binding,vector_binding,default_binding,invalid_unnamed_event
};
struct NativeCatalogNameVerification {
  NativeCatalogNameVerificationError error=NativeCatalogNameVerificationError::source_required;
  core::platform::Uuid object_uuid,referenced_uuid;
  std::size_t definitions=0,vectors=0,entries=0;
  bool ok() const noexcept{return error==NativeCatalogNameVerificationError::none;}
};
// One semantic component of startup verification. Input can only be captured
// from a complete actual selected catalog scan. This proves object uniqueness
// and object/vector/default/alias bindings; namespace, normalization profiles,
// security policy, dependencies and full lifecycle admission remain separate.
inline NativeCatalogNameVerification VerifyNativeCatalogNameBindings(
    const storage::database::NativeCatalogCapture& capture,std::span<std::size_t> scratch,std::span<std::size_t> extra={}) noexcept {
  namespace c=core::catalog;using E=NativeCatalogNameVerificationError;
  NativeCatalogNameVerification out;
  const auto fail=[&](E error,core::platform::Uuid object={},core::platform::Uuid reference={}){
    out.error=error;out.object_uuid=object;out.referenced_uuid=reference;
    out.definitions=out.vectors=out.entries=0;return out;};
  if(!capture.ok())return out;
  const auto rows=capture.rows();
  if(scratch.size()<rows.size()||!storage::disk::detail::DecodedRange(scratch).valid||
      !capture.DisjointWorkspace(std::as_bytes(scratch)))
    return fail(E::workspace_required);
  auto order=scratch.first(rows.size());std::iota(order.begin(),order.end(),std::size_t{0});
  const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  for(std::size_t i=1;i<order.size();++i)if(identity(order[i-1])==identity(order[i]))
    return fail(E::duplicate_identity,identity(order[i]));
  const auto find=[&](const core::platform::Uuid& id)->const storage::database::NativeCatalogVisibleRow*{
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    if(at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted)return nullptr;
    return &rows[*at].row;
  };
  const auto schema=[](const auto& optional,const core::platform::TypedUuid& expected){
    return optional?optional->value==expected.value:expected.value.is_nil();};
  // SB_OPEN_VERIFY_SORTED: the rows that can be a definition's vector or its default entry (a live vector row, or a live entry
  // marked default_for_object), sorted by the object they name and then by capture order, so a definition's candidates are one
  // contiguous range read in capture order (the order the linear scan below visits them, so the first failure and the second
  // occurrence it names are the same).
  std::size_t candidates=0;
  const bool indexed=extra.size()>=rows.size()&&capture.DisjointWorkspace(std::as_bytes(extra))&&
      storage::disk::detail::DisjointNativeDecodeRegions(scratch,extra);
  const auto referenced=[&](std::size_t i)->const core::platform::Uuid&{
    const auto& payload=*rows[i].row.name_payload;
    if(const auto* vv=std::get_if<c::CatalogNameVectorView>(&payload))return vv->object_uuid.value;
    return std::get<c::CatalogNameEntryView>(payload).object_uuid.value;};
  if(indexed){
    for(std::size_t i=0;i<rows.size();++i){
      const auto& candidate=rows[i].row;
      if(candidate.metadata.record.header.deleted||!candidate.name_payload)continue;
      if(std::holds_alternative<c::CatalogNameVectorView>(*candidate.name_payload)||
          std::get<c::CatalogNameEntryView>(*candidate.name_payload).default_for_object)extra[candidates++]=i;
    }
    std::sort(extra.begin(),extra.begin()+static_cast<std::ptrdiff_t>(candidates),[&](auto a,auto b){
      const auto& x=referenced(a);const auto& y=referenced(b);return x==y?a<b:x<y;});
  }
  for(const auto& captured:rows){
    const auto& row=captured.row;const auto& m=row.metadata;const auto id=m.record.header.object_uuid.value;
    if(row.provisional)return fail(E::provisional_row,id);
    if((captured.role==2)!=(m.record.header.kind==c::CatalogRecordKind::localized_name)||
        row.name_payload.has_value()!=(m.record.header.kind==c::CatalogRecordKind::localized_name))return fail(E::invalid_role,id);
    // SB_NATIVE_COMMENT_RELATION: the comments relation (role 7) holds exactly
    // the localized-comment rows. They are data, unnamed (owner Q79 = 1); the
    // comment verifier checks them (VerifyNativeCatalogComments).
    if((captured.role==storage::page::kNativeCommentRelationRole)!=(m.record.header.kind==c::CatalogRecordKind::localized_comment))
      return fail(E::invalid_role,id);
    if(m.record.header.deleted)continue;
    if(m.record.header.kind==c::CatalogRecordKind::localized_comment)continue;
    if(!row.name_payload){
      ++out.definitions;
      if(m.record.header.kind==c::CatalogRecordKind::database&&
          (!m.record.header.parent_uuid.value.is_nil()||!m.owning_schema_uuid.value.is_nil()))
        return fail(E::object_binding,id);
      // This registered immutable evidence family has no aliases. Nullable
      // names alone never exempt an ordinary catalog definition from lookup.
      if(m.object_subtype==scratchbird::core::catalog::CatalogObjectSubtype::node_construction_intent||c::IsCatalogConstructionIntentPayload(m.record.payload)){
        if(captured.role!=6||!c::CatalogConstructionIntentMatchesDatabase(m,capture.source()->database_uuid))
          return fail(E::invalid_unnamed_event,id);
        continue;
      }
      // SB_AUDIT_CATALOG_METADATA_CHANGE (owner Q31 = 1): the registered
      // catalog-change event is immutable evidence too, and unnamed.
      if(m.object_subtype==scratchbird::core::catalog::CatalogObjectSubtype::catalog_metadata_change||c::IsCatalogMetadataChangePayload(m.record.payload)){
        if(captured.role!=6||!c::CatalogMetadataChangeMatchesDatabase(m,capture.source()->database_uuid))
          return fail(E::invalid_unnamed_event,id);
        continue;
      }
      // SEARCH_KEY: SB_NATIVE_NAME_DEFAULTS_ON_ENTRIES (owner Q228 revisited,
      // Q248 = 1, 2026-11-16): the object's default name is its one live
      // entry marked default_for_object (found by object, the definition
      // holding no name reference); none is missing_default_name, two are
      // default_binding. (Was: the entry the definition's default_name_uuid
      // named, equal to the vector's default_name_entry_uuid, in the vector's
      // default language.)
      // SB_CATALOG_NO_NAME_REFERENCE (Q261(b) = 1, 2026-11-18): the object's
      // vector is the one live vector row whose object_uuid is the object
      // (found by object, the definition holding no vector reference); none
      // is missing_vector, two are vector_binding. (Was: the vector the
      // definition's name_vector_uuid named.)
      const c::CatalogNameVectorView* vector=nullptr;const storage::database::NativeCatalogVisibleRow* vector_row=nullptr;
      const c::CatalogNameEntryView* default_entry=nullptr;
      const auto visit=[&](const storage::database::NativeCatalogVisibleRow& candidate)->std::optional<NativeCatalogNameVerification>{
        if(candidate.metadata.record.header.deleted||!candidate.name_payload)return std::nullopt;
        if(const auto* vv=std::get_if<c::CatalogNameVectorView>(&*candidate.name_payload)){
          if(vv->object_uuid.value!=id)return std::nullopt;
          if(vector)return fail(E::vector_binding,id,vv->name_vector_uuid.value);
          vector=vv;vector_row=&candidate;return std::nullopt;
        }
        const auto* x=std::get_if<c::CatalogNameEntryView>(&*candidate.name_payload);
        if(!x||x->object_uuid.value!=id||!x->default_for_object)return std::nullopt;
        if(default_entry)return fail(E::default_binding,id,x->name_entry_uuid.value);
        default_entry=x;return std::nullopt;
      };
      if(indexed){
        const auto begin=extra.begin();const auto end=begin+static_cast<std::ptrdiff_t>(candidates);
        for(auto at=std::lower_bound(begin,end,id,[&](auto i,const core::platform::Uuid& value){return referenced(i)<value;});
            at!=end&&referenced(*at)==id;++at)
          if(const auto failed=visit(rows[*at].row))return *failed;
      }else for(const auto& candidate:rows)
        if(const auto failed=visit(candidate.row))return *failed;
      if(!vector||!vector_row)return fail(E::missing_vector,id);
      if(!default_entry)return fail(E::missing_default_name,id);
      const auto& v=*vector;
      const auto& e=*default_entry;
      if(v.object_uuid.value!=id||e.object_uuid.value!=id||v.object_kind!=m.record.header.kind||
          e.object_kind!=v.object_kind||!schema(v.owning_schema_uuid,m.owning_schema_uuid))return fail(E::object_binding,id);
      if(e.name_vector_uuid.value!=v.name_vector_uuid.value)
        return fail(E::vector_binding,id);
      if(!e.default_for_language)
        return fail(E::default_binding,id,e.name_entry_uuid.value);
    }else if(const auto* v=std::get_if<c::CatalogNameVectorView>(&*row.name_payload)){
      ++out.vectors;const auto* object=find(v->object_uuid.value);
      if(!object||object->name_payload)return fail(E::missing_object,id,v->object_uuid.value);
      // (SB_NATIVE_NAME_DEFAULTS_ON_ENTRIES: was also the definition's
      // default_name_uuid equal to the vector's default_name_entry_uuid;
      // Q261(b) = 1: was also the definition's name_vector_uuid equal to this
      // vector. The definition branch now checks one vector per object.)
    }else{
      ++out.entries;const auto& e=std::get<c::CatalogNameEntryView>(*row.name_payload);
      const auto* object=find(e.object_uuid.value);const auto* vector=find(e.name_vector_uuid.value);
      if(!object||object->name_payload)return fail(E::missing_object,id,e.object_uuid.value);
      if(!vector||!vector->name_payload||!std::holds_alternative<c::CatalogNameVectorView>(*vector->name_payload))
        return fail(E::missing_vector,id,e.name_vector_uuid.value);
      const auto& parent_vector=std::get<c::CatalogNameVectorView>(*vector->name_payload);
      const auto expected_scope=object->metadata.record.header.kind==c::CatalogRecordKind::database?
          object->metadata.record.header.object_uuid.value:object->metadata.record.header.parent_uuid.value;
      // (Q261(b) = 1: was also the definition's name_vector_uuid equal to the entry's.)
      if(e.object_uuid.value!=parent_vector.object_uuid.value||e.object_kind!=parent_vector.object_kind||
          !schema(e.parent_schema_uuid,object->metadata.owning_schema_uuid)||
          e.scope_uuid.value!=expected_scope)return fail(E::object_binding,id,e.object_uuid.value);
      // SB_NATIVE_NAME_DEFAULTS_ON_ENTRIES: the object default is also its
      // language's default. (Was: default_for_object equal to being the
      // vector's default_name_entry_uuid.)
      if(e.default_for_object&&!e.default_for_language)return fail(E::default_binding,id);
    }
  }
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
