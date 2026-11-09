// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_namespace_verification.hpp"
#include "native_catalog_naming_profile_verification.hpp"
#include "catalog_naming_collision_policy.hpp"
#include "catalog_policy_descriptor.hpp"
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeCatalogCollisionError {
  none,source_required,workspace_required,duplicate_identity,provisional_row,
  invalid_body,group_ownership,missing_vector,missing_object,missing_policy,
  invalid_policy,missing_body,body_generation,unsupported_scope,missing_class,
  invalid_name,unsupported_lifecycle,naming_profile,conflicting_policy,duplicate_name
};
struct NativeCatalogCollisionVerification {
  NativeCatalogCollisionError error=NativeCatalogCollisionError::source_required;
  core::platform::Uuid object_uuid,referenced_uuid;
  std::size_t bodies=0,groups=0,entries=0,policy_references=0;
  bool ok()const noexcept{return error==NativeCatalogCollisionError::none;}
};
// First-create private catalog verification under explicitly selected database
// policies. Descriptive result only: no public disclosure, policy activation,
// mutation permission or full startup receipt. Non-database scopes require their
// owning scope resolver; transitional name lifecycles require rename/recovery.
inline NativeCatalogCollisionVerification VerifyNativeCatalogNamingCollisions(
    const storage::database::NativeCatalogCapture& capture,NativeCatalogNamespaceWorkspace workspace) noexcept {
  namespace c=core::catalog;namespace ranges=storage::disk::detail;
  using E=NativeCatalogCollisionError;using K=c::CatalogRecordKind;
  NativeCatalogCollisionVerification out;
  const auto fail=[&](E error,core::platform::Uuid object={},core::platform::Uuid reference={}){
    out.error=error;out.object_uuid=object;out.referenced_uuid=reference;
    out.bodies=out.groups=out.entries=out.policy_references=0;return out;
  };
  if(!capture.ok())return out;
  const auto rows=capture.rows();const auto count=rows.size();
  // Nine low bits store only a bounded, ephemeral group/rule projection. The
  // upper bits index this immutable capture; neither projection grants authority.
  if(count>SIZE_MAX>>9||workspace.order.size()<count||workspace.parents.size()<count||workspace.colors.size()<count||
      !ranges::DisjointNativeDecodeRegions(workspace.order,workspace.parents,workspace.colors)||
      !capture.DisjointWorkspace(std::as_bytes(workspace.order))||!capture.DisjointWorkspace(std::as_bytes(workspace.parents))||
      !capture.DisjointWorkspace(std::as_bytes(workspace.colors)))return fail(E::workspace_required);
  if(!VerifyNativeCatalogNamingProfileReferences(capture,workspace.order).ok())return fail(E::naming_profile);
  auto order=workspace.order.first(count),bindings=workspace.parents.first(count);auto marks=workspace.colors.first(count);
  const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
  const auto row_identity=[&](std::size_t i){return rows[i].row.metadata.record.header.row_uuid.value;};
  std::iota(order.begin(),order.end(),std::size_t{0});std::iota(bindings.begin(),bindings.end(),std::size_t{0});
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  std::sort(bindings.begin(),bindings.end(),[&](auto a,auto b){return row_identity(a)<row_identity(b);});
  for(std::size_t i=1;i<count;++i)
    if(identity(order[i-1])==identity(order[i])||row_identity(bindings[i-1])==row_identity(bindings[i]))return fail(E::duplicate_identity);
  const auto raw_find=[&](const core::platform::Uuid& id){
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    return at==order.end()||identity(*at)!=id?SIZE_MAX:*at;
  };
  const auto find=[&](const core::platform::Uuid& id){const auto at=raw_find(id);
    return at==SIZE_MAX||rows[at].row.metadata.record.header.deleted?SIZE_MAX:at;};
  const auto body_family=[](const c::CatalogMetadataVersionView& m){
    return m.object_subtype=="naming_collision_policy_body"||c::IsCatalogNamingCollisionPolicyPayload(m.record.payload);
  };
  const auto active=[](const c::CatalogMetadataVersionView& m){return m.status==c::CatalogObjectStatus::active&&m.lifecycle==c::CatalogObjectLifecycle::active;};
  const auto database=capture.source()->database_uuid;
  for(std::size_t i=0;i<count;++i){
    const auto& m=rows[i].row.metadata;if(rows[i].row.provisional)return fail(E::provisional_row,identity(i));
    if(m.record.header.deleted||!body_family(m))continue;
    if(rows[i].role!=5||!c::CatalogNamingCollisionPolicyMatchesMetadata(m)||!c::CatalogNamingCollisionPolicyMatchesDatabase(m,database))return fail(E::invalid_body,identity(i));
    const auto body=*c::DecodeCatalogNamingCollisionPolicy(m.record.payload).record;
    for(std::size_t g=0;g<body.group_count;++g){const auto id=body.groups[g].group_uuid;
      const auto row_at=std::lower_bound(bindings.begin(),bindings.end(),id,[&](auto j,const auto& value){return row_identity(j)<value;});
      if(raw_find(id)!=SIZE_MAX||(row_at!=bindings.end()&&row_identity(*row_at)==id))return fail(E::group_ownership,identity(i),id);
      for(const auto& fs:capture.filespaces())if(id==fs.bootstrap.filespace_uuid||id==fs.bootstrap.page_size_profile_uuid)
        return fail(E::group_ownership,identity(i),id);
      for(const auto& page:capture.physical_pages())if(id==page.header.page_uuid||id==page.owner_uuid)
        return fail(E::group_ownership,identity(i),id);
    }
    for(std::size_t j=0;j<i;++j){const auto& other=rows[j].row.metadata;
      if(other.record.header.deleted||!body_family(other))continue;
      const auto previous=c::DecodeCatalogNamingCollisionPolicy(other.record.payload);if(!previous.ok())return fail(E::invalid_body,identity(j));
      std::size_t a=0,b=0;
      while(a<body.group_count&&b<previous.record->group_count){
        const auto x=body.groups[a].group_uuid,y=previous.record->groups[b].group_uuid;
        if(x==y)return fail(E::group_ownership,identity(i),x);if(x<y)++a;else ++b;
      }
    }
    ++out.bodies;out.groups+=body.group_count;
  }
  std::fill(bindings.begin(),bindings.end(),SIZE_MAX);std::fill(marks.begin(),marks.end(),0);
  for(std::size_t i=0;i<count;++i){const auto& row=rows[i].row;
    if(row.metadata.record.header.deleted||!row.name_payload)continue;
    const auto* entry=std::get_if<c::CatalogNameEntryView>(&*row.name_payload);if(!entry)continue;
    if(entry->lifecycle_state==c::CatalogNameLifecycle::dropped||entry->lifecycle_state==c::CatalogNameLifecycle::archived)continue;
    if(entry->lifecycle_state!=c::CatalogNameLifecycle::active)return fail(E::unsupported_lifecycle,identity(i));
    const auto object=find(entry->object_uuid.value),vector=find(entry->name_vector_uuid.value);
    if(object==SIZE_MAX)return fail(E::missing_object,identity(i),entry->object_uuid.value);
    if(vector==SIZE_MAX||rows[vector].role!=2||!rows[vector].row.name_payload)return fail(E::missing_vector,identity(i),entry->name_vector_uuid.value);
    const auto* names=std::get_if<c::CatalogNameVectorView>(&*rows[vector].row.name_payload);
    const auto& definition=rows[object].row.metadata;
    if(!names||names->object_uuid.value!=entry->object_uuid.value||entry->object_class!=c::CatalogRecordKindName(definition.record.header.kind)||
        names->object_class!=entry->object_class||definition.name_vector_uuid.value!=entry->name_vector_uuid.value)
      return fail(E::invalid_name,identity(i));
    const auto descriptor=find(names->name_collision_policy_uuid.value);
    if(descriptor==SIZE_MAX)return fail(E::missing_policy,identity(i),names->name_collision_policy_uuid.value);
    const auto& dm=rows[descriptor].row.metadata;
    if(rows[descriptor].role!=5||!active(dm)||!c::CatalogPolicyDescriptorMatchesMetadata(dm)||
        !c::CatalogPolicyDescriptorMatchesDatabase(dm,database))return fail(E::invalid_policy,identity(i),identity(descriptor));
    const auto policy=*c::DecodeCatalogPolicyDescriptor(dm.record.payload).record;
    if(policy.policy_class!=c::CatalogPolicyClass::metadata)return fail(E::invalid_policy,identity(i),identity(descriptor));
    if(policy.scope!=c::CatalogPolicyScope::database||policy.scope_uuid!=database)return fail(E::unsupported_scope,identity(i),identity(descriptor));
    const auto selected=find(policy.body_uuid);
    if(selected==SIZE_MAX)return fail(E::missing_body,identity(i),policy.body_uuid);
    const auto& bm=rows[selected].row.metadata;
    if(rows[selected].role!=5||!active(bm)||!body_family(bm)||!c::CatalogNamingCollisionPolicyMatchesMetadata(bm)||
        !c::CatalogNamingCollisionPolicyMatchesDatabase(bm,database))return fail(E::invalid_body,identity(i),policy.body_uuid);
    const auto body=*c::DecodeCatalogNamingCollisionPolicy(bm.record.payload).record;
    if(body.generation!=policy.body_generation)return fail(E::body_generation,identity(i),policy.body_uuid);
    const auto classes=std::span(body.classes).first(body.class_count);
    const auto binding=std::find_if(classes.begin(),classes.end(),[&](const auto& b){return b.kind==definition.record.header.kind;});
    if(binding==classes.end())return fail(E::missing_class,identity(i),policy.body_uuid);
    const auto groups=std::span(body.groups).first(body.group_count);
    const auto group=std::find_if(groups.begin(),groups.end(),[&](const auto& g){return g.group_uuid==binding->group_uuid;});
    if(group==groups.end())return fail(E::invalid_body,identity(i),binding->group_uuid);
    const auto flags=static_cast<std::size_t>(group->language_lane)-1+
        (static_cast<std::size_t>(group->key_lane)-1)*4;
    bindings[i]=(selected<<9)|(std::size_t(group-groups.begin())<<3)|flags;marks[i]=1;
    ++out.entries;++out.policy_references;
  }
  std::size_t used=0;for(std::size_t i=0;i<count;++i)if(marks[i])order[used++]=i;auto entries=order.first(used);
  const auto name=[&](std::size_t i)->const c::CatalogNameEntryView&{return std::get<c::CatalogNameEntryView>(*rows[i].row.name_payload);};
  // SB_NAME_COLLISION_ORDER: the order the native catalog writer also applies.
  const auto common=[&](std::size_t a,std::size_t b){return c::CompareCatalogNameCollisionScope(name(a),name(b));};
  std::sort(entries.begin(),entries.end(),[&](auto a,auto b){const auto c=common(a,b);return c?c<0:name(a).object_class<name(b).object_class;});
  for(std::size_t i=1;i<entries.size();++i){const auto a=entries[i-1],b=entries[i];
    if(!common(a,b)&&name(a).object_class==name(b).object_class&&(bindings[a]>>9)!=(bindings[b]>>9))
      return fail(E::conflicting_policy,name(a).object_uuid.value,name(b).object_uuid.value);
  }
  const auto compare=[&](std::size_t a,std::size_t b){
    if(const auto result=common(a,b))return result;
    const auto xgroup=bindings[a]>>3,ygroup=bindings[b]>>3;
    if(xgroup!=ygroup)return xgroup<ygroup?-1:1;
    const auto flags=bindings[a]&7;
    return c::CompareCatalogNameCollisionLanes(name(a),name(b),
        static_cast<c::CatalogCollisionLanguageLane>((flags&3)+1),static_cast<c::CatalogCollisionKeyLane>(((flags>>2)&1)+1));
  };
  std::sort(entries.begin(),entries.end(),[&](auto a,auto b){return compare(a,b)<0;});
  for(std::size_t i=1;i<entries.size();++i){const auto a=entries[i-1],b=entries[i];
    if(!compare(a,b)&&name(a).object_uuid.value!=name(b).object_uuid.value)
      return fail(E::duplicate_name,name(a).object_uuid.value,name(b).object_uuid.value);
  }
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
