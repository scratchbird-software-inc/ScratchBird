// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_allocation.hpp"
#include "hash_digest_parts.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

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
  for(const auto& m:maps)if(n>=m.first_page&&n-m.first_page<m.states.size()){
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
} // namespace
NativeManagementControlAllocationError ValidateNativeManagementControlAllocation(
  const Bytes& base_bytes,const Bytes& target_bytes,const Bytes& plan_bytes,
  const std::vector<Bytes>& extent_bytes,const std::vector<Bytes>& before_bytes,
  const std::vector<Bytes>& after_bytes,u64 budget) noexcept {
  try{
    u64 used=0;const auto charge=[&](const Bytes& b){Require(b.size()<=budget-used,E::resource_exhausted);used+=b.size();};
    charge(base_bytes);charge(target_bytes);charge(plan_bytes);for(const auto* group:{&extent_bytes,&before_bytes,&after_bytes})for(const auto& b:*group)charge(b);
    auto base=DecodeNativeCheckpointRoot(base_bytes);CheckpointError(base.error);auto target=DecodeNativeCheckpointRoot(target_bytes);CheckpointError(target.error);
    auto encoded=DecodeNativePublicationPlan(plan_bytes);PlanError(encoded.error);const auto& p=*encoded.plan;const auto& a=*base.root;const auto& b=*target.root;
    Require(p.management_extent.has_value(),E::invalid_plan);
    const auto& extent=*p.management_extent;
    const u64 extent_budget=u64{extent.page_count}*p.header.page_size_bytes+4*u64{extent.aggregate_bytes}+2*u64{p.header.page_size_bytes};
    PlanError(BindNativePublicationPlanToManagementExtent(p,extent_bytes,extent_budget));
    Require(!(a.flags&4)&&!(b.flags&4),E::cluster_requires_authority);Require(a.completed&&b.completed&&!a.header.flags&&!b.header.flags,E::invalid_checkpoint);
    Require(SameFile(a.header,p.header)&&SameFile(b.header,p.header)&&p.base_checkpoint==Self(a.header)&&p.base_checkpoint_object_uuid==a.object_uuid&&p.base_checkpoint_sha256==Hash(base_bytes)&&p.base_checkpoint_generation==a.checkpoint_generation&&p.base_root_set_generation==a.root_set_generation&&p.timeline_uuid==a.timeline_uuid,E::binding_mismatch);
    Require(p.target_checkpoint==Self(b.header)&&p.target_checkpoint_object_uuid==b.object_uuid&&p.operation_uuid==b.creator_operation_uuid&&b.creator_transaction_uuid.is_nil()&&!b.creator_local_transaction_id&&p.reserved_generation==b.checkpoint_generation&&p.reserved_generation==b.header.page_generation&&p.target_root_set_generation==b.root_set_generation&&b.predecessor==p.base_checkpoint&&b.predecessor_sha256==p.base_checkpoint_sha256,E::binding_mismatch);
    const auto* head=Root(b,16);Require(head&&head->page_type==0x500&&head->page==Self(p.header)&&head->object_uuid==p.object_uuid&&head->sha256==encoded.sha256,E::binding_mismatch);
    const auto* old_head=Root(a,16);if(old_head)Require(p.previous_plan&&*p.previous_plan==old_head->page&&p.previous_plan_object_uuid==old_head->object_uuid&&p.previous_plan_sha256==old_head->sha256,E::binding_mismatch);else Require(!p.previous_plan,E::binding_mismatch);
    const auto graph=ComputeNativePublicationTargetGraphDigest(target_bytes);PlanError(graph.error);Require(graph.sha256==p.target_graph_sha256,E::binding_mismatch);
    Require(b.roots.size()==a.roots.size()+(old_head?0u:1u),E::invalid_delta);
    for(const auto& r:a.roots)if(r.role!=4&&r.role!=16){const auto* next=Root(b,r.role);Require(next&&r==*next,E::invalid_delta);}
    auto normalized=b;normalized.header=a.header;normalized.creator_transaction_uuid=a.creator_transaction_uuid;normalized.creator_local_transaction_id=a.creator_local_transaction_id;normalized.creator_operation_uuid=a.creator_operation_uuid;normalized.checkpoint_generation=a.checkpoint_generation;normalized.root_set_generation=a.root_set_generation;normalized.predecessor=a.predecessor;normalized.predecessor_sha256=a.predecessor_sha256;normalized.roots=a.roots;
    const auto unchanged=EncodeNativeCheckpointRoot(normalized);CheckpointError(unchanged.error);Require(unchanged.bytes==base_bytes,E::invalid_delta);
    const auto* old_root=Root(a,4);const auto* new_root=Root(b,4);Require(old_root&&new_root,E::invalid_allocation);
    const auto before=DecodeMaps(before_bytes,*old_root,p.header);const auto after=DecodeMaps(after_bytes,*new_root,p.header);
    Require(before.size()==after.size()&&before.front().total_pages==after.front().total_pages&&before.front().object_uuid==after.front().object_uuid&&before.front().capacity_generation==after.front().capacity_generation&&after.front().map_generation>before.front().map_generation,E::invalid_delta);
    Allocated(before,a.header,a.object_uuid);
    std::map<u64,Control> controls;std::set<Uuid> page_ids,old_allocations,old_pages;
    for(const auto& m:before)for(const auto& r:m.records){old_allocations.insert(r.allocation_uuid);if(!r.page_uuid.is_nil())old_pages.insert(r.page_uuid);}
    const auto add=[&](const H& h,const Uuid& owner){Require(SameFile(h,p.header)&&!h.flags&&h.page_generation==p.reserved_generation&&controls.emplace(h.page_number,Control{h,owner}).second&&page_ids.insert(h.page_uuid).second&&!old_pages.contains(h.page_uuid),E::invalid_delta);S state{};const auto* record=Record(before,h.page_number,&state);Require(!record&&state==S::free,E::invalid_delta);};
    add(b.header,b.object_uuid);add(p.header,p.object_uuid);
    for(const auto& raw:extent_bytes){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::invalid_extent);add(*h.header,p.management_extent->object_uuid);}
    for(const auto& m:after){Require(m.map_generation==p.reserved_generation&&m.creator_transaction_uuid.is_nil()&&!m.creator_local_transaction_id&&m.creator_operation_uuid==p.operation_uuid,E::invalid_delta);add(m.header,m.object_uuid);}
    std::size_t matched=0;
    for(std::size_t n=0;n<before.size();++n){const auto& x=before[n];const auto& y=after[n];Require(x.first_page==y.first_page&&x.states.size()==y.states.size(),E::invalid_delta);std::size_t xi=0,yi=0;
      for(std::size_t i=0;i<x.states.size();++i){const auto number=x.first_page+i;const auto* xr=xi<x.records.size()&&x.records[xi].page_number==number?&x.records[xi++]:nullptr;const auto* yr=yi<y.records.size()&&y.records[yi].page_number==number?&y.records[yi++]:nullptr;const auto found=controls.find(number);
        if(found==controls.end()){Require(x.states[i]==y.states[i]&&bool(xr)==bool(yr)&&(!xr||*xr==*yr),E::invalid_delta);continue;}
        const auto& c=found->second;Require(yr&&y.states[i]==S::allocated&&yr->page_uuid==c.header.page_uuid&&yr->page_generation==c.header.page_generation&&yr->page_type==c.header.page_type&&yr->owner_uuid==c.owner&&yr->creator_transaction_uuid.is_nil()&&!yr->creator_local_transaction_id&&yr->creator_operation_uuid==p.operation_uuid&&!yr->reuse_horizon&&!old_allocations.contains(yr->allocation_uuid),E::invalid_delta);++matched;
      }
    }
    Require(matched==controls.size(),E::invalid_delta);return E::none;
  }catch(E e){return e;}catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::invalid_request;}
}
} // namespace scratchbird::storage::database
