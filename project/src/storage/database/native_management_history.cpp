// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_history.hpp"
#include "native_filespace_directory.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

namespace scratchbird::storage::database {
namespace {
using E=NativeManagementHistoryError;
using namespace core::platform;
void Require(bool ok,E error){if(!ok)throw error;}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
bool SameBootstrap(const disk::FilespaceBootstrap& a,const disk::FilespaceBootstrap& b){
  return std::tie(a.database_uuid,a.filespace_uuid,a.page_size_profile_uuid,a.checksum_profile_uuid,a.encryption_profile_uuid,
    a.page_size_bytes,a.durable_format_generation,a.flags,a.filespace_role,a.lifecycle_state)==
    std::tie(b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,b.checksum_profile_uuid,b.encryption_profile_uuid,
    b.page_size_bytes,b.durable_format_generation,b.flags,b.filespace_role,b.lifecycle_state);
}
NativeManagementHistory Fail(E error){NativeManagementHistory r;r.error=error;return r;}
auto Hash(const std::vector<byte>& b){const auto hash=core::hash::ComputeSha256Digest(b);Require(hash.ok(),E::hash_failure);return hash.digest;}
disk::NativePageReference Self(const NativeCheckpointRoot& r){const auto& h=r.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
disk::NativePageReference Self(const NativePublicationPlan& r){const auto& h=r.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
void PlanError(NativePublicationPlanError e){if(e==NativePublicationPlanError::none)return;
  throw e==NativePublicationPlanError::resource_exhausted?E::resource_exhausted:e==NativePublicationPlanError::hash_failure?E::hash_failure:e==NativePublicationPlanError::cluster_requires_authority?E::cluster_requires_authority:E::plan_failure;}
void BootstrapError(disk::FilespacePageZeroError e){throw e==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:e==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:e==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;}
const NativeCheckpointRootReference* Head(const NativeCheckpointRoot& cp){const auto it=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;});return it==cp.roots.end()?nullptr:&*it;}
bool SameHead(const NativeCheckpointRootReference* a,const NativeCheckpointRootReference* b){return (!a||!b)?a==b:a->page==b->page&&a->object_uuid==b->object_uuid&&a->sha256==b->sha256&&a->page_type==b->page_type;}
struct Checkpoint {NativeCheckpointRoot root;std::vector<byte> bytes;std::array<byte,32> sha{};};
struct Context {
  Uuid database,primary,timeline;
  u64 budget=0,used=0;
  std::vector<disk::NativeFilespaceDevice> devices;
  std::vector<std::unique_lock<std::recursive_mutex>> guards;
  std::map<Uuid,disk::FilespacePageZero> zeros;
  std::map<Uuid,std::vector<byte>> zero_images;
  std::map<Uuid,u64> physical_sizes;
  bool historical=false;
  void Charge(u64 n){Require(n<=budget-used,E::resource_exhausted);used+=n;}
  const disk::NativeFilespaceDevice& File(const Uuid& fs,const Uuid& profile){
    const auto it=std::lower_bound(devices.begin(),devices.end(),fs,[](const auto& a,const auto& id){return a.filespace_uuid<id;});
    Require(it!=devices.end()&&it->filespace_uuid==fs&&it->page_size_profile_uuid==profile,E::invalid_request);return *it;
  }
  void HistoricalContexts(){if(!zero_images.empty())return;
    for(const auto& [id,z]:zeros){Charge(z.bootstrap.page_size_bytes);auto encoded=disk::EncodeFilespacePageZero(z);
      if(!encoded.ok())BootstrapError(encoded.error);zero_images.emplace(id,std::move(*encoded.bytes));}}
  void Bound(const NativeCheckpointRoot& cp){
    const auto bound=[&](const disk::NativePageReference& r){const auto at=zeros.find(r.filespace_uuid);
      if(at!=zeros.end())Require(r.page_size_profile_uuid==at->second.bootstrap.page_size_profile_uuid&&r.page_number<at->second.total_pages,E::history_mismatch);};
    bound(Self(cp));if(cp.predecessor)bound(*cp.predecessor);for(const auto& root:cp.roots)bound(root.page);
  }
  Checkpoint Read(const disk::NativePageReference& ref,const Uuid& object,const std::array<byte,32>& expected){
    const auto& file=File(ref.filespace_uuid,ref.page_size_profile_uuid);const auto* profile=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
    Require(profile,E::invalid_request);Charge(3*u64{profile->page_size_bytes});
    const disk::FilespaceRootReference root{9,0x300,ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid,object};
    NativeCheckpointRootResult r;
    if(historical){Require(ref.page_number<zeros.at(ref.filespace_uuid).total_pages,E::history_mismatch);std::vector<byte> raw(profile->page_size_bytes);
      const auto io=file.device->ReadAt(ref.page_number*u64{profile->page_size_bytes},raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);
      const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::checkpoint_failure);Require(!(h.header->flags&1u),E::encrypted_requires_authority);
      r=DecodeNativeCheckpointRoot(raw);
      if(r.ok())Require(r.root->header.database_uuid==database&&Self(*r.root)==ref&&r.root->object_uuid==object,E::history_mismatch);
    }else r=ReadNativeCheckpointRootFromOpenDevice(*file.device,database,root);
    if(!r.ok())throw r.error==NativeCheckpointError::resource_exhausted?E::resource_exhausted:r.error==NativeCheckpointError::hash_failure?E::hash_failure:r.error==NativeCheckpointError::io_failure?E::io_failure:r.error==NativeCheckpointError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:E::checkpoint_failure;
    Require(r.root->completed&&r.root->timeline_uuid==timeline,E::history_mismatch);Require(!(r.root->flags&4),E::cluster_requires_authority);
    Bound(*r.root);const auto hash=Hash(r.bytes);Require(hash==expected,E::history_mismatch);return {std::move(*r.root),std::move(r.bytes),hash};
  }
  Checkpoint Parent(const Checkpoint& current){const auto& r=current.root;Require(r.predecessor.has_value(),E::history_mismatch);
    auto p=Read(*r.predecessor,r.object_uuid,r.predecessor_sha256);
    Require(p.root.checkpoint_generation<r.checkpoint_generation&&p.root.root_set_generation<r.root_set_generation,E::history_mismatch);return p;
  }
  NativeManagementExtentRead Extent(const disk::NativeFilespaceDevice& file,const NativeManagementExtentRoot& r,const Uuid& bootstrap,u64 allowance){
    const auto valid=ValidateNativeManagementExtentRoot(r,database,bootstrap,allowance);
    if(valid!=NativeManagementExtentError::none)return {valid,{},{}};
    const auto& z=zeros.at(file.filespace_uuid);Require(r.first.filespace_uuid==file.filespace_uuid&&r.first.page_size_profile_uuid==file.page_size_profile_uuid&&
      r.first.page_number<z.total_pages&&r.page_count<=z.total_pages-r.first.page_number,E::history_mismatch);
    Require(z.page_uuid==bootstrap&&std::any_of(z.roots.begin(),z.roots.end(),[](const auto& root){return root.kind==20;})&&
      std::any_of(z.roots.begin(),z.roots.end(),[](const auto& root){return root.kind==21;}),E::bootstrap_failure);
    for(const auto& root:z.roots)if(root.filespace_uuid==file.filespace_uuid)Require(root.page_number<r.first.page_number||root.page_number-r.first.page_number>=r.page_count,E::history_mismatch);
    if(!historical)return ReadNativeManagementExtentFromOpenDevice(file,r,database,bootstrap,allowance);
    std::vector<std::vector<byte>> pages(r.page_count);
    for(u64 n=0;n<r.page_count;++n){auto& raw=pages[n];raw.resize(z.bootstrap.page_size_bytes);
      const auto io=file.device->ReadAt((r.first.page_number+n)*u64{z.bootstrap.page_size_bytes},raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);}
    return DecodeNativeManagementExtent(pages,r,database,bootstrap,allowance);
  }
  void ObserveHistorical(const disk::NativeFilespaceDevice& file,const std::vector<byte>& image,bool final){
    const auto& z=zeros.at(file.filespace_uuid);const auto size=file.device->Size();Require(size.ok(),E::io_failure);
    if(final)Require(size.size_bytes==physical_sizes.at(file.filespace_uuid),E::history_mismatch);
    else{Require(size.size_bytes>=z.total_pages*u64{z.bootstrap.page_size_bytes},E::history_mismatch);physical_sizes.emplace(file.filespace_uuid,size.size_bytes);}
    std::vector<byte> actual(z.bootstrap.page_size_bytes);const auto io=file.device->ReadAt(0,actual.data(),actual.size());Require(io.ok()&&io.bytes_transferred==actual.size(),E::io_failure);
    Require(std::equal(image.begin(),image.begin()+4096,actual.begin())&&std::equal(image.begin()+4480,image.end(),actual.begin()+4480),E::history_mismatch);
  }
};
void Index(const std::vector<NativeManagementHistoryEntry>& entries,
    std::map<Uuid,std::size_t>& latest,std::map<std::pair<u16,std::string>,Uuid>& idempotency,
    const NativeManagementOperation* append=nullptr){
  using Semantic=std::tuple<Uuid,u16,Uuid,Uuid,Uuid,Uuid,Uuid,std::optional<u64>,std::array<byte,32>>;
  std::map<Semantic,Uuid> semantics;std::map<Uuid,Uuid> steps;
  for(std::size_t i=0;i<entries.size()+(append?1:0);++i){const auto& o=i==entries.size()?*append:entries[i].record;
    const auto old=latest.find(o.uuid);
    if(old==latest.end())Require(o.revision==1&&o.state==NativeManagementState::created,E::transition_failure);
    else{const auto e=ValidateNativeManagementOperationEvolution(entries[old->second].record,o);
      if(e==NativeManagementOperationError::hash_failure)throw E::hash_failure;
      if(e==NativeManagementOperationError::resource_exhausted)throw E::resource_exhausted;
      Require(e==NativeManagementOperationError::none,E::transition_failure);}
    latest[o.uuid]=i;
    const auto key=std::make_pair(u16(o.scope),o.idempotency_key);const auto [k,added]=idempotency.emplace(key,o.uuid);(void)added;Require(k->second==o.uuid,E::idempotency_conflict);
    const Semantic semantic{o.descriptor_uuid,u16(o.scope),o.target_type_uuid,o.target_uuid,o.initiator_uuid,o.request_context_uuid,o.policy_snapshot_uuid,o.generation_guards[3],o.normalized_request_sha256};
    const auto [s,inserted]=semantics.emplace(semantic,o.uuid);(void)inserted;Require(s->second==o.uuid,E::idempotency_conflict);
    for(const auto& step:o.steps){const auto [entry,fresh]=steps.emplace(step.uuid,o.uuid);(void)fresh;Require(entry->second==o.uuid,E::history_mismatch);}
  }
  for(const auto& [step,owner]:steps){(void)owner;Require(!latest.contains(step),E::history_mismatch);}
}
NativeManagementHistory ReadHistory(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& supplied,const Uuid& primary,u64 budget,const NativeManagementCheckpointAnchor* requested,
    const std::map<Uuid,std::vector<byte>>* historical_context=nullptr) noexcept {
  try{
    std::optional<NativeManagementCheckpointAnchor> explicit_anchor;
    if(requested){explicit_anchor=*requested;const auto& a=*explicit_anchor;
      Require(V7(a.checkpoint.filespace_uuid)&&a.checkpoint.page_number&&a.checkpoint.page_generation&&disk::FindCanonicalFilespacePageProfile(a.checkpoint.page_size_profile_uuid)&&
        V7(a.checkpoint_object_uuid)&&V7(a.timeline_uuid)&&a.checkpoint_generation&&a.root_set_generation&&
        std::any_of(a.checkpoint_sha256.begin(),a.checkpoint_sha256.end(),[](byte v){return v!=0;}),E::invalid_request);}
    Require(V7(database)&&V7(primary)&&!supplied.empty(),E::invalid_request);Context c;c.database=database;c.primary=primary;c.budget=budget;c.devices=supplied;
    c.historical=historical_context!=nullptr;Require(!c.historical||(requested&&historical_context->size()==supplied.size()),E::invalid_request);
    std::sort(c.devices.begin(),c.devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});std::set<disk::FileDevice*> handles;c.guards.reserve(c.devices.size());
    for(std::size_t i=0;i<c.devices.size();++i){const auto& f=c.devices[i];const auto* profile=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);
      Require(V7(f.filespace_uuid)&&profile&&f.device&&handles.insert(f.device).second&&(!i||c.devices[i-1].filespace_uuid!=f.filespace_uuid),E::invalid_request);
      c.guards.push_back(f.device->AcquireOperationGuard());Require(f.device->is_open(),E::invalid_request);}
    for(const auto& f:c.devices){const auto* profile=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);c.Charge(2*u64{profile->page_size_bytes});
      const disk::FilespaceBootstrapBinding binding{database,f.filespace_uuid,f.page_size_profile_uuid};disk::FilespacePageZeroDecodeResult zero;
      if(historical_context){const auto at=historical_context->find(f.filespace_uuid);Require(at!=historical_context->end(),E::invalid_request);c.Charge(profile->page_size_bytes);
        zero=disk::DecodeFilespacePageZero(at->second.data(),at->second.size(),&binding);if(zero.ok())c.zero_images.emplace(f.filespace_uuid,at->second);
      }else zero=disk::ReadFilespacePageZeroFromOpenDevice(*f.device,&binding);
      if(!zero.ok())BootstrapError(zero.error);
      Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
      c.zeros.emplace(f.filespace_uuid,std::move(*zero.record));if(historical_context)c.ObserveHistorical(f,c.zero_images.at(f.filespace_uuid),false);}
    const auto zi=c.zeros.find(primary);Require(zi!=c.zeros.end(),E::invalid_request);const auto& z=zi->second;Require(z.bootstrap.filespace_role<=4,E::bootstrap_failure);const auto& file=c.File(primary,z.bootstrap.page_size_profile_uuid);const u64 size=z.bootstrap.page_size_bytes;
    NativeManagementCheckpointAnchor anchor;std::optional<NativeCheckpointSelection> actual_selection;
    if(explicit_anchor)anchor=*explicit_anchor;
    else{
    c.Charge(4*size);std::array<std::vector<byte>,2> slots;std::array<disk::FilespaceRootReference,2> roots;
    for(unsigned i=0;i<2;++i){const auto root=std::find_if(z.roots.begin(),z.roots.end(),[&](const auto& r){return r.kind==18+i;});Require(root!=z.roots.end(),E::selection_failure);roots[i]=*root;auto& b=slots[i];b.resize(size);
      const auto io=file.device->ReadAt(root->page_number*size,b.data(),b.size());Require(io.ok()&&io.bytes_transferred==b.size(),E::io_failure);
      const disk::NativeCommonPageHeaderBinding binding{{database,primary,z.bootstrap.page_size_profile_uuid},root->page_number,root->page_generation,0x30e,{}};Require(disk::DecodeNativeCommonPageHeader(b.data(),128,&binding).ok(),E::selection_failure);}
    const auto pair=ClassifyNativeCheckpointSelectionPair(slots[0],slots[1]);if(!pair.ok())throw pair.error==NativeCheckpointSelectionError::hash_failure?E::hash_failure:pair.error==NativeCheckpointSelectionError::resource_exhausted?E::resource_exhausted:E::selection_failure;
    const auto& selection=*pair.selection;Require(selection.bootstrap_uuid==z.page_uuid&&selection.object_uuid==roots[0].object_uuid&&selection.object_uuid==roots[1].object_uuid,E::selection_failure);
    anchor={selection.checkpoint,selection.checkpoint_object_uuid,selection.checkpoint_sha256,selection.checkpoint_generation,selection.root_set_generation,selection.timeline_uuid};actual_selection=selection;
    }
    c.timeline=anchor.timeline_uuid;auto current=c.Read(anchor.checkpoint,anchor.checkpoint_object_uuid,anchor.checkpoint_sha256);
    Require(current.root.checkpoint_generation==anchor.checkpoint_generation&&current.root.root_set_generation==anchor.root_set_generation,E::history_mismatch);
    if(actual_selection){const auto& selection=*actual_selection;
      if(selection.selection_generation==1)Require(!current.root.predecessor,E::history_mismatch);
      else Require(current.root.predecessor==selection.previous_checkpoint&&current.root.predecessor_sha256==selection.previous_checkpoint_sha256&&current.root.object_uuid==selection.previous_checkpoint_object_uuid,E::history_mismatch);
      if(!current.root.creator_operation_uuid.is_nil())Require(current.root.creator_operation_uuid==selection.publication_uuid,E::history_mismatch);
    }
    NativeManagementHistory result;std::set<Uuid> attempts;
    while(true){
      const auto head=Head(current.root);
      if(current.root.creator_operation_uuid.is_nil()){
        if(!current.root.predecessor){const auto genesis=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==9;});Require(genesis!=z.roots.end()&&!head&&current.root.checkpoint_generation==1&&current.root.root_set_generation==1&&Self(current.root)==disk::NativePageReference{genesis->filespace_uuid,genesis->page_number,genesis->page_generation,genesis->page_size_profile_uuid}&&current.root.object_uuid==genesis->object_uuid,E::history_mismatch);break;}
        auto parent=c.Parent(current);Require(SameHead(head,Head(parent.root)),E::history_mismatch);current=std::move(parent);continue;
      }
      Require(head&&head->page_type==0x500&&head->page.filespace_uuid==primary&&head->page.page_size_profile_uuid==z.bootstrap.page_size_profile_uuid&&head->page.page_number<z.total_pages,E::history_mismatch);
      c.Charge(2*size);std::vector<byte> bytes(size);const auto io=file.device->ReadAt(head->page.page_number*size,bytes.data(),bytes.size());Require(io.ok()&&io.bytes_transferred==bytes.size(),E::io_failure);
      auto image=DecodeNativePublicationPlan(bytes);PlanError(image.error);auto p=std::move(*image.plan);
      if(actual_selection&&Self(current.root)==anchor.checkpoint&&p.base_selection_generation)
        Require(*p.base_selection_generation+1==actual_selection->selection_generation,E::history_mismatch);
      Require(p.management_extent&&Self(p)==head->page&&p.object_uuid==head->object_uuid&&image.sha256==head->sha256&&p.bootstrap_uuid==z.page_uuid&&p.header.database_uuid==database&&p.timeline_uuid==c.timeline&&p.operation_uuid==current.root.creator_operation_uuid&&p.target_checkpoint==Self(current.root)&&p.target_checkpoint_object_uuid==current.root.object_uuid&&p.reserved_generation==current.root.checkpoint_generation&&p.target_root_set_generation==current.root.root_set_generation,E::history_mismatch);
      Require(attempts.insert(p.operation_uuid).second,E::history_mismatch);
      const auto graph=ComputeNativePublicationTargetGraphDigest(current.bytes);PlanError(graph.error);Require(graph.sha256==p.target_graph_sha256,E::history_mismatch);
      auto base=c.Parent(current);Require(p.base_checkpoint==Self(base.root)&&p.base_checkpoint_object_uuid==base.root.object_uuid&&p.base_checkpoint_sha256==base.sha&&p.base_checkpoint_generation==base.root.checkpoint_generation&&p.base_root_set_generation==base.root.root_set_generation,E::history_mismatch);
      const auto previous=Head(base.root);if(previous)Require(p.previous_plan&&*p.previous_plan==previous->page&&p.previous_plan_object_uuid==previous->object_uuid&&p.previous_plan_sha256==previous->sha256,E::history_mismatch);else Require(!p.previous_plan,E::history_mismatch);
      const auto& r=*p.management_extent;const u64 allowance=u64{r.page_count}*size+4*u64{r.aggregate_bytes}+2*size;c.Charge(allowance);
      auto extent=c.Extent(file,r,z.page_uuid,allowance);
      if(!extent.ok())throw extent.error==NativeManagementExtentError::resource_exhausted?E::resource_exhausted:extent.error==NativeManagementExtentError::hash_failure?E::hash_failure:extent.error==NativeManagementExtentError::io_failure?E::io_failure:extent.error==NativeManagementExtentError::cluster_requires_authority?E::cluster_requires_authority:extent.error==NativeManagementExtentError::encrypted_requires_authority?E::encrypted_requires_authority:E::extent_failure;
      PlanError(BindNativePublicationPlanToManagementRecord(p,*extent.record));
      for(const auto& h:extent.page_headers)Require(h.page_uuid!=p.header.page_uuid,E::history_mismatch);
      std::vector<disk::NativeCommonPageHeader> bundle_headers;std::vector<std::vector<byte>> allocation_images,inventory_images,directory_images,growth_images;
      if(p.control_bundle){const auto& root=*p.control_bundle;const u64 allowance=root.page_count*size+4*(root.directory_count?root.payload_bytes:(root.map_count+root.inventory_count)*size)+2*size;c.Charge(allowance);
        if(root.directory_count)c.HistoricalContexts();
        auto bundle=c.zero_images.empty()?ReadNativeManagementControlBundleFromOpenDevice(file,root,database,z.page_uuid,allowance):
          ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(file,root,database,z.page_uuid,c.zero_images.at(primary),allowance);
        if(!bundle.ok())throw bundle.error==NativeManagementControlBundleError::resource_exhausted?E::resource_exhausted:bundle.error==NativeManagementControlBundleError::hash_failure?E::hash_failure:bundle.error==NativeManagementControlBundleError::io_failure?E::io_failure:bundle.error==NativeManagementControlBundleError::cluster_requires_authority?E::cluster_requires_authority:bundle.error==NativeManagementControlBundleError::encrypted_requires_authority?E::encrypted_requires_authority:E::extent_failure;
        const auto primary_image=std::find_if(bundle.allocation_images.begin(),bundle.allocation_images.end(),[&](const auto& raw){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);return h.ok()&&h.header->filespace_uuid==primary;});
        Require(primary_image!=bundle.allocation_images.end(),E::history_mismatch);const auto first=page::DecodeNativeAllocationMap(*primary_image);
        if(!first.ok())throw first.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:first.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::history_mismatch;
        const auto target=std::find_if(current.root.roots.begin(),current.root.roots.end(),[](const auto& r){return r.role==4;});const auto& h=first.map->header;
        Require(target!=current.root.roots.end()&&target->page==disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}&&target->object_uuid==first.map->object_uuid&&target->sha256==Hash(*primary_image),E::history_mismatch);
        std::set<Uuid> control_ids{p.header.page_uuid};for(const auto& e:extent.page_headers)control_ids.insert(e.page_uuid);
        for(const auto& h:bundle.page_headers)Require(control_ids.insert(h.page_uuid).second,E::history_mismatch);
        if(root.inventory_count){const auto head=page::DecodeNativeTransactionInventoryPage(bundle.inventory_images.front());
          if(!head.ok())throw head.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:head.error==page::NativeInventoryError::hash_failure?E::hash_failure:E::history_mismatch;
          const auto& inventory=*head.page;const auto& h=inventory.header;const auto target=std::find_if(current.root.roots.begin(),current.root.roots.end(),[](const auto& r){return r.role==1;});
          Require(target!=current.root.roots.end()&&target->page_type==0x301&&target->page==disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}&&target->object_uuid==inventory.object_uuid&&target->sha256==Hash(bundle.inventory_images.front())&&current.root.selected_local_transaction_id==inventory.inventory.next_local_transaction_id-1,E::history_mismatch);
          for(const auto& bytes:bundle.inventory_images){const auto h=disk::DecodeNativeCommonPageHeader(bytes.data(),128);Require(h.ok()&&control_ids.insert(h.header->page_uuid).second,E::history_mismatch);}
        }
        if(root.directory_count){
          const auto target=std::find_if(current.root.roots.begin(),current.root.roots.end(),[](const auto& r){return r.role==3;});Require(target!=current.root.roots.end(),E::history_mismatch);
          for(std::size_t n=0;n<bundle.directory_images.size();++n){const auto& raw=bundle.directory_images[n];const auto decoded=page::DecodeNativeFilespaceDirectory(raw);
            if(!decoded.ok())throw decoded.error==page::NativeDirectoryError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeDirectoryError::hash_failure?E::hash_failure:E::history_mismatch;
            const auto& dir=*decoded.directory;const auto& h=dir.header;Require(control_ids.insert(h.page_uuid).second,E::history_mismatch);
            if(!n)Require(target->page_type==9&&target->page==disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}&&target->object_uuid==dir.object_uuid&&target->sha256==Hash(raw),E::history_mismatch);
            for(const auto& member:dir.records){const auto at=c.zeros.find(member.bootstrap.filespace_uuid);Require(at!=c.zeros.end(),E::history_mismatch);const auto& zero=at->second;
              Require(SameBootstrap(member.bootstrap,zero.bootstrap)&&member.page_zero_uuid==zero.page_uuid&&member.page_zero_generation==zero.page_generation&&
                member.root_set_generation==zero.root_set_generation&&member.total_pages==zero.total_pages,E::history_mismatch);}
          }
        }
        if(!bundle.growth_images.empty()){const auto& before=bundle.growth_images.front();const auto& after=bundle.growth_images.back();
          const auto decoded=disk::DecodeFilespacePageZero(before.data(),before.size());if(!decoded.ok())BootstrapError(decoded.error);const auto& fs=decoded.record->bootstrap.filespace_uuid;
          Require(c.zero_images.contains(fs)&&c.zero_images.at(fs)==after,E::history_mismatch);
          c.zeros.at(fs)=*decoded.record;c.zero_images.at(fs)=before;c.Bound(base.root);
          const auto capacity=c.zeros.at(primary).total_pages;
          Require(p.header.page_number<capacity&&p.target_checkpoint.page_number<capacity&&p.management_extent->first.page_number<capacity&&
            p.management_extent->page_count<=capacity-p.management_extent->first.page_number&&root.first.page_number<capacity&&root.page_count<=capacity-root.first.page_number,E::history_mismatch);
        }
        bundle_headers=std::move(bundle.page_headers);allocation_images=std::move(bundle.allocation_images);
        inventory_images=std::move(bundle.inventory_images);
        directory_images=std::move(bundle.directory_images);growth_images=std::move(bundle.growth_images);
      }
      result.entries.push_back({std::move(p),std::move(*extent.record),std::move(extent.page_headers),image.sha256,current.sha,std::move(bundle_headers),std::move(allocation_images),std::move(inventory_images),std::move(directory_images),std::move(growth_images)});current=std::move(base);
    }
    std::reverse(result.entries.begin(),result.entries.end());Index(result.entries,result.latest,result.idempotency);
    if(c.historical)for(const auto& file:c.devices)c.ObserveHistorical(file,c.zero_images.at(file.filespace_uuid),true);
    result.anchor=anchor;result.selection=actual_selection;result.verified_image_bytes=c.used;result.error=E::none;return result;
  }catch(E e){return Fail(e);}catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::io_failure);}
}
} // namespace
NativeManagementHistoryError ValidateNativeManagementHistoryAppend(const NativeManagementGraphHistory& history,
    const NativeManagementOperation& record,u64 budget) noexcept {
  try{
    Require(history.ok(),E::invalid_request);
    // Bound index nodes, retained strings and steps before constructing indexes.
    u64 used=0;const auto charge=[&](u64 bytes){Require(bytes<=budget-used,E::resource_exhausted);used+=bytes;};
    const auto account=[&](const NativeManagementOperation& o){charge(1024);charge(o.idempotency_key.size());
      Require(o.steps.size()<=(budget-used)/768,E::resource_exhausted);charge(o.steps.size()*768);
      for(const auto& step:o.steps)charge(step.idempotency_key.size());};
    for(const auto& entry:history.entries)account(entry.record);account(record);
    const auto valid=ValidateNativeManagementOperation(record);
    if(valid!=NativeManagementOperationError::none)throw valid==NativeManagementOperationError::resource_exhausted?E::resource_exhausted:valid==NativeManagementOperationError::hash_failure?E::hash_failure:E::transition_failure;
    std::map<Uuid,std::size_t> latest;std::map<std::pair<u16,std::string>,Uuid> idempotency;
    Index(history.entries,latest,idempotency,&record);return E::none;
  }catch(E e){return e;}catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::history_mismatch;}
}
NativeManagementHistory ReadNativeManagementHistoryFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,u64 budget) noexcept {
  return ReadHistory(database,devices,primary,budget,nullptr);
}
NativeManagementGraphHistory ReadNativeManagementGraphHistoryFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,const NativeManagementCheckpointAnchor& anchor,u64 budget) noexcept {
  auto result=ReadHistory(database,devices,primary,budget,&anchor);
  return std::move(static_cast<NativeManagementGraphHistory&>(result));
}
NativeManagementGraphHistory ReadNativeManagementGraphHistoryAtHistoricalContextFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativeManagementCheckpointAnchor& anchor,const std::map<Uuid,std::vector<byte>>& context,u64 budget) noexcept {
  auto result=ReadHistory(database,devices,primary,budget,&anchor,&context);
  return std::move(static_cast<NativeManagementGraphHistory&>(result));
}
} // namespace scratchbird::storage::database
