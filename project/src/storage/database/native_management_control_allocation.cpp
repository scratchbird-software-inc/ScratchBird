// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_allocation.hpp"
#include "native_inventory_publication_delta.hpp"
#include "native_storage_action_intent.hpp"
#include "native_filespace_directory.hpp"
#include "hash_digest_parts.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <limits>
#include <tuple>

namespace scratchbird::storage::database {
namespace {
using E=NativeManagementControlAllocationError;
using Bytes=std::vector<byte>;
using Maps=std::vector<page::NativeAllocationMap>;
using H=disk::NativeCommonPageHeader;
using S=page::NativeAllocationState;
void Require(bool ok,E e){if(!ok)throw e;}
auto Hash(const Bytes& bytes){const auto hash=core::hash::ComputeSha256Digest(bytes);Require(hash.ok(),E::hash_failure);return hash.digest;}
auto Self(const H& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
bool SameFile(const H& a,const H& b){return a.database_uuid==b.database_uuid&&a.filespace_uuid==b.filespace_uuid&&a.page_size_profile_uuid==b.page_size_profile_uuid&&a.page_size_bytes==b.page_size_bytes;}
const NativeCheckpointRootReference* Root(const NativeCheckpointRoot& cp,u16 role){const auto it=std::find_if(cp.roots.begin(),cp.roots.end(),[&](const auto& r){return r.role==role;});return it==cp.roots.end()?nullptr:&*it;}
void CheckpointError(NativeCheckpointError e){if(e==NativeCheckpointError::none)return;throw e==NativeCheckpointError::hash_failure?E::hash_failure:e==NativeCheckpointError::resource_exhausted?E::resource_exhausted:E::invalid_checkpoint;}
void PlanError(NativePublicationPlanError e){if(e==NativePublicationPlanError::none)return;throw e==NativePublicationPlanError::hash_failure?E::hash_failure:e==NativePublicationPlanError::resource_exhausted?E::resource_exhausted:e==NativePublicationPlanError::cluster_requires_authority?E::cluster_requires_authority:E::invalid_plan;}
const page::NativeAllocationRecord* Record(const Maps& maps,u64 n,S* state=nullptr){
  auto at=std::upper_bound(maps.begin(),maps.end(),n,[](u64 number,const auto& map){return number<map.first_page;});
  if(at!=maps.begin()){const auto& m=*std::prev(at);Require(n-m.first_page<m.states.size(),E::invalid_allocation);
    if(state)*state=m.states[n-m.first_page];
    const auto it=std::lower_bound(m.records.begin(),m.records.end(),n,[](const auto& r,u64 v){return r.page_number<v;});return it==m.records.end()||it->page_number!=n?nullptr:&*it;}
  throw E::invalid_allocation;
}
void Allocated(const Maps& maps,const H& h,const Uuid& owner){S state{};const auto* r=Record(maps,h.page_number,&state);Require(r&&state==S::allocated&&r->page_uuid==h.page_uuid&&r->page_generation==h.page_generation&&r->page_type==h.page_type&&r->owner_uuid==owner,E::binding_mismatch);}
Maps DecodeMaps(const std::vector<Bytes>& bytes,const NativeCheckpointRootReference& root,const H& file){
  Require(!bytes.empty(),E::invalid_allocation);Maps maps;maps.reserve(bytes.size());std::set<u64> slots;std::set<Uuid> ids,allocations,record_pages;u64 covered=0;
  for(const auto& b:bytes){auto decoded=page::DecodeNativeAllocationMap(b);
    if(!decoded.ok())throw decoded.error==page::NativeAllocationError::hash_failure?E::hash_failure:decoded.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:E::invalid_allocation;
    auto m=std::move(*decoded.map);const auto& h=m.header;Require(!h.flags,E::cluster_requires_authority);
    Require(SameFile(h,file)&&m.object_uuid==root.object_uuid&&m.first_page==covered&&slots.insert(h.page_number).second&&ids.insert(h.page_uuid).second,E::binding_mismatch);
    if(maps.empty())Require(root.page_type==3&&Self(h)==root.page&&Hash(b)==root.sha256,E::binding_mismatch);
    else{const auto& first=maps.front();const auto& last=maps.back();Require(last.next&&*last.next==Self(h)&&last.next_sha256==Hash(b)&&m.map_generation==first.map_generation&&m.capacity_generation==first.capacity_generation&&m.total_pages==first.total_pages&&m.creator_transaction_uuid==first.creator_transaction_uuid&&m.creator_local_transaction_id==first.creator_local_transaction_id&&m.creator_operation_uuid==first.creator_operation_uuid,E::binding_mismatch);}
    for(const auto& r:m.records)Require(allocations.insert(r.allocation_uuid).second&&(r.page_uuid.is_nil()||record_pages.insert(r.page_uuid).second),E::binding_mismatch);
    covered+=m.states.size();maps.push_back(std::move(m));
  }
  Require(!maps.back().next&&covered==maps.front().total_pages,E::binding_mismatch);
  for(const auto& m:maps)Allocated(maps,m.header,m.object_uuid);
  return maps;
}
struct Control {H header;Uuid owner;};
using Pages=std::vector<Bytes>;
using Member=page::NativeFilespaceDirectoryRecord;
bool SameBootstrap(const disk::FilespaceBootstrap& a,const disk::FilespaceBootstrap& b){
  return std::tie(a.database_uuid,a.filespace_uuid,a.page_size_profile_uuid,a.checksum_profile_uuid,a.encryption_profile_uuid,
    a.page_size_bytes,a.durable_format_generation,a.flags,a.filespace_role,a.lifecycle_state)==
    std::tie(b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,b.checksum_profile_uuid,b.encryption_profile_uuid,
    b.page_size_bytes,b.durable_format_generation,b.flags,b.filespace_role,b.lifecycle_state);
}
bool SameMember(const Member& a,const Member& b){return SameBootstrap(a.bootstrap,b.bootstrap)&&
  std::tie(a.locator_uuid,a.page_zero_uuid,a.page_zero_generation,a.root_set_generation,a.total_pages,a.verification_epoch,a.operation,a.allocation_root)==
  std::tie(b.locator_uuid,b.page_zero_uuid,b.page_zero_generation,b.root_set_generation,b.total_pages,b.verification_epoch,b.operation,b.allocation_root);}
struct Directory {page::NativeFilespaceDirectory head;std::vector<H> headers;std::map<Uuid,Member> members;};
Directory DecodeDirectory(const Pages& images,const NativeCheckpointRootReference& root,const H& file){
  Require(!images.empty()&&root.page_type==9,E::invalid_delta);Directory out;u64 covered=0;
  std::optional<disk::NativePageReference> next=root.page;auto sha=root.sha256;
  std::set<Uuid> page_ids,zero_ids;std::set<u64> slots;std::optional<Uuid> previous;
  for(const auto& raw:images){const auto decoded=page::DecodeNativeFilespaceDirectory(raw);
    if(!decoded.ok())throw decoded.error==page::NativeDirectoryError::hash_failure?E::hash_failure:
      decoded.error==page::NativeDirectoryError::resource_exhausted?E::resource_exhausted:E::invalid_delta;
    const auto& d=*decoded.directory;const auto& h=d.header;
    Require(SameFile(h,file)&&!h.flags&&next&&*next==Self(h)&&Hash(raw)==sha&&d.object_uuid==root.object_uuid&&
      d.first_record==covered&&page_ids.insert(h.page_uuid).second&&slots.insert(h.page_number).second,E::binding_mismatch);
    if(out.headers.empty())out.head=d;
    else Require(d.directory_generation==out.head.directory_generation&&d.total_records==out.head.total_records&&
      h.page_generation==out.head.header.page_generation&&d.creator_transaction_uuid==out.head.creator_transaction_uuid&&
      d.creator_local_transaction_id==out.head.creator_local_transaction_id&&d.creator_operation_uuid==out.head.creator_operation_uuid,E::binding_mismatch);
    for(const auto& r:d.records){const auto& id=r.bootstrap.filespace_uuid;
      Require((!previous||*previous<id)&&out.members.emplace(id,r).second&&zero_ids.insert(r.page_zero_uuid).second,E::binding_mismatch);previous=id;}
    covered+=d.records.size();next=d.next;sha=d.next_sha256;out.headers.push_back(h);
  }
  Require(!next&&covered==out.head.total_records&&out.members.contains(file.filespace_uuid),E::binding_mismatch);return out;
}
using Groups=std::map<Uuid,Maps>;
Groups DecodeGroups(const Pages& images,const Directory& directory,const NativeCheckpointRootReference& primary,
    const std::map<Uuid,disk::FilespacePageZero>& zeros,const H& file,bool candidate){
  std::map<Uuid,Pages> grouped;
  for(const auto& raw:images){Require(raw.size()>=128,E::invalid_allocation);const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);
    Require(h.ok(),E::invalid_allocation);grouped[h.header->filespace_uuid].push_back(raw);}
  Require(grouped.contains(file.filespace_uuid),E::binding_mismatch);Groups out;
  for(const auto& [fs,pages]:grouped){const auto member=directory.members.find(fs);const auto zero=zeros.find(fs);
    Require(member!=directory.members.end()&&zero!=zeros.end(),E::binding_mismatch);const auto& r=member->second;
    H expected=file;expected.filespace_uuid=fs;expected.page_size_profile_uuid=r.bootstrap.page_size_profile_uuid;expected.page_size_bytes=r.bootstrap.page_size_bytes;
    NativeCheckpointRootReference root;
    if(fs==file.filespace_uuid)root=primary;
    else if(r.allocation_root){const auto& a=*r.allocation_root;root={4,3,a.page,a.object_uuid,a.sha256};}
    else {Require(!candidate,E::binding_mismatch);const auto& z=zero->second;
      const auto a=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& v){return v.kind==3;});
      Require(a!=z.roots.end(),E::binding_mismatch);root={4,3,{a->filespace_uuid,a->page_number,a->page_generation,a->page_size_profile_uuid},a->object_uuid,Hash(pages.front())};}
    auto maps=DecodeMaps(pages,root,expected);const auto& first=maps.front();Require(first.total_pages==r.total_pages,E::binding_mismatch);
    if(r.allocation_root){const auto& a=*r.allocation_root;Require(a.page==root.page&&a.object_uuid==root.object_uuid&&a.sha256==root.sha256&&
      a.map_generation==first.map_generation&&a.capacity_generation==first.capacity_generation,E::binding_mismatch);}
    else Require(!candidate,E::binding_mismatch);
    out.emplace(fs,std::move(maps));
  }
  return out;
}
void DirectoryAllocation(const NativePublicationPlan& p,const NativeCheckpointRoot& a,const NativeCheckpointRoot& b,
    const Pages& extent_bytes,const Pages& before_bytes,const Pages& after_bytes,const NativeManagementDirectoryBase& base,
    const Pages& after_directory,const Pages& after_inventory,const Pages& growth_images,const std::vector<H>& bundle_headers,u64 budget){
  const auto* old_dir=Root(a,3);const auto* new_dir=Root(b,3);const auto* old_map=Root(a,4);const auto* new_map=Root(b,4);
  Require(old_dir&&new_dir&&old_map&&new_map,E::invalid_delta);
  const auto before_directory=DecodeDirectory(base.directory_images,*old_dir,p.header);
  const auto target_directory=DecodeDirectory(after_directory,*new_dir,p.header);
  Require(before_directory.head.object_uuid==target_directory.head.object_uuid&&
    before_directory.head.directory_generation<target_directory.head.directory_generation&&
    target_directory.head.directory_generation==p.reserved_generation&&target_directory.head.header.page_generation==p.reserved_generation&&
    target_directory.head.creator_operation_uuid==p.operation_uuid&&target_directory.head.creator_transaction_uuid.is_nil()&&
    !target_directory.head.creator_local_transaction_id&&before_directory.members.size()==target_directory.members.size(),E::invalid_delta);
  std::map<Uuid,disk::FilespacePageZero> zeros;std::map<Uuid,const Bytes*> zero_bytes;
  for(const auto& raw:base.page_zero_images){const auto z=disk::DecodeFilespacePageZero(raw.data(),raw.size());
    if(!z.ok())throw z.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
      z.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:E::binding_mismatch;
    const auto& v=*z.record;const auto& fs=v.bootstrap.filespace_uuid;
    Require(!(v.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
    Require(!(v.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
    const auto r=before_directory.members.find(fs);Require(r!=before_directory.members.end()&&SameBootstrap(v.bootstrap,r->second.bootstrap)&&
      v.page_uuid==r->second.page_zero_uuid&&v.page_generation==r->second.page_zero_generation&&v.root_set_generation==r->second.root_set_generation&&
      v.total_pages==r->second.total_pages&&zeros.emplace(fs,v).second&&zero_bytes.emplace(fs,&raw).second,E::binding_mismatch);
  }
  Require(zeros.contains(p.header.filespace_uuid)&&zeros.at(p.header.filespace_uuid).page_uuid==p.bootstrap_uuid,E::binding_mismatch);
  const auto before=DecodeGroups(before_bytes,before_directory,*old_map,zeros,p.header,false);
  const auto after=DecodeGroups(after_bytes,target_directory,*new_map,zeros,p.header,true);
  Require(before.size()==after.size()&&zeros.size()==before.size(),E::invalid_delta);
  std::optional<NativeStorageActionIntent> request;std::optional<disk::FilespacePageZero> growth;
  if(p.intent.recovery_profile==3||p.intent.recovery_profile==4){const auto& e=*p.management_extent;
    const auto allowance=u64{e.page_count}*p.header.page_size_bytes+4*u64{e.aggregate_bytes}+2*u64{p.header.page_size_bytes};
    const auto operation=DecodeNativeManagementExtent(extent_bytes,e,p.header.database_uuid,p.bootstrap_uuid,allowance);
    if(!operation.ok())throw operation.error==NativeManagementExtentError::hash_failure?E::hash_failure:
      operation.error==NativeManagementExtentError::resource_exhausted?E::resource_exhausted:E::invalid_extent;
    const auto decoded=ReadNativeStorageActionIntentFromOperation(*operation.record,budget);
    if(!decoded.ok())throw decoded.error==NativeStorageIntentError::hash_failure?E::hash_failure:
      decoded.error==NativeStorageIntentError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
    request=*decoded.intent;const auto& i=*request;
    Require(before.contains(i.filespace_uuid)&&i.action==(p.intent.recovery_profile==4?NativeStorageAction::physical_growth:NativeStorageAction::page_preallocation),E::binding_mismatch);
    const auto& z=zeros.at(i.filespace_uuid);const auto& member=before_directory.members.at(i.filespace_uuid);const auto& m=before.at(i.filespace_uuid).front();
    const auto map_image=std::find_if(before_bytes.begin(),before_bytes.end(),[&](const auto& raw){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);return h.ok()&&Self(*h.header)==Self(m.header);});
    Require(map_image!=before_bytes.end()&&i.database_uuid==p.header.database_uuid&&i.locator_uuid==member.locator_uuid&&
      i.page_zero_uuid==z.page_uuid&&i.page_zero_generation==z.page_generation&&i.filespace_root_set_generation==z.root_set_generation&&
      i.page_size_profile_uuid==z.bootstrap.page_size_profile_uuid&&i.page_size_bytes==z.bootstrap.page_size_bytes&&
      i.current_total_pages==m.total_pages&&i.map_generation==m.map_generation&&i.capacity_generation==m.capacity_generation&&
      i.directory_generation==before_directory.head.directory_generation&&i.allocation_root.filespace_uuid==m.header.filespace_uuid&&
      i.allocation_root.page_number==m.header.page_number&&i.allocation_root.page_generation==m.header.page_generation&&
      i.allocation_root.page_size_profile_uuid==m.header.page_size_profile_uuid&&i.allocation_root.object_uuid==m.object_uuid&&i.allocation_sha256==Hash(*map_image)&&
      i.checkpoint.filespace_uuid==p.base_checkpoint.filespace_uuid&&i.checkpoint.page_number==p.base_checkpoint.page_number&&
      i.checkpoint.page_generation==p.base_checkpoint.page_generation&&i.checkpoint.page_size_profile_uuid==p.base_checkpoint.page_size_profile_uuid&&
      i.checkpoint.object_uuid==p.base_checkpoint_object_uuid&&i.checkpoint_sha256==p.base_checkpoint_sha256&&
      i.checkpoint_generation==p.base_checkpoint_generation&&i.checkpoint_root_set_generation==p.base_root_set_generation,E::binding_mismatch);
    if(p.intent.recovery_profile==4){Require(growth_images.size()==2&&growth_images.front()==*zero_bytes.at(i.filespace_uuid),E::binding_mismatch);
      const auto& raw=growth_images.back();const auto decoded=disk::DecodeFilespacePageZero(raw.data(),raw.size());if(!decoded.ok())throw decoded.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:decoded.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:E::invalid_delta;
      growth=*decoded.record;Require(growth->bootstrap.filespace_uuid==i.filespace_uuid&&i.first_page==m.total_pages&&
        growth->total_pages>m.total_pages&&growth->total_pages-m.total_pages==i.page_count,E::invalid_delta);}
  }
  Require(growth_images.empty()==!growth.has_value(),E::invalid_delta);
  for(const auto& [fs,old]:before_directory.members){const auto next=target_directory.members.find(fs);Require(next!=target_directory.members.end(),E::invalid_delta);auto normalized=next->second;
    if(after.contains(fs))normalized.allocation_root=old.allocation_root;
    if(growth&&fs==growth->bootstrap.filespace_uuid){normalized.page_zero_generation=old.page_zero_generation;normalized.root_set_generation=old.root_set_generation;normalized.total_pages=old.total_pages;}
    Require(SameMember(old,normalized),E::invalid_delta);}
  std::set<Uuid> old_allocations,old_pages,new_pages;std::map<std::pair<Uuid,u64>,Control> controls;
  for(const auto& [fs,maps]:before){Require(after.contains(fs),E::invalid_delta);const auto& x=maps.front();const auto& y=after.at(fs).front();const bool grows=growth&&fs==growth->bootstrap.filespace_uuid;
    Require(x.object_uuid==y.object_uuid&&y.map_generation==p.reserved_generation&&y.map_generation>x.map_generation&&
      (grows?(x.capacity_generation<std::numeric_limits<u64>::max()&&y.capacity_generation==x.capacity_generation+1&&y.total_pages==growth->total_pages):
      (x.capacity_generation==y.capacity_generation&&x.total_pages==y.total_pages)),E::invalid_delta);
    const auto& z=zeros.at(fs);Allocated(maps,{z.bootstrap.page_size_bytes,z.bootstrap.filespace_role<=4?1u:2u,z.bootstrap.database_uuid,fs,z.page_uuid,0,z.page_generation,0,z.bootstrap.page_size_profile_uuid},fs);
    for(const auto& m:maps)for(const auto& r:m.records)Require(old_allocations.insert(r.allocation_uuid).second&&(r.page_uuid.is_nil()||old_pages.insert(r.page_uuid).second),E::invalid_delta);
  }
  Allocated(before.at(p.header.filespace_uuid),a.header,a.object_uuid);
  for(const auto& h:before_directory.headers)Allocated(before.at(p.header.filespace_uuid),h,before_directory.head.object_uuid);
  const auto add=[&](const H& h,const Uuid& owner){Require(before.contains(h.filespace_uuid)&&h.database_uuid==p.header.database_uuid&&
      h.page_size_profile_uuid==zeros.at(h.filespace_uuid).bootstrap.page_size_profile_uuid&&!h.flags&&h.page_generation==p.reserved_generation&&
      controls.emplace(std::make_pair(h.filespace_uuid,h.page_number),Control{h,owner}).second&&new_pages.insert(h.page_uuid).second&&!old_pages.contains(h.page_uuid),E::invalid_delta);
    for(const auto& [fs,r]:before_directory.members){(void)fs;Require(h.page_uuid!=r.page_zero_uuid,E::invalid_delta);}
    S state{};const auto* r=Record(before.at(h.filespace_uuid),h.page_number,&state);Require(!r&&state==S::free,E::invalid_delta);};
  add(b.header,b.object_uuid);add(p.header,p.object_uuid);for(const auto& h:bundle_headers)add(h,p.control_bundle->object_uuid);
  for(const auto& raw:extent_bytes){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::invalid_extent);add(*h.header,p.management_extent->object_uuid);}
  for(const auto& h:target_directory.headers)add(h,target_directory.head.object_uuid);
  for(const auto& raw:after_inventory){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok()&&Root(b,1),E::invalid_delta);add(*h.header,Root(b,1)->object_uuid);}
  for(const auto& [fs,maps]:after)for(const auto& m:maps){(void)fs;Require(m.creator_operation_uuid==p.operation_uuid&&m.creator_transaction_uuid.is_nil()&&!m.creator_local_transaction_id,E::invalid_delta);add(m.header,m.object_uuid);}
  std::size_t matched=0;u64 reserved=0;
  const auto preallocated=[&](const page::NativeAllocationRecord* r){Require(request&&r&&r->page_uuid.is_nil()&&!r->page_generation&&
    r->owner_uuid==request->allocation_owner_uuid&&r->page_type==request->allocation_page_type&&r->creator_operation_uuid==p.operation_uuid&&
    r->creator_transaction_uuid.is_nil()&&!r->creator_local_transaction_id&&!r->reuse_horizon&&!old_allocations.contains(r->allocation_uuid),E::invalid_delta);++reserved;};
  for(const auto& [fs,maps]:before){const auto& next=after.at(fs);for(const auto& x:maps)for(std::size_t j=0;j<x.states.size();++j){
    const auto n=x.first_page+j;const auto* old=Record(maps,n);S state{};const auto* current=Record(next,n,&state);const auto control=controls.find({fs,n});
    const bool reserve=request&&request->action==NativeStorageAction::page_preallocation&&fs==request->filespace_uuid&&n>=request->first_page&&n-request->first_page<request->page_count;
    if(reserve){Require(control==controls.end()&&!old&&x.states[j]==S::free&&state==S::preallocated,E::invalid_delta);preallocated(current);continue;}
    if(control!=controls.end()){const auto& c=control->second;Require(current&&state==S::allocated&&current->page_uuid==c.header.page_uuid&&
      current->page_generation==c.header.page_generation&&current->page_type==c.header.page_type&&current->owner_uuid==c.owner&&
      current->creator_operation_uuid==p.operation_uuid&&current->creator_transaction_uuid.is_nil()&&!current->creator_local_transaction_id&&
      !current->reuse_horizon&&!old_allocations.contains(current->allocation_uuid),E::invalid_delta);++matched;continue;}
    if(!n&&growth&&fs==growth->bootstrap.filespace_uuid){Require(old&&current&&x.states[j]==S::allocated&&state==S::allocated&&current->page_generation==growth->page_generation,E::invalid_delta);
      auto normalized=*current;normalized.page_generation=old->page_generation;Require(normalized==*old,E::invalid_delta);continue;}
    Require(x.states[j]==state&&bool(old)==bool(current)&&(!old||*old==*current),E::invalid_delta);
  }
    for(u64 n=maps.front().total_pages;n<next.front().total_pages;++n){S state{};const auto* r=Record(next,n,&state);
      Require(request&&growth&&fs==request->filespace_uuid&&n>=request->first_page&&n-request->first_page<request->page_count,E::invalid_delta);
      if(request->intended_state==NativeStorageIntentState::preallocated){Require(state==S::preallocated,E::invalid_delta);preallocated(r);}
      else Require(state==S::free&&!r,E::invalid_delta);}
  }
  Require(matched==controls.size()&&reserved==(request&&request->intended_state==NativeStorageIntentState::preallocated?request->page_count:0),E::invalid_delta);
}
NativeManagementControlAllocationError ValidateControlAllocation(
  const Bytes& base_bytes,const Bytes& target_bytes,const Bytes& plan_bytes,
  const std::vector<Bytes>& extent_bytes,const std::vector<Bytes>& before_bytes,
  const std::vector<Bytes>& after_bytes,u64 budget,const std::vector<Bytes>& bundle_bytes,
  const std::vector<Bytes>& before_inventory,const NativeManagementDirectoryBase* directory_base) noexcept {
  try{
    u64 used=0;const auto charge=[&](const Bytes& b){Require(b.size()<=budget-used,E::resource_exhausted);used+=b.size();};
    charge(base_bytes);charge(target_bytes);charge(plan_bytes);for(const auto* group:{&extent_bytes,&before_bytes,&after_bytes,&bundle_bytes})for(const auto& b:*group)charge(b);
    if(directory_base)for(const auto* group:{&directory_base->directory_images,&directory_base->page_zero_images})for(const auto& raw:*group)charge(raw);
    auto base=DecodeNativeCheckpointRoot(base_bytes);CheckpointError(base.error);auto target=DecodeNativeCheckpointRoot(target_bytes);CheckpointError(target.error);
    auto encoded=DecodeNativePublicationPlan(plan_bytes);PlanError(encoded.error);const auto& p=*encoded.plan;const auto& a=*base.root;const auto& b=*target.root;
    Require(p.management_extent.has_value(),E::invalid_plan);
    const bool directory=p.control_bundle&&p.control_bundle->directory_count;
    Require(directory==bool(directory_base),E::invalid_plan);
    const bool inventory=p.intent.recovery_profile==2;
    Require(inventory!=before_inventory.empty(),E::invalid_request);
    u64 inventory_work=0;
    if(inventory){const auto charge_inventory=[&](u64 count,u64 unit){Require(unit&&count<=(budget-used)/unit,E::resource_exhausted);used+=count*unit;inventory_work+=count*unit;};
      for(const auto& raw:before_inventory)charge_inventory(raw.size(),4);
      if(!directory)charge_inventory(p.control_bundle->inventory_count,4*u64{p.header.page_size_bytes});}
    const auto& extent=*p.management_extent;
    const u64 extent_budget=u64{extent.page_count}*p.header.page_size_bytes+4*u64{extent.aggregate_bytes}+2*u64{p.header.page_size_bytes};
    PlanError(BindNativePublicationPlanToManagementExtent(p,extent_bytes,extent_budget));
    std::vector<H> bundle_headers;std::vector<Bytes> after_inventory,after_directory,growth_images;
    if(p.control_bundle){const auto& r=*p.control_bundle;const u64 allowance=r.page_count*p.header.page_size_bytes+4*(directory?r.payload_bytes:(r.map_count+r.inventory_count)*p.header.page_size_bytes)+2*u64{p.header.page_size_bytes};
      auto decoded=DecodeNativeManagementControlBundle(bundle_bytes,r,p.header.database_uuid,p.bootstrap_uuid,allowance);
      if(!decoded.ok())throw decoded.error==NativeManagementControlBundleError::resource_exhausted?E::resource_exhausted:decoded.error==NativeManagementControlBundleError::hash_failure?E::hash_failure:decoded.error==NativeManagementControlBundleError::cluster_requires_authority?E::cluster_requires_authority:E::invalid_allocation;
      Require(decoded.allocation_images==after_bytes,E::binding_mismatch);bundle_headers=std::move(decoded.page_headers);after_inventory=std::move(decoded.inventory_images);
      after_directory=std::move(decoded.directory_images);growth_images=std::move(decoded.growth_images);
      if(directory&&inventory)for(const auto& raw:after_inventory){Require(raw.size()<=(budget-used)/4,E::resource_exhausted);used+=4*raw.size();inventory_work+=4*raw.size();}
    }else Require(bundle_bytes.empty(),E::invalid_request);
    Require(!(a.flags&4)&&!(b.flags&4),E::cluster_requires_authority);Require(a.completed&&b.completed&&!a.header.flags&&!b.header.flags,E::invalid_checkpoint);
    Require(SameFile(a.header,p.header)&&SameFile(b.header,p.header)&&p.base_checkpoint==Self(a.header)&&p.base_checkpoint_object_uuid==a.object_uuid&&p.base_checkpoint_sha256==Hash(base_bytes)&&p.base_checkpoint_generation==a.checkpoint_generation&&p.base_root_set_generation==a.root_set_generation&&p.timeline_uuid==a.timeline_uuid,E::binding_mismatch);
    Require(p.target_checkpoint==Self(b.header)&&p.target_checkpoint_object_uuid==b.object_uuid&&p.operation_uuid==b.creator_operation_uuid&&b.creator_transaction_uuid.is_nil()&&!b.creator_local_transaction_id&&p.reserved_generation==b.checkpoint_generation&&p.reserved_generation==b.header.page_generation&&p.target_root_set_generation==b.root_set_generation&&b.predecessor==p.base_checkpoint&&b.predecessor_sha256==p.base_checkpoint_sha256,E::binding_mismatch);
    const auto* head=Root(b,16);Require(head&&head->page_type==0x500&&head->page==Self(p.header)&&head->object_uuid==p.object_uuid&&head->sha256==encoded.sha256,E::binding_mismatch);
    const auto* old_head=Root(a,16);if(old_head)Require(p.previous_plan&&*p.previous_plan==old_head->page&&p.previous_plan_object_uuid==old_head->object_uuid&&p.previous_plan_sha256==old_head->sha256,E::binding_mismatch);else Require(!p.previous_plan,E::binding_mismatch);
    const auto graph=ComputeNativePublicationTargetGraphDigest(target_bytes);PlanError(graph.error);Require(graph.sha256==p.target_graph_sha256,E::binding_mismatch);
    Require(b.roots.size()==a.roots.size()+(old_head?0u:1u),E::invalid_delta);
    for(const auto& r:a.roots)if(r.role!=4&&r.role!=16&&!(inventory&&r.role==1)&&!(directory&&r.role==3)){const auto* next=Root(b,r.role);Require(next&&r==*next,E::invalid_delta);}
    if(inventory){const auto* old_inventory=Root(a,1);const auto* new_inventory=Root(b,1);Require(old_inventory&&new_inventory,E::invalid_delta);
      const auto delta=ValidateNativeInventoryPublicationDelta(p.header.database_uuid,*old_inventory,*new_inventory,p.reserved_generation,before_inventory,after_inventory,inventory_work);
      if(!delta.ok())throw delta.error==NativeInventoryDeltaError::resource_exhausted?E::resource_exhausted:delta.error==NativeInventoryDeltaError::hash_failure?E::hash_failure:delta.error==NativeInventoryDeltaError::encrypted_requires_authority?E::encrypted_requires_authority:E::invalid_delta;
      Require(!delta.delta->cluster_difference,E::cluster_requires_authority);
      Require(a.selected_local_transaction_id==delta.delta->before.next_local_transaction_id-1&&b.selected_local_transaction_id==delta.delta->after.next_local_transaction_id-1,E::invalid_delta);
      if(p.intent.startup_binding){const auto& binding=*p.intent.startup_binding;
        const auto& entries=delta.delta->after.entries;
        const auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.identity.local_id.value==binding.local_transaction_id;});
        Require(entry!=entries.end()&&entry->identity.transaction_uuid.value==binding.transaction_uuid&&
          entry->identity.scope==transaction::mga::TransactionScope::local_node,E::binding_mismatch);
        for(const auto& difference:delta.delta->differences){
          Require(difference.after&&difference.after->identity.local_id.value==binding.local_transaction_id&&
            difference.after->identity.transaction_uuid.value==binding.transaction_uuid&&
            difference.after->identity.scope==transaction::mga::TransactionScope::local_node,E::invalid_delta);
        }
      }
    }
    auto normalized=b;normalized.header=a.header;normalized.creator_transaction_uuid=a.creator_transaction_uuid;normalized.creator_local_transaction_id=a.creator_local_transaction_id;normalized.creator_operation_uuid=a.creator_operation_uuid;normalized.checkpoint_generation=a.checkpoint_generation;normalized.root_set_generation=a.root_set_generation;normalized.predecessor=a.predecessor;normalized.predecessor_sha256=a.predecessor_sha256;normalized.roots=a.roots;
    if(inventory)normalized.selected_local_transaction_id=a.selected_local_transaction_id;
    const auto unchanged=EncodeNativeCheckpointRoot(normalized);CheckpointError(unchanged.error);Require(unchanged.bytes==base_bytes,E::invalid_delta);
    if(directory){DirectoryAllocation(p,a,b,extent_bytes,before_bytes,after_bytes,*directory_base,after_directory,after_inventory,growth_images,bundle_headers,budget);return E::none;}
    const auto* old_root=Root(a,4);const auto* new_root=Root(b,4);Require(old_root&&new_root,E::invalid_allocation);
    const auto before=DecodeMaps(before_bytes,*old_root,p.header);const auto after=DecodeMaps(after_bytes,*new_root,p.header);
    Require(before.front().total_pages==after.front().total_pages&&before.front().object_uuid==after.front().object_uuid&&before.front().capacity_generation==after.front().capacity_generation&&after.front().map_generation>before.front().map_generation,E::invalid_delta);
    std::optional<NativeStorageActionIntent> preallocation;
    if(p.intent.recovery_profile==3){
      const auto operation=DecodeNativeManagementExtent(extent_bytes,extent,p.header.database_uuid,p.bootstrap_uuid,extent_budget);
      if(!operation.ok())throw operation.error==NativeManagementExtentError::resource_exhausted?E::resource_exhausted:
        operation.error==NativeManagementExtentError::hash_failure?E::hash_failure:E::invalid_extent;
      const auto request=ReadNativeStorageActionIntentFromOperation(*operation.record,budget);
      if(!request.ok())throw request.error==NativeStorageIntentError::resource_exhausted?E::resource_exhausted:
        request.error==NativeStorageIntentError::hash_failure?E::hash_failure:E::binding_mismatch;
      preallocation=*request.intent;const auto& i=*preallocation;
      Require(i.action==NativeStorageAction::page_preallocation&&i.filespace_uuid==p.header.filespace_uuid&&
        i.page_size_profile_uuid==p.header.page_size_profile_uuid&&i.page_size_bytes==p.header.page_size_bytes&&
        i.checkpoint.filespace_uuid==p.base_checkpoint.filespace_uuid&&i.checkpoint.page_number==p.base_checkpoint.page_number&&
        i.checkpoint.page_generation==p.base_checkpoint.page_generation&&i.checkpoint.page_size_profile_uuid==p.base_checkpoint.page_size_profile_uuid&&
        i.checkpoint.object_uuid==p.base_checkpoint_object_uuid&&i.checkpoint_sha256==p.base_checkpoint_sha256&&
        i.checkpoint_generation==p.base_checkpoint_generation&&i.checkpoint_root_set_generation==p.base_root_set_generation&&
        i.allocation_root.filespace_uuid==old_root->page.filespace_uuid&&i.allocation_root.page_number==old_root->page.page_number&&
        i.allocation_root.page_generation==old_root->page.page_generation&&i.allocation_root.object_uuid==old_root->object_uuid&&
        i.allocation_sha256==old_root->sha256&&i.current_total_pages==before.front().total_pages&&
        i.map_generation==before.front().map_generation&&i.capacity_generation==before.front().capacity_generation,E::binding_mismatch);
    }
    Allocated(before,a.header,a.object_uuid);
    std::map<u64,Control> controls;std::set<Uuid> page_ids,old_allocations,old_pages;
    for(const auto& m:before)for(const auto& r:m.records){old_allocations.insert(r.allocation_uuid);if(!r.page_uuid.is_nil())old_pages.insert(r.page_uuid);}
    const auto add=[&](const H& h,const Uuid& owner){Require(SameFile(h,p.header)&&!h.flags&&h.page_generation==p.reserved_generation&&controls.emplace(h.page_number,Control{h,owner}).second&&page_ids.insert(h.page_uuid).second&&!old_pages.contains(h.page_uuid),E::invalid_delta);S state{};const auto* record=Record(before,h.page_number,&state);Require(!record&&state==S::free,E::invalid_delta);};
    add(b.header,b.object_uuid);add(p.header,p.object_uuid);
    for(const auto& h:bundle_headers)add(h,p.control_bundle->object_uuid);
    for(const auto& raw:after_inventory){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::invalid_delta);add(*h.header,Root(b,1)->object_uuid);}
    for(const auto& raw:extent_bytes){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::invalid_extent);add(*h.header,p.management_extent->object_uuid);}
    for(const auto& m:after){Require(m.map_generation==p.reserved_generation&&m.creator_transaction_uuid.is_nil()&&!m.creator_local_transaction_id&&m.creator_operation_uuid==p.operation_uuid,E::invalid_delta);add(m.header,m.object_uuid);}
    std::size_t matched=0;u64 reserved=0;
    for(const auto& x:before){std::size_t xi=0;
      for(std::size_t i=0;i<x.states.size();++i){const auto number=x.first_page+i;const auto* xr=xi<x.records.size()&&x.records[xi].page_number==number?&x.records[xi++]:nullptr;
        S next_state{};const auto* yr=Record(after,number,&next_state);const auto found=controls.find(number);
        const bool reserve=preallocation&&number>=preallocation->first_page&&number-preallocation->first_page<preallocation->page_count;
        if(reserve){
          Require(found==controls.end()&&x.states[i]==S::free&&!xr&&next_state==S::preallocated&&yr&&
            yr->page_uuid.is_nil()&&!yr->page_generation&&yr->owner_uuid==preallocation->allocation_owner_uuid&&
            yr->page_type==preallocation->allocation_page_type&&yr->creator_operation_uuid==p.operation_uuid&&
            yr->creator_transaction_uuid.is_nil()&&!yr->creator_local_transaction_id&&!yr->reuse_horizon&&
            !old_allocations.contains(yr->allocation_uuid),E::invalid_delta);++reserved;continue;
        }
        if(found==controls.end()){Require(x.states[i]==next_state&&bool(xr)==bool(yr)&&(!xr||*xr==*yr),E::invalid_delta);continue;}
        const auto& c=found->second;Require(yr&&next_state==S::allocated&&yr->page_uuid==c.header.page_uuid&&yr->page_generation==c.header.page_generation&&yr->page_type==c.header.page_type&&yr->owner_uuid==c.owner&&yr->creator_transaction_uuid.is_nil()&&!yr->creator_local_transaction_id&&yr->creator_operation_uuid==p.operation_uuid&&!yr->reuse_horizon&&!old_allocations.contains(yr->allocation_uuid),E::invalid_delta);++matched;
      }
    }
    Require(matched==controls.size()&&reserved==(preallocation?preallocation->page_count:0),E::invalid_delta);return E::none;
  }catch(E e){return e;}catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::invalid_request;}
}
} // namespace
NativeManagementControlAllocationError ValidateNativeManagementControlAllocation(
  const Bytes& base,const Bytes& target,const Bytes& plan,const Pages& extent,
  const Pages& before,const Pages& after,u64 budget,const Pages& bundle,const Pages& inventory) noexcept {
  return ValidateControlAllocation(base,target,plan,extent,before,after,budget,bundle,inventory,nullptr);
}
NativeManagementControlAllocationError ValidateNativeManagementDirectoryControlAllocation(
  const Bytes& base,const Bytes& target,const Bytes& plan,const Pages& extent,
  const Pages& before,const Pages& after,const NativeManagementDirectoryBase& directory,u64 budget,
  const Pages& bundle,const Pages& inventory) noexcept {
  return ValidateControlAllocation(base,target,plan,extent,before,after,budget,bundle,inventory,&directory);
}
} // namespace scratchbird::storage::database
