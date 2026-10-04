// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_checkpoint_selection.hpp"
#include "native_management_control_authority.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "native_bound_checkpoint_selection_backing.hpp"
#include "native_checkpoint_inventory_backing.hpp"
#include "native_management_control_authority_backing.hpp"
#include "native_allocation_chain_backing.hpp"
#include "native_directory_chain_backing.hpp"
#include "native_metadata_decode_scratch.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "transaction_inventory_validation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <mutex>
#include <set>

namespace scratchbird::storage::database {
namespace {
using E=NativeCheckpointSelectionError;
namespace mga=scratchbird::transaction::mga;
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
NativeBoundCheckpointSelectionView Fail(E e){NativeBoundCheckpointSelectionView r;r.error=e;return r;}
NativeBoundCheckpointSelectionView CheckpointFailure(const NativeCheckpointInventoryView& source){
  using C=NativeCheckpointError;using I=page::NativeInventoryError;
  const auto nested=source.error==C::inventory_failure?source.inventory_error:I::none;
  auto r=Fail(source.error==C::hash_failure||nested==I::hash_failure?E::hash_failure:
    source.error==C::resource_exhausted||nested==I::resource_exhausted?E::resource_exhausted:
    source.error==C::io_failure||nested==I::io_failure?E::io_failure:
    source.error==C::encrypted_requires_crypto_authority||nested==I::encrypted_requires_crypto_authority?E::encrypted_requires_authority:
    source.error==C::cluster_requires_authority?E::cluster_requires_authority:E::checkpoint_failure);
  r.checkpoint_error=source.error;
  r.checkpoint_inventory.error=source.error;
  r.checkpoint_inventory.inventory_error=source.inventory_error;return r;
}
NativeBoundCheckpointSelectionView AllocationFailure(page::NativeAllocationError error){
  using A=page::NativeAllocationError;
  auto r=Fail(error==A::hash_failure?E::hash_failure:error==A::resource_exhausted?E::resource_exhausted:
    error==A::io_failure?E::io_failure:E::allocation_failure);r.allocation_error=error;return r;
}
using Batch=disk::FileDevice::ReadLatencyBatch;
using Image=std::span<const byte>;
auto Hash(Image image){return core::hash::ComputeSha256DigestNative(image.data(),image.size());}
struct BoundContext {
 const Uuid& database;
 std::span<const disk::NativeFilespaceDevice> files;
 std::span<Batch* const> batches;
 detail::NativeMetadataScratch& scratch;
 NativeBoundCheckpointSelectionDeviceRead& receipt;
 template<class R> void Merge(R& nested){
  if(nested.physical_bytes_read>std::numeric_limits<u64>::max()-receipt.physical_bytes_read)throw std::bad_alloc();
  receipt.physical_bytes_read+=nested.physical_bytes_read;
  receipt.io_status=nested.io_status;receipt.io_diagnostic=std::move(nested.io_diagnostic);
 }
 Batch& Observation(const disk::NativeFilespaceDevice& file){
  const auto at=std::lower_bound(files.begin(),files.end(),file.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
  if(at==files.end()||at->device!=file.device)throw E::invalid_filespace;
  return *batches[at-files.begin()];
 }
 auto Read(const disk::NativeFilespaceDevice& file,u64 offset,void* bytes,std::size_t size){
  auto io=Observation(file).ReadAt(offset,bytes,size);
  receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);
  if(io.bytes_transferred>std::numeric_limits<u64>::max()-receipt.physical_bytes_read)throw std::bad_alloc();
  receipt.physical_bytes_read+=io.bytes_transferred;return io;
 }
 auto Zero(const disk::NativeFilespaceDevice& file){
  const disk::FilespaceBootstrapBinding binding{database,file.filespace_uuid,file.page_size_profile_uuid};
  Image raw;
  return detail::ReadNativeMetadataPageZero(binding,scratch,
   [&](u64 offset,void* bytes,std::size_t size){return Read(file,offset,bytes,size);},
   [&]{auto io=file.device->Size();receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);return io;},raw);
 }
 auto Inventory(const disk::FilespaceRootReference& root,u64 budget){
  auto read=detail::VerifyNativeCheckpointInventoryBacked(database,files,root,budget,batches,*scratch.resource);
  Merge(read);return read.inventory;
 }
 auto Proof(const Uuid& primary,u64 budget){
  auto read=detail::ReadNativeManagementControlAuthorityBacked(database,files,primary,budget,
   NativeManagementHistoryReadContext::selected,nullptr,{},batches,*scratch.resource);
  Merge(read);return read.authority;
 }
 auto Allocation(const disk::NativeFilespaceDevice& file,const disk::FilespaceRootReference* root,u64 budget){
  const disk::FilespaceBootstrapBinding binding{database,file.filespace_uuid,file.page_size_profile_uuid};
  auto read=page::detail::ReadNativeAllocationChainBacked(*file.device,binding,budget,
   root?page::NativeAllocationChainReadContext::selected:page::NativeAllocationChainReadContext::bootstrap,
   root,nullptr,{},Observation(file),*scratch.resource);
  Merge(read);return read.chain;
 }
 auto Directory(const disk::FilespaceRootReference& root,u64 budget){
  auto read=page::detail::ReadNativeDirectoryChainBacked(database,files,root,budget,
   page::NativeDirectoryChainReadContext::current,nullptr,{},batches,*scratch.resource);
  Merge(read);return read.chain;
 }
};
NativeBoundCheckpointSelectionView ReadBoundView(
    const Uuid& database_uuid,std::span<const disk::NativeFilespaceDevice> supplied,
    const Uuid& primary_uuid,u64 budget,std::span<Batch* const> observations,
    std::pmr::memory_resource& resource,NativeBoundCheckpointSelectionDeviceRead& receipt) noexcept {
  try {
    if(!V7(database_uuid)||!V7(primary_uuid)||supplied.empty())return Fail(E::invalid_filespace);
    detail::NativeMetadataScratch scratch{&resource};
    std::pmr::vector<disk::NativeFilespaceDevice> devices(supplied.begin(),supplied.end(),&resource);
    std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::pmr::vector<Batch*> batches(&resource);batches.reserve(devices.size());
    std::pmr::set<disk::FileDevice*> handles(&resource);
    std::pmr::vector<std::unique_lock<std::recursive_mutex>> guards(&resource);guards.reserve(devices.size());
    if(observations.size()!=supplied.size())return Fail(E::invalid_filespace);
    for(std::size_t i=0;i<supplied.size();++i)
      if(!supplied[i].device||!observations[i]||&observations[i]->device()!=supplied[i].device)return Fail(E::invalid_filespace);
    for(std::size_t i=0;i<devices.size();++i){const auto& f=devices[i];
      if(!V7(f.filespace_uuid)||!disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)||!f.device||!handles.insert(f.device).second||
        (i&&devices[i-1].filespace_uuid==f.filespace_uuid))return Fail(E::invalid_filespace);
      const auto at=std::find_if(supplied.begin(),supplied.end(),[&](const auto& file){return file.device==f.device;});
      batches.push_back(observations[at-supplied.begin()]);
    }
    BoundContext context{database_uuid,devices,batches,scratch,receipt};
    for(const auto& f:devices)guards.push_back(f.device->AcquireOperationGuard());
    const auto primary=std::lower_bound(devices.begin(),devices.end(),primary_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
    if(primary==devices.end()||primary->filespace_uuid!=primary_uuid)return Fail(E::invalid_filespace);
    const disk::FilespaceBootstrapBinding binding{database_uuid,primary_uuid,primary->page_size_profile_uuid};
    const auto zero=context.Zero(*primary);
    if(!zero.ok())return Fail(zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
      zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure);
    const auto& z=*zero.record;if(z.bootstrap.filespace_role>4)return Fail(E::invalid_filespace);
    const auto first=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==18;});
    const auto second=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==19;});
    if(first==z.roots.end()||second==z.roots.end())return Fail(E::slot_binding_mismatch);
    const u64 size=z.bootstrap.page_size_bytes;
    // Classification decodes fixed values directly from these two images.
    // It does not allocate two additional untracked copies of the payloads.
    if(budget<2*size)return Fail(E::resource_exhausted);
    NativeBoundCheckpointSelectionView result;std::array<disk::NativeCommonPageHeader,2> headers;
    const disk::FilespaceRootReference* refs[]={&*first,&*second};
    for(unsigned i=0;i<2;++i){auto bytes=scratch.Array<byte>(size);result.slots[i]=bytes;const auto& ref=*refs[i];
      const auto io=context.Read(*primary,ref.page_number*size,bytes.data(),bytes.size());
      if(!io.ok()||io.bytes_transferred!=bytes.size())return Fail(E::io_failure);
      const disk::NativeCommonPageHeaderBinding expected{binding,ref.page_number,ref.page_generation,0x30e,{}};
      const auto h=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&expected);if(!h.ok())return Fail(E::slot_binding_mismatch);headers[i]=*h.header;}
    const auto classified=ClassifyNativeCheckpointSelectionPair(result.slots[0],result.slots[1]);
    if(!classified.ok())return Fail(classified.error);
    const auto& selection=*classified.selection;
    if(selection.bootstrap_uuid!=z.page_uuid||selection.object_uuid!=first->object_uuid||selection.object_uuid!=second->object_uuid)return Fail(E::slot_binding_mismatch);
    const auto& ref=selection.checkpoint;
    const disk::FilespaceRootReference checkpoint{9,0x300,ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid,selection.checkpoint_object_uuid};
    result.retained_image_bytes=2*size;
    result.checkpoint_inventory=context.Inventory(checkpoint,budget-result.retained_image_bytes);
    if(!result.checkpoint_inventory.ok())return CheckpointFailure(result.checkpoint_inventory);
    const auto& pair=result.checkpoint_inventory;const auto& cp=*pair.checkpoint;
    if(pair.checkpoint_sha256!=selection.checkpoint_sha256||cp.checkpoint_generation!=selection.checkpoint_generation||
      cp.root_set_generation!=selection.root_set_generation||cp.timeline_uuid!=selection.timeline_uuid||
      bool(cp.flags&4)!=bool(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required))return Fail(E::checkpoint_binding_mismatch);
    result.retained_image_bytes+=pair.retained_image_bytes;
    if(selection.previous_checkpoint){
      if(!cp.predecessor||*cp.predecessor!=*selection.previous_checkpoint||cp.predecessor_sha256!=selection.previous_checkpoint_sha256||
        cp.object_uuid!=selection.previous_checkpoint_object_uuid)return Fail(E::checkpoint_binding_mismatch);
      const auto& p=*selection.previous_checkpoint;
      const disk::FilespaceRootReference previous{9,0x300,p.filespace_uuid,p.page_number,p.page_generation,p.page_size_profile_uuid,selection.previous_checkpoint_object_uuid};
      result.predecessor=context.Inventory(previous,budget-result.retained_image_bytes);
      if(!result.predecessor.ok())return CheckpointFailure(result.predecessor);
      const auto& before=result.predecessor;const auto& old=*before.checkpoint;
      if(before.checkpoint_sha256!=selection.previous_checkpoint_sha256||old.checkpoint_generation>=cp.checkpoint_generation||old.root_set_generation>=cp.root_set_generation||
        old.timeline_uuid!=cp.timeline_uuid||before.inventory.next_local_transaction_id>pair.inventory.next_local_transaction_id||
        before.inventory.next_commit_sequence>pair.inventory.next_commit_sequence)return Fail(E::checkpoint_binding_mismatch);
      // Individually valid, fully hashed snapshots can still contradict one
      // another. Apply the same retained-history rule as inventory publication
      // before this selection can become any downstream caller's authority.
      const auto count=std::max(before.inventory.entries.size(),pair.inventory.entries.size());
      const auto evolution=page::ValidateNativeTransactionInventoryEvolutionView(before.inventory,pair.inventory,
        scratch.Array<std::size_t>(count),scratch.Array<std::size_t>(count),scratch.Array<byte>(count));
      if(!evolution.ok())return Fail(evolution.error==page::NativeInventoryError::resource_exhausted?
        E::resource_exhausted:E::checkpoint_binding_mismatch);
      result.retained_image_bytes+=before.retained_image_bytes;
    }
    const auto target=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});
    if(target==cp.roots.end()||target->page.filespace_uuid!=primary_uuid||target->page.page_size_profile_uuid!=primary->page_size_profile_uuid)return Fail(E::allocation_binding_mismatch);
    std::optional<NativeManagementControlAuthorityView> operation_proof;
    if(std::any_of(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;})){
      auto proof=context.Proof(primary_uuid,budget-result.retained_image_bytes);
      if(!proof.ok()){using P=NativeManagementControlAuthorityError;return Fail(proof.error==P::hash_failure?E::hash_failure:proof.error==P::resource_exhausted?E::resource_exhausted:proof.error==P::io_failure?E::io_failure:proof.error==P::encrypted_requires_authority?E::encrypted_requires_authority:proof.error==P::cluster_requires_authority?E::cluster_requires_authority:E::creator_mismatch);}
      if(proof.selection->checkpoint!=selection.checkpoint||proof.selection->checkpoint_sha256!=selection.checkpoint_sha256)return Fail(E::checkpoint_binding_mismatch);
      result.retained_image_bytes+=proof.verified_image_bytes;operation_proof=std::move(proof);
    }
    const auto transaction_creator=[&](const Uuid& id,u64 number,bool committed){
      const auto owner=std::find_if(pair.inventory.entries.begin(),pair.inventory.entries.end(),[&](const auto& e){return e.identity.local_id.value==number;});
      return number&&owner!=pair.inventory.entries.end()&&owner->identity.transaction_uuid.value==id&&
        owner->identity.scope==mga::TransactionScope::local_node&&(!committed||mga::HasCommittedInventoryOutcome(*owner));};
    const auto map_creator=[&](const page::NativeAllocationMapView& map){return map.creator_operation_uuid.is_nil()?transaction_creator(map.creator_transaction_uuid,map.creator_local_transaction_id,true):operation_proof&&MatchesNativeManagementControlMap(*operation_proof,map);};
    const auto record_creator=[&](const Uuid& fs,const page::NativeAllocationRecord& r,page::NativeAllocationState state,bool committed){return r.creator_operation_uuid.is_nil()?transaction_creator(r.creator_transaction_uuid,r.creator_local_transaction_id,committed):operation_proof&&MatchesNativeManagementControlAllocation(*operation_proof,fs,r,state);};
    const disk::FilespaceRootReference allocation{3,3,primary_uuid,target->page.page_number,target->page.page_generation,target->page.page_size_profile_uuid,target->object_uuid};
    result.allocation=context.Allocation(*primary,&allocation,budget-result.retained_image_bytes);
    if(!result.allocation.ok())return AllocationFailure(result.allocation.error);
    const auto digest=Hash(result.allocation.pages.front().image);
    if(!digest.ok())return Fail(E::hash_failure);
    if(digest.digest!=target->sha256)return Fail(E::allocation_binding_mismatch);
    std::pmr::set<Uuid> allocation_ids(&resource),page_ids(&resource);
    std::pmr::set<Uuid> controls({z.page_uuid,cp.header.page_uuid,headers[0].page_uuid,headers[1].page_uuid},&resource);
    if(controls.size()!=4)return Fail(E::slot_binding_mismatch);
    for(const auto& image:result.allocation.pages)if(!controls.insert(image.map.header.page_uuid).second)return Fail(E::allocation_binding_mismatch);
    bool matched[2]={false,false};
    for(const auto& image:result.allocation.pages){const auto& map=image.map;
      if(!map_creator(map))return Fail(E::creator_mismatch);
      for(const auto& r:map.records){
        if(!allocation_ids.insert(r.allocation_uuid).second||(!r.page_uuid.is_nil()&&!page_ids.insert(r.page_uuid).second))return Fail(E::allocation_binding_mismatch);
        if(!record_creator(map.header.filespace_uuid,r,map.states[r.page_number-map.first_page],false))return Fail(E::creator_mismatch);
        for(unsigned i=0;i<2;++i)if(r.page_number==headers[i].page_number){
          if(map.states[r.page_number-map.first_page]!=page::NativeAllocationState::allocated||r.page_uuid!=headers[i].page_uuid||
            r.page_generation!=headers[i].page_generation||r.page_type!=0x30e||r.owner_uuid!=selection.object_uuid||!record_creator(map.header.filespace_uuid,r,page::NativeAllocationState::allocated,true))return Fail(E::allocation_binding_mismatch);
          matched[i]=true;
        }
      }
    }
    if(!matched[0]||!matched[1])return Fail(E::allocation_binding_mismatch);
    result.retained_image_bytes+=result.allocation.retained_image_bytes;
    // MGA-NATIVE-SELECTED-CONTROL-ALLOCATION-001. Image integrity and a
    // committed checkpoint creator do not prove its physical allocation.
    std::pmr::vector<NativeInventoryPageBinding> required(&resource);required.push_back({cp.header,cp.object_uuid});
    required.insert(required.end(),pair.inventory_pages.begin(),pair.inventory_pages.end());
    if(result.predecessor.ok()){
      const auto& old=*result.predecessor.checkpoint;required.push_back({old.header,old.object_uuid});
      required.insert(required.end(),result.predecessor.inventory_pages.begin(),result.predecessor.inventory_pages.end());
    }
    page::NativeDirectoryChainView control_directory;
    if(std::any_of(required.begin(),required.end(),[&](const auto& r){return r.header.filespace_uuid!=primary_uuid;})){
      const auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==3;});
      if(root==cp.roots.end())return Fail(E::checkpoint_binding_mismatch);
      const disk::FilespaceRootReference directory{5,root->page_type,root->page.filespace_uuid,root->page.page_number,
        root->page.page_generation,root->page.page_size_profile_uuid,root->object_uuid};
      control_directory=context.Directory(directory,budget-result.retained_image_bytes);
      if(!control_directory.ok()){using D=page::NativeDirectoryError;const auto error=control_directory.error;
        return Fail(error==D::resource_exhausted?E::resource_exhausted:error==D::hash_failure?E::hash_failure:
          error==D::io_failure?E::io_failure:E::checkpoint_binding_mismatch);}
      const auto hash=Hash(control_directory.pages.front().image);
      if(!hash.ok())return Fail(E::hash_failure);
      if(hash.digest!=root->sha256)return Fail(E::checkpoint_binding_mismatch);
      const auto& creator=control_directory.pages.front().directory;
      if(creator.creator_operation_uuid.is_nil()){
        if(!transaction_creator(creator.creator_transaction_uuid,creator.creator_local_transaction_id,true))return Fail(E::creator_mismatch);
      }else if(!operation_proof||!MatchesNativeManagementPublishedDirectory(*operation_proof,control_directory,hash.digest))return Fail(E::creator_mismatch);
      result.retained_image_bytes+=control_directory.retained_image_bytes;
      for(const auto& image:control_directory.pages)for(const auto& entry:image.directory.records){
        if(entry.bootstrap.filespace_uuid!=primary_uuid||!entry.allocation_root)continue;
        const auto& r=*entry.allocation_root;const auto& map=result.allocation.pages.front().map;
        if(r.page!=target->page||r.object_uuid!=target->object_uuid||r.sha256!=target->sha256||
            r.map_generation!=map.map_generation||r.capacity_generation!=map.capacity_generation||entry.total_pages!=map.total_pages)
          return Fail(E::allocation_binding_mismatch);
      }
    }
    for(const auto& file:devices){
      if(std::none_of(required.begin(),required.end(),[&](const auto& r){return r.header.filespace_uuid==file.filespace_uuid;}))continue;
      page::NativeAllocationChainView secondary;
      const auto* maps=&result.allocation;
      if(file.filespace_uuid!=primary_uuid){
        const page::NativeFilespaceDirectoryRecord* member=nullptr;
        for(const auto& image:control_directory.pages)for(const auto& entry:image.directory.records)
          if(entry.bootstrap.filespace_uuid==file.filespace_uuid)member=&entry;
        if(!member)return Fail(E::allocation_binding_mismatch);
        const disk::FilespaceBootstrapBinding secondary_binding{database_uuid,file.filespace_uuid,file.page_size_profile_uuid};
        if(member->allocation_root){const auto& r=*member->allocation_root;
          const disk::FilespaceRootReference selected{3,3,r.page.filespace_uuid,r.page.page_number,r.page.page_generation,r.page.page_size_profile_uuid,r.object_uuid};
          secondary=context.Allocation(file,&selected,budget-result.retained_image_bytes);
        }else secondary=context.Allocation(file,nullptr,budget-result.retained_image_bytes);
        if(!secondary.ok())return AllocationFailure(secondary.error);
        if(member->allocation_root){const auto& r=*member->allocation_root;const auto& map=secondary.pages.front().map;
          const auto hash=Hash(secondary.pages.front().image);if(!hash.ok())return Fail(E::hash_failure);
          if(hash.digest!=r.sha256||map.map_generation!=r.map_generation||map.capacity_generation!=r.capacity_generation||
              map.total_pages!=member->total_pages)return Fail(E::allocation_binding_mismatch);}
        result.retained_image_bytes+=secondary.retained_image_bytes;
        maps=&secondary;
        for(const auto& image:maps->pages){const auto& map=image.map;
          if(!map_creator(map))return Fail(E::creator_mismatch);
          for(const auto& record:map.records){
            if(!allocation_ids.insert(record.allocation_uuid).second||(!record.page_uuid.is_nil()&&!page_ids.insert(record.page_uuid).second))return Fail(E::allocation_binding_mismatch);
            if(!record_creator(file.filespace_uuid,record,map.states[record.page_number-map.first_page],false))return Fail(E::creator_mismatch);
          }
        }
      }
      for(const auto& control:required){const auto& h=control.header;if(h.filespace_uuid!=file.filespace_uuid)continue;
        const page::NativeAllocationRecord* found=nullptr;
        for(const auto& image:maps->pages){const auto& map=image.map;
          if(h.page_number<map.first_page||h.page_number-map.first_page>=map.states.size())continue;
          if(map.states[h.page_number-map.first_page]!=page::NativeAllocationState::allocated)return Fail(E::allocation_binding_mismatch);
          const auto record=std::lower_bound(map.records.begin(),map.records.end(),h.page_number,
            [](const auto& r,u64 n){return r.page_number<n;});
          if(record!=map.records.end()&&record->page_number==h.page_number)found=&*record;
          break;
        }
        if(!found||found->page_uuid!=h.page_uuid||found->page_generation!=h.page_generation||
          found->page_type!=h.page_type||found->owner_uuid!=control.object_uuid)return Fail(E::allocation_binding_mismatch);
        if(!record_creator(file.filespace_uuid,*found,page::NativeAllocationState::allocated,true))return Fail(E::creator_mismatch);
      }
    }
    result.selection=selection;result.error=E::none;return result;
  }catch(E error){return Fail(error);}catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::io_failure);}
}
NativeCheckpointRoot OwnCheckpoint(const NativeCheckpointRootView& v){
 NativeCheckpointRoot out;out.header=v.header;out.object_uuid=v.object_uuid;
 out.checkpoint_generation=v.checkpoint_generation;out.root_set_generation=v.root_set_generation;
 out.selected_local_transaction_id=v.selected_local_transaction_id;out.stable_local_transaction_id=v.stable_local_transaction_id;
 out.local_durable_transaction_id=v.local_durable_transaction_id;out.cluster_quorum_transaction_id=v.cluster_quorum_transaction_id;
 out.timeline_uuid=v.timeline_uuid;out.creator_transaction_uuid=v.creator_transaction_uuid;
 out.creator_local_transaction_id=v.creator_local_transaction_id;out.flags=v.flags;out.predecessor=v.predecessor;
 out.predecessor_sha256=v.predecessor_sha256;out.completed=v.completed;out.creator_operation_uuid=v.creator_operation_uuid;
 out.roots.assign(v.roots.begin(),v.roots.end());return out;
}
NativeCheckpointInventoryResult OwnInventory(const NativeCheckpointInventoryView& v){
 NativeCheckpointInventoryResult out;out.error=v.error;out.inventory_error=v.inventory_error;
 if(!v.ok())return out;
 out.checkpoint=OwnCheckpoint(*v.checkpoint);out.checkpoint_sha256=v.checkpoint_sha256;
 out.inventory.next_local_transaction_id=v.inventory.next_local_transaction_id;
 out.inventory.next_commit_sequence=v.inventory.next_commit_sequence;
 out.inventory.entries.assign(v.inventory.entries.begin(),v.inventory.entries.end());
 out.inventory_pages.assign(v.inventory_pages.begin(),v.inventory_pages.end());
 out.inventory_generation=v.inventory_generation;out.retained_image_bytes=v.retained_image_bytes;return out;
}
page::NativeAllocationChainResult OwnAllocation(const page::NativeAllocationChainView& v){
 page::NativeAllocationChainResult out;out.error=v.error;if(!v.ok())return out;
 out.state_counts=v.state_counts;out.retained_image_bytes=v.retained_image_bytes;out.pages.reserve(v.pages.size());
 for(const auto& image:v.pages){const auto& x=image.map;page::NativeAllocationMap map;
  map.header=x.header;map.object_uuid=x.object_uuid;map.map_generation=x.map_generation;map.capacity_generation=x.capacity_generation;
  map.total_pages=x.total_pages;map.first_page=x.first_page;map.creator_transaction_uuid=x.creator_transaction_uuid;
  map.creator_local_transaction_id=x.creator_local_transaction_id;map.creator_operation_uuid=x.creator_operation_uuid;
  map.next=x.next;map.next_sha256=x.next_sha256;
  map.states.assign(x.states.begin(),x.states.end());map.records.assign(x.records.begin(),x.records.end());
  page::NativeAllocationMapResult value;value.error=page::NativeAllocationError::none;
  value.map=std::move(map);value.bytes.assign(image.image.begin(),image.image.end());out.pages.push_back(std::move(value));
 }return out;
}
NativeBoundCheckpointSelection Materialize(const NativeBoundCheckpointSelectionView& v){
 NativeBoundCheckpointSelection out;out.error=v.error;out.checkpoint_error=v.checkpoint_error;out.allocation_error=v.allocation_error;
 out.checkpoint_inventory.error=v.checkpoint_inventory.error;
 out.checkpoint_inventory.inventory_error=v.checkpoint_inventory.inventory_error;
 if(!v.ok())return out;
 out.selection=v.selection;for(unsigned n=0;n<2;++n)out.slots[n].assign(v.slots[n].begin(),v.slots[n].end());
 out.checkpoint_inventory=OwnInventory(v.checkpoint_inventory);out.predecessor=OwnInventory(v.predecessor);
 out.allocation=OwnAllocation(v.allocation);out.retained_image_bytes=v.retained_image_bytes;return out;
}
}

