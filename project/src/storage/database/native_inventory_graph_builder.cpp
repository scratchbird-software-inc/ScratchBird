// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_inventory_graph_builder.hpp"
#include "native_metadata_decode_scratch.hpp"
#include "hash_digest_parts.hpp"
#include "native_decoded_storage_ranges.hpp"
#include <map>
#include <set>

namespace scratchbird::storage::database {
namespace {
using E=NativePublicationError;
using Image=std::span<const byte>;
using Scratch=detail::NativeMetadataScratch;
void Require(bool condition,E error){if(!condition)throw error;}
auto Ref(const disk::NativeCommonPageHeader& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
auto Hash(Image image){const auto hash=core::hash::ComputeSha256DigestNative(image.data(),image.size());Require(hash.ok(),E::hash_failure);return hash.digest;}
template<class T> void Codec(T error){
  if(error==T::none)return;
  throw error==T::resource_exhausted?E::resource_exhausted:error==T::hash_failure?E::hash_failure:E::image_failure;
}
bool Disjoint(const NativeInventoryGraphInputs& in,std::span<byte> backing){
  const auto safe=[&](auto region){return disk::detail::DisjointNativeDecodeRegions(backing,region);};
  const auto text=[&](std::string_view value){return safe(std::span{value.data(),value.size()});};
  if(!safe(std::span{&in,1})||!safe(std::span{&in.source,1})||!safe(std::span{&in.directory,1})||
      !safe(std::span{&in.primary,1})||!safe(std::span{&in.reservation,1})||!safe(std::span{&in.record,1})||
      !safe(std::span{&in.inventory,1})||!safe(in.primary.roots)||!safe(in.excluded_pages)||
      !safe(in.inventory.entries)||!safe(in.record.steps)||!safe(in.record.normalized_request_bytes)||!safe(in.record.operation_state_bytes)||
      !text(in.record.idempotency_key)||!safe(in.directory.pages)||!safe(in.source.allocation.pages))return false;
  for(const auto& s:in.record.steps)if(!text(s.idempotency_key))return false;
  for(const auto image:in.source.slots)if(!safe(image))return false;
  for(const auto* cp:{&in.source.checkpoint_inventory,&in.source.predecessor}){
    if(!safe(cp->inventory.entries)||!safe(cp->inventory_pages)||(cp->checkpoint&&!safe(cp->checkpoint->roots)))return false;
  }
  for(const auto& m:in.source.allocation.pages)if(!safe(m.image)||!safe(m.map.states)||!safe(m.map.records))return false;
  for(const auto& p:in.directory.pages)if(!safe(p.image)||!safe(p.directory.records))return false;
  return true;
}
struct MutableMap {
  NativeReadAllocationMap source;
  std::pmr::vector<page::NativeAllocationState> states;
  std::pmr::vector<page::NativeAllocationRecord> records;
  MutableMap(const NativeReadAllocationMap& base,u64 first,u64 count,std::pmr::memory_resource* memory)
      :source(base),states(memory),records(memory){
    source.first_page=first;
    const auto offset=first-base.first_page;
    states.assign(base.states.begin()+offset,base.states.begin()+offset+count);
    const auto begin=std::lower_bound(base.records.begin(),base.records.end(),first,[](const auto& r,u64 n){return r.page_number<n;});
    const auto end=std::lower_bound(begin,base.records.end(),first+count,[](const auto& r,u64 n){return r.page_number<n;});
    records.assign(begin,end);
  }
  page::NativeAllocationMapConstView View() const {
    const auto& s=source;
    return {s.header,s.object_uuid,s.map_generation,s.capacity_generation,s.total_pages,s.first_page,
      s.creator_transaction_uuid,s.creator_local_transaction_id,s.next,s.next_sha256,states,records,s.creator_operation_uuid};
  }
};
}
NativeInventoryGraphBuildResult BuildNativeInventoryGraphInto(const NativeInventoryGraphInputs& in,
    core::uuid::StandaloneUuidV7Issuer& issuer,std::span<byte> backing) noexcept {
  NativeInventoryGraphBuildResult out;
  if(!Disjoint(in,backing)||!disk::detail::DisjointNativeDecodeRegions(backing,std::span{&issuer,1}))return out;
  try{
    Require(in.source.ok()&&in.directory.ok()&&in.maximum_image_bytes,E::invalid_request);
    const auto& zero=in.primary;const auto& reserved=in.reservation;
    const auto& watermark=reserved.watermark;const auto& record=in.record;
    const auto& base=*in.source.checkpoint_inventory.checkpoint;
    const auto& selected=*in.source.selection;
    const auto database=zero.bootstrap.database_uuid,primary=zero.bootstrap.filespace_uuid;
    const auto profile=zero.bootstrap.page_size_profile_uuid;const u64 size=zero.bootstrap.page_size_bytes,payload=size-384;
    Require(disk::FindCanonicalFilespacePageProfile(profile)&&size==disk::FindCanonicalFilespacePageProfile(profile)->page_size_bytes,E::invalid_request);
    Require(zero.bootstrap.filespace_role<=4&&!(zero.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)&&
        !(zero.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::invalid_request);
    Require(watermark.intent&&watermark.intent->recovery_profile==2&&!watermark.abandonment&&!watermark.publication_plan&&
        watermark.watermark>selected.checkpoint_generation,E::operation_pending);
    const auto& intent=*watermark.intent;
    Require(base.header.database_uuid==database&&base.header.filespace_uuid==primary&&base.header.page_size_profile_uuid==profile&&
        watermark.header.database_uuid==database&&watermark.header.filespace_uuid==primary&&watermark.bootstrap_uuid==zero.page_uuid&&
        watermark.base_checkpoint==selected.checkpoint&&watermark.base_checkpoint_sha256==selected.checkpoint_sha256&&
        reserved.selection.checkpoint==selected.checkpoint&&reserved.selection.checkpoint_sha256==selected.checkpoint_sha256&&
        reserved.selection.selection_generation==selected.selection_generation&&
        selected.selection_generation<UINT64_MAX,E::binding_mismatch);
    Require(record.database_uuid==database&&record.bootstrap_uuid==zero.page_uuid&&
        record.initiator_uuid==intent.initiator_uuid&&record.initiator_kind==intent.initiator_kind&&
        record.request_context_uuid==intent.request_context_uuid&&record.policy_snapshot_uuid==intent.policy_snapshot_uuid&&
        record.normalized_request_sha256==intent.normalized_request_sha256&&
        issuer.binding().database_uuid==database&&issuer.binding().policy_snapshot_uuid==record.policy_snapshot_uuid,E::request_mismatch);
    Require(record.scope!=NativeManagementScope::cluster&&record.cluster_uuid.is_nil()&&!record.generation_guards[3],E::cluster_requires_authority);
    detail::NativeMetadataMemory memory(backing);Scratch scratch{&memory};
    u64 image_bytes=0;
    const auto image=[&](u64 bytes){Require(bytes<=in.maximum_image_bytes-image_bytes&&bytes<=SIZE_MAX,E::resource_exhausted);
      image_bytes+=bytes;return scratch.Array<byte>(bytes);};
    u64 aggregate_size=512;
    const auto add=[&](u64 bytes){Require(bytes<=in.maximum_image_bytes-std::min(aggregate_size,in.maximum_image_bytes),E::resource_exhausted);aggregate_size+=bytes;};
    add(record.idempotency_key.size());add(record.normalized_request_bytes.size());
    if(!record.operation_state_bytes.empty()){add(4);add(record.operation_state_bytes.size());}
    for(const auto& step:record.steps){add(256);add(step.idempotency_key.size());}
    auto aggregate=scratch.Array<byte>(aggregate_size);
    Require(record.steps.size()<=(SIZE_MAX-7)/2,E::resource_exhausted);
    const auto encoded_record=EncodeNativeManagementOperationInto(record,in.maximum_image_bytes,aggregate,scratch.Array<Uuid>(record.steps.size()*2+7));
    Codec(encoded_record.error);
    const auto entries=scratch.Copy(in.inventory.entries);
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.identity.local_id.value<b.identity.local_id.value;});
    const page::NativeTransactionInventoryView after{in.inventory.next_local_transaction_id,in.inventory.next_commit_sequence,entries};
    const auto& old=in.source.checkpoint_inventory.inventory;
    const page::NativeTransactionInventoryView before{old.next_local_transaction_id,old.next_commit_sequence,scratch.Copy(old.entries)};
    const auto count=std::max(before.entries.size(),after.entries.size());
    Codec(page::ValidateNativeTransactionInventoryEvolutionView(before,after,scratch.Array<std::size_t>(count),
        scratch.Array<std::size_t>(before.entries.size()),scratch.Array<byte>(count)).error);
    const auto root=[&](u16 role)->const NativeCheckpointRootReference&{
      const auto at=std::find_if(base.roots.begin(),base.roots.end(),[&](const auto& r){return r.role==role;});
      Require(at!=base.roots.end(),E::binding_mismatch);return *at;
    };
    const auto& inventory_root=root(1);const auto& directory_root=root(3);
    const auto prior_plan=std::find_if(base.roots.begin(),base.roots.end(),[](const auto& r){return r.role==16;});
    std::pmr::vector<page::NativeFilespaceDirectoryRecord> directory(&memory);
    for(const auto& p:in.directory.pages)directory.insert(directory.end(),p.directory.records.begin(),p.directory.records.end());
    const auto primary_member=std::find_if(directory.begin(),directory.end(),[&](const auto& d){return d.bootstrap.filespace_uuid==primary;});
    Require(primary_member!=directory.end(),E::binding_mismatch);
    const bool publish_directory=directory.size()>1||primary_member->allocation_root.has_value();
    const auto ceil=[](u64 n,u64 d){return n/d+(n%d!=0);};
    const u64 per_inventory=payload/72,inventory_count=std::max<u64>(1,ceil(entries.size(),per_inventory));
    const u64 extent_count=ceil(aggregate_size,payload),directory_count=publish_directory?ceil(directory.size(),payload/320):0;
    struct Partition{std::size_t source;u64 first,count;};
    std::pmr::vector<Partition> partitions(&memory);
    for(std::size_t i=0;i<in.source.allocation.pages.size();++i){const auto& m=in.source.allocation.pages[i].map;
      Require(m.header.filespace_uuid==primary&&m.header.page_size_profile_uuid==profile,E::binding_mismatch);
      partitions.push_back({i,m.first_page,m.states.size()});}
    std::pmr::set<u64> selected_slots(&memory),excluded(&memory);excluded.insert(in.excluded_pages.begin(),in.excluded_pages.end());
    const auto allocate=[&](u64 n){
      Require(n&&n<=zero.total_pages,E::allocation_exhausted);
      std::pmr::vector<u64> run(&memory);run.reserve(n);
      for(const auto& p:in.source.allocation.pages){const auto& m=p.map;
        for(std::size_t i=0;i<m.states.size();++i){const auto number=m.first_page+i;
          if(!number||m.states[i]!=page::NativeAllocationState::free||selected_slots.contains(number)||excluded.contains(number)){
            run.clear();continue;}
          if(!run.empty()&&number-run.back()!=1)run.clear();run.push_back(number);
          if(run.size()==n){selected_slots.insert(run.begin(),run.end());return run;}
        }}throw E::allocation_exhausted;
    };
    std::pmr::vector<u64> extent_slots(&memory),bundle_slots(&memory),map_slots(&memory),inventory_slots(&memory),directory_slots(&memory);
    u64 plan_slot=0,checkpoint_slot=0,bundle_count=0;
    for(;;){
      Require(partitions.size()<=UINT64_MAX/(size+8)-inventory_count-directory_count,E::resource_exhausted);
      bundle_count=ceil((partitions.size()+inventory_count+directory_count)*(publish_directory?size+8:size),payload);
      selected_slots.clear();map_slots.clear();inventory_slots.clear();directory_slots.clear();
      extent_slots=allocate(extent_count);bundle_slots=allocate(bundle_count);
      plan_slot=allocate(1).front();checkpoint_slot=allocate(1).front();
      for(std::size_t i=0;i<partitions.size();++i)map_slots.push_back(allocate(1).front());
      for(u64 i=0;i<inventory_count;++i)inventory_slots.push_back(allocate(1).front());
      for(u64 i=0;i<directory_count;++i)directory_slots.push_back(allocate(1).front());
      std::pmr::vector<Partition> refined(&memory);
      for(const auto& part:partitions){const auto& m=in.source.allocation.pages[part.source].map;
        const u64 records_at=(384+(part.count+1)/2+7)&~u64{7};
        u64 records=0;auto old_record=std::lower_bound(m.records.begin(),m.records.end(),part.first,[](const auto& r,u64 n){return r.page_number<n;});
        for(u64 n=part.first;n-part.first<part.count;++n){
          const bool exists=old_record!=m.records.end()&&old_record->page_number==n;
          if(exists||selected_slots.contains(n))++records;if(exists)++old_record;
        }
        if(records_at>size||records>(size-records_at)/128){
          Require(part.count>1,E::resource_exhausted);const auto half=part.count/2;
          refined.push_back({part.source,part.first,half});refined.push_back({part.source,part.first+half,part.count-half});
        }else refined.push_back(part);
      }
      if(refined.size()==partitions.size())break;
      partitions=std::move(refined);
    }
    std::pmr::set<Uuid> identities(&memory);
    const auto retain=[&](const Uuid& id){if(!id.is_nil())identities.insert(id);};
    for(const auto& id:{database,primary,profile,zero.page_uuid,zero.creation_operation_uuid,zero.writer_identity_uuid,
        watermark.operation_uuid,base.object_uuid,base.timeline_uuid,record.uuid,record.descriptor_uuid,record.family_uuid,
        record.target_type_uuid,record.target_uuid,record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,
        record.security_snapshot_uuid,record.phase_uuid,record.boundary_uuid,record.created_at,record.updated_at,record.terminal_at,
        record.resource_plan_uuid,record.lock_plan_uuid,record.result_uuid,record.diagnostic_uuid,record.evidence_uuid,record.metric_evidence_uuid})retain(id);
    for(const auto& r:base.roots){retain(r.object_uuid);retain(r.page.filespace_uuid);retain(r.page.page_size_profile_uuid);}
    for(const auto& p:in.source.allocation.pages)for(const auto& r:p.map.records)
      for(const auto& id:{r.allocation_uuid,r.page_uuid,r.owner_uuid,r.creator_transaction_uuid,r.creator_operation_uuid})retain(id);
    for(const auto& entry:before.entries)retain(entry.identity.transaction_uuid.value);
    for(const auto& entry:after.entries)retain(entry.identity.transaction_uuid.value);
    for(const auto& p:in.source.checkpoint_inventory.inventory_pages){retain(p.header.page_uuid);retain(p.object_uuid);}
    for(const auto& p:in.directory.pages){retain(p.directory.header.page_uuid);retain(p.directory.object_uuid);
      retain(p.directory.creator_operation_uuid);retain(p.directory.creator_transaction_uuid);}
    for(const auto& member:directory){retain(member.bootstrap.filespace_uuid);retain(member.bootstrap.page_size_profile_uuid);
      retain(member.locator_uuid);retain(member.page_zero_uuid);if(member.allocation_root)retain(member.allocation_root->object_uuid);}
    for(const auto& step:record.steps)for(const auto& id:{step.uuid,step.operation_uuid,step.family_uuid,step.target_uuid,
        step.started_at,step.completed_at,step.evidence_uuid,step.metric_evidence_uuid,step.diagnostic_uuid,step.boundary_uuid})retain(id);
    const auto issue=[&](core::platform::UuidKind kind){const auto result=issuer.Issue(kind);
      Require(result.error!=core::uuid::StandaloneUuidV7Error::resource_exhausted,E::resource_exhausted);
      Require(result.ok()&&identities.insert(result.value->value).second,E::identity_failure);return result.value->value;};
    using core::platform::UuidKind;
    const auto plan_object=prior_plan==base.roots.end()?issue(UuidKind::object):prior_plan->object_uuid;
    const auto extent_object=issue(UuidKind::object),bundle_object=issue(UuidKind::object);
    std::pmr::vector<MutableMap> maps(&memory);maps.reserve(partitions.size());
    for(const auto& p:partitions)maps.emplace_back(in.source.allocation.pages[p.source].map,p.first,p.count,&memory);
    const auto control=[&](u64 n,u32 type,const Uuid& owner){
      disk::NativeCommonPageHeader h{u32(size),type,database,primary,issue(UuidKind::page),n,watermark.watermark,0,profile};
      const auto found=std::find_if(maps.begin(),maps.end(),[&](const auto& m){return n>=m.source.first_page&&n-m.source.first_page<m.states.size();});
      Require(found!=maps.end(),E::allocation_mismatch);auto& m=*found;
      Require(m.states[n-m.source.first_page]==page::NativeAllocationState::free,E::allocation_mismatch);
      const auto at=std::lower_bound(m.records.begin(),m.records.end(),n,[](const auto& r,u64 page){return r.page_number<page;});
      Require(at==m.records.end()||at->page_number!=n,E::allocation_mismatch);
      page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=issue(UuidKind::object);r.page_uuid=h.page_uuid;
      r.owner_uuid=owner;r.page_generation=h.page_generation;r.page_type=type;r.creator_operation_uuid=watermark.operation_uuid;
      m.records.insert(at,r);m.states[n-m.source.first_page]=page::NativeAllocationState::allocated;return h;
    };
    NativeInventoryGraphView graph;auto& plan=graph.plan;
    plan.header=control(plan_slot,0x500,plan_object);
    NativeCheckpointRootView target{base.header,base.object_uuid,base.checkpoint_generation,base.root_set_generation,
      base.selected_local_transaction_id,base.stable_local_transaction_id,base.local_durable_transaction_id,
      base.cluster_quorum_transaction_id,base.timeline_uuid,base.creator_transaction_uuid,base.creator_local_transaction_id,
      base.flags,base.predecessor,base.predecessor_sha256,base.completed,{},base.creator_operation_uuid};
    target.header=control(checkpoint_slot,0x300,base.object_uuid);
    const auto headers=[&](const auto& slots,u32 type,const Uuid& object){auto result=scratch.Array<disk::NativeCommonPageHeader>(slots.size());
      for(std::size_t i=0;i<slots.size();++i)result[i]=control(slots[i],type,object);return result;};
    const auto extent_headers=headers(extent_slots,0x500,extent_object),bundle_headers=headers(bundle_slots,0x500,bundle_object);
    for(std::size_t i=0;i<maps.size();++i)maps[i].source.header=control(map_slots[i],3,maps[i].source.object_uuid);
    const auto inventory_headers=headers(inventory_slots,0x301,inventory_root.object_uuid),directory_headers=headers(directory_slots,9,directory_root.object_uuid);
    auto map_images=scratch.Array<Image>(maps.size()),inventory_images=scratch.Array<Image>(inventory_count),directory_images=scratch.Array<Image>(directory_count);
    for(std::size_t i=maps.size();i--;){auto& m=maps[i];m.source.map_generation=watermark.watermark;
      m.source.creator_transaction_uuid={};m.source.creator_local_transaction_id=0;m.source.creator_operation_uuid=watermark.operation_uuid;
      m.source.next=i+1<maps.size()?std::optional{Ref(maps[i+1].source.header)}:std::nullopt;
      m.source.next_sha256=i+1<maps.size()?Hash(map_images[i+1]):std::array<byte,32>{};
      auto raw=image(size);Codec(page::EncodeNativeAllocationMapInto(m.View(),raw).error);map_images[i]=raw;
    }
    auto indices=scratch.Array<std::size_t>(std::min<u64>(per_inventory,entries.size()));auto markers=scratch.Array<byte>(indices.size());
    for(std::size_t i=0;i<inventory_images.size();++i){page::NativeTransactionInventoryPageConstView p;
      p.header=inventory_headers[i];p.object_uuid=inventory_root.object_uuid;p.inventory_generation=watermark.watermark;
      p.previous=i?std::optional{Ref(inventory_headers[i-1])}:std::nullopt;p.next=i+1<inventory_count?std::optional{Ref(inventory_headers[i+1])}:std::nullopt;
      const auto first=std::min<u64>(i*per_inventory,entries.size());p.inventory={after.next_local_transaction_id,after.next_commit_sequence,
        std::span<const transaction::mga::TransactionInventoryEntry>(entries).subspan(first,std::min<u64>(per_inventory,entries.size()-first))};
      auto raw=image(size);Codec(page::EncodeNativeTransactionInventoryPageInto(p,raw,indices,markers).error);inventory_images[i]=raw;
    }
    for(auto& member:directory)if(member.bootstrap.filespace_uuid==primary)
      member.allocation_root=page::NativeFilespaceAllocationRoot{Ref(maps.front().source.header),maps.front().source.object_uuid,
        Hash(map_images.front()),maps.front().source.map_generation,maps.front().source.capacity_generation};
    const u64 per_directory=payload/320;auto directory_ids=scratch.Array<Uuid>(std::min<u64>(per_directory,directory.size()));
    for(std::size_t i=directory_count;i--;){page::NativeFilespaceDirectoryConstView p;
      p.header=directory_headers[i];p.object_uuid=directory_root.object_uuid;p.directory_generation=watermark.watermark;
      p.creator_operation_uuid=watermark.operation_uuid;p.total_records=directory.size();p.first_record=i*per_directory;
      p.records=std::span<const page::NativeFilespaceDirectoryRecord>(directory).subspan(p.first_record,std::min<u64>(per_directory,directory.size()-p.first_record));
      if(i+1<directory_count){p.next=Ref(directory_headers[i+1]);p.next_sha256=Hash(directory_images[i+1]);}
      auto raw=image(size);Codec(page::EncodeNativeFilespaceDirectoryInto(p,raw,directory_ids).error);directory_images[i]=raw;
    }
    auto extent_bytes=image(extent_count*size);
    const auto extent=EncodeNativeManagementExtentInto(record,extent_object,extent_headers,in.maximum_image_bytes,extent_bytes,
      aggregate,scratch.Array<Uuid>(std::max(extent_count*2+1,record.steps.size()*2+7)));
    Codec(extent.error);
    auto bundle_bytes=image(bundle_count*size);
    auto target_roots=scratch.Array<NativeCheckpointRootReference>(base.roots.size()+(prior_plan==base.roots.end()));
    auto checkpoint_bytes=image(size),plan_bytes=image(size);auto projection_roots=scratch.Array<NativeCheckpointRootReference>(16);
    auto extent_views=scratch.Array<Image>(extent_count),bundle_views=scratch.Array<Image>(bundle_count);
    // Metadata codec scratch is separate from every retained graph byte and
    // descriptor. Its bounded temporary allocation is reclaimed on return.
    const auto bundle_backing=scratch.Array<byte>(std::min<u64>(in.maximum_image_bytes,backing.size()-memory.used()));
    const auto bundle=EncodeNativeManagementControlBundleInto(map_images,database,zero.page_uuid,bundle_object,
      watermark.operation_uuid,bundle_headers,in.maximum_image_bytes,bundle_bytes,bundle_backing,inventory_images,directory_images);
    Codec(bundle.error);
    plan.object_uuid=plan_object;plan.bootstrap_uuid=zero.page_uuid;plan.timeline_uuid=base.timeline_uuid;
    plan.operation_uuid=watermark.operation_uuid;plan.security_snapshot_uuid=record.security_snapshot_uuid;plan.intent=intent;
    plan.reservation_state_sha256=reserved.state_sha256;plan.base_checkpoint=selected.checkpoint;
    plan.base_checkpoint_object_uuid=base.object_uuid;plan.base_checkpoint_sha256=selected.checkpoint_sha256;
    plan.base_checkpoint_generation=base.checkpoint_generation;plan.base_root_set_generation=base.root_set_generation;
    plan.base_selection_generation=selected.selection_generation;plan.target_checkpoint=Ref(target.header);plan.target_checkpoint_object_uuid=base.object_uuid;
    plan.reserved_generation=plan.target_root_set_generation=watermark.watermark;
    if(prior_plan!=base.roots.end()){plan.previous_plan=prior_plan->page;plan.previous_plan_object_uuid=prior_plan->object_uuid;plan.previous_plan_sha256=prior_plan->sha256;}
    plan.generation_guard_flags=0;for(unsigned i=0;i<3;++i)if(record.generation_guards[i])plan.generation_guard_flags|=1u<<i;
    plan.catalog_generation=record.generation_guards[0].value_or(0);plan.configuration_generation=record.generation_guards[1].value_or(0);
    plan.security_generation=record.generation_guards[2].value_or(0);plan.management_extent=extent.root;plan.control_bundle=bundle.root;
    target.creator_transaction_uuid={};target.creator_local_transaction_id=0;target.creator_operation_uuid=watermark.operation_uuid;
    target.checkpoint_generation=target.root_set_generation=watermark.watermark;target.predecessor=plan.base_checkpoint;target.predecessor_sha256=plan.base_checkpoint_sha256;
    target.selected_local_transaction_id=after.next_local_transaction_id-1;
    target.roots=target_roots;
    std::copy(base.roots.begin(),base.roots.end(),target.roots.begin());
    for(auto& r:target.roots){
      if(r.role==1)r={1,0x301,Ref(inventory_headers.front()),inventory_root.object_uuid,Hash(inventory_images.front())};
      if(r.role==4)r={4,3,Ref(maps.front().source.header),maps.front().source.object_uuid,Hash(map_images.front())};
      if(r.role==3&&publish_directory)r={3,9,Ref(directory_headers.front()),directory_root.object_uuid,Hash(directory_images.front())};
    }
    auto at=prior_plan==base.roots.end()?target.roots.end()-1:
      std::find_if(target.roots.begin(),target.roots.end(),[](const auto& r){return r.role==16;});
    *at={16,0x500,Ref(plan.header),plan.object_uuid,{}};at->sha256.fill(1); // Digest excluded from projection; never installed.
    Codec(EncodeNativeCheckpointRootInto(target,checkpoint_bytes).error);
    const auto projection=ComputeNativePublicationTargetGraphDigestInto(checkpoint_bytes,projection_roots);Codec(projection.error);plan.target_graph_sha256=projection.sha256;
    const auto encoded_plan=EncodeNativePublicationPlanInto(plan,plan_bytes);Codec(encoded_plan.error);at->sha256=encoded_plan.sha256;
    Codec(EncodeNativeCheckpointRootInto(target,checkpoint_bytes).error);
    for(u64 i=0;i<extent_count;++i)extent_views[i]=std::span<const byte>(extent_bytes).subspan(i*size,size);
    for(u64 i=0;i<bundle_count;++i)bundle_views[i]=std::span<const byte>(bundle_bytes).subspan(i*size,size);
    graph.plan_image=plan_bytes;graph.checkpoint=checkpoint_bytes;graph.extent=extent_views;graph.bundle=bundle_views;
    graph.allocation=map_images;graph.inventory=inventory_images;graph.directory=directory_images;
    out.graph=graph;out.backing_bytes_used=memory.used();out.error=E::none;
  }catch(E error){out.error=error;}
   catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
  return out;
}
} // namespace scratchbird::storage::database