NativeBoundCheckpointSelectionDeviceRead detail::ReadNativeBoundCheckpointSelectionBacked(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const Uuid& primary,u64 budget,
 std::span<Batch* const> observations,std::pmr::memory_resource& resource) noexcept {
 NativeBoundCheckpointSelectionDeviceRead out;
 out.selection=ReadBoundView(database,files,primary,budget,observations,resource,out);return out;
}
NativeBoundCheckpointSelectionDeviceRead ReadNativeBoundCheckpointSelectionInto(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const Uuid& primary,u64 budget,
 std::span<Batch* const> observations,std::span<byte> backing) noexcept {
 NativeBoundCheckpointSelectionDeviceRead out;
 const auto disjoint=[&](auto value){return disk::detail::DisjointNativeDecodeRegions(backing,value);};
 bool valid=disjoint(std::span{&database,1})&&disjoint(std::span{&primary,1})&&disjoint(files)&&disjoint(observations);
 for(const auto& file:files)valid=valid&&(!file.device||disjoint(std::span{file.device,1}));
 for(const auto* batch:observations)valid=valid&&(!batch||disjoint(std::span{batch,1}));
 if(!valid){out.selection.error=E::invalid_backing;return out;}
 detail::NativeMetadataMemory resource(backing);
 out=detail::ReadNativeBoundCheckpointSelectionBacked(database,files,primary,budget,observations,resource);
 if(out.ok())out.selection.backing_bytes_used=resource.used();return out;
}
NativeBoundCheckpointSelection ReadNativeBoundCheckpointSelectionFromOpenDevices(
 const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& files,const Uuid& primary,u64 budget) noexcept {
 const auto fail=[](E error){NativeBoundCheckpointSelection out;out.error=error;return out;};
 try{
  detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);
  detail::NativeMetadataScratch scratch{&resource};auto pointers=scratch.Array<Batch*>(files.size());
  if(files.size()>std::numeric_limits<std::size_t>::max()/sizeof(Batch))throw std::bad_alloc();
  auto* batches=static_cast<Batch*>(resource.allocate(files.size()*sizeof(Batch),alignof(Batch)));
  NativeBoundCheckpointSelectionDeviceRead read;
  {struct Cleanup{Batch* batches;std::size_t count=0;~Cleanup(){while(count)std::destroy_at(batches+--count);}} cleanup{batches};
   for(std::size_t n=0;n<files.size();++n){if(!files[n].device)return fail(E::invalid_filespace);
    pointers[n]=std::construct_at(batches+n,*files[n].device);++cleanup.count;}
   read=detail::ReadNativeBoundCheckpointSelectionBacked(database,files,primary,budget,pointers,resource);
  }
  return Materialize(read.selection);
 }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
  catch(const std::length_error&){return fail(E::resource_exhausted);}catch(...){return fail(E::io_failure);}
}
} // namespace scratchbird::storage::database
